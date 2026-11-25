# Releasing xemu-macos

Releases are cut from `master` of `MichaelJSr/xemu-macos`, never
upstream. This clone has several remotes and no gh default repo, so
every `gh` command names `--repo MichaelJSr/xemu-macos`. macOS arm64 is
the tested artifact; Windows and Linux ship CI-compiled with the caveats
in `docs/windows-gating-audit.md`. Group everything since the last tag
into one release.

**Published tags are immutable.** Never delete, move, force-push or
reuse one: deleting it turns its release back into a draft, and moving
it re-runs the release workflow against the published assets. Cut a new
version instead.

## Steps

1. **Preflight:** clean tree, `git log origin/master..HEAD` empty, new
   work since `gh release list --repo MichaelJSr/xemu-macos
   --exclude-pre-releases --limit 1`, CI green.
2. **Tag:** `git tag -a vX.Y.Z -m "vX.Y.Z — <headline>"` (minor for a
   feature or perf batch, patch for fixes only), `git push origin
   vX.Y.Z`.
3. **CI drafts it:** `release-on-tag.yml` builds the matrix and creates
   a draft with exactly **6 assets**: the source `.tar.zst`, the macOS
   universal zip, the Windows x86_64 and arm64 zips, and the x86_64 and
   aarch64 AppImages. `-dbg` builds and `-pdb` zips stay workflow
   artifacts. Attach nothing else.
4. **Rewrite the notes** (below).
5. **Publish:** `gh release edit vX.Y.Z --repo MichaelJSr/xemu-macos
   --draft=false`, then `gh release view vX.Y.Z --repo
   MichaelJSr/xemu-macos --json assets,isDraft`: 6 assets, sizes within
   about 10% of the last release (a swing means a bundling bug).

Releases are ad-hoc signed, without hardened runtime
(`XEMU_CODESIGN_ENTITLEMENTS=1`). `scripts/sign-macos-release.sh` is for
notarized builds; say so in the notes when it was used. Releases bundle
MoltenVK `XEMU_MOLTENVK_VERSION` from `build.sh` (logged as `Bundling
MoltenVK from …`), while the dev box tests the `/usr/local` MoltenVK
from `scripts/build-moltenvk.sh`. A MoltenVK pin bump is a renderer
change: run the `xemu-operate` skill's Test section before trusting it.

## Notes

Title `vX.Y.Z — <headline>` (em dash, about 70 characters; join the two
biggest unrelated changes with `;`). Body about 25 lines, at most about
ten one-line bullets:

```md
macOS (Apple Silicon) release; ad-hoc-signed `xemu.app` for macOS 14+.

<One sentence: what this release is.>

**Before upgrading**
- <Real caveats only (savestates, cold shader caches, changed defaults).>

**Changes**
- <One line per user-visible change, plain English.>

**Validation:** <One line: headline evidence, goldens, CI.>

**Full Changelog**: https://github.com/MichaelJSr/xemu-macos/compare/vA...vB
```

Omit Before upgrading when there are no caveats. No function names or
root-cause narratives; concrete numbers for perf claims, never "much
faster"; no emoji, callouts or screenshots.

## KosmicKrisp pin bump

Pins in `build.sh`: `kk_mesa_tag`, `kk_mesa_sha`, `kk_respin` and
`kk_asset_sha256`. The asset is the prerelease
`kosmickrisp-<mesa-tag>[-<respin>]` (never `latest`).

1. `git ls-remote https://gitlab.freedesktop.org/mesa/mesa.git
   'refs/tags/<tag>^{}'` for the sha (retry on a 504).
2. `gh workflow run kosmickrisp-asset.yml --repo MichaelJSr/xemu-macos
   -f mesa_tag=<tag> -f mesa_sha=<sha>`. It builds, asserts provenance
   and publishes; it refuses an existing tag.
3. One commit: the new tag, sha and sha256 with `kk_respin=` cleared,
   plus the asset's `share/kosmickrisp/LICENSES` copied over
   `licenses/kosmickrisp` (`build.sh` refuses notices that differ).
4. Gate before a release ships it (`docs/kosmickrisp.md`): C1's check
   (the stream-e2 walk on KK at least 40 times at load 70+, all
   identical), and for the next asset an `-exported_symbols_list` of
   `_vk_icd*` with `nm -gU` asserts (rc1 exports 268 symbols). A bump
   meant to put Auto back on KK must pass the whole Auto-back gate (The
   Auto-back gate, What flips Auto back).

**Respin** (a script, LLVM or SPIR-V change, or a bad asset): `-f
respin=<n>` from 2 publishes `kosmickrisp-<mesa-tag>-<n>` holding
`kosmickrisp-<mesa-tag>-<n>-arm64.tar.xz`, and PROVENANCE's `release=`
line names it (the workflow asserts it). Step 3 then sets
`kk_respin=<n>` and `kk_asset_sha256` (and `licenses/kosmickrisp` if
the notices moved), and the commit body names the release. Gate on the
published asset.

## Local packaging (CI down)

Fix packaging bugs in `build.sh`'s `package_macos`, never by hand.
`XEMU_ARM_CPU=apple-m1 XEMU_PGO=use ./build.sh`, check the `Bundling`
lines, `codesign --verify --deep --strict dist/xemu.app`, then `cd dist
&& zip -r -y -q xemu-vX.Y.Z-macos-arm64.zip xemu.app LICENSE.txt`
(about 14 MiB; much smaller means a dylib is missing). For universal,
copy that bundle aside first: `./build.sh -a x86_64` reconfigures
`build/` and rewrites `dist/`. Then merge as `build-macos.yml`'s
`build_universal` job does: unzip both into one `xemu.app`, `lipo
-create` the two `Contents/MacOS/xemu`, re-sign. Upload with `gh release
upload vX.Y.Z --repo MichaelJSr/xemu-macos <zip>`.
