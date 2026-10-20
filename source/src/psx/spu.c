/*
 * spu.c - the PS1 sound processor in software (see spu.h).
 *
 * After the "SPU" chapter of psx-spx (nocash): 16-byte ADPCM blocks of
 * 28 samples with the five prediction filters and loop flags; the ADSR
 * envelope with its linear/exponential steps; a pitch counter in 4.12
 * (1000h = 44.1 kHz); the reverb at 22.05 kHz with its 32 registers.
 * Differences: samples are interpolated with a cubic B-spline (the
 * hardware's Gaussian table is not reproduced), and the reverb's input
 * and output are not FIR-resampled.
 */
#include <string.h>
#include "psx/spu.h"
#include "psx/savestate.h"

uint8_t g_spu_ram[SPU_RAM_SIZE];

#define BLOCK_SIZE              16
#define BLOCK_SAMPLES           28
#define PITCH_MAX               0x4000
#define LEVEL_MAX               0x7FFF

/* ADPCM block flags */
#define FLAG_LOOP_END           1
#define FLAG_LOOP_REPEAT        2
#define FLAG_LOOP_START         4

enum Phase { PHASE_OFF, PHASE_ATTACK, PHASE_DECAY, PHASE_SUSTAIN, PHASE_RELEASE };

typedef struct {
    uint32_t start, repeat, current;    /* byte addresses */
    int16_t block[BLOCK_SAMPLES];
    int block_pos;
    int block_flags;
    int32_t hist1, hist2;               /* ADPCM prediction */
    int16_t last[4];                    /* the last samples, for interpolation */
    uint32_t counter;                   /* pitch counter, 12 fraction bits */
    int pitch;
    int volume[2];
    int reverb;
    uint16_t adsr1, adsr2;
    int phase;
    int32_t level;                      /* envelope 0..7FFFh */
    int32_t wait;                       /* samples until the next envelope step */
} Voice;

static Voice s_voices[SPU_VOICES];
static int s_main_volume[2];
static int s_reverb_volume[2];
static int s_reverb_on;
static int16_t s_reverb[32];            /* dAPF1 .. vRIN as signed values */
static uint32_t s_reverb_base, s_reverb_current;
static int32_t s_reverb_in[2], s_reverb_out[2];
static int s_reverb_phase;

/* reverb register indices */
enum {
    R_DAPF1, R_DAPF2, R_VIIR, R_VCOMB1, R_VCOMB2, R_VCOMB3, R_VCOMB4, R_VWALL,
    R_VAPF1, R_VAPF2, R_MLSAME, R_MRSAME, R_MLCOMB1, R_MRCOMB1, R_MLCOMB2, R_MRCOMB2,
    R_DLSAME, R_DRSAME, R_MLDIFF, R_MRDIFF, R_MLCOMB3, R_MRCOMB3, R_MLCOMB4, R_MRCOMB4,
    R_DLDIFF, R_DRDIFF, R_MLAPF1, R_MRAPF1, R_MLAPF2, R_MRAPF2, R_VLIN, R_VRIN
};

static const int FILTER_POS[5] = { 0, 60, 115, 98, 122 };
static const int FILTER_NEG[5] = { 0, 0, -52, -55, -60 };

void spu_reset(void)
{
    memset(g_spu_ram, 0, sizeof(g_spu_ram));
    memset(s_voices, 0, sizeof(s_voices));
    memset(s_reverb, 0, sizeof(s_reverb));
    s_main_volume[0] = s_main_volume[1] = 0;
    s_reverb_volume[0] = s_reverb_volume[1] = 0;
    s_reverb_on = 0;
    s_reverb_base = s_reverb_current = SPU_RAM_SIZE - 2;
    s_reverb_in[0] = s_reverb_in[1] = s_reverb_out[0] = s_reverb_out[1] = 0;
    s_reverb_phase = 0;
}

static int16_t clamp16(int32_t v)
{
    return (int16_t) (v < -0x8000 ? -0x8000 : v > 0x7FFF ? 0x7FFF : v);
}

/* ---- voice registers -------------------------------------------------- */

void spu_voice_set_volume(int voice, int left, int right)
{
    s_voices[voice].volume[0] = left;
    s_voices[voice].volume[1] = right;
}

void spu_voice_set_pitch(int voice, int pitch)
{
    s_voices[voice].pitch = pitch & 0xFFFF;
}

void spu_voice_set_start(int voice, uint32_t address)
{
    s_voices[voice].start = address & (SPU_RAM_SIZE - BLOCK_SIZE);
}

