/*
 * libsnd.c - LIBSND: the sound effects (a VAB sound bank played with
 * SsVoKeyOn/Off) on top of the software SPU (spu.c), and the volume of
 * the CD audio.
 *
 * D uses only a small part of LIBSND: one sound bank (ART/DSE.VH/VB) and
 * SsVoKeyOn(), which plays every tone of a program that covers the note.
 * The volume and panning of a key-on follow the library (_SsVmKeyOnNow);
 * the pitch is computed from the note directly (the library takes it from
 * tables, which gives the same up to rounding).  Voices: a free one first,
 * else the oldest.
 */
#include <math.h>
#include <stdlib.h>
#include "psx/psx.h"
#include "psx/system.h"
#include "psx/spu.h"
#include "psx/savestate.h"
#include "psx/xa.h"

#define PERCENT                 100
#define SERIAL_VOLUME_MAX       127     /* SsSetSerialVol() */
#define XA_UNITY                256     /* xa_mix(): as decoded */

#define VAB_MAX                 16
#define PROGRAMS                128
#define TONES_PER_PROGRAM       16
#define VAG_TABLE_ENTRIES       256
#define VOL_MAX                 127
#define VELOCITY_MAX            127
#define PAN_CENTER              64
#define SPU_VOLUME_MAX          0x3FFF
#define SPU_PITCH_UNITY         0x1000  /* the sample's own rate (44.1 kHz) */
#define SPU_PITCH_MAX           0x3FFF
#define SPU_ALLOC_START         0x1010  /* LIBSPU leaves the first 1010h bytes */
#define SEMITONES_PER_OCTAVE    12
#define FINE_STEPS              128     /* fine tune: 1/128 semitone */

/* VAB header layout */
#define VAB_MAGIC               0x56414270      /* "pBAV" */
#define VAB_HEADER_SIZE         0x20
#define VAB_PROGRAM_SIZE        16
#define VAB_TONE_SIZE           32
#define VAB_PROGRAM_COUNT       0x12            /* ps */
#define VAB_MASTER_VOLUME       0x18            /* mvol */
#define TONE_MODE_REVERB        0x04

/* VagAtr fields */
enum { TONE_PRIOR, TONE_MODE, TONE_VOL, TONE_PAN, TONE_CENTER, TONE_SHIFT, TONE_MIN, TONE_MAX,
       TONE_ADSR1 = 16, TONE_ADSR2 = 18, TONE_PROG = 20, TONE_VAG = 22 };
/* ProgAtr fields */
enum { PROG_TONES, PROG_VOL, PROG_PRIOR, PROG_MODE, PROG_PAN };

/* the parts of a VAB the voices need (the header stays in the game's buffer) */
typedef struct {
    int open;
    const uint8_t *header;
    uint32_t vag_address[VAG_TABLE_ENTRIES];
    uint32_t body_size;
} Vab;

/* what a voice plays (SsVoKeyOff finds it by program and note) */
typedef struct {
    int vab_prog;                       /* -1: free */
    int note;
    uint32_t age;
} VoiceOwner;

static Vab s_vabs[VAB_MAX];
static VoiceOwner s_owners[SPU_VOICES];
static uint32_t s_age;
static int s_volume_cd = PERCENT, s_volume_effects = PERCENT, s_volume_master = PERCENT;
static int s_main_left = VOL_MAX, s_main_right = VOL_MAX;   /* SsSetMVol() */
static int s_serial_left, s_serial_right;       /* SsSetSerialVol(SS_SERIAL_A): 0..127 */

static void apply_main_volume(void)
{
    int scale = s_volume_master * s_volume_effects;             /* 0..PERCENT^2 */
    spu_set_main_volume(s_main_left * SPU_VOLUME_MAX / VOL_MAX * scale / (PERCENT * PERCENT),
                        s_main_right * SPU_VOLUME_MAX / VOL_MAX * scale / (PERCENT * PERCENT));
}

void psx_set_sound_volumes(int master, int cd, int effects)
{
    s_volume_master = master;
    s_volume_cd = cd;
    s_volume_effects = effects;
    apply_main_volume();
}

void libsnd_reset(void)
{
    memset(s_vabs, 0, sizeof(s_vabs));
    for (int i = 0; i < SPU_VOICES; i++) {
        s_owners[i].vab_prog = -1;
    }
    s_serial_left = s_serial_right = 0;
    s_main_left = s_main_right = VOL_MAX;
    spu_reset();
    apply_main_volume();
}

/* once per frame (the sound thread, or the vertical blank) */
void libsnd_tick(void)
{
}

