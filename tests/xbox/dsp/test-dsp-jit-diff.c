/*
 * DSP56300 interpreter/JIT differential tests.
 *
 * A corpus of synthetic, peripheral/DMA-free DSP56300 programs
 * (hand-assembled below, encodings verified against the interpreter's
 * opcode tables in hw/xbox/mcpx/apu/dsp/interp/dsp_cpu.c and
 * dsp_emu.c.inc) covering the instruction classes the fork's
 * archaeology names as JIT divergence sources:
 *
 *   - parallel-move classes pm_1 / pm_2(_2) / pm_3 / pm_4x /
 *     pm_5 (immediate, short-absolute and mode-6 forms) / pm_8.
 *     pm_0 (parmove select nibble 0) is deliberately NOT covered: no
 *     24-bit instruction word can select it (select 0 implies
 *     inst < 0x100000, which dispatches to the non-parallel table),
 *     and the only route to it — a raw pram word with the high byte
 *     set, as left by DMA — trips the interpreter's own
 *     `assert((r & 0xFF000000) == 0)` in read_memory_p
 *     (interp/dsp_cpu.c), so an interp-vs-JIT comparison over it
 *     cannot run. Verified empirically during test bring-up.
 *   - REP (immediate + register forms) and DO hardware loops
 *   - calc_ea mode-6 two-word forms (pm_5 load/store, jmp/jcc long,
 *     movep immediate, long-immediate ALU)
 *   - ALU ops with CCR effects, observed through conditional branches
 *     (taken and not-taken) and through final SR state
 *   - block-boundary / PC-exit behavior (short + long jumps, DO
 *     loop-back re-entry, REP repeat-in-place, halt mid-block)
 *
 * Three consumers, wired in meson.build:
 *   1. /corpus under XEMU_DSP_JIT=0 — golden-value regression of the
 *      interpreter (all platforms).
 *   2. /corpus under XEMU_DSP_JIT=1 — the same goldens through the
 *      inline ARM64 JIT, plus an assertion that the JIT actually
 *      engaged (dsp56k_jit_blocks_executed > 0). aarch64 POSIX only.
 *   3. /corpus under XEMU_DSP_JIT=1 XEMU_DSP_JIT_DIFF=1
 *      XEMU_DSP_JIT_DIFF_SYNC=1 — the in-engine per-block bit-exact
 *      validator replays every translated block through the
 *      interpreter and aborts on any state divergence.
 *   4. /jit-differential — re-executes this binary per program with
 *      XEMU_DSP_JIT=0 and =1 (engine selection is parsed once per
 *      process, so the arms must be separate processes) and
 *      byte-compares the final DspCoreState (registers, stacks, all
 *      memories, loop/interrupt/runtime fields) across the arms.
 *
 * The programs terminate by writing 1 to peripheral 0xFFFFC4 (the
 * halt request — the same idiom as tests/xbox/dsp/data/basic) and
 * every runner asserts the halt was reached, so a program that
 * wanders off into uninitialized P-memory fails loudly instead of
 * burning its cycle budget silently.
 *
 * Copyright (c) 2026 xemu-macos contributors
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
#include <glib/gstdio.h>
#include "hw/xbox/mcpx/apu/dsp/dsp.h"
#include "hw/xbox/mcpx/apu/dsp/interp/dsp_cpu.h"
#include "hw/xbox/mcpx/apu/dsp/interp/dsp56k_jit_arm64.h"

/*
 * Same reason as test-dsp.c: libdsp.a's engine-mediation code
 * (dsp.c) references g_config, normally defined by ui/xemu-settings.cc
 * which this standalone binary does not link. Zero-init selects the
 * C interpreter engine; XEMU_DSP_JIT toggles the inline JIT within it.
 */
#include "ui/xemu-settings.h"
struct config g_config;

static void scratch_rw(void *opaque, uint8_t *ptr, uint32_t addr, size_t len,
                       bool dir)
{
    g_assert_not_reached(); /* corpus programs are DMA-free by design */
}

