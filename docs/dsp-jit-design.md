# MCPX APU DSP JIT — design and phased roadmap

This document describes the JIT (dynamic binary translator) for the
Motorola DSP56300 / M56001 interpreter that runs the Xbox MCPX APU
GP + EP effects cores. It supersedes the "MCPX APU DSP dynarec"
entry in the README "Future vectors" section.

## 1. Current interpreter

| Piece | File | Size | Notes |
|---|---|---|---|
| Dispatcher / PC update / interrupt pump | [hw/xbox/mcpx/apu/dsp/dsp_cpu.c](../hw/xbox/mcpx/apu/dsp/dsp_cpu.c) | 1397 | `dsp56k_execute_instruction`, `dsp_postexecute_update_pc`, `dsp_postexecute_interrupts`, `lookup_opcode` |
| Per-instruction emu handlers | [hw/xbox/mcpx/apu/dsp/dsp_emu.c.inc](../hw/xbox/mcpx/apu/dsp/dsp_emu.c.inc) | 8123 | ~383 `emu_*` handlers; `opcodes_alu[256]`, `opcodes_parmove[16]` dispatch tables |
| Disassembler | [hw/xbox/mcpx/apu/dsp/dsp_dis.c.inc](../hw/xbox/mcpx/apu/dsp/dsp_dis.c.inc) | 2226 | Reused for JIT-side decode debug dumps |
| CPU state | [hw/xbox/mcpx/apu/dsp/dsp_cpu.h](../hw/xbox/mcpx/apu/dsp/dsp_cpu.h) | 148 | `dsp_core_t`: `pc`, `registers[0x40]`, `xram[4096]`, `yram[2048]`, `pram[4096]`, `pram_opcache[4096]` (caches resolved `emu_func_t`), interrupt table |
| Register / memory-space macros | [hw/xbox/mcpx/apu/dsp/dsp_cpu_regs.h](../hw/xbox/mcpx/apu/dsp/dsp_cpu_regs.h) | 138 | SR flag bits, `DSP_REG_*` indices, X/Y/P space constants |
| Driver (GP + EP frames) | [hw/xbox/mcpx/apu/dsp/gp_ep.c](../hw/xbox/mcpx/apu/dsp/gp_ep.c) | 535 | One core each (GP + EP); driven per APU frame from the VP frame thread via `dsp_run(dsp, cycles)` |

### 1.1 Per-instruction bookkeeping the JIT must preserve

`dsp_postexecute_update_pc` and `dsp_postexecute_interrupts` run after
every instruction:

- `pc += cur_inst_len` (the handler sets `cur_inst_len` to 0 when
  REP keeps PC on the current instruction, or 2 for two-word ops).
- DO-loop end test: if `pc == LA + 1`, decrement `LC`, either pop the
  loop stack or jump back to `SSH`.
- REP-loop decrement: decrement `LC` until zero, then restore `LCSAVE`.
- Interrupt pipeline state machine (`interrupt_pipeline_count` counts
  down 5 → 4 → 3 → 2 → 1 with instruction prefetch).
- `num_inst += instr_cycle` cycle accounting (caller loop in
  `dsp_run` consumes these).

## 2. JIT architecture

```
dsp56k_execute_instruction (fast dispatcher)
  ├─ XEMU_DSP_JIT=0  → existing interpreter (fallback)
  └─ XEMU_DSP_JIT=1  → enter translated block
                      (block chain directly to next block,
                       or fall back to interpreter for one op
                       on any untranslatable instruction / IRQ / REP edge)
```

### 2.1 Translation unit = basic block

End-of-block triggers:
- Any branch / call / return (JMP/JSR/JCC/BCC/BSR/RTS/RTI/...).
- `REP` or `DO` instruction (loops are runtime-variable; fall back).
- Illegal / unimplemented opcode.
- `pram` write that touches this block's PC range (invalidation).
- A pending interrupt becomes visible.