void spu_voice_set_adsr(int voice, uint16_t adsr1, uint16_t adsr2)
{
    s_voices[voice].adsr1 = adsr1;
    s_voices[voice].adsr2 = adsr2;
}

void spu_voice_set_reverb(int voice, int on)
{
    s_voices[voice].reverb = on;
}

static void decode_block(Voice *v)
{
    const uint8_t *b = &g_spu_ram[v->current & (SPU_RAM_SIZE - BLOCK_SIZE)];
    int shift = b[0] & 0xF, filter = (b[0] >> 4) & 7;
    if (shift > 12) {
        shift = 9;
    }
    if (filter > 4) {
        filter = 4;
    }
    v->block_flags = b[1];
    if (v->block_flags & FLAG_LOOP_START) {
        v->repeat = v->current;
    }
    for (int i = 0; i < BLOCK_SAMPLES; i++) {
        int nibble = (b[2 + i / 2] >> ((i & 1) * 4)) & 0xF;
        int32_t s = (int32_t) (int16_t) (nibble << 12) >> shift;
        s += (v->hist1 * FILTER_POS[filter] + v->hist2 * FILTER_NEG[filter] + 32) >> 6;
        s = clamp16(s);
        v->hist2 = v->hist1;
        v->hist1 = s;
        v->block[i] = (int16_t) s;
    }
    v->block_pos = 0;
}

void spu_key_on(uint32_t voices)
{
    for (int i = 0; i < SPU_VOICES; i++) {
        if (voices & (1u << i)) {
            Voice *v = &s_voices[i];
            v->current = v->start;
            v->repeat = v->start;
            v->hist1 = v->hist2 = 0;
            memset(v->last, 0, sizeof(v->last));
            v->counter = 0;
            v->phase = PHASE_ATTACK;
            v->level = 0;
            v->wait = 0;
            decode_block(v);
        }
    }
}

void spu_key_off(uint32_t voices)
{
    for (int i = 0; i < SPU_VOICES; i++) {
        if ((voices & (1u << i)) && s_voices[i].phase != PHASE_OFF) {
            s_voices[i].phase = PHASE_RELEASE;
            s_voices[i].wait = 0;
        }
    }
}

int spu_voice_active(int voice)
{
    return s_voices[voice].phase != PHASE_OFF;
}

int spu_voice_releasing(int voice)
{
    return s_voices[voice].phase == PHASE_RELEASE || s_voices[voice].phase == PHASE_OFF;
}

void spu_set_main_volume(int left, int right)
{
    s_main_volume[0] = left;
    s_main_volume[1] = right;
}

void spu_set_reverb_volume(int left, int right)
{
    s_reverb_volume[0] = left;
    s_reverb_volume[1] = right;
}

void spu_set_reverb_enabled(int on)
{
    s_reverb_on = on;
}

void spu_set_reverb_registers(const uint16_t regs[32], uint32_t base_address)
{
    for (int i = 0; i < 32; i++) {
        s_reverb[i] = (int16_t) regs[i];
    }
    s_reverb_base = base_address & 0x7FFFE;
    s_reverb_current = s_reverb_base;
}

void spu_clear_reverb_area(void)
{
    memset(&g_spu_ram[s_reverb_base], 0, SPU_RAM_SIZE - s_reverb_base);
}

/* ---- envelope --------------------------------------------------------- */

static void envelope_step(Voice *v)
{
    int exponential, decreasing, shift, step;
    switch (v->phase) {
    case PHASE_ATTACK:
        exponential = v->adsr1 >> 15;
        decreasing = 0;
        shift = (v->adsr1 >> 10) & 0x1F;
        step = 7 - ((v->adsr1 >> 8) & 3);
        break;
    case PHASE_DECAY:
        exponential = 1;
        decreasing = 1;
        shift = ((v->adsr1 >> 4) & 0xF) << 2;
        step = -8;
        break;
    case PHASE_SUSTAIN:
        exponential = v->adsr2 >> 15;
        decreasing = (v->adsr2 >> 14) & 1;
        shift = (v->adsr2 >> 8) & 0x1F;
        step = decreasing ? -8 + ((v->adsr2 >> 6) & 3) : 7 - ((v->adsr2 >> 6) & 3);
        break;
    case PHASE_RELEASE:
        exponential = (v->adsr2 >> 5) & 1;
        decreasing = 1;
        shift = v->adsr2 & 0x1F;
        step = -8;
        break;
    default:
        return;
    }
    if (--v->wait > 0) {
        return;
    }
    int32_t cycles = 1 << (shift > 11 ? shift - 11 : 0);
    int32_t delta = step << (shift < 11 ? 11 - shift : 0);
    if (exponential && !decreasing && v->level > 0x6000) {
        cycles *= 4;
    }
    if (exponential && decreasing) {
        delta = (delta * v->level) >> 15;
    }
    v->wait = cycles;
    v->level += delta;
    if (v->level > LEVEL_MAX) {
        v->level = LEVEL_MAX;
    }
    if (v->level < 0) {
        v->level = 0;
    }

    int32_t sustain = ((v->adsr1 & 0xF) + 1) * 0x800;
    switch (v->phase) {
    case PHASE_ATTACK:
        if (v->level >= LEVEL_MAX) {
            v->phase = PHASE_DECAY;
            v->wait = 0;
        }
        break;
    case PHASE_DECAY:
        if (v->level <= (sustain > LEVEL_MAX ? LEVEL_MAX : sustain)) {
            v->phase = PHASE_SUSTAIN;
            v->wait = 0;
        }
        break;
    case PHASE_RELEASE:
        if (v->level <= 0) {
            v->phase = PHASE_OFF;
        }
        break;
    }
}

