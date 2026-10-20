/*
 * main_pc.cpp - entry point of the PC port of D.
 *
 * The program starts in the launcher (choose the discs, settings).  "Play"
 * runs the game's own main() (src/psx/system.c); the game hands control
 * back once per frame, in VSync(), to game_frame() below, which shows
 * the PS1 display area in the window.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>
#include "imgui.h"
#include "platform/platform.h"
#include "platform/input.h"
#include "launcher/settings.h"
#include "launcher/launcher.h"
#include "launcher/ui.h"
#include "launcher/controls.h"
#include "launcher/states.h"
#include "platform/audio.h"
#include "disc/disc.h"
#include "psx/system.h"
#include "psx/savestate.h"
#include "psx/gpu.h"
#include "psx/log.h"

#define APP_NAME                "D"
#define BACKGROUND              0x050304
#define GAME_BACKGROUND         0x000000
#define PSX_ASPECT              (4.0f / 3.0f)   /* the TV picture */
#define HINT_FRAMES             (60 * 4)        /* "Esc" hint at the start */
#define PAD_SELECT_START        0x0009          /* PS1 buttons that open the settings */
#define PAD_CIRCLE              0x2000
#define VK_KEY_P                0x50            /* pause (with VK_PAUSE, R3 and Start) */
#define PAD_START               0x0008          /* PS1 buttons (read_pad) */
#define PAD_SELECT              0x0001
#define PAUSE_DIM               IM_COL32(0, 0, 0, 120)
#define PAUSE_TITLE_SIZE        64.0f           /* before DPI scaling */
#define SETTINGS_WIDTH          760.0f          /* in-game settings, before DPI scaling */
#define SETTINGS_HEIGHT         560.0f
#define SETTINGS_DIM            IM_COL32(0, 0, 0, 150)
#define PAUSE_TITLE_Y           0.22f           /* of the window's height */
#define PAUSE_LAYOUT_HEIGHT     800.0f          /* the pause screen grows with taller windows */
#define PAUSE_LIST_TITLE_Y      0.08f           /* higher over the lists of states (all 8 fit) */
#define PAUSE_MENU_WIDTH        300.0f          /* before DPI scaling */
#define STATES_WIDTH            640.0f
#define STATES_ROW              54.0f
#define MESSAGE_COLOUR          IM_COL32(242, 214, 90, 255)

static int s_quit;                      /* the window was closed */

/* A crash: the address and the return addresses on the stack go to the
 * log (llvm-symbolizer turns them into source lines). */
