/* Goxel 3D voxels editor
 *
 * copyright (c) 2015 Guillaume Chereau <guillaume@noctua-software.com>
 *
 * Goxel is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.

 * Goxel is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.

 * You should have received a copy of the GNU General Public License along with
 * goxel.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef GUI_HAS_SCROLLBARS
#   define GUI_HAS_SCROLLBARS 1
#endif

extern "C" {
#include "goxel.h"
#include "metadata.h"
#include "utils/color.h"

void gui_app(void);
void gui_render_panel(void);
bool gui_pan_scroll_behavior(int dir);
}

#ifndef GUI_ITEM_HEIGHT
#   define GUI_ITEM_HEIGHT 18
#endif

#ifndef GUI_ICON_HEIGHT
#   define GUI_ICON_HEIGHT 32
#endif

#ifndef typeof
#   define typeof __typeof__
#endif

#include <cctype>
#include <cstring>

#define IM_VEC4_CLASS_EXTRA \
        ImVec4(const uint8_t f[4]) { \
            x = f[0] / 255.; \
            y = f[1] / 255.; \
            z = f[2] / 255.; \
            w = f[3] / 255.; }     \
        ImVec4(const float f[4]) { \
            x = f[0]; \
            y = f[1]; \
            z = f[2]; \
            w = f[3]; }     \


// Prevent warnings in vendored imgui headers.
#pragma GCC diagnostic push
#ifdef __clang__
#pragma clang diagnostic ignored "-Wunknown-warning-option"
#pragma clang diagnostic ignored "-Wnontrivial-memcall"
#elif __GNUC__ >= 8
#pragma GCC diagnostic ignored "-Wclass-memaccess"
#endif

#define IMGUI_DEFINE_MATH_OPERATORS
// #define IMGUI_DISABLE_OBSOLETE_FUNCTIONS

#include "../ext_src/imgui/imgui.h"
#include "../ext_src/imgui/imgui_internal.h"
#include "../ext_src/imgui/ImGuizmo.h"

#pragma GCC diagnostic pop

// How much space we keep for the labels on the left
// Use gui_label_size_get to acquire, gui_label_size_push(value) to temp change from the default, and remember to gui_label_size_pop() afterwards
static float g_label_size_stack[16];
static int   g_label_size_top = 0;
static float g_label_size_default = 90.0f;

// Base height of items (note: maybe remove and use the font size instead?).
static const float ITEM_HEIGHT = 18;
static const float ICON_HEIGHT = 32;
static const float CONDENSE_FACTOR = 0.6;
static const ImVec2 ITEM_SPACING = ImVec2(8, 4);

#define COL_HEX(x) ImVec4( \
        ((uint8_t)((x >> 24) & 0xff)) / 255.0, \
        ((uint8_t)((x >> 16) & 0xff)) / 255.0, \
        ((uint8_t)((x >> 8) & 0xff)) / 255.0, \
        ((uint8_t)((x >> 0) & 0xff)) / 255.0)

static inline ImVec4 color_lighten(ImVec4 c, float k = 0.1)
{
    float h, s, v, r, g, b;
    r = c.x;
    g = c.y;
    b = c.z;
    ImGui::ColorConvertRGBtoHSV(r, g, b, h, s, v);
    v += k;
    ImGui::ColorConvertHSVtoRGB(h, s, v, r, g, b);
    return ImVec4(r, g, b, c.w);
}

static inline ImVec4 color_lighten2(ImVec4 v)
{
    return color_lighten(v, 0.2);
}

static texture_t *g_tex_icons = NULL;

static const char *VSHADER =
    "                                                               \n"
    "attribute vec3 a_pos;                                          \n"
    "attribute vec2 a_tex_pos;                                      \n"
    "attribute vec4 a_color;                                        \n"
    "                                                               \n"
    "uniform mat4 u_proj_mat;                                       \n"
    "                                                               \n"
    "varying vec2 v_tex_pos;                                        \n"
    "varying vec4 v_color;                                          \n"
    "                                                               \n"
    "void main()                                                    \n"
    "{                                                              \n"
    "    gl_Position = u_proj_mat * vec4(a_pos, 1.0);               \n"
    "    v_tex_pos = a_tex_pos;                                     \n"
    "    v_color = a_color;                                         \n"
    "}                                                              \n"
;

static const char *FSHADER =
    "                                                               \n"
    "#ifdef GL_ES                                                   \n"
    "precision mediump float;                                       \n"
    "#endif                                                         \n"
    "                                                               \n"
    "uniform sampler2D u_tex;                                       \n"
    "                                                               \n"
    "varying vec2 v_tex_pos;                                        \n"
    "varying vec4 v_color;                                          \n"
    "                                                               \n"
    "void main()                                                    \n"
    "{                                                              \n"
    "    gl_FragColor = v_color * texture2D(u_tex, v_tex_pos);      \n"
    "}                                                              \n"
;

enum {
    A_POS_LOC = 0,
    A_TEX_POS_LOC,
    A_COLOR_LOC,
};

static const char *ATTR_NAMES[] = {
    "a_pos",      /* A_POS_LOC */
    "a_tex_pos",  /* A_TEX_POS_LOC */
    "a_color",    /* A_COLOR_LOC */
    NULL
};

typedef typeof(((inputs_t*)0)->safe_margins) margins_t;

typedef struct gui_t {
    gl_shader_t *shader;
    GLuint  array_buffer;
    GLuint  index_buffer;
    margins_t margins;

    // bitmask: 1 - some window is scrolling.
    //          2 - some window was scrolling last frame.
    int     scrolling;

    int     can_move_window;

    int     win_dir; // Store the current window direction (for scrolling).
    /* Sticky panel header: body child started after gui_panel_header. */
    bool    win_body_started;
    bool    win_autofit_y;
    float   win_max_h;

    int     is_row;
    float   item_size;
    /* Raw mods from inputs_t (set before ImGui::NewFrame). Prefer these over
     * io.KeyShift/KeyCtrl - NewFrame key remapping can drop them. */
    bool    shift_down;
    bool    ctrl_down;

    struct {
        const char *title;
        int       (*func)(void *data);
        void      (*on_closed)(int);
        int         flags;
        void       *data; // Automatically released when popup close.
        bool        opened;
        float       size[2]; // Optional fixed size (0 = auto).
    } popup[8]; // Stack of modal popups
    int popup_count;
} gui_t;

static gui_t *gui = NULL;

static void on_click(void) {
    if (DEFINED(GUI_SOUND))
        sound_play("click", 1.0, 1.0);
}

static bool isCharPressed(int c)
{
    // TODO: remove this function if possible.
    ImGuiContext& g = *GImGui;
    if (g.IO.InputQueueCharacters.Size == 0) return false;
    return g.IO.InputQueueCharacters[0] == c;
}

/*
 * Multi-character shortcut tokens (same spelling as ACTION_REGISTER .shortcut).
 * Return -1 if `token` is not a recognized name.
 */
static int shortcut_named_legacy_key(const char *token)
{
    static const struct {
        const char *name;
        int         key;
    } map[] = {
        {"Delete", KEY_DELETE},
        {"Tab", KEY_TAB},
    };
    if (!token || !token[0]) return -1;
    for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (strcmp(token, map[i].name) == 0) return map[i].key;
    return -1;
}

#define COLOR(g, c, s) ({ \
        uint8_t c_[4]; \
        theme_get_color(THEME_GROUP_##g, THEME_COLOR_##c, (s), c_); \
        ImVec4 ret_ = c_; ret_; })

/*
 * Return the color that should be used to draw an icon depending on the
 * style and the icon.  Some icons shouldn't have their color change with
 * the style and some other do.
 */
static uint32_t get_icon_color(int icon, bool selected)
{
    int group;
    uint8_t color[4];

    group = icon >> 16;
    if (group == 0)
        return ImGui::GetColorU32(COLOR(ICON, TEXT, selected));
    if (group == THEME_GROUP_ICON)
        return 0xFFFFFFFF;
    theme_get_color(group, THEME_COLOR_ITEM, false, color);
    return ImGui::GetColorU32(color);
}

static ImVec2 get_icon_uv(int icon)
{
    icon = icon & 0xffff; // Remove the theme group part.
    return ImVec2(((icon - 1) % 8) / 8.0, ((icon - 1) / 8) / 8.0);
}

static void render_prepare_context(void)
{
    #define OFFSETOF(TYPE, ELEMENT) ((size_t)&(((TYPE *)0)->ELEMENT))
    // Setup render state: alpha-blending enabled, no face culling, no depth testing, scissor enabled
    GL(glEnable(GL_BLEND));
    GL(glBlendEquation(GL_FUNC_ADD));
    GL(glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));

    GL(glDisable(GL_CULL_FACE));
    GL(glDisable(GL_DEPTH_TEST));
    GL(glEnable(GL_SCISSOR_TEST));
    GL(glActiveTexture(GL_TEXTURE0));

    // Setup orthographic projection matrix
    const float width = ImGui::GetIO().DisplaySize.x;
    const float height = ImGui::GetIO().DisplaySize.y;
    const float ortho_projection[4][4] =
    {
        { 2.0f/width,	0.0f,			0.0f,		0.0f },
        { 0.0f,			2.0f/-height,	0.0f,		0.0f },
        { 0.0f,			0.0f,			-1.0f,		0.0f },
        { -1.0f,		1.0f,			0.0f,		1.0f },
    };
    GL(glUseProgram(gui->shader->prog));
    gl_update_uniform(gui->shader, "u_tex", 0);
    gl_update_uniform(gui->shader, "u_proj_mat", ortho_projection);

    GL(glBindBuffer(GL_ARRAY_BUFFER, gui->array_buffer));
    GL(glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gui->index_buffer));
    // This could probably be done only at init time.
    GL(glEnableVertexAttribArray(A_POS_LOC));
    GL(glEnableVertexAttribArray(A_TEX_POS_LOC));
    GL(glEnableVertexAttribArray(A_COLOR_LOC));
    GL(glVertexAttribPointer(A_POS_LOC, 2, GL_FLOAT, false,
                             sizeof(ImDrawVert),
                             (void*)OFFSETOF(ImDrawVert, pos)));
    GL(glVertexAttribPointer(A_TEX_POS_LOC, 2, GL_FLOAT, false,
                             sizeof(ImDrawVert),
                             (void*)OFFSETOF(ImDrawVert, uv)));
    GL(glVertexAttribPointer(A_COLOR_LOC, 4, GL_UNSIGNED_BYTE,
                             true, sizeof(ImDrawVert),
                             (void*)OFFSETOF(ImDrawVert, col)));
    #undef OFFSETOF
}

static void ImImpl_RenderDrawLists(ImDrawData* draw_data)
{
    const float height = ImGui::GetIO().DisplaySize.y;
    const float scale = ImGui::GetIO().DisplayFramebufferScale.y;
    render_prepare_context();
    for (int n = 0; n < draw_data->CmdListsCount; n++)
    {
        const ImDrawList* cmd_list = draw_data->CmdLists[n];

        if (cmd_list->VtxBuffer.size())
            GL(glBufferData(GL_ARRAY_BUFFER,
                    (GLsizeiptr)cmd_list->VtxBuffer.size() * sizeof(ImDrawVert),
                    (GLvoid*)&cmd_list->VtxBuffer.front(), GL_DYNAMIC_DRAW));

        if (cmd_list->IdxBuffer.size())
            GL(glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                    (GLsizeiptr)cmd_list->IdxBuffer.size() * sizeof(ImDrawIdx),
                    (GLvoid*)&cmd_list->IdxBuffer.front(), GL_DYNAMIC_DRAW));

        for (const ImDrawCmd* pcmd = cmd_list->CmdBuffer.begin(); pcmd != cmd_list->CmdBuffer.end(); pcmd++)
        {
            if (pcmd->UserCallback)
            {
                pcmd->UserCallback(cmd_list, pcmd);
                render_prepare_context(); // Restore context.
            }
            else
            {
                GL(glBindTexture(GL_TEXTURE_2D, (GLuint)(intptr_t)pcmd->GetTexID()));
                GL(glScissor((int)pcmd->ClipRect.x * scale,
                             (int)(height - pcmd->ClipRect.w) * scale,
                             (int)(pcmd->ClipRect.z - pcmd->ClipRect.x) * scale,
                             (int)(pcmd->ClipRect.w - pcmd->ClipRect.y) * scale));
                GL(glDrawElements(GL_TRIANGLES, (GLsizei)pcmd->ElemCount,
                                  GL_UNSIGNED_SHORT,
                                  (void*)(uintptr_t)(pcmd->IdxOffset * 2)));
            }
        }
    }
    GL(glDisable(GL_SCISSOR_TEST));
}

static void load_fonts_texture()
{
    ImGuiIO& io = ImGui::GetIO();

    float scale = goxel.screen_scale;
    unsigned char* pixels;
    int width, height;
    const void *data;
    int data_size;
    ImFontConfig conf;

    const ImWchar ranges[] = {
        0x0020, 0x00FF, // Basic Latin + Latin Supplement
        0x25A0, 0x25FF, // Geometric shapes
        0
    };
    conf.FontDataOwnedByAtlas = false;

    data = assets_get("asset://data/fonts/DejaVuSans-light.ttf", &data_size);
    assert(data);
    io.Fonts->AddFontFromMemoryTTF((void*)data, data_size, 14 * scale,
                                   &conf, ranges);
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);

    GLuint tex_id;
    GL(glGenTextures(1, &tex_id));
    GL(glActiveTexture(GL_TEXTURE0));
    GL(glBindTexture(GL_TEXTURE_2D, tex_id));
    GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
    GL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));
    GL(glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                    GL_RGBA, GL_UNSIGNED_BYTE, pixels));
    io.Fonts->TexID = (intptr_t)tex_id;
}

static void init_ImGui(void)
{
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f/60.0f;
    io.IniFilename = NULL;

    io.KeyMap[ImGuiKey_Tab]         = KEY_TAB;
    io.KeyMap[ImGuiKey_LeftArrow]   = KEY_LEFT;
    io.KeyMap[ImGuiKey_RightArrow]  = KEY_RIGHT;
    io.KeyMap[ImGuiKey_UpArrow]     = KEY_UP;
    io.KeyMap[ImGuiKey_DownArrow]   = KEY_DOWN;
    io.KeyMap[ImGuiKey_PageUp]      = KEY_PAGE_UP;
    io.KeyMap[ImGuiKey_PageDown]    = KEY_PAGE_DOWN;
    io.KeyMap[ImGuiKey_Home]        = KEY_HOME;
    io.KeyMap[ImGuiKey_End]         = KEY_END;
    io.KeyMap[ImGuiKey_Delete]      = KEY_DELETE;
    io.KeyMap[ImGuiKey_Backspace]   = KEY_BACKSPACE;
    io.KeyMap[ImGuiKey_Enter]       = KEY_ENTER;
    io.KeyMap[ImGuiKey_Escape]      = KEY_ESCAPE;
    io.KeyMap[ImGuiKey_Space]       = ' ';
    io.KeyMap[ImGuiKey_A]           = 'A';
    io.KeyMap[ImGuiKey_C]           = 'C';
    io.KeyMap[ImGuiKey_V]           = 'V';
    io.KeyMap[ImGuiKey_X]           = 'X';
    io.KeyMap[ImGuiKey_Y]           = 'Y';
    io.KeyMap[ImGuiKey_Z]           = 'Z';

    io.BackendFlags |= ImGuiBackendFlags_HasMouseCursors;

    if (DEFINED(__linux__)) {
        io.SetClipboardTextFn = sys_set_clipboard_text;
        io.GetClipboardTextFn = sys_get_clipboard_text;
    }
}


static void gui_init(void)
{
    if (!gui) {
        gui = (gui_t*)calloc(1, sizeof(*gui));
        init_ImGui();
        goxel.gui.panel_width = GUI_PANEL_WIDTH_NORMAL;
        goxel.gui.layers_panel_width = GUI_PANEL_WIDTH_NORMAL + 60;
        goxel.gui.layers_panel_open = true;
        goxel.gui.view_cube_open = true;
        goxel.gui.camera_presets_open = true;
    }

    if (!gui->shader) {
        gui->shader = gl_shader_create(VSHADER, FSHADER, NULL, ATTR_NAMES);
        GL(glGenBuffers(1, &gui->array_buffer));
        GL(glGenBuffers(1, &gui->index_buffer));
    }

    if (!g_tex_icons) {
        g_tex_icons = texture_new_image("asset://data/images/icons.png",
                                        TF_NEAREST);
        GL(glBindTexture(GL_TEXTURE_2D, g_tex_icons->tex));
    }

    ImGuiIO& io = ImGui::GetIO();
    if (!io.Fonts->TexID) load_fonts_texture();

    g_label_size_stack[0] = g_label_size_default;
    g_label_size_top = 1;
}