static void fifo_rw(void *opaque, uint8_t *ptr, unsigned int index, size_t len,
                    bool dir)
{
    g_assert_not_reached();
}

/* ------------------------------------------------------------------ */
/* Mini program builder: hand-verified instruction WORDS, computed     */
/* ADDRESSES (labels/patches), so layout arithmetic can't go stale.    */
/* ------------------------------------------------------------------ */

typedef struct {
    DSPState *s;
    uint32_t pc;
} Prog;

static void emit(Prog *p, uint32_t word)
{
    dsp_write_memory(p->s, 'P', p->pc++, word);
}

static uint32_t here(Prog *p)
{
    return p->pc;
}

/* Reserve one word for a forward-referencing instruction. */
static uint32_t reserve(Prog *p)
{
    return p->pc++;
}

static void patch(Prog *p, uint32_t addr, uint32_t word)
{
    dsp_write_memory(p->s, 'P', addr, word);
}

/*
 * Standard tail: request halt via peripheral 0xFFFFC4 (write_peripheral
 * in dsp.c sets halt_requested), then park in a self-loop.
 *   movep #$000001,x:$ffffc4   08F484 000001   (mode-6 immediate form)
 *   jmp <self>                 0C0000|self
 * Returns the address of the movep (jump target for "go finish"). */
static uint32_t emit_halt_tail(Prog *p)
{
    uint32_t at = here(p);
    emit(p, 0x08F484);
    emit(p, 0x000001);
    emit(p, 0x0C0000 | here(p));
    return at;
}

/* Program scaffold: reset vector jmp to 0x40. */
static void begin_prog(Prog *p, DSPState *s)
{
    p->s = s;
    p->pc = 0;
    emit(p, 0x0C0040);          /* jmp $40 */
    p->pc = 0x40;
}

/* jcc #addr (12-bit): 0x0E0000 | cc<<12 | addr. cc codes per emu_calc_cc. */
#define CC_NE 0x2
#define CC_EQ 0xA
#define CC_MI 0xB
#define CC_CS 0x8
static uint32_t jcc_imm(uint32_t cc, uint32_t addr)
{
    g_assert(addr < 0x1000);
    return 0x0E0000 | (cc << 12) | addr;
}

/* pm_3 immediate-short to register: 001ddddd iiiiiiii 00000000. */
static uint32_t move_imm8_reg(uint32_t reg5, uint32_t imm8)
{
    g_assert(reg5 >= 0x10 && reg5 <= 0x1F); /* R/N regs only in this corpus */
    g_assert(imm8 <= 0xFF);
    return (0x1u << 21) | (reg5 << 16) | (imm8 << 8);
}

/* ------------------------------------------------------------------ */
/* Shared runner                                                       */
/* ------------------------------------------------------------------ */

static DSPState *make_dsp(void)
{
    return dsp_init(NULL, scratch_rw, fifo_rw, false);
}

static void run_to_halt(DSPState *s)
{
    for (int i = 0; i < 64 && !dsp_get_halt_requested(s); i++) {
        dsp_run(s, 10000);
    }
    /* A program that never reaches the halt tail is a test bug (or a
     * real control-flow divergence) — fail, don't loop forever. */
    g_assert_true(dsp_get_halt_requested(s));
    dsp_sync_to_vm(s);
}

#define REG(s, r) ((s)->core.registers[(r)])
#define XMEM(s, a) ((s)->core.xram[(a)])
#define YMEM(s, a) ((s)->core.yram[(a)])

/* Common negative assert: no poison path was taken. */
static void check_no_poison(DSPState *s)
{
    g_assert_cmphex(REG(s, DSP_REG_N7), ==, 0);
}

/* ------------------------------------------------------------------ */
/* Program 1: parallel-move classes pm_1/2/3/4x/5/8 + mpy/mac + CCR    */
/* ------------------------------------------------------------------ */