static LONG WINAPI crash_handler(EXCEPTION_POINTERS *info)
{
    const CONTEXT *c = info->ContextRecord;
    psx_log("CRASH: exception %08lx at %08lx (access %08lx)",
            (unsigned long) info->ExceptionRecord->ExceptionCode, (unsigned long) c->Eip,
            info->ExceptionRecord->NumberParameters > 1
                ? (unsigned long) info->ExceptionRecord->ExceptionInformation[1] : 0ul);
    psx_log("CRASH: eax %08lx ebx %08lx ecx %08lx edx %08lx esi %08lx edi %08lx esp %08lx ebp %08lx",
            c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Esp, c->Ebp);
    const DWORD *frame = (const DWORD *) c->Ebp;
    for (int i = 0; i < 24 && frame && !IsBadReadPtr(frame, 8); i++) {
        psx_log("CRASH: called from %08lx", frame[1]);
        frame = (const DWORD *) frame[0];
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static int s_frame;                     /* frames of the running game */

/*
 * Debugging and tests: D_INPUT=<frame>:<button>[,...] presses a button of
 * controller 1 for a few frames at the given game frame, or holds it with
 * <first>-<last>:<button>, e.g. "300:start,420:cross,900-1500:up" (names:
 * up down left right cross circle square triangle l1 r1 l2 r2 start
 * select).  See also D_VRAM_DUMP, D_WAV_DUMP, D_MUTE, D_AUTOSTART,
 * D_NATIVE and D_LEAVE below, and tests/.
 */
#define SCRIPT_PRESS_FRAMES     6

static int scripted_buttons(void)
{
    static const struct { const char *name; int bit; } BUTTONS[] = {
        { "select", 0x0001 }, { "start", 0x0008 }, { "up", 0x0010 }, { "right", 0x0020 },
        { "down", 0x0040 }, { "left", 0x0080 }, { "l2", 0x0100 }, { "r2", 0x0200 },
        { "l1", 0x0400 }, { "r1", 0x0800 }, { "triangle", 0x1000 }, { "circle", 0x2000 },
        { "cross", 0x4000 }, { "square", 0x8000 },
    };
    static char script[16384];
    int buttons = 0;
    DWORD n = GetEnvironmentVariableA("D_INPUT", script, sizeof(script));
    if (n == 0 || n >= sizeof(script)) {
        return 0;
    }
    for (char *p = script; *p;) {
        int frame = atoi(p), last = frame + SCRIPT_PRESS_FRAMES - 1;
        char *dash = strchr(p, '-');
        char *name = strchr(p, ':');
        if (dash && name && dash < name) {
            last = atoi(dash + 1);
        }
        char *end = strchr(p, ',');
        if (!name || (end && name > end)) {
            break;
        }
        name++;
        int length = end ? (int) (end - name) : (int) strlen(name);
        if (s_frame >= frame && s_frame <= last) {
            for (const auto &b : BUTTONS) {
                if ((int) strlen(b.name) == length && _strnicmp(b.name, name, length) == 0) {
                    buttons |= b.bit;
                }
            }
        }
        p = end ? end + 1 : name + length;
    }
    return buttons;
}

/* After the pause screen the game sees no buttons until all are let go
 * (Enter / A chose "Continue": the game must not take it as Cross). */
static int s_hold_buttons;

/* A controller's buttons: the settings, plus D_INPUT for player 1. */
static int read_pad(int port)
{
    int buttons = controls_psx_buttons(port);
    if (s_hold_buttons) {
        if (controls_psx_buttons(0) <= 0 && controls_psx_buttons(1) <= 0) {
            s_hold_buttons = 0;
        } else if (buttons > 0) {
            buttons = 0;
        }
    }
    if (port == 0) {
        int scripted = scripted_buttons();
        if (scripted) {
            buttons = (buttons < 0 ? 0 : buttons) | scripted;
        }
    }
    return buttons;
}

/* Debugging: D_WAV_DUMP=<file> records the game's sound (16-bit stereo WAV). */
static FILE *s_wav;
static uint32_t s_wav_bytes;

static void wav_header(FILE *f, uint32_t data_bytes)
{
    const uint32_t rate = 44100, byte_rate = rate * 4;
    const uint16_t pcm = 1, channels = 2, align = 4, bits = 16;
    uint32_t riff = 36 + data_bytes, fmt_size = 16;
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f);
    fwrite(&riff, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt_size, 4, 1, f);
    fwrite(&pcm, 2, 1, f);
    fwrite(&channels, 2, 1, f);
    fwrite(&rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    fwrite(&align, 2, 1, f);
    fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&data_bytes, 4, 1, f);
}

static void record(const int16_t *samples, int frames)
{
    if (s_wav) {
        fwrite(samples, 4, (size_t) frames, s_wav);
        s_wav_bytes += (uint32_t) frames * 4;
    }
}

/* real time (the sound device's thread): what is heard is also recorded */
static void sound_stream(int16_t *samples, int frames)
{
    psx_sound_frame(samples);
    record(samples, frames);
}

static void sound_sink(const int16_t *samples, int frames)
{
    audio_submit(samples, frames);
    record(samples, frames);
}

/* the displayed picture (internal resolution) as a 24-bit BMP */
static void dump_picture(const char *path)
{
    int w, h;
    const uint32_t *picture = gpu_display_rgba(&w, &h);
    FILE *f = picture ? fopen(path, "wb") : NULL;
    if (!f) {
        return;
    }
    uint8_t header[54] = { 'B', 'M' };
    uint32_t size = 54 + (uint32_t) (w * h * 3);
    int32_t bw = w, bh = -h;            /* top-down */
    memcpy(header + 2, &size, 4);
    header[10] = 54;
    header[14] = 40;
    memcpy(header + 18, &bw, 4);
    memcpy(header + 22, &bh, 4);
    header[26] = 1;
    header[28] = 24;
    fwrite(header, 1, sizeof(header), f);
    for (int i = 0; i < w * h; i++) {
        uint8_t bgr[3] = { (uint8_t) (picture[i] >> 16), (uint8_t) (picture[i] >> 8),
                           (uint8_t) picture[i] };
        fwrite(bgr, 1, 3, f);
    }
    fclose(f);
}

/* Debugging: D_VRAM_DUMP=<frame>[,<frame>...] writes vram_<frame>.bmp
 * and the displayed picture as shot_<frame>.bmp. */
static void debug_dump_vram(void)
{
    static char frames[4096];
    char name[64];
    if (!GetEnvironmentVariableA("D_VRAM_DUMP", frames, sizeof(frames))) {
        return;
    }
    for (char *p = frames; *p;) {
        if (atoi(p) == s_frame) {
            snprintf(name, sizeof(name), "vram_%d.bmp", s_frame);
            gpu_debug_dump(name);
            snprintf(name, sizeof(name), "shot_%d.bmp", s_frame);
            dump_picture(name);
        }
        while (*p && *p != ',') {
            p++;
        }
        if (*p == ',') {
            p++;
        }
    }
}

/* where the TV picture is in the window (draw_picture()) */
static float s_tv_x0, s_tv_y0, s_tv_x1, s_tv_y1;

/* The debug font's text (FntFlush), where its window is on the TV picture
 * (its y is in the drawing buffer: the second buffer is 240 lines down). */
#define TV_WIDTH                320.0f
#define TV_LINES                240

static void draw_font_text(void)
{
    int rect[4];
    const char *text;
    for (int i = 0; psx_font_text(i, rect, &text); i++) {
        if (!text[0]) {
            continue;
        }
        float sx = (s_tv_x1 - s_tv_x0) / TV_WIDTH, sy = (s_tv_y1 - s_tv_y0) / TV_LINES;
        ImVec2 at(s_tv_x0 + rect[0] * sx, s_tv_y0 + (rect[1] % TV_LINES) * sy);
        float size = ImGui::GetFontSize() * (sy > 1 ? sy / 2 : 1);
        ImDrawList *draw = ImGui::GetForegroundDrawList();
        draw->AddText(ImGui::GetFont(), size, ImVec2(at.x + 1, at.y + 1), IM_COL32(0, 0, 0, 200), text);
        draw->AddText(ImGui::GetFont(), size, at, IM_COL32(255, 255, 255, 255), text);
    }
}

/* The PS1 picture in the window: 4:3 with black bars, or stretched. */
static void draw_picture(int w, int h)
{
    int pw, ph;
    const uint32_t *picture = gpu_display_rgba(&pw, &ph);
    if (!picture) {
        return;                         /* display off: black */
    }
    ui_video_update(picture, pw, ph);
    float x0 = 0, y0 = 0, x1 = (float) w, y1 = (float) h;
    if (g_settings_pc.aspect == ASPECT_4_3) {
        if (w > h * PSX_ASPECT) {
            float width = h * PSX_ASPECT;
            x0 = (w - width) / 2;
            x1 = x0 + width;
        } else {
            float height = w / PSX_ASPECT;
            y0 = (h - height) / 2;
            y1 = y0 + height;
        }
    }
    /* where the display area appears on the TV (a smaller picture, e.g.
     * the movies, is shown with black around it) */
    s_tv_x0 = x0;
    s_tv_y0 = y0;
    s_tv_x1 = x1;
    s_tv_y1 = y1;
    float screen[4];
    gpu_display_screen(screen);
    float tw = x1 - x0, th = y1 - y0;
    ImVec2 p0(x0 + tw * screen[0], y0 + th * screen[1]), p1(x0 + tw * screen[2], y0 + th * screen[3]);
    ImGui::GetBackgroundDrawList()->PushClipRect(ImVec2(x0, y0), ImVec2(x1, y1), true);
    ui_draw_video(ImGui::GetBackgroundDrawList(), p0, p1, g_settings_pc.video_filter,
                  gpu_display_width(), gpu_display_lines());
    ImGui::GetBackgroundDrawList()->PopClipRect();
}

/* Esc or Circle (B on an Xbox controller) pressed on the title screen:
 * the program closes (the title only takes Start and Select). */
static int title_quit_pressed(void)
{
    static int esc_was_down, circle_was_down;
    int esc = input_key_down(VK_ESCAPE);
    int buttons = read_pad(0);
    int circle = buttons > 0 && (buttons & PAD_CIRCLE);
    int quit = psx_on_title_screen()
               && ((esc && !esc_was_down) || (circle && !circle_was_down));
    esc_was_down = esc;
    circle_was_down = circle;
    return quit;
}

/* small text with a shadow, centred at y (window pixels) */
static void hint_text(const char *text, float y, int w, ImU32 colour)
{
    ImFont *font = ImGui::GetFont();
    float size = ImGui::GetFontSize() * 1.15f;
    ImVec2 extent = font->CalcTextSizeA(size, FLT_MAX, 0, text);
    ImVec2 at((w - extent.x) / 2, y);
    ImDrawList *draw = ImGui::GetForegroundDrawList();
    draw->AddText(font, size, ImVec2(at.x + 1, at.y + 1), IM_COL32(0, 0, 0, 170), text);
    draw->AddText(font, size, at, colour, text);
}

/* The settings that take effect at once while the game runs. */
static void apply_game_settings(void)
{
    /* (tests: D_NATIVE=1 draws and sounds like the console, for comparisons) */
    int native = GetEnvironmentVariableA("D_NATIVE", NULL, 0) != 0;
    gpu_set_enhancements(native ? 1 : g_settings_pc.internal_scale);
    psx_set_sound_volumes(g_settings_pc.volume_master, g_settings_pc.volume_cd,
                          g_settings_pc.volume_effects);
    psx_set_movie_smoothing(!native && g_settings_pc.smooth_movies);
    psx_set_effects_bass_cut(!native && g_settings_pc.softer_bass);
    /* (tests: D_KEEP_LOGOS=1 plays them whatever the settings say) */
    psx_set_skip_logos(g_settings_pc.skip_intro && !GetEnvironmentVariableA("D_KEEP_LOGOS", NULL, 0));
}

/* F1 or Select+Start on controller 1 was pressed (not held) */
static int settings_key_pressed(void)
{
    static int was_down;
    int buttons = read_pad(0);
    int down = input_key_down(VK_F1)
               || (buttons > 0 && (buttons & PAD_SELECT_START) == PAD_SELECT_START);
    int pressed = down && !was_down;
    was_down = down;
    return pressed;
}

/*
 * The settings over the paused game (the game waits inside this VSync()
 * until they are closed with Esc, F1, Select+Start or "Close").  FALSE if
 * the window was closed meanwhile.
 */
static DWORD s_paused_ms;               /* time in game_settings(), for the log */

static int wait_escape_released(void)
{
    /* Esc must not reach the game (it would leave for the launcher) */
    while (input_key_down(VK_ESCAPE)) {
        if (!platform_pump()) {
            s_quit = 1;
            return 0;
        }
        input_poll();
        platform_present();
    }
    return 1;
}

/* The game paused (sound and the CD's clock stop, psx_set_paused()):
 * from the pause screen and while the settings are shown. */
static int s_paused;

static void set_paused(int paused)
{
    s_paused = paused;
    psx_set_paused(paused);
}

static int game_settings(void)
{
    int open = 1, first = 1;
    int esc_was_down = 1;
    int was_paused = s_paused;
    DWORD start = GetTickCount();
    set_paused(1);

    while (open) {
        int w, h;
        if (!platform_pump()) {
            s_quit = 1;
            return 0;
        }
        input_poll();
        int esc = input_key_down(VK_ESCAPE);
        if ((esc && !esc_was_down && !settings_capturing()) || settings_key_pressed()) {
            open = 0;
        }
        esc_was_down = esc;
        platform_clear(GAME_BACKGROUND);
        ui_begin_frame();
        platform_client_size(&w, &h);
        draw_picture(w, h);
        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), ImVec2((float) w, (float) h),
                                                      SETTINGS_DIM);
        float scale = platform_dpi_scale();
        ImVec2 size(SETTINGS_WIDTH * scale, SETTINGS_HEIGHT * scale);
        size.x = size.x > w - 32.0f ? w - 32.0f : size.x;
        size.y = size.y > h - 32.0f ? h - 32.0f : size.y;
        ImGui::SetNextWindowPos(ImVec2((w - size.x) / 2, (h - size.y) / 2));
        ImGui::SetNextWindowSize(size);
        if (first) {
            ImGui::SetNextWindowFocus();
            first = 0;
        }
        ImGui::Begin("Settings (the game is paused)", NULL,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove
                         | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
        if (!settings_screen()) {
            open = 0;
        }
        ImGui::End();
        ui_end_frame();
        platform_present();
        apply_game_settings();
    }
    settings_save();
    s_paused_ms += GetTickCount() - start;
    set_paused(was_paused);
    return wait_escape_released();
}

