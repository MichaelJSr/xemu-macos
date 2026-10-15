/*
 * xemu SDL display driver
 *
 * Copyright (c) 2020-2025 Matt Borgerson
 *
 * Based on sdl2.c, sdl2-gl.c
 *
 * Copyright (c) 2003 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
/* Ported SDL 1.2 code to 2.0 by Dave Airlie. */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/thread.h"
#include "qemu/main-loop.h"
#include "qemu/rcu.h"
#include "qemu/cutils.h"
#include "qemu-version.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-block.h"
#include "qapi/qapi-commands-ui.h"
#include "qapi/qapi-commands-xemu.h"
#include "qemu/coroutine.h"
#include "qobject/qdict.h"
#include "ui/console.h"
#include "ui/input.h"
#include "ui/kbd-state.h"
#include "system/runstate.h"
#include "system/runstate-action.h"
#include "system/system.h"
#include "system/cpus.h"
#include "exec/icount.h"
#include "exec/cpu-common.h"
#include "exec/target_page.h"
#include "hw/core/cpu.h"
#include "qemu/rcu.h"
#include "qemu/units.h"
#include "xui/xemu-hud.h"
#include "xemu-input.h"
#include "xemu-settings.h"
#include "xemu-snapshots.h"
#include "xemu-version.h"
#include "xemu-os-utils.h"
#include "xemu-present.h"

#include "data/xemu_64x64.png.h"

#include "hw/xbox/smbus.h" // For eject, drive tray
#include "hw/xbox/nv2a/nv2a.h"
#include "ui/xemu-notifications.h"

#include <stb_image.h>
#include <locale.h>
#include <math.h>
#ifdef __APPLE__
#include <pthread.h>
#include "xemu-metal.h"
#include "hw/xbox/nv2a/nsprof.h"
#endif
#include <SDL3/SDL.h>

#ifdef _WIN32
#include "xui/win32-dxgi-present.h"
#endif

#ifndef DEBUG_XEMU_C
#define DEBUG_XEMU_C 0
#endif

/*
 * The lock-occupancy stat is runtime-enableable on macOS (where it is
 * measured) and stays compile-time-only elsewhere, so the Windows/Linux
 * release paths are unchanged.
 */
#if DEBUG_XEMU_C || defined(__APPLE__)
#define XEMU_LOCK_STATS 1
#else
#define XEMU_LOCK_STATS 0
#endif

uint64_t vblank_interval_ns = 16666666LL;
bool use_vblank_timer_thread = true;

/*
 * Presentation backend resolution. Resolved once, before window
 * creation (the SDL window type is fixed at creation time), and
 * stable for the process lifetime: a mid-run config change or a
 * renderer switch under `auto` requires a restart to take effect.
 */
bool xemu_present_is_metal(void)
{
#ifdef __APPLE__
    static int resolved = -1;
    if (resolved < 0) {
        switch (g_config.display.window.presentation_backend) {
        case CONFIG_DISPLAY_WINDOW_PRESENTATION_BACKEND_METAL:
            resolved = 1;
            break;
        case CONFIG_DISPLAY_WINDOW_PRESENTATION_BACKEND_OPENGL:
            resolved = 0;
            break;
        case CONFIG_DISPLAY_WINDOW_PRESENTATION_BACKEND_AUTO:
        default:
            resolved = (g_config.display.renderer ==
                        CONFIG_DISPLAY_RENDERER_VULKAN);
            break;
        }
    }
    return resolved == 1;
#else
    return false;
#endif
}

XemuHeadlessMode xemu_headless_mode(void)
{
    static int mode = -1;
    if (mode < 0) {
        const char *e = getenv("XEMU_HEADLESS");
        mode = XEMU_HEADLESS_OFF;
        if (e && !strcmp(e, "1")) {
            mode = XEMU_HEADLESS_OFFSCREEN;
        } else if (e && !strcmp(e, "null")) {
            mode = XEMU_HEADLESS_NULL;
        } else if (e && e[0] && strcmp(e, "0")) {
            fprintf(stderr, "xemu: ignoring XEMU_HEADLESS=%s (use 1 or null)\n",
                    e);
        }
    }
    return mode;
}

#ifdef _WIN32
/*
 * XEMU_WIN32_DXGI=0 restores the legacy present path: skip the DXGI
 * flip-model swapchain + WGL_NV_DX_interop presenter entirely and present
 * with SDL_GL_SwapWindow. Unset or any other value keeps the DXGI
 * presenter (the default). Read once; the first call is on the main
 * thread during display_early_init(), before any frame is rendered, so
 * the cached value is stable for the process lifetime.
 */
static bool win32_dxgi_present_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("XEMU_WIN32_DXGI");
        cached = (e && e[0] == '0') ? 0 : 1;
    }
    return cached == 1;
}
#endif

/*
 * Drain and log the first OpenGL error observed per session.
 * Upstream uses `assert(glGetError() == GL_NO_ERROR)` at the end of
 * gl_render_frame, which hard-crashes on macOS for transient errors
 * we otherwise tolerate (OpenGL renderer's own GL contexts vs the
 * main display context, ImGui backend hiccups on renderer toggle).
 * Log once with the error code so the user can report if the
 * display is actually broken; drain the queue either way.
 *
 * where == NULL drains silently: used at the top of gl_render_frame
 * to absorb errors inherited from a previous frame (most commonly a
 * one-shot stale-state flush on renderer switch, e.g. VULKAN ->
 * OPENGL where a rect-texture / IOSurface resource was torn down
 * after ImGui's last frame bound it), so gl_render_frame only
 * attributes errors produced inside its own body.
 */
static void gl_drain_errors(const char *where)
{
    static bool warned_once;
    int drained = 0;
    GLenum err;
    while ((err = glGetError()) != GL_NO_ERROR) {
        if (!warned_once && where) {
            warned_once = true;
            fprintf(stderr,
                    "xemu: OpenGL error 0x%04x at %s "
                    "(non-fatal; further errors suppressed; "
                    "switch to VULKAN renderer if display is garbled)\n",
                    err, where);
        }
        if (++drained > 64) {
            /* Bail out of a driver error storm so we don't spin. */
            break;
        }
    }
}

struct xemu_console {
    DisplayChangeListener dcl;
    DisplaySurface *surface;
    DisplayOptions *opts;
    SDL_Window *real_window;
    int idx;
    int hidden;
    int ignore_hotkeys;
    SDL_GLContext winctx;
    QKbdState *kbd;
};

#ifdef _WIN32
#include "nvapi.h"
// Provide hint to prefer high-performance graphics for hybrid systems
// https://gpuopen.com/learn/amdpowerxpressrequesthighperformance/
__declspec(dllexport) DWORD AmdPowerXpressRequestHighPerformance = 1;
// https://docs.nvidia.com/gameworks/content/technologies/desktop/optimus.htm
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
#endif

static int num_outputs;
static struct xemu_console *scon_list;
static SDL_Surface *guest_sprite_surface;
static int gui_grab; /* if true, all keyboard/mouse events are grabbed */
static bool alt_grab;
static bool ctrl_grab;
static int gui_saved_grab;
static int gui_fullscreen;
static int gui_grab_code = SDL_KMOD_LALT | SDL_KMOD_LCTRL;
static SDL_Cursor *sdl_cursor_normal;
static SDL_Cursor *sdl_cursor_hidden;
static int absolute_enabled;
static int guest_cursor;
static int guest_x, guest_y;
static SDL_Cursor *guest_sprite;
static Notifier mouse_mode_notifier;
static SDL_Window *m_window;
static SDL_GLContext m_context;
static QemuSemaphore display_init_sem;
static QemuSemaphore display_shutdown_sem;
static QEMUTimer *vblank_timer;
static QemuThread vblank_thread;
static bool qemu_exiting;
static int exit_status;

/* A message box would block a headless run that nobody can see. */
static void show_error_box(const char *title, const char *msg)
{
    if (xemu_headless_mode()) {
        fprintf(stderr, "%s: %s\n", title, msg);
        return;
    }
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, msg, m_window);
}

void tcg_register_init_ctx(void); // tcg.c

#if XEMU_LOCK_STATS
static uint64_t lock_held_acc;
static uint64_t lock_start;

/*
 * Main-loop-mutex + BQL occupancy accounting. DEBUG_XEMU_C keeps it
 * unconditional as before; otherwise XEMU_UI_LOCK_STATS=1 turns it on in a
 * normal build. Latched on first use; the racing writers all store the same
 * value. lock_start is only touched between lock and unlock, i.e. under the
 * locks it measures.
 */
static bool lock_stats_enabled(void)
{
    static int cached = -1;
    if (unlikely(cached < 0)) {
#if DEBUG_XEMU_C
        cached = 1;
#else
        const char *e = getenv("XEMU_UI_LOCK_STATS");
        cached = (e && e[0] && e[0] != '0') ? 1 : 0;
#endif
    }
    return cached == 1;
}
#endif

void xemu_main_loop_lock(void)
{
    qemu_mutex_lock_main_loop();
    bql_lock();
#if XEMU_LOCK_STATS
    if (unlikely(lock_stats_enabled())) {
        lock_start = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    }
#endif
}

void xemu_main_loop_unlock(void)
{
#if XEMU_LOCK_STATS
    if (unlikely(lock_stats_enabled())) {
        lock_held_acc += qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - lock_start;
    }
#endif
    bql_unlock();
    qemu_mutex_unlock_main_loop();
}

SDL_Window *xemu_get_window(void)
{
    return m_window;
}

static struct xemu_console *get_scon_from_window(uint32_t window_id)
{
    int i;
    for (i = 0; i < num_outputs; i++) {
        if (scon_list[i].real_window == SDL_GetWindowFromID(window_id)) {
            return &scon_list[i];
        }
    }
    return NULL;
}

static void window_resize(struct xemu_console *scon)
{
    if (!scon->real_window) {
        return;
    }

    SDL_SetWindowSize(scon->real_window,
                      surface_width(scon->surface),
                      surface_height(scon->surface));
}

static void hide_cursor(struct xemu_console *scon)
{
    if (scon->opts->has_show_cursor && scon->opts->show_cursor) {
        return;
    }

    SDL_HideCursor();
    SDL_SetCursor(sdl_cursor_hidden);

    if (!qemu_input_is_absolute(scon->dcl.con)) {
        SDL_SetWindowRelativeMouseMode(scon->real_window, true);
    }
}

static void show_cursor(struct xemu_console *scon)
{
    if (scon->opts->has_show_cursor && scon->opts->show_cursor) {
        return;
    }

    if (!qemu_input_is_absolute(scon->dcl.con)) {
        SDL_SetWindowRelativeMouseMode(scon->real_window, false);
    }

    if (guest_cursor &&
        (gui_grab || qemu_input_is_absolute(scon->dcl.con) || absolute_enabled)) {
        SDL_SetCursor(guest_sprite);
    } else {
        SDL_SetCursor(sdl_cursor_normal);
    }

    SDL_ShowCursor();
}

static void grab_start(struct xemu_console *scon)
{
}

