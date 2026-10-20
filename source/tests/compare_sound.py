#!/usr/bin/env python3
"""Compares the port's sound with DuckStation's: both are recorded through
the same steps (the logos, the opening skipped, the title music, Start, the
game's first movies skipped, the dining hall), and two parts are compared:

  title    the title music (XA audio streamed from the disc)
  castle   the dining hall's background sound (an SPU effect looping)

The two recordings do not run in step (the emulator is driven by time,
the port by frames), so each part is first lined up by its loudness over
time.  Then: the level difference (dB), the spectrum's shape (energy per
octave band, the largest difference in dB) and, for the title music, the
waveform's correlation after lining up to the sample (1.0 = the same).

    python tests/compare_sound.py [disc1.cue]

DuckStation records with its audio capture (Media Foundation, .mp4), which
tests/mf_to_wav.c turns into a WAV; the port records with D_WAV_DUMP (what
the sound device plays).  Needs a sound device for the port's recording.
"""
import math
import os
import struct
import subprocess
import sys
import time
import wave

import compare_duckstation as ds
from testlib import PORT, close, find_window, post_keys, run_port, KEYS

OUT = os.path.join(PORT, 'build', 'test', 'sound')
MF_TO_WAV_SRC = os.path.join(PORT, 'tests', 'mf_to_wav.c')
MF_TO_WAV = os.path.join(PORT, 'build', 'mf_to_wav.exe')
CLANG = os.path.join(PORT, 'tools', 'llvm-mingw', 'bin', 'i686-w64-mingw32-clang.exe')
RATE = 44100

KEYS['f9'] = 0x78                       # the emulator's audio capture on / off
TITLE_SECONDS = 20                      # the title shown before Start (its music fades in)
SKIP_PRESSES, SKIP_EVERY = 12, 2.5      # Start through the game's first movies
CASTLE_SECONDS = 15

# the port: Start skips the opening (frames 300..500), the title until 1800,
# then Start every 40 frames through the movies, the castle until the end
PORT_INPUT = ','.join([f'{t}:start' for t in range(300, 500, 40)]
                      + [f'{t}:start' for t in range(1800, 3500, 40)])
PORT_LAST_FRAME = 4500

ENVELOPE_STEP = 441                     # 10 ms
FINE_SEARCH = 4410                      # +-100 ms around the envelope's place
FINE_PROBE = 2205                       # the 50 ms lined up to the sample
FINE_COARSE_STEP = 4
PART_SECONDS = 8
BANDS = [(63, 125), (125, 250), (250, 500), (500, 1000), (1000, 2000), (2000, 4000),
         (4000, 8000), (8000, 16000)]
FFT_SIZE = 2048


# ---- recording ------------------------------------------------------------

def build_mf_to_wav():
    if not os.path.exists(MF_TO_WAV) or os.path.getmtime(MF_TO_WAV) < os.path.getmtime(MF_TO_WAV_SRC):
        subprocess.run([CLANG, '-O2', '-municode', '-o', MF_TO_WAV, MF_TO_WAV_SRC, '-lmfplat',
                        '-lmfreadwrite', '-lmfuuid', '-lole32'], check=True)


