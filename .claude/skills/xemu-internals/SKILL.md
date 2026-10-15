---
name: xemu-internals
description: xemu-macos invariants and triage. Load before touching hw/xbox/nv2a/pgraph/vk, hw/xbox/mcpx/apu/dsp, the ui/xemu.c / xemu-metal.m present path, BQL or clocks; when triaging a crash, hang, artifact or fps regression; or when reading XEMU_* diagnostic output.
---

# xemu internals: what must not break, and how to triage

Symbols, not line numbers: re-grep before relying on one. For past failed attempts, see xemu-history. V = `hw/xbox/nv2a/pgraph/vk/`, J = `hw/xbox/mcpx/apu/dsp/interp/dsp56k_jit_arm64.c`.

## Invariants (rule → what breaks → where)

### NV2A Vulkan renderer (all of V runs on the PFIFO thread)
- **One queue** (`queueCount = 1`). Apple fast paths (`target_is_active` in `pgraph_vk_upload_surface_data`) assume submission order = execution order. Add no second queue or submit thread; keep the non-Apple `#else` full-finish branches. → in-flight image reuse, corruption/crash. V`instance.c`, V`surface.c`.
- **Deferred eviction**: `invalidate_surface_full(allow_deferred)` parks images in `r->invalid_surfaces`. Reuse and prune skip anything `surface_image_in_recording_cb()`. Destroy only when `evict_submit_seq <= retired_submit_count`, on all platforms (submitted ≠ done). New fence-observation sites advance the watermark; new destroy paths take the same gate. → destroying an in-flight image. V`surface.c` `prune_invalid_surfaces`; V`command.c` `pgraph_vk_wait_slot_fence`.
- **One command buffer per frame, `NUM_FLIGHT_SLOTS = 2`**. `pgraph_vk_init_flight_partitions` splits descriptor sets, staging buffers and the query pool per slot. A new per-slot resource, or a faster consumption rate, needs a runtime submit-and-continue guard: release builds compile `nv2a_vk_assert` out. → overrun into the next slot: corruption or a MoltenVK crash. V`renderer.h`, V`debug.h`.
- **Query guard** at the top of `begin_pre_draw()`, before any setup: `num_queries_in_flight >= queries_per_slot - 1` → `pgraph_vk_finish(QUERIES_FULL)` (the -1 reserves the primer). `alloc_report()` guards with `REPORTS_FULL`. Pool = 4096 (`XEMU_MAX_QUERIES`). → a late guard binds `framebuffers[-1]`; no guard crashes AGX in `setVisibilityResultMode`. V`draw.c`, V`reports.c`.
- **Primer query**: `pgraph_vk_begin_command_buffer` issues an empty query (`primer_index`) outside any pass, so MoltenVK's `_needsVisibilityResultMTLBuffer` is set before the first encoder exists. New render-pass paths go after it. Reset the query pool only at CB begin, never inside a pass. → AGX crash when a frame's first pass holds its first zpass draw (scenes that open with a clear hide it). V`draw.c`.
- **`MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS=0`**: a second layer on top of the primer; keep both. → prefill=2 corrupts streamed textures (large magenta rects), allows the AGX crash, and is slower.
- **Vertex dirty tracking is page-granular.** The byte-exact refinement (`uploaded_span_content_differs`) may skip a finish only when memcmp proves the bytes identical. When unsure, finish. → stale vertices, intermittent garbled geometry. V`vertex.c`.
- **A load must leave PGRAPH as a fresh process's load does** (icount): method state the main `nv2a` section lacks rides in `nv2a/pgraph-methods`; loads without it run `pgraph_reset_unsaved_state`, and every load marks all state dirty (`pgraph_mark_all_state_dirty`). New method state the next draws read goes in that subsection. → repeated loads in one process diverge (stale vertex arrays). `hw/xbox/nv2a/nv2a.c`.
- **Deterministic reports are lazy** (icount): GET_REPORT records the slot and traps its page; the first guest access, flatview or mapped DMA to the slot, a pause, savevm/sync_ram, RESTORE_VM, slot reclaim and the full-pool or flush drains deliver. The APU's direct `ram_ptr` accesses are not trapped. A new renderer path that reads or writes guest VRAM calls `pgraph_vk_det_reports_guard`; a new reader that copies RAM directly (like trace samples) sets `nv2a_set_reports_eager`; after a load, `flush_discard` drops what is left. → RAM differs from an `XEMU_REPORTS_SYNC=1` run. V`reports.c`.
- **Surface expiry** throttles on monotonic time (`surface_expire_interval_ns` 33 ms), never on `pg->frame_time`. → black-screen freeze on rapid level loads. V`surface.c`. Exception, icount only: expiry runs once per flip or every 4096 updates, reports are delivered before PFIFO rests, and a load discards renderer state (`flush_discard`), so offscreen Vulkan is deterministic.