static void grab_end(struct xemu_console *scon)
{
    SDL_SetWindowKeyboardGrab(scon->real_window, false);
    SDL_SetWindowMouseGrab(scon->real_window, false);
    gui_grab = 0;
    show_cursor(scon);
}

static void absolute_mouse_grab(struct xemu_console *scon)
{
    float mouse_x, mouse_y;
    int scr_w, scr_h;
    SDL_GetMouseState(&mouse_x, &mouse_y);
    SDL_GetWindowSize(scon->real_window, &scr_w, &scr_h);
    if (mouse_x > 0 && mouse_x < scr_w - 1 &&
        mouse_y > 0 && mouse_y < scr_h - 1) {
        grab_start(scon);
    }
}

static void mouse_mode_change(Notifier *notify, void *data)
{
    if (qemu_input_is_absolute(scon_list[0].dcl.con)) {
        if (!absolute_enabled) {
            absolute_enabled = 1;
            SDL_SetWindowRelativeMouseMode(scon_list[0].real_window, false);
            absolute_mouse_grab(&scon_list[0]);
        }
    } else if (absolute_enabled) {
        if (!gui_fullscreen) {
            grab_end(&scon_list[0]);
        }
        absolute_enabled = 0;
    }
}

static void send_mouse_event(struct xemu_console *scon, int dx, int dy,
                                 int x, int y, int state)
{
    static uint32_t bmap[INPUT_BUTTON__MAX] = {
        [INPUT_BUTTON_LEFT]       = SDL_BUTTON_MASK(SDL_BUTTON_LEFT),
        [INPUT_BUTTON_MIDDLE]     = SDL_BUTTON_MASK(SDL_BUTTON_MIDDLE),
        [INPUT_BUTTON_RIGHT]      = SDL_BUTTON_MASK(SDL_BUTTON_RIGHT),
    };
    static uint32_t prev_state;

    if (prev_state != state) {
        qemu_input_update_buttons(scon->dcl.con, bmap, prev_state, state);
        prev_state = state;
    }

    if (qemu_input_is_absolute(scon->dcl.con)) {
        qemu_input_queue_abs(scon->dcl.con, INPUT_AXIS_X,
                             x, 0, surface_width(scon->surface));
        qemu_input_queue_abs(scon->dcl.con, INPUT_AXIS_Y,
                             y, 0, surface_height(scon->surface));
    } else {
        if (guest_cursor) {
            x -= guest_x;
            y -= guest_y;
            guest_x += x;
            guest_y += y;
            dx = x;
            dy = y;
        }
        qemu_input_queue_rel(scon->dcl.con, INPUT_AXIS_X, dx);
        qemu_input_queue_rel(scon->dcl.con, INPUT_AXIS_Y, dy);
    }
    qemu_input_event_sync();
}

static void set_full_screen(struct xemu_console *scon, bool set)
{
    gui_fullscreen = set;

    if (gui_fullscreen) {
        const SDL_DisplayMode *mode = NULL;
        SDL_DisplayMode **modes = NULL;
        if (g_config.display.window.fullscreen_exclusive) {
            SDL_DisplayID display = SDL_GetDisplayForWindow(scon->real_window);
            if (display) {
                int num_modes = 0;
                modes = SDL_GetFullscreenDisplayModes(display, &num_modes);
                if (modes && num_modes > 0) {
                    // First mode is the highest resolution, typically the
                    // native resolution. Among the modes at that
                    // resolution, prefer the highest refresh rate (the
                    // list is not guaranteed to order by refresh; e.g.
                    // 120Hz ProMotion vs a 60Hz first entry).
                    mode = modes[0];
                    for (int i = 1; i < num_modes; i++) {
                        if (modes[i]->w == mode->w &&
                            modes[i]->h == mode->h &&
                            modes[i]->pixel_density == mode->pixel_density &&
                            modes[i]->refresh_rate > mode->refresh_rate) {
                            mode = modes[i];
                        }
                    }
                }
            }
            if (mode) {
                fprintf(stderr, "Selected exclusive fullscreen mode: %dx%d pixel_density=%f refresh_rate=%f\n", mode->w, mode->h, mode->pixel_density, mode->refresh_rate);
            } else {
                fprintf(stderr, "Failed to get fullscreen display mode: %s\n", SDL_GetError());
            }
        }
        SDL_SetWindowFullscreenMode(scon->real_window, mode);
        SDL_free(modes);
        SDL_SetWindowFullscreen(scon->real_window, true);
        gui_saved_grab = gui_grab;
        grab_start(scon);
    } else {
        if (!gui_saved_grab) {
            grab_end(scon);
        }
        SDL_SetWindowFullscreen(scon->real_window, false);
    }
}

static void toggle_full_screen(struct xemu_console *scon)
{
    set_full_screen(scon, !gui_fullscreen);
}

void xemu_toggle_fullscreen(void)
{
    toggle_full_screen(&scon_list[0]);
}

int xemu_is_fullscreen(void)
{
    return gui_fullscreen;
}

static int get_mod_state(void)
{
    SDL_Keymod mod = SDL_GetModState();

    if (alt_grab) {
        return (mod & (gui_grab_code | SDL_KMOD_LSHIFT)) ==
            (gui_grab_code | SDL_KMOD_LSHIFT);
    } else if (ctrl_grab) {
        return (mod & SDL_KMOD_RCTRL) == SDL_KMOD_RCTRL;
    } else {
        return (mod & gui_grab_code) == gui_grab_code;
    }
}

static void process_key(struct xemu_console *scon, SDL_KeyboardEvent *ev)
{
    int qcode;

    if (ev->scancode >= qemu_input_map_usb_to_qcode_len) {
        return;
    }
    qcode = qemu_input_map_usb_to_qcode[ev->scancode];
    qkbd_state_key_event(scon->kbd, qcode, ev->type == SDL_EVENT_KEY_DOWN);
}

static void handle_keydown(SDL_Event *ev)
{
    int win;
    struct xemu_console *scon = get_scon_from_window(ev->key.windowID);
    if (scon == NULL) return;
    int gui_key_modifier_pressed = get_mod_state();
    int gui_keysym = 0;

    if (!scon->ignore_hotkeys && gui_key_modifier_pressed && !ev->key.repeat) {
        switch (ev->key.scancode) {
        case SDL_SCANCODE_2:
        case SDL_SCANCODE_3:
        case SDL_SCANCODE_4:
        case SDL_SCANCODE_5:
        case SDL_SCANCODE_6:
        case SDL_SCANCODE_7:
        case SDL_SCANCODE_8:
        case SDL_SCANCODE_9:
            if (gui_grab) {
                grab_end(scon);
            }

            win = ev->key.scancode - SDL_SCANCODE_1;
            if (win < num_outputs) {
                scon_list[win].hidden = !scon_list[win].hidden;
                if (scon_list[win].real_window) {
                    if (scon_list[win].hidden) {
                        SDL_HideWindow(scon_list[win].real_window);
                    } else {
                        SDL_ShowWindow(scon_list[win].real_window);
                    }
                }
                gui_keysym = 1;
            }
            break;
        case SDL_SCANCODE_F:
            toggle_full_screen(scon);
            gui_keysym = 1;
            break;
        case SDL_SCANCODE_G:
            gui_keysym = 1;
            if (!gui_grab) {
                grab_start(scon);
            } else if (!gui_fullscreen) {
                grab_end(scon);
            }
            break;
        case SDL_SCANCODE_U:
            window_resize(scon);
            gui_keysym = 1;
            break;
        default:
            break;
        }
    }
    if (!gui_keysym) {
        process_key(scon, &ev->key);
    }
}

static void handle_keyup(SDL_Event *ev)
{
    struct xemu_console *scon = get_scon_from_window(ev->key.windowID);
    if (!scon) return;

    scon->ignore_hotkeys = false;
    process_key(scon, &ev->key);
}

static void handle_mousemotion(SDL_Event *ev)
{
    int max_x, max_y;
    struct xemu_console *scon = get_scon_from_window(ev->motion.windowID);

    if (!scon || !qemu_console_is_graphic(scon->dcl.con)) {
        return;
    }

    if (qemu_input_is_absolute(scon->dcl.con) || absolute_enabled) {
        int scr_w, scr_h;
        SDL_GetWindowSize(scon->real_window, &scr_w, &scr_h);
        max_x = scr_w - 1;
        max_y = scr_h - 1;
        if (gui_grab && !gui_fullscreen
            && (ev->motion.x == 0 || ev->motion.y == 0 ||
                ev->motion.x == max_x || ev->motion.y == max_y)) {
            grab_end(scon);
        }
        if (!gui_grab &&
            (ev->motion.x > 0 && ev->motion.x < max_x &&
             ev->motion.y > 0 && ev->motion.y < max_y)) {
            grab_start(scon);
        }
    }
    if (gui_grab || qemu_input_is_absolute(scon->dcl.con) || absolute_enabled) {
        send_mouse_event(scon, ev->motion.xrel, ev->motion.yrel,
                             ev->motion.x, ev->motion.y, ev->motion.state);
    }
}

static void handle_mousebutton(SDL_Event *ev)
{
    int buttonstate = SDL_GetMouseState(NULL, NULL);
    SDL_MouseButtonEvent *bev;
    struct xemu_console *scon = get_scon_from_window(ev->button.windowID);

    if (!scon || !qemu_console_is_graphic(scon->dcl.con)) {
        return;
    }

    bev = &ev->button;
    if (!gui_grab && !qemu_input_is_absolute(scon->dcl.con)) {
        if (ev->type == SDL_EVENT_MOUSE_BUTTON_UP && bev->button == SDL_BUTTON_LEFT) {
            /* start grabbing all events */
            grab_start(scon);
        }
    } else {
        if (ev->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            buttonstate |= SDL_BUTTON_MASK(bev->button);
        } else {
            buttonstate &= ~SDL_BUTTON_MASK(bev->button);
        }
        send_mouse_event(scon, 0, 0, bev->x, bev->y, buttonstate);
    }
}

static void handle_mousewheel(SDL_Event *ev)
{
    struct xemu_console *scon = get_scon_from_window(ev->wheel.windowID);
    SDL_MouseWheelEvent *wev = &ev->wheel;
    InputButton btn;

    if (!scon || !qemu_console_is_graphic(scon->dcl.con)) {
        return;
    }

    if (wev->y > 0) {
        btn = INPUT_BUTTON_WHEEL_UP;
    } else if (wev->y < 0) {
        btn = INPUT_BUTTON_WHEEL_DOWN;
    } else {
        return;
    }

    qemu_input_queue_btn(scon->dcl.con, btn, true);
    qemu_input_event_sync();
    qemu_input_queue_btn(scon->dcl.con, btn, false);
    qemu_input_event_sync();
}

