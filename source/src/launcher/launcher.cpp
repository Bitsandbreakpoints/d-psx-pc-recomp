/*
 * launcher.cpp - the launcher and the settings (Dear ImGui).
 *
 * The launcher is a small borderless splash window (WINDOW_MODE_SPLASH),
 * whatever the game's display settings: a dark background drawn here, the
 * title, a menu of text items (mouse, keyboard or controller), the state
 * of the three discs and the button hints.  "Settings" turns it into a settings page in the same
 * style, "Load game" into the list of save states (launcher/states.cpp): load
 * one, or delete it.  The settings rows are shared with the in-game settings window
 * (settings_screen(), F1).
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <float.h>
#include <math.h>
#include "imgui.h"
#include "launcher/launcher.h"
#include "launcher/settings.h"
#include "launcher/controls.h"
#include "platform/platform.h"
#include "platform/input.h"
#include "disc/disc.h"
#include "launcher/ui.h"
#include "launcher/states.h"
#include "psx/savestate.h"

/* window size presets (client area) */
static const struct { int w, h; } WINDOW_SIZES[] = {
    { 960, 720 }, { 1280, 960 }, { 1440, 1080 }, { 1920, 1440 },
    { 1280, 720 }, { 1600, 900 }, { 1920, 1080 }, { 2560, 1440 },
};

/* colours */
#define COL_ACCENT              IM_COL32(214, 58, 52, 255)
#define COL_ACCENT_SOFT         IM_COL32(214, 58, 52, 56)
#define COL_ACCENT_NONE         IM_COL32(214, 58, 52, 0)
#define COL_TEXT                IM_COL32(246, 240, 232, 255)
#define COL_TEXT_DIM            IM_COL32(246, 240, 232, 150)
#define COL_TEXT_FAINT          IM_COL32(246, 240, 232, 95)
#define COL_TEXT_DISABLED       IM_COL32(246, 240, 232, 60)
#define COL_PANEL               IM_COL32(14, 8, 8, 210)
#define COL_KEY                 IM_COL32(246, 240, 232, 120)
#define COL_OK                  IM_COL32(120, 214, 130, 255)
#define COL_WARN                IM_COL32(242, 214, 90, 255)
#define COL_BAD                 IM_COL32(255, 115, 102, 255)

static const ImVec4 ACCENT_V4(0.84f, 0.23f, 0.20f, 1.0f);

/* the three discs: their state after disc_check() and its explanation */
static int s_disc_status[SETTINGS_DISCS];
static char s_disc_detail[SETTINGS_DISCS][256];
static int s_in_game;                   /* the game runs: display settings apply at once */
static int s_in_launcher;               /* inside launcher_frame(): sizes follow the window */
static float s_scale = 1.0f;            /* ... by this (pixels per pixel at 96 DPI) */

/* pixels per layout pixel: the launcher scales with its window, the
 * in-game settings with the monitor's DPI */
static float ui_scale(void)
{
    return s_in_launcher ? s_scale : platform_dpi_scale();
}

/* "press a button" capture */
static int s_capture_player = -1;
static int s_capture_button;
static int s_capture_pad;               /* TRUE: a controller input, else a key */

#define CHECK_SIZE              0.75f   /* a checkbox's side, of a row's height */

/* ImGui::Checkbox for an int setting: a little smaller than the other
 * controls, centred in a row of the usual height */
static bool check(const char *label, int *value)
{
    bool b = *value != 0;
    float frame = ImGui::GetFrameHeight(), pad = ImGui::GetStyle().FramePadding.y;
    float small = (frame * CHECK_SIZE - ImGui::GetFontSize()) / 2;
    small = small < 0 ? 0 : small;
    float top = ImGui::GetCursorPosY();
    ImGui::SetCursorPosY(top + pad - small);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(small, small));
    bool changed = ImGui::Checkbox(label, &b);
    ImGui::PopStyleVar();
    /* (the row keeps its full height; a note after it lines up with the label) */
    ImGui::SameLine(0, 0);
    ImGui::SetCursorPosY(top);
    ImGui::Dummy(ImVec2(0, frame));
    *value = b;
    return changed;
}

static int disc_usable(int d)
{
    return s_disc_status[d] == DISC_OK || s_disc_status[d] == DISC_OTHER_DUMP;
}

/* The game starts on disc 1; the others are asked for when needed. */
static int disc_playable(void)
{
    return disc_usable(0);
}

/* "size:time" of a file, to know if a verified image has changed since */
static void file_stamp(const char *path, char *out, int size)
{
    wchar_t wpath[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA a;
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH);
    if (!GetFileAttributesExW(wpath, GetFileExInfoStandard, &a)) {
        snprintf(out, size, "");
        return;
    }
    snprintf(out, size, "%lu%08lu:%lu%08lu", a.nFileSizeHigh, a.nFileSizeLow,
             a.ftLastWriteTime.dwHighDateTime, a.ftLastWriteTime.dwLowDateTime);
}

/*
 * Checking the discs runs on a thread of its own, so the window keeps
 * drawing (the first time, the checksum of a disc takes a few seconds):
 * the thread works on a copy of the discs' paths and verification stamps
 * and leaves its results per disc; launcher_poll() takes them over.
 */
typedef struct {
    int set;                            /* this disc was checked */
    int status;
    char detail[256];
    char path[SETTINGS_PATH_MAX];
    char stamp[64];                     /* when verified */
} DiscResult;

static struct {
    HANDLE thread;
    volatile LONG busy, done;
    volatile LONG current;              /* index into paths[] being checked */
    char paths[PLATFORM_MAX_FILES + SETTINGS_DISCS][SETTINGS_PATH_MAX];
    int count;
    int find_others;
    /* the settings when it started */
    char known_paths[SETTINGS_DISCS][SETTINGS_PATH_MAX];
    char known_stamps[SETTINGS_DISCS][64];
    int usable[SETTINGS_DISCS];
    DiscResult results[SETTINGS_DISCS];
    DiscResult not_d;                   /* a file that is not a disc of D */
    int just_finished;                  /* the menu puts the focus on PLAY */
} s_check;

/*
 * Checks one image; a disc of D goes into its result slot.  The data
 * track's checksum is only computed the first time; afterwards the file's
 * size and time say it is unchanged.  Returns the disc's number or 0.
 */
