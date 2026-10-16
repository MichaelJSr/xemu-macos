/*
 * xemu input binding: host sources that change under a plugged pad
 *
 * An emulated pad (usb-xbox-gamepad[-s] at hub port 1.P.1, P from
 * xemu_input_hub_ports) reads its port's host source, bound_controllers[index], at
 * every guest poll (hw/xbox/xid.c), so the source can change under it
 * without a replug. Loads, checkpoint restores and resets load into the
 * devices that exist and never touch the bindings, so a bound port keeps
 * driving its pad across them.
 *
 * QMP xemu-input-bind sets a port's source (the keyboard pad, which
 * XEMU_INPUT_PIPE also drives, a host gamepad by GUID, or none) and pins
 * the port: its pad stays plugged while a host gamepad comes and goes,
 * and auto-binding skips it. query-xemu-input-bindings lists the ports.
 * xemu_input_bind() (the settings UI, auto-binding) drives a pad already
 * on the port instead of plugging a second one onto the occupied port.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qapi/error.h"
#include "qapi/util.h"
#include "qapi/qapi-commands-xemu.h"
#include "qobject/qdict.h"
#include "monitor/qdev.h"
#include "hw/qdev-core.h"
#include "hw/xbox/xid.h"
#include "xemu-input.h"
#include "xemu-notifications.h"

typedef struct PortPin {
    bool on;                /* bound with xemu-input-bind */
    bool pending;           /* apply at the next vblank */
    XemuInputSource source; /* KEYBOARD, GAMEPAD or NONE */
    SDL_GUID guid;
} PortPin;

static PortPin pins[4];

typedef struct FindUsb {
    BusState *bus;
    const char *path;
    const char *type;
    DeviceState *found;
} FindUsb;

static int find_usb_cb(Object *obj, void *opaque)
{
    FindUsb *f = opaque;
    USBDevice *u = (USBDevice *)object_dynamic_cast(obj, TYPE_USB_DEVICE);

    if (!u || !DEVICE(u)->realized || !u->port ||
        (f->bus && DEVICE(u)->parent_bus != f->bus) ||
        strcmp(u->port->path, f->path) ||
        (f->type && !object_dynamic_cast(obj, f->type))) {
        return 0;
    }
    f->found = DEVICE(u);
    return 1;
}

static DeviceState *usb_dev_at(BusState *bus, const char *path,
                               const char *type)
{
    FindUsb f = { .bus = bus, .path = path, .type = type };

    object_child_foreach_recursive(object_get_root(), find_usb_cb, &f);
    return f.found;
}

/* The bus the pads go on: the motherboard hub's (port 1, from vl.c). */
static BusState *pad_bus(void)
{
    DeviceState *hub = usb_dev_at(NULL, "1", "usb-hub");

    return hub ? hub->parent_bus : NULL;
}

static USBXIDGamepadState *as_pad(Object *obj)
{
    if (object_dynamic_cast(obj, TYPE_USB_XID_GAMEPAD) ||
        object_dynamic_cast(obj, TYPE_USB_XID_GAMEPAD_S)) {
        return (USBXIDGamepadState *)obj;
    }
    return NULL;
}

typedef struct FindPad {
    int index;
    USBXIDGamepadState *found;
    bool neutral;
} FindPad;

static int find_pad_cb(Object *obj, void *opaque)
{
    FindPad *f = opaque;
    USBXIDGamepadState *s = as_pad(obj);

    if (!s || !DEVICE(s)->realized || s->device_index != f->index) {
        return 0;
    }
    if (f->neutral) {
        /* As a fresh pad and xid_post_load's neutral report. */
        memset(&s->in_state, 0, sizeof(s->in_state));
        s->in_state.bLength = sizeof(s->in_state);
    }
    if (!f->found) {
        f->found = s;
    }
    return 0;
}

/* An emulated pad reading port index; the one at 1.P.1 if several. */
static USBXIDGamepadState *port_pad(int index, bool neutral)
{
    FindPad f = { .index = index, .neutral = neutral };
    char path[16];

    object_child_foreach_recursive(object_get_root(), find_pad_cb, &f);
    snprintf(path, sizeof(path), "1.%d.1", xemu_input_hub_ports[index]);
    if (f.found && f.found->dev.port && strcmp(f.found->dev.port->path, path)) {
        DeviceState *at = usb_dev_at(DEVICE(f.found)->parent_bus, path, NULL);
        USBXIDGamepadState *s = at ? as_pad(OBJECT(at)) : NULL;
        if (s && s->device_index == index) {
            f.found = s;
        }
    }
    return f.found;
}

