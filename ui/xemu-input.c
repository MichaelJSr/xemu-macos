/*
 * xemu Input Management
 *
 * Copyright (C) 2020-2021 Matt Borgerson
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#include "qemu/osdep.h"
#include "hw/qdev-core.h"
#include "hw/qdev-properties.h"
#include "qapi/error.h"
#include "monitor/qdev.h"
#include "qobject/qdict.h"
#include "qemu/option.h"
#include "qemu/timer.h"
#include "qemu/cutils.h"
#include "qemu/config-file.h"
#include "exec/icount.h"
#include "system/runstate.h"
#include "migration/vmstate.h"

#include "xemu-input.h"
#include "xemu-notifications.h"
#include "xemu-settings.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "system/blockdev.h"

// #define DEBUG_INPUT

#ifdef DEBUG_INPUT
#define DPRINTF(fmt, ...) \
    do { fprintf(stderr, fmt, ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) \
    do { } while (0)
#endif

#define XEMU_INPUT_MIN_INPUT_UPDATE_INTERVAL_US  2500
#define XEMU_INPUT_MIN_RUMBLE_UPDATE_INTERVAL_US 2500

#if 0
static void xemu_input_print_controller_state(ControllerState *state)
{
    DPRINTF("     A = %d,      B = %d,     X = %d,     Y = %d\n"
           "  Left = %d,     Up = %d, Right = %d,  Down = %d\n"
           "  Back = %d,  Start = %d, White = %d, Black = %d\n"
           "Lstick = %d, Rstick = %d, Guide = %d\n"
           "\n"
           "LTrig   = %.3f, RTrig   = %.3f\n"
           "LStickX = %.3f, RStickX = %.3f\n"
           "LStickY = %.3f, RStickY = %.3f\n\n",
        !!(state->buttons & CONTROLLER_BUTTON_A),
        !!(state->buttons & CONTROLLER_BUTTON_B),
        !!(state->buttons & CONTROLLER_BUTTON_X),
        !!(state->buttons & CONTROLLER_BUTTON_Y),
        !!(state->buttons & CONTROLLER_BUTTON_DPAD_LEFT),
        !!(state->buttons & CONTROLLER_BUTTON_DPAD_UP),
        !!(state->buttons & CONTROLLER_BUTTON_DPAD_RIGHT),
        !!(state->buttons & CONTROLLER_BUTTON_DPAD_DOWN),
        !!(state->buttons & CONTROLLER_BUTTON_BACK),
        !!(state->buttons & CONTROLLER_BUTTON_START),
        !!(state->buttons & CONTROLLER_BUTTON_WHITE),
        !!(state->buttons & CONTROLLER_BUTTON_BLACK),
        !!(state->buttons & CONTROLLER_BUTTON_LSTICK),
        !!(state->buttons & CONTROLLER_BUTTON_RSTICK),
        !!(state->buttons & CONTROLLER_BUTTON_GUIDE),
        state->axis[CONTROLLER_AXIS_LTRIG],
        state->axis[CONTROLLER_AXIS_RTRIG],
        state->axis[CONTROLLER_AXIS_LSTICK_X],
        state->axis[CONTROLLER_AXIS_RSTICK_X],
        state->axis[CONTROLLER_AXIS_LSTICK_Y],
        state->axis[CONTROLLER_AXIS_RSTICK_Y]
        );
}
#endif

ControllerStateList available_controllers =
    QTAILQ_HEAD_INITIALIZER(available_controllers);
ControllerState *bound_controllers[4] = { NULL, NULL, NULL, NULL };
const char *bound_drivers[4] = { DRIVER_DUKE, DRIVER_DUKE, DRIVER_DUKE,
                                 DRIVER_DUKE };
int test_mode;

static const char **port_index_to_settings_key_map[] = {
    &g_config.input.bindings.port1,
    &g_config.input.bindings.port2,
    &g_config.input.bindings.port3,
    &g_config.input.bindings.port4,
};

static const char **port_index_to_driver_settings_key_map[] = {
    &g_config.input.bindings.port1_driver,
    &g_config.input.bindings.port2_driver,
    &g_config.input.bindings.port3_driver, 
    &g_config.input.bindings.port4_driver
};

static int *peripheral_types_settings_map[4][2] = {
    { &g_config.input.peripherals.port1.peripheral_type_0,
      &g_config.input.peripherals.port1.peripheral_type_1 },
    { &g_config.input.peripherals.port2.peripheral_type_0,
      &g_config.input.peripherals.port2.peripheral_type_1 },
    { &g_config.input.peripherals.port3.peripheral_type_0,
      &g_config.input.peripherals.port3.peripheral_type_1 },
    { &g_config.input.peripherals.port4.peripheral_type_0,
      &g_config.input.peripherals.port4.peripheral_type_1 }
};

static const char **peripheral_params_settings_map[4][2] = {
    { &g_config.input.peripherals.port1.peripheral_param_0,
      &g_config.input.peripherals.port1.peripheral_param_1 },
    { &g_config.input.peripherals.port2.peripheral_param_0,
      &g_config.input.peripherals.port2.peripheral_param_1 },
    { &g_config.input.peripherals.port3.peripheral_param_0,
      &g_config.input.peripherals.port3.peripheral_param_1 },
    { &g_config.input.peripherals.port4.peripheral_param_0,
      &g_config.input.peripherals.port4.peripheral_param_1 }
};

int *g_keyboard_scancode_map[25] = {
    &g_config.input.keyboard_controller_scancode_map.a,
    &g_config.input.keyboard_controller_scancode_map.b,
    &g_config.input.keyboard_controller_scancode_map.x,
    &g_config.input.keyboard_controller_scancode_map.y,
    &g_config.input.keyboard_controller_scancode_map.back,
    &g_config.input.keyboard_controller_scancode_map.guide,
    &g_config.input.keyboard_controller_scancode_map.start,
    &g_config.input.keyboard_controller_scancode_map.lstick_btn,
    &g_config.input.keyboard_controller_scancode_map.rstick_btn,
    &g_config.input.keyboard_controller_scancode_map.white,
    &g_config.input.keyboard_controller_scancode_map.black,
    &g_config.input.keyboard_controller_scancode_map.dpad_up,
    &g_config.input.keyboard_controller_scancode_map.dpad_down,
    &g_config.input.keyboard_controller_scancode_map.dpad_left,
    &g_config.input.keyboard_controller_scancode_map.dpad_right,
    &g_config.input.keyboard_controller_scancode_map.lstick_up,
    &g_config.input.keyboard_controller_scancode_map.lstick_left,
    &g_config.input.keyboard_controller_scancode_map.lstick_right,
    &g_config.input.keyboard_controller_scancode_map.lstick_down,
    &g_config.input.keyboard_controller_scancode_map.ltrigger,
    &g_config.input.keyboard_controller_scancode_map.rstick_up,
    &g_config.input.keyboard_controller_scancode_map.rstick_left,
    &g_config.input.keyboard_controller_scancode_map.rstick_right,
    &g_config.input.keyboard_controller_scancode_map.rstick_down,
    &g_config.input.keyboard_controller_scancode_map.rtrigger,
};

static void check_and_reset_in_range(int *btn, int min, int max,
                                     const char *message)
{
    if (*btn < min || *btn >= max) {
        fprintf(stderr, "%s\n", message);
        *btn = min;
    }
}

static void xemu_input_bindings_set_in_range(ControllerState *con)
{
#define CHECK_RESET_BUTTON(btn)                                            \
    check_and_reset_in_range(&con->controller_map->controller_mapping.btn, \
                             SDL_GAMEPAD_BUTTON_INVALID,                   \
                             SDL_GAMEPAD_BUTTON_COUNT,                     \
                             "Invalid entry for button " #btn ", resetting")

    CHECK_RESET_BUTTON(a);
    CHECK_RESET_BUTTON(b);
    CHECK_RESET_BUTTON(x);
    CHECK_RESET_BUTTON(y);
    CHECK_RESET_BUTTON(dpad_left);
    CHECK_RESET_BUTTON(dpad_up);
    CHECK_RESET_BUTTON(dpad_right);
    CHECK_RESET_BUTTON(dpad_down);
    CHECK_RESET_BUTTON(back);
    CHECK_RESET_BUTTON(start);
    CHECK_RESET_BUTTON(lshoulder);
    CHECK_RESET_BUTTON(rshoulder);
    CHECK_RESET_BUTTON(lstick_btn);
    CHECK_RESET_BUTTON(rstick_btn);
    CHECK_RESET_BUTTON(guide);

#undef CHECK_RESET_BUTTON

#define CHECK_RESET_AXIS(axis)                                              \
    check_and_reset_in_range(&con->controller_map->controller_mapping.axis, \
                             SDL_GAMEPAD_AXIS_INVALID,                      \
                             SDL_GAMEPAD_AXIS_COUNT,                        \
                             "Invalid entry for button " #axis ", resetting")

    CHECK_RESET_AXIS(axis_trigger_left);
    CHECK_RESET_AXIS(axis_trigger_right);
    CHECK_RESET_AXIS(axis_left_x);
    CHECK_RESET_AXIS(axis_left_y);
    CHECK_RESET_AXIS(axis_right_x);
    CHECK_RESET_AXIS(axis_right_y);

#undef CHECK_RESET_AXIS
}

static void xemu_input_bindings_reload_map(ControllerState *con)
{
    assert(con->type == INPUT_DEVICE_SDL_GAMEPAD);

    char guid[35] = { 0 };
    SDL_GUIDToString(con->sdl_joystick_guid, guid, sizeof(guid));
    if (!xemu_settings_load_gamepad_mapping(guid, &con->controller_map)) {
        return;
    }

    // If this controller did not exist in the mapping array, the config will
    // have been reallocated. Any gamepad mapping pointers for other controllers
    // are now invalid, and need to be reloaded.
    ControllerState *iter, *next;
    bool is_new_mapping;
    QTAILQ_FOREACH_SAFE (iter, &available_controllers, entry, next) {
        if (iter == con || iter->type != INPUT_DEVICE_SDL_GAMEPAD) {
            continue;
        }

        memset(guid, 0, sizeof(guid));
        SDL_GUIDToString(iter->sdl_joystick_guid, guid, sizeof(guid));

        is_new_mapping =
            xemu_settings_load_gamepad_mapping(guid, &iter->controller_map);
        assert(!is_new_mapping &&
               "Existing controller GUIDs should exist in the config");

        xemu_input_bindings_set_in_range(iter);
    }
}

static const char *get_bound_driver(int port)
{
    assert(port >= 0 && port <= 3);
    const char *driver = *port_index_to_driver_settings_key_map[port];

    // If the driver in the config is NULL, empty, or unrecognized 
    // then default to DRIVER_DUKE
    if (driver == NULL)
        return DRIVER_DUKE;
    if (strlen(driver) == 0)
        return DRIVER_DUKE;
    if (strcmp(driver, DRIVER_DUKE) == 0)
        return DRIVER_DUKE;
    if (strcmp(driver, DRIVER_S) == 0)
        return DRIVER_S;

    return DRIVER_DUKE;
}

/* Player index -> port on the motherboard hub (also ui/xemu-input-bind.c). */
const int xemu_input_hub_ports[4] = { 3, 4, 1, 2 };

