//
// xemu User Interface
//
// Copyright (C) 2020-2022 Matt Borgerson
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include <SDL3/SDL.h>
#include <epoxy/gl.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <functional>
#include <assert.h>
#include <fpng.h>

#include <deque>
#include <vector>
#include <string>
#include <memory>

#include "actions.hh"
#include "common.hh"
#include "xemu-hud.h"
#include "misc.hh"
#include "gl-helpers.hh"
#include "input-manager.hh"
#include "snapshot-manager.hh"
#include "viewport-manager.hh"
#include "font-manager.hh"
#include "scene.hh"
#include "scene-manager.hh"
#include "main-menu.hh"
#include "popup-menu.hh"
#include "notifications.hh"
#include "monitor.hh"
#include "debug.hh"
#include "welcome.hh"
#include "menubar.hh"
#include "compat.hh"
#include "game-library-window.hh"
#include "game-patches-window.hh"
#include "xbox-drive-explorer.hh"

#ifdef __APPLE__
#include "metal-helpers.hh"
extern "C" {
#include "ui/xemu-present.h"
#include "ui/xemu-metal.h"
}
#include <mach/mach.h>
#include <sys/resource.h>
#include <time.h>
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#endif

bool g_screenshot_pending;
const char *g_snapshot_pending_load_name;

float g_main_menu_height;

static ImGuiStyle g_base_style;
static float g_last_scale;
static int g_vsync;
static uintptr_t g_tex;
static bool g_flip_req;
static void *g_tex_wait_event;
static uint64_t g_tex_wait_value;