static int check_disc(const char *path)
{
    char detail[256], stamp[64];
    int number = 0;
    file_stamp(path, stamp, sizeof(stamp));
    int known = 0;
    for (int d = 0; d < SETTINGS_DISCS; d++) {
        known |= strcmp(s_check.known_paths[d], path) == 0 && stamp[0]
                 && strcmp(s_check.known_stamps[d], stamp) == 0;
    }
    int status = disc_check(path, !known, &number, detail, sizeof(detail));
    disc_close();
    if (number < 1 || number > SETTINGS_DISCS) {
        DiscResult *r = &s_check.not_d;
        r->set = 1;
        r->status = status;
        snprintf(r->detail, sizeof(r->detail), "%s", detail);
        return 0;
    }
    DiscResult *r = &s_check.results[number - 1];
    if (known) {
        status = DISC_OK;
        snprintf(detail, sizeof(detail), "D (USA) disc %d – verified.", number);
    }
    r->set = 1;
    r->status = status;
    snprintf(r->detail, sizeof(r->detail), "%s", detail);
    snprintf(r->path, sizeof(r->path), "%s", path);
    snprintf(r->stamp, sizeof(r->stamp), "%s", status == DISC_OK ? stamp : "");
    return number;
}

static int result_usable(int d)
{
    const DiscResult *r = &s_check.results[d];
    return r->set ? r->status == DISC_OK || r->status == DISC_OTHER_DUMP : s_check.usable[d];
}

/*
 * The other discs of a set lie next to a chosen one, with the same name
 * but another number: "D (USA) (Disc 1).chd" -> "... (Disc 2).chd".  They
 * are added to the list (for the discs still missing).
 */
static void add_other_discs(const char *path, int number)
{
    const char *name = path;
    for (const char *p = path; *p; p++) {
        if (*p == '\\' || *p == '/') {
            name = p + 1;
        }
    }
    /* the last digit in the file name that is the disc's number */
    const char *digit = NULL;
    for (const char *p = name; *p; p++) {
        if (*p == '0' + number) {
            digit = p;
        }
    }
    if (!digit) {
        return;
    }
    for (int d = 0; d < SETTINGS_DISCS; d++) {
        if (d + 1 == number || result_usable(d) || s_check.count >= (int) ARRAYSIZE(s_check.paths)) {
            continue;
        }
        char *other = s_check.paths[s_check.count];
        snprintf(other, SETTINGS_PATH_MAX, "%s", path);
        other[digit - path] = (char) ('1' + d);
        wchar_t wpath[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, other, -1, wpath, MAX_PATH);
        int already = 0;
        for (int i = 0; i < s_check.count; i++) {
            already |= strcmp(s_check.paths[i], other) == 0;
        }
        if (!already && GetFileAttributesW(wpath) != INVALID_FILE_ATTRIBUTES) {
            s_check.count++;
        }
    }
}

static DWORD WINAPI check_thread(void *)
{
    int chosen = s_check.count;
    for (int i = 0; i < s_check.count; i++) {
        InterlockedExchange(&s_check.current, i);
        int number = check_disc(s_check.paths[i]);
        if (number && s_check.find_others && i < chosen) {
            add_other_discs(s_check.paths[i], number);
        }
    }
    InterlockedExchange(&s_check.done, 1);
    return 0;
}

/* Starts checking `count` images (and, with find_others, their missing
 * siblings); FALSE if a check is still running. */
static int start_check(char paths[][SETTINGS_PATH_MAX], int count, int find_others)
{
    if (s_check.busy || count <= 0) {
        return 0;
    }
    memset(s_check.results, 0, sizeof(s_check.results));
    memset(&s_check.not_d, 0, sizeof(s_check.not_d));
    s_check.count = count < PLATFORM_MAX_FILES ? count : PLATFORM_MAX_FILES;
    for (int i = 0; i < s_check.count; i++) {
        snprintf(s_check.paths[i], SETTINGS_PATH_MAX, "%s", paths[i]);
    }
    s_check.find_others = find_others;
    memcpy(s_check.known_paths, g_settings_pc.disc_paths, sizeof(s_check.known_paths));
    memcpy(s_check.known_stamps, g_settings_pc.disc_verified, sizeof(s_check.known_stamps));
    for (int d = 0; d < SETTINGS_DISCS; d++) {
        s_check.usable[d] = disc_usable(d);
    }
    s_check.current = 0;
    s_check.done = 0;
    s_check.busy = 1;
    s_check.thread = CreateThread(NULL, 0, check_thread, NULL, 0, NULL);
    if (!s_check.thread) {
        check_thread(NULL);             /* (no thread: here, then) */
    }
    return 1;
}

int launcher_checking(void)
{
    return s_check.busy != 0;
}

void launcher_poll(void)
{
    if (!s_check.busy || !s_check.done) {
        return;
    }
    if (s_check.thread) {
        WaitForSingleObject(s_check.thread, INFINITE);
        CloseHandle(s_check.thread);
        s_check.thread = NULL;
    }
    for (int d = 0; d < SETTINGS_DISCS; d++) {
        const DiscResult *r = &s_check.results[d];
        if (!r->set) {
            continue;
        }
        s_disc_status[d] = r->status;
        snprintf(s_disc_detail[d], sizeof(s_disc_detail[d]), "%s", r->detail);
        snprintf(g_settings_pc.disc_paths[d], sizeof(g_settings_pc.disc_paths[d]), "%s", r->path);
        snprintf(g_settings_pc.disc_verified[d], sizeof(g_settings_pc.disc_verified[d]), "%s",
                 r->stamp);
    }
    /* a file that is not a disc of D: shown in the slot of disc 1 if it is empty */
    if (s_check.not_d.set && !disc_usable(0)) {
        s_disc_status[0] = s_check.not_d.status;
        snprintf(s_disc_detail[0], sizeof(s_disc_detail[0]), "%s", s_check.not_d.detail);
    }
    settings_save();
    s_check.busy = 0;
    s_check.just_finished = 1;
}

/* What is being checked, for the loading indicator. */
static void checking_text(char *out, int size)
{
    int i = (int) s_check.current;
    const char *path = i >= 0 && i < s_check.count ? s_check.paths[i] : "";
    const char *name = path;
    for (const char *p = path; *p; p++) {
        if (*p == '\\' || *p == '/') {
            name = p + 1;
        }
    }
    int progress = disc_check_progress();
    if (progress > 0) {
        snprintf(out, size, "Checking %s  %d %%", name, progress / 10);
    } else {
        snprintf(out, size, "Checking %s", name);
    }
}

int launcher_check_discs(char paths[][PLATFORM_PATH_MAX], int count)
{
    return start_check(paths, count, 1);
}