static void build_parmove(DSPState *s)
{
    Prog p;
    begin_prog(&p, s);

    emit(&p, 0x44F400); emit(&p, 0x000111); /* move #$000111,x0   (pm_5 imm) */
    emit(&p, 0x45F400); emit(&p, 0x000222); /* move #$000222,x1 */
    emit(&p, 0x46F400); emit(&p, 0x000333); /* move #$000333,y0 */
    emit(&p, 0x47F400); emit(&p, 0x000444); /* move #$000444,y1 */
    emit(&p, 0x56F400); emit(&p, 0x123456); /* move #$123456,a */
    emit(&p, 0x57F400); emit(&p, 0x654321); /* move #$654321,b */

    emit(&p, 0x452000);                     /* move x1,x:$20  (pm_5 short-abs) */
    emit(&p, 0x4E2000);                     /* move y0,y:$20  */
    emit(&p, 0x4F1000);                     /* move y1,y:$10  */

    emit(&p, move_imm8_reg(DSP_REG_R0, 0x30)); /* move #$30,r0  (pm_3) */
    emit(&p, move_imm8_reg(DSP_REG_R1, 0x18)); /* move #$18,r1 */
    emit(&p, move_imm8_reg(DSP_REG_R4, 0x10)); /* move #$10,r4 */
    emit(&p, move_imm8_reg(DSP_REG_R5, 0x19)); /* move #$19,r5 */

    emit(&p, 0x200040);                     /* add x0,a       -> A1=$123567 */
    emit(&p, 0x21C500);                     /* move a,x1      (pm_2_2)      */
    emit(&p, 0x453000);                     /* move x1,x:$30  */
    emit(&p, 0x453100);                     /* move x1,x:$31  */

    emit(&p, 0x109800);        /* move x:(r0)+,x0  a,y0        (pm_1)      */
    emit(&p, 0xF09800);        /* move x:(r0)+,x0  y:(r4)+,y0  (pm_8 load) */
    emit(&p, 0xBB3900);        /* move a,x:(r1)+   b,y:(r5)+   (pm_8 store)*/
    emit(&p, 0x4AA000);        /* move l:$20,ab                (pm_4x load)*/
    emit(&p, 0x4A2100);        /* move ab,l:$21                (pm_4x store)*/

    /* Restore a heavier A and exercise the multiply pipeline. x1 was
     * overwritten by the pm_4x load path? No: pm_4x AB writes A and B
     * only; x0/x1 still hold their pm_1/pm_8 loads ($123567). */
    emit(&p, 0x200080);        /* mpy +x0,x0,a  -> A=2*x0^2            */
    emit(&p, 0x2000A2);        /* mac +x1,x0,a  -> A=2*x0^2 + 2*x1*x0  */
    emit(&p, 0x567000); emit(&p, 0x000100); /* move a,x:$100 (pm_5 mode-6) */

    /* CCR observation through branches. A is large positive but
     * smaller than x0<<24, so cmp leaves Z=0 (and N=1). */
    emit(&p, 0x200045);                     /* cmp x0,a */
    uint32_t j1 = reserve(&p);              /* jne ok1 (must be taken) */
    uint32_t poison1 = here(&p);
    emit(&p, move_imm8_reg(DSP_REG_N7, 0x66)); /* poison marker */
    uint32_t ok1 = here(&p);
    patch(&p, j1, jcc_imm(CC_NE, ok1));
    emit(&p, move_imm8_reg(DSP_REG_R6, 0x21)); /* r6=$21: reached ok1 */
    (void)poison1;

    emit(&p, 0x200013);                     /* clr a  -> Z=1 */
    uint32_t j2 = reserve(&p);              /* jeq ok2 (must be taken) */
    emit(&p, move_imm8_reg(DSP_REG_N7, 0x66));
    uint32_t ok2 = here(&p);
    patch(&p, j2, jcc_imm(CC_EQ, ok2));
    emit(&p, move_imm8_reg(DSP_REG_R7, 0x22)); /* r7=$22: reached ok2 */

    emit_halt_tail(&p);
}

