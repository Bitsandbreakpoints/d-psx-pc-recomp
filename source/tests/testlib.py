"""Shared helpers for the PC port's tests (Windows only).

The tests never use the global keyboard or move the focus: the port is
driven by D_INPUT (and D_EVENT), an emulator by key messages to its window
(PostMessage), pictures come from D_VRAM_DUMP and the emulator's own
screenshots, so the PC stays usable while they run.  The discs come from build/settings.ini
(choose them in the launcher once).
"""
import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PORT = os.path.dirname(HERE)
ROOT = os.path.dirname(PORT)
EXE = os.path.join(PORT, 'build', 'd.exe')
LOG = os.path.join(PORT, 'build', 'd.log')

user32 = ctypes.windll.user32
WM_CLOSE = 0x0010
WM_KEYDOWN, WM_KEYUP = 0x0100, 0x0101
KEYS = {'enter': 0x0D, 'esc': 0x1B, 'up': 0x26, 'down': 0x28, 'left': 0x25, 'right': 0x27,
        'x': 0x58, 'c': 0x43, 'z': 0x5A, 's': 0x53, 'q': 0x51, 'e': 0x45, 'back': 0x08,
        'f1': 0x70, 'f10': 0x79}
ARROW_FIRST, ARROW_LAST = 0x25, 0x28
EXTENDED_KEY = 0x01000000
KEY_HOLD_SECONDS = 0.12
KEY_GAP_SECONDS = 0.25
ENUM_PROC = ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
POLL_SECONDS = 1


def _area(hwnd):
    r = wt.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(r))
    return (r.right - r.left) * (r.bottom - r.top)


def find_window(pid, child=False):
    """The largest visible top-level window of `pid` (or its largest child,
    e.g. an emulator's display)."""
    best = [None, -1]

    def visit(hwnd, _):
        if user32.IsWindowVisible(hwnd) and _area(hwnd) >= best[1]:
            best[0], best[1] = hwnd, _area(hwnd)
        return True

    def top(hwnd, _):
        owner = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(hwnd):
            if child:
                user32.EnumChildWindows(hwnd, ENUM_PROC(visit), 0)
            else:
                visit(hwnd, 0)
        return True

    user32.EnumWindows(ENUM_PROC(top), 0)
    return best[0]


def post_keys(hwnd, keys):
    """'enter wait500 x': key messages to one window (held ~120 ms each)."""
    for key in keys.split():
        if key.startswith('wait'):
            time.sleep(int(key[4:]) / 1000)
            continue
        code = KEYS[key]
        scan = user32.MapVirtualKeyW(code, 0)
        ext = EXTENDED_KEY if ARROW_FIRST <= code <= ARROW_LAST else 0
        user32.PostMessageW(hwnd, WM_KEYDOWN, code, 1 | scan << 16 | ext)
        time.sleep(KEY_HOLD_SECONDS)
        user32.PostMessageW(hwnd, WM_KEYUP, code, 0xC0000001 | scan << 16 | ext)
        time.sleep(KEY_GAP_SECONDS)


def close(proc, timeout=5):
    """Closes the program's window (so files are finished), else kills it."""
    hwnd = find_window(proc.pid)
    if hwnd:
        user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
    try:
        proc.wait(timeout)
    except subprocess.TimeoutExpired:
        proc.kill()


def log_lines():
    try:
        with open(LOG, encoding='utf-8', errors='replace') as f:
            return [line.rstrip('\n') for line in f]
    except OSError:
        return []


def run_port(out_dir, script, frames, last_frame, timeout, events=None, wav=None, env=None):
    """Runs the game (D_AUTOSTART) with D_INPUT=`script`, pictures at
    `frames` (shot_<frame>.bmp in out_dir), until frame `last_frame` (then
    the game leaves, D_LEAVE) or `timeout` seconds; `env`: more
    environment variables (e.g. D_NATIVE).  Returns the log."""
    os.makedirs(out_dir, exist_ok=True)
    extra = env or {}
    # (the logos always play: the scripts' frame numbers expect them)
    env = dict(os.environ, D_AUTOSTART='1', D_KEEP_LOGOS='1', D_INPUT=script, D_LEAVE=str(last_frame),
               D_VRAM_DUMP=','.join(str(f) for f in frames))
    env.update(extra)
    if events:
        env['D_EVENT'] = events
    if wav:
        env['D_WAV_DUMP'] = os.path.join(out_dir, wav)
    else:
        env['D_MUTE'] = '1'
    if os.path.exists(LOG):
        os.remove(LOG)
    proc = subprocess.Popen([EXE], cwd=out_dir, env=env)
    start = time.time()
    while time.time() - start < timeout:
        time.sleep(POLL_SECONDS)
        if any('game stopped' in line for line in log_lines()):
            break
    close(proc)
    return log_lines()


def montage(out_dir, frames, out):
    """The pictures side by side (Pillow), with the frame numbers."""
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        print('montage: Pillow is not installed (pip install pillow) - skipped')
        return
    pictures = []
    for f in frames:
        path = os.path.join(out_dir, f'shot_{f}.bmp')
        if os.path.exists(path):
            pictures.append((f, Image.open(path).convert('RGB')))
    if not pictures:
        return
    width = sum(p.width for _, p in pictures) + 8 * len(pictures)
    height = max(p.height for _, p in pictures) + 16
    sheet = Image.new('RGB', (width, height), (32, 32, 32))
    draw = ImageDraw.Draw(sheet)
    x = 0
    for frame, picture in pictures:
        sheet.paste(picture, (x, 16))
        draw.text((x + 2, 2), str(frame), fill=(255, 255, 255))
        x += picture.width + 8
    sheet.save(out)
