/*
 * gpu_hires.c - the high-resolution copy of VRAM (see gpu_hires.h).
 *
 * Coordinates: high-resolution pixel X covers original pixels
 * [X / scale, (X + 1) / scale) and is sampled at its centre, so a sprite
 * or a 2D polygon covers exactly the pixels it covers in the original,
 * each original texel becoming scale x scale pixels.  Polygon edges use
 * 4 fraction bits and the same top-left rule as the original renderer.
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "psx/gpu.h"
#include "psx/gpu_hires.h"
#include "psx/log.h"
#include "platform/threads.h"

#define SUBPIXEL_BITS           4
#define SUBPIXEL                (1 << SUBPIXEL_BITS)
#define QUEUE_SIZE              16384
#define BANDS_PER_THREAD        2

enum { CMD_TRIANGLE, CMD_RECTANGLE, CMD_LINE, CMD_FILL, CMD_NONE };

/* attributes interpolated over a triangle */
enum { ATTR_R, ATTR_G, ATTR_B, ATTR_U, ATTR_V, ATTR_W, ATTR_COUNT };

typedef struct {
    float base, dx, dy;         /* value at (min_x, min_y), steps per pixel */
} Plane;

typedef struct {
    int type;
    int min_x, min_y, max_x, max_y;     /* high-resolution pixels, inclusive */
    GpuPrim p;
    GpuEnv e;
    union {
        struct {
            int64_t a[3], b[3], c[3];   /* edge i: a*X + b*Y + c >= 0 inside */
            Plane plane[ATTR_COUNT];
            int perspective;
            int filtered;                   /* bilinear texture filtering */
            int flat_r, flat_g, flat_b;     /* without Gouraud shading */
            int16_t u_min, u_max, v_min, v_max;     /* float errors stay inside */
        } tri;
        struct {
            int x, y;                   /* original pixels */
            int r, g, b, u0, v0, flip_x, flip_y;
        } rect;
        struct {
            HiresVertex a, b;
        } line;
        struct {
            uint16_t color;
        } fill;
    } u;
} Command;

static int s_scale;
static int s_texture_filter;
static int s_width, s_height;           /* of the copy */
static uint16_t *s_vram;
static Command *s_queue;
static int s_queued;
static unsigned s_generation;           /* + 1 at every flush */

static const int8_t DITHER[4][4] = {
    { -4, 0, -3, 1 },
    { 2, -2, 3, -1 },
    { -3, 1, -4, 0 },
    { 3, -1, 2, -2 },
};

int hires_scale(void)
{
    return s_scale;
}

void hires_set_texture_filter(int on)
{
    s_texture_filter = on;
}

void hires_configure(int scale)
{
    scale = scale < 0 ? 0 : scale > HIRES_MAX_SCALE ? HIRES_MAX_SCALE : scale;
    if (scale == s_scale) {
        return;
    }
    free(s_vram);
    s_vram = NULL;
    s_queued = 0;
    s_scale = 0;
    if (scale == 0) {
        return;
    }
    s_width = GPU_VRAM_WIDTH * scale;
    s_height = GPU_VRAM_HEIGHT * scale;
    s_vram = (uint16_t *) malloc((size_t) s_width * s_height * sizeof(uint16_t));
    if (!s_queue) {
        s_queue = (Command *) malloc(sizeof(Command) * QUEUE_SIZE);
    }
    if (!s_vram || !s_queue) {
        psx_log("GPU: no memory for %dx internal resolution", scale);
        free(s_vram);
        s_vram = NULL;
        return;
    }
    s_scale = scale;
    hires_copy_from_original(0, 0, GPU_VRAM_WIDTH, GPU_VRAM_HEIGHT);
    psx_log("GPU: internal resolution %dx (%d threads)", scale, threads_count());
}

static uint16_t *pixel_at(int x, int y)
{
    return &s_vram[(size_t) y * s_width + x];
}

/* ---- pixels (as gpu.c, reading textures from the original VRAM) ------- */

