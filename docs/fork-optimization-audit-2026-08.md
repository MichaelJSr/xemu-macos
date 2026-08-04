# Fork-wide optimization audit — 2026-08-04

A deep, adversarially-verified audit of the whole fork for remaining
optimization and refinement candidates, run at the arm-(b)-on
equilibrium (F8 heavy anchor ~36-39 fps @ ~831 draws/flip, vCPU-bound,
guest-TCG generated code ~55% of vCPU samples — `docs/roadmap.md`
state header, 2026-07-19).

**Method.** 49-agent audit: one digest agent condensed the
institutional record into 92 dead-end entries + 14 open items (from
`docs/optimizations.md`, `docs/roadmap.md`, and the failure-archaeology
skill); 8 subsystem auditors (NV2A/Vulkan, TCG/CPU, DSP/APU,
presentation, build/toolchain, memory/BQL, a profile-grounded sweep
over `bench-out-profile-f8/`, and frontier/upstream) produced 40
findings, each carrying `file:line` evidence, a benefit range with
arithmetic, and a pre-registered prediction + kill threshold; every
finding then got an independent adversarial verifier instructed to
refute it (prior-art check, code re-derivation, Amdahl re-derivation
of the benefit math, architecture-contract invariant check). Verdicts:
**8 confirmed, 17 plausible, 15 refuted.** A manager pass then
deduplicated convergent findings, folded verifier corrections into the
numbers below, and spot-checked the four load-bearing claims directly
in the tree.

**Reading this doc.** Nothing here is planned work. Every item is a
candidate gated on its stated measurement, per
`xemu-research-methodology` (predict a number and a kill threshold
before running anything). Benefit figures are the *verifier-refined*
ranges, not the auditors' original (usually higher) claims. Items on
threads other than the vCPU thread are tiered quality/latency —
Amdahl says they are ~0 fps on the F8 anchor no matter how good they
look locally.

---

## 1. vCPU-thread fps candidates (the F8 bottleneck), ranked

### 1.1 Elide `can_do_io` bookkeeping + `cpu_io_recompile` when icount/replay are off

The single largest verified candidate. `accel/tcg/translator.c:236/239`
emit `set_can_do_io(false)`/`(true)` stores into **every** multi-insn
TB body with no icount/replay gate, and `io_prepare`
(`accel/tcg/cputlb.c:1313-1317`) fires `cpu_io_recompile` on every
mid-TB MMIO access — a g_tree `tcg_tb_lookup` + state restore +
siglongjmp + `CF_MEMI_ONLY` re-translate + re-execute round trip, on
the guest's NV2A/APU register traffic. `cpu_io_recompile` is a stable
top-25 vCPU symbol in both committed profiles (0.25%
`ab_E1.log:1281`, 0.31% `ab_warmup.log:583`); nothing can observe
`can_do_io` when icount and replay are off.

- **Mechanism.** Gate *both* stores (`:236` and `:239` — gating only
  `:236` halves the win) on `icount_enabled() || replay_mode !=
  REPLAY_MODE_NONE`, behind an XBOX escape hatch (`XEMU_CANDOIO=0`
  reverts). Verifier defused the watchpoint hazard: `watchpoint.c:102`'s
  `!can_do_io` branch is nested inside `replay_running_debug()`, dead
  here; the surface-callback path is unaffected.
- **Expected benefit.** +0.35 to +1.6 fps (0.95-4.4% of vCPU wall),
  central ~+0.7 fps — counter-based estimate, haircut applied for the
  arch-7.5 precedent where a same-path prediction overshot into
  jitter-only.
- **Validate.** First a `cpu_io_recompile` call counter: KILL before
  building if < ~40k/s on F8. Then `XEMU_GUEST_PROF` (symbol must go
  to 0.00%), then 3 interleaved pairs: KILL if mean < +0.20 fps or
  sign-mixed. Correctness gate before any fps claim:
  level-transition repro + loadvm soak (`mem_io_pc` semantics change).

### 1.2 `notdirty_write` dirty-check fast path (two convergent findings, one campaign)

