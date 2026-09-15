#define IMGUI_DEFINE_MATH_OPERATORS
#include "ps2_ui.h"
#include "ps2_settings.h"

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_vulkan.h"

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void ps2_log(const char *fmt, ...);

namespace {

bool g_init, g_visible;
SDL_Window *g_window;
float g_max_aniso;
float g_dpi = 1.0f;
VkFormat g_color_format;

enum { CAP_NONE, CAP_KEY, CAP_PAD };
int g_cap = CAP_NONE, g_cap_act, g_cap_slot;
bool g_cap_armed;
Uint64 g_cap_start;
Sint16 g_cap_base[SDL_GAMEPAD_AXIS_COUNT];

enum { TAB_NONE = -1, TAB_KEYBOARD = 4, TAB_CONTROLLER = 5, TAB_ANALOG = 6 };
int g_want_tab = TAB_NONE;

const ImVec4 ACCENT(0.40f, 0.86f, 0.50f, 1.0f);
const ImVec4 WARN(1.00f, 0.76f, 0.32f, 1.0f);

void check_vk(VkResult r) {
    if (r < 0) ps2_log("ui: Vulkan call failed (%d)", (int)r);
}

void changed(int dirty) { ps2_settings_touch(dirty | PS2_CFG_SAVE); }

void gfx_changed(int dirty) {
    ps2_cfg.preset = ps2_settings_preset_matches(&ps2_cfg);
    changed(dirty);
}

void help(const char *text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void env_note(const char *var, const char *what) {
    const char *v = getenv(var);
    if (v && *v) ImGui::TextColored(WARN, "%s=%s is set and %s.", var, v, what);
}

bool slider_pct(const char *label, float *v, float lo, float hi) {
    float pct = *v * 100.0f;
    if (ImGui::SliderFloat(label, &pct, lo * 100.0f, hi * 100.0f, "%.1f%%")) {
        *v = pct / 100.0f;
        return true;
    }
    return false;
}

void apply_style() {
    ImGui::StyleColorsDark();
    ImGuiStyle &s = ImGui::GetStyle();
    s.WindowRounding = 8.0f;
    s.ChildRounding = 6.0f;
    s.FrameRounding = 5.0f;
    s.PopupRounding = 6.0f;
    s.GrabRounding = 5.0f;
    s.TabRounding = 5.0f;
    s.ScrollbarRounding = 6.0f;
    s.WindowPadding = ImVec2(16.0f, 14.0f);
    s.FramePadding = ImVec2(9.0f, 5.0f);
    s.ItemSpacing = ImVec2(10.0f, 8.0f);
    s.WindowBorderSize = 1.0f;
    s.SeparatorTextBorderSize = 2.0f;
    ImVec4 *c = s.Colors;
    c[ImGuiCol_WindowBg]         = ImVec4(0.07f, 0.09f, 0.10f, 0.96f);
    c[ImGuiCol_ChildBg]          = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_PopupBg]          = ImVec4(0.08f, 0.10f, 0.11f, 0.98f);
    c[ImGuiCol_Border]           = ImVec4(0.30f, 0.55f, 0.36f, 0.45f);
    c[ImGuiCol_FrameBg]          = ImVec4(0.13f, 0.17f, 0.16f, 1.00f);
    c[ImGuiCol_FrameBgHovered]   = ImVec4(0.19f, 0.27f, 0.22f, 1.00f);
    c[ImGuiCol_FrameBgActive]    = ImVec4(0.22f, 0.34f, 0.26f, 1.00f);
    c[ImGuiCol_TitleBg]          = ImVec4(0.07f, 0.12f, 0.09f, 1.00f);
    c[ImGuiCol_TitleBgActive]    = ImVec4(0.10f, 0.22f, 0.14f, 1.00f);
    c[ImGuiCol_Button]           = ImVec4(0.16f, 0.26f, 0.19f, 1.00f);
    c[ImGuiCol_ButtonHovered]    = ImVec4(0.24f, 0.42f, 0.29f, 1.00f);
    c[ImGuiCol_ButtonActive]     = ImVec4(0.30f, 0.55f, 0.36f, 1.00f);
    c[ImGuiCol_Header]           = ImVec4(0.16f, 0.30f, 0.20f, 1.00f);
    c[ImGuiCol_HeaderHovered]    = ImVec4(0.22f, 0.40f, 0.27f, 1.00f);
    c[ImGuiCol_HeaderActive]     = ImVec4(0.28f, 0.50f, 0.33f, 1.00f);
    c[ImGuiCol_Tab]              = ImVec4(0.11f, 0.18f, 0.13f, 1.00f);
    c[ImGuiCol_TabHovered]       = ImVec4(0.24f, 0.44f, 0.30f, 1.00f);
    c[ImGuiCol_TabSelected]      = ImVec4(0.19f, 0.36f, 0.24f, 1.00f);
    c[ImGuiCol_CheckMark]        = ACCENT;
    c[ImGuiCol_SliderGrab]       = ImVec4(0.36f, 0.70f, 0.44f, 1.00f);
    c[ImGuiCol_SliderGrabActive] = ACCENT;
    c[ImGuiCol_SeparatorHovered] = ACCENT;
    c[ImGuiCol_TextSelectedBg]   = ImVec4(0.30f, 0.55f, 0.36f, 0.45f);
    c[ImGuiCol_NavCursor]        = ACCENT;
    c[ImGuiCol_PlotHistogram]    = ImVec4(0.36f, 0.70f, 0.44f, 1.00f);
}

void bind_key(int act, int slot, int sc) {
    if (sc > 0)
        for (int a = 0; a < PS2_ACT_COUNT; a++)
            for (int k = 0; k < PS2_BIND_SLOTS; k++)
                if (ps2_cfg.key[a][k] == sc) ps2_cfg.key[a][k] = 0;
    ps2_cfg.key[act][slot] = sc;
    changed(0);
}

void bind_pad(int act, int slot, int code) {
    if (code > 0)
        for (int a = 0; a < PS2_ACT_COUNT; a++)
            for (int k = 0; k < PS2_BIND_SLOTS; k++)
                if (ps2_cfg.pad[a][k] == code) ps2_cfg.pad[a][k] = 0;
    ps2_cfg.pad[act][slot] = code;
    changed(0);
}

void start_capture(int kind, int act, int slot) {
    g_cap = kind;
    g_cap_act = act;
    g_cap_slot = slot;
    g_cap_armed = false;
    g_cap_start = SDL_GetTicks();
}

void end_capture() { g_cap = CAP_NONE; }

void update_capture_arming() {
    if (g_cap == CAP_NONE || g_cap_armed) return;
    if (SDL_GetTicks() - g_cap_start < 150) return;
    int nk = 0;
    const bool *ks = SDL_GetKeyboardState(&nk);
    for (int i = 0; ks && i < nk; i++)
        if (ks[i]) return;
    SDL_Gamepad *g = ps2_video_gamepad();
    memset(g_cap_base, 0, sizeof g_cap_base);
    if (g) {
        for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++)
            if (SDL_GetGamepadButton(g, (SDL_GamepadButton)b)) return;
        for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; a++)
            g_cap_base[a] = SDL_GetGamepadAxis(g, (SDL_GamepadAxis)a);
    }
    g_cap_armed = true;
}