static uint16_t original_at(int x, int y)
{
    return g_vram[y & (GPU_VRAM_HEIGHT - 1)][x & (GPU_VRAM_WIDTH - 1)];
}

static uint16_t texel(const Command *c, int u, int v)
{
    const GpuPrim *p = &c->p;
    u &= 0xFF;
    v &= 0xFF;
    u = (u & ~(c->e.win_mask_x * 8)) | ((c->e.win_off_x & c->e.win_mask_x) * 8);
    v = (v & ~(c->e.win_mask_y * 8)) | ((c->e.win_off_y & c->e.win_mask_y) * 8);
    switch (p->depth) {
    case 0: {
        uint16_t word = original_at(p->page_x + u / 4, p->page_y + v);
        return original_at(p->clut_x + ((word >> ((u & 3) * 4)) & 0xF), p->clut_y);
    }
    case 1: {
        uint16_t word = original_at(p->page_x + u / 2, p->page_y + v);
        return original_at(p->clut_x + ((word >> ((u & 1) * 8)) & 0xFF), p->clut_y);
    }
    default:
        return original_at(p->page_x + u, p->page_y + v);
    }
}

static int clamp255(int value)
{
    return value < 0 ? 0 : value > 255 ? 255 : value;
}

static uint16_t to_15bit(int r, int g, int b, int x, int y, int dither)
{
    if (dither) {
        int d = DITHER[y & 3][x & 3];
        r += d;
        g += d;
        b += d;
    }
    r = clamp255(r);
    g = clamp255(g);
    b = clamp255(b);
    return (uint16_t) ((r >> 3) | (g >> 3) << 5 | (b >> 3) << 10);
}

static uint16_t blend(uint16_t back, uint16_t front, int mode)
{
    int result = 0;
    for (int shift = 0; shift < 15; shift += 5) {
        int b = (back >> shift) & 31, f = (front >> shift) & 31, c;
        switch (mode) {
        case 0: c = (b + f) >> 1; break;
        case 1: c = b + f; break;
        case 2: c = b - f; break;
        default: c = b + (f >> 2); break;
        }
        c = c < 0 ? 0 : c > 31 ? 31 : c;
        result |= c << shift;
    }
    return (uint16_t) result;
}

/* one high-resolution pixel (inside the copy and the drawing area) */
static void plot(const Command *c, int x, int y, int r, int g, int b, int u, int v)
{
    const GpuPrim *p = &c->p;
    uint16_t *dst = pixel_at(x, y);
    uint16_t color;
    uint16_t mask = (uint16_t) (c->e.set_mask << 15);
    int semi = p->semi;
    /* dithering in original pixels, as on the console */
    int dx = x / s_scale, dy = y / s_scale;

    if (c->e.check_mask && (*dst & 0x8000)) {
        return;
    }
    if (p->textured) {
        uint16_t t = texel(c, u, v);
        if (t == 0) {
            return;
        }
        semi = p->semi && (t & 0x8000);
        mask |= t & 0x8000;
        if (p->raw) {
            color = t & 0x7FFF;
        } else {
            color = to_15bit(((t & 31) * r) >> 4, (((t >> 5) & 31) * g) >> 4,
                             (((t >> 10) & 31) * b) >> 4, dx, dy, p->dither);
        }
    } else {
        color = to_15bit(r, g, b, dx, dy, p->dither);
    }
    if (semi) {
        color = blend(*dst, color, p->blend);
    }
    *dst = color | mask;
}

/*
 * One pixel of a textured 3D polygon with bilinear filtering: the four
 * texels around the sample point (u, v), kept inside the polygon's own
 * texture area so that neighbouring images on the page do not bleed in.
 * Transparent texels (0) do not count; whether the pixel is drawn at
 * all, and its semi-transparency and mask bits, follow the nearest texel
 * as without filtering, so outlines stay where they were.  Weights are
 * 8-bit fractions (the hottest loop of the renderer: big filtered
 * polygons such as the cloth cover most of the picture).
 */
