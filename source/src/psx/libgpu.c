/*
 * libgpu.c - LIBGPU: VRAM transfers, drawing and display environments,
 * ordering tables and primitives, on top of the software GPU (gpu.c).
 *
 * Replacement for the Psy-Q library in the PC port.  Drawing finishes at
 * once, so DrawSync() never waits.  DRAWENV and POLY_FT4 have the Psy-Q
 * layout (../include/psyq/gpu.h).
 */
#include "psx/psx.h"
#include "psx/gpu.h"
#include "psx/savestate.h"

/* GP0 commands */
#define GP0_FILL                0x02000000u
#define GP0_DRAW_MODE           0xE1000000u
#define GP0_TEXTURE_WINDOW      0xE2000000u

/* primitive header: the length byte of the tag and the code byte after
 * the colour */
#define PRIM_LENGTH_BYTE        3
#define PRIM_CODE_BYTE          7
#define CODE_SEMI_TRANS         0x02
#define CODE_POLY_F4            0x28
#define CODE_POLY_FT4           0x2C
#define POLY_F4_WORDS           5
#define POLY_FT4_WORDS          9
#define DR_MODE_WORDS           2

/* draw mode bits (GP0(E1h)) above the texture page */
#define DRAW_MODE_PAGE_MASK     0x9FFu
#define DRAW_MODE_DITHER        (1u << 9)
#define DRAW_MODE_TO_DISPLAY    (1u << 10)

/* texture colour modes (GetTPage) */
#define TPAGE_4BIT              0
#define TPAGE_8BIT              1

static int s_video_mode;

void ResetGraph(int mode)
{
    gpu_set_display_enabled(0);
    if (mode == 0) {
        gpu_set_draw_area(0, 0, GPU_VRAM_WIDTH - 1, GPU_VRAM_HEIGHT - 1);
        gpu_set_draw_offset(0, 0);
        gpu_set_draw_mode(0);
        gpu_set_texture_window(0);
    }
}

int SetGraphDebug(int level)
{
    return 0;
}

void SetVideoMode(GPU_VideoMode mode)
{
    s_video_mode = mode;
}

GPU_VideoMode GetVideoMode(void)
{
    return (GPU_VideoMode) s_video_mode;
}

int DrawSync(int mode)
{
    return 0;                           /* nothing left to draw */
}

void SetDispMask(int mask)
{
    gpu_set_display_enabled(mask);
}

/* ---- environments ----------------------------------------------------- */

void PutDispEnv(const DISPENV *env)
{
    gpu_set_display(env->disp.x, env->disp.y, env->disp.w, env->disp.h, env->isrgb24);
    gpu_set_display_screen(env->screen.x, env->screen.y, env->screen.w, env->screen.h);
}

DISPENV *SetDefDispEnv(DISPENV *env, int x, int y, int w, int h)
{
    env->disp.x = (int16_t) x;
    env->disp.y = (int16_t) y;
    env->disp.w = (int16_t) w;
    env->disp.h = (int16_t) h;
    env->screen.x = env->screen.y = env->screen.w = env->screen.h = 0;
    env->isinter = 0;
    env->isrgb24 = 0;
    env->reverse = 0;
    return env;
}

static uint32_t texture_window(const RECT *tw)
{
    if (!tw || !tw->w || !tw->h) {
        return GP0_TEXTURE_WINDOW;
    }
    return GP0_TEXTURE_WINDOW | ((~(tw->w - 1) >> 3) & 0x1F) | ((~(tw->h - 1) >> 3) & 0x1F) << 5
           | ((tw->x >> 3) & 0x1F) << 10 | ((tw->y >> 3) & 0x1F) << 15;
}

static uint32_t draw_mode(int dfe, int dtd, int tpage)
{
    return GP0_DRAW_MODE | ((uint32_t) tpage & DRAW_MODE_PAGE_MASK)
           | (dtd ? DRAW_MODE_DITHER : 0) | (dfe ? DRAW_MODE_TO_DISPLAY : 0);
}

DRAWENV *SetDefDrawEnv(DRAWENV *env, int x, int y, int w, int h)
{
    env->clip.x = (int16_t) x;
    env->clip.y = (int16_t) y;
    env->clip.w = (int16_t) w;
    env->clip.h = (int16_t) h;
    env->ofs[0] = (int16_t) x;
    env->ofs[1] = (int16_t) y;
    env->tw.x = env->tw.y = env->tw.w = env->tw.h = 0;
    env->tpage = 10;                    /* as the library: page (640, 0) */
    env->dtd = 1;
    env->dfe = 1;
    env->isbg = 0;
    env->r0 = env->g0 = env->b0 = 0;
    return env;
}

/* (Psy-Q semantics: the offset is a VRAM position, like the clip area) */
DRAWENV *PutDrawEnv(DRAWENV *env)
{
    gpu_set_draw_area(env->clip.x, env->clip.y, env->clip.x + env->clip.w - 1,
                      env->clip.y + env->clip.h - 1);
    gpu_set_draw_offset(env->ofs[0], env->ofs[1]);
    gpu_gp0(draw_mode(env->dfe, env->dtd, env->tpage));
    gpu_gp0(texture_window(&env->tw));
    if (env->isbg) {
        gpu_fill(env->clip.x, env->clip.y, env->clip.w, env->clip.h,
                 env->r0 | env->g0 << 8 | (uint32_t) env->b0 << 16);
    }
    return env;
}