bool capture_event(const SDL_Event *e) {
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN: {
        if (e->key.repeat || !g_cap_armed) return true;
        SDL_Scancode sc = e->key.scancode;
        if (sc == SDL_SCANCODE_ESCAPE) { end_capture(); return true; }
        bool clear = sc == SDL_SCANCODE_BACKSPACE || sc == SDL_SCANCODE_DELETE;
        if (g_cap == CAP_KEY) bind_key(g_cap_act, g_cap_slot, clear ? 0 : (int)sc);
        else if (clear) bind_pad(g_cap_act, g_cap_slot, 0);
        else return true;
        end_capture();
        return true;
    }
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        if (g_cap == CAP_PAD && g_cap_armed) {
            bind_pad(g_cap_act, g_cap_slot, PS2_PADBIND_BUTTON(e->gbutton.button));
            end_capture();
        }
        return true;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        if (g_cap == CAP_PAD && g_cap_armed && e->gaxis.axis < SDL_GAMEPAD_AXIS_COUNT) {
            int axis = e->gaxis.axis;
            int d = (int)e->gaxis.value - (int)g_cap_base[axis];
            bool trig = axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER
                     || axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
            if (d > 16000 || d < -16000) {
                bind_pad(g_cap_act, g_cap_slot,
                         PS2_PADBIND_AXISDIR(axis, trig ? 1 : d > 0));
                end_capture();
            }
        }
        return true;
    default:
        return false;
    }
}

struct Group { const char *title; int acts[4]; };
const Group GROUPS[] = {
    { "Face buttons", { PS2_ACT_CROSS, PS2_ACT_CIRCLE, PS2_ACT_SQUARE, PS2_ACT_TRIANGLE } },
    { "D-pad",        { PS2_ACT_UP, PS2_ACT_DOWN, PS2_ACT_LEFT, PS2_ACT_RIGHT } },
    { "Shoulders",    { PS2_ACT_L1, PS2_ACT_R1, PS2_ACT_L2, PS2_ACT_R2 } },
    { "Start, Select and stick clicks", { PS2_ACT_START, PS2_ACT_SELECT, PS2_ACT_L3, PS2_ACT_R3 } },
    { "Left stick",   { PS2_ACT_LS_UP, PS2_ACT_LS_DOWN, PS2_ACT_LS_LEFT, PS2_ACT_LS_RIGHT } },
    { "Right stick",  { PS2_ACT_RS_UP, PS2_ACT_RS_DOWN, PS2_ACT_RS_LEFT, PS2_ACT_RS_RIGHT } },
};

void binding_table(bool pad) {
    const ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH
                             | ImGuiTableFlags_SizingStretchProp;
    for (const Group &grp : GROUPS) {
        ImGui::SeparatorText(grp.title);
        ImGui::PushID(grp.title);
        if (ImGui::BeginTable("binds", 3, tf)) {
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 1.1f);
            ImGui::TableSetupColumn("Primary", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Secondary", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            for (int act : grp.acts) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(ps2_action_name(act));
                for (int slot = 0; slot < PS2_BIND_SLOTS; slot++) {
                    ImGui::TableSetColumnIndex(1 + slot);
                    ImGui::PushID(act * 4 + slot);
                    int code = pad ? ps2_cfg.pad[act][slot] : ps2_cfg.key[act][slot];
                    bool capturing = g_cap == (pad ? CAP_PAD : CAP_KEY)
                                  && g_cap_act == act && g_cap_slot == slot;
                    const char *label = capturing ? "..."
                                      : pad ? ps2_padbind_name(code) : ps2_key_name(code);
                    bool dim = !code && !capturing;
                    if (dim) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                    if (ImGui::Button(label, ImVec2(-FLT_MIN, 0.0f)))
                        start_capture(pad ? CAP_PAD : CAP_KEY, act, slot);
                    if (dim) ImGui::PopStyleColor();
                    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                        if (pad) bind_pad(act, slot, 0);
                        else bind_key(act, slot, 0);
                    }
                    ImGui::SetItemTooltip("Click to rebind, right-click to clear.");
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }
        ImGui::PopID();
    }
}