Typical Xbox DSP block is 8-40 instructions before hitting a branch.

### 2.2 Translation backend: hand-rolled ARM64 emitter

TCG was considered and rejected. TCG is tightly coupled to `CPUState`
/ `cpu_exec` / BQL and assumes full vCPU integration. The DSP is a
subordinate processor running on the APU VP frame thread — a
dedicated thin emitter is a better fit.

Emitter primitives needed in Phase 0/1:

- `MOV imm64` (`MOVZ` + `MOVK` chain)
- `LDR` / `STR` immediate offset (word and doubleword)
- `LDR` / `STR` register offset
- `ADD` / `SUB` immediate (for PC and cycle counters)
- `BL` / `BR` / `BLR` / `RET`
- `B` / `B.cond` / `CBZ` / `CBNZ` (for block chaining and conditional exits)
- `TBZ` / `TBNZ` (for flag bit tests when we later inline them)
- `ISB` / `__builtin___clear_cache` on I-cache flush after emission.

### 2.3 Register strategy (static map, pinned for block lifetime)

| DSP reg | ARM64 | Rationale |
|---|---|---|
| `A0` / `A1` / `A2` (56-bit accumulator A: 24+24+8 split) | X19 / X20 / X21 | ~20% of all handlers read/write A |
| `B0` / `B1` / `B2` | X22 / X23 / X24 | Symmetric with A |
| `X0` / `X1` | X25 / X26 | Most-common data-bus operands |
| `Y0` / `Y1` | X27 / X28 | Most-common data-bus operands |
| `dsp_core_t *dsp` | X0 | Context pointer, constant across a block |
| `SR` (full), `R[0..7]`, `N[0..7]`, `M[0..7]` | in-memory (`LDR [X0, #offsetof]`) | Less hot |

Phases 0-7 hold DSP regs in memory (no pinning); Phase 8 introduces
pinning with save/restore at block boundary.

### 2.4 Memory-access lowering

- X / Y / P address < `DSP_{X,Y,P}RAM_SIZE`, linear mode
  (`Mn == 0xFFFFFF`): inline `LDR [X0, #base + addr * 4]` / `STR`.
- Address-register mode, linear: inline
  `Rn = (Rn + Nn) & MASK` with MASK = space size − 1.
- Modulo / reverse-carry addressing (`Mn` non-linear): helper call
  (rare in Xbox audio).
- Peripheral (`addr >= 0xFFFF80`): always helper
  (`dsp->read_peripheral` / `dsp->write_peripheral`).

### 2.5 SR flag updates

Phases 2 through 7: eager per-instruction update into
`dsp->registers[DSP_REG_SR]`. Phase 8: lazy `cc_op` shadow that
records the last flag-producing op + inputs and only materializes
SR on read, mirroring the x86 TCG lazy-flag scheme.

### 2.6 Block cache + invalidation

- Hash table keyed on `(pc, sr & (LF|loop_rep_bit))` — DO/REP state
  materially changes per-instruction semantics, include conservatively;
  refine in Phase 8.
- Size bounded by `DSP_PRAM_SIZE = 4096` (at most one block starts
  per PC). LRU eviction at capacity.
- Invalidation: `dsp56k_write_memory(space = DSP_SPACE_P, address, ...)`
  already clears `pram_opcache[address]`. Extend to also evict any
  translated block whose PC range contains `address`. Per-block
  bitmap over covered PCs is cheap (~4 KB each but dense blocks
  stay well below).

### 2.7 JIT memory (macOS MAP_JIT)

Reuse the existing `qemu_thread_jit_write()` / `qemu_thread_jit_execute()`
wrappers from [include/qemu/osdep.h](../include/qemu/osdep.h) that
QEMU's TCG already uses for Apple Silicon. Allocate a ~4-8 MB code
buffer via `mmap(MAP_JIT | PROT_READ | PROT_WRITE | PROT_EXEC)` on
macOS, bump-allocate per block, reset bump pointer on full-cache flush.