/*
 * Pause: Start, when the game has no use for it (psx_start_is_free(): not
 * in the intro, on the title or in the first movie; Select+Start still
 * opens the settings), or P / Pause, or R3 (the right stick pressed in)
 * on a controller, which the PS1 controller does not have.  Only the
 * player's own controllers and keyboard count (not the tests' D_INPUT).
 */
static int s_in_pause_menu;              /* (Enter is Start on the keyboard: not there) */

static int pause_pressed(void)
{
    static int r3_was_down, start_was_down;
    int r3 = 0;
    for (int pad = 0; pad < INPUT_MAX_PADS; pad++) {
        r3 |= input_pad_connected(pad) && input_pad_down(pad, PAD_R3);
    }
    int buttons = controls_psx_buttons(0);
    int start = buttons > 0 && (buttons & PAD_START) && !(buttons & PAD_SELECT)
                && !(s_in_pause_menu && (input_key_down(VK_RETURN) || input_key_down(VK_SPACE)));
    int start_pressed = start && !start_was_down && (s_paused || psx_start_is_free());
    int pressed = platform_take_key_press(VK_KEY_P) | platform_take_key_press(VK_PAUSE)
                  | (r3 && !r3_was_down) | start_pressed;
    r3_was_down = r3;
    start_was_down = start;
    return pressed;
}