static void check_parmove(DSPState *s)
{
    check_no_poison(s);
    g_assert_cmphex(REG(s, DSP_REG_R6), ==, 0x21);
    g_assert_cmphex(REG(s, DSP_REG_R7), ==, 0x22);

    /* Address-register update trail */
    g_assert_cmphex(REG(s, DSP_REG_R0), ==, 0x32);
    g_assert_cmphex(REG(s, DSP_REG_R1), ==, 0x19);
    g_assert_cmphex(REG(s, DSP_REG_R4), ==, 0x11);
    g_assert_cmphex(REG(s, DSP_REG_R5), ==, 0x1A);

    /* Data registers */
    g_assert_cmphex(REG(s, DSP_REG_X0), ==, 0x123567); /* pm_8 load       */
    g_assert_cmphex(REG(s, DSP_REG_X1), ==, 0x123567); /* pm_2_2          */
    g_assert_cmphex(REG(s, DSP_REG_Y0), ==, 0x000444); /* pm_8 y-load     */
    g_assert_cmphex(REG(s, DSP_REG_Y1), ==, 0x000444);
    /* A cleared at the end; B holds the pm_4x l:$20 load */
    g_assert_cmphex(REG(s, DSP_REG_A2), ==, 0);
    g_assert_cmphex(REG(s, DSP_REG_A1), ==, 0);
    g_assert_cmphex(REG(s, DSP_REG_A0), ==, 0);
    g_assert_cmphex(REG(s, DSP_REG_B2), ==, 0);
    g_assert_cmphex(REG(s, DSP_REG_B1), ==, 0x000333);
    g_assert_cmphex(REG(s, DSP_REG_B0), ==, 0);

    /* Memory trail */
    g_assert_cmphex(XMEM(s, 0x20), ==, 0x000222);  /* short-abs store  */
    g_assert_cmphex(XMEM(s, 0x21), ==, 0x000222);  /* pm_4x store (A24)*/
    g_assert_cmphex(XMEM(s, 0x30), ==, 0x123567);
    g_assert_cmphex(XMEM(s, 0x31), ==, 0x123567);
    g_assert_cmphex(XMEM(s, 0x18), ==, 0x123567);  /* pm_8 store (A24) */
    g_assert_cmphex(YMEM(s, 0x10), ==, 0x000444);
    g_assert_cmphex(YMEM(s, 0x19), ==, 0x654321);  /* pm_8 store (B24) */
    g_assert_cmphex(YMEM(s, 0x20), ==, 0x000333);
    g_assert_cmphex(YMEM(s, 0x21), ==, 0x000333);  /* pm_4x store (B24)*/

    /* mpy/mac pipeline: A24 of 4*x0^2 (x0=x1=$123567), stored before
     * the branch section clobbered A. Precomputed: 4*0x123567^2 =
     * 0x052E367F3DC4 -> A1 = 0x052E36. */
    g_assert_cmphex(XMEM(s, 0x100), ==, 0x052E36);
}

/* ------------------------------------------------------------------ */
/* Program 2: REP (imm + reg) and DO loops, branch-observed CCR        */
/* ------------------------------------------------------------------ */

