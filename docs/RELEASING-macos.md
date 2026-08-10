# Releasing xemu-macos

How releases are cut on
[`MichaelJSr/xemu-macos`](https://github.com/MichaelJSr/xemu-macos).
Intended audience: AI coding agents (and humans) preparing a release
on this fork. Rewritten 2026-07-05 to match the CI release flow; the
manual packaging recipe is kept at the end as the fallback.

## Scope

- Releases are cut from the `macos-optimizations` branch. Do **not**
  target upstream or release from `master`.
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
   git log origin/macos-optimizations..HEAD --oneline   # must be empty
   LAST=$(gh release list --repo MichaelJSr/xemu-macos --limit 1 \
          --json tagName --jq '.[0].tagName')
   git log "$LAST"..HEAD --oneline                      # must be non-empty
   ```

   Commits follow the fork conventions (imperative title with an
   area prefix, why-first body, measured numbers;
   `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>` footer
   for agent-authored commits).

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
change: run the validation gauntlet (`xemu-validation-and-qa`)
before trusting it for release.

## Release title format

- `vX.Y.Z — <headline>` with an em-dash (`—`, not hyphen).
- Headline ≤ ~70 chars, descriptive of the primary change:
  - `v0.8.150 — Fix single-frame pink-tile flashes on 2/4-bpp textures`
  - `v0.8.151 — VRAM snapshot closes texture-upload tear race; zero-copy GPU unswizzle`
- If a release bundles unrelated changes, separate the two most
  prominent with `;`; don't try to list all of them.

## Release notes body format

The body is Markdown. Keep this shape — deviating makes diffs
between releases hard to read:

```md
macOS (Apple Silicon) release. Attached build is an ad-hoc-signed `xemu.app` for macOS 14+ (macOS 26 Tahoe recommended for MetalFX + frame interpolation).

## Highlights

<1–3 sentence summary of what's in this release and what problem it solves. If this is a small patch on top of the previous tag, say so explicitly, e.g. "Single bug-fix commit on top of v0.8.XYZ.">

### Vulkan renderer (pgraph/vk)

- **<Short bold title of change #1>.** <Plain-English explanation
  of the root cause / motivation (2–4 sentences), followed by the
  fix. Reference specific functions, buffers, and access masks by
  their code names in backticks. For perf changes, give concrete
  before/after numbers.>
```

Style rules:

- One `###` subsection per subsystem touched. Typical subsystems:
  `Vulkan renderer (pgraph/vk)`, `APU`, `TCG / JIT`,
  `MetalFX + presentation`. Skip subsystems with no changes.
- Each bullet starts with a bold short title followed by a period,
  then the prose. Don't use a colon after the bold title.
- Describe the **why** first (symptom / root cause / contended
  constraint) and the fix second. Historical context on prior
  related commits belongs inline, in 1–2 sentences.
- Prefer concrete numbers over adjectives for perf work. Avoid
  "significantly" / "much faster" / "greatly improved".
- No emojis. No GitHub-Flavoured-Markdown callouts (`> [!NOTE]`).
  No screenshots.

## Manual packaging fallback

Only needed if CI is unavailable or a local repackage must be
shipped. The canonical logic is `build.sh`'s `package_macos`
(MoltenVK selection: `resolve_moltenvk`, same file); mirror it —
never hand-evolve a parallel recipe. The essential steps, in order:

1. `rm -rf dist`; copy `build/qemu-system-i386` to
   `dist/xemu.app/Contents/MacOS/xemu`.
2. `dylibbundler` with `-s` paths `macos-libs/${arch}/opt/local/lib`,
   `/usr/local/lib`, `/opt/homebrew/lib`.
3. Rewrite residual `/opt/local/` references on the main binary to
   `@executable_path/../Libraries/${arch}/…`, and on every bundled
   dylib to `@rpath/…` (re-codesign each dylib after).
4. Strip all `LC_RPATH` entries, then add exactly
   `@executable_path/../Libraries/${arch}/` (macOS 26+ dyld rejects
   duplicates).
5. Bundle `libMoltenVK.dylib` with id `@rpath/libMoltenVK.dylib`;
   re-codesign. Check the `Bundling MoltenVK from …` provenance line.
6. `.icns` via `iconutil` from `ui/icons/xemu_*.png`; copy
   `Info.plist`; set versions via `plutil` (defaults to `0.0.0`
   without an `XEMU_VERSION` file — fine; the tag is canonical).
7. Ad-hoc codesign: `codesign --force --deep
   --preserve-metadata=entitlements,requirements,flags,runtime --sign -`.
8. `LICENSE.txt`:

   ```bash
   PKG_CONFIG_LIBDIR="$PWD/macos-libs/arm64/opt/local/lib/pkgconfig" \
     python3 scripts/gen-license.py \
       --version-file=macos-libs/arm64/INSTALLED > dist/LICENSE.txt
   ```

   `PKG_CONFIG_LIBDIR` is mandatory — without it the script fails on
   `pkg-config --modversion slirp` and truncates. Expect ~4000
   lines.
9. Verify and zip (symlinks preserved, exact naming):

   ```bash
   codesign --verify --deep --strict dist/xemu.app
   cd dist && zip -r -y -q "xemu-vX.Y.Z-macos-arm64.zip" xemu.app LICENSE.txt
   ```

   Expect the app ~30–35 MiB, the zip ~12–13 MiB; much smaller
   means a dylib or MoltenVK wasn't bundled. Attach to the draft
   with `gh release upload vX.Y.Z <zip>`.

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