#define FILTER_BITS             8
#define FILTER_ONE              (1 << FILTER_BITS)

static void plot_filtered(const Command *c, int x, int y, int r, int g, int b, float u, float v)
{
    const GpuPrim *p = &c->p;
    uint16_t *dst = pixel_at(x, y);

    if (c->e.check_mask && (*dst & 0x8000)) {
        return;
    }
    /* the sample point between texel centres, as fixed point (floor) */
    int uf = (int) ((u - 0.5f) * FILTER_ONE + 65536.0f * FILTER_ONE) - 65536 * FILTER_ONE;
    int vf = (int) ((v - 0.5f) * FILTER_ONE + 65536.0f * FILTER_ONE) - 65536 * FILTER_ONE;
    int fu = uf & (FILTER_ONE - 1), fv = vf & (FILTER_ONE - 1);
    int u0 = uf >> FILTER_BITS, v0 = vf >> FILTER_BITS;
    int u1 = u0 + 1, v1 = v0 + 1;
    int u_min = c->u.tri.u_min, u_max = c->u.tri.u_max;
    int v_min = c->u.tri.v_min, v_max = c->u.tri.v_max;
    u0 = u0 < u_min ? u_min : u0 > u_max ? u_max : u0;
    u1 = u1 < u_min ? u_min : u1 > u_max ? u_max : u1;
    v0 = v0 < v_min ? v_min : v0 > v_max ? v_max : v0;
    v1 = v1 < v_min ? v_min : v1 > v_max ? v_max : v1;
    uint16_t t[4] = { texel(c, u0, v0), texel(c, u1, v0), texel(c, u0, v1), texel(c, u1, v1) };
    int w[4] = { (FILTER_ONE - fu) * (FILTER_ONE - fv), fu * (FILTER_ONE - fv),
                 (FILTER_ONE - fu) * fv, fu * fv };
    /* the nearest texel: the first of the largest weights */
    int n = 0;
    for (int i = 1; i < 4; i++) {
        n = w[i] > w[n] ? i : n;
    }
    uint16_t nearest = t[n];
    if (nearest == 0) {
        return;
    }
    int sr = 0, sg = 0, sb = 0, weight = 0;
    for (int i = 0; i < 4; i++) {
        if (t[i] != 0) {
            sr += w[i] * (t[i] & 31);
            sg += w[i] * ((t[i] >> 5) & 31);
            sb += w[i] * ((t[i] >> 10) & 31);
            weight += w[i];
        }
    }
    /* texel channels as 0..248, like the 5-bit values times 8 */
    int tr, tg, tb;
    if (weight == FILTER_ONE * FILTER_ONE) {
        tr = sr >> (2 * FILTER_BITS - 3);
        tg = sg >> (2 * FILTER_BITS - 3);
        tb = sb >> (2 * FILTER_BITS - 3);
    } else {
        tr = sr * 8 / weight;
        tg = sg * 8 / weight;
        tb = sb * 8 / weight;
    }
    uint16_t color;
    if (p->raw) {
        color = to_15bit(tr, tg, tb, 0, 0, 0);
    } else {
        color = to_15bit((tr * r) >> 7, (tg * g) >> 7, (tb * b) >> 7, x / s_scale, y / s_scale,
                         p->dither);
    }
    if (p->semi && (nearest & 0x8000)) {
        color = blend(*dst, color, p->blend);
    }
    *dst = color | (uint16_t) (c->e.set_mask << 15) | (nearest & 0x8000);
}

/* the drawing area in high-resolution pixels */
static void clip_rect(const GpuEnv *e, int *x1, int *y1, int *x2, int *y2)
{
    *x1 = e->clip_x1 * s_scale;
    *y1 = e->clip_y1 * s_scale;
    *x2 = (e->clip_x2 + 1) * s_scale - 1;
    *y2 = (e->clip_y2 + 1) * s_scale - 1;
}