/*
 * Reads save state `slot` for the next start of the game (run_game());
 * FALSE, with the reason in `error`, if it cannot be used.
 */
static int load_state(int slot, char *error, int size)
{
    if (!psx_state_load(slot)) {
        snprintf(error, size, "%s", psx_state_error());
        return 0;
    }
    int disc = psx_state_pending_disc();
    if (!launcher_disc_path(disc)) {
        snprintf(error, size, "That state is on disc %d, which has not been chosen (Choose discs).",
                 disc);
        psx_state_cancel_load();
        return 0;
    }
    return 1;
}

/* Pixels per layout pixel of the pause screen: it grows with the window
 * (a full-screen 1440p window is twice as tall as the layout), never
 * smaller than the monitor's DPI scale. */
static float pause_scale(int h)
{
    float dpi = platform_dpi_scale(), by_height = h / PAUSE_LAYOUT_HEIGHT;
    return by_height > dpi ? by_height : dpi;
}

/* The dimmed picture and "PAUSED" at y; returns the bottom of the text. */
static float draw_paused(int w, int h, float y)
{
    ImDrawList *draw = ImGui::GetBackgroundDrawList();
    draw->AddRectFilled(ImVec2(0, 0), ImVec2((float) w, (float) h), PAUSE_DIM);
    float size = PAUSE_TITLE_SIZE * pause_scale(h);
    ImVec2 extent = ui_font_display()->CalcTextSizeA(size, FLT_MAX, 0, "PAUSED");
    ImVec2 at((w - extent.x) / 2, y);
    draw->AddText(ui_font_display(), size, ImVec2(at.x + 2, at.y + 2), IM_COL32(0, 0, 0, 200),
                  "PAUSED");
    draw->AddText(ui_font_display(), size, at, IM_COL32(255, 255, 255, 235), "PAUSED");
    return y + extent.y;
}

/*
 * The pause screen: the picture stays (dimmed) with "PAUSED" and a menu:
 * continue, save a state, load one, the settings, back to the launcher.
 * The game waits inside this VSync(), its sound and the movie where they
 * stopped.  Start / P / Pause / R3 go on, F1 shows the settings, Esc / B
 * go back (Esc from the menu: to the launcher, FALSE, as when the window
 * is closed).
 *
 * Saving: a state is taken where the game passes the top of its main loop
 * (src/psx/savestate.c), so the game runs on to there - a frame or two,
 * silent and under the pause screen (s_saving) - and the pause screen
 * comes back with the result.  Loading: the game stops and starts again
 * with the state (run_game()).
 */
enum { PAUSE_MENU, PAUSE_SAVE, PAUSE_LOAD };
enum { CHOICE_NONE, CHOICE_CONTINUE, CHOICE_SAVE, CHOICE_LOAD, CHOICE_SETTINGS, CHOICE_LEAVE };

static int s_saving;                    /* slot + 1: a state is being saved */
static int s_saving_quietly;            /* (tests: D_SAVE_STATE, no pause screen after) */
static char s_pause_message[256];       /* the result of the last save or load */

