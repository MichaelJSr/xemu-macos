/*
 * xemu trace recording: guest memory at every vblank, written to a file
 *
 * XEMU_TRACE_RECORD=<file> writes one line per vblank while the VM runs,
 * for demos played in real time (windowed or headless) and for any run:
 *
 *   # xemu trace recording: <n> <frame> <hex per region>; ...
 *   # regions 0x1000:16 v0x80012340:64
 *   # load frame=1234
 *   1 1235 00112233... 8899aabb...
 *
 * <n> counts vblanks since the last load (or launch), as the "+N" of an
 * XEMU_INPUT_RECORD recording does, and <frame> is the process's frame
 * counter (query-xemu-frame, the "frame" of xemu-frame-advance's trace
 * samples). Each region's bytes come as lowercase hex, as in those
 * samples, or "-" when guest RAM does not back the whole region. Each
 * load writes a "# load" line, and a region change a "# regions" line.
 *
 * Regions: XEMU_TRACE_REGIONS=<addr>:<size>[,<addr>:<size>...] (C number
 * syntax, size 1-4096, ending at or below 4 GiB, at most 256 regions); a
 * "v" before the address reads it through CPU 0's page tables at that
 * vblank. QMP xemu-trace-regions replaces the list from the next vblank
 * on (an empty list leaves only the counts).
 *
 * Under icount a line is taken at the vblank itself, before the guest
 * sees it, where xemu-frame-advance takes its trace samples, so a
 * recorded run's lines equal a traced frame advance of its replay. The
 * bytes are RAM as it is: recording never changes the run. (With a GPU
 * renderer, a report slot the guest has not read yet still holds its old
 * bytes, since deterministic reports are lazy; a traced frame advance
 * delivers them eagerly instead, which on Vulkan can change later report
 * values, so compare those under the null renderer.) In normal mode the
 * sample is queued to the vCPU thread, which takes it between two blocks
 * of guest code around that vblank's interrupt (never mid-instruction;
 * where the guest is depends on host timing).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/rcu.h"
#include "qapi/error.h"
#include "qapi/clone-visitor.h"
#include "qapi/qapi-commands-xemu.h"
#include "qapi/qapi-visit-xemu.h"
#include "exec/icount.h"
#include "exec/cpu-common.h"
#include "exec/target_page.h"
#include "hw/core/cpu.h"
#include "xemu-trace-record.h"

#define MAX_REGIONS 256
#define MAX_REGION_SIZE 4096

static FILE *trace_file;
static XemuTraceRegionList *trace_regions, *trace_pending;
static bool trace_pending_set; /* trace_pending (NULL: no regions) is due */
static bool trace_regions_changed;
static uint64_t trace_base;   /* frame counter at the last load */
static uint32_t trace_gen;    /* bumped by loads: drops queued samples */

/*
 * Shared with xemu-frame-advance traces (ui/xemu.c): a region list's
 * limits. The guest's RAM and virtual addresses lie below 4 GiB, which
 * also keeps address + size from wrapping.
 */
bool xemu_trace_regions_check(XemuTraceRegionList *list, Error **errp)
{
    int n = 0;

    for (; list; list = list->next) {
        XemuTraceRegion *r = list->value;

        if (r->size < 1 || r->size > MAX_REGION_SIZE) {
            error_setg(errp, "a trace region's size must be 1 to %d",
                       MAX_REGION_SIZE);
            return false;
        }
        if (r->address > UINT32_MAX ||
            r->size > (int64_t)(UINT32_MAX - r->address) + 1) {
            error_setg(errp, "trace region 0x%" PRIx64 ":%" PRId64
                       " ends past 4 GiB", r->address, r->size);
            return false;
        }
        if (++n > MAX_REGIONS) {
            error_setg(errp, "at most %d trace regions", MAX_REGIONS);
            return false;
        }
    }
    return true;
}

static XemuTraceRegionList *parse_regions(const char *spec, Error **errp)
{
    XemuTraceRegionList *head = NULL, **tail = &head;
    g_auto(GStrv) items = g_strsplit(spec, ",", -1);

    for (char **it = items; *it; it++) {
        const char *p = g_strstrip(*it), *end;
        XemuTraceRegion *r;
        uint64_t addr, size;
        bool virt = false;

        if (!*p) {
            continue;
        }
        if (*p == 'v' || *p == 'V') {
            virt = true;
            p++;
        }
        if (qemu_strtou64(p, &end, 0, &addr) || *end != ':' ||
            qemu_strtou64(end + 1, NULL, 0, &size)) {
            error_setg(errp, "XEMU_TRACE_REGIONS: '%s' is not "
                       "[v]<address>:<size>", *it);
            qapi_free_XemuTraceRegionList(head);
            return NULL;
        }
        r = g_new0(XemuTraceRegion, 1);
        r->address = addr;
        r->size = size;
        r->has_q_virtual = virt;
        r->q_virtual = virt;
        QAPI_LIST_APPEND(tail, r);
    }
    if (!xemu_trace_regions_check(head, errp)) {
        qapi_free_XemuTraceRegionList(head);
        return NULL;
    }
    return head;
}

/*
 * Each line reaches the file at once, for a harness that tails it and in
 * case xemu is killed. (setvbuf's _IOLBF is full buffering on Windows.)
 */
static void trace_write(const char *line)
{
    fputs(line, trace_file);
    fflush(trace_file);
}

