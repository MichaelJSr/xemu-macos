/*
 * xemu trace recording: guest memory at every vblank, written to a file
 *
 * XEMU_TRACE_RECORD=<file> writes one line per vblank while the VM runs,
 * for demos played in real time (windowed or headless) and for any run:
 *
 *   # xemu trace recording: <n> <frame> <hex per region>; ...
 *   # regions 0x1000:16 v0x80012340:64 v[[0x83a6e2c0+0x20]+0x154]:4
 *   # load frame=1234
 *   1 1235 00112233... 8899aabb... 01000000
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
 * vblank. An address that is not a number is a hook address expression
 * (XemuHookRegion's addr, e.g. "[[0x83a6e2c0+0x20]+0x154]", the hooks'
 * parser and reader), always virtual, evaluated afresh at every line with
 * CPU 0's registers and page tables, so the region follows pointers the
 * guest re-allocates; its field is "?" when a dereference reads memory
 * that is not guest RAM (such as a null pointer; a freed heap block is
 * still RAM) and "-" when the bytes it points at aren't all RAM. A "# regions" line writes such a region
 * as "v<expression>:<size>", blanks dropped, so each item parses back as
 * a region reading the same memory. QMP xemu-trace-regions replaces the
 * list from the next vblank on (an empty list leaves only the counts).
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
#include "qemu/rcu.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-xemu.h"
#include "exec/icount.h"
#include "exec/cpu-common.h"
#include "exec/target_page.h"
#include "hw/core/cpu.h"
#include "exec/xemu-hook-expr.h"
#include "xemu-trace-record.h"

#define MAX_REGIONS 256
#define MAX_REGION_SIZE 4096

/* A recorded region: a fixed address, or an address expression. */
typedef struct TraceRegion {
    uint64_t address;           /* fixed */
    uint32_t size;
    bool virt;                  /* fixed: through CPU 0's page tables */
    XemuHookExpr *expr;         /* or NULL */
    char *text;                 /* expr's source, blanks dropped */
} TraceRegion;

static FILE *trace_file;
static GArray *trace_regions, *trace_pending;   /* TraceRegion, or NULL */
static bool trace_pending_set; /* trace_pending is due */
static bool trace_regions_changed;
static uint64_t trace_base;   /* frame counter at the last load */
static uint32_t trace_gen;    /* bumped by loads: drops queued samples */

static bool size_check(int64_t size, Error **errp)
{
    if (size < 1 || size > MAX_REGION_SIZE) {
        error_setg(errp, "a trace region's size must be 1 to %d",
                   MAX_REGION_SIZE);
        return false;
    }
    return true;
}

/*
 * The guest's RAM and virtual addresses lie below 4 GiB, which also keeps
 * address + size from wrapping.
 */
static bool fixed_check(uint64_t address, int64_t size, Error **errp)
{
    if (!size_check(size, errp)) {
        return false;
    }
    if (address > UINT32_MAX ||
        size > (int64_t)(UINT32_MAX - address) + 1) {
        error_setg(errp, "trace region 0x%" PRIx64 ":%" PRId64
                   " ends past 4 GiB", address, size);
        return false;
    }
    return true;
}

/* Shared with xemu-frame-advance traces (ui/xemu.c): a list's limits. */
bool xemu_trace_regions_check(XemuTraceRegionList *list, Error **errp)
{
    int n = 0;

    for (; list; list = list->next) {
        if (!fixed_check(list->value->address, list->value->size, errp)) {
            return false;
        }
        if (++n > MAX_REGIONS) {
            error_setg(errp, "at most %d trace regions", MAX_REGIONS);
            return false;
        }
    }
    return true;
}

static void region_clear(gpointer p)
{
    TraceRegion *r = p;

    g_free(r->expr);
    g_free(r->text);
}

static GArray *regions_new(void)
{
    GArray *a = g_array_new(false, true, sizeof(TraceRegion));

    g_array_set_clear_func(a, region_clear);
    return a;
}

static void regions_free(GArray *a)
{
    if (a) {
        g_array_free(a, true);
    }
}

static bool count_check(GArray *a, Error **errp)
{
    if (a->len >= MAX_REGIONS) {
        error_setg(errp, "at most %d trace regions", MAX_REGIONS);
        return false;
    }
    return true;
}

static bool add_fixed(GArray *a, uint64_t address, int64_t size, bool virt,
                      Error **errp)
{
    TraceRegion r = { .address = address, .size = size, .virt = virt };

    if (!fixed_check(address, size, errp) || !count_check(a, errp)) {
        return false;
    }
    g_array_append_val(a, r);
    return true;
}