static void test_input_poll(void);
static void xemu_input_publish_controller(ControllerState *state);

/*
 * XEMU_ICOUNT_LIVE_PADS=1 (icount only): host gamepads and the host
 * keyboard drive their ports as in normal mode, but are sampled only at
 * vblanks, where pipe lines apply, so the run stays deterministic for its
 * input and XEMU_INPUT_RECORD replays it. With sleep=on the idle vCPU is
 * paced on real time (accel/tcg/icount-common.c). A gamepad's connect and
 * disconnect apply at a vblank and never plug or unplug an emulated pad:
 * a connected pad drives a port whose pad is plugged, a disconnected one
 * leaves its pad plugged with neutral input and its port waiting for it.
 */
bool xemu_input_live_pads(void)
{
    static int live = -1;

    if (live < 0) {
        live = icount_enabled() && icount_live_pads_requested();
    }
    return live;
}

static bool live_keys[SDL_SCANCODE_COUNT]; /* the host keyboard at the last vblank */
static GQueue live_added = G_QUEUE_INIT;   /* gamepads to bind at the next vblank */
static GQueue live_removed = G_QUEUE_INIT; /* bound gamepads gone, let go then */
/*
 * Let go at a vblank, freed by the UI thread: the settings UI reads the
 * bound gamepads without the BQL, so it must not race a vCPU-thread free.
 */
static GQueue live_freed = G_QUEUE_INIT;

/*
 * Deterministic mode: on every start (cont, frame advance, after a load),
 * queue what the pipe holds and publish every pad before the first guest
 * instruction, so a run never starts on pad state from before the load.
 */
static void xemu_input_vm_state_change(void *opaque, bool running,
                                       RunState state)
{
    ControllerState *iter;

    if (!running) {
        return;
    }
    test_input_poll();
    QTAILQ_FOREACH(iter, &available_controllers, entry) {
        if (xemu_input_live_pads() && iter->type == INPUT_DEVICE_SDL_GAMEPAD) {
            continue; /* keeps its last vblank's sample (neutral after a load) */
        }
        xemu_input_publish_controller(iter);
    }
}

static void xemu_input_vmstate_init(void);
static void input_record_init(void);
static void input_record_load(void);

void xemu_input_init(void)
{
    if (icount_enabled()) {
        qemu_add_vm_change_state_handler(xemu_input_vm_state_change, NULL);
    }
    xemu_input_vmstate_init();
    input_record_init();

    if (g_config.input.background_input_capture) {
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    }

    /*
     * Deterministic mode ignores physical pads (pads change only from the
     * pipe), so it skips probing the USB and Bluetooth devices for them,
     * unless XEMU_ICOUNT_LIVE_PADS samples them at vblanks.
     */
    if ((!icount_enabled() || xemu_input_live_pads()) &&
        !SDL_Init(SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "Failed to initialize SDL gamepad subsystem: %s\n", SDL_GetError());
        exit(1);
    }
    if (xemu_input_live_pads()) {
        fprintf(stderr, "xemu: icount live pads: host gamepads and keyboard "
                "drive their ports, sampled at vblanks\n");
    }

    if (g_config.input.gamecontrollerdb_path && strlen(g_config.input.gamecontrollerdb_path) > 0) {
        int count = SDL_AddGamepadMappingsFromFile(g_config.input.gamecontrollerdb_path);
        if (count > 0) {
            fprintf(stderr, "Loaded %d custom gamepad mapping(s) from '%s'\n",
                    count, g_config.input.gamecontrollerdb_path);
        } else if (count < 0) {
            fprintf(stderr, "Failed to load gamepad mappings from '%s': %s\n",
                    g_config.input.gamecontrollerdb_path, SDL_GetError());
        }
    }

    // Create the keyboard input (always first)
    ControllerState *new_con = malloc(sizeof(ControllerState));
    memset(new_con, 0, sizeof(ControllerState));
    new_con->type = INPUT_DEVICE_SDL_KEYBOARD;
    new_con->name = "Keyboard";
    new_con->bound = -1;
    new_con->peripheral_types[0] = PERIPHERAL_NONE;
    new_con->peripheral_types[1] = PERIPHERAL_NONE;
    new_con->peripherals[0] = NULL;
    new_con->peripherals[1] = NULL;

    for (int i = 0; i < 25; i++) {
        static const char *format_str =
            "WARNING: Keyboard controller map scancode out of range "
            "(%d) : Disabled\n";
        char buf[128];
        snprintf(buf, sizeof(buf), format_str, i);
        check_and_reset_in_range(g_keyboard_scancode_map[i],
                                 SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_COUNT, buf);
    }

    bound_drivers[0] = get_bound_driver(0);
    bound_drivers[1] = get_bound_driver(1);
    bound_drivers[2] = get_bound_driver(2);
    bound_drivers[3] = get_bound_driver(3);

    // Check to see if we should auto-bind the keyboard
    int port = xemu_input_get_controller_default_bind_port(new_con, 0);
    if (port >= 0) {
        xemu_input_bind(port, new_con, 0);
        char buf[128];
        snprintf(buf, sizeof(buf), "Connected '%s' to port %d", new_con->name, port+1);
        xemu_queue_notification(buf);
        xemu_input_rebind_xmu(port);
    }

    QTAILQ_INSERT_TAIL(&available_controllers, new_con, entry);
}

int xemu_input_get_controller_default_bind_port(ControllerState *state, int start)
{
    char guid[35] = { 0 };
    if (state->type == INPUT_DEVICE_SDL_GAMEPAD) {
        SDL_GUIDToString(state->sdl_joystick_guid, guid, sizeof(guid));
    } else if (state->type == INPUT_DEVICE_SDL_KEYBOARD) {
        snprintf(guid, sizeof(guid), "keyboard");
    }

    for (int i = start; i < 4; i++) {
        if (strcmp(guid, *port_index_to_settings_key_map[i]) == 0) {
            return i;
        }
    }

    return -1;
}

void xemu_save_peripheral_settings(int player_index, int peripheral_index,
                                   int peripheral_type,
                                   const char *peripheral_parameter)
{
    int *peripheral_type_ptr =
        peripheral_types_settings_map[player_index][peripheral_index];
    const char **peripheral_param_ptr =
        peripheral_params_settings_map[player_index][peripheral_index];

    assert(peripheral_type_ptr);
    assert(peripheral_param_ptr);

    *peripheral_type_ptr = peripheral_type;
    xemu_settings_set_string(
        peripheral_param_ptr,
        peripheral_parameter == NULL ? "" : peripheral_parameter);
}

