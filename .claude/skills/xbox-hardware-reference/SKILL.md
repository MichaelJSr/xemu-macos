---
name: xbox-hardware-reference
description: >-
  Domain-theory reference for the Xbox hardware this emulator models — NV2A
  GPU (PFIFO/PGRAPH/surfaces/swizzle/zpass/vblank), DSP56300 ISA (X/Y/P
  memory, 56-bit accumulators, REP/DO loops, CCR flags), MCPX APU voice
  processor, x87 floatx80 numerics, USB pad topology, and the
  Apple-GPU/MoltenVK constraints (TBDR, single queue, visibility buffer)
  that shape this fork's optimizations. Load this when you need to
  understand WHY a mechanism exists before changing it: what a
  register/method name means (NV097_*, NV_PGRAPH_*, NV_PAVS_*, NV1BA0_*),
  what PFIFO/PGRAPH/flip/zpass/swizzle/TBDR/floatx80/parmove/REP-DO/voice
  actually are, why pass-count is a first-order cost on Apple GPUs, why the
  DSP JIT packs 56-bit accumulators sign-extended, why voice registers must
  be re-read from guest RAM, or why hard_fpu is a double-precision
  approximation of an 80-bit type. This is background theory, not a runbook
  — for symptom triage see xemu-debugging-playbook, for the
  incident-by-incident story see xemu-failure-archaeology, for
  fork-specific design decisions see xemu-architecture-contract, for the
  JIT's own roadmap see docs/dsp-jit-design.md.
---

# Xbox hardware reference

This skill is the domain-theory layer: the hardware and numerics facts a
mid-level engineer needs *before* a perf or correctness change makes
sense. Every fact below is tied to a file, register, or function in this
repo — this is not a general Xbox-modding wiki. Read it before touching
NV2A, the DSP JIT, the APU voice pipeline, x87 FPU code, USB pad wiring,
or anything under `hw/xbox/nv2a/pgraph/vk/`, when you need the *why*, not
just the *how*. Facts current as of 2026-07-04 unless marked otherwise;
re-verify commands are in "Provenance and maintenance" at the end.

Section index: **§1** Machine at a glance · **§2** NV2A (PFIFO, PGRAPH,
flips/FLIP_STALL, vblank, surfaces/swizzle, zpass, PVIDEO, vertex modes) ·
**§3** DSP56300 ISA for the JIT · **§4** APU voice processor · **§5** x87
numerics · **§6** USB/input topology · **§7** Apple GPU + MoltenVK
constraints · register-prefix cheat table · When NOT to use · Provenance.

---

## 1. Machine at a glance

The guest is a fixed, non-configurable machine: one specific PC clone from
1999-2001, not a generic x86 target.

- **CPU**: Intel Pentium III-class core. `hw/xbox/xbox.c:457` sets
  `m->default_cpu_type = X86_CPU_TYPE_NAME("pentium3")`, and
  `m->max_cpus = 1` (`hw/xbox/xbox.c:452`) — single-core, no SMP. (733 MHz
  is a well-known public spec of the retail Xbox; the emulator does not
  model clock speed directly — TCG runs guest instructions as fast as the
  host can translate/execute them, gated by the 60 Hz vblank pacing in
  §2.)
- **RAM**: unified memory architecture (UMA) — the GPU has no separate
  VRAM; it reads/writes the same physical RAM as the CPU, asserted
  directly in code ("xbox is UMA - vram *is* ram", `hw/xbox/nv2a/nv2a.c`)
  where NV2A's PCI VRAM BAR is a memory-region **alias** over the same RAM
  (`nv2a_init_memory`, `nv2a.c:203-221`). Size is a boot-time choice:
  `sys.mem_limit` enum `['64','128']` default `'64'` (MiB,
  `config_spec.yml:381-384`); `system/vl.c:3056` converts it to `-m`:
  `mem = (mem_limit + 1) * 64` — `'64'` = retail-accurate, `'128'` = a
  common softmod RAM upgrade some titles support.
- **Machine model**: `hw/xbox/xbox.c`, class `xbox_machine_options`.
  Notable non-default PC-machine flags: `no_floppy=1`, `no_cdrom=1`
  (Xbox uses a custom DVD/IDE path, not PC CD-ROM), `has_acpi_build =
  false`, `smbios_defaults = false` — several standard PC-machine
  subsystems are explicitly turned off.
- **Firmware boot chain**: MCPX boot ROM (512 bytes, hardware mask ROM —
  `xbox_flash_init`'s loader asserts the size, `hw/xbox/xbox.c:151-153`)
  is mapped at the top of the BIOS flash region and is what the CPU
  executes at reset; it decrypts/validates and jumps into the flash
  **BIOS** image (`bios_name = ms->firmware ?: "bios.bin"`, `xbox.c:70`).
  Both paths come from `xemu.toml [sys.files]` —
  `bootrom_path`/`flashrom_path` (`config_spec.yml:391-392`), consumed as
  `-bios <flashrom_path>` and the `bootrom` machine property
  (`xbox.c:143-146`). Missing/invalid flash blocks autostart with a
  settings error (`ui/xemu.c`); missing bootrom falls back to an
  all-`0xff` blob (`xbox.c:100-102`) rather than hard-failing.
- **Video output**: `avpack` machine property (`xbox.c:365-383`,
  `config_spec.yml:385-390`) models which A/V cable is plugged in —
  `scart|hdtv|vga|rfu|svideo|composite|none`, default `hdtv`. This is a
  connector-type value surfaced to guest software via SMBus
  (`hw/xbox/smbus_xbox_smc.c`); it does not affect vblank timing — see
  the PAL note in §2.

## 2. NV2A for this repo

NV2A is the Xbox's GPU, an NVIDIA NV2A (an NV20-family/GeForce3-class
part with an integrated vertex/pixel pipeline and no separate video
memory — see §1 UMA). Emulated in `hw/xbox/nv2a/`. The renderer backend
this fork optimizes is Vulkan-on-MoltenVK
(`hw/xbox/nv2a/pgraph/vk/`) — §7 covers the Apple-specific constraints
on top of these NV2A concepts.

### PFIFO — the command FIFO, i.e. "the renderer thread"