static Command *new_command(int type, const GpuPrim *p, const GpuEnv *e)
{
    if (s_queued == QUEUE_SIZE) {
        hires_flush();
    }
    Command *c = &s_queue[s_queued++];
    c->type = type;
    if (p) {
        c->p = *p;
    }
    if (e) {
        c->e = *e;
    }
    return c;
}

/* ---- triangles -------------------------------------------------------- */

static int top_left(int64_t dx, int64_t dy)
{
    return dy < 0 || (dy == 0 && dx > 0);
}

static void make_plane(Plane *plane, const double x[3], const double y[3], const double a[3],
                       double area, int min_x, int min_y)
{
    double dx = ((a[1] - a[0]) * (y[2] - y[0]) - (a[2] - a[0]) * (y[1] - y[0])) / area;
    double dy = ((a[2] - a[0]) * (x[1] - x[0]) - (a[1] - a[0]) * (x[2] - x[0])) / area;
    plane->dx = (float) dx;
    plane->dy = (float) dy;
    plane->base = (float) (a[0] + dx * (min_x - x[0]) + dy * (min_y - y[0]));
}

void hires_triangle(const GpuPrim *p, const GpuEnv *e, const HiresVertex *v0,
                    const HiresVertex *v1, const HiresVertex *v2)
{
    const HiresVertex *v[3] = { v0, v1, v2 };
    int64_t fx[3], fy[3];

    if (!s_scale) {
        return;
    }
    /* sample point of pixel X is original x = (X + 0.5) / scale */
    for (int i = 0; i < 3; i++) {
        fx[i] = (int64_t) llround(((double) v[i]->x * s_scale - 0.5) * SUBPIXEL);
        fy[i] = (int64_t) llround(((double) v[i]->y * s_scale - 0.5) * SUBPIXEL);
    }
    int64_t area = (fx[1] - fx[0]) * (fy[2] - fy[0]) - (fy[1] - fy[0]) * (fx[2] - fx[0]);
    if (area == 0) {
        return;
    }
    if (area < 0) {
        const HiresVertex *t = v[1];
        v[1] = v[2];
        v[2] = t;
        int64_t tx = fx[1], ty = fy[1];
        fx[1] = fx[2];
        fy[1] = fy[2];
        fx[2] = tx;
        fy[2] = ty;
        area = -area;
    }

    int x1, y1, x2, y2;
    clip_rect(e, &x1, &y1, &x2, &y2);
    int64_t lo_x = fx[0], hi_x = fx[0], lo_y = fy[0], hi_y = fy[0];
    for (int i = 1; i < 3; i++) {
        lo_x = fx[i] < lo_x ? fx[i] : lo_x;
        hi_x = fx[i] > hi_x ? fx[i] : hi_x;
        lo_y = fy[i] < lo_y ? fy[i] : lo_y;
        hi_y = fy[i] > hi_y ? fy[i] : hi_y;
    }
    int min_x = (int) ((lo_x + SUBPIXEL - 1) >> SUBPIXEL_BITS);
    int min_y = (int) ((lo_y + SUBPIXEL - 1) >> SUBPIXEL_BITS);
    int max_x = (int) (hi_x >> SUBPIXEL_BITS);
    int max_y = (int) (hi_y >> SUBPIXEL_BITS);
    min_x = min_x < x1 ? x1 : min_x;
    min_y = min_y < y1 ? y1 : min_y;
    max_x = max_x > x2 ? x2 : max_x;
    max_y = max_y > y2 ? y2 : max_y;
    if (min_x > max_x || min_y > max_y) {
        return;
    }

    Command *c = new_command(CMD_TRIANGLE, p, e);
    c->min_x = min_x;
    c->min_y = min_y;
    c->max_x = max_x;
    c->max_y = max_y;
    /* edge i is opposite vertex i: v1->v2, v2->v0, v0->v1 */
    for (int i = 0; i < 3; i++) {
        int a = (i + 1) % 3, b = (i + 2) % 3;
        int64_t dx = fx[b] - fx[a], dy = fy[b] - fy[a];
        c->u.tri.a[i] = -dy * SUBPIXEL;
        c->u.tri.b[i] = dx * SUBPIXEL;
        c->u.tri.c[i] = dy * fx[a] - dx * fy[a] - (top_left(dx, dy) ? 0 : 1);
    }

    double px[3], py[3], attr[3];
    double darea = (double) area / (SUBPIXEL * SUBPIXEL);
    for (int i = 0; i < 3; i++) {
        px[i] = (double) fx[i] / SUBPIXEL;
        py[i] = (double) fy[i] / SUBPIXEL;
    }
    /* textures perspective-correct; colours stay linear on the screen as on
     * the console (the lighting baked into the vertex colours, e.g. the
     * bright spot on the cloth, is laid out for that) */
    int perspective = v[0]->z > 0 && v[1]->z > 0 && v[2]->z > 0 && p->textured;
    c->u.tri.perspective = perspective;
    c->u.tri.filtered = s_texture_filter && perspective;
    c->u.tri.flat_r = v0->r;
    c->u.tri.flat_g = v0->g;
    c->u.tri.flat_b = v0->b;
    c->u.tri.u_min = c->u.tri.u_max = (int16_t) v[0]->u;
    c->u.tri.v_min = c->u.tri.v_max = (int16_t) v[0]->v;
    for (int i = 1; i < 3; i++) {
        c->u.tri.u_min = v[i]->u < c->u.tri.u_min ? (int16_t) v[i]->u : c->u.tri.u_min;
        c->u.tri.u_max = v[i]->u > c->u.tri.u_max ? (int16_t) v[i]->u : c->u.tri.u_max;
        c->u.tri.v_min = v[i]->v < c->u.tri.v_min ? (int16_t) v[i]->v : c->u.tri.v_min;
        c->u.tri.v_max = v[i]->v > c->u.tri.v_max ? (int16_t) v[i]->v : c->u.tri.v_max;
    }
    for (int k = ATTR_R; k < ATTR_COUNT; k++) {
        for (int i = 0; i < 3; i++) {
            double value;
            switch (k) {
            case ATTR_R: value = v[i]->r; break;
            case ATTR_G: value = v[i]->g; break;
            case ATTR_B: value = v[i]->b; break;
            case ATTR_U: value = v[i]->u; break;
            case ATTR_V: value = v[i]->v; break;
            default: value = 1.0; break;
            }
            attr[i] = perspective && k >= ATTR_U ? value / v[i]->z : value;
        }
        make_plane(&c->u.tri.plane[k], px, py, attr, darea, min_x, min_y);
    }
}