/*
 * A gamepad that just connected takes the pinned port waiting for it, else
 * a port configured for its GUID, else (auto_bind) any free port. Live pads
 * (at a vblank) take only a port whose emulated pad is already plugged and
 * plug no memory units.
 */
static void gamepad_autobind(ControllerState *new_con, bool live)
{
    bool skipped = false;

    // A pinned port (QMP xemu-input-bind) waiting for this pad
    int port = xemu_input_pinned_attach(new_con);
    if (port >= 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Connected '%s' to port %d",
                 new_con->name, port + 1);
        xemu_queue_notification(buf);
        return;
    }

    // Do not replace binding for a currently bound device. In the case that
    // the same GUID is specified multiple times, on different ports, allow
    // any available port to be bound.
    //
    // This can happen naturally with X360 wireless receiver, in which each
    // controller gets the same GUID (go figure). We cannot remember which
    // controller is which in this case, but we can try to tolerate this
    // situation by binding to any previously bound port with this GUID. The
    // upside in this case is that a person can use the same GUID on all
    // ports and just needs to bind to the receiver and never needs to hit
    // this dialog.


    // Attempt to re-bind to port previously bound to
    port = 0;
    bool did_bind = false;
    while (!did_bind) {
        port = xemu_input_get_controller_default_bind_port(new_con, port);
        if (port < 0) {
            // No (additional) default mappings
            break;
        } else if (!xemu_input_get_bound(port) &&
                   !xemu_input_port_pinned(port) &&
                   (!live || xemu_input_port_has_pad(port))) {
            xemu_input_bind(port, new_con, 0);
            did_bind = true;
            break;
        } else {
            skipped |= live && !xemu_input_get_bound(port) &&
                       !xemu_input_port_pinned(port);
            // Try again for another port
            port++;
        }
    }

    // Try to bind to any open port, and if so remember the binding
    if (!did_bind && g_config.input.auto_bind) {
        for (port = 0; port < 4; port++) {
            if (!xemu_input_get_bound(port) &&
                !xemu_input_port_pinned(port)) {
                if (live && !xemu_input_port_has_pad(port)) {
                    skipped = true;
                    continue;
                }
                xemu_input_bind(port, new_con, 1);
                did_bind = true;
                break;
            }
        }
    }

    if (did_bind) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Connected '%s' to port %d", new_con->name, port+1);
        xemu_queue_notification(buf);
        if (!live) {
            xemu_input_rebind_xmu(port);
        }
    } else if (skipped) {
        fprintf(stderr, "xemu: icount live pads: '%s' drives no port: its "
                "free ports have no pad plugged, and live pads plug none "
                "mid-run (bind it to a port with a pad: QMP "
                "xemu-input-bind)\n", new_con->name);
    }
}

void xemu_input_process_sdl_events(const SDL_Event *event)
{
    if (icount_enabled() && !xemu_input_live_pads() &&
        (event->type == SDL_EVENT_GAMEPAD_ADDED ||
         event->type == SDL_EVENT_GAMEPAD_REMOVED)) {
        /* Deterministic mode: only XEMU_INPUT_PIPE drives the pads. */
        return;
    }

    if (event->type == SDL_EVENT_GAMEPAD_ADDED) {
        DPRINTF("Controller Added: %d\n", event->gdevice.which);

        // Attempt to open the added controller
        SDL_Gamepad *sdl_con;
        sdl_con = SDL_OpenGamepad(event->gdevice.which);
        if (sdl_con == NULL) {
            DPRINTF("Could not open joystick %d as a Gamepad\n", event->gdevice.which);
            return;
        }

        // Success! Create a new node to track this controller and continue init
        ControllerState *new_con = malloc(sizeof(ControllerState));
        memset(new_con, 0, sizeof(ControllerState));
        new_con->type                 = INPUT_DEVICE_SDL_GAMEPAD;
        new_con->name                 = SDL_GetGamepadName(sdl_con);
        new_con->sdl_gamepad          = sdl_con;
        new_con->sdl_joystick         = SDL_GetGamepadJoystick(new_con->sdl_gamepad);
        new_con->sdl_joystick_id      = SDL_GetJoystickID(new_con->sdl_joystick);
        new_con->sdl_joystick_guid    = SDL_GetJoystickGUID(new_con->sdl_joystick);
        new_con->bound                = -1;
        new_con->peripheral_types[0] = PERIPHERAL_NONE;
        new_con->peripheral_types[1] = PERIPHERAL_NONE;
        new_con->peripherals[0] = NULL;
        new_con->peripherals[1] = NULL;

        char guid_buf[35] = { 0 };
        SDL_GUIDToString(new_con->sdl_joystick_guid, guid_buf, sizeof(guid_buf));
        DPRINTF("Opened %s (%s)\n", new_con->name, guid_buf);

        QTAILQ_INSERT_TAIL(&available_controllers, new_con, entry);
        xemu_input_bindings_reload_map(new_con);

        if (icount_enabled()) {
            /* Live pads: it takes a port at the next vblank. */
            g_queue_push_tail(&live_added, new_con);
            return;
        }
        gamepad_autobind(new_con, false);
    } else if (event->type == SDL_EVENT_GAMEPAD_REMOVED) {
        DPRINTF("Controller Removed: %d\n", event->gdevice.which);
        int handled = 0;
        ControllerState *iter, *next;
        QTAILQ_FOREACH_SAFE(iter, &available_controllers, entry, next) {
            if (iter->type != INPUT_DEVICE_SDL_GAMEPAD) continue;

            if (iter->sdl_joystick_id == event->gdevice.which) {
                DPRINTF("Device removed: %s\n", iter->name);

                if (icount_enabled()) {
                    g_queue_remove(&live_added, iter);
                }
                if (icount_enabled() && iter->bound >= 0) {
                    /*
                     * Live pads: its port lets go at the next vblank; until
                     * then the guest sees the last vblank's sample.
                     */
                    QTAILQ_REMOVE(&available_controllers, iter, entry);
                    SDL_CloseGamepad(iter->sdl_gamepad);
                    iter->sdl_gamepad = NULL;
                    iter->name = "Disconnected gamepad"; /* SDL's is freed */
                    g_queue_push_tail(&live_removed, iter);
                    handled = 1;
                    break;
                }

                // Disconnect (a pinned port keeps its emulated pad)
                if (iter->bound >= 0 && !xemu_input_pinned_detach(iter)) {
                    // Queue a notification to inform user controller disconnected
                    // FIXME: Probably replace with a callback registration thing,
                    // but this works well enough for now.
                    char buf[128];
                    snprintf(buf, sizeof(buf), "Port %d disconnected", iter->bound+1);
                    xemu_queue_notification(buf);

                    // Unbind the controller, but don't save the unbinding in
                    // case the controller is reconnected
                    xemu_input_bind(iter->bound, NULL, 0);
                }

                // Unlink
                QTAILQ_REMOVE(&available_controllers, iter, entry);

                // Deallocate
                if (iter->sdl_gamepad) {
                    SDL_CloseGamepad(iter->sdl_gamepad);
                }

                for (int i = 0; i < 2; i++) {
                    if (iter->peripherals[i])
                        g_free(iter->peripherals[i]);
                }
                free(iter);

                handled = 1;
                break;
            }
        }
        if (!handled) {
            DPRINTF("Could not find handle for joystick instance\n");
        }
    } else if (event->type == SDL_EVENT_GAMEPAD_REMAPPED) {
        DPRINTF("Controller Remapped: %d\n", event->gdevice.which);
    } else if (event->type == SDL_EVENT_JOYSTICK_ADDED) {
        if (!SDL_IsGamepad(event->jdevice.which)) {
            const char *name = SDL_GetJoystickNameForID(event->jdevice.which);
            SDL_GUID guid = SDL_GetJoystickGUIDForID(event->jdevice.which);
            char guid_buf[35] = { 0 };
            SDL_GUIDToString(guid, guid_buf, sizeof(guid_buf));
            fprintf(
                stderr,
                "Input: Detected unmapped joystick '%s' (GUID: %s, ID: %u). "
                "This device is not recognized as a gamepad by SDL and "
                "requires an SDL gamepad mapping to work with xemu.\n",
                name ? name : "Unknown", guid_buf,
                (unsigned int)event->jdevice.which);
        }
    }
}

static void xemu_input_publish_controller(ControllerState *state)
{
    if (state->type == INPUT_DEVICE_SDL_KEYBOARD) {
        xemu_input_update_sdl_kbd_controller_state(state);
    } else if (state->type == INPUT_DEVICE_SDL_GAMEPAD) {
        xemu_input_update_sdl_controller_state(state);
    }

    state->last_input_updated_ts = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
}

static FILE *record_file;

/*
 * Pads change only at vblanks (xemu_input_frame_tick), never on a host-time
 * poll from the USB device or the UI loop: in deterministic mode (icount),
 * and while recording (XEMU_INPUT_RECORD), so that the recorded run gets
 * its input on the vblanks a replay gives it.
 */