int launcher_checking_text(char *out, int size)
{
    if (!s_check.busy) {
        return 0;
    }
    checking_text(out, size);
    return 1;
}

/* A spinning ring of dots (the loading indicator). */
#define SPINNER_DOTS            8
#define SPINNER_TURNS_PER_SECOND 1.2f

void launcher_draw_spinner(ImDrawList *draw, ImVec2 centre, float radius, ImU32 colour)
{
    float turn = (float) ImGui::GetTime() * SPINNER_TURNS_PER_SECOND;
    int head = (int) (turn * SPINNER_DOTS) % SPINNER_DOTS;
    for (int i = 0; i < SPINNER_DOTS; i++) {
        float angle = i * 2 * 3.14159265f / SPINNER_DOTS;
        int age = (head - i + SPINNER_DOTS) % SPINNER_DOTS;
        int alpha = 255 - age * 255 / SPINNER_DOTS;
        ImU32 c = (colour & ~IM_COL32_A_MASK) | ((ImU32) alpha << IM_COL32_A_SHIFT);
        draw->AddCircleFilled(ImVec2(centre.x + cosf(angle) * radius, centre.y + sinf(angle) * radius),
                              radius * 0.22f, c);
    }
}

static void choose_discs(char paths[][SETTINGS_PATH_MAX], int count)
{
    start_check(paths, count, 1);
}

void launcher_set_in_game(int in_game)
{
    s_in_game = in_game;
}

/* The game's display settings; the launcher keeps its splash window. */
static void apply_window_settings(void)
{
    if (!s_in_game) {
        return;
    }
    PlatformConfig config = {};
    config.width = g_settings_pc.window_width;
    config.height = g_settings_pc.window_height;
    config.window_mode = g_settings_pc.window_mode;
    config.vsync = g_settings_pc.vsync;
    platform_apply_config(&config);
}

/* ---- settings rows ---------------------------------------------------- */

#define ROW_LABEL_WIDTH         190.0f  /* before DPI scaling */

/* a settings row: the label, then the control filling the rest */
static void row(const char *label)
{
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(ROW_LABEL_WIDTH * ui_scale());
    ImGui::SetNextItemWidth(-FLT_MIN);
}

/* a group of rows: its name, small and red, and a thin line */
static void section(const char *title)
{
    ImGui::Dummy(ImVec2(1, 4 * ui_scale()));
    ImGui::PushFont(ui_font_display(), ImGui::GetFontSize() * 0.9f);
    ImGui::TextColored(ACCENT_V4, "%s", title);
    ImGui::PopFont();
    ImGui::Separator();
}

/* a note after a row's control */
static void note(const char *text)
{
    ImGui::SameLine();
    ImGui::TextDisabled("%s", text);
}

static void help(const char *text)
{
    ImGui::Spacing();
    ImGui::PushTextWrapPos(0);
    ImGui::TextDisabled("%s", text);
    ImGui::PopTextWrapPos();
}

/* ---- settings tabs -------------------------------------------------- */