static int64_t floor_div(int64_t n, int64_t d)       /* d > 0 */
{
    return n >= 0 ? n / d : -((-n + d - 1) / d);
}

/* the pixels x1..x2 of row y inside all three edges; FALSE if none */
static int triangle_span(const Command *c, int y, int *x1, int *x2)
{
    int64_t lo = c->min_x, hi = c->max_x;
    for (int i = 0; i < 3; i++) {
        int64_t a = c->u.tri.a[i], k = c->u.tri.b[i] * y + c->u.tri.c[i];
        if (a > 0) {
            int64_t x = -floor_div(k, a);           /* ceil(-k / a) */
            lo = x > lo ? x : lo;
        } else if (a < 0) {
            int64_t x = floor_div(k, -a);
            hi = x < hi ? x : hi;
        } else if (k < 0) {
            return 0;
        }
    }
    *x1 = (int) lo;
    *x2 = (int) hi;
    return lo <= hi;
}

static void draw_triangle(const Command *c, int band_y1, int band_y2)
{
    int y1 = c->min_y > band_y1 ? c->min_y : band_y1;
    int y2 = c->max_y < band_y2 ? c->max_y : band_y2;
    const Plane *pl = c->u.tri.plane;
    int textured = c->p.textured, gouraud = c->p.gouraud, perspective = c->u.tri.perspective;
    int flat_r = c->u.tri.flat_r, flat_g = c->u.tri.flat_g, flat_b = c->u.tri.flat_b;

    for (int y = y1; y <= y2; y++) {
        int x1, x2;
        if (!triangle_span(c, y, &x1, &x2)) {
            continue;
        }
        float row = (float) (y - c->min_y), col = (float) (x1 - c->min_x);
        float val[ATTR_COUNT];
        for (int k = 0; k < ATTR_COUNT; k++) {
            val[k] = pl[k].base + pl[k].dy * row + pl[k].dx * col;
        }
        for (int x = x1; x <= x2; x++) {
            {
                int r = flat_r, g = flat_g, b = flat_b, u = 0, v = 0;
                float w = perspective ? 1.0f / val[ATTR_W] : 1.0f;
                if (gouraud) {
                    r = clamp255((int) val[ATTR_R]);
                    g = clamp255((int) val[ATTR_G]);
                    b = clamp255((int) val[ATTR_B]);
                }
                if (c->u.tri.filtered) {
                    plot_filtered(c, x, y, r, g, b, val[ATTR_U] * w, val[ATTR_V] * w);
                    goto next;
                }
                if (textured) {
                    u = (int) floorf(val[ATTR_U] * w);
                    v = (int) floorf(val[ATTR_V] * w);
                    u = u < c->u.tri.u_min ? c->u.tri.u_min : u > c->u.tri.u_max ? c->u.tri.u_max : u;
                    v = v < c->u.tri.v_min ? c->u.tri.v_min : v > c->u.tri.v_max ? c->u.tri.v_max : v;
                }
                plot(c, x, y, r, g, b, u, v);
            }
        next:
            for (int k = 0; k < ATTR_COUNT; k++) {
                val[k] += pl[k].dx;
            }
        }
    }
}