static int game_paused(void)
{
    int result = 1, page = PAUSE_MENU, focus = 1;
    int esc_was_down = 1;
    DWORD start = GetTickCount();
    set_paused(1);
    states_refresh();
    s_in_pause_menu = 1;
    psx_log("paused at frame %d", s_frame);
    for (;;) {
        int w, h;
        if (!platform_pump()) {
            s_quit = 1;
            result = 0;
            break;
        }
        input_poll();
        if (pause_pressed()) {
            break;
        }
        int esc = input_key_down(VK_ESCAPE);
        int esc_pressed = esc && !esc_was_down;
        esc_was_down = esc;
        int choice = settings_key_pressed() ? CHOICE_SETTINGS : CHOICE_NONE;
        int slot = -1;
        int can_save = psx_state_can_save();

        platform_clear(GAME_BACKGROUND);
        ui_begin_frame();
        platform_client_size(&w, &h);
        draw_picture(w, h);
        /* (Dear ImGui's sizes and text grow by as much, put back below) */
        float scale = pause_scale(h), grow = scale / platform_dpi_scale();
        ImGuiStyle &style = ImGui::GetStyle();
        ImGuiStyle saved_style = style;
        style.ScaleAllSizes(grow);
        ImGui::PushFont(NULL, saved_style.FontSizeBase * grow);
        float bottom = draw_paused(w, h, h * (page == PAUSE_MENU ? PAUSE_TITLE_Y : PAUSE_LIST_TITLE_Y));
        int back = esc_pressed || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);

        if (page == PAUSE_MENU) {
            ImGui::SetNextWindowPos(ImVec2(w / 2.0f, bottom + 28 * scale), 0, ImVec2(0.5f, 0.0f));
        } else {
            ImGui::SetNextWindowPos(ImVec2(w / 2.0f, bottom + 20 * scale), 0, ImVec2(0.5f, 0.0f));
        }
        if (focus) {
            ImGui::SetNextWindowFocus();
            ImGui::SetNavCursorVisible(true);   /* Enter / A act on the selection at once */
        }
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18 * scale, 16 * scale));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8 * scale, 8 * scale));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10 * scale, 7 * scale));
        ImGui::Begin("pause", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize
                                        | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove);
        if (page == PAUSE_MENU) {
            ImVec2 button(PAUSE_MENU_WIDTH * scale, 0);
            if (focus) {
                ImGui::SetKeyboardFocusHere();
            }
            if (ImGui::Button("Continue", button)) {
                choice = CHOICE_CONTINUE;
            }
            ImGui::BeginDisabled(!can_save);
            if (ImGui::Button("Save state", button)) {
                choice = CHOICE_SAVE;
            }
            ImGui::EndDisabled();
            ImGui::BeginDisabled(!states_any());
            if (ImGui::Button("Load state", button)) {
                choice = CHOICE_LOAD;
            }
            ImGui::EndDisabled();
            if (ImGui::Button("Settings", button)) {
                choice = CHOICE_SETTINGS;
            }
            if (ImGui::Button("Back to the launcher", button)) {
                choice = CHOICE_LEAVE;
            }
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + button.x);
            if (!can_save) {
                ImGui::TextDisabled("%s", "Saving works while looking around, in the movies and in "
                                          "the inventory (not in the puzzles or on the title).");
            }
            if (s_pause_message[0]) {
                ImGui::PushStyleColor(ImGuiCol_Text, MESSAGE_COLOUR);
                ImGui::TextUnformatted(s_pause_message);
                ImGui::PopStyleColor();
            }
            ImGui::PopTextWrapPos();
        } else {
            ImGui::TextUnformatted(page == PAUSE_SAVE ? "Save state: choose a slot"
                                                      : "Load state: choose a slot");
            float rows = PSX_STATE_SLOTS * (STATES_ROW * scale + ImGui::GetStyle().ItemSpacing.y);
            float room = h - (bottom + 20 * scale) - ImGui::GetFrameHeightWithSpacing() * 2 - 60 * scale;
            ImGui::BeginChild("slots", ImVec2(STATES_WIDTH * scale, rows < room ? rows : room),
                              ImGuiChildFlags_NavFlattened);
            slot = states_list(page == PAUSE_SAVE ? STATES_SAVING : STATES_LOADING,
                               ImGui::GetContentRegionAvail().x, STATES_ROW * scale, focus, NULL);
            ImGui::EndChild();
            if (ImGui::Button("Back", ImVec2(PAUSE_MENU_WIDTH * scale * 0.6f, 0))) {
                back = 1;
            }
        }
        focus = 0;
        ImGui::End();
        ImGui::PopStyleVar(3);
        hint_text(page == PAUSE_MENU
                      ? "Start / P: continue    F1: settings    Esc: back to the launcher"
                      : "Enter / A: choose    Esc / B: back",
                  h - 40.0f * scale, w, IM_COL32(255, 255, 255, 200));
        ImGui::PopFont();
        style = saved_style;
        ui_end_frame();
        platform_present();

        if (back) {
            if (page != PAUSE_MENU) {
                page = PAUSE_MENU;
                focus = 1;
            } else if (esc_pressed) {
                choice = CHOICE_LEAVE;
            } else {
                choice = CHOICE_CONTINUE;   /* (B on a controller) */
            }
        }
        if (choice == CHOICE_CONTINUE) {
            break;
        } else if (choice == CHOICE_LEAVE) {
            result = 0;
            break;
        } else if (choice == CHOICE_SAVE || choice == CHOICE_LOAD) {
            page = choice == CHOICE_SAVE ? PAUSE_SAVE : PAUSE_LOAD;
            focus = 1;
            s_pause_message[0] = 0;
        } else if (choice == CHOICE_SETTINGS) {
            DWORD settings_start = GetTickCount();
            if (!game_settings()) {
                result = 0;
                break;
            }
            start += GetTickCount() - settings_start;   /* (counted there) */
            esc_was_down = 1;
            focus = 1;
        }
        if (slot >= 0 && page == PAUSE_SAVE) {
            if (!can_save) {
                snprintf(s_pause_message, sizeof(s_pause_message), "%s",
                         "The game cannot be saved here.");
                page = PAUSE_MENU;
                focus = 1;
            } else {
                psx_state_request_save(slot);
                s_saving = slot + 1;
                break;
            }
        } else if (slot >= 0 && page == PAUSE_LOAD) {
            char error[256];
            if (load_state(slot, error, sizeof(error))) {
                psx_log("loading state %d: the game starts again", slot + 1);
                result = 0;             /* (run_game() starts it again) */
                break;
            }
            snprintf(s_pause_message, sizeof(s_pause_message), "%s", error);
            page = PAUSE_MENU;
            focus = 1;
        }
    }
    s_in_pause_menu = 0;
    s_paused_ms += GetTickCount() - start;
    psx_log("going on after %lu ms", (unsigned long) (GetTickCount() - start));
    if (s_saving) {
        psx_set_muted(1);               /* (the game runs on to where it can be saved) */
    }
    set_paused(0);
    s_hold_buttons = 1;
    return result && wait_escape_released();
}