static void build_rep_do(DSPState *s)
{
    Prog p;
    begin_prog(&p, s);

    emit(&p, move_imm8_reg(DSP_REG_R0, 0x50)); /* r0=$50 */
    emit(&p, 0x44F400); emit(&p, 0x000001);    /* move #1,x0 */
    emit(&p, 0x200013);                        /* clr a */

    emit(&p, 0x0604A0);                        /* rep #4                */
    emit(&p, 0x200040);                        /* add x0,a   (x4) A1=4  */
    emit(&p, 0x567000); emit(&p, 0x000100);    /* move a,x:$100         */

    emit(&p, 0x062080);                        /* do #32,<LA>           */
    uint32_t la_word = reserve(&p);            /* second word = LA      */
    emit(&p, 0x200040);                        /*   add x0,a            */
    uint32_t la = here(&p);
    emit(&p, 0x545800);                        /*   move a1,x:(r0)+  <- LA */
    patch(&p, la_word, la);
    /* 32 iterations: stores 5..36 at x:$50..$6F, r0 -> $70, A1=$24 */

    emit(&p, 0x200045);                        /* cmp x0,a  (36 vs 1)   */
    uint32_t j1 = reserve(&p);                 /* jne ok1 (taken)       */
    emit(&p, move_imm8_reg(DSP_REG_N7, 0x66));
    uint32_t ok1 = here(&p);
    patch(&p, j1, jcc_imm(CC_NE, ok1));
    emit(&p, move_imm8_reg(DSP_REG_R6, 0x21));

    /* C is clear after cmp (no borrow): jcs must NOT be taken. */
    uint32_t j2 = reserve(&p);                 /* jcs poison2 */
    emit(&p, 0x200013);                        /* clr a */
    emit(&p, 0x200044);                        /* sub x0,a -> A=-1<<24, N=1 C=1 */
    uint32_t j3 = reserve(&p);                 /* jmi ok2 (taken) */
    uint32_t poison2 = here(&p);
    emit(&p, move_imm8_reg(DSP_REG_N7, 0x66));
    patch(&p, j2, jcc_imm(CC_CS, poison2));
    uint32_t ok2 = here(&p);
    patch(&p, j3, jcc_imm(CC_MI, ok2));
    emit(&p, move_imm8_reg(DSP_REG_R7, 0x22));

    uint32_t j4 = reserve(&p);                 /* jcs ok3 (taken now: C=1) */
    emit(&p, move_imm8_reg(DSP_REG_N7, 0x66));
    uint32_t ok3 = here(&p);
    patch(&p, j4, jcc_imm(CC_CS, ok3));
    emit(&p, move_imm8_reg(DSP_REG_R3, 0x23));

    emit(&p, 0x200026);                        /* abs a -> +1<<24, Z=0  */
    uint32_t j5 = reserve(&p);                 /* jeq poison (not taken)*/

    emit(&p, 0x44F400); emit(&p, 0x000003);    /* move #3,x0            */
    emit(&p, 0x06C420);                        /* rep x0                */
    emit(&p, 0x545800);                        /* move a1,x:(r0)+  (x3) */
    /* stores 1,1,1 at x:$70..$72; r0 -> $73 */

    uint32_t done = emit_halt_tail(&p);
    uint32_t poison5 = here(&p);
    emit(&p, move_imm8_reg(DSP_REG_N7, 0x66));
    emit(&p, 0x0C0000 | done);                 /* jmp halt */
    patch(&p, j5, jcc_imm(CC_EQ, poison5));
}

static void check_rep_do(DSPState *s)
{
    check_no_poison(s);
    g_assert_cmphex(REG(s, DSP_REG_R6), ==, 0x21);
    g_assert_cmphex(REG(s, DSP_REG_R7), ==, 0x22);
    g_assert_cmphex(REG(s, DSP_REG_R3), ==, 0x23);

    g_assert_cmphex(XMEM(s, 0x100), ==, 4);    /* rep #4 accumulation */
    for (int i = 0; i < 32; i++) {             /* do #32 body trail   */
        g_assert_cmphex(XMEM(s, 0x50 + i), ==, 5 + i);
    }
    for (int i = 0; i < 3; i++) {              /* rep x0 stores       */
        g_assert_cmphex(XMEM(s, 0x70 + i), ==, 1);
    }
    g_assert_cmphex(REG(s, DSP_REG_R0), ==, 0x73);
    g_assert_cmphex(REG(s, DSP_REG_A1), ==, 1);
    g_assert_cmphex(REG(s, DSP_REG_A2), ==, 0);
    g_assert_cmphex(REG(s, DSP_REG_A0), ==, 0);
    /* Loop machinery fully unwound: LC restored, loop flag clear. */
    g_assert_cmphex(REG(s, DSP_REG_LC), ==, 0);
    g_assert_cmphex(REG(s, DSP_REG_SR) & (1 << DSP_SR_LF), ==, 0);
    g_assert_cmpuint(s->core.loop_rep, ==, 0);
}