/* ---- samples ---------------------------------------------------------- */

/* the next sample of a voice's stream (blocks, loops) */
static int16_t next_sample(Voice *v)
{
    if (v->block_pos >= BLOCK_SAMPLES) {
        if (v->block_flags & FLAG_LOOP_END) {
            v->current = v->repeat;
            if (!(v->block_flags & FLAG_LOOP_REPEAT)) {
                v->phase = PHASE_OFF;           /* (release with level 0) */
                v->level = 0;
            }
        } else {
            v->current = (v->current + BLOCK_SIZE) & (SPU_RAM_SIZE - 1);
        }
        decode_block(v);
    }
    return v->block[v->block_pos++];
}

/* cubic B-spline through the last four samples, t in 0..FFFh */
static int32_t interpolate(const int16_t s[4], uint32_t t)
{
    double x = t / 4096.0, x2 = x * x, x3 = x2 * x;
    double w0 = (1 - x) * (1 - x) * (1 - x) / 6;
    double w1 = (3 * x3 - 6 * x2 + 4) / 6;
    double w2 = (-3 * x3 + 3 * x2 + 3 * x + 1) / 6;
    double w3 = x3 / 6;
    return (int32_t) (s[0] * w0 + s[1] * w1 + s[2] * w2 + s[3] * w3);
}

/* ---- reverb ----------------------------------------------------------- */

static uint32_t reverb_address(int32_t offset_bytes)
{
    uint32_t size = SPU_RAM_SIZE - s_reverb_base;
    int32_t a = (int32_t) (s_reverb_current - s_reverb_base) + offset_bytes;
    a %= (int32_t) size;
    if (a < 0) {
        a += (int32_t) size;
    }
    return s_reverb_base + (uint32_t) a;
}

static int32_t reverb_read(int reg, int extra_bytes)
{
    uint32_t a = reverb_address((uint16_t) s_reverb[reg] * 8 + extra_bytes) & ~1u;
    return (int16_t) (g_spu_ram[a] | g_spu_ram[a + 1] << 8);
}

static void reverb_write(int reg, int32_t value)
{
    uint32_t a = reverb_address((uint16_t) s_reverb[reg] * 8) & ~1u;
    int16_t v = clamp16(value);
    g_spu_ram[a] = (uint8_t) v;
    g_spu_ram[a + 1] = (uint8_t) (v >> 8);
}

static int32_t mul(int32_t a, int reg)
{
    return (a * s_reverb[reg]) >> 15;
}

