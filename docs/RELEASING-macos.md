# Releasing xemu-macos

How releases are cut on
[`MichaelJSr/xemu-macos`](https://github.com/MichaelJSr/xemu-macos).
Intended audience: AI coding agents (and humans) preparing a release
on this fork. Rewritten 2026-07-05 to match the CI release flow; a
local `build.sh` packaging fallback is at the end.

## Scope

- Releases are cut from the fork's `master` branch. Do **not** target
  upstream xemu-project/xemu.
- CI builds every platform, but macOS arm64 is the tested,
  first-class artifact (macOS 14+; macOS 26 Tahoe recommended for
  MetalFX + frame interpolation). Windows/Linux artifacts ship as
  CI-compiled and carry the caveats in
  `docs/windows-gating-audit.md`.
- One release per fix / perf batch. If multiple unrelated commits
  have landed since the last tag, group them into a single release
  whose notes list all of them.

## Version numbering

- Fork-versioned tags: `v0.9`, `v0.10`, `v0.10.1`, … — bump the
  minor for a feature/perf batch, the patch for a fix-only release.
  (The older upstream-suffixed scheme ended at `v0.8.153-macos.1`,
  which remains published.)
- Use annotated tags with the release title as the message:
  `git tag -a vX.Y[.Z] -m "vX.Y[.Z] — <headline>"`. (`v0.9` is a
  historical lightweight tag; everything since is annotated.)
- Tags are immutable once published. Do **not** reuse, delete, or
  force-push a tag that has a release: deleting a published
  release's tag silently flips the release back to draft (it
  vanishes from the public releases page), and force-updating a tag
  re-fires the release workflow against already-published assets.

## Release flow (CI — the normal path)

1. **Preflight.** Working tree clean, branch pushed, and the release
   contains new work:

   ```bash
   git status
   git log origin/master..HEAD --oneline   # must be empty
   LAST=$(gh release list --repo MichaelJSr/xemu-macos --limit 1 \
          --exclude-pre-releases --json tagName --jq '.[0].tagName')
   git log "$LAST"..HEAD --oneline                      # must be non-empty
   ```

   Commits follow the fork conventions (imperative title with an
   area prefix, why-first body, measured numbers; agent-authored
   commits end with the `Co-Authored-By:` line the agent's harness
   supplies, e.g. `Co-Authored-By: Claude Opus 5.5
   <noreply@anthropic.com>` as of 2026-10-10).

2. **Tag and push.**

   ```bash
   git tag -a vX.Y.Z -m "vX.Y.Z — <headline>"
   git push origin vX.Y.Z
   ```

3. **CI builds a draft release.** The `v*` tag push triggers
   `.github/workflows/release-on-tag.yml` (owner-guarded to
   `xemu-project` / `MichaelJSr`), which runs the full build matrix
   and creates a **draft** release named after the tag, with the
   body seeded from `.github/scripts/gen-changelog.py` and these
   assets attached: source `.tar.zst`, Windows x86_64/arm64 zips,
   the macOS **universal** zip, and Linux AppImages.

4. **Rewrite the notes.** Replace the generated changelog with notes
   in the format below, keeping the title `vX.Y.Z — <headline>`.

5. **Publish the draft** (owner action in the GitHub UI or
   `gh release edit vX.Y.Z --draft=false`), then verify:

   ```bash
   gh release view vX.Y.Z --repo MichaelJSr/xemu-macos \
     --json tagName,name,assets,isDraft,isPrerelease
   ```

   Confirm the asset set is complete and sizes are within ~10% of
   the previous release (sudden swings almost always mean a bundling
   bug). Optional smoke test: download the macOS zip, unzip, launch.

### MoltenVK release parity (know what you are shipping)

Release artifacts bundle the **pinned official MoltenVK**
(`XEMU_MOLTENVK_VERSION` in `build.sh`) because CI runners have no
system copy — while the dev machine builds and tests against the
custom `/usr/local` pipeline (`scripts/build-moltenvk.sh`, currently
a newer pin). `build.sh` logs the bundled version + UUID (`Bundling
MoltenVK from …`) precisely so this tested-vs-shipped driver gap
stays visible. Treat any MoltenVK pin bump as a renderer-behavior
change: run the `xemu-operate` skill's Test section before trusting
it for release.

### KosmicKrisp pin bump

KosmicKrisp is built from a pinned Mesa tag, never on a push. The
pins live in `build.sh` (`kk_mesa_tag`, `kk_mesa_sha`,
`kk_asset_sha256`), and the asset is the prerelease
`kosmickrisp-<mesa-tag>` holding `kosmickrisp-<mesa-tag>-arm64.tar.xz`
(dylib, ICD manifest, `share/kosmickrisp/PROVENANCE`, `LICENSES/`).
These prereleases are never `latest`; release tooling that wants the
last xemu release passes `--exclude-pre-releases`.