/* ------------------------------------------------------------------ */
/* Program 3: mode-6 loads/stores, long-immediate ALU, long jmp/jcc    */
/* ------------------------------------------------------------------ */

static void build_mode6_jumps(DSPState *s)
{
    Prog p;
    begin_prog(&p, s);

    emit(&p, 0x56F400); emit(&p, 0x00789A);    /* move #$789A,a          */
    emit(&p, 0x567000); emit(&p, 0x000100);    /* move a,x:$100 (mode 6) */
    emit(&p, 0x57F000); emit(&p, 0x000100);    /* move x:$100,b (mode-6 LOAD) */
    emit(&p, 0x0140C0); emit(&p, 0x000006);    /* add #$6,a (long-imm ALU) */

    emit(&p, 0x0AF080);                        /* jmp <long>             */
    uint32_t jw = reserve(&p);
    /* dead words the jmp must skip (would poison if executed) */
    emit(&p, move_imm8_reg(DSP_REG_N7, 0x66));
    emit(&p, move_imm8_reg(DSP_REG_N7, 0x66));
    uint32_t cont1 = here(&p);
    patch(&p, jw, cont1);

    emit(&p, 0x200003);                        /* tst a  -> N=0, Z=0     */
    emit(&p, 0x0AF0AA);                        /* jeq <long> poison (not taken) */
    uint32_t jw2 = reserve(&p);
    emit(&p, 0x0AF0A2);                        /* jne <long> ok (taken)  */
    uint32_t jw3 = reserve(&p);
    uint32_t poison = here(&p);
    emit(&p, move_imm8_reg(DSP_REG_N7, 0x66));
    patch(&p, jw2, poison);
    uint32_t ok = here(&p);
    patch(&p, jw3, ok);
    emit(&p, move_imm8_reg(DSP_REG_R6, 0x21));

    emit(&p, 0x200018);                        /* add a,b -> B1=$F13A    */
    emit(&p, 0x577000); emit(&p, 0x000101);    /* move b,x:$101 (mode 6) */

    emit_halt_tail(&p);
}

static void check_mode6_jumps(DSPState *s)
{
    check_no_poison(s);
    g_assert_cmphex(REG(s, DSP_REG_R6), ==, 0x21);
    g_assert_cmphex(REG(s, DSP_REG_A1), ==, 0x0078A0);
    g_assert_cmphex(REG(s, DSP_REG_B1), ==, 0x00F13A);
    g_assert_cmphex(XMEM(s, 0x100), ==, 0x00789A);
    g_assert_cmphex(XMEM(s, 0x101), ==, 0x00F13A);
}

/* ------------------------------------------------------------------ */
/* Corpus table + runners                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *name;
    void (*build)(DSPState *s);
    void (*check)(DSPState *s);
} TestProg;

static const TestProg progs[] = {
    { "parmove",     build_parmove,     check_parmove },
    { "rep-do",      build_rep_do,      check_rep_do },
    { "mode6-jumps", build_mode6_jumps, check_mode6_jumps },
};

static const TestProg *find_prog(const char *name)
{
    for (size_t i = 0; i < G_N_ELEMENTS(progs); i++) {
        if (strcmp(progs[i].name, name) == 0) {
            return &progs[i];
        }
    }
    return NULL;
}

static void test_corpus_prog(gconstpointer arg)
{
    const TestProg *tp = arg;
    DSPState *s = make_dsp();

    tp->build(s);
    run_to_halt(s);
    tp->check(s);

#if DSP56K_JIT_SUPPORTED
    /*
     * In the JIT arm, prove the translator actually ran translated
     * blocks — otherwise a translator that bails out of everything
     * would pass the goldens on pure interpreter fallback.
     */
    if (dsp56k_jit_enabled()) {
        dsp_core_t *core = (dsp_core_t *)s->backend;
        g_assert_cmpuint(dsp56k_jit_blocks_executed(core), >, 0);
    }