static void InitializeStyle()
{
    g_font_mgr.Rebuild();

    ImGui::StyleColorsDark();
    ImVec4 *c = ImGui::GetStyle().Colors;
    c[ImGuiCol_Text]                  = ImVec4(0.94f, 0.94f, 0.94f, 1.00f);
    c[ImGuiCol_TextDisabled]          = ImVec4(0.86f, 0.93f, 0.89f, 0.28f);
    c[ImGuiCol_WindowBg]              = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    c[ImGuiCol_ChildBg]               = ImVec4(0.06f, 0.06f, 0.06f, 0.98f);
    c[ImGuiCol_PopupBg]               = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    c[ImGuiCol_Border]                = ImVec4(0.11f, 0.11f, 0.11f, 0.60f);
    c[ImGuiCol_BorderShadow]          = ImVec4(0.16f, 0.16f, 0.16f, 0.00f);
    c[ImGuiCol_FrameBg]               = ImVec4(0.18f, 0.18f, 0.18f, 1.00f);
    c[ImGuiCol_FrameBgHovered]        = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    c[ImGuiCol_FrameBgActive]         = ImVec4(0.28f, 0.71f, 0.25f, 1.00f);
    c[ImGuiCol_TitleBg]               = ImVec4(0.20f, 0.51f, 0.18f, 1.00f);
    c[ImGuiCol_TitleBgActive]         = ImVec4(0.26f, 0.66f, 0.23f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed]      = ImVec4(0.16f, 0.16f, 0.16f, 0.75f);
    c[ImGuiCol_MenuBarBg]             = ImVec4(0.14f, 0.14f, 0.14f, 0.00f);
    c[ImGuiCol_ScrollbarBg]           = ImVec4(0.16f, 0.16f, 0.16f, 0.00f);
    c[ImGuiCol_ScrollbarGrab]         = ImVec4(0.30f, 0.30f, 0.30f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.24f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive]   = ImVec4(0.24f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_CheckMark]             = ImVec4(0.26f, 0.66f, 0.23f, 1.00f);
    c[ImGuiCol_SliderGrab]            = ImVec4(0.90f, 0.90f, 0.90f, 1.00f);
    c[ImGuiCol_SliderGrabActive]      = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
    c[ImGuiCol_Button]                = ImVec4(0.17f, 0.17f, 0.17f, 1.00f);
    c[ImGuiCol_ButtonHovered]         = ImVec4(0.24f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_ButtonActive]          = ImVec4(0.26f, 0.66f, 0.23f, 1.00f);
    c[ImGuiCol_Header]                = ImVec4(0.24f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_HeaderHovered]         = ImVec4(0.24f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_HeaderActive]          = ImVec4(0.24f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_Separator]             = ImVec4(1.00f, 1.00f, 1.00f, 0.25f);
    c[ImGuiCol_SeparatorHovered]      = ImVec4(0.13f, 0.87f, 0.16f, 0.78f);
    c[ImGuiCol_SeparatorActive]       = ImVec4(0.25f, 0.75f, 0.10f, 1.00f);
    c[ImGuiCol_ResizeGrip]            = ImVec4(0.47f, 0.83f, 0.49f, 0.04f);
    c[ImGuiCol_ResizeGripHovered]     = ImVec4(0.28f, 0.71f, 0.25f, 0.78f);
    c[ImGuiCol_ResizeGripActive]      = ImVec4(0.28f, 0.71f, 0.25f, 1.00f);
    c[ImGuiCol_Tab]                   = ImVec4(0.26f, 0.67f, 0.23f, 0.95f);
    c[ImGuiCol_TabHovered]            = ImVec4(0.24f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_TabActive]             = ImVec4(0.24f, 0.60f, 0.00f, 1.00f);
    c[ImGuiCol_TabUnfocused]          = ImVec4(0.21f, 0.54f, 0.19f, 0.99f);
    c[ImGuiCol_TabUnfocusedActive]    = ImVec4(0.24f, 0.60f, 0.21f, 1.00f);
    c[ImGuiCol_PlotLines]             = ImVec4(0.86f, 0.93f, 0.89f, 0.63f);
    c[ImGuiCol_PlotLinesHovered]      = ImVec4(0.28f, 0.71f, 0.25f, 1.00f);
    c[ImGuiCol_PlotHistogram]         = ImVec4(0.86f, 0.93f, 0.89f, 0.63f);
    c[ImGuiCol_PlotHistogramHovered]  = ImVec4(0.28f, 0.71f, 0.25f, 1.00f);
    c[ImGuiCol_TextSelectedBg]        = ImVec4(0.26f, 0.66f, 0.23f, 1.00f);
    c[ImGuiCol_DragDropTarget]        = ImVec4(1.00f, 1.00f, 0.00f, 0.90f);
    c[ImGuiCol_NavHighlight]          = ImVec4(0.28f, 0.71f, 0.25f, 1.00f);
    c[ImGuiCol_NavWindowingHighlight] = ImVec4(1.00f, 1.00f, 1.00f, 0.70f);
    c[ImGuiCol_NavWindowingDimBg]     = ImVec4(0.80f, 0.80f, 0.80f, 0.20f);
    c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.16f, 0.16f, 0.16f, 0.73f);

    ImGuiStyle &s = ImGui::GetStyle();
    s.WindowRounding = 6.0;
    s.FrameRounding = 6.0;
    s.PopupRounding = 6.0;
    g_base_style = s;
}

void xemu_hud_init(SDL_Window* window, void* sdl_gl_context)
{
    xemu_monitor_init();
    g_vsync = g_config.display.window.vsync;

    InitCustomRendering();

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = NULL;

    // Setup Platform/Renderer bindings
#ifdef __APPLE__
    if (xemu_present_is_metal()) {
        ImGui_ImplSDL3_InitForMetal(window);
        MetalImGuiInit();
    } else
#endif
    {
        ImGui_ImplSDL3_InitForOpenGL(window, sdl_gl_context);
        ImGui_ImplOpenGL3_Init("#version 150");
    }
    ImPlot::CreateContext();

    g_last_scale = g_viewport_mgr.m_scale;
    InitializeStyle();
    g_main_menu.SetNextViewIndex(g_config.general.last_viewed_menu_index);
    first_boot_window.is_open = g_config.general.show_welcome;
}