void draw_capture_popup() {
    const char *id = "Rebind###capture";
    bool open = ImGui::IsPopupOpen(id);
    if (g_cap == CAP_NONE && !open) return;
    if (g_cap != CAP_NONE && !open) ImGui::OpenPopup(id);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_AlwaysAutoResize
                                | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings))
        return;
    if (g_cap == CAP_NONE) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ImGui::Text("%s  (%s)", ps2_action_name(g_cap_act), g_cap_slot ? "secondary" : "primary");
    ImGui::Spacing();
    ImGui::TextColored(ACCENT, "%s", g_cap == CAP_KEY
                       ? "Press a key..."
                       : "Press a button, or push a stick or trigger...");
    if (!g_cap_armed) ImGui::TextDisabled("Release all keys and buttons first.");
    ImGui::TextDisabled("Esc cancels, Backspace clears.");
    ImGui::Spacing();
    if (ImGui::Button("Clear binding")) {
        if (g_cap == CAP_KEY) bind_key(g_cap_act, g_cap_slot, 0);
        else bind_pad(g_cap_act, g_cap_slot, 0);
        end_capture();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) end_capture();
    if (g_cap == CAP_NONE) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

const char *const WINDOW_MODES[] = { "Windowed", "Borderless fullscreen", "Exclusive fullscreen" };

struct WinSize { int w, h; const char *tag; };
const WinSize WIN_SIZES[] = {
    { 640, 448, "1x, native" }, { 1280, 896, "2x" }, { 1920, 1344, "3x" }, { 2560, 1792, "4x" },
    { 1280, 720, "720p" }, { 1600, 900, "" }, { 1920, 1080, "1080p" },
    { 2560, 1440, "1440p" }, { 3840, 2160, "4K" },
};

void fs_changed() {
    changed(ps2_cfg.window_mode == PS2_WIN_EXCLUSIVE ? PS2_CFG_WINDOW : 0);
}