#endif

    dsp_destroy(s);
}

/* ------------------------------------------------------------------ */
/* Cross-process differential                                          */
/*                                                                     */
/* Engine selection (XEMU_DSP_JIT) is parsed once per process, so the  */
/* interpreter and JIT arms are separate child processes of this same  */
/* binary (--dump-state <prog> <outfile>), and the parent              */
/* byte-compares the resulting DspCoreState blobs.                     */
/* ------------------------------------------------------------------ */

static const char *g_argv0;

static int dump_state_main(const char *prog_name, const char *out_path)
{
    const TestProg *tp = find_prog(prog_name);
    if (tp == NULL) {
        fprintf(stderr, "unknown program '%s'\n", prog_name);
        return 2;
    }

    DSPState *s = make_dsp();
    tp->build(s);

    for (int i = 0; i < 64 && !dsp_get_halt_requested(s); i++) {
        dsp_run(s, 10000);
    }
    if (!dsp_get_halt_requested(s)) {
        fprintf(stderr, "program '%s' did not reach halt\n", prog_name);
        return 3;
    }
    dsp_sync_to_vm(s);

    FILE *f = fopen(out_path, "wb");
    if (f == NULL) {
        perror("fopen");
        return 4;
    }
    /*
     * DspCoreState is the engine-independent VM view (dsp.h): pc,
     * registers, stacks, X/Y/P RAM, mixbuffer, peripheral window,
     * cycle/instruction bookkeeping, loop and interrupt state. The
     * DSPState wrapper is g_new0'd, so struct padding is zero in
     * both arms and a whole-struct byte compare is well-defined.
     */
    size_t n = fwrite(&s->core, 1, sizeof(s->core), f);
    fclose(f);
    dsp_destroy(s);
    return n == sizeof(s->core) ? 0 : 5;
}

#if DSP56K_JIT_SUPPORTED
/* Describe a byte offset within DspCoreState for failure messages. */
static const char *core_state_region(size_t off)
{
#define IN(field) \
    (off >= G_STRUCT_OFFSET(DspCoreState, field) && \
     off < G_STRUCT_OFFSET(DspCoreState, field) + \
           sizeof(((DspCoreState *)0)->field))
    if (IN(pc)) return "pc";
    if (IN(registers)) return "registers";
    if (IN(stack)) return "stack";
    if (IN(xram)) return "xram";
    if (IN(yram)) return "yram";
    if (IN(pram)) return "pram";
    if (IN(mixbuffer)) return "mixbuffer";
    if (IN(periph)) return "periph";
    if (IN(cycle_count)) return "cycle_count";
    if (IN(instr_cycle)) return "instr_cycle";
    if (IN(halt_requested)) return "halt_requested";
    if (IN(loop_rep)) return "loop_rep";
    if (IN(pc_on_rep)) return "pc_on_rep";
    if (IN(cur_inst)) return "cur_inst";
    if (IN(cur_inst_len)) return "cur_inst_len";
#undef IN
    return "interrupt/other";
}

