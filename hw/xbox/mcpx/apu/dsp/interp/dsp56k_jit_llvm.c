/*
 * MCPX APU DSP (DSP56300 / M56001) JIT — portable LLVM backend
 *
 * Copyright (c) 2026 xemu-macos contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * Design summary
 * --------------
 * The hand-written emitter in dsp56k_jit_arm64.c only exists on
 * AArch64 POSIX hosts (see DSP56K_JIT_SUPPORTED in dsp56k_jit_arm64.h).
 * This translation unit is the portable counterpart: it implements the
 * exact same API (declared in dsp56k_jit_arm64.h) on top of the LLVM-C
 * API, so it builds and runs wherever libLLVM is available — in
 * particular on the Intel/x86_64 hosts the AArch64 emitter leaves on the
 * interpreter.
 *
 * The build defines DSP56K_JIT_LLVM only with -Ddsp_llvm_jit=enabled and
 * LLVM >= 21 found, and never where the AArch64 emitter is compiled, so
 * the two backends never define the same API symbols. Outside that
 * configuration this file is not built.
 *
 * Phase 1 (this file): plumbing only. Each basic block is one LLVM
 * function of signature
 *
 *     unsigned dsp_blk_<pc>_<gen>(dsp_core_t *dsp, int budget)
 *
 * whose body simply calls the interpreter entry point
 *
 *     void dsp56k_execute_instruction(dsp_core_t *dsp)
 *
 * in a loop until either `budget` cycles have been consumed (measured on
 * dsp->num_inst, which the interpreter advances by dsp->instr_cycle per
 * op) or the core goes idle. It returns the cycle delta. Because the
 * block delegates to the interpreter, its output is bit-exact by
 * construction: this phase exists to prove init / translate / cache /
 * execute / finalize work, and to gate the tests/xbox/dsp JIT arms on
 * x86_64. Later phases replace the loop body with a per-op IR
 * translation (Phase 2+) and inline the hot ops (Phase 3+), exactly as
 * the ARM64 emitter's own phase plan does.
 *
 * Consequences of delegating to the interpreter, relied on below:
 *   - the generated code bakes no P-space words, so a P write needs no
 *     block invalidation (the interpreter always reads current memory);
 *     dsp56k_jit_invalidate / _invalidate_all / _revalidate_all are
 *     therefore no-ops here, and the block cache never thrashes;
 *   - interrupts, DO/REP loop state and the exit conditions (idle,
 *     self-modifying-code request) are all handled inside the
 *     interpreter call, so the block needs no per-op epilogue yet.
 */

#include "qemu/osdep.h"
#include "dsp_cpu.h"
#include "dsp56k_jit_arm64.h"

#if DSP56K_JIT_LLVM_SUPPORTED

#include <llvm-c/Core.h>
#include <llvm-c/Error.h>
#include <llvm-c/LLJIT.h>
#include <llvm-c/Orc.h>
#include <llvm-c/Target.h>

/* --------------------------------------------------------------- *
 * Runtime flags (parsed once) — mirrors dsp56k_jit_arm64.c
 * --------------------------------------------------------------- */

static bool g_jit_parsed;
static bool g_jit_enabled;
static bool g_jit_diff;
static bool g_jit_config_enabled;

void dsp56k_jit_set_enabled_from_config(bool enabled)
{
    g_jit_config_enabled = enabled;
}

static void parse_flags_once(void)
{
    if (g_jit_parsed) {
        return;
    }
    g_jit_parsed = true;

    g_jit_enabled = g_jit_config_enabled;

    /* XEMU_DSP_JIT_DIFF: accepted for parity with the ARM64 backend.
     * Phase 1 has no validator (the generated code is the interpreter,
     * so a diff would be trivially exact); dsp56k_jit_diff_counts()
     * reports "no diff mode", and tests/xbox/dsp registers its diff arms
     * for the ARM64 emitter only. */
    const char *e = getenv("XEMU_DSP_JIT_DIFF");
    if (e && e[0]) {
        long n = strtol(e, NULL, 0);
        if (n >= 1 && n <= 1000000) {
            g_jit_diff = true;
        }
    }

    if (g_jit_enabled) {
        fprintf(stderr, "xemu: DSP JIT enabled (LLVM backend)\n");
    }
}

bool dsp56k_jit_enabled(void)
{
    parse_flags_once();
    return g_jit_enabled;
}

bool dsp56k_jit_diff_enabled(void)
{
    parse_flags_once();
    return g_jit_diff;
}