static DeviceState *plug(const char *driver, const char *port, int index,
                         Error **errp)
{
    QDict *opts = qdict_new();
    DeviceState *dev;

    qdict_put_str(opts, "driver", driver);
    qdict_put_str(opts, "port", port);
    if (!strcmp(driver, "usb-hub")) {
        qdict_put_int(opts, "ports", 3); /* as xemu_input_bind */
    } else {
        qdict_put_int(opts, "index", index);
    }
    dev = qdev_device_add_from_qdict(opts, true, errp);
    qobject_unref(opts);
    if (dev) {
        object_unref(OBJECT(dev)); /* the QOM tree keeps it */
    }
    return dev;
}

typedef enum PortSlot { SLOT_EMPTY, SLOT_PAD, SLOT_OTHER } PortSlot;

/*
 * What sits where xemu_input_bind() plugs port index's pad: nothing, a
 * pad (a hub at 1.P, or a pad of this index at 1.P) or something else.
 */
static PortSlot port_slot(int index, BusState **bus, DeviceState **at)
{
    char path[16];
    USBXIDGamepadState *s;

    *bus = pad_bus();
    if (!*bus) {
        *at = NULL;
        return SLOT_OTHER;
    }
    snprintf(path, sizeof(path), "1.%d", xemu_input_hub_ports[index]);
    *at = usb_dev_at(*bus, path, NULL);
    if (!*at) {
        return SLOT_EMPTY;
    }
    if (object_dynamic_cast(OBJECT(*at), "usb-hub")) {
        return SLOT_PAD;
    }
    s = as_pad(OBJECT(*at));
    return s && s->device_index == index ? SLOT_PAD : SLOT_OTHER;
}

/*
 * The emulated pad already where xemu_input_bind() would plug one for
 * port index: what the port's source should own. A hub at 1.P without a
 * pad at 1.P.1 gets one of driver.
 */
void *xemu_input_port_adopt_pad(int index, const char *driver)
{
    BusState *bus;
    DeviceState *at;
    char path[16];

    if (port_slot(index, &bus, &at) != SLOT_PAD) {
        return NULL;
    }
    snprintf(path, sizeof(path), "1.%d.1", xemu_input_hub_ports[index]);
    if (object_dynamic_cast(OBJECT(at), "usb-hub") &&
        !usb_dev_at(bus, path, NULL)) {
        Error *err = NULL;
        if (!plug(driver, path, index, &err)) {
            error_report_err(err);
        }
    }
    return at;
}

bool xemu_input_port_pinned(int index)
{
    return pins[index].on;
}

static int pending_count;

void xemu_input_port_unpin(int index)
{
    pending_count -= pins[index].pending;
    memset(&pins[index], 0, sizeof(pins[index]));
}

static bool has_mu(ControllerState *c)
{
    return c && (c->peripherals[0] || c->peripherals[1]);
}

static bool guid_eq(SDL_GUID a, SDL_GUID b)
{
    return !memcmp(a.data, b.data, sizeof(a.data));
}

static ControllerState *keyboard_source(void)
{
    ControllerState *c;

    QTAILQ_FOREACH(c, &available_controllers, entry) {
        if (c->type == INPUT_DEVICE_SDL_KEYBOARD) {
            return c;
        }
    }
    return NULL;
}

/* An open gamepad with guid: the one on index, else a free one, else any. */
static ControllerState *gamepad_source(SDL_GUID guid, int index)
{
    ControllerState *c, *pick = NULL;

    QTAILQ_FOREACH(c, &available_controllers, entry) {
        if (c->type != INPUT_DEVICE_SDL_GAMEPAD ||
            !guid_eq(c->sdl_joystick_guid, guid)) {
            continue;
        }
        if (c->bound == index) {
            return c;
        }
        if (!pick || (pick->bound >= 0 && c->bound < 0)) {
            pick = c;
        }
    }
    return pick;
}

/* The port's source leaves; its pad stays plugged and reports neutral. */
static void port_detach(int index)
{
    ControllerState *c = bound_controllers[index];

    if (c) {
        c->bound = -1;
        c->device = NULL;
        bound_controllers[index] = NULL;
    }
    port_pad(index, true);
}