def record_emulator(cue, out_wav):
    ds.setup_emulator()
    settings = os.path.join(ds.EMU_DIR, 'settings.ini')
    text = open(settings, encoding='utf-8').read()
    text = text.replace('Screenshot = Keyboard/F10', 'Screenshot = Keyboard/F10\nToggleAudioCapture = Keyboard/F9')
    text += '\n[MediaCapture]\nBackend = MediaFoundation\n'
    open(settings, 'w', encoding='utf-8', newline='\n').write(text)
    videos = os.path.join(ds.EMU_DIR, 'videos')
    os.makedirs(videos, exist_ok=True)
    before = set(os.listdir(videos))
    shots = os.path.join(ds.EMU_DIR, 'screenshots')
    os.makedirs(shots, exist_ok=True)
    probe = os.path.join(OUT, 'emu_probe.png')
    proc = subprocess.Popen([os.path.join(ds.EMU_DIR, ds.EMU_EXE), '-batch', '-nofullscreen', cue],
                            cwd=ds.EMU_DIR)
    try:
        time.sleep(3)
        post_keys(find_window(proc.pid, child=True), 'f9')          # capture on
        start = time.time()
        while time.time() - start < ds.TITLE_TIMEOUT:
            time.sleep(ds.TITLE_POLL)
            if ds.screenshot(proc, shots, probe) and ds.picture_size(probe)[0] in ds.OPENING_WIDTHS \
                    and ds.picture_size(probe)[1] == ds.OPENING_HEIGHT:
                post_keys(find_window(proc.pid, child=True), 'enter')
                break
        time.sleep(TITLE_SECONDS)
        for _ in range(SKIP_PRESSES):
            post_keys(find_window(proc.pid, child=True), 'enter')
            time.sleep(SKIP_EVERY)
        time.sleep(CASTLE_SECONDS)
        post_keys(find_window(proc.pid, child=True), 'f9')          # capture off
        time.sleep(2)
    finally:
        close(proc)
    new = sorted(set(os.listdir(videos)) - before)
    if not new:
        sys.exit('the emulator recorded nothing (audio capture)')
    subprocess.run([MF_TO_WAV, os.path.join(videos, new[-1]), out_wav], check=True)


def record_port(out_wav):
    log = run_port(OUT, PORT_INPUT, [], PORT_LAST_FRAME, 600, wav=os.path.basename(out_wav),
                   env={'D_NATIVE': '1'})
    for line in log:
        if 'CRASH' in line or 'frames in' in line or 'sound' in line:
            print('port:', line)


# ---- analysis ---------------------------------------------------------------