/* --------------------------------------------------------------- *
 * Per-core state and block cache
 * --------------------------------------------------------------- */

/* One translated block: unsigned (dsp_core_t *dsp, int budget). */
typedef unsigned int (*DspJitLlvmBlockFn)(dsp_core_t *dsp, int budget);

typedef struct DspJitLlvmBlock {
    DspJitLlvmBlockFn entry;
    uint32_t generation;
} DspJitLlvmBlock;

typedef struct DspJitLlvmState {
    LLVMOrcLLJITRef jit;
    DspJitLlvmBlock blocks[DSP_PRAM_SIZE];
    uint64_t blocks_executed;
    uint64_t blocks_translated;
    uint64_t fallbacks;
    uint32_t generation;
} DspJitLlvmState;

/* The native target only has to be initialized once per process. */
static bool g_llvm_initialized;

static void jit_llvm_report_error(const char *what, LLVMErrorRef err)
{
    char *msg = LLVMGetErrorMessage(err);
    fprintf(stderr, "xemu: DSP LLVM JIT: %s: %s\n", what, msg);
    LLVMDisposeErrorMessage(msg);
}

/* --------------------------------------------------------------- *
 * Translation: one LLVM function per starting PC
 * --------------------------------------------------------------- */

static bool jit_llvm_compile_block(DspJitLlvmState *s, uint32_t pc)
{
    /*
     * One LLVMContext per module (ThreadSafeContext wraps it and shares
     * ownership with the module). This is the LLVM 22-safe pattern: the
     * C API no longer exposes ThreadSafeContextGetContext, and a fresh
     * context per block sidesteps any cross-module context reuse rules.
     */
    LLVMContextRef ctx = LLVMContextCreate();
    LLVMTypeRef i8ty = LLVMInt8TypeInContext(ctx);
    LLVMTypeRef i32ty = LLVMInt32TypeInContext(ctx);
    LLVMTypeRef voidty = LLVMVoidTypeInContext(ctx);
    LLVMTypeRef ptrty = LLVMPointerType(i8ty, 0);

    char name[64];
    snprintf(name, sizeof(name), "dsp_blk_%u_%u", pc, s->generation);

    LLVMModuleRef mod = LLVMModuleCreateWithNameInContext("dsp_block", ctx);
    LLVMSetDataLayout(mod, LLVMOrcLLJITGetDataLayoutStr(s->jit));
    LLVMSetTarget(mod, LLVMOrcLLJITGetTripleString(s->jit));

    /* extern void dsp56k_execute_instruction(dsp_core_t *dsp); */
    LLVMTypeRef exec_params[1] = { ptrty };
    LLVMTypeRef exec_fnty = LLVMFunctionType(voidty, exec_params, 1, 0);
    LLVMValueRef exec_fn =
        LLVMAddFunction(mod, "dsp56k_execute_instruction", exec_fnty);

    /* unsigned dsp_blk_<pc>_<gen>(dsp_core_t *dsp, int budget); */
    LLVMTypeRef blk_params[2] = { ptrty, i32ty };
    LLVMTypeRef blk_fnty = LLVMFunctionType(i32ty, blk_params, 2, 0);
    LLVMValueRef fn = LLVMAddFunction(mod, name, blk_fnty);

    LLVMBuilderRef b = LLVMCreateBuilderInContext(ctx);
    LLVMBasicBlockRef entry = LLVMAppendBasicBlockInContext(ctx, fn, "entry");
    LLVMBasicBlockRef loop = LLVMAppendBasicBlockInContext(ctx, fn, "loop");
    LLVMBasicBlockRef done = LLVMAppendBasicBlockInContext(ctx, fn, "done");

    LLVMValueRef dspv = LLVMGetParam(fn, 0);
    LLVMValueRef budgetv = LLVMGetParam(fn, 1);

    /*
     * start = dsp->num_inst
     * limit = start + MAX(budget, 1)
     * loop: dsp56k_execute_instruction(dsp);
     *       until (int32_t)(dsp->num_inst - limit) >= 0 || dsp->is_idle;
     * return dsp->num_inst - start;
     *
     * num_inst wraps: an unsigned compare misses a limit that an op's
     * cycles step over at the wrap, and the block runs 2^32 more cycles.
     */
    LLVMPositionBuilderAtEnd(b, entry);
    LLVMValueRef off_ni = LLVMConstInt(i32ty, offsetof(dsp_core_t, num_inst), 0);
    LLVMValueRef ni_ptr = LLVMBuildGEP2(b, i8ty, dspv, &off_ni, 1, "num_inst.p");
    LLVMValueRef start = LLVMBuildLoad2(b, i32ty, ni_ptr, "start");

    LLVMValueRef one = LLVMConstInt(i32ty, 1, 0);
    LLVMValueRef budget_ok =
        LLVMBuildICmp(b, LLVMIntSGT, budgetv, one, "budget_ok");
    LLVMValueRef budget =
        LLVMBuildSelect(b, budget_ok, budgetv, one, "budget");
    LLVMValueRef limit = LLVMBuildAdd(b, start, budget, "limit");
    LLVMBuildBr(b, loop);

    LLVMPositionBuilderAtEnd(b, loop);
    LLVMBuildCall2(b, exec_fnty, exec_fn, &dspv, 1, "");
    LLVMValueRef ni = LLVMBuildLoad2(b, i32ty, ni_ptr, "num_inst");
    LLVMValueRef off_idle = LLVMConstInt(i32ty, offsetof(dsp_core_t, is_idle), 0);
    LLVMValueRef idle_ptr =
        LLVMBuildGEP2(b, i8ty, dspv, &off_idle, 1, "idle.p");
    LLVMValueRef idle = LLVMBuildLoad2(b, i8ty, idle_ptr, "idle");
    LLVMValueRef past = LLVMBuildSub(b, ni, limit, "past");
    LLVMValueRef hit_limit = LLVMBuildICmp(b, LLVMIntSGE, past,
                                           LLVMConstInt(i32ty, 0, 0),
                                           "hit_limit");
    LLVMValueRef is_idle =
        LLVMBuildICmp(b, LLVMIntNE, idle, LLVMConstInt(i8ty, 0, 0), "is_idle");
    LLVMValueRef stop = LLVMBuildOr(b, hit_limit, is_idle, "stop");
    LLVMBuildCondBr(b, stop, done, loop);

    LLVMPositionBuilderAtEnd(b, done);
    LLVMValueRef cycles = LLVMBuildSub(b, ni, start, "cycles");
    LLVMBuildRet(b, cycles);

    LLVMDisposeBuilder(b);

    /* Hand the module to the JIT and resolve this block's entry point.
     * LLVMOrcCreateNewThreadSafeContextFromLLVMContext takes ownership of
     * ctx; LLVMOrcCreateNewThreadSafeModule takes ownership of mod and
     * shares the context; our ThreadSafeContextRef is dropped right after
     * (the module keeps the underlying data alive). */
    LLVMOrcThreadSafeContextRef tsc =
        LLVMOrcCreateNewThreadSafeContextFromLLVMContext(ctx);
    LLVMOrcThreadSafeModuleRef tsm =
        LLVMOrcCreateNewThreadSafeModule(mod, tsc);
    LLVMOrcDisposeThreadSafeContext(tsc);

    LLVMErrorRef err = LLVMOrcLLJITAddLLVMIRModule(
        s->jit, LLVMOrcLLJITGetMainJITDylib(s->jit), tsm);
    if (err) {
        jit_llvm_report_error("failed to add block module", err);
        return false;
    }

    LLVMOrcExecutorAddress addr = 0;
    err = LLVMOrcLLJITLookup(s->jit, &addr, name);
    if (err) {
        jit_llvm_report_error("failed to resolve block entry", err);
        return false;
    }

    s->blocks[pc].entry = (DspJitLlvmBlockFn)(uintptr_t)addr;
    s->blocks[pc].generation = s->generation;
    s->blocks_translated++;
    return true;
}