static void handle_windowevent(SDL_Event *ev)
{
    struct xemu_console *scon = get_scon_from_window(ev->window.windowID);
    bool allow_close = true;

    if (!scon) {
        return;
    }

    switch (ev->type) {
    case SDL_EVENT_WINDOW_RESIZED:
        {
            QemuUIInfo info;
            memset(&info, 0, sizeof(info));
            info.width = ev->window.data1;
            info.height = ev->window.data2;
            dpy_set_ui_info(scon->dcl.con, &info, true);

            if (!gui_fullscreen) {
                g_config.display.window.last_width = ev->window.data1;
                g_config.display.window.last_height = ev->window.data2;
            }

#ifdef _WIN32
            if (win32_dxgi_present_is_active()) {
                int width;
                int height;
                if (!SDL_GetWindowSizeInPixels(scon->real_window, &width,
                                               &height)) {
                    fprintf(stderr, "SDL_GetWindowSizeInPixels failed "
                                    "responding to resize event.\n");
                } else {
                    win32_dxgi_present_resize(width, height);
                }
            }
#endif
        }
        break;
#ifdef _WIN32
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        if (win32_dxgi_present_is_active()) {
            win32_dxgi_present_resize(ev->window.data1, ev->window.data2);
        }
        break;
#endif

    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_MOUSE_ENTER:
        if (!gui_grab && (qemu_input_is_absolute(scon->dcl.con) || absolute_enabled)) {
            absolute_mouse_grab(scon);
        }
        /* If a new console window opened using a hotkey receives the
         * focus, SDL sends another KEYDOWN event to the new window,
         * closing the console window immediately after.
         *
         * Work around this by ignoring further hotkey events until a
         * key is released.
         */
        scon->ignore_hotkeys = get_mod_state();
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        if (gui_grab && !gui_fullscreen) {
            grab_end(scon);
        }
        break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        if (qemu_console_is_graphic(scon->dcl.con)) {
            if (scon->opts->has_window_close && !scon->opts->window_close) {
                allow_close = false;
            }
            if (allow_close) {
                shutdown_action = SHUTDOWN_ACTION_POWEROFF;
                qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_UI);
            }
        } else {
            SDL_HideWindow(scon->real_window);
            scon->hidden = true;
        }
        break;
    case SDL_EVENT_WINDOW_SHOWN:
        scon->hidden = false;
        break;
    case SDL_EVENT_WINDOW_HIDDEN:
        scon->hidden = true;
        break;
    }
}

static void mouse_warp(DisplayChangeListener *dcl,
                       int x, int y, bool on)
{
    struct xemu_console *scon = container_of(dcl, struct xemu_console, dcl);

    if (!qemu_console_is_graphic(scon->dcl.con)) {
        return;
    }

    if (on) {
        if (!guest_cursor) {
            show_cursor(scon);
        }
        if (gui_grab || qemu_input_is_absolute(scon->dcl.con) || absolute_enabled) {
            SDL_SetCursor(guest_sprite);
            if (!qemu_input_is_absolute(scon->dcl.con) && !absolute_enabled) {
                SDL_WarpMouseInWindow(scon->real_window, x, y);
            }
        }
    } else if (gui_grab) {
        hide_cursor(scon);
    }
    guest_cursor = on;
    guest_x = x, guest_y = y;
}

static void mouse_define(DisplayChangeListener *dcl,
                             QEMUCursor *c)
{

    if (guest_sprite) {
        SDL_DestroyCursor(guest_sprite);
    }

    if (guest_sprite_surface) {
        SDL_DestroySurface(guest_sprite_surface);
    }

    guest_sprite_surface =
        SDL_CreateSurfaceFrom(c->width, c->height, SDL_PIXELFORMAT_ARGB8888, c->data, c->width * 4);

    if (!guest_sprite_surface) {
        fprintf(stderr, "Failed to make rgb surface from %p\n", c);
        return;
    }
    guest_sprite = SDL_CreateColorCursor(guest_sprite_surface,
                                         c->hot_x, c->hot_y);
    if (!guest_sprite) {
        fprintf(stderr, "Failed to make color cursor from %p\n", c);
        return;
    }
    if (guest_cursor &&
        (gui_grab || qemu_input_is_absolute(dcl->con) || absolute_enabled)) {
        SDL_SetCursor(guest_sprite);
    }
}

static void xb_surface_gl_create_texture(DisplaySurface *surface)
{
    assert(QEMU_IS_ALIGNED(surface_stride(surface), surface_bytes_per_pixel(surface)));

    switch (surface_format(surface)) {
    case PIXMAN_BE_b8g8r8x8:
    case PIXMAN_BE_b8g8r8a8:
        surface->glformat = GL_BGRA_EXT;
        surface->gltype = GL_UNSIGNED_BYTE;
        break;
    case PIXMAN_BE_x8r8g8b8:
    case PIXMAN_BE_a8r8g8b8:
        surface->glformat = GL_RGBA;
        surface->gltype = GL_UNSIGNED_BYTE;
        break;
    case PIXMAN_r5g6b5:
        surface->glformat = GL_RGB;
        surface->gltype = GL_UNSIGNED_SHORT_5_6_5;
        break;
    default:
        g_assert_not_reached();
    }

    if (!surface->texture) {
        glGenTextures(1, &surface->texture);
    }
    glBindTexture(GL_TEXTURE_2D, surface->texture);
    glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT,
                  surface_stride(surface) / surface_bytes_per_pixel(surface));
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB,
                 surface_width(surface),
                 surface_height(surface),
                 0, surface->glformat, surface->gltype,
                 surface_data(surface));
    glPixelStorei(GL_UNPACK_ROW_LENGTH_EXT, 0);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
}

static void xb_surface_gl_destroy_texture(DisplaySurface *surface)
{
    if (!surface || !surface->texture) {
        return;
    }
    glDeleteTextures(1, &surface->texture);
    surface->texture = 0;
}

static bool xb_console_gl_check_format(DisplayChangeListener *dcl,
                                       pixman_format_code_t format)
{
    switch (format) {
    case PIXMAN_BE_b8g8r8x8:
    case PIXMAN_BE_b8g8r8a8:
    case PIXMAN_r5g6b5:
        return true;
    default:
        return false;
    }
}

static void gl_switch(DisplayChangeListener *dcl,
                      DisplaySurface *new_surface)
{
    struct xemu_console *scon = container_of(dcl, struct xemu_console, dcl);
    scon->surface = new_surface;
}

static float update_avg(float avg, float ms, float r) {
    if (fabs(avg-ms) > 0.25*avg) avg = ms;
    else avg = avg*(1.0-r)+ms*r;
    return avg;
}

static float fps = 1.0;

static void update_fps(void)
{
    static float avg = 1.0;
    static int64_t last_update = 0;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    if (!last_update) {
        last_update = now;
        return;
    }
    float ms = ((float)(now-last_update)/1000000.0);
    last_update = now;
    avg = update_avg(avg, ms, 0.5);
    fps = 1000.0/avg;
}

/*
 * Frame counter for automation: vblanks delivered while the VM runs
 * (QMP query-xemu-frame / xemu-frame-advance, XEMU_INPUT_PIPE "@N").
 * BQL-protected.
 */
static uint64_t frame_count;
static uint64_t frame_advance_target;
static Coroutine *frame_advance_co;
static bool frame_advance_stopping;

/* xemu-frame-advance "trace": regions read at sampled vblanks. */
static XemuTraceRegionList *frame_trace_regions;
static int64_t frame_trace_every;
static uint64_t frame_trace_start;
static XemuTraceSampleList *frame_trace_samples, **frame_trace_tail;

static void dump_virtual(uint8_t *out, const uint8_t *ram, uint64_t ram_size,
                         vaddr address, uint64_t size, int64_t *unmapped);

static void frame_trace_sample(void)
{
    RAMBlock *rb = qemu_ram_block_by_name("xbox.ram");
    const uint8_t *ram = qemu_ram_get_host_addr(rb);
    uint64_t ram_size = qemu_ram_get_used_length(rb);
    XemuTraceSample *sample = g_new0(XemuTraceSample, 1);
    strList **tail = &sample->data;
    uint8_t buf[4096];

    sample->frame = frame_count;
    for (XemuTraceRegionList *l = frame_trace_regions; l; l = l->next) {
        XemuTraceRegion *r = l->value;
        int64_t unmapped = 0;

        if (r->q_virtual) {
            dump_virtual(buf, ram, ram_size, r->address, r->size, &unmapped);
        } else if (r->address + r->size <= ram_size) {
            memcpy(buf, ram + r->address, r->size);
        } else {
            unmapped = 1;
        }
        QAPI_LIST_APPEND(tail, unmapped ? g_strdup("") :
                         g_string_free(qemu_hexdump_line(NULL, buf, r->size,
                                                         0, 0), false));
    }
    QAPI_LIST_APPEND(frame_trace_tail, sample);
}

/*
 * vm_start/vm_stop must not run in coroutine context: do them in BHs, on
 * the iohandler context, which only the main loop polls. In the main
 * context a vCPU's nested aio_poll (an IDE wait under icount) could run
 * them, where vm_stop only queues a request.
 */
static void frame_run_bh(void *opaque)
{
    if (!runstate_is_running()) {
        vm_start();
    }
}

static bool frame_advance_debug_stop;

static void frame_stop_bh(void *opaque)
{
    Coroutine *co = frame_advance_co;

    frame_advance_co = NULL;
    frame_advance_stopping = false;
    /* A gdbstub stop ends the advance early; leave the VM to the debugger. */
    if (!runstate_check(RUN_STATE_DEBUG)) {
        vm_stop(RUN_STATE_PAUSED);
    }
    aio_co_wake(co);
}

static void frame_advance_vm_state_change(void *opaque, bool running,
                                          RunState state)
{
    if (!running && state == RUN_STATE_DEBUG && frame_advance_co &&
        !frame_advance_stopping) {
        frame_advance_stopping = true;
        frame_advance_debug_stop = true;
        aio_bh_schedule_oneshot(iohandler_get_aio_context(), frame_stop_bh,
                                NULL);
    }
}

static void process_vblank(struct xemu_console *scon)
{
    assert(bql_locked());

    update_fps();

    if (runstate_is_running()) {
        frame_count++;
        /* Input tagged for this frame lands before the vblank IRQ. */
        xemu_input_frame_tick(frame_count);
        if (frame_trace_regions && frame_advance_co &&
            !frame_advance_stopping && frame_count <= frame_advance_target &&
            ((frame_count - frame_trace_start) % frame_trace_every == 0 ||
             frame_count == frame_advance_target)) {
            frame_trace_sample();
        }
    }

    graphic_hw_update(scon->dcl.con);

    if (frame_advance_co && !frame_advance_stopping &&
        frame_count >= frame_advance_target) {
        frame_advance_stopping = true;
        if (icount_enabled()) {
            /*
             * Deterministic mode: no guest instruction may run between this
             * vblank and the pause, so stop the vCPU now; the BH only
             * finishes the stop. This runs on the vCPU thread or, with the
             * vCPU parked waiting for the BQL, on the main loop.
             */
            if (qemu_in_vcpu_thread()) {
                cpu_stop_current();
            } else if (first_cpu) {
                first_cpu->stop = true;
                qemu_cpu_kick(first_cpu);
            }
        }
        aio_bh_schedule_oneshot(iohandler_get_aio_context(), frame_stop_bh, NULL);
    }
}

static XemuFrameInfo *frame_info(void)
{
    XemuFrameInfo *info = g_new0(XemuFrameInfo, 1);
    info->frame = frame_count;
    info->running = runstate_is_running();
    return info;
}