void gui_release(void)
{
    if (gui) ImGui::DestroyContext();
}

void gui_release_graphics(void)
{
    ImGuiIO& io = ImGui::GetIO();
    gl_shader_delete(gui->shader);
    gui->shader = NULL;
    GL(glDeleteBuffers(1, &gui->array_buffer));
    GL(glDeleteBuffers(1, &gui->index_buffer));
    texture_delete(g_tex_icons);
    g_tex_icons = NULL;

    GL(glDeleteTextures(1, (GLuint*)&io.Fonts->TexID));
    io.Fonts->TexID = 0;
    io.Fonts->Clear();
}

float gui_get_available_height() {
    return ImGui::GetContentRegionAvail().y;
}

static int alert_popup(void *data)
{
    if (data) gui_text((const char *)data);
    return gui_button("OK", 0, 0);
}

static int check_action_shortcut(action_t *action, void *user)
{
    ImGuiIO& io = ImGui::GetIO();
    const char *s = action->shortcut;
    bool check_key = true;
    bool check_char = true;
    if (!*s) return 0;
    /* Hold/release preview - polled in goxel_iter via goxel_layer_pick_key_update. */
    if (action->idx == ACTION_select_layer_under_cursor)
        return 0;
    if (goxel.image && goxel.image->active_camera &&
        goxel.image->active_camera->mode == CAMERA_MODE_PLAYER &&
        !goxel.player_flycam_hold && !io.KeyCtrl) {
        /*
         * Suppress WASD shortcuts in player movement mode only when the shortcut
         * is a single-letter binding (otherwise "Delete" would be skipped wrongly).
         */
        if (strlen(s) == 1) {
            int c0 = std::tolower((unsigned char)s[0]);
            if (c0 == 'w' || c0 == 'a' || c0 == 's' || c0 == 'd')
                return 0;
        }
    }
    if (io.KeyCtrl) {
        if (!str_startswith(s, "Ctrl")) return 0;
        s += strlen("Ctrl ");
        check_char = false;
    } else {
        if (str_startswith(s, "Ctrl")) return 0;
    }
    if (str_startswith(s, "Shift")) {
        if (!io.KeyShift) return 0;
        s += strlen("Shift ");
        check_char = false;
    } else if (io.KeyShift) {
        /* Shift held but shortcut has no Shift prefix: only match via
         * produced characters (e.g. "#", "{"), not raw key codes. */
        check_key = false;
    }
    if (strlen(s) != 1) {
        int named = shortcut_named_legacy_key(s);
        if (named >= 0 && check_key &&
            ImGui::IsKeyPressed((ImGuiKey)named, false)) {
            action_exec(action);
            return 1;
        }
        return 0;
    }
    if (    (check_char && isCharPressed(s[0])) ||
            (check_key && ImGui::IsKeyPressed((ImGuiKey)(unsigned char)s[0],
                                              false))) {
        action_exec(action);
        return 1;
    }
    return 0;
}

static void render_popups(int index)
{
    int r;
    int flags;
    typeof(gui->popup[0]) *popup;
    ImGuiIO& io = ImGui::GetIO();

    popup = &gui->popup[index];
    if (!popup->title) return;

    if (!popup->opened) {
        ImGui::OpenPopup(popup->title);
        popup->opened = true;
    }
    flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove;
    if (popup->flags & GUI_POPUP_FULL) {
        ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x - 40,
                                        io.DisplaySize.y - 40),
                (popup->flags & GUI_POPUP_RESIZE) ?  ImGuiCond_Once : 0);
    } else if (popup->size[0] > 0.f && popup->size[1] > 0.f) {
        ImGui::SetNextWindowSize(ImVec2(popup->size[0], popup->size[1]),
                                 ImGuiCond_Always);
        flags &= ~ImGuiWindowFlags_AlwaysAutoResize;
    }
    if (popup->flags & GUI_POPUP_RESIZE) {
        flags &= ~(ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_AlwaysAutoResize);
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 10));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, COLOR(WINDOW, BACKGROUND, false));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, COLOR(WINDOW, INNER, false));

    if (ImGui::BeginPopupModal(popup->title, NULL, flags)) {
        typeof(popup->func) func = popup->func;
        if ((r = func(popup->data))) {
            ImGui::CloseCurrentPopup();
            gui->popup_count--;
            popup->title = NULL;
            popup->func = NULL;
            free(popup->data);
            popup->data = NULL;
            if (popup->on_closed) popup->on_closed(r);
            popup->on_closed = NULL;
            popup->opened = false;
        }
        render_popups(index + 1);
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
}

static void render_fps(int fps) {
    ImDrawList* draw_list = ImGui::GetBackgroundDrawList();

    ImVec2 pos = ImVec2(2, ImGui::GetIO().DisplaySize.y - 15);

    char buffer[32]; // Buffer for FPS text
    sprintf(buffer, "FPS: %d", fps); // Convert integer to string

    draw_list->AddText(ImGui::GetFont(), ImGui::GetFontSize(), pos, IM_COL32(255, 255, 255, 255), buffer);
}

static void apply_camera_gizmo_preset(camera_t *camera, camera_mode_t mode,
                                      bool ortho, bool top_down)
{
    camera_set_mode(camera, mode);
    camera->ortho = ortho;

    if (top_down) {
        /* 580 fits a 512×512 map; scale by the longest XY side. */
        float map_xy = 512.0f;
        float image_center[3] = {0};
        if (!box_is_null(goxel.image->box)) {
            int dims[3];
            box_get_dimensions(goxel.image->box, dims);
            map_xy = (float)max(dims[0], dims[1]);
            mat4_mul_vec3(goxel.image->box, vec3_zero, image_center);
            if (map_xy <= 0.f)
                map_xy = 512.0f;
        }
        camera->dist = 580.0f * (map_xy / 512.0f);
        mat4_set_identity(camera->mat);
        vec3_copy(image_center, camera->mat[3]);
        mat4_itranslate(camera->mat, 0, 0, camera->dist);
        camera_turntable(camera, 0, 0);
    }
}

static bool gizmo_camera_icon_button(const char *id, int icon, float size)
{
    ImDrawList *draw_list = ImGui::GetWindowDrawList();
    const ImVec2 button_size(size, size);
    const ImVec4 bg = ImVec4(0.08f, 0.08f, 0.08f, 0.55f);
    const ImVec4 bg_hover = ImVec4(0.14f, 0.14f, 0.14f, 0.72f);
    const ImVec4 bg_active = ImVec4(0.20f, 0.20f, 0.20f, 0.82f);

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_Button, bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, bg_hover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, bg_active);
    bool ret = ImGui::Button(id, button_size);
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();

    ImVec2 center = ImGui::GetItemRectMin() + ImGui::GetItemRectSize() * 0.5f;
    const float icon_half_size = size * 0.52f;
    const int icon_no_theme = icon & 0xffff;
    const int ix = (icon_no_theme - 1) % 8;
    const int iy = (icon_no_theme - 1) / 8;
    const float cell = 1.0f / 8.0f;
    ImVec2 uv0(ix * cell, iy * cell);
    ImVec2 uv1(uv0.x + cell, uv0.y + cell);
    draw_list->AddImage((intptr_t)g_tex_icons->tex,
                        center - ImVec2(icon_half_size, icon_half_size),
                        center + ImVec2(icon_half_size, icon_half_size),
                        uv0, uv1, get_icon_color(icon, 0));
    return ret;
}

static void gizmo_camera_tooltip_if_hovered(const char *info)
{
    if (!info || !ImGui::IsItemHovered() || gui->scrolling)
        return;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, COLOR(TOOLTIP, BACKGROUND, 0));
    ImGui::SetTooltip("%s", info);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
}

/*
 * The overlays are submitted after the rest of the UI, so they would end up
 * above it in the window stack and swallow clicks on whatever they cover (the
 * menu bar panel toggles in the top right corner, the top bar, ...).  Keeping
 * them at the back of the stack makes them behave like viewport decoration.
 */
static void gizmo_window_send_to_back(const char *name)
{
    ImGuiWindow *window = ImGui::FindWindowByName(name);
    if (window)
        ImGui::BringWindowToDisplayBack(window);
}

static void render_view_cube(void)
{
    ImGuiIO& io = ImGui::GetIO();

    ImGuizmo::SetRect(0, 0, io.DisplaySize.x, io.DisplaySize.y);
    camera_t *camera = goxel.image->active_camera;
    float view[4][4];
    const float w = 128, h = 128;
    const float *projection= (float*)camera->proj_mat;
    const float zup2yup[4][4] = {
        {1, 0, 0, 0},
        {0, 0, -1, 0},
        {0, 1, 0, 0},
        {0, 0, 0, 1},
    };
    const float yup2zup[4][4] = {
        {1, 0, 0, 0},
        {0, 0, 1, 0},
        {0, -1, 0, 0},
        {0, 0, 0, 1},
    };
    ImGuizmo::Style &style = ImGuizmo::GetStyle();

    style.Colors[ImGuizmo::DIRECTION_X] = ImVec4(0.666f, 0.000f, 0.000f, 1.000f);
    style.Colors[ImGuizmo::DIRECTION_Z] = ImVec4(0.000f, 0.666f, 0.000f, 1.000f);
    style.Colors[ImGuizmo::DIRECTION_Y] = ImVec4(0.000f, 0.000f, 0.666f, 1.000f);

    // XXX: ImGuizmo is using Y up.
    mat4_mul(zup2yup, camera->mat, view);
    mat4_invert(view, view);

    const float icon_size = 42.0f;
    const float icon_spacing = 6.0f;
    const float icons_h = icon_size * 4 + icon_spacing * 3;
    const float win_w = w;
    const float icon_x = max(2.0f, (win_w - icon_size) * 0.5f) + 30.0f;
    /* Viewport already excludes the layers panel width when it is open. */
    const float cube_x = goxel.gui.viewport[0] + goxel.gui.viewport[2] - win_w;
    /* Start below the menu bar: the overlay draws behind it, so any overlap
     * would clip the cube. */
    const float cube_y = ImMax(goxel.gui.viewport[1],
                               ImGui::GetMainViewport()->WorkPos.y);

    /* ViewManipulate turns `length` into eye position (target + dir * length).
     * First-person modes stash dist as 0; passing 0 collapses the camera. */
    float gizmo_dist = camera->dist;
    if (gizmo_dist <= 0.f)
        gizmo_dist = camera->prev_dist > 0.f ? camera->prev_dist : 128.f;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    if (goxel.gui.view_cube_open) {
        ImGui::SetNextWindowSize(ImVec2(win_w, h));
        ImGui::SetNextWindowPos(ImVec2(cube_x, cube_y));
        ImGui::Begin("Gizmo", NULL, ImGuiWindowFlags_NoDecoration |
                                    ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGuizmo::SetDrawlist();

        ImGuizmo::SetRect(
                ImGui::GetWindowPos().x, ImGui::GetWindowPos().y, w, h);
        ImGuizmo::ViewManipulate(
               (float*)view, projection,
               ImGuizmo::ROTATE, ImGuizmo::LOCAL,
               (float*)&mat4_identity, gizmo_dist,
               ImGui::GetWindowPos(),
               ImVec2(w, h), 0x0);

        /* Orbit only: cube may rewrite the view. In FPV/Player, dist is not
         * the eye offset - writing back would yank the camera along the look
         * axis. */
        if (!camera_is_firstperson(camera)) {
            mat4_invert(view, view);
            mat4_mul(yup2zup, view, camera->mat);
        }

        ImGui::End();
    }

    if (goxel.gui.camera_presets_open) {
        /* Separate window so preset clicks are outside ViewManipulate's hit
         * rect. */
        ImGui::SetNextWindowSize(ImVec2(win_w, icons_h + 4.0f));
        const float presets_y = goxel.gui.view_cube_open ?
                cube_y + h - 5.0f : cube_y + 4.0f;
        ImGui::SetNextWindowPos(ImVec2(cube_x, presets_y));
        ImGui::Begin("GizmoCameraPresets", NULL,
                     ImGuiWindowFlags_NoDecoration |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::PushStyleVar(
                ImGuiStyleVar_ItemSpacing, ImVec2(0, icon_spacing));
        ImGui::SetCursorPos(ImVec2(icon_x, 2.0f));

        if (gizmo_camera_icon_button(
                    "##camera_ptz", ICON_CAMERA_PTZ, icon_size))
            apply_camera_gizmo_preset(
                    camera, CAMERA_MODE_ORBIT, false, false);
        gizmo_camera_tooltip_if_hovered(
                "Orbit camera - middle click to orbit, right click to pan, "
                "scroll to zoom");

        ImGui::SetCursorPosX(icon_x);
        if (gizmo_camera_icon_button(
                    "##camera_fly", ICON_CAMERA_FLY, icon_size))
            apply_camera_gizmo_preset(camera, CAMERA_MODE_FPV, false, false);
        gizmo_camera_tooltip_if_hovered(
                "Fly camera - use arrow keys to move, right click to look");

        ImGui::SetCursorPosX(icon_x);
        if (gizmo_camera_icon_button(
                    "##camera_player", ICON_CAMERA_PLAYER, icon_size))
            apply_camera_gizmo_preset(
                    camera, CAMERA_MODE_PLAYER, false, false);
        gizmo_camera_tooltip_if_hovered(
                "Player camera - WASD to move, right click look, space jump "
                "and ctrl crouch; hold alt to fly");

        ImGui::SetCursorPosX(icon_x);
        if (gizmo_camera_icon_button(
                    "##camera_topdown", ICON_CAMERA_TOPDOWN, icon_size)) {
            apply_camera_gizmo_preset(
                    camera, CAMERA_MODE_ORBIT, true, true);
            /* Surface paint only applies in Paint mode; leave it on so
             * switching into Paint (or already being in Paint) uses the
             * top-down stamp. */
            goxel.brush_surface_paint = true;
        }
        gizmo_camera_tooltip_if_hovered(
                "Top-down camera - right click to pan, scroll to zoom");
        ImGui::PopStyleVar();

        ImGui::End();
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();

    /* Presets first, so the cube ends up behind them as before. */
    gizmo_window_send_to_back("GizmoCameraPresets");
    gizmo_window_send_to_back("Gizmo");
}

static void gui_world_labels_flush(void);

static void gui_iter(const inputs_t *inputs)
{
    gui_init();
    unsigned int i;
    ImGuiIO& io = ImGui::GetIO();
    ImGuiStyle& style = ImGui::GetStyle();

    io.DisplaySize = ImVec2((float)goxel.screen_size[0],
                            (float)goxel.screen_size[1]);

    io.DisplayFramebufferScale = ImVec2(goxel.screen_scale,
                                        goxel.screen_scale);
    io.DeltaTime = goxel.delta_time;
    io.ConfigDragClickToInputText = true;

    if (inputs) {
        io.DisplayFramebufferScale = ImVec2(inputs->scale, inputs->scale);
        io.FontGlobalScale = 1 / inputs->scale;
        io.MousePos.x = inputs->touches[0].pos[0];
        io.MousePos.y = inputs->touches[0].pos[1];
        /* inputs: down[0]=L, down[1]=M, down[2]=R - ImGui: 0=L, 1=R, 2=M */
        io.MouseDown[0] = inputs->touches[0].down[0];
        io.MouseDown[1] = inputs->touches[0].down[2];
        io.MouseDown[2] = inputs->touches[0].down[1];
        gui->margins = inputs->safe_margins;
        io.MouseWheel = inputs->mouse_wheel;

        for (i = 0; i < ARRAY_SIZE(inputs->keys); i++)
            io.KeysDown[i] = inputs->keys[i];
        /* Stash before NewFrame; io.KeyShift can be remapped away. */
        gui->shift_down = inputs->keys[KEY_LEFT_SHIFT] ||
                          inputs->keys[KEY_RIGHT_SHIFT];
        gui->ctrl_down = inputs->keys[KEY_CONTROL];
        io.KeyShift = gui->shift_down;
        io.KeyCtrl = gui->ctrl_down;
        for (i = 0; i < ARRAY_SIZE(inputs->chars); i++) {
            if (!inputs->chars[i]) break;
            io.AddInputCharacter(inputs->chars[i]);
        }
        memset((void*)inputs->chars, 0, sizeof(inputs->chars));
    } else {
        gui->shift_down = false;
        gui->ctrl_down = false;
    }


    // Setup theme.
    ImGui::StyleColorsDark();
    style.WindowBorderSize = 0;
    style.WindowPadding = ImVec2(8, 5);
    style.FrameRounding = 2;
    style.ChildRounding = 4;
    style.WindowRounding = 6;
    style.ChildBorderSize = 0;
    style.SelectableTextAlign = ImVec2(0.5, 0.5);
    style.Colors[ImGuiCol_WindowBg] = COLOR(WINDOW, BACKGROUND, false);
    style.Colors[ImGuiCol_ChildBg] = COLOR(SECTION, BACKGROUND, false);
    style.Colors[ImGuiCol_Header] = ImVec4(0, 0, 0, 0);
    style.Colors[ImGuiCol_Text] = COLOR(BASE, TEXT, false);
    style.Colors[ImGuiCol_MenuBarBg] = COLOR(MENU, BACKGROUND, false);

    gui->scrolling = (gui->scrolling & 1) ? 2 : 0;
    gui->can_move_window = (gui->can_move_window & 1) ? 2 : 0;

    // Old code, to remove.
    ImGui::NewFrame();
    ImGuizmo::BeginFrame();

    tool_cursor_on_gui_frame();
    gui_app();
    custom_objects_render_labels(goxel.image);
    tool_cursor_render_labels();
    gui_world_labels_flush();
    if (goxel.gui.ui_visible &&
        (goxel.gui.view_cube_open || goxel.gui.camera_presets_open))
        render_view_cube();
    render_popups(0);

    if (!io.WantCaptureKeyboard) {
        actions_iter(check_action_shortcut, NULL);
    }
    if (goxel.gui.ui_visible)
        render_fps((int)round(goxel.fps));
    ImGui::EndFrame();

    sys_set_mouse_cursor(ImGui::GetMouseCursor());
    sys_show_keyboard(io.WantTextInput);
}

void gui_render(const inputs_t *inputs)
{
    gui_init();
    gui_iter(inputs);

    ImGui::Render();
    ImImpl_RenderDrawLists(ImGui::GetDrawData());
}

/* Queued world labels: flushed once so all fills then all text share draw
 * cmds (Fill/Text alternation per label was splitting the draw list). */
typedef struct {
    ImVec2 a, b, text_pos;
    const char *text;
    ImU32 border;
} world_label_t;
static world_label_t g_world_labels[LAYER_SUBTREE_MAX];
static int g_world_label_n = 0;

static void gui_world_labels_flush(void)
{
    ImDrawList *dl;
    int i;

    if (g_world_label_n <= 0) return;
    dl = ImGui::GetBackgroundDrawList();
    for (i = 0; i < g_world_label_n; i++)
        dl->AddRectFilled(g_world_labels[i].a, g_world_labels[i].b,
                          IM_COL32(0, 0, 0, 140));
    for (i = 0; i < g_world_label_n; i++)
        dl->AddRect(g_world_labels[i].a, g_world_labels[i].b,
                    g_world_labels[i].border);
    for (i = 0; i < g_world_label_n; i++)
        dl->AddText(g_world_labels[i].text_pos, IM_COL32(255, 255, 255, 255),
                    g_world_labels[i].text);
    g_world_label_n = 0;
}

void gui_world_label(const float pos[3], const char *text,
                     const uint8_t color[4])
{
    const float *viewport = goxel.gui.viewport;
    const camera_t *camera = goxel.image ? goxel.image->active_camera : NULL;
    const float pad_x = 6, pad_y = 3, gap = 10;
    float p[4] = {pos[0], pos[1], pos[2], 1};
    float x, y;
    ImVec2 size, a, b;
    world_label_t *slot;

    if (!camera || !text || !text[0]) return;
    if (g_world_label_n >= LAYER_SUBTREE_MAX) return;

    mat4_mul_vec4(camera->view_mat, p, p);
    mat4_mul_vec4(camera->proj_mat, p, p);
    if (p[3] <= 0) return;
    p[0] /= p[3];
    p[1] /= p[3];
    p[2] /= p[3];
    if (p[0] < -1 || p[0] > 1 || p[1] < -1 || p[1] > 1 ||
        p[2] < -1 || p[2] > 1)
        return;

    /* The gl viewport has its origin at the bottom left, imgui at the top. */
    x = viewport[0] + (p[0] * 0.5f + 0.5f) * viewport[2];
    y = ImGui::GetIO().DisplaySize.y -
        (viewport[1] + (p[1] * 0.5f + 0.5f) * viewport[3]);

    size = ImGui::CalcTextSize(text);
    b = ImVec2(roundf(x + size.x / 2 + pad_x), roundf(y - gap));
    a = ImVec2(roundf(x - size.x / 2 - pad_x), roundf(b.y - size.y - pad_y * 2));

    slot = &g_world_labels[g_world_label_n++];
    slot->a = a;
    slot->b = b;
    slot->text_pos = ImVec2(a.x + pad_x, a.y + pad_y);
    slot->text = text;
    slot->border = color ? IM_COL32(color[0], color[1], color[2], 200)
                         : IM_COL32(255, 255, 255, 90);
}

void gui_group_begin(const char *label)
{
    if (label && label[0] != '#') ImGui::Text("%s", label);
    ImGui::PushID(label ?: "group");
    ImGui::BeginGroup();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 2));
}

void gui_group_end(void)
{
    ImGui::PopID();
    ImGui::PopStyleVar(1);
    ImGui::Dummy(ImVec2(0, 0));
    ImGui::EndGroup();
    if (gui->is_row) ImGui::SameLine();
}

void gui_scrollable_begin(float max_height) {
    ImVec2 available_size = ImGui::GetContentRegionAvail();
    float height = max_height > 0.0f ? max_height : available_size.y;

    // Create a scrollable area with a specific height
    ImGui::BeginChild("scrollable_group", ImVec2(0, height), true);
}
void gui_scrollable_end() {
    ImGui::EndChild();
}

bool gui_scroll_item_into_view(void)
{
    ImGuiWindow *window = ImGui::GetCurrentWindow();
    const ImVec2 a = ImGui::GetItemRectMin();
    const ImVec2 b = ImGui::GetItemRectMax();
    const ImRect &clip = window->InnerClipRect;
    /* Extra margin so the row is not flush against the clip edge (and so a
     * scrollbar appearing next frame does not leave it one pixel short). */
    float pad = ImMax(ImGui::GetStyle().ItemSpacing.y, 6.0f);
    float view_h = clip.Max.y - clip.Min.y;
    float item_h = b.y - a.y;
    if (item_h + 2.0f * pad > view_h)
        pad = 0.0f;

    if (a.y >= clip.Min.y + pad && b.y <= clip.Max.y - pad)
        return true;

    float scroll = window->Scroll.y;
    /* Far off-screen: jump so the row is centred. Near the edge: nudge. */
    if (b.y < clip.Min.y || a.y > clip.Max.y) {
        float item_mid = (a.y + b.y) * 0.5f;
        float view_mid = (clip.Min.y + clip.Max.y) * 0.5f;
        scroll += item_mid - view_mid;
    } else {
        if (a.y < clip.Min.y + pad)
            scroll -= (clip.Min.y + pad) - a.y;
        if (b.y > clip.Max.y - pad)
            scroll += b.y - (clip.Max.y - pad);
    }
    /* ScrollTarget is applied on the next Begin(); caller should keep
     * requesting until this returns true (content size / clamp lag). */
    ImGui::SetScrollY(window, ImMax(0.0f, scroll));
    return false;
}

bool gui_section_begin(const char *label, int flags)
{
    ImGuiChildFlags childflags =
        ImGuiChildFlags_AutoResizeY |
        ImGuiChildFlags_AlwaysUseWindowPadding;
    float padding, w;

    // We ensure that everything stays aligned with widgets outside a section.
    padding = ImGui::GetStyle().WindowPadding.x;
    w = ImGui::GetContentRegionAvail().x;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(padding / 2, padding / 2));
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() - padding / 2);
    ImGui::BeginChild(label, ImVec2(w + padding, 0), childflags);

    if (flags & (GUI_SECTION_COLLAPSABLE | GUI_SECTION_COLLAPSABLE_CLOSED)) {
        ImGui::SetNextItemOpen(
                !(flags & GUI_SECTION_COLLAPSABLE_CLOSED), ImGuiCond_Once);
        return ImGui::CollapsingHeader(label);
    } else {
        if (label && label[0] != '#')
            ImGui::Text("%s", label);
        return true;
    }
}