/* --------------------------------------------------------------- *
 * Public API (dsp56k_jit_arm64.h)
 * --------------------------------------------------------------- */

void dsp56k_jit_init(dsp_core_t *dsp)
{
    parse_flags_once();
    if (!g_jit_enabled) {
        return;
    }

    if (!g_llvm_initialized) {
        LLVMInitializeNativeTarget();
        LLVMInitializeNativeAsmPrinter();
        g_llvm_initialized = true;
    }

    DspJitLlvmState *s = g_malloc0(sizeof(*s));

    LLVMErrorRef err = LLVMOrcCreateLLJIT(&s->jit, NULL);
    if (err) {
        jit_llvm_report_error("failed to create LLJIT", err);
        g_free(s);
        return;
    }

    LLVMOrcJITDylibRef jd = LLVMOrcLLJITGetMainJITDylib(s->jit);

    /*
     * Resolve the interpreter entry point by address rather than by name
     * lookup: on macOS/Linux the executable's own symbols are not
     * guaranteed to be visible to the JIT's process-symbol search (they
     * may be absent from the dynamic symbol table without -rdynamic /
     * -export_dynamic). LLVMOrcLLJITMangleAndIntern applies the target's
     * global prefix ('_' on MachO, none on ELF/COFF) so the definition
     * matches the mangled name the module's declaration lowers to.
     */
    LLVMOrcCSymbolMapPair pair;
    pair.Name = LLVMOrcLLJITMangleAndIntern(s->jit,
                                            "dsp56k_execute_instruction");
    pair.Sym.Address = (LLVMOrcExecutorAddress)(uintptr_t)
        &dsp56k_execute_instruction;
    pair.Sym.Flags.GenericFlags = LLVMJITSymbolGenericFlagsExported |
                                  LLVMJITSymbolGenericFlagsCallable;
    pair.Sym.Flags.TargetFlags = 0;
    err = LLVMOrcJITDylibDefine(jd, LLVMOrcAbsoluteSymbols(&pair, 1));
    if (err) {
        jit_llvm_report_error("failed to define interpreter symbol", err);
        LLVMOrcDisposeLLJIT(s->jit);
        g_free(s);
        return;
    }

    /* Fall back to the process's own symbols for anything else the
     * generated code might reference (e.g. libc). */
    LLVMOrcDefinitionGeneratorRef gen = NULL;
    err = LLVMOrcCreateDynamicLibrarySearchGeneratorForProcess(
        &gen, LLVMOrcLLJITGetGlobalPrefix(s->jit), NULL, NULL);
    if (!err) {
        LLVMOrcJITDylibAddGenerator(jd, gen);
    } else {
        LLVMConsumeError(err);
    }

    s->generation = 1;
    dsp->jit_state = s;
}