void xemu_hud_cleanup(void)
{
#ifdef __APPLE__
    if (xemu_present_is_metal()) {
        MetalImGuiShutdown();
    } else
#endif
    {
        ImGui_ImplOpenGL3_Shutdown();
    }
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

void xemu_hud_process_sdl_events(SDL_Event *event)
{
    // Ignore inputs that are consumed by rebinding
    if (g_main_menu.ConsumeRebindEvent(event)) {
        return;
    }

    ImGui_ImplSDL3_ProcessEvent(event);
}

void xemu_hud_should_capture_kbd_mouse(int *kbd, int *mouse)
{
    ImGuiIO& io = ImGui::GetIO();
    if (kbd) *kbd = io.WantCaptureKeyboard;
    if (mouse) *mouse = io.WantCaptureMouse;
}

void xemu_hud_set_framebuffer_texture(uintptr_t tex, bool flip,
                                      void *wait_event, uint64_t wait_value)
{
    g_tex = tex;
    g_flip_req = flip;
    g_tex_wait_event = wait_event;
    g_tex_wait_value = wait_value;
}

static void DrawPerformanceOverlay();

void xemu_hud_update(void)
{
    ImGuiIO& io = ImGui::GetIO();
    uint32_t now = SDL_GetTicks();

    g_viewport_mgr.Update();
    g_font_mgr.Update();
    if (g_last_scale != g_viewport_mgr.m_scale) {
        ImGuiStyle &style = ImGui::GetStyle();
        style = g_base_style;
        style.ScaleAllSizes(g_viewport_mgr.m_scale);
        g_last_scale = g_viewport_mgr.m_scale;
    }

    if (!first_boot_window.is_open) {
        int ww, wh;
        SDL_GetWindowSizeInPixels(xemu_get_window(), &ww, &wh);
        RenderFramebuffer(g_tex, ww, wh, g_flip_req);
    }

#ifdef __APPLE__
    if (xemu_present_is_metal()) {
        MetalImGuiNewFrame();
    } else
#endif
    {
        ImGui_ImplOpenGL3_NewFrame();
    }
    io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
    ImGui_ImplSDL3_NewFrame();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    g_input_mgr.Update();

    ImGui::NewFrame();
    ProcessKeyboardShortcuts();

#if defined(CONFIG_RENDERDOC)
    if (g_capture_renderdoc_frame) {
        nv2a_dbg_renderdoc_capture_frames(1, false);
        g_capture_renderdoc_frame = false;
    }
#endif

    if (g_config.display.ui.show_menubar && !first_boot_window.is_open) {
        // Auto-hide main menu after 5s of inactivity
        static uint32_t last_check = 0;
        float alpha = 1.0;
        const uint32_t timeout = 5000;
        const float fade_duration = 1000.0;
        bool menu_wakeup = g_input_mgr.MouseMoved();
        if (menu_wakeup) {
            last_check = now;
        }
        if ((now-last_check) > timeout) {
            if (g_config.display.ui.use_animations) {
                float t = fmin((float)((now-last_check)-timeout)/fade_duration, 1);
                alpha = 1-t;
                if (t >= 1) {
                    alpha = 0;
                }
            } else {
                alpha = 0;
            }
        }
        if (alpha > 0.0) {
            ImVec4 tc = ImGui::GetStyle().Colors[ImGuiCol_Text];
            tc.w = alpha;
            ImGui::PushStyleColor(ImGuiCol_Text, tc);
            ImGui::SetNextWindowBgAlpha(alpha);
            ShowMainMenu();
            ImGui::PopStyleColor();
        } else {
            g_main_menu_height = 0;
        }
    }

    static uint32_t last_mouse_move = 0;
    if (g_input_mgr.MouseMoved()) {
        last_mouse_move = now;
    }

    // FIXME: Handle time wrap around
    if (g_config.display.ui.hide_cursor && (now - last_mouse_move) > 3000) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
    }

    if (!ImGui::IsWindowFocused(ImGuiFocusedFlags_AnyWindow) &&
        !g_scene_mgr.IsDisplayingScene()) {

        // If the guide button is pressed, wake the ui
        bool menu_button = false;
        uint32_t buttons = g_input_mgr.CombinedButtons();
        if (buttons & CONTROLLER_BUTTON_GUIDE) {
            menu_button = true;
        }

        // Allow controllers without a guide button to also work
        if ((buttons & CONTROLLER_BUTTON_BACK) &&
            (buttons & CONTROLLER_BUTTON_START)) {
            menu_button = true;
        }

        if (ImGui::IsKeyPressed(ImGuiKey_F1)) {
            g_scene_mgr.PushScene(g_main_menu);
        } else if (ImGui::IsKeyPressed(ImGuiKey_F2)) {
            g_scene_mgr.PushScene(g_popup_menu);
        } else if (menu_button ||
                   (ImGui::IsMouseClicked(ImGuiMouseButton_Right) &&
                    !ImGui::IsAnyItemFocused() && !ImGui::IsAnyItemHovered())) {
            g_scene_mgr.PushScene(g_popup_menu);
        } else if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            xemu_toggle_fullscreen();
        }

        bool mod_key_down = ImGui::IsKeyDown(ImGuiKey_ModShift);
        for (int f_key = 0; f_key < 4; ++f_key) {
            if (ImGui::IsKeyPressed((enum ImGuiKey)(ImGuiKey_F5 + f_key))) {
                ActionActivateBoundSnapshot(f_key, mod_key_down);
                break;
            }
        }
    }

    first_boot_window.Draw();
    monitor_window.Draw();
    apu_window.Draw();
    video_window.Draw();
    compatibility_reporter_window.Draw();
    XboxExplorerUpdate();
    XboxExplorerWindowDraw();
    GamePatchesWindowDraw();
    GameLibraryWindowDraw();
    g_scene_mgr.Draw();
    if (!first_boot_window.is_open) notification_manager.Draw();
    g_snapshot_mgr.Draw();
    if (!first_boot_window.is_open) DrawPerformanceOverlay();

    // static bool show_demo = true;
    // if (show_demo) ImGui::ShowDemoWindow(&show_demo);
}