### DSP56300 JIT / APU
- **Engine precedence** lives only in `dsp_want_external_jit_engine()`: `DSP56K_JIT_SUPPORTED` (aarch64, non-Windows) + `audio.dsp_jit.enabled` → fork JIT inside the C interpreter (`c_dsp_ops`); else `audio.use_dsp_jit` → upstream `jit_dsp_ops`; else the plain interpreter. Keep the `dsp56k_jit_*` (fork) and `dsp_jit_*` (upstream) namespaces apart. `use_dsp_jit: true` is a fork default: re-assert it after upstream merges. The inline JIT runs only the GP core (`dsp56k_jit_init` keys on `dsp_core_t.is_gp`, set in `dsp_c_init`); the EP gets no JIT state and stays on the interpreter. → wrong engine live; diagnostics measure the wrong one. `dsp/dsp.c`, `config_spec.yml`.
- `dsp->core` is only the save/restore snapshot (`dsp_sync_to_vm`/`_from_vm`); live state is `dsp->backend`.
- **W^X**: every code-buffer write, including `chain_repatch_b` patches into other blocks, happens inside `qemu_thread_jit_write()` → writes → `jit_clear_icache` → `qemu_thread_jit_execute()`, in one function. No TLS-cached W^X state, no `pthread_jit_write_with_callback_np`. → SIGSEGV now, or SIGSEGV/SIGILL about a minute in. J.
- **Pins**: A/B in x26/x27, packed X/Y in x20/x21, SR low 16 bits in x28. `dsp->registers[]` stays authoritative: write through, and reload pins (`emit_reload_*_pins`, `emit_reload_sr_pin`) after any BLR fallback that may touch them. Verify with `XEMU_DSP_JIT_PIN_AUDIT=1`. → silent audio corruption on rare paths. J.
- No block boundary may skip `dsp_postexecute_update_pc`, which decides REP/DO continuation. → PC desync.
- Voice registers live in guest RAM; the `voice_buf` cache must fall through to `ram_ldl`. → stale voices. `apu/vp/vp.c`.

### Present path
- **Push ring is the default** (`pgraph_vk_push_present_enabled`; off only with `XEMU_PUSH_PRESENT=0`; needs the Metal backend and the metal-objects extension). `pgraph_vk_present_schedule_publish` publishes at flip; `nv2a_get_present_frame_pushed` consumes. Leaf mutex (`present_slot_lock`), a CFRetain per entry, a generation counter: never lock-free. The cursor only moves forward. → use-after-free of IOSurface handles; frames out of order. V`display.c`.
- **Pull fallback** (GL, before the first publish, after a resize, push off): `nv2a_get_present_frame` → `pgraph_vk_get_present_frame` (`sync_pending` + `pfifo_kick` + `qemu_event_wait`). One consumer (`framebuffer_in_use` assert). Keep both paths correct; add no synchronous work to the `sync_pending` handler. → UI latency on every frame. `pgraph/pgraph.c`, `ui/xemu.c` `metal_render_frame`.
- **`encodeWaitForEvent` comes before the drawable render pass** opens. → a frame the GPU hasn't finished gets presented. `ui/xemu-metal.m` `xemu_metal_begin_frame`.
- **Vblank**: `vblank_timer_thread` ticks every `vblank_interval_ns = 16666666` on monotonic deadlines and raises the guest VBLANK interrupt; never tie it to host refresh. No PAL 50 Hz path exists. → guest simulation retimed. `ui/xemu.c`.
- **Launch parity**: every `Info.plist` `LSEnvironment` variable is mirrored in `main()` with `setenv(...,0)` before MoltenVK loads (six MVK keys). A variable exported in the shell overrides both. → Finder and terminal launches run different configs. `ui/xemu.c`, `Info.plist`.