Explicitly not reused:

- Apple's `pthread_jit_write_with_callback_np` (failed-experiment
  entry A3 — scoped-W semantics break nested JIT-write paths).
- A per-thread cache of W^X state (failed-experiment entry
  "TLS-cached `pthread_jit_write_protect_np`" — state drifts from
  kernel on nested codegen, producing SIGSEGV/SIGILL after ~1 min
  under load).

### 2.8 Dispatcher

New `dsp_jit_execute_block(dsp)` called in place of (or in addition
to, when differential mode is active) `dsp56k_execute_instruction`:

1. Look up block for `dsp->pc`; translate on miss.
2. Enter block prologue (save callee-saved regs we clobber).
3. Block body runs as many translated ops as supported; exits on:
   - Control-flow instruction (block terminator).
   - Pending interrupt (`interrupt_counter > 0`).
   - Unsupported opcode (fall back to interpreter for one op, then
     re-enter dispatcher).
4. Return to caller with `save_cycles` correctly decremented.

Existing `dsp56k_execute_instruction` gains an early branch on the
JIT flag. The interpreter path is preserved verbatim as fallback.

### 2.9 Differential test mode (`XEMU_DSP_JIT_DIFF=1`)

For correctness validation during Phases 1-6 (and permanently
available for debug), the validator runs the interpreter on a
**private copy** of `dsp_core_t` and byte-compares against the
JIT's post-block state. On divergence:

- Log first divergent register / xram / yram / pram / etc. byte.
- Disassemble the failing block and dump Rn/Nn/Mn at fault time.
- `abort()` so the crash carries useful diagnostic.

**Per-translation validation gate (crucial)**. A translated block
is fully deterministic given its pre-state — the emitted ARM64
stubs run the same `emu_func_t` handlers every time — so a single
passing validation proves correctness for every future execution of
that translation. The diff path sets `DspJitBlock.diff_checked`
on first enqueue, and the per-block fast-path peek in
`dsp_jit_execute_block` skips the diff path entirely once the bit
is set. It is cleared on `dsp_jit_invalidate` (self-modifying
code) and `translate_block` (fresh translation); the validator
only stamps VALIDATED if the block's entry pointer hasn't changed
since enqueue (guards against the retranslation race). This
bounds total validation work to **~(number of unique block
translations)** across the session — typically a few hundred to a
few thousand for a game — rather than revalidating every block on
every execution (millions per second). Without this gate the
continuous validator load drowned the APU thread's `d->lock` hold
time, starving the main thread's MCPX MMIO and hanging emulator
startup with an indefinite dock-icon bounce.