The guest doesn't call into PGRAPH directly. It writes 3D commands
(method + parameter words) into a ring buffer in RAM; **PFIFO** is the
NV2A engine that pulls those words out and dispatches them to PGRAPH. In
this emulator PFIFO is a real OS thread
(`qemu_thread_create(&d->pfifo.thread, "nv2a.pfifo_thread", pfifo_thread,
...)`, `hw/xbox/nv2a/nv2a.c:235-236`) with a **pusher**
(`pfifo_run_pusher`, walks the guest's DMA command buffer) and a
**puller** (`pfifo_run_puller`, decodes one method+parameter and applies
it to PGRAPH) — both in `hw/xbox/nv2a/pfifo.c`. **"PFIFO thread" is this
repo's name for the render thread.** Nearly all `nsprof` counters
(SHADER_GEN, TEX_UPLOAD, FENCE_WAIT, DRAW, RENDERPASS, …) accrue here
because PGRAPH's Vulkan renderer calls happen synchronously inside the
puller's method dispatch — "on the PFIFO thread" means "blocking the GPU
command-processing loop," the highest-leverage place to add or remove
latency in this codebase.

### PGRAPH — the 3D engine, methods named NV097_*

**PGRAPH** is NV2A's 3D graphics engine state machine — the thing that
knows what a triangle, texture stage, or blend mode is. Its command
namespace is `hw/xbox/nv2a/nv2a_regs.h` (~1405 `NV*` defines total; 462
`NV_PGRAPH_*` register-level defines and 469 `NV097_*` object-class
methods — sibling namespaces, not subset/superset — e.g.
`NV097_SET_SURFACE_FORMAT = 0x208`, `NV097_DRAW_ARRAYS = 0x1810`,
`NV097_SET_BEGIN_END = 0x17FC`). Handlers live in
`hw/xbox/nv2a/pgraph/pgraph.c`: 128 plain `DEF_METHOD(NV097, ...)`
handlers plus INC/INT/NON_INC/RANGE variants (240 DEF_METHOD-family
entries in that file today).

### Flips and FLIP_STALL — why "flips/s" is this repo's fps metric

A **flip** is the guest's frame-boundary swap: finish rendering into one
surface, ask the display to present it, start the next. The guest signals
this with `NV097_FLIP_STALL` (`DEF_METHOD(NV097, FLIP_STALL)`,
`hw/xbox/nv2a/pgraph/pgraph.c:989-997`), which flushes pending draws,
tells the renderer to stall, and sets `pg->waiting_for_flip = true`.
PFIFO's puller won't dispatch the next method while that flag is set
(`pfifo_stall_for_flip`, `hw/xbox/nv2a/pfifo.c:141-154`) until
`is_flip_stall_complete` sees the surface's read/write counters equalize
(`pfifo.c:122-137`, comparing `NV_PGRAPH_SURFACE_READ_3D` vs `WRITE_3D`)
— i.e. until the display has consumed the previously-written buffer.
**Consequence**: counting flips per second *is* counting in-game fps —
one flip is one displayed frame, and the guest is capped at 60 Hz by the
vblank thread below, so neither can exceed 60 for a game paced on vblank.

### The vblank thread and the missing PAL path

xemu drives its own display-refresh cadence independent of any guest
timer: a dedicated vblank thread (`vblank_timer_thread`,
`ui/xemu.c:848-877`) fires at a hardcoded `vblank_interval_ns =
16666666LL` (`ui/xemu.c:78`) — exactly 60 Hz, fixed at compile time, not
derived from `avpack`. Each tick calls `process_vblank` →
`graphic_hw_update` (`ui/xemu.c:822-834`), which raises
`NV_PCRTC_INTR_0_VBLANK` on the NV2A side (`nv2a_vga_gfx_update`,
`hw/xbox/nv2a/nv2a.c:195-203`) — what many Xbox titles use as their
master timing source (poll or interrupt on vblank count to pace game
simulation, independent of PGRAPH throughput). **There is no 50 Hz/PAL
vblank path anywhere in this repo** — grepping `hw/xbox/` and `ui/` for
`PAL`/`50 Hz` finds nothing outside the `avpack` connector-type enum
(§1), which only affects what the guest believes is plugged in, not the
interrupt cadence. A title that on real PAL hardware would simulate at
50 Hz instead runs at this emulator's fixed 60 Hz — a known, unfixed gap,
worth knowing before chasing "wrong speed" reports on PAL-region titles.

### Surfaces (color/zeta), shape, and swizzle

A **surface** is an NV2A render target — color or zeta (depth/stencil) —
backed by a byte range in RAM (RAM *is* VRAM, §1). Its **shape** struct is
`SurfaceShape` (`hw/xbox/nv2a/pgraph/surface.h:25-33`): `color_format`,
`zeta_format`, `log_width`/`log_height`, `clip_*`, `anti_aliasing`. Layout
— linear (row-major, "pitch") vs **swizzled** — is a separate axis, set
via `NV097_SET_SURFACE_FORMAT_TYPE` = `TYPE_PITCH` (0x1) or
`TYPE_SWIZZLE` (0x2) (`hw/xbox/nv2a/nv2a_regs.h:884-886`).

**Swizzle = Morton order.** The Xbox interleaves the bits of x/y (and z,
for volumes) coordinates so spatially-nearby texels are also nearby in
memory — a Z-order/Morton curve. Mask generation is
`generate_swizzle_masks` (`hw/xbox/nv2a/pgraph/swizzle.c:48-66`, comment
cites the Z-order Wikipedia article); the box copy is
`swizzle_box_internal` (`swizzle.c:72+`). **This is why torn or mid-copy
texture corruption shows up as small squares in a
checkerboard/fractal-looking pattern** rather than horizontal tearing
bands — you're seeing the Morton curve's tiling laid bare. The README's
pink-tile incident names this signature directly ("Morton-tiled magenta
[blocks]", `README.md:339,448`) as the texture-corruption fingerprint,
distinct from large screen-aligned rectangles (a compositor/present/
driver-level issue instead) — see xemu-debugging-playbook for that
symptom-to-cause split.

### Zpass occlusion reports

**zpass** (occlusion query) is how a game asks "of everything I just drew,
how many pixels passed the depth test?" — used to skip shading occluded
objects. Guest sequence: `NV097_SET_ZPASS_PIXEL_COUNT_ENABLE` to arm,
`NV097_CLEAR_REPORT_VALUE` to reset a counter slot, `NV097_GET_REPORT`
(type `ZPASS_PIXEL_CNT`) to read one back
(`hw/xbox/nv2a/nv2a_regs.h:1149-1156`). Titles poll these every frame to
decide what to draw next, so report latency gates the next frame's
PGRAPH work. This fork maps each report to a `VkQueryPool` occlusion
query (`hw/xbox/nv2a/pgraph/vk/reports.c`); §7 covers why *when* a query
begins relative to render-pass boundaries caused two separate production
incidents (a pass-count regression and an AGX driver crash).

### PVIDEO overlay

**PVIDEO** is NV2A's video overlay engine — a second scanout path for YUV
video playback (DVD, cutscenes) composited by the CRTC rather than going
through PGRAPH. Registers (`NV_PVIDEO_*`, 40 defines) are handled in
`hw/xbox/nv2a/pvideo.c`: `NV_PVIDEO_BUFFER` sets the overlay source,
`NV_PVIDEO_STOP` tears it down only on bit 0 (`pvideo.c:64-68` — the guest
can write `STOP` repeatedly mid-playback without disabling the overlay).
Actual pixel compositing is stubbed out here
(`// d->vga.enable_overlay = true` is commented out) — worth knowing if a
title's video playback looks wrong.

### Inline vertex data vs. vertex arrays

NV2A supports several ways for the guest to hand PGRAPH vertex data;
this fork's Vulkan draw path (`hw/xbox/nv2a/pgraph/vk/draw.c`,
`pgraph_vk_flush_draw` ~line 2900) has one branch per mode:

| Guest submission mode | pgraph_t field | Draw path |
|---|---|---|
| Indexed draw from vertex-array buffers | `draw_arrays_start/count[]` | `vkCmdDrawIndexed`/`vkCmdDraw` over bound attribute buffers |
| `NV097_ARRAY_ELEMENT16`/`32` index stream | `inline_elements[]` | Rebases to a tight `[min,max]` range, remaps attributes, `vkCmdDrawIndexed` |
| `NV097_INLINE_ARRAY` (whole vertex bytes, no separate arrays) | `inline_array_length` | Bespoke inline-array bind path |
| Direct per-attribute floats (`glVertex`-style immediate mode) | `inline_buffer_length` / `VertexAttribute.inline_buffer` | CPU-side vertex assembly into a staging buffer |

Mutual-exclusion asserts (`nv2a_vk_assert(pg->inline_array_length == 0)`
etc.) confirm a draw call only ever uses one mode — which branch fires
matters when profiling "why is this draw slow": inline/immediate-mode
paths pay a CPU-side vertex-buffer build cost indexed vertex-array draws
skip (`vertex.c` holds attribute-binding helpers shared across paths).

## 3. DSP56300 essentials for the JIT

The MCPX APU's programmable audio cores are Motorola **DSP56300**-family
(DSP56001/M56001-lineage) 24-bit fixed-point DSPs — two of them, per §4.
This is the ISA-level background the ARM64 JIT
(`hw/xbox/mcpx/apu/dsp/interp/dsp56k_jit_arm64.c`, design doc
`docs/dsp-jit-design.md`) has to preserve bit-for-bit.

- **24-bit words.** Every register and every X/Y/P memory cell is a
  24-bit quantity stored in a 32-bit host word (`dsp_core_t.xram/yram/
  pram` are `uint32_t[]`, `hw/xbox/mcpx/apu/dsp/interp/dsp_cpu.h:63-65`)
  — why the JIT is full of 24-bit masks and sign-extensions instead of
  using the host's native width directly.
- **X, Y, P memory spaces**, sizes verified in
  `hw/xbox/mcpx/apu/dsp/interp/dsp_cpu_regs.h:113-115`:
  `DSP_XRAM_SIZE = 4096`, `DSP_YRAM_SIZE = 2048`, `DSP_PRAM_SIZE = 4096`
  words. X and Y are data memory (the DSP56300's dual data-bus Harvard
  architecture — one ALU operand fetched from X, another from Y, same
  cycle — which parmove below exploits); P is program memory. A small
  **mixbuffer** window sits inside X space, `DSP_MIXBUFFER_BASE = 0x1400`,
  size 1024 words (`dsp_cpu_regs.h:117-118`) — see §4's VP→DSP handoff.
- **56-bit accumulators A and B**, each split into three register-file
  slots: `a2`/`b2` (8-bit extension), `a1`/`b1` (24-bit high/mantissa),
  `a0`/`b0` (24-bit low) — `DSP_REG_A0/A1/A2/B0/B1/B2` and the combined
  `DSP_REG_A`/`DSP_REG_B` (`dsp_cpu_regs.h:59-66`). This fork's JIT packs
  the three slots into **one 64-bit ARM64 register**, sign-extended, so
  ALU ops become single native instructions instead of a three-word
  load/mask/combine sequence — contract documented at
  `dsp56k_jit_arm64.c:759-766` ("Phase 2 primitives... 56-bit accumulator
  arithmetic... sign-...extends"). A/B stay resident across a whole
  translated block in callee-saved x26/x27 (`dsp56k_jit_arm64.c:1425-1442`,
  which also pins X0:X1/Y0:Y1 in x20/x21). Reading an accumulator back as
  a plain 24-bit value has a fast inlined path **only when scaling mode
  is 0** (`dsp56k_jit_arm64.c:2056-2065` — see the CCR bullet below).
- **Parallel moves (parmove).** A DSP56300 instruction can execute an ALU
  op *and* up to two data moves (register↔memory) in the same cycle — the
  architecture's signature throughput trick, enabled by the separate X/Y
  buses. The interpreter's dispatch table is `opcodes_parmove[16]`
  (`docs/dsp-jit-design.md` §1, backed by
  `hw/xbox/mcpx/apu/dsp/dsp_emu.c.inc`); the JIT must emit both the ALU op
  and the move(s) from one decoded instruction word — see
  `docs/dsp-jit-design.md` §2.2 for why that drove hand-written ARM64
  emission instead of TCG (out of scope here).
- **REP/DO hardware loops and the postexecute PC rewind.** `REP` repeats
  the *next single instruction* N times with no per-iteration branch
  overhead; `DO` is a zero-overhead hardware loop over an instruction
  range. Both are **PC bookkeeping done after normal instruction
  execution**, not control flow during it — `dsp_postexecute_update_pc`
  (`hw/xbox/mcpx/apu/dsp/interp/dsp_cpu.c:688-736`). For `REP`: if
  `dsp->loop_rep` is set and `pc_on_rep == 0`, decrement `LC`, and if
  still nonzero set `cur_inst_len = 0` — this cancels the normal
  `pc += cur_inst_len`, so **the PC does not move** and the same
  instruction runs again next cycle. For `DO`: after the normal PC
  increment, if the new PC equals `LA + 1` (one past the loop's last
  instruction) and `LC` hasn't hit zero, PC rewinds to `SSH` (the
  loop-top address on the stack). **This is why a naive "skip past
  REP/DO in the JIT, let the interpreter loop" design broke**: the
  loop-continuation decision reads state *after* the instruction ran,
  not a decoded branch target up front — any JIT block boundary that
  skips this postexecute step desyncs the guest's effective PC. The
  "sentinel measured 9.75% legitimate PC divergence" finding (mode-6
  `calc_ea` two-word parmoves plus REP/DO PC rewind) is this exact
  mechanism surfacing as a false-positive validator divergence before
  the instruction-length classifier fix landed.
- **CCR flags E/U/N/Z/V/C and scaling modes S0/S1.** Condition Code
  Register bit positions (`dsp_cpu_regs.h:34-45`): `C`=0 (carry), `V`=1
  (overflow), `Z`=2 (zero), `N`=3 (negative), `U`=4 (unnormalized), `E`=5
  (extension — extension byte is a pure sign-extension of mantissa bit
  23), `L`=6 (limit/saturate sticky bit). Scaling is a separate 2-bit
  field, `S0`/`S1` at SR bits 10-11 (`dsp_cpu_regs.h:44-45`), controlling
  how the 56-bit accumulator shifts when read back as a 24-bit
  parallel-move value. The JIT is explicit that **scaling mode 0 is the
  universal case in Xbox audio code** (`dsp56k_jit_arm64.c:2057-2061`:
  "the limited 24-bit read is a1 when the 56-bit accu is a pure sign
  extension of a1... the universal case on Xbox audio") — it inlines only
  that case and falls back to a helper (`dsp56k_jit_helper_pm_read_accu24`)
  for the other three, a safe net precisely because it's rarely taken.
- **GP vs EP cores and frame cadence.** See §4 for what these cores *do*;
  the frame-driving loop is `mcpx_apu_dsp_frame`
  (`hw/xbox/mcpx/apu/dsp/gp_ep.c`, called once per **VP frame**, §4).
  GP runs **every** VP frame when its reset bits are set
  (`gp_ep.c:479-483`); EP runs only when `d->ep_frame_div % 8 == 0`
  (`gp_ep.c:505-508`) — **EP executes at 1/8th GP's rate**. Retranslation
  churn is a separate, per-block fact with the opposite shape: **EP is
  the high-churn core** — ~1 retranslation per 10 block executions,
  because its program overlays P-space every pass, invalidating and
  retranslating each time — while GP is healthy at ~1 per 10,000+
  (`21e9045344`: EP 418k translations against 4.2M executions vs GP 1.3k
  against 14.5M — a gap of ~3 orders of magnitude). The 1:8 frame cadence
  only scales how often each core runs in wall-clock terms; it does not
  produce the churn-ratio gap (mechanism and throttle response:
  `xemu-failure-archaeology` 4.6).
- **Peripheral space at `0xFFFF80`.** The top 128 words of X memory
  (`DSP_PERIPH_BASE = 0xFFFF80`, `DSP_PERIPH_SIZE = 128`,
  `dsp_cpu_regs.h:120-121`) are memory-mapped I/O, not real RAM — routed
  through `read_peripheral`/`write_peripheral` callbacks on `dsp_core_t`
  (`dsp_cpu.h:88-89`) instead of plain array indexing. A JIT fast path
  that inlines memory access must branch to the slow/callback path once
  the address reaches this base — see "Fast path: if addr < 0xc00, direct
  array access" (`dsp56k_jit_arm64.c:1739`).

For the JIT's own architecture (translation-unit boundaries, block
chaining, the diff-validator, W^X handling) see `docs/dsp-jit-design.md`
— this section is ISA background, not JIT design.

## 4. APU voice processor (VP)

The **APU** (MCPX Audio Processing Unit — `hw/xbox/mcpx/apu/`, PCI device
comment "MCPX Audio Processing Unit", `apu.c:628`) sits inside **MCPX**
(the Xbox southbridge ASIC; this repo's `hw/xbox/mcpx/` directory also
houses the audio codec interface `aci.c` and the network MAC `nvnet/`,
matching the "APU + I/O" framing). The APU has two halves: a fixed-function
**voice processor (VP)** that mixes up to `MCPX_HW_MAX_VOICES = 256`
independent audio voices (`hw/xbox/mcpx/apu/apu_regs.h:330`), and the two
programmable **DSP56300 cores** from §3 (GP/EP) that post-process the VP's
mix.

- **Per-voice pipeline**: each voice has pitch (sample-rate conversion),
  an ADSR-style envelope (`NV_PAVS_VOICE_CFG_ENV0/ENVA/ENV1/ENVF` register
  groups, consumed throughout `hw/xbox/mcpx/apu/vp/vp.c`), and a filter
  stage — a **state-variable filter (SVF)** (`hw/xbox/mcpx/apu/vp/svf.h`,
  ported from a LADSPA plugin) supporting low-pass/high-pass/band-pass/
  band-reject/all-pass (`F_LP`/`F_HP`/`F_BP`/`F_BR`/`F_AP`, `svf.h:17-21`)
  — titles predominantly use it as an LPF for distance/occlusion cues.
- **Mixbins**: voices don't write straight to the output; each is
  weighted into one or more of `NUM_MIXBINS = 32` accumulation buses
  (`apu_regs.h:334`), each holding `NUM_SAMPLES_PER_FRAME = 32` samples
  per VP frame (`apu_regs.h:333`). The GP DSP core reads the mixbins as
  its input — the hand-off point between fixed-function VP and
  programmable DSP.
- **HRTF 3D voices**: a voice can route through a Head-Related Transfer
  Function filter for positional 3D audio instead of simple pan/volume —
  `hrtf_filter_set_target_params`/`hrtf_filter_clear_history`
  (`hw/xbox/mcpx/apu/vp/hrtf.h`, called from `vp.c`), keyed by a per-voice
  HRTF target handle (`NV_PAVS_VOICE_CFG_HRTF_TARGET`).
- **Why voice registers live in guest RAM and mutate via DMA.** Each
  voice's parameter block is `NV_PAVS_SIZE = 0x80` bytes
  (`apu_regs.h:220`), **living in guest RAM** at `NV_PAPU_VPVADDR +
  voice_handle * NV_PAVS_SIZE` — real hardware reads these from system
  memory rather than dedicated registers, and guest code can rewrite them
  any time via ordinary stores (a DMA-style shared-memory contract, not a
  register API). This fork adds a per-voice stack-resident cache
  (`d->vp.filters[v].voice_buf`, `vp.c:1640-1689`) so a hot voice's
  fields skip the `ram_ldl`/`ram_stl` round-trip within one frame — but
  every accessor (`voice_get_mask`, `voice_get_word`, `voice_set_mask`,
  `vp.c:281-345`) has an explicit **fallback to `ram_ldl`/`ram_stl`
  against `NV_PAPU_VPVADDR`** for when `voice_buf` is absent, plus a
  `qatomic_read` on `VPVADDR` since the guest can relocate the whole
  voice table live ("rare but observed on title transitions," `vp.c:
  284-291`). **The failure mode this guards against**: an earlier
  `__thread`-local cache (README "Voice register `__thread` cache" row)
  went stale because it read a voice's fields once and reused them,
  missing writes the guest made directly to RAM out-of-band — since the
  voice block *is* just memory, any cache must either invalidate eagerly
  or always fall through to the authoritative RAM copy, as this design
  does.
- **GP DSP mixbuffer handoff.** Once a VP frame's mixbins are computed,
  `mcpx_apu_dsp_frame` (`hw/xbox/mcpx/apu/dsp/gp_ep.c:450-475`) converts
  them float→24-bit fixed-point (`float_to_24b_bulk`, vectorized) and
  writes them into the GP core's X memory at `DSP_MIXBUFFER_BASE` —
  preferentially one bulk call, `dsp_write_mixbuffer_bulk` (`dsp.h:131`),
  falling back to a per-sample `dsp_write_memory` loop when the active
  engine doesn't support the bulk path. This is the exact boundary
  between fixed-function voice mixing and programmable DSP
  post-processing.
- **Frame thread + workers.** The whole VP+DSP pipeline for one frame
  runs on a dedicated `mcpx.apu_thread` (`apu.c:464`), paced by
  `EP_FRAME_US = 5333` microseconds (`apu_regs.h:363`, "256/48000 sec
  (~5.33ms)" — one VP frame is 256 samples at 48 kHz) via `throttle()`
  (`apu.c:172-217`), which paces against the host audio queue's fill
  level rather than a plain sleep, to avoid drift. Per-voice work
  (pitch/envelope/filter) is farmed across a worker pool:
  `audio.vp.num_workers` (`config_spec.yml:332-335`, default 0 = auto)
  resolves to `SDL_GetNumLogicalCPUCores()` clamped to
  `MAX_VOICE_WORKERS` (`vp.c:2218-2219`) — this fork's voice-processing
  parallelization knob, separate from the DSP engine selection in §3.

## 5. x87 numerics

Xbox game code (and this emulator's baseline x86 interpretation) uses the
**x87 FPU**, whose native type is **`floatx80`**: an 80-bit extended
format with a 64-bit explicit mantissa, 15-bit exponent, and 1 sign bit
(vs. IEEE-754 **double**'s 52-bit mantissa / 11-bit exponent / 1 sign
bit). QEMU's softfloat type `floatx80` stores this as a 16-bit `.high`
(sign+exponent) plus a 64-bit `.low` (mantissa) — visible directly in
this repo's hard-float conversion code
(`target/i386/tcg/fpu_helper.c:200+`, `floatx80_to_double_fast`/
`double_to_floatx80_fast`).

- **What `hard_fpu` trades.** This fork's ARM64 inline-x87 optimization
  (`perf.hard_fpu`, default `true`) does not emulate the 80-bit type at
  all: it converts every `floatx80` operand to a native IEEE **double**
  (52-bit mantissa), does the arithmetic in host double precision, and
  converts back (`floatx80_add__hard`/`sub`/`mul`/`div__hard`,
  `target/i386/tcg/fpu_helper.c:284-306`). The code says so outright:
  *"XXX: Currently emulating double extended precision with double
  precision when using hard floats"* (`target/i386/cpu.h:2828-2829`,
  repeated at `fpu_helper.c:82-83`) — a **12-bit mantissa reduction**
  (64→52 bits) per operation, a real precision loss that's "almost always
  invisible" because (a) Xbox game code rarely chains enough consecutive
  x87 ops on one value for the rounding error to matter visually or
  logically, and (b) the softfloat path this replaces is itself already
  an emulation, so bit-exactness against real hardware was never
  guaranteed either way. The ARM64 path is branchless with one unlikely
  branch for zero/inf/NaN/denormal/overflow (`fpu_helper.c:203-253`),
  claimed "~3-6x speedup over softfloat for typical Xbox game math"
  (`fpu_helper.c:206`). Compiles only on `XBOX && (__x86_64__ ||
  __aarch64__)` (`fpu_helper.c:76-77`); x86_64 hosts get a simpler `long
  double`-based hard path in the same file (`fpu_helper.c:78-171`).
- **Inline emission gate.** A second, independent switch,
  `TCG_TARGET_HAS_fpu`, controls whether x87 FP ops emit as *native
  inline AArch64 FP instructions in the TCG stream* (fastest) versus
  calling the `__hard`/`__soft` C helpers above (still fast, but a
  function-call boundary per op). It's `1` on AArch64 hosts and forced to
  `0` on `_WIN32` (`tcg/aarch64/tcg-target-has.h:56-71`): with the
  generic TCG FP paths reachable, llvm-mingw's Windows cross-compiler
  fails to constant-fold every `qemu_build_not_reached()` branch guarding
  aarch64-only code, leaving undefined references at link time.
  **Windows/ARM64 therefore always uses the helper-call hard-FPU path**,
  never the fully-inlined one — a deliberate, documented divergence from
  macOS-arm64 codegen, not an oversight.
- **FPU control word (FPUC) and why every write must resync flags.**
  x87's rounding mode (`RC`, bits 10-11) and precision control (`PC`,
  bit 9) are guest-writable any time (`FLDCW`, `FNSTCW`/`FLDENV`,
  task/segment restores, `XRSTOR`, SEV launch state — all funnel through
  `cpu_set_fpuc` hits in `cpu.c`, `machine.c`, `sev.c`,
  `xsave_helper.c`, `tcg/fpu_helper.c`). `cpu_set_fpuc`
  (`target/i386/cpu.h:2821-2845`) is the **single funnel**: besides
  storing `env->fpuc`, it re-derives `HF_FPU_PC_MASK`/`HF_FPU_RC_MASK` in
  `env->hflags` (`cpu.h:2833-2842`) because this fork's translator bakes
  the guest's rounding mode into `tb->flags` **at translation time**
  (`gen_flcr`, `target/i386/tcg/translate.c:1650-1679`: "HF_FPU_RC is
  baked into tb->flags at translation time... every TB currently
  translating thus pins the guest RC bits to a compile-time-known
  value") so it can pick the matching native `FCVT*` rounding instruction
  for `FIST`/`FISTP` at codegen time instead of reading a runtime
  register per conversion. `cpu_set_fpuc` also invalidates a
  translator-side cache (`env->cached_fpuc_rc = 0xFFFF`,
  `cpu.h:2843-2845`) so a rounding-mode change from **outside the normal
  instruction stream** (savestate restore, `XRSTOR`, SEV) can't be
  silently ignored by already-translated code holding the old mode. The
  rule this generalizes to: any path that sets `env->fpuc` directly
  instead of calling `cpu_set_fpuc` desyncs hflags/RC caching and
  produces wrong rounding in already-JIT'd blocks.
- **Signaling vs. quiet NaN compares — FCOMI vs FUCOMI.** x87 has two
  compare families differing only in NaN behavior: `FCOM`/`FCOMI`
  **signal** (raise invalid-operation) on any NaN, quiet or signaling;
  `FUCOM`/`FUCOMI` only signal on a *signaling* NaN, passing a *quiet*
  NaN through silently. This repo's helpers implement that directly:
  `helper_fcomi_ST0_FT0` calls `floatx80_compare` (`fpu_helper.c:990`)
  while `helper_fucomi_ST0_FT0` calls `floatx80_compare_quiet`
  (`fpu_helper.c:1003`) — same split for the non-`I` forms
  (`fpu_helper.c:967,977`). The ARM64 hard path's compare
  (`fpu_helper.c:322-330`) reduces both operands to double, which is safe
  for ordering but relies on the *quiet-vs-signaling dispatch happening
  at the call site* (which helper got called), not inside the comparison
  — worth knowing if you touch the hard-FPU compare path, since it
  doesn't independently re-check NaN signaling-ness.

## 6. USB/input topology

Xbox controllers attach through a two-level USB topology that this fork
constructs dynamically per bound controller, in
`ui/xemu-input.c:xemu_input_bind` (~line 770-820):

1. **A `usb-hub` per controller port.** For player slot `index` (0-3), a
   hub is created at `port="1.<port_map[index]>"` with `ports=3`
   (`xemu-input.c:783-789`). `port_map = {3, 4, 1, 2}` (`xemu-input.c:261`)
   — the mapping from player-facing slot to physical USB root-port number
   is **not** the identity map; matters if you hand-construct `-device`
   args to match a snapshot (see below).
2. **A `usb-xbox-gamepad` (or other XID driver) on hub sub-port 1.** The
   gamepad is created at `port="1.<port_map[index]>.1"` with
   `index=<index>` (`xemu-input.c:800-807`) — it always occupies port 1
   of its own 3-port hub, leaving ports 2-3 free for XMU memory-unit
   expansion slots (`xemu_input_bind_xmu`, `xemu-input.c:824+`, using
   `port="1.<port_map[player]>.<slot>"` at slot 2 or 3).
3. **Neutral input for unbound pads.** `hw/xbox/xid.c:update_input`
   (`xid.c:60-70`) checks `xemu_input_get_bound`; if unbound, it returns
   early: *"Unbound pad reports neutral input (in_state stays at its
   initialized defaults) instead of asserting."* This is what lets
   headless/harness sessions instantiate the full 4-port USB topology via
   raw `-device` arguments (to satisfy a snapshot's saved device tree)
   without a real controller in every slot — an unbound pad reports "no
   input" forever rather than crashing.

**Why savestates embed this tree, and why topology mismatches break
loads.** A QEMU savestate serializes the full device tree, including
every `usb-hub`/`usb-xbox-gamepad` instantiated at snapshot time — not
just CPU/RAM/PGRAPH state. Loading into a VM whose USB tree doesn't
already match (e.g. no controller bound, so no hubs exist) fails with an
"Unknown section" error naming the missing device (`usb-hub` is the
common one), because the snapshot's state stream has data for a device
instance the freshly-booted machine doesn't have. This is also why the
CLI's `-loadvm` doesn't work here: xemu's UI attaches USB controllers
*after* initial boot (per the user's controller-binding config), but
`-loadvm` restores state *during* early boot, before that attachment —
see xemu-run-and-operate for the monitor-socket `loadvm` workaround.

## 7. Apple GPU + MoltenVK constraints as domain facts

Apple Silicon GPUs are architecturally different from the desktop
IMR (immediate-mode renderer) GPUs Vulkan was originally designed
around, and MoltenVK (the Vulkan-on-Metal translation layer this fork
runs on) has specific behavioral limits on top of that. Both are
external facts this codebase must design around — verified here by
where this repo depends on each one.

- **TBDR (tile-based deferred renderer) — pass count is a first-order
  cost.** Apple GPUs split the framebuffer into small tiles, rasterize
  and shade each tile entirely in fast on-chip memory, and only write the
  finished tile to main memory at pass end — so **every render pass
  boundary forces a full tile memory load-then-store cycle** per
  attachment, regardless of how little work happened in that pass. One
  Vulkan render pass per draw call (instead of batching draws into one
  pass) pays this load/store tax once per draw instead of once per frame.
  This is the measured, named root cause of this fork's single largest
  win: the occlusion/report rework (2026-07-03) cut render passes per
  flip from **376 to 14** (the old design forced a pass teardown on every
  occlusion-query reset, since `vkCmdResetQueryPool` — next bullet — is
  illegal inside a pass), driving the savestate-bench scene from 15.78 to
  38.57±0.80 fps (+144%) per commit `6bfbc22863`'s protocol receipt
  (README prose rounds this change to 35.6 fps / 2.25x) and fence-wait
  per flip from 31.9 ms to 2.3 ms (README figures; the commit records
  2.2 ms on the new path). "One
  render pass per draw ≈ one full tile load/store per draw" is spelled
  out in the fix's own comment (`hw/xbox/nv2a/pgraph/vk/draw.c:2029-2035`:
  "a full tile load/store cycle per draw on Apple GPUs").
- **MoltenVK exposes a single queue.** `queueCount=1` for the one queue
  family this renderer uses is a MoltenVK/Metal limitation, not a repo
  choice — documented where an earlier attempt to acquire a second
  (compute) queue is explained: "MoltenVK exposes queueCount=1 anyway"
  (`hw/xbox/nv2a/pgraph/vk/instance.c:503-508`; that second-queue design
  is a recorded failed experiment). Everything this renderer submits is
  strictly ordered by submission order on one queue — no true parallel
  GPU submission-stream overlap on this backend, so overlap comes from
  software pipelining (`NUM_FLIGHT_SLOTS = 2`,
  `hw/xbox/nv2a/pgraph/vk/renderer.h:542`) rather than multiple hardware
  queues.
- **The visibility-result buffer is bound at encoder creation, not
  lazily.** A MoltenVK/Metal quirk found the hard way:
  `MVKCommandBuffer::_needsVisibilityResultMTLBuffer` resets at
  `vkBeginCommandBuffer` and is only *set* by `vkCmdBeginQuery`; MoltenVK
  attaches the Metal visibility-result buffer to a render encoder **only
  if that flag is already set when the encoder for that pass is
  created**. Under an earlier design (immediate command-buffer prefill),
  a command buffer's first in-pass occlusion query could begin *after*
  the first pass's encoder already existed, so that encoder got created
  with no visibility buffer — and the driver crashed in
  `setVisibilityResultMode` the moment a query used it. This fork's fix
  is an **empty "primer" occlusion query**, begun and ended outside any
  render pass immediately after `vkBeginCommandBuffer` and the
  query-pool reset (`hw/xbox/nv2a/pgraph/vk/draw.c:2046-2069`) — it sets
  the needs-visibility-buffer flag before any pass begins, correct by
  construction regardless of when the first in-pass query lands. This is
  the two-act crash in xemu-failure-archaeology: a query-pool-capacity
  theory fixed a real but different bug first, and only reading
  MoltenVK's own source revealed this mechanism.
- **`vkCmdResetQueryPool` is illegal inside a render pass.** Standard
  Vulkan rule, not Apple-specific, but it's what forced the 376-pass
  design in the first place (per-query-rotation reset needed a pass
  teardown to be legal); the fix is a **bulk reset of the whole
  per-flight-slot query partition, once, at command-buffer-begin, outside
  any pass** — reorganizing *when* a legal operation happens, not working
  around the rule.
- **MSL translation path and why pipeline compiles are expensive.** This
  renderer generates SPIR-V from GLSL via `glslang`
  (`hw/xbox/nv2a/pgraph/vk/glsl.c`) and hands it to standard Vulkan
  pipeline-creation calls (`vkCreateGraphicsPipelines`,
  `hw/xbox/nv2a/pgraph/vk/draw.c:1027,1489`) — MoltenVK translates that
  SPIR-V to Metal Shading Language (MSL) and asks the Metal compiler to
  build a real GPU pipeline object, the expensive step (measured
  worst-case 164 ms for one pipeline, README.md:235). Since that cost is
  paid per unique permutation (blend state × shader × vertex layout ×
  render-target format) and titles can have thousands across a session,
  this fork persists the *compiled* pipeline cache to disk
  (`pipeline_cache.bin` in the settings base path,
  `hw/xbox/nv2a/pgraph/vk/draw.c:457-521`, loaded at startup and written
  back at flip boundaries every 30 seconds via
  `pgraph_vk_maybe_save_pipeline_cache`) so a warm run hits ≤4 ms instead
  of the cold 164 ms — a disk cache of already-compiled Metal state, not
  something that speeds up the first compile itself.
- **Metal argument buffers.** `MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=1`
  is one of the five MVK_CONFIG variables this fork pins at process start
  (`Info.plist` `LSEnvironment` and `ui/xemu.c:1610-1614`'s
  `setenv(..., 0)`) — it tells MoltenVK to bind Vulkan descriptor sets
  via Metal's argument-buffer feature (one indirectable buffer of
  resource references) instead of per-resource binding, changing how
  many Metal bind calls happen per draw. Treated as a fixed, tested
  production setting, not a per-title toggle — see the pink-tile saga
  (xemu-failure-archaeology) for why all five MVK_CONFIG values are
  pinned regardless of launch path.
- **`MAP_JIT` / W^X on Apple Silicon.** Apple Silicon enforces
  write-xor-execute on JIT pages: writable or executable, never both at
  once for a thread, toggled via `pthread_jit_write_protect_np` (wrapped
  here as `qemu_thread_jit_write()`/`qemu_thread_jit_execute()`). Both
  QEMU's TCG code-generation region (`tcg/region.c:717`, `flags |=
  MAP_JIT`) and this fork's DSP56300 ARM64 JIT
  (`hw/xbox/mcpx/apu/dsp/interp/dsp56k_jit_arm64.c:1288`) allocate code
  buffers this way and must bracket every emission region with
  `qemu_thread_jit_write()` and every call into generated code with
  `qemu_thread_jit_execute()` (over a dozen sites in the DSP JIT,
  `dsp56k_jit_arm64.c:9479-9881`). **The DSP JIT's W^X incident**: an
  early block-chaining self-patch ("chain-site repatching") wrote a
  branch target into already-emitted code *outside* the write-protect
  window — a latent fault that could appear to work (if the page was
  still write-enabled from a prior op) and then crash unpredictably once
  scheduling changed the timing. Fixed by moving the repatch strictly
  inside the JIT-write window, before the matching
  `qemu_thread_jit_execute()` (`dsp56k_jit_arm64.c:9850-9881`: "BEFORE
  qemu_thread_jit_execute() — chain_repatch_b writes").

## Reading register/method names — cheat table

| Prefix | Namespace | Defined in | Handled in |
|---|---|---|---|
| `NV_PGRAPH_*` | PGRAPH engine registers (control/state bits) | `hw/xbox/nv2a/nv2a_regs.h` | `hw/xbox/nv2a/pgraph/pgraph.c` |
| `NV097_*` | PGRAPH object-class methods (the 3D command stream) | `hw/xbox/nv2a/nv2a_regs.h` | `hw/xbox/nv2a/pgraph/pgraph.c` (`DEF_METHOD(NV097, ...)`) |
| `NV_PFIFO_*` | FIFO/DMA-pusher engine registers | `hw/xbox/nv2a/nv2a_regs.h` | `hw/xbox/nv2a/pfifo.c` |
| `NV_PCRTC_*` | CRT controller (scanout, vblank IRQ enable) | `hw/xbox/nv2a/nv2a_regs.h` | `hw/xbox/nv2a/pcrtc.c`, `nv2a.c` |
| `NV_PVIDEO_*` | Video overlay engine | `hw/xbox/nv2a/nv2a_regs.h` | `hw/xbox/nv2a/pvideo.c` |
| `NV_PRAMDAC_*` | RAMDAC (palette/video timing) | `hw/xbox/nv2a/nv2a_regs.h` | `hw/xbox/nv2a/pramdac.c` |
| `NV_PFB_*` | Framebuffer/memory-controller interface | `hw/xbox/nv2a/nv2a_regs.h` | `hw/xbox/nv2a/pfb.c` |
| `NV_PMC_*` | Master control (IRQ routing, chip ID, reset) | `hw/xbox/nv2a/nv2a_regs.h` | `hw/xbox/nv2a/pmc.c` |
| `NV_PTIMER_*` | GPU-side timer/alarm | `hw/xbox/nv2a/nv2a_regs.h` | `hw/xbox/nv2a/ptimer.c` |
| `NV_PBUS_*` | Internal PCI/bus config shadow registers | `hw/xbox/nv2a/nv2a_regs.h` | `hw/xbox/nv2a/pbus.c` |
| `NV_PAPU_*` | APU (audio) engine registers — GP/EP control, mixbuffer windows, IRQ | `hw/xbox/mcpx/apu/apu_regs.h` | `hw/xbox/mcpx/apu/apu.c`, `.../dsp/gp_ep.c` |
| `NV_PAVS_*` | Per-voice register block layout (the `NV_PAVS_SIZE`-byte struct in guest RAM) | `hw/xbox/mcpx/apu/apu_regs.h` | `hw/xbox/mcpx/apu/vp/vp.c` |
| `NV1BA0_PIO_*` | Voice-processor "PIO" submission-channel methods (voice on/off/pause, antecedent-voice list linking) | `hw/xbox/mcpx/apu/apu_regs.h` | `hw/xbox/mcpx/apu/vp/vp.c` |
| `DSP_REG_*` / `DSP_SR_*` | DSP56300 register-file indices / status-register bit positions | `hw/xbox/mcpx/apu/dsp/interp/dsp_cpu_regs.h` | `hw/xbox/mcpx/apu/dsp/interp/dsp_cpu.c`, `dsp56k_jit_arm64.c` |

General rule for this codebase: the `NV_<ENGINE>_*` prefix names an NV2A
(or APU) hardware **engine block** and its registers; `NV097_*` (and
`NV1BA0_*`) name **object-class method streams** — commands a driver
pushes through PFIFO/PIO rather than registers it pokes directly. Both
kinds of names always resolve to the same header
(`nv2a_regs.h` or `apu_regs.h`) regardless of which `.c` file you found
them in.

## When NOT to use this skill

- **Deciding whether a fork design choice is correct or load-bearing**
  (e.g. "should PFIFO really be its own thread," "is N=2 flight slots
  right") — that's `xemu-architecture-contract`, which covers invariants
  and known weak points, not hardware background.
- **Diagnosing a live symptom** (pink tiles, a crash, a hang, an fps
  regression) — start at `xemu-debugging-playbook`'s symptom table; come
  back here only if you need to understand a mechanism the playbook
  points you at.
- **The full incident story with numbers and dates** for the pink-tile
  saga, the visibility-buffer crash, the occlusion/report rework, etc. —
  that's `xemu-failure-archaeology`; this skill only states the
  standing domain fact each incident depended on, not the narrative.
- **The DSP JIT's own architecture** — translation-unit boundaries,
  block chaining, the async diff-validator, per-phase rollout history —
  that's `docs/dsp-jit-design.md`. This skill covers the DSP56300 *ISA*
  the JIT has to be correct against, not the JIT's own design.
- **Config flag catalog / what to set** — `xemu-config-and-flags` is the
  exhaustive list of every `xemu.toml`/`XEMU_*`/`MVK_CONFIG_*` knob with
  prod-vs-experimental labels; this skill only explains the hardware
  reason a handful of them exist.

## Provenance and maintenance

Facts in this skill were verified against the repo at commit `cf85e96597`
(2026-07-04, tag v0.9) unless stated as historical measurements (dated
separately, e.g. the 376→14 pass-count figures). Re-verify drift-prone
claims with:

- CPU/RAM/machine class: `grep -n 'default_cpu_type\|max_cpus\|mem_limit' hw/xbox/xbox.c system/vl.c config_spec.yml`
- PFIFO thread + flip stall: `grep -n 'qemu_thread_create.*pfifo_thread\|waiting_for_flip' hw/xbox/nv2a/nv2a.c hw/xbox/nv2a/pfifo.c hw/xbox/nv2a/pgraph/pgraph.c`
- Fixed 60 Hz vblank, no PAL path: `grep -n 'vblank_interval_ns' ui/xemu.c` then `grep -rn 'PAL\|50 *[Hh]z' hw/xbox ui`
- DSP memory sizes: `grep -n 'DSP_XRAM_SIZE\|DSP_YRAM_SIZE\|DSP_PRAM_SIZE\|DSP_MIXBUFFER' hw/xbox/mcpx/apu/dsp/interp/dsp_cpu_regs.h`
- Accumulator packing/pinning: `grep -n 'Phase 2 primitives\|Phase 8 register pinning' hw/xbox/mcpx/apu/dsp/interp/dsp56k_jit_arm64.c`
- REP/DO postexecute PC logic: `grep -n 'dsp_postexecute_update_pc' -A 40 hw/xbox/mcpx/apu/dsp/interp/dsp_cpu.c`
- GP/EP frame cadence (1:8 ratio): `grep -n 'ep_frame_div % 8\|Run GP\|Run EP' hw/xbox/mcpx/apu/dsp/gp_ep.c`
- Voice count / mixbins / voice struct size: `grep -n 'MCPX_HW_MAX_VOICES\|NUM_MIXBINS\|NUM_SAMPLES_PER_FRAME\|NV_PAVS_SIZE' hw/xbox/mcpx/apu/apu_regs.h`
- DSP engine precedence: `grep -n 'dsp_want_external_jit_engine' -A 10 hw/xbox/mcpx/apu/dsp/dsp.c`
- hard_fpu double-precision approximation: `grep -n 'emulating double extended precision' target/i386/cpu.h target/i386/tcg/fpu_helper.c`
- TCG_TARGET_HAS_fpu Windows gating: `grep -n 'TCG_TARGET_HAS_fpu' -B8 tcg/aarch64/tcg-target-has.h`
- FCOMI/FUCOMI signaling split: `grep -n 'floatx80_compare\b\|floatx80_compare_quiet' target/i386/tcg/fpu_helper.c`
- USB port/hub topology: `grep -n 'port_map\|usb-hub\|usb_input_bind' ui/xemu-input.c`
- xid.c neutral-input guard: `grep -n 'Unbound pad reports neutral' hw/xbox/xid.c`
- TBDR pass-count rationale + primer query: `grep -n 'tile load/store\|Prime MoltenVK' hw/xbox/nv2a/pgraph/vk/draw.c`
- Single-queue MoltenVK: `grep -n 'queueCount = 1' -B5 hw/xbox/nv2a/pgraph/vk/instance.c`
- Pipeline cache persistence: `grep -n 'pipeline_cache.bin\|PIPELINE_CACHE_SAVE_INTERVAL_NS' hw/xbox/nv2a/pgraph/vk/draw.c`
- MAP_JIT sites: `grep -rn 'MAP_JIT' tcg/region.c hw/xbox/mcpx/apu/dsp/interp/dsp56k_jit_arm64.c`