/* ---- rectangles ------------------------------------------------------- */

void hires_rectangle(const GpuPrim *p, const GpuEnv *e, int x, int y, int w, int h,
                     int r, int g, int b, int u0, int v0, int flip_x, int flip_y)
{
    int x1, y1, x2, y2;
    if (!s_scale || w <= 0 || h <= 0) {
        return;
    }
    clip_rect(e, &x1, &y1, &x2, &y2);
    int min_x = x * s_scale, min_y = y * s_scale;
    int max_x = (x + w) * s_scale - 1, max_y = (y + h) * s_scale - 1;
    min_x = min_x < x1 ? x1 : min_x;
    min_y = min_y < y1 ? y1 : min_y;
    max_x = max_x > x2 ? x2 : max_x;
    max_y = max_y > y2 ? y2 : max_y;
    if (min_x > max_x || min_y > max_y) {
        return;
    }
    Command *c = new_command(CMD_RECTANGLE, p, e);
    c->min_x = min_x;
    c->min_y = min_y;
    c->max_x = max_x;
    c->max_y = max_y;
    c->u.rect.x = x;
    c->u.rect.y = y;
    c->u.rect.r = r;
    c->u.rect.g = g;
    c->u.rect.b = b;
    c->u.rect.u0 = u0;
    c->u.rect.v0 = v0;
    c->u.rect.flip_x = flip_x;
    c->u.rect.flip_y = flip_y;
}

static void draw_rectangle(const Command *c, int band_y1, int band_y2)
{
    int y1 = c->min_y > band_y1 ? c->min_y : band_y1;
    int y2 = c->max_y < band_y2 ? c->max_y : band_y2;
    for (int y = y1; y <= y2; y++) {
        int row = (y - c->u.rect.y * s_scale) / s_scale;
        int v = c->u.rect.flip_y ? c->u.rect.v0 - row : c->u.rect.v0 + row;
        for (int x = c->min_x; x <= c->max_x; x++) {
            int col = (x - c->u.rect.x * s_scale) / s_scale;
            int u = c->u.rect.flip_x ? c->u.rect.u0 - col : c->u.rect.u0 + col;
            plot(c, x, y, c->u.rect.r, c->u.rect.g, c->u.rect.b, u, v);
        }
    }
}