1. **Resolve the tag's commit:** `git ls-remote
   https://gitlab.freedesktop.org/mesa/mesa.git
   'refs/tags/<tag>^{}'`. GitLab sometimes answers 504; retry.
2. **Dispatch:** `gh workflow run kosmickrisp-asset.yml --repo
   MichaelJSr/xemu-macos -f mesa_tag=<tag> -f mesa_sha=<sha>`. Its
   `build` job (`macos-26`, read-only token) builds with
   `scripts/build-kosmickrisp.sh package` and asserts the provenance
   (`Mesa <ver> (git-<sha10>)`, arm64, system-only deps, minos 26.0,
   SDK 26, the `vk_icd` entry points). Its `publish` job (Ubuntu, the
   only one that can write) checks the sha256 and PROVENANCE again and
   publishes the prerelease, tagged at the dispatching commit, with
   the sha256 in its body. Both refuse a tag that already exists,
   because published tags are immutable.
3. **One commit** that updates the tag, sha and sha256 together in
   `build.sh`.

**Respin.** To rebuild a Mesa tag that's already published (a script,
LLVM or SPIR-V pin change, or a bad asset), add `-f respin=<n>`,
counting from 2: the release is then `kosmickrisp-<mesa-tag>-<n>`,
holding `kosmickrisp-<mesa-tag>-<n>-arm64.tar.xz`, and PROVENANCE's
`release=` line names it. Step 3 then changes only `kk_asset_sha256`;
its commit body names the release.

`scripts/build-kosmickrisp.sh package` builds the same tarball
locally (in `../kk` by default, with source, build and install kept
per tag), but with the local Xcode. Gate on the published asset, not
on a local build.

## Release title format

- `vX.Y.Z — <headline>` with an em-dash (`—`, not hyphen).
- Headline ≤ ~70 chars, descriptive of the primary change:
  - `v0.8.150 — Fix single-frame pink-tile flashes on 2/4-bpp textures`
  - `v0.8.151 — VRAM snapshot closes texture-upload tear race; zero-copy GPU unswizzle`
- If a release bundles unrelated changes, separate the two most
  prominent with `;`; don't try to list all of them.

## Release notes body format

Keep notes short (owner preference since 2026-10-10). A reader should
take in the whole body in under a minute. Aim for about 25 lines or
fewer. Mechanism, root cause and full evidence go in commit bodies and
`docs/`, not here.

```md
macOS (Apple Silicon) release; ad-hoc-signed `xemu.app` for macOS 14+.

<One sentence: what this release is, e.g. "Two upstream syncs and a
dependency refresh; no performance change.">

**Before upgrading**
- <Only real upgrade caveats: savestate compatibility, cold shader
  caches, changed defaults. Omit the section when there are none.>

**Changes**
- <One line per user-visible change, plain English.>
- <Group minor items: "Windows: 6 hardening fixes from the native-port
  audit.">

**Validation:** <One line with the headline evidence, e.g.
"azurik-heavy-f1 fps parity with vA (<b> vs <e> fps); goldens
unchanged; CI green.">

**Full Changelog**: https://github.com/MichaelJSr/xemu-macos/compare/vA...vB
```

Style rules:

- One line per bullet, and roughly ten bullets at most. Merge related
  items rather than listing each commit.
- No function names, register names or root-cause narratives. Link a
  doc if a reader needs the detail.
- Give concrete numbers for performance claims, and never
  "significantly" / "much faster" / "greatly improved".
- No emojis, GitHub-Flavoured-Markdown callouts (`> [!NOTE]`) or
  screenshots.

## Local packaging fallback

Only for when CI is unavailable. `build.sh`'s `package_macos` is the
recipe: fix packaging bugs there, never by hand.

1. `XEMU_ARM_CPU=apple-m1 XEMU_PGO=use ./build.sh` builds `dist/xemu.app`
   and `dist/LICENSE.txt`; check its `Bundling MoltenVK from …` line.
2. `codesign --verify --deep --strict dist/xemu.app`, then
   `cd dist && zip -r -y -q xemu-vX.Y.Z-macos-arm64.zip xemu.app LICENSE.txt`.
   Expect the zip at ~12–13 MiB; much smaller means a dylib or MoltenVK
   wasn't bundled.
3. For the universal zip, keep that bundle, run `./build.sh -a x86_64`
   (it reconfigures `build/` and rewrites `dist/`), then merge the two
   as `build-macos.yml`'s `build_universal` job does: unzip both into
   one `xemu.app`, `lipo -create` the two `Contents/MacOS/xemu`, re-sign.
4. `gh release upload vX.Y.Z <zip>` onto the draft.

## Do-not-do list

- Don't delete/re-push/force-update a published tag (see Version
  numbering — this has bitten before).
- Don't publish without rewriting the generated changelog into the
  notes format.
- Don't use hardened-runtime signing
  (`XEMU_CODESIGN_ENTITLEMENTS=1`) for standard releases;
  `scripts/sign-macos-release.sh` exists for notarized builds and
  its use belongs in the notes.
- Don't attach extra assets to a CI draft (unpackaged binaries,
  PDBs); the CI asset set is the payload.
- Don't cut a release from a dirty tree or an unpushed branch.