/*
 * A state is being saved: the game runs on (silent) to the top of its
 * main loop, with the pause screen shown, then the pause screen comes back
 * with the result.  FALSE if the game is to stop.
 */
static int saving_frame(int w, int h)
{
    draw_paused(w, h, h * PAUSE_TITLE_Y);
    int result = psx_state_save_result();
    if (result == 0) {
        return 1;
    }
    psx_set_muted(0);
    if (result > 0) {
        snprintf(s_pause_message, sizeof(s_pause_message), "Saved in slot %d.", s_saving);
    } else {
        snprintf(s_pause_message, sizeof(s_pause_message), "%s", psx_state_error());
    }
    s_saving = 0;
    if (s_saving_quietly) {
        s_saving_quietly = 0;
        psx_log("D_SAVE_STATE: %s", s_pause_message);
        s_pause_message[0] = 0;
        return 0;
    }
    return -1;                          /* the pause screen again */
}

/* Tests: D_SAVE_STATE=<frame>:<slot> saves a state in slot 1..8 at that
 * frame, as the pause screen does (without it); D_LOAD_STATE=<slot> with
 * D_AUTOSTART starts from one.  D_SAVE_DIR keeps them apart. */
static void test_save_state(void)
{
    char value[64];
    if (s_saving || !GetEnvironmentVariableA("D_SAVE_STATE", value, sizeof(value))
        || atoi(value) != s_frame) {
        return;
    }
    const char *colon = strchr(value, ':');
    int slot = colon ? atoi(colon + 1) : 1;
    psx_log("D_SAVE_STATE: slot %d at frame %d%s", slot, s_frame,
            psx_state_can_save() ? "" : " (the game cannot be saved here)");
    psx_state_request_save(slot - 1);
    s_saving = slot;
    s_saving_quietly = 1;
    psx_set_muted(1);
}

/*
 * The game waits for a disc that was not chosen in the launcher (the lid
 * is open, src/psx/libcd.c): the player chooses its image (or all of
 * them), which is checked in the background (launcher.cpp) with the
 * loading indicator meanwhile.  FALSE if the window was closed or the
 * player went back to the launcher.
 */
static int ask_for_disc(int number)
{
    static int was_checking;
    int w, h;
    if (!platform_pump()) {
        s_quit = 1;
        return 0;
    }
    input_poll();
    launcher_poll();
    int checking = launcher_checking();
    const char *path = launcher_disc_path(number);
    if (!checking && path) {
        psx_insert_disc(path);          /* the lid closes at the game's next look */
        was_checking = 0;
        return 1;
    }
    if (was_checking && !checking) {
        char message[160];
        snprintf(message, sizeof(message), "None of those is disc %d of D (USA).", number);
        platform_message(APP_NAME, message);
    }
    was_checking = checking;

    platform_clear(GAME_BACKGROUND);
    ui_begin_frame();
    platform_client_size(&w, &h);
    draw_picture(w, h);
    char text[400];
    float scale = platform_dpi_scale();
    ImGui::SetNextWindowPos(ImVec2(w / 2.0f, h * 0.8f), 0, ImVec2(0.5f, 0.5f));
    ImGui::Begin("disc", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize
                                   | ImGuiWindowFlags_NoSavedSettings);
    snprintf(text, sizeof(text), "Insert disc %d", number);
    ImGui::TextUnformatted(text);
    int chosen = 0;
    if (launcher_checking_text(text, sizeof(text))) {
        ImVec2 at = ImGui::GetCursorScreenPos();
        float line = ImGui::GetTextLineHeight();
        launcher_draw_spinner(ImGui::GetWindowDrawList(), ImVec2(at.x + line / 2, at.y + line / 2),
                              line * 0.4f, IM_COL32(214, 58, 52, 255));
        ImGui::SetCursorScreenPos(ImVec2(at.x + line * 1.4f, at.y));
        ImGui::TextUnformatted(text);
    } else {
        ImGui::TextUnformatted("Load the image of the disc (.bin, .cue or .chd) - or all of them -");
        ImGui::TextUnformatted("or drop it on the window. Esc: back to the launcher.");
        chosen = ImGui::Button("Choose discs...", ImVec2(220 * scale, 0));
    }
    ImGui::End();
    ui_end_frame();
    platform_present();

    char paths[PLATFORM_MAX_FILES][PLATFORM_PATH_MAX];
    int count = 0;
    if (!checking) {
        count = platform_take_dropped_files(paths, PLATFORM_MAX_FILES);
        if (!count && chosen) {
            count = platform_open_files_dialog(paths, PLATFORM_MAX_FILES);
        }
    }
    if (count) {
        launcher_check_discs(paths, count);
        was_checking = 1;
    }
    return !input_key_down(VK_ESCAPE);
}

