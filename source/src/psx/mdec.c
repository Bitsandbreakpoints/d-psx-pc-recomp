/*
 * mdec.c - the movie decoder in software: the STR bitstream's VLC codes
 * (what LIBPRESS's DecDCTvlc does on the CPU) and the MDEC's work
 * (dequantisation, inverse DCT, YCbCr to RGB).
 *
 * After psx-spx ("MDEC") and the MPEG-1 standard, whose variable length
 * codes and default quantisation matrix the PS1 uses:
 *  - a frame's bitstream: a 4 halfword header (code count, 0x3800, the
 *    quantisation scale, version), then 16-bit little-endian words read
 *    from the most significant bit.  Each 8x8 block: in version 2 the DC
 *    coefficient as 10 signed bits, then AC codes (MPEG-1 table B.14,
 *    "dct_coeff_next") until the end-of-block code; the escape code is
 *    followed by a 6-bit run and a 10-bit signed level.
 *  - the MDEC's run-length codes: per block a DC code (scale << 10 | DC),
 *    then (run << 10 | level) codes, ended by 0xFE00.  Six blocks per
 *    16x16 macroblock: Cr, Cb, then the four Y blocks; the macroblocks in
 *    columns, top to bottom.
 *
 * Only this file reads the codes it writes, so after the MDEC's header
 * word comes one more with the frame's size (from the STR sector header).
 */
#include <math.h>
#include <string.h>
#include "psx/mdec.h"
#include "psx/log.h"

#define BLOCK_SIZE              8
#define BLOCK_COEFFS            64
#define BLOCKS_PER_MACROBLOCK   6
#define MDEC_HEADER             0x38000000u     /* decode command (upper half: 0x3800) */
#define CODES_START             2               /* words: the header, the frame size */
#define CODE_END_OF_BLOCK       0xFE00u
#define CODE_RUN_SHIFT          10
#define CODE_VALUE_MASK         0x3FF
#define SIGNED_10_BIT(x)        ((int) ((x) & 0x3FF) - (((x) & 0x200) ? 0x400 : 0))
#define COEFF_MIN               (-0x400)
#define COEFF_MAX               0x3FF

/* the bitstream header (halfwords) */
#define HDR_CODE_COUNT          0
#define HDR_MAGIC               1
#define HDR_QSCALE              2
#define HDR_VERSION             3
#define HDR_WORDS               4
#define BITSTREAM_MAGIC         0x3800
#define DC_BITS_V2              10

/* ---- variable length codes (MPEG-1 table B.14, without the sign bit) - */

typedef struct {
    const char *code;
    uint8_t run, level;
} AcCode;

#define ESCAPE_CODE             "000001"
#define ESCAPE_RUN_BITS         6
#define ESCAPE_LEVEL_BITS       10
#define END_OF_BLOCK_CODE       "10"
#define CODE_MAX_BITS           16