static bool input_at_vblanks(void)
{
    return icount_enabled() || record_file;
}

bool xemu_input_pads_at_vblanks(void)
{
    return input_at_vblanks();
}

void xemu_input_update_controller(ControllerState *state)
{
    if (input_at_vblanks()) {
        return;
    }

    int64_t now = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    if (ABS(now - state->last_input_updated_ts) <
        XEMU_INPUT_MIN_INPUT_UPDATE_INTERVAL_US) {
        return;
    }

    xemu_input_publish_controller(state);
}

void xemu_input_update_controllers(void)
{
    ControllerState *iter;

    if (icount_enabled()) {
        /* Queue pipe lines promptly, so "+N" counts from the frame they
         * were written at, even while paused. */
        test_input_poll();
        if (xemu_input_live_pads()) {
            while ((iter = g_queue_pop_head(&live_freed))) {
                for (int i = 0; i < 2; i++) {
                    if (iter->peripheral_types[i] == PERIPHERAL_XMU &&
                        iter->peripherals[i]) {
                        g_free((void *)((XmuState *)iter->peripherals[i])
                                   ->filename);
                    }
                    g_free(iter->peripherals[i]);
                }
                free(iter);
            }
            QTAILQ_FOREACH(iter, &available_controllers, entry) {
                xemu_input_update_rumble(iter);
            }
        }
        return;
    }

    QTAILQ_FOREACH(iter, &available_controllers, entry) {
        xemu_input_update_controller(iter);
    }
    QTAILQ_FOREACH(iter, &available_controllers, entry) {
        xemu_input_update_rumble(iter);
    }
}

/*
 * Test-input channel: when XEMU_INPUT_PIPE names a FIFO (POSIX) or a
 * named pipe (Windows, see below), lines of the form
 * "down <sdl_scancode>", "up <sdl_scancode>" or "clear"
 * are OR'd into the keyboard-controller state below. Injected keys
 * flow through the user's normal scancode bindings and work with
 * the window unfocused — macOS drops synthetic key events for
 * background apps, which makes OS-level injection (System Events,
 * CGEventPostToPid) unusable for automated gameplay testing. No
 * cost when the variable is unset.
 *
 * "axis <lx|ly|rx|ry> <-1..1>" and "trigger <lt|rt> <0..1>" set an
 * analog value (positive = right/up; 0 hands the axis back to the
 * keys). Any line may start with "@<frame>" or "+<frames>" to apply it
 * at that frame (vblanks while running, see xemu_input_frame_tick)
 * instead of on arrival; "+0" means the next vblank.
 */
static uint8_t test_input_override[SDL_SCANCODE_COUNT];
static int16_t test_axis_override[CONTROLLER_AXIS__COUNT];

typedef struct TestInputPending {
    uint64_t frame;
    char *line;
} TestInputPending;

static GQueue test_input_pending = G_QUEUE_INIT;
static uint64_t test_input_frame;
static char test_input_partial[256];
static size_t test_input_partial_len;

static int test_input_axis(const char *name)
{
    static const char *const names[CONTROLLER_AXIS__COUNT] = {
        [CONTROLLER_AXIS_LTRIG] = "lt",   [CONTROLLER_AXIS_RTRIG] = "rt",
        [CONTROLLER_AXIS_LSTICK_X] = "lx", [CONTROLLER_AXIS_LSTICK_Y] = "ly",
        [CONTROLLER_AXIS_RSTICK_X] = "rx", [CONTROLLER_AXIS_RSTICK_Y] = "ry",
    };
    for (int i = 0; i < CONTROLLER_AXIS__COUNT; i++) {
        if (!strcmp(name, names[i])) {
            return i;
        }
    }
    return -1;
}

static void test_input_apply(const char *line)
{
    int sc;
    char name[8];
    double v;

    if (sscanf(line, "down %d", &sc) == 1) {
        if (sc >= 0 && sc < SDL_SCANCODE_COUNT) {
            test_input_override[sc] = 1;
        }
    } else if (sscanf(line, "up %d", &sc) == 1) {
        if (sc >= 0 && sc < SDL_SCANCODE_COUNT) {
            test_input_override[sc] = 0;
        }
    } else if (!strncmp(line, "clear", 5)) {
        memset(test_input_override, 0, sizeof(test_input_override));
        memset(test_axis_override, 0, sizeof(test_axis_override));
    } else if (sscanf(line, "axis %7s %lf", name, &v) == 2 ||
               sscanf(line, "trigger %7s %lf", name, &v) == 2) {
        int axis = test_input_axis(name);
        bool trigger = axis == CONTROLLER_AXIS_LTRIG ||
                       axis == CONTROLLER_AXIS_RTRIG;
        if (axis >= 0 && (line[0] == 't') == trigger) {
            /* A stick reaches -32768, one step below -1.0 (recordings). */
            v = MAX(trigger ? 0.0 : -32768.0 / 32767.0, MIN(v, 1.0));
            test_axis_override[axis] = (int16_t)lround(v * 32767.0);
        }
    }
}

static void test_input_line(char *line)
{
    if (line[0] != '@' && line[0] != '+') {
        if (!icount_enabled()) {
            test_input_apply(line);
            return;
        }
        /* Deterministic mode: an untagged line waits for the next vblank. */
        TestInputPending *p = g_new(TestInputPending, 1);
        p->frame = test_input_frame;
        p->line = g_strdup(line);
        g_queue_push_tail(&test_input_pending, p);
        return;
    }

    char *rest;
    uint64_t n = strtoull(line + 1, &rest, 10);
    TestInputPending *p = g_new(TestInputPending, 1);
    p->frame = line[0] == '@' ? n : test_input_frame + n;
    if (icount_enabled() && line[0] == '@' && n <= test_input_frame) {
        fprintf(stderr, "xemu: input line for frame %" PRIu64 " read at "
                "frame %" PRIu64 "; applied at the next vblank\n",
                n, test_input_frame);
    }
    p->line = g_strdup(g_strchug(rest));
    g_queue_push_tail(&test_input_pending, p);
}

/* Split pipe data into lines; a line cut across reads waits for its end. */
static void test_input_feed(const char *data, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        char c = data[i];
        if (c == '\n' || c == '\r') {
            if (test_input_partial_len) {
                test_input_partial[test_input_partial_len] = '\0';
                test_input_line(test_input_partial);
                test_input_partial_len = 0;
            }
        } else if (test_input_partial_len < sizeof(test_input_partial) - 1) {
            test_input_partial[test_input_partial_len++] = c;
        }
    }
}

/*
 * QMP xemu-run: pipe lines handed over in the call (BQL held, VM stopped),
 * queued as if the pipe had delivered them now. The check accepts what
 * test_input_apply() acts on, written exactly (a new pipe verb goes in
 * both), and what the pipe ignores: blank lines and '#' comments, as
 * XEMU_INPUT_RECORD writes them, which are not queued.
 */
static const char *input_line_body(const char *p, Error **errp)
{
    if (*p == '@' || *p == '+') {
        uint64_t n;
        const char *rest;

        if (!g_ascii_isdigit(p[1]) || qemu_strtou64(p + 1, &rest, 10, &n)) {
            error_setg(errp, "input line '%s': a tag is @ or + and a frame "
                       "number", p);
            return NULL;
        }
        p = rest;
        while (g_ascii_isspace(*p)) {
            p++;
        }
    }
    return p;
}

static bool input_line_ignored(const char *body)
{
    return !body[0] || body[0] == '#';
}

bool xemu_input_check_line(const char *line, Error **errp);
bool xemu_input_check_line(const char *line, Error **errp)
{
    g_autofree char *copy = g_strstrip(g_strdup(line));
    const char *p = input_line_body(copy, errp);
    char name[8];
    double v;
    int sc, end = -1;

    if (!p) {
        return false;
    }
    if (input_line_ignored(p) || !strcmp(p, "clear")) {
        return true;
    }
    if ((sscanf(p, "down %d%n", &sc, &end) == 1 ||
         sscanf(p, "up %d%n", &sc, &end) == 1) && end >= 0 && !p[end]) {
        if (sc < 0 || sc >= SDL_SCANCODE_COUNT) {
            error_setg(errp, "input line '%s': scancode out of range", line);
            return false;
        }
        return true;
    }
    end = -1;
    if ((sscanf(p, "axis %7s %lf%n", name, &v, &end) == 2 ||
         sscanf(p, "trigger %7s %lf%n", name, &v, &end) == 2) &&
        end >= 0 && !p[end]) {
        int axis = test_input_axis(name);
        bool trigger = axis == CONTROLLER_AXIS_LTRIG ||
                       axis == CONTROLLER_AXIS_RTRIG;

        if (axis < 0 || (p[0] == 't') != trigger) {
            error_setg(errp, "input line '%s': no such %s", line,
                       p[0] == 't' ? "trigger (lt, rt)"
                                   : "axis (lx, ly, rx, ry)");
            return false;
        }
        if (!isfinite(v)) {
            error_setg(errp, "input line '%s': the value is not finite",
                       line);
            return false;
        }
        return true;
    }
    error_setg(errp, "input line '%s' is none of down, up, clear, axis and "
               "trigger", line);
    return false;
}