### Deterministic mode (`-icount ... sleep=off`, null renderer)
- **Guest state changes only at fixed instruction counts.** No device thread may touch guest RAM, registers or IRQs on its own schedule: PFIFO pushes inline on the vCPU (`pfifo_icount_sync`), APU frames run in `mcpx_apu_det_tick` (virtual timer, BQL held, `FE_DFL_ENV`), IDE drains before the guest access returns (`ide_bus_icount_settle`), pads change only at vblank ticks. A new device thread or async completion breaks bit-exact replays. → RAM differs between identical runs.
- **Host state not in the savestate is reset on load** under icount (APU resamplers/HRTF/`ep_frame_div`, DSP JIT throttle, AC'97, the PM GPIO field pin). Per-read toggles and static counters in read handlers are the usual culprit. → runs alternate A/B/A/B.
- **Run device work on the vCPU thread under `FE_DFL_ENV`**: TCG leaves the guest x87 RC in the host FPCR and reprograms it lazily. → float results depend on history.
- **Deferred x87 FIP/FCS lands at mid-TB exits from insn_start word 2** (both modes): `i386_tr_insn_start` records the distance back to the pending insn, `x86_restore_state_to_opc` → `xemu_x87_restore_fip` stores it. Under icount FDP stays per insn and FDS is skipped while the same segment was stored earlier in the basic block. Put nothing else in word 2; a new inline write of `segs[].selector` or `fpds` resets `xemu_fds_seg`, as `gen_op_movl_seg_real` does. Refute with `XEMU_X87_REFUTE=1`: every `*violations=` field must be 0. → FNSTENV/FNSAVE see a stale FIP/FDS after a guest fault or MMIO exit. `target/i386/tcg/xemu-x87.h`.
- **Find a divergence** with a per-frame RAM hash (`xemu-frame-advance 1` + `pmemsave`), then a temporary MMIO/PIO trace diff (`memory_region_dispatch_read/write`); the first differing read names the device.

### Threads, locks, clocks
- `QEMU_CLOCK_REALTIME` is `CLOCK_MONOTONIC`; `QEMU_CLOCK_HOST` is wall time and can jump. Pace and throttle on REALTIME. The macOS main loop never busy-waits: sub-ms timer precision comes from a one-shot kqueue `NOTE_CRITICAL` timer in the poll set plus a 25 µs final spin (`qemu_poll_ns`). `util/qemu-timer.c`.
- Under `-icount sleep=off` only the vCPU thread warps `QEMU_CLOCK_VIRTUAL`, between slices. Off the vCPU the virtual clock lags by the running slice's uncounted instructions (HLT reads as idle before `icount_process_data`), so no other thread may warp it or arm a VIRTUAL timer relative to now. → identical runs rarely diverge. `accel/tcg/icount-common.c`.
- Never block while holding the BQL. `surface_access_callback` drops it before taking `pgraph.lock`. → the whole VM freezes.
- Every `pfifo_kick(d)` caller holds `d->pfifo.lock` across the kick-set and the broadcast (the idle wait is an untimed `qemu_cond_wait`). `pfifo_run_puller` drops `pfifo.lock` to take `pgraph.lock`, so don't hoist checks that assume it's held (except the deterministic null inline run, which holds both throughout: `pfifo.inline_locked`; method code that takes the BQL goes through `pgraph_inline_bql_lock`). Never suppress a kick. Normal mode's DMA_PUT doorbell skips the lock (`pfifo_put_doorbell`): a release store of PUT, then a Dekker pair with the thread, which clears `doorbell` before the pusher's acquire load of PUT and rechecks it after setting `parked`; keep both halves. → permanent PFIFO hang. `nv2a/pfifo.c`.
- `cpu->mem_access_callbacks` is an RCU list (entries freed after a grace period). Any thread doing guest DMA walks it and must be `rcu_register_thread()`-ed (qemu_main is, in `ui/xemu.c`). A callback change refills only the TLB entries for its pages (`tlb_flush_host_range`, inside the insert/remove safe work), so nothing flushes the TLB in a steady scene and it is floored at 8192 entries in both modes. → SIGSEGV in `flatview_read_continue`; a trap that misses its first access. `system/physmem.c`.
- PFIFO, vblank, and any new frame-critical thread run at `QOS_CLASS_USER_INTERACTIVE`. → jitter that looks like a regression.
- The x87 control word changes only through `cpu_set_fpuc()`, because PC and RC are baked into `tb->flags`. → the write-through x87 path runs at a stale precision.

## Triage (one escape hatch per fresh launch)

| Symptom | First check | Discriminating experiment | Owner |
|---|---|---|---|
| Fix didn't take effect | Title `xemu \| v…` vs `git describe`; `pgrep -fl xemu` | Kill the stale process, relaunch | build |
| Finder ≠ terminal | `env \| grep -E 'MVK_CONFIG\|XEMU_'` in each; bundle plist | Export nothing, then one variable at a time | launch env |
| Small Morton magenta squares; stale or late textures | nsprof `tex_upload` active? | `XEMU_TEX_BIND_RECHECK=1` | V`texture.c` bind fast path |
| Large screen-aligned magenta rects | Effective prefill | Export `MVK_CONFIG_PREFILL_METAL_COMMAND_BUFFERS=0` | MVK config |
| Garbled geometry | Recent vertex change? | `XEMU_VTX_EXACT=0` | V`vertex.c` |
| Depth artifacts after a render-target switch | Same switch every time? | `XEMU_ZETA_SHAPE_READBACK=1` | V`surface.c` zeta switch |
| AGX `setVisibilityResultMode` crash | Prefill 0? `primer_index` present? | `XEMU_MAX_QUERIES=64`: gone at 64 = capacity; crashes either way = primer/first pass | V`reports.c`/`draw.c` |
| `vkWaitForFences timeout … (>5s)` abort | Which wait (`where`) | `sample xemu 5`; usually a MoltenVK deadlock | V`command.c` |
| Frozen picture / black screen at level load | Just loaded a snapshot? (failed loads toast and resume) | `XEMU_PFIFO_HEARTBEAT=1` (see below) | `pfifo.c` |
| SIGSEGV/SIGILL after a JIT change | Fault address in a MAP_JIT region? | `dsp_jit.enabled=false`; then audit the W^X windows | J |
| SIGSEGV `flatview_read_continue` (OHCI) | Build contains `13afd5f244`? | Check the walking thread is RCU-registered | `physmem.c` |
| Startup hang, dock icon bounces | `XEMU_DSP_JIT_DIFF*` set? | Unset it; else `sample xemu 5` | DSP diff validator |
| Boot wedge with validators on | More than 2 refuters stacked? | Keep 2; `DIFF=10` | APU `d->lock` |
| Audio crackle | `XEMU_APU_PROF=1` ≈100% or <187 frames/s? | Per run: `dsp_jit.enabled=false` (then also `use_dsp_jit=false`); `XEMU_DSP_JIT_DIFF=10` | APU/DSP |
| loadvm "Unknown section …usb-hub" | Pads bound at save time? CLI `-loadvm` always fails | Monitor `loadvm` after boot, with `usb-hub` at `1.<port_map[i]>`, `port_map={3,4,1,2}` | `ui/xemu-input.c` |
| fps dropped | nsprof `draws per_flip` within 15%? (if not, a different scene) | Diff `finish_*` and `fence_wait`; then `XEMU_REPORTS_SYNC=1`, then `XEMU_VTX_EXACT=0` | VK sync policy |

## Diagnostics

| Knob / output | Healthy | Bad |
|---|---|---|
| `XEMU_NV2A_NSPROF=1`: 5 s intervals, `N flips (X/s)` + per-counter `ev total per_flip max` | `draws`/flip = scene ID (menu ~3, attract 350-390, heavy-f1 ~679, heavy-w2 ~323); `renderpass` ~14 (~28 heavy); `fence_wait` 1-3.5 ms; `finish_flip_stall` ≈1; `pipeline_gen` ≈0 warm; `flip_idle` ≈1000/fps at cap | Passes in the hundreds; `fence_wait` ≥8 ms with few draws (sync bug); `pipeline_gen` bursts = stutter; `flip_idle`→0 = below cap. Drop the first interval after loadvm |
| `XEMU_APU_PROF=1`: `apu: utilization N% (M frames/s)`, covers VP+DSP | 19-25%, ~187 frames/s | ≈100%, or frames/s well below 187 |
| `XEMU_PFIFO_HEARTBEAT=1`: every ~2 s, `iters halt flush sync waiting_flip …` | `iters` advancing | `iters` frozen = PFIFO stuck; `flush`/`sync` stuck at 1 = hung in fence/submit; `waiting_flip` stuck with `flip_idle` growing = vblank/present stuck |
| `XEMU_DSP_JIT_STATS=1` (exit, GP core) | executed ≫ translated; ALU ~99% inlined, cf ~94% | A big `cf_fallback` bucket = inline it next; only a GP block prints (the EP has no JIT state); `auto-throttled on GP core` = churn >1/16; ALU/CF counters are translate-time and process-wide |
| `XEMU_DSP_JIT_DIFF=N` (1 = every translation, N = sample), `_SYNC` | Silence | `DIFF FAILURE in block pc_start=…`, then abort |
| `XEMU_DSP_JIT_PIN_AUDIT=1` | No output | `pin audit FAIL: which= pc=` (no abort) |
| `XEMU_X87_REFUTE`, other refuters | `violations=0` with a large `checks=` | Any violation; never left on during an fps run |

Notes: read `XEMU_INV_PROF`'s `(f)` line only from an `XEMU_ELIDE_CANDOIO=0` run. `info jit` "TLB full flushes" is always 0 on this guest; use (Δpartial+Δelided)/16. Host: `sample xemu 5 1 -file /tmp/s.txt`; `xcrun xctrace record --template 'Time Profiler' --attach xemu --time-limit 10s --output /tmp/x.trace`. A vCPU thread at ~95% doesn't prove CPU-bound (Azurik busy-polls); look at PFIFO starvation (cond_wait − `flip_idle`) instead.

## Glossary (fork-specific)
- **flip**: guest `FLIP_STALL`; flips/s = guest fps (cap 60). **finish**: `pgraph_vk_finish(reason)` = submit + wait on the previous slot's fence.
- **zeta shape switch**: Azurik flips one depth allocation between 1280×480 and 256×256 swizzled every frame.
- **zpass report**: occlusion query delivered at flip or slot reclaim; plain submit after 300 µs idle (`XEMU_REPORTS_BUDGET_US`). `XEMU_REPORTS_SYNC=1` is the only path to the legacy `STALLED` drain (under icount: eager delivery before every PFIFO park).
- **EP** DSP runs at 1/8 of GP's rate and churns on P-space overlays. **hard_fpu**: x87 math in double precision (`hard_fpu=false` for exact). **Anchors**: `azurik-heavy-f1`, `-w2`, F5, listed in `../fixtures/README.md`.

## Analysis recipes
1. **Budget**: sum all nsprof counters against measured frame time; if short, instrument more before ranking causes.
2. **CPU- or GPU-bound**: remove a wait class structurally. If total wait and fps stay flat, it is GPU time; confirm with a second, structurally different attempt at the same draws/flip.
3. **Bisect**: list the full good-vs-bad delta (launch path!), reproduce the failure first, flip one variable per fresh launch, run the all-but-X control, and score frames with an automated oracle.
4. **Driver crash survives a structural fix**: read the pinned MoltenVK version's source; fix by construction.
5. **Pool cadence changes**: worst-case demand vs per-slot capacity; add a runtime guard with its own finish reason; prove it fires by shrinking the pool (`XEMU_MAX_QUERIES=64`).