static const AcCode AC_CODES[] = {
    { "11", 0, 1 }, { "011", 1, 1 }, { "0100", 0, 2 }, { "0101", 2, 1 },
    { "00101", 0, 3 }, { "00110", 4, 1 }, { "00111", 3, 1 },
    { "000100", 7, 1 }, { "000101", 6, 1 }, { "000110", 1, 2 }, { "000111", 5, 1 },
    { "0000100", 2, 2 }, { "0000101", 9, 1 }, { "0000110", 0, 4 }, { "0000111", 8, 1 },
    { "00100000", 13, 1 }, { "00100001", 0, 6 }, { "00100010", 12, 1 }, { "00100011", 11, 1 },
    { "00100100", 3, 2 }, { "00100101", 1, 3 }, { "00100110", 0, 5 }, { "00100111", 10, 1 },
    { "0000001000", 16, 1 }, { "0000001001", 5, 2 }, { "0000001010", 0, 7 },
    { "0000001011", 2, 3 }, { "0000001100", 1, 4 }, { "0000001101", 15, 1 },
    { "0000001110", 14, 1 }, { "0000001111", 4, 2 },
    { "000000010000", 0, 11 }, { "000000010001", 8, 2 }, { "000000010010", 4, 3 },
    { "000000010011", 0, 10 }, { "000000010100", 2, 4 }, { "000000010101", 7, 2 },
    { "000000010110", 21, 1 }, { "000000010111", 20, 1 }, { "000000011000", 0, 9 },
    { "000000011001", 19, 1 }, { "000000011010", 18, 1 }, { "000000011011", 1, 5 },
    { "000000011100", 3, 3 }, { "000000011101", 0, 8 }, { "000000011110", 6, 2 },
    { "000000011111", 17, 1 },
    { "0000000010000", 10, 2 }, { "0000000010001", 9, 2 }, { "0000000010010", 5, 3 },
    { "0000000010011", 3, 4 }, { "0000000010100", 2, 5 }, { "0000000010101", 1, 7 },
    { "0000000010110", 1, 6 }, { "0000000010111", 0, 15 }, { "0000000011000", 0, 14 },
    { "0000000011001", 0, 13 }, { "0000000011010", 0, 12 }, { "0000000011011", 26, 1 },
    { "0000000011100", 25, 1 }, { "0000000011101", 24, 1 }, { "0000000011110", 23, 1 },
    { "0000000011111", 22, 1 },
    { "00000000010000", 0, 31 }, { "00000000010001", 0, 30 }, { "00000000010010", 0, 29 },
    { "00000000010011", 0, 28 }, { "00000000010100", 0, 27 }, { "00000000010101", 0, 26 },
    { "00000000010110", 0, 25 }, { "00000000010111", 0, 24 }, { "00000000011000", 0, 23 },
    { "00000000011001", 0, 22 }, { "00000000011010", 0, 21 }, { "00000000011011", 0, 20 },
    { "00000000011100", 0, 19 }, { "00000000011101", 0, 18 }, { "00000000011110", 0, 17 },
    { "00000000011111", 0, 16 },
    { "000000000010000", 0, 40 }, { "000000000010001", 0, 39 }, { "000000000010010", 0, 38 },
    { "000000000010011", 0, 37 }, { "000000000010100", 0, 36 }, { "000000000010101", 0, 35 },
    { "000000000010110", 0, 34 }, { "000000000010111", 0, 33 }, { "000000000011000", 0, 32 },
    { "000000000011001", 1, 14 }, { "000000000011010", 1, 13 }, { "000000000011011", 1, 12 },
    { "000000000011100", 1, 11 }, { "000000000011101", 1, 10 }, { "000000000011110", 1, 9 },
    { "000000000011111", 1, 8 },
    { "0000000000010000", 1, 18 }, { "0000000000010001", 1, 17 },
    { "0000000000010010", 1, 16 }, { "0000000000010011", 1, 15 },
    { "0000000000010100", 6, 3 }, { "0000000000010101", 16, 2 },
    { "0000000000010110", 15, 2 }, { "0000000000010111", 14, 2 },
    { "0000000000011000", 13, 2 }, { "0000000000011001", 12, 2 },
    { "0000000000011010", 11, 2 }, { "0000000000011011", 31, 1 },
    { "0000000000011100", 30, 1 }, { "0000000000011101", 29, 1 },
    { "0000000000011110", 28, 1 }, { "0000000000011111", 27, 1 },
};

/* what the next 16 bits start with */
enum { LOOKUP_INVALID = 0, LOOKUP_CODE, LOOKUP_ESCAPE, LOOKUP_END_OF_BLOCK };

typedef struct {
    uint8_t kind;
    uint8_t length;                     /* bits of the code, without the sign */
    uint8_t run, level;
} Lookup;

static Lookup s_lookup[1 << CODE_MAX_BITS];
static int s_lookup_ready;

static void fill_lookup(const char *code, Lookup entry)
{
    int length = (int) strlen(code);
    unsigned prefix = 0;
    for (int i = 0; i < length; i++) {
        prefix = prefix << 1 | (unsigned) (code[i] - '0');
    }
    unsigned first = prefix << (CODE_MAX_BITS - length);
    unsigned count = 1u << (CODE_MAX_BITS - length);
    entry.length = (uint8_t) length;
    for (unsigned i = 0; i < count; i++) {
        s_lookup[first + i] = entry;
    }
}

static void build_lookup(void)
{
    Lookup entry = { 0 };
    for (size_t i = 0; i < sizeof(AC_CODES) / sizeof(AC_CODES[0]); i++) {
        entry.kind = LOOKUP_CODE;
        entry.run = AC_CODES[i].run;
        entry.level = AC_CODES[i].level;
        fill_lookup(AC_CODES[i].code, entry);
    }
    entry.kind = LOOKUP_ESCAPE;
    entry.run = entry.level = 0;
    fill_lookup(ESCAPE_CODE, entry);
    entry.kind = LOOKUP_END_OF_BLOCK;
    fill_lookup(END_OF_BLOCK_CODE, entry);
    s_lookup_ready = 1;
}