XemuFrameInfo *qmp_query_xemu_frame(Error **errp)
{
    return frame_info();
}

XemuFrameInfo *coroutine_fn qmp_xemu_frame_advance(bool has_frames,
                                                   int64_t frames,
                                                   bool has_trace,
                                                   XemuTraceRegionList *trace,
                                                   bool has_trace_every,
                                                   int64_t trace_every,
                                                   Error **errp)
{
    if (!has_frames) {
        frames = 1;
    }
    if (frames < 0) {
        error_setg(errp, "frames must not be negative");
        return NULL;
    }
    if (frame_advance_co) {
        error_setg(errp, "a frame advance is already in progress");
        return NULL;
    }
    if (!has_trace_every) {
        trace_every = 1;
    }
    if (trace_every < 1) {
        error_setg(errp, "trace-every must be at least 1");
        return NULL;
    }
    int regions = 0;
    for (XemuTraceRegionList *l = trace; l; l = l->next) {
        if (l->value->size < 1 || l->value->size > 4096) {
            error_setg(errp, "a trace region's size must be 1 to 4096");
            return NULL;
        }
        if (++regions > 256) {
            error_setg(errp, "at most 256 trace regions");
            return NULL;
        }
    }
    /*
     * Prelaunch: started with -S, so that nothing runs before a load.
     * Debug: halted by the gdbstub, which a run continues from exactly
     * that instruction (a gdbstub breakpoint still at it fires again).
     */
    if (!runstate_is_running() && !runstate_check(RUN_STATE_PAUSED) &&
        !runstate_check(RUN_STATE_PRELAUNCH) &&
        !runstate_check(RUN_STATE_DEBUG)) {
        error_setg(errp, "the VM is neither running, paused nor stopped "
                   "by the debugger");
        return NULL;
    }
    static bool state_handler;
    if (!state_handler) {
        qemu_add_vm_change_state_handler(frame_advance_vm_state_change, NULL);
        state_handler = true;
    }
    frame_advance_debug_stop = false;

    frame_advance_co = qemu_coroutine_self();
    frame_trace_regions = trace;
    frame_trace_every = trace_every;
    frame_trace_start = frame_count;
    frame_trace_samples = NULL;
    frame_trace_tail = &frame_trace_samples;
    if (frames == 0) {
        frame_advance_stopping = true;
        aio_bh_schedule_oneshot(iohandler_get_aio_context(), frame_stop_bh, NULL);
    } else {
        frame_advance_target = frame_count + frames;
        aio_bh_schedule_oneshot(iohandler_get_aio_context(), frame_run_bh, NULL);
    }
    qemu_coroutine_yield();
    XemuFrameInfo *info = frame_info();
    if (trace) {
        info->trace = frame_trace_samples;
        info->has_trace = true;
    }
    if (frame_advance_debug_stop) {
        info->has_debug_stop = true;
        info->debug_stop = true;
    }
    frame_trace_regions = NULL;
    frame_trace_samples = NULL;
    return info;
}

/*
 * Deterministic mode (icount): vblank is a QEMU_CLOCK_VIRTUAL timer on
 * multiples of vblank_interval_ns, so frames follow executed instructions
 * instead of the host clock (and with -icount sleep=off run as fast as the
 * host allows). The deadline depends only on the virtual clock, which a
 * loadvm restores, so a frame starts at the same instruction every run.
 */
static bool vblank_virtual;

static int64_t vblank_virtual_next(void)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    return (now / vblank_interval_ns + 1) * vblank_interval_ns;
}

static void vblank_virtual_callback(void *opaque)
{
    process_vblank(opaque);
    timer_mod_ns(vblank_timer, vblank_virtual_next());
}

static bool vblank_virtual_rearm;

static void vblank_virtual_vm_state_change(void *opaque, bool running,
                                           RunState state)
{
    if (running && (vblank_virtual_rearm || !timer_pending(vblank_timer))) {
        /*
         * A load may have moved the virtual clock: restart on the grid. A
         * plain pause leaves the schedule alone, including a vblank that
         * fell due just before it.
         */
        vblank_virtual_rearm = false;
        timer_mod_ns(vblank_timer, vblank_virtual_next());
    }
}

/* After loadvm or an incoming migration (savevm.c). */
void xemu_state_loaded(void);
void xemu_state_loaded(void)
{
    vblank_virtual_rearm = true;
    xemu_input_state_loaded();
}

/*
 * QMP xemu-dump-ram: guest memory to a file in one write, from the RAM
 * block's host pointer (a 128 MiB dump takes tens of milliseconds; pmemsave
 * goes 1 KiB at a time through the memory API).
 */
static void dump_virtual(uint8_t *out, const uint8_t *ram, uint64_t ram_size,
                         vaddr address, uint64_t size, int64_t *unmapped)
{
    CPUState *cpu = qemu_get_cpu(0);
    uint64_t page = qemu_target_page_size();

    for (uint64_t off = 0; off < size;) {
        vaddr va = address + off;
        uint64_t in_page = va & (page - 1);
        uint64_t chunk = MIN(size - off, page - in_page);
        hwaddr pa = cpu_get_phys_page_debug(cpu, va - in_page);

        if (pa != -1 && pa + in_page + chunk <= ram_size) {
            memcpy(out + off, ram + pa + in_page, chunk);
        } else {
            memset(out + off, 0, chunk);
            (*unmapped)++;
        }
        off += chunk;
    }
}

XemuRamDump *qmp_xemu_dump_ram(const char *filename,
                               bool has_address, uint64_t address,
                               bool has_size, uint64_t size,
                               bool has_virtual, bool virt,
                               bool has_pause, bool pause,
                               bool has_gpu_sync, bool gpu_sync,
                               Error **errp)
{
    RAMBlock *rb;
    uint8_t *ram;
    uint64_t ram_size;

    WITH_RCU_READ_LOCK_GUARD() {
        rb = qemu_ram_block_by_name("xbox.ram");
    }
    if (!rb) {
        error_setg(errp, "no xbox.ram block");
        return NULL;
    }
    ram = qemu_ram_get_host_addr(rb);
    ram_size = qemu_ram_get_used_length(rb);

    if (!has_address) {
        address = 0;
    }
    if (virt) {
        if (!has_size) {
            error_setg(errp, "a virtual dump needs a size");
            return NULL;
        }
        if (size > 4ULL * GiB) {
            error_setg(errp, "size must be at most 4 GiB");
            return NULL;
        }
    } else {
        if (address > ram_size) {
            error_setg(errp, "address is past the end of RAM (%" PRIu64 ")",
                       ram_size);
            return NULL;
        }
        if (!has_size) {
            size = ram_size - address;
        }
        if (size > ram_size - address) {
            error_setg(errp, "range runs past the end of RAM (%" PRIu64 ")",
                       ram_size);
            return NULL;
        }
    }
    if (!has_pause) {
        pause = true;
    }
    if (!has_gpu_sync) {
        gpu_sync = true;
    }

    bool resume = pause && runstate_is_running();
    if (resume) {
        vm_stop(RUN_STATE_PAUSED);
    }
    bool paused = !runstate_is_running();
    if (paused && gpu_sync) {
        nv2a_sync_ram();
    }

    int64_t t0 = get_clock();
    int64_t unmapped = 0;
    bool ok = false;
    int fd = qemu_create(filename, O_WRONLY | O_TRUNC | O_BINARY, 0644, errp);
    if (fd >= 0) {
        const uint8_t *src = ram + address;
        g_autofree uint8_t *buf = NULL;
        if (virt) {
            buf = g_try_malloc(size ? size : 1);
            if (!buf) {
                error_setg(errp, "out of memory for %" PRIu64 " bytes", size);
            } else {
                dump_virtual(buf, ram, ram_size, address, size, &unmapped);
                src = buf;
            }
        }
        if ((!virt || buf) && qemu_write_full(fd, src, size) != size) {
            error_setg_errno(errp, errno, "writing '%s' failed", filename);
        } else if (!virt || buf) {
            ok = true;
        }
        close(fd);
    }
    int64_t elapsed_us = (get_clock() - t0) / SCALE_US;

    if (resume) {
        vm_start();
    }
    if (!ok) {
        return NULL;
    }

    XemuRamDump *info = g_new0(XemuRamDump, 1);
    info->size = size;
    info->ram_size = ram_size;
    info->frame = frame_count;
    info->paused = paused;
    info->unmapped = unmapped;
    info->elapsed_us = elapsed_us;
    return info;
}

static void vblank_timer_callback(void *opaque)
{
    struct xemu_console *scon = (struct xemu_console *)opaque;

    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    process_vblank(scon);
    timer_mod_ns(vblank_timer, now + vblank_interval_ns);
}

static void *vblank_timer_thread(void *opaque)
{
    struct xemu_console *scon = (struct xemu_console *)opaque;

#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif

    int64_t next_vblank = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

    while (!qatomic_read(&qemu_exiting)) {
        // Schedule next vblank at fixed interval (absolute deadline)
        next_vblank += vblank_interval_ns;

        // Wait until deadline
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        if (now < next_vblank) {
            SDL_DelayPrecise(next_vblank - now);
        } else if (now > next_vblank + vblank_interval_ns) {
            // We've fallen behind by more than one frame, reset to avoid
            // rapid-fire catch-up
            next_vblank = now;
        }

        if (!qatomic_read(&qemu_exiting)) {
            xemu_main_loop_lock();
            process_vblank(scon);
            xemu_main_loop_unlock();
        }
    }

    return NULL;
}

#if XEMU_LOCK_STATS
static void report_stats(void)
{
    if (!lock_stats_enabled()) {
        return;
    }

    uint64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    static uint64_t last_reported = 0;
    static int num_frames = 0;
    uint64_t delta_ms = now - last_reported;
    num_frames += 1;
    if (delta_ms >= 1000) {
        fprintf(stderr,
                "[[ vblank @%fHz avg - bql %"PRIu64"ns/iter, %g%% time avg ]]\n",
                fps, lock_held_acc / num_frames,
                (double)lock_held_acc / (double)(delta_ms * 10000.0));
        lock_held_acc = 0;
        last_reported = now;
        num_frames = 0;
    }
}
#endif

/**
 * Renders the main interface. Usually called from the main thread,
 * but may sometimes be called from another thread.
 */