static bool port_attach(int index, ControllerState *c, Error **errp)
{
    ControllerState *old = bound_controllers[index];

    if (old) {
        /* The pad and any memory units on its hub stay; c drives them. */
        c->device = old->device;
        for (int i = 0; i < 2; i++) {
            c->peripheral_types[i] = old->peripheral_types[i];
            c->peripherals[i] = old->peripherals[i];
            old->peripheral_types[i] = PERIPHERAL_NONE;
            old->peripherals[i] = NULL;
        }
        old->bound = -1;
        old->device = NULL;
    } else {
        c->device = xemu_input_port_adopt_pad(index, bound_drivers[index]);
    }
    if (c->device) {
        bound_controllers[index] = c;
        c->bound = index;
        return true;
    }

    /* No pad on the port: plug one, as binding it in the settings does. */
    BusState *bus;
    DeviceState *at;
    if (port_slot(index, &bus, &at) != SLOT_EMPTY) {
        error_setg(errp, "port %d's hub port 1.%d holds %s, not a pad",
                   index + 1, xemu_input_hub_ports[index],
                   at ? object_get_typename(OBJECT(at)) : "no USB bus");
        return false;
    }
    PortPin pin = pins[index];
    xemu_input_bind(index, c, 0);
    pins[index] = pin;
    xemu_input_rebind_xmu(index);
    return true;
}

/*
 * Bind port index to what its pin asks for (check only, with dry). A
 * source on another port moves here and leaves that port's pad in place.
 */
static bool port_apply(int index, bool dry, Error **errp)
{
    PortPin *pin = &pins[index];
    ControllerState *c = NULL, *old = bound_controllers[index];

    if (pin->source == XEMU_INPUT_SOURCE_KEYBOARD) {
        c = keyboard_source();
    } else if (pin->source == XEMU_INPUT_SOURCE_GAMEPAD) {
        c = gamepad_source(pin->guid, index);
    }
    if (c == old) {
        return true;
    }
    if (!c && has_mu(old)) {
        error_setg(errp, "port %d has a memory unit bound; unbind it in "
                   "the settings first", index + 1);
        return false;
    }
    if (c && c->bound >= 0 && has_mu(c)) {
        error_setg(errp, "'%s' drives port %d, which has a memory unit "
                   "bound; unbind it in the settings first", c->name,
                   c->bound + 1);
        return false;
    }
    if (c && !old) {
        BusState *bus;
        DeviceState *at;

        if (port_slot(index, &bus, &at) == SLOT_OTHER) {
            error_setg(errp, "port %d's hub port 1.%d holds %s, not a pad",
                       index + 1, xemu_input_hub_ports[index],
                       at ? object_get_typename(OBJECT(at)) : "no USB bus");
            return false;
        }
    }
    if (dry) {
        return true;
    }

    if (c && c->bound >= 0) {
        int from = c->bound;
        port_detach(from);
        if (pins[from].on && !pins[from].pending) {
            /* That port no longer waits for this source. */
            pins[from].source = XEMU_INPUT_SOURCE_NONE;
        }
    }
    if (!c) {
        port_detach(index);
        return true;
    }
    return port_attach(index, c, errp);
}

void xemu_input_bind_frame_tick(void)
{
    if (!pending_count) {
        return;
    }
    for (int i = 0; i < 4; i++) {
        Error *err = NULL;

        if (!pins[i].pending) {
            continue;
        }
        pins[i].pending = false;
        pending_count--;
        if (!port_apply(i, false, &err)) {
            error_prepend(&err, "xemu-input-bind port %d: ", i + 1);
            error_report_err(err);
        }
    }
}

/* A host gamepad disconnected: a pinned port keeps its pad. */
bool xemu_input_pinned_detach(ControllerState *c)
{
    int index = c->bound;
    char buf[128];

    if (index < 0 || !pins[index].on || has_mu(c)) {
        return false;
    }
    port_detach(index);
    if (!pins[index].pending) {
        pins[index].source = XEMU_INPUT_SOURCE_GAMEPAD;
        pins[index].guid = c->sdl_joystick_guid;
    }
    snprintf(buf, sizeof(buf), "Port %d disconnected (pad kept)", index + 1);
    xemu_queue_notification(buf);
    return true;
}

/* A host gamepad connected: drive the pinned port that waits for it. */
int xemu_input_pinned_attach(ControllerState *c)
{
    for (int i = 0; i < 4; i++) {
        Error *err = NULL;

        if (!pins[i].on || pins[i].pending || bound_controllers[i] ||
            pins[i].source != XEMU_INPUT_SOURCE_GAMEPAD ||
            !guid_eq(pins[i].guid, c->sdl_joystick_guid)) {
            continue;
        }
        if (port_attach(i, c, &err)) {
            return i;
        }
        error_report_err(err);
    }
    return -1;
}