**Async validator (default)**. The APU thread snapshots pre- and
post-state into a 16-slot SPSC ring and publishes. A dedicated
validator thread (`mcpx.dsp_diff`) pops slots, runs the interpreter
replay + compare off the APU thread, and aborts on divergence. APU
thread cost is **two memcpys per block** (~160 KB); `d->lock` is
never held across the slow interpreter replay. When the ring fills
(validator can't keep up) the producer drops newest entries —
validation becomes a sampler rather than blocking the emulator.

**Sync validator (`XEMU_DSP_JIT_DIFF_SYNC=1`)**. Kept for bring-up
debugging where the abort must fire **immediately** on divergence
(rather than "eventually, when the validator catches up"). Runs the
interpreter replay + compare inline on the APU thread; re-introduces
the ~2-10x slowdown that async mode eliminates. Both modes use the
same validator core and produce identical diagnostics.

**Narrowed compare (write-set optimisation)**. Each translated
block stores a conservative `write_set` bitmask (xram / yram / pram
/ mixbuffer / periph) computed at translation time. The comparator
skips byte-comparing memory regions the block demonstrably didn't
write to. FIR / IIR kernel blocks (pure register computation with
read-only memory access) compare ~2 KB instead of ~40 KB — a 20×
compare-path speedup.

**Peripheral shims**. The interpreter replay runs on a private copy
of `dsp_core_t`. The copy's `read_peripheral` / `write_peripheral`
function pointers are replaced with stubs that mark the slot as
"skip compare" — the real callbacks would `container_of` into a
bogus `DSPState*`. Peripheral I/O is non-replayable anyway (DMA
against Xbox host RAM shared with the x86 CPU thread is
non-deterministic); the live JIT flags such blocks via
`core->jit_skip_diff_compare = 1` set from `write_peripheral` on
`DMA_CONTROL`.

## 3. Phased roadmap

Each phase is one session (Phases 4 and 8 may span two). Every phase
lands behind `XEMU_DSP_JIT=1` gating; interpreter remains the default
until Phase 7 completes.

| Phase | Status | Scope | Target DSP-CPU delta vs interpreter |
|---|---|---|---|
| 0 | **landed** | Infrastructure (allocator, block cache, emitter, dispatcher, invalidation hook, `XEMU_DSP_JIT` flag) | 0 — no translation yet |
| 1 | **landed** | Skeleton: MOVE inline; every other op as helper call to the existing `emu_*`. Diff mode validates infra | 0-5% (helper-call overhead offsets small inline wins, but block chaining cuts per-instruction dispatcher overhead) |
| 2 | **landed (aggressive — MAC included)** | Inline arithmetic + MAC core (~170 of 256 ALU opcodes): ADD/SUB/CMP/CMPM/TST/CLR/NEG/ABS/AND/OR/EOR/NOT + all 128 MPY/MPYR/MAC/MACR variants + TFR family. 56-bit accumulator held packed + sign-extended in a single X-reg; 3-word add-with-carry dance collapses to one 64-bit ADD; dsp_mul56 collapses to SMULL + LSL #1 (+ optional NEG). SR E/U/N/Z computed via single shared BLR to `dsp_jit_helper_ccr_e_u_n_z` shim (inlining deferred to Phase 8's lazy-flag rework). ASL/ASR/LSL/LSR + the rare tail (RND/ROL/ROR/ADDL/SUBL/ADDR/SUBR/ADC/SBC/MAX) stay as BLR fallback. `XEMU_DSP_JIT_STATS=1` prints `alu_inlined / alu_fallback` per run. | 40-60% on MAC-heavy kernels |
| 3 | **landed (partial — via Phase 4)** | Memory + linear addressing (xram/yram direct up to 0xc00; mixbuffer / peripheral via C helper). Inlined via `emit_mem_read_xy` / `emit_mem_write_xy`. | +10-15% |
| 4 | **landed** | Parallel moves (all 16 select values: pm_0 / pm_1 / pm_2 / pm_3 / pm_4 / pm_5 / pm_8). Inline fetch → BLR opcodes_alu → inline writeback. `emu_move` special-cased (no ALU BLR). Non-linear Mn and calc_ea modes 5-7 bail to C helper. pm_4x (long-accu l:ea) goes through a single C helper BLR rather than inline. | +30-40% |
| 5a | **landed (round 2)** | Inline control-flow handlers across the full immediate / `_ea` / `_aa` / `_pp` / `_reg` variant matrix (~30 handlers). Round 1 covered JMP/JSR/RTS/RTI/JCC/JSCC/BRA/BSR/BCC `_imm` + `_long`. Round 2 extends to: `_ea` variants (JMP/JSR/JCC/JSCC) using the existing `emit_calc_ea_inline` fast path (linear Rn modes 0-4 inline, modes 5-7 via the slow-call shim); the full 4×4 bit-test matrix (JCLR/JSET/JSCLR/JSSET × AA/EA/PP/REG); and PC-relative bit-tests (BRCLR/BRSET × PP/REG). Round 2 also fully inlines `emu_calc_cc` (the 16-case cc switch emits a 2-7 insn direct bitfield extract per cc — no BLR). Bit-test memory reads route through the existing `emit_mem_read_xy` helper, keeping linear xram/yram addresses inline while peripheral/out-of-range still BLR'ing `dsp56k_read_memory`. Write-sets for inlined CF ops tightened from `WS_ALL` to 0 — all their effects land in `dsp_state_diff`'s always-compared ranges, so the DIFF validator's per-block compare drops from ~40 KB to ~2 KB matching parmove stubs. `XEMU_DSP_JIT_STATS=1` prints `cf_inlined / cf_fallback` counts. | +6-12% (round 2 ~3-6% on top of round 1 from the calc_cc inline + broader coverage; higher on bit-test-heavy guest code) |
| 5b / 5c | **deferred** | Static block chaining. Design involves per-block chain-site tracking, pending-chain pool, and patch-on-invalidate across a live executing code buffer; the prologue/epilogue coupling (frame saves) and stack-growth-on-chain semantics need an entry-split design (normal vs chained entry) to avoid unbounded stack growth. Will be landed as a dedicated session once the entry-split prologue refactor stabilises. The chain-free 5a path gives the bulk of the CF inlining win without the patching risk | +5-12% (when it lands) |
| 6 | **landed** | Inline REP / DO / DOR / ENDDO (the `_imm` variants — the single- and two-word forms that Xbox audio actually emits). Loop-body iteration arithmetic still runs in `dsp_postexecute_update_pc` (BLR'd from the epilogue), because the post-exec block-exit check on `loop_rep != 0` and `SR.LF` is already what implements the "stay on this instruction" / "end-of-loop pop" edges. `_ea` / `_aa` / `_reg` REP variants stay as BLR fallback (rare in audio programs; always chase a register-sourced LC which we don't bake at translate time) | +3-8% (higher on DO-heavy workloads like filter kernels) |
| **round 3** | **landed** | Three compounding extensions to the inline coverage: (a) calc_ea modes 5 (Rn+Nn) and 7 (-(Rn)) inlined with a linear-Mn fast path (modulo Mn still falls back through the existing slow-call shim); mode 6 (aa) deferred because inlining it would require threading `pc` through every caller of `emit_calc_ea_inline`. (b) ALU shifts ASL/ASR (56-bit, shift-by-1) and LSL/LSR (A1-only, shift-by-1) — the `opcodes_alu[]` shift opcodes 0x22/0x23/0x2a/0x2b/0x32/0x33/0x3a/0x3b, previously always BLR-fallback, now inline. The 56-bit variants packed-load into a 64-bit X-reg and match dsp_asl56 / dsp_asr56's flag behaviour bit-for-bit (including dsp_asr56's LOGICAL-not-arithmetic right shift which the "ASR" mnemonic masks). (c) ALU long-immediate handlers ADD/SUB/CMP/AND/OR_long: the 2-word ALU forms where pram[pc+1] was being read at run time via `read_memory_p`. The immediate is now baked at translate time, the handler body emits inline, and the BLR + read_memory_p + helper prologue/epilogue are all dropped. (EOR_long has no interpreter handler — the opcode-table entry is NULL — so it stays as a trap.) | +4-8% (cumulative on top of Phase 5a round 2 + Phase 6) |
| 7 | **landed** | Interrupt handling at block boundaries (inline fast-path skip when interrupt_state / counter / pipeline_count all zero) | neutral (correctness) |
| 8 | pending | Lazy flag evaluation (cc_op shadow), dead-flag-store elimination, ARM64 register pinning for A/B/X0/X1/Y0/Y1, parmove+ALU fusion | +15-25% |