/* The CD's sound (XA) into a frame of sound: at the game's serial volume
 * (SsSetSerialVol), through the SPU's main volume (SsSetMVol) as on the
 * console, and at the settings' volumes. */
void libsnd_mix_cd(int16_t *out, int frames)
{
    int scale = s_volume_master * s_volume_cd;                  /* 0..PERCENT^2 */
    int left = s_serial_left * s_main_left * XA_UNITY / (SERIAL_VOLUME_MAX * VOL_MAX)
               * scale / (PERCENT * PERCENT);
    int right = s_serial_right * s_main_right * XA_UNITY / (SERIAL_VOLUME_MAX * VOL_MAX)
                * scale / (PERCENT * PERCENT);
    xa_mix(out, frames, left, right);
}

/* ---- softer bass ------------------------------------------------------ */

/*
 * The effects' bass softened as a TV's small speaker would have played it
 * (the console was heard through one): a second-order Butterworth
 * high-pass at 150 Hz on the SPU's output, before the CD's sound is added.
 * Some samples - the inventory's scroll sound - are a loud thump at
 * 125..250 Hz that makes PC speakers rattle; the filter takes about 3.5 dB
 * off it, 7 dB at 100 Hz and 16 dB at 60 Hz.  Off, the sound is the
 * console's own (tests/compare_sound.py, D_NATIVE).  The setting is
 * "Softer bass" (launcher.cpp).
 */
#define TV_BASS_CUTOFF_HZ       150.0
#define BUTTERWORTH_Q           0.70710678
#define PI                      3.14159265358979

static volatile int s_bass_cut;         /* psx_set_effects_bass_cut() */
static int s_bass_cut_running;          /* (the sound thread's view: its history is valid) */
static double s_hp_b0, s_hp_b1, s_hp_b2, s_hp_a1, s_hp_a2;
static double s_hp_x1[2], s_hp_x2[2], s_hp_y1[2], s_hp_y2[2];

void psx_set_effects_bass_cut(int on)
{
    s_bass_cut = on;
}

/* the biquad's coefficients (RBJ's audio EQ cookbook) */
static void bass_cut_init(void)
{
    double w = 2 * PI * TV_BASS_CUTOFF_HZ / (double) SPU_SAMPLE_RATE;    /* (spu.h) */
    double alpha = sin(w) / (2 * BUTTERWORTH_Q), c = cos(w), a0 = 1 + alpha;
    s_hp_b0 = (1 + c) / 2 / a0;
    s_hp_b1 = -(1 + c) / a0;
    s_hp_b2 = s_hp_b0;
    s_hp_a1 = -2 * c / a0;
    s_hp_a2 = (1 - alpha) / a0;
    for (int ch = 0; ch < 2; ch++) {
        s_hp_x1[ch] = s_hp_x2[ch] = s_hp_y1[ch] = s_hp_y2[ch] = 0;
    }
}

/* The SPU's output (stereo frames), before the CD's sound is added. */
void libsnd_filter_effects(int16_t *out, int frames)
{
    if (!s_bass_cut) {
        s_bass_cut_running = 0;
        return;
    }
    if (!s_bass_cut_running) {
        bass_cut_init();                /* (from silence: no click from an old history) */
        s_bass_cut_running = 1;
    }
    for (int i = 0; i < frames; i++) {
        for (int ch = 0; ch < 2; ch++) {
            double x = out[i * 2 + ch];
            double y = s_hp_b0 * x + s_hp_b1 * s_hp_x1[ch] + s_hp_b2 * s_hp_x2[ch]
                       - s_hp_a1 * s_hp_y1[ch] - s_hp_a2 * s_hp_y2[ch];
            s_hp_x2[ch] = s_hp_x1[ch];
            s_hp_x1[ch] = x;
            s_hp_y2[ch] = s_hp_y1[ch];
            s_hp_y1[ch] = y;
            out[i * 2 + ch] = (int16_t) (y < -32768 ? -32768 : y > 32767 ? 32767 : y);
        }
    }
}

/* ---- pitch and volume ------------------------------------------------- */

/* The SPU pitch of `note` (+ `fine` 128ths) for a tone recorded at
 * `center` (+ `shift` 128ths). */
static int pitch_from_note(int note, int fine, int center, int shift)
{
    double semitones = (note - center) + (double) (fine - shift) / FINE_STEPS;
    double pitch = SPU_PITCH_UNITY * pow(2.0, semitones / SEMITONES_PER_OCTAVE);
    return pitch > SPU_PITCH_MAX ? SPU_PITCH_MAX : (int) (pitch + 0.5);
}

static const uint8_t *program_attr(const Vab *vab, int program)
{
    return vab->header + VAB_HEADER_SIZE + program * VAB_PROGRAM_SIZE;
}