void xemu_input_queue_line(const char *line);
void xemu_input_queue_line(const char *line)
{
    g_autofree char *copy = g_strstrip(g_strdup(line));
    const char *body = input_line_body(copy, NULL);

    if (body && !input_line_ignored(body)) {
        test_input_line(copy);
    }
}

#ifdef _WIN32
/* Windows has no FIFOs, so the same line protocol rides a Win32 named
 * pipe: XEMU_INPUT_PIPE either names the pipe in full ("\\.\pipe\foo")
 * or gives a path whose LAST component becomes the pipe name (so a
 * single "/tmp/xemu.fifo" env value works unchanged on both hosts).
 * The instance is created PIPE_NOWAIT so every poll is a bounded,
 * non-blocking peek from the UI thread, matching the POSIX
 * O_NONBLOCK read()<=0 -> break shape. Unlike a FIFO, a named pipe
 * does not tolerate writer churn implicitly: each one-shot writer
 * (inject_input.py opens, writes, closes) must be accepted and then
 * recycled with DisconnectNamedPipe, which is what the state machine
 * below does. No cost when the variable is unset. */
static HANDLE test_input_pipe;      /* NULL = unprobed or disabled */
static bool test_input_probed;
static bool test_input_connected;

static void test_input_open(void)
{
    const char *path = getenv("XEMU_INPUT_PIPE");
    char name[MAX_PATH];

    test_input_probed = true;
    if (!path || !path[0]) {
        return;
    }

    if (path[0] == '\\' && path[1] == '\\') {
        snprintf(name, sizeof(name), "%s", path);
    } else {
        const char *base = path;
        for (const char *p = path; *p; p++) {
            if (*p == '/' || *p == '\\' || *p == ':') {
                base = p + 1;
            }
        }
        if (!base[0]) {
            fprintf(stderr, "xemu: XEMU_INPUT_PIPE has no pipe name: %s\n",
                    path);
            return;
        }
        snprintf(name, sizeof(name), "\\\\.\\pipe\\%s", base);
    }

    HANDLE h = CreateNamedPipeA(name, PIPE_ACCESS_INBOUND,
                                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE |
                                    PIPE_NOWAIT,
                                1, 0, 4096, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "xemu: XEMU_INPUT_PIPE create failed (%s): %lu\n",
                name, (unsigned long)GetLastError());
        return;
    }
    test_input_pipe = h;
    fprintf(stderr, "xemu: test-input pipe listening on %s\n", name);
}

static void test_input_recycle(void)
{
    /* Client gone: drop the instance back to listening so the NEXT
     * one-shot writer can connect. DisconnectNamedPipe discards whatever
     * is still queued, so this is only ever reached after a peek/read has
     * already failed (i.e. there is nothing left to drain).
     *
     * Re-arm in the SAME poll. A disconnected instance is not listening,
     * and this is the only instance (nMaxInstances=1), so any writer that
     * calls CreateFile between the disconnect and the next poll's
     * ConnectNamedPipe gets ERROR_PIPE_BUSY and gives up. This is polled
     * once per UI event-pump iteration (~16 ms at 60 Hz) and the writers
     * are one-shot (scripts/win/inject_input.py opens, writes, closes), so
     * that window swallows keystrokes. On a PIPE_NOWAIT
     * instance ConnectNamedPipe never blocks: TRUE means "listening again,
     * no client yet" (the common case, and what leaves test_input_connected
     * false for the next poll to notice), while ERROR_PIPE_CONNECTED /
     * ERROR_NO_DATA mean a writer attached — and possibly already wrote and
     * closed — inside this call, so mark it connected and let the next poll
     * drain it instead of dropping it. ERROR_PIPE_LISTENING cannot come back
     * here (only from a second Connect on an already-listening instance), and
     * any other error leaves the state machine where it was: disconnected,
     * retried next poll. */
    DisconnectNamedPipe(test_input_pipe);
    test_input_connected = false;
    if (!ConnectNamedPipe(test_input_pipe, NULL)) {
        DWORD err = GetLastError();
        if (err == ERROR_PIPE_CONNECTED || err == ERROR_NO_DATA) {
            test_input_connected = true;
        }
    }
}

static void test_input_poll(void)
{
    if (!test_input_probed) {
        test_input_open();
    }
    if (!test_input_pipe) {
        return;
    }

    if (!test_input_connected) {
        /* On a non-blocking instance ConnectNamedPipe returns TRUE only to
         * say "the instance is now listening again" (no client yet) — which
         * test_input_recycle() has already consumed, so what normally lands
         * here is ERROR_PIPE_LISTENING = idle, nothing to do. An attached
         * client is reported as failure + ERROR_PIPE_CONNECTED, and
         * ERROR_NO_DATA means a writer already connected, wrote and closed
         * between two polls. NO_DATA still has its bytes queued, so it falls
         * through to the drain rather than disconnecting (which would
         * discard them). */
        if (ConnectNamedPipe(test_input_pipe, NULL)) {
            return;
        }
        DWORD err = GetLastError();
        if (err != ERROR_PIPE_CONNECTED && err != ERROR_NO_DATA) {
            return;
        }
        test_input_connected = true;
    }

    char buf[512];
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(test_input_pipe, NULL, 0, NULL, &avail, NULL)) {
            test_input_recycle();
            return;
        }
        if (avail == 0) {
            break;
        }
        DWORD n = 0;
        if (!ReadFile(test_input_pipe, buf, sizeof(buf) - 1, &n, NULL)) {
            test_input_recycle();
            return;
        }
        if (n == 0) {
            break;
        }
        /* Windows writers (PowerShell Add-Content, cmd echo) emit CRLF;
         * the feeder splits on '\r' as well. */
        test_input_feed(buf, n);
    }
}
#else
static int test_input_fd = -2; /* -2 unprobed, -1 disabled */

static void test_input_poll(void)
{
    if (test_input_fd == -2) {
        const char *path = getenv("XEMU_INPUT_PIPE");
        test_input_fd = -1;
        if (path && path[0]) {
            int fd = open(path, O_RDONLY | O_NONBLOCK);
            if (fd >= 0) {
                test_input_fd = fd;
            } else {
                fprintf(stderr, "xemu: XEMU_INPUT_PIPE open failed: %s\n",
                        path);
            }
        }
    }
    if (test_input_fd < 0) {
        return;
    }
    char buf[512];
    for (;;) {
        ssize_t n = read(test_input_fd, buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        test_input_feed(buf, n);
    }
}
#endif /* !_WIN32 */

/*
 * Deterministic mode (icount): the keys and axes the pipe holds are pad
 * input the guest sees at the next vblank, so saves made there carry them
 * and a load continues with them (xemu-input). Only icount saves have the
 * section; normal-mode savestates keep their format.
 */
static bool test_input_loaded;

/* The held input as it travels in xemu-input. */
static struct {
    uint8_t keys[SDL_SCANCODE_COUNT];
    int16_t axes[CONTROLLER_AXIS__COUNT];
} test_input_saved;

static bool xemu_input_vmstate_needed(void *opaque)
{
    return icount_enabled();
}

static int xemu_input_vmstate_pre_save(void *opaque)
{
    memcpy(test_input_saved.keys, test_input_override,
           sizeof(test_input_saved.keys));
    memcpy(test_input_saved.axes, test_axis_override,
           sizeof(test_input_saved.axes));
    return 0;
}

static int xemu_input_vmstate_post_load(void *opaque, int version_id)
{
    memcpy(test_input_override, test_input_saved.keys,
           sizeof(test_input_saved.keys));
    memcpy(test_axis_override, test_input_saved.axes,
           sizeof(test_input_saved.axes));
    test_input_loaded = true;
    return 0;
}

static const VMStateDescription vmstate_xemu_input = {
    .name = "xemu-input",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = xemu_input_vmstate_needed,
    .pre_save = xemu_input_vmstate_pre_save,
    .post_load = xemu_input_vmstate_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_BUFFER_UNSAFE(keys, typeof(test_input_saved), 0,
                              sizeof(test_input_saved.keys)),
        VMSTATE_INT16_ARRAY(axes, typeof(test_input_saved),
                            CONTROLLER_AXIS__COUNT),
        VMSTATE_END_OF_LIST()
    },
};

static void xemu_input_vmstate_init(void)
{
    vmstate_register(NULL, 0, &vmstate_xemu_input, &test_input_saved);
}

/*
 * After loadvm or an incoming migration (savevm.c). Deterministic mode
 * drops lines still queued from the previous run, so an in-process replay
 * matches a fresh one; lines written after the load are kept. Keys and
 * axes held at the save are restored (xemu-input); a save without them
 * starts from neutral pad input.
 */