/* ---- bit reader: 16-bit little-endian words, most significant bit first */

typedef struct {
    const uint16_t *words;
    int count;                          /* words in the stream */
    int position;                       /* next word */
    uint32_t bits;                      /* the next bits, at the top */
    int available;
} BitReader;

static void refill(BitReader *r)
{
    while (r->available <= 16) {
        uint32_t word = r->position < r->count ? r->words[r->position] : 0;
        r->position++;
        r->bits |= word << (16 - r->available);
        r->available += 16;
    }
}

static unsigned peek(BitReader *r, int n)
{
    refill(r);
    return r->bits >> (32 - n);
}

static unsigned take(BitReader *r, int n)
{
    unsigned value = peek(r, n);
    r->bits <<= n;
    r->available -= n;
    return value;
}

/* ---- DecDCTvlc -------------------------------------------------------- */

static int round_up_macroblock(int n)
{
    return (n + MDEC_MACROBLOCK - 1) / MDEC_MACROBLOCK;
}

int mdec_vlc_decode(const uint32_t *bitstream, int width, int height, uint32_t *out,
                    int out_words)
{
    const uint16_t *header = (const uint16_t *) bitstream;
    uint16_t *codes = (uint16_t *) (out + CODES_START);
    int max_codes = (out_words - CODES_START) * 2;
    int n = 0;

    if (!s_lookup_ready) {
        build_lookup();
    }
    if (header[HDR_MAGIC] != BITSTREAM_MAGIC || header[HDR_VERSION] != 2) {
        psx_log("mdec: bitstream version %u not supported", header[HDR_VERSION]);
        return 0;
    }
    int qscale = header[HDR_QSCALE];
    int blocks = round_up_macroblock(width) * round_up_macroblock(height) * BLOCKS_PER_MACROBLOCK;
    /* (the header's code count is half the number of codes, so the
     * bitstream is shorter than that many words) */
    BitReader r = { header + HDR_WORDS, header[HDR_CODE_COUNT] * 2, 0, 0, 0 };

    for (int b = 0; b < blocks; b++) {
        /* the DC coefficient */
        unsigned dc = take(&r, DC_BITS_V2);
        codes[n++] = (uint16_t) (qscale << CODE_RUN_SHIFT | (dc & CODE_VALUE_MASK));
        /* the AC coefficients, until the end of the block */
        for (;;) {
            if (n >= max_codes - 1) {
                psx_log("mdec: the frame does not fit the buffer");
                return 0;
            }
            const Lookup *l = &s_lookup[peek(&r, CODE_MAX_BITS)];
            int run, level;
            if (l->kind == LOOKUP_END_OF_BLOCK || l->kind == LOOKUP_INVALID) {
                take(&r, l->kind == LOOKUP_INVALID ? CODE_MAX_BITS : l->length);
                codes[n++] = CODE_END_OF_BLOCK;
                break;
            }
            take(&r, l->length);
            if (l->kind == LOOKUP_CODE) {
                run = l->run;
                level = take(&r, 1) ? -l->level : l->level;
            } else {
                run = (int) take(&r, ESCAPE_RUN_BITS);
                unsigned raw = take(&r, ESCAPE_LEVEL_BITS);     /* (the macro reads it twice) */
                level = SIGNED_10_BIT(raw);
            }
            codes[n++] = (uint16_t) (run << CODE_RUN_SHIFT | (level & CODE_VALUE_MASK));
        }
    }
    if (n & 1) {
        codes[n++] = CODE_END_OF_BLOCK;     /* (whole words) */
    }
    out[0] = MDEC_HEADER | (uint32_t) (n / 2);
    out[1] = (uint32_t) width | (uint32_t) height << 16;
    return n / 2 + CODES_START;
}

/* ---- the MDEC --------------------------------------------------------- */

/* zigzag index -> position in the block (row * 8 + column) */
static const uint8_t ZIGZAG[BLOCK_COEFFS] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

/* MPEG-1's default intra quantisation matrix (row-major), with 2 for the
 * DC coefficient as the PS1 uses it */