static void gl_render_frame(struct xemu_console *scon)
{
    static bool rendering;
    if (qatomic_xchg(&rendering, true) || qatomic_read(&qemu_exiting)) {
        return;
    }

    SDL_GL_MakeCurrent(scon->real_window, scon->winctx);

    /*
     * Absorb any GL errors that may have leaked in from a prior
     * frame's work before attributing anything to this frame. The
     * known case is the VULKAN -> OPENGL renderer switch: the last
     * VK-path frame binds an IOSurface-backed rect texture that's
     * then torn down during switch, leaving a transient
     * GL_INVALID_OPERATION that surfaces on the next frame's first
     * GL call. Benign one-shot.
     */
    gl_drain_errors(NULL);

    bool flip_required = false;
    bool release_surface_texture = false;

    /* XXX: Note that this bypasses the usual VGA path in order to quickly
     * get the surface. This is simple and fast, at the cost of accuracy.
     * Ideally, this should go through the VGA code and opportunistically pull
     * the surface like this, but handle the VGA logic as well. For now, just
     * use this fast path to handle the common case.
     *
     * In the event the surface is not found in the surface cache, e.g. when
     * the guest code isn't using HW accelerated rendering, but just blitting
     * to the framebuffer, fall back to the VGA path.
     */
    GLuint tex = nv2a_get_framebuffer_surface();

    if (tex == 0) {
        xemu_main_loop_lock();
        xb_surface_gl_create_texture(scon->surface);
        tex = scon->surface->texture;
        flip_required = true;
        release_surface_texture = true;
        xemu_main_loop_unlock();
        xemu_set_framebuffer_texture_is_rect(false);
    } else {
#if defined(__APPLE__)
        xemu_set_framebuffer_texture_is_rect(
            g_config.display.renderer == CONFIG_DISPLAY_RENDERER_VULKAN);
#else
        xemu_set_framebuffer_texture_is_rect(false);
#endif
    }

#ifdef _WIN32
    if (win32_dxgi_present_is_active()) {
        win32_dxgi_present_begin_frame();
    }
#endif

    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
    xemu_snapshots_set_framebuffer_texture(tex, flip_required);
    xemu_hud_set_framebuffer_texture(tex, flip_required);

    xemu_main_loop_lock();
    xemu_hud_update();
    if (release_surface_texture) {
        xb_surface_gl_destroy_texture(scon->surface);
    }
    xemu_main_loop_unlock();

    xemu_hud_render();
    glFlush();

    nv2a_release_framebuffer_surface();

#ifdef _WIN32
    if (win32_dxgi_present_is_active()) {
        win32_dxgi_present_end_frame(g_config.display.window.vsync);
    } else {
        static bool warned = false;
        /* Not a failure when XEMU_WIN32_DXGI=0 disabled the presenter on
         * purpose -- that path already logged once at init. */
        if (!warned && win32_dxgi_present_enabled()) {
            fprintf(stderr,
                    "win32_dxgi present failed or unavailable, falling back to "
                    "SDL_GL_SwapWindow\n");
            warned = true;
        }
        SDL_GL_SwapWindow(scon->real_window);
    }
#else
    SDL_GL_SwapWindow(scon->real_window);
#endif
    gl_drain_errors("gl_render_frame");

    qatomic_set(&rendering, false);

#if XEMU_LOCK_STATS
    report_stats();
#endif
}

#ifdef __APPLE__
/*
 * Push-present refuter (XEMU_PUSH_PRESENT_REFUTE=1, debug). Every frame,
 * PEEK the push ring (the step the UI's next read will consume, cursor
 * untouched) and validate the schedule-vs-pull equivalence the ring must
 * uphold:
 *   - seq monotonicity / no skipped-then-resurrected frames: the peeked
 *     (soon-consumed) seq never regresses below one already handed out.
 *   - handle/eventvalue equality where comparable: when the pull path
 *     would return this exact seq, both MUST describe identical content.
 *     For an unpaced single-compositor frame (interp off / hitch) that is
 *     the full texture+value+dims check. For a paced interpolation step it
 *     is only the shared event object + dims: push and pull each run their
 *     own MetalFX submit for the step, handing pixel-equivalent outputs in
 *     different allocations (the texture/value pointers legitimately
 *     differ; that content is instead equal by construction — both copy the
 *     same render_display interp output). The monotonicity check still
 *     covers every consumed step. Prints a running tally every ~5 s. No-op
 *     when push is off. Enabling this re-introduces the pull round trip for
 *     cross-checking — measure perf with it OFF.
 */
static bool push_present_refute_enabled(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("XEMU_PUSH_PRESENT_REFUTE");
        cached = (e && e[0] == '1') ? 1 : 0;
    }
    return cached == 1;
}

static void push_present_refute_step(void)
{
    /* Peek the step the UI's next read will consume (cursor untouched). */
    NV2APresentFrame pf = { 0 };
    if (!nv2a_get_present_frame_peek(&pf)) {
        return; /* push inactive / nothing published — nothing to refute */
    }
    void *p_tex = pf.mtl_texture, *p_ios = pf.iosurface, *p_ev = pf.event;
    uint64_t p_evv = pf.event_value, p_seq = pf.frame_seq;
    uint64_t p_dur = pf.display_duration_ns;
    int p_w = pf.width, p_h = pf.height;
    if (pf.mtl_texture) {
        xemu_metal_release_handle(pf.mtl_texture);
    } else if (pf.iosurface) {
        xemu_metal_release_handle(pf.iosurface);
    }
    nv2a_release_framebuffer_surface();

    static uint64_t compared, mismatches, resurrections, peeked, last_seq;
    static bool have_last;

    peeked++;
    if (have_last && p_seq < last_seq) {
        resurrections++;
        if (resurrections <= 8) {
            fprintf(stderr,
                    "push_refute RESURRECTION seq=%llu < last handed %llu\n",
                    (unsigned long long)p_seq, (unsigned long long)last_seq);
        }
    }
    if (!have_last || p_seq > last_seq) {
        last_seq = p_seq;
        have_last = true;
    }

    /* Content equivalence where comparable: the pull path's frame, when it
     * lands on this exact seq, must be identical. (Enabling the refuter
     * pays the pull round trip purely for this cross-check.) */
    NV2APresentFrame lf = { 0 };
    nv2a_get_present_frame(&lf);
    void *l_tex = lf.mtl_texture, *l_ios = lf.iosurface, *l_ev = lf.event;
    uint64_t l_evv = lf.event_value, l_seq = lf.frame_seq;
    int l_w = lf.width, l_h = lf.height;
    nv2a_release_framebuffer_surface();

    if (p_seq && p_seq == l_seq) {
        compared++;
        /*
         * A paced entry (p_dur > 0) is an interpolation step. Push and pull
         * are independent steppers there: each runs its OWN MetalFX submit
         * for the step, so the two hand pixel-equivalent outputs in
         * DIFFERENT texture allocations with per-submit event values — the
         * texture/value pointers legitimately differ (observed: value off by
         * exactly 1). Only the shared event object, dims, and seq order are
         * comparable. The full texture/value identity check applies to
         * unpaced single-compositor-output frames (interp off, hitch), where
         * both paths hand the same underlying frame.
         */
        bool bad = p_ev != l_ev || p_w != l_w || p_h != l_h;
        if (p_dur == 0) {
            bad = bad || p_tex != l_tex || p_ios != l_ios || p_evv != l_evv;
        }
        if (bad) {
            mismatches++;
            if (mismatches <= 8) {
                fprintf(stderr,
                        "push_refute MISMATCH seq=%llu dur=%llu tex %p/%p "
                        "ios %p/%p ev %p/%p val %llu/%llu dim %dx%d/%dx%d\n",
                        (unsigned long long)p_seq, (unsigned long long)p_dur,
                        p_tex, l_tex, p_ios, l_ios, p_ev, l_ev,
                        (unsigned long long)p_evv, (unsigned long long)l_evv,
                        p_w, p_h, l_w, l_h);
            }
        }
    }
    uint64_t now_ms = SDL_GetTicks();
    static uint64_t last_report_ms;
    if (now_ms - last_report_ms >= 5000) {
        last_report_ms = now_ms;
        fprintf(stderr,
                "push_refute: peeked=%llu compared=%llu mismatches=%llu "
                "resurrections=%llu\n",
                (unsigned long long)peeked, (unsigned long long)compared,
                (unsigned long long)mismatches,
                (unsigned long long)resurrections);
    }
}

/**
 * Metal-native sibling of gl_render_frame: same structure (pull the
 * NV2A present frame, run the HUD, release, present), but the game
 * frame is consumed as an IOSurface wrapped into an MTLTexture and
 * drawn into the CAMetalLayer drawable instead of through GL.
 */
