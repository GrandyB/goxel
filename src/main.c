/* Goxel 3D voxels editor
 *
 * copyright (c) 2015-2022 Guillaume Chereau <guillaume@noctua-software.com>
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

#include "goxel.h"
#include "script.h"
#include <getopt.h>

#ifdef GLES2
#   define GLFW_INCLUDE_ES2
#endif
#include <GLFW/glfw3.h>

static inputs_t     *g_inputs = NULL;
static GLFWwindow   *g_window = NULL;
static float        g_scale = 1;

static void on_glfw_error(int code, const char *msg)
{
    // Discard GLFW error 65548
    // This error indicates that setting the window icon was unsuccessful,
    // which is not fatal and normal on Wayland.
    if (code == 65548) {
        return;
    }

    fprintf(stderr, "glfw error %d (%s)\n", code, msg);
    assert(false);
}

void on_scroll(GLFWwindow *win, double x, double y)
{
    g_inputs->mouse_wheel = y;
}

void on_char(GLFWwindow *win, unsigned int c)
{
    inputs_insert_char(g_inputs, c);
}

void on_drop(GLFWwindow* win, int count, const char** paths)
{
    int i;
    for (i = 0;  i < count;  i++)
        goxel_import_file(paths[i], NULL);
}

void on_close(GLFWwindow *win)
{
    glfwSetWindowShouldClose(win, GLFW_FALSE);
    gui_query_quit();
}

typedef struct
{
    char *input;
    char *export;
    float scale;

    const char *script;
    int script_args_nb;
    const char *script_args[32];
} args_t;

#define OPT_HELP 1
#define OPT_VERSION 2
#define OPT_SCRIPT 3

typedef struct {
    const char *name;
    int val;
    int has_arg;
    const char *arg_name;
    const char *help;
} gox_option_t;

static const gox_option_t OPTIONS[] = {
    {"export", 'e', required_argument, "FILENAME",
        .help="Export the image to a file"},
    {"scale", 's', required_argument, "FLOAT", .help="Set UI scale"},
    {"script", OPT_SCRIPT, required_argument, "FILENAME",
        .help="Run a script and exit"},
    {"help", OPT_HELP, .help="Give this help list"},
    {"version", OPT_VERSION, .help="Print program version"},
    {}
};

static void print_help(void)
{
    const gox_option_t *opt;
    char buf[128];

    printf("Usage: goxel [OPTION...] [INPUT]\n");
    printf("A 3D voxels editor\n");
    printf("\n");

    for (opt = OPTIONS; opt->name; opt++) {
        if (opt->val >= 'a')
            printf("  -%c, ", opt->val);
        else
            printf("      ");

        if (opt->has_arg)
            snprintf(buf, sizeof(buf), "--%s=%s", opt->name, opt->arg_name);
        else
            snprintf(buf, sizeof(buf), "--%s", opt->name);
        printf("%-23s %s\n", buf, opt->help);
    }
    printf("\n");
    printf("Report bugs to <guillaume@noctua-software.com>.\n");
}

static void parse_options(int argc, char **argv, args_t *args)
{
    int i, c, option_index;
    const gox_option_t *opt;
    struct option long_options[ARRAY_SIZE(OPTIONS)] = {};

    for (i = 0; i < ARRAY_SIZE(OPTIONS); i++) {
        opt = &OPTIONS[i];
        long_options[i] = (struct option) {
            opt->name,
            opt->has_arg,
            NULL,
            opt->val,
        };
    }

    while (true) {
        c = getopt_long(argc, argv, "e:s:", long_options, &option_index);
        if (c == -1) break;
        switch (c) {
        case 'e':
            args->export = optarg;
            break;
        case 's':
            args->scale = atof(optarg);
            break;
        case OPT_HELP:
            print_help();
            exit(0);
        case OPT_VERSION:
            printf("Goxel " GOXEL_VERSION_STR "\n");
            exit(0);
        case OPT_SCRIPT:
            args->script = optarg;
            break;
        case '?':
            exit(-1);
        }
    }
    if (optind < argc) {
        if (args->script) {
            args->script_args[args->script_args_nb++] = argv[optind];
        } else {
            args->input = argv[optind];
        }
    }
}


static void loop_function(void)
{
    int fb_size[2];
    int i;
    double xpos, ypos;
    float scale;
    float scales[2];
    GLFWmonitor *monitor;

    if (    !glfwGetWindowAttrib(g_window, GLFW_VISIBLE) ||
             glfwGetWindowAttrib(g_window, GLFW_ICONIFIED)) {
        glfwWaitEvents();
        goto end;
    }

    glfwGetFramebufferSize(g_window, &fb_size[0], &fb_size[1]);
    monitor = glfwGetPrimaryMonitor();
    glfwGetMonitorContentScale(monitor, &scales[0], &scales[1]);
    scale = g_scale * scales[0];

    g_inputs->window_size[0] = fb_size[0] / scale;
    g_inputs->window_size[1] = fb_size[1] / scale;
    g_inputs->scale = scale;

    GL(glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));

    for (i = GLFW_KEY_SPACE; i <= GLFW_KEY_LAST; i++) {
        g_inputs->keys[i] = glfwGetKey(g_window, i) == GLFW_PRESS;
    }
    glfwGetCursorPos(g_window, &xpos, &ypos);
#ifndef __APPLE__ // As far as I can tell this is a bug in glfw on Mac.
    xpos /= scales[0];
    ypos /= scales[1];
#endif
    vec2_set(g_inputs->touches[0].pos, xpos / g_scale, ypos / g_scale);

    g_inputs->touches[0].down[0] =
        glfwGetMouseButton(g_window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    g_inputs->touches[0].down[1] =
        glfwGetMouseButton(g_window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;
    g_inputs->touches[0].down[2] =
        glfwGetMouseButton(g_window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;

    goxel_iter(g_inputs);
    goxel_render(g_inputs);

    memset(g_inputs, 0, sizeof(*g_inputs));
    glfwSwapBuffers(g_window);
end:
    glfwPollEvents();
}

#ifndef __EMSCRIPTEN__
static void start_main_loop(void (*func)(void))
{
    while (!glfwWindowShouldClose(g_window)) {
        func();
        if (goxel.quit) break;
    }
    glfwTerminate();
}
#else
static void start_main_loop(void (*func)(void))
{
    emscripten_set_main_loop(func, 0, 1);
}
#endif

#if GLFW_VERSION_MAJOR >= 3 && GLFW_VERSION_MINOR >= 2

static void load_icon(GLFWimage *image, const char *path)
{
    uint8_t *img;
    int w, h, bpp = 0, size;
    const void *data;
    data = assets_get(path, &size);
    assert(data);
    img = img_read_from_mem(data, size, &w, &h, &bpp);
    assert(img);
    assert(bpp == 4);
    image->width = w;
    image->height = h;
    image->pixels = img;
}

static void set_window_icon(GLFWwindow *window)
{
    GLFWimage icons[7];
    int i;
    load_icon(&icons[0], "asset://data/icons/icon16.png");
    load_icon(&icons[1], "asset://data/icons/icon24.png");
    load_icon(&icons[2], "asset://data/icons/icon32.png");
    load_icon(&icons[3], "asset://data/icons/icon48.png");
    load_icon(&icons[4], "asset://data/icons/icon64.png");
    load_icon(&icons[5], "asset://data/icons/icon128.png");
    load_icon(&icons[6], "asset://data/icons/icon256.png");
    glfwSetWindowIcon(window, 7, icons);
    for (i = 0; i < 7; i++) free(icons[i].pixels);
}

#else
static void set_window_icon(GLFWwindow *window) {}
#endif

static void set_window_title(void *user, const char *title)
{
    glfwSetWindowTitle(g_window, title);
}

static const char *get_clipboard_text(void *user)
{
    return glfwGetClipboardString(g_window);
}

static void set_clipboard_text(void *user, const char *text)
{
    glfwSetClipboardString(g_window, text);
}

int main(int argc, char **argv)
{
    args_t args = {.scale = 1};
    GLFWwindow *window;
    GLFWmonitor *monitor;
    const GLFWvidmode *mode;
    int width = 640, height = 480, ret = 0;
    inputs_t inputs = {};
    g_inputs = &inputs;

    // Setup sys callbacks.
    sys_callbacks.set_window_title = set_window_title;
    sys_callbacks.get_clipboard_text = get_clipboard_text;
    sys_callbacks.set_clipboard_text = set_clipboard_text;
    parse_options(argc, argv, &args);

    g_scale = args.scale;

    glfwSetErrorCallback(on_glfw_error);
    glfwInit();
    glfwWindowHint(GLFW_SAMPLES, 4);
    monitor = glfwGetPrimaryMonitor();
    mode = glfwGetVideoMode(monitor);
    if (mode) {
        width = mode->width ?: 640;
        height = mode->height ?: 480;
    }
    glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE);
    window = glfwCreateWindow(width, height, "Goxel", NULL, NULL);
    assert(window);
    g_window = window;
    glfwMakeContextCurrent(window);
    if (!DEFINED(EMSCRIPTEN))
        glfwSetScrollCallback(window, on_scroll);
    glfwWindowHint(GLFW_DOUBLEBUFFER, GLFW_TRUE);
    glfwSwapInterval(1);
    glfwSetDropCallback(window, on_drop);
    glfwSetCharCallback(window, on_char);
    glfwSetWindowCloseCallback(window, on_close);
    glfwSetInputMode(window, GLFW_STICKY_MOUSE_BUTTONS, false);
    set_window_icon(window);

#ifdef WIN32
    glewInit();
#endif
    goxel_init();

    // Run the unit tests in debug.
    if (DEBUG) {
        tests_run();
        goxel_reset();
    }

    if (args.input)
        goxel_import_file(args.input, NULL);

    if (args.script) {
        script_run_from_file(args.script, args.script_args_nb, args.script_args);
        goto end;
    }

    if (args.export) {
        if (!args.input) {
            LOG_E("trying to export an empty image");
            ret = -1;
        } else {
            ret = goxel_export_to_file(args.export, NULL);
        }
        goto end;
    }
    start_main_loop(loop_function);
end:
    glfwTerminate();
    goxel_release();
    return ret;
}