static const uint8_t DEFAULT_QUANT[BLOCK_COEFFS] = {
    2, 16, 19, 22, 26, 27, 29, 34,
    16, 16, 22, 24, 27, 29, 34, 37,
    19, 22, 26, 27, 29, 34, 34, 38,
    22, 22, 26, 27, 29, 34, 37, 40,
    22, 26, 27, 29, 32, 35, 40, 48,
    26, 27, 29, 32, 35, 40, 48, 58,
    26, 27, 29, 34, 38, 46, 56, 69,
    27, 29, 35, 38, 46, 56, 69, 83,
};

/*
 * The MDEC's inverse DCT, in integers as the chip does it (and as the
 * accurate emulators do): the scale table LIBPRESS loads (DecDCTReset):
 * 32768 * C(u) * cos((2y + 1) u pi / 16), rounded down, C(0) = 1/sqrt(2);
 * two passes with 64-bit sums, the result from bit 32 up, rounded, as a
 * 9-bit signed value clamped to -128..127.
 */
#define SCALE_ONE               32768.0
#define SAMPLE_MIN              (-128)
#define SAMPLE_MAX              127
#define RESULT_SHIFT            32

static int16_t s_scale[BLOCK_COEFFS];   /* [u * 8 + y] */
static int s_scale_ready;

static void build_scale_table(void)
{
    for (int u = 0; u < BLOCK_SIZE; u++) {
        double c = u == 0 ? sqrt(0.5) : 1.0;
        for (int y = 0; y < BLOCK_SIZE; y++) {
            s_scale[u * BLOCK_SIZE + y] =
                (int16_t) floor(SCALE_ONE * c * cos((2 * y + 1) * u * M_PI / 16));
        }
    }
    s_scale_ready = 1;
}

typedef struct {
    const uint16_t *codes;
    int count, position;
} CodeReader;

static int sign_extend_9(int64_t v)
{
    int x = (int) (v & 0x1FF);
    return x & 0x100 ? x - 0x200 : x;
}

/* Decodes one block's codes into 64 samples (-128..127), [y * 8 + x]. */
static int decode_block(CodeReader *r, int16_t out[BLOCK_COEFFS])
{
    int16_t coeff[BLOCK_COEFFS] = { 0 };
    uint16_t code;

    /* (the MDEC skips padding codes before a block) */
    do {
        if (r->position >= r->count) {
            return 0;
        }
        code = r->codes[r->position++];
    } while (code == CODE_END_OF_BLOCK);
    int qscale = code >> CODE_RUN_SHIFT;
    int k = 0;
    int value = SIGNED_10_BIT(code) * DEFAULT_QUANT[0];
    for (;;) {
        if (qscale == 0) {
            value = SIGNED_10_BIT(code) * 2;    /* special mode: no matrix, no zigzag */
        }
        value = value < COEFF_MIN ? COEFF_MIN : value > COEFF_MAX ? COEFF_MAX : value;
        coeff[qscale ? ZIGZAG[k] : k] = (int16_t) value;
        if (r->position >= r->count) {
            break;
        }
        code = r->codes[r->position++];
        if (code == CODE_END_OF_BLOCK) {
            break;
        }
        k += (code >> CODE_RUN_SHIFT) + 1;
        if (k >= BLOCK_COEFFS) {
            break;
        }
        value = (SIGNED_10_BIT(code) * DEFAULT_QUANT[ZIGZAG[k]] * qscale + 4) / 8;
    }
    /* columns (vertical frequencies -> lines), then lines */
    int64_t temp[BLOCK_COEFFS];
    for (int x = 0; x < BLOCK_SIZE; x++) {
        for (int y = 0; y < BLOCK_SIZE; y++) {
            int64_t sum = 0;
            for (int u = 0; u < BLOCK_SIZE; u++) {
                sum += (int64_t) coeff[u * BLOCK_SIZE + x] * s_scale[u * BLOCK_SIZE + y];
            }
            temp[x + y * BLOCK_SIZE] = sum;
        }
    }
    for (int x = 0; x < BLOCK_SIZE; x++) {
        for (int y = 0; y < BLOCK_SIZE; y++) {
            int64_t sum = 0;
            for (int u = 0; u < BLOCK_SIZE; u++) {
                sum += temp[u + y * BLOCK_SIZE] * s_scale[u * BLOCK_SIZE + x];
            }
            int v = sign_extend_9((sum >> RESULT_SHIFT) + ((sum >> (RESULT_SHIFT - 1)) & 1));
            out[x + y * BLOCK_SIZE] = (int16_t) (v < SAMPLE_MIN ? SAMPLE_MIN
                                                 : v > SAMPLE_MAX ? SAMPLE_MAX : v);
        }
    }
    return 1;
}