#ifdef __APPLE__
/*
 * GPU utilization published by the accelerator driver through IOKit.
 * Intel and AMD drivers expose it as "Device Utilization %"; other
 * drivers use different keys, so probe the common ones. Returns a
 * negative value when the driver does not report it.
 */
static double macos_gpu_utilization(void)
{
    io_iterator_t iter;
    if (IOServiceGetMatchingServices(kIOMainPortDefault,
                                     IOServiceMatching("IOAccelerator"),
                                     &iter) != KERN_SUCCESS) {
        return -1.0;
    }

    static const CFStringRef keys[] = {
        CFSTR("Device Utilization %"),
        CFSTR("GPU Activity(%)"),
        CFSTR("GPU Core Utilization"),
    };

    double util = -1.0;
    io_registry_entry_t entry;
    while ((entry = IOIteratorNext(iter)) != 0) {
        CFMutableDictionaryRef props = NULL;
        if (IORegistryEntryCreateCFProperties(entry, &props, kCFAllocatorDefault, 0) == KERN_SUCCESS &&
            props != NULL) {
            CFDictionaryRef stats = (CFDictionaryRef)CFDictionaryGetValue(props, CFSTR("PerformanceStatistics"));
            if (stats != NULL && CFGetTypeID(stats) == CFDictionaryGetTypeID()) {
                for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
                    CFNumberRef num = (CFNumberRef)CFDictionaryGetValue(stats, keys[i]);
                    int v;
                    if (num != NULL && CFGetTypeID(num) == CFNumberGetTypeID() &&
                        CFNumberGetValue(num, kCFNumberIntType, &v)) {
                        util = v;
                        break;
                    }
                }
            }
            CFRelease(props);
        }
        IOObjectRelease(entry);
        if (util >= 0.0) {
            break;
        }
    }
    IOObjectRelease(iter);
    return util;
}