/* ---- lines (the original's pixels, each scale x scale) ---------------- */

void hires_line(const GpuPrim *p, const GpuEnv *e, const HiresVertex *a, const HiresVertex *b)
{
    if (!s_scale) {
        return;
    }
    Command *c = new_command(CMD_LINE, p, e);
    int ay = (int) a->y, by = (int) b->y;
    c->min_x = 0;
    c->max_x = s_width - 1;
    c->min_y = (ay < by ? ay : by) * s_scale;
    c->max_y = ((ay > by ? ay : by) + 1) * s_scale - 1;
    c->u.line.a = *a;
    c->u.line.b = *b;
}

static int lerp(int a, int b, int i, int n)
{
    return n ? a + ((b - a) * i * 2 + (b >= a ? n : -n)) / (2 * n) : a;
}

static void draw_line(const Command *c, int band_y1, int band_y2)
{
    const HiresVertex *a = &c->u.line.a, *b = &c->u.line.b;
    int ax = (int) a->x, ay = (int) a->y, bx = (int) b->x, by = (int) b->y;
    int dx = bx - ax, dy = by - ay;
    int n = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
    const GpuEnv *e = &c->e;
    for (int i = 0; i <= n; i++) {
        int x = lerp(ax, bx, i, n), y = lerp(ay, by, i, n);
        if (x < e->clip_x1 || x > e->clip_x2 || y < e->clip_y1 || y > e->clip_y2) {
            continue;
        }
        int r = a->r, g = a->g, bl = a->b;
        if (c->p.gouraud) {
            r = lerp(a->r, b->r, i, n);
            g = lerp(a->g, b->g, i, n);
            bl = lerp(a->b, b->b, i, n);
        }
        for (int sy = y * s_scale; sy < (y + 1) * s_scale; sy++) {
            if (sy < band_y1 || sy > band_y2) {
                continue;
            }
            for (int sx = x * s_scale; sx < (x + 1) * s_scale; sx++) {
                plot(c, sx, sy, r, g, bl, 0, 0);
            }
        }
    }
}

/* ---- VRAM operations -------------------------------------------------- */

void hires_fill(int x, int y, int w, int h, uint16_t color)
{
    if (!s_scale || w <= 0 || h <= 0) {
        return;
    }
    Command *c = new_command(CMD_FILL, NULL, NULL);
    c->min_x = x * s_scale;
    c->min_y = y * s_scale;
    c->max_x = (x + w) * s_scale - 1;
    c->max_y = (y + h) * s_scale - 1;
    c->u.fill.color = color;
}

/* (fills wrap around the edges of VRAM like the original) */
static void draw_fill(const Command *c, int band_y1, int band_y2)
{
    for (int y = c->min_y; y <= c->max_y; y++) {
        int wy = y % s_height;
        if (wy < band_y1 || wy > band_y2) {
            continue;
        }
        for (int x = c->min_x; x <= c->max_x; x++) {
            *pixel_at(x % s_width, wy) = c->u.fill.color;
        }
    }
}

void hires_copy_from_original(int x, int y, int w, int h)
{
    if (!s_scale) {
        return;
    }
    hires_flush();
    for (int row = 0; row < h; row++) {
        int oy = (y + row) & (GPU_VRAM_HEIGHT - 1);
        for (int col = 0; col < w; col++) {
            int ox = (x + col) & (GPU_VRAM_WIDTH - 1);
            uint16_t value = g_vram[oy][ox];
            for (int sy = 0; sy < s_scale; sy++) {
                uint16_t *dst = pixel_at(ox * s_scale, oy * s_scale + sy);
                for (int sx = 0; sx < s_scale; sx++) {
                    dst[sx] = value;
                }
            }
        }
    }
}

