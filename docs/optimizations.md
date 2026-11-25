# xemu-macos optimization ledger

Every landed optimization and every settled experiment in this fork,
with the receipt that shipped or killed it. Open candidates live in
[roadmap.md](roadmap.md). The top-level [README](../README.md) is the
short user doc; [automation.md](automation.md) is the harness interface
(headless mode, QMP commands, input pipe, deterministic mode) and
[building.md](building.md) the build guide. Below: the knob catalog,
the landed changes by subsystem, then the failed, reverted and
settled-negative experiment tables. "Archaeology N.M" cites section N.M
of the retired failure-archaeology skill:
`git show ad569d78c5:.claude/skills/xemu-failure-archaeology/SKILL.md`.

## Runtime diagnostic knobs and normal-mode switches

Default-off knobs turn on with `1`; a `0` knob turns a default-on
behavior off. Every knob has a `getenv` on this branch and belongs to an
entry under Changes. A shipped change keeps a behavior switch only when
it is a normal-mode change the deterministic goldens can't verify (owner
decision 2026-10-15): the first fourteen rows. Removed: `XEMU_SPIRV_CACHE`
(now the `perf.cache_shaders` setting, `a922602c75`) and
`XEMU_VTX_SPAN_FINISH` (v0.16.1). Wave-1-only knobs:
[`Finished/windows-wave1-review-2026-09-08.md`](Finished/windows-wave1-review-2026-09-08.md),
tag `archive/windows-wave1-wip`.