/*
 * CPU time consumed by the whole process (every emulation thread) since
 * the previous call, as a percentage of one core, matching how Activity
 * Monitor reports it.
 */
static double macos_process_cpu_percent(uint64_t *prev_cpu_ns, uint64_t *prev_wall_ns)
{
    struct rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0) {
        return -1.0;
    }

    uint64_t cpu_ns = (uint64_t)ru.ru_utime.tv_sec * 1000000000ULL + (uint64_t)ru.ru_utime.tv_usec * 1000ULL +
                      (uint64_t)ru.ru_stime.tv_sec * 1000000000ULL + (uint64_t)ru.ru_stime.tv_usec * 1000ULL;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t wall_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;

    double percent = -1.0;
    if (*prev_wall_ns != 0 && wall_ns > *prev_wall_ns) {
        percent = 100.0 * (double)(cpu_ns - *prev_cpu_ns) / (double)(wall_ns - *prev_wall_ns);
    }

    *prev_cpu_ns = cpu_ns;
    *prev_wall_ns = wall_ns;
    return percent;
}
#endif

/*
 * Optional performance readout (Settings > Display). Drawn as a small,
 * input-transparent window in the top-left corner, below the menu bar so
 * the two never overlap. CPU and GPU load are sampled at most twice a
 * second: reading the accelerator's utilization walks the IORegistry, so
 * doing it every frame would be wasteful and jittery.
 */
static void DrawPerformanceOverlay()
{
    if (!g_config.display.ui.show_fps) {
        return;
    }

#ifdef __APPLE__
    static uint64_t prev_cpu_ns;
    static uint64_t prev_wall_ns;
    static uint64_t last_sample_ns;
    static double cpu_percent = 0.0;
    static double gpu_percent = -1.0;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    if (last_sample_ns == 0 || now_ns - last_sample_ns >= 500000000ULL) {
        last_sample_ns = now_ns;
        double cpu = macos_process_cpu_percent(&prev_cpu_ns, &prev_wall_ns);
        if (cpu >= 0.0) {
            cpu_percent = cpu;
        }
        double gpu = macos_gpu_utilization();
        if (gpu >= 0.0) {
            gpu_percent = gpu;
        }
    }
#endif

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoInputs;

    ImVec2 offset = g_viewport_mgr.Scale(ImVec2(10.0f, 10.0f));
    ImGui::SetNextWindowPos(ImVec2(offset.x, offset.y + g_main_menu_height),
                            ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.45f);
    if (ImGui::Begin("###xemu_performance_overlay", nullptr, flags)) {
        ImGui::Text("%u FPS", g_nv2a_stats.increment_fps);
#ifdef __APPLE__
        ImGui::Text("CPU %3.0f%%", cpu_percent);
        if (gpu_percent >= 0.0) {
            ImGui::Text("GPU %3.0f%%", gpu_percent);
        }
#endif
    }
    ImGui::End();
}

void xemu_hud_render()
{
    ImGui::Render();
#ifdef __APPLE__
    if (xemu_present_is_metal()) {
        MetalImGuiRenderDrawData(ImGui::GetDrawData());
    } else
#endif
    {
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }

    if (g_vsync != g_config.display.window.vsync) {
        g_vsync = g_config.display.window.vsync;
#ifdef __APPLE__
        if (xemu_present_is_metal()) {
            xemu_metal_set_vsync(g_vsync != 0);
        } else
#endif
        {
            SDL_GL_SetSwapInterval(g_vsync ? 1 : 0);
        }
    }

    if (g_screenshot_pending) {
        /* As screendump does: the compositor may still be on it. */
        SaveScreenshot(g_tex, g_flip_req, g_tex_wait_event, g_tex_wait_value);
        g_screenshot_pending = false;
    }
}