void xemu_input_state_loaded(void)
{
    input_record_load();
    if (!icount_enabled()) {
        test_input_loaded = false;
        return;
    }
    if (xemu_input_live_pads()) {
        /*
         * Live sources start neutral, as a recording's "# load" section
         * does; the next vblank samples them.
         */
        ControllerState *c;
        memset(live_keys, 0, sizeof(live_keys));
        for (int i = 0; i < 4; i++) {
            c = bound_controllers[i];
            if (c && c->type == INPUT_DEVICE_SDL_GAMEPAD) {
                c->buttons = 0;
                memset(c->axis, 0, sizeof(c->axis));
            }
        }
        QTAILQ_FOREACH(c, &available_controllers, entry) {
            if (c->type == INPUT_DEVICE_SDL_GAMEPAD) {
                c->buttons = 0;
                memset(c->axis, 0, sizeof(c->axis));
            }
        }
    }
    test_input_poll();
    TestInputPending *p;
    while ((p = g_queue_pop_head(&test_input_pending))) {
        g_free(p->line);
        g_free(p);
    }
    if (!test_input_loaded) {
        memset(test_input_override, 0, sizeof(test_input_override));
        memset(test_axis_override, 0, sizeof(test_axis_override));
    }
    test_input_loaded = false;
    test_input_partial_len = 0;
}

/*
 * XEMU_INPUT_RECORD=<file>: at every vblank, the state one pad gave the
 * guest (XEMU_INPUT_RECORD_PORT, 1-4; default the first bound port), as
 * XEMU_INPUT_PIPE lines for the changes, tagged "+N" with the vblanks
 * since the last load (or launch); each load starts a "# load" section
 * from neutral. Buttons are written as down/up of their keyboard-map
 * scancodes and sticks and triggers as values that give back the same
 * 16-bit reading, so the keyboard pad bound to that port in a replay with
 * the same keyboard map hands the guest the same state from the same
 * vblank: write a section's lines after loading the same state, then run.
 */
static int record_port = -1;
static uint64_t record_base;
static uint16_t record_buttons;
static int16_t record_axes[CONTROLLER_AXIS__COUNT];

static void input_record_init(void)
{
    const char *path = getenv("XEMU_INPUT_RECORD");
    const char *port = getenv("XEMU_INPUT_RECORD_PORT");

    if (!path || !path[0]) {
        return;
    }
    record_file = fopen(path, "w");
    if (!record_file) {
        fprintf(stderr, "xemu: XEMU_INPUT_RECORD open failed: %s\n", path);
        return;
    }
    setvbuf(record_file, NULL, _IOLBF, 0);
    if (port && port[0] >= '1' && port[0] <= '4' && !port[1]) {
        record_port = port[0] - '1';
    }
    fprintf(record_file, "# xemu input recording: XEMU_INPUT_PIPE lines; "
            "+N counts vblanks from the last load\n");
}

static void input_record_load(void)
{
    if (!record_file) {
        return;
    }
    record_base = test_input_frame;
    record_buttons = 0;
    memset(record_axes, 0, sizeof(record_axes));
    fprintf(record_file, "# load\n");
}

static void input_record_frame(uint64_t frame)
{
    static const int buttons[] = { /* bit order of ControllerState.buttons */
        0, 1, 2, 3, 13, 11, 14, 12, 4, 6, 9, 10, 7, 8, 5,
    };
    static const char *const axes[CONTROLLER_AXIS__COUNT] = {
        [CONTROLLER_AXIS_LTRIG] = "trigger lt", [CONTROLLER_AXIS_RTRIG] = "trigger rt",
        [CONTROLLER_AXIS_LSTICK_X] = "axis lx", [CONTROLLER_AXIS_LSTICK_Y] = "axis ly",
        [CONTROLLER_AXIS_RSTICK_X] = "axis rx", [CONTROLLER_AXIS_RSTICK_Y] = "axis ry",
    };
    static bool warned[4];
    ControllerState *c = NULL;
    uint16_t now_buttons = 0;
    const int16_t *now_axes = NULL;
    uint64_t at = frame - record_base;
    int port = record_port;

    if (port < 0) {
        for (port = 0; port < 3 && !bound_controllers[port]; port++) {
        }
    }
    c = bound_controllers[port];

    /*
     * Input another port's pad got (a host gamepad there or, with live
     * pads, the host keyboard on the keyboard pad) is not in the
     * recording, so a replay diverges from that vblank on.
     */
    for (int i = 0; i < 4; i++) {
        ControllerState *o = bound_controllers[i];
        bool active = o && o->buttons;

        for (int a = 0; o && a < CONTROLLER_AXIS__COUNT; a++) {
            active |= o->axis[a] != 0;
        }
        if (i != port && active && !warned[i]) {
            warned[i] = true;
            fprintf(stderr, "xemu: XEMU_INPUT_RECORD: port %d's pad got "
                    "input at +%" PRIu64 " that the recording (port %d) "
                    "leaves out; a replay diverges from there\n", i + 1, at,
                    port + 1);
        }
    }
    if (c) {
        now_buttons = c->buttons;
        now_axes = c->axis;
    }
    for (int i = 0; i < ARRAY_SIZE(buttons); i++) {
        uint16_t bit = 1 << i;
        if ((now_buttons ^ record_buttons) & bit) {
            fprintf(record_file, "+%" PRIu64 " %s %d\n", at,
                    now_buttons & bit ? "down" : "up",
                    *g_keyboard_scancode_map[buttons[i]]);
        }
    }
    record_buttons = now_buttons;
    for (int i = 0; i < CONTROLLER_AXIS__COUNT; i++) {
        int16_t v = now_axes ? now_axes[i] : 0;
        if (v != record_axes[i]) {
            fprintf(record_file, "+%" PRIu64 " %s %.9f\n", at, axes[i],
                    v / 32767.0);
            record_axes[i] = v;
        }
    }
}

static void input_frame_tick(uint64_t frame);

/* Live pads: gamepad connects and disconnects take effect at a vblank. */
static void live_hotplug_apply(void)
{
    ControllerState *c;

    while ((c = g_queue_pop_head(&live_removed))) {
        if (c->bound >= 0 && !xemu_input_live_detach(c)) {
            /*
             * A memory unit rides on its hub: unplug, as normal mode does.
             * Its state goes with c (freed by the UI thread), not with the
             * unbind here.
             */
            void *mus[2] = { c->peripherals[0], c->peripherals[1] };
            char buf[128];

            for (int i = 0; i < 2; i++) {
                XmuState *xmu = mus[i];
                if (c->peripheral_types[i] == PERIPHERAL_XMU && xmu &&
                    xmu->dev) {
                    qdev_unplug((DeviceState *)xmu->dev, &error_abort);
                    object_unref(OBJECT(xmu->dev));
                    xmu->dev = NULL;
                }
                c->peripherals[i] = NULL;
            }
            snprintf(buf, sizeof(buf), "Port %d disconnected", c->bound + 1);
            xemu_queue_notification(buf);
            xemu_input_bind(c->bound, NULL, 0);
            c->peripherals[0] = mus[0];
            c->peripherals[1] = mus[1];
        }
        g_queue_push_tail(&live_freed, c);
    }
    while ((c = g_queue_pop_head(&live_added))) {
        if (c->bound < 0) {
            gamepad_autobind(c, true);
        }
    }
}

void xemu_input_frame_tick(uint64_t frame)
{
    xemu_input_bind_frame_tick();
    if (xemu_input_live_pads()) {
        live_hotplug_apply();
    }
    input_frame_tick(frame);
    if (record_file) {
        input_record_frame(frame);
    }
}

static void input_frame_tick(uint64_t frame)
{
    bool applied = false;

    if (icount_enabled()) {
        /*
         * Lines written before this vblank count for it; "+N" counts from
         * the frame they were written at.
         */
        test_input_poll();
    }
    test_input_frame = frame;
    for (GList *l = test_input_pending.head, *next; l; l = next) {
        TestInputPending *p = l->data;
        next = l->next;
        if (p->frame <= frame) {
            test_input_apply(p->line);
            g_free(p->line);
            g_free(p);
            g_queue_delete_link(&test_input_pending, l);
            applied = true;
        }
    }
    ControllerState *iter;
    if (input_at_vblanks()) {
        if (xemu_input_live_pads()) {
            int n;
            const bool *keys = SDL_GetKeyboardState(&n);
            memcpy(live_keys, keys, MIN(n, SDL_SCANCODE_COUNT) * sizeof(bool));
        }
        /* Every pad is published here and only here. */
        QTAILQ_FOREACH(iter, &available_controllers, entry) {
            xemu_input_publish_controller(iter);
        }
        return;
    }

    if (!applied) {
        return;
    }

    /* Publish now, so the change lands on this vblank, not the UI's next poll. */
    QTAILQ_FOREACH(iter, &available_controllers, entry) {
        if (iter->type == INPUT_DEVICE_SDL_KEYBOARD) {
            xemu_input_publish_controller(iter);
        }
    }
}

