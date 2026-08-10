/*
 * DSP tests.
 *
 * Copyright (c) 2025 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "hw/xbox/mcpx/apu/dsp/dsp.h"

/*
 * The standalone DSP test links libdsp.a on its own (deps: qemuutil,
 * dsp, glib) and deliberately does NOT pull in ui/xemu-settings.cc,
 * which is where the emulator normally defines g_config. The fork's
 * engine-mediation code in dsp.c (dsp_want_external_jit_engine /
 * dsp_set_engine) reads g_config.audio.{dsp_jit.enabled,use_dsp_jit}
 * directly, so libdsp.a carries an unresolved reference to g_config.
 * Provide a zero-initialised definition here to satisfy the link.
 *
 * Zero-init means dsp_want_external_jit_engine() returns false, so
 * dsp_init() selects the C interpreter engine (dsp_c_init). On
 * aarch64 that engine embeds the fork's inline ARM64 JIT, toggled at
 * runtime by the XEMU_DSP_JIT env var (see the differential test
 * below) rather than by g_config — the JIT layer is decoupled from
 * g_config on purpose (dsp56k_jit_set_enabled_from_config()).
 */
#include "ui/xemu-settings.h"
struct config g_config;

static void scratch_rw(void *opaque, uint8_t *ptr, uint32_t addr, size_t len, bool dir)
{
    assert(!"Not implemented");
}

static void fifo_rw(void *opaque, uint8_t *ptr, unsigned int index, size_t len, bool dir)
{
    assert(!"Not implemented");
}

static void load_prog(DSPState *s, const char *path)
{
    FILE *file = fopen(path, "r");
    assert(file && "Error opening file");

    char type, line[100], arg1[20], arg2[20];
    int addr, value;

    while (fgets(line, sizeof(line), file)) {
        if (sscanf(line, "%c %s %s", &type, arg1, arg2) >= 2) {
            switch (type) {
                case 'P':
                case 'X':
                case 'Y': {
                    assert(sscanf(arg1, "%x", &addr) == 1);
                    assert(sscanf(arg2, "%x", &value) == 1);
                    dsp_write_memory(s, type, addr, value);
                    break;
                }
            }
        } else {
            printf("Invalid line: %s\n", line);
            assert(0);
        }
    }

    fclose(file);
}

static void test_dsp_basic(void)
{
    g_autofree gchar *path = g_test_build_filename(G_TEST_DIST, "data", "basic", NULL);

    DSPState *s = dsp_init(NULL, scratch_rw, fifo_rw, false);

    load_prog(s, path);
    dsp_run(s, 1000);

    uint32_t v = dsp_read_memory(s, 'X', 3);
    g_assert_cmphex(v, ==, 0x123456);

    dsp_destroy(s);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/basic", test_dsp_basic);

    return g_test_run();
}