void dsp56k_jit_finalize(dsp_core_t *dsp)
{
    DspJitLlvmState *s = (DspJitLlvmState *)dsp->jit_state;
    if (!s) {
        return;
    }
    LLVMOrcDisposeLLJIT(s->jit);
    g_free(s);
    dsp->jit_state = NULL;
}

unsigned int dsp56k_jit_execute_block(dsp_core_t *dsp, int budget)
{
    DspJitLlvmState *s = (DspJitLlvmState *)dsp->jit_state;
    if (!s || dsp->pc >= DSP_PRAM_SIZE) {
        return 0;
    }
    dsp->jit_chain_limit = dsp->num_inst + (uint32_t)MAX(budget, 1);

    DspJitLlvmBlock *b = &s->blocks[dsp->pc];
    if (b->entry == NULL) {
        if (!jit_llvm_compile_block(s, dsp->pc)) {
            s->fallbacks++;
            return 0;
        }
    }

    uint32_t num_inst_before = dsp->num_inst;
    b->entry(dsp, budget);
    s->blocks_executed++;
    return dsp->num_inst - num_inst_before;
}

/*
 * The Phase-1 block delegates every op to the interpreter, which reads
 * P-space at run time, so a translated block never goes stale on a P
 * write and none of the invalidation hooks has anything to do. They stay
 * as explicit no-ops so the callers' contract (dsp_cpu.c's P-write path,
 * dsp_c.c's reset/bootstrap paths) is satisfied unchanged.
 */
void dsp56k_jit_invalidate(dsp_core_t *dsp, uint32_t addr)
{
    (void)dsp;
    (void)addr;
}

void dsp56k_jit_invalidate_all(dsp_core_t *dsp)
{
    (void)dsp;
}

void dsp56k_jit_revalidate_all(dsp_core_t *dsp)
{
    (void)dsp;
}

void dsp56k_jit_reset_throttle(dsp_core_t *dsp)
{
    (void)dsp;
}

uint64_t dsp56k_jit_blocks_executed(const dsp_core_t *dsp)
{
    const DspJitLlvmState *s = (const DspJitLlvmState *)dsp->jit_state;
    return s ? s->blocks_executed : 0;
}

bool dsp56k_jit_diff_counts(dsp_core_t *dsp, Dsp56kJitDiffCounts *out)
{
    /* Phase 1: no differential validator in this backend. The harness
     * treats "false" as "diff mode not active" and skips its count
     * assertions. */
    (void)dsp;
    (void)out;
    return false;
}

#endif  /* DSP56K_JIT_LLVM_SUPPORTED */