void resolution_controls() {
    if (ImGui::Combo("Window mode", &ps2_cfg.window_mode, WINDOW_MODES, 3)) {
        if (ps2_cfg.window_mode != PS2_WIN_WINDOWED) ps2_cfg.fullscreen_type = ps2_cfg.window_mode;
        changed(PS2_CFG_WINDOW);
    }
    help("Borderless covers the screen at the desktop resolution and switches instantly. "
         "Exclusive changes the display mode itself. F11 or Alt+Enter toggles fullscreen "
         "using whichever of the two you last picked.");

    if (ps2_cfg.window_mode == PS2_WIN_WINDOWED) {
        char preview[48];
        snprintf(preview, sizeof preview, "%d x %d", ps2_cfg.window_w, ps2_cfg.window_h);
        if (ImGui::BeginCombo("Window size", preview)) {
            for (const WinSize &s : WIN_SIZES) {
                char label[64];
                if (s.tag[0]) snprintf(label, sizeof label, "%d x %d  (%s)", s.w, s.h, s.tag);
                else snprintf(label, sizeof label, "%d x %d", s.w, s.h);
                bool sel = ps2_cfg.window_w == s.w && ps2_cfg.window_h == s.h;
                if (ImGui::Selectable(label, sel)) {
                    ps2_cfg.window_w = s.w;
                    ps2_cfg.window_h = s.h;
                    changed(PS2_CFG_WINDOW);
                }
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        help("The window's size in pixels. Dragging the window edge also works and is remembered.");
    } else if (ps2_cfg.window_mode == PS2_WIN_EXCLUSIVE) {
        SDL_DisplayID d = SDL_GetDisplayForWindow(g_window);
        char preview[64];
        if (ps2_cfg.fs_w <= 0) snprintf(preview, sizeof preview, "Desktop resolution");
        else snprintf(preview, sizeof preview, "%d x %d @ %.0f Hz", ps2_cfg.fs_w, ps2_cfg.fs_h,
                      (double)ps2_cfg.fs_hz);
        if (ImGui::BeginCombo("Fullscreen resolution", preview)) {
            if (ImGui::Selectable("Desktop resolution", ps2_cfg.fs_w <= 0)) {
                ps2_cfg.fs_w = ps2_cfg.fs_h = 0;
                ps2_cfg.fs_hz = 0.0f;
                fs_changed();
            }
            int n = 0;
            SDL_DisplayMode **modes = d ? SDL_GetFullscreenDisplayModes(d, &n) : nullptr;
            for (int i = 0; modes && i < n; i++) {
                const SDL_DisplayMode *m = modes[i];
                if (m->pixel_density != 1.0f) continue;
                char label[64];
                snprintf(label, sizeof label, "%d x %d @ %.0f Hz", m->w, m->h, (double)m->refresh_rate);
                bool sel = ps2_cfg.fs_w == m->w && ps2_cfg.fs_h == m->h
                        && std::fabs(ps2_cfg.fs_hz - m->refresh_rate) < 0.5f;
                ImGui::PushID(i);
                if (ImGui::Selectable(label, sel)) {
                    ps2_cfg.fs_w = m->w;
                    ps2_cfg.fs_h = m->h;
                    ps2_cfg.fs_hz = m->refresh_rate;
                    fs_changed();
                }
                ImGui::PopID();
            }
            SDL_free(modes);
            ImGui::EndCombo();
        }
    } else {
        ImGui::TextDisabled("Borderless fullscreen always runs at the desktop resolution.");
    }
}

const char *const ASPECTS[] = {
    "Stretch to fill the window", "Auto (4:3, or 16:9 with widescreen)", "4:3", "16:9", "Custom",
};

void aspect_controls() {
    if (ImGui::Combo("Aspect ratio", &ps2_cfg.aspect, ASPECTS, 5)) changed(0);
    help("The shape of the picture inside the window. Auto keeps the PS2's 4:3 picture "
         "undistorted and becomes 16:9 when the widescreen patch is on. Stretch fills the "
         "window whatever its shape, which is how the runtime always behaved.");
    env_note("PS2_ASPECT", "overrides the aspect ratio");
    if (ps2_cfg.aspect == PS2_ASPECT_CUSTOM
        && ImGui::SliderFloat("Custom ratio", &ps2_cfg.aspect_custom, 1.0f, 3.0f, "%.3f : 1"))
        changed(0);
}

void widescreen_control() {
    bool ws = ps2_cfg.widescreen != 0;
    if (ImGui::Checkbox("Widescreen (16:9)", &ws)) {
        ps2_cfg.widescreen = ws;
        if (ws && ps2_cfg.aspect == PS2_ASPECT_4_3) ps2_cfg.aspect = PS2_ASPECT_AUTO;
        changed(0);
    }
    help("Applies the 16:9 patch from PCSX2's patch database (by nemesis2000) to the game's "
         "own camera, so the 3D view really is wider instead of stretched. The patch does not "
         "touch the 2D HUD, which will look wider. Use Auto or 16:9 aspect with it.");
    if (ws && ps2_cfg.aspect == PS2_ASPECT_STRETCH && g_window) {
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(g_window, &w, &h);
        if (h > 0 && std::fabs((float)w / (float)h - 16.0f / 9.0f) > 0.05f)
            ImGui::TextColored(WARN, "This window is not 16:9, so Stretch distorts the picture. Pick Auto.");
    }
}

const char *const PRESENT_MODES[] = {
    "Mailbox - low latency, no tearing", "VSync (FIFO)",
    "Immediate - tearing, lowest latency", "Adaptive VSync (FIFO relaxed)",
};

void present_controls() {
    if (ImGui::Combo("Presentation", &ps2_cfg.present_mode, PRESENT_MODES, 4))
        changed(PS2_CFG_SWAPCHAIN);
    help("The game always runs at the PS2's 59.94 fields per second; this only decides how "
         "finished frames reach the screen. A mode your display does not offer falls back "
         "to VSync.");
    env_note("PS2_PRESENT_MODE", "overrides the presentation mode");
    bool lim = ps2_cfg.fps_limit > 0;
    if (ImGui::Checkbox("Limit frame rate", &lim)) {
        ps2_cfg.fps_limit = lim ? 60 : 0;
        changed(PS2_CFG_FPS);
    }
    if (lim) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
        if (ImGui::SliderInt("##fpslimit", &ps2_cfg.fps_limit, 20, 240, "%d FPS"))
            changed(PS2_CFG_FPS);
    }
    help("Shows fewer frames. Below 60, frames are skipped evenly -- every other one at 30 -- "
         "while the game keeps running at the PS2's 59.94 fields a second underneath. It saves "
         "only the final presentation, so it cannot make a slow machine run the game faster; "
         "leave it off unless you want an even, lower frame rate.");
    env_note("PS2_FPS_CAP", "overrides the frame-rate limit");
}

struct PresetInfo { const char *name, *blurb; };
const PresetInfo PRESETS[4] = {
    { "Low",    "The PS2's native 640x448 with plain bilinear scaling to the window -- the "
                "renderer as it was before this menu. Lightest on the GPU; the one for "
                "integrated graphics." },
    { "Medium", "2x internal resolution (1280x896) for clean edges, lines and effects, and "
                "4x anisotropic filtering." },
    { "High",   "3x internal resolution (1920x1344), 8x anisotropic filtering, sharp-bilinear "
                "scaling and light sharpening." },
    { "Ultra",  "4x internal resolution (2560x1792), averaged down to the window for "
                "anti-aliasing, 16x anisotropic filtering and light sharpening. Uses the "
                "most video memory." },
};

void internal_res_status() {
    ps2_render_info ri;
    ps2_video_render_info(&ri);
    ImGui::TextDisabled("Drawing at %u x %u (%ux the PS2's %u x %u), about %.0f MB of video memory.",
                        ri.guest_w * ri.scale, ri.guest_h * ri.scale, ri.scale,
                        ri.guest_w, ri.guest_h,
                        (double)(ri.bytes_at_1x * ri.scale * ri.scale) / (1024.0 * 1024.0));
    if (ri.failed_scale)
        ImGui::TextColored(WARN, "%ux did not fit in video memory, so it stays at %ux.",
                           ri.failed_scale, ri.scale);
}

void internal_res_control() {
    ps2_render_info ri;
    ps2_video_render_info(&ri);
    static const int VALUES[] = { 0, 1, 2, 3, 4, 5, 6, 8 };
    auto label = [&ri](int v, char *buf, size_t n) {
        double mb = (double)(ri.bytes_at_1x * (uint64_t)(v * v)) / (1024.0 * 1024.0);
        if (v <= 0)
            snprintf(buf, n, "Auto - cover the window (%ux now)",
                     ri.auto_scale < ri.max_scale ? ri.auto_scale : ri.max_scale);
        else if (v == 1)
            snprintf(buf, n, "Native - %u x %u", ri.guest_w, ri.guest_h);
        else
            snprintf(buf, n, "%dx - %u x %u  (about %.0f MB)", v, ri.guest_w * v, ri.guest_h * v, mb);
    };
    char preview[96];
    label(ps2_cfg.internal_res, preview, sizeof preview);
    if (ImGui::BeginCombo("Internal resolution", preview)) {
        for (int v : VALUES) {
            char buf[96];
            label(v, buf, sizeof buf);
            bool too_big = v > (int)ri.max_scale;
            if (too_big) ImGui::BeginDisabled();
            if (ImGui::Selectable(buf, ps2_cfg.internal_res == v)) {
                ps2_cfg.internal_res = v;
                gfx_changed(0);
            }
            if (too_big) ImGui::EndDisabled();
        }
        ImGui::EndCombo();
    }
    help("How many pixels the game is drawn with before it is scaled to the window. Native is "
         "the PS2's own 640x448. 2x and up draw the same frame with that many times the pixels "
         "in each direction, so polygon edges, HUD lines and the effects the game renders into "
         "its own buffers come out sharp; the textures themselves keep the game's resolution. "
         "Above the window's size the extra pixels are averaged down, which anti-aliases. Video "
         "memory and GPU time grow with the square of the factor. Auto picks the smallest factor "
         "that covers the window.");
    env_note("PS2_UPSCALE", "overrides the internal resolution");
    internal_res_status();
}

void tab_quick() {
    ImGuiStyle &st = ImGui::GetStyle();
    ImGui::SeparatorText("Graphics quality");
    float w = (ImGui::GetContentRegionAvail().x - st.ItemSpacing.x * 3.0f) / 4.0f;
    float h = ImGui::GetFrameHeight() * 2.2f;
    for (int i = PS2_PRESET_ULTRA; i >= PS2_PRESET_LOW; i--) {
        bool sel = ps2_cfg.preset == i;
        if (sel) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.25f, 0.52f, 0.32f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.60f, 0.38f, 1.0f));
        }
        if (ImGui::Button(PRESETS[i].name, ImVec2(w, h))) {
            ps2_settings_apply_preset(&ps2_cfg, i);
            changed(PS2_CFG_SAMPLER);
        }
        if (sel) ImGui::PopStyleColor(2);
        ImGui::SetItemTooltip("%s", PRESETS[i].blurb);
        if (i > PS2_PRESET_LOW) ImGui::SameLine();
    }
    if (ps2_cfg.preset == PS2_PRESET_CUSTOM)
        ImGui::TextWrapped("Custom: options have been changed individually on the other tabs.");
    else
        ImGui::TextWrapped("%s: %s", PRESETS[ps2_cfg.preset].name, PRESETS[ps2_cfg.preset].blurb);
    internal_res_status();

    ImGui::SeparatorText("Display");
    resolution_controls();
    aspect_controls();
    widescreen_control();
    present_controls();
    bool fps = ps2_cfg.show_fps != 0;
    if (ImGui::Checkbox("Show FPS counter", &fps)) { ps2_cfg.show_fps = fps; changed(0); }

    ImGui::SeparatorText("Controls");
    if (ImGui::Button("Keyboard bindings")) g_want_tab = TAB_KEYBOARD;
    ImGui::SameLine();
    if (ImGui::Button("Controller bindings")) g_want_tab = TAB_CONTROLLER;
    ImGui::SameLine();
    if (ImGui::Button("Dead zones")) g_want_tab = TAB_ANALOG;

    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("Reset all settings to defaults")) ImGui::OpenPopup("Reset all settings?");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Reset all settings?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Graphics, display, bindings and dead zones all return to their defaults.");
        if (ImGui::Button("Reset", ImVec2(ImGui::GetFontSize() * 7.0f, 0.0f))) {
            ps2_settings_defaults(&ps2_cfg);
            changed(PS2_CFG_SWAPCHAIN | PS2_CFG_WINDOW | PS2_CFG_SAMPLER | PS2_CFG_FPS);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(ImGui::GetFontSize() * 7.0f, 0.0f))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void tab_display() {
    ImGui::SeparatorText("Window");
    resolution_controls();

    ImGui::SeparatorText("Picture");
    aspect_controls();
    widescreen_control();
    bool is = ps2_cfg.integer_scale != 0;
    if (ImGui::Checkbox("Integer scaling", &is)) { ps2_cfg.integer_scale = is; changed(0); }
    help("Scale only by whole multiples of the game's 448 lines, leaving a border instead of "
         "resampling unevenly. Combine with Nearest for exact pixels.");
    static const char *const SCALE[] = {
        "Nearest - blocky, exact pixels", "Bilinear - smooth", "Sharp bilinear - crisp, even pixels",
    };
    if (ImGui::Combo("Scaling filter", &ps2_cfg.scale_filter, SCALE, 3)) gfx_changed(0);
    help("How the rendered picture is resized to the window. Sharp bilinear keeps pixels crisp "
         "at any size without the uneven pixel widths of Nearest. A picture drawn larger than "
         "the window is averaged down with any filter except Nearest.");

    ImGui::SeparatorText("Presentation");
    present_controls();

    ImGui::SeparatorText("Interface");
    bool fps = ps2_cfg.show_fps != 0;
    if (ImGui::Checkbox("Show FPS counter", &fps)) { ps2_cfg.show_fps = fps; changed(0); }
    if (ImGui::SliderFloat("Menu text size", &ps2_cfg.ui_scale, 0.75f, 2.0f, "%.2fx")) changed(0);
}

