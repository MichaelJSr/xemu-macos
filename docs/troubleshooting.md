# Troubleshooting

Problems players hit, and the settings that trade quality for speed.
Config keys are `xemu.toml` keys; on macOS the file and the data folder
are in `~/Library/Application Support/xemu/xemu/`. The macOS
presentation backend is `[display.window] presentation_backend`: `auto`
(the default) presents through Metal with the Vulkan renderer and
through OpenGL otherwise, and `metal` or `opengl` forces one. It is read
at startup only. For anything marked "report it", include the title and
the scene. Build failures are in
[building.md](building.md#build-failures); diagnostic `XEMU_*` switches
are in [optimizations.md](optimizations.md).

## Performance settings

| Setting | Effect |
|---|---|
| `display.quality.surface_scale` | Internal resolution. `1` is the Xbox's 640x480, `2` is 1280x960, `4` is 2560x1920. Fractions (`0.25`, `0.5`, `0.75`) render below native: less detail, a real frame-rate gain |
| `display.metalfx_mode` | `spatial` or `temporal` MetalFX upscaling to the window (Vulkan renderer). Under the OpenGL presentation backend, input past the 1920 px cap (2560x1920 at `surface_scale = 4`) is skipped; the Metal backend has no cap |
| `display.filtering` | `linear` and `nearest` are plain blits; `fsr`, `bicubic` and `lanczos` upscale, and only act when the frame is enlarged. `display.fsr_sharpness` sets FSR's RCAS sharpening, 0 (off) to 1 (maximum) |
| `display.frame_interpolation` | MetalFX midpoints between real frames, macOS 26 (below) |
| `perf.async_pipeline_compile` | A draw whose pipeline is new compiles on a worker thread and is skipped until it is built (built synchronously after 200 ms), so long compiles stay off the frame. MoltenVK and KosmicKrisp only; off under `-icount`; restart to apply |
| `perf.optimizations` | Keeps GPU work in VRAM: an NV09F surface blit becomes a VRAM-to-VRAM copy instead of a download and upload. Restart to apply |
| `perf.cache_shaders` | Caches compiled shaders on disk to reduce stutter |

### Frame interpolation

`frame_interpolation` (macOS 26, with MetalFX upscaling on) shows a
generated midpoint before each real frame without changing the
simulation rate. A mode runs only while a guest frame lasts that many
refreshes (2 for `2x`, 4 for `4x`; on from 90% of that, off below 75%),
and frames longer than 50 ms count as hitches, which skip it. Otherwise
real frames go out as with it off, saving about 4 ms of GPU time per
frame at 2880x2160 on an M2 Ultra.

| Panel | `2x` | `4x` |
|---|---|---|
| 60 Hz | Runs at or below ~33 fps (once on, up to 40); off at the 60 fps cap | Never fits; acts as `2x` |
| 120 Hz | Runs up to the 60 fps cap | 4x at or below ~33 fps (once on, up to 40), 2x above |

Each step is held for whole refreshes. MetalFX makes one midpoint per
real frame, so `4x` shows it for three of four steps and holds the real
frame back longer: `2x` is smoother wherever both fit. With an unknown
refresh rate, or on the OpenGL presentation backend, the configured mode
runs every frame, as with `XEMU_INTERP_GATE=0` (which also drops the
whole-refresh holds).

### Present stalls

On the Metal presentation backend the UI thread waits at most 100 ms
for a CAMetalLayer drawable. A frame with none in time is dropped, and
input, the input pipe and `screendump` keep running at about 60 Hz
until drawables come back; menus and notifications wait. Each stall
emits the QMP event `XEMU_PRESENT_STALL` and logs a start and an end
line:

```
xemu: present stall N: no drawable for M ms; dropping frames until one comes
xemu: present stall N over after M ms, F frames dropped
```

nsprof counts the drops as `present_drop`. `XEMU_PRESENT_STALL_MS=<ms>`
sets the bound; `0` waits without a bound, as v0.16 did.

## Display and Vulkan

**Black or garbled window, or Metal init failure at launch.** Set
`[display.window] presentation_backend = 'opengl'` and report it.

**Crash at launch with "duplicate LC_RPATH".** An old build: rebuild.

**Vulkan renderer aborts at launch after an unclean exit.** The shader
cache now writes atomically and validates every entry before the driver
sees it. To reset it by hand, delete `spirv_cache_v*/` in the data
folder, or turn off `perf.cache_shaders` (Settings > Cache shaders to
disk).

**Stutter for the first minutes after an upgrade.** The SPIR-V cache is
keyed on the linked glslang version, so after a glslang change every
shader compiles once into a fresh `spirv_cache_v*/`. It settles; the old
directory can be deleted.

**Objects missing for a few frames the first time a scene shows.** With
`perf.async_pipeline_compile` on, a draw with a new pipeline is skipped
until the pipeline is built. Turn it off (restart) to compile in line.

**MetalFX not activating.** It needs `renderer = 'VULKAN'` and
`metalfx_mode = 'spatial'` or `'temporal'`; see the OpenGL backend cap
above. The log says `MetalFX: Spatial upscaler initialized ...` when it
starts.

**Frame interpolation not working.** It needs macOS 26 and MetalFX
upscaling, and it turns itself off when the display can't show its steps
(above).

**Interpolation ghosting on moving objects.** Should be fixed; report
the title if it persists.

**FMV shows a black screen with audio on Vulkan** (games that play movies
through the PVIDEO overlay, such as Midtown Madness 3). Fixed after
v0.16.5 by willemabc's overlay fix; update.

**Wrong GPU, or "No supported Vulkan device found for the GPU driving
the display".** On Windows and Linux the Vulkan renderer uses the GPU
its OpenGL context runs on; macOS takes the first compatible device. To
override, set `[display.vulkan] force_physical_device` to a name from
the log's `Available physical devices:` list, exactly (a trailing ` *`
marks the matched device; the pick is logged as
`Selected physical device: ...`). xemu never writes this key; it
replaces `preferred_physical_device`.

