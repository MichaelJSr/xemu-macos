# v0.14 release notes — DRAFT

Draft body for the GitHub release, in the short format of
`docs/RELEASING-macos.md`. Delete this file once the release is published;
the release body becomes the canonical notes.

Title: `v0.14 — Fix the post-loadvm crash; live texture bounds checks; retrained PGO`

---

macOS (Apple Silicon) release; ad-hoc-signed `xemu.app` for macOS 14+.

A stability release: a long-standing crash fixed, renderer and CPU
fixes from a code review, dead experiment knobs removed, and the
profile-guided build retrained on the current code. No performance
change.

**Changes**
- Fixed an occasional crash soon after loading a savestate, or during
  play, in the USB controller path. The cause was a use-after-free in
  the GPU surface-tracking list that USB transfers walk; upstream xemu
  has the same bug.
- A malformed guest texture now stops the emulator with an error
  instead of reading past the end of video memory.
- Surface uploads no longer reuse staging memory the GPU may still be
  reading (possible garbled textures or a corrupted video overlay).
- A full occlusion-query pool no longer corrupts the next draw's render
  state; the capacity check now runs before per-draw setup.
- After the guest remaps a code page, the CPU's return-address cache
  drops entries for that page (stale code was possible, never seen).
- Profile-guided optimization retrained on this release's code; the
  previous profile dated from 2026-08-04.
- Removed the off-by-default knobs `XEMU_TB_RANGE_INV`, `XEMU_RETC_BITS`,
  `XEMU_X87_ELIDE_CLEAN`, `XEMU_PRESENT_DRAWABLE_FIRST`, `XEMU_HARDENING`
  and `XEMU_PGRAPH_MMIO_STATS`.
- Debug builds now run the Vulkan renderer's internal assertions;
  release builds are unchanged.

**Validation:** fps parity with v0.13.3 on the heavy savestate (31.05 vs 30.89 fps, 6 interleaved pairs, signs 3/3); 200-frame artifact soak through a Finder launch clean; the crash reproduced on demand before the fix (3/3) and not after it (120 savestate loads); CI green.

**Full Changelog**: https://github.com/MichaelJSr/xemu-macos/compare/v0.13.3...v0.14
