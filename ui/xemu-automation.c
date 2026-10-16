/*
 * xemu.toml's [automation] table
 *
 *   [automation]
 *   ram_shm = "azurik-live"                         # as XEMU_RAM_SHM
 *   qmp = "tcp:127.0.0.1:4444,server=on,wait=off"   # what -qmp takes
 *   enabled = true                                  # Settings toggle
 *
 * The table applies only to the player's own interactive launch: xemu.toml
 * is the SDL pref dir's (no -config_path, no XEMU_BASE_PATH, not portable
 * mode) and the guest name is not the fork's automation marker
 * (-name xemu-fork-auto). Sandboxes and benches therefore never take the
 * player's name or port, even from a copied config. XEMU_RAM_SHM (when
 * non-empty) overrides ram_shm, and a QMP monitor on the command line
 * (-qmp, -qmp-pretty, -mon mode=control) overrides qmp.
 *
 * Each key that is set gets one stderr line: xemu: [automation] <key> =
 * "<value>": then "applied", "ignored (<why>)" or "skipped: <why>". An
 * endpoint that cannot open (something listens on that address and port,
 * a live listener holds the unix socket path, a live xemu holds the
 * shared-memory name) is skipped with that line and an in-app
 * notification, and xemu keeps running. A listener on another address
 * doesn't count: on macOS 127.0.0.1:P binds beside another program's
 * *:P (QEMU sets SO_REUSEADDR) and the key applies. The monitor's wait is
 * always off, so a launch never blocks on a client.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qemu/config-file.h"
#include "qemu/option.h"
#include "qapi/error.h"
#include "chardev/char.h"
#include "monitor/monitor.h"
#include "system/system.h"

#include "xemu-automation.h"
#include "xemu-notifications.h"
#include "xemu-settings.h"

#define AUTOMATION_MARKER "xemu-fork-auto"

static char *toml_ram_shm; /* the table's ram_shm, when it applies */

void xemu_automation_skipped(const char *line, const char *notification)
{
    fprintf(stderr, "%s\n", line);
    xemu_queue_notification(notification);
}

static bool key_set(const char *v)
{
    return v && v[0];
}

/* NULL when the table applies to this launch, else why not. */
static const char *automation_veto(void)
{
    const char *why = xemu_settings_why_not_pref_dir();
    if (why) {
        return why;
    }
    const char *name = qemu_get_vm_name();
    if (name && !strcmp(name, AUTOMATION_MARKER)) {
        return "-name " AUTOMATION_MARKER;
    }
    if (!g_config.automation.enabled) {
        return "turned off in Settings: automation.enabled = false";
    }
    return NULL;
}

static int mon_is_qmp(void *opaque, QemuOpts *opts, Error **errp)
{
    const char *mode = qemu_opt_get(opts, "mode");
    return mode && !strcmp(mode, "control");
}

static void automation_open_qmp(const char *spec)
{
    Error *err = NULL;
    Chardev *chr = NULL;
    QemuOpts *opts = qemu_chr_parse_compat("xemu-automation-qmp", spec, false);

    if (!opts) {
        error_setg(&err, "not a -qmp argument");
    } else {
        /* A server socket waits for its first client unless told not to. */
        if (qemu_opt_get_bool(opts, "server", false)) {
            qemu_opt_set(opts, "wait", "off", &error_abort);
        }
        chr = qemu_chr_new_from_opts(opts, NULL, &err);
        qemu_opts_del(opts);
    }
    if (chr) {
        monitor_init_qmp(chr, false, &err);
        if (err) {
            object_unparent(OBJECT(chr));
        }
    }

    if (err) {
        g_autofree char *line = g_strdup_printf(
            "xemu: [automation] qmp = \"%s\": skipped: %s", spec,
            error_get_pretty(err));
        g_autofree char *toast = g_strdup_printf(
            "Automation: QMP %s did not open (%s)", spec,
            error_get_pretty(err));
        xemu_automation_skipped(line, toast);
        error_free(err);
        return;
    }
    fprintf(stderr, "xemu: [automation] qmp = \"%s\": applied\n", spec);
}

void xemu_automation_apply(void)
{
    const char *ram_shm = g_config.automation.ram_shm;
    const char *qmp = g_config.automation.qmp;

    if (!key_set(ram_shm) && !key_set(qmp)) {
        return;
    }

    const char *veto = automation_veto();

    if (key_set(ram_shm)) {
        const char *env = g_getenv("XEMU_RAM_SHM");
        if (veto) {
            fprintf(stderr, "xemu: [automation] ram_shm = \"%s\": ignored (%s)\n",
                    ram_shm, veto);
        } else if (key_set(env)) {
            fprintf(stderr, "xemu: [automation] ram_shm = \"%s\": ignored "
                    "(XEMU_RAM_SHM=%s overrides it)\n", ram_shm, env);
        } else {
            /* hw/xbox/xbox.c prints the outcome when it builds guest RAM. */
            toml_ram_shm = g_strdup(ram_shm);
        }
    }

    if (key_set(qmp)) {
        if (veto) {
            fprintf(stderr, "xemu: [automation] qmp = \"%s\": ignored (%s)\n",
                    qmp, veto);
        } else if (qemu_opts_foreach(qemu_find_opts("mon"), mon_is_qmp, NULL,
                                     NULL)) {
            fprintf(stderr, "xemu: [automation] qmp = \"%s\": ignored "
                    "(a QMP monitor on the command line overrides it)\n", qmp);
        } else {
            automation_open_qmp(qmp);
        }
    }
}

const char *xemu_automation_ram_shm(bool *from_toml)
{
    const char *env = g_getenv("XEMU_RAM_SHM");

    *from_toml = false;
    if (key_set(env)) {
        return env;
    }
    if (toml_ram_shm) {
        *from_toml = true;
        return toml_ram_shm;
    }
    return NULL;
}
