# xemu-macos: Optimized Xbox Emulator for Apple Silicon

A personal fork of [xemu](https://github.com/xemu-project/xemu) with performance
work targeted at Apple Silicon (M1/M2+) running macOS 14+ (best on macOS 26 Tahoe).
Adds MoltenVK compatibility — which upstream does not officially support — plus
an inline ARM64 FPU, MetalFX upscaling, frame interpolation, and Accelerate-backed audio.

- Upstream: [xemu-project/xemu](https://github.com/xemu-project/xemu)
- MoltenVK compat seed: [CosmicSnow/xemu](https://github.com/CosmicSnow/xemu)

---

## Quick start

Prerequisites: macOS 14.0+ (macOS 26 for MetalFX and frame interpolation),
Apple Silicon, Xcode Command Line Tools, Homebrew `pkg-config`.

```bash
git clone --recurse-submodules https://github.com/MichaelJSr/xemu-macos.git
cd xemu-macos
git checkout macos-optimizations
git submodule update --init --recursive
./build.sh
open dist/xemu.app
```

Clean rebuild: `rm -rf macos-libs macos-pkgs build dist && ./build.sh`.

### Build knobs

| Env var | Default | Purpose |
|---|---|---|
| `XEMU_ARM_CPU` | auto | Override `-mcpu=`; auto-picks `apple-mN` from `sysctl machdep.cpu.brand_string` with progressive fallback to older chips clang recognizes |
| `XEMU_PGO` / `XEMU_PGO_DIR` | unset / `./pgo` | `generate` then `use` for profile-guided optimization |
| `XEMU_CODESIGN_ENTITLEMENTS` | `0` | Opt into hardened-runtime codesign with `xemu.entitlements` (only for local notarization-style testing) |
| `XEMU_COREAUDIO_FRAMES` | `1024` | CoreAudio buffer size (≈21 ms @ 48 kHz) |
| `XEMU_PFIFO_HEARTBEAT` | `0` | 2-second pfifo-thread diagnostic snapshot (`halt` / `flush_pending` / `sync_pending` / `waiting_for_*`); used to categorize "frozen but UI responsive" reports |

### Recommended `xemu.toml`

`~/Library/Application Support/xemu/xemu/xemu.toml`:

```toml
[display]
renderer = 'VULKAN'
metalfx_mode = 'spatial'        # off | spatial | temporal
frame_interpolation = '4x'      # off | 2x | 4x

[display.quality]
surface_scale = 2               # 1 = 640x480 ... 4 = 2560x1920 (MetalFX skipped)

[display.window]
fullscreen_on_startup = true
fullscreen_exclusive = true
startup_size = '1920x1080'

[display.ui]
fit = 'stretch'

[perf]
hard_fpu = true                 # ARM64 inline x87; disable only for rare precision bugs
cache_shaders = true
```

The in-app Settings UI exposes the main MetalFX / interpolation / scale /
hard-FPU knobs; TOML is only needed for fine tuning.

---

## What this fork changes

### CPU / JIT (ARM64)

- **Inline hard FPU.** x87 ops emit native AArch64 FP instructions
  (`FADD`/`FMUL`/`FDIV`/`FSQRT` etc.). `floatx80`↔`double` conversion is
  inlined (~15 host insns vs ~40 in the helper). Values stay in D-regs
  across translation blocks. `fucomi`/`fnstsw`/`frndint`/FPU constant
  loads and `fdecstp`/`fincstp` are also inline. ~30% faster than the
  helper-call path, 4–8× faster than softfloat.
- **FPCR caching.** `MSR FPCR` only re-emitted when guest RC bits change
  (invalidated on save/load and `cpu_set_fpuc`). FPCR `DN=0` so NaN
  propagation matches x87.
- **Rounding-mode-correct FIST.** `FIST`/`FISTP` honor the current
  control word via mode-specific fused TCG ops
  (`cvt_{rn,rm,rp}_i{32,64}_f{32,64}`) that lower to a single host
  `FCVT{N,M,P,Z}S` on AArch64.
- **JIT tightening.** `ld80f` NOP-copy removed; `gen_stn_ptr` uses
  shift-by-4; `insertion_sort_syncs` replaces `qsort` for N ≤ 16;
  `flcr` is 5 insns via `RBIT`. `gen_flcr` materializes rc-bits
  from `tb->flags`' `HF_FPU_RC` instead of re-loading `env->fpuc`;
  `fnstcw` inlines to a single `ld16u_i32`.
- **Snapshot/migration FPU resync.** `xsave_helper.c` and
  `cpu_post_load` route through `cpu_set_fpuc` so `HF_FPU_RC` in
  `hflags` is rebuilt; otherwise the fused FIST `cvt_{rn,rm,rp}` ops
  would round with the pre-restore mode.
- **FCOMI vs FUCOMI signaling split.** New `coms_f32`/`coms_f64` TCG
  ops lower to `FCMPE` on AArch64 and `COMISS`/`COMISD` on x86;
  existing `com_f32`/`com_f64` stay quiet (`FCMP` / `UCOMISS`).
  Matches x87 semantics (FCOMI raises IE on any NaN; FUCOMI only
  on SNaN).

### Vulkan renderer (pgraph/vk)

- **MoltenVK compatibility.** `VK_KHR_portability_subset`,
  `VK_EXT_metal_objects` zero-copy IOSurfaces. CPU-side primitive
  emulation for quads, line loops, triangle fans, provoking vertex.
  Fragment-shader depth fallback via `gl_FragCoord.z + dFdx/dFdy`.
- **Split descriptor sets.** Set 0 UBOs, set 1 the `NV2A_MAX_TEXTURES`
  combined image samplers. Per-set pool/array/index with last-bound
  tracking — a draw that only changes textures doesn't rewrite UBOs
  and vice versa. Roughly halves `vkUpdateDescriptorSets` work.
- **Single-submit fast path + N=2 flight slots.** CPU records slot 1
  while GPU executes slot 0. Aux command buffer skipped when staging
  is empty and no VRAM page is dirty.
- **Texture upload batching + bump allocator** on `BUFFER_STAGING_SRC`.
  Multiple textures share one main-CB submission.
- **Parallel + GPU texture decode.** `GThreadPool` capped at
  `MIN(num_processors, 4)` for S3TC / unswizzle / conversion (mips
  ≤ 16×16 inline). Non-compressed 2bpp/4bpp textures bypass the CPU
  pool — the raw swizzled data stages to a buffer and a compute
  shader unswizzles it with CPU-computed `mask_x`/`mask_y` push
  constants for any power-of-two dimensions.
- **Dirty tracking.** UBO `uniform_copy` does `memcmp` before
  `memcpy` with per-layout dirty flag. Pipeline key re-hashes only
  when `check_pipeline_dirty` fires, hashing only the non-shader
  slice (`ShaderState` sub-hash cached). Vertex layout fingerprinted
  via `fast_hash`. Surface expiry throttled to ~33 ms host wall-clock
  (not guest `frame_time` — see failed experiments).
- **Incremental texture content hash.** Page-aligned textures
  ≥ 256 KiB keep a per-chunk XXH3 array (64 KiB chunks). Only chunks
  whose backing pages changed get re-hashed; aggregate is the XOR
  plus cached palette hash. Saves ~30–60% hash CPU for large atlases
  with sparse updates. Small / non-aligned textures fall back to the
  single-shot full-buffer hash.
- **Tight barriers.** `ALL_COMMANDS_BIT` replaced with precise stage
  flags; `VK_WHOLE_SIZE` with exact byte ranges.
  `vmaFlush`/`vmaInvalidate` skipped on coherent memory (Apple
  Silicon unified memory; per-buffer `is_coherent` flag). Vertex-RAM
  flush scoped to the tracked `[first, last]` dirty-page range.
- **Primitive restart.** `primitiveRestartEnable = VK_TRUE` for
  strip / fan topologies so NV2A restart-index draws render correctly.
- **Display path.** PVIDEO + display merged into one GPU submit.
  GL rebind cache keyed on `IOSurfaceGetID`. Pending reports /
  compute descriptor reset consolidated in `pgraph_vk_finish`.
- **Dynamic blend / depth bias.** `BLENDCOLOR` / `ZOFFSETBIAS` /
  `ZOFFSETFACTOR` removed from the pipeline key and set as Vulkan
  dynamic state per draw (skipped on clear pipelines).
- **CONTROL_3 off the pipeline key.** `NV_PGRAPH_CONTROL_3` dropped
  from `PipelineKey.regs[]`; its bits (`SHADEMODE`, `FOG_MODE`,
  `FOGENABLE`, `POINTPARAMSENABLE`) already route through
  `ShaderState` and `PROVOKING_VERTEX` is CPU-side. Eliminates
  false-positive pipeline rehashes on fog/shade toggles.
- **Dynamic-state cache.** `vkCmdSet{Viewport,Scissor,LineWidth,
  DepthBias,BlendConstants}` only re-emitted when the value
  changes; cache invalidated per `pgraph_vk_begin_command_buffer`.
- **Coalesced descriptor writes.** Texture bindings 0..3 and
  VSH/PSH UBO bindings 0..1 write as one `VkWriteDescriptorSet`
  with `descriptorCount=N` (spec §14.2.3 overflow) instead of N
  separate structs.
- **Renderer-switch hardening.** `pgraph_process_pending` uses
  `qatomic_set` on `flush_pending`, drops `pgraph.lock` before
  acquiring `pfifo.lock` (fixes AB-BA inversion against
  `pgraph_write`), and doesn't hold `pgraph.lock` across the
  `framebuffer_released` cond_wait.
- **Fence-wait diagnostics.** `pgraph_vk_end_single_time_commands`
  / `pgraph_vk_wait_for_previous_flight` use a 5 s timeout wrapper
  that aborts with the named call site instead of hanging silently
  on MoltenVK internal-mutex deadlocks.
- **Monotonic sync clock.** `pgraph_vk_sync` uses `QEMU_CLOCK_HOST`
  instead of `QEMU_CLOCK_REALTIME` so NTP slew / suspend-resume
  don't stall the 8 ms gate.

### MetalFX + presentation

- **Spatial / temporal upscaling** via `MTLFXSpatialScaler` /
  `MTLFXTemporalScaler`. Temporal uses 8-frame Halton(2,3) jitter
  and optional compute-kernel synthetic depth. Shared `MTLDevice` +
  `MTLCommandQueue` refcounted. MRC lifetime: every `new*` /
  `alloc+init` has an explicit `release`.
- **Frame interpolation** via `MTLFXFrameInterpolator` (macOS 26+).
  2× = 60 fps, 4× = 120 fps from a 30 fps source, with deferred
  generation on idle display syncs. On-failure slot is skipped.
- **Teardown safety.** Every encode path attaches a completion handler
  that decrements a per-subsystem in-flight counter; destroy/reinit
  drains the counter before releasing scalers/textures.
- **IOSurface lifetime.** `IOSurfaceGetID` is the cache key everywhere
  (Metal + GL), `CFRetain`/`CFRelease` balanced. Output width capped
  at 1920 to work around a macOS 26 BGRA `bytesPerRow` bug.

### MCPX APU

- **VP voice processing.** Voice struct (128 B) memcpy'd to stack at
  the top of `voice_process`; all `voice_get/set_mask` calls hit the
  local copy. Attenuation (4096), pitch (65536), LPF cutoff (65536),
  and envelope decay/release (1024 each, linear interp) LUTs replace
  transcendentals on the hot path.
- **Resampler.** `SRC_LINEAR` replaces `SRC_SINC_FASTEST` (matches
  Xbox hardware).
- **Accelerate / vDSP.** `vDSP_vsma` for 8-bin × 32-sample mix
  accumulation; `vDSP_vadd` for `float_accumulate`; `float_to_24b`
  uses `vcvtnq_s32_f32` in bulk on ARM64.
- **Single-precision SVF/LPF.** `setup_svf` uses `sqrtf` + float
  literals; per-sample clamp uses `fminf`/`fmaxf` + `1.0f`/`-1.0f`
  to eliminate float→double→float round-trips.
- **DSP dispatch.** `pram_opcache[pc]` caches the resolved
  `emu_func_t` directly instead of a `const OpcodeEntry *`, saving a
  dependent load and a NULL-branch per DSP instruction. `emu_undefined`
  is cached for opcodes without a dedicated handler.
- **DSP JIT (opt-in).** ARM64 basic-block JIT for the MCPX APU
  DSP56300 cores (both GP and EP). Off by default; enable with
  `XEMU_DSP_JIT=1`. Each DSP basic block is translated once into a
  short sequence of ARM64 stubs that inline the per-instruction
  bookkeeping (`cur_inst`, `cur_inst_len`, `instr_cycle` + cycle
  accumulation) and BLR the existing C `emu_*()` handlers; the
  handler bodies are unchanged so correctness is equivalent to the
  interpreter by construction. Inline fast-paths skip the calls to
  `dsp_postexecute_update_pc` (when no REP / DO loop active) and
  `dsp_postexecute_interrupts` (when no interrupt state is active
  — the steady-state case). Helper addresses are hoisted into
  block-scope callee-saved x20/x21 so each stub is just BLR Xn.
  A shared epilogue absorbs the per-op exit checks (pending IRQ,
  REP entry, is-idle, PC mismatch). Invalidation on P-space writes
  keeps the block cache coherent with self-modifying DSP code; the
  cache is flushed entirely on `dsp_reset` and `dsp_bootstrap`.
  Phase 4 adds native translation for all 16 parallel-move select
  values (pm_0 / pm_1 / pm_2 / pm_3 / pm_4 / pm_5 / pm_8) —
  previously the `inst >= 0x100000` parmove-bearing instructions
  fell back entirely to the interpreter, costing a per-op C round
  trip on ~55% of dynamic instructions in audio kernels. Inline
  calc_ea handles the 5 linear Rn addressing modes; inline
  xram/yram linear memory access bypasses the mixbuffer /
  peripheral / reverse-carry helpers for `addr < 0xc00`. Phase 2
  inlines the ALU kernel itself: ~170 of 256 opcodes — every
  ADD/SUB/CMP/CMPM/TST/CLR/NEG/ABS/AND/OR/EOR/NOT/TFR variant plus
  all 128 MPY/MPYR/MAC/MACR multiplier ops — translate to inline
  ARM64 that collapses the interpreter's 3-word add-with-carry
  dance into a single 64-bit ADD and `dsp_mul56` into one SMULL +
  LSL #1. The rare tail (RND/ROL/ROR/ADDL/SUBL/ADDR/SUBR/ADC/SBC/MAX,
  plus shifts) stays as BLR fallback. Phase 5a (round 2) inlines the
  full control-flow handler matrix — immediate / `_long` / `_ea` /
  `_aa` / `_pp` / `_reg` variants of JMP, JSR, RTS, RTI, BRA, BSR,
  JCC, JSCC, BCC, plus the bit-test families (JCLR, JSET, JSCLR,
  JSSET × AA/EA/PP/REG and BRCLR, BRSET × PP/REG). The 16-case
  `emu_calc_cc` switch is fully inlined too: at translate time the
  cc_code is known, so each condition (CC, GE, NE, PL, NN, EC, LC,
  GT plus their 8 inverses) emits its own 2–7-insn bitfield extract
  directly into the stub — no BLR, no shim. Bit-test memory reads
  route through the existing `emit_mem_read_xy` helper so linear
  xram/yram addresses stay inline; peripheral / out-of-range paths
  still BLR `dsp56k_read_memory`. `_ea` variants that go through
  calc_ea's mode 5/6/7 slow path correctly preserve calc_ea's side
  effects on `instr_cycle` and `cur_inst_len` via additive (not
  SET) cycle/len updates. Phase 6 inlines the `_imm` forms of
  REP / DO / DOR / ENDDO — loop-body iteration still routes through
  `dsp_postexecute_update_pc` in the epilogue, which already handles
  the "stay on this instruction" / "end-of-loop pop" edges. The
  differential validator's per-block compare is narrowed for
  inlined CF ops via a zero write-set (all their effects land in
  always-compared `dsp_state_diff` ranges), dropping the typical
  compare from ~40 KB to ~2 KB — matching what parmove stubs
  already achieve. `XEMU_DSP_JIT_STATS=1` prints per-run
  `alu_inlined / alu_fallback` and `cf_inlined / cf_fallback`
  counts plus percentages, printed from an `atexit` hook so the
  normal xemu quit path (Cmd-Q / SIGTERM — xemu shutdown doesn't
  call `dsp_destroy`) still dumps them. Round 3 extends the
  inline coverage further: (a) `emu_calc_ea` modes 5 (Rn+Nn) and
  7 (-(Rn)) are inline under linear Mn (still slow-call for
  modulo Mn); mode 6 (aa) is deferred pending a pc-threading
  refactor. (b) The four ALU shift kinds (ASL/ASR/LSL/LSR) are
  inline — the 56-bit ASL/ASR via packed-accu ops, the A1-only
  LSL/LSR via direct-register ops, all four matching the
  interpreter's dsp_asl56 / dsp_asr56 / emu_lsl_a / emu_lsr_a
  flag semantics bit-for-bit (which includes dsp_asr56's LOGICAL
  right shift despite the "ASR" mnemonic). (c) The 2-word ALU
  long-immediate handlers (`add_long`, `sub_long`, `cmp_long`,
  `and_long`, `or_long`) have their 24-bit immediate baked from
  `pram[pc+1]` at translate time, eliminating both the BLR and
  the `read_memory_p` from every long-imm ALU instruction. Round
  4 lands two epilogue cost-reductions on every translated op:
  (1) the `loop_rep / is_idle / jit_exit_block_request` trio of
  exit checks, previously three separate `LDR+CBNZ` pairs, is
  fused into one `OR`-then-`CBNZ`, saving two insns + two
  exit-patch slots per op with identical semantics (any non-zero
  still triggers the exit). (2) the PC-mismatch check uses a
  `SUB w0, w0, #imm12 + CBNZ` fast path when `expected_next_pc`
  fits in ARM64's imm12 (which it almost always does since DSP
  PRAM is 4 KiB), dropping it from 3 to 2 insns. Also cleans up
  the dead `dsp_jit_helper_calc_cc` shim that Round 2 made
  unreachable.   Two other round-4 experiments (skipping the
  `dsp->cur_inst` preset for inlined ops, and skipping the
  PC-mismatch check for inlined long-imm + parmove stubs via
  `EPI_NO_PC`) regressed with an `op=0x001000` startup assert
  — at least one inlined path reads `cur_inst` or mutates `pc`
  in a way the static classifier can't predict; both are
  deferred pending the runtime-sentinel bisect harness. That
  harness is available via `XEMU_DSP_JIT_SENTINEL=N`:
    * bit 0 (1) — re-applies the cur_inst skip with a poison
      value `0x00adbeef` so any stale-read path surfaces as
      `lookup_opcode_slow(op = 00adbeef)` with a stack trace
      identifying the reader;
    * bit 1 (2) — keeps the PC-mismatch exit in place but also
      logs every mismatch for parmove-stub + inlined-long-imm
      ops (the round-4 skip candidates), giving a histogram of
      the inst patterns whose handlers actually mutate `pc`
      despite the classifier believing they don't. Logger is
      rate-limited per-inst and no-ops on the matching path.
  Both bits can be combined (`=3`). Not for production — the
  extra BLR to the logger on sentinel-watched ops costs ~5
  cycles / op. Static block
  chaining (direct `B <target.entry>` patch on known branch
  targets) is a followup commit — the entry-split prologue
  refactor it depends on is scoped separately.
  Correctness harness: `XEMU_DSP_JIT_DIFF=N` validates JIT blocks
  against the interpreter. Because a translated block is fully
  deterministic given its pre-state, a single passing validation
  proves correctness for every future execution of that
  translation — each `DspJitBlock` carries a `diff_checked` bit
  set on first enqueue, so the diff path skips validated blocks
  entirely. Total validator work bounded to ~(number of unique
  block translations) — hundreds to a few thousand per session
  rather than millions-per-second. Recommended day-to-day run is
  `XEMU_DSP_JIT_DIFF=10` (sample every 10th unchecked block,
  spreading validation bursts over wall time); `=1` validates
  every unique translation. The validator itself runs **off the
  APU thread** on a dedicated worker (`mcpx.dsp_diff`): the APU
  thread snapshots pre- / post-state into a 16-slot SPSC ring and
  publishes, and the worker replays the interpreter on a private
  copy (callbacks swapped to shims so `container_of` can't reach
  into a bogus `DSPState`) and byte-compares. APU-thread cost per
  unchecked block is ~2 memcpys (~160 KB); `d->lock` is never
  held across the interpreter replay, so the main thread is never
  starved during diff runs. Ring-full producer drops the new
  entry (validation degrades gracefully to a sampler rather than
  stalling). The compare is narrowed per-block via a
  translation-time write-set bitmask — FIR/IIR kernel blocks
  (register-only computation) byte-compare ~2 KB instead of ~40
  KB, a 20× speedup on the compare path. `XEMU_DSP_JIT_DIFF_SYNC=1`
  runs the validator inline on the APU thread for cases where the
  abort must fire immediately (bring-up debugging); `XEMU_DSP_JIT_DIFF_MAX=N`
  caps total validations (useful for CI gating). Passed 300+
  stress-runs of the built-in test across interp / JIT / JIT+DIFF
  (sync+async, `DIFF=1/10/100`) modes. See
  [docs/dsp-jit-design.md](docs/dsp-jit-design.md) for the 9-phase
  roadmap and register-map plans for later inline phases
  (arithmetic, control-flow chaining, lazy flags).
- **Atomic consistency.** All `d->regs[]` writers in `fe_method`
  (including a single `qatomic_set` FECTL mask+set under
  `d->lock`), `voice_locked[]` via `qatomic_or`/`qatomic_and`,
  `pause_requested`, and the `NV_PAPU_FEMEMADDR` load go through
  `qatomic_*` so the VP frame thread, workers, and lock-free
  MMIO dispatcher agree under weak ordering.
- **`voice_set_mask` fast-skip.** Early-out when the new value
  equals the old; skips the stack-buf and guest-RAM stores that
  voice-tick paths commonly hit on unchanged envelope/pause
  fields.
- **`dsp_dma_run` scratch-buffer leak fix.** `malloc` →
  `g_realloc`.
- **CoreAudio.** `os_unfair_lock` with trylock; default buffer 1024
  frames (~21 ms @ 48 kHz, override via `XEMU_COREAUDIO_FRAMES`).
  Silence on contention; underrun returns `BadDevice` / `Unknown`
  correctly instead of mis-reporting success.

### Threads + runtime

- **P-core QoS.** PFIFO and vblank-timer threads request
  `QOS_CLASS_USER_INTERACTIVE` on Apple Silicon.
- **BQL discipline.** `surface_access_callback` conditionally drops
  BQL before blocking so the vblank thread can still fire interrupts.
- **PFIFO wait.** Untimed `qemu_cond_wait`. All `pfifo_kick` sites
  hold `d->pfifo.lock` across the kick-set + broadcast, so the
  reader's kick-check + atomic release-wait can't miss a concurrent
  kick. Removes the ~1 kHz idle wake-up of the previous 1 ms
  `timedwait`. `halt` is now read via `qatomic_read` on the main
  pfifo thread loop to match the writer contract.

### Build + packaging

- `-O3`, thin LTO with caching, `-ffp-contract=fast`. `-mcpu`
  auto-detected with progressive fallback.
- macOS 26 build fixes: `download-macos-libs.py` uses `os.path.abspath`
  and repairs stale `prefix=` lines in vendored `.pc` files;
  `build.sh` strips all `LC_RPATH` before `dylibbundler` runs and
  adds the single correct one. Local codesign stays ad-hoc without
  hardened runtime (MAP_JIT works); set `XEMU_CODESIGN_ENTITLEMENTS=1`
  to opt into hardened runtime + `xemu.entitlements` (for notarized
  distribution use `scripts/sign-macos-release.sh`).
- Optional `XEMU_PGO=generate` then `XEMU_PGO=use` build loop using
  `llvm-profdata` merge.
- STBI_NEON + fpng CRC32 for ARM64.

### MoltenVK runtime config (`Info.plist` `LSEnvironment`)

| Key | Value | Purpose |
|---|---|---|
| `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS` | `1` | Reduce descriptor binding overhead |
| `MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS` | `2` | Prefill at CB end |
| `MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS` | `0` | Async submits |
| `MVK_CONFIG_FAST_MATH_ENABLED` | `1` | Metal shader fast-math |
| `MVK_CONFIG_RESUME_LOST_DEVICE` | `1` | Ignore transient GPU errors |

---

## Failed / reverted experiments

Lessons worth preserving so they aren't re-attempted.

| Attempt | Reason |
|---|---|
| `floatx80` union overlay on ARM64 | Layout incompatible with IEEE 64-bit — segfaults |
| Voice register `__thread` cache | Stale data; Xbox HW mutates voice regs via DMA |
| Async MetalFX (all variants: sem-primed / double-buffered IOSurface / `dispatch_semaphore`) | GL ↔ Metal cross-API sync can't be expressed with `SDL_GL_SwapWindow` + vsync alone; needs `MTLSharedEvent` + `glWaitSync` or triple-buffering. Sync MetalFX is the baseline until the display path goes Metal-native |
| Separate compute queue | MoltenVK only exposes `queueCount=1` |
| Depth export (CPU readback or `vkExportMetalObjectsEXT`) | CPU: 2 ms/frame + 18 MiB of copies for minimal quality. Metal export: deadlocks MoltenVK's internal device mutex |
| Direct-VRAM compute unswizzle via `VK_EXT_external_memory_host` | `HOST_WRITE → SHADER_READ` barriers enforce visibility up to *submission*, not GPU execution; on HOST_COHERENT memory the CPU can tear the read mid-frame → particle/HUD flicker. Staging-copy path is kept |
| Host-imported `BUFFER_VERTEX_RAM` (α2) | Same torn-read class for vertex data: GPU fetches can see two different CPU frames mid-primitive on dynamic meshes. Needs snapshot-COW or `MTLSharedEvent` to re-enable |
| `pthread_jit_write_with_callback_np` for `tb_phys_invalidate` (A3) | Scoped-W semantics force-drop W on callback return, incompatible with QEMU's nested JIT-write paths. Restored the manual `qemu_thread_jit_write` / `execute` pair |
| `VK_KHR_dynamic_rendering` full lowering (B2) | Missing the render pass's `VK_SUBPASS_EXTERNAL → first subpass` dependency; back-to-back passes stalled Metal's tile renderer. Re-enable needs explicit `vkCmdPipelineBarrier` around every Begin/End |
| APU voice resampler `rate == 1.0` fast path | Skipped libsamplerate but didn't replicate its silence-padding on voice drain → `voice_process`'s outer loop spun on draining voices during scene transitions |
| `HLT` BSOD recovery (IF=1 force at-HLT and post-halt) | Unblocks Xbox-kernel `CLI; HLT` deadlocks, but the IRQ-loop pattern is a *symptom* of a stuck thread elsewhere, not the cause. Removed |
| Surface-expiry throttle keyed on `pg->frame_time` | `frame_time` only advances in `NV097_FLIP_INCREMENT_WRITE`, so rapid level-load bursts with no flips starved the prune and `r->invalid_surfaces` grew unbounded → black-screen freeze on rapid double-level-load + die. Switched to host wall-clock (~33 ms) |
| Surface-range binary search on `end` | `end` isn't monotonic in a start-sorted array → skipped valid overlaps |
| Two-level quick texture hash / hash-skip for > 64 KiB | 192-byte sampling missed content changes (YUV / FMV); dirty-bitmap false positives caused massive GPU re-upload churn |
| Dirty-range VRAM flush (early) | Bitmap cleared before `flush_memory_buffer` read it. Tracked per-flight `[first, last]` range does the safe equivalent |
| Incremental texture hash on misaligned textures | Host-page boundaries straddle chunks → `test_and_clear_dirty` by one chunk steals another's dirty signal. Gated to page-aligned textures only |
| HRTF hand-NEON / always-on bounds checks | Gather-then-FMA on a circular buffer defeated OoO overlap; `-O3 -mcpu=native` autovectorizes better. Always-on bounds: ~5–10% GPU-pipeline regression |
| BQL event batching | BQL around the SDL event loop breaks QEMU cooperative scheduling |
| TLS-cached `pthread_jit_write_protect_np` | Per-thread enum shadow (`UNINITIALIZED`/`EXECUTE`/`WRITABLE`) to elide no-op syscalls on `cpu_tb_exec`. SIGSEGV/SIGILL after ~1 min of gameplay: the actual per-thread kernel W^X state drifts from our cache (nested `tb_gen_code` → `tb_phys_invalidate` paths or setjmp/longjmp out of codegen), leaving TBs written to execute-only pages or executed from writable pages. The unconditional syscall pattern is defensive against this drift and the perf delta isn't worth the correctness risk |
| Hoist `can_fifo_access` out of pfifo pusher word loop | Checked `NV_PGRAPH_FIFO_ACCESS` once on entry to `pfifo_run_pusher`, rechecked only `waiting_for_nop` per word. The invariant "pfifo.lock held throughout" is false: `pfifo_run_puller` drops `pfifo.lock` when taking `pgraph.lock`, which creates a window where state the pusher assumed stable can change. On game boot this surfaced as `ERROR_CALL` (nested CALL) from a `subroutine_state` / pushbuffer mismatch. Reverted to per-iteration full check |

---

## Future vectors

Explored but not yet attempted, or punted on risk. Each entry either
cites the reverted-experiment entry it would need to sidestep, or
describes the blocking infrastructure work.

- **MCPX APU DSP JIT static block chaining (Phases 5b/5c) + Phase 8.**
  Phases 0, 1, 2, 3 (partial, via 4), 4, 5a (round 2 — full
  variant matrix + inline calc_cc + tight write-sets), 6, and 7
  of the roadmap in [docs/dsp-jit-design.md](docs/dsp-jit-design.md)
  have landed: infrastructure, call-threaded translator, inline
  ALU + MAC + TFR kernels (~170 of 256 opcodes), all 16 parallel-
  move variants, inline calc_ea (linear Rn modes 0-4), inline
  xram/yram linear memory, inline control-flow across the full
  immediate / `_long` / `_ea` / `_aa` / `_pp` / `_reg` matrix
  (~30 handlers including JCLR/JSET/JSCLR/JSSET/BRCLR/BRSET bit-
  tests and all conditional variants with fully-inlined calc_cc),
  inline loop handlers (REP/DO/DOR/ENDDO `_imm`), inline post-op
  fast paths for `postexecute_update_pc` and
  `postexecute_interrupts`, block-scope cached helper addresses,
  XEMU_DSP_JIT_DIFF bit-exact validation. After Phase 5a round 2
  the only CF fallbacks are the bsr-Rn / bscc / punlock / pflush
  tail that no interpreter handler ever implemented in the first
  place (the opcode table entries are NULL). Remaining phases:
  **5b / 5c** — static block chaining (direct `B <target.entry>`
  patch on known branch targets) needs an entry-split prologue
  refactor so that chained entries skip the frame-save (otherwise
  chained loops grow the stack unboundedly per iteration);
  **8** — lazy flag evaluation (cc_op shadow), ARM64 register
  pinning for A/B/X0/X1/Y0/Y1, parmove+ALU fusion. Target
  aggregate once 5b+5c+8 land: 2-4× DSP throughput vs the
  baseline Phase 1 call-threaded translator.
- **Metal-native presentation.** Replace SDL3 + OpenGL +
  `CGLTexImageIOSurface2D` with `CAMetalLayer` direct drawable
  acquisition. Would eliminate the GL↔Metal bridge that currently
  blocks async MetalFX (see reverted async-MetalFX entry).
- **Async MetalFX via `MTLSharedEvent` + `glWaitSync`.** Revisit now
  that the cross-API fence failure mode is understood. Needs either
  a shared event or a triple-buffered IOSurface ring with explicit
  CPU-side fences.
- **`VK_EXT_external_memory_host` with snapshot scheme.** The α2
  failed-experiment entry explains why live VRAM import tears; a
  per-flight copy-on-write snapshot or `MTLSharedEvent`-keyed
  boundary would let the optimization land.
- **`VK_KHR_dynamic_rendering` with explicit barriers.** The B2
  failed-experiment entry identifies the missing
  `VK_SUBPASS_EXTERNAL → first-subpass` equivalent. Re-enable by
  emitting `vkCmdPipelineBarrier(COLOR_ATTACHMENT_OUTPUT +
  EARLY/LATE_FRAGMENT_TESTS, COLOR/DEPTH_STENCIL
  ATTACHMENT_READ|WRITE)` around every `BeginRendering` /
  `EndRendering`.
- **`MTLResidencySet` (macOS 15+).** Pin frequently-used Vulkan
  resources' underlying Metal buffers/textures as resident so the OS
  doesn't page them out under memory pressure.
- **Push-constant uniforms for small UBOs.** VSH/PSH uniform blocks
  under 128 bytes could migrate to push constants, eliminating a
  descriptor update + bind per draw.
- **BINK video via VideoToolbox.** The Xbox BINK decoder path is
  CPU-bound. Pre-decode via VideoToolbox where BINK-to-H.264/HEVC
  is feasible, or at least offload YUV → RGBA conversion.
- **Shader specialization constants.** Burn fragment-shader
  conditionals (alpha test, fog enable) into constants at pipeline-
  compile time instead of runtime-evaluating them in shader;
  reduces uniform bandwidth.
- **GPU-based S3TC decode.** Currently S3TC runs on the CPU thread
  pool. A compute shader decoder would offload this and parallelize
  across many blocks.
- **Aux-submit pipelining.** `pgraph_vk_end_single_time_commands`
  currently waits fully. Classify callers by whether they need
  GPU-visible completion vs just ordered commit, and chain the
  latter via semaphores into the next main submit. Flight-slot
  reclaim becomes the shared sync point.
- **Query-pool drain at slot reclaim.** `vkGetQueryPoolResults` with
  `WAIT_BIT` stalls per-submit; move the drain into the flight
  slot's fence wait to amortize with the existing sync.
- **`pgraph_vk_upload_surface_data` flush narrowing.** Currently
  calls `pgraph_vk_finish` unconditionally. Safe to skip when the
  target surface isn't bound for the open render pass.
- **Indexed-draw attr-remap narrowing.**
  `copy_remapped_attributes_to_inline_buffer` copies `[0..max_element]`
  — narrowing to `[min_element..max_element]` cuts over-copy cost on
  indexed draws with large shared VBOs.
- **LRU eviction fast path.** `lru_try_evict_one` walks the tail
  linearly when many entries are in-use; an auxiliary evictable
  queue or per-slot bitmask would collapse this.
- **`MTLFXFrameInterpolator` `deltaTime` in seconds.** Currently
  fed a frame-sequence ratio; feeding wall-clock seconds (with
  matching `motionVectorScale{X,Y}`) should improve temporal IQ.
- **Voice-register writeback batching.** Defer `ram_stl` writes in
  `voice_set_mask` to a single `memcpy` at the end of `voice_process`.
  Behavioral risk: titles that read voice regs mid-frame via MMIO
  would see one-frame-stale data.
- **`qemu_cpu_kick` via `dispatch_semaphore_t` on Apple Silicon.**
  The current `pthread_kill(SIGUSR1)` path has ~50 µs P99 jitter on
  macOS 26. A dispatch semaphore per vCPU would remove signal
  delivery from the critical path.
- **Vblank cadence aligned to host refresh.** `ui/xemu.c` uses a
  hardcoded 60 Hz `vblank_interval_ns`. On 120 Hz ProMotion panels,
  aligning to `SDL_GetDisplayMode` refresh avoids micro-judder,
  especially under MetalFX temporal + frame interpolation.
- **TCG AArch64 3-MOVK constant materialization.** Three-halfword
  constants currently fall through to the literal pool. A 3-insn
  `MOVZ` + `MOVK` + `MOVK` fast path could beat the pool LDR
  latency on Apple chips.
- **Cross-TB FPCR elision.** `cached_fpuc_rc` lives in memory and
  is reloaded once per FPU-using TB. A TCG global register could
  carry the cached value across chained TBs and turn the gate into
  a pure register compare.

---

## Troubleshooting

**Build fails with `sizeof(size_t) doesn't match GLIB_SIZEOF_SIZE_T`.**
Stale `pkg-config` paths. `rm -rf macos-libs macos-pkgs build && ./build.sh`.

**Crash on launch with "duplicate LC_RPATH".**
Older builds only. Rebuild from scratch; `build.sh` now strips rpaths.

**MetalFX not activating.**

- Needs `renderer = 'VULKAN'` and `metalfx_mode = 'spatial'` or `'temporal'`.
- `surface_scale = 4` produces 2560×1920 input which exceeds the 1920 px
  BGRA-IOSurface safe cap, so MetalFX is skipped.
- Look for `MetalFX: Spatial upscaler initialized …` in the log.

**Frame interpolation not working.**
Requires macOS 26.0+ and MetalFX upscaling active (width ≤ 1920).

**FPU precision bug in a game.**
Disable the inline path: `[perf] hard_fpu = false`. The inline FPU
uses IEEE double (52-bit mantissa) vs x87 extended precision (64-bit);
extremely rare in practice.

**Audio glitches / too much latency.**
`XEMU_COREAUDIO_FRAMES=2048 ./build.sh` for a larger buffer.

---

## Architecture at a glance

```
Xbox game (30fps, x87 FPU, MCPX APU)
       │
       ├─ CPU: ARM64 TCG with inline x87 → AArch64 FP
       │  floatx80↔double inline JIT · FPCR cache · D-regs across TBs
       │
       ├─ GPU: NV2A Vulkan renderer (MoltenVK on macOS)
       │  flight slots · bump-alloc staging · dirty-tracked pipelines
       │  compute unswizzle + YUV · IOSurface zero-copy
       │        │
       │        └─ MetalFX Spatial / Temporal → 1280×960→1920×1440
       │             │
       │             └─ MTLFXFrameInterpolator (30→60 or 30→120)
       │                  │
       │                  └─ CGLTexImageIOSurface2D → SDL3 window
       │
       └─ APU: VP (per-voice pitch/vol) + DSP (scripted M56001)
           linear resampler · vDSP_vsma mixbin · CoreAudio IOProc
```