void gui_section_end(void)
{
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

bool gui_tabsheet_begin(const char *id, const char **labels, int count,
                        int *current)
{
    int i;
    ImGuiStorage *storage;
    ImGuiID sel_key;
    int prev;
    int want;
    int reported;
    bool force_select;

    if (!id || !labels || !current || count < 1)
        return false;
    if (*current < 0 || *current >= count)
        *current = 0;

    ImGui::PushStyleColor(ImGuiCol_Tab, COLOR(TAB, BACKGROUND, false));
    ImGui::PushStyleColor(ImGuiCol_TabHovered,
                          color_lighten(COLOR(TAB, SELECTED, false)));
    ImGui::PushStyleColor(ImGuiCol_TabSelected, COLOR(TAB, SELECTED, false));
    ImGui::PushStyleColor(ImGuiCol_TabSelectedOverline, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_TabDimmed, COLOR(TAB, BACKGROUND, false));
    ImGui::PushStyleColor(ImGuiCol_TabDimmedSelected,
                          COLOR(TAB, SELECTED, false));
    ImGui::PushStyleColor(ImGuiCol_TabDimmedSelectedOverline,
                          ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, COLOR(TAB, TEXT, false));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 3));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemInnerSpacing, ImVec2(4, 2));

    ImGui::PushID(id);
    if (!ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_None)) {
        ImGui::PopID();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(8);
        return false;
    }

    /* Force ImGui's selection when the bound value changed externally (e.g.
     * Shift+click enters Palette mode while the Color tab is still selected).
     * Keep applying SetSelected and refuse to write *current from a stale tab
     * until ImGui reports the wanted tab - otherwise one frame of Color wins,
     * brush exits Palette mode, and the UI flickers back. */
    storage = ImGui::GetStateStorage();
    sel_key = ImGui::GetID("tabsheet_sel");
    want = *current;
    prev = storage->GetInt(sel_key, want);
    force_select = (prev != want);

    reported = -1;
    for (i = 0; i < count; i++) {
        ImGuiTabItemFlags flags = 0;
        if (force_select && want == i)
            flags |= ImGuiTabItemFlags_SetSelected;
        if (ImGui::BeginTabItem(labels[i] ? labels[i] : "", NULL, flags)) {
            reported = i;
            ImGui::EndTabItem();
        }
    }

    if (force_select) {
        *current = want;
        /* Only mark synced once ImGui agrees; else force again next frame. */
        if (reported == want)
            storage->SetInt(sel_key, want);
    } else {
        if (reported >= 0)
            *current = reported;
        storage->SetInt(sel_key, *current);
    }

    ImGui::EndTabBar();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(8);

    ImGui::BeginGroup();
    return true;
}

void gui_tabsheet_end(void)
{
    ImGui::EndGroup();
    ImGui::PopID();
}

void gui_row_begin(int nb)
{
    float spacing;
    float avail;
    ImGui::BeginGroup();
    gui->is_row++;
    gui->item_size = 0;
    if (nb) {
        spacing = ImGui::GetStyle().ItemSpacing.x;
        avail = ImGui::GetContentRegionAvail().x;
        gui->item_size = (avail - (nb - 1) * spacing) / nb;
    }
}

void gui_row_end(void)
{
    ImGui::EndGroup();
    gui->is_row--;
    gui->item_size = 0;
}

int gui_window_begin(const char *label, float x, float y, float w, float h,
                     int flags)
{
    ImGuiWindowFlags win_flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoDecoration;
    float max_size;
    ImGuiStorage *storage = ImGui::GetStateStorage();
    ImGuiID key;
    float *last_pos;
    int ret = 0;
    int dir = (flags & GUI_WINDOW_HORIZONTAL) ? 0 : 1;

    ImGui::PushID(label);
    if (!gui->can_move_window)
        win_flags |= ImGuiWindowFlags_NoMove;
    if (gui->scrolling)
        win_flags |= ImGuiWindowFlags_NoMouseInputs;
    if (dir == 0)
        win_flags |= ImGuiWindowFlags_HorizontalScrollbar;
    /* Vertical panels scroll inside a body child (see gui_panel_header) so
     * the title/close bar stays fixed. */
    if (dir == 1)
        win_flags |= ImGuiWindowFlags_NoScrollbar |
                     ImGuiWindowFlags_NoScrollWithMouse;
    if (flags & GUI_WINDOW_CENTER) {
        ImGuiViewport *vp = ImGui::GetMainViewport();
        ImGuiCond cond = ImGuiCond_Always;
        if ((flags & GUI_WINDOW_MOVABLE) && !(flags & GUI_WINDOW_CENTER_FORCE))
            cond = ImGuiCond_Appearing;
        ImGui::SetNextWindowPos(
                vp->GetCenter(), cond, ImVec2(0.5f, 0.5f));
    } else {
        ImGui::SetNextWindowPos(
                ImVec2(x, y),
                (flags & GUI_WINDOW_MOVABLE) ? ImGuiCond_Appearing
                                             : ImGuiCond_Always);
    }
    ImGui::SetNextWindowSize(ImVec2(w, h));

    key = ImGui::GetID("last_pos");
    last_pos = storage->GetFloatRef(key, dir == 0 ? x : y);

    gui->win_body_started = false;
    gui->win_autofit_y = (h == 0 && dir == 1);
    gui->win_max_h = h;

    if ((w == 0) && (dir == 0))
    {
        max_size = ImGui::GetMainViewport()->Size.x - *last_pos;
        ImGui::SetNextWindowSizeConstraints(
            ImVec2(0, 0), ImVec2(max_size, FLT_MAX));
    }
    if ((h == 0) && (dir == 1))
    {
        max_size = ImGui::GetMainViewport()->Size.y - *last_pos;
        gui->win_max_h = max_size;
        ImGui::SetNextWindowSizeConstraints(
            ImVec2(0, 0), ImVec2(FLT_MAX, max_size));
    }

    ImGui::Begin(label, NULL, win_flags);

    if (flags & GUI_WINDOW_MOVABLE)
    {
        if (ImGui::GetWindowPos() != ImVec2(x, y))
            ret |= GUI_WINDOW_MOVED;
        *last_pos = dir == 0 ? ImGui::GetWindowPos().x : ImGui::GetWindowPos().y;
    }

    gui->win_dir = dir;
    ImGui::BeginGroup();
    return ret;
}

gui_window_ret_t gui_window_end(void)
{
    gui_window_ret_t ret = {};
    if (gui->win_body_started) {
        ret.has_v_scrollbar = ImGui::GetCurrentWindow()->ScrollbarY;
        ImGui::EndChild();
        ImGui::PopStyleColor(/* ChildBg */ 1);
        gui->win_body_started = false;
    }
    ImGui::EndGroup();
    if (!GUI_HAS_SCROLLBARS && !gui->can_move_window) {
        if (gui_pan_scroll_behavior(gui->win_dir))
            gui->scrolling |= 1;
    }
    ret.h = ImGui::GetWindowHeight();
    ret.w = ImGui::GetWindowWidth();
    ImGui::End();
    ImGui::PopID();

    return ret;
}

void gui_window_resize_left_edge(float *width, float min_w, float max_w)
{
    ImGuiContext& g = *GImGui;
    ImGuiWindow *window;
    ImGuiID id;
    const float pad = 6.0f;
    ImRect bb;
    bool hovered, held;
    float w;

    if (!width) return;
    window = ImGui::GetCurrentWindow();
    /* Hit-test the panel frame, not an inner scroll body child. */
    if (window->RootWindow)
        window = window->RootWindow;
    id = window->GetID("##resize_left");
    bb = ImRect(ImVec2(window->Pos.x - pad, window->Pos.y),
                ImVec2(window->Pos.x + pad, window->Pos.y + window->Size.y));

    /* Hit-test the edge directly so hover works even when later panel widgets
     * cover the strip, and slightly outside the window (NoResize windows get
     * no ImGui hover padding). */
    hovered = bb.Contains(g.IO.MousePos);
    held = false;
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        ImGui::SetActiveID(id, window);
    if (g.ActiveId == id) {
        held = ImGui::IsMouseDown(ImGuiMouseButton_Left);
        if (!held)
            ImGui::ClearActiveID();
        else
            ImGui::KeepAliveID(id);
    }
    if (hovered || held)
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (held) {
        w = *width - g.IO.MouseDelta.x;
        if (w < min_w) w = min_w;
        if (w > max_w) w = max_w;
        *width = w;
    }
}

void gui_floating_panel_begin(const char *title, float init_w, float init_h)
{
    ImGuiViewport *vp = ImGui::GetMainViewport();

    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_FirstUseEver,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(init_w, init_h), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(220.f, 140.f), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::Begin(title, NULL, ImGuiWindowFlags_None);
}

void gui_floating_panel_end(void)
{
    ImGui::End();
}