void tab_rendering() {
    ImGui::SeparatorText("Resolution");
    internal_res_control();

    ImGui::SeparatorText("Textures");
    static const char *const TF[] = {
        "As the game requests (accurate)", "Force nearest (pixelated)", "Force bilinear (smooth)",
    };
    if (ImGui::Combo("Texture filtering", &ps2_cfg.tex_filter, TF, 3)) gfx_changed(0);
    help("The PS2 picks nearest or bilinear filtering per draw. Forcing one applies to the 3D "
         "world's textures. The HUD, menus and text keep what the game asked for: they are 2D "
         "art laid out texel by texel, and smoothing them pulls in the neighbouring pictures "
         "(lines across the minimap). So do effects such as the clouds.");

    static const int AF_VALUES[] = { 0, 2, 4, 8, 16 };
    static const char *const AF[] = { "Off", "2x", "4x", "8x", "16x" };
    int idx = 0;
    for (int i = 0; i < 5; i++)
        if (AF_VALUES[i] == ps2_cfg.anisotropy) idx = i;
    bool unsupported = g_max_aniso < 2.0f;
    if (unsupported) ImGui::BeginDisabled();
    if (ImGui::BeginCombo("Anisotropic filtering", AF[idx])) {
        for (int i = 0; i < 5; i++) {
            if (AF_VALUES[i] > 0 && (float)AF_VALUES[i] > g_max_aniso) continue;
            if (ImGui::Selectable(AF[i], i == idx)) {
                ps2_cfg.anisotropy = AF_VALUES[i];
                gfx_changed(PS2_CFG_SAMPLER);
            }
        }
        ImGui::EndCombo();
    }
    if (unsupported) ImGui::EndDisabled();
    help(unsupported ? "This GPU does not support anisotropic filtering."
                     : "Sharpens textures seen at a steep angle -- mostly the ground running "
                       "towards the horizon. Costs a little GPU time.");

    env_note("PS2_NO_MIP", "turns the game's mipmaps off, which garbles distant terrain");

    ImGui::SeparatorText("Frames");
    bool di = ps2_cfg.deinterlace != 0;
    if (ImGui::Checkbox("Deinterlace", &di)) { ps2_cfg.deinterlace = di; gfx_changed(0); }
    help("The game draws alternate lines each field, as an interlaced TV expects. On weaves "
         "the two back into one steady frame; off shows the raw fields, which bob by a line.");
}

