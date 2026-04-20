# Releasing xemu-macos

Step-by-step guide for cutting a GitHub release on
[`MichaelJSr/xemu-macos`](https://github.com/MichaelJSr/xemu-macos).
Follow this exactly so release naming, asset layout, and notes
formatting stay consistent across releases. Intended audience: AI
coding agents (and humans) preparing a release on this fork.

## Scope

- arm64 macOS builds only. Target is macOS 14+ (macOS 26 Tahoe
  recommended for MetalFX + frame interpolation).
- Releases are cut from the `macos-optimizations` branch on the
  fork. Do **not** target upstream or release from `master`.
- One release per fix / perf batch. If multiple unrelated commits
  have landed since the last tag, group them into a single release
  that lists all of them in the notes.

## Version numbering

- Tags follow `vMAJOR.MINOR.PATCH` where the first two components
  track upstream QEMU/xemu and `PATCH` increments per fork release
  (e.g. `v0.8.150` → `v0.8.151`). Use `gh release list --repo
  MichaelJSr/xemu-macos --limit 5` to find the most recent tag and
  bump `PATCH` by 1.
- Do **not** reuse tags. Do **not** delete and re-push a tag once it
  has an attached binary — that silently invalidates anyone who
  already downloaded it.

## Preflight

1. Confirm the working tree is clean and the branch is pushed:

   ```bash
   git status
   git log origin/macos-optimizations..HEAD --oneline   # must be empty
   ```

   If there are uncommitted changes, commit + push them first (see
   `README.md` "Committing" conventions — short title + `Made-with:
   Cursor` footer if authored by an agent).

2. Confirm the release will actually contain new work:

   ```bash
   LAST=$(gh release list --repo MichaelJSr/xemu-macos --limit 1 \
          --json tagName --jq '.[0].tagName')
   git log "$LAST"..HEAD --oneline
   ```

   If this prints nothing, there is nothing to release — stop.

3. Confirm the build is current. `qemu-system-i386` in `build/`
   must reflect every commit you plan to ship:

   ```bash
   (cd build && ninja)
   file build/qemu-system-i386      # expect: Mach-O 64-bit executable arm64
   ```

## Packaging `xemu.app`

The canonical packaging logic lives in `build.sh`'s
`package_macos` function. That function runs at the end of a full
`./build.sh` invocation, but rerunning `./build.sh` forces a
reconfigure + rebuild. For a release where you only need to
re-package an already-built binary, replicate the function's steps
in a throwaway script **in the build directory**, then delete the
script once the release is live.

The essential steps, in order (refer to `build.sh` for the exact
commands; do not hand-edit this list, always mirror `build.sh`):

1. `rm -rf build/dist` and `mkdir -p build/dist/xemu.app/Contents/MacOS`.
2. `cp build/qemu-system-i386 build/dist/xemu.app/Contents/MacOS/xemu`.
3. `dylibbundler` with `-s` paths including
   `macos-libs/${arch}/opt/local/lib`, `/usr/local/lib`,
   `/opt/homebrew/lib`.
4. Rewrite `/opt/local/` paths on the main binary to
   `@executable_path/../Libraries/${arch}/…`.
5. Rewrite `/opt/local/` paths on every bundled `.dylib` to
   `@rpath/…`, and re-codesign each with `codesign -s - -f`.
6. Strip duplicate `LC_RPATH` entries from the main binary (macOS
   26+ dyld rejects duplicates), then add
   `@executable_path/../Libraries/${arch}/`.
7. Bundle `libMoltenVK.dylib` (fat binary is fine) with id
   `@rpath/libMoltenVK.dylib`; re-codesign it.
8. Generate the `.icns` from `ui/icons/xemu_*.png` via
   `iconutil`.
9. Copy `Info.plist` and set `CFBundleShortVersionString` /
   `CFBundleVersion` via `plutil` (defaults to `0.0.0` if
   `XEMU_VERSION` is absent; that's fine — the release tag is the
   canonical version).
10. Ad-hoc codesign the main executable with
    `codesign --force --deep
    --preserve-metadata=entitlements,requirements,flags,runtime
    --sign -`.
11. Generate `build/dist/LICENSE.txt` via:

    ```bash
    PKG_CONFIG_LIBDIR="$PWD/macos-libs/arm64/opt/local/lib/pkgconfig" \
      python3 scripts/gen-license.py \
        --version-file=macos-libs/arm64/INSTALLED \
        > build/dist/LICENSE.txt
    ```

    `PKG_CONFIG_LIBDIR` is mandatory — without it `gen-license.py`
    fails on `pkg-config --modversion slirp` and the license file
    is truncated. Expect a ~4000-line LICENSE.txt; if it's much
    shorter something went wrong.

12. Sanity-check the bundle:

    ```bash
    codesign --verify --deep --strict build/dist/xemu.app
    du -sh build/dist/xemu.app          # expect ~30–35 MiB
    ```

## Creating the release zip

Zip with symlinks preserved (`-y`) and no path prefix stripping:

```bash
cd build/dist
zip -r -y -q "xemu-vX.Y.Z-macos-arm64.zip" xemu.app LICENSE.txt
```

- Zip name format is **exactly** `xemu-vMAJOR.MINOR.PATCH-macos-arm64.zip`.
  Do not change separators, do not add build metadata, do not use
  `.tar.gz`. Matching the existing naming is what keeps download
  tooling and docs stable across releases.
- Expect the zip around 12–13 MiB. A dramatically smaller zip
  usually means MoltenVK or a dylib wasn't bundled.

## Tag and push

```bash
git tag -a vX.Y.Z -m "vX.Y.Z — <short title>"
git push origin vX.Y.Z
```

The tag message short title should match the release title (see
below) so `git log --decorate` reads cleanly.

## Creating the GitHub release

Use `gh release create` with explicit `--repo`, `--target`,
`--title`, `--notes`, and the zip as a positional argument:

```bash
gh release create vX.Y.Z \
  --repo MichaelJSr/xemu-macos \
  --target macos-optimizations \
  --title "vX.Y.Z — <short description of headline change>" \
  --notes "$(cat <<'EOF'
<body — see format below>
EOF
)" \
  ./build/dist/xemu-vX.Y.Z-macos-arm64.zip
```

### Title format

- `vX.Y.Z — <headline>` with an em-dash (`—`, not hyphen).
- Headline is ≤ ~70 chars, imperative or descriptive of the
  primary change. Examples:
  - `v0.8.150 — Fix single-frame pink-tile flashes on 2/4-bpp textures`
  - `v0.8.151 — VRAM snapshot closes texture-upload tear race; zero-copy GPU unswizzle`
- If a single release bundles multiple unrelated changes, separate
  the two most prominent with `;`; don't try to list all of them.

### Notes body format

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
  their code names in backticks (`upload_texture_image`,
  `BUFFER_COMPUTE_DST`, `VK_ACCESS_SHADER_WRITE_BIT`, …). For
  perf-only changes, give concrete before/after bounds
  (e.g. "1 + 2L memcpys → 1 + L memcpys").>

- **<Short bold title of change #2>.** …
```

Style rules:

- One `###` subsection per subsystem touched. Typical subsystems
  on this fork: `Vulkan renderer (pgraph/vk)`, `APU`, `TCG / JIT`,
  `MetalFX + presentation`. Skip subsystems that have no changes.
- Each bullet starts with a bold short title followed by a period,
  then the prose. Don't use a colon after the bold title.
- Describe the **why** first (symptom / root cause / contended
  constraint) and the fix second. Historical context on prior
  related commits belongs inline, in 1–2 sentences, so a reader
  can understand this release without clicking through.
- Prefer concrete numbers over adjectives for perf work. Avoid
  "significantly" / "much faster" / "greatly improved".
- No emojis. No GitHub-Flavoured-Markdown callouts
  (`> [!NOTE]`). No screenshots — the release UI doesn't need
  them and they bloat the page.

### Flags

- Always `--target macos-optimizations`. Never leave it unset;
  `gh` will default to the default branch.
- Do **not** pass `--draft` unless the user explicitly asks for
  a draft. Do **not** pass `--prerelease` for a normal fork
  release.
- Attach exactly one zip. Do not attach unpackaged binaries,
  source archives, or PDBs — the zip is the complete payload.

## Post-publish verification

```bash
gh release view vX.Y.Z --repo MichaelJSr/xemu-macos \
  --json tagName,name,assets,isDraft,isPrerelease
```

Confirm:

- `tagName` matches the tag you pushed.
- `assets[].name` is `xemu-vX.Y.Z-macos-arm64.zip`.
- `assets[].size` is within ~10% of the previous release's size
  (sudden large swings almost always mean a bundling bug).
- `isDraft: false`, `isPrerelease: false`.

Optional smoke test: download the zip, unzip, and launch
`xemu.app` once to confirm it runs on the release host.

## Cleanup

- Delete any throwaway packaging scripts you created during the
  release. The working tree on `macos-optimizations` must be
  clean after the release is published.
- `build/dist/` can stay — it's gitignored — but if you re-ran
  packaging for multiple attempts, the zips accumulate there;
  remove the ones you didn't ship.

## Do-not-do list

- Don't run `./build.sh` just to re-package unless the build is
  also out of date. `./build.sh` reconfigures and re-invokes
  LTO, which is slow and churns `.lto-cache`.
- Don't use hardened-runtime signing (`XEMU_CODESIGN_ENTITLEMENTS=1`)
  for standard releases. Use `scripts/sign-macos-release.sh` only
  if the release needs notarization, and document that
  explicitly in the release notes.
- Don't force-push tags (`git push --force-with-lease` on a tag,
  `gh release delete` then re-create, etc.). Tags are immutable
  handles for users who already pulled the zip.
- Don't include unrelated branches or stashes. The release target
  is always `macos-optimizations` at the exact commit that the
  attached binary was built from.