bool gui_palette_window_begin(float init_w, float init_h)
{
    if (!goxel.gui.palette_win_open)
        return false;

    if (goxel.gui.palette_win_expand_once) {
        ImGui::SetNextWindowCollapsed(false, ImGuiCond_Always);
        goxel.gui.palette_win_expand_once = false;
    }

    ImGuiViewport *vp = ImGui::GetMainViewport();

    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_FirstUseEver,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(init_w, init_h), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(220.f, 140.f), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::Begin("Palette##palette_floating", &goxel.gui.palette_win_open,
                 ImGuiWindowFlags_None);
    goxel.gui.palette_win_collapsed = ImGui::IsWindowCollapsed();
    return true;
}

void gui_palette_window_end(void)
{
    ImGui::End();
}

void gui_set_cursor_pos(float x, float y)
{
    ImGui::SetCursorPos(ImVec2(x, y));
}

float gui_window_content_region_min_x(void)
{
    return ImGui::GetWindowContentRegionMin().x;
}

float gui_window_content_region_max_x(void)
{
    return ImGui::GetWindowContentRegionMax().x;
}

float gui_get_cursor_pos_x(void)
{
    return ImGui::GetCursorPos().x;
}

float gui_get_cursor_pos_y(void)
{
    return ImGui::GetCursorPos().y;
}

float gui_get_item_rect_size_y(void)
{
    return ImGui::GetItemRectSize().y;
}

float gui_style_item_spacing_x(void)
{
    return ImGui::GetStyle().ItemSpacing.x;
}

float gui_style_item_spacing_y(void)
{
    return ImGui::GetStyle().ItemSpacing.y;
}

void gui_push_item_spacing(float x, float y)
{
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(x, y));
}

void gui_pop_style_var(int count)
{
    ImGui::PopStyleVar(count);
}

void gui_same_line_spaced(float spacing)
{
    ImGui::SameLine(0, spacing);
}

bool gui_toolbar_segment(const char *label, bool selected)
{
    bool ret;

    if (selected) {
        ImGui::PushStyleColor(ImGuiCol_Button,
                ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
    }
    ret = ImGui::SmallButton(label);
    if (selected)
        ImGui::PopStyleColor(3);
    if (ret)
        on_click();
    return ret;
}

float gui_calc_text_width(const char *text)
{
    if (!text)
        return 0.f;
    return ImGui::CalcTextSize(text, NULL, true).x;
}

float gui_toolbar_segment_width(const char *label)
{
    const ImGuiStyle &st = ImGui::GetStyle();
    ImVec2 ts = ImGui::CalcTextSize(label, NULL, true);
    return ts.x + st.FramePadding.x * 2.f;
}

float gui_content_avail_x(void)
{
    return ImGui::GetContentRegionAvail().x;
}

void gui_new_line(void)
{
    ImGui::NewLine();
}

void gui_label_size_push(float v)
{
    if (g_label_size_top >= (int)(sizeof(g_label_size_stack) / sizeof(g_label_size_stack[0]))) {
        LOG_E("gui_label_size_push: label size stack overflow");
        return;
    }
    g_label_size_stack[g_label_size_top++] = v;
}

void gui_label_size_pop(void)
{
    if (g_label_size_top <= 1) {
        LOG_E("gui_label_size_pop: attempted to pop base label size");
        return;
    }
    if (g_label_size_top > 1)
        g_label_size_top--;
}
float gui_label_size_get(void)
{
    return g_label_size_stack[g_label_size_top - 1];
}

bool gui_input_int(const char *label, int *v, int minv, int maxv)
{
    float minvf = minv;
    float maxvf = maxv;
    bool ret;
    float vf = *v;
    if (minv == 0 && maxv == 0) {
        minvf = -FLT_MAX;
        maxvf = +FLT_MAX;
    }
    ret = gui_input_float(label, &vf, 1, minvf, maxvf, "%.0f");
    if (ret) *v = vf;
    return ret;
}

static void label_aligned(const char *label, float size)
{
    ImVec2 spacing;
    const char *label_end = ImGui::FindRenderedTextEnd(label);
    float text_size = ImGui::CalcTextSize(label, label_end).x;
    const ImGuiStyle &style = ImGui::GetStyle();

    if (size <= 0)
        return;

    spacing = style.ItemSpacing;
    spacing.x = ITEM_SPACING.x;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, spacing);

    ImGui::SetCursorPosX(size - text_size - ITEM_SPACING.x);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label, label_end);
    ImGui::SameLine();
    ImGui::PopStyleVar(1);
}

/*
 * Custom slider widget.
 */
bool slider_float(const char *label, float *v, float minv, float maxv, const char *format)
{
    bool ret;
    float step = (maxv - minv) * 0.008;
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 rmin, rmax;
    float k;
    bool highlighted;
    ImVec4 color;
    ImU32 col;

    ImGui::PushID(label);
    ImGui::BeginGroup();
    label_aligned(label, gui_label_size_get());
    // Render an imgui DragFloat with transparent background.
    draw_list->ChannelsSplit(2);
    draw_list->ChannelsSetCurrent(1);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
    ret = ImGui::DragFloat("", v, step, minv, maxv, format);
    ImGui::PopStyleColor(3);
    highlighted = ImGui::IsItemHovered();

    // Render our own slider below the input.
    rmin = ImGui::GetItemRectMin();
    rmax = ImGui::GetItemRectMax();
    rmax.x -= 17; // don't know why, but it ate into padding exactly this much in colors.c filter

    draw_list->ChannelsSetCurrent(0);
    k = (*v - minv) / (maxv - minv);
    color = COLOR(NUMBER_INPUT, INNER, false);
    if (highlighted) color = color_lighten(color);
    col = ImGui::GetColorU32(color);
    draw_list->AddRectFilled(rmin, rmax, col, 2);

    rmax.x = mix(rmin.x, rmax.x, k);
    color = COLOR(NUMBER_INPUT, ITEM, false);
    if (highlighted) color = color_lighten(color);
    col = ImGui::GetColorU32(color);
    draw_list->AddRectFilled(rmin, rmax, col, 2);

    draw_list->ChannelsMerge();
    ImGui::EndGroup();
    ImGui::PopID();
    return ret;
}

/* Snap to a step grid using double math so values like 1.3/0.1 and 0.3/0.01
 * don't get stuck on float32 division noise (see gui-input-float.md). */
static float snap_float_grid(float v, float quant)
{
    return (float)(round((double)v / (double)quant) * (double)quant);
}

bool gui_input_float(const char *label, float *v, float step,
                     float minv, float maxv, const char *format)
{
    bool ret = false;
    float button_width = 20; // Compute exactly.
    const char *left_utf = "◀";
    const char *right_utf = "▶";
    bool snap_grid;
    float snap_quant;
    float arrow_step;
    float v_speed;
    bool drag_active;
    bool drag_edited_end;

    if (minv == 0.f && maxv == 0.f) {
        minv = -FLT_MAX;
        maxv = +FLT_MAX;
    }

    if (step < 0.f) {
        snap_grid = false;
        arrow_step = -step;
        snap_quant = 0.f;
    } else {
        snap_grid = true;
        if (step == 0.f)
            snap_quant = 0.1f;
        else
            snap_quant = step;
        arrow_step = snap_quant;
    }

    if (!format) format = "%.1f";

    if (!snap_grid) {
        if (maxv > minv && minv > -FLT_MAX / 4.f && maxv < FLT_MAX / 4.f)
            v_speed = (maxv - minv) * 0.003f;
        else
            v_speed = 1.f;
    } else {
        v_speed = snap_quant / 10;
    }

    ImGui::PushID(label);

    ImGui::PushStyleColor(ImGuiCol_FrameBg, COLOR(NUMBER_INPUT, INNER, false));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,
                    color_lighten(COLOR(NUMBER_INPUT, INNER, false)));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,
                    color_lighten2(COLOR(NUMBER_INPUT, INNER, false)));
    ImGui::PushStyleColor(ImGuiCol_Button, COLOR(NUMBER_INPUT, INNER, false));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                    color_lighten(COLOR(NUMBER_INPUT, INNER, false)));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                    color_lighten2(COLOR(NUMBER_INPUT, INNER, false)));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab,
                    COLOR(NUMBER_INPUT, ITEM, false));

    label_aligned(label, gui_label_size_get());
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 2));
    ImGui::BeginGroup();
    ImGui::PushButtonRepeat(true);

        if (ImGui::Button(left_utf)) {
            (*v) -= arrow_step;
            ret = true;
        }
        ImGui::SameLine();
        ImGui::PushItemWidth(
                ImGui::GetContentRegionAvail().x - button_width);
        ret = ImGui::DragFloat("", v, v_speed, minv, maxv, format) || ret;
        // Capture before later buttons overwrite the last-item flags.
        drag_active = ImGui::IsItemActive();
        drag_edited_end = ImGui::IsItemDeactivatedAfterEdit();
        ImGui::PopItemWidth();
        ImGui::SameLine();
        if (ImGui::Button(right_utf)) {
            (*v) += arrow_step;
            ret = true;
        }

    ImGui::PopButtonRepeat();
    ImGui::PopStyleVar(1);
    ImGui::PopStyleColor(7);
    ImGui::EndGroup();
    ImGui::PopID();

    if (ret) {
        *v = clamp(*v, minv, maxv);
        // While dragging, ImGui's format rounding owns the value. Re-snapping
        // every frame fights DragCurrentAccum (1dp floor snap-back, and 2dp
        // bit rewrites like 0.30f → 0.29999998f). Snap on arrows / drag end.
        if (snap_grid && snap_quant > 0.f && !drag_active)
            *v = snap_float_grid(*v, snap_quant);
        *v = clamp(*v, minv, maxv);
        on_click();
    } else if (drag_edited_end && snap_grid && snap_quant > 0.f) {
        float snapped = clamp(snap_float_grid(*v, snap_quant), minv, maxv);
        if (snapped != *v) {
            *v = snapped;
            ret = true;
        }
    }
    return ret;
}

bool gui_input_float_stack(const char *label, float *v, float step,
                           float minv, float maxv, const char *format)
{
    bool ret = false;
    const char *up_utf = "▲";
    const char *down_utf = "▼";
    bool snap_grid;
    float snap_quant;
    float arrow_step;
    float v_speed;
    float label_w;
    float stack_w;
    float total_w;
    bool drag_active;
    bool drag_edited_end;
    ImVec2 origin;
    ImVec2 stack_size;
    const ImGuiStyle &style = ImGui::GetStyle();

    if (minv == 0.f && maxv == 0.f) {
        minv = -FLT_MAX;
        maxv = +FLT_MAX;
    }

    if (step < 0.f) {
        snap_grid = false;
        arrow_step = -step;
        snap_quant = 0.f;
    } else {
        snap_grid = true;
        if (step == 0.f)
            snap_quant = 0.1f;
        else
            snap_quant = step;
        arrow_step = snap_quant;
    }

    if (!format) format = "%.1f";

    if (!snap_grid) {
        if (maxv > minv && minv > -FLT_MAX / 4.f && maxv < FLT_MAX / 4.f)
            v_speed = (maxv - minv) * 0.003f;
        else
            v_speed = 1.f;
    } else {
        v_speed = snap_quant / 10;
    }

    ImGui::PushID(label);

    ImGui::PushStyleColor(ImGuiCol_FrameBg, COLOR(NUMBER_INPUT, INNER, false));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,
                    color_lighten(COLOR(NUMBER_INPUT, INNER, false)));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,
                    color_lighten2(COLOR(NUMBER_INPUT, INNER, false)));
    ImGui::PushStyleColor(ImGuiCol_Button, COLOR(NUMBER_INPUT, INNER, false));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                    color_lighten(COLOR(NUMBER_INPUT, INNER, false)));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                    color_lighten2(COLOR(NUMBER_INPUT, INNER, false)));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab,
                    COLOR(NUMBER_INPUT, ITEM, false));

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 1));
    ImGui::PushButtonRepeat(true);

    total_w = gui->item_size ? gui->item_size : ImGui::GetContentRegionAvail().x;
    label_w = ImGui::CalcTextSize(label, NULL, true).x + style.ItemSpacing.x;
    stack_w = total_w - label_w;
    if (stack_w < 24.f)
        stack_w = 24.f;

    origin = ImGui::GetCursorScreenPos();

    ImGui::SetCursorScreenPos(ImVec2(origin.x + label_w, origin.y));
    ImGui::BeginGroup();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x, 0));
    if (ImGui::Button(up_utf, ImVec2(stack_w, 0.f))) {
        (*v) += arrow_step;
        ret = true;
    }
    ImGui::PopStyleVar(1);
    ImGui::PushItemWidth(stack_w);
    ret = ImGui::DragFloat("##v", v, v_speed, minv, maxv, format) || ret;
    drag_active = ImGui::IsItemActive();
    drag_edited_end = ImGui::IsItemDeactivatedAfterEdit();
    ImGui::PopItemWidth();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(style.FramePadding.x, 0));
    if (ImGui::Button(down_utf, ImVec2(stack_w, 0.f))) {
        (*v) -= arrow_step;
        ret = true;
    }
    ImGui::PopStyleVar(1);
    ImGui::EndGroup();
    stack_size = ImGui::GetItemRectSize();

    ImGui::SetCursorScreenPos(ImVec2(
            origin.x,
            origin.y + (stack_size.y - ImGui::GetFontSize()) * 0.5f));
    ImGui::TextUnformatted(label);

    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(label_w + stack_size.x, stack_size.y));

    ImGui::PopButtonRepeat();
    ImGui::PopStyleVar(1);
    ImGui::PopStyleColor(7);
    ImGui::PopID();

    if (ret) {
        *v = clamp(*v, minv, maxv);
        if (snap_grid && snap_quant > 0.f && !drag_active)
            *v = snap_float_grid(*v, snap_quant);
        *v = clamp(*v, minv, maxv);
        on_click();
    } else if (drag_edited_end && snap_grid && snap_quant > 0.f) {
        float snapped = clamp(snap_float_grid(*v, snap_quant), minv, maxv);
        if (snapped != *v) {
            *v = snapped;
            ret = true;
        }
    }
    if (gui->is_row) ImGui::SameLine();
    return ret;
}

bool gui_bbox(float box[4][4])
{
    int x, y, z, w, h, d;
    bool ret = false;
    float p[3];
    w = box[0][0] * 2;
    h = box[1][1] * 2;
    d = box[2][2] * 2;
    x = round(box[3][0] - box[0][0]);
    y = round(box[3][1] - box[1][1]);
    z = round(box[3][2] - box[2][2]);

    gui_group_begin("Origin");
    ret |= gui_input_int("x", &x, 0, 0);
    ret |= gui_input_int("y", &y, 0, 0);
    ret |= gui_input_int("z", &z, 0, 0);
    gui_group_end();
    gui_group_begin("Size");
    ret |= gui_input_int("w", &w, 0, 0);
    ret |= gui_input_int("h", &h, 0, 0);
    ret |= gui_input_int("d", &d, 0, 0);
    w = max(1, w);
    h = max(1, h);
    d = max(1, d);
    gui_group_end();

    if (ret) {
        vec3_set(p, x + w / 2., y + h / 2., z + d / 2.);
        bbox_from_extents(box, p, w / 2., h / 2., d / 2.);
    }
    return ret;
}

bool gui_angle(const char *id, float *v, int vmin, int vmax)
{
    int a;
    bool ret;
    a = round(*v * DR2D);
    ret = gui_input_int(id, &a, vmin, vmax);
    if (ret) {
        if (vmin == 0 && vmax == 360) {
            while (a < 0) a += 360;
            a %= 360;
        }
        a = clamp(a, vmin, vmax);
        *v = (float)(a * DD2R);
    }
    return ret;
}

bool gui_action_button(int id, const char *label, float size)
{
    bool ret;
    const action_t *action;

    action = action_get(id, true);
    assert(action);
    ImGui::PushID(action->id);
    ret = gui_button(label, size, action->icon);
    if (ImGui::IsItemHovered())
        goxel_set_help_text(action_get(id, true)->help);
    if (ret) {
        action_exec(action_get(id, true));
    }
    ImGui::PopID();
    if (gui->is_row) ImGui::SameLine();
    return ret;
}