void xemu_input_update_sdl_kbd_controller_state(ControllerState *state)
{
    state->buttons = 0;
    memset(state->axis, 0, sizeof(state->axis));

    /*
     * Deterministic mode: the pipe is read at vblanks only (frame tick), and
     * the host keyboard is ignored.
     */
    static const bool no_keys[SDL_SCANCODE_COUNT];
    const bool *kbd = no_keys;
    if (!icount_enabled()) {
        test_input_poll();
        kbd = SDL_GetKeyboardState(NULL);
    } else if (xemu_input_live_pads()) {
        kbd = live_keys;
    }

#define KBD_STATE(btn) \
    (kbd[g_config.input.keyboard_controller_scancode_map.btn] || \
     test_input_override[g_config.input.keyboard_controller_scancode_map.btn])

    state->buttons |= KBD_STATE(a) << 0;
    state->buttons |= KBD_STATE(b) << 1;
    state->buttons |= KBD_STATE(x) << 2;
    state->buttons |= KBD_STATE(y) << 3;
    state->buttons |= KBD_STATE(dpad_left) << 4;
    state->buttons |= KBD_STATE(dpad_up) << 5;
    state->buttons |= KBD_STATE(dpad_right) << 6;
    state->buttons |= KBD_STATE(dpad_down) << 7;
    state->buttons |= KBD_STATE(back) << 8;
    state->buttons |= KBD_STATE(start) << 9;
    state->buttons |= KBD_STATE(white) << 10;
    state->buttons |= KBD_STATE(black) << 11;
    state->buttons |= KBD_STATE(lstick_btn) << 12;
    state->buttons |= KBD_STATE(rstick_btn) << 13;
    state->buttons |= KBD_STATE(guide) << 14;

    if (KBD_STATE(lstick_up))
        state->axis[CONTROLLER_AXIS_LSTICK_Y] = 32767;
    if (KBD_STATE(lstick_left))
        state->axis[CONTROLLER_AXIS_LSTICK_X] = -32768;
    if (KBD_STATE(lstick_right))
        state->axis[CONTROLLER_AXIS_LSTICK_X] = 32767;
    if (KBD_STATE(lstick_down))
        state->axis[CONTROLLER_AXIS_LSTICK_Y] = -32768;
    if (KBD_STATE(ltrigger))
        state->axis[CONTROLLER_AXIS_LTRIG] = 32767;

    if (KBD_STATE(rstick_up))
        state->axis[CONTROLLER_AXIS_RSTICK_Y] = 32767;
    if (KBD_STATE(rstick_left))
        state->axis[CONTROLLER_AXIS_RSTICK_X] = -32768;
    if (KBD_STATE(rstick_right))
        state->axis[CONTROLLER_AXIS_RSTICK_X] = 32767;
    if (KBD_STATE(rstick_down))
        state->axis[CONTROLLER_AXIS_RSTICK_Y] = -32768;
    if (KBD_STATE(rtrigger))
        state->axis[CONTROLLER_AXIS_RTRIG] = 32767;

#undef KBD_STATE

    for (int i = 0; i < CONTROLLER_AXIS__COUNT; i++) {
        if (test_axis_override[i]) {
            state->axis[i] = test_axis_override[i];
        }
    }
}

void xemu_input_update_sdl_controller_state(ControllerState *state)
{
    state->buttons = 0;
    memset(state->axis, 0, sizeof(state->axis));

#define SDL_MASK_BUTTON(state, btn, idx)                  \
    (SDL_GetGamepadButton(                                \
         (state)->sdl_gamepad,                            \
         (state)->controller_map->controller_mapping.btn) \
     << idx)

    state->buttons |= SDL_MASK_BUTTON(state, a, 0);
    state->buttons |= SDL_MASK_BUTTON(state, b, 1);
    state->buttons |= SDL_MASK_BUTTON(state, x, 2);
    state->buttons |= SDL_MASK_BUTTON(state, y, 3);
    state->buttons |= SDL_MASK_BUTTON(state, dpad_left, 4);
    state->buttons |= SDL_MASK_BUTTON(state, dpad_up, 5);
    state->buttons |= SDL_MASK_BUTTON(state, dpad_right, 6);
    state->buttons |= SDL_MASK_BUTTON(state, dpad_down, 7);
    state->buttons |= SDL_MASK_BUTTON(state, back, 8);
    state->buttons |= SDL_MASK_BUTTON(state, start, 9);
    state->buttons |= SDL_MASK_BUTTON(state, lshoulder, 10);
    state->buttons |= SDL_MASK_BUTTON(state, rshoulder, 11);
    state->buttons |= SDL_MASK_BUTTON(state, lstick_btn, 12);
    state->buttons |= SDL_MASK_BUTTON(state, rstick_btn, 13);
    state->buttons |= SDL_MASK_BUTTON(state, guide, 14);

#undef SDL_MASK_BUTTON

#define SDL_GET_AXIS(state, axis)    \
    SDL_GetGamepadAxis(              \
        (state)->sdl_gamepad,        \
        (state)->controller_map->controller_mapping.axis)

    state->axis[0] = SDL_GET_AXIS(state, axis_trigger_left);
    state->axis[1] = SDL_GET_AXIS(state, axis_trigger_right);
    state->axis[2] = SDL_GET_AXIS(state, axis_left_x);
    state->axis[3] = SDL_GET_AXIS(state, axis_left_y);
    state->axis[4] = SDL_GET_AXIS(state, axis_right_x);
    state->axis[5] = SDL_GET_AXIS(state, axis_right_y);

#undef SDL_GET_AXIS

// FIXME: Check range
#define INVERT_AXIS(controller_axis) \
    state->axis[controller_axis] = -1 - state->axis[controller_axis]

    if (state->controller_map->controller_mapping.invert_axis_left_x) {
        INVERT_AXIS(CONTROLLER_AXIS_LSTICK_X);
    }

    if (!state->controller_map->controller_mapping.invert_axis_left_y) {
        INVERT_AXIS(CONTROLLER_AXIS_LSTICK_Y);
    }

    if (state->controller_map->controller_mapping.invert_axis_right_x) {
        INVERT_AXIS(CONTROLLER_AXIS_RSTICK_X);
    }

    if (!state->controller_map->controller_mapping.invert_axis_right_y) {
        INVERT_AXIS(CONTROLLER_AXIS_RSTICK_Y);
    }

#undef INVERT_AXIS

    // xemu_input_print_controller_state(state);
}

void xemu_input_update_rumble(ControllerState *state)
{
    if (state->type != INPUT_DEVICE_SDL_GAMEPAD) {
        return;
    }

    if (!state->controller_map->enable_rumble) {
        return;
    }

    int64_t now = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
    if (ABS(now - state->last_rumble_updated_ts) <
        XEMU_INPUT_MIN_RUMBLE_UPDATE_INTERVAL_US) {
        return;
    }

    SDL_RumbleGamepad(state->sdl_gamepad, state->rumble_l, state->rumble_r, 250);
    state->last_rumble_updated_ts = qemu_clock_get_us(QEMU_CLOCK_REALTIME);
}

ControllerState *xemu_input_get_bound(int index)
{
    return bound_controllers[index];
}