static void graphics_tab(void)
{
    Settings *s = &g_settings_pc;
    const char *modes[] = { "Window", "Fullscreen (borderless)" };
    const char *aspects[] = { "4:3 (original)", "Stretched" };
    const char *filters[] = { "Sharp pixels", "Bilinear (smooth scaling)", "CRT (tube TV)",
                              "Soft TV (GTU-like)" };
    char current[64], format[64];

    section("DISPLAY");
    row("Display mode");
    if (ImGui::Combo("##mode", &s->window_mode, modes, 2)) {
        apply_window_settings();
    }
    snprintf(current, sizeof(current), "%d × %d", s->window_width, s->window_height);
    ImGui::BeginDisabled(s->window_mode != WINDOW_MODE_WINDOWED);
    row("Window size");
    if (ImGui::BeginCombo("##size", current)) {
        for (const auto &size : WINDOW_SIZES) {
            char label[64];
            snprintf(label, sizeof(label), "%d × %d%s", size.w, size.h,
                     size.w * 3 == size.h * 4 ? "  (4:3)" : "  (16:9)");
            if (ImGui::Selectable(label, s->window_width == size.w && s->window_height == size.h)) {
                s->window_width = size.w;
                s->window_height = size.h;
                apply_window_settings();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    row("Vsync");
    if (check("##vsync", &s->vsync)) {
        apply_window_settings();
    }

    section("PICTURE");
    snprintf(format, sizeof(format), "%%d×    (%d × %d)", 320 * s->internal_scale,
             240 * s->internal_scale);
    row("Internal resolution");
    ImGui::SliderInt("##scale", &s->internal_scale, 1, HIRES_MAX_SCALE, format);
    row("Aspect ratio");
    ImGui::Combo("##aspect", &s->aspect, aspects, 2);
    row("Picture filter");
    ImGui::Combo("##filter", &s->video_filter, filters, 4);
    row("Smooth movies");
    check("##movies", &s->smooth_movies);
    note("softer colour edges in the movies");
    help("The internal resolution sharpens the turning and scaled pictures of the puzzles (the "
         "movies keep their own resolution). Everything here can be changed during the game "
         "with F1.");
}

static const char *device_name(int device)
{
    static char name[64];
    if (device == DEVICE_NONE) return "None";
    if (device == DEVICE_KEYBOARD) return "Keyboard";
    if (device == DEVICE_AUTO) return "Automatic";
    snprintf(name, sizeof(name), "Controller %d%s", device,
             input_pad_connected(device - DEVICE_PAD_1) ? "  (connected)" : "");
    return name;
}

/* Assigns `value` to button b; a button that had it already gets b's old one. */
static void assign(int *table, int b, int value)
{
    for (int i = 0; i < PSX_BUTTON_COUNT; i++) {
        if (i != b && table[i] == value) {
            table[i] = table[b];
        }
    }
    table[b] = value;
}

static void poll_capture(void)
{
    ImGuiIO &io = ImGui::GetIO();
    if (s_capture_player < 0) {
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        return;
    }
    /* the key pressed must not also move through the menu */
    io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
    ControllerConfig *c = &g_settings_pc.players[s_capture_player];
    if (s_capture_pad) {
        int pad = controls_player_pad(s_capture_player);
        if (input_new_key() == VK_ESCAPE) {
            s_capture_player = -1;
            return;
        }
        int in = pad >= 0 ? input_new_pad_input(pad) : PAD_NONE;
        if (in != PAD_NONE) {
            assign(c->pad, s_capture_button, in);
            s_capture_player = -1;
        }
    } else {
        int vk = input_new_key();
        if (vk == VK_ESCAPE) {
            s_capture_player = -1;
        } else if (vk) {
            assign(c->keys, s_capture_button, vk);
            s_capture_player = -1;
        }
    }
}

static void controls_player(int p)
{
    ControllerConfig *c = &g_settings_pc.players[p];
    int pad = controls_player_pad(p);

    row("Device");
    if (ImGui::BeginCombo("##device", device_name(c->device))) {
        for (int d = DEVICE_NONE; d <= DEVICE_AUTO; d++) {
            if (ImGui::Selectable(device_name(d), c->device == d)) {
                c->device = d;
            }
        }
        ImGui::EndCombo();
    }
    if (pad >= 0) {
        ImGui::TextColored(ACCENT_V4, "Controller %d connected – the mapping below is used.", pad + 1);
    } else if (c->device != DEVICE_NONE && c->device != DEVICE_KEYBOARD) {
        ImGui::TextDisabled("No controller connected%s.", p == 0 ? " – the keyboard is used" : "");
    }

    if (ImGui::BeginTable("map", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("PS1 button", ImGuiTableColumnFlags_WidthFixed, 110 * ui_scale());
        ImGui::TableSetupColumn("Keyboard");
        ImGui::TableSetupColumn("Controller (XInput)");
        ImGui::TableHeadersRow();
        for (int b = 0; b < PSX_BUTTON_COUNT; b++) {
            char label[96];
            ImGui::PushID(b);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(input_psx_button_name(b));
            ImGui::TableNextColumn();
            int waiting = s_capture_player == p && s_capture_button == b;
            snprintf(label, sizeof(label), "%s###k",
                     waiting && !s_capture_pad ? "Press a key… (Esc cancels)" : input_key_name(c->keys[b]));
            if (ImGui::Button(label, ImVec2(-1, 0))) {
                s_capture_player = p;
                s_capture_button = b;
                s_capture_pad = 0;
            }
            ImGui::TableNextColumn();
            snprintf(label, sizeof(label), "%s###p",
                     waiting && s_capture_pad ? (pad >= 0 ? "Press a controller button…" : "No controller connected")
                                               : input_pad_input_name(c->pad[b]));
            if (ImGui::Button(label, ImVec2(-1, 0))) {
                s_capture_player = p;
                s_capture_button = b;
                s_capture_pad = 1;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    check("The left stick also moves the directions", &c->stick_as_dpad);
    if (ImGui::Button("Auto-map controller")) {
        settings_default_pad(c);
    }
    ImGui::SameLine();
    if (ImGui::Button("Keyboard defaults")) {
        settings_default_keys(c);
    }
}

/* (D is for one player: the controller in port 1) */
static void controls_tab(void)
{
    poll_capture();
    controls_player(0);
}

static void sound_tab(void)
{
    Settings *s = &g_settings_pc;
    section("VOLUME");
    row("Master");
    ImGui::SliderInt("##master", &s->volume_master, 0, 100, "%d %%");
    row("Movies and music");
    ImGui::SliderInt("##cd", &s->volume_cd, 0, 100, "%d %%");
    row("Sound effects");
    ImGui::SliderInt("##effects", &s->volume_effects, 0, 100, "%d %%");
    help("Heard at once in the game (F1). Movies and music = the sound from the disc, "
         "effects = the sounds of the castle and the puzzles.");
    section("TONE");
    row("Softer bass");
    check("##softer_bass", &s->softer_bass);
    note("in the sound effects");
    help("Some sound effects, such as scrolling in the inventory, are a deep thump that can "
         "make speakers rattle. This takes the deepest bass out of them, as the small TV "
         "speaker the game was made for did. The movies and the music are not changed. "
         "Off: the effects exactly as on the console.");
}

static void gameplay_tab(void)
{
    Settings *s = &g_settings_pc;
    section("GAME");
    row("Skip the logos");
    check("##intro", &s->skip_intro);
    note("Acclaim and WARP before the title");
}

static void reset_all(void)
{
    Settings kept = g_settings_pc;
    settings_defaults(&g_settings_pc);
    memcpy(g_settings_pc.disc_paths, kept.disc_paths, sizeof(kept.disc_paths));
    memcpy(g_settings_pc.disc_verified, kept.disc_verified, sizeof(kept.disc_verified));
    apply_window_settings();
}

int settings_capturing(void)
{
    return s_capture_player >= 0;
}

/* The settings with tabs (the in-game window, F1). */
int settings_screen(void)
{
    int open = 1;
    if (ImGui::BeginTabBar("settings")) {
        if (ImGui::BeginTabItem("Graphics")) { graphics_tab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Controls")) { controls_tab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Sound")) { sound_tab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Game")) { gameplay_tab(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::Separator();
    if (ImGui::Button("Close", ImVec2(160 * ui_scale(), 0)) && s_capture_player < 0) {
        settings_save();
        open = 0;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset all")) {
        reset_all();
    }
    return open;
}

/* ---- the launcher --------------------------------------------------- */

/* layout, in pixels at 96 DPI */
#define MARGIN                  52.0f
#define TAG_SIZE                15.0f
#define TITLE_SIZE              66.0f
#define MENU_SIZE               30.0f
#define MENU_WIDTH              360.0f
#define PAGE_TITLE_SIZE         40.0f
#define TAB_SIZE                22.0f
#define TAB_WIDTH               200.0f
#define SMALL_SIZE              14.0f
#define PANEL_LEFT              280.0f      /* the settings panel's left edge */
#define STATE_ROW               64.0f       /* a save state's row */
#define MENU_TOP                0.36f       /* of the window's height */
#define WINDOW_BUTTON           40.0f       /* minimise / close, top right */

enum { PAGE_MENU, PAGE_SETTINGS, PAGE_STATES };
enum { TAB_GRAPHICS, TAB_CONTROLS, TAB_SOUND, TAB_GAMEPLAY, TAB_COUNT };

static int s_page = PAGE_MENU;
static int s_tab = TAB_GRAPHICS;
static int s_focus = 1;                 /* put the keyboard/controller focus on the page */
static int s_state_slot = -1;           /* chosen on the "Load game" page */
static int s_delete_slot = -1;          /* the state the player is asked to delete */
static char s_states_note[160];         /* why a state was not loaded */

void launcher_shutdown(void)
{
    states_free();
    if (s_check.thread) {
        WaitForSingleObject(s_check.thread, INFINITE);     /* (a check still running) */
        CloseHandle(s_check.thread);
        s_check.thread = NULL;
    }
}

/* Text with a soft shadow, so it stays readable on the photo. */
static void shadowed_text(ImDrawList *draw, ImFont *font, float size, ImVec2 pos, ImU32 colour,
                          const char *text)
{
    float off = size / 20 > 1 ? size / 20 : 1;
    draw->AddText(font, size, ImVec2(pos.x + off, pos.y + off), IM_COL32(0, 0, 0, 170), text);
    draw->AddText(font, size, pos, colour, text);
}

/*
 * A menu item: large text, no box; when selected (hovered or focused) a
 * red bar and a glow fading to the right.  The mouse moves the focus
 * too, so there is always one selection.  Returns TRUE when chosen.
 */
static bool s_mouse_focus;              /* the selection follows the mouse, not the keys */

/* TRUE when the keyboard or a controller moves the selection */
static bool nav_key_pressed(void)
{
    static const ImGuiKey KEYS[] = { ImGuiKey_UpArrow, ImGuiKey_DownArrow, ImGuiKey_Tab,
                                     ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadDpadDown,
                                     ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown };
    for (int i = 0; i < (int) (sizeof(KEYS) / sizeof(KEYS[0])); i++) {
        if (ImGui::IsKeyPressed(KEYS[i])) {
            return true;
        }
    }
    return false;
}

static bool menu_item(const char *label, float size, float width, bool enabled, bool current)
{
    float scale = ui_scale();
    ImFont *font = ui_font_display();
    float height = size * 1.55f;

    ImGui::BeginDisabled(!enabled);
    /* (its own highlight instead of Dear ImGui's focus frame) */
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
    bool chosen = ImGui::InvisibleButton(label, ImVec2(width, height), ImGuiButtonFlags_EnableNav);
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
    if (enabled && ImGui::IsItemHovered() && (ImGui::GetIO().MouseDelta.x != 0
                                              || ImGui::GetIO().MouseDelta.y != 0)) {
        ImGui::SetKeyboardFocusHere(-1);
        s_mouse_focus = true;
    }
    bool hot = enabled && (ImGui::IsItemHovered() || ImGui::IsItemFocused());
    ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
    ImDrawList *draw = ImGui::GetWindowDrawList();

    if (hot) {
        draw->AddRectFilledMultiColor(p0, p1, COL_ACCENT_SOFT, COL_ACCENT_NONE, COL_ACCENT_NONE,
                                      COL_ACCENT_SOFT);
    }
    if (hot || current) {
        float inset = height * 0.22f;
        draw->AddRectFilled(ImVec2(p0.x, p0.y + inset), ImVec2(p0.x + 4 * scale, p1.y - inset),
                            hot ? COL_ACCENT : COL_TEXT_FAINT);
    }
    ImU32 colour = !enabled ? COL_TEXT_DISABLED : hot || current ? COL_TEXT : COL_TEXT_DIM;
    ImVec2 text = font->CalcTextSizeA(size, FLT_MAX, 0, label);
    shadowed_text(draw, font, size, ImVec2(p0.x + (hot ? 24 : 18) * scale, (p0.y + p1.y - text.y) / 2),
                  colour, label);
    return chosen;
}

/* The button hints at the bottom right: [key] action ... */
static void hints(ImDrawList *draw, float right, float y, const char *const *pairs, int count)
{
    float scale = ui_scale();
    ImFont *font = ui_font_bold(), *plain = ImGui::GetFont();
    float size = SMALL_SIZE * scale, pad = 7 * scale, gap = 22 * scale;
    float x = right;
    for (int i = count - 1; i >= 0; i--) {
        const char *key = pairs[i * 2], *action = pairs[i * 2 + 1];
        ImVec2 a = plain->CalcTextSizeA(size, FLT_MAX, 0, action);
        ImVec2 k = font->CalcTextSizeA(size, FLT_MAX, 0, key);
        x -= a.x;
        shadowed_text(draw, plain, size, ImVec2(x, y), COL_TEXT_DIM, action);
        x -= 8 * scale + k.x + 2 * pad;
        draw->AddRect(ImVec2(x, y - 3 * scale), ImVec2(x + k.x + 2 * pad, y + k.y + 3 * scale), COL_KEY,
                      5 * scale, 0, 1.2f * scale);
        draw->AddText(font, size, ImVec2(x + pad, y), COL_TEXT, key);
        x -= gap;
    }
}

/* Minimise and close, top right (the splash has no title bar).  Returns
 * TRUE for close. */
static bool window_buttons(float w)
{
    float scale = ui_scale();
    float size = WINDOW_BUTTON * scale;
    bool close = false;
    const char *labels[2] = { "\xE2\x80\x93", "\xC3\x97" };     /* en dash, multiplication sign */
    for (int i = 0; i < 2; i++) {
        ImVec2 p0(w - size * (2 - i), 0);
        ImGui::SetCursorScreenPos(p0);
        ImGui::PushID(i);
        bool clicked = ImGui::InvisibleButton("window", ImVec2(size, size * 0.8f));
        ImGui::PopID();
        bool hover = ImGui::IsItemHovered();
        ImDrawList *draw = ImGui::GetWindowDrawList();
        if (hover) {
            draw->AddRectFilled(p0, ImVec2(p0.x + size, size * 0.8f),
                                i == 1 ? IM_COL32(196, 43, 28, 230) : IM_COL32(255, 255, 255, 40));
        }
        float text_size = 20 * scale;
        ImVec2 t = ImGui::GetFont()->CalcTextSizeA(text_size, FLT_MAX, 0, labels[i]);
        draw->AddText(ImGui::GetFont(), text_size,
                      ImVec2(p0.x + (size - t.x) / 2, (size * 0.8f - t.y) / 2),
                      hover ? COL_TEXT : COL_TEXT_DIM, labels[i]);
        if (clicked) {
            if (i == 0) {
                platform_minimize();
            } else {
                close = true;
            }
        }
    }
    return close;
}

/*
 * The background: the dark of the castle with the light of a candle from
 * the right, a dim vignette, and a large faint "D".
 */
#define BG_DARK                 IM_COL32(6, 4, 5, 255)
#define BG_GLOW                 IM_COL32(74, 20, 16, 255)
#define BG_GLOW_NONE            IM_COL32(74, 20, 16, 0)
#define BG_VIGNETTE             IM_COL32(0, 0, 0, 190)
#define BG_VIGNETTE_NONE        IM_COL32(0, 0, 0, 0)
#define BG_LETTER               IM_COL32(150, 36, 30, 34)

static void draw_background(float w, float h, bool settings)
{
    ImDrawList *draw = ImGui::GetBackgroundDrawList();

    draw->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), BG_DARK);
    /* the light: brightest on the right, a little higher than the middle */
    draw->AddRectFilledMultiColor(ImVec2(w * 0.35f, 0), ImVec2(w, h * 0.55f), BG_GLOW_NONE,
                                  BG_GLOW, BG_GLOW, BG_GLOW_NONE);
    draw->AddRectFilledMultiColor(ImVec2(w * 0.35f, h * 0.55f), ImVec2(w, h), BG_GLOW_NONE,
                                  BG_GLOW, BG_GLOW_NONE, BG_GLOW_NONE);
    if (!settings) {
        float size = h * 1.25f;
        ImVec2 extent = ui_font_display()->CalcTextSizeA(size, FLT_MAX, 0, "D");
        draw->AddText(ui_font_display(), size, ImVec2(w - extent.x * 0.92f, (h - extent.y) / 2),
                      BG_LETTER, "D");
    }
    /* darker towards the edges */
    draw->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(w, h * 0.25f), BG_VIGNETTE, BG_VIGNETTE,
                                  BG_VIGNETTE_NONE, BG_VIGNETTE_NONE);
    draw->AddRectFilledMultiColor(ImVec2(0, h * 0.7f), ImVec2(w, h), BG_VIGNETTE_NONE,
                                  BG_VIGNETTE_NONE, BG_VIGNETTE, BG_VIGNETTE);
    if (settings) {
        draw->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), IM_COL32(4, 2, 2, 150));
    }
}

static void title(float x, float y)
{
    float scale = ui_scale();
    ImDrawList *draw = ImGui::GetWindowDrawList();
    float size = TITLE_SIZE * scale * 1.6f;

    shadowed_text(draw, ui_font_bold(), TAG_SIZE * scale, ImVec2(x + 2 * scale, y), COL_ACCENT,
                  "PC PORT");
    y += TAG_SIZE * scale * 1.4f;
    shadowed_text(draw, ui_font_display(), size, ImVec2(x - 2 * scale, y), COL_TEXT, "D");
}

/* The three discs: a dot for each one's state, the text and the file;
 * above them the loading indicator while discs are checked. */
static void disc_status(float x, float y)
{
    float scale = ui_scale();
    ImDrawList *draw = ImGui::GetWindowDrawList();
    ImFont *font = ImGui::GetFont();
    float size = SMALL_SIZE * scale;
    char checking[400];
    if (launcher_checking_text(checking, sizeof(checking))) {
        float line = y - size * 1.9f;
        launcher_draw_spinner(draw, ImVec2(x + 5 * scale, line + size * 0.55f), 6 * scale, COL_ACCENT);
        shadowed_text(draw, font, size, ImVec2(x + 16 * scale, line), COL_TEXT, checking);
    }
    for (int d = 0; d < SETTINGS_DISCS; d++) {
        int status = s_disc_status[d];
        ImU32 dot = status == DISC_OK ? COL_OK : status == DISC_OTHER_DUMP ? COL_WARN
                    : status == DISC_NONE ? COL_TEXT_FAINT : COL_BAD;
        char line[400];
        const char *path = g_settings_pc.disc_paths[d];
        const char *name = path;
        for (const char *p = path; *p; p++) {
            if (*p == '\\' || *p == '/') {
                name = p + 1;
            }
        }
        if (status == DISC_NONE && launcher_checking() && path[0]) {
            snprintf(line, sizeof(line), "Disc %d: checking...    %s", d + 1, name);
        } else if (status == DISC_NONE) {
            snprintf(line, sizeof(line), "Disc %d: not chosen%s", d + 1,
                     d == 0 ? " (drop .bin, .cue or .chd files on the window)" : "");
        } else {
            snprintf(line, sizeof(line), "%s    %s", s_disc_detail[d], name);
        }
        float row_y = y + d * size * 1.5f;
        draw->AddCircleFilled(ImVec2(x + 4 * scale, row_y + size * 0.55f), 4 * scale, dot);
        shadowed_text(draw, font, size, ImVec2(x + 16 * scale, row_y),
                      status == DISC_NONE ? COL_TEXT_FAINT : COL_TEXT, line);
    }
}

static void choose_discs_dialog(void)
{
    char paths[PLATFORM_MAX_FILES][PLATFORM_PATH_MAX];
    int count = platform_open_files_dialog(paths, PLATFORM_MAX_FILES);
    if (count) {
        choose_discs(paths, count);
    }
}

static void open_page(int page)
{
    if (s_page == PAGE_SETTINGS && page != PAGE_SETTINGS) {
        settings_save();
    }
    if (page == PAGE_STATES) {
        states_refresh();
    }
    s_page = page;
    s_focus = 1;
}

/* the main menu; returns an enum LauncherAction */
static int menu_page(float w, float h)
{
    float scale = ui_scale();
    float margin = MARGIN * scale;
    int action = LAUNCHER_STAY;

    title(margin, margin * 0.9f);
    ImGui::SetCursorScreenPos(ImVec2(margin - 18 * scale, h * MENU_TOP));
    int checking = launcher_checking();
    int playable = disc_playable() && !checking;
    if (s_check.just_finished) {
        s_check.just_finished = 0;
        s_focus = playable;             /* the discs are ready: PLAY */
    }
    if (s_focus && playable) {
        ImGui::SetKeyboardFocusHere();
    }
    if (menu_item("PLAY", MENU_SIZE * scale, MENU_WIDTH * scale, playable, false)) {
        action = LAUNCHER_PLAY;
    }
    ImGui::SetCursorScreenPos(ImVec2(margin - 18 * scale, ImGui::GetCursorScreenPos().y));
    /* (opened below: the page's focus must outlive this one's) */
    bool open_states = menu_item("LOAD GAME", MENU_SIZE * scale, MENU_WIDTH * scale,
                                 !checking && states_any(), false);
    ImGui::SetCursorScreenPos(ImVec2(margin - 18 * scale, ImGui::GetCursorScreenPos().y));
    if (s_focus && !playable) {
        ImGui::SetKeyboardFocusHere();
    }
    s_focus = 0;
    if (open_states) {
        open_page(PAGE_STATES);
    }
    if (menu_item("CHOOSE DISCS", MENU_SIZE * scale, MENU_WIDTH * scale, !checking, false)) {
        choose_discs_dialog();
    }
    ImGui::SetCursorScreenPos(ImVec2(margin - 18 * scale, ImGui::GetCursorScreenPos().y));
    if (menu_item("SETTINGS", MENU_SIZE * scale, MENU_WIDTH * scale, true, false)) {
        open_page(PAGE_SETTINGS);
    }
    ImGui::SetCursorScreenPos(ImVec2(margin - 18 * scale, ImGui::GetCursorScreenPos().y));
    if (menu_item("QUIT", MENU_SIZE * scale, MENU_WIDTH * scale, true, false)) {
        action = LAUNCHER_QUIT;
    }

    disc_status(margin, h - margin - SMALL_SIZE * scale * 3.6f);
    static const char *const HINTS[] = { "Enter / A", "Select", "Esc / B", "Quit" };
    hints(ImGui::GetWindowDrawList(), w - margin * 0.6f, h - margin * 0.75f, HINTS, 2);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
        action = LAUNCHER_QUIT;
    }
    return action;
}

/* the settings: categories on the left, the rows in a panel */
static void settings_page(float w, float h)
{
    static const char *const TABS[TAB_COUNT] = { "GRAPHICS", "CONTROLS", "SOUND", "GAME" };
    float scale = ui_scale();
    float margin = MARGIN * scale;

    shadowed_text(ImGui::GetWindowDrawList(), ui_font_display(), PAGE_TITLE_SIZE * scale,
                  ImVec2(margin, margin * 0.8f), COL_TEXT, "SETTINGS");
    float y = margin * 0.8f + PAGE_TITLE_SIZE * scale * 1.7f;
    if (nav_key_pressed()) {
        s_mouse_focus = false;
    }
    for (int i = 0; i < TAB_COUNT; i++) {
        ImGui::SetCursorScreenPos(ImVec2(margin - 18 * scale, y));
        if (s_focus && i == s_tab) {
            ImGui::SetKeyboardFocusHere();
        }
        if (menu_item(TABS[i], TAB_SIZE * scale, TAB_WIDTH * scale, true, i == s_tab)) {
            s_tab = i;
        }
        if (ImGui::IsItemFocused() && !s_mouse_focus) {
            s_tab = i;                  /* the keys show each page in turn; the mouse clicks */
        }
        y = ImGui::GetCursorScreenPos().y;
    }
    s_focus = 0;
    ImGui::SetCursorScreenPos(ImVec2(margin - 18 * scale, y + 16 * scale));
    if (menu_item("BACK", TAB_SIZE * scale, TAB_WIDTH * scale, true, false)) {
        open_page(PAGE_MENU);
    }
    /* (out of the way: at the bottom, level with the panel's) */
    float reset_size = SMALL_SIZE * 1.1f * scale;
    ImGui::SetCursorScreenPos(ImVec2(margin - 18 * scale, h - margin * 1.45f - reset_size * 1.55f));
    if (menu_item("Reset all", reset_size, TAB_WIDTH * scale, true, false)) {
        reset_all();
    }

    /* the panel */
    float left = PANEL_LEFT * scale, top = margin * 0.8f;
    ImGui::SetCursorScreenPos(ImVec2(left, top));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(COL_PANEL));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10 * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22 * scale, 16 * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10 * scale, 8 * scale));
    ImGui::BeginChild("panel", ImVec2(w - left - margin * 0.6f, h - top - margin * 1.45f),
                      ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened);
    switch (s_tab) {
    case TAB_GRAPHICS: graphics_tab(); break;
    case TAB_CONTROLS: controls_tab(); break;
    case TAB_SOUND: sound_tab(); break;
    default: gameplay_tab(); break;
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();

    static const char *const HINTS[] = { "Enter / A", "Select", "Esc / B", "Back" };
    hints(ImGui::GetWindowDrawList(), w - margin * 0.6f, h - margin * 0.75f, HINTS, 2);
    if (!settings_capturing()
        && (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))) {
        open_page(PAGE_MENU);
    }
}

