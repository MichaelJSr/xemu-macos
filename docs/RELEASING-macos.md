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
          --json tagName --jq '.[0].tagName')
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
