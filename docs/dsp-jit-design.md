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
| `SR` (low 16 bits) | w28 (`DSP56K_JIT_SR_PIN_REG`, zero-extended, write-through to `registers[SR]`) | CCR read/write on nearly every ALU op |
| `R[0..7]`, `N[0..7]`, `M[0..7]` | in-memory (`LDR [X0, #offsetof]`) | Less hot |

Phases 0-7 held DSP regs in memory (no pinning). Phase 8 landed:
- A/B accumulator pinning: packed 56-bit sign-extended in x26/x27.
- X0/X1/Y0/Y1 pinning: packed 48-bit unsigned in x20/x21 (displacing
  the cached postexecute helper ptrs into inline materialisation at
  their slow-path BLR sites).
- Extended lazy-flag: multi-step (up to 4) CCR-dead lookahead with
  parmove-MOVE passthroughs + RTI/ENDDO full-SR-overwrite writers.
All three use write-through to `registers[]` so BLR fallbacks see a
consistent register file; a `XEMU_DSP_JIT_PIN_AUDIT=1` diagnostic
stays in-tree for future pinning work. `x28`, formerly the last free
callee-saved register, has since been claimed as the SR pin
(`DSP56K_JIT_SR_PIN_REG`, `dsp56k_jit_arm64.c:1452` — SR's low 16
bits, zero-extended, write-through like the other pins; landed after
Phase 8). No free callee-saved GPR remains: the deferred parmove+ALU
fusion work must share the existing pins or spill. See the Phase 8
table rows below for the full landing breakdown.

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
| 5b / 5c | **landed** | Static block chaining. Each block exposes a post-prologue `chain_entry` (skipping the prologue because the chaining source block's prologue has already set up x19=dsp / x20+x21=helpers / saved callee-saved regs). The source block's last-op epilogue, when the op is a chainable terminator (BRA/JMP/BCC/JCC/JSCC imm or long with statically-known target) whose target is already translated, emits a direct `B target->chain_entry` in place of the fall-through to the shared exit. The PC-mismatch check uses `target_pc` as expected, so the taken path passes (and falls into the chain B) while non-taken / unexpected-pc paths exit to the dispatcher via the shared exit (unchanged). Each block tracks up to `DSP_JIT_MAX_INCOMING_CHAINS=8` incoming chain sites; on invalidation, those sites are re-patched to their source blocks' `shared_exit` so the caller exits to the dispatcher instead of jumping into stale code. Source `generation` counter makes stale chain-site pointers safely skippable on cascading invalidations. Chaining is disabled when `XEMU_DSP_JIT_DIFF` is on (the diff harness snapshots per dispatcher call). | +5-12% (hot inner loops stay in JIT across iterations) |
| 6 | **landed** | Inline REP / DO / DOR / ENDDO (the `_imm` variants — the single- and two-word forms that Xbox audio actually emits). Loop-body iteration arithmetic still runs in `dsp_postexecute_update_pc` (BLR'd from the epilogue), because the post-exec block-exit check on `loop_rep != 0` and `SR.LF` is already what implements the "stay on this instruction" / "end-of-loop pop" edges. `_ea` / `_aa` / `_reg` REP variants stay as BLR fallback (rare in audio programs; always chase a register-sourced LC which we don't bake at translate time) | +3-8% (higher on DO-heavy workloads like filter kernels) |
| **round 3** | **landed** | Three compounding extensions to the inline coverage: (a) calc_ea modes 5 (Rn+Nn) and 7 (-(Rn)) inlined with a linear-Mn fast path (modulo Mn still falls back through the existing slow-call shim); mode 6 (aa) deferred because inlining it would require threading `pc` through every caller of `emit_calc_ea_inline`. (b) ALU shifts ASL/ASR (56-bit, shift-by-1) and LSL/LSR (A1-only, shift-by-1) — the `opcodes_alu[]` shift opcodes 0x22/0x23/0x2a/0x2b/0x32/0x33/0x3a/0x3b, previously always BLR-fallback, now inline. The 56-bit variants packed-load into a 64-bit X-reg and match dsp_asl56 / dsp_asr56's flag behaviour bit-for-bit (including dsp_asr56's LOGICAL-not-arithmetic right shift which the "ASR" mnemonic masks). (c) ALU long-immediate handlers ADD/SUB/CMP/AND/OR_long: the 2-word ALU forms where pram[pc+1] was being read at run time via `read_memory_p`. The immediate is now baked at translate time, the handler body emits inline, and the BLR + read_memory_p + helper prologue/epilogue are all dropped. (EOR_long has no interpreter handler — the opcode-table entry is NULL — so it stays as a trap.) | +4-8% (cumulative on top of Phase 5a round 2 + Phase 6) |
| **round 4 (partial)** | **landed** | Epilogue cost-reduction across every translated op. Landed pieces: (a) **Fused exit check** — `loop_rep` (u32) / `is_idle` (u8) / `jit_exit_block_request` (u8) used to emit three separate `LDR+CBNZ` pairs; now OR'd into one register and branched once, saving two insns + two exit-patch slots per op with identical semantics (any non-zero still triggers the exit). (b) **SUB+CBNZ PC check fast path** — when `expected_next_pc` fits in ARM64's imm12 (which it almost always does since DSP PRAM is 4 KiB), the PC-mismatch check drops from `MOV imm32 + CMP + B.NE` (3 insns) to `SUB w0, w0, #imm + CBNZ w0` (2 insns), with an imm32+CMP+B.NE fallback for the edge case. (c) **Dead-code cleanup** — removed the `dsp_jit_helper_calc_cc` shim that Phase 5a round 2 made unreachable (the 16-case cc switch is materialised inline at translate time). Dropped pieces: (d) **EPI_NO_PC** (skip PC-mismatch exit check for parmove stubs + inlined long-imm) was proven unsafe by the runtime-sentinel harness at 9.75% mismatch rate (5.77M / 59.2M watched ops in Azurik): calc_ea mode 6 lengthens parmoves to 2 words and REP/DO loops rewind pc via the postexec helper; both diverge legitimately and need the exit to fire. Dropped permanently. (e) **cur_inst preset skip** for inlinable ops also regressed with `op=0x001000` on startup — kept deferred pending investigation via the `XEMU_DSP_JIT_SENTINEL=1` poison harness (re-apply the skip with `0x00adbeef` as the value; any reader that decodes cur_inst at runtime surfaces via the assertion path). | +1-3% (two landed epilogue pieces; coverage unchanged) |
| **round 5** | **landed** | Three targeted inlines on top of round 4. (a) **calc_ea mode 6 (aa)** — the last remaining slow-call in the EA fast path, now bakes the 24-bit `pram[pc+1]` immediate at translate time along with the runtime `instr_cycle += 2 / cur_inst_len++` / retour flag. Signature threaded through all 10 call sites (8 parmove stubs + 4 CF `_ea` ops + bit-test generic). (b) **ALU tail inlining** — six more opcodes leave BLR fallback for direct ARM64: ROL / ROR (1-bit circular rotates on A1/B1, not through-carry despite the mnemonic) and the shift-arith quartet ADDL / SUBL / ADDR / SUBR (56-bit ASL or ASR of dest, then add or sub the other accumulator; shared `emit_alu_shift_arith` helper reuses the existing `emit_load_accu56` / `emit_addsub_flags` / `emit_store_accu56` / `emit_sr_clear_vc_or_newsr` primitives). Remaining BLR-fallback: RND (scaling-mode-dependent round), ADC / SBC (extended-precision), MAX (known interp quirk). (c) **`emu_ccr_update_e_u_n_z` fast path** — the SR.E/U/N/Z update is called from ~180 ALU handlers (every arithmetic + every MAC + every shift + every ADDL/SUBL/etc.) and was always a BLR. Scaling mode 0 (the universal Xbox audio case) is now inline: ~20 ARM64 insns of UBFX / BFI / CBNZ / CMP. Scaling 1 / 2 keep the BLR to the shim. Bisect-validated clean via `XEMU_DSP_JIT_DIFF=50`. | +3-6% (the ccr inline is the dominant win — one BLR per ALU op eliminated on ~100M ops/run in Azurik) |
| **round 6** | **landed** | **Mode-6 parmove `inst_len` classifier fix**. Extends `dsp_jit_helper_inst_length` to detect parmove instructions whose EA is calc_ea mode 6 (absolute-address → 2-word). Applied to pm_0 (select=0), pm_1 (select=1), pm_5 (select=5/6/7), and pm_4 fall-through to pm_5 (select=4 AND NOT pm_4x). The mode bits are at EA[5:3] of the 6-bit `ea_mode = (inst >> 8) & 0x3f`. pm_5 additionally requires `ea_form = inst[14] == 1` (when clear, pm_5 uses inst[13:8] directly as a short-absolute, no calc_ea). pm_2/pm_3/pm_4x/pm_8 cannot select mode 6 by construction (pm_2 R-update uses 5-bit EA modes 0-3; pm_3 no EA; pm_4x has its own helper; pm_8's ea1/ea2 decoding forces modes 1-4). Parmove translator path updated to use the classifier and advance `pc_end = pc + inst_len` instead of unconditional `pc + 1`. Blocks containing mode-6 parmoves now stay intact (expected_next_pc matches the actual post-exec pc), eliminating the 9.75% PC-mismatch-exit rate the sentinel measured and reducing block fragmentation across mode-6-heavy code (e.g. audio code that does `x:#imm,X0` absolute-loads). | +1-3% (10% of parmove ops no longer block-exit; downstream effect is cumulative on workloads that load absolute constants frequently) |
| **round 7** | **landed** | Three more inline landings and one cleanup: (a) **ADC / SBC inline** — eight opcode variants ({ADC,SBC} × {X,Y} × {A,B}) that previously fell back to BLR the emu handler. Implemented branchlessly: load orig + 56-bit sign-extended X1:X0 / Y1:Y0 src, run the first add/sub + flag calc, then run a second add/sub with the SR.C bit as a zero-extended w-reg operand. When SR.C is 0 the second op is identity and emit_addsub_flags produces zero-filled flags, matching the interp's `if (curcarry) { ... OR newsr }` pattern without any conditional branch. (b) **RND inline** — `emu_rnd_a` / `emu_rnd_b` used to BLR-fall through to the full interpreter wrapper; now the JIT calls `dsp_jit_helper_rnd56` directly with the packed accumulator, saving the wrapper's register-pack / unpack and dropping one BLR hop (since the post-op E/U/N/Z runs inline). (c) **Full-coverage `emu_ccr_update_e_u_n_z` inline** — the previous round's fast path covered scaling=0 only; the two exotic scaling modes (SR.S0 / SR.S1) now have inline branches emitting the correct E/U bit formula for each mode, plus an early-exit for scaling=3 (the interp's "illegal mode — no update") that skips both E/U and N/Z. The `dsp_jit_helper_ccr_e_u_n_z` shim in `dsp_cpu.c` + its declaration in `dsp_jit.h` are deleted as dead code. Only MAX (ALU opcode 0x1d) remains in BLR-fallback — skipped intentionally because the interpreter's `emu_max` has a B2↔B0 swap quirk that would need careful bit-exact replication. | +2-4% (the ccr full inline + RND + ADC/SBC combined; the ccr win dominates on any workload using SR.S0/S1 at all; RND + ADC/SBC are moderate savings on audio-kernel-heavy paths) |
| **round 8** | **landed** | Non-parallel "misc" tail + write-set narrowing. **ANDI / ORI / LUA / LUA_REL inlined** — ANDI / ORI do 8-bit AND/OR against SR.MR (regnum=0), SR.CCR (=1), or OMR (=2) depending on the two bottom bits of inst; 2-3 insns each (load register, mask with computed immediate, store). LUA takes a 5-bit EA mode, snapshots R[srcreg], runs calc_ea for its Rn-update side-effect, reads the new Rn into a temp, restores R[srcreg], writes the temp into N[dst] or R[dst] (depending on inst[3]). LUA_REL uses a 7-bit signed offset directly from the inst fields without calc_ea. All four previously hit the BLR fallback through their emu handlers. **REP coverage completed** — Phase 6 only inlined `_imm`; now `_aa` (6-bit absolute via `emit_mem_read_xy`), `_ea` (calc_ea → `emit_mem_read_xy`), and `_reg` (A/B via `emit_pm_read_reg`, others direct LDR, mask to 16 bits) are all inline, sharing `emit_cf_rep_common_start` for the LCSAVE / pc_on_rep / loop_rep trio. **Parmove write-set narrowing** — for pm_5 with `ea_form=0` (short-absolute 6-bit memory address), the address is provably in `[0, 0x3F]` at translate time, which is always xram / yram. The write-set previously flagged `X_ANY` (xram + mixbuffer + peripheral); now narrows to `DSP_JIT_WS_XRAM` only, cutting the DIFF compare region for every such pm_5 write. Remaining BLR-fallbacks in the CF / misc category: movec / movem / movep / do _aa / _ea / _reg / dor_reg / tcc / stop / wait / reset — all low-frequency audio ops, deferred for follow-up unless workload-specific profiling shows them hot. | +1-3% (the misc ops aren't as hot as ALU, but andi/ori are common in boot/setup blocks; REP coverage shrinks the cf_fallback count further) |
| 7 | **landed** | Interrupt handling at block boundaries (inline fast-path skip when interrupt_state / counter / pipeline_count all zero) | neutral (correctness) |
| 8 (partial) | **landed (lazy-flag subset)** | Lazy-flag elimination for `emit_ccr_e_u_n_z`: translate_block peeks one op ahead in the block. If the next op is a "full ccr writer" (another ALU op that unconditionally overwrites all of E/U/N/Z via the interpreter's / inlined ccr_update path), this op's ccr call is dead — the ~25-insn fast-path is elided via a one-shot flag consumed by `emit_ccr_e_u_n_z`'s fast return. Full-writer classification: parmove ALU bytes outside MOVE/TFR/LSL/LSR/ROL/ROR, plus long-imm ALU (`(inst & 0xFFFFF0) == 0x0140C0`). Skip count reported as `alu_ccr_skipped` in the stats output. | +2-5% landed |
| 8 (A/B pinning) | **landed** | **Accumulator register pinning (A/B)** — the block's prologue loads packed 56-bit A into x26 and packed 56-bit B into x27 (callee-saved ARM64 regs, added to the prologue/epilogue STP/LDP frame); within the block, `emit_load_accu56` degenerates to a single `MOV xdst, x26/x27` (from a 7-insn `LDRB + SBFX + LSL + LDR + BFI + LDR + BFI` chain) and `emit_store_accu56` adds an `SBFX + MOV` to the pinned reg in front of its unchanged write-through `UBFX + STR` trio — i.e. memory stays the authoritative copy (no epilogue spill needed) so any BLR fallback into the C interpreter (emu_max, emu_pm_4x, postexecute_interrupts) still reads a consistent `registers[A/B]`. After every BLR-fallback that may mutate A/B via the interpreter path (the single remaining ALU fallback `emu_max`, the `pm_4x` helper, and the generic non-inlined `emu_*` fallback for movec/movem/movep/etc.), the JIT emits `emit_reload_ab_pins` (~14 insns) to reload x26/x27 from the updated memory. Direct-memory write sites that bypass `emit_store_accu56` — CLR (whole accu), TFR (3-word copy), LSL/LSR/ROL/ROR (A1-only), ANDI/ORI long-imm (A1-only), AND/OR/EOR/NOT logical (A1-only), and the non-accu-src TFR variant (X0/X1/Y0/Y1 → A/B with sign-extend) — each got a matching pin-sync step (`MOV`, `BFI`, or `SBFX+LSL` as appropriate) so the pin and registers[] never diverge mid-block. Chaining safety: the prologue's accu-load lives before `chain_entry`, so a chained-in block skips the reload and trusts the upstream block's pin to be current — which it is, by the write-through invariant. Estimated savings: ~6 insns per ALU read, ~1 insn per ALU store, on the dominant ADD/SUB/MAC/MPY/ASL/ASR/ADDL/SUBL/ADDR/SUBR/ADC/SBC/RND paths. Per the `100.0%-of-ALU-ops-inlined` + `72.6M-blocks-executed` baseline, the typical block touches A or B 4-6× so this removes ~30 insns / block from the ALU hot path. | +10-20% on ALU-heavy blocks |
| 8 (X/Y pinning) | **landed** | **X0 / X1 / Y0 / Y1 register pinning** — x20 and x21 (previously cached `postexecute_update_pc` / `postexecute_interrupts` helper pointers) repurposed as packed 48-bit X and Y bank pins (`x20[47:24]=X1, x20[23:0]=X0`, `x21[47:24]=Y1, x21[23:0]=Y0`). Helpers materialised inline at their BLR sites in the epilogue slow path (rare — only fires when REP / DO loop state is active, so +4-insn cost is negligible). Hot read sites converted to UBFX-from-pin (1 insn vs LDR's 2-3): `emit_load_alu_src` (`ALU_SRC_X/Y` / `*_AT_A1` forms), `emit_alu_mac` src1/src2 (FIR kernel saves ~6 insns per MAC), `emit_alu_tfr` (Xn/Yn → A/B), `emit_alu_logical` (AND/OR/EOR), `emit_pm_read_reg` (non-A/B path). Hot write sites extended with BFI-into-pin: `emit_pm_write_reg`'s X/Y dispatch arms plus the pm_8 / pm_1 direct-STR-to-registers paths (which originally bypassed `emit_pm_write_reg` entirely for non-A/B destinations — fixed at `54d1543d50` after PIN_AUDIT caught 184k Y-pin divergences in Azurik's FIR kernel). BLR-fallback reload extended: `emit_parmove_pm4` (pm_4x numreg 2/3 writes X/Y) and `emit_instruction` generic `emu_*` fallback both call `emit_reload_ab_pins + emit_reload_xy_pins`. `XEMU_DSP_JIT_PIN_AUDIT=1` diagnostic (emits per-op pin-vs-memory check, BLR-logging first divergence) stays in-tree as a dev flag — caught the `pm_2_2 Y0,A1` direct-A1-slot leak at `91343ad754` and the pm_8 X/Y leak within 30 seconds each. `x28` has since been claimed as the SR pin (`DSP56K_JIT_SR_PIN_REG`, `dsp56k_jit_arm64.c:1452`) — no free callee-saved GPR remains for fusion, which must share pins or spill. | +10-15% on MAC-heavy kernels |
| 8 (extended lazy-flag) | **landed** | Multi-step CCR-dead lookahead. The original `dec694349f` peek was 1-op-only; now `translate_block` walks forward up to `DSP_JIT_CCR_LF_MAX_STEPS`=4 ops, skipping over CCR-passthrough parmoves (ALU byte = `ALU_KIND_MOVE`, no SR.E/U/N/Z touch). `inst_is_full_ccr_writer` extended to also match RTI / ENDDO via the `dsp_jit_helper_classify_cf` classifier (both pop SR off the stack, fully overwriting E/U/N/Z). Catches the common "ALU + pure-parmove pre-roll + ALU" shape (e.g. FIR kernels with `move x:(r0)+,x0  move y:(r4)+,y0  mac ...`). Translate-time cost ≤200 cycles per op (bounded by the 4-step limit); runtime saving ≥ 25 ARM64 insns × every block execution per newly-eliminated CCR emit. | +1-3% additional (incremental over the one-step subset) |
| cmpu inline | **landed** | Last entry in the `cf_fallback` bucket to cross the "inline it anyway for completeness" threshold. Emits a CMP-with-sign-extended-24-bit kernel: loads the source (X0/X1/Y0/Y1 direct from X/Y pin UBFX, or A/B via the existing `dsp_jit_helper_pm_read_accu24` BLR shim so SR.L gets updated on saturation), sign-extends the 24-bit value into bits [47:24] of x11 with `SBFX + LSL #24` (matching `emu_cmpu`'s `source[0] = sign(v)*0xff; source[1] = v; source[2] = 0` layout), subtracts from x10=dest, re-sign-extends to 56, pulls C out of the existing `emit_addsub_flags` output, and BFIs C/N/Z into SR after a `V\|C\|Z\|N` clear. Does NOT write the accu (CMPU is a pure compare). After landing, `cf_fallback` drops to ~500/run — all in `illegal` (emu_undefined — genuine opcode-decode failures) and the 180-op `bit_manip` tail (bset/bclr/bchg/btst, which isn't worth the emit-time cost at <10µs/run on the hot workload). | +0.1-0.3% (low-frequency op, but closes the CF bucket for audit cleanliness) |
| per-flag E/U lazy-flag | **landed** | Symmetric variant of the N/Z partial-kill. Kills an upstream full-writer's E/U half when the downstream eu-passthrough walk reaches a full writer that the narrower ccr-passthrough walk would miss. The eu-passthrough set is a strict superset of ccr-passthrough: everything in ccr-passthrough (reads nothing from CCR) plus TCC (reads only C/V/N/Z via emu_calc_cc, never E/U; non-terminator). Conditional branches (JCC/BCC/JSCC/JCLR/JSET/JSCLR/JSSET/BRCLR/BRSET) are also E/U-safe readers but they're all terminators so the walk breaks on them naturally. Extended walk runs after the main walk only when no full writer was found (full-dead already subsumes eu-dead); MAX_STEPS bound applies symmetrically. `emit_ccr_e_u_n_z` restructured so the three scaling-mode branches + E/U computations are guarded by `if (!skip_eu)`; the scaling==3 → skip_nz_label branch still runs unconditionally (its semantic matches the intended all-zero-CCR outcome for scaling==3, regardless of skip mode). Folds `skip_eu && skip_nz` back into the full-dead fast path. In practice likely to fire ~0 on Azurik (matching the N/Z zero-fire observation) — TCC is rare in audio DSP code — but completes the per-flag coverage and keeps the lookahead infrastructure symmetric. `alu_ccr_eu_skipped` counter added alongside the others. | +0-2% (measure-dependent; TCC-free workloads see zero fire) |
| per-flag N/Z lazy-flag | **landed** | Partial-kill variant of the CCR emit elision. The existing all-or-nothing knob (`g_ccr_dead_next_emit`) only fires when a later op fully overwrites E/U/N/Z, missing the common case where an intermediate passthrough (LSL / LSR / ROL / ROR / AND / OR / EOR / NOT parmove, LSL_IMM / AND_IMM CF, long-imm and/or) overwrites N/Z but not E/U. Added `g_ccr_nz_dead_next_emit` + `inst_overwrites_nz` classifier: the lookahead tracks `saw_nz_ovr` along with the full-writer search, and if it walks past any N/Z-overwriter without finding a full writer, the upstream op's N/Z half is dead while the E/U half must still run. `emit_ccr_e_u_n_z` gains a `skip_nz` branch that omits the N/Z block (~7 ARM64 insns + 1 patch slot per skip) while keeping the scaling-mode-specific E/U computation. Safety invariant: the preceding clear of E\|U\|N\|Z still runs, leaving N/Z = 0 in SR; the downstream N/Z writer's own clear-and-set step fully rewrites N/Z before any reader can observe the intermediate zeros. `alu_ccr_nz_skipped` reported alongside `alu_ccr_skipped` in stats. | +2-5% on audio kernels where `ALU → logical_pm → ALU` chains are common (mixer saturation, FIR tap-and-clamp) |
| lazy-flag correctness + expansion | **landed** | Two refinements to `inst_is_full_ccr_writer` / `inst_is_ccr_passthrough`. **Correctness fix**: parmove AND/OR/EOR/NOT (A1-only, writes only N/Z/V) and long-imm and_long/or_long (same semantics — distinguished from add/sub/cmp_long by `inst[2:0]`) were previously lumped into the catch-all "full writer" default. They do NOT overwrite E/U, so if one immediately followed a full-writer ALU within the lookahead window, the upstream's E/U emit was wrongly elided — a latent bug the DIFF harness never tripped (too narrow a dynamic pattern in Azurik) but semantically wrong. Now explicitly excluded from full-writer and added as passthrough. **Passthrough expansion**: adds AND/OR/EOR/NOT parmove + long-imm and/or to the walk-past set, plus CF ops that provably neither read nor write E/U/N/Z (NOP, LUA/LUA_REL, LSL_IMM, AND_IMM, REP_* all variants). Bumped `DSP_JIT_CCR_LF_MAX_STEPS` from 4 to 8 now that more op shapes qualify as passthrough — gives the walk headroom to bridge longer "ALU → misc → parmove pre-roll → ALU" patterns common in the audio glue between FIR kernels. | +1-3% (on top of the one-step + four-step subsets; the correctness half has zero observable perf effect but removes a latent divergence path) |
| cf_fallback tail | **landed** | **Data-driven CF/misc inlining informed by `g_cf_fallback_buckets`**. Fallback classifier (`dsp_jit_helper_classify_fallback`) groups BLR handlers (NOP / INC_DEC / SHORT_IMM_ALU / SHIFT_IMM / BIT_MANIP / MOVE_EXTENDED / CMPU / MPYI / DIV / NORM / MOVEP_1/23/XQQ / STOP / WAIT / RESET / ILLEGAL / UNDEFINED), printed by `XEMU_DSP_JIT_STATS`. Iterated on the hottest bucket each round: **NOP** (empty function → emit zero code; was 26% of `cf_fallback`), **INC / DEC** (reuses `emit_alu_long_imm_arith` with src=1), **short-imm ALU** (add_imm / sub_imm / cmp_imm / and_imm — 6-bit `inst[13:8]` immediate zero-extends into the long-imm pipeline), **move_extended** (`move_x_long` 2-word + `move_x_imm` / `move_y_imm` 1-word: R[r]-based address + optional baked 24-bit offset, BLR-bail on SP / SSH / SSL register side-effects), and **shift_imm** (ASL / ASR 6-bit + LSL 5-bit — `emit_cf_asl_imm_op` builds C/V/L inline from a UBFX-masked 56-bit source, `emit_cf_asr_imm_op` uses logical right shift on the zero-ext value, `emit_cf_lsl_imm_op` mirrors the A1-only bulk shift with new C = orig[24-ii] and per-pin BFI). ASL_IMM / ASR_IMM / INC / DEC / ADD/SUB/CMP_IMM added to `inst_is_full_ccr_writer` so they participate in the multi-step lazy-flag lookahead. Net effect on Azurik: `cf_fallback` dropped from ~60% of emitted CF ops (pre-NOP) to ~0.03% (post-shift_imm) — `cf_inlined` at 99.97%, with the remaining tail (cmpu / mpyi / bit_manip / illegal) all below 0.1% of total ops each. | +3-6% (the shift_imm round alone is 8.8% of total ops, the entire tail is ~12% of total ops pre-inline) |
| 8 (remaining) | **deferred** | **Parmove+ALU fusion** — with A/B/X/Y pinning in place the "save/restore" is already a 1-insn UBFX, so the remaining fold candidates are narrow. Specifically: when the parmove's `save_reg_N` holds the value that will also be written to memory (the write-d=0 case), and the ALU doesn't touch the source reg, we can stream directly from pin → memory skipping the intermediate `save_reg_N` register. Needs case-by-case analysis per pm_N family; deferred until profiling shows a specific shape hot. cc_op shadow is LANDED (both N/Z and E/U halves — see the "per-flag N/Z lazy-flag" and "per-flag E/U lazy-flag" rows above). Measured `alu_ccr_nz_skipped` and `alu_ccr_eu_skipped` are both zero on Azurik — the audio workload hits full-dead (20% of ALU ops) but never exercises the partial-kill paths (ALU+LSL/AND+… chains with no full writer in 8 steps, or ALU+TCC+ALU patterns). Infrastructure stays in for future non-audio DSP programs. | +0-2% (partial-kill paths unfired on audio; fusion narrow) |
| pm_read_accu24 inline (S=0) | **landed** | `emit_pm_read_reg`'s A/B arm — previously an unconditional BLR to `dsp_jit_helper_pm_read_accu24` on *every* accumulator parmove read — now inlines the scaling==0 case (S1:S0 == 0, universal in Xbox audio code). The sign-extended 56-bit pin makes the limited-read check 2 insns: `ASR x0, pin, #24` (signed a2:a1) vs `SBFX x1, x0, #0, #24` — equal iff a2 is the pure sign extension of a1 (exactly the interpreter's a2==0x00/a1≤0x7fffff ∪ a2==0xff/a1≥0x800000 no-limit conditions). Fast path: `UBFX value, x0, #0, #24`. Limited path: `CSEL 0x800000/0x7fffff` on the accu sign + SR.L set via load/BFI/store-through (pin + memory). Runtime `UBFX (SR >> S0) & 3 / CBNZ` guards the fall-back BLR for non-zero scaling (and the indeterminate scaling==3). | +2-5% DSP (hits every A/B parmove read) |
| bit_manip inline | **landed** | The 16-handler bset/bclr/bchg/btst × aa/ea/pp/reg tail — the last fallback bucket with steady-state volume (~180 ops/run) — now emits inline via `emit_bit_manip_op` + `dsp_jit_helper_classify_bit_manip`. Shared shape: read (aa = immediate addr, ea = `emit_calc_ea_inline`, pp = 0xffffc0+imm, reg = raw LDR or limited A/B via the new pm_read inline), `UBFX` carry, OR/AND/EOR with translate-time bit mask, write-back (`emit_mem_write_xy` / `emit_pm_write_reg`), SR.C via load/BFI/store, `instr_cycle += 2` additive (preserves calc_ea's mode-5/6/7 +2). REG variants targeting dsp_write_reg side-effect registers (SR / OMR / SP / SSH / SSL) keep the BLR fallback; btst (no write-back) always inlines. Memory-writing variants feed the diff validator's write-set (aa → exact X/Y bit; ea / pp → WS_ALL, conservative for peripheral DMA triggers). | <1% steady-state; closes the last bucket |
| retro-chaining | **landed** | Static chaining extended to targets translated *after* the source: a chainable terminator whose target block doesn't exist yet still swaps `expected_next_pc` and emits a patchable chain `B` (initially routed to the source's own shared exit = unchained behavior), registered in a per-core `DspJitState.pending_chains` pool (128 entries; overflow drops the registration — still correct, just unchained). When a block is installed at some pc, `retro_chain_patch_pending` claims every pending site targeting it: generation-checked (stale source re-translation skips), patched to the new `chain_entry`, registered in `incoming_chains` for un-patch on invalidation, per-site icache flush. `DSP_JIT_MAX_INCOMING_CHAINS` raised 8 → 16 for shared subroutine heads. The install-time chain bookkeeping (including the pre-existing list-full repatch) was moved before `qemu_thread_jit_execute()` — the old placement wrote to the code buffer after W^X re-protect, a latent fault on the list-full path. Pool cleared on `dsp_jit_invalidate_all` (code buffer recycled). | +1-3% (first-execution chains no longer permanently lost to translation order) |

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