static bool _selectable(const char *label, bool *v, const char *tooltip,
                        float w, int icon, bool condensed)
{
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    ImVec2 size;
    ImVec2 center;
    bool ret = false;
    bool default_v = false;
    ImVec2 uv0, uv1; // The position in the icon texture.

    float icon_height = ICON_HEIGHT * (condensed ? CONDENSE_FACTOR : 1);

    if (gui->item_size) w = gui->item_size;

    v = v ? v : &default_v;
    size = (icon != -1) ?
        ImVec2(icon_height, icon_height) :
        ImVec2(w, icon_height);

    if (!tooltip && icon != -1) {
        tooltip = label;
        while (*tooltip == '#') tooltip++;
    }

    ImGui::PushID(label);
    if (icon == -1) {
        ImGui::PushStyleColor(ImGuiCol_Button, COLOR(SELECTABLE, INNER, (*v)));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                color_lighten(COLOR(SELECTABLE, INNER, true)));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                color_lighten2(COLOR(SELECTABLE, INNER, true)));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, COLOR(ICON, INNER, (*v)));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                color_lighten(COLOR(ICON, INNER, true)));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                color_lighten2(COLOR(ICON, INNER, true)));
    }

    if (icon != -1) {
        ret = ImGui::Button("", size);
        if (icon) {
            center = (ImGui::GetItemRectMin() + ImGui::GetItemRectMax()) / 2;
            center.y += 0.5;
            uv0 = get_icon_uv(icon);
            uv1 = uv0 + ImVec2(1. / 8, 1. / 8);
            window->DrawList->AddImage((intptr_t)g_tex_icons->tex,
                                       center - ImVec2(icon_height/2, icon_height/2),
                                       center + ImVec2(icon_height/2, icon_height/2),
                                       uv0, uv1, get_icon_color(icon, *v));
        }
    } else {
        ret = ImGui::Button(label, size);
    }
    ImGui::PopStyleColor(3);
    if (ret) *v = !*v;
    if (tooltip && ImGui::IsItemHovered()) {
        gui_tooltip(tooltip);
        goxel_set_help_text(tooltip);
    }
    ImGui::PopID();
    if (gui->is_row) ImGui::SameLine();

    if (ret) on_click();
    return ret;
}

bool gui_selectable(const char *name, bool *v, const char *tooltip, float w)
{
    return _selectable(name, v, tooltip, w, -1, false);
}

bool gui_condensed_selectable(const char *name, bool *v, const char *tooltip, float w)
{
    return _selectable(name, v, tooltip, w, -1, true);
}

bool gui_selectable_toggle(const char *name, int *v, int set_v,
                           const char *tooltip, float w)
{
    bool b = *v == set_v;
    if (gui_selectable(name, &b, tooltip, w)) {
        if (b) *v = set_v;
        return true;
    }
    return false;
}

void gui_layer_target_picker(layer_target_t *target)
{
    bool has_layer;
    int mode;

    if (!target) return;

    has_layer = goxel.image && goxel.image->active_layer;
    if (!has_layer && *target != LAYER_TARGET_NEW_LAYER)
        *target = LAYER_TARGET_NEW_LAYER;

    mode = (int)*target;
    gui_row_begin(3);
    gui_selectable_toggle("New layer", &mode, LAYER_TARGET_NEW_LAYER,
        "Create a new top-level layer.",
        -1);
    gui_enabled_begin(has_layer);
    gui_selectable_toggle("New child", &mode, LAYER_TARGET_NEW_CHILD,
        "Create a child layer under the selected layer.",
        -1);
    gui_alert_if_disabled_clicked(has_layer, "No layer selected",
                                  "Select a layer first.");
    gui_selectable_toggle("Replace current", &mode, LAYER_TARGET_REPLACE,
        "Write into the selected layer.",
        -1);
    gui_alert_if_disabled_clicked(has_layer, "No layer selected",
                                  "Select a layer first.");
    gui_enabled_end();
    gui_row_end();
    *target = (layer_target_t)mode;
}

bool gui_selectable_icon(const char *name, bool *v, int icon)
{
    return _selectable(name, v, NULL, 0, icon, false);
}

bool gui_condensed_selectable_icon(const char *name, bool *v, int icon)
{
    return _selectable(name, v, NULL, 0, icon, true);
}

void gui_text(const char *label, ...)
{
    va_list args;
    va_start(args, label);
    ImGui::TextV(label, args);
    va_end(args);
}

void gui_text_wrapped(const char *label, ...)
{
    va_list args;
    ImGui::PushTextWrapPos(0);
    va_start(args, label);
    ImGui::TextV(label, args);
    va_end(args);
    ImGui::PopTextWrapPos();
}

void gui_dummy(int w, int h)
{
    ImGui::Dummy(ImVec2(w, h));
}

bool gui_remaining_space_clicked(void)
{
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 1.f) avail.x = 1.f;
    if (avail.y < 1.f) avail.y = 1.f;
    ImGui::InvisibleButton("##remaining_space", avail);
    return ImGui::IsItemClicked(ImGuiMouseButton_Left);
}

void gui_image_gl_subrect(
        uint32_t gl_tex, int tex_w, int tex_h, int img_w, int img_h,
        float display_w, float display_h)
{
    ImVec2 uv0(0, 0);
    ImVec2 uv1(1, 1);
    if (tex_w > 0 && tex_h > 0) {
        uv1.x = (float)img_w / (float)tex_w;
        uv1.y = (float)img_h / (float)tex_h;
    }
    ImGui::Image((ImTextureID)(intptr_t)gl_tex, ImVec2(display_w, display_h),
                 uv0, uv1);
}

float gui_row_cell_width(void)
{
    if (!gui)
        return 0.f;
    if (gui->item_size > 0.f)
        return gui->item_size;
    return ImGui::GetContentRegionAvail().x;
}

static const char *utf8_prev(const char *start, const char *p)
{
    while (p > start) {
        p--;
        if (((unsigned char)*p & 0xC0) != 0x80)
            return p;
    }
    return start;
}

static float placer_past_text_span_px(ImFont *font, float font_px,
        const char *text_begin, const char *text_end)
{
    if (font && font_px > 0.f)
        return font->CalcTextSizeA(
                font_px, FLT_MAX, -1.0f, text_begin, text_end, NULL).x;
    return ImGui::CalcTextSize(text_begin, text_end, true).x;
}

/* File name: up to 2 lines, break by UTF-8 codepoint; add ellipsis on line2 if
 * the remainder is too long. Buffers nul-terminated, truncated if needed. */
static void placer_past_name_two_lines(
        const char *name, float max_w, char *l1, int l1n, char *l2, int l2n,
        ImFont *font, float font_px)
{
    l1[0] = 0;
    l2[0] = 0;
    if (!name || l1n < 2 || l2n < 2)
        return;
    const char *e = name + strlen(name);
    if (e <= name)
        return;
    // Line 1: longest prefix with width <= max_w
    const char *p = name;
    const char *l1e = name;
    while (p < e) {
        unsigned c;
        int n = ImTextCharFromUtf8(&c, p, e);
        if (n < 1)
            break;
        if (placer_past_text_span_px(font, font_px, name, p + n) > max_w)
            break;
        l1e = p + n;
        p += n;
    }
    if (l1e == name) {
        unsigned c;
        int n = ImTextCharFromUtf8(&c, p, e);
        if (n < 1)
            return;
        l1e = p + n;
    }
    {
        int n1 = (int)(l1e - name);
        if (n1 >= l1n)
            n1 = l1n - 1;
        memcpy(l1, name, (size_t)n1);
        l1[n1] = 0;
    }
    if (l1e >= e)
        return;
    // Line 2: remainder, or prefix + "…" if it does not fit
    const char *s2 = l1e;
    static const char ell[] = "\xE2\x80\xA6";
    const char *ell_end = ell + strlen(ell);
    const float ell_w = placer_past_text_span_px(font, font_px, ell, ell_end);
    p = s2;
    const char *l2e = s2;
    while (p < e) {
        unsigned c;
        int n = ImTextCharFromUtf8(&c, p, e);
        if (n < 1)
            break;
        if (placer_past_text_span_px(font, font_px, s2, p + n) > max_w)
            break;
        l2e = p + n;
        p += n;
    }
    if (l2e == s2 && s2 < e) {
        unsigned c;
        int n = ImTextCharFromUtf8(&c, s2, e);
        if (n < 1) {
            l2[0] = 0;
            return;
        }
        l2e = s2 + n;
    }
    if (l2e < e) {
        while (l2e > s2) {
            if (placer_past_text_span_px(font, font_px, s2, l2e) + ell_w
                    <= max_w) {
                (void)snprintf(
                        l2, (size_t)l2n, "%.*s%s", (int)(l2e - s2), s2, ell);
                return;
            }
            l2e = utf8_prev(s2, l2e);
        }
        (void)snprintf(l2, (size_t)l2n, "%s", ell);
        return;
    }
    {
        int n2 = (int)(l2e - s2);
        if (n2 >= l2n)
            n2 = l2n - 1;
        memcpy(l2, s2, (size_t)n2);
        l2[n2] = 0;
    }
}

bool gui_placer_past_entry(
        uint32_t gl_tex, int tex_w, int tex_h, int img_w, int img_h,
        const char *file_name, const char *path_tooltip, bool *out_remove,
        float cell_w, float label_font_scale)
{
    bool load = false;
    bool remove_btn = false;
    float scale = label_font_scale > 0.f ? label_font_scale : 1.f;
    ImFont *font = ImGui::GetFont();
    float label_font_px = ImGui::GetFontSize() * scale;
    float line_h = ImGui::GetTextLineHeight() * scale;

    if (out_remove)
        *out_remove = false;
    if (!gui)
        return false;

    float s = cell_w;
    if (s <= 0.f)
        s = gui->item_size;
    if (s <= 0.f)
        s = ImGui::GetContentRegionAvail().x;
    if (s < 1.f)
        s = 1.f;

    const float xpad = 3.f;
    const float ypad = 2.f;
    const ImVec2 p0 = ImGui::GetCursorPos();

    ImVec2 uv0(0, 0);
    ImVec2 uv1(1, 1);
    if (tex_w > 0 && tex_h > 0) {
        uv1.x = (float)img_w / (float)tex_w;
        uv1.y = (float)img_h / (float)tex_h;
    }

    ImGui::BeginGroup();

    if (gl_tex) {
        ImGui::Image(
                (ImTextureID)(intptr_t)gl_tex, ImVec2(s, s), uv0, uv1);
    } else {
        ImGui::Dummy(ImVec2(s, s));
        ImVec2 a = ImGui::GetItemRectMin();
        ImVec2 b = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddRectFilled(
                a, b, IM_COL32(32, 32, 32, 255));
    }

    ImGui::SetCursorPos(p0);
    /* Let the remove button receive clicks on top of the full-cell hit box. */
    ImGui::SetNextItemAllowOverlap();
    load = ImGui::InvisibleButton("##pl_load", ImVec2(s, s));
    ImVec2 rmin = ImGui::GetItemRectMin();
    ImVec2 rmax = ImGui::GetItemRectMax();
    const bool load_area_hovered = ImGui::IsItemHovered();

    const float xbtn = ImGui::GetFrameHeight() * 0.7f;
    ImGui::SetCursorPos(
            ImVec2(p0.x + s - xbtn - xpad, p0.y + ypad * 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 0));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.2f, 0.2f, 0.9f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.35f, 0.15f, 0.15f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.5f, 0.1f, 0.1f, 1.f));
    remove_btn = ImGui::SmallButton("x##remove");
    const bool remove_hovered = ImGui::IsItemHovered();
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(1);
    if (remove_btn) {
        load = false;
        if (out_remove)
            *out_remove = true;
    }

    char b1[256], b2[256];
    placer_past_name_two_lines(
            file_name, s - 2.f * xpad, b1, (int)sizeof(b1), b2, (int)sizeof(b2),
            font, label_font_px);
    const float bar_h = (b2[0] != 0) ? (2.f * line_h + ypad * 2.f)
                                     : (line_h + ypad * 2.f);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImU32 bar_col = IM_COL32(0, 0, 0, 150);
    dl->AddRectFilled(
            ImVec2(rmin.x, rmax.y - bar_h), rmax, bar_col);
    {
        const ImU32 tcol = IM_COL32(255, 255, 255, 255);
        const ImU32 sh = IM_COL32(0, 0, 0, 220);
        const float ty = rmax.y - bar_h + ypad * 0.5f;
        ImVec2 t1 = ImVec2(rmin.x + xpad, ty);
        static const int s_off[8][2] = {
            {-1, -1},
            {0, -1},
            {1, -1},
            {-1, 0},
            {1, 0},
            {-1, 1},
            {0, 1},
            {1, 1},
        };
        for (int o = 0; o < 8; o++) {
            dl->AddText(
                    font,
                    label_font_px,
                    ImVec2(
                            t1.x + (float)s_off[o][0],
                            t1.y + (float)s_off[o][1]),
                    sh,
                    b1,
                    NULL);
        }
        dl->AddText(font, label_font_px, t1, tcol, b1, NULL);
        if (b2[0]) {
            ImVec2 t2 = ImVec2(rmin.x + xpad, ty + line_h);
            for (int o = 0; o < 8; o++) {
                dl->AddText(
                        font,
                        label_font_px,
                        ImVec2(
                                t2.x + (float)s_off[o][0],
                                t2.y + (float)s_off[o][1]),
                        sh,
                        b2,
                        NULL);
            }
            dl->AddText(font, label_font_px, t2, tcol, b2, NULL);
        }
    }

    ImGui::EndGroup();

    if (!gui->scrolling) {
        if (path_tooltip && remove_hovered)
            gui_tooltip("Remove model from placer history");
        else if (path_tooltip && load_area_hovered)
            gui_tooltip(path_tooltip);
    }

    return load;
}

bool gui_placer_past_details_row(
        const char *file_name, const char *path_tooltip, bool *out_remove)
{
    bool load = false;
    bool rm = false;

    if (out_remove)
        *out_remove = false;
    if (!gui)
        return false;

    ImGuiStyle &st = ImGui::GetStyle();
    const float xbtn = ImGui::GetFrameHeight() * 1.05f;
    float avail = ImGui::GetContentRegionAvail().x;
    float w_name = avail - xbtn - st.ItemSpacing.x;
    if (w_name < 40.f)
        w_name = 40.f;

    ImGui::PushStyleColor(ImGuiCol_Button, COLOR(BUTTON, INNER, false));
    if (ImGui::Button(file_name && file_name[0] ? file_name : "-",
                    ImVec2(w_name, 0)))
        load = true;
    ImGui::PopStyleColor();
    if (!gui->scrolling && path_tooltip && ImGui::IsItemHovered())
        gui_tooltip(path_tooltip);

    ImGui::SameLine(0, st.ItemSpacing.x);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 0));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.2f, 0.2f, 0.9f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.35f, 0.15f, 0.15f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.5f, 0.1f, 0.1f, 1.f));
    rm = ImGui::SmallButton("x##pl_hist_rm");
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(1);
    if (!gui->scrolling && ImGui::IsItemHovered())
        gui_tooltip("Remove model from placer history");

    if (rm) {
        load = false;
        if (out_remove)
            *out_remove = true;
    }
    if (load || rm)
        on_click();
    return load;
}

bool gui_texture_swatch_entry(
        const char *id, uint32_t gl_tex, int tex_w, int tex_h, int img_w, int img_h,
        const char *label, bool selected, float cell_w)
{
    bool clicked;
    ImVec2 uv0(0, 0);
    ImVec2 uv1(1, 1);
    const float s = cell_w > 0.f ? cell_w : 64.f;
    ImDrawList *dl;
    ImVec2 mn, mx;

    if (!gui)
        return false;

    if (tex_w > 0 && tex_h > 0) {
        uv1.x = (float)img_w / (float)tex_w;
        uv1.y = (float)img_h / (float)tex_h;
    }

    ImGui::BeginGroup();
    if (gl_tex) {
        ImGui::Image((ImTextureID)(intptr_t)gl_tex, ImVec2(s, s), uv0, uv1);
    } else {
        ImGui::Dummy(ImVec2(s, s));
    }

    mn = ImGui::GetItemRectMin();
    mx = ImGui::GetItemRectMax();
    dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(mn, mx, IM_COL32(30, 30, 30, 255));
    if (gl_tex)
        dl->AddImage((ImTextureID)(intptr_t)gl_tex, mn, mx, uv0, uv1);
    dl->AddRect(mn, mx, selected ? IM_COL32(255, 200, 80, 255)
                                 : IM_COL32(90, 90, 90, 255),
                0.f, 0, selected ? 2.f : 1.f);

    ImGui::SetCursorScreenPos(mn);
    clicked = ImGui::InvisibleButton(id, ImVec2(s, s));
    if (!gui->scrolling && ImGui::IsItemHovered() && label && *label)
        gui_tooltip(label);

    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + s);
    ImGui::TextUnformatted(label && *label ? label : "Unnamed");
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    return clicked;
}

void gui_same_line(void)
{
    ImGui::SameLine();
}

void gui_spacing(int w)
{
    ImGui::Dummy(ImVec2(w, 0));
    ImGui::SameLine();
}