static void metal_render_frame(struct xemu_console *scon)
{
    static bool rendering;
    if (qatomic_xchg(&rendering, true) || qatomic_read(&qemu_exiting)) {
        return;
    }

    /*
     * XEMU_UI_FRAME_CAP_NS (test-only, zero cost unset): floor the UI
     * frame period to simulate a vsynced display from an unfocused
     * bench window (occluded/background windows present unpaced here,
     * so push-present ring pacing cannot be reproduced without it —
     * e.g. 16666666 simulates a 60 Hz consumer).
     */
    static int64_t ui_cap_ns = -1;
    if (unlikely(ui_cap_ns < 0)) {
        const char *e = getenv("XEMU_UI_FRAME_CAP_NS");
        ui_cap_ns = e ? atoll(e) : 0;
        if (ui_cap_ns < 0) {
            ui_cap_ns = 0;
        }
    }
    if (unlikely(ui_cap_ns > 0)) {
        static uint64_t last_present_ns;
        uint64_t now_ns = SDL_GetTicksNS();
        if (now_ns - last_present_ns < (uint64_t)ui_cap_ns) {
            qatomic_set(&rendering, false);
            return;
        }
        last_present_ns = now_ns;
    }

    if (push_present_refute_enabled()) {
        push_present_refute_step();
    }

    /*
     * Pull the present frame from the renderer first: the frame's
     * shared-event value must be known before the drawable render
     * pass is opened (the GPU-side wait is encoded ahead of it).
     * Returned pointers are borrowed: valid until
     * nv2a_release_framebuffer_surface(), and the MTLTexture (wrap or
     * MetalFX ring entry) is retained by the display state / command
     * buffer for as long as the GPU needs it.
     */
    NV2APresentFrame frame = { 0 };
    /*
     * Push model (XEMU_PUSH_PRESENT): read the frame the PFIFO thread
     * published at the last flip with no cross-thread round trip. Falls
     * back to the pull handshake when push is inactive, on the GL
     * backend, or before the first publish (startup / post-resize) —
     * `pushed` stays false and the pull path runs exactly as before.
     */
    bool pushed = nv2a_get_present_frame_pushed(&frame);
    if (!pushed) {
        nv2a_get_present_frame(&frame);
    }

    /*
     * Paced presentation: during gameplay (no menu capture) with
     * frame interpolation active, don't re-present unchanged content.
     * Presenting duplicates would reset the reference frame that
     * presentDrawable:afterMinimumDuration: paces against, and the
     * HUD overlays (notifications, menubar fade) animate fine at the
     * step cadence (>= 60 Hz). Menus keep full-rate rendering.
     *
     * Staleness cap: if the published frame hasn't changed for a
     * while (title stopped flipping — loading screen, pause), drop
     * back to normal full-rate rendering so the HUD stays live and
     * menu shortcuts (processed inside xemu_hud_update) keep working.
     */
    static uint64_t last_frame_seq;
    static uint64_t last_seq_change_ms;
    uint64_t now_ms = SDL_GetTicks();
    if (frame.frame_seq != last_frame_seq) {
        last_frame_seq = frame.frame_seq;
        last_seq_change_ms = now_ms;
    } else if (frame.frame_seq &&
               (frame.display_duration_ns > 0 || pushed) &&
               now_ms - last_seq_change_ms < 250) {
        /*
         * Same content as last present: skip re-compositing. Under push
         * this is the frame_seq dedup the pull model structurally could
         * not do (it had to pay the round trip to learn frame_seq). The
         * 250 ms staleness cap + capture check keep the HUD and menus
         * live when the title stops flipping (loading / pause).
         */
        int kbd = 0, mouse = 0;
        xemu_hud_should_capture_kbd_mouse(&kbd, &mouse);
        if (!kbd && !mouse) {
            if (pushed && frame.mtl_texture) {
                xemu_metal_release_handle(frame.mtl_texture);
            } else if (pushed && frame.iosurface) {
                xemu_metal_release_handle(frame.iosurface);
            }
            nv2a_release_framebuffer_surface();
            qatomic_set(&rendering, false);
            /* Poll for the next step without spinning the handshake */
            SDL_DelayNS(1000000);
            return;
        }
    }

    bool flip_required = false;
    uintptr_t tex = 0;
    if (frame.mtl_texture) {
        tex = (uintptr_t)frame.mtl_texture;
        if (!pushed) {
            /* Pull: borrowed (renderer-owned) pointer; take our own
             * retain for the rest of the frame. Push already holds a
             * slot-read retain in `frame` that serves the same role and
             * is dropped by the release_handle at end-of-frame. */
            xemu_metal_retain_handle(frame.mtl_texture);
        }
    } else if (frame.iosurface) {
        /* The wrap cache holds its own texture reference, and the
         * texture retains the IOSurface. */
        tex = (uintptr_t)xemu_metal_wrap_iosurface(frame.iosurface);
        if (pushed) {
            /* Drop the slot-read retain now the wrap holds its own. */
            xemu_metal_release_handle(frame.iosurface);
        }
    }

    /*
     * Done with the renderer's borrowed pointers — release the
     * framebuffer handshake *before* acquiring a drawable so the
     * PFIFO thread is never coupled to nextDrawable stalls (vsync /
     * occlusion). The shared event outlives subsystem teardowns
     * (sticky), and our retain/wrap covers the texture.
     */
    nv2a_release_framebuffer_surface();

    if (!xemu_metal_begin_frame(frame.event,
                                tex ? frame.event_value : 0)) {
        /* No drawable available (e.g. window fully occluded). */
        if (frame.mtl_texture) {
            xemu_metal_release_handle(frame.mtl_texture);
        }
        qatomic_set(&rendering, false);
        return;
    }

    /* GPU-enforced hold time for paced interpolation steps */
    xemu_metal_set_present_duration(frame.display_duration_ns);

    if (!tex) {
        /* VGA fallback (pixman surface, software rendering). */
        xemu_main_loop_lock();
        DisplaySurface *surface = scon->surface;
        if (surface &&
            surface_bytes_per_pixel(surface) == 4) {
            tex = (uintptr_t)xemu_metal_upload_vga_surface(
                surface_data(surface), surface_width(surface),
                surface_height(surface), surface_stride(surface));
            flip_required = true;
        } else if (surface) {
            static bool warned_once;
            if (!warned_once) {
                warned_once = true;
                fprintf(stderr,
                        "xemu-metal: unsupported VGA fallback format "
                        "(%d bpp); frame skipped\n",
                        surface_bytes_per_pixel(surface) * 8);
            }
        }
        xemu_main_loop_unlock();
    }

    xemu_snapshots_set_framebuffer_texture(tex, flip_required);
    xemu_hud_set_framebuffer_texture(tex, flip_required);

    int64_t nsprof_lock_t0 = nsprof_begin();
    xemu_main_loop_lock();
    xemu_hud_update();
    xemu_main_loop_unlock();
    nsprof_end(NSPROF_UI_HUD_LOCK, nsprof_lock_t0);

    xemu_hud_render();

    xemu_metal_end_frame();

    /* Presented-frame cadence: end_frame -> end_frame. */
    static int64_t nsprof_last_present_t0 = -1;
    if (nsprof_last_present_t0 >= 0) {
        nsprof_end(NSPROF_UI_PRESENT_PERIOD, nsprof_last_present_t0);
    }
    nsprof_last_present_t0 = nsprof_begin();
    nsprof_event(NSPROF_EV_UI_PRESENT);

    if (frame.mtl_texture) {
        xemu_metal_release_handle(frame.mtl_texture);
    }

    qatomic_set(&rendering, false);

#if XEMU_LOCK_STATS
    report_stats();
#endif
}
#endif /* __APPLE__ */

static void render_frame(struct xemu_console *scon)
{
#ifdef __APPLE__
    if (xemu_present_is_metal()) {
        metal_render_frame(scon);
        return;
    }
#endif
    gl_render_frame(scon);
}

static bool event_watch_callback(void *userdata, SDL_Event *event)
{
    struct xemu_console *scon = (struct xemu_console *)userdata;

    if (xemu_headless_mode()) {
        return true;
    }

    if (event->type == SDL_EVENT_WINDOW_RESIZED) {
#ifdef _WIN32
        if (win32_dxgi_present_is_active()) {
            int width;
            int height;
            if (!SDL_GetWindowSizeInPixels(scon->real_window, &width,
                                           &height)) {
                fprintf(stderr, "SDL_GetWindowSizeInPixels failed "
                                "responding to resize event.\n");
            } else {
                win32_dxgi_present_resize(width, height);
            }
        }
#endif
        /* render_frame() dispatches to the Metal or GL presenter. */
        render_frame(scon);
    } else if (event->type == SDL_EVENT_WINDOW_EXPOSED) {
        render_frame(scon);
    }

    return true; // Ignored
}

static void poll_events(struct xemu_console *scon)
{
    SDL_Event ev1, *ev = &ev1;
    bool allow_close = true;
    /* Headless never draws the HUD, so ImGui must not queue input. */
    bool hud = !xemu_headless_mode();

    int kbd = 0, mouse = 0;
    if (hud) {
        xemu_hud_should_capture_kbd_mouse(&kbd, &mouse);
    }

    while (SDL_PollEvent(ev)) {
        xemu_main_loop_lock();

        if (hud) {
            xemu_hud_process_sdl_events(ev);
        }
        xemu_input_process_sdl_events(ev);

        switch (ev->type) {
        case SDL_EVENT_KEY_DOWN:
            if (kbd) break;
            handle_keydown(ev);
            break;
        case SDL_EVENT_KEY_UP:
            if (kbd) break;
            handle_keyup(ev);
            break;
        case SDL_EVENT_QUIT:
            if (scon->opts->has_window_close && !scon->opts->window_close) {
                allow_close = false;
            }
            if (allow_close) {
                shutdown_action = SHUTDOWN_ACTION_POWEROFF;
                qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_UI);
            }
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (mouse) break;
            handle_mousemotion(ev);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (mouse) break;
            handle_mousebutton(ev);
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            if (mouse) break;
            handle_mousewheel(ev);
            break;
        case SDL_EVENT_WINDOW_FIRST ... SDL_EVENT_WINDOW_LAST:
            handle_windowevent(ev);
            break;
        default:
            break;
        }

        xemu_main_loop_unlock();
    }

    xemu_main_loop_lock();
    xemu_input_update_controllers();
    xemu_main_loop_unlock();
}

/*
 * QMP screendump. The renderer's frame can only be read on this (UI)
 * thread and without the BQL, so the QMP coroutine parks a request here
 * and yields; the UI loop writes the file, then a main-loop BH wakes the
 * coroutine. Works windowed and headless. QMP only: HMP waits for
 * coroutine commands with the BQL held, which the UI thread may need.
 */
typedef struct ScreendumpRequest {
    const char *filename;
    bool png;
    Coroutine *co;
    Error *err;
} ScreendumpRequest;

static ScreendumpRequest *screendump_pending;

void coroutine_fn qmp_screendump(const char *filename, const char *device,
                                 bool has_head, int64_t head,
                                 bool has_format, ImageFormat format,
                                 Error **errp)
{
    if (device || has_head) {
        error_setg(errp, "xemu has a single display; omit device and head");
        return;
    }

    ScreendumpRequest req = {
        .filename = filename,
        .png = has_format && format == IMAGE_FORMAT_PNG,
        .co = qemu_coroutine_self(),
    };
    if (qatomic_cmpxchg(&screendump_pending, NULL, &req) != NULL) {
        error_setg(errp, "a screendump is already in progress");
        return;
    }
    qemu_coroutine_yield();
    error_propagate(errp, req.err);
}

static void screendump_done_bh(void *opaque)
{
    ScreendumpRequest *req = opaque;
    aio_co_wake(req->co);
}

/* Fallback when the renderer has no frame: the guest's VGA scanout. */
static bool screendump_vga(struct xemu_console *scon, ScreendumpRequest *req)
{
    bool ok = false;

    xemu_main_loop_lock();
    DisplaySurface *surface = scon->surface;
    if (!surface || surface_bytes_per_pixel(surface) != 4) {
        error_setg(&req->err, "no displayable frame");
    } else {
        int w = surface_width(surface), h = surface_height(surface);
        int stride = surface_stride(surface);
        const uint8_t *src = surface_data(surface);
        uint8_t *rgb = g_malloc((size_t)w * h * 3);
        for (int y = 0; y < h; y++) {
            const uint8_t *s = src + (size_t)y * stride;
            uint8_t *d = rgb + (size_t)y * w * 3;
            for (int x = 0; x < w; x++) {
                d[x * 3 + 0] = s[x * 4 + 2];
                d[x * 3 + 1] = s[x * 4 + 1];
                d[x * 3 + 2] = s[x * 4 + 0];
            }
        }
        ok = xemu_hud_save_rgb(rgb, w, h, req->filename, req->png, &req->err);
        g_free(rgb);
    }
    xemu_main_loop_unlock();
    return ok;
}

static void screendump_service(struct xemu_console *scon)
{
    ScreendumpRequest *req = qatomic_read(&screendump_pending);
    if (!req) {
        return;
    }

    bool done = false;
    if (xemu_headless_mode() != XEMU_HEADLESS_NULL) {
#ifdef __APPLE__
        if (xemu_present_is_metal()) {
            NV2APresentFrame frame = { 0 };
            uintptr_t tex = 0;
            nv2a_get_present_frame(&frame);
            if (frame.mtl_texture) {
                tex = (uintptr_t)frame.mtl_texture;
                xemu_metal_retain_handle(frame.mtl_texture);
            } else if (frame.iosurface) {
                tex = (uintptr_t)xemu_metal_wrap_iosurface(frame.iosurface);
            }
            nv2a_release_framebuffer_surface();
            if (tex) {
                xemu_hud_save_frame(tex, false, req->filename, req->png,
                                    &req->err);
                done = true;
            }
            if (frame.mtl_texture) {
                xemu_metal_release_handle(frame.mtl_texture);
            }
        } else
#endif
        {
            SDL_GL_MakeCurrent(scon->real_window, scon->winctx);
            GLuint tex = nv2a_get_framebuffer_surface();
            if (tex) {
#if defined(__APPLE__)
                xemu_set_framebuffer_texture_is_rect(
                    g_config.display.renderer == CONFIG_DISPLAY_RENDERER_VULKAN);
#endif
                xemu_hud_save_frame(tex, false, req->filename, req->png,
                                    &req->err);
                done = true;
            }
            nv2a_release_framebuffer_surface();
        }
    }
    if (!done) {
        screendump_vga(scon, req);
    }

    qatomic_set(&screendump_pending, NULL);
    aio_bh_schedule_oneshot(qemu_get_aio_context(), screendump_done_bh, req);
}