/* ---- VRAM ------------------------------------------------------------- */

int LoadImage(const RECT *rect, const uint32_t *data)
{
    gpu_load_image(rect->x, rect->y, rect->w, rect->h, (const uint16_t *) data);
    return 0;
}

int StoreImage(const RECT *rect, uint32_t *data)
{
    gpu_store_image(rect->x, rect->y, rect->w, rect->h, (uint16_t *) data);
    return 0;
}

int MoveImage(const RECT *rect, int x, int y)
{
    gpu_move_image(rect->x, rect->y, x, y, rect->w, rect->h);
    return 0;
}

/* as the GPU fill command: x and width in steps of 16 */
int ClearImage(RECT *rect, uint8_t r, uint8_t g, uint8_t b)
{
    uint32_t words[3];
    words[0] = GP0_FILL | r | g << 8 | (uint32_t) b << 16;
    words[1] = (uint16_t) rect->x | (uint32_t) (uint16_t) rect->y << 16;
    words[2] = (uint16_t) rect->w | (uint32_t) (uint16_t) rect->h << 16;
    gpu_gp0_words(words, 3);
    return 0;
}

uint16_t GetTPage(int tp, int abr, int x, int y)
{
    return (uint16_t) (((tp & 3) << 7) | ((abr & 3) << 5) | ((y & 0x100) >> 4) | ((x & 0x3FF) >> 6));
}

/* Loads a texture (w x h texels of mode tp) to (x, y) and returns its page. */
uint16_t LoadTPage(uint32_t *pixels, int tp, int abr, int x, int y, int w, int h)
{
    RECT rect;
    rect.x = (int16_t) x;
    rect.y = (int16_t) y;
    rect.w = (int16_t) (tp == TPAGE_4BIT ? w / 4 : tp == TPAGE_8BIT ? w / 2 : w);
    rect.h = (int16_t) h;
    LoadImage(&rect, pixels);
    return GetTPage(tp, abr, x, y);
}

/* ---- primitives and ordering tables ----------------------------------- */

static void set_header(void *p, int words, uint8_t code)
{
    ((uint8_t *) p)[PRIM_LENGTH_BYTE] = (uint8_t) words;
    ((uint8_t *) p)[PRIM_CODE_BYTE] = code;
}

void SetPolyF4(POLY_F4 *p)
{
    set_header(p, POLY_F4_WORDS, CODE_POLY_F4);
}

void SetPolyFT4(POLY_FT4 *p)
{
    set_header(p, POLY_FT4_WORDS, CODE_POLY_FT4);
}

void SetSemiTrans(void *p, int abe)
{
    uint8_t *code = (uint8_t *) p + PRIM_CODE_BYTE;
    *code = abe ? (uint8_t) (*code | CODE_SEMI_TRANS) : (uint8_t) (*code & ~CODE_SEMI_TRANS);
}

void SetDrawMode(DR_MODE *p, int dfe, int dtd, int tpage, RECT *tw)
{
    ((uint8_t *) p)[PRIM_LENGTH_BYTE] = DR_MODE_WORDS;
    p->code[0] = draw_mode(dfe, dtd, tpage);
    p->code[1] = texture_window(tw);
}

void AddPrim(uint32_t *ot, const void *pri)
{
    uint32_t *p = (uint32_t *) pri;
    *p = (*p & 0xFF000000u) | (*ot & PSX_LINK_MASK);
    *ot = (*ot & 0xFF000000u) | psx_link(p);
}

/* Links ot[0] -> ot[1] -> ... -> ot[n-1] -> end (drawn from ot[0]). */
void ClearOTag(uint32_t *ot, size_t length)
{
    for (size_t i = 0; i + 1 < length; i++) {
        ot[i] = psx_link(&ot[i + 1]);
    }
    ot[length - 1] = PSX_LINK_END;
}

/* Links ot[n-1] -> ... -> ot[0] -> end (drawn from ot[n-1]). */
void ClearOTagR(uint32_t *ot, size_t length)
{
    for (size_t i = length - 1; i > 0; i--) {
        ot[i] = psx_link(&ot[i - 1]);
    }
    ot[0] = PSX_LINK_END;
}

/* Sends every primitive of the list starting at `ot` to the GPU. */
int g_psx_skip_frames;

int DrawOTag(const uint32_t *ot)
{
    const uint32_t *p = ot;
    if (g_psx_skip_frames) {
        return 0;                       /* a frame that will not be shown */
    }
    int guard = 0x100000;               /* a broken list must not hang */
    while (guard-- > 0) {
        int words = (int) (*p >> 24);
        if (words) {
            gpu_gp0_words(p + 1, words);
        }
        if ((*p & PSX_LINK_MASK) == PSX_LINK_END) {
            break;
        }
        p = (const uint32_t *) psx_pointer(*p);
    }
    return 0;
}

/* (a save state: savestate.c) */
void libgpu_state(StateIO *io)
{
    state_section(io, "LGPU");
    STATE(io, s_video_mode);
}