/* The tones of the VAB's programs follow the 128 program entries, 16 per
 * program that has tones, in program order: the program's place among
 * those, not its number. */
static const uint8_t *tone_attr(const Vab *vab, int program, int tone)
{
    int place = 0;
    for (int p = 0; p < program; p++) {
        place += program_attr(vab, p)[PROG_TONES] != 0;
    }
    return vab->header + VAB_HEADER_SIZE + PROGRAMS * VAB_PROGRAM_SIZE
           + (place * TONES_PER_PROGRAM + tone) * VAB_TONE_SIZE;
}

static void pan_scale(int *left, int *right, int pan)
{
    if (pan < PAN_CENTER) {
        *right = *right * pan / (PAN_CENTER - 1);
    } else {
        *left = *left * (VOL_MAX - pan) / (PAN_CENTER - 1);
    }
}

/* _SsVmKeyOnNow: velocity x VAB, tone and program volume, then the
 * caller's left/right volume, the pans, squared */
static void key_on_volume(const Vab *vab, const uint8_t *prog, const uint8_t *tone, int vol_left,
                          int vol_right, int *left, int *right)
{
    int v = VELOCITY_MAX * (vab->header[VAB_MASTER_VOLUME] * SPU_VOLUME_MAX) / (VOL_MAX * VOL_MAX);
    v = v * tone[TONE_VOL] * prog[PROG_VOL] / (VOL_MAX * VOL_MAX);
    int l = v * vol_left / VOL_MAX, r = v * vol_right / VOL_MAX;
    pan_scale(&l, &r, tone[TONE_PAN]);
    pan_scale(&l, &r, prog[PROG_PAN]);
    *left = l * l / SPU_VOLUME_MAX;
    *right = r * r / SPU_VOLUME_MAX;
}

/* ---- voices ----------------------------------------------------------- */

static int allocate_voice(void)
{
    int best = -1;
    uint32_t oldest = 0xFFFFFFFF;
    for (int i = SPU_VOICES - 1; i >= 0; i--) {
        if (!spu_voice_active(i)) {
            return i;
        }
    }
    /* steal: the oldest releasing voice, else the oldest */
    for (int pass = 0; pass < 2 && best < 0; pass++) {
        for (int i = 0; i < SPU_VOICES; i++) {
            if ((pass == 1 || spu_voice_releasing(i)) && s_owners[i].age < oldest) {
                oldest = s_owners[i].age;
                best = i;
            }
        }
    }
    return best;
}

static const Vab *vab_of(int vab_prog)
{
    int id = vab_prog >> 8;
    return id >= 0 && id < VAB_MAX && s_vabs[id].open ? &s_vabs[id] : NULL;
}

/* ---- LIBSND ----------------------------------------------------------- */

void SsInit(void)
{
    libsnd_reset();
}

void SsQuit(void)
{
}

void SsStart(void)
{
}

void SsEnd(void)
{
}

void SsSetTickMode(int tick_mode)
{
}

void SsSetMVol(int16_t left, int16_t right)
{
    s_main_left = left;
    s_main_right = right;
    apply_main_volume();
}

void SsSetSerialAttr(char serial, char attr, char mode)
{
}

void SsSetSerialVol(char serial, int16_t left, int16_t right)
{
    if (serial == SS_SERIAL_A) {
        s_serial_left = left;
        s_serial_right = right;
    }
}

int16_t SsVabOpenHead(uint8_t *header, int16_t vab_id)
{
    uint32_t magic;
    int id = vab_id;
    memcpy(&magic, header, sizeof(magic));
    if (magic != VAB_MAGIC) {
        psx_log("LIBSND: not a VAB header");
        return -1;
    }
    if (id < 0) {
        for (id = 0; id < VAB_MAX && s_vabs[id].open; id++) {
        }
    }
    if (id >= VAB_MAX) {
        return -1;
    }
    Vab *vab = &s_vabs[id];
    int programs = header[VAB_PROGRAM_COUNT] | header[VAB_PROGRAM_COUNT + 1] << 8;
    /* the sizes of the samples (in 8-byte units) follow the tones; VAG n
     * starts after the sizes of 0 .. n-1 (entry 0 is empty) */
    const uint8_t *sizes = header + VAB_HEADER_SIZE + PROGRAMS * VAB_PROGRAM_SIZE
                           + programs * TONES_PER_PROGRAM * VAB_TONE_SIZE;
    uint32_t total = 0;
    for (int i = 0; i < VAG_TABLE_ENTRIES; i++) {
        vab->vag_address[i] = SPU_ALLOC_START + total;
        total += (uint32_t) (sizes[i * 2] | sizes[i * 2 + 1] << 8) << 3;
    }
    if (SPU_ALLOC_START + total > SPU_RAM_SIZE) {
        psx_log("LIBSND: the VAB is too big for the SPU memory");
        return -1;
    }
    vab->body_size = total;
    vab->header = header;
    vab->open = 1;
    return (int16_t) id;
}