static void write_regions(void)
{
    GString *s = g_string_new("# regions");

    for (XemuTraceRegionList *l = trace_regions; l; l = l->next) {
        g_string_append_printf(s, " %s0x%" PRIx64 ":%" PRId64,
                               l->value->q_virtual ? "v" : "",
                               l->value->address, l->value->size);
    }
    g_string_append_c(s, '\n');
    trace_write(s->str);
    g_string_free(s, true);
}

void xemu_trace_record_init(void)
{
    const char *path = getenv("XEMU_TRACE_RECORD");
    const char *spec = getenv("XEMU_TRACE_REGIONS");
    Error *err = NULL;

    if (!path || !path[0]) {
        return;
    }
    if (spec && spec[0]) {
        trace_regions = parse_regions(spec, &err);
        if (err) {
            error_report_err(err);
            exit(1);
        }
    }
    trace_file = fopen(path, "w");
    if (!trace_file) {
        fprintf(stderr, "xemu: XEMU_TRACE_RECORD open failed: %s\n", path);
        return;
    }
    trace_write("# xemu trace recording: <n> <frame> <hex per region>; n "
                "counts vblanks from the last load, '-' marks a region RAM "
                "does not back\n");
    write_regions();
}

/* CPU 0's page tables, page by page. */
static bool read_virtual(uint8_t *out, const uint8_t *ram, uint64_t ram_size,
                         vaddr address, uint64_t size)
{
    CPUState *cpu = qemu_get_cpu(0);
    uint64_t page = qemu_target_page_size();

    for (uint64_t off = 0; off < size;) {
        vaddr va = address + off;
        uint64_t in_page = va & (page - 1);
        uint64_t chunk = MIN(size - off, page - in_page);
        hwaddr pa = cpu_get_phys_page_debug(cpu, va - in_page);

        if (pa == -1 || pa >= ram_size || in_page + chunk > ram_size - pa) {
            return false;
        }
        memcpy(out + off, ram + pa + in_page, chunk);
        off += chunk;
    }
    return true;
}

/*
 * Shared with xemu-frame-advance traces: region r's bytes into buf, or
 * false when guest RAM does not back all of it.
 */
bool xemu_trace_region_read(uint8_t *buf, const uint8_t *ram,
                            uint64_t ram_size, const XemuTraceRegion *r)
{
    if (r->q_virtual) {
        return read_virtual(buf, ram, ram_size, r->address, r->size);
    }
    if (r->address >= ram_size || r->size > ram_size - r->address) {
        return false;
    }
    memcpy(buf, ram + r->address, r->size);
    return true;
}

static void trace_sample(uint64_t frame)
{
    RAMBlock *rb;
    const uint8_t *ram;
    uint64_t ram_size;
    uint8_t buf[MAX_REGION_SIZE];
    GString *s;

    WITH_RCU_READ_LOCK_GUARD() {
        rb = qemu_ram_block_by_name("xbox.ram");
    }
    if (!rb) {
        return;
    }
    ram = qemu_ram_get_host_addr(rb);
    ram_size = qemu_ram_get_used_length(rb);

    if (trace_regions_changed) {
        trace_regions_changed = false;
        write_regions();
    }
    s = g_string_sized_new(64);
    g_string_append_printf(s, "%" PRIu64 " %" PRIu64, frame - trace_base,
                           frame);
    for (XemuTraceRegionList *l = trace_regions; l; l = l->next) {
        g_string_append_c(s, ' ');
        if (xemu_trace_region_read(buf, ram, ram_size, l->value)) {
            qemu_hexdump_line(s, buf, l->value->size, 0, 0);
        } else {
            g_string_append_c(s, '-');
        }
    }
    g_string_append_c(s, '\n');
    trace_write(s->str);
    g_string_free(s, true);
}

/* Normal mode: the vCPU thread takes the sample (BQL held). */
static void trace_sample_on_cpu(CPUState *cpu, run_on_cpu_data data)
{
    uint64_t v = data.target_ptr;
    uint64_t frame = v >> 8;

    if ((uint8_t)v == (uint8_t)trace_gen) {
        trace_sample(frame);
    }
}

void xemu_trace_record_frame(uint64_t frame)
{
    if (!trace_file) {
        return;
    }
    if (trace_pending_set) {
        qapi_free_XemuTraceRegionList(trace_regions);
        trace_regions = trace_pending;
        trace_pending = NULL;
        trace_pending_set = false;
        trace_regions_changed = true;
    }
    if (icount_enabled()) {
        trace_sample(frame);
    } else if (first_cpu) {
        async_run_on_cpu(first_cpu, trace_sample_on_cpu,
                         RUN_ON_CPU_TARGET_PTR(frame << 8 |
                                               (uint8_t)trace_gen));
    }
}

void xemu_trace_record_load(uint64_t frame)
{
    if (!trace_file) {
        return;
    }
    g_autofree char *line = g_strdup_printf("# load frame=%" PRIu64 "\n",
                                            frame);

    trace_gen++;
    trace_base = frame;
    trace_write(line);
}

void qmp_xemu_trace_regions(XemuTraceRegionList *regions, Error **errp)
{
    if (!trace_file) {
        error_setg(errp, "XEMU_TRACE_RECORD is not set");
        return;
    }
    if (!xemu_trace_regions_check(regions, errp)) {
        return;
    }
    /* An empty list arrives as NULL, and is a change too. */
    qapi_free_XemuTraceRegionList(trace_pending);
    trace_pending = QAPI_CLONE(XemuTraceRegionList, regions);
    trace_pending_set = true;
}
