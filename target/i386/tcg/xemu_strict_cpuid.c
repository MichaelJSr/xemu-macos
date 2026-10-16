/*
 * xemu strict CPUID decode (XEMU_STRICT_CPUID=1)
 *
 * QEMU's decoder raises #UD for a missing CPUID feature only when the
 * instruction's table entry names one (cpuid(...) in decode-new.c.inc).
 * The legacy SSE/MMX opcodes of the 0F map name none, so on the Xbox's
 * pentium3 (MMX and SSE, no SSE2) every SSE2 instruction except MFENCE and
 * MOVNTI runs, and so do FISTTP (SSE3) and LFENCE (SSE2, tabled as SSE).
 * Code built for a later CPU then works in xemu and faults on a real
 * Xbox. Strict mode makes those raise #UD as the CPU model would (the
 * table and the decode hook are in xemu-strict-cpuid.c.inc), and reports
 * each fault on stderr and as the QMP event XEMU_CPUID_FAULT, before the
 * guest's #UD handler runs. Off by default: translation is then unchanged.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "cpu.h"
#include "exec/helper-proto.h"
#include "helper-tcg.h"
#include "qapi/qapi-events-xemu.h"
#include "xemu-strict-cpuid.h"

/* Faults reported (stderr line and event) per launch; later ones only #UD. */
#define XEMU_STRICT_REPORT_MAX 32

static unsigned xemu_strict_faults;

bool xemu_strict_cpuid_enabled(void)
{
    static int on = -1;

    if (on < 0) {
        const char *e = getenv("XEMU_STRICT_CPUID");

        on = e && e[0] && strcmp(e, "0") != 0;
        if (on) {
            fprintf(stderr, "xemu: strict CPUID: instructions the guest CPU "
                    "lacks raise #UD\n");
        }
    }
    return on;
}

/*
 * need: XEMU_STRICT_* in bits 0-7, bit 8 set when the instruction has a
 * ModRM byte. insn: SSE prefix byte (0 for none) << 24 | escape byte
 * (0x0f, or 0 for the one-byte map) << 16 | opcode << 8 | ModRM.
 */
void helper_xemu_strict_ud(CPUX86State *env, uint32_t need, uint32_t insn)
{
    static const char *const names[] = {
        [XEMU_STRICT_MMX] = "MMX", [XEMU_STRICT_SSE] = "SSE",
        [XEMU_STRICT_SSE2] = "SSE2", [XEMU_STRICT_SSE3] = "SSE3",
    };
    static const XemuCpuidFeature features[] = {
        [XEMU_STRICT_MMX] = XEMU_CPUID_FEATURE_MMX,
        [XEMU_STRICT_SSE] = XEMU_CPUID_FEATURE_SSE,
        [XEMU_STRICT_SSE2] = XEMU_CPUID_FEATURE_SSE2,
        [XEMU_STRICT_SSE3] = XEMU_CPUID_FEATURE_SSE3,
    };
    unsigned n = qatomic_fetch_inc(&xemu_strict_faults) + 1;
    unsigned f = need & 0xff;

    if (n <= XEMU_STRICT_REPORT_MAX && f >= XEMU_STRICT_MMX &&
        f <= XEMU_STRICT_SSE3) {
        uint64_t pc = (uint32_t)(env->segs[R_CS].base + env->eip);
        g_autoptr(GString) op = g_string_new(NULL);

        for (int sh = 24; sh >= 0; sh -= 8) {
            uint8_t byte = insn >> sh;

            if (sh == 8 || (sh > 8 && byte) || (sh == 0 && (need & 0x100))) {
                g_string_append_printf(op, "%s%02X", op->len ? " " : "", byte);
            }
        }
        fprintf(stderr, "xemu: strict CPUID: #UD at %08" PRIx64 ": %s needs "
                "%s%s\n", pc, op->str, names[f],
                n == XEMU_STRICT_REPORT_MAX ? " (further faults not reported)"
                                            : "");
        /*
         * vCPU thread without the BQL: event emission takes only the
         * monitor locks (as XEMU_DEVICE_LOST does from the PFIFO thread).
         * Nothing here touches guest state.
         */
        qapi_event_send_xemu_cpuid_fault(pc, features[f], op->str, n);
    }
    raise_exception(env, EXCP06_ILLOP);
}