static void display_very_early_init(DisplayOptions *o)
{
#ifdef __linux__
    /* SDL3 falls back to XWayland on compositors without fifo-v1 [1],
     * breaking HiDPI. Swap may block when occluded, but the BQL is released
     * before swap so emulation is unaffected.
     *
     * [1] https://github.com/libsdl-org/SDL/pull/9383
     */
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "wayland,x11");
#endif

    XemuHeadlessMode headless = xemu_headless_mode();
#ifdef __APPLE__
    if (headless) {
        /* Don't activate on launch (no focus steal). */
        SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP, "1");
    }
#endif

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "Failed to initialize SDL video subsystem: %s\n",
                SDL_GetError());
        exit(1);
    }

#ifdef __APPLE__
    if (headless) {
        xemu_os_set_background_app(); /* no Dock icon */
    }
#endif

#ifdef SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR /* only available since SDL 2.0.8 */
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
#endif
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");

    if (headless == XEMU_HEADLESS_NULL) {
        /* The null renderer draws nothing: no window, presenter or UI. */
        fprintf(stderr, "CPU: %s\n", xemu_get_cpu_info());
        fprintf(stderr, "OS_Version: %s\n", xemu_get_os_info());
        return;
    }

    bool use_metal = xemu_present_is_metal();

    // Initialize rendering context
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(
        SDL_GL_CONTEXT_PROFILE_MASK,
        SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    char *title = g_strdup_printf("xemu | v%s"
#ifdef XEMU_DEBUG_BUILD
                                  " Debug"
#endif
                                  , xemu_version);

    // Decide window size
    int min_window_width = 640;
    int min_window_height = 480;
    int window_width = min_window_width;
    int window_height = min_window_height;

    const int res_table[][2] = {
        {640,  480},
        {720,  480},
        {1280, 720},
        {1280, 800},
        {1280, 960},
        {1920, 1080},
        {2560, 1440},
        {2560, 1600},
        {2560, 1920},
        {3840, 2160}
    };

    if (g_config.display.window.startup_size == CONFIG_DISPLAY_WINDOW_STARTUP_SIZE_LAST_USED) {
        window_width  = g_config.display.window.last_width;
        window_height = g_config.display.window.last_height;
    } else {
        window_width  = res_table[g_config.display.window.startup_size-1][0];
        window_height = res_table[g_config.display.window.startup_size-1][1];
    }

    if (window_width < min_window_width) {
        window_width = min_window_width;
    }
    if (window_height < min_window_height) {
        window_height = min_window_height;
    }

    SDL_WindowFlags window_flags = (SDL_WindowFlags)(
        (use_metal ? SDL_WINDOW_METAL : SDL_WINDOW_OPENGL) |
        SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY |
        (headless ? SDL_WINDOW_HIDDEN : 0));

    // Create main window
    m_window = SDL_CreateWindow(
        title, window_width, window_height,
        window_flags);
    if (m_window == NULL) {
        fprintf(stderr, "Failed to create main window: %s\n", SDL_GetError());
        SDL_Quit();
        exit(1);
    }
    g_free(title);
    SDL_SetWindowMinimumSize(m_window, min_window_width, min_window_height);

    const SDL_DisplayMode *disp_mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(m_window));
    if (disp_mode && (disp_mode->w < window_width || disp_mode->h < window_height)) {
        SDL_SetWindowSize(m_window, min_window_width, min_window_height);
        SDL_SetWindowPosition(m_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }

#ifdef __APPLE__
    if (use_metal) {
        m_context = NULL;
        if (!xemu_metal_init(m_window)) {
            show_error_box(
                "Unable to initialize Metal",
                "Unable to initialize the Metal presentation backend.\r\n"
                "Set display.window.presentation_backend = 'opengl' in\r\n"
                "xemu.toml to fall back to OpenGL presentation.\r\n"
                "\r\n"
                "xemu cannot continue and will now exit.");
            SDL_DestroyWindow(m_window);
            SDL_Quit();
            exit(1);
        }
        xemu_metal_set_vsync(g_config.display.window.vsync);
    } else
#endif
    {
        m_context = SDL_GL_CreateContext(m_window);

        if (m_context != NULL && epoxy_gl_version() < 40) {
            SDL_GL_MakeCurrent(NULL, NULL);
            SDL_GL_DestroyContext(m_context);
            m_context = NULL;
        }

        if (m_context == NULL) {
            show_error_box(
                "Unable to create OpenGL context",
                "Unable to create OpenGL context. This usually means the\r\n"
                "graphics device on this system does not support OpenGL 4.0.\r\n"
                "\r\n"
                "xemu cannot continue and will now exit.");
            SDL_DestroyWindow(m_window);
            SDL_Quit();
            exit(1);
        }
    }

    int width, height, channels = 0;
    stbi_set_flip_vertically_on_load(0);
    unsigned char *icon_data = stbi_load_from_memory(xemu_64x64_data, xemu_64x64_size, &width, &height, &channels, 4);
    if (icon_data) {
        SDL_Surface *icon = SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_RGBA32, icon_data, width*4);
        if (icon) {
            SDL_SetWindowIcon(m_window, icon);
        }
        // Note: Retaining the memory allocated by stbi_load. It's used in place
        // by the SDL surface.
    }

    fprintf(stderr, "CPU: %s\n", xemu_get_cpu_info());
    fprintf(stderr, "OS_Version: %s\n", xemu_get_os_info());
#ifdef __APPLE__
    if (use_metal) {
        fprintf(stderr, "MTL_DEVICE: %s\n", xemu_metal_device_name());
    } else
#endif
    {
        fprintf(stderr, "GL_VENDOR: %s\n", glGetString(GL_VENDOR));
        fprintf(stderr, "GL_RENDERER: %s\n", glGetString(GL_RENDERER));
        fprintf(stderr, "GL_VERSION: %s\n", glGetString(GL_VERSION));
        fprintf(stderr, "GL_SHADING_LANGUAGE_VERSION: %s\n", glGetString(GL_SHADING_LANGUAGE_VERSION));
    }

    /*
     * Initialize offscreen rendering contexts now. Under the Metal
     * backend there is no main-window GL context to share with: the
     * first gloffscreen context created becomes the share-group root
     * for the rest (each glo context is current when the next one is
     * created), which is all the NV2A GL/Vulkan renderers need — the
     * UI no longer consumes GL texture names.
     */
    nv2a_context_init();
    SDL_GL_MakeCurrent(NULL, NULL);
}

static void display_early_init(DisplayOptions *o)
{
    assert(o->type == DISPLAY_TYPE_XEMU);
    display_opengl = 1;

    if (xemu_headless_mode() == XEMU_HEADLESS_NULL) {
        return; /* no window, so no HUD */
    }

    if (!xemu_present_is_metal()) {
        SDL_GL_MakeCurrent(m_window, m_context);
        SDL_GL_SetSwapInterval(g_config.display.window.vsync ? 1 : 0);
#ifdef _WIN32
        /* Upstream's DXGI swapchain + WGL_NV_DX_interop presenter: GL
         * swaps go through D3D11 so a vsync wait can't stall the pfifo
         * thread behind the driver's GL swap lock. Only the GL presenter
         * reaches here; the Metal presenter is Apple-only. */
        if (win32_dxgi_present_enabled()) {
            win32_dxgi_present_init(m_window);
        } else {
            fprintf(stderr, "win32_dxgi_present: disabled by XEMU_WIN32_DXGI=0, "
                            "presenting with SDL_GL_SwapWindow\n");
        }
#endif
    }
    xemu_hud_init(m_window, m_context);
}

static const DisplayChangeListenerOps dcl_gl_ops = {
    .dpy_name                = "xemu-gl",
    .dpy_gfx_switch          = gl_switch,
    .dpy_gfx_check_format    = xb_console_gl_check_format,
    .dpy_mouse_set           = mouse_warp,
    .dpy_cursor_define       = mouse_define,
};