void hires_move(int sx, int sy, int dx, int dy, int w, int h, int set_mask, int check_mask)
{
    if (!s_scale) {
        return;
    }
    hires_flush();
    for (int row = 0; row < h * s_scale; row++) {
        int from_y = (sy * s_scale + row) % s_height, to_y = (dy * s_scale + row) % s_height;
        for (int col = 0; col < w * s_scale; col++) {
            uint16_t *dst = pixel_at((dx * s_scale + col) % s_width, to_y);
            if (check_mask && (*dst & 0x8000)) {
                continue;
            }
            *dst = *pixel_at((sx * s_scale + col) % s_width, from_y) | (uint16_t) (set_mask << 15);
        }
    }
}

/* ---- drawing the queue ------------------------------------------------ */

typedef struct {
    int first_y, band_height;
} FlushJob;

static void draw_band(int band, void *context)
{
    const FlushJob *job = (const FlushJob *) context;
    int y1 = job->first_y + band * job->band_height;
    int y2 = y1 + job->band_height - 1;
    for (int i = 0; i < s_queued; i++) {
        const Command *c = &s_queue[i];
        if (c->type == CMD_NONE || (c->type != CMD_FILL && (c->max_y < y1 || c->min_y > y2))) {
            continue;
        }
        switch (c->type) {
        case CMD_TRIANGLE: draw_triangle(c, y1, y2); break;
        case CMD_RECTANGLE: draw_rectangle(c, y1, y2); break;
        case CMD_LINE: draw_line(c, y1, y2); break;
        default: draw_fill(c, y1, y2); break;
        }
    }
}

void hires_discard(void)
{
    s_queued = 0;
    s_generation++;
}

void hires_flush(void)
{
    if (!s_scale || s_queued == 0) {
        s_queued = 0;
        return;
    }
    int min_y = s_height, max_y = 0;
    for (int i = 0; i < s_queued; i++) {
        if (s_queue[i].type == CMD_NONE) {
            continue;
        }
        if (s_queue[i].type == CMD_FILL) {
            min_y = 0;                  /* may wrap: all rows */
            max_y = s_height - 1;
            break;
        }
        min_y = s_queue[i].min_y < min_y ? s_queue[i].min_y : min_y;
        max_y = s_queue[i].max_y > max_y ? s_queue[i].max_y : max_y;
    }
    min_y = min_y < 0 ? 0 : min_y;
    max_y = max_y >= s_height ? s_height - 1 : max_y;
    if (min_y <= max_y) {
        FlushJob job;
        int bands = threads_count() * BANDS_PER_THREAD;
        int rows = max_y - min_y + 1;
        job.first_y = min_y;
        job.band_height = (rows + bands - 1) / bands;
        bands = (rows + job.band_height - 1) / job.band_height;
        threads_run(bands, draw_band, &job);
    }
    s_queued = 0;
    s_generation++;
}

static uint32_t expand_5bit(int c)
{
    return (uint32_t) ((c << 3) | (c >> 2));
}

typedef struct {
    uint32_t *out;
    int x, y, w, h;                     /* high-resolution pixels */
    int rows_per_task;
} ReadJob;

static void read_rows(int task, void *context)
{
    const ReadJob *job = (const ReadJob *) context;
    int first = task * job->rows_per_task;
    int last = first + job->rows_per_task < job->h ? first + job->rows_per_task : job->h;
    for (int row = first; row < last; row++) {
        uint32_t *out = job->out + (size_t) row * job->w;
        int vy = (job->y + row) % s_height;
        for (int col = 0; col < job->w; col++) {
            uint16_t p = *pixel_at((job->x + col) % s_width, vy);
            *out++ = 0xFF000000u | expand_5bit(p & 31) | expand_5bit((p >> 5) & 31) << 8
                     | expand_5bit((p >> 10) & 31) << 16;
        }
    }
}

void hires_read(uint32_t *out, int x, int y, int w, int h)
{
    ReadJob job = { out, x * s_scale, y * s_scale, w * s_scale, h * s_scale, 0 };
    int tasks = threads_count();
    hires_flush();
    job.rows_per_task = (job.h + tasks - 1) / tasks;
    threads_run((job.h + job.rows_per_task - 1) / job.rows_per_task, read_rows, &job);
}