def load_mono(path):
    with wave.open(path) as w:
        frames = w.readframes(w.getnframes())
        channels = w.getnchannels()
    s = struct.unpack('<%dh' % (len(frames) // 2), frames)
    return [sum(s[i:i + channels]) / channels for i in range(0, len(s), channels)]


def envelope(samples):
    return [math.sqrt(sum(x * x for x in samples[i:i + ENVELOPE_STEP]) / ENVELOPE_STEP)
            for i in range(0, len(samples) - ENVELOPE_STEP, ENVELOPE_STEP)]


def best_offset(a, b, length):
    """Where in b the part a[0:length] fits best (normalised correlation)."""
    a = a[:length]
    ma = sum(a) / len(a)
    na = math.sqrt(sum((x - ma) ** 2 for x in a)) or 1
    best, where = -2, 0
    for off in range(0, len(b) - length):
        seg = b[off:off + length]
        mb = sum(seg) / length
        nb = math.sqrt(sum((x - mb) ** 2 for x in seg)) or 1
        c = sum((x - ma) * (y - mb) for x, y in zip(a, seg)) / (na * nb)
        if c > best:
            best, where = c, off
    return where, best


def fft(x):
    n = len(x)
    if n == 1:
        return x
    even, odd = fft(x[0::2]), fft(x[1::2])
    out = [0] * n
    for k in range(n // 2):
        t = complex(math.cos(-2 * math.pi * k / n), math.sin(-2 * math.pi * k / n)) * odd[k]
        out[k] = even[k] + t
        out[k + n // 2] = even[k] - t
    return out


def band_levels(samples):
    """dB per octave band (averaged spectra of FFT_SIZE windows)."""
    energy = [0.0] * len(BANDS)
    windows = 0
    for i in range(0, len(samples) - FFT_SIZE, FFT_SIZE * 4):
        spectrum = fft([complex(v, 0) for v in samples[i:i + FFT_SIZE]])
        windows += 1
        for b, (lo, hi) in enumerate(BANDS):
            k0, k1 = int(lo * FFT_SIZE / RATE), int(hi * FFT_SIZE / RATE)
            energy[b] += sum(abs(c) ** 2 for c in spectrum[k0:k1])
    total = sum(energy) or 1
    return [10 * math.log10(e / total + 1e-12) for e in energy]


def level_db(samples):
    rms = math.sqrt(sum(x * x for x in samples) / max(1, len(samples)))
    return 20 * math.log10(rms + 1e-9)


def correlation(a, b):
    n = min(len(a), len(b))
    ma, mb = sum(a[:n]) / n, sum(b[:n]) / n
    num = sum((x - ma) * (y - mb) for x, y in zip(a[:n], b[:n]))
    den = math.sqrt(sum((x - ma) ** 2 for x in a[:n]) * sum((y - mb) ** 2 for y in b[:n])) or 1
    return num / den


def compare_part(name, emu, port, emu_start, fine):
    """The part of the emulator's recording from emu_start (seconds), found
    in the port's recording by its loudness."""
    length = int(PART_SECONDS * RATE)
    a = emu[int(emu_start * RATE):int(emu_start * RATE) + length]
    env_a, env_b = envelope(a), envelope(port)
    off, match = best_offset(env_a, env_b, len(env_a))
    b = port[off * ENVELOPE_STEP:off * ENVELOPE_STEP + length]
    if fine:
        # to the sample: the loudest moment of the part (a quiet one says
        # nothing), searched around the envelope's place, coarse then exact
        loudest = max(range(len(env_a)), key=lambda i: env_a[i]) * ENVELOPE_STEP
        loudest = min(max(loudest, FINE_SEARCH), length - FINE_SEARCH - FINE_PROBE)
        probe = a[loudest:loudest + FINE_PROBE]
        base = off * ENVELOPE_STEP + loudest

        def fit(s):
            seg = port[base + s:base + s + FINE_PROBE]
            return correlation(probe, seg) if len(seg) == FINE_PROBE else -2

        coarse = max(range(-FINE_SEARCH, FINE_SEARCH, FINE_COARSE_STEP), key=fit)
        shift = max(range(coarse - FINE_COARSE_STEP, coarse + FINE_COARSE_STEP + 1), key=fit)
        b = port[off * ENVELOPE_STEP + shift:off * ENVELOPE_STEP + shift + length]
    la, lb = band_levels(a), band_levels(b)
    worst = max(abs(x - y) for x, y in zip(la, lb))
    print(f'{name:7} emulator {emu_start:5.1f} s, port {off * ENVELOPE_STEP / RATE:6.1f} s (envelope match {match:.2f}); '
          f'level emulator {level_db(a):5.1f} dB, port {level_db(b):5.1f} dB '
          f'(difference {level_db(b) - level_db(a):+.1f}); spectrum shape: largest band '
          f'difference {worst:.1f} dB'
          + (f'; waveform correlation {correlation(a, b):.2f}' if fine else ''))
    print('        bands (Hz: emulator/port dB of the total): ' + ', '.join(
        f'{lo}-{hi}: {x:.1f}/{y:.1f}' for (lo, hi), x, y in zip(BANDS, la, lb)))


def main():
    cue = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else ds.DEFAULT_DISC
    os.makedirs(OUT, exist_ok=True)
    build_mf_to_wav()
    emu_wav, port_wav = os.path.join(OUT, 'emulator.wav'), os.path.join(OUT, 'port.wav')
    if '--reuse' not in sys.argv:
        record_emulator(cue, emu_wav)
        record_port(port_wav)
    emu, port = load_mono(emu_wav), load_mono(port_wav)
    print(f'recordings: emulator {len(emu) / RATE:.1f} s, port {len(port) / RATE:.1f} s')
    # the title music: a few seconds after the opening was skipped; the
    # castle: the last seconds of the emulator's recording
    title_start = find_title_start(emu)
    compare_part('title', emu, port, title_start, fine=True)
    compare_part('castle', emu, port, len(emu) / RATE - PART_SECONDS - 3, fine=False)


def find_title_start(emu):
    """The title music starts after a quiet moment (the opening skipped):
    the first second of sound after a gap of at least half a second, 20 s
    or more into the recording."""
    env = envelope(emu)
    quiet = 0
    for i, v in enumerate(env):
        if i * ENVELOPE_STEP / RATE < 20:
            continue
        if v < 50:
            quiet += 1
        elif quiet >= 50:
            return i * ENVELOPE_STEP / RATE + 2.0
        else:
            quiet = 0
    return 30.0


if __name__ == '__main__':
    main()