void gui_spacing_f(float w)
{
    ImGui::Dummy(ImVec2(w, 0));
    ImGui::SameLine();
}

float gui_frame_height(void)
{
    return ImGui::GetFrameHeight();
}

static bool color_picker(const char *label, uint8_t color[4], bool show_diff)
{
    float colorf[4] = {color[0] / 255.f,
                       color[1] / 255.f,
                       color[2] / 255.f,
                       color[3] / 255.f};
    static uint8_t backup_color[4];
    bool ret;

    if (ImGui::IsWindowAppearing())
        memcpy(backup_color, color, sizeof(backup_color));
    ret = ImGui::ColorPicker4(label, colorf,
            ImGuiColorEditFlags_NoSidePreview |
            ImGuiColorEditFlags_NoSmallPreview);
    if (ret) {
        color[0] = colorf[0] * 255;
        color[1] = colorf[1] * 255;
        color[2] = colorf[2] * 255;
        color[3] = colorf[3] * 255;
    }
    if (show_diff) {
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::Text("Current");
        ImGui::ColorButton("##current", color,
                ImGuiColorEditFlags_NoPicker, ImVec2(60, 40));
        ImGui::Text("Original");
        if (ImGui::ColorButton("##previous", backup_color,
                    ImGuiColorEditFlags_NoPicker, ImVec2(60, 40))) {
            memcpy(color, backup_color, sizeof(backup_color));
            ret = true;
        }
        ImGui::EndGroup();
    }

    return ret;
}

bool gui_color(const char *label, uint8_t color[4])
{
    bool ret = false;
    ImVec2 size(ICON_HEIGHT, ICON_HEIGHT);

    ImGui::PushID(label);
    if (ImGui::ColorButton(label, color, 0, size)) {
        ImGui::OpenPopup("GoxelPicker");
    }

    if (ImGui::BeginPopupContextItem("GoxelPicker")) {
        if (color_picker(label, color, true)) {
            ret = true;
        }
        ImGui::EndPopup();
    }

    ImGui::PopID();
    return ret;
}

void gui_palette_mode_swatch(const char *label)
{
    ImDrawList *draw_list = ImGui::GetWindowDrawList();
    ImVec2 size(ICON_HEIGHT, ICON_HEIGHT);
    ImVec2 p0, p1, center;
    ImVec2 uv0, uv1;
    const float icon_half = 12.f;
    /* Hue stops across the square for a rainbow fill. */
    static const ImU32 stops[] = {
        IM_COL32(255, 0, 0, 255),
        IM_COL32(255, 165, 0, 255),
        IM_COL32(255, 255, 0, 255),
        IM_COL32(0, 200, 0, 255),
        IM_COL32(0, 180, 255, 255),
        IM_COL32(80, 0, 255, 255),
        IM_COL32(200, 0, 200, 255),
    };
    const int nstops = (int)(sizeof(stops) / sizeof(stops[0]));
    int i;

    ImGui::PushID(label ? label : "##palette_mode");
    ImGui::InvisibleButton("##rainbow", size);
    p0 = ImGui::GetItemRectMin();
    p1 = ImGui::GetItemRectMax();
    for (i = 0; i < nstops - 1; i++) {
        float t0 = (float)i / (float)(nstops - 1);
        float t1 = (float)(i + 1) / (float)(nstops - 1);
        ImVec2 a(p0.x + (p1.x - p0.x) * t0, p0.y);
        ImVec2 b(p0.x + (p1.x - p0.x) * t1, p1.y);
        draw_list->AddRectFilled(a, b, stops[i]);
    }
    draw_list->AddRect(p0, p1, IM_COL32(0, 0, 0, 255), 0, 0, 1.f);
    center = ImVec2((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
    uv0 = get_icon_uv(ICON_PALETTE);
    uv1 = uv0 + ImVec2(1.f / 8.f, 1.f / 8.f);
    /* Soft dark disc behind icon for contrast on bright stripes. */
    draw_list->AddCircleFilled(center, icon_half + 2.f, IM_COL32(0, 0, 0, 140));
    draw_list->AddImage((intptr_t)g_tex_icons->tex,
                        center - ImVec2(icon_half, icon_half),
                        center + ImVec2(icon_half, icon_half),
                        uv0, uv1, IM_COL32(255, 255, 255, 255));
    if (ImGui::IsItemHovered())
        gui_tooltip("Palette brush (multi-colour)");
    ImGui::PopID();
}
bool gui_color_small_conditional_label(const char *label, uint8_t color[4], bool show_label)
{
    bool ret;
    float colorf[4] = {color[0] / 255.f,
                       color[1] / 255.f,
                       color[2] / 255.f,
                       color[3] / 255.f};
    
    ImGui::PushID(label);
    if (show_label) {
        label_aligned(label, gui_label_size_get());
    }
    ret = ImGui::ColorEdit4("", colorf, ImGuiColorEditFlags_NoInputs);
    ImGui::PopID();
    if (ret) {
        color[0] = colorf[0] * 255;
        color[1] = colorf[1] * 255;
        color[2] = colorf[2] * 255;
        color[3] = colorf[3] * 255;
    }
    return ret;
}

bool gui_color_small(const char *label, uint8_t color[4]) {
    return gui_color_small_conditional_label(label, color, true);
}
bool gui_color_small_no_label(const char *id, uint8_t color[4]) {
    return gui_color_small_conditional_label(id, color, false);
}

bool gui_color_inline(const char *label, uint8_t color[4]) {
    return color_picker(label, color, false);
}

bool gui_color_small_f3(const char *label, float color[3])
{
    uint8_t c[4];
    bool ret;
    rgb_to_srgb8(color, c);
    c[3] = 255;
    ret = gui_color_small(label, c);
    srgb8_to_rgb(c, color);
    return ret;
}

bool gui_color_opacity(uint8_t color[4]) {
    int opacity = roundf((color[3] / 255.) * 100);
    bool ret = gui_input_int("Opacity %", &opacity, 0, 100);
    if (ret) {
        color[3] = roundf((opacity / 100.0f) * 255.0f);
    }
    return ret;
}

int gui_color_swatch(const char *id, const uint8_t color[4], float size) {
    /* ColorButton() only uses ButtonBehavior for the left button; RMB does
     * not register on the item, so we use InvisibleButton with L+R. */
    ImVec4 c((float)color[0] / 255.f, (float)color[1] / 255.f,
               (float)color[2] / 255.f, (float)color[3] / 255.f);
    ImGui::PushID(id);
    const ImVec2 s(size, size);
    ImGui::InvisibleButton("##sw", s, ImGuiButtonFlags_MouseButtonLeft |
                                     ImGuiButtonFlags_MouseButtonRight);
    const ImVec2 a = ImGui::GetItemRectMin();
    const ImVec2 b = ImGui::GetItemRectMax();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImGuiContext &g = *GImGui;
    const float grid_step = ImMin(s.x, s.y) / 2.99f;
    const float rounding = ImMin(g.Style.FrameRounding, grid_step * 0.5f);
    const float off = -0.75f;
    const ImU32 col_u32 = ImGui::GetColorU32(c);
    if (c.w < 1.0f) {
        ImGui::RenderColorRectWithAlphaCheckerboard(
            dl, a, b, col_u32, grid_step, ImVec2(off, off), rounding);
    } else {
        dl->AddRectFilled(a, b, col_u32, rounding);
    }
    if (g.Style.FrameBorderSize > 0.0f) {
        dl->AddRect(a, b, ImGui::GetColorU32(ImGuiCol_Border), rounding, 0,
                    g.Style.FrameBorderSize);
    } else {
        dl->AddRect(a, b, ImGui::GetColorU32(ImGuiCol_FrameBg), rounding);
    }
    int ret = 0;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
        ret = 1;
    else if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
        ret = 2;
    ImGui::PopID();
    return ret;
}

bool gui_checkbox(const char *label, bool *v, const char *hint)
{
    bool ret;
    label_aligned("", gui_label_size_get());
    ImGui::PushStyleColor(ImGuiCol_FrameBg, COLOR(CHECKBOX, INNER, false));
    ImGui::PushStyleColor(ImGuiCol_CheckMark, COLOR(CHECKBOX, ITEM, false));
    ret = ImGui::Checkbox(label, v);
    if (hint && ImGui::IsItemHovered()) gui_tooltip(hint);
    if (ret) on_click();
    ImGui::PopStyleColor(2);
    return ret;
}

void gui_tooltip_if_hovered(const char *info) {
    if (info && ImGui::IsItemHovered()) gui_tooltip(info);
}

bool gui_checkbox_flag(const char *label, int *v, int flag, const char *hint)
{
    bool ret, b;
    b = (*v) & flag;
    ret = gui_checkbox(label, &b, hint);
    if (ret) {
        if (b) *v |= flag;
        else   *v &= ~flag;
    }
    return ret;
}


static bool gui_button_ex(const char *label, float size, int icon, bool primary)
{
    bool ret;
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 uv0, uv1;
    ImVec2 button_size;
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec2 center;
    ImVec4 bg;
    int w, isize;
    int ncolors = 0;
    bool is_text = label && label[0] != '#';

    button_size = ImVec2(size * ImGui::GetContentRegionAvail().x, ITEM_HEIGHT);
    if (size == -1) button_size.x = ImGui::GetContentRegionAvail().x;
    if (size == 0 && (label == NULL || label[0] == '#')) {
        button_size.x = ICON_HEIGHT;
        button_size.y = ICON_HEIGHT;
    }
    if (size == 0 && label && label[0] != '#') {
        w = ImGui::CalcTextSize(label, NULL, true).x + style.FramePadding.x * 2;
        if (w < ITEM_HEIGHT)
            button_size.x = ITEM_HEIGHT;
    }

    if (gui->item_size) button_size.x = gui->item_size;

    isize = is_text ? 12 : 16;
    if (primary && is_text) {
        bg = COLOR(BUTTON, INNER, true);
        ImGui::PushStyleColor(ImGuiCol_Button, bg);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, color_lighten(bg));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, color_lighten2(bg));
        ImGui::PushStyleColor(ImGuiCol_Text, COLOR(BUTTON, TEXT, true));
        ncolors = 4;
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button,
                is_text ? COLOR(BUTTON, INNER, false) : COLOR(ICON, INNER, false));
        ncolors = 1;
    }
    ret = ImGui::Button(label ?: "", button_size);
    ImGui::PopStyleColor(ncolors);
    if (icon) {
        center = ImGui::GetItemRectMin() +
            ImVec2(ImGui::GetItemRectSize().y / 2,
                   ImGui::GetItemRectSize().y / 2);
        uv0 = ImVec2(((icon - 1) % 8) / 8.0, ((icon - 1) / 8) / 8.0);
        uv1 = ImVec2(uv0.x + 1. / 8, uv0.y + 1. / 8);
        draw_list->AddImage((intptr_t)g_tex_icons->tex,
                            center - ImVec2(isize, isize),
                            center + ImVec2(isize, isize),
                            uv0, uv1, get_icon_color(icon, 0));
    }
    if (ret) on_click();
    if (gui->is_row) ImGui::SameLine();
    return ret;
}

bool gui_button(const char *label, float size, int icon)
{
    return gui_button_ex(label, size, icon, false);
}

bool gui_button_primary(const char *label, float size, int icon)
{
    return gui_button_ex(label, size, icon, true);
}

bool gui_button_right(const char *label, int icon)
{
    const ImGuiStyle& style = ImGui::GetStyle();
    float text_size = ImGui::CalcTextSize(label).x;
    float w = text_size + 2 * style.FramePadding.x;
    w = max(w, ITEM_HEIGHT);
    w += style.FramePadding.x;
    ImGui::SameLine();
    ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x - w, 0));
    ImGui::SameLine();
    return gui_button(label, 0, icon);
}

bool gui_open_in_shell(const char *path)
{
    ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
    if (!path || !path[0] || !platform_io.Platform_OpenInShellFn)
        return false;
    return platform_io.Platform_OpenInShellFn(GImGui, path);
}

bool gui_input_text(const char *label, char *txt, int size)
{
    bool ret;

    if (!label || label[0] == '\0' || label[0] == '#')
        return ImGui::InputText(label, txt, size);

    ImGui::PushID(label);
    label_aligned(label, gui_label_size_get());
    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
    ret = ImGui::InputText("##input", txt, size);
    ImGui::PopItemWidth();
    ImGui::PopID();
    return ret;
}

bool gui_input_text_row(const char *label, char *buf, int size,
                        float width, float height)
{
    ImGuiStyle &style = ImGui::GetStyle();
    float font_size = ImGui::GetFontSize();
    float h = (height > 0.f) ? height
                             : (font_size + style.FramePadding.y * 2.f);
    float pad_y = ImMax(0.f, (h - font_size) * 0.5f);
    bool ret;

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(style.FramePadding.x, pad_y));
    if (width > 0.f)
        ImGui::SetNextItemWidth(width);
    ret = ImGui::InputText(label ? label : "##input", buf, (size_t)size,
                           ImGuiInputTextFlags_AutoSelectAll);
    ImGui::PopStyleVar();
    return ret;
}

bool gui_input_text_multiline(const char *label, char *buf, int size,
                              float width, float height)
{
    // We set the frame color to a semi transparent value, because otherwise
    // we cannot render the error highlight.
    // XXX: fix that.
    bool ret;
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4 col = style.Colors[ImGuiCol_FrameBg];
    style.Colors[ImGuiCol_FrameBg].w = 0.5;
    ret = ImGui::InputTextMultiline(label, buf, size, ImVec2(width, height));
    style.Colors[ImGuiCol_FrameBg] = col;
    return ret;
}

bool gui_combo(const char *label, int *v, const char **names, int nb)
{
    bool ret;
    bool has_label = label && label[0] != '\0' && label[0] != '#';

    ImGui::PushStyleColor(ImGuiCol_FrameBg, COLOR(COMBO, INNER, 0));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, COLOR(COMBO, BACKGROUND, 0));
    ImGui::PushStyleColor(ImGuiCol_Button, COLOR(COMBO, ITEM, 0));
    if (has_label) {
        ImGui::PushID(label);
        label_aligned(label, gui_label_size_get());
        ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
        ret = ImGui::Combo("##combo", v, names, nb);
        ImGui::PopItemWidth();
        ImGui::PopID();
    } else {
        ImGui::PushItemWidth(gui_row_cell_width());
        ret = ImGui::Combo(label, v, names, nb);
        ImGui::PopItemWidth();
    }
    ImGui::PopStyleColor(3);
    return ret;
}

/* Reset each combo so duplicate labels (e.g. two layers named "Layer")
 * still get distinct ImGui IDs. */
static int g_combo_item_idx;

bool gui_combo_begin(const char *label, const char *preview)
{
    bool ret;
    bool has_label = label && label[0] != '\0' && label[0] != '#';
    const char *combo_id;

    g_combo_item_idx = 0;
    ImGui::PushID(label ? label : "combo");
    ImGui::PushStyleColor(ImGuiCol_FrameBg, COLOR(COMBO, INNER, 0));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, COLOR(COMBO, BACKGROUND, 0));
    ImGui::PushStyleColor(ImGuiCol_Button, COLOR(COMBO, ITEM, 0));
    if (has_label) {
        label_aligned(label, gui_label_size_get());
        combo_id = "##combo";
    } else {
        combo_id = label;
    }
    ImGui::PushItemWidth(has_label ?
                         ImGui::GetContentRegionAvail().x : -1);
    ret = ImGui::BeginCombo(combo_id, preview);

    if (!ret) {
        ImGui::PopItemWidth();
        ImGui::PopStyleColor(3);
        ImGui::PopID();
    }
    return ret;
}

void gui_combo_end(void)
{
    ImGui::EndCombo();
    ImGui::PopStyleColor(3);
    ImGui::PopItemWidth();
    ImGui::PopID();
}

bool gui_combo_item(const char *label, bool is_selected)
{
    bool ret;
    ImGui::PushID(g_combo_item_idx++);
    ret = ImGui::Selectable(label, is_selected);
    if (is_selected)
        ImGui::SetItemDefaultFocus();
    ImGui::PopID();
    return ret;
}

void gui_combo_separator(void)
{
    ImGui::Separator();
}

void gui_input_text_multiline_highlight(int line)
{
    float h = ImGui::CalcTextSize("").y;
    ImVec2 rmin = ImGui::GetItemRectMin();
    ImVec2 rmax = ImGui::GetItemRectMax();
    rmin.y = rmin.y + line * h + 2;
    rmax.y = rmin.y + h;
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(rmin, rmax, 0xff0000ff);
}

void gui_enabled_begin(bool enabled)
{
    /* Dim alone left widgets clickable; BeginDisabled also blocks input. */
    ImGui::BeginDisabled(!enabled);
}