/* one 22.05 kHz step of the reverb */
static void reverb_step(int32_t in_left, int32_t in_right)
{
    int32_t lin = mul(in_left, R_VLIN), rin = mul(in_right, R_VRIN);
    int32_t prev;

    prev = reverb_read(R_MLSAME, -2);
    reverb_write(R_MLSAME, mul(lin + mul(reverb_read(R_DLSAME, 0), R_VWALL) - prev, R_VIIR) + prev);
    prev = reverb_read(R_MRSAME, -2);
    reverb_write(R_MRSAME, mul(rin + mul(reverb_read(R_DRSAME, 0), R_VWALL) - prev, R_VIIR) + prev);
    prev = reverb_read(R_MLDIFF, -2);
    reverb_write(R_MLDIFF, mul(lin + mul(reverb_read(R_DRDIFF, 0), R_VWALL) - prev, R_VIIR) + prev);
    prev = reverb_read(R_MRDIFF, -2);
    reverb_write(R_MRDIFF, mul(rin + mul(reverb_read(R_DLDIFF, 0), R_VWALL) - prev, R_VIIR) + prev);

    int32_t l = mul(reverb_read(R_MLCOMB1, 0), R_VCOMB1) + mul(reverb_read(R_MLCOMB2, 0), R_VCOMB2)
                + mul(reverb_read(R_MLCOMB3, 0), R_VCOMB3) + mul(reverb_read(R_MLCOMB4, 0), R_VCOMB4);
    int32_t r = mul(reverb_read(R_MRCOMB1, 0), R_VCOMB1) + mul(reverb_read(R_MRCOMB2, 0), R_VCOMB2)
                + mul(reverb_read(R_MRCOMB3, 0), R_VCOMB3) + mul(reverb_read(R_MRCOMB4, 0), R_VCOMB4);

    int32_t d1 = -(uint16_t) s_reverb[R_DAPF1] * 8, d2 = -(uint16_t) s_reverb[R_DAPF2] * 8;
    int32_t t;
    t = reverb_read(R_MLAPF1, d1);
    l -= mul(t, R_VAPF1);
    reverb_write(R_MLAPF1, l);
    l = mul(l, R_VAPF1) + t;
    t = reverb_read(R_MRAPF1, d1);
    r -= mul(t, R_VAPF1);
    reverb_write(R_MRAPF1, r);
    r = mul(r, R_VAPF1) + t;
    t = reverb_read(R_MLAPF2, d2);
    l -= mul(t, R_VAPF2);
    reverb_write(R_MLAPF2, l);
    l = mul(l, R_VAPF2) + t;
    t = reverb_read(R_MRAPF2, d2);
    r -= mul(t, R_VAPF2);
    reverb_write(R_MRAPF2, r);
    r = mul(r, R_VAPF2) + t;

    s_reverb_out[0] = clamp16((l * s_reverb_volume[0]) >> 15);
    s_reverb_out[1] = clamp16((r * s_reverb_volume[1]) >> 15);
    s_reverb_current = reverb_address(2);
}

/* ---- mixing ----------------------------------------------------------- */

void spu_render(int16_t *out, int frames)
{
    for (int f = 0; f < frames; f++) {
        int32_t mix[2] = { 0, 0 }, rev[2] = { 0, 0 };
        for (int i = 0; i < SPU_VOICES; i++) {
            Voice *v = &s_voices[i];
            if (v->phase == PHASE_OFF) {
                continue;
            }
            int32_t s = interpolate(v->last, v->counter & 0xFFF);
            envelope_step(v);
            s = (s * v->level) >> 15;
            for (int c = 0; c < 2; c++) {
                int32_t o = (s * (v->volume[c] * 2)) >> 15;
                mix[c] += o;
                if (v->reverb) {
                    rev[c] += o;
                }
            }
            v->counter += (uint32_t) (v->pitch > PITCH_MAX ? PITCH_MAX : v->pitch);
            while (v->counter >= 0x1000 && v->phase != PHASE_OFF) {
                v->counter -= 0x1000;
                v->last[0] = v->last[1];
                v->last[1] = v->last[2];
                v->last[2] = v->last[3];
                v->last[3] = next_sample(v);
            }
        }
        /* the reverb runs at half the rate */
        s_reverb_in[0] += rev[0];
        s_reverb_in[1] += rev[1];
        if (++s_reverb_phase == 2) {
            s_reverb_phase = 0;
            if (s_reverb_on) {
                reverb_step(clamp16(s_reverb_in[0] / 2), clamp16(s_reverb_in[1] / 2));
            } else {
                s_reverb_out[0] = s_reverb_out[1] = 0;
            }
            s_reverb_in[0] = s_reverb_in[1] = 0;
        }
        for (int c = 0; c < 2; c++) {
            int32_t total = clamp16(mix[c]) + s_reverb_out[c];
            out[f * 2 + c] = clamp16((total * (s_main_volume[c] * 2)) >> 15);
        }
    }
}

/* (a save state: savestate.c) */
void spu_state(StateIO *io)
{
    state_section(io, "SPU ");
    STATE(io, g_spu_ram);
    STATE(io, s_voices);
    STATE(io, s_main_volume);
    STATE(io, s_reverb_volume);
    STATE(io, s_reverb_on);
    STATE(io, s_reverb);
    STATE(io, s_reverb_base);
    STATE(io, s_reverb_current);
    STATE(io, s_reverb_in);
    STATE(io, s_reverb_out);
    STATE(io, s_reverb_phase);
}