static void display_init(DisplayState *ds, DisplayOptions *o)
{
    uint8_t data = 0;
    int i;

    assert(o->type == DISPLAY_TYPE_XEMU);
    if (!xemu_present_is_metal()) {
        SDL_GL_MakeCurrent(m_window, m_context);
    }

    gui_fullscreen = o->has_full_screen && o->full_screen;
    gui_fullscreen |= g_config.display.window.fullscreen_on_startup;
    if (xemu_headless_mode()) {
        gui_fullscreen = 0; /* going fullscreen would show the window */
    }

    num_outputs = 1;
    scon_list = g_new0(struct xemu_console, num_outputs);
    for (i = 0; i < num_outputs; i++) {
        QemuConsole *con = qemu_console_lookup_by_index(i);
        assert(con != NULL);
        if (!qemu_console_is_graphic(con) &&
            qemu_console_get_index(con) != 0) {
            scon_list[i].hidden = true;
        }
        scon_list[i].idx = i;
        scon_list[i].opts = o;
        scon_list[i].dcl.ops = &dcl_gl_ops;
        scon_list[i].dcl.con = con;
        scon_list[i].kbd = qkbd_state_init(con);
        register_displaychangelistener(&scon_list[i].dcl);

#if defined(SDL_VIDEO_DRIVER_WINDOWS)
        HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(scon_list[i].real_window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
        if (hwnd) {
            qemu_console_set_window_id(con, (uintptr_t)hwnd);
        }
#elif defined(SDL_VIDEO_DRIVER_X11)
        Window xwindow = (Window)SDL_GetNumberProperty(SDL_GetWindowProperties(scon_list[i].real_window), SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
        if (xwindow) {
            qemu_console_set_window_id(con, xwindow);
        }
#endif
    }

    scon_list[0].real_window = m_window;
    scon_list[0].winctx = m_context;

    mouse_mode_notifier.notify = mouse_mode_change;
    qemu_add_mouse_mode_change_notifier(&mouse_mode_notifier);

    sdl_cursor_hidden = SDL_CreateCursor(&data, &data, 8, 1, 0, 0);
    sdl_cursor_normal = SDL_GetCursor();

    // SDL_PollEvent may block during main window resize or drag operations.
    // Register event watch to handle rendering during these operations.
    SDL_AddEventWatch(event_watch_callback, &scon_list[0]);

    if (icount_enabled()) {
        vblank_virtual = true;
        vblank_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                    vblank_virtual_callback, &scon_list[0]);
        timer_mod_ns(vblank_timer, vblank_virtual_next());
        qemu_add_vm_change_state_handler(vblank_virtual_vm_state_change, NULL);
    } else if (use_vblank_timer_thread) {
        qemu_thread_create(&vblank_thread, "vblank-timer", vblank_timer_thread,
                           &scon_list[0], QEMU_THREAD_JOINABLE);
    } else {
        vblank_timer = timer_new_ns(QEMU_CLOCK_REALTIME, vblank_timer_callback, &scon_list[0]);
        timer_mod_ns(vblank_timer, qemu_clock_get_ns(QEMU_CLOCK_REALTIME) + vblank_interval_ns);
    }

    /* Tell main thread to go ahead and create the app and enter the run loop */
    if (!xemu_present_is_metal()) {
        SDL_GL_MakeCurrent(NULL, NULL);
    }
    qemu_sem_post(&display_init_sem);
}

static void display_finalize(void)
{
    if (use_vblank_timer_thread && !vblank_virtual) {
        qemu_thread_join(&vblank_thread);
    } else if (vblank_timer) {
        timer_free(vblank_timer);
        vblank_timer = NULL;
    }

    SDL_RemoveEventWatch(event_watch_callback, &scon_list[0]);
#ifdef __APPLE__
    if (xemu_present_is_metal()) {
        xemu_metal_finalize();
    } else
#endif
    {
#ifdef _WIN32
        win32_dxgi_present_cleanup();
#endif
        SDL_GL_MakeCurrent(NULL, NULL);
        SDL_GL_DestroyContext(m_context);
    }
    SDL_DestroyWindow(m_window);
    SDL_Quit();
}

static QemuDisplay qemu_display_xemu = {
    .type       = DISPLAY_TYPE_XEMU,
    .early_init = display_early_init,
    .init       = display_init,
};

static void register_xemu_display(void)
{
    qemu_display_register(&qemu_display_xemu);
}

type_init(register_xemu_display);

int gArgc;
char **gArgv;

static void *qemu_main(void *opaque)
{
    /*
     * qemu_init() and the main loop run here, not on the process's first
     * thread, so rcu_init()'s constructor-time registration does not cover
     * them. Register so the main loop's RCU readers (address_space_*,
     * device DMA) are visible to synchronize_rcu() instead of being
     * invisible to it. Once only: rcu_register_thread() does not detect a
     * second registration of the same thread.
     */
    rcu_register_thread();

    qemu_init(gArgc, gArgv);
#ifdef __APPLE__
    if (icount_enabled()) {
        /*
         * Deterministic mode: with sleep=off an idle vCPU waits for this
         * thread to warp the virtual clock, so it is frame-critical too.
         */
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    }
#endif
    exit_status = qemu_main_loop();
    qatomic_set(&qemu_exiting, true);
    bql_unlock();
    qemu_mutex_unlock_main_loop();

    qemu_sem_wait(&display_shutdown_sem);
    bql_lock();
    qemu_cleanup(exit_status);
    bql_unlock();

    rcu_unregister_thread();
    return NULL;
}

#ifdef _WIN32
static const wchar_t *get_executable_name(void)
{
    static wchar_t exe_name[MAX_PATH] = { 0 };
    static bool initialized = false;

    if (!initialized) {
        wchar_t full_path[MAX_PATH];
        DWORD length = GetModuleFileNameW(NULL, full_path, MAX_PATH);
        if (length == 0 || length == MAX_PATH) {
            return NULL;
        }

        wchar_t *last_slash = wcsrchr(full_path, L'\\');
        if (last_slash) {
            wcsncpy_s(exe_name, MAX_PATH, last_slash + 1, _TRUNCATE);
        } else {
            wcsncpy_s(exe_name, MAX_PATH, full_path, _TRUNCATE);
        }

        initialized = true;
    }

    return exe_name;
}

static void setup_nvidia_profile(void)
{
    const wchar_t *exe_name = get_executable_name();
    if (exe_name == NULL) {
        fprintf(stderr, "Failed to get current executable name\n");
        return;
    }

    if (nvapi_init()) {
        nvapi_setup_profile((NvApiProfileOpts){
            .profile_name = L"xemu",
            .executable_name = exe_name,
            .threaded_optimization = false,
            .present_method = OGL_CPL_PREFER_DXPRESENT_PREFER_DISABLED,
        });
        nvapi_finalize();
    }
}
#endif

static void init_sdl_app_metadata(void)
{
    SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_NAME_STRING, "xemu");
    SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_VERSION_STRING,
                               xemu_version);
    SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_IDENTIFIER_STRING,
                               "app.xemu.xemu");
    SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_URL_STRING,
                               "https://xemu.app");
}

static int configured_renderer;

static void restore_configured_renderer(void)
{
    g_config.display.renderer = configured_renderer;
}

int main(int argc, char **argv)
{
    QemuThread thread;

    setlocale(LC_NUMERIC, "C");

#ifdef __APPLE__
    /*
     * MoltenVK configuration must not depend on how the app was
     * launched: Info.plist LSEnvironment is applied only by
     * LaunchServices (Finder/`open`), so terminal launches of the
     * same binary previously ran MoltenVK defaults — that split hid
     * a texture-corruption bug for weeks (Finder ran prefill=2 and
     * flashed magenta; every harness run was clean). Set the tested
     * configuration here, before MoltenVK is dlopen'd, with
     * overwrite=0 so an explicit user override still wins. Keep in
     * sync with Info.plist. Prefill MUST stay 0: immediate encoding
     * corrupts streamed textures now that command buffers span
     * whole frames, enabled an AGX visibility crash, and measures
     * slower (encoding lands on the PFIFO thread).
     */
    setenv("MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS", "0", 0);
    setenv("MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS", "1", 0);
    setenv("MVK_CONFIG_FAST_MATH_ENABLED", "1", 0);
    setenv("MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS", "0", 0);
    setenv("MVK_CONFIG_RESUME_LOST_DEVICE", "1", 0);
    /*
     * Warnings, not INFO (the driver default, off the hot path). Not errors
     * only: with RESUME_LOST_DEVICE, MoltenVK reports a command buffer that
     * failed on the GPU as a warning, so errors-only hid every fault until
     * macOS stopped accepting the process's work and the device was lost.
     */
    setenv("MVK_CONFIG_LOG_LEVEL", "2", 0);
    /*
     * NB: MVK_CONFIG_SHOULD_MAXIMIZE_CONCURRENT_COMPILATION was tried and
     * reverted — measured inert on macOS 26 / M2 Ultra (the compile-task
     * ceiling is already 24 by default and pipeline creation is serial, so
     * nothing issues concurrent compiles). Reintroduce it only alongside a
     * concurrent-compile producer (prewarm). See
     * docs/Future/moltenvk-optimization-experiments.md.
     */
#endif

#ifdef _WIN32
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        // Launched with a console. If stdout and stderr are not associated with
        // an output stream, redirect to parent console.
        if (_fileno(stdout) == -2) {
            freopen("CONOUT$", "w+", stdout);
        }
        if (_fileno(stderr) == -2) {
            freopen("CONOUT$", "w+", stderr);
        }
    } else {
        // Launched without a console. Redirect stdout and stderr to a log file.
        HANDLE logfile = CreateFileA("xemu.log",
            GENERIC_WRITE, FILE_SHARE_WRITE|FILE_SHARE_READ,
            NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (logfile != INVALID_HANDLE_VALUE) {
            freopen("xemu.log", "a", stdout);
            freopen("xemu.log", "a", stderr);
        }
    }

    _set_error_mode(_OUT_TO_STDERR);
#endif

    fprintf(stderr, "xemu_version: %s\n", xemu_version);
    fprintf(stderr, "xemu_commit: %s\n", xemu_commit);
    fprintf(stderr, "xemu_date: %s\n", xemu_date);

    XemuHeadlessMode headless = xemu_headless_mode();
    if (headless) {
        fprintf(stderr, "xemu: headless mode: %s\n",
                headless == XEMU_HEADLESS_NULL ? "null" : "offscreen");
    }

    init_sdl_app_metadata();

    gArgc = argc;
    gArgv = argv;

    for (int i = 1; i < argc; i++) {
        if (argv[i] && strcmp(argv[i], "-config_path") == 0) {
            argv[i] = NULL;
            if (i < argc - 1 && argv[i+1]) {
                xemu_settings_set_path(argv[i+1]);
                argv[i+1] = NULL;
            }
            break;
        }
    }

    if (!xemu_settings_load()) {
        const char *err_msg = xemu_settings_get_error_message();
        fprintf(stderr, "%s", err_msg);
        show_error_box("Failed to load xemu config file", err_msg);
        SDL_Quit();
        exit(1);
    }
    atexit(xemu_settings_save);

    if (headless == XEMU_HEADLESS_NULL) {
        /*
         * Null renderer for this run only. atexit handlers run in reverse,
         * so the configured renderer is back before the exit-time save.
         */
        configured_renderer = g_config.display.renderer;
        g_config.display.renderer = CONFIG_DISPLAY_RENDERER_NULL;
        atexit(restore_configured_renderer);
    }

#ifdef _WIN32
    if (g_config.display.setup_nvidia_profile) {
        setup_nvidia_profile();
    }
#endif

    display_very_early_init(NULL);

    qemu_sem_init(&display_init_sem, 0);
    qemu_sem_init(&display_shutdown_sem, 0);
    qemu_thread_create(&thread, "qemu_main", qemu_main,
                       NULL, QEMU_THREAD_JOINABLE);
    qemu_sem_wait(&display_init_sem);

    gui_grab = 0;
    if (gui_fullscreen) {
        grab_start(0);
        set_full_screen(&scon_list[0], gui_fullscreen);
    }

    /*
     * FIXME: May want to create a callback mechanism for main QEMU thread
     * to just run functions to avoid TLS bugs and locking issues.
     */
    tcg_register_init_ctx();
    qemu_set_current_aio_context(qemu_get_aio_context());

    xemu_main_loop_lock();
    xemu_input_init();
    xemu_main_loop_unlock();
    /* Machine and pads exist: a harness may loadvm or migrate-incoming now. */
    fprintf(stderr, "xemu: ready\n");

    struct xemu_console *scon = &scon_list[0];
    while (!qatomic_read(&qemu_exiting)) {
        poll_events(scon);
        screendump_service(scon);
        if (headless) {
            /* Nothing to present; keep input and screendump responsive. */
            SDL_DelayNS(vblank_interval_ns);
        } else {
            render_frame(scon);
        }
    }
    qemu_sem_post(&display_shutdown_sem);
    qemu_thread_join(&thread);
    display_finalize();
    return exit_status;
}

void xemu_eject_disc(Error **errp)
{
    Error *error = NULL;

    xbox_smc_eject_button();
    xemu_settings_set_string(&g_config.sys.files.dvd_path, "");

    // Xbox software may request that the drive open, but do it now anyway
    qmp_eject("ide0-cd1", NULL, true, false, &error);
    if (error) {
        error_propagate(errp, error);
    }

    xbox_smc_update_tray_state();
}

void xemu_load_disc(const char *path, Error **errp)
{
    Error *error = NULL;

    // Ensure an eject sequence is always triggered so Xbox software reloads
    xbox_smc_eject_button();
    xemu_settings_set_string(&g_config.sys.files.dvd_path, "");

    qmp_blockdev_change_medium("ide0-cd1", NULL, path, "raw", false, false,
                               false, 0, &error);
    if (error) {
        error_propagate(errp, error);
    } else {
        xemu_settings_set_string(&g_config.sys.files.dvd_path, path);
    }

    xbox_smc_update_tray_state();
}