/* the frame's planes while it is decoded */
#define MAX_FRAME_PIXELS        (640 * 480)
static int16_t s_y[MAX_FRAME_PIXELS];
static int16_t s_cr[MAX_FRAME_PIXELS / 4], s_cb[MAX_FRAME_PIXELS / 4];

#define BYTE_OFFSET             128     /* unsigned output */

static int to_byte(int v)
{
    return (v < SAMPLE_MIN ? SAMPLE_MIN : v > SAMPLE_MAX ? SAMPLE_MAX : v) + BYTE_OFFSET;
}

/* a colour difference sample of the half-resolution plane at full-
 * resolution pixel (x, y): repeated (as the MDEC), or interpolated between
 * the samples' centres (smooth) */
static float chroma_at(const int16_t *plane, int cw, int ch, int x, int y, int smooth)
{
    if (!smooth) {
        return plane[(y / 2) * cw + x / 2];
    }
    float fx = (x - 0.5f) / 2, fy = (y - 0.5f) / 2;
    int x0 = (int) floorf(fx), y0 = (int) floorf(fy);
    float ax = fx - x0, ay = fy - y0;
    int x1 = x0 + 1, y1 = y0 + 1;
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 >= cw ? cw - 1 : x1;
    y1 = y1 >= ch ? ch - 1 : y1;
    float top = plane[y0 * cw + x0] * (1 - ax) + plane[y0 * cw + x1] * ax;
    float bottom = plane[y1 * cw + x0] * (1 - ax) + plane[y1 * cw + x1] * ax;
    return top * (1 - ay) + bottom * ay;
}

int mdec_decode_frame(const uint32_t *codes, int smooth, uint32_t *rgb, int max_pixels,
                      int *width, int *height)
{
    CodeReader r = { (const uint16_t *) (codes + CODES_START), (int) (codes[0] & 0xFFFF) * 2, 0 };
    int16_t block[BLOCKS_PER_MACROBLOCK][BLOCK_COEFFS];
    int columns = round_up_macroblock((int) (codes[1] & 0xFFFF));
    int rows = round_up_macroblock((int) (codes[1] >> 16));
    int w = columns * MDEC_MACROBLOCK, h = rows * MDEC_MACROBLOCK;

    if (!s_scale_ready) {
        build_scale_table();
    }
    if (columns <= 0 || rows <= 0 || w * h > max_pixels || w * h > MAX_FRAME_PIXELS) {
        return 0;
    }
    *width = w;
    *height = h;
    int cw = w / 2, ch = h / 2;

    /* the blocks into the planes */
    for (int m = 0; m < columns * rows; m++) {
        int mx = (m / rows) * MDEC_MACROBLOCK, my = (m % rows) * MDEC_MACROBLOCK;
        for (int b = 0; b < BLOCKS_PER_MACROBLOCK; b++) {
            decode_block(&r, block[b]);
        }
        for (int y = 0; y < BLOCK_SIZE; y++) {
            for (int x = 0; x < BLOCK_SIZE; x++) {
                int c = (my / 2 + y) * cw + mx / 2 + x;
                s_cr[c] = block[0][y * BLOCK_SIZE + x];
                s_cb[c] = block[1][y * BLOCK_SIZE + x];
            }
        }
        for (int q = 0; q < 4; q++) {
            int ox = mx + (q & 1) * BLOCK_SIZE, oy = my + (q >> 1) * BLOCK_SIZE;
            for (int y = 0; y < BLOCK_SIZE; y++) {
                for (int x = 0; x < BLOCK_SIZE; x++) {
                    s_y[(oy + y) * w + ox + x] = block[2 + q][y * BLOCK_SIZE + x];
                }
            }
        }
    }

    /* YCbCr -> RGB (ITU-R BT.601, as the MDEC: the colour terms cut
     * towards zero) */
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float cr = chroma_at(s_cr, cw, ch, x, y, smooth);
            float cb = chroma_at(s_cb, cw, ch, x, y, smooth);
            int luma = s_y[y * w + x];
            int red = to_byte(luma + (int16_t) (1.402f * cr));
            int green = to_byte(luma + (int16_t) (-0.3437f * cb + -0.7143f * cr));
            int blue = to_byte(luma + (int16_t) (1.772f * cb));
            rgb[y * w + x] = (uint32_t) red << 16 | (uint32_t) green << 8 | (uint32_t) blue;
        }
    }
    return 1;
}