**Aggregate target after Phase 8**: 2-4× DSP throughput, dropping
DSP from ~10% of total CPU budget to ~3-5% on a DSP-heavy title
(e.g. titles with active EP reverb + GP DSP programs).

## 4. Risks / open questions

- **REP / DO pipeline** — timing in `dsp_postexecute_update_pc` is
  subtle; mis-translation silently corrupts audio filter kernels.
  Differential mode is the safety net throughout Phases 2-6.
- **Interrupt granularity** — checking pending interrupts at block
  boundary instead of per instruction may shift audio-clock ISRs
  by a few cycles. Likely inaudible; validate with an ISR-heavy
  title before merging Phase 7.
- **Accumulator 56-bit semantics** — A/B layout is
  `A2[55:48] sign-extend | A1[47:24] | A0[23:0]`. ARM64 handles
  64-bit, but signed saturation and limit-bit behavior must be
  bit-exact. Phase 2 adds per-op fuzz harness against the interpreter.
- **Self-modifying DSP code mid-block execute**. Rare but possible.
  Invalidate-while-executing path: mark block "dead", complete
  current iteration via interpreter, retranslate on next entry.
- **ARM64-only for the foreseeable future**. x86_64 backend is
  Phase 9+ and only matters if this fork ever re-targets Intel Mac.