void gui_enabled_end(void)
{
    ImGui::EndDisabled();
}

bool gui_alert_if_disabled_clicked(bool enabled,
                                   const char *title, const char *msg)
{
    if (enabled) return false;
    if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        return false;
    if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        return false;
    gui_alert(title, msg);
    return true;
}

bool gui_quat(const char *label, float q[4])
{
    // Hack to prevent weird behavior when we change the euler angles.
    // We keep track of the last used euler angles value and reuse them if
    // the quaternion is the same.
    static struct {
        float quat[4];
        float eul[3];
    } last = {};
    float eul[3];
    bool ret = false;

    if (memcmp(q, &last.quat, sizeof(last.quat)) == 0)
        vec3_copy(last.eul, eul);
    else
        quat_to_eul(q, EULER_ORDER_DEFAULT, eul);
    gui_group_begin(label);
    if (gui_angle("x", &eul[0], -180, +180)) ret = true;
    if (gui_angle("y", &eul[1], -180, +180)) ret = true;
    if (gui_angle("z", &eul[2], -180, +180)) ret = true;
    gui_group_end();

    if (ret) {
        eul_to_quat(eul, EULER_ORDER_DEFAULT, q);
        quat_copy(q, last.quat);
        vec3_copy(eul, last.eul);
    }
    return ret;
}

void gui_open_popup(const char *title, int flags, void *data,
                    int (*func)(void *data))
{
    typeof(gui->popup[0]) *popup;
    popup = &gui->popup[gui->popup_count++];
    popup->title = title;
    popup->func = func;
    popup->flags = flags;
    assert(!popup->data);
    popup->data = data;
    popup->size[0] = popup->size[1] = 0.f;
}

void gui_open_popup_sized(const char *title, float w, float h, int flags,
                          void *data, int (*func)(void *data))
{
    typeof(gui->popup[0]) *popup;
    gui_open_popup(title, flags, data, func);
    popup = &gui->popup[gui->popup_count - 1];
    popup->size[0] = w;
    popup->size[1] = h;
}

void gui_on_popup_closed(void (*func)(int))
{
    gui->popup[gui->popup_count - 1].on_closed = func;
}

void gui_popup_bottom_begin(void)
{
    float w = ImGui::GetContentRegionAvail().y -
              ImGui::GetFrameHeightWithSpacing();
    ImGui::Dummy(ImVec2(0, w));
    gui_row_begin(0);
}

void gui_popup_bottom_end(void)
{
    gui_row_end();
}

void gui_alert(const char *title, const char *msg)
{
    gui_open_popup(title, 0, msg ? strdup(msg) : NULL, alert_popup);
}

bool gui_collapsing_header(const char *label, bool default_opened)
{
    if (default_opened)
        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    return ImGui::CollapsingHeader(label);
}

bool gui_collapsing_header_force_open(const char *label, bool force_open)
{
    if (force_open)
        ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    return ImGui::CollapsingHeader(label);
}

void gui_columns(int count)
{
    ImGui::Columns(count);
}

void gui_next_column(void)
{
    ImGui::NextColumn();
}

void gui_separator(void)
{
    ImGui::Separator();
}

void gui_push_id(const char *id)
{
    ImGui::PushID(id);
}

void gui_pop_id(void)
{
    ImGui::PopID();
}

void gui_request_panel_width(float width)
{
    goxel.gui.panel_width = width;
}

bool _model_item(int idx, bool *selected, const char *name, int len)
{
    bool ret = false;

    ImGui::PushID(idx);
    ImGui::PushStyleColor(ImGuiCol_Button, COLOR(WIDGET, INNER, *selected));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          color_lighten(COLOR(WIDGET, INNER, *selected)));

    if (ImGui::Button(name, ImVec2(-1, GUI_ICON_HEIGHT)))
    {
        *selected = true;
        ret = true;
    }
    ImGui::PopStyleColor(2);
    ImGui::PopID();
    return ret;
}


static char *g_layer_edit_name = NULL;
static bool g_layer_start_edit = false;

bool _layer_item(int idx, int icons_count, const int *icons,
                    bool *visible, bool *selected,
                    char *name, int len, bool condensed, float trailing_w,
                    bool allow_deselect, bool solo_active, bool *solo_pressed,
                    bool reserve_visibility_space, bool reserve_solo_space,
                    bool selectable, bool *name_double_clicked)
{
    bool ret = false;
    bool selected_ = *selected;
    bool highlighted = selectable && *selected;
    int icon;
    int i;
    ImVec2 center;
    ImVec2 uv0, uv1;
    ImVec2 padding;
    ImDrawList *draw_list = ImGui::GetWindowDrawList();
    ImGuiStyle &style = ImGui::GetStyle();
    float btn_h = GUI_ICON_HEIGHT * (condensed ? CONDENSE_FACTOR : 1);
    float name_w;

    ImGui::PushID(idx);
    ImGui::PushStyleColor(ImGuiCol_Button, COLOR(WIDGET, INNER, highlighted));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          color_lighten(COLOR(WIDGET, INNER, highlighted)));
    if (visible)
    {
        bool visibility = condensed
            ? gui_condensed_selectable_icon("##visible", &selected_, *visible ? ICON_VISIBILITY : ICON_VISIBILITY_OFF)
            : gui_selectable_icon("##visible", &selected_, *visible ? ICON_VISIBILITY : ICON_VISIBILITY_OFF);
        if (visibility)
        {
            *visible = !*visible;
            ret = true;
        }
        ImGui::SameLine();
    }
    else if (reserve_visibility_space)
    {
        ImGui::InvisibleButton("##visible_reserve", ImVec2(btn_h, btn_h));
        ImGui::SameLine();
    }

    if (solo_pressed)
    {
        ImGui::PushStyleColor(ImGuiCol_Button,
                              solo_active ? COLOR(WIDGET, INNER, true)
                                          : COLOR(WIDGET, INNER, false));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              color_lighten(solo_active ? COLOR(WIDGET, INNER, true)
                                                        : COLOR(WIDGET, INNER, false)));
        if (ImGui::Button("S", ImVec2(btn_h, btn_h)))
        {
            *solo_pressed = true;
            ret = true;
        }
        if (ImGui::IsItemHovered())
            gui_tooltip("Solo - temp only show this item");
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
    }
    else if (reserve_solo_space)
    {
        ImGui::InvisibleButton("##solo_reserve", ImVec2(btn_h, btn_h));
        ImGui::SameLine();
    }

    if (g_layer_edit_name != name)
    {
        float icon_slot = btn_h * 0.75f;
        float icon_half = btn_h * (12.f / (float)GUI_ICON_HEIGHT);

        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0, 0.5));
        padding = style.FramePadding;
        padding.x += icon_slot * icons_count;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, padding);
        name_w = ImGui::GetContentRegionAvail().x - trailing_w;
        if (name_w < btn_h) name_w = btn_h;
        if (ImGui::Button(name, ImVec2(name_w, btn_h)))
        {
            if (selectable) {
                if (allow_deselect && *selected)
                    *selected = false;
                else
                    *selected = true;
                ret = true;
            }
        }
        ImGui::PopStyleVar();

        for (i = 0; i < icons_count; i++)
        {
            icon = icons[i];
            center = ImGui::GetItemRectMin() +
                     ImVec2(icon_slot * (i + 0.5f), btn_h * 0.5f);
            uv0 = ImVec2(((icon - 1) % 8) / 8.0, ((icon - 1) / 8) / 8.0);
            uv1 = ImVec2(uv0.x + 1. / 8, uv0.y + 1. / 8);
            draw_list->AddImage(
                    (intptr_t)g_tex_icons->tex,
                    center - ImVec2(icon_half, icon_half),
                    center + ImVec2(icon_half, icon_half),
                    uv0, uv1, get_icon_color(icon, 0));
        }
        ImGui::PopStyleVar();
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0))
        {
            if (name_double_clicked)
                *name_double_clicked = true;
            else {
                g_layer_edit_name = name;
                g_layer_start_edit = true;
            }
        }
    }
    else
    {
        name_w = ImGui::GetContentRegionAvail().x - trailing_w;
        if (name_w < btn_h) name_w = btn_h;
        if (g_layer_start_edit)
            ImGui::SetKeyboardFocusHere();
        gui_input_text_row("##name_edit", name, len, name_w, btn_h);
        if (!g_layer_start_edit && !ImGui::IsItemActive())
            g_layer_edit_name = NULL;
        g_layer_start_edit = false;
    }
    ImGui::PopStyleColor(2);
    ImGui::PopID();
    return ret;
}

float gui_icon_height(bool condensed)
{
    return ICON_HEIGHT * (condensed ? CONDENSE_FACTOR : 1.f);
}

bool gui_condensed_layer_item(int idx, int icons_count, const int *icons,
                    bool *visible, bool *selected,
                    char *name, int len)
{
    return _layer_item(idx, icons_count, icons, visible, selected, name, len,
                       true, 0, false, false, NULL, false, false, true, NULL);
}

bool gui_condensed_layer_item_trailing(int idx, int icons_count, const int *icons,
                    bool *visible, bool *selected,
                    char *name, int len, float trailing_w,
                    bool allow_deselect, bool solo_active, bool *solo_pressed,
                    bool reserve_visibility_space, bool reserve_solo_space,
                    bool selectable, bool *name_double_clicked)
{
    return _layer_item(idx, icons_count, icons, visible, selected, name, len,
                       true, trailing_w, allow_deselect, solo_active,
                       solo_pressed, reserve_visibility_space,
                       reserve_solo_space, selectable, name_double_clicked);
}

bool gui_layer_item(int idx, int icons_count, const int *icons,
                    bool *visible, bool *selected,
                    char *name, int len)
{
    return _layer_item(idx, icons_count, icons, visible, selected, name, len,
                       false, 0, false, false, NULL, false, false, true, NULL);
}

bool gui_is_key_down(int key)
{
    return ImGui::IsKeyDown((ImGuiKey)key);
}

void gui_item_group_begin(float pad_top)
{
    ImGui::BeginGroup();
    if (pad_top > 0.f)
        ImGui::Dummy(ImVec2(0.f, pad_top));
}

void gui_item_group_end(float pad_bottom)
{
    if (pad_bottom > 0.f)
        ImGui::Dummy(ImVec2(0.f, pad_bottom));
    ImGui::EndGroup();
}

bool gui_is_item_hovered(void)
{
    /* RectOnly: row pads stay hovered even when a DnD gap InvisibleButton
     * overlaps the seam between abutting groups. */
    return ImGui::IsItemHovered(ImGuiHoveredFlags_RectOnly);
}

bool gui_menu_bar_begin(void)
{
    bool ret;
    ImGui::PushStyleColor(ImGuiCol_MenuBarBg, COLOR(MENU, BACKGROUND, false));
    ImGui::PushStyleColor(ImGuiCol_Header,
            color_lighten(COLOR(MENU, BACKGROUND, false)));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
            color_lighten(COLOR(MENU, BACKGROUND, false)));
    ImGui::PushStyleColor(ImGuiCol_Text, COLOR(MENU, TEXT, false));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, COLOR(MENU, BACKGROUND, false));

    ret = ImGui::BeginMainMenuBar();
    if (!ret) {
        ImGui::PopStyleColor(5);
    }
    return ret;
}

/* Text in the menu bar cannot go through BeginMenu(): a menu whose label is
 * empty gets a selectable that spans the whole remaining bar width, which then
 * claims the hovered id and swallows clicks on anything right-aligned. */
void gui_menu_bar_text(const char *text)
{
    if (!text || !text[0]) return;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 24.0f);
    ImGui::BeginDisabled();
    ImGui::TextUnformatted(text);
    ImGui::EndDisabled();
}

static bool menu_bar_panel_toggle(const char *id, int icon, bool selected,
                                  const char *tooltip)
{
    ImGuiWindow *window = ImGui::GetCurrentWindow();
    const float size = 18.0f;
    const ImVec2 button_size(size, size);
    const ImVec2 uv0 = get_icon_uv(icon);
    const ImVec2 uv1 = uv0 + ImVec2(1.0f / 8.0f, 1.0f / 8.0f);

    ImGui::PushID(id);
    bool clicked = ImGui::InvisibleButton("##toggle", button_size);
    const ImVec2 a = ImGui::GetItemRectMin();
    const ImVec2 b = ImGui::GetItemRectMax();

    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        const ImVec4 bg = color_lighten(COLOR(MENU, BACKGROUND, false));
        window->DrawList->AddRectFilled(a, b, ImGui::GetColorU32(bg), 2.0f);
    }
    window->DrawList->AddImage(
            (intptr_t)g_tex_icons->tex, a, b, uv0, uv1,
            get_icon_color(icon, selected));
    if (selected) {
        window->DrawList->AddRect(
                a + ImVec2(0.5f, 0.5f), b - ImVec2(0.5f, 0.5f),
                get_icon_color(icon, true), 2.0f, 0, 1.5f);
    }
    if (ImGui::IsItemHovered())
        gui_tooltip(tooltip);
    ImGui::PopID();
    return clicked;
}

static void menu_bar_toggles_separator(float w, float h)
{
    ImGuiWindow *window = ImGui::GetCurrentWindow();
    ImGui::Dummy(ImVec2(w, h));
    const ImVec2 a = ImGui::GetItemRectMin();
    const ImVec2 b = ImGui::GetItemRectMax();
    const float x = ImFloor((a.x + b.x) * 0.5f) + 0.5f;
    window->DrawList->AddLine(ImVec2(x, a.y + 2.0f), ImVec2(x, b.y - 2.0f),
                              ImGui::GetColorU32(ImGuiCol_Separator));
}

void gui_menu_bar_panel_toggles(void)
{
    ImGuiWindow *window = ImGui::GetCurrentWindow();
    const float button_size = 18.0f;
    const float spacing = 3.0f;
    const float separator_w = 9.0f;
    const float total_w =
            button_size * 5.0f + spacing * 5.0f + separator_w;
    const float right_x =
            window->Size.x - ImGui::GetStyle().WindowPadding.x - total_w;
    const action_t *plane_action;
    char tooltip[128];

    ImGui::SetCursorPosX(ImMax(ImGui::GetCursorPosX(), right_x));

    const action_t *tools_action =
            action_get(ACTION_view_toggle_tools, true);
    snprintf(tooltip, sizeof(tooltip), "Tools (%s)",
             tools_action ? tools_action->shortcut : "");
    bool tools_open = gui_panel_is_detached(PANEL_TOOLS);
    if (menu_bar_panel_toggle(
                "menu_tools", ICON_TOOLS, tools_open, tooltip))
        action_exec(tools_action);

    ImGui::SameLine(0.0f, spacing);
    if (menu_bar_panel_toggle(
                "menu_layers", ICON_LAYERS, goxel.gui.layers_panel_open,
                "Layers"))
        gui_layers_panel_toggle();

    ImGui::SameLine(0.0f, spacing);
    if (menu_bar_panel_toggle(
                "menu_palette", ICON_PALETTE, goxel.gui.palette_win_open,
                "Palette"))
        gui_palette_window_toggle();

    ImGui::SameLine(0.0f, spacing);
    menu_bar_toggles_separator(separator_w, button_size);

    ImGui::SameLine(0.0f, spacing);
    if (menu_bar_panel_toggle(
                "menu_image_box", ICON_IMAGE, !goxel.hide_box,
                "Image box on/off")) {
        goxel.hide_box = !goxel.hide_box;
        settings_save();
    }

    ImGui::SameLine(0.0f, spacing);
    plane_action = action_get(ACTION_toggle_plane_visible, true);
    snprintf(tooltip, sizeof(tooltip), "Plane on/off (%s)",
             plane_action ? plane_action->shortcut : "");
    if (menu_bar_panel_toggle(
                "menu_plane", ICON_TOOL_PLANE, goxel.snap_mask & SNAP_PLANE,
                tooltip))
        action_exec(plane_action);
}

void gui_menu_bar_end(void)
{
    ImGui::PopStyleColor(5);
    ImGui::EndMainMenuBar();
}


bool gui_menu_begin(const char *label, bool enabled)
{
    return ImGui::BeginMenu(label, enabled);
}

static bool g_menu_checkbox_column = false;

void gui_menu_end(void)
{
    g_menu_checkbox_column = false;
    ImGui::EndMenu();
}

void gui_menu_checkbox_column(bool enabled)
{
    g_menu_checkbox_column = enabled;
}

/* Menu row with an optional 25px left checkbox column (ImGui's built-in
 * checkmark is on the right; we want toggles aligned on the left). */