void tab_post() {
    ImGui::SeparatorText("Anti-aliasing and sharpness");
    bool fx = ps2_cfg.fxaa != 0;
    if (ImGui::Checkbox("FXAA", &fx)) { ps2_cfg.fxaa = fx; gfx_changed(0); }
    help("Fast approximate anti-aliasing on the final picture: softens jagged edges on "
         "aircraft and the horizon, and can soften fine HUD text a little.");
    if (ImGui::SliderFloat("Sharpening", &ps2_cfg.sharpen, 0.0f, 1.0f, "%.2f")) gfx_changed(0);
    help("Contrast-limited sharpening of the final picture. Counteracts the softness of "
         "scaling 640x448 up to a large window.");

    ImGui::SeparatorText("Colour");
    bool c = false;
    c |= ImGui::SliderFloat("Brightness", &ps2_cfg.brightness, -0.5f, 0.5f, "%+.2f");
    c |= ImGui::SliderFloat("Contrast", &ps2_cfg.contrast, 0.5f, 1.5f, "%.2f");
    c |= ImGui::SliderFloat("Gamma", &ps2_cfg.gamma, 0.5f, 2.0f, "%.2f");
    c |= ImGui::SliderFloat("Saturation", &ps2_cfg.saturation, 0.0f, 2.0f, "%.2f");
    if (c) changed(0);
    if (ImGui::Button("Reset colour")) {
        ps2_cfg.brightness = 0.0f;
        ps2_cfg.contrast = 1.0f;
        ps2_cfg.gamma = 1.0f;
        ps2_cfg.saturation = 1.0f;
        changed(0);
    }
}

void tab_keyboard() {
    ImGui::TextWrapped("Click a binding, then press the key you want. Esc cancels; Backspace or a "
                       "right-click clears it. Each key drives one action, so binding a key "
                       "again moves it here.");
    if (ImGui::Button("Reset keyboard to defaults")) {
        ps2_settings_default_bindings(&ps2_cfg, 1, 0);
        changed(0);
    }
    binding_table(false);
}

void tab_controller() {
    SDL_Gamepad *cur = ps2_video_gamepad();
    int n = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&n);
    const char *preview = cur ? SDL_GetGamepadName(cur) : "No controller connected";
    if (!preview) preview = "Controller";
    if (ImGui::BeginCombo("Controller", preview)) {
        for (int i = 0; ids && i < n; i++) {
            const char *name = SDL_GetGamepadNameForID(ids[i]);
            char label[192];
            snprintf(label, sizeof label, "%s##%u", name ? name : "Controller", (unsigned)ids[i]);
            bool sel = cur && SDL_GetGamepadID(cur) == ids[i];
            if (ImGui::Selectable(label, sel)) ps2_video_select_gamepad(ids[i]);
        }
        ImGui::EndCombo();
    }
    SDL_free(ids);
    bool block = ps2_cfg.block_input_in_menu != 0;
    if (ImGui::Checkbox("Hold the game's controls while this menu is open", &block)) {
        ps2_cfg.block_input_in_menu = block;
        changed(0);
    }
    ImGui::TextWrapped("Click a binding, then press a button, or push a stick or trigger in the "
                       "direction you want. Esc cancels; Backspace or a right-click clears it.");
    if (ImGui::Button("Reset controller to defaults")) {
        ps2_settings_default_bindings(&ps2_cfg, 0, 1);
        changed(0);
    }
    binding_table(true);
}

void stick_view(int s, float size) {
    SDL_Gamepad *g = ps2_video_gamepad();
    float rx = 0.0f, ry = 0.0f, ox, oy;
    if (g) {
        rx = (float)SDL_GetGamepadAxis(g, s ? SDL_GAMEPAD_AXIS_RIGHTX : SDL_GAMEPAD_AXIS_LEFTX) / 32767.0f;
        ry = (float)SDL_GetGamepadAxis(g, s ? SDL_GAMEPAD_AXIS_RIGHTY : SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f;
    }
    const ps2_stick_cfg &c = ps2_cfg.stick[s];
    ps2_stick_process(&c, rx, ry, &ox, &oy);

    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(size, size));
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float r = size * 0.5f - 4.0f;
    ImVec2 ctr = p + ImVec2(size * 0.5f, size * 0.5f);
    dl->AddRectFilled(p, p + ImVec2(size, size), IM_COL32(18, 24, 22, 255), 6.0f);
    const ImU32 dz = IM_COL32(190, 70, 70, 80);
    if (c.shape == PS2_DZ_AXIAL) {
        float d = r * c.inner;
        dl->AddRectFilled(ctr + ImVec2(-d, -r), ctr + ImVec2(d, r), dz);
        dl->AddRectFilled(ctr + ImVec2(-r, -d), ctr + ImVec2(r, d), dz);
    } else {
        dl->AddCircleFilled(ctr, r * c.inner, dz, 48);
    }
    dl->AddLine(ctr - ImVec2(r, 0.0f), ctr + ImVec2(r, 0.0f), IM_COL32(60, 72, 66, 255));
    dl->AddLine(ctr - ImVec2(0.0f, r), ctr + ImVec2(0.0f, r), IM_COL32(60, 72, 66, 255));
    dl->AddCircle(ctr, r, IM_COL32(130, 150, 140, 255), 64, 1.5f);
    if (c.outer > 0.0f) dl->AddCircle(ctr, r * (1.0f - c.outer), IM_COL32(100, 120, 110, 200), 64, 1.0f);
    dl->AddCircleFilled(ctr + ImVec2(rx * r, ry * r), 4.0f, IM_COL32(210, 210, 210, 220));
    dl->AddCircleFilled(ctr + ImVec2(ox * r, oy * r), 5.5f, IM_COL32(100, 235, 130, 255));
}