static char *guid_str(SDL_GUID guid)
{
    char buf[33];

    SDL_GUIDToString(guid, buf, sizeof(buf));
    return g_strdup(buf);
}

static XemuInputSource source_of(ControllerState *c)
{
    if (!c) {
        return XEMU_INPUT_SOURCE_NONE;
    }
    return c->type == INPUT_DEVICE_SDL_KEYBOARD ? XEMU_INPUT_SOURCE_KEYBOARD
                                                : XEMU_INPUT_SOURCE_GAMEPAD;
}

static XemuInputPort *port_info(int index)
{
    XemuInputPort *p = g_new0(XemuInputPort, 1);
    ControllerState *c = bound_controllers[index];
    USBXIDGamepadState *pad = port_pad(index, false);

    p->port = index + 1;
    p->source = source_of(c);
    if (c) {
        p->name = g_strdup(c->name);
        if (c->type == INPUT_DEVICE_SDL_GAMEPAD) {
            p->guid = guid_str(c->sdl_joystick_guid);
        }
    }
    p->pad = pad != NULL;
    if (pad) {
        p->driver = g_strdup(object_get_typename(OBJECT(pad)));
    }
    p->pinned = pins[index].on;
    if (p->pinned) {
        p->has_pinned_source = true;
        p->pinned_source = pins[index].source;
        if (pins[index].source == XEMU_INPUT_SOURCE_GAMEPAD) {
            p->pinned_guid = guid_str(pins[index].guid);
        }
    }
    p->pending = pins[index].pending;
    return p;
}

XemuInputBindings *qmp_query_xemu_input_bindings(Error **errp)
{
    XemuInputBindings *b = g_new0(XemuInputBindings, 1);
    XemuInputPortList **ports = &b->ports;
    XemuInputGamepadList **pads = &b->gamepads;
    ControllerState *c;

    for (int i = 0; i < 4; i++) {
        QAPI_LIST_APPEND(ports, port_info(i));
    }
    QTAILQ_FOREACH(c, &available_controllers, entry) {
        if (c->type == INPUT_DEVICE_SDL_GAMEPAD) {
            XemuInputGamepad *g = g_new0(XemuInputGamepad, 1);
            g->name = g_strdup(c->name);
            g->guid = guid_str(c->sdl_joystick_guid);
            g->has_port = c->bound >= 0;
            g->port = c->bound + 1;
            QAPI_LIST_APPEND(pads, g);
        }
    }
    return b;
}

XemuInputPort *qmp_xemu_input_bind(int64_t port, XemuInputSource source,
                                   const char *guid, Error **errp)
{
    PortPin saved;
    SDL_GUID g = { 0 };
    int index = port - 1;

    if (port < 1 || port > 4) {
        error_setg(errp, "port must be 1 to 4");
        return NULL;
    }
    if (source == XEMU_INPUT_SOURCE_GAMEPAD) {
        if (!guid || strlen(guid) != 32 ||
            strspn(guid, "0123456789abcdefABCDEF") != 32) {
            error_setg(errp, "source gamepad needs a guid of 32 hex digits");
            return NULL;
        }
        if (!SDL_WasInit(SDL_INIT_GAMEPAD)) {
            error_setg(errp, "host gamepads are not open (deterministic "
                       "mode opens none)");
            return NULL;
        }
        g = SDL_StringToGUID(guid);
    } else if (guid) {
        error_setg(errp, "guid applies only to source gamepad");
        return NULL;
    }
    if (source == XEMU_INPUT_SOURCE_PIPE) {
        source = XEMU_INPUT_SOURCE_KEYBOARD;
    }

    saved = pins[index];
    pins[index] = (PortPin){ .on = true, .source = source, .guid = g };
    if (!port_apply(index, true, errp)) {
        pins[index] = saved;
        return NULL;
    }
    /*
     * Pads change only at vblanks in deterministic mode and while
     * recording, so a source change waits for one too.
     */
    if (xemu_input_pads_at_vblanks()) {
        pins[index].pending = true;
        pending_count += !saved.pending;
    } else {
        pending_count -= saved.pending;
        if (!port_apply(index, false, errp)) {
            pins[index] = saved;
            pins[index].pending = false;
            return NULL;
        }
    }
    return port_info(index);
}