/* "Delete the state in slot N?" over the page; TRUE while it is shown */
static bool delete_question(void)
{
    if (s_delete_slot < 0) {
        return false;
    }
    float scale = ui_scale();
    if (!ImGui::IsPopupOpen("delete")) {
        ImGui::OpenPopup("delete");
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22 * scale, 18 * scale));
    if (ImGui::BeginPopupModal("delete", NULL, ImGuiWindowFlags_NoTitleBar
                                                   | ImGuiWindowFlags_AlwaysAutoResize
                                                   | ImGuiWindowFlags_NoSavedSettings)) {
        char text[200];
        snprintf(text, sizeof(text), "Delete the state in slot %d?", s_delete_slot + 1);
        ImGui::PushFont(ui_font_bold(), ImGui::GetFontSize());
        ImGui::TextUnformatted(text);
        ImGui::PopFont();
        char detail[120];
        states_describe(s_delete_slot, detail, sizeof(detail));
        if (detail[0]) {
            ImGui::TextUnformatted(detail);
        }
        ImGui::TextDisabled("%s", "It cannot be undone.");
        ImGui::Dummy(ImVec2(0, 6 * scale));
        ImVec2 button(140 * scale, 0);
        bool remove = ImGui::Button("Delete", button);
        ImGui::SameLine();
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();      /* (the safe choice is the selected one) */
        }
        bool cancel = ImGui::Button("Cancel", button)
                      || ImGui::IsKeyPressed(ImGuiKey_Escape, false)
                      || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);
        if (remove) {
            if (!psx_state_delete(s_delete_slot)) {
                snprintf(s_states_note, sizeof(s_states_note), "Slot %d could not be deleted.",
                         s_delete_slot + 1);
            }
            states_refresh();
        }
        if (remove || cancel) {
            s_delete_slot = -1;
            s_focus = 1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
    return true;                        /* (also the frame it closes: its key is used up) */
}