| Env var | Purpose |
|---|---|
| `XEMU_VCPU_QOS` | macOS normal mode, **default on** (`94a722acd8`): the MTTCG vCPU thread and the main-loop thread (`qemu_main`) run at `QOS_CLASS_USER_INTERACTIVE`; `0` leaves both at the default class (slower under load). Deterministic mode's RR and main-loop threads always request it; it applies in null mode only, which gives the process the UI role (`xemu_os_set_app_role`). PFIFO and the APU frame thread are `USER_INTERACTIVE` regardless. Windows (`f96f00ba9d`): the normal-mode vCPU, PFIFO and APU frame threads run at `THREAD_PRIORITY_ABOVE_NORMAL`; `0` leaves all three at normal (the Windows main loop and det RR thread stay at normal). Threads + runtime |
| `XEMU_PFIFO_PUT_LOCK` | Normal mode: `1` makes the DMA_PUT doorbell and USER reads take `pfifo.lock` again (lockless doorbell, `3391b92357`). Deterministic mode always locks: `kick_seq` feeds `pfifo_icount_sync`. Threads + runtime |
| `XEMU_PFIFO_STALL_DOORBELL` | Normal mode, **default on**: a DMA_PUT doorbell inside a flip stall (FLIP_STALL waiting, READ_3D == WRITE_3D) publishes PUT without waking the PFIFO thread; `0` wakes it on every doorbell. Deterministic mode never takes the lockless doorbell. Threads + runtime |
| `XEMU_PFIFO_RING_FREEZE` | Normal mode, **default on**: a DMA_PUT write that moves PUT behind DMA_GET (a ring restart) copies the unread words, DMA_GET through the old PUT (at most 256, CACHE1's worth), and the threaded pusher reads the copy (`pfifo_frozen.h`), so a guest that rewrites the ring head early can't change unread commands; `0` reads guest RAM. Fixes pbkit's `pb_reset()` (waits only 100 ticks for DMA_GET) losing the unread tail when PFIFO lagged: Roswell FpuTestXiso hang, "Reserved pb command" aborts. Deterministic mode never freezes. Threads + runtime |
| `XEMU_GUEST_MO` | `1` keeps every guest memory-order barrier in both modes. By default normal mode drops the load-load barriers (`60fc127871`) and icount drops all of them (`30316da269`) until a network backend attaches: `xemu_net_enable` calls `xemu_guest_mo_keep_all`, which restores them and queues a safe-work tb flush. Normal-mode stores and explicit fences keep theirs. Replaces `XEMU_ICOUNT_MO` (gone). CPU / JIT |
| `XEMU_KI_IDLE_HALT` | Normal mode, **default on**: the vCPU halts in the Xbox kernel's idle loop after spinning 50 µs per idle span (`helper_xemu_ki_idle`); `0` keeps spinning until the next interrupt. Deterministic mode halts at once. Threads + runtime |
| `XEMU_BQL_SPIN` | Normal mode, **default on**: the vCPU polls a contended BQL with trylock for up to 10 µs before it sleeps (`bql_lock_spin`); `0` sleeps at once. The deterministic RR thread never spins. Threads + runtime |
| `XEMU_INTERP_GATE` | Metal backend, **default on**: frame interpolation runs a mode only while a guest frame lasts `mode` refreshes of the display (4x drops to 2x, then off) and holds paced steps whole refreshes; `0` runs the configured mode every flip with the computed holds. Headless, GL and an unknown refresh rate keep the configured mode. MetalFX + presentation |
| `XEMU_VK_CLEAR_IN_PASS` | Vulkan renderer, **default on**: NV097_CLEAR_SURFACE records into the open render pass (it ends the occlusion query, never the pass); `0` ends the pass around every clear (a full-surface load and store each). Kept because pass splits can move pixels on AGX and the null-renderer goldens never reach `vk/draw.c`. nsprof: `clears`, `rpcause_clear`. Vulkan renderer (pgraph/vk) |
| `XEMU_NV2A_RESET_IRQ` | Both modes, **default on** (v0.16.1): a machine reset (`system_reset`, and the reset every load runs first) clears the NV2A interrupt enables and PGRAPH pending bits and recomputes the line, as power-on does; `0` keeps them (v0.16). Without it a pending GPU interrupt re-raised IRQ 3 before the guest ran, and about 1 windowed reset in 100 after a load hung in the kernel's GPU fence wait (0x800426d4). A correctness fix; the goldens don't reset after running. Reset lane, Part 11 |
| `XEMU_OHCI_HCCA_FIRST` | Normal mode, **default on** (v0.16.1, `858e2af19f`): an OHCI frame writes HccaFrameNumber and Pad1 every frame and HccaDoneHead only with a done queue, then raises FNO, WDH and SF (OHCI 1.0a 7.1.4); `0` restores v0.16's order. It matters when a DMA access drops the BQL mid-frame (nv2a's surface access callback): the XDK USB driver lost the done queue and bugchecked 0x0A (Azurik's V22 boot hang, about 3% of real-time boots). Icount runs the frame on the vCPU thread, so the goldens can't verify it. USB (`hw/usb/hcd-ohci.c`, `ohci_hcca_first`) |
| `XEMU_VBLANK_KQUEUE` | macOS normal mode, **default on**: the vblank thread waits for each deadline on one kqueue timer (`NOTE_CRITICAL`) and spins the last 25 µs, instead of `SDL_DelayPrecise`'s 1 ms nanosleep chunks; `=0` restores SDL's wait. Thread CPU 1.0% → 0.25% of a core at a 30 fps cap; frame rate unchanged |
| `XEMU_PFIFO_LOCK_BATCH` | Normal mode, **default on**: the threaded puller keeps `pgraph.lock` across commands and retakes `pfifo.lock` by trylock, ending a batch on a failed trylock, a counted waiter (`nv2a_pgraph_lock_ext`), 256 commands or the pusher's exit (nsprof `pg_handoff`, `pg_tryfail`, `pg_cap`). `=0` swaps both locks around every command. Icount runs never batch |
| `XEMU_PFIFO_FUSE` | Normal mode, **default on**: the threaded pusher runs leaf kelvin methods (pgraph.c's list: no lock, BQL, renderer op, kick or stall flag) back to back holding both `pfifo.lock` and `pgraph.lock`, ending a run on a non-leaf, a counted waiter for either lock (`nv2a_pgraph_lock_ext`, `nv2a_pfifo_lock_ext`), 256 calls or a ring freeze (nsprof `fuse_call`, `fuse_yield`, `fuse_frozen`). `=0` runs every command per word. Off with `XEMU_PFIFO_LOCK_BATCH=0`; icount runs never fuse |
| `XEMU_NV2A_NSPROF` | Wall-time frame profiler, 5 s summaries to stderr. UI-thread buckets: `drawable_acq` (block inside `nextDrawable`), `ui_hud_lock` (main-loop mutex + BQL around the ImGui HUD build), `ui_frame_dt` (present to present); counters `ui_present`, `mfx_scaler_rebuild`. UI buckets are per flip, not per present |
| `XEMU_PFIFO_HEARTBEAT` | 2-second PFIFO diagnostic snapshot |
| `XEMU_MMIO_PROF` | `1` prints an exit histogram of guest MMIO traffic (region × page, loads/stores) and BQL acquire-wait totals |
| `XEMU_MAX_QUERIES` | Occlusion-query pool size, default `4096` (`begin_pre_draw` submits before exhaustion) |
| `XEMU_INPUT_PIPE` | Test input: lines `down <sdl_scancode>`, `up <sdl_scancode>` and `clear` are OR'd into the keyboard-controller state, so keys flow through the user's bindings and work with the window unfocused. POSIX: a FIFO path, opened `O_NONBLOCK`. Windows: a value starting `\\` is the pipe name verbatim (`\\.\pipe\<name>`); otherwise only its last `/`, `\` or `:` component is used, so one `/tmp/xemu.fifo` value works on both hosts. `\r` also ends a line (CRLF writers). Unset = nothing is created and nothing is polled. Client: `scripts/inject_input.py` (both hosts) |
| `XEMU_MFX_REAL_DEPTH` | Real zeta depth for the temporal scaler (A/B): `2` (or any other non-empty value but `0`) = flip-time snapshot; unset/`0` = synthetic. Engages only when a zeta matches the scaler input dims |
| `XEMU_GUEST_PROF` | Apple Silicon only (other hosts print a one-time "ignored" line). One-run guest profiler: a mach-thread sampler resolves vCPU samples to guest TBs vs host symbols. Not benchmark-neutral. The per-exit-kind TB-lookup census was removed 2026-10-15 (revert the removal to restore it) |
| `XEMU_GUEST_BUGCHECK` | `1` hooks the kernel's KeBugCheckEx, found in the export table at each load and at a boot's second vblank (`xemu: guest bugcheck: KeBugCheckEx at 0x80019442 hooked` on the 4627 kernel). Each call prints `xemu: guest bugcheck 0x<code> (<p1>, <p2>, <p3>, <p4>) from <return address>` and a register line, and sends QMP `XEMU_GUEST_BUGCHECK {code, params, caller, count, regs, stack}` (`stack`: 4 KiB of guest memory from ESP in hex, event only). A HLT with interrupts disabled prints `xemu: guest halted with interrupts disabled at <eip> (esp <esp>)` and sends `XEMU_GUEST_HALTED {eip, esp, count}`. First 16 of each per launch. Reads guest state only, so the goldens match with it on (`target/i386/tcg/system/xemu-bugcheck.c`) |
| `XEMU_TB_PROF` | Prints TB jump-cache totals at exit (lookups, hit%, way-1 hits, htable walks, translations, tb_flush count); under icount also step cuts, steps and step-cache hits; and the same-value CR3 reloads (`tlb-reval`: kept, changed, pages flushed, chain severs, full flushes, page tables, mean wall µs) |
| `XEMU_INV_PROF` | Prints SMC / TB-invalidation counters at exit: invalidations by source (notdirty, explicit, single-TB), the false-invalidation share a byte-range overlap check would skip, inv-htable recycle hit rate, a translate-time exit census, and an `(f)` MMIO line (`io_prepare` calls, `cpu_io_recompile` calls and rate, icount io-retaddr cache misses). The `(f)` line reads ~0 in normal mode, where the `can_do_io` elision is unconditional; `io_prepare` is 2x-inflated by recompile re-execution |
| `XEMU_INV_TIMING` | `1` adds a cycle-counter split of the `notdirty_write` body (invalidation+scan+recycle vs preamble+tail) to the `XEMU_INV_PROF` dump. Tick source per host (`accel/tcg/xemu-inv-prof.h`): `cntvct_el0` on aarch64, `CLOCK_UPTIME_RAW` on Apple non-aarch64, `lfence` + `rdtsc` calibrated against `g_get_monotonic_time()` on non-Apple x86, none elsewhere (zeros). Timings are order-of-magnitude; counts are load-immune |
| `XEMU_SUBPAGE_REFUTE` | `1` checks each sub-page skip against a ground-truth live-TB byte-overlap scan (both pages of spanning TBs) and counts violations while the skip stays live. Soak: **0 violations over 28.6M skips across 33 loadvm cycles** |
| `XEMU_XPAGE_PROF` | Cross-page-direct exit census: taken rate by target region (kernel-identity vs low), the backstop's unlink walks (events, dests, cost), the jumps they reset by `tb_add_jump`'s predicate (same-page, onto the emitter's own page, cross-page), link patches per second and the link flush queue (stale-slot exits, patches drained at `cpu_exec` returns and in full queues). Also armed by `XEMU_INV_PROF`. Dark; atexit summary |
| `XEMU_XPAGE_REFUTE` | `1` routes every cross-page-direct exit through the safe lookup path plus a page-table-walk check that the target's phys is unchanged (loud and counted on violation). Slow; falsification only |
| `XEMU_SSE_HOST` | Host fast path for single-precision SSE arithmetic. Always on for aarch64 with `perf.hard_fpu` (+1.90 fps, CPU / JIT). On by default on x86_64 (det heavy-f1 −8.8% instructions, −3.4% core cycles per frame), where the host ops also raise the guest's sticky MXCSR flags exactly; it moved the Windows det hashes once (`57cfe4fb51`: heavy-f1 f1200 `05b0af304a933676` → `acb930ea329c99d6`). `=2`, on any host, runs both paths with strict brackets, aborts on divergence and keeps packed add/sub/mul/div in the helpers; only `=2` matches softfloat's hashes. The `XEMU_SSE_NEON` alias is gone and ignored |
| `XEMU_X87_CENSUS` | `1` arms the x87/SSE Gate-0 census (audit §1.4) and its atexit dump: FIP/FDP store pairs vs deferred updates, x87 runs and insns (mean run length), SSE scalar compares `ss`/`sd`. Emits counter RMWs into generated code; never for an fps run |
| `XEMU_X87_REFUTE` | `1` shadow-checks the x87 exception-pointer deferral (deferred FIP/FCS/FDP/FDS vs the legacy stores at each consumer or helper boundary), the write-through path with stale register images poisoned (`wt-*`, `wt-stack-*`), TOP-keyed translation (`top-key-*`) and MMX entry (`mmx-*`); implies the census dump. Gate: every `*violations=` field 0. At ~1 KB of host code per guest insn it covers TBs up to ~260 guest bytes (production reaches ~800). **Do not combine with both `XEMU_SUBPAGE_FAST_REFUTE` and `XEMU_DSP_JIT_DIFF`**: the 3-way combination deadlocks boot (Failed table) |
| `XEMU_SUBPAGE_FAST` | Sub-page arm (b): the aarch64 store slow-path stub completes a store inline when the slow path is a provable no-op (mismatch exactly `TLB_NOTDIRTY`, non-code 64 B sub-block, all NOCODE dirty clients set). Always on in Apple builds; `=1` opts in on non-Apple aarch64 (unvalidated). Inert on other hosts, where an explicit `=1` says so once on stderr (`XEMU_SUBPAGE_REFUTE` latch, `accel/tcg/tb-maint.c`). Receipt: **+12% draws/s, 6/6 pairs**, fps ~flat, from a build that indexed the dirty bitmaps from ram_addr 0 (the unused `pc.ram`) and so skipped every pure-NOTDIRTY store, code pages included; rebased to `xbox.ram` on 2026-10-15: heavy-f1 skips 78% of 3.3M stub entries, refuter 0 violations on 2.46M decisions. Cold stub only. The histogram dump needs `XEMU_SUBPAGE_FAST_STATS=1` |
| `XEMU_SUBPAGE_FAST_REFUTE` | `1` routes every would-skip decision to a C validator (independent `probe_access` re-derivation, live-TB overlap scan, NOCODE dirty ground truth) that counts violations and performs the real store. Forces `XEMU_SUBPAGE_FAST_STATS=1` |
| `XEMU_SUBPAGE_FAST_STATS` | `1` re-enables arm (b)'s six inline counter RMWs (`xemu_sf_seen`, `xemu_sf_skips`, four `xemu_sf_demote` reasons). Default off: production stubs emit ~39 fewer instructions, and the dump prints `subpage-fast counters disabled (set XEMU_SUBPAGE_FAST_STATS=1)`. Latched at the first stub emission. The decision logic is identical either way. Counter cost: **−0.32% draws/s / −0.57 fps, 3/3 pairs** |
| `XEMU_TLB_WCLEAN_REFUTE` | `1` re-runs the full TLB scan after every `tlb_reset_dirty` bitmap pass, arms what the writable-clean bitmaps missed and prints `xemu: tlb-wclean refute checks= entries= misses= unset= (calls= arming=)` at exit (`misses`: eligible entries in range left unarmed; `unset`: eligible entries anywhere missing a bit). Gate: `misses=0 unset=0`. Never for an fps run |
| `XEMU_TLB_REVAL_REFUTE` | `1` checks what each same-value CR3 reload keeps (`xemu_tlb_reval`, CPU / JIT): every valid TLB entry of the paging mmu_idx and every jump-cache, step-cache and ret-memo TB against a fresh page walk (`cpu_get_phys_page_debug`) and the fill log, and the xpage registry against each frame dropped without severing; aborts on a mismatch. Exit line `xemu: tlb-reval` (also under `XEMU_TB_PROF`). Never for an fps run |
| `XEMU_TB_REVAL_REFUTE` | `1` runs the full TB check after each scoped one of an incremental checkpoint restore (`xemu_tb_revalidate_pages`, `accel/tcg/tb-maint.c`, `82567106f6`) and prints `xemu: reval refute: ...` at exit and every 256 scoped loads; extra drops, unprotected pages and untailed pages must stay 0. The full check's results are kept, so a violation doesn't spread |
| `XEMU_PUSH_PRESENT_REFUTE` | Debug: peeks the present ring and cross-checks the consumed-step stream against the pull path (monotonic `frame_seq`, no skipped-then-resurrected step, same texture/event/dims). Prints `peeked`/`compared`/`mismatches`/`resurrections` every ~5 s. Re-adds the pull round trip; measure perf with it off |
| `XEMU_PRESENT_STALL_MS` | Windowed Metal: how long the UI thread waits for the `xemu-drawable` thread's `nextDrawable` before dropping the frame (default 100); `0` calls it on the UI thread unbounded (v0.16). A tuning knob, not a kept switch. Stalls print on stderr and send QMP `XEMU_PRESENT_STALL`; nsprof counts `present_drop` (`ui/xemu-metal.m` `drawable_thread_start`, `188ff96260`; `docs/troubleshooting.md`, Present stalls) |
| `XEMU_VTX_SPAN_REFUTE` | `1` checks every vertex finish or fence wait a copy skips (`vk/vertex.c`) against an independent record of each flight slot's vertex-RAM reads, taken from the vertex buffer bindings (offset, stride, element size, vertex count); a recorded read the copy changes is a violation (the first 8 print). Exit line `xemu: vtx-span refute checks=N violations=M` (`checks`: skips whose copy overlapped a recorded read) |
| `XEMU_DSP_JIT_STATS` / `XEMU_DSP_JIT_DIFF=N` | DSP JIT counters / bit-exact validation |
| `XEMU_DSP_JIT_DIFF_SYNC` | `1` (with `XEMU_DSP_JIT_DIFF`): the validator replays each block inline on the APU thread, not through the async worker ring. Set by the xbox suite's `xbox-mcpx-dsp-jit-diff-sync` arm |
| `XEMU_DSP_JIT_PIN_AUDIT` | `1`: a per-op check that the A/B (x26/x27) and X/Y (x20/x21) pins match `dsp->registers`; a mismatch prints `DSP JIT pin audit FAIL` on stderr and doesn't abort. SR is not audited |
| `XEMU_APU_PROF` | Per-second APU-thread utilization to stderr (deterministic mode measures it only when set) |
| `XEMU_VK_DRIVER` | Diagnostic (POSIX): the Vulkan driver, overriding `display.vulkan.driver` and the MoltenVK pin of deterministic and offscreen runs: `kosmickrisp` or `moltenvk` (the bundled copies) or a dylib path. A failure exits 1, no fallback. The launchers drop an inherited one (`AUTOMATION_ENV`). `docs/kosmickrisp.md` |

### Build-time and harness knobs

Read by `build.sh` at configure time or by the bench harness; `build.sh`
is the full list of build knobs. `XEMU_WIN_O3` is gone (`6a58566a54`):
x86_64 Windows builds default to `-O3` without the stack protector, as
Darwin and Linux do.

| Env var | Purpose |
|---|---|
| `XEMU_BENCH_ISOLATE_CACHES` | `0`: shader/pipeline caches live in the bench cache every bench shares (`XEMU_BASE_PATH` = `$XEMU_BENCH_CACHE`), never the owner's pref dir. `1` or `--isolate-caches` (`bench-abx.sh`, the Windows bench) plants the scratch config as a portable-mode marker in each cloned bundle's `Contents/Resources`, so `spirv_cache_v*/` and `pipeline_cache.bin` start cold in the work dir at batch start and warm during the warmup. Both arms share one cache only when they run one bundle (`bench-abx.sh` clones it once); two bundles get a cache each. `bench-abx.sh --one-warmup` warms once, on E. Binaries older than `XEMU_BASE_PATH` get this automatically. Recorded as `cache_isolation` (`off`, `shared`, `per-arm`) in `meta.json` / `receipt.json` |
| `XEMU_BENCH_CACHE` | The folder every macOS bench and probe launcher passes as `XEMU_BASE_PATH` (default `~/Library/Caches/xemu-fork-bench`; `scripts/xemu_lease.py`, `scripts/xemu-lease.zsh`). The Windows bench (`bench-savestate-ab-win.py`) defaults to `%LOCALAPPDATA%\xemu-fork-bench` |
| `XEMU_TEST_CONFIG` | Source config every macOS harness clones its HDD, EEPROM and scratch config from; unset = `../fixtures/xemu.toml` beside the checkout (`bench-det.py`, `bench-abx.sh`, `probe-run.sh`, `cast-probe.py --walk`; `azurik-pack.py`, `bench-det.py --check d98` and cast-probe's casts read `AZURIK_FX` instead). `--config-src` wins where a script takes it; `bench-det.py`'s old `XEMU_FIXTURES` folder is gone. No fallback to the personal `xemu.toml`. The Windows bench uses `--config-template`, else this, else `%APPDATA%`'s `xemu.toml` (only ever copied) |

## Changes

> **2026-08-04 optimization wave.** Bullets dated 2026-08-04 implement
> [`fork-optimization-audit-2026-08.md`](Finished/fork-optimization-audit-2026-08.md).
> Wave receipt, interleaved A/B on the F8 heavy anchor: **38.12 ± 0.59 →
> ~48.1 fps (+10.0 fps, +26%; +20.4% draws/s), 5 clean pairs,
> unanimous**. Its known post-`loadvm` SIGSEGV in the `ohci`/`flatview`
> path was a use-after-free in xemu's NV2A memory-access callback list,
> fixed 2026-10-11 (Threads + runtime; archaeology 7.6).

### Windows native port (2026-09-07)

First run on real Windows hardware (Windows 10, i7-4770K, NVIDIA TITAN
Xp 561.09, MSYS2 MINGW64 gcc 16). Records:
[`Finished/windows-port-2026-09.md`](Finished/windows-port-2026-09.md)
(it folds the 2026-09 audit summary; the raw audit is
`git show 45c5c70bbb:docs/Campaigns/windows-audit-2026-09.json`),
`docs/windows-gating-audit.md` §S-§W, the review
`docs/Finished/windows-wave1-review-2026-09-08.md`.
macOS behaviour is unchanged unless stated. Wave 1 of the audit plan
landed one batch at a time from 2026-09-08, except item 3
(`285779ce83`, held by the review); the branch is tag
`archive/windows-wave1-wip`. Its ~10 s Azurik segfault, blamed on the
`-O3` flags of `fb42bce22e`, was SDL's `WIN_OpenClipboard` race
(`058431f83b`; `docs/windows-gating-audit.md` §T, §U).

| Change | Contents |
|---|---|
| **Upstream merge `2e0aad3e18`** (xemu `429c9972eb`) | DXGI / WGL_NV_DX_interop presenter, PTIMER alarm IRQs (nv2a vmstate v4), controllerdb from the config path, glslang 16.5.0, and upstream's `audio.use_dsp_jit` flip to false (`fc13b78060`). Only `ui/xemu.c` conflicted; the DXGI hooks sit inside the fork's non-Metal GL branch |
| **DSP engine default restored to the fork's (2026-09-08)** | That flip put every host, macOS included, on the C interpreter; `config_spec.yml` is back to `use_dsp_jit: default: true`, and `[audio.dsp_jit] enabled` selects the fork's inline ARM64 JIT instead. Precedence: `dsp_want_external_jit_engine()` (`hw/xbox/mcpx/apu/dsp/dsp.c`) |
| **Native MSYS2 configure fixes (`ac28e5a5bd`)** | `build.sh` passes `--prefix=$(cygpath -m /qemu)` (Python ≥ 3.13's `ntpath.isabs`) and unpacks the dsp56300 prebuilt with tar `--force-local` |
| **DXGI presenter actually activates on NVIDIA (`2182baec46`)** | The interop FBO is completeness-checked with the WGL_NV_DX_interop object locked (1:1 lock/unlock per frame); before, `GL_FRAMEBUFFER_UNSUPPORTED` silently fell back to `SDL_GL_SwapWindow`. Activation logs `win32_dxgi_present: DXGI Presentation Helper successfully initialized (mode=flip, tearing=supported)` |
| **First real-ICD result** | The Vulkan renderer renders Azurik correctly on native NVIDIA (intro ≈ 30 flips/s, title 21–25). Open validation VUIDs: `VUID-VkDeviceCreateInfo-enabledLayerCount-12384`, `VUID-VkShaderModuleCreateInfo-pCode-08740` |
| **Wave 1, item 1** | The `XEMU_WIN32_DXGI` hatch (removed 2026-10-16) and two non-Apple `ui/` compile warnings (see the test-input row) |
| **Wave 1, item 2: SPIR-V disk cache (`818fe27828`)** | `qemu_fopen` (UTF-16 paths), atomic temp+rename writes, a `-dbg` tag under `debug_shaders`, regenerate-on-miss. `spirv_blob_is_valid()` walks the instruction stream and requires a final `OpFunctionEnd`, since SPIRV-Reflect asserts on or accepts truncated blobs; orphan `.tmp` files are swept at open |
| **Wave 1, item 3: PVIDEO/display (held)** | Staging reclaim hoisted ahead of the display aux command buffer; no overlay-upload skip on a register-only key; `display_skip_strict()` honours resolution/PVIDEO changes |
| **Wave 1, item 4: APU/XID** | XID index guard (`hw/xbox/xid.c`, `hw/xbox/xid-gamepad.c`); voice handles bounds-checked before `filters[]` / HRTF / SSL lookups; an out-of-range `NEXT_VOICE_HANDLE` stops the walk (one stderr line); out-of-range handles are dropped (the `address_space_*` fallback could take the BQL under `d->lock`; an in-range handle with a guest-bogus `VPVADDR` still reaches it). APU RAM stores mark dirty off Apple only; the `use_dsp_jit` switch runs under the BQL; the APU frame thread registers with RCU. The `int16` pitch-LUT clamp was not taken (`3495cd28c0`, `40f0a01ad3`) |
| **Wave 1, item 5: Vulkan** | `nv2a_vk_bounds_check` real in every build, macOS too (was `__builtin_unreachable()` in release builds); `volkLoadDevice()` on non-Apple; `VK_KHR_external_semaphore_win32` optional; LRU in-use filter restored (as upstream's `lru_try_evict_one`); `CPUJumpCache` stride static-asserted |
| **Wave 1, item 6: Windows/GL diagnostics (`eb971f860d`)** | `nsprof` under OpenGL, x86 `rdtsc` in `xemu_inv_ticks()`, stderr flushes, `NV2A_STRIP_PROFILE_COUNTERS` tracks the debug build, `XEMU_SUBPAGE_FAST=1` warns once without an emitter |
| **Wave 1, item 7: build.sh** | GCC PGO arm (`.gcda` discovery, `cygpath -m` profile dir); `XEMU_HARDENING` (removed 2026-10-10); `-O3` opt-in behind `XEMU_WIN_O3=1` until `6a58566a54`; tar `--force-local` in `eeb992dcd2` |
| **Wave 1, item 8: Windows bench** | `scripts/bench-savestate-ab-win.py` + `scripts/win/` (`add066354f`, `c77d8a4b05`, `4d0cec2790`) and the `XEMU_INPUT_PIPE` named-pipe transport |
| **Windows test-input transport, plus a DXGI present hatch (2026-09-08)** | `XEMU_INPUT_PIPE` rides a Win32 named pipe polled from `xemu_input_update_sdl_kbd_controller_state()` (`ui/xemu-input.c`; naming in the knob table). The `PIPE_NOWAIT` instance is re-armed with `ConnectNamedPipe` in the same poll as `DisconnectNamedPipe`, so a one-shot writer never gets `ERROR_PIPE_BUSY`. `XEMU_WIN32_DXGI=0` went 2026-10-16 (upstream's `SDL_GL_SwapWindow` fallback remains). `ui/xemu-present.h` is included unconditionally; `g_framebuffer_rect_shader` is Apple-only |

### Rendering (correctness fixes)

The first four fix native Vulkan drivers.

| Fix | Contents |
|---|---|
| **Windows / native-Vulkan black screen fixed (2026-08-09)** | The set-0 UBOs (`VSH_UBO`, `PSH_UBO`) are two `descriptorCount=1` writes, not one spanning stages (`VUID-VkWriteDescriptorSet-descriptorCount-10776`, `-dstArrayElement-00321`), which left the fragment UBO unwritten. The display descriptor skip cache is Apple-only: its raw `SurfaceBinding*` key was never cleared on eviction, so a destroyed `VkImageView` stayed bound. MoltenVK tolerated both; macOS behaviour is unchanged |
| **`shaderTessellationAndGeometryPointSize` no longer required on non-Apple (2026-08-09)** | Weak or paravirtual ICDs lack the feature (Apple already ran without it). Without it, device init continues and the geometry shader omits the `gl_PointSize` passthrough |
| **Dynamic-state reuse fixed on native drivers (2026-08-09)** | `begin_draw`'s dedup of line width, depth bias and blend constants could skip `vkCmdSet*` against a previous command buffer's value (VUID-vkCmdDraw-None-07833/07834/07835); MoltenVK keeps dynamic state across command buffers. Within one command buffer, binding a pipeline that held one of those states statically replaced the value on Mesa drivers (KosmicKrisp included) behind the cache. Since 2026-10-19 (`848c45ba58`, every host) every frame pipeline, draw and clear, declares one dynamic-state list (`frame_dynamic_states`) and the `r->dyn` shadow resets only at command-buffer begin |
| **Texture-upload `bufferOffset` aligned to texel block size (2026-08-09)** | Staging offsets round up to 16 (VUID-VkBufferImageCopy-bufferOffset-00193); MoltenVK accepts any byte offset |
| **PVIDEO overlay movies on the Vulkan renderer (`632b7fd705`, willemabc)** | FMV through the video overlay (Midtown Madness 3) was black on Vulkan. `vk/display.c` re-uploads the overlay every frame while enabled, its YUV staging reserved before the command buffer begins (`43a23d17bf`); `surface-compute.c` reads YUY2 as Y0 U Y1 V (BT.601 limited range); the Metal UI pulls the display only while the overlay is on, the guest runs and no flip has published for 100 ms |

### CPU / numerics (host-architecture correctness)

- **Inline x87 FPU host-correctness fixes (2026-08-09).** (1)
  `g_use_hard_fpu_inline` is gated on `TCG_TARGET_HAS_fpu`, so Windows
  ARM64 uses the helper-based hard FPU (the gate in fact turned the
  cache off on every host; its fork layers went on 2026-10-15). (2) On
  x86_64, `FIST`/`FISTP` under round-toward-±∞ and `FRNDINT` under any
  non-nearest mode rounded to nearest. Fixed in `cfcc8e68ab` (moved
  every Windows det hash): the hard conversions take the guest RC and
  saturate out-of-range values with invalid, as softfloat does. Since
  `deb98ee93f` they round with `frndint` under the guest's control word
  (`truncl` for the truncating forms); inline since `67e48b43b2`.
- **Sub-page dirty-tracking default gated to Apple (2026-08-09).**
  `XEMU_SUBPAGE_FAST=1` opts in elsewhere (x86_64 emits no stub).

### CPU / JIT (x86_64 hosts, 2026-10-18)

Measured with `scripts/win/det_golden.py --pmc` on the Windows box
(i7-4770K, MSYS2 GCC 16): instructions and core cycles per det heavy-f1
frame to 1200, interleaved cross-binary pairs. Every row kept the
Windows goldens (heavy-f1 f300 `37fe3d839c4be757` f1200
`ec2e9596cc9dfa9c`, stream-e2 f600 `38b50b4c6f35ac02`, cold boot f900
`d6381be67430024d` f1800 `1daeb1149976062c`). The x87 rows took det
heavy-f1 from 38.8 to 76 fps. The code gates on `__x86_64__`; only the
last three rows are Windows-only. "Rosetta 2" receipts come from the
x86_64 macOS build, which reproduces the Windows debug hashes (goldens
unchanged, `XEMU_X87_REFUTE=1` 0 violations); **their Windows numbers
are owed**.

| Change | Mechanism | Receipt |
|---|---|---|
| **x87 hard helpers on the registers in place (`66a991d060`)** | Hot helpers `fldt` env's registers, compute and `fstpt` back instead of passing `floatx80` by value | Cycles −26.1%, insns −6.2%, det fps 38.8 → 52.8, 4/4 |
| **x87 helpers without the global sync, fused memory operands (`acecea21c8`)** | Helpers are `TCG_CALL_NO_WG`; an m32fp/m64fp/m32int operand is one helper call (`fop_ST0_m32`/`m64`/`i32`, every host); 8+2-byte register copies | Cycles −1.9%, 4/4 |
| **The host x87 control word is the guest's (`deb98ee93f`)** | Windows starts threads at CW `0x27f`; `update_fp_status` and `x86_cpu_exec_enter` load the guest's PC and RC on the vCPU thread; libm sin/cos/tan run under `0x37f` | Insns −2.35% |
| **Hot x87 ops inline on the host x87 unit (`aae214c27a`)** | TCG ops `x87_arith`, `x87_arith_m`, `x87_ld`, `x87_st`, `x87_cmp` (`TCG_TARGET_HAS_x87`, tcg/i386) emit the helpers' instructions in the TB, bit for bit | Cycles −23.5%, insns −15.3%, det fps 54.4 → 72.4, 4/4 |
| **TBs keyed by the x87 TOP on x86_64 (`873ab92ed4`)** | `xemu_x87_top_key`: ST(i), tags, pushes and pops are constant env offsets; `XEMU_X87_REFUTE=1` 5.0G checks, 0 violations | Insns −8.3%, cycles −6.4%, 4/4 |
| **fist, fsqrt and frndint inline too (`67e48b43b2`)** | `x87_st` integer kinds and `x87_unary` | Insns −0.25% (4/4), cycles within noise |
| **fcmov as a select, not a branch** | `setcond` + `movcond` (`gen_fcmov_hw`); the branch ended the TCG basic block | Cycles −3.0%, det fps +3.3%, 4/4 |
| **x87 stack bookkeeping and FDP deferred, as on aarch64** | fpstt, tag bytes and (under icount) FDP/FDS deferred as on the write-through path (`x87_defer`, `x87_wt_land`, `xemu_x87_defer_fdp`; `st_f64_slow` stores a held FDP) | Rosetta 2: TB host insns/frame 48.9M → 47.1M; expect det insns −1.5..−2% |
| **x87 memory operands straight from the guest page** | `x87_ld_g`, `x87_arith_g`, `x87_cmp_g`, `x87_st_g` run the x87 insn on the guest page after the TLB check (static TOP only; m16int and m80 keep the old forms); a tcg/i386 TLB memo must count them with qemu_ld/st | Rosetta 2: 47.1M → 45.8M; expect −1.3..−1.6% |
| **Softmmu TLB walks shared within straight-line code, as on aarch64** | TLB memo plan (`tcg_tlbm_plan`) moved to `tcg/tcg-tlbm.c.inc`; aarch64 output is unchanged. Load slot in R10/R11, store slot in R8/R9 (`tlbm_st_addend` under icount); `tcg_reg_alloc_call` takes R8/R9 back as argument registers on both ABIs (a probe without that crashed at the first loadvm). `--debug` aborts with "the TLB entry changed under a TLB memo". `ROSETTA_ADVERTISE_AVX=1` makes Rosetta advertise AVX; without it 16-byte SSE accesses are helper calls, so only with it do the qemu_ld2/st2 shares and the aligned `ror` form run. It moves the det base: both A/B arms or neither | Rosetta 2: 45.8M → 40.0M (−12.6%), `XEMU_TLB_WCLEAN_REFUTE=1` `misses=0 unset=0`; owed: 6 pairs plus a `--debug` offscreen loadvm pass for Win64's R8/R9, expect insns −3..−6%, cycles −2..−4% |
| **Hot/cold TB layout on tcg/i386 (`6d0e98c828`)** | As on aarch64 | Cycles −0.85% median, 3/4 |
| **No `-fzero-call-used-regs` on Windows (`baf5e0c100`)** | Build flag | Insns −5.5% (4/4), cycles within the spread |
| **file-win32 block I/O in coroutines (`4e50b583d4`)** | Block layer | Det cold boot cycles −6.1% (3/3) |
| **vCPU, PFIFO and APU threads above normal priority on Windows (`f96f00ba9d`, `XEMU_VCPU_QOS=0` opts out)** | `THREAD_PRIORITY_ABOVE_NORMAL` | Windowed heavy-f1 under an all-core load 6.8 → 9.3 fps (+36%, 3/3) |

### CPU / JIT (ARM64)

- **Sub-page code dirty tracking (default on, 2026-07-11).** A
  per-`PageDesc` 64-block bitmap lets a data store that misses every
  code sub-block skip the Xbox whole-page TB invalidation (100% of
  invalidations were data-write false sharing). Refuter: 0 violations
  over 60M+ checked skips. A/B: **F8 30.35 → 34.68 fps, F5 47.33 →
  58.96**.
- **Cross-page direct block chaining, Xbox-relaxed (default on,
  2026-07-12).** Direct jumps to another page patch a real `goto_tb`
  chain instead of the ~20-op inline jump-cache probe. Backstops: INVLPG
  invalidates the flushed code page; CR3 writes and every other
  full-TLB-flush class (`tlb_flush_by_mmuidx` hook) sever the registered
  cross-page chains (a same-value CR3 reload only when a page that held
  TBs changed, below). `XEMU_XPAGE_REFUTE`: 0 violations over 4.86B
  checks. A/B: **+1.30 fps F8**.
  **Backstop v2 (v0.11.1, the "new-area lag" fix).** v0.11 severed
  chains with a whole translation-cache flush, and level streaming fires
  full-flush classes continuously (F8 trough 14.6 vs 38.6 fps). Every
  cross-page link now registers its destination TB, and the backstop
  unlinks exactly those chains (`tb_jmp_unlink`); registry overflow
  (128k dests) falls back to the full flush. The walk batches its icache
  flushes (`flush_idcache_defer_begin`): stream-e2 walk mean 449 → 98
  µs. Severing only cross-page jumps would not shrink the rest
  (`XEMU_XPAGE_PROF`, Settled vectors).
- **Link patches queue their icache flush (2026-10-16).** A B replacing
  another B is a permitted concurrent modification, so the aarch64
  backend queues each `tb_add_jump` link's flush (`flush_idcache_link`)
  and runs one DSB / IC IVAU / DSB / ISB at 64 entries, on a stale-line
  exit, at every `cpu_exec` return and before `tb_flush`. Resets,
  LDR-form links and new code flush at once. Det against cced50204c:
  stream-e2 Mcycles/frame −1.4%, harness cost per `xemu-run` −21%.
- **Same-value CR3 reloads revalidate instead of flushing
  (2026-10-19).** The kernel's only TLB flush is a reload of the CR3 it
  holds (509 per 600 det stream-e2 frames). Each 32-bit-walk fill logs
  its PDE and PTE (`xemu_tlb_log_fill`); a reload (`xemu_tlb_reval`)
  flushes only logged pages whose PTE changed beyond A/D
  (`tlb_flush_page_by_mmuidx`), severs chains only when such a page's
  old frame held TBs, and falls back to a full flush on a changed PDE,
  an unloggable fill (PAE, NPT, table outside RAM), a 65th page table or
  over 1024 changed pages. Det against c3327bc088: stream-e2
  Mcycles/frame −6.6%. `XEMU_TLB_REVAL_REFUTE=1` and
  `XEMU_TLB_WCLEAN_REFUTE=1` clean.
- **Inline hard FPU (retired).** The inline x87 cache has not run since
  `3eb1b2de10` (its `TCG_TARGET_HAS_fpu` gate is private to tcg/); its
  fork layers were removed, and upstream's skeleton stays behind
  `g_use_hard_fpu_inline = 0`. Still live on the helper path: `fnstsw`,
  register moves, `fchs`/`fabs`, `fld1`/`fldz`, `ffree`/`ffreep` as TCG
  code (`613670d8eb`), and `fdecstp`/`fincstp`. The exact write-through
  path (`4d2466d106`, +5.6% det heavy-f1) replaced the cache on aarch64:
  `floatx80`↔`double` inline via the backend's `ld80f`/`st80f`.
  Refinements (det heavy-f1; "x87 lane" means on 22329bd8bc):
  - `st80f` waits for the next flush; a mid-TB exit lands pending ones
    from shadow doubles in `restore_state_to_opc`, and only `st_f64_slow`
    stores: −5%, then −1.2% Minsns/frame.
  - `fucom*`/`fcomi*` compare cached doubles, `fcmov` selects, and
    argument-only `cc_compute_all`/`cc_compute_c` skip the flush: −4.8%
    Minsns/frame.
  - Stack ops move a translate-time `fpstt` offset; `env->fpstt` and
    `env->fptags` land at the flush or from insn_start word 2: −5.7%
    Mcycles/frame, x87 lane.
  - TBs keyed on TOP (`env->fpstt` in `cs_base` bits 32-34): −2.1%
    Minsns/frame, x87 lane.
  - Results cross flushes as doubles (`env->xemu_x87_dstale` marks a
    stale image): −2.9% Mcycles/frame, x87 lane.
  - Inline `fsqrt`, `frndint`, `fist`/`fistp`; an `fldcw` keeping RC
    and PC bit 9 stores `fpuc` and the softfloat precision itself and
    chains on: −4.2% Mcycles/frame, x87 lane.
  - Shorter compares (2026-10-16): `com` uses three CSELs, `fcmov` the
    new `movcond_f64` op (FCSEL): −1.46% Minsns/frame against the
    ret-memo commit.
- **x87 m32fp/m64fp operands go straight between the guest page and an
  FP register (2026-10-16).** New `qemu_ld_fp`/`qemu_st_fp` ops
  (`tcg_gen_qemu_ld_f32` and friends; aarch64 LDR/STR S or D) serve
  `fld`, the m32 arithmetic operands and `fst`/`fstp` m32fp/m64fp,
  removing the GPR↔FP moves. Every `st_f64_slow` register stays
  reserved until the access is emitted. Det against 9a8cb87de9: heavy-f1
  Minsns/frame −1.28% (16/16).
- **x87 m64fp loads quiet SNaNs on the FP side (2026-10-16).** LDR D
  (`tcg_gen_qemu_ld_f64`) plus the new `quiet_f64` op (FMAX Dd, Dn, Dn),
  bit-exact only under FPCR.DN = FZ = AH = 0 (0 mismatches over 8.3e8
  operands). Det against v0.15 (6f9f751565): Minsns/frame −0.38%
  heavy-f1, −0.41% stream-e2.
- **fpatan: upstream's algorithm with cheaper steps (2026-10-16).**
  `helper_fpatan__hard` runs the pack steps inline (`fpatan_pack`), the
  polynomial as one double chain (`fpatan_poly`; an empty asm blocks
  FMADD contraction) and NaN tests only for exponent 0x7fff.
  Compile-time `XEMU_FPATAN_FAST`, not a switch. 0 mismatches in 1.2e9
  differential checks; ~470 → ~305 cycles a call.
- **x87 memory operands hold their own FDP (2026-10-16).** With 32-bit
  addressing and no segment base, each x87 memory insn gets its own A0,
  which is also the FDP hold, dropping a MOV per operand. Det
  Minsns/frame −1.30% heavy-f1.
  `XEMU_X87_REFUTE` also checks the held FDP at every flush
  (`xemu_x87_fdp_chk`).
- **FPCR caching + rounding-correct FIST (retired with the cache,
  2026-10-15).** `cached_fpuc_rc`, the fused
  `cvt_{rn,rm,rp}_i{32,64}_f{32,64}` ops, `rint` and `rint_cvt` are
  gone; `rint_f64` (FRINTX) is back for write-through `frndint`/`fist`.
- **Signaling vs quiet compares (retired with the cache, 2026-10-15).**
  `coms_f32`/`coms_f64` (FCMPE) are removed; `com` (FCMP) stays for the
  write-through `fcom`, `fucom*` and `fcomi*`.
- **Control-word resync.** Every `env->fpuc` writer goes through
  `cpu_set_fpuc`, which rebuilds `HF_FPU_RC`/`HF_FPU_PC` in `hflags`
  (only `gen_fldcw_wt`, for words that change no `hflags` bit, skips it
  and stores `fpuc` and the softfloat precision itself).
  `x86_cpu_exec_enter` re-applies the x87 RC to the vCPU thread's
  FPCR at every `cpu_exec` entry. Every MXCSR write reaches
  `update_mxcsr_status` (keeps `HF_SSE_DFL`); reset uses `cpu_set_mxcsr`.
- **Misc.** `fnstcw` is one load; `insertion_sort_syncs` replaces
  `qsort` for N ≤ 16. `gen_flcr` was retired with the cache; the aarch64
  backend dropped the `flcr` op and every arm only upstream's inexact
  inline x87 cache emitted, which can no longer be switched on there.
- **3-MOVK constant materialization.** `tcg_out_movi` emits MOVZ/MOVN
  plus up to 3 MOVK instead of a literal-pool load, so 48-bit host
  pointers stay in the instruction stream.
- **Sticky host-FPU bracket: the SSE fast path goes live on Apple
  Silicon (+1.9 fps).** The host stays in guest-SSE FPCR mode between
  helpers instead of a serializing restore per op (`=2` differential
  keeps strict brackets). **+1.90 fps, 3/3 pairs**; default on for
  aarch64 with `perf.hard_fpu`. x87 hard-FPU helpers save/restore around
  their own ops.
- **Inline SSE packed-single arithmetic (2026-10-15).**
  `addps`/`subps`/`mulps`/`divps` compile to one NEON `.4S` op
  (`f{add,sub,mul,div}_vec` behind `TCG_TARGET_HAS_fvec`: aarch64 only,
  off on Windows/ARM64 with the other FP ops) in TBs with `HF_SSE_DFL`
  (MXCSR nearest, FTZ and DAZ clear) and x87 RC nearest, bit for bit
  with the helper. LDMXCSR, FXRSTOR and XRSTOR end their TB. Det
  heavy-f1 Minsns/frame −2.67%, cycle-neutral.
- **Guest registers pinned in host registers (2026-10-15).** EAX..EDI
  live in x8-x15 and EIP, CC_DST, CC_SRC, CC_OP in x24-x27
  (`tcg_pin_env_global()` from the `TCGCPUOps.pin_globals` hook). Every
  helper call saves and reloads x8-x15 (call-clobbered); EIP/CC_* are
  stored only if the helper may read globals or fault, and reloaded
  only if it may write them. `TCG_CALL_NO_RW_PINNED` exempts the x87
  and SSE compute helpers that touch none of it and cannot fault (not
  fcomi/fucomi, which write CC_SRC, nor fwait). A `--debug` build
  (CONFIG_DEBUG_TCG) poisons the env homes (0xdeadbeef) while TB code
  owns them and aborts if a helper without a reload changed one. Det
  against its parent (on 94a722acd8): Mcycles/frame −3.9%. All eight
  GPRs in call-saved x20-x27 cost +4.6% (spills).
- **Softmmu TLB walks shared within straight-line TB code
  (2026-10-15).** The TLB memo: an access to the page an earlier access
  of the same kind and mmu_idx walked reuses the walk. Loads keep ntag
  in x4 and the addend in x5 (check: 3 insns); stores keep ntag in x6
  and the TLB entry's address in x7, so a sharing store re-reads
  `addr_write` and the addend (check: 6 insns), which catches nv2a's
  cross-thread TLB_NOTDIRTY re-arm. The plan
  (`tcg_tlbm_plan`) fills a slot only when a later access will share it:
  42% of heavy-f1 accesses share. A `--debug` build re-walks every
  shared access. Det against its parent (on 94a722acd8): Mcycles/frame
  −3.9%, Minsns −5.25%.
  - Aligned and 16-byte accesses share too (2026-10-16; EXTR rotation,
    misaligned addresses take the slow path's #GP); new `qemu_ld_v128`
    op: heavy-f1 Minsns −0.59%.
  - Under icount only, stores keep the addend (`tlbm_st_addend`); the
    sub-page prefilter's store ends the slot. Normal mode keeps the
    6-insn form (keeping the addend there widens the re-arm race), which
    the det goldens no longer cover. Det against 9a8cb87de9:
    Minsns/frame −2.48% heavy-f1.
- **MMX entry inline, once per TB (2026-10-15).** `helper_enter_mmx`
  per MMX insn was 6.6-18.6% of the det cold-boot vCPU. The TB's first
  MMX insn now inlines the entry and calls `helper_enter_mmx_sync` only
  for a stale register; later ones emit nothing until an x87 escape,
  EMMS/FEMMS, FXRSTOR or XRSTOR clears `mmx_entered`. Det cold boot
  Mcycles/frame −17.8%. `punpck*` and `pmulhw` later became inline TCG
  ops, and `movd mm, r/m32` one 8-byte store of the zero-extended value
  instead of a zero store plus a 4-byte store.
- **TB-dispatch inline probes (+1.5 fps family, 2026-07-05).**
  `XEMU_GUEST_PROF`'s per-exit-kind TB-lookup census (removed
  2026-10-15; revert the removal commit to restore it) put near-returns
  at 46% of TB lookups. Shipped: a 4096-entry near-return memo probed
  inline at ret sites (**+0.77 fps**; `-d exec,nochain` keeps exec
  logs complete) and the same inline probe on
  the jump cache (`accel/tcg/xemu-inline-jc.c`, sole owner of the cache
  layout; +0.70 fps). The APU VP doorbell block joined the lockless-MMIO
  set. `QEMU_BUILD_BUG_ON` pins the emitter's layout facts
  (`xemu-inline-jc.c:50-51`); the memo drops a page's entries whenever
  the jump cache does; since 2026-10-16 entries hold `tc_ptr` and the
  index is `eip ^ eip >> 14` (15 host insns, was 24; det against
  cced50204c: Minsns/frame −1.07% heavy-f1). The kills en route (JIT
  write-protect caching, the per-depth return-address ring) are in the
  failed-experiments table.
- **icount step TBs cached by pc (det mode, 2026-10-16).**
  `xemu_step_lookup` fronts the qht lookup of each `xemu_istep` step with
  an 8192-entry `{pc, tb, stamp}` cache in `CPUJumpCache`, dropped per
  page by bucket stamps when the jump cache drops. `CF_PCREL` TB
  invalidation clears the jump cache but not this cache (a hit relies on
  the `CF_INVALID` check instead); off-RAM one-shot TBs are never
  cached. `XEMU_TB_PROF`
  `step_hit` 94.2% on cold boot; det cold boot Minsns/frame −1.42%.
  Step TBs stay out of the jump cache and unchained.
- **Two-way jump cache (2026-10-16).** 2048 two-way sets inside each
  page's 64-entry range (`tb_jmp_cache_hash_func(pc) & ~1`); the inline
  probe reads way 0 only. `XEMU_TB_PROF` htable walks 3711 → 287 a
  cold-boot frame; det stream-e2 Mcycles/frame −1.68%. An inline way-1
  check cost translation time for no gain.
- **`can_do_io` bookkeeping elided unless replay is on (default on,
  2026-08-04; under icount too since 2026-10-15).** The
  `set_can_do_io` stores sit behind a translate-time latch that only
  `replay_mode != REPLAY_MODE_NONE` disables, avoiding the
  `cpu_io_recompile` round trip (`XEMU_INV_PROF` `(f)` line: 242k/s).
  A/B F8: **41.59 → 48.12 fps**. Under icount `io_prepare` asks
  `xemu_io_ok`, which calls `xemu_io_ra_in_last_insn` (TB found by
  return address through a 1024-set, 4-way cache indexed by
  `(ra >> 2) ^ (ra >> 14)`); det against 94a722acd8 Minsns/frame −1.6%.
  A 32768-bit pc filter answers first for the translator's per-insn
  `xemu_io_pc_is_set` check (its `g_hash_table_contains`).
  `cpu_watchpoint_hit_reruns` reads the flag in every mode since
  `32b1ddf856`. With the flag left true, icount's "Bad icount read"
  guard no longer fires.
- **`notdirty_write` dirty-check fast path (default on, deliberately no
  fps claim, 2026-08-04).** `is_clean` is known locally from the XBOX
  invalidation's "page still holds code" result; single-page dirty
  queries read all five clients under one RCU guard. Its population was
  already gone (≤ 0.06% of the vCPU), so no A/B; it stays as
  behaviour-equivalent and simpler.
- **x87 translator constant-factor pack (default on, 2026-08-04).**
  `XEMU_X87_CENSUS`: 127.3M x87 insns/s. FIP/FCS/FDP/FDS stores land
  once per contiguous x87 run at the flush points (**+7.46 fps F8**;
  under icount too since 2026-10-15, landing a pending FIP/FCS, and
  since 2026-10-16 FDS, from insn_start word 2 on a mid-TB exit). The
  FT0 write-back elision (+1.56 fps) died with the cache (`3eb1b2de10`).
  `XEMU_X87_REFUTE`: 2.06e10 checks, 0 violations. Clean-ST(i) elision
  (`XEMU_X87_ELIDE_CLEAN`) measured −0.23 fps and was removed (Failed /
  reverted experiments).
- **arm-(b) prefilter counters off the hot path (default off,
  2026-08-04).** The store stub's six counter RMWs are emitted only under
  `XEMU_SUBPAGE_FAST_STATS=1` (~39 insns per stub). Carrying them cost
  **−0.57 fps**.
- **Devirtualized TB-state extraction (default, no knob, 2026-08-04).**
  The four hot TB-lookup sites call `x86_get_tb_cpu_state` directly,
  guarded by a `__builtin_types_compatible_p` assert and an identity
  `assert()` in `tcg_exec_realizefn`. No individual fps claim.
- **Writable-clean TLB bitmaps for `tlb_reset_dirty` (2026-10-16).**
  Each `CPUTLBDesc` keeps a whole-table bitmap and 64 host-page bucket
  bitmaps of armable entries, so a dirty clear no longer scans every
  entry under `tlb_c.lock` (59-60% → 6.3% of PFIFO cycles). bench-abx
  heavy-f1 48.0 → 59.1 fps. `XEMU_TLB_WCLEAN_REFUTE=1`: misses=0
  unset=0. Skipping the walk in `physical_memory_snapshot_and_clear_dirty`
  on a clean VGA snapshot was left out: an entry can go in writable-clean
  just after a clear.
- **No W^X flip per TB entry (Apple, both modes, 2026-10-15).** The vCPU
  stays in execute mode outside self-closing write brackets
  (`tb_gen_code`, `tb_add_jump`, `cpu_exec_longjmp_cleanup`), instead of
  `pthread_jit_write_protect_np(true)` per TB entry; `cpu_exec` keeps
  one flip per slice as a safety net. Det cold boot Mcycles/frame
  −2.07%; steady play ≤0.25%.
- **Guest memory-order barriers only where a device thread can see the
  order (2026-10-15).** icount drops every guest-access `dmb`
  (`30316da269`; det heavy-f1 97.0 → 103.1 fps); normal mode drops only
  the load-load ones (`60fc127871`; OHCI needs store order).
  `xemu_net_enable` restores all barriers. Explicit fences (`mfence`,
  locked instructions) are untouched. `XEMU_GUEST_MO=1` keeps every
  barrier (normal-mode switch: the goldens can't see device-thread
  ordering).
- **Hot/cold TB layout (2026-10-16).** On aarch64
  (`TCG_TARGET_COLD_CODE`) 1 MiB chunks pack TB bodies in their first
  quarter (`tb->tc`) and slow paths, pools and metadata in the rest
  (`tb->xemu_cold`), within branch range. Det heavy-f1 Mcycles/frame
  −7.7%, movie-e2 −10.9%. `-d out_asm` prints cold code as "tb slow
  paths", `XEMU_GUEST_PROF` attributes cold samples to their TB, and
  Linux perf maps cover bodies only. Keeping warm stubs beside the body
  cost 3-7%. A region tail short of a chunk becomes one more, shorter
  chunk when it is at least 256 KiB (unused, it left `-accel
  tcg,tb-size=2` runs 1 MiB of their 2).
- **Liveness and the optimizer visit only the temps in play
  (2026-10-16).** Liveness tracks live temps in bitmaps
  (`LivenessSets`), pass 0 runs inside `reachable_code_pass`, and
  `tcg_optimize` does one walk per op. Code identical over 174k TBs (on
  9bc470fc21). Det cold boot translation −4.0%; heavy-f1 Mcycles/frame
  −3.1%.
- **The PFIFO thread keeps pgraph.lock between commands (2026-10-24).**
  It keeps pgraph.lock across commands and retakes pfifo.lock by
  trylock; handlers still run with pfifo.lock free, and other
  pgraph.lock takers count themselves (`nv2a_pgraph_lock_ext`).
  `XEMU_PFIFO_LOCK_BATCH=0` vs default: l256_120 probe +2.8% frames a
  vblank. icount never batches.
- **The PFIFO thread runs leaf methods under both locks (2026-10-25).**
  It keeps `pfifo.lock` too while a command's calls are leaves:
  handlers that, with everything they call, take no lock or BQL, call
  no renderer op, kick nothing and set no stall flag (174 of 201,
  matched by handler pointer; `scripts/nv2a-leaf-audit.py` checks the
  list against the LLVM IR of everything they reach). Non-leaf
  handlers still run with pfifo.lock free. A run ends on a non-leaf, a
  counted waiter (every off-thread taker counts itself:
  `nv2a_pfifo_lock_ext`), 256 calls or a ring freeze; DMA_GET is still
  stored after each call. `XEMU_PFIFO_FUSE=0` vs default: probe +7.8%
  frames a vblank. icount never fuses. A 1024-word run cap gave no gain
  (both locks dropped after every 2047-word index run).
- **The vblank thread waits on one kqueue timer (2026-10-24, macOS).**
  One `NOTE_CRITICAL` kqueue timer to 25 µs before the deadline, then a
  spin, replaces `SDL_DelayPrecise`'s 1 ms sleeps (~13 wakeups a
  vblank): vblank thread 1.0% → 0.25% of a core. `XEMU_VBLANK_KQUEUE=0`
  keeps SDL's wait (det runs use the virtual vblank and never reach it).
- **A code-page write flushes the jump cache once, not once per TB
  (2026-10-24).** The page pass marks the jump-cache and near-return-memo
  flush owed and does it once at its end, before any TB runs on its
  thread. Det l256_120 probe Minsns/frame −1.25%.
- **tlb_set_dirty visits only dirty MMU indexes (2026-10-24).** A first
  store to a clean non-code page probed all 22 fast tables and 176
  victim entries, though the Xbox fills 2 indexes (~1% of vCPU busy on
  heavy-f1; no det A/B resolves it). `XEMU_TLB_WCLEAN_REFUTE=1` clean.

### Vulkan renderer (pgraph/vk)

- **MoltenVK compatibility.** `VK_KHR_portability_subset`,
  `VK_EXT_metal_objects` zero-copy IOSurfaces, CPU-side quads / line
  loops / triangle fans / provoking vertex, fragment-shader depth
  fallback.
- **Vulkan driver choice (macOS).**
  `display.vulkan.driver = auto | kosmickrisp | moltenvk`, latched per
  process. Auto and `moltenvk` run the bundled MoltenVK. `kosmickrisp`
  (opt-in) runs Mesa KosmicKrisp on macOS 26 with a Metal 4 GPU and
  Metal presentation once it passes acceptance (copy-present, every
  surface and texture format), else MoltenVK. `-icount` and offscreen
  runs pin MoltenVK. KosmicKrisp failed C2 at the v0.16 gate; Auto may
  return to it once KosmicKrisp on Mesa 26.3.0 final or later passes the
  Auto-back gate, C1 and C2 included. A KosmicKrisp fatal error, or a
  crash in its start or first 600 flips, leaves a strike that falls
  back to MoltenVK until the build changes or KosmicKrisp is picked
  again. One pipeline cache per driver; driver paths test the driver
  ID, never the setting; copy-present engages on capability
  (`VK_EXT_external_memory_metal` without `VK_EXT_metal_objects`).
  `docs/kosmickrisp.md`.
- **CPU triangle fans on MoltenVK too.** `emulate_triangle_fans` is on
  for every Apple driver: fans go out as CPU-built triangle lists
  through draw.c's emulated-index path instead of MoltenVK's per-draw
  compute conversion, which restarts the pass (heavy-f1: 101 → 38 Metal
  encoders a flip). Windowed at the cap: heavy-f1 process CPU 119.9 →
  109.4% of a core. Standing stream-e2 f300 moves `69490c302ed37f1a` →
  `dc7c25c4a00d5723` (19 occlusion values off by 1-4, AGX pass splits);
  other RAM and null goldens unchanged.
- **Descriptor / pipeline.** Split sets (UBOs vs textures) with
  last-bound tracking; coalesced writes; dynamic state cached per CB.
  The pipeline key excludes dynamic state and `NV_PGRAPH_CONTROL_3`,
  and holds derived `RasterState`, not raw registers (stream-e2 walk
  172 → 117 pipelines). `check_pipeline_dirty` ignores
  `texture_bindings_changed`; `pgraph_vk_update_descriptor_sets`
  handles rebinds. It compares against the previous draw, so a bound
  clear pipeline reads as dirty and the next draw's keyed lookup
  rebinds its own pipeline.
- **Flight slots (N=2) + single-submit fast path.** CPU records slot 1
  while the GPU runs slot 0; aux CB skipped when staging is empty.
- **Texture upload.** Batched bump-allocator staging; CPU decode pool
  capped at `MIN(nproc, 4)`; compute unswizzler for 2/4-bpp textures.
- **Dirty tracking.** Per-chunk XXH3 on page-aligned textures ≥ 256 KiB
  (30-60% less hash CPU on sparse updates); UBO `memcmp` before
  `memcpy`; surface expiry throttled to ~33 ms wall-clock.
- **`possibly_dirty` cleared after verified bind.** Upstream left the
  mark sticky, re-hashing every bind forever; guest writes re-mark via
  the spatial index, and torn-snapshot binds keep it. Dirty-check +
  hash ~2.1x faster (0.6-1.6 ms/flip).
- **VkPipelineCache persisted across runs.** `<base>pipeline_cache.bin`
  (`pipeline_cache-kosmickrisp.bin` under KosmicKrisp) was saved only
  in `pgraph_destroy()`, which a normal quit never reaches. Now flushed
  at flips (write-then-rename): worst pipeline create 164 ms cold → ≤ 4
  ms warm.
- **Pipeline-cache saves off the PFIFO thread, sooner, and only when
  the cache grew.** Creations chain `VkPipelineCreationFeedback` and
  count only misses; a background thread saves after 16 new entries or
  a load burst plus 1 s without a miss (≥ 5 s apart, else 30 s); exit
  saves only unsaved entries. An 11.4 MB save (13-19 ms) no longer sits
  on a flip. The save thread keeps the PFIFO thread's QoS: at utility
  QoS a save took 48-228 ms.
- **`XEMU_NV2A_NSPROF=1` wall-time profiler.** `nv2a/nsprof.[ch]`:
  release-safe ns accumulators around every PFIFO-thread cost center,
  plus counters for finish reasons, draws/flip, `vk_draw_call`,
  `vk_fan_draw` and pass-end causes (`rpcause_*`); 5 s summaries to
  stderr. Off costs a cached env check per event. The draw-merge census
  (`038f596332`) was removed.
- **Deferred-report idle budget cut 5 ms → 300 µs.** Guests that spin
  on a zpass report waited up to the budget for the fallback submit.
  770 draws/flip scene: +5.5% fps. `REPORTS_IDLE_BUDGET_NS`.
- **Vertex-mirror overwrite waits the just-submitted slot.** The
  `VERTEX_BUFFER_DIRTY` conflict memcpy raced the GPU under flight
  slots; it now waits that slot's fence. fps parity.
- **Invalid-surface destruction gated on submission retirement.**
  Evictions carry their last submission index; a fence watermark gates
  `vkDestroyImage`, and `pgraph_vk_surface_flush` drains all slots.
  Reuse needs no gate (see `get_any_compatible_invalid_surface`).
- **Texture/sampler eviction gated on submission retirement.** Both
  caches refuse eviction until `retired_submit_count` covers the
  entry's last submission, with a fence drain if a pool fills with
  pinned nodes. fps parity, 291-capture soak clean.
- **Zeta shape-switch fast path.** A zeta ping-ponging between two
  shapes cost ~9 finishes a flip (8-19 ms). The guest clears re-shaped
  zeta before use, so the switch skips readback and seed upload and
  defers eviction; CPU-requested downloads are still honored.
- **Byte-exact vertex-conflict refinement.** Per-page written spans +
  memcmp skipped byte-identical conflicts: forced finishes 5.65 → 0.37
  a flip, fps +5.4% (the windowed A/B, `scripts/bench-abx.sh` today).
  Superseded by read spans; `uploaded_page_content_differs` went with
  `XEMU_VTX_SPAN_FINISH=0` in v0.16.1.
- **Targeted vertex-RAM conflict wait.** A write into pages of an
  already-submitted slot waits only that slot's fence; the full finish
  stays for the recording CB. With the zeta fast path: medium scenes
  22-26 → 60 flips/s, fence waits 8-19 → 1-3.5 ms a flip.
- **Once-per-frame texture verification.** A binding verified earlier
  in the frame skips the dirty scan + hash on re-bind (~85% of binds).
  A CPU write between two binds shows one frame later; render-to-texture
  is unaffected (surfaces don't take this path).
- **No hidden GL contexts under the Metal backend.** The 4 startup
  gloffscreen contexts are skipped; switching away from Vulkan waits
  for the next launch.
- **Texture VRAM spatial index.** `TextureBinding`s bucketed by 128 KiB
  so `pgraph_vk_mark_textures_possibly_dirty` scans only touched
  buckets, not all 65536 LRU bins.
- **MoltenVK configuration is launch-path independent (pink-tile
  fix).** `MVK_CONFIG_*` lived only in `Info.plist` `LSEnvironment`
  (Finder launches only); `main()` now sets it before MoltenVK loads
  (env overrides). `MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS` moved 2 →
  0: prefill corrupted streamed textures, enabled an AGX crash and was
  slower (46.3 vs 48.5 fps).
- **Maintained MoltenVK build** (`scripts/build-moltenvk.sh`): pinned
  commit, `-O3`, per-machine `-mcpu`, arm64-only, installed to
  `/usr/local/lib`, where `build.sh` prefers it and logs version +
  UUID. fps parity, half the size.
- **Crash fixes in the new query path.** A capacity guard submits
  before a slot's query partition runs out (the pool grew 1024 →
  4096), and a per-CB primer query keeps MoltenVK's visibility buffer
  attached.
- **Texture snapshot copy-until-clean.** A guest write mid-copy shipped
  a torn magenta frame; the copy retries until clean (bounded).
- **Snapshot robustness.** A failed load (USB topology drift) resumes
  the game and shows the error; unbound emulated pads report neutral
  input instead of aborting.
- **In-pass occlusion queries + deferred zpass reports (2.4x in
  report-heavy scenes).** The slot's query partition is bulk-reset at
  CB begin so queries run inside the pass, and reports ride the next
  submission instead of a submit + sync each. Heavy savestate 15.78 →
  38.57 fps, render passes 376 → 14 a flip.
- **Query-pool drain at slot reclaim.** Queries and reports drain when
  their slot's fence is reaped, so `pgraph_vk_finish` no longer waits
  on work it just submitted. `STALLED`, `REPORTS_FULL`, `FLUSH` and
  `QUERIES_FULL` still drain synchronously.
- **LRU free-list + true-LRU eviction.** `lru.h` keeps free nodes on
  their own list and `global` holds in-use nodes only: O(1) allocation,
  eviction at the true LRU entry.
- **Texture-upload CPU trims.** Post-snapshot re-hash skipped when no
  write landed during the copy; `TextureLayout` reuses one scratch.
- **Tight barriers.** Precise stage masks and byte ranges instead of
  `ALL_COMMANDS_BIT` / `VK_WHOLE_SIZE`; coherent flushes skipped.
- **Renderer-switch hardening.** AB-BA-safe lock order on GL↔VK
  toggle, atomic `flush_pending`, `pgraph.lock` dropped across the
  `framebuffer_released` wait.
- **Monotonic sync clock.** `pgraph_vk_sync` uses `QEMU_CLOCK_REALTIME`
  (see "Monotonic present-path clocks" below).
- **Fence-wait diagnostics.** Fence waits warn at 5 s and abort at 60 s
  naming the call site, instead of hanging on MoltenVK deadlocks.
- **Narrowed GPU syncs.** `pgraph_vk_upload_surface_data` fully syncs
  only for the active color/zeta binding or a recording CB;
  `invalidate_overlapping_surfaces` collects into a 64-entry stack
  array.
- **Narrowed remapped-attribute staging.**
  `remap_unaligned_attributes` copies only `[min_element..max_element]`
  and draws rebase via `firstVertex` / `vertexOffset`.
- **Draw-path dedup caches.** Vertex/index buffer binds, push
  constants, the vertex layout snapshot and `surface_binding_dim` skip
  on a match with the last CB-scoped value.
- **`surface_ranges` insert / remove.** `lower_bound` insert and cached
  `surface_range_slot` remove replace linear scans.
- **Shader-uniform pull gated on `shader_uniform_inputs_dirty`.** Every
  uniform-input writer sets it; `bind_shaders` skips
  `update_shader_uniforms` when clean.
- **Correctness.** Frame-interpolation state dropped on resize;
  screenshots use `GL_TEXTURE_RECTANGLE` on the IOSurface path;
  partial-channel clears update cached scissor + blend constants;
  PVIDEO flush uses `row_bytes * in_height`; GPU unswizzle emits WAR
  fences; `create_texture` hashes and uploads from one VRAM snapshot
  and re-checks the dirty bitmap (no magenta tear flashes).
- **Surface CPU-access-callback churn was measured — and it was the
  guest's TLB-flush source (instrumentation + dark mechanism; both
  since removed).** Each surface create/destroy ran a vCPU stop, a full
  all-mmuidx TLB flush and a jump-cache flush. The census
  (`XEMU_SURFACE_CB_STATS=1`) found 199.8 events/s against ~190 flush
  calls/s (0.95:1); the reuse pool
  (`XEMU_SURFACE_CB_REUSE=1`, `804bc5d87e`) measured −0.66 fps and was
  killed. The fix is `tlb_flush_host_range`. The census was removed
  2026-10-15; revert that commit to recount. HMP `info jit`'s `TLB full
  flushes` never counts on this guest (`NB_MMU_MODES=22`); use the
  partial + elided counters.
- **SPIR-V disk cache keyed on the shader compiler.** The directory is
  `spirv_cache_v<x.y.z>-glslang<major.minor.patch>`, so a glslang bump
  can't serve stale blobs. A same-version revision bump is still not
  distinguished.
- **That key now asks the linked glslang, not a header.** It comes from
  `glslang_get_version()`, since a Homebrew `build_info.h` could name
  the directory while the binary linked the subproject's glslang. The
  suffix is `glslang<major>.<minor>.<patch><flavor>`, byte-identical to
  the old macro, so warm caches survive; hosts without `build_info.h`
  used to key on `glslang-unknown`. `build.sh` warns when a subproject
  checkout drifts from its `.wrap`.
- **The SPIR-V cache stopped trusting what it reads.** From
  `818fe27828`. Files open via `qemu_fopen()` (non-ASCII `%APPDATA%`);
  stores write `<hash>.spv.<pid>.tmp` and rename, and `.tmp` files
  orphaned by a crash are swept once per process, when the cache
  directory is first resolved. Each blob is checked
  (size, magic, an instruction walk, a final `OpFunctionEnd`) before
  SPIRV-Reflect, and a reject is deleted and recompiled. The checks
  reject 99.85% of 2.4M truncations and Reflect errors on the rest;
  zero false rejects on 448 real modules. `debug_shaders` adds a `-dbg`
  suffix. Bypass: `perf.cache_shaders` off (`XEMU_SPIRV_CACHE=0`
  removed, `a922602c75`); validation is unconditional. Landing check:
  truncate a cached `.spv`; it must be deleted and regenerated.
- **glslang's built-in symbol tables warm at renderer init.**
  `pgraph_vk_init_glsl_compiler` compiles a throwaway `#version 450`
  shader on a short-lived thread. First in-game compile 64.2 → 2.5 ms;
  output byte-identical.
- **Guest-driven bounds checks are real in release, on every
  platform.** `nv2a_vk_bounds_check()` was `__builtin_unreachable()` in
  release. It is now an `abort()` naming the expression and
  `__FILE__:__LINE__` in every build (`debug.h`), at upstream
  `assert()`'s cost. Six table-index sites whose field is wider than
  the table now abort where they used to read neighbouring `.rodata`.
  Corpus-soaked (a cold-boot smoke over the disc corpus); no env hatch
  by design. Restore by editing that macro (`NV2A_VK_ASSERTS` governs only
  `nv2a_vk_assert`).
- **volk device-level dispatch on non-Apple.** `volkLoadDevice()` skips
  the loader trampolines (one `VkDevice`). Hygiene; the
  `XEMU_VK_VOLK_DEVICE=0` hatch was removed. Apple untouched.
- **`VK_KHR_external_semaphore_win32` is optional, not required.** No
  semaphore crosses GL interop; since `6e2accff54` it is not enabled.
  Windows only.
- **LRU eviction skips out-of-use nodes again.** `lru_try_evict_one()`
  regained `lru_is_node_in_use()`, matching upstream; a no-op here.
- **Query-capacity guard moved ahead of per-draw setup.** In
  `begin_draw` it ran after the framebuffer, descriptor sets and inline
  vertex/index data were built in the old slot; its finish moved to the
  next slot, so `begin_render_pass` read
  `framebuffers[framebuffer_index - 1]` with the index at 0. It now
  runs first in `begin_pre_draw` and finishes as `QUERIES_FULL`
  (counter `NV2A_PROF_FINISH_QUERIES_FULL`, nsprof
  `finish_queries_full`). `XEMU_MAX_QUERIES=64`
  shrinks only the occlusion pool (`54671ac359`); the report pool stays
  ≥ 4096 (`max_reports_in_flight`) and `REPORTS_FULL` has no shrink
  knob.
- **`nv2a_vk_assert()` is live in debug builds.** `9834214c58` had
  compiled ~80 checks out of every build. `NV2A_VK_ASSERTS` now follows
  `XEMU_DEBUG_BUILD` (`-DNV2A_VK_ASSERTS=1`/`=0` overrides); release
  code is byte-identical. Pre-enable fixes: `bf002d5fa8`, `a825c1ff07`.
- **Surface uploads stage in the current flight slot.**
  `pgraph_vk_upload_surface_data` staged in slot 0's partition
  whatever the slot, racing in-flight copies; it now stages at the top
  of the current slot's.
- **Texture VRAM-end checks are live in release.** Three guest-driven
  bounds in `vk/texture.c` are now `nv2a_vk_bounds_check`; the
  `check_texture_dirty` off-by-one is fixed (`<=`).
- **Deterministic reports stay lazy while hooks, traces and value stops
  read RAM.** Readers ask `nv2a_det_reports_sync_read` for their bytes
  instead of forcing eager delivery; `XEMU_TRACE_RECORD` reads RAM as
  is. Azurik d2: 89.5 → 56.7 Mcyc/frame (−37%), RAM `10b5a35aa66b`.
- **Vertex copies wait only for draws that read a changed byte.** Each
  flight slot keeps the read spans of its draws; waits happen only when
  a copy changes a byte they read (nsprof `vtx_wait_skip`). Bench-det
  heavy-f1 wall fps +41-49%; RAM identical.
- **Read spans decide vertex finishes too.** The recording CB finishes
  only when a copy changes a byte its draws read (nsprof
  `vtx_exact_skip`). `XEMU_VTX_SPAN_FINISH=0` was removed in v0.16.1.
  Same RAM (heavy-f1 600 `adcf86bd78fd71f9`, stream-e2 600
  `3b986ed5d3acf666`); ≤ 29 pixels move ≤ 2 levels (tag
  `archive/vtx-read-span-finish`). Heavy-f1 Mcyc/frame −31%.
  `XEMU_VTX_SPAN_REFUTE=1`: 0 violations.
- **Up to four read spans per page.** `VTX_PAGE_SPANS` (4) spans a page
  stop gap writes counting as changes. Det heavy-f1 vertex finishes ~17
  → 2 a flip, Mcyc/frame −16% vs one span; RAM identical.
- **Det zeta shape switches clear in the command buffer.** Under icount
  a re-shaped zeta still uploaded from RAM (`fa860ee510`). Both modes
  now mark the discarded zeta initialized, and under icount the CB
  clears it to depth 1.0, stencil 0 (`clear_discarded_zeta`). RAM can
  differ from older builds where the guest leaves part of a re-shaped
  zeta unwritten and that part is read or written back (a CPU access,
  savevm, `xemu-checkpoint-save`, a `gpu-sync` RAM dump); anchors equal
  (heavy-f1 f300 `e1e78c2b7aafcdab`, heavy-w2 600 `14af161a59baea25`).
  Det heavy-f1 Minsns −0.99%; normal mode unchanged.
- **Clears stay in the open render pass** (default on;
  `XEMU_VK_CLEAR_IN_PASS=0` restores a pass per clear). A clear ends
  the occlusion query, never the pass, unlike upstream `ca42f0f2df`;
  the primer query is the only first-pass guard left. nxdk FPU text
  frames 1700 → 2 passes a flip; M2MF no longer hits
  `kIOGPUCommandBufferCallbackErrorOutOfMemory`. nsprof `clears`.
- **Whole pages skip the read-span search.**
  `pgraph_vk_note_vertex_ram_reads` stores a whole page's span
  directly. l256_120 probe Minsns/frame −1.40%.
- **The method handlers keep each batch's index range.**
  `pgraph_note_inline_elements` folds min/max as indices are appended.
  Probe Minsns/frame −1.7%.
- **Index batches upload as u16 when no index can be 0xFFFF.** 0xFFFF
  is the u16 restart value Metal can't disable; batches pad to an even
  count, emulated fans stay u32. Probe Minsns/frame −1.1%; with the two
  above, 0.573 → 0.605 frames a vblank.
- **Point size and params are uniforms, not shader key fields.** The
  size is the `pointSizeReg` uniform (not `pointSize`, the
  fixed-function prelude's attribute v6), so animated sprite sizes stop
  building pipelines; SET_POINT_PARAMS marks uniforms dirty.
- **Pipeline prewarm.** Missed draw-pipeline keys go to
  `<base>vk_prewarm-<driver>-<schema>.bin` (up to 2048, newest first,
  appended at flips once misses settle); a `vk.prewarm` worker rebuilds
  them at launch and destroys each, warming the driver and disk caches.
  It yields while the renderer compiles, never runs under icount and
  draws nothing. Schema from `scripts/nv2a-key-schema.py`; a launch
  that dies mid-replay sets its manifest aside. Off with Settings >
  Cache shaders to disk. Cast probe (`scripts/cast-probe.py`): first
  cast 198 → 47 ms.
- **dsp56300 0.1.3 → 1.0.0, the DSP engine of hosts without the fork's
  JIT.** Windows, x86_64 macOS, Linux, and arm64 with
  `[audio.dsp_jit] enabled = false` run upstream 1.0.0. Det heavy-f1
  −3.50% Mcycles/frame; goldens equal. Translation costs ~2× (worst
  post-load GP frame 8.5-12.5 ms); a load resets the core before
  set_state.
- **The prewarm runs on up to 4 workers.** 4 on Macs with 16+ cores,
  2 on 8-core ones. 226 cold pipelines 10.4 → 6.6 s. Parallel
  first-use compile read 1.68× (3× needed), not built.
- **2.8 GB less preallocated Vulkan buffer memory.** On unified memory
  inline-vertex and uniform data bind in staging, dropping their
  device twins. Inline-vertex staging is 256 MiB a slot (was 1280) and
  grows after a GPU drain (`pgraph_vk_grow_staging`); surface-download
  staging starts at 16 MiB (was 512) and grows on demand
  (`pgraph_vk_reserve_staging_dst`). Footprint 5303 → 2490 MiB.
- **Uniform pulls compare packed arrays in one memcmp.** `uniform_copy`
  compares and copies tightly packed arrays as one run, not a vec4 at a
  time. Det heavy-f1 Mcycles/frame −2.44%.
- **One command buffer per finish on unified memory.** The index twin
  aliases staging and coherent mirrors record no barrier, so no aux CB.
  Footprint 2490 → 2287 MiB.
- **Constant uploads compare and store whole rows.**
  SET_TRANSFORM_CONSTANT handles aligned 4-word rows at once. Probe
  Minsns/frame −1.92%.
- **The pipeline-cache save waits for a quiet second.** The 30 s
  fallback also waits 1 s without a miss (forced after 120 s).
- **Texture descriptor sets are reused while their handles match.** A
  draw reuses the set its CB last wrote when the view and sampler
  handles match. Probe sets written a flip ~1,740 → 176; det heavy-f1
  Mcycles/frame −2.07%. nsprof
  `tex_set_write`, `tex_set_reuse`.
- **Remapped vertex attributes copy in fixed sizes.** Sizes 2, 4, 6, 8,
  12 and 16 copy with a constant size, not a runtime `memcpy`. Det
  heavy-f1 Mcycles/frame −1.73%.
- **Read spans skip pages already read whole.** A per-slot
  `whole_bitmap` skips them a word at a time. Probe Minsns/frame
  −2.06%.
- **NV09F surface blits copy on the GPU.** SRCCOPY between two
  distinct, unswizzled same-format colour surfaces is a
  `vkCmdCopyImage`, not a CPU round trip; Y8, X8, swizzled and uncached
  blits keep the CPU path. Pending CPU writes upload and pending zpass
  reports deliver first. Gated by `perf.optimizations` (Settings >
  General, read at start). Since `f4762721d0`: same guest colour format
  on both sides, integral scale factors only, `xemu_events_gpu_write`
  like the CPU path. No Azurik scene takes it.
- **Async first-use pipeline compile** (Better-xemu4macOS, on by
  default; `vk/async.c`). A pipeline-cache miss compiles on a worker and
  its draw is skipped until the build lands (sync after 200 ms). Since
  `507db5fee6` a miss builds in line when the prewarm or this process
  already built the key, while the guest isn't flipping (loads, boot)
  and while zpass counting is on; nsprof `async_skip`, `async_warm`,
  `async_idle`, `async_zpass_sync`. cast-probe (fire-armor, windowed,
  load 72-105): cold first cast worst gap 152 ms on vs 164-180 off, the
  frame after the load 266-271 vs ~302 ms; warm, no hitch and no skipped
  draw either way (223 of 223 misses warm in line). Azurik keeps an
  occlusion query open for most draws, so 198 of 224 cold misses build
  in line and ~100 draws a cold run are skipped.
- **Fractional surface scale** (`display.quality.surface_scale` 0.25,
  0.5, 0.75; validated by `pgraph_valid_surface_scale`). On the M2 Ultra
  heavy-f1 holds the 60 fps cap at 0.5, 1 and 2 with flat PFIFO headroom
  (flip_idle 12.0-12.3 ms) and GPU 4-9%: no gain here; it is for weak
  GPUs (Intel Macs).

### MetalFX + presentation

- **Metal-native presentation backend** (macOS).
  `display.window.presentation_backend = auto | opengl | metal`; `auto`
  picks Metal with the Vulkan renderer. The window is `SDL_WINDOW_METAL` +
  `CAMetalLayer` (`ui/xemu-metal.m`); the NV2A frame arrives as an
  IOSurface/`MTLTexture` (`nv2a_get_present_frame`, a new
  `get_present_frame` renderer op) and is composited with an MSL gamma
  blit, with no GL blit, `glFlush` or CGL rebind. ImGui renders through
  `imgui_impl_metal`; the UI has a Metal twin (`ui/xui/metal-helpers.mm`).
  `opengl` stays selectable and is the only path for the OpenGL
  renderer. Switching the renderer away from Vulkan under Metal prompts
  for a restart.
- **Push-model present handoff** (default on; Metal backend). At flip
  (`pgraph_vk_flip_stall`) PFIFO publishes the present tuple into a
  mutex-guarded ring that also carries interpolation's paced steps; the
  UI reads it with no kick or wait and skips an unchanged `frame_seq`.
  `present_wait` 81.2 ms per interval → 0 on F5, flips/s identical;
  refuter `XEMU_PUSH_PRESENT_REFUTE=1`. A bounded step debt (interp
  mode + 2) stops dropped steps when the step rate beats the display
  (`78c4573dfb`). GL, headless, startup and resize use the pull path.
- **Async MetalFX via `MTLSharedEvent`** (Metal backend). The three
  `waitUntilCompleted` stalls (1-5 ms/frame on PFIFO) become a
  shared-event signal the UI pass waits on GPU-side. GL keeps the waits.
- **Async `render_display`** (Metal backend). No
  `vkWaitForFences(aux_fence)` on PFIFO: a timeline `VkSemaphore`
  exported as an `MTLSharedEvent` (`VK_EXT_metal_objects`, sync fallback)
  orders MetalFX and the UI pass.
- **Dedicated compositor command-buffer ring.** The compositor's own
  2-deep CB ring replaces the shared aux CB and fence, whose reclaim cost
  1.4-1.9 ms/flip in heavy scenes (now ~0.2 µs). Uploads never wait on it.
- **IOSurface-free present chain** (Metal backend). When MoltenVK exports
  the compositor `VkImage`'s `MTLTexture`, MetalFX and the UI use it
  directly, so MetalFX engages at `surface_scale = 4`. IOSurface stays the
  fallback and the GL path.
- **Frame interpolation quality.** Interpolated frames show before the
  real frame they lead to, paced at `frame_period / mode`; a `nil` motion
  texture (MetalFX estimates motion); `fieldOfView`/`aspectRatio` and
  synthetic depth set; temporal mode links the interpolator to the scaler
  (`MTLFXFrameInterpolatableScaler`); 4x re-presents the cached midpoint;
  a hitch guard resets history on frame-gap spikes.
- **1920px output cap lifted** (Metal backend). MetalFX outputs rotate
  through rings of 3 private `MTLTexture`s, which avoids the macOS 26 BGRA
  `bytesPerRow` IOSurface bug, so output targets the `CAMetalLayer` pixel
  size (2880×2160 on a 4K panel).
- **Spatial + temporal upscaling** (`MTLFXSpatialScaler` /
  `MTLFXTemporalScaler`). Temporal gets a zero jitter offset (pgraph
  renders unjittered) and optional synthetic depth. Shared, refcounted
  `MTLDevice` + `MTLCommandQueue`.
- **Frame interpolation** (`MTLFXFrameInterpolator`, macOS 26+): 2x = 60
  fps, 4x = 120 fps from 30 fps; deferred generation on idle syncs.
- **Teardown safety.** In-flight counters drain before release.
  IOSurface cache keyed on `IOSurfaceGetID`; GL backend output capped at
  1920 wide (macOS 26 BGRA `bytesPerRow` bug).
- **`MTLFXFrameInterpolator.deltaTime` in real seconds**
  (`QEMU_CLOCK_REALTIME`, clamped `[1/240, 1/10]` s): less ghosting.
- **Exclusive fullscreen picks the highest refresh** among
  native-resolution modes. Metal vsync maps to
  `CAMetalLayer.displaySyncEnabled`.
- **GPU-paced interpolation steps.** The UI presents with
  `presentDrawable:afterMinimumDuration:` and skips unchanged frames in
  gameplay, for exact `frame_period / mode` steps. Menus render at full
  rate.
- **Monotonic present-path clocks.** The present path uses
  `QEMU_CLOCK_REALTIME`, not `QEMU_CLOCK_HOST` (gettimeofday). The
  guest-vblank IRQ timer is its own fixed-60 Hz thread.
- **Opt-in real depth for temporal** (`XEMU_MFX_REAL_DEPTH`). `2` blits
  the display-matching NV2A zeta at FLIP_STALL into an exportable snapshot
  for the temporal scaler, falling back to synthetic depth when the
  format is rejected or none matches the scaler's input dims (e.g.
  Azurik's 2x super-width 3D). Dark; mode `1` was removed.
- **MetalFX resize quantization (default on, 2026-08-04).** Output width
  snaps to a 64-px ladder and must hold ~200 ms before the scaler
  rebuilds (each rebuild blocks PFIFO in `metalfx_drain_inflight()`); a
  guest mode change rebuilds at once. Scripted drag: 90 → 7 rebuilds
  (`mfx_scaler_rebuild`).
- **UI-thread present instrumentation (2026-08-04).**
  `XEMU_NV2A_NSPROF=1` times `drawable_acq`, `ui_hud_lock`, `ui_frame_dt`
  and counts `ui_present`, `mfx_scaler_rebuild`; its numbers closed the
  ImGui-under-lock rework (Settled vectors). `XEMU_UI_LOCK_STATS` was
  removed 2026-10-23; a `DEBUG_XEMU_C=1` build prints the lock line.
- **Interpolate only as many steps as the display can show
  (2026-10-16).** The configured mode runs only while the guest frame
  period holds `mode` refreshes (on from 0.9x, off below 0.75x); else
  2x, else none.
  Steps hold whole refreshes (`present_hold_ns`). Against
  `XEMU_INTERP_GATE=0`: heavy-w2 58.49 → 59.93 fps. nsprof:
  `interp_gated`, `interp_degraded`.

### MCPX APU

- **Voice processing.** The 128-byte voice struct is copied to the stack
  and `voice_get/set_mask` hit the copy; paused voices skip it. LUTs
  replace transcendentals: attenuation (4096), pitch (65536), LPF cutoff
  (65536), envelope decay / release (1024 each).
- **Batched voice register reads.** VBIN / FMT / VOLA / VOLB / VOLC are
  one word-load each, replacing ~14 `voice_get_mask` calls per voice per
  frame; same in `voice_get_samples` and `get_voice_bin_src_dst`.
- **Mono LPF skip.** The DLS2 SVF filters ch 0 of a mono voice and
  mirrors it: half the SVF cost.
- **HRTF FIR linearization.** `hrtf_filter_process` linearizes the
  circular history once per frame, so the 31-tap convolution has no
  per-tap `% HRTF_BUFLEN`; a hand-NEON pass runs over outputs. Other
  hosts keep the C forms.
- **SSE2 mix kernels (non-Apple x86_64).** SSE2 mixbin accumulate,
  `float_accumulate` and `float_to_24b_bulk`, saturating like NEON/lrint.
- **SVF coefficient cache.** `setup_svf` returns early when
  `(fc, q, filter-type)` is unchanged.
- **Mono libsamplerate.** A 1-channel `src_callback_new` for mono
  voices halves the resampler loop.
- **ADPCM block reuse.** Each voice keeps its last decoded block
  (`MCPXAPUVoiceFilter.adpcm`) and decodes only when bytes, size or
  channel count differ; short blocks decode over zeros. Det instructions
  per frame −0.14% heavy-f1, −0.42% stream-e2.
- **Resampler loops per channel count.** `linear_src_callback_read` takes
  a constant channel count; `voice_src_read` has mono and stereo copies.
  Det instructions −0.27% heavy-f1.
- **No utilization clock in deterministic mode.** `se_frame` skips its
  clock reads unless `XEMU_APU_PROF` is set: ~4k fewer instructions per
  APU tick.
- **Accelerate / vDSP.** A fused loop (Apple Silicon; `vDSP_vsma` on
  Intel) for 8-bin × 32-sample mix accumulation, `vDSP_vadd` for
  `float_accumulate`, bulk `vcvtnq_s32_f32` in `float_to_24b`. The
  `SRC_LINEAR` resampler matches Xbox hardware.
- **Atomic consistency.** `d->regs[]` writers, `voice_locked[]`,
  `pause_requested` and the lock-free VP and `fe_method` register reads
  use `qatomic_*`, so the VP frame thread and the lock-free MMIO
  dispatcher agree under weak ordering.
- **DSP execution engines (post upstream merge).** Upstream has the C
  interpreter (`dsp/interp/`) and a JIT around the `dsp56300` Rust
  subproject (`audio.use_dsp_jit`, default on; upstream flipped it off in
  `fc13b78060`, the fork turned it back on 2026-09-08). The fork's ARM64
  JIT lives inside the interpreter (`interp/dsp56k_jit_arm64.c`) and
  takes precedence when `audio.dsp_jit.enabled` (default true): AArch64
  POSIX hosts run it, others dsp56300. Without the AArch64 emitter, a
  portable LLVM backend (`interp/dsp56k_jit_llvm.c`, from
  Better-xemu4macOS) builds when libLLVM is found; its blocks call the
  interpreter, so it runs only with `audio.use_dsp_jit = false`.
- **Full inline DSP JIT (Apple Silicon).** ARM64 basic-block JIT for the
  GP and EP. `[audio.dsp_jit] enabled = false` (or the Settings toggle)
  is the only switch.
- **Per-core retranslation auto-throttle.** A core retranslating more
  than 1/16 of block executions goes back to the interpreter; neither
  core trips it. The EP ran on the interpreter from `d287c6cb23` until
  blocks could overlap.
- **JIT coverage + mechanics.** 100% ALU and 99.99% CF inlined;
  A/B/X/Y/SR pinned in callee-saved registers; static block chaining
  with retro-chaining, repatched inside the W^X window with an icache
  flush on unpatch; lazy flags. Harnesses: `XEMU_DSP_JIT_DIFF=N`
  (bit-exact validation), `XEMU_DSP_JIT_STATS=1` (counters). See
  [docs/Finished/dsp-jit-design.md](Finished/dsp-jit-design.md).
- **`dsp_core_t` reordered into the ARM64 imm12 window (2026-08-04).**
  The JIT-hot scalars moved ahead of `xram`, below offset 450, so
  generated code uses one `LDR`/`STR`. `QEMU_BUILD_BUG_ON`s in
  `dsp56k_jit_arm64.c` keep them there; `dsp_state_diff()`'s table changed
  with it. `code_buf_used` −12.79%, `XEMU_DSP_JIT_DIFF=1` failures 0.
  Open: moving `yram` (base 16836) ahead of `xram`.
- **One exit-word test per DSP op (2026-10-16).** The per-op interrupt,
  idle and exit checks share one 8-byte `jit_exit_word`, tested with
  `LDR X` + `CBNZ`; ops that cannot move pc store it from a constant. A
  typical in-loop epilogue is 11 instructions (was ~27). Det heavy-f1
  instructions −1.26%; `XEMU_DSP_JIT_DIFF=1` failures 0.
- **DSP DMA copy loops keep the scratch pointer in a register
  (2026-10-16).** `dsp_dma_run` copies through a local pointer,
  specialised on `item_size`. Det instructions −0.15% heavy-f1.
- **Guest voice handles bounds-checked; APU RAM dirty marking behind a
  per-platform default (2026-09-08).** From `971292ed48`. Voice accessors
  and the HRTF / SSL setters range-check guest handles (out of range:
  reads 0, writes dropped, one stderr report; no guest-RAM fallback,
  which would take the BQL under `d->lock`), and the voice-list walk
  stops on a bad `NEXT_VOICE_HANDLE` instead of asserting. The APU frame
  thread registers with RCU; the `audio.use_dsp_jit` switch takes the BQL
  around `dsp_set_engine()`. Voice-store dirty marking is a build-time
  default, off on Apple, on elsewhere (like `0aeb1998c3`). Not taken:
  `971292ed48`'s int16 clamp of the modulated pitch-LUT index (removed in
  `3495cd28c0`, whose bisect `40f0a01ad3` retracted).
- **DSP JIT dead code removed (2026-10-10).** Ten uncalled functions:
  seven ARM64 emitter primitives added unused in `1288fae90f`,
  `4dfbae90d7`, `5e40a982ef`, the `dsp56k_jit_sentinel_enabled` /
  `_force_enabled` accessors and the `dsp56k_jit_helper_update_rn` shim.
  `XEMU_DSP_JIT_SENTINEL` and `XEMU_DSP_JIT_FORCE` were removed
  2026-10-15 (harness in `1f444fa272`/`4668947cc9`).
- **Voices run on the APU frame thread (2026-10-16).** The four voice
  workers are gone: `voice_work_dispatch` runs the partitions in order on
  the frame thread, as det mode did, so the floats match.
  `audio.vp.num_workers` sets the partition count (default 4). Windowed
  process CPU −10.5 points heavy-f1; `XEMU_AUDIO_WAV` gapless.
- **Fewer fixed costs per APU frame (2026-10-16).** The debug snapshot
  runs only while the Audio debug window reads it
  (`mcpx_apu_get_debug_info`) or a voice is isolated, and a frame sums
  only the mixbins its partitions wrote (`bins_written`). Det
  instructions −0.70% heavy-f1.
- **DSP DMA copies RAM spans in bulk (2026-10-16).** `dsp_dma_run` tests
  for a plain X/Y RAM span once per transfer and copies it in bulk; P and
  peripheral targets stay per word. Det instructions −0.37% heavy-f1.
- **DSP JIT chains fall-throughs, calls, DO entries and loop rewinds
  (2026-10-16).** Block-ending ops chain to every static successor
  (`jit_static_succ`): GP dispatcher entries −83%, det instructions
  −0.77% heavy-f1. Chain entries stop at `dsp_run`'s budget
  (`jit_chain_limit`, four instructions), or a two-block poll hangs the
  APU thread (xbox suite `/corpus/budget-poll-*`). `XEMU_DSP_JIT_DIFF=1`
  turns chaining off; `XEMU_DSP_JIT_PIN_AUDIT=1` stays silent with it on.
- **DSP JIT blocks survive DSP resets and loads (2026-10-16).**
  `dsp56k_jit_revalidate_all` replaces the cache flushes: the next lookup
  compares each block's P words (`dep_words`) and evicts only changed
  blocks. 41 restores: 1848 / 3612 GP / EP translations → 44 / 86 (xbox
  suite `/corpus/budget-replay`).
- **A voice unlock wakes the frame thread only when it waits for one
  (2026-10-18).** Only an unlock with `vp.unlock_waiter` set signals
  `d->cond`. Contended `vp_write` locks 10,415 → 187 per 30 s; det
  heavy-f1 cycles −1.1 to −2.3%.
- **Upstream's DSP engine gets DMA spans and the bulk mixbuffer copy
  (2026-10-18).** The span hook (`b0c863b4a5`) now covers dsp56300:
  `jit_dma_mem_span` indexes its X and Y RAM, and
  `dsp_write_mixbuffer_bulk` serves both engines. Det with
  `[audio.dsp_jit] enabled = false`: heavy-f1 cycles −3.06%,
  instructions −5.30%.

### Input

- **XID pad `index` is bounded before it reaches `bound_controllers[]`
  (2026-09-08).** `xid_get_bound_controller()` (`hw/xbox/xid.c:53`)
  returns NULL for an index past the 4-slot array, and realize rejects
  one via `error_setg`, so `-device usb-xbox-gamepad,index=4` fails
  loudly. The app only binds 0..3.
- **In-app rebinding UI polish** (Input tab → Input Mapping). Esc
  cancels a capture, right-click unbinds, every Xbox control has a
  keyboard binding, and per-stick inversion toggles sit under the table;
  per controller (SDL GUID), saved to `input.gamepad_mappings` /
  `input.keyboard_controller_scancode_map`.

### Threads + runtime

- **Windows main loop sleeps on a high-resolution waitable timer
  (2026-10-18).** `g_poll` takes milliseconds, so `qemu_poll_ns`
  busy-waited every sub-millisecond deadline. A
  `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION` handle (Windows 10 1803+) joins
  the poll set, as the kqueue timer does on macOS, and only the last 50 µs
  spin. Process CPU 245.9% → 147.5% of a core.
- **Minimized on Windows, the UI loop waits a vblank (2026-10-18).** On
  `DXGI_STATUS_OCCLUDED` the loop sleeps a vblank interval instead of
  spinning. Process CPU 176% → 158%; the guest vblank stays 60.4/s.
- **Memory-access callback list is RCU-protected (2026-10-11).** Fixes
  the SIGSEGV in `flatview_read_continue` under `ohci_frame_boundary`,
  most often after a `loadvm`: since upstream xemu `966115336a`, NV2A
  surface callbacks leave `cpu->mem_access_callbacks` with the BQL
  dropped, freeing nodes under main-loop DMA walkers. The list uses the
  RCU variants and `g_free_rcu` (`13afd5f244`), and `qemu_main` is an RCU
  reader (`4161ab5f24`).
- **P-core QoS.** The PFIFO, vblank-timer, vCPU (det RR thread; MTTCG
  since `94a722acd8`), main-loop (`qemu_main`) and APU frame threads
  request `QOS_CLASS_USER_INTERACTIVE`. vCPU: windowed heavy-f1 37.2 →
  58.3 fps. Main loop and APU thread hold the BQL / `d->lock` and were
  preempted inside it under load; their QoS cuts the vCPU's lock-wait
  tail, fps at the cap unchanged. `XEMU_VCPU_QOS=0` leaves the vCPU and
  main loop at the default class; the APU thread has no switch.
- **Null mode takes the app role UI (2026-10-17).** `XEMU_HEADLESS=null`
  starts no Cocoa app, so xnu squashed its QoS threads to priority 31 and
  E-cores. `xemu_os_set_app_role` calls
  `setpriority(PRIO_DARWIN_ROLE, 0, PRIO_DARWIN_ROLE_UI)`; a launchd
  daemon gets ENOTSUP. bench-det heavy-f1 82 → 213 fps at load 71-96;
  bench-det numbers from before this change are not comparable with
  later ones. No switch: a scheduling hint, verified by the goldens.
- **Normal mode halts the vCPU in the kernel's idle loop (2026-10-16).**
  KiIdleLoop never executes HLT and was 35-67% of the windowed vCPU.
  `helper_xemu_ki_idle` spins 50 µs of host time per idle span, then
  halts and wakes through the stock kick. Process CPU at the cap: heavy-f1
  179 → 153%, W2 161 → 125%. Heavy GPU contention from other processes
  makes transitions hitch more. 0 µs, 200 µs and rehalt-after-wake
  variants were rejected. `XEMU_KI_IDLE_HALT=0` keeps the spin.
- **The normal-mode vCPU spins for a contended BQL (2026-10-16).**
  `bql_lock_impl` polls trylock every 200 ns (`cntvct_el0`) for up to
  10 µs on the MTTCG thread (`bql_spin_enable_self`), then sleeps. The
  vCPU's contended-acquire time 2.12% → 0.52% of wall; process CPU flat.
  `XEMU_BQL_SPIN=0` keeps the plain lock; the RR thread never spins.
- **Deterministic IDE requests settle without waking the main loop
  (2026-10-16).** `aio_wait_kick` skips its bottom half when every waiter
  is in the calling thread's own `aio_poll` (`aio_wait_self_waiters`),
  and IDE accesses open a no-notify window (`ide_bus_icount_begin`).
  `bench-det --timeline` null cold boot 14.15 → 9.47 s (−33%).
- **Deterministic mode's FP brackets write FPCR only when it differs.**
  `hw/xbox/host_fp.h` swaps FPCR around device work on the vCPU only when
  it differs from the default, and leaves FPSR alone: ~75 cycles per
  pusher run. x86_64 hosts do the same with the x87 CW and MXCSR (CW
  `0x37f`): det heavy-f1 instructions −0.58%. Other hosts keep `fenv`.
- **BQL discipline.** `surface_access_callback` drops the BQL before
  blocking so the vblank thread can still raise interrupts.
- **PFIFO wait.** Untimed `qemu_cond_wait`; kick-check + release-wait
  under `d->pfifo.lock` means no missed wake-up. Normal mode's DMA_PUT
  doorbell skips the lock (`pfifo_put_doorbell`, next entry).
- **Lockless DMA_PUT doorbell in normal mode (2026-10-15,
  `3391b92357`).** PUT is stored with release, and a doorbell flag pairs
  with the PFIFO thread's parked flag (full barriers): either the vCPU
  sees the thread parked and kicks it under the lock, or the thread sees
  the doorbell and reruns the pusher. USER reads skip the lock too; det
  keeps the locked path (`kick_seq` feeds `pfifo_icount_sync`). Windowed
  heavy-f1 35.3 → 42.3 fps. `XEMU_PFIFO_PUT_LOCK=1` restores the lock.
- **No doorbell wake inside a flip stall (2026-10-16).** With
  `waiting_for_flip` set, READ_3D == WRITE_3D and PUT != GET,
  `pfifo_put_doorbell` publishes PUT and the flag without waking: only
  the vCPU ends a flip stall. A re-rung PUT equal to GET still wakes, so
  FIFO-idle report delivery is unchanged. In-stall wakes heavy-f1 4246 →
  182/s.
  `XEMU_PFIFO_STALL_DOORBELL=0` restores the wake.
- **BQL-free MMIO dispatch for the hottest guest register blocks.**
  `XEMU_MMIO_PROF` put 78% of guest MMIO on PFB (`NV_PFB_WBC`) and USER
  doorbells; upstream's `lockless_io` is honored on TCG and the audited
  PFB / USER / APU-VP regions opt in. fps parity, steadier runs (stdev
  1.21 → 0.89).
- **PGRAPH joins the BQL-free MMIO set.** Per the audit
  (`docs/Finished/pgraph-lockless-audit.md`), `pg->lock` / `pfifo.lock`
  guard every shared access but the interrupt pair, now `qatomic`. fps
  parity. `XEMU_PGRAPH_MMIO_STATS=1` (removed 2026-10-10) found
  `NV_PGRAPH_INTR` the hottest register. The hatches
  `XEMU_PGRAPH_LOCKLESS=0` and `XEMU_MMIO_BQL=1` were removed 2026-10-15.
- **PFIFO kick census (counter only, 2026-08-04; removed 2026-10-15).**
  `XEMU_PFIFO_KICK_STATS=1` found 0 of 412,074 kicks suppressible
  (Settled vectors). Kicks are never suppressed (a wrong skip hangs
  PFIFO), except the doorbell inside a flip stall.
- **Leaner deterministic inline pusher (2026-10-15).** The vCPU's inline
  pusher (`pfifo_icount_sync`) gets its own loop copy
  (`pfifo_run_pusher_impl(d, true)`) without the threaded path's atomics
  and repeated checks; `pgraph_method` is `always_inline` under clang.
  Det heavy-f1 Minsns/frame −1.64% (22329bd8bc lane).
- **Fused per-command loop in the deterministic inline pusher
  (2026-10-16).** `pfifo_inline_command` runs a kelvin command's data
  words in one loop, checking PULL0, engine and class once per
  subchannel. Method 0, object methods, other classes and the threaded
  pusher keep the per-word path. Det heavy-f1 Minsns/frame −1.92%.
- **ARRAY_ELEMENT16 stores a command's indices in one loop
  (2026-10-23).** One bounds check per run
  (`len + 2n <= NV2A_MAX_BATCH_LENGTH`, which also fixes a one-past write)
  and one widening loop, shared by both pushers. Per-word work added to
  the handler later must loop over all n words. Null stream-e2 Mcycles
  −1.62%.
- **The det Vulkan pusher runs inline too (2026-10-19).** Offscreen det
  used to kick the PFIFO thread at every PFIFO / USER / PGRAPH write
  (null alone ran inline, `ed21e852cf`); Vulkan now runs on the vCPU too,
  GL keeps the thread. Each sync first waits until the thread has acted
  on every kick (`kick_seq` starts at 1, so the first waits for renderer
  init); each run ends with `pgraph_process_pending_reports`, and a
  report trap inside a run delivers in place (`pfifo_in_inline_run`).
  Offscreen RAM unchanged (heavy-f1 f300 `e1e78c2b7aafcdab`, f600
  `adcf86bd78fd71f9`, stream-e2 600 `3b986ed5d3acf666`, heavy-w2 600
  `14af161a59baea25`, pack 05 d2 `10b5a35aa66b`). Offscreen heavy-f1
  wall fps +72%. Windows offscreen det is unverified; normal mode is
  untouched.
- **Headless null mode scans VGA only for a screendump (2026-10-15).**
  `nv2a_vga_gfx_update` skips the scan under `XEMU_HEADLESS=null` and
  `screendump_vga` calls `nv2a_vga_refresh()` (a full redraw that clears
  no dirty bits) first; screendumps are byte-identical. Saves 0.12%
  Minsns/frame on top of `bd02c7c745`. Windowed and offscreen modes
  still scan every vblank.
- **PTIMER zero-divisor guard (2026-09-08).** The `NV_PTIMER_ALARM_0`
  handler (`d73326b621`) divided by `ptimer.numerator`,
  `ptimer.denominator` and `pramdac.core_clock_freq`, all zero early in
  boot. `ptimer_clock_configured()` guards all three: conversions return
  0, and an `ALARM_0` write before the clock is set deletes the timer.
  No vmstate or struct change, so old snapshots are unaffected. Reasoned
  from the code; the fault was not reproduced.

### Build + packaging

- **Compiler flags.** `-O3`, thin LTO with caching, `-ffp-contract=fast`,
  `-mcpu` auto-detected; STBI_NEON and fpng CRC32 on ARM64.
- **PGO shipped for arm64 release builds.** CI applies the committed
  `pgo/default.profdata` (the four Azurik bench scenes, ~75 s each) to the
  macOS arm64 release legs: **+9.4%** fps on the 770 draws/flip scene.
  x86_64 stays plain; `build.sh` fails if the profile is missing. Retrain
  per the build-knob table. Manual flow: `XEMU_PGO=generate` → run →
  `XEMU_PGO=use` (one shared `setup_pgo` for macOS, Linux and MSYS2).
- **PGO builds report profile staleness (2026-08-04).** `XEMU_PGO=use`
  prints CFG-hash mismatches in the hot set (`cpu_exec*` / `helper_*` /
  `tlb_*` / `tcg_*` / `pgraph_*`) and the commits since the retrain on
  `accel/tcg tcg target/i386 hw/xbox`. CFG hashes miss straight-line
  edits; the commit count misses uncommitted ones.
  `XEMU_PGO_STALE_FATAL=1` is gone (2026-10-23). Retrain before tagging
  (precedent `46b15d19b4`).
- **GCC PGO on native Windows (2026-09-08).** Re-cut of `fb42bce22e`:
  `pgo_compiler_family()` asks the compiler (`cc -dM -E -`, testing
  `__clang__`). GCC gets `-fprofile-generate -fprofile-update=atomic`,
  then `-fprofile-use -fprofile-correction -fprofile-partial-training
  -Wno-missing-profile -Wno-coverage-mismatch`, with a `cygpath -m`
  profile dir. A `.gcda` presence check names a wrong-family profile
  instead of building unprofiled, and the staleness parser reports "not
  implemented" on GCC. The native GCC PGO run is in gating audit §T D7.
- **x86_64 Windows builds default to `-O3` (`6a58566a54`, 2026-10-18).**
  `build.sh` adds `-Doptimization=3 -Dstack_protector=disabled` for
  x86_64 only; arm64 (llvm-mingw) stays at `-O2`. Native det heavy-f1:
  core cycles −1.4%, instructions −1.1%. `XEMU_WIN_O3` is gone. The
  stack-protector option is not a mingw no-op: `meson.build:512-536`
  probes `-fstack-protector-strong`, stock MSYS2 MINGW64 passes the
  probe, so the option strips the canary from the whole tree.
- **No `-fzero-call-used-regs` on macOS arm64 (2026-10-15), then on any
  host (2026-10-23).** Upstream's `used-gpr` zeroing, plus the tail calls
  it turns into `bl`+`ret`, was 3.2% of host instructions: heavy-f1
  instructions/frame −3.15% (12/12), cycles −0.5..−1.7%, goldens
  bit-identical. `meson.build` no longer has the `used-gpr` block;
  Windows dropped it in `baf5e0c100`; `-ftrivial-auto-var-init=zero`
  stays. Expected on clang x86_64: instructions −3..−6% (Windows/GCC
  −5.5%); no cycle claim. No x86_64 configure ran locally: check those
  legs and the Windows mingw-gcc compile on the next CI run.
- **macOS 26 build fixes.** `download-macos-libs.py` uses
  `os.path.abspath` and repairs `.pc` `prefix=` lines and `gi18n.h`'s
  absolute `libintl.h` include; `build.sh` strips every `LC_RPATH` before
  `dylibbundler` and adds the correct one.
- **`download-macos-libs.py` vendors the newest MacPorts build and
  survives Python 3.14 (2026-10-10).** It ranks by numeric version (it
  had picked SDL3 3.4.8 over 3.4.18) and extracts with tarfile's `tar`
  filter (3.14's `data` default refuses `dbus`'s absolute symlink).
- **Codesigning.** Ad-hoc by default (MAP_JIT works);
  `XEMU_CODESIGN_ENTITLEMENTS=1` adds hardened runtime +
  `xemu.entitlements`; notarize with `scripts/sign-macos-release.sh`.
- **Cross-platform parity batch (2026-07-05).** The DSP56K JIT builds on
  all POSIX aarch64 hosts and CI runs the `xbox` suite on
  `ubuntu-22.04-arm` (an in-game `XEMU_DSP_JIT_DIFF` run there is still
  owed: `docs/windows-gating-audit.md`). `build.sh` gained Linux PGO, a
  stale-arch `build/` guard and `resolve_moltenvk` / `vendor_moltenvk`.
- **Native Windows release builds default to `-Dx86_version=3`** (AVX2 /
  BMI2 / FMA, as CI); pass your own `-Dx86_version=` to override.
- **Windows cross-compile fixes.** Hard-FPU ops in `fpu_helper_hard.c`,
  `g_mkdir_with_parents` / `g_rename` in the shader and pipeline caches,
  `gl_internal_format` in the `HAVE_EXTERNAL_MEMORY` display path.
- **Shipped MoltenVK converged with the tested one — 1.4.2
  (2026-08-04).** `build.sh:66`'s default is the single home for the
  expected version; macOS CI fails if the bundle's `Bundling MoltenVK …
  (version X)` line disagrees. `scripts/build-moltenvk.sh` builds the
  local driver at `MVK_PIN`. Pin-bump gauntlet: fps parity, 0 artifacts.
- **Local MoltenVK pin → main `67b26826` (2026-10-10; self-reports
  1.4.3).** 54 commits past v1.4.2 (#2839, #2846, #2845, #2819, #2823,
  #2861, #2794). Releases vendor stock 1.4.2 (`XEMU_MOLTENVK_VERSION`,
  CI-asserted) until a 1.4.3 tag ships. Bundled UUID
  `0C9CFBE7-F160-37A1-B1CD-407EA736B5DF`.
- **KosmicKrisp bundled beside MoltenVK on arm64 (2026-10-18).**
  `resolve_kk` takes `XEMU_KK_PREFIX`, else the CI asset
  `kosmickrisp-mesa-26.3.0-rc1-arm64.tar.xz` (`kk_asset_sha256`, cached
  in `macos-libs/arm64`); never a system copy. `package_macos` signs it
  into `Contents/Libraries/arm64`, keeps its `@rpath` install name and
  logs `Bundling KosmicKrisp from … (Mesa 26.3.0-rc1 (git-4706512989))`,
  which CI asserts. The cached asset is re-checked on every build and
  downloaded again when missing or different. CI also asserts that
  `LICENSE.txt` gains the MoltenVK and Mesa licenses (MIT, BSL-1.0,
  SPIRV-Tools). arm64 zip 11.7 → 13.9 MiB.
- **CI pins an `-mcpu` floor (2026-08-04).** The macOS `build` job sets
  `XEMU_ARM_CPU=apple-m1` so a runner refresh cannot raise the shipped
  ISA; `build.sh` ignores it on the x86_64 leg. Dev boxes auto-detect
  and warn above `apple-m1`.
- **Bench batches can isolate caches (2026-08-04).**
  `scripts/bench-abx.sh --isolate-caches` /
  `XEMU_BENCH_ISOLATE_CACHES=1` moves `spirv_cache_v*/` and
  `pipeline_cache.bin` into the work-dir clone: caches start cold, the
  warmup run warms them and both arms share them. The default (`0`)
  keeps caches warm-shared so existing baselines stay comparable.
  Receipt field `cache_isolation`.
- **build.sh warns on subproject drift (2026-09-08).** A checkout whose
  `HEAD` differs from its `[wrap-git]` `revision` prints `meson
  subprojects update --reset <name>` (`check_wrap_drift`, warning only).
- **dsp56300 `tar --force-local` keyed on the build machine
  (2026-09-08).** `build_machine.system()`, not `host_machine`, so bsdtar
  never gets it (first hunk of `fb42bce22e`).
- **Upstream merge `bce8323063` (2026-10-10).** xemu master `478b4f4961`:
  PTIMER alarms round up to whole ticks with a 1 ns floor
  (`muldiv64_round_up`, `get_next_alarm_time`), and `tests/xbox/ptimer`
  runs in the `xbox` suite; glslang 16.6.0; SDL3 and nlohmann_json git
  wraps (macOS still takes SDL3 from the vendored MacPorts bottle via
  pkg-config); a `make check-unit` CI job. `schedule_qemu_timer()` keeps
  the fork's zero-divisor guard and `tests/xbox/meson.build` the fork's
  `test-xbox` alias.

### MoltenVK runtime config

Set in `main()` before MoltenVK loads (explicit env overrides win) and
mirrored in `Info.plist` `LSEnvironment`; keep the two in sync. They are
set whatever `display.vulkan.driver` says: MoltenVK may load as a fallback
after KosmicKrisp, which ignores them.

Driver-source review at the v1.4.2 pin: the per-CB visibility primer
stays (archaeology 2.2 addendum). Triage lead: the changelog's "channel
corruption on color RTs used as transfer sources" fix was reverted
upstream before the tag, so shipped 1.4.2 behaves like rc1 there
(archaeology 2.2).

| Key | Value | Purpose |
|---|---|---|
| `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS` | `1` | Reduce descriptor binding overhead |
| `MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS` | `0` | Deferred encoding. Must stay 0: with whole-frame CBs, prefill corrupted streamed textures, enabled the AGX visibility-buffer crash, and measured slower (46.3 vs 48.5 fps) |
| `MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS` | `0` | Async submits |
| `MVK_CONFIG_FAST_MATH_ENABLED` | `1` | Metal shader fast-math (ALWAYS; driver default is ON_DEMAND) |
| `MVK_CONFIG_RESUME_LOST_DEVICE` | `1` | Ignore transient GPU errors |
| `MVK_CONFIG_LOG_LEVEL` | `2` | Warnings (driver default is INFO). Under `RESUME_LOST_DEVICE` a failed GPU command buffer is only a warning (`24465976a6`) |

`MVK_CONFIG_SHOULD_MAXIMIZE_CONCURRENT_COMPILATION` was tried and
reverted: inert on macOS 26 / M2 Ultra. A driver built with
`MVK_USE_METAL_PRIVATE_API` (`wideLines`/`logicOp`/
`VK_EXT_provoking_vertex`) is still unbuilt:
[moltenvk-optimization-experiments.md](Future/moltenvk-optimization-experiments.md).
The pipeline prewarm shipped 2026-10-24 (Pipeline prewarm).

- **Sub-page arm (b): inline NOTDIRTY store-skip (default-on 2026-07-18,
  +12% heavy-scene throughput).** The aarch64 store slow-path stub
  finishes a store inline when the mismatch is exactly `TLB_NOTDIRTY`,
  its 64 B sub-block holds no code and every NOCODE dirty client is set.
  `XEMU_SUBPAGE_FAST_REFUTE`: ~19.5M decisions, 0 violations. Receipts
  void since `f25bb789f2` (see the `XEMU_SUBPAGE_FAST` row).
- **Superblock seam-following and region/diamond formation (2026-07-18;
  code removed 2026-10-10).** Translate-time TB merging and a region
  former on a `tcg/tcg.c` liveness elision, sized by a cc-flag census
  (39-44% dead flag materializations; erratum `ce38bb8204`). Every policy
  regressed (Failed table).

### Testing & tooling (2026-07-18)

| Change | What it does |
|---|---|
| Harness runs are muted and carry their own pads (2026-10-11) | `scripts/harness-config.py` sets `[audio] volume_limit = 0`, stubs a hub + gamepad (`-device` args, `auto_bind = false`) for players 1-2 not bound to the keyboard, and with `--workdir D` writes the HDD/EEPROM/disc clones, `fullscreen_on_startup = false` and `screenshot_dir = D`; it refuses a toml outside D or a source config and never clones. A run whose xemu exits mid-run is DEAD |
| Mid-run screenshot artifact check | `scripts/bench-abx.sh` scores one window shot (`scripts/bench-screenshot.py`, by PID, works unfocused) for white-screen, stuck-uniform-frame and magenta-tile; any ARTIFACT exits 3. Verdicts go to `shot_<label>.verdict` sidecars (xemu's stdout redirect lacks `O_APPEND`) |
| Scene-anchored bench gating | `scripts/bench-receipt.py` anchors each run at the trailing contiguous in-band run of nsprof intervals, then drops one post-load transient. `--b-env`/`--e-env KNOB=V` serve N-valued knob A/Bs (the Windows bench: `--e-value V`) |
| Movement-phase probe | `scripts/cast-probe.py --walk` (the movement probe's successor, 2026-11-24): settle 15 s → forward 20 s → rest 5 s → backward 20 s, per-phase `info jit` TB-flush deltas, a scored mid-forward screendump, KK's C2 count. Required for TB lifetime/invalidation changes. Takes `--app`, starts with cold caches unless `--base`; quits via QMP so atexit dumps and PGO profiles land |
| Bench harness monitor socket moved to `/tmp` | `/tmp/xemu-bench.$$.<label>.sock`, removed per run, stays under macOS's 104-byte AF_UNIX limit. A socket under a deep work dir went past it and every run died as `DEAD (no monitor socket)` (archaeology 9.9) |
| Ret-memo fill path exports the lookup | `xemu_lookup_tb` (`accel/tcg/cpu-exec.c`) returns the TB, so the fill skips a `tcg_tb_lookup` g_tree walk (~1.8% of vCPU samples). Sub-noise by design |
| Sub-page consult branch hint corrected | A stale `unlikely()` on the ~90k/s notdirty-write consult is gone |
| Spanning-dest chain-decline counter | `xemu_inv_span_nochain` (`XEMU_INV_PROF` line (d')) counts refusals to chain into a TB spanning two guest pages |
| The measurement channels report on Windows and under OpenGL (2026-09-08) | From `eb971f860d`: `pgraph_gl_flip_stall()` ticks `nsprof_flip_tick()`, so `XEMU_NV2A_NSPROF=1` works on GL (`fence_wait`, `present_wait`, `flip_idle` only; sparse means not instrumented); `XEMU_INV_TIMING` has an `rdtsc` arm; `NV2A_STRIP_PROFILE_COUNTERS` follows `XEMU_DEBUG_BUILD`; stderr lines flush; `XEMU_SUBPAGE_FAST=1` / `XEMU_SUBPAGE_FAST_REFUTE=1` say when a backend has no inline emitter |
| Windows A/B harness survives the failure it exists to measure (2026-09-08) | `scripts/bench-savestate-ab-win.py` records an xemu that dies or hangs around `loadvm` as one DEAD run with its exit code and likely cause (empty log ⇒ `xemu.log` in the exe dir; a non-empty log with zero `nsprof:` headers is reported with the renderer named); pins `VULKAN` by default (other renderers need `--allow-renderer`), because only the Vulkan call sites fill the per-draw, upload and shader buckets, so a GL receipt is comparable only with another GL receipt; refuses MSYS2 Python. `scripts/win/capture_screen.ps1` is DPI-aware. `--dry-run`: 21 checks |
| `xemu-run` restores incrementally (2026-10-16) | Passes `incremental` (default true) to `qmp_xemu_checkpoint_restore`; `"incremental": false` forces the full compare (RAM written through `XEMU_RAM_SHM`). The first restore after a loadvm, migration or failed restore compares every page. `"verify": true` also compares every other page and reports the differing ones in `pages-missed` (0 in validation). heavy-f1 L=1: −22.0% Mcycles per candidate |
| `xemu-ram-digest` takes `"algorithm": "xxh3-128"` and hashes pages in parallel (2026-10-16) | XXH3-128 of RAM ~4 ms vs ~55 ms sha256; page hashes (sha256, or XXH3-64) run as 64 tasks of 512 pages on `dispatch_apply_f` (`ThreadPool` elsewhere): `pages` 125 → 68 ms. Default replies byte-identical; `against` stays serial. xxHash is inline in the file: the in-tree library (`XXH_CPU_LITTLE_ENDIAN=0`) matches no other xxHash (zero page `d91784870c6ee3f1`, not `93d76fe148c689ba`) |
| gdbstub breakpoints are guest PC hook sites (2026-10-16) | Each BP_GDB PC is a hook site (`xemu_gdb_bp_site_insert`) whose `XEMU_HOOK_PRIO_GDB` hook raises EXCP_DEBUG unless single-stepping (`s` and the `Qxemu.stepover` step), at every RAM alias of the PC too. Only a PC's first breakpoint invalidates its page; a `z0` leaves a hookless site, so re-arming costs nothing. CF_BP_PAGE stays only for DR7 (BP_CPU) breakpoints (`xemu_cpu_breakpoints`). A never-hit `Z0` on a hot page: +0.1% Mcycles/frame (was +31%) |
| The TB check after an incremental restore visits only written pages (2026-10-16) | `xemu_tb_revalidate_pages` hashes and re-protects only the pages `xemu_checkpoint_ram_reval_scope` names (`restore_page` writes plus the migration dirty log), not all ~36.5k TBs; `XEMU_TB_PEEKS` TBs are always dropped; loadvm, migration and full restores keep the full walk. Restore −31% Mcycles. Validation: `XEMU_TB_REVAL_REFUTE=1` (the full walk after each scoped check) found 0 misses, and `verify` restores reported `pages-missed` 0 |
| GPU counters count an ARRAY_ELEMENT16 run in one call (2026-10-24) | `nv2a_gc_array_element16_run` replaces a per-index-word `nv2a_gc_array_element` call (~20 ms a flip on Azurik's l256_120 probe, FRAME-8bffffeb0243). Same counts; counters-on rate 0.3256 → 0.4911 frames a vblank (vanilla 0.4994) |

### Retired diagnostics and dark experiments (2026-10-10)

Each removal is its own commit; revert it to bring the code back. The
numbers stay where they were recorded.

| Removed | What it was | Notes |
|---|---|---|
| `XEMU_PGRAPH_MMIO_STATS` | Per-register PGRAPH MMIO hit counter (`a477ad9f91`) for the PGRAPH lockless audit | Results: PGRAPH bullet (Threads + runtime), `Finished/pgraph-lockless-audit.md` §G |
| Trace-formation machinery: `XEMU_SUPERBLOCK`, `XEMU_REGION`, `XEMU_REGION_CHECK`, `XEMU_CCOP_CENSUS`, `XEMU_SUPERBLOCK_SIZE` | Superblock/region formation and its censuses (`ee1de9f654`, `ce38bb8204`, `5274b3b050`, `bbb802c8c8`, `3b17ae08e9`, `d65fc5f83e`) | All five policies lost; the region former had a latent rollback bug. `tcg/tcg.c`, `include/tcg/tcg.h`, `accel/tcg/cpu-exec-common.c`, `gen_CALL`, `gen_JMP` and `gen_jmp_rel` match upstream. Never revert `bbb802c8c8` or `3b17ae08e9` (live code). Record: Failed rows, `Finished/roadmap-closed-items.md` item 1, archaeology 6.7-6.8 |
| `XEMU_TB_RANGE_INV` | Upstream's per-TB byte-range check in the Xbox whole-page code-write invalidation (`69636c84a7`) | Superseded by sub-page arm (a). The `(b)`/`(b')` `XEMU_INV_PROF` counters stay, but the `(b')` line lost its `RANGE_INV=` field; never revert `69636c84a7`. Record: Failed row, archaeology 1.22, `Finished/subpage-gate0-prediction.md` §0/§3 |
| `XEMU_HARDENING` | `build.sh` `=0` arm adding `-fzero-call-used-regs=skip` (`00a404c774`; all platforms since `5dfde3356d`) | Failed two macOS sign tests; moot since no host adds `used-gpr` (Build + packaging). Record: Failed row, archaeology 8.7 |
| `XEMU_RETC_BITS` (knob) | Ret-memo index-width knob (`4ca101772b`) | 13 bits lost (−0.23 fps, 4/4). The width is the compile-time `XEMU_RETC_BITS` (12, `target/i386/cpu.h`); table and per-flush memset back to 128 KiB. Record: Failed row "Ret-memo 13-bit index", closed roadmap item 2 |
| `XEMU_SURFACE_CB_REUSE` | 4-entry surface CPU-access-callback reuse pool in `pgraph/vk/surface.c` (`804bc5d87e`) and its `surface_cb_audit` | −0.66 fps, 3/3. The ranged TLB flush landed 2026-10-15 as `tlb_flush_host_range` |
| `XEMU_X87_ELIDE_CLEAN` | Clean-ST(i) write-back elision (built `8da40e6fed`, killed `7aa746a952`) with `xemu_fp_dirty` / `XEMU_ST_DIRTY` tracking at 21 `ops_fpu.h` sites | Changed numerics (kept 80-bit residue); under-powered kill (−0.23 fps, 3/3). `ELIDE_FT0`, `DEFER_FIP` and `XEMU_X87_REFUTE` untouched |
| `XEMU_PRESENT_DRAWABLE_FIRST` | Opt-in Metal present reorder (`5b8aef5717`): drawable before the push-present ring | Only an fps A/B (+0.50, noise) on a latency change. `xemu_metal_acquire_drawable()` folded into `xemu_metal_begin_frame()`; `drawable_acq` stays. Reopen bar in its Failed row |
| `XEMU_PFIFO_KICK_STATS` (2026-10-15) | `pgraph_write` kick census and PFIFO idle-park counter (`804bc5d87e`) | No kick is suppressible (Settled vectors). `XEMU_PFIFO_HEARTBEAT` `iters` counts loop passes, not parks |
| `XEMU_SURFACE_CB_STATS` (2026-10-15) | Surface CPU-access-callback census (`804bc5d87e`) | Sized the pool and the ranged flush (199.8 events/s on F8, Vulkan renderer bullet) |
| `XEMU_UI_LOCK_STATS` (2026-10-23) | Runtime gate on the main-loop-mutex + BQL hold line (`5b8aef5717`) | Answered the ImGui-under-lock question (Settled vectors). `DEBUG_XEMU_C=1` builds still print it; nsprof `ui_hud_lock` covers the HUD lock |
| `XEMU_UI_FRAME_CAP_NS` (2026-10-23) | Test-only floor on the Metal UI present period (`78c4573dfb`) | Busy-spun the UI loop and no longer modelled a real panel |

## Failed / reverted experiments

Lessons worth preserving so they aren't re-attempted.

| Attempt | Reason |
|---|---|
| x86_64 inline fld/fst/fild as TCG integer ops (2026-10-18) | det heavy-f1 +10% instructions, +9% cycles: each special-class brcond ends the TCG basic block, so live globals sync and reload around every load. The host x87 instructions (`x87_ld`, `aae214c27a`) do it branch-free in two |
| CNG (BCrypt) SHA-256 for `xemu-ram-digest` on Windows (2026-10-18) | 551 vs 548 ms for 128 MiB without SHA-NI (Haswell); xxh3-128 takes 29-41 ms |
| Surface-callback reuse pool default-on (`XEMU_SURFACE_CB_REUSE`, 2026-08-04) | Mechanism real (~200 register/unregister events/s, 0.95:1 with in-scene guest TLB flushes, which wipe the jump cache and near-return memo via `tcg_flush_jmp_cache` → `xemu_i386_ras_flush`) and all correctness gates passed, but **−0.66 fps, 3/3, draws/s −1.9%** vs a ≥ +0.25 bar. Restore by reverting the removal commit, never `804bc5d87e` (also `XEMU_PFIFO_KICK_STATS` and the SPIR-V cache key). The ranged flush landed as `tlb_flush_host_range` |
| x87 clean-ST(i) write-back elision default-on (`XEMU_X87_ELIDE_CLEAN`, 2026-08-04) | Refuter 3.49e9 checks, 0 violations, but blind to the 80-bit residue the elision keeps. **−0.23 fps, 3/3** vs ≥ +0.20, under-powered (3 pairs, B always first). Restore by reverting the removal commit, never `8da40e6fed` (also `ELIDE_FT0`, `DEFER_FIP`). Reopen needs per-op host-PC attribution (`XEMU_GUEST_PROF` sees only whole TBs) and an artifact A/B on an x87-transcendental title |
| Reports idle-budget 300 → 50 µs (2026-08-04) | **−12.1 fps, draws/s −21.4%**: early submits land on the arch-1.1 submit-storm side of the cliff. Hardcoded at 300 µs 2026-10-15 |
| Ret-memo 13-bit index (`XEMU_RETC_BITS=13`, 2026-07-18) | Predicted +0.3-0.5 fps, measured **−0.23 fps, 4/4**: the ret set seen between full TLB flushes fits at 12 bits, so the bigger table only adds footprint. Neither a memo hit nor a jump-cache hit checks the physical page; INVLPG clears the memo per page since 2026-10-10 (near-return memo bullet, CPU / JIT). Retest only as a two-binary `scripts/bench-abx.sh` interleave, once per-flush wipes stop and a census shows ≥ 1M collision fills/s |
| TB byte-range invalidation filter (`XEMU_TB_RANGE_INV`, 2026-07-11) | `XEMU_INV_PROF`: 100% of invalidations were data-write false sharing. Invalidations 338k → 675, but notdirty traps **25.2×**: whole-page invalidation is what fires `tlb_unprotect_code`. Base xemu dropped the check in `703566ce33`; `c1e4e87f5f` added the recycle cache. Gate-0 later put the real cost in the O(N) scan (archaeology 1.23). Superseded by sub-page arm (a), the same filter with an O(1) consult |
| L2 victim jump-cache (2026-07-05) | Live hit rate **11.2%**: residual misses are one-shot. Measure the miss stream's recurrence, not its volume |
| JIT write-protect flip caching (2026-07-05) | **Parity** (6 pairs); the 9.7% share was `thread_suspend` skid onto barriers. **Reopened and shipped 2026-10-15** (CPU / JIT, No W^X flip per TB entry): −2.07% Mcycles/frame on the det cold boot (22329bd8bc). PMI skids onto the ISB too: check entries × cost |
| Per-depth return-address ring (2026-07-05) | Same-depth call sites share slots: fills ran 2.6× hits. Superseded by the eip-keyed ret-target memo |
| Vertex copy-on-conflict transient remap (`XEMU_VTX_TRANSIENT`, 2026-07-05) | **−2.09 fps, 6/6**: the 64-entry/2 MiB table overflowed every frame (all-slot drains). The targeted submitted-slot wait stays |
| TB jump-cache enlargement (12→16 bits, 2026-07-05) | **−1.10 fps, 4/6**: the 64 KiB cache is L1-resident; 1 MiB slows every hit. Target lookup volume, not geometry |
| Eager report submit (`XEMU_REPORTS_EAGER=N`, 2026-07-04) | −0.08 fps (6 pairs, mixed): with the 300 µs idle budget the residual wait is GPU catch-up time |
| Occlusion-rework intermediates: in-pass queries with synchronous drains; submit-on-idle per pending report | Both regressed vs 15.8 fps (6.9; ~11.2). Query placement × report delivery is a policy pair (shipped pair: 2.4x, +144%) |
| Per-flight vertex-RAM mirrors | ~Neutral: the cost was the recording-CB conflict cascade. The salvage, byte-exact conflict refinement, shipped (+5.4% fps) |
| `floatx80` union overlay on ARM64 | Layout incompatible with IEEE 64-bit: segfaults |
| Voice register `__thread` cache | Stale data; Xbox HW mutates voice regs via DMA |
| Async MetalFX under *GL presentation* | GL↔Metal sync can't be expressed with `SDL_GL_SwapWindow` + vsync. Landed for Metal presentation (`MTLSharedEvent`) |
| Separate compute queue | MoltenVK only exposes `queueCount=1` |
| Depth export (CPU readback or `vkExportMetalObjectsEXT`) | CPU: 2 ms/frame + 18 MiB of copies. Metal export deadlocks MoltenVK's internal mutex |
| Direct-VRAM compute unswizzle via `VK_EXT_external_memory_host` | `HOST_WRITE → SHADER_READ` barriers cover submission, not GPU execution: torn reads (particle/HUD flicker) |
| Host-imported `BUFFER_VERTEX_RAM` | Same torn-read class for vertex data |
| `pthread_jit_write_with_callback_np` for `tb_phys_invalidate` | Scoped W drops on callback return; QEMU nests JIT writes |
| `VK_KHR_dynamic_rendering` full lowering | Missing `VK_SUBPASS_EXTERNAL → first subpass` dep stalled Metal's tile renderer |
| APU voice resampler `rate == 1.0` fast path | Lacked silence-padding on voice drain: `voice_process` spun |
| `HLT` BSOD recovery (IF=1 force at-HLT) | The `CLI;HLT` IRQ loop is a symptom of a stuck thread elsewhere |
| Surface-expiry throttle keyed on `pg->frame_time` | `frame_time` advances only on `NV097_FLIP_INCREMENT_WRITE`: level loads starved the prune (black screen). Uses host wall-clock |
| Surface-range binary search on `end` | `end` isn't monotonic in a start-sorted array |
| Two-level quick texture hash / hash-skip > 64 KiB | 192-byte sampling missed YUV / FMV changes |
| Dirty-range VRAM flush (early) | Bitmap cleared before `flush_memory_buffer` read it; a per-flight `[first, last]` range does it safely |
| Incremental texture hash on misaligned textures | `test_and_clear_dirty` steals another chunk's signal. Page-aligned textures only |
| HRTF hand-NEON (gather over taps) | Defeated OoO overlap; autovectorization wins. Lanes over outputs shipped later (2026-10-15) |
| BQL event batching | Breaks QEMU cooperative scheduling |
| TLS-cached `pthread_jit_write_protect_np` | Drifts from kernel W^X on nested `tb_gen_code` / setjmp paths: SIGSEGV/SIGILL |
| Hoist `can_fifo_access` out of pfifo pusher word loop | `pfifo_run_puller` drops `pfifo.lock`: `ERROR_CALL` on boot. The det inline run's copy does drop the checks (Threads + runtime, Leaner deterministic inline pusher) |
| DSP JIT round-4 `cur_inst` preset skip | Regresses at startup (`op=0x001000`). Recover from `1f444fa272`/`4668947cc9` |
| DSP JIT round-4 EPI_NO_PC | 9.75% mismatch: calc_ea mode 6 and REP/DO legitimately move pc |
| Vblank cadence aligned to host display refresh | Host `vblank_interval_ns` drives `NV_PCRTC_INTR_0_VBLANK`, so retuning retimes the guest IRQ. Needs a fixed 60 / 50 Hz NV2A timer |
| AArch64 `fsin` / `fcos` libm-helper inline | `helper_sin_fast_f{32,64}` / `cos_fast_*` made audio crackly: `gen_flush_fp` or `TCG_CALL_NO_RWG_SE` ordering was an implicit fence |
| Voice-list `SE2FE_IDLE_VOICE` re-notify skip | No guest-write hook on `NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE` to clear the flag |
| Cross-TB FPCR elision via `cached_fpuc_rc` as TCG global | TCG globals reload per TB, so `gen_flcr`'s `ld16u` is only renamed. Removed with the inline cache 2026-10-15 |

Superblock seam-following campaign (2026-07-18; code removed 2026-10-10;
story in `docs/Finished/roadmap-closed-items.md`, item 1):

| Attempt | Reason |
|---|---|
| Superblock M1 alone: concatenate through unconditional same-page forward jmps (`XEMU_SUPERBLOCK`) | Capturable subset **3.7% of taken goto_tb exits**, under the ≥10% bar |
| Superblock M2, all-conditionals policy (cap 4) | Throughput **−1.6%** (3/3); compare throughput, not raw fps |
| Superblock M2, forward-conditionals-only policy | **−2.3%, 3/3**: TCG spills cc/eip before any TB-exiting branch, so only both-successors-inline designs can harvest the 39-44% |
| Region/diamond formation, 64 B window (`XEMU_REGION=4`) | Liveness elision proven by `XEMU_REGION_CHECK`; **−1.34 fps (−4.5%), 3/3**: 72% of regions drain-demoted |
| Region/diamond formation, 16 B window (the single pre-registered mechanism iteration) | Demotes 64%, **−0.69 fps, 3/3**. Campaign STOPPED permanently |

Fork-wide optimization wave (2026-08-04;
`docs/Finished/fork-optimization-audit-2026-08.md`):

| Attempt | Reason |
|---|---|
| Drop upstream's `-fzero-call-used-regs=used-gpr` on macOS builds (`XEMU_HARDENING`, 2026-08-04; knob removed 2026-10-10) | **Reopened and shipped** (Build + packaging) on det counters (−3.15% instructions/frame). The fps record: +0.52 at 3+/2− (5 pairs), then −0.39 at 3+/5− (8 pairs, `7aa746a952`), neither recorded on a profile trained on its own tree (retrain `6ff4467c27`); `__text` −411 KB ruled insufficient; default reverted in `00a404c774`. Arm (b) is default-off on non-Apple aarch64 since `0aeb1998c3` |
| Running the x87, sub-page-fast and DSP-JIT refuters in one process (`XEMU_X87_REFUTE` + `XEMU_SUBPAGE_FAST_REFUTE` + `XEMU_DSP_JIT_DIFF` together) | Guest deadlocks at **BOOT, 2/2** (validator contention, arch-4.5 family; also the campaign's run-1 `vp_write` wedge). Singles and pairs are clean: cap refuter soaks at two validators per process |
| Drawable-first present ordering (`XEMU_PRESENT_DRAWABLE_FIRST`, opt-in, 2026-08-04; code removed 2026-10-10) | Only A/B was fps, **+0.50 under noise**, which a latency-only reorder cannot move; it acts only when the bounded-debt catch-up fires or on the pull path, never measured on presented-frame age. Restore by reverting the removal commit, never `5b8aef5717` (also MetalFX resize quantization, the `drawable_acq`/`ui_hud_lock`/`ui_frame_dt` buckets and `XEMU_UI_LOCK_STATS`). Reopen bar: a `present_age` nsprof bucket showing **≥ 2 ms mean gain** with `push_policy_skip > 0` in a focused vsynced 60 Hz window |

---

## Settled vectors — measured, not worth it (or premise dead)

Closed with data, not attempted and reverted. Reopen only when the
recorded bar is met with fresh numbers (`XEMU_NV2A_NSPROF` /
`XEMU_GUEST_PROF` / the named counter).

| Vector | Measured reality / why closed |
|---|---|
| Synchronous qcow2 reads for the icount vCPU (2026-10-16) | Coroutine-free reads (`xemu_sync_preadv`): cold-boot instructions −0.7..−1.0% (9/9), but Mcycles 4/6 and wall 3/6 against ±15% swings. Not committed; the no-kick part alone is the next row |
| IDE-window bottom halves without a vCPU kick (2026-10-16) | One kick at the window's end instead of icount_notify_exit: cold-boot instructions −0.52% (24/24), Mcycles mixed, wall 13/24. Not committed |
| DSP JIT residue after chaining (2026-10-16) | Inline X mixbuffer windows, `num_inst` only at exits, RTS inline cache: each under cycle resolution (≤ 0.3% stop line). Reopen for a title with several times Azurik's DSP load |
| x87 residue after the FP-register accesses (2026-10-16) | ~6.3K helper calls/frame, 1.0-1.7% of vCPU; fwait as a no-op −0.50% Mcycles; pinning doubles in d8-d15 realistic 0.2-0.8%, under the 1% bar. Reopen with a prototype measuring ≥1% |
| Lazy FCS/FDS at x87 exception-pointer landings (2026-10-16) | Minsns −0.54..−0.66%, but stream-e2 Mcycles +0.45%. Reopen with a cheaper marker or a title that reads FCS/FDS |
| `preserve_most` for the flag helpers (2026-10-16) | `cc_compute_all`/`cc_compute_c` via `exec/helper-head.h.inc`: Minsns −0.26..−0.37%, Mcycles 4/8 and 5/8 (gate 3/4). Only leaf helpers qualify: `cc_compute_nz` asserts, so its `preserve_most` prologue saved all seven registers on every entry. Reopen if more guest state moves into caller-saved registers |
| xpage backstop severing only the cross-page jumps (2026-10-16) | `XEMU_XPAGE_PROF`: 89% of reset jumps are cross-page, so ≤ ~11% of link patches kept vs a 50% bar. Reopen only with a backstop that keeps cross-page chains whose target page still maps where it did |
| PFIFO spin before parking (2026-10-16) | Polling `doorbell`/`fifo_kick`/`exiting` every 250 ns for up to 30 µs: parks −92..−95%, fps flat (`bench-abx` 2+/5−), CPU +1-6% of a core. Polling with only an `isb` between reads lost fps outright: the poll steals the flags' cache line from the vCPU's doorbell barrier. Not committed |
| APU lock hand-off to a waiting vCPU between frames (2026-10-16) | `fe_waiters` hand-off: vCPU `d->lock` wait −30%, ~0.25% of wall. Splitting `d->lock` across VP/DSP is out (`fe_method` writes state the DSP DMA reads) |
| Texture-upload barrier batching | Per-mip `pre/post_compute` pairs are load-bearing on reused `COMPUTE_DST/SRC`; uploads ≤ 15 ms per 5 s |
| GPU S3TC decode (compute shader) | Upload CPU incl. S3TC ~0 steady |
| `VK_EXT_external_memory_host` revival (snapshot scheme) | Snapshot memcpy < 0.5 ms per 5 s vs a ≥ 1 ms/frame bar (archaeology 1.6) |
| MetalFX *input* ring (drop `metalfx_drain_inflight`) | Drain ~0.1 µs/flip |
| ~~Shader specialization constants~~ | Stale premise: alpha-test / fog-enable are already compile-time variants (`PshState`/`VshState`); only value uniforms are per-draw. Reframe as variant-count / compile-stutter work |
| `MTLResidencySet` (macOS 15+) | MetalFX subsystems own only 2-4 long-lived resources, and MoltenVK exposes no residency sets to Vulkan clients (`metalfx_upscale.m` Phase-5 note). Re-check on a MoltenVK capability change |
| Streamed-vertex stall reduction (CPU-side sync) | Waits were GPU frame time; the byte-exact skip shipped (+5.4% fps) |
| GPU frame-cost campaign menu (draw merging, pass cuts, `VK_EXT_multi_draw`) | No coalesce opportunity, ~14 ms/flip GPU slack. MoltenVK's multi-draw (`bc0d5bf7`) emits one `MVKCmdDraw` per sub-draw, so xemu doesn't request it. Re-run Phase 0/2 gates first (merge census `038f596332`, removed 2026-10-23); a revival must break pairs at clears, reports and zpass-enable changes |
| Dirty-clear TLB-walk coalescing | Parked (~1-1.5 fps ceiling; widens an archaeology-1.9-class race). Renderer `test_and_clear_dirty` < 1% post-sub-page; the walks got cheap via writable-clean bitmaps (2026-10-16) |
| Voice-register writeback batching | Unsafe: guests read voice state from RAM (`NV1BA0_PIO_GET_VOICE_POSITION`), so deferring `voice_set_mask`'s `ram_stl` serves stale state (archaeology 5.2). The no-op-store elision shipped |
| `qemu_cpu_kick` via `dispatch_semaphore_t` | Wrong premise: under MTTCG the kick is `tcg_kick_vcpu_thread` (two atomic stores); `pthread_kill(SIG_IPI)` survives only on cold paths |
| DSP JIT parmove+ALU fusion | Proxies (`alu_ccr_nz_skipped` / `alu_ccr_eu_skipped`) read 0, and the fold is instruction-neutral by construction: `emit_pm_read_reg` (`dsp56k_jit_arm64.c`) writes its destination directly (the file moves; find it with `grep -n -A85 "static void emit_pm_read_reg"`). Do not build the write-d=0 eligibility counter (design-doc row 8). Reopen only for a non-audio DSP program whose parmove sources are neither pinned nor A/B, after deriving a nonzero static delta. Arithmetic: `git show ad569d78c5:.claude/skills/xemu-frontier-and-positioning/SKILL.md` §2.5 |
| COMISS / UCOMISS inline lowering (audit §1.4c) | `XEMU_X87_CENSUS=1` (`sse scalar compares: ss=… sd=…`): 0.0/s in-scene. Reopen above ~1.5M `ss`/s |
| `pfifo_kick` broadcast suppression (audit §1.5) | `XEMU_PFIFO_KICK_STATS=1`: 0 of 412,074 kicks suppressible; a wrong skip hangs PFIFO (archaeology 7.1). Do not implement. 62.9% land with a kick already pending: a different class, unsized |
| ImGui frame build under BQL + main-loop mutex (audit §1.9) | `XEMU_UI_LOCK_STATS=1`: 0.386% occupancy, under 0.5%. Re-check with nsprof `ui_hud_lock` (occupancy % = per_flip ms × flips/s ÷ 10, an upper bound on hold time) or a `DEBUG_XEMU_C=1` build |
| `notdirty_write` dirty-check **fps claim** (audit §1.2) — the claim, not the code | Arm (b) took the population (34-57x collapse). The code stays, unconditional since `XEMU_DIRTY_FAST` was removed 2026-10-15, with no fps claim. Re-measure a candidate's benefit base at build time, not audit time |
| Lockless MMIO for the remaining BQL blocks (NV2A `PMC`/`PCRTC`/`PTIMER`, APU `main`/`gp`/`ep`) | **[`docs/Finished/lockless-mmio-verdict.md`](Finished/lockless-mmio-verdict.md)**: ≤ +0.06 fps; BQL-free `nv2a_update_irq` races vblank. Reopen bar: `XEMU_MMIO_PROF` `BQL wait total` ≥ 500 ms / 75 s (measured 75.6 ms) |
| Code buffer within ADRP reach of the binary (2026-10-16) | A `vm_region` hole search keeps the buffer within 4 GiB of text (ADRP+ADD instead of MOVZ+MOVK×2), but only ~44K external helper calls run per frame: ceiling −0.07% instructions. Not committed |
| icount TB-entry store forwarding (`icount_decr.low` stored 16-bit, the next TB loads `.u32`) | No measurable stall: a matched (racy) 32-bit store measured 0% over 24 det pairs, the exact form +1.4..+1.5% Minsns; the 3.5% on the entry `ldur` is probably skid |