**KosmicKrisp chosen but MoltenVK runs.** KosmicKrisp has to load, start
and pass an acceptance check, or xemu logs
`xemu: KosmicKrisp unavailable (<why>); using MoltenVK`. A fatal Vulkan
error or crash on KosmicKrisp leaves `kosmickrisp-strike.txt` in the
data folder, and later launches use MoltenVK until xemu or the bundled
KosmicKrisp changes. Picking KosmicKrisp in Settings again removes the
strike. Details: [kosmickrisp.md](kosmickrisp.md).

**xemu exits or aborts after GPU errors.** After repeated GPU errors
macOS stops taking the process's GPU work: headless xemu prints
`xemu: Vulkan device lost; exiting` and exits 1, windowed xemu aborts
with a crash report. Any other failed Vulkan call prints
`vk_result = N in <function> (<file>:<line>)`; headless xemu then exits
1 after `xemu: Vulkan call failed; exiting`, windowed xemu aborts. GPU
errors are logged as they happen (MoltenVK warnings, and
`xemu: Metal command buffer "..." failed on the GPU`). `[display]
renderer = 'OPENGL'` avoids MoltenVK and the compositor, but runs
Azurik's heavy scene about 3x slower. Report it.

**Abort with `<file>:<line>: bounds check failed: <expr>`.** The Vulkan
renderer's guest-driven bounds checks are real checks in every build.
It is a title this fork has not seen, not a host problem: report it.
There is no switch for it.

**Depth artifacts after a render-target switch.** A depth surface
re-shaped at the same address skips its GPU-to-CPU readback, because
titles clear it before drawing. Report it.

**Stale or late-updating textures.** Textures are content-checked once
per frame, so a CPU write between two binds of the same texture in one
frame lands a frame late. Report it.

## Emulation

**FPU precision bug.** Set `[perf] hard_fpu = false`. The inline FPU
uses IEEE double (52-bit mantissa) instead of x87 extended (64-bit);
this is extremely rare.

**Audio glitches.** For DSP-heavy titles, check that the DSP JIT is on:
`[audio.dsp_jit] enabled` on ARM64 macOS and Linux, `[audio]
use_dsp_jit` on other hosts (and on ARM64 with the first one off).

**More battery drain or fan noise on a laptop.** The emulated CPU's
thread and the main-loop thread run at macOS's user-interactive QoS
class, like the GPU threads, so they keep a performance core under
background load. `XEMU_VCPU_QOS=0` leaves them at the default class.

**Hitches while other heavy apps load the GPU and every core.** The
emulated CPU sleeps in the Xbox kernel's idle loop after 50 µs instead
of spinning until the next interrupt, which frees a fifth to two thirds
of a core at the 60 fps cap. Waking it can take a millisecond or more
when no core is free; `XEMU_KI_IDLE_HALT=0` restores the spin.

**A reset hangs on a black screen (v0.16 and older).** About 1 reset in
100 after loading a state (the Reset menu item or QMP `system_reset`)
never got past the Xbox kernel, because a pending GPU interrupt was
lost. Since v0.16.1 a reset clears the GPU's interrupt state as a cold
boot does; `XEMU_NV2A_RESET_IRQ=0` restores the old behaviour.

## Savestates

**A savestate from this build fails to load on an older xemu-macos.**
Expected: since the 2026-09-07 upstream merge the NV2A device state is
vmstate v4. Older savestates load here; the reverse does not work.

**A savestate fails to load after changing System Memory.** A savestate
loads only into the RAM size it was saved with (`[sys] mem_limit`). See
[automation.md](automation.md) for the RAM sizes.

## Windows

**"Failed to initialize Vulkan renderer", or a fallback to OpenGL.**
The Vulkan loader (`vulkan-1.dll`) comes from the GPU driver; xemu
bundles none. Update the vendor driver. VMs usually have no Vulkan ICD,
so the fallback is expected there. `vulkaninfo` shows what the loader
sees.

**Presentation glitches, or `win32_dxgi present failed or unavailable`
in the log.** Windows presents through a DXGI flip-model swapchain
(`WGL_NV_DX_interop`), so a vsync wait can't stall the PFIFO thread
behind the driver's GL swap lock. When it can't start, xemu logs that
line once and falls back to `SDL_GL_SwapWindow`, which brings the stall
back. Report the GPU and driver.

**Black screen with working audio on Vulkan.** Fixed in v0.13.2; update.
On older builds, set `renderer = 'OPENGL'`.