static GBytes *run_arm(const char *prog_name, const char *jit_value)
{
    gchar *out_path = NULL;
    GError *err = NULL;
    int fd = g_file_open_tmp("dsp-diff-XXXXXX.bin", &out_path, &err);
    g_assert_no_error(err);
    g_close(fd, NULL);

    gchar **envp = g_get_environ();
    envp = g_environ_setenv(envp, "XEMU_DSP_JIT", jit_value, TRUE);
    /* Hermetic children: no diff/stats/sentinel knobs leaking in. */
    envp = g_environ_unsetenv(envp, "XEMU_DSP_JIT_DIFF");
    envp = g_environ_unsetenv(envp, "XEMU_DSP_JIT_DIFF_SYNC");
    envp = g_environ_unsetenv(envp, "XEMU_DSP_JIT_DIFF_MAX");
    envp = g_environ_unsetenv(envp, "XEMU_DSP_JIT_STATS");
    envp = g_environ_unsetenv(envp, "XEMU_DSP_JIT_SENTINEL");
    envp = g_environ_unsetenv(envp, "XEMU_DSP_JIT_FORCE");
    envp = g_environ_unsetenv(envp, "XEMU_DSP_JIT_PIN_AUDIT");
    envp = g_environ_unsetenv(envp, "XEMU_DSP_JIT_NO_THROTTLE");
    envp = g_environ_unsetenv(envp, "XEMU_DSP_JIT_DUMP");

    gchar *args[5];
    args[0] = (gchar *)g_argv0;
    args[1] = (gchar *)"--dump-state";
    args[2] = (gchar *)prog_name;
    args[3] = out_path;
    args[4] = NULL;

    gint wstatus = -1;
    gboolean spawned = g_spawn_sync(NULL, args, envp, G_SPAWN_DEFAULT,
                                    NULL, NULL, NULL, NULL, &wstatus, &err);
    g_assert_no_error(err);
    g_assert_true(spawned);
    g_assert_true(g_spawn_check_wait_status(wstatus, &err));
    g_assert_no_error(err);
    g_strfreev(envp);

    gchar *contents = NULL;
    gsize len = 0;
    g_assert_true(g_file_get_contents(out_path, &contents, &len, &err));
    g_assert_no_error(err);
    g_assert_cmpuint(len, ==, sizeof(DspCoreState));
    g_unlink(out_path);
    g_free(out_path);

    return g_bytes_new_take(contents, len);
}

static void test_jit_differential(gconstpointer arg)
{
    const TestProg *tp = arg;

    GBytes *interp = run_arm(tp->name, "0");
    GBytes *jit = run_arm(tp->name, "1");

    gsize ilen, jlen;
    const uint8_t *ib = g_bytes_get_data(interp, &ilen);
    const uint8_t *jb = g_bytes_get_data(jit, &jlen);
    g_assert_cmpuint(ilen, ==, jlen);

    if (memcmp(ib, jb, ilen) != 0) {
        int shown = 0;
        for (gsize i = 0; i < ilen && shown < 16; i++) {
            if (ib[i] != jb[i]) {
                g_test_message(
                    "state mismatch at byte %#zx (%s): interp=%02x jit=%02x",
                    (size_t)i, core_state_region(i), ib[i], jb[i]);
                shown++;
            }
        }
        g_test_fail_printf(
            "interpreter and JIT final DspCoreState differ for '%s'",
            tp->name);
    }

    g_bytes_unref(interp);
    g_bytes_unref(jit);
}
#endif /* DSP56K_JIT_SUPPORTED */

static void test_jit_differential_unsupported(gconstpointer arg)
{
    (void)arg;
    g_test_skip("DSP56K JIT not supported on this host (aarch64 POSIX only)");
}

int main(int argc, char **argv)
{
    g_argv0 = argv[0];

    if (argc == 4 && strcmp(argv[1], "--dump-state") == 0) {
        return dump_state_main(argv[2], argv[3]);
    }

    g_test_init(&argc, &argv, NULL);

    for (size_t i = 0; i < G_N_ELEMENTS(progs); i++) {
        gchar *path = g_strdup_printf("/corpus/%s", progs[i].name);
        g_test_add_data_func(path, &progs[i], test_corpus_prog);
        g_free(path);

        path = g_strdup_printf("/jit-differential/%s", progs[i].name);
#if DSP56K_JIT_SUPPORTED
        g_test_add_data_func(path, &progs[i], test_jit_differential);
#else
        g_test_add_data_func(path, &progs[i],
                             test_jit_differential_unsupported);
#endif
        g_free(path);
    }

    return g_test_run();
}