static bool menu_item_checkbox_column(const char *label, const char *shortcut,
                                      bool is_toggle, bool checked, bool enabled)
{
    const float check_col_w = 25.0f;
    ImGuiWindow *window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
        return false;

    ImGuiContext& g = *GImGui;
    ImGuiStyle& style = g.Style;
    ImVec2 pos = window->DC.CursorPos;
    ImVec2 label_size = ImGui::CalcTextSize(label, NULL, true);
    float shortcut_w = (shortcut && shortcut[0]) ?
            ImGui::CalcTextSize(shortcut, NULL).x : 0.0f;
    float icon_w = check_col_w;
    float checkmark_w = 0.0f;
    float min_w = window->DC.MenuColumns.DeclColumns(
            icon_w, label_size.x, shortcut_w, checkmark_w);
    float stretch_w = ImMax(0.0f, ImGui::GetContentRegionAvail().x - min_w);
    const ImGuiSelectableFlags selectable_flags =
            ImGuiSelectableFlags_SelectOnRelease |
            ImGuiSelectableFlags_NoSetKeyOwner |
            ImGuiSelectableFlags_SetNavIdOnHover;

    ImGui::PushID(label);
    if (!enabled)
        ImGui::BeginDisabled();

    bool pressed = ImGui::Selectable(
            "", false,
            selectable_flags | ImGuiSelectableFlags_SpanAvailWidth,
            ImVec2(min_w, label_size.y));
    const ImGuiMenuColumns *offsets = &window->DC.MenuColumns;
    if (g.LastItemData.StatusFlags & ImGuiItemStatusFlags_Visible) {
        if (is_toggle) {
            const float box_s = ImMin(check_col_w - 6.0f, g.FontSize);
            ImVec2 box_min = pos + ImVec2(
                    offsets->OffsetIcon + (check_col_w - box_s) * 0.5f,
                    (label_size.y - box_s) * 0.5f);
            ImVec2 box_max = box_min + ImVec2(box_s, box_s);
            ImU32 col = ImGui::GetColorU32(
                    enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled);
            window->DrawList->AddRect(
                    box_min, box_max, col, 0.0f, 0, 1.0f);
            if (checked) {
                ImGui::RenderCheckMark(
                        window->DrawList,
                        box_min + ImVec2(box_s * 0.15f, box_s * 0.15f),
                        col, box_s * 0.7f);
            }
        }
        ImGui::RenderText(pos + ImVec2(offsets->OffsetLabel, 0.0f), label);
        if (shortcut_w > 0.0f) {
            ImGui::PushStyleColor(ImGuiCol_Text,
                                 style.Colors[ImGuiCol_TextDisabled]);
            ImGui::RenderText(
                    pos + ImVec2(offsets->OffsetShortcut + stretch_w, 0.0f),
                    shortcut, NULL, false);
            ImGui::PopStyleColor();
        }
    }

    if (!enabled)
        ImGui::EndDisabled();
    ImGui::PopID();
    return pressed;
}

bool gui_menu_item(int action, const char *label, bool enabled)
{
    const action_t *a = NULL;
    if (action) {
        a = action_get(action, true);
        assert(a);
    }
    const char *shortcut = a ? a->shortcut : NULL;
    bool clicked;
    if (g_menu_checkbox_column)
        clicked = menu_item_checkbox_column(
                label, shortcut, false, false, enabled);
    else
        clicked = ImGui::MenuItem(label, shortcut, false, enabled);
    if (clicked && a)
        action_exec(a);
    return clicked;
}

bool gui_menu_toggle(int action, const char *label, bool checked, bool enabled)
{
    const action_t *a = NULL;
    if (action) {
        a = action_get(action, true);
        assert(a);
    }
    const char *shortcut = a ? a->shortcut : NULL;
    bool clicked = menu_item_checkbox_column(
            label, shortcut, true, checked, enabled);
    if (clicked && a)
        action_exec(a);
    return clicked;
}

void gui_tooltip(const char *str)
{
    if (gui->scrolling) return;
    ImGui::PushStyleColor(ImGuiCol_PopupBg, COLOR(TOOLTIP, BACKGROUND, 0));
    ImGui::SetTooltip("%s", str);
    ImGui::PopStyleColor();
}

bool gui_tab(const char *label, int icon, bool *v)
{
    return _selectable(label, v, NULL, 0, icon, false);
}

static bool panel_header_close_button(void)
{
    float w;
    ImVec2 uv0, uv1;
    ImVec2 center;
    const ImGuiStyle& style = ImGui::GetStyle();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    bool ret;

    w = ITEM_HEIGHT + style.FramePadding.x;
    ImGui::SameLine();
    ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x - w, 0));
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ret = ImGui::Button("", ImVec2(ITEM_HEIGHT, ITEM_HEIGHT));
    ImGui::PopStyleColor();

    center = ImGui::GetItemRectMin() +
        ImVec2(ImGui::GetItemRectSize().y / 2,
               ImGui::GetItemRectSize().y / 2);
    uv0 = get_icon_uv(ICON_CLOSE);
    uv1 = uv0 + ImVec2(1. / 8, 1. / 8);
    draw_list->AddImage((intptr_t)g_tex_icons->tex,
                            center - ImVec2(12, 12),
                            center + ImVec2(12, 12),
                            uv0, uv1, get_icon_color(ICON_CLOSE, 0));
    return ret;
}

bool gui_panel_header(const char *label)
{
    bool ret;
    float label_w = ImGui::CalcTextSize(label).x;
    float w = ImGui::GetContentRegionAvail().x - ITEM_HEIGHT;

    ImGui::PushID("panel_header");
    ImGui::BeginGroup();
    ImGui::Dummy(ImVec2((w - label_w) / 2, 0));
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    gui_text(label);
    ret = panel_header_close_button();
    ImGui::EndGroup();
    ImGui::PopID();
    if (ImGui::IsItemHovered())
        gui->can_move_window |= 1;

    /* Everything after the header scrolls inside a child so the title/close
     * bar (and window drag hit-target) stay fixed. */
    if (gui->win_dir == 1 && !gui->win_body_started) {
        float bottom_pad = ImGui::GetStyle().WindowPadding.y;
        float max_body = gui->win_max_h - ImGui::GetCursorPosY() - bottom_pad;
        if (max_body < 1.0f)
            max_body = 1.0f;

        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
        if (gui->win_autofit_y) {
            ImGui::SetNextWindowSizeConstraints(
                    ImVec2(0, 0), ImVec2(FLT_MAX, max_body));
            ImGui::BeginChild("##gui_panel_body", ImVec2(0, 0),
                              ImGuiChildFlags_AutoResizeY);
        } else {
            ImGui::BeginChild("##gui_panel_body", ImVec2(0, 0));
        }
        gui->win_body_started = true;
    }
    return ret;
}

bool gui_icons_grid(int nb, const gui_icon_info_t *icons, int *current)
{
    const gui_icon_info_t *icon;
    char label[128];
    bool v;
    bool ret = false;
    int i;
    float last_button_x;
    float next_button_x;
    float max_x;
    const ImGuiStyle &style = ImGui::GetStyle();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    bool clicked;
    float size;
    bool is_colors_grid;
    float spacing = 2;

    is_colors_grid = (nb > 0 && !icons[0].icon);

    if (is_colors_grid) spacing = 8;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(spacing, spacing));

    max_x = ImGui::GetWindowPos().x + ImGui::GetContentRegionAvail().x;
    max_x += 16; // ?

    for (i = 0; i < nb; i++) {
        icon = &icons[i];
        ImGui::PushID(i);
        if (icon->sublabel) {
            snprintf(label, sizeof(label), "%s (%s)",
                     icon->label, icon->sublabel);
        } else {
            snprintf(label, sizeof(label), "%s", icon->label);
        }
        v = (current && *current >= 0 && i == *current);
        if (!is_colors_grid) {
            size = ICON_HEIGHT;
            clicked = gui_selectable_icon(label, &v, icon->icon);
        } else { // Color icon.
            size = ITEM_HEIGHT;
            ImGui::PushStyleColor(ImGuiCol_Button, icon->color);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, icon->color);
            clicked = ImGui::Button("", ImVec2(size, size));
            ImGui::PopStyleColor(2);
            if (icon->label && ImGui::IsItemHovered())
                gui_tooltip(icon->label);
            if (v) {
                ImVec2 c1 = ImGui::GetItemRectMin() - ImVec2(1, 1);
                ImVec2 c2 = ImGui::GetItemRectMax() + ImVec2(1, 1);
                draw_list->AddRect(c1, c2, 0xFF000000, 0, 0, 2);
                draw_list->AddRect(c1, c2, 0xFFFFFFFF, 0, 0, 1);
            }
        }
        if (clicked) {
            ret = true;
            *current = i;
        }
        last_button_x = ImGui::GetItemRectMax().x;
        next_button_x = last_button_x + style.ItemSpacing.x + size;
        if (i + 1 < nb && next_button_x < max_x)
            ImGui::SameLine();

        ImGui::PopID();

    }
    ImGui::PopStyleVar(1);

    return ret;
}

int gui_color_swatches_grid(int nb, const gui_icon_info_t *icons,
                            const bool *multi_selected, int *current)
{
    const gui_icon_info_t *icon;
    int i;
    int ret = 0;
    float last_button_x;
    float next_button_x;
    float max_x;
    float size = ITEM_HEIGHT;
    float spacing = 8;
    const ImGuiStyle &style = ImGui::GetStyle();
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    bool highlight;
    bool shift = gui && gui->shift_down && !(gui && gui->ctrl_down);

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(spacing, spacing));
    max_x = ImGui::GetWindowPos().x + ImGui::GetContentRegionAvail().x;
    max_x += 16;

    for (i = 0; i < nb; i++) {
        icon = &icons[i];
        ImGui::PushID(i);
        if (multi_selected)
            highlight = multi_selected[i];
        else
            highlight = (current && *current >= 0 && i == *current);

        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(icon->color));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(icon->color));
        ImGui::Button("", ImVec2(size, size));
        ImGui::PopStyleColor(2);
        /* IsItemClicked: reliable with modifiers; Button() alone can miss them. */
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            if (current) *current = i;
            ret = shift ? 2 : 1;
        } else if (ImGui::IsItemHovered() &&
                   ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            if (current) *current = i;
            ret = 3;
        }
        if (icon->label && icon->label[0] && ImGui::IsItemHovered())
            gui_tooltip(icon->label);
        if (highlight) {
            ImVec2 c1 = ImGui::GetItemRectMin() - ImVec2(1, 1);
            ImVec2 c2 = ImGui::GetItemRectMax() + ImVec2(1, 1);
            draw_list->AddRect(c1, c2, 0xFF000000, 0, 0, 2);
            draw_list->AddRect(c1, c2, 0xFFFFFFFF, 0, 0, 1);
        }
        last_button_x = ImGui::GetItemRectMax().x;
        next_button_x = last_button_x + style.ItemSpacing.x + size;
        if (i + 1 < nb && next_button_x < max_x)
            ImGui::SameLine();
        ImGui::PopID();
    }
    ImGui::PopStyleVar(1);
    return ret;
}

bool gui_want_capture_mouse(void)
{
    gui_init();
    ImGuiIO& io = ImGui::GetIO();
    return io.WantCaptureMouse;
}

bool gui_want_capture_keyboard(void)
{
    gui_init();
    ImGuiIO& io = ImGui::GetIO();
    return io.WantCaptureKeyboard;
}

bool gui_pick_rgb_keep_alpha(void)
{
    gui_init();
    return gui->ctrl_down && gui->shift_down;
}

typedef struct list_item list_item_t;
struct list_item {
    int ret;
    list_item_t *next, *prev;
};
static void list_move_item(list_item_t **list, list_item_t *item, int d)
{
    // XXX: ugly code.
    list_item_t *other = NULL;
    assert(d == -1 || d == +1);
    if (d == -1) {
        other = item->next;
        SWAP(other, item);
    } else if (item != *list) {
        other = item->prev;
    }
    if (!other || !item) return;
    DL_DELETE(*list, item);
    DL_PREPEND_ELEM(*list, other, item);
}
void gui_list(const gui_list_t *list)
{
    list_item_t **items = (list_item_t**)list->items;
    list_item_t *item;
    bool is_current;
    int i;
    int move_dir = 0;
    int count;
    list_item_t *move_item = NULL;
    DL_COUNT(*items, item, count);
    gui_group_begin(NULL);
    i = 0;
    DL_FOREACH_REVERSE(*items, item) {
        is_current = *list->current == item;
        if (list->render((void*)item, i, is_current)) {
            *list->current = item;
            if (is_current && list->can_be_null) {
                *list->current = NULL;
            }
        }
        if (!move_dir && ImGui::IsItemActive() && !ImGui::IsItemHovered()) {
            /* DL_FOREACH_REVERSE: invert vs forward-order drag mapping. */
            move_dir = ImGui::GetMouseDragDelta(0).y < 0.f ? -1 : +1;
            move_item = item;
        }
        i++;
    }
    gui_group_end();
    if (move_item) {
        list_move_item(items, move_item, move_dir);
        move_item = NULL;
        move_dir = 0;
    }
}

bool gui_dnd_source(const char *type, const void *payload, int size,
                    const char *preview)
{
    if (!ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID))
        return false;
    ImGui::SetDragDropPayload(type, payload, (size_t)size);
    if (preview && preview[0])
        ImGui::TextUnformatted(preview);
    ImGui::EndDragDropSource();
    return true;
}

int gui_dnd_target(const char *type, void *payload_out, int size)
{
    const ImGuiPayload *payload;
    float my, y0, y1, t;
    int kind = 0;

    if (!ImGui::BeginDragDropTarget())
        return 0;
    payload = ImGui::AcceptDragDropPayload(type);
    if (payload && payload->DataSize == size) {
        memcpy(payload_out, payload->Data, (size_t)size);
        y0 = ImGui::GetItemRectMin().y;
        y1 = ImGui::GetItemRectMax().y;
        my = ImGui::GetMousePos().y;
        t = (y1 > y0) ? (my - y0) / (y1 - y0) : 0.5f;
        if (t < 0.25f)
            kind = 2; /* above in UI */
        else if (t > 0.75f)
            kind = 3; /* below in UI */
        else
            kind = 1; /* onto */
    }
    ImGui::EndDragDropTarget();
    return kind;
}

/* Drop hitbox at the seam above the next row. Does not add layout height.
 * With ItemSpacing.y > 0, centers in that band; with 0 (abutting padded
 * rows), centers on the shared edge. indent_x shifts the line start;
 * slot_index / slot_count stack several gaps in one band (0 = top).
 * On delivery returns drop_kind; otherwise 0. */
int gui_dnd_gap_target(const char *type, void *payload_out, int size,
                       float height, float indent_x, int drop_kind,
                       int slot_index, int slot_count)
{
    const ImGuiPayload *payload;
    ImGuiWindow *window = ImGui::GetCurrentWindow();
    float spacing_y = ImGui::GetStyle().ItemSpacing.y;
    float h = (height > 0.f) ? height : 3.f;
    ImVec2 restore = ImGui::GetCursorScreenPos();
    float x0 = window->WorkRect.Min.x + ImMax(0.f, indent_x);
    float x1 = window->WorkRect.Max.x;
    float base_y = restore.y - spacing_y * 0.5f;
    int slots = (slot_count > 0) ? slot_count : 1;
    int slot = (slot_index < 0) ? 0 : slot_index;
    /* Keep stacked lines tight - step by ~2px, not full hitbox height. */
    float step = 2.f;
    float y_center = base_y - (slots - 1) * (step * 0.5f) + slot * step;
    float y0 = y_center - h * 0.5f;
    ImU32 col = ImGui::GetColorU32(ImGuiCol_DragDropTarget);
    int kind = 0;
    char id[32];

    if (x1 <= x0 + 1.f)
        x1 = x0 + 1.f;

    /* Include drop_kind so nest-exit (4) and below-last (3) gaps under the
     * same PushID do not share an ImGui ID (DebugHighlightIdConflicts). */
    snprintf(id, sizeof(id), "##dnd_gap_%d_%d", drop_kind, slot);
    ImGui::SetCursorScreenPos(ImVec2(x0, y0));
    ImGui::InvisibleButton(id, ImVec2(x1 - x0, h));

    if (ImGui::BeginDragDropTarget()) {
        payload = ImGui::AcceptDragDropPayload(
                type, ImGuiDragDropFlags_AcceptBeforeDelivery |
                      ImGuiDragDropFlags_AcceptNoDrawDefaultRect);
        if (payload && payload->DataSize == size) {
            memcpy(payload_out, payload->Data, (size_t)size);
            ImGui::GetWindowDrawList()->AddRectFilled(
                    ImVec2(x0, y_center - 1.f),
                    ImVec2(x1, y_center + 1.f),
                    col);
            if (payload->IsDelivery())
                kind = drop_kind;
        }
        ImGui::EndDragDropTarget();
    }

    ImGui::SetCursorScreenPos(restore);
    return kind;
}