/* the save states: one is chosen to play from, or deleted */
static int states_page(float w, float h)
{
    float scale = ui_scale();
    float margin = MARGIN * scale;
    int action = LAUNCHER_STAY;

    /* (as large as on the other pages, if it fits left of the panel) */
    float size = PAGE_TITLE_SIZE * scale;
    float room = PANEL_LEFT * scale - margin - 16 * scale;
    float width = ui_font_display()->CalcTextSizeA(size, FLT_MAX, 0, "LOAD GAME").x;
    size = width > room ? size * room / width : size;
    shadowed_text(ImGui::GetWindowDrawList(), ui_font_display(), size,
                  ImVec2(margin, margin * 0.8f), COL_TEXT, "LOAD GAME");
    float y = margin * 0.8f + PAGE_TITLE_SIZE * scale * 1.7f;
    ImGui::SetCursorScreenPos(ImVec2(margin - 18 * scale, y));
    bool back = menu_item("BACK", TAB_SIZE * scale, TAB_WIDTH * scale, true, false);
    if (s_states_note[0]) {
        ImGui::SetCursorScreenPos(ImVec2(margin, y + TAB_SIZE * scale * 2.4f));
        ImGui::PushTextWrapPos(PANEL_LEFT * scale - 16 * scale);
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(COL_WARN), "%s", s_states_note);
        ImGui::PopTextWrapPos();
    }

    float left = PANEL_LEFT * scale, top = margin * 0.8f;
    ImGui::SetCursorScreenPos(ImVec2(left, top));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(COL_PANEL));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10 * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * scale, 12 * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10 * scale, 4 * scale));
    ImGui::BeginChild("states", ImVec2(w - left - margin * 0.6f, h - top - margin * 1.45f),
                      ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_NavFlattened);
    int remove = -1;
    int chosen = states_list(STATES_BROWSING, ImGui::GetContentRegionAvail().x,
                             STATE_ROW * scale, s_focus, &remove);
    ImGui::EndChild();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
    s_focus = 0;

    bool asking = delete_question();
    if (remove >= 0 && !asking) {
        s_delete_slot = remove;
        s_states_note[0] = 0;
    }
    static const char *const HINTS[] = { "Enter / A", "Load", "Del / X", "Delete", "Esc / B", "Back" };
    hints(ImGui::GetWindowDrawList(), w - margin * 0.6f, h - margin * 0.75f, HINTS, 3);
    if (asking || s_delete_slot >= 0) {
        return action;
    }
    if (chosen >= 0) {
        static PsxStateInfo info;       /* (large: its picture) */
        psx_state_info(chosen, &info);
        if (!info.usable) {
            snprintf(s_states_note, sizeof(s_states_note), "%s",
                     info.problem ? info.problem : "That state cannot be loaded.");
        } else if (!disc_playable()) {
            snprintf(s_states_note, sizeof(s_states_note), "%s", "Choose the discs first (CHOOSE DISCS).");
        } else {
            s_state_slot = chosen;
            action = LAUNCHER_LOAD_STATE;
            back = true;                /* (the menu again after the game) */
        }
    }
    if (back || ImGui::IsKeyPressed(ImGuiKey_Escape, false)
        || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
        s_states_note[0] = 0;
        open_page(PAGE_MENU);
    }
    return action;
}

