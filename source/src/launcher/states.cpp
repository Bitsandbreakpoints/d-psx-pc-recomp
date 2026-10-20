/*
 * states.cpp - the list of save states (see states.h).
 */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "imgui.h"
#include "launcher/states.h"
#include "launcher/ui.h"
#include "psx/savestate.h"

#define FRAMES_PER_SECOND       60
#define ROW_PADDING             0.08f   /* of a row's height */
#define DELETE_LABEL            "Delete"

typedef struct {
    PsxStateInfo info;
    UiImage picture;
} Slot;

static Slot s_slots[PSX_STATE_SLOTS];

void states_free(void)
{
    for (int i = 0; i < PSX_STATE_SLOTS; i++) {
        ui_image_free(&s_slots[i].picture);
    }
}

void states_refresh(void)
{
    states_free();
    for (int i = 0; i < PSX_STATE_SLOTS; i++) {
        Slot *slot = &s_slots[i];
        if (psx_state_info(i, &slot->info)) {
            ui_image_from_rgba(slot->info.thumbnail, PSX_STATE_THUMB_WIDTH, PSX_STATE_THUMB_HEIGHT,
                               &slot->picture);
        }
    }
}

int states_any_loadable(void)
{
    for (int i = 0; i < PSX_STATE_SLOTS; i++) {
        if (s_slots[i].info.exists && s_slots[i].info.usable) {
            return 1;
        }
    }
    return 0;
}

int states_any(void)
{
    for (int i = 0; i < PSX_STATE_SLOTS; i++) {
        if (s_slots[i].info.exists) {
            return 1;
        }
    }
    return 0;
}

void states_describe(int slot, char *out, int size)
{
    const PsxStateInfo *info = &s_slots[slot].info;
    if (!info->exists) {
        snprintf(out, size, "%s", "");
        return;
    }
    unsigned seconds = info->play_frames / FRAMES_PER_SECOND;
    snprintf(out, size, "Disc %d, %u:%02u:%02u played", info->disc, seconds / 3600,
             seconds / 60 % 60, seconds % 60);
}

int states_list(int mode, float width, float row_height, int focus, int *remove)
{
    int chosen = -1;
    float pad = row_height * ROW_PADDING;
    float picture_h = row_height - 2 * pad, picture_w = picture_h * 4 / 3;
    ImFont *font = ImGui::GetFont();
    float size = ImGui::GetFontSize();
    ImU32 text = ImGui::GetColorU32(ImGuiCol_Text);
    ImU32 dim = ImGui::GetColorU32(ImGuiCol_TextDisabled);

    for (int i = 0; i < PSX_STATE_SLOTS; i++) {
        const Slot *slot = &s_slots[i];
        bool enabled = mode == STATES_SAVING
                       || (mode == STATES_LOADING ? slot->info.exists && slot->info.usable
                                                  : slot->info.exists != 0);
        bool deletable = mode == STATES_BROWSING && slot->info.exists && remove;
        ImGui::PushID(i);
        if (focus && enabled) {
            ImGui::SetKeyboardFocusHere();
            focus = 0;
        }
        ImGui::BeginDisabled(!enabled);
        if (deletable) {
            ImGui::SetNextItemAllowOverlap();       /* (the Delete button on top) */
        }
        bool clicked = ImGui::Selectable("##slot", false, 0, ImVec2(width, row_height));
        ImGui::EndDisabled();
        if (clicked) {
            chosen = i;
        }
        if (deletable && ImGui::IsItemFocused()
            && (ImGui::IsKeyPressed(ImGuiKey_Delete, false)
                || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false))) {
            *remove = i;
        }
        ImVec2 row0 = ImGui::GetItemRectMin(), row1 = ImGui::GetItemRectMax();
        if (deletable) {
            ImVec2 next = ImGui::GetCursorScreenPos();
            ImVec2 button = ImGui::CalcTextSize(DELETE_LABEL);
            button.x += ImGui::GetStyle().FramePadding.x * 2;
            button.y += ImGui::GetStyle().FramePadding.y * 2;
            ImGui::SetCursorScreenPos(ImVec2(row1.x - button.x - row_height * ROW_PADDING * 2,
                                             (row0.y + row1.y - button.y) / 2));
            if (ImGui::Button(DELETE_LABEL)) {
                *remove = i;
            }
            ImGui::SetCursorScreenPos(next);
        }
        ImGui::PopID();

        ImDrawList *draw = ImGui::GetWindowDrawList();
        ImVec2 p0 = row0;
        ImVec2 pic0(p0.x + pad, p0.y + pad), pic1(pic0.x + picture_w, pic0.y + picture_h);
        if (slot->picture.view) {
            ui_draw_image_cover(draw, &slot->picture, pic0, pic1, ImVec2(0.5f, 0.5f));
        } else {
            draw->AddRectFilled(pic0, pic1, IM_COL32(0, 0, 0, 120));
        }
        draw->AddRect(pic0, pic1, IM_COL32(255, 255, 255, 40));

        char line[160], when[64] = "";
        float x = pic1.x + pad * 2, y = p0.y + row_height / 2 - size * 1.1f;
        snprintf(line, sizeof(line), "Slot %d", i + 1);
        draw->AddText(ui_font_bold(), size * 1.1f, ImVec2(x, y), enabled ? text : dim, line);
        y += size * 1.3f;
        if (!slot->info.exists) {
            snprintf(line, sizeof(line), "%s", "Empty");
        } else if (!slot->info.usable) {
            snprintf(line, sizeof(line), "%s - cannot be loaded",
                     slot->info.problem ? slot->info.problem : "Saved by another program");
        } else {
            time_t t = (time_t) slot->info.saved_at;
            const struct tm *local = localtime(&t);
            if (local) {
                strftime(when, sizeof(when), "    %Y-%m-%d %H:%M", local);
            }
            states_describe(i, line, sizeof(line));
            snprintf(line + strlen(line), sizeof(line) - strlen(line), "%s", when);
        }
        draw->AddText(font, size, ImVec2(x, y), slot->info.exists && slot->info.usable ? text : dim,
                      line);
    }
    return chosen;
}