/* Tests: D_PAUSE=<frame>:<milliseconds> - the game paused at that frame
 * for that long, as with P (the movie and its sound must go on where they
 * stopped). */
static void test_pause(void)
{
    char value[64];
    if (!GetEnvironmentVariableA("D_PAUSE", value, sizeof(value)) || atoi(value) != s_frame) {
        return;
    }
    const char *colon = strchr(value, ':');
    DWORD length = colon ? (DWORD) atoi(colon + 1) : 1000;
    set_paused(1);
    psx_log("D_PAUSE: paused at frame %d for %lu ms", s_frame, (unsigned long) length);
    DWORD start = GetTickCount();
    while (GetTickCount() - start < length && platform_pump()) {
        platform_present();
    }
    s_paused_ms += GetTickCount() - start;
    set_paused(0);
}

/* One frame of the game (from VSync()); FALSE stops the game. */
static int game_frame(void)
{
    int w, h;

    int missing = psx_missing_disc();
    if (missing) {
        ++s_frame;
        return ask_for_disc(missing);
    }
    if (!platform_pump()) {
        s_quit = 1;
        return 0;
    }
    input_poll();
    if (title_quit_pressed()) {
        s_quit = 1;                     /* closes the program */
        return 0;
    }
    g_psx_skip_frames = 0;
    platform_clear(GAME_BACKGROUND);
    ui_begin_frame();
    platform_client_size(&w, &h);
    draw_picture(w, h);
    draw_font_text();
    debug_dump_vram();
    int saved = s_saving ? saving_frame(w, h) : 0;
    if (s_saving) {
        /* (only the pause screen) */
    } else if (psx_on_title_screen()) {
        hint_text("B / Esc: quit    F1 / Select+Start: settings", h - 34.0f, w,
                  IM_COL32(255, 255, 255, 200));
    } else if (s_frame < HINT_FRAMES) {
        ImGui::GetForegroundDrawList()->AddText(
            ImVec2(12, 10), IM_COL32(255, 255, 255, 150),
            "Start (in the game) / P: pause    Esc: back to the launcher    F1 / Select+Start: settings");
    }
    ++s_frame;
    ui_end_frame();
    platform_present();
    if (saved < 0 && !game_paused()) {
        return 0;
    }
    if (s_saving) {
        return 1;
    }
    if (settings_key_pressed() && !game_settings()) {
        return 0;
    }
    if (pause_pressed() && !game_paused()) {
        return 0;
    }
    /* (tests: D_PAUSE=<frame>:<milliseconds> pauses there for that long) */
    test_pause();
    test_save_state();
    /* (tests: D_LEAVE=<frame> goes back to the launcher there, as Esc) */
    char leave[16];
    if (GetEnvironmentVariableA("D_LEAVE", leave, sizeof(leave)) && s_frame >= atoi(leave)) {
        return 0;
    }
    return !input_key_down(VK_ESCAPE);
}

/* the launcher's window: a splash of this size at 96 DPI */
#define SPLASH_WIDTH            1000
#define SPLASH_HEIGHT           600

static void show_splash(void)
{
    PlatformConfig config = {};
    config.width = g_settings_pc.launcher_width;
    config.height = g_settings_pc.launcher_height;
    config.window_mode = WINDOW_MODE_SPLASH;
    config.vsync = 1;
    platform_apply_config(&config);
}

/* keeps the launcher window's size (at 96 DPI) for next time */
static void remember_splash_size(void)
{
    int w, h;
    float scale = platform_dpi_scale();
    platform_client_size(&w, &h);
    if (w > 0 && h > 0) {
        g_settings_pc.launcher_width = (int) (w / scale + 0.5f);
        g_settings_pc.launcher_height = (int) (h / scale + 0.5f);
    }
}

/* the game's window: the display settings */
static void show_game_window(void)
{
    PlatformConfig config = {};
    config.width = g_settings_pc.window_width;
    config.height = g_settings_pc.window_height;
    config.window_mode = g_settings_pc.window_mode;
    config.vsync = g_settings_pc.vsync;
    platform_apply_config(&config);
}