The store slow path's tail is measured at ~1.9-2.0% of vCPU wall
(`ab_E1.log:1219-1220`: 6.7M calls, ~278 ns/call outside the
invalidation, timing-probe inclusive). Two mechanisms, verified down
to the machine code (`otool` shows all five `bl
_physical_memory_get_dirty_flag` calls executing unconditionally):

- **(a) Delete `physical_memory_is_clean` from `notdirty_write`**
  (`cputlb.c:1401`). After `:1398` ran `set_dirty_range(...,
  DIRTY_CLIENTS_NOCODE)`, the four non-CODE bits are set by
  construction — `is_clean(ram_addr) == !(CODE dirty)`, already known
  from `:1382`. Best shape (verifier): give
  `tb_invalidate_phys_range_fast` a bool return so 100% of calls skip
  the re-read. Refined: **+0.13 to +0.22 fps**, central +0.17.
- **(b) Fuse the RCU critical sections** across the remaining
  dirty-bitmap ops in `notdirty_write` and the TLB-refill dirty check
  (`cputlb.c:1115`) — one guard, one idx/offset computation, `test_bit`
  instead of five `find_next_bit` loops. Refined: **+0.09 to +0.19
  fps** (the auditor's `dmb ish` claim was false — all guards are
  nested inside `cpu-exec.c:1597`'s outer guard; the cost is the
  noinline `get_ptr_rcu_reader`/`_tlv_get_addr` traffic, 0.42% of vCPU
  samples shared across sites).
- Combined honest expectation (overlapping denominators, **not**
  additive): **~+0.2 to +0.35 fps**.
- **Validate.** `XEMU_INV_PROF=1 XEMU_INV_TIMING=1` on F8: `rest=`
  must drop from ~278 ns/call to ≤ 200 (a); ≤ 180 (a+b), with call
  counts within 2% of baseline. KILL if the drop is < 60 ns/call.
  Then the standard interleaved A/B. Risk: silent missed-SMC if the
  NOCODE argument is wrong — it is not (`physmem.c:1181-1200` sets all
  four unconditionally), but the soak gate stands.

### 1.3 Surface CPU-access-callback churn issues full guest TLB flushes (rate unmeasured)