static bool add_expr(GArray *a, const char *text, int64_t size, Error **errp)
{
    TraceRegion r = { .size = size, .virt = true };
    GString *g;

    if (!size_check(size, errp) || !count_check(a, errp)) {
        return false;
    }
    r.expr = xemu_hook_expr_new(text, errp);
    if (!r.expr) {
        return false;
    }
    /* Blanks only separate tokens: dropped, the "# regions" item is one. */
    g = g_string_new(NULL);
    for (const char *c = text; *c; c++) {
        if (!g_ascii_isspace(*c)) {
            g_string_append_c(g, *c);
        }
    }
    r.text = g_string_free(g, false);
    g_array_append_val(a, r);
    return true;
}

static GArray *parse_regions(const char *spec, Error **errp)
{
    GArray *a = regions_new();
    g_auto(GStrv) items = g_strsplit(spec, ",", -1);

    for (char **it = items; *it; it++) {
        const char *p = g_strstrip(*it), *colon, *end;
        g_autofree char *at = NULL;
        uint64_t addr, size;
        bool virt = false;

        if (!*p) {
            continue;
        }
        if (*p == 'v' || *p == 'V') {
            virt = true;
            p++;
        }
        colon = strchr(p, ':');
        if (!colon || qemu_strtou64(colon + 1, NULL, 0, &size)) {
            error_setg(errp, "XEMU_TRACE_REGIONS: '%s' is not "
                       "[v]<address>:<size>", *it);
            goto fail;
        }
        at = g_strndup(p, colon - p);
        g_strstrip(at);
        /* A number is a fixed address; anything else an expression. */
        if (!qemu_strtou64(at, &end, 0, &addr) && !*end) {
            if (!add_fixed(a, addr, size, virt, errp)) {
                goto fail;
            }
        } else if (!add_expr(a, at, size, errp)) {
            error_prepend(errp, "XEMU_TRACE_REGIONS: '%s': ", *it);
            goto fail;
        }
    }
    return a;

fail:
    regions_free(a);
    return NULL;
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

    for (guint i = 0; trace_regions && i < trace_regions->len; i++) {
        TraceRegion *r = &g_array_index(trace_regions, TraceRegion, i);

        if (r->expr) {
            g_string_append_printf(s, " v%s:%" PRIu32, r->text, r->size);
        } else {
            g_string_append_printf(s, " %s0x%" PRIx64 ":%" PRIu32,
                                   r->virt ? "v" : "", r->address, r->size);
        }
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
static bool read_fixed(uint8_t *buf, const uint8_t *ram, uint64_t ram_size,
                       uint64_t address, uint64_t size, bool virt)
{
    if (virt) {
        return read_virtual(buf, ram, ram_size, address, size);
    }
    if (address >= ram_size || size > ram_size - address) {
        return false;
    }
    memcpy(buf, ram + address, size);
    return true;
}

bool xemu_trace_region_read(uint8_t *buf, const uint8_t *ram,
                            uint64_t ram_size, const XemuTraceRegion *r)
{
    return read_fixed(buf, ram, ram_size, r->address, r->size, r->q_virtual);
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
    for (guint i = 0; trace_regions && i < trace_regions->len; i++) {
        TraceRegion *r = &g_array_index(trace_regions, TraceRegion, i);
        XemuHookExprRead got;

        g_string_append_c(s, ' ');
        if (r->expr) {
            got = xemu_hook_expr_read_cpu0(r->expr, buf, r->size);
        } else {
            got = read_fixed(buf, ram, ram_size, r->address, r->size, r->virt)
                  ? XEMU_HOOK_EXPR_READ_OK : XEMU_HOOK_EXPR_UNBACKED;
        }
        if (got == XEMU_HOOK_EXPR_READ_OK) {
            qemu_hexdump_line(s, buf, r->size, 0, 0);
        } else {
            g_string_append_c(s, got == XEMU_HOOK_EXPR_UNRESOLVED ? '?' : '-');
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
        regions_free(trace_regions);
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

void qmp_xemu_trace_regions(XemuTraceRecordRegionList *regions,
                            Error **errp)
{
    GArray *a;

    if (!trace_file) {
        error_setg(errp, "XEMU_TRACE_RECORD is not set");
        return;
    }
    a = regions_new();
    for (; regions; regions = regions->next) {
        XemuTraceRecordRegion *r = regions->value;
        bool ok;

        if (r->has_address == !!r->addr) {
            error_setg(errp, "a trace region takes one of address and addr");
            ok = false;
        } else if (r->addr && r->has_q_virtual && !r->q_virtual) {
            error_setg(errp, "an address expression is always virtual");
            ok = false;
        } else if (r->addr) {
            ok = add_expr(a, r->addr, r->size, errp);
        } else {
            ok = add_fixed(a, r->address, r->size, r->q_virtual, errp);
        }
        if (!ok) {
            regions_free(a);
            return;
        }
    }
    /* An empty list is a change too. */
    regions_free(trace_pending);
    trace_pending = a;
    trace_pending_set = true;
}