int16_t SsVabTransBody(uint8_t *body, int16_t vab_id)
{
    if (vab_id < 0 || vab_id >= VAB_MAX || !s_vabs[vab_id].open) {
        return -1;
    }
    memcpy(&g_spu_ram[SPU_ALLOC_START], body, s_vabs[vab_id].body_size);
    return vab_id;
}

int16_t SsVabTransCompleted(int16_t immediate)
{
    return 1;
}

void SsVabClose(int16_t vab_id)
{
    if (vab_id >= 0 && vab_id < VAB_MAX) {
        s_vabs[vab_id].open = 0;
    }
}

/* vab_prog = (VAB << 8) | program, pitch = (note << 8) | fine tune */
int16_t SsVoKeyOn(int32_t vab_prog, int32_t pitch, uint16_t volume_l, uint16_t volume_r)
{
    const Vab *vab = vab_of(vab_prog);
    int program = vab_prog & 0xFF;
    int note = pitch >> 8, fine = pitch & 0xFF;
    if (!vab || program >= PROGRAMS) {
        return -1;
    }
    const uint8_t *prog = program_attr(vab, program);
    int tones = prog[PROG_TONES];
    if (getenv("D_SOUND_LOG")) {
        psx_log("SsVoKeyOn: program %d note %d, %d tones", program, note, tones);
        for (int t = 0; t < tones && t < TONES_PER_PROGRAM; t++) {
            const uint8_t *tone = tone_attr(vab, program, t);
            psx_log("  tone %d (of program %d): notes %d..%d centre %d vag %d vol %d adsr %02x%02x %02x%02x",
                    t, tone[TONE_PROG], tone[TONE_MIN], tone[TONE_MAX], tone[TONE_CENTER], tone[TONE_VAG],
                    tone[TONE_VOL],
                    tone[TONE_ADSR1 + 1], tone[TONE_ADSR1], tone[TONE_ADSR2 + 1], tone[TONE_ADSR2]);
        }
    }
    for (int t = 0; t < tones && t < TONES_PER_PROGRAM; t++) {
        const uint8_t *tone = tone_attr(vab, program, t);
        if (note < tone[TONE_MIN] || note > tone[TONE_MAX]) {
            continue;
        }
        int voice = allocate_voice();
        if (voice < 0) {
            break;
        }
        int left, right;
        int vag = tone[TONE_VAG] | tone[TONE_VAG + 1] << 8;
        key_on_volume(vab, prog, tone, volume_l, volume_r, &left, &right);
        spu_voice_set_volume(voice, left, right);
        spu_voice_set_pitch(voice, pitch_from_note(note, fine, tone[TONE_CENTER], tone[TONE_SHIFT]));
        spu_voice_set_start(voice, vab->vag_address[vag & 0xFF]);
        spu_voice_set_adsr(voice, (uint16_t) (tone[TONE_ADSR1] | tone[TONE_ADSR1 + 1] << 8),
                           (uint16_t) (tone[TONE_ADSR2] | tone[TONE_ADSR2 + 1] << 8));
        spu_voice_set_reverb(voice, (tone[TONE_MODE] & TONE_MODE_REVERB) != 0);
        spu_key_on(1u << voice);
        s_owners[voice].vab_prog = vab_prog;
        s_owners[voice].note = note;
        s_owners[voice].age = ++s_age;
    }
    return 0;
}

int16_t SsVoKeyOff(int32_t vab_prog, int32_t pitch)
{
    uint32_t voices = 0;
    for (int i = 0; i < SPU_VOICES; i++) {
        if (s_owners[i].vab_prog == vab_prog && s_owners[i].note == pitch >> 8) {
            voices |= 1u << i;
            s_owners[i].vab_prog = -1;
        }
    }
    spu_key_off(voices);
    return 0;
}

/* (a save state: savestate.c; the player's volume settings stay) */
void libsnd_state(StateIO *io)
{
    state_section(io, "SND ");
    STATE(io, s_vabs);
    STATE(io, s_owners);
    STATE(io, s_age);
    STATE(io, s_main_left);
    STATE(io, s_main_right);
    STATE(io, s_serial_left);
    STATE(io, s_serial_right);
    if (io->loading) {
        apply_main_volume();
    }
}