int launcher_state_slot(void)
{
    return s_state_slot;
}

/* The discs of the settings, checked again in the background (the
 * checksum only if a file has changed since it was verified). */
void launcher_init(void)
{
    char paths[SETTINGS_DISCS][SETTINGS_PATH_MAX];
    int count = 0;
    for (int d = 0; d < SETTINGS_DISCS; d++) {
        s_disc_status[d] = DISC_NONE;
        if (g_settings_pc.disc_paths[d][0]) {
            snprintf(paths[count++], SETTINGS_PATH_MAX, "%s", g_settings_pc.disc_paths[d]);
        }
    }
    start_check(paths, count, 0);
    states_refresh();
}

/* The image of disc `number` (1..3) if it can be used, else NULL. */
const char *launcher_disc_path(int number)
{
    return number >= 1 && number <= SETTINGS_DISCS && disc_usable(number - 1)
               ? g_settings_pc.disc_paths[number - 1] : NULL;
}


/* the layout's size at 96 DPI; the window scales it (keeping proportions) */
#define LAYOUT_WIDTH            1000.0f
#define LAYOUT_HEIGHT           600.0f
#define MIN_LAYOUT_SCALE        0.6f
#define MAX_LAYOUT_SCALE        4.0f

int launcher_frame(void)
{
    int action = LAUNCHER_STAY;
    int iw, ih;

    launcher_poll();
    char dropped_paths[PLATFORM_MAX_FILES][PLATFORM_PATH_MAX];
    int dropped = launcher_checking() ? 0 : platform_take_dropped_files(dropped_paths, PLATFORM_MAX_FILES);
    if (dropped) {
        choose_discs(dropped_paths, dropped);
    }
    platform_client_size(&iw, &ih);
    float w = (float) iw, h = (float) ih;

    /* everything scales with the window: our own sizes by s_scale, Dear
     * ImGui's spacing by a scaled copy of the style and its text by a
     * pushed font size (both put back at the end) */
    float dpi = platform_dpi_scale();
    float k = w / (LAYOUT_WIDTH * dpi) < h / (LAYOUT_HEIGHT * dpi) ? w / (LAYOUT_WIDTH * dpi)
                                                                   : h / (LAYOUT_HEIGHT * dpi);
    k = k < MIN_LAYOUT_SCALE ? MIN_LAYOUT_SCALE : k > MAX_LAYOUT_SCALE ? MAX_LAYOUT_SCALE : k;
    s_scale = dpi * k;
    ImGuiStyle &style = ImGui::GetStyle();
    ImGuiStyle saved = style;
    style.ScaleAllSizes(k);
    s_in_launcher = 1;

    draw_background(w, h, s_page != PAGE_MENU);

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(w, h));
    if (s_focus) {
        ImGui::SetNextWindowFocus();
        ImGui::SetNavCursorVisible(true);   /* Enter / A acts on the selection at once */
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 2 * ui_scale()));
    ImGui::Begin("launcher", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove
                                       | ImGuiWindowFlags_NoSavedSettings
                                       | ImGuiWindowFlags_NoBackground
                                       | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PushFont(NULL, saved.FontSizeBase * k);
    if (s_page == PAGE_MENU) {
        action = menu_page(w, h);
    } else if (s_page == PAGE_STATES) {
        action = states_page(w, h);
    } else {
        settings_page(w, h);
    }
    if (window_buttons(w)) {
        action = LAUNCHER_QUIT;
    }
    /* the splash has no title bar: drag it by any free spot */
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered()
        && ImGui::IsWindowHovered() && !ImGui::IsAnyItemActive()) {
        platform_begin_window_drag();
        ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    }
    ImGui::PopFont();
    ImGui::End();
    ImGui::PopStyleVar(2);
    style = saved;
    s_in_launcher = 0;
    return action;
}