- **Open infra question**: reuse TCG's code buffer or a dedicated
  one. Initial plan: separate ~8 MB buffer to avoid cross-
  interference; revisit if wasteful.

## 5. Failed-experiment traps explicitly sidestepped

- **A3 `pthread_jit_write_with_callback_np`** (README "Failed /
  reverted experiments"). We use the manual
  `qemu_thread_jit_write` / `qemu_thread_jit_execute` pair — same
  contract as TCG, no scoped-W issues.
- **TLS-cached `pthread_jit_write_protect_np`** (same list). We do
  NOT cache per-thread W^X state; every write/execute toggle
  through the helpers issues the real syscall.

## 6. Test strategy

- **Differential mode** (always enabled during development): every
  block pair-runs against the interpreter with a register + memory
  comparison.
- **Game regression**: Azurik Rise of Perathia — boot, in-game
  walk, rapid level-load, death-reload. All known DSP-dependent
  audio paths (Dolby, reverb) exercised.
- **Microbenchmark**: a synthetic loop that pegs the DSP
  (`dsp_run(core, 1e6)`) timed with and without `XEMU_DSP_JIT=1`,
  measured against interpreter baseline.
- **Fuzz corpus**: a small collection of opcode-family test programs
  (arithmetic, addressing, parmoves, REP/DO) — translate each with
  `XEMU_DSP_JIT_DIFF=1` and assert 0 divergences.

## 7. Runtime knobs

| Env var | Default | Purpose |
|---|---|---|
| `XEMU_DSP_JIT` | from menu toggle | Enable the JIT. `1` = JIT + interpreter fallback; `0` = pure interpreter. Overrides the `audio.dsp_jit.enabled` menu toggle for CI / bisect |
| `XEMU_DSP_JIT_DIFF` | `0` | Differential validation. `1` = every block; `N ≥ 2` = sample 1 of every N blocks. Async by default (validator runs off the APU thread on a worker named `mcpx.dsp_diff`; APU cost is ~2 memcpy/block) |
| `XEMU_DSP_JIT_DIFF_SYNC` | `0` | Force on-thread (synchronous) validation — runs interpreter replay + compare inline on the APU thread before the next block. Slow; for bring-up only |
| `XEMU_DSP_JIT_DIFF_MAX` | `0` (unlimited) | Optional cap: stop validating after N enqueues. Useful for CI that wants "validate first N, then run full-speed" gating. Day-to-day runs should use `XEMU_DSP_JIT_DIFF=N` sampling instead (the per-translation gate already keeps unbounded DIFF=1 runs cheap in the normal case) |
| `XEMU_DSP_JIT_DUMP` | `0` | Hex-dump each emitted ARM64 block at translate time for offline disassembly |
| `XEMU_DSP_JIT_STATS` | `0` | On emulator exit, print per-core stats: `blocks_translated / executed / cache_flushes / fallbacks / diff_ops_checked / code_buf_used / alu_inlined / alu_fallback / cf_inlined / cf_fallback` and, when the async validator ran, `enqueued / checked / dropped / failures` |