void tab_analog() {
    ImGui::TextWrapped("How it worked before this menu: each stick axis had its own dead zone -- a "
                       "square around the centre -- of 24%% of travel on the left stick and 26.5%% "
                       "on the right (Microsoft's XInput recommendations), rescaled so movement "
                       "starts smoothly at its edge. Triggers ignored the first 10%% as resting "
                       "noise and counted as held past 24%%. Those are still the defaults.");
    env_note("PS2_PAD_DEADZONE", "overrides both sticks' dead zones");
    if (!ps2_video_gamepad())
        ImGui::TextDisabled("Connect a controller to see the live preview.");
    else
        ImGui::TextDisabled("Preview: grey is the raw stick, green is what the game receives.");

    static const char *const SHAPES[] = {
        "Axial (square, per axis)", "Radial (circle)", "Scaled radial (circle, smoothest)",
    };
    float view = ImGui::GetFontSize() * 11.0f;
    for (int s = 0; s < 2; s++) {
        ImGui::PushID(s);
        ImGui::SeparatorText(s ? "Right stick" : "Left stick");
        if (ImGui::BeginTable("stick", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("controls", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("view", ImGuiTableColumnFlags_WidthFixed, view + 8.0f);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ps2_stick_cfg &c = ps2_cfg.stick[s];
            bool ch = false;
            ch |= ImGui::Combo("Shape", &c.shape, SHAPES, 3);
            help("Axial zeroes each axis on its own, which snaps small diagonals onto the axes. "
                 "Radial treats the stick as the circle it is. Scaled radial also rescales so "
                 "movement starts smoothly at the dead zone's edge -- usually best for flying.");
            ch |= slider_pct("Dead zone", &c.inner, 0.0f, 0.9f);
            help("Travel ignored around the centre. Raise it if the aircraft drifts with the "
                 "stick released; lower it for finer control.");
            ch |= slider_pct("Outer dead zone", &c.outer, 0.0f, 0.5f);
            help("Travel at the rim that already counts as full deflection, for sticks that "
                 "never quite reach their edge.");
            ch |= ImGui::SliderFloat("Response curve", &c.curve, 0.3f, 3.0f, "%.2f");
            help("1.00 is linear. Above 1, small movements become finer while full deflection "
                 "is unchanged -- steadier aiming. Below 1 the stick reacts sharply near centre.");
            bool ix = c.invert_x != 0, iy = c.invert_y != 0;
            if (ImGui::Checkbox("Invert X", &ix)) { c.invert_x = ix; ch = true; }
            ImGui::SameLine();
            if (ImGui::Checkbox("Invert Y", &iy)) { c.invert_y = iy; ch = true; }
            ImGui::SameLine();
            if (ImGui::Button("Reset")) {
                ps2_settings d;
                ps2_settings_defaults(&d);
                c = d.stick[s];
                ch = true;
            }
            if (ch) changed(0);
            ImGui::TableSetColumnIndex(1);
            stick_view(s, view);
            ImGui::EndTable();
        }
        ImGui::PopID();
    }

    ImGui::SeparatorText("Triggers and stick-as-button");
    bool ch = false;
    ch |= slider_pct("Trigger dead zone", &ps2_cfg.trigger_deadzone, 0.0f, 0.5f);
    help("Resting noise a trigger ignores before it counts at all. Measured from where the "
         "trigger rests when the controller connects.");
    ch |= slider_pct("L2 / R2 press point", &ps2_cfg.trigger_press, 0.02f, 1.0f);
    help("How far a trigger travels before the game sees L2 or R2 held.");
    ch |= slider_pct("Stick-as-button press point", &ps2_cfg.axis_press, 0.1f, 0.95f);
    help("When a stick direction is bound to a button on the Controller tab, how far the "
         "stick must be pushed to press it.");
    if (ch) changed(0);
    if (SDL_Gamepad *g = ps2_video_gamepad()) {
        for (int t = 0; t < 2; t++) {
            float v = (float)SDL_GetGamepadAxis(g, t ? SDL_GAMEPAD_AXIS_RIGHT_TRIGGER
                                                     : SDL_GAMEPAD_AXIS_LEFT_TRIGGER) / 32767.0f;
            char overlay[32];
            snprintf(overlay, sizeof overlay, "%s raw %.0f%%", t ? "R2" : "L2", (double)(v * 100.0f));
            ImGui::ProgressBar(v < 0.0f ? 0.0f : v, ImVec2(-FLT_MIN, 0.0f), overlay);
        }
    }
}

void draw_fps() {
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(10.0f * g_dpi, 10.0f * g_dpi), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.45f);
    const ImGuiWindowFlags f = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize
                             | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing
                             | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##fps", nullptr, f))
        ImGui::Text("%.0f FPS  %.1f ms", (double)io.Framerate,
                    io.Framerate > 0.0f ? 1000.0 / (double)io.Framerate : 0.0);
    ImGui::End();
}

void draw_menu() {
    ImGuiIO &io = ImGui::GetIO();
    float s = g_dpi * ps2_cfg.ui_scale;
    ImVec2 size(std::fmin(io.DisplaySize.x * 0.94f, 880.0f * s),
                std::fmin(io.DisplaySize.y * 0.92f, 700.0f * s));
    ImGui::SetNextWindowSize(size, ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(io.DisplaySize * 0.5f, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    bool open = true;
    if (ImGui::Begin("Ace Combat 5 - Settings###ps2settings", &open, ImGuiWindowFlags_NoCollapse)) {
        float footer = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2.0f;
        if (ImGui::BeginTabBar("tabs")) {
            static const char *const TABS[] = {
                "Quick", "Display", "Rendering", "Post-processing",
                "Keyboard", "Controller", "Analog & dead zones",
            };
            static void (*const PAGES[])() = {
                tab_quick, tab_display, tab_rendering, tab_post,
                tab_keyboard, tab_controller, tab_analog,
            };
            for (int i = 0; i < 7; i++) {
                ImGuiTabItemFlags f = g_want_tab == i ? ImGuiTabItemFlags_SetSelected : 0;
                if (ImGui::BeginTabItem(TABS[i], nullptr, f)) {
                    if (ImGui::BeginChild("page", ImVec2(0.0f, -footer))) PAGES[i]();
                    ImGui::EndChild();
                    ImGui::EndTabItem();
                }
            }
            g_want_tab = TAB_NONE;
            ImGui::EndTabBar();
        }
        ImGui::Separator();
        ImGui::TextDisabled("F4 or Esc closes  |  F11 or Alt+Enter toggles fullscreen  |  %s",
                            ps2_settings_path());
    }
    ImGui::End();
    if (!open) ps2_ui_set_visible(0);
}

}

int ps2_ui_init(const ps2_ui_init_info *info) {
    if (g_init) return 0;
    if (!info || !info->window) return -1;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad
                    | ImGuiConfigFlags_NoMouseCursorChange;
    g_window = info->window;
    g_max_aniso = info->max_anisotropy;
    g_dpi = SDL_GetWindowDisplayScale(info->window);
    if (!(g_dpi > 0.0f)) g_dpi = 1.0f;
    apply_style();
    ImGui::GetStyle().ScaleAllSizes(g_dpi);
    ImGui::GetStyle().FontScaleDpi = g_dpi;

    if (!ImGui_ImplSDL3_InitForVulkan(info->window)) {
        ps2_log("ui: the Dear ImGui SDL3 backend did not initialise");
        ImGui::DestroyContext();
        return -1;
    }
    g_color_format = info->color_format;
    ImGui_ImplVulkan_InitInfo vi = {};
    vi.ApiVersion = VK_API_VERSION_1_3;
    vi.Instance = info->instance;
    vi.PhysicalDevice = info->phys;
    vi.Device = info->device;
    vi.QueueFamily = info->queue_family;
    vi.Queue = info->queue;
    vi.DescriptorPoolSize = 32;
    vi.MinImageCount = info->image_count >= 2 ? info->image_count : 2;
    vi.ImageCount = vi.MinImageCount;
    vi.UseDynamicRendering = true;
    vi.PipelineInfoMain.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    vi.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    vi.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &g_color_format;
    vi.CheckVkResultFn = check_vk;
    if (!ImGui_ImplVulkan_Init(&vi)) {
        ps2_log("ui: the Dear ImGui Vulkan backend did not initialise");
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        return -1;
    }
    g_init = true;
    ps2_log("ui: Dear ImGui %s ready -- F4 opens the settings menu", IMGUI_VERSION);
    if (const char *e = getenv("PS2_UI_OPEN")) {
        ps2_ui_set_visible(1);
        g_want_tab = atoi(e);
    }
    return 0;
}

void ps2_ui_shutdown(void) {
    if (!g_init) return;
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    g_init = false;
    g_visible = false;
}

int ps2_ui_visible(void) { return g_init && g_visible; }

void ps2_ui_set_visible(int on) {
    if (!g_init || (on != 0) == g_visible) return;
    g_visible = on != 0;
    ImGuiIO &io = ImGui::GetIO();
    io.ClearEventsQueue();
    io.ClearInputKeys();
    g_cap = CAP_NONE;
    if (g_visible) {
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
        SDL_ShowCursor();
    } else {
        io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    }
}

int ps2_ui_blocks_game_input(void) {
    return g_init && g_visible && ps2_cfg.block_input_in_menu;
}

int ps2_ui_event(const SDL_Event *e) {
    if (!g_init) return 0;
    if (e->type == SDL_EVENT_KEY_DOWN && e->key.key == SDLK_F4 && !e->key.repeat) {
        ps2_ui_set_visible(!g_visible);
        return 1;
    }
    if (g_cap != CAP_NONE && capture_event(e)) return 1;
    if (!g_visible) {
        if (e->type == SDL_EVENT_GAMEPAD_ADDED || e->type == SDL_EVENT_GAMEPAD_REMOVED)
            ImGui_ImplSDL3_ProcessEvent(e);
        return 0;
    }
    if (e->type == SDL_EVENT_KEY_DOWN && e->key.key == SDLK_ESCAPE) {
        ps2_ui_set_visible(0);
        return 1;
    }
    ImGui_ImplSDL3_ProcessEvent(e);
    return 0;
}

void ps2_ui_draw(VkCommandBuffer cmd) {
    if (!g_init || (!g_visible && !ps2_cfg.show_fps)) return;
    ImGuiIO &io = ImGui::GetIO();
    if (g_cap == CAP_PAD) io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
    else io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    ImGui::GetStyle().FontScaleMain = ps2_cfg.ui_scale;
    update_capture_arming();

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    if (ps2_cfg.show_fps) draw_fps();
    if (g_visible) {
        draw_menu();
        draw_capture_popup();
    }
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
}

void ps2_ui_swapchain_changed(uint32_t image_count) {
    if (g_init && image_count >= 2) ImGui_ImplVulkan_SetMinImageCount(image_count);
}