void xemu_input_bind(int index, ControllerState *state, int save)
{
    // FIXME: Attempt to disable rumble when unbinding so it's not left
    // in rumble mode

    xemu_input_port_unpin(index);

    // Unbind existing controller
    if (bound_controllers[index]) {
        assert(bound_controllers[index]->device != NULL);
        Error *err = NULL;

        // Unbind any XMUs
        for (int i = 0; i < 2; i++) {
            if (bound_controllers[index]->peripherals[i]) {
                // If this was an XMU, unbind the XMU
                if (bound_controllers[index]->peripheral_types[i] ==
                    PERIPHERAL_XMU)
                    xemu_input_unbind_xmu(index, i);

                // Free up the XmuState and set the peripheral type to none
                g_free(bound_controllers[index]->peripherals[i]);
                bound_controllers[index]->peripherals[i] = NULL;
                bound_controllers[index]->peripheral_types[i] = PERIPHERAL_NONE;
            }
        }

        qdev_unplug((DeviceState *)bound_controllers[index]->device, &err);
        assert(err == NULL);

        bound_controllers[index]->bound = -1;
        bound_controllers[index]->device = NULL;
        bound_controllers[index] = NULL;
    }

    // Save this controller's GUID in settings for auto re-connect
    if (save) {
        char guid_buf[35] = { 0 };
        if (state) {
            if (state->type == INPUT_DEVICE_SDL_GAMEPAD) {
                SDL_GUIDToString(state->sdl_joystick_guid, guid_buf, sizeof(guid_buf));
            } else if (state->type == INPUT_DEVICE_SDL_KEYBOARD) {
                snprintf(guid_buf, sizeof(guid_buf), "keyboard");
            }
        }
        xemu_settings_set_string(port_index_to_settings_key_map[index], guid_buf);
        xemu_settings_set_string(port_index_to_driver_settings_key_map[index],
                                 bound_drivers[index]);
    }

    // Bind new controller
    if (state) {
        if (state->bound >= 0) {
            // Device was already bound to another port. Unbind it.
            xemu_input_bind(state->bound, NULL, 1);
        }

        bound_controllers[index] = state;
        bound_controllers[index]->bound = index;

        // An emulated pad already on the port with no host source (a
        // -device stub, a pinned port, a pad a load plugged): drive it
        // rather than plug a second one onto the occupied hub port.
        state->device = xemu_input_port_adopt_pad(index, bound_drivers[index]);
        if (state->device) {
            return;
        }

        char *tmp;

        // Create controller's internal USB hub.
        QDict *usbhub_qdict = qdict_new();
        qdict_put_str(usbhub_qdict, "driver", "usb-hub");
        tmp = g_strdup_printf("1.%d", xemu_input_hub_ports[index]);
        qdict_put_str(usbhub_qdict, "port", tmp);
        qdict_put_int(usbhub_qdict, "ports", 3);
        QemuOpts *usbhub_opts = qemu_opts_from_qdict(qemu_find_opts("device"), usbhub_qdict, &error_abort);
        DeviceState *usbhub_dev = qdev_device_add(usbhub_opts, &error_abort);
        g_free(tmp);

        // Create XID controller. This is connected to Port 1 of the controller's internal USB Hub
        QDict *qdict = qdict_new();

        // Specify device driver
        qdict_put_str(qdict, "driver", bound_drivers[index]);

        // Specify device identifier
        static int id_counter = 0;
        tmp = g_strdup_printf("gamepad_%d", id_counter++);
        qdict_put_str(qdict, "id", tmp);
        g_free(tmp);

        // Specify index/port
        qdict_put_int(qdict, "index", index);
        tmp = g_strdup_printf("1.%d.1", xemu_input_hub_ports[index]);
        qdict_put_str(qdict, "port", tmp);
        g_free(tmp);

        // Create the device
        QemuOpts *opts = qemu_opts_from_qdict(qemu_find_opts("device"), qdict, &error_abort);
        DeviceState *dev = qdev_device_add(opts, &error_abort);
        assert(dev);

        // Unref for eventual cleanup
        qobject_unref(usbhub_qdict);
        object_unref(OBJECT(usbhub_dev));
        qobject_unref(qdict);
        object_unref(OBJECT(dev));

        state->device = usbhub_dev;
    }
}

bool xemu_input_bind_xmu(int player_index, int expansion_slot_index,
                         const char *filename, bool is_rebind)
{
    assert(player_index >= 0 && player_index < 4);
    assert(expansion_slot_index >= 0 && expansion_slot_index < 2);

    ControllerState *player = bound_controllers[player_index];
    enum peripheral_type peripheral_type =
        player->peripheral_types[expansion_slot_index];
    if (peripheral_type != PERIPHERAL_XMU)
        return false;

    XmuState *xmu = (XmuState *)player->peripherals[expansion_slot_index];

    // Unbind existing XMU
    if (xmu->dev != NULL) {
        xemu_input_unbind_xmu(player_index, expansion_slot_index);
    }

    if (filename == NULL)
        return false;

    // Look for any other XMUs that are using this file, and unbind them
    for (int player_i = 0; player_i < 4; player_i++) {
        ControllerState *state = bound_controllers[player_i];
        if (state != NULL) {
            for (int peripheral_i = 0; peripheral_i < 2; peripheral_i++) {
                if (state->peripheral_types[peripheral_i] == PERIPHERAL_XMU) {
                    XmuState *xmu_i =
                        (XmuState *)state->peripherals[peripheral_i];
                    assert(xmu_i);

                    if (xmu_i->filename != NULL &&
                        strcmp(xmu_i->filename, filename) == 0) {
                        char *buf =
                            g_strdup_printf("This XMU is already mounted on "
                                            "player %d slot %c\r\n",
                                            player_i + 1, 'A' + peripheral_i);
                        xemu_queue_notification(buf);
                        g_free(buf);
                        return false;
                    }
                }
            }
        }
    }

    xmu->filename = g_strdup(filename);

    const int xmu_map[2] = { 2, 3 };
    char *tmp;

    static int id_counter = 0;
    tmp = g_strdup_printf("xmu_%d", id_counter++);

    // Add the file as a drive
    QDict *qdict1 = qdict_new();
    qdict_put_str(qdict1, "id", tmp);
    qdict_put_str(qdict1, "format", "raw");
    qdict_put_str(qdict1, "file", filename);

    QemuOpts *drvopts =
        qemu_opts_from_qdict(qemu_find_opts("drive"), qdict1, &error_abort);

    DriveInfo *dinfo = drive_new(drvopts, 0, &error_abort);
    assert(dinfo);

    // Create the usb-storage device
    QDict *qdict2 = qdict_new();

    // Specify device driver
    qdict_put_str(qdict2, "driver", "usb-storage");

    // Specify device identifier
    qdict_put_str(qdict2, "drive", tmp);
    g_free(tmp);

    // Specify index/port
    tmp = g_strdup_printf("1.%d.%d", xemu_input_hub_ports[player_index],
                          xmu_map[expansion_slot_index]);
    qdict_put_str(qdict2, "port", tmp);
    g_free(tmp);

    // Create the device
    QemuOpts *opts =
        qemu_opts_from_qdict(qemu_find_opts("device"), qdict2, &error_abort);

    DeviceState *dev = qdev_device_add(opts, &error_abort);
    assert(dev);

    xmu->dev = (void *)dev;

    // Unref for eventual cleanup
    qobject_unref(qdict1);
    qobject_unref(qdict2);

    if (!is_rebind) {
        xemu_save_peripheral_settings(player_index, expansion_slot_index,
                                      peripheral_type, xmu->filename);
    }

    return true;
}

void xemu_input_unbind_xmu(int player_index, int expansion_slot_index)
{
    assert(player_index >= 0 && player_index < 4);
    assert(expansion_slot_index >= 0 && expansion_slot_index < 2);

    ControllerState *state = bound_controllers[player_index];
    if (state->peripheral_types[expansion_slot_index] != PERIPHERAL_XMU)
        return;

    XmuState *xmu = (XmuState *)state->peripherals[expansion_slot_index];
    if (xmu != NULL) {
        if (xmu->dev != NULL) {
            qdev_unplug((DeviceState *)xmu->dev, &error_abort);
            object_unref(OBJECT(xmu->dev));
            xmu->dev = NULL;
        }

        g_free((void *)xmu->filename);
        xmu->filename = NULL;
    }
}

void xemu_input_rebind_xmu(int port)
{
    // Try to bind peripherals back to controller
    for (int i = 0; i < 2; i++) {
        enum peripheral_type peripheral_type =
            (enum peripheral_type)(*peripheral_types_settings_map[port][i]);

        // If peripheralType is out of range, change the settings for this
        // controller and peripheral port to default
        if (peripheral_type < PERIPHERAL_NONE ||
            peripheral_type >= PERIPHERAL_TYPE_COUNT) {
            xemu_save_peripheral_settings(port, i, PERIPHERAL_NONE, NULL);
            peripheral_type = PERIPHERAL_NONE;
        }

        const char *param = *peripheral_params_settings_map[port][i];

        if (peripheral_type == PERIPHERAL_XMU) {
            if (param != NULL && strlen(param) > 0) {
                // This is an XMU and needs to be bound to this controller
                if (qemu_access(param, R_OK | W_OK) == 0) {
                    bound_controllers[port]->peripheral_types[i] =
                        peripheral_type;
                    bound_controllers[port]->peripherals[i] =
                        g_malloc(sizeof(XmuState));
                    memset(bound_controllers[port]->peripherals[i], 0,
                           sizeof(XmuState));
                    bool did_bind = xemu_input_bind_xmu(port, i, param, true);
                    if (did_bind) {
                        char *buf =
                            g_strdup_printf("Connected XMU %s to port %d%c",
                                            param, port + 1, 'A' + i);
                        xemu_queue_notification(buf);
                        g_free(buf);
                    }
                } else {
                    char *buf =
                        g_strdup_printf("Unable to bind XMU at %s to port %d%c",
                                        param, port + 1, 'A' + i);
                    xemu_queue_error_message(buf);
                    g_free(buf);
                }
            }
        }
    }
}

void xemu_input_set_test_mode(int enabled)
{
    test_mode = enabled;
}

int xemu_input_get_test_mode(void)
{
    return test_mode;
}

void xemu_input_reset_input_mapping(ControllerState *state)
{
    if (state->type == INPUT_DEVICE_SDL_GAMEPAD) {
        char guid[35] = { 0 };
        SDL_GUIDToString(state->sdl_joystick_guid, guid, sizeof(guid));
        xemu_settings_reset_controller_mapping(guid);
    } else if (state->type == INPUT_DEVICE_SDL_KEYBOARD) {
        xemu_settings_reset_keyboard_mapping();
    }
}