Two auditors converged on this independently (pgraph-vk and
memio-threads). Every NV2A surface create/destroy calls
`mem_access_callback_insert/remove` (`vk/surface.c:716-735`, wired at
`:910`/`:828`), and each of those is `async_safe_run_on_cpu` **plus**
`tlb_flush_all_cpus_synced` (`system/physmem.c:891/895` and
`:914/:918`, both carrying the fork's own `// FIXME: flush only
applicable pages`) — i.e. two exclusive-execution vCPU stops and a
full all-mmuidx TLB flush *plus* an unconditional
`tcg_flush_jmp_cache` (`cputlb.c:395`, outside the elide test) per
event. Renderer-initiated TLB flushing has never appeared in any
profile; nobody has read the existing flush counters.

- **The dispute the measurement must settle.** The pgraph auditor
  argues the Azurik zeta ping-pong (`surface.c:2236-2239`, "twice per
  flip") makes this ≥ 4 events/flip = ≥ 152 full flushes/s *in the
  steady F8 scene*; the memio verifier found the transition logs show
  ~3.3 events/s and steady-state churn near zero. Both agree on the
  mechanism; the rate is the whole question.
- **Expected benefit.** If the per-flip rate is real: +0.35 to +1.5
  fps on F8 (TLB+jump-cache refill waves, 0.15-1.0 ms per flush). If
  not: a transition/streaming-hitch reduction in the v0.11
  new-area-lag symptom class, ~0 steady-state fps.
- **Zero-code first gate** (verifier): HMP **`info jit`** already
  prints `TLB full flushes` (`tcg-stats.c:141-163`) and the monitor
  socket is already scripted — two samples 10 s apart on the F8
  savestate give the rate today. PREDICT ≥ 100/s if the ping-pong
  theory holds; KILL the steady-state half if < 30/s (the hitch half
  then proceeds under the movement probe instead).
- **Mechanism if it fires.** (a) Reuse-cache registrations keyed on
  `(ram_addr, len)` — the callback body (`surface.c:661-700`) re-derives
  hits from `r->surface_ranges` and ignores the registered range, so a
  stale-superset registration is semantically harmless; a 2-entry
  cache kills 100% of ping-pong churn. (b) Batch
  invalidate-then-create in `update_surface_part` into one deferred
  flush. (c) The FIXME itself: ranged invalidation by host address
  (template: `tlb_reset_dirty_range_locked`), which also spares the
  jump cache. Risk class: stale-framebuffer artifacts if coverage ever
  under-runs the live surface set — debug-build assert + artifact
  scorer on every soak.

### 1.4 x87/SSE translator constant-factor pack (shared Gate-0 census)

Three related candidates in the fork-owned i386 translator, all
gated on one cheap x87/SSE-density census (the `xemu_gen_counter_inc`
idiom, dumped under `XEMU_INV_PROF`) because the F8 profile is flat
and nobody knows E (executions/s) for any of them:

- **(a) x87 register-cache dirty tracking.** `flush_fp_regs`
  (`target/i386/ops_fpu.h:67-83`) writes back every cached slot with
  no dirty bit, and the flush fires at every helper call and BB end —
  a pure ST(i) *read* costs a ~20-insn `st80f` write-back at the next
  flush. Verifier's sharpening: `gen_fpop` already NULLs popped slots
  without write-back, so the dominant `fld/fmul/fadd/fstp` pattern
  mostly leaves only the **dead FT0 store** — the FT0 half is the
  bigger, cheaper, safer harvest; ship it first behind its own bit.
  Refined ceiling: E capped ~18M/s ⇒ up to ~+1 fps at the top,
  realistically **+0.2 to +0.6 fps**. Gate-0 bar (raised by verifier):
  KILL if elidable E < 8M/s (≈ +0.43 fps predicted; below that a
  3-pair A/B cannot resolve it). Risk: silent wrong-math if a writer
  is missed — needs the `XEMU_X87_REFUTE` shadow-check harness before
  any promotion.
- **(b) Defer x87 FIP/FCS/FDP/FDS exception-pointer stores.** Every
  x87 memory-form instruction emits 6 dead-in-practice stores
  (`translate.c:4082-4092`, `:4493-4500`); under TCG the *only* reader
  is `do_fstenv` (FXSAVE writes literal zeros —
  `fpu_helper.c:3050-3055`), and every consumer site already ends the
  TB. Defer to one update per contiguous x87 run. Refined: **+0.11 to
  +0.60 fps**, central ~+0.31. Same census, same refuter pattern.
- **(c) Lower SSE `COMISS/UCOMISS` to the existing `com/coms` TCG
  ops** instead of a helper call (helper call ⇒ `gen_bb_epilogue` ⇒
  full x87-cache flush + global spills; `emit.c.inc:4319-4325`,
  `:4714-4721`). Verifier corrections that reshape it: **COMISD is
  unreachable** (guest is Pentium III, no SSE2 — scope to the SS
  forms only), and there is no `ld_f32` TCG op (use the mov32 bitcast
  path). Refined: rides entirely on the unknown compare rate;
  **+0.1 to +0.5 fps** if the census clears ~1.5M compares/s.
  Denormal risk: needs a `=2`-style differential oracle over ≥ 50M
  compares including a level transition before default-on.

### 1.5 Suppress provably-useless `pfifo_kick` broadcasts — re-scoped to a jitter win

`pgraph_write` kicks unconditionally in the `NV_PGRAPH_INTR` case and
for both directions of `FIFO_ACCESS` (`pgraph.c:222`, `:289-293`);
on the NOP-notify ISR path 2 of 3 broadcasts provably cannot unblock
the PFIFO thread. The verifier bench-measured the real constants on
this machine (~1.1 µs with a re-sleeping waiter, 7-9 ns without;
the auditor's model double-counted) and re-derived the central mean-fps
gain at **~0.48% — a coin flip against the 0.5% kill line**. The
precedent (arch 7.5, same shape) delivered jitter, not mean.

- **Disposition.** Keep as a *jitter* candidate: pre-register fps
  stdev (predict 1.2 → < 1.0 class improvement) alongside mean. The
  lock-free `pgraph_read` sub-mechanism is dead on its own math
  (+0.02 fps = the static-baseline resolution; unvalidatable). First
  step: `XEMU_PFIFO_KICK_STATS` counter bucketing call site ×
  waiter-existed × ISR write order. **Highest-care item in this doc**:
  a wrongly-suppressed kick is a permanent PFIFO hang (arch 7.1
  family) — `XEMU_PFIFO_HEARTBEAT=1` on every run, plus the untimed-wait
  invariant audit (`pfifo.c:110-113`) in review.

### 1.6 Gate the arm-(b) prefilter's diagnostic counters off the hot path

`sf_out_store_prefilter` (`tcg/aarch64/tcg-target.c.inc:1786-1910`)
unconditionally emits 6-insn counter RMWs at six sites into a stub
that runs ~2M times/s; the counters are read only by an atexit dump.
Verifier bounded the whole prefilter at ~1.8% of vCPU and struck the
auditor's latency argument (entries are ~1750 cycles apart; no
store-forwarding stall) — honest cost is issue/port pressure, ~2-4 of
~32 stub cycles. **Refined: ~+0.1-0.2% vCPU (≈ +0.04-0.08 fps)** —
micro, but the fix is a translate-time `if (xemu_sf_prof_on())` with
zero behavior change and zero risk, so the benefit-per-effort is
excellent. Validate via draws/s (not raw fps — arm-(b) precedent),
refuter soak unchanged at 0 violations.

### 1.7 Reports idle-budget scan below 300 µs — zero code, modest expectation

`XEMU_REPORTS_BUDGET_US` (default 300, `reports.c:312-319`) can be
scanned to 150/50 µs. The verifier recalibrated against the knob's own
5 ms → 300 µs receipt (commit `7a9b116680`): realized/naive conversion
was **20.7%**, and the "1-2.2 expiries/flip" figures are from the 5 ms
era on other fixtures. Honest expectation at 50 µs: **~+0.1-0.2 fps**,
likely under the noise line. Run it as a free experiment only after
re-measuring expiries/flip at 300 µs (`finish_reports_submit` in
nsprof; KILL permanently if < 0.3/flip). Guard: passes/flip must not
rise > 2 (submit-storm cliff, arch 1.1).

### 1.8 Devirtualize `TCGCPUOps::get_tb_cpu_state` (micro)

Single-target fork pays a double-indirect vtable call at four hot
lookup sites; the 25-insn callee shows as its own symbol (0.24-0.31%)
in both profiles. `#if defined(XBOX)` direct call + compile-time
assert against `tcg-cpu.c:169`. Verifier halved the auditor's number
(the ret-memo-hit-path claim was false — helper hit rate is 0.3%, the
probe is inlined): **+0.07 to +0.11 fps**. ~20 lines, near-zero risk;
worth batching with 1.6 rather than benching alone (needs 4+ pairs to
resolve).

### 1.9 ImGui frame build under BQL+main-loop mutex — measure first, expect a kill

The HUD build holds both locks (`ui/xemu.c:1273-1275`, `:215-222`).
The verifier capped the ceiling with the arch-7.5 receipt: total vCPU
BQL acquire-wait on the heavy scene was 0.61% *summed over all
holders*, max single hold 340 µs — so the recoverable share here is
**~0.05-0.3%**, not the auditor's 0.76-3.8%. The `DEBUG_XEMU_C=1`
bql-occupancy line (`ui/xemu.c:885`) settles it in one run: KILL if
UI occupancy < 0.5% (expected outcome). Record the number either way.

---

## 2. Hitch, latency, and quality candidates (~0 fps on the anchor by Amdahl)

### 2.1 Quantize MetalFX output size + rebuild cooldown on window resize

`display.c:2209-2218` derives the scaler output from the live layer
size with only even-pixel quantization, so a corner/vertical drag
rebuilds the MTLFXSpatialScaler **and its 3-texture ring** at up to
the event rate — verifier: ~180-360 rebuilds per 3 s drag (~33 MB of
private-texture alloc/free each at 1440p-class layers), each preceded
by a MetalFX drain that stalls the PFIFO thread (max ~4 ms measured,
arch 1.12). Snap to a 64/128-px ladder + ~200 ms cooldown; the
existing present blit absorbs the mismatch. Floor guard: quantizing
below `disp->width` silently disables upscaling (`display.c:2230`).
Validate with a rebuild counter + `MFX_DRAIN` totals during a scripted
drag (predict ≥ 60 → ≤ 5). Note: `config_spec.yml`'s
`metalfx_upscale` bool (:236-238) is a dead key — no reader; remove or
wire it while in the file.

### 2.2 Acquire the CAMetalLayer drawable before consuming the present ring

`ui/xemu.c:1154` consumes the ring step, then `:1234` blocks in
`nextDrawable` (`ui/xemu-metal.m:194`); the ordering comment only
requires the event value before the render pass opens (`:221`).
Splitting acquire/begin-pass moves the step choice after the block.
Verifier correction that reshapes it: for the FIFO majority the same
entry is returned either way — the gain comes only via the
bounded-debt catch-up sampling later, so **A/B at the default
`XEMU_PUSH_DEBT`** or it measures 0. Value: up to ~8-16 ms of
presented-frame age in the interpolation regime (step rate ≥ panel
refresh); 0 fps everywhere. Needs the 2.3 instrumentation first.

### 2.3 Restore UI-thread timing instrumentation (enabler)

Under the shipped push-present default there is no time-attributed
measurement of drawable-acquire block, HUD/lock hold, or ring-read →
present age (the only UI-thread bucket, `NSPROF_PRESENT_WAIT`, is ~0
under push). Three nsprof buckets (`DRAWABLE_ACQUIRE`, `UI_HUD_LOCK`,
`UI_PRESENT_PERIOD`) make items 1.9, 2.1, 2.2 measurable without
bespoke builds. 0 fps by construction; ~0 cost when the knob is off.
**Higher-value side finding (doc drift, fix with change control):**
the architecture-contract skill's Invariant 8 still says pull-model
present is the default — `display.c:99-106` has defaulted **push ON
since 2026-07-12**. Update the invariant text and its drifted line
anchors.

### 2.4 Movement-probe gate for async pipeline creation (arch 1.12 reopen test)

Zero code: run `XEMU_NV2A_NSPROF=1 scripts/movement-probe.sh` and read
`pipeline_gen` in the forward (first-visit) phase vs the backward
control — the one workload class the original steady-state kill never
measured. **Use the verifier's corrected gate**: a 164 ms compile
averages to 0.86 ms/flip over a 5 s interval, so a per-flip-average
threshold is broken by construction — gate on `max=` and event count
per interval (any single compile ≥ 8 ms in forward phase = the hitch
is real), warm and cold `pipeline_cache.bin` runs. Fires ⇒ Experiment
B option (a) (prewarm, `docs/moltenvk-optimization-experiments.md`)
graduates with a baseline number; doesn't ⇒ close arch 1.12
permanently for this fixture set.

### 2.5 Reorder `dsp_core_t` so JIT-hot scalars sit inside the ARM64 imm12 window

122 KB of arrays sit between the near scalars and the JIT-hot tail
(`dsp_cpu.h:48-105`); 12 of ~49 fixed insns per translated DSP op are
3-insn far-offset sequences that become 1-insn forms if the hot
scalars move ahead of `xram` (verifier compiled a struct replica and
confirmed all 19 quoted offsets exact; count is 12, not the claimed
13). APU thread is ~20% utilized and off every critical path ⇒ **0
fps**; value is code-buffer footprint (predict −10-14%, not the
auditor's −18-26% — re-register before running) and cache locality.
Mandatory coupled edit: re-cut `dsp_state_diff`'s offset range table
in the same commit or the DIFF validator silently goes false-green.
`XEMU_DSP_JIT_DIFF=1` full-session zero-failures is the gate.

---

## 3. Build, toolchain, and release-integrity candidates

### 3.1 Disable `-fzero-call-used-regs=used-gpr` behind a hardening knob

The flagship build finding, confirmed end-to-end. Upstream's
hardening block (`meson.build:729-750`) applies used-gpr register
zeroing globally with no option and no fork override; the fork already
disables `stack_protector` (`build.sh:368`) but missed these. Measured
on the local (non-PGO) binary: 3.4-3.8% of the dynamic
compiled-instruction stream is zeroing movs; verifier recompiled hot
TUs under `-fprofile-use` and applied the PGO haircut (~half), plus
found the flag also defeats tail-call optimization (~25-33% of the
removed insns are restored `b`-for-`bl+ret`, real work). Fix is one
last-flag-wins append in `build.sh` (`-fzero-call-used-regs=skip`),
escape hatch `XEMU_HARDENING=1` restores it. **Refined: +0.11 to
+0.83 fps, central ~+0.34 (+0.9% draws/s)**, ~170 KiB off `__text`
post-PGO. Run **5-6 interleaved pairs** (lower half of the band is
inside the noise-suspect zone), cross-binary protocol with draws/flip
scene identity. Security posture: ROP-gadget hardening only; xemu
already runs a W^X JIT and is not a sandbox boundary — record the
trade in the ledger row.

### 3.2 Converge the shipped MoltenVK with the tested one (v1.4.2 final now exists)

Releases vendor **stock 1.4.1** (`build.sh:60`,
`XEMU_MOLTENVK_VERSION:-1.4.1`) while every local soak, A/B, and
gauntlet runs the custom pinned **1.4.2-rc1** (`build-moltenvk.sh:22`)
— 0% of validation runs the driver users load, and upstream v1.4.2
final has since shipped. The new enabler (final tag) is what
distinguishes this from the previously-settled release-parity policy
note in `docs/RELEASING-macos.md`. Move the vendored default and
`MVK_PIN` to v1.4.2, and turn `build.sh:196`'s provenance echo
("Bundling MoltenVK … version X") into a CI assertion. 0 fps
(fps-parity is the recorded expectation); the win is closing the exact
driver-drift channel the pink-tile class escaped through. Full
pin-bump gauntlet against the *vendored universal* build, not only the
custom arm64 one; `PREFILL` stays 0. Two of the five changelog fixes
the auditor cited don't apply (imported-MTLTexture paths, unused
here) — the coverage argument stands on the other three.

### 3.3 Pin an `-mcpu` distribution floor in CI

`build.sh:507-547` auto-detects `-mcpu=apple-mN` from the build host;
CI sets no `XEMU_ARM_CPU`. Verifier resolved the decisive unknown from
the actual CI log: today's releases build **apple-m1**, and the
apple-m1 vs apple-m2 codegen delta is nil (27/28 hot TUs
byte-identical; ~0.00 fps). So this is purely a guard: when GitHub
refreshes runner silicon, auto-detect silently raises the ISA baseline
above the advertised M1 floor — SIGILL-on-launch class breakage with
no CI signal (the arch 8.5 family). Set `XEMU_ARM_CPU=apple-m1` (or an
assert on the selected value) in the release legs; also closes the
shipped-vs-measured codegen gap as a declared quantity (dev benches are
apple-m2).

### 3.4 Gate the PGO build on profile staleness — but not on the naive diagnostic

`setup_pgo` (`build.sh:88-125`) never checks profile/source
consistency; the CI comment calls stale-profile degradation
"graceful", which is the silent failure. The profile itself is healthy
and workload-shaped (F8-class counts confirmed in
`pgo/default.profdata`). Verifier falsified the auditor's detector,
constructively: straight-line edits don't change the IR-PGO CFG hash —
**0 mismatches on today's tree** — so a bare `hash mismatch` grep of
`build.log` reads green precisely when the drift is small-but-real.
The workable gate: count mismatches weighted against the profile's
top-N hot functions (fatal for `cpu_exec_loop`-class names, warn
otherwise) *plus* a staleness age check (commits-since-retrain on hot
paths). Protects the recorded +9.4% PGO win, honestly discounted:
PGO reaches only the ~45% host-C share. Mind arch 8.6: the counting
pipeline must be pipefail-safe (awk-reads-everything idiom).

### 3.5 Key the SPIR-V cache on the shader compiler; give the bench harness its own base path

`glsl.c:30-47` keys the cache dir on xemu version only and the file on
a GLSL hash — compiler identity is absent, so the one substantive
pending upstream commit (`111685a2aa`, glslang 16.4.0) is **A/B-invalid
by construction** on any warm-cache machine (both arms serve the old
compiler's blobs; arch 9.1/9.5 family). Fold `GLSLANG_VERSION_*` (or
the wrap revision) into the key, and give the harness an isolated base
path so shader/pipeline caches become controlled variables like the
hdd already is. Verifier corrections: current dir is
`spirv_cache_v0.12.2` (not v0.11.1), released users mostly self-heal on
tag bumps (don't bundle a prune), and the cold-cost is amortized
(671 shaders × 2-10 ms, lazy). 0 fps; measurement-integrity fix that
unblocks the only upstream merge with behavioral content.

---

## 4. Record hygiene — decisions this audit supports making without code

- **Retire frontier 2.5 (DSP parmove+ALU fusion) as
  instruction-neutral by construction.** Confirmed by static count
  against the post-Phase-8 emitters: every write_d=0 source class
  (pinned X/Y = 1 UBFX, A/B = ~15-insn limited-read either way, LDR
  fallback = 1 LDR) saves **0 instructions** when folded across the
  ALU; even a 100%-eligibility counter would authorize a 0-insn win.
  Do not build the eligibility counter `docs/optimizations.md:1050`
  proposes. Honest narrower reopen condition: a non-audio DSP program
  whose parmove sources are neither pinned nor A/B.
- **Close arch 7.5 (lockless MMIO) with a written verdict instead of
  building PMC/PCRTC/PTIMER lockless.** Fixture-independent ceiling:
  the *entire* pre-campaign BQL-MMIO prize was 117k ops/s × 52 ns =
  0.61% of a core, and PFB/USER/PGRAPH/APU-VP already harvested most
  of it; the residual is ~+0.01-0.06 fps. PMC lockless without atomic
  conversion reintroduces the §C data race on the IRQ aggregate (hang
  class). One `XEMU_MMIO_PROF` run pins the residual number for the
  verdict page; reopen bar: BQL wait ≥ 500 ms / 75 s.
- **Fix architecture-contract Invariant 8 drift** (push-present has
  been the default since 2026-07-12; the skill says pull) and its
  stale line anchors. Fold in `config_spec.yml`'s dead
  `metalfx_upscale` key while touching the area (§2.1, §2.3).
- **The frontier ranked list itself is current.** A proposed re-rank
  (close 2/8, demote 1/4, promote 3) was refuted — every status change
  it proposed is already recorded in the digest/roadmap/ledger; the
  drift it saw was its own misreading of "landed-dark ≠ closed".

---

## 5. Examined and rejected — verified negatives from this audit

Recorded so the next audit doesn't re-derive them (one line each; the
full refutations live in the audit transcript).

| Idea | Why it died |
|---|---|
| Instrument the pushbuffer BEGIN/END squash to reopen draw merging | arch 1.14's reopen condition not satisfied; the squash's ring-occupancy gating was already distinguishable in A1's own data |
| Count surface access-callback invocations as a vCPU cliff | Watchpoint OR-in is real but the dirty bitmap doesn't subsume the callback contract; no unguarded cliff exists at measured rates |
| Paired 64-bit TB-validation compares (jump-cache + ret memo) | Same marginal-cost class as the 12→16-bit jump-cache and 13-bit ret-memo kills: a few fused µops on an OoO core, unmeasurable |
| Fold DSP per-op epilogue reloads at translate time | Mechanism sound but the container (49-insn fixed epilogue on a 20%-utilized off-critical-path thread) prices it at 0 fps — same kill reason as the fusion closure |
| Re-arm the DSP JIT auto-throttle latch | Governed by arch 4.6: the permanent latch *is* the fix; a re-arm reintroduces the churn oscillation it exists to prevent |
| Shrink `d->lock` hold across the APU frame | vCPU-side VP MMIO already lockless (`apu.c:428-448`); the residual writes are off the F8 critical path; voice_buf lifetime pins the current shape |
| Lock-free ring peek for the frame_seq dedup | +0.02 fps ceiling = the static-baseline resolution; unvalidatable under the evidence bar |
| CI assertion alone for MoltenVK parity (without the version bump) | Settled policy per `docs/RELEASING-macos.md`; superseded by §3.2's v1.4.2 convergence which carries the new enabler |
| Profile-derived `-Wl,-order_file` | ThinLTO+PGO already do the layout work; measured `__text` residency gap too small; ceiling under the noise line |
| Single-bit specialization of the dirty-bitmap primitives + dropping MIGRATION | Subsumed by §1.2's fused read; deleting the MIGRATION client diverges shared code for no additional win |
| Skip the vblank thread's VGA scanout emulation | The `gfx_update` path is what latches the raster/interrupt state the guest ISR reads; "no consumer" was false |
| Hull-reject the `mem_access_callback` list walk | List length is 1-8 in practice; the walk is 2 adds + 2 compares per node, already below measurement |
| Widen the guest-profiler RA cache | The 1.78% "phantom" was already fixed and shipped (ledger, Testing & tooling 2026-07-18); the profile lines cited predate it |
| Close the x86_64 `XEMU_SSE_HOST` arm as already-root-caused | The ±0-sign divergence is *documented*, not root-caused; ubuntu x86_64 CI runs the unit suite, not the in-game differential — the dark status stands |
| Frontier re-rank (close 2/8, demote 1/4, promote 3) | Every proposed status change already recorded; see §4 |

---

## 6. Suggested measurement order (cheapest decisive gate first)

Each step is independent; any can be skipped. Order minimizes cost to
kill or confirm, not implementation order.

1. `info jit` TLB-flush rate on the F8 savestate (zero code) — settles
   §1.3's steady-state question in ten minutes.
2. `cpu_io_recompile` counter + `XEMU_GUEST_PROF` re-run (20 lines) —
   gates §1.1, the largest candidate.
3. The x87/SSE Gate-0 census (§1.4, one counter patch serves all
   three sub-items).
4. `XEMU_INV_TIMING` before/after for §1.2(a) — the is_clean deletion
   is small enough to prototype same-day.
5. `-fzero-call-used-regs=skip` A/B, 5-6 pairs, cross-binary (§3.1).
6. `DEBUG_XEMU_C` BQL-occupancy read (§1.9) — expected kill, record it.
7. Movement-probe `pipeline_gen` with max-based gates (§2.4).
8. The zero-risk landings that need no bench: §3.3 CI `-mcpu` pin,
   §3.2 MoltenVK convergence + gauntlet, §1.6 counter gating
   (draws/s check), §4's three record closures.

## Provenance

Audit run 2026-08-04 on `macos-optimizations` @ `f4bb867d3c` (49
agents, ~5.5M tokens; transcript under the session's
`subagents/workflows/wf_be6e87bc-927/`). Baselines cited from
`docs/roadmap.md` (2026-07-19 state header),
`bench-out-profile-f8/ab_B1.log`/`ab_E1.log`, and the ledger. Every
`file:line` above was re-verified by an adversarial verifier against
this tree; line numbers drift — re-grep before acting. Benefit ranges
are pre-registered predictions in the §6 gates' sense, not results.