static void run_game(void)
{
    const char *paths[SETTINGS_DISCS];
    for (int d = 0; d < SETTINGS_DISCS; d++) {
        paths[d] = launcher_disc_path(d + 1);
    }
    /* the game starts on disc 1, or on the disc of the state it loads */
    int first = psx_state_pending_disc() ? psx_state_pending_disc() : 1;
    psx_set_discs(paths, first);
    if (!disc_is_open() || disc_number() != first) {
        psx_state_cancel_load();
        char message[80];
        snprintf(message, sizeof(message), "Disc %d cannot be read.", first);
        platform_message(APP_NAME, message);
        return;
    }
    settings_save();
    platform_take_key_press(VK_KEY_P);
    platform_take_key_press(VK_PAUSE);
    set_paused(0);
    psx_set_muted(0);
    s_saving = s_saving_quietly = 0;
    s_pause_message[0] = 0;
    launcher_set_in_game(1);
    show_game_window();
    s_frame = 0;
    char wav_path[MAX_PATH];
    if (GetEnvironmentVariableA("D_WAV_DUMP", wav_path, sizeof(wav_path))) {
        s_wav = fopen(wav_path, "wb");
        s_wav_bytes = 0;
        if (s_wav) {
            wav_header(s_wav, 0);
        }
    }
    /* (D_MUTE=1: no sound device, e.g. for recording tests) */
    int mute = GetEnvironmentVariableA("D_MUTE", NULL, 0) != 0;
    int sound = !mute && audio_init();
    if (!sound && !mute) {
        psx_log("no sound device (XAudio2)");
    }
    psx_set_audio_sink(sound_sink);
    /* the sound in real time on its own thread; in step with the frames
     * when it is not heard */
    psx_set_sound_realtime(sound && audio_stream_start(sound_stream, PSX_SOUND_FRAME_SAMPLES));
    apply_game_settings();
    DWORD start = GetTickCount();
    s_paused_ms = 0;
    for (;;) {
        psx_run_game(game_frame);
        /* a state loaded in the pause screen: the game starts again with it */
        int disc = psx_state_pending_disc();
        if (s_quit || !disc) {
            break;
        }
        psx_set_discs(paths, disc);
        if (!disc_is_open() || disc_number() != disc) {
            psx_state_cancel_load();
            platform_message(APP_NAME, "The disc of that state cannot be read.");
            break;
        }
        set_paused(0);
        psx_set_muted(0);
        s_saving = 0;
    }
    psx_state_cancel_load();
    double seconds = (GetTickCount() - start - s_paused_ms) / 1000.0;
    psx_log("%d frames in %.1f s (%.1f fps)", s_frame, seconds,
            seconds > 0 ? s_frame / seconds : 0.0);
    audio_stream_stop();
    audio_stop();
    if (s_wav) {             /* (the sound thread has stopped) */
        wav_header(s_wav, s_wav_bytes);
        fclose(s_wav);
        s_wav = NULL;
    }
    disc_close();
    /* wait for Esc to be released, or the launcher sees it too */
    while (!s_quit && input_key_down(VK_ESCAPE) && platform_pump()) {
        input_poll();
        platform_present();
    }
    launcher_set_in_game(0);
    if (!s_quit) {
        show_splash();
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    PlatformConfig config = {};

    SetUnhandledExceptionFilter(crash_handler);
    /* first, before DLLs could take its addresses */
    if (!psx_system_init()) {
        MessageBoxW(NULL, L"The PS1 memory (0x80000000) could not be reserved.", L"D",
                    MB_ICONERROR);
        return 1;
    }
    settings_load();
    if (g_settings_pc.launcher_width < PLATFORM_SPLASH_MIN_W
        || g_settings_pc.launcher_height < PLATFORM_SPLASH_MIN_H) {
        g_settings_pc.launcher_width = SPLASH_WIDTH;
        g_settings_pc.launcher_height = SPLASH_HEIGHT;
    }
    config.width = g_settings_pc.launcher_width;
    config.height = g_settings_pc.launcher_height;
    config.window_mode = WINDOW_MODE_SPLASH;
    config.vsync = 1;
    if (!platform_init(&config)) {
        return 1;
    }
    ui_init();
    launcher_init();
    psx_set_pad_reader(read_pad);
    /* debugging: D_AUTOSTART=1 starts the game right away (once the discs
     * of the settings are checked) */
    if (GetEnvironmentVariableA("D_AUTOSTART", NULL, 0)) {
        while (launcher_checking() && platform_pump()) {
            launcher_poll();
            Sleep(10);
        }
        char load[16], error[256];
        if (GetEnvironmentVariableA("D_LOAD_STATE", load, sizeof(load))
            && !load_state(atoi(load) - 1, error, sizeof(error))) {
            psx_log("D_LOAD_STATE: %s", error);
        }
        if (launcher_disc_path(1)) {
            run_game();
        }
    }

    while (!s_quit && platform_pump()) {
        input_poll();
        platform_clear(BACKGROUND);
        ui_begin_frame();
        int action = launcher_frame();
        ui_end_frame();
        platform_present();
        if (!IsIconic((HWND) platform_hwnd())) {
            remember_splash_size();     /* (saved with the settings) */
        }
        if (action == LAUNCHER_PLAY) {
            settings_save();
            run_game();
            states_refresh();
        } else if (action == LAUNCHER_LOAD_STATE) {
            char error[256];
            settings_save();
            if (load_state(launcher_state_slot(), error, sizeof(error))) {
                run_game();
                states_refresh();
            } else {
                platform_message(APP_NAME, error);
            }
        } else if (action == LAUNCHER_QUIT) {
            break;
        }
    }
    settings_save();
    audio_shutdown();
    launcher_shutdown();
    ui_shutdown();
    platform_shutdown();
    return 0;
}
