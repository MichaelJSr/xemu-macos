# xemu-macos on real Windows hardware: test plan

Written 2026-10-17 on the Mac; branch `windows` starts at master `42900e676d` (v0.15.1). It is meant for a fresh Claude Code session on the Windows machine, working with the owner; the kit's `AGENT-START.md` is the one-page brief to read first. Work top to bottom. Every step has exact PowerShell and a pass/fail line. Write results into `C:\xemu-kit\results\RESULTS.md` as you go (template in section 6).

Tags used below: **[owner]** means a person has to look, listen or hold the controller. **[agent]** means Claude can run it alone. **P0** = do first, **P1** = do this session, **P2** = only if there's time.

## 0. Context and ground rules

### What is already known about this fork on Windows

- The only earlier real-hardware session was 2026-09-07: Windows 10, i7-4770K, NVIDIA TITAN Xp. The Vulkan renderer rendered correctly. Upstream's DXGI presenter was fixed there (`2182baec46`). Audio picked `dsound`. The title screen ran at 21-25 fps, below its 30 fps cap, so that CPU had headroom to recover. Details are in `docs/Campaigns/windows-port-2026-09.md`.
- One problem from that session is still open: native MSYS2 builds of the "wave-1" branch crashed intermittently (about 5 runs in 6) 6-10 s after launch. Review blamed the `-O3 -Dstack_protector=disabled` flags, but that was never confirmed on hardware. Release zips are CI cross-builds at `-O2`, and today `-O3` is opt-in (`XEMU_WIN_O3=1`). The protocol is in section 7.3.
- v0.13.1 gave a black screen on an AMD GPU. It was fixed in v0.13.2 (two Vulkan spec violations that MoltenVK tolerated). The current renderer has never been checked on AMD or Intel hardware.
- Everything that shipped from v0.14 onward has never run on real Windows. That covers headless mode, the QMP automation commands, deterministic mode and its goldens, `XEMU_RAM_SHM` (a Windows named file mapping), the `XEMU_INPUT_PIPE` named pipe in a full run, the normal-mode BQL spin and KiIdle halt, and the fork's aarch64 TCG backend on Windows ARM64. `scripts/bench-savestate-ab-win.py` has never driven a full A/B, so expect rough edges and fix them as commits (section 6.3).
- `docs/windows-gating-audit.md` lists what CI compile alone has validated: the B-list, D1-D8 and I.9-I.11. `docs/roadmap.md` item 5 and xemu-history's frontier item 4 ("Real-hardware validation debt") track the same debt.

### Ground rules on Windows (CLAUDE.md, mapped)

- Never write to `C:\xemu-kit\personal` or `C:\xemu-kit\fixtures`. Both are marked read-only in step 1.4. Tests run on copies: the helpers copy the HDD and EEPROM for every run, and the owner plays on `C:\xemu-kit\play`.
- Always pass `-config_path`. Without it xemu reads and rewrites `%APPDATA%\xemu\xemu\xemu.toml`. Never put an `xemu.toml` next to an `xemu.exe` under `C:\xemu-kit\bin`, because that turns on portable mode.
- Mute every agent run (`[audio] volume_limit = 0`, or headless). The helpers force it.
- Mark every automated launch with `-name xemu-fork-auto` and set `XEMU_BASE_PATH` to the bench cache (`%LOCALAPPDATA%\xemu-fork-bench`). `Start-Xemu`, the bench and `det_golden.py` already do this.
- Commit to the `windows` branch only. Use `<subsystem>: <short description>` with one logical change per commit and docs in the same commit. Never touch `master` or any tag.
- Run at most two refuters at once. Never set `XEMU_X87_REFUTE`, `XEMU_SUBPAGE_FAST_REFUTE` and `XEMU_DSP_JIT_DIFF` together. Never time fps with `*_REFUTE`, `XEMU_X87_CENSUS`, `XEMU_GUEST_PROF` or `XEMU_MMIO_PROF` set.
- Don't wait for an idle machine. Quote the load, and add pairs instead.
- Personal set (`kit\personal`): use it for playing and for smoke-testing the owner's real setup. Fixture set (`kit\fixtures`): use it for benchmarks, goldens and savestate tests. Never mix them.
- Map macOS paths in the skills to Windows ones: `../fixtures` becomes `C:\xemu-kit\fixtures` (via `$env:XEMU_TEST_CONFIG`), `bench-det.py` becomes `scripts\win\det_golden.py`, `bench-savestate-ab.sh` becomes `bench-savestate-ab-win.py`, `ninja -C build qemu-system-i386` becomes `ninja -C build qemu-system-i386w.exe` (MSYS2), and APFS clones become plain copies.
- Claude Code runs these blocks through its PowerShell tool, one fresh process per call: dot-source `kit.ps1` in every call, keep each `Start-Xemu` with its `Stop-Xemu`, and run anything longer than 2 min in the background (1.10).

### Time budget

| Block | Priority | Time |
|---|---|---|
| 1 Setup | P0 | 60-90 min, plus about 15 min of hashing |
| 2 Smoke | P0 | 45 min |
| 3.1-3.6 Automation paths (logs, headless, QMP, pipe, shared RAM, gdbstub) | P0 | 45 min |
| 3.7 Deterministic goldens | P1 (do on day 1) | 45 min, mostly unattended |
| 3.8-3.14 Remaining Windows paths, soak, refuters, timers | P1 | 2.5-3 h, the soak unattended |
| 4 Performance | P1 | 1.5-2.5 h unattended (don't use the machine meanwhile) |
| 5 Upstream Roswell tests | P2 | 45 min |
| 6 Write-up and commits | P0 | 30-45 min |
| 7 Local builds, the exit-crash protocol, PGO | P2 | 2-4 h |

Suggested day: 1, 2, 3.1-3.6, then start 3.7 and do 3.10a in parallel, then 3.11 over lunch, then 4 in the afternoon while nobody touches the machine, then the rest of 3, then 6. Use 5 and 7 if time remains.

## 1. Machine setup (P0)

### 1.1 Record the machine [agent]

```powershell
$Kit = 'C:\xemu-kit'
New-Item -ItemType Directory -Force "$Kit\results" | Out-Null
$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
$info = [ordered]@{
  os      = (Get-CimInstance Win32_OperatingSystem | ForEach-Object { "$($_.Caption) $($_.Version) build $($_.BuildNumber)" })
  cpu     = "$($cpu.Name) ($($cpu.NumberOfCores)C/$($cpu.NumberOfLogicalProcessors)T) arch=$($cpu.Architecture)"
  ram_gb  = [math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB, 1)
  gpus    = (Get-CimInstance Win32_VideoController | ForEach-Object { "$($_.Name) driver $($_.DriverVersion)" }) -join '; '
  display = (Get-CimInstance Win32_VideoController | ForEach-Object { "$($_.CurrentHorizontalResolution)x$($_.CurrentVerticalResolution)@$($_.CurrentRefreshRate)Hz" }) -join '; '
  power   = (powercfg /getactivescheme) -join ' '
}
$info | ConvertTo-Json | Set-Content "$Kit\results\machine.json"
$info
dxdiag /t "$Kit\results\dxdiag.txt"
if (Get-Command vulkaninfo -ErrorAction SilentlyContinue) { vulkaninfo --summary > "$Kit\results\vulkaninfo.txt" }
```

`arch=9` is x86_64 and `arch=12` is ARM64 (Windows on Snapdragon). Note whether the CPU is a hybrid (Intel 12th gen or later, with P- and E-cores), because section 3.13 depends on it.

### 1.2 Install prerequisites [owner, about 15 min]

First the GPU driver: install the vendor's current driver (from the NVIDIA, AMD or Intel website, not only Windows Update) and reboot. Then check that `Test-Path "$env:WINDIR\System32\vulkan-1.dll"` is True and that `Get-CimInstance Win32_VideoController` doesn't show "Microsoft Basic Display Adapter". On a laptop, plug in, and once 1.5 has unzipped the builds, set Settings > System > Display > Graphics to High performance for each `xemu.exe` under `C:\xemu-kit\bin`.

If `winget` isn't recognized on a fresh PC, update "App Installer" from the Microsoft Store (or run `Add-AppxPackage -RegisterByFamilyName -MainPackage Microsoft.DesktopAppInstaller_8wekyb3d8bbwe`) and open a new window.

Open PowerShell (Windows Terminal is fine) and run:

```powershell
$wg = '--accept-source-agreements', '--accept-package-agreements', '-e'
winget install @wg --id Git.Git
winget install @wg --id Python.Python.3.12
winget install @wg --id GitHub.cli
winget install @wg --id 7zip.7zip
# only if you will build xemu yourself (section 7):
# winget install @wg --id MSYS2.MSYS2   # only if you will build (section 7)
# Claude Code, native installer: auto-updates, needs no Node, puts claude.exe in %USERPROFILE%\.local\bin
irm https://claude.ai/install.ps1 | iex      # alternative: winget install @wg --id Anthropic.ClaudeCode
# make the new tools visible in this window
$env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' + [Environment]::GetEnvironmentVariable('Path', 'User')
if (-not (Get-Command claude -ErrorAction SilentlyContinue)) {
  [Environment]::SetEnvironmentVariable('Path', [Environment]::GetEnvironmentVariable('Path', 'User') + ";$env:USERPROFILE\.local\bin", 'User')
  $env:Path += ";$env:USERPROFILE\.local\bin"
}
Set-ExecutionPolicy -Scope CurrentUser RemoteSigned -Force
git --version; py -3 --version; gh --version; claude --version
gh auth login
```

`gh auth login` is an [owner] step: pick GitHub.com, HTTPS, "Authenticate Git with your GitHub credentials" = Yes, and the browser login. It's needed to push `windows` and for `gh run`. The repo is public, so the clone and the kit's zips need no login.

Pass: every `--version` prints and `gh auth status` says you're logged in. Use `py -3` for every script. MSYS2's own python must not run the bench: it refuses, because it needs `os.name == 'nt'`.

### 1.3 Clone the fork and check out `windows` [owner]

```powershell
git config --global core.autocrlf false      # build.sh dies on CRLF checkouts
git config --global core.longpaths true
git config --global user.name 'MichaelJSr'
git config --global user.email 'miky.srouji@gmail.com'
Set-Location $env:USERPROFILE
git clone https://github.com/MichaelJSr/xemu-macos.git
Set-Location xemu-macos
git fetch origin
git switch windows
if ($LASTEXITCODE -ne 0) { git fetch C:\xemu-kit\repo\xemu-macos.bundle windows:windows; git switch windows }
git remote add upstream https://github.com/xemu-project/xemu.git
git log --oneline -3; git describe --tags
```

Fallback if GitHub has no `windows` branch, or there's no network. Clone from the kit's bundle instead:

```powershell
git bundle list-heads C:\xemu-kit\repo\xemu-macos.bundle
git clone -b windows C:\xemu-kit\repo\xemu-macos.bundle "$env:USERPROFILE\xemu-macos"
Set-Location "$env:USERPROFILE\xemu-macos"
git remote set-url origin https://github.com/MichaelJSr/xemu-macos.git
```

The repo should live at `C:\Users\<you>\xemu-macos`. Keep it free of spaces in the path if you can, because PowerShell 5.1's `Start-Process` doesn't quote arguments. The `roms/*` submodules aren't needed.

### 1.4 Lay out the kit and verify it [agent]

The kit is copied to `C:\xemu-kit` (see `WINDOWS-KIT-README.md` for the layout and the SHA256 check). It needs about 14 GB, plus another 25 GB free for play copies, builds, caches and scratch runs (check with `Get-PSDrive C`). If it's still on an external drive (here `E:`), copy it first. The folder on the drive is named `xemu-fork-windows-kit` when it comes from the Mac; use its real name. robocopy exit codes 0-7 mean success, 8 or more failure.

```powershell
robocopy E:\xemu-fork-windows-kit C:\xemu-kit /E /J /R:1 /W:1 /NFL /NDL
# macOS leaves ._* and .DS_Store files on exFAT drives, and a download marks files as from the internet
Get-ChildItem C:\xemu-kit -Recurse -Force -File | Where-Object { $_.Name -like '._*' -or $_.Name -eq '.DS_Store' } | Remove-Item -Force
Get-ChildItem C:\xemu-kit -Recurse -File | Unblock-File -ErrorAction SilentlyContinue
```

Then verify every file against `SHA256SUMS.txt` (about 15 min for roughly 13 GB) and mark the two pristine sets read-only:

```powershell
$Kit = 'C:\xemu-kit'; $bad = 0
Get-Content -LiteralPath "$Kit\SHA256SUMS.txt" | ForEach-Object {
  if ($_ -match '^([0-9a-fA-F]{64}) [ *](.+)$') {
    $want = $Matches[1].ToLower(); $rel = $Matches[2] -replace '/', '\'
    $p = Join-Path $Kit $rel
    if (-not (Test-Path -LiteralPath $p)) { "MISSING $rel"; $bad++ }
    elseif ((Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.ToLower() -ne $want) { "BAD     $rel"; $bad++ }
    else { "ok      $rel" }
  }
}
"problems: $bad"
Get-ChildItem -LiteralPath "$Kit\personal", "$Kit\fixtures" -Recurse -File | ForEach-Object { $_.IsReadOnly = $true }
```

Pass: `problems: 0`. `SHA256SUMS.txt` is authoritative. `fixtures\README.md`'s manifest dates from 2026-10-11 and the HDD has changed since (same 11 snapshot tags), so its `xbox_hdd.qcow2` line (1c6f030a...) is stale; don't treat that mismatch as corruption. The fixtures hash to `xbox_hdd.qcow2` 9e31cd901452ed87..., `Azurik_qol.iso` c167168e..., `eeprom.bin` a1784937..., `mcpx_1.0.bin` e99e3a77..., `Complex_4627v1.03.bin` 1de4c87e....

### 1.5 Unzip the release builds [agent]

Use the arm64 zips on Windows on ARM and the x86_64 zips everywhere else. The kit already holds every x86_64 zip (matching GitHub's digests) and the v0.15.1 arm64 ones: the fork's v0.15.1 and v0.14 zips in `releases\`, upstream's v0.8.137 in `releases\upstream-v0.8.137\`. The `gh release download` lines below only run when `gh auth status` passes, and with `--skip-existing` they only fetch what's missing. If `gh release list --repo MichaelJSr/xemu-macos --limit 3` shows a release newer than v0.15.1, set `$Ver` here and in `kit.ps1`.

```powershell
$Kit = 'C:\xemu-kit'
$Arch = if ((Get-CimInstance Win32_Processor | Select-Object -First 1).Architecture -eq 12) { 'arm64' } else { 'x86_64' }
$Ver = 'v0.15.1'; $V = $Ver.TrimStart('v')
$Rel = "$Kit\releases"; $Up = "$Rel\upstream-v0.8.137"
gh auth status *> $null
if ($LASTEXITCODE -eq 0) {
  gh release download $Ver --repo MichaelJSr/xemu-macos --dir $Rel --skip-existing `
    --pattern "xemu-$V-windows-$Arch.zip" --pattern "xemu-$V-windows-$Arch-pdb.zip" `
    --pattern "xemu-$V-dbg-windows-$Arch.zip" --pattern "xemu-$V-dbg-windows-$Arch-pdb.zip"
  gh release download v0.14 --repo MichaelJSr/xemu-macos --dir $Rel --skip-existing --pattern "xemu-0.14-windows-$Arch.zip"
  gh release download v0.8.137 --repo xemu-project/xemu --dir $Up --skip-existing --pattern "xemu-0.8.137-windows-$Arch.zip"
}
Get-ChildItem $Rel -Recurse -Filter *.zip | Unblock-File
Expand-Archive "$Rel\xemu-$V-windows-$Arch.zip"         "$Kit\bin\$Ver"     -Force
Expand-Archive "$Rel\xemu-$V-windows-$Arch-pdb.zip"     "$Kit\bin\$Ver"     -Force
Expand-Archive "$Rel\xemu-$V-dbg-windows-$Arch.zip"     "$Kit\bin\$Ver-dbg" -Force
Expand-Archive "$Rel\xemu-$V-dbg-windows-$Arch-pdb.zip" "$Kit\bin\$Ver-dbg" -Force
Expand-Archive "$Rel\xemu-0.14-windows-$Arch.zip"       "$Kit\bin\v0.14"    -Force
Expand-Archive "$Up\xemu-0.8.137-windows-$Arch.zip"     "$Kit\bin\upstream-0.8.137" -Force
Get-ChildItem "$Kit\bin\*\xemu.exe" | ForEach-Object { '{0}  {1}' -f (Get-FileHash $_ -Algorithm SHA256).Hash.Substring(0, 16), $_.FullName }
```

Record the hashes under "Builds" in RESULTS.md. The `-dbg` build has asserts: use it for deterministic runs and crash triage, never for fps. v0.14 lacks headless mode, `XEMU_BASE_PATH`, `XEMU_LOADVM_ADD_PADS` and the QMP digest commands, so use it only in the cross-version fps A/B (section 4.4).

### 1.6 Session helpers [agent]

The kit ships `C:\xemu-kit\tools\kit.ps1`, byte-identical to the repo's `scripts\win\kit.ps1`. Dot-source it directly. If it's missing: `Copy-Item "$env:USERPROFILE\xemu-macos\scripts\win\kit.ps1" C:\xemu-kit\tools\`. Start every PowerShell session (and every Claude Code PowerShell call, see 1.10) with:

```powershell
. C:\xemu-kit\tools\kit.ps1
```

The kit and the repo don't have to sit at `C:\xemu-kit` and `%USERPROFILE%\xemu-macos`: `kit.ps1` finds them from its own location (the kit's `tools\` or the repo's `scripts\win\`) or as siblings of each other (for example `Documents\xemu-kit` next to `Documents\xemu-macos`), and `$env:XEMU_KIT` / `$env:XEMU_REPO` override both. Every `C:\xemu-kit` in this plan then means `$Kit`.

It defines `$Kit $Repo $Work $Res $XNew $XNewDbg $XOld $XUp $Ctl $Det $Inj $Score $Bench`, sets `XEMU_TEST_CONFIG` and `XEMU_BENCH_CACHE`, and provides `Q`, `Get-TomlKey`, `Set-TomlKey`, `Remove-TomlKey`, `New-Scratch`, `Start-Xemu`, `Stop-Xemu`, `Save-Shot`, `Get-XemuLog`, `Show-Nsprof`, `Copy-Dist` and `Stop-AutoXemu`. `New-Scratch` deletes the HDD copy of every earlier run dir whose xemu has exited (logs, configs and screenshots stay), so scratch runs don't fill the disk. `Start-Xemu` refuses to launch when its QMP or monitor port is already taken (a leftover xemu: run `Stop-AutoXemu`), and the process it returns keeps its `ExitCode` after xemu exits. `Get-XemuLog <dir>` names the run's log: `stderr.log`, or `xemu.log` when xemu had no console to attach to. `stderr.log` fills as xemu runs (the inherited stderr is unbuffered); `xemu.log` was fully buffered by the C runtime before the fix that unbuffers it, so on such builds read it after the quit. The blocks below grep after `Stop-Xemu`, which works either way.

### 1.7 Write the two Windows configs [agent]

`xemu-fixtures.toml` is the template for every benchmark, golden and savestate test. It's derived from the fixture's own config, so it keeps the keyboard-pad map (WASD = 26/4/22/7, A = 81, B = 79, Start = 42) and `mem_limit`. `xemu-personal.toml` is the owner's setup, derived from a copy of their macOS config so it keeps their `mem_limit`, keyboard map and display preferences. The original is never edited.

```powershell
. C:\xemu-kit\tools\kit.ps1
New-Item -ItemType Directory -Force "$Work\shots", "$Kit\play" | Out-Null

# --- fixtures: benchmarks, goldens, savestate tests (pristine files, copied per run by the tools) ---
$F = "$Kit\fixtures"; $cfg = "$Work\xemu-fixtures.toml"
Copy-Item -LiteralPath "$F\xemu.toml" $cfg -Force; (Get-Item $cfg).IsReadOnly = $false
Set-TomlKey $cfg 'sys.files' 'bootrom_path'  (Q "$F\mcpx_1.0.bin")
Set-TomlKey $cfg 'sys.files' 'flashrom_path' (Q "$F\Complex_4627v1.03.bin")
Set-TomlKey $cfg 'sys.files' 'eeprom_path'   (Q "$F\eeprom.bin")
Set-TomlKey $cfg 'sys.files' 'hdd_path'      (Q "$F\xbox_hdd.qcow2")
Set-TomlKey $cfg 'sys.files' 'dvd_path'      (Q "$F\Azurik_qol.iso")
Set-TomlKey $cfg 'general' 'games_dir'       (Q $F)
Set-TomlKey $cfg 'general' 'screenshot_dir'  (Q "$Work\shots")
Set-TomlKey $cfg 'general' 'show_welcome'    'false'
Set-TomlKey $cfg 'general.updates' 'check'   'false'
Set-TomlKey $cfg 'audio' 'volume_limit'      '0'
Set-TomlKey $cfg 'display' 'renderer'        "'VULKAN'"
Remove-TomlKey $cfg 'display.vulkan' 'preferred_physical_device'   # the Mac's 'Apple M2 Ultra'; 2.3 checks the pick
Set-TomlKey $cfg 'display.window' 'fullscreen_on_startup' 'false'
Set-TomlKey $cfg 'input' 'auto_bind'         'false'
if ((Get-TomlKey $cfg 'input.bindings' 'port1') -ne 'keyboard') { Remove-TomlKey $cfg 'input.bindings' 'port1' }  # the Mac controller GUID; runs stub player 1
Select-String -LiteralPath $cfg -Pattern '_path|games_dir|volume_limit|renderer|port[1-4]|mem_limit|hard_fpu|dsp_jit|auto_bind'

# --- personal: the owner's setup (play copies of the HDD and EEPROM; ROMs and discs read in place) ---
$P = "$Kit\personal"
foreach ($f in 'xbox_hdd.qcow2', 'eeprom.bin') {   # never overwrite the owner's Windows play copies on a re-run
  if (-not (Test-Path -LiteralPath "$Kit\play\$f")) { Copy-Item -LiteralPath "$P\$f" "$Kit\play" }
}
Get-ChildItem -LiteralPath "$Kit\play" -File | ForEach-Object { $_.IsReadOnly = $false }
Select-String -LiteralPath "$P\xemu.toml" -Pattern 'bootrom_path|flashrom_path|dvd_path|mem_limit'   # the macOS file names, for reference
$bios = Get-ChildItem -LiteralPath "$P\BIOS" -File | Where-Object Name -eq 'Complex_4627v1.03.bin'
if (-not $bios) { $bios = Get-ChildItem -LiteralPath "$P\BIOS" -File | Select-Object -First 1 }
$rom = Get-ChildItem -LiteralPath "$P\Boot ROM image" -File | Where-Object Name -eq 'mcpx_1.0.bin'
if (-not $rom) { $rom = Get-ChildItem -LiteralPath "$P\Boot ROM image" -File | Select-Object -First 1 }
$macDvd = Get-TomlKey "$P\xemu.toml" 'sys.files' 'dvd_path'
$dvd = if ($macDvd) { Join-Path "$P\games" ([IO.Path]::GetFileName($macDvd)) } else { '' }
if (-not $dvd -or -not (Test-Path -LiteralPath $dvd)) { $dvd = "$P\games\Azurik_qol.iso" }
$cfg = "$Work\xemu-personal.toml"
Copy-Item -LiteralPath "$P\xemu.toml" $cfg -Force; (Get-Item $cfg).IsReadOnly = $false
Set-TomlKey $cfg 'sys.files' 'bootrom_path'  (Q $rom.FullName)
Set-TomlKey $cfg 'sys.files' 'flashrom_path' (Q $bios.FullName)
Set-TomlKey $cfg 'sys.files' 'eeprom_path'   (Q "$Kit\play\eeprom.bin")
Set-TomlKey $cfg 'sys.files' 'hdd_path'      (Q "$Kit\play\xbox_hdd.qcow2")
Set-TomlKey $cfg 'sys.files' 'dvd_path'      (Q $dvd)
Set-TomlKey $cfg 'general' 'games_dir'       (Q "$P\games")
Set-TomlKey $cfg 'general' 'screenshot_dir'  (Q "$Work\shots")
Set-TomlKey $cfg 'general.updates' 'check'   'false'
Set-TomlKey $cfg 'audio' 'volume_limit'      '0'      # muted for agent runs; raise it in Settings > Audio when you play
Set-TomlKey $cfg 'display.window' 'fullscreen_on_startup' 'false'
Remove-TomlKey $cfg 'display.vulkan' 'preferred_physical_device'   # xemu picks and saves this PC's GPU
Set-TomlKey $cfg 'display.ui' 'auto_scale' 'true'                    # the Mac's 2x Retina UI scale doesn't fit here
Remove-TomlKey $cfg 'display.ui' 'scale'
Remove-TomlKey $cfg 'display.window' 'startup_size'
if ((Get-TomlKey $cfg 'input.bindings' 'port1') -ne 'keyboard') { Remove-TomlKey $cfg 'input.bindings' 'port1' }  # your pad gets a Windows GUID; bind it in Settings > Input
Select-String -LiteralPath $cfg -Pattern '_path|games_dir|volume_limit|renderer|port[1-4]|mem_limit'
```

Pass: every `_path` names a file that exists (`Test-Path`), both configs have `volume_limit = 0`, and the personal config's `mem_limit` matches the macOS one (its savestates need the same RAM size). The macOS-only keys (`metalfx_mode`, `frame_interpolation`, `presentation_backend`) don't matter on Windows. The `[automation]` table only applies to the pref-dir config, so with `-config_path` it's ignored.

### 1.8 Kit helper scripts in the repo [agent]

The helpers are already in the `windows` branch: `scripts\win\xemu_ctl.py` (one-shot QMP, HMP and gdbstub calls, plus a shared-memory hash) and `scripts\win\det_golden.py` (the Windows stand-in for `scripts/bench-det.py`, which is macOS-only because it uses libproc, AF_UNIX and `.app` bundles). They were only tested against fake servers on the Mac, so fix them on their first real run and commit the fixes to `windows` (section 6.3).

```powershell
. C:\xemu-kit\tools\kit.ps1
py -3 $Ctl --help; py -3 $Det --help
py -3 $Bench --dry-run "$Res\bench-dryrun"
py -3 "$Repo\scripts\win\quiet_check.py" --info
py -3 -I "$Repo\scripts\list_snapshots.py" "$Kit\fixtures\xbox_hdd.qcow2"
```

Pass: help text prints, the bench dry run ends without a traceback, and the snapshot list shows `azurik-heavy-f1`, `azurik-heavy-w2`, `azurik-stream-e2`, `azurik-transition-f1`, `azurik-movie-e2` and `vm-20260704032357` (F5).

### 1.9 Crash dumps and event logs [owner, elevated PowerShell]

```powershell
# Run as administrator
$dump = 'C:\xemu-kit\results\dumps'; New-Item -ItemType Directory -Force $dump | Out-Null
$reg = 'HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\xemu.exe'
New-Item -Path $reg -Force | Out-Null
New-ItemProperty -Path $reg -Name DumpFolder -Value $dump -PropertyType ExpandString -Force | Out-Null
New-ItemProperty -Path $reg -Name DumpCount -Value 20 -PropertyType DWord -Force | Out-Null
New-ItemProperty -Path $reg -Name DumpType -Value 2 -PropertyType DWord -Force | Out-Null
winget install --accept-source-agreements --accept-package-agreements -e --id Microsoft.WinDbg
# optional, record it in RESULTS.md if you do it: no Defender scans of multi-GB copies and JIT code
# Add-MpPreference -ExclusionPath 'C:\xemu-kit'
```

To read a dump: `windbg -z <dump> -y C:\xemu-kit\bin\v0.15.1` (the PDB sits next to `xemu.exe`), then `!analyze -v` and `~*k`. To see crashes and GPU resets afterwards:

```powershell
Get-WinEvent -FilterHashtable @{ LogName = 'Application'; ProviderName = 'Application Error' } -MaxEvents 20 -ErrorAction SilentlyContinue | Where-Object Message -like '*xemu*' | Format-List TimeCreated, Message
Get-WinEvent -FilterHashtable @{ LogName = 'System'; Id = 4101 } -MaxEvents 5 -ErrorAction SilentlyContinue | Format-List TimeCreated, Message   # display driver reset (TDR)
```

### 1.10 Claude Code and the memory files [owner]

CLAUDE.md and the three skills (`xemu-operate`, `xemu-internals`, `xemu-history`) come with the repo. The owner's memory files come from `kit\claude-memory`. Claude Code keeps per-project memory in `%USERPROFILE%\.claude\projects\<project-dir>\memory\`. `<project-dir>` is the repo's full path with every character that isn't a letter or digit replaced by `-`. For example, `C:\Users\mike\xemu-macos` becomes `C--Users-mike-xemu-macos` (the `:` and the `\` after the drive letter each become `-`). On the Mac it was `-Users-michaelsrouji-Documents-xemu-dev-xemu-macos`.

```powershell
Set-Location "$env:USERPROFILE\xemu-macos"
$proj = (Get-Location).Path -replace '[^A-Za-z0-9]', '-'
$mem = Join-Path $env:USERPROFILE ".claude\projects\$proj\memory"
New-Item -ItemType Directory -Force $mem | Out-Null
Copy-Item 'C:\xemu-kit\claude-memory\*.md' $mem
Get-ChildItem $mem
claude            # log in, check that it starts in the repo, then /exit
Get-ChildItem "$env:USERPROFILE\.claude\projects" | Select-Object Name
```

Check after the first `claude` run. If the folder Claude created differs from `$proj` (for example a lower-case drive letter), move the files: `Move-Item "$mem\*" "$env:USERPROFILE\.claude\projects\<created-name>\memory\"`. Then start Claude Code in the repo with the kit as an extra working directory, `claude --add-dir C:\xemu-kit` (CLAUDE.md, the skills and the memory load only for a session started in the repo), and ask "what do you remember about this project?". It should mention the golden hashes, the lean dev process and the `windows-session` memory (commit to `windows` only). If Claude Code can't find Git Bash, set `CLAUDE_CODE_GIT_BASH_PATH` to `C:\Program Files\Git\bin\bash.exe` (in the `env` block of `%USERPROFILE%\.claude\settings.json`, or in the environment before starting `claude`).

How Claude runs this plan's PowerShell: Claude Code on Windows has a PowerShell tool, on by default with Git for Windows (claude.ai and Console accounts). It uses `pwsh` 7 if installed, else Windows PowerShell 5.1, with `-ExecutionPolicy Bypass`; the Bash tool (Git Bash) stays for POSIX commands. Each call is a fresh process, so functions, `$p` and `$env:` changes don't carry over: start every call with `. C:\xemu-kit\tools\kit.ps1`, and keep each `Start-Xemu` and its `Stop-Xemu` in the same call. Foreground calls time out after 2 min by default (10 min at most). Run longer blocks (2.2, 2.3, 3.7, 3.8, 3.10a, 3.10d, 3.11, 3.12, 3.13, section 4) in the background: write the block to `C:\xemu-kit\work\step-<id>.ps1` and run `powershell -NoProfile -ExecutionPolicy Bypass -File C:\xemu-kit\work\step-<id>.ps1 *> C:\xemu-kit\results\step-<id>.log` with `run_in_background`, then read the log when it finishes. After any interrupted call, run `Stop-AutoXemu` before the next launch.

v0.15.1 and older write to the redirected `stderr.log` only when they can attach to the parent's console; without one they send both streams to `xemu.log` in the working directory. Later builds keep a stream the parent redirected to a file and send only the others to `xemu.log`, so this paragraph applies to the older builds. The kit's helpers cope (`Get-XemuLog`), but the bench's `ab_*.log` captures don't. Step 3.1 shows which case you're in. If its `xemu.log` exists, give the bench blocks a console of their own: `Start-Process powershell -WindowStyle Minimized -Wait -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-Command', "& 'C:\xemu-kit\work\step-<id>.ps1' *> 'C:\xemu-kit\results\step-<id>.log'"` (in the background as above).

### 1.11 Vulkan SDK for validation layers [owner, optional, needed by 3.10a]

```powershell
winget install --accept-source-agreements --accept-package-agreements -e --id KhronosGroup.VulkanSDK   # needs UAC
```

Create `C:\xemu-kit\results\RESULTS.md` from the section 6.1 template now.

## 2. Smoke (P0)

### 2.1 The owner's own setup [owner, 20 min]

```powershell
. C:\xemu-kit\tools\kit.ps1
& $XNew -config_path "$Work\xemu-personal.toml"
```

The log prints in this PowerShell window. Go through the checklist below and write PASS/FAIL plus notes for each item into RESULTS.md.

| Check | Pass when |
|---|---|
| Title bar | reads `xemu \| v0.15...` |
| Dashboard | with the tray empty (Machine > Eject) the BIOS boots the HDD dashboard; `NevolutionX.iso` (Machine > Load Disc) boots to its menu |
| Azurik | `Azurik_qol.iso` reaches intro, title and in-game with no black screen, pink tiles or missing HUD; the vanilla `Azurik - Rise of Perathia (USA).xiso.iso` and `Vexx (USA).xiso.iso` reach their titles |
| Renderer | Settings > Display shows Vulkan; the log has `Selected physical device: <your GPU>` (on a laptop it must be the discrete GPU; if not, set `[display.vulkan] preferred_physical_device` or Windows Settings > Graphics > xemu.exe = High performance) |
| Presenter | the log has `win32_dxgi_present: DXGI Presentation Helper successfully initialized`, not `win32_dxgi present failed or unavailable` |
| Audio | raise Settings > Audio volume, then intro and title music play without crackle or dropouts; the log names the audio backend |
| Controller | Settings > Input lists the pad; bind it to Port 1, it drives Azurik, rumble works, and unplugging and replugging mid-game recovers |
| Fullscreen | View > Fullscreen toggles; try `fullscreen_exclusive` on and off; alt-tab out and back works with no black screen |
| UI scale | on a HiDPI monitor (125-200%) the menus are crisp and sized right with auto-scale; manual Settings > Display > UI scale works; resizing the window works in each fit mode |
| Snapshots | the Snapshots menu shows thumbnails; one of your own snapshots loads (a disc dialog is expected for snapshots saved before disc fingerprints, see 3.14) |

### 2.2 Five cold launches: the exit-crash check [agent, 5 min]

```powershell
. C:\xemu-kit\tools\kit.ps1
1..5 | ForEach-Object {
  $d = New-Scratch -Name "cold$_"
  $p = Start-Xemu -Exe $XNew -Dir $d -Stub
  Start-Sleep 40
  $alive = -not $p.HasExited
  if ($alive) { Save-Shot "$d\boot.png"; Stop-Xemu $p }
  'run {0}: alive at 40 s = {1}, exit code = {2}' -f $_, $alive, $p.ExitCode
}
```

Pass: 5 of 5 alive at 40 s (the 2026-09 crash came at 6-10 s), and each screenshot is PASS or shows the intro or title. On FAIL, record the exit code (`-1073741819` = 0xC0000005 access violation, `-1073740791` = 0xC0000409 fail-fast), the dump in `results\dumps` and the last 30 lines of the run's log (`Get-Content (Get-XemuLog $d) -Tail 30`; with no console, a v0.15.1 `xemu.log` can stop at its last flushed 4 KiB block, so also read the dump). Repeat with `$XNewDbg`, then go to section 7.3.

### 2.3 The fixture savestates load [agent, 10 min]

```powershell
. C:\xemu-kit\tools\kit.ps1
foreach ($t in 'azurik-heavy-f1', 'azurik-heavy-w2', 'azurik-stream-e2', 'vm-20260704032357') {
  $d = New-Scratch -Name "load-$t"
  $p = Start-Xemu -Exe $XNew -Dir $d -Stub -Vars @{ XEMU_NV2A_NSPROF = '1' }
  py -3 $Ctl wait 4444 180
  py -3 $Ctl loadvm 4444 $t
  Start-Sleep 25
  Save-Shot "$d\$t.png"
  py -3 $Ctl qmp 4444 query-xemu-display
  Stop-Xemu $p
  Show-Nsprof $d
}
```

Pass, for each tag: `loaded <tag>`, a PASS screenshot, and nsprof draws per flip in the tag's band. Expect heavy-f1 576-690, W2 306-323, F5 about 471, and stream-e2 about 207 standing. There should be no `vkWaitForFences` warning and the process should stay alive. A `loadvm` error `Unknown section ... 1.3/usb-hub` means the player-1 stub is missing. The `-Stub` switch adds it through `scripts\harness-config.py`; outside it, set `XEMU_LOADVM_ADD_PADS=1` (v0.15 and later).

Also check the GPU: `Select-String -LiteralPath (Get-XemuLog $d) -Pattern 'Selected physical device'` must name the discrete GPU. With no `preferred_physical_device`, xemu takes the first compatible device, which can be the integrated one. If it does, pin it: `Set-TomlKey "$Work\xemu-fixtures.toml" 'display.vulkan' 'preferred_physical_device' (Q '<exact name from the "Available physical devices" list in the same log>')`, and repeat 2.3.

### 2.4 Vulkan versus OpenGL on this GPU [agent, 5 min]

```powershell
. C:\xemu-kit\tools\kit.ps1
foreach ($r in 'VULKAN', 'OPENGL') {
  $d = New-Scratch -Name "renderer-$r"
  Set-TomlKey "$d\xemu.toml" 'display' 'renderer' "'$r'"
  $p = Start-Xemu -Exe $XNew -Dir $d -Stub -Vars @{ XEMU_NV2A_NSPROF = '1' }
  py -3 $Ctl wait 4444 180; py -3 $Ctl loadvm 4444 azurik-heavy-f1
  Start-Sleep 30; Save-Shot "$d\$r.png"; Stop-Xemu $p; Show-Nsprof $d
}
```

Pass: both render the scene. Record flips/s for each. Section M of the gating audit says OpenGL crashed right after the PFIFO thread started in a Parallels VM, and real hardware has never confirmed or cleared that. A crash here is a P0 finding.

## 3. The fork's Windows-specific paths

These are fork features that take a Windows-specific branch (found by `git diff upstream/master...HEAD` grepped for `_WIN32`, `__APPLE__` and host-architecture gates), plus the gating audit's needs-real-hardware list.

| Code | Windows behaviour | Test |
|---|---|---|
| `ui/xemu.c` console attach | stderr goes to the parent console or to the file the parent redirected it to, else to `xemu.log` in the working directory | 3.1 |
| `hw/xbox/nv2a/pfifo.c`, `nsprof.c` | each heartbeat or nsprof report goes out in one write (stderr is unbuffered, and mingw's printf writes a character at a time) | 3.1 |
| `ui/xemu.c` headless (`SDL_WINDOW_HIDDEN`) | hidden window, no host audio | 3.2 |
| `ui/xemu-ram-digest.c` | non-Apple uses the thread pool plus qcrypto (Apple uses GCD and CommonCrypto) | 3.3 |
| `ui/xemu-input.c` `_WIN32` arm | `XEMU_INPUT_PIPE` is a Win32 named pipe (`\\.\pipe\<name>`), PIPE_NOWAIT, one instance, recycled per writer | 3.4 |
| `hw/xbox/xbox.c` | `XEMU_RAM_SHM` is a named file mapping; no lock file; a name that exists always has a live holder | 3.5 |
| gdbstub (`gdbstub/xemu-gdb.c`) | portable; never run on Windows | 3.6 |
| icount, block I/O | deterministic I/O: macOS and Linux run qcow2 I/O inline on the vCPU; Windows' file-win32 still hands it to worker threads (the IDE settle covers it) | 3.7 |
| `target/i386/tcg/fpu_helper.c` | x86_64 hard FPU uses host `long double` (x87); aarch64 uses `double`; the SSE host path is on by default with `hard_fpu` (since 2026-10-18; `XEMU_SSE_HOST=2` is the differential) | 3.7, 3.8 |
| `tcg/aarch64/tcg-target-has.h` | Windows ARM64: `TCG_TARGET_HAS_fpu 0` (helper hard FPU, no inline x87 or SSE), but pinned registers, the TLB memo and hot/cold TB code are active | 3.9 |
| `include/qemu/cacheflush.h` | batched icache flush off on Windows ARM64 | 3.9 |
| `dsp56k_jit_arm64.h` | the fork's DSP JIT exists only on aarch64 POSIX; Windows runs upstream's dsp56300 JIT (`use_dsp_jit = true`) or the interpreter | 3.7, 3.9, 4.5 |
| `hw/xbox/mcpx/apu` `fpconv.h`, `vp.c` | SSE2 mix kernels on x86_64, NEON on aarch64 | 3.7 (WAV), 2.1 |
| `vk/instance.c` | strict non-Apple device features (geometryShader, occlusionQueryPrecise, fillModeNonSolid, depthClamp, shaderClipDistance); `VK_KHR_external_semaphore_win32` optional; Win32 external memory VK-to-GL present | 2.1, 2.4, 3.9, 3.10 |
| gating audit B1-B8, F1 | occlusion rework, deferred reports, vertex refinement, bind skips, PFIFO untimed wait, surface expiry, 5 s/60 s fence watchdog, zeta quarantine, texture LRU retirement | 3.10, 3.11 |
| `system/cpus.c` | BQL spin paced on `get_clock()` (QueryPerformanceCounter) on x86_64, `cntvct_el0` on Windows ARM64 | 4.5 |
| `target/i386/tcg/system/misc_helper.c` | KiIdleLoop halt paced on `get_clock()` | 4.5 |
| no `QOS_CLASS` equivalent (audit A6) | vCPU, PFIFO and vblank run at normal Windows priority | 3.13 |
| `util/qemu-timer.c` | macOS kqueue timer is Darwin-only; Windows uses upstream timers; vblank uses `SDL_DelayPrecise` | 3.13 |
| `accel/tcg/tb-maint.c` | `XEMU_SUBPAGE_FAST=1` prints "inert" on x86_64 (the store prefilter is aarch64-only); sub-page code tracking itself is portable | 3.12 |
| `ui/xemu-disc-fingerprint.c` | no inode change time on Windows, so snapshots without a fingerprint always ask | 3.14 |
| `util/qemu-sockets.c`, `io/channel-socket.c`, `migration/savevm.c` | the unix-listener guard is POSIX-only; Windows keeps upstream behaviour | 3.15 |
| `ui/xui/main-menu.cc` | DSP JIT setting hidden on Windows; hard-FPU setting shown on x86_64 and aarch64 | 2.1 |

### 3.1 Logs and console [agent, P0, 5 min]

```powershell
. C:\xemu-kit\tools\kit.ps1
$d = New-Scratch -Name logs
$p = Start-Xemu -Exe $XNew -Dir $d -Stub -Vars @{ XEMU_PFIFO_HEARTBEAT = '1'; XEMU_NV2A_NSPROF = '1' }
py -3 $Ctl wait 4444 180; py -3 $Ctl loadvm 4444 azurik-heavy-f1; Start-Sleep 20
Stop-Process -Id $p.Id -Force          # a hard kill, the case the Windows flush exists for
Get-Content (Get-XemuLog $d) -TotalCount 5
Select-String -LiteralPath (Get-XemuLog $d) -Pattern 'heartbeat|nsprof: .*interval' | Select-Object -Last 3
Test-Path "$d\xemu.log"
```

Pass: `xemu_version:` and `xemu_commit:` sit at the top, the last heartbeat or nsprof line is no older than about 5 s before the kill, and there's no `xemu.log` (the redirect worked). If `stderr.log` is empty and `xemu.log` exists, the launching shell had no console and the build is v0.15.1 or older. The kit's helpers then read `xemu.log` (`Get-XemuLog`), but the bench's `ab_*.log` captures stay empty, so run the benches from a console (1.10) and note this in RESULTS.md.

### 3.2 Headless mode [agent, P0, 5 min]

```powershell
. C:\xemu-kit\tools\kit.ps1
$d = New-Scratch -Name headless-offscreen
$p = Start-Xemu -Exe $XNew -Dir $d -Stub -Vars @{ XEMU_HEADLESS = '1' }
py -3 $Ctl wait 4444 180
(Get-Process -Id $p.Id).MainWindowHandle        # 0 = no visible window
py -3 $Ctl loadvm 4444 azurik-heavy-f1; Start-Sleep 10
Save-Shot "$d\offscreen.png"; Stop-Xemu $p
Select-String -LiteralPath (Get-XemuLog $d) -Pattern 'headless mode'

$d = New-Scratch -Name headless-null
$p = Start-Xemu -Exe $XNew -Dir $d -Stub -Vars @{ XEMU_HEADLESS = 'null' }
py -3 $Ctl wait 4444 180; py -3 $Ctl loadvm 4444 azurik-heavy-f1; Start-Sleep 5
py -3 $Ctl qmp 4444 screendump "filename=$d\null.png" format=png
Stop-Xemu $p
Select-String -LiteralPath (Get-XemuLog $d) -Pattern 'headless mode'
$r = Get-TomlKey "$d\xemu.toml" 'display' 'renderer'   # null mode never saves over it; xemu writes only non-default keys
'renderer after the null run: ' + $(if ($r) { $r } else { 'VULKAN (the default, so not written)' })   # NULL is a failure
```

Pass: `xemu: headless mode: offscreen` (or `null`), window handle 0, no taskbar button, no focus steal (the owner types in another window meanwhile), no xemu entry in the Volume Mixer, the offscreen screenshot is PASS showing the scene, and the null screendump returns a PNG (usually blank).

### 3.3 QMP automation commands [agent, P0, 10 min]

```powershell
. C:\xemu-kit\tools\kit.ps1
$d = New-Scratch -Name qmp
$p = Start-Xemu -Exe $XNew -Dir $d -Stub -Vars @{ XEMU_HEADLESS = 'null' } -Extra '-S', '-icount', 'shift=2,align=off,sleep=off'
py -3 $Ctl wait 4444 180
py -3 $Ctl loadvm 4444 azurik-heavy-f1
py -3 $Ctl qmp 4444 xemu-checkpoint-save name=base
py -3 $Ctl qmp 4444 xemu-frame-advance frames=300
$a = py -3 $Ctl qmp 4444 xemu-ram-digest | ConvertFrom-Json
py -3 $Ctl qmp 4444 xemu-ram-digest algorithm=xxh3-128 pages=true | ConvertFrom-Json | Select-Object frame, 'ram-size', 'elapsed-us'
py -3 $Ctl qmp 4444 xemu-dump-ram "filename=$d\ram.bin"
$fileHash = (Get-FileHash "$d\ram.bin" -Algorithm SHA256).Hash.ToLower()
py -3 $Ctl qmp 4444 xemu-checkpoint-restore name=base incremental=true verify=true
py -3 $Ctl qmp 4444 xemu-frame-advance frames=300
$b = py -3 $Ctl qmp 4444 xemu-ram-digest | ConvertFrom-Json
py -3 $Ctl qmp 4444 query-xemu-display
py -3 $Ctl qmp 4444 query-xemu-input
Stop-Xemu $p
'digest f300 {0}; replay same = {1}; dump file matches = {2}; digest took {3} us' -f $a.sha256.Substring(0, 16), ($a.sha256 -eq $b.sha256), ($fileHash -eq $a.sha256), $a.'elapsed-us'
```

Pass: replay same = True, the dump file matches, the restore with `verify=true` reports 0 missed pages, `elapsed-us` is roughly 50-150 ms for 128 MiB (macOS: about 55 ms), and every command returns. This is also the first in-process determinism check on Windows.

### 3.4 `XEMU_INPUT_PIPE` named pipe [agent, P0, 10 min]

```powershell
. C:\xemu-kit\tools\kit.ps1
$d = New-Scratch -Name pipe
$p = Start-Xemu -Exe $XNew -Dir $d -Stub -Vars @{ XEMU_INPUT_PIPE = 'xemu-kit-pipe'; XEMU_NV2A_NSPROF = '1' }
py -3 $Ctl wait 4444 180; py -3 $Ctl loadvm 4444 azurik-stream-e2
Save-Shot "$d\before.png"
py -3 $Inj down xemu-kit-pipe W; Start-Sleep 1
py -3 $Ctl qmp 4444 query-xemu-input              # keys contains 26, port 2 ly > 0
Start-Sleep 8; py -3 $Inj clear xemu-kit-pipe
Save-Shot "$d\walked.png"
py -3 $Inj send xemu-kit-pipe '+30 axis ly 1.0' '+150 axis ly 0'
py -3 $Ctl qmp 4444 query-xemu-input              # queued: both lines, with frames
$fail = 0; 1..100 | ForEach-Object { py -3 $Inj tap xemu-kit-pipe D 20 2>$null; if ($LASTEXITCODE) { $fail++ } }
"one-shot writer failures: $fail of 100"
Stop-Xemu $p
Select-String -LiteralPath (Get-XemuLog $d) -Pattern 'test-input pipe'
```

Run it while another window has focus. Pass: `xemu: test-input pipe listening on \\.\pipe\xemu-kit-pipe`, the input state shows the key and stick, the character walks (the two screenshots differ and draws per flip change), both frame-tagged lines appear in `queued`, and there are 0 failures out of 100 writers. A failure count above 0 points at the disconnect/reconnect window that the comment in `ui/xemu-input.c` describes.

### 3.5 `XEMU_RAM_SHM` named file mapping [agent, P0, 10 min]

```powershell
. C:\xemu-kit\tools\kit.ps1
$d = New-Scratch -Name shm
$p = Start-Xemu -Exe $XNew -Dir $d -Stub -Vars @{ XEMU_RAM_SHM = 'xemu-kit-shm'; XEMU_HEADLESS = 'null' } -Extra '-S'
py -3 $Ctl wait 4444 180
py -3 $Ctl loadvm 4444 azurik-heavy-f1
py -3 $Ctl qmp 4444 xemu-frame-advance frames=60
$dig = py -3 $Ctl qmp 4444 xemu-ram-digest | ConvertFrom-Json
$shm = py -3 $Ctl shmhash xemu-kit-shm $dig.'ram-size'
"shared memory matches guest RAM: $($shm -eq $dig.sha256)"
# a second xemu asking for the same name must keep private RAM and keep running
$d2 = New-Scratch -Name shm2
$p2 = Start-Xemu -Exe $XNew -Dir $d2 -Stub -Qmp 4446 -Mon 4447 -Vars @{ XEMU_RAM_SHM = 'xemu-kit-shm'; XEMU_HEADLESS = 'null' }
py -3 $Ctl wait 4446 180
"second alive: $(-not $p2.HasExited)"
Stop-Xemu $p2 -Qmp 4446; Stop-Xemu $p
Select-String -LiteralPath (Get-XemuLog $d) -Pattern 'shared memory'
Select-String -LiteralPath (Get-XemuLog $d2) -Pattern 'XEMU_RAM_SHM|shared memory'
# the name is free again once both have exited
$d3 = New-Scratch -Name shm3
$p3 = Start-Xemu -Exe $XNew -Dir $d3 -Stub -Vars @{ XEMU_RAM_SHM = 'xemu-kit-shm'; XEMU_HEADLESS = 'null' }
py -3 $Ctl wait 4444 180; Stop-Xemu $p3; Select-String -LiteralPath (Get-XemuLog $d3) -Pattern 'shared memory'
```

Pass: `xemu: guest RAM in shared memory xemu-kit-shm`, the hashes match, the second xemu prints that the name is held and that guest RAM stays private, without exiting, and the third launch gets the name. The README says a reader that keeps the mapping open across a relaunch makes the new xemu keep private RAM. That's expected, not a bug.

### 3.6 gdbstub [agent, P0, 5 min]

```powershell
. C:\xemu-kit\tools\kit.ps1
$d = New-Scratch -Name gdb
$p = Start-Xemu -Exe $XNew -Dir $d -Stub -Extra '-gdb', 'tcp:127.0.0.1:1234'
py -3 $Ctl wait 4444 180; py -3 $Ctl loadvm 4444 azurik-heavy-f1; Start-Sleep 5
py -3 $Ctl gdbcheck 1234 8c230 30                  # Azurik's player tick, hit every game tick
$f0 = (py -3 $Ctl qmp 4444 query-xemu-frame | ConvertFrom-Json).frame; Start-Sleep 2
$f1 = (py -3 $Ctl qmp 4444 query-xemu-frame | ConvertFrom-Json).frame
"running after detach: $($f1 -gt $f0)"
Stop-Xemu $p
```

Pass: `stop=T05 ... eip=0x8c230 ... PASS` within a second or two, and the game runs again after detaching.

### 3.7 Deterministic mode and the golden hashes [agent, P1, 45 min, mostly unattended]

The goldens below are from `scripts/bench-det.py` on macOS arm64, and they are bit-exact on master through v0.15. The recipe is: fixture HDD, EEPROM and config; player 1 stubbed; muted; `XEMU_HEADLESS=null`; `-S -icount shift=2,align=off,sleep=off`; then `loadvm`; then `xemu-frame-advance`; then the first 16 hex digits of the sha256 of guest RAM. `det_golden.py` repeats that recipe exactly.

| Golden | Command | macOS hash |
|---|---|---|
| heavy-f1, 300 frames | `--tag azurik-heavy-f1 --hash-at 300,1200` | `f300=6a553d8c7727c6c6` |
| heavy-f1, 1200 frames | same run | `f1200=037271ebadb297c5` |
| stream-e2 walk | `--tag azurik-stream-e2 --hash-at 600 -i "axis ly 1.0"` | `f600=1e98a85ee122f87c` |
| cold boot | `--tag - --hash-at 900,1800` | `f900=9ffd0e1d2e1f36ea f1800=85dec4a26bcb49e9` |

In principle the guest computation doesn't depend on the host. A match on Windows would therefore be a strong cross-host check. In practice there are known reasons a hash can differ, so first separate "Windows is deterministic" (must pass) from "Windows matches macOS" (a finding either way):

1. The DSP engine. On macOS the fork's DSP JIT runs inside the C interpreter. On Windows the default config runs upstream's dsp56300 JIT, a different engine that may differ in timing or results. `audio.use_dsp_jit = false` gives Windows the plain interpreter, the closest thing to macOS.
2. The x87 hard FPU. x86_64 uses host `long double` arithmetic on the x87 unit (64-bit mantissa, or 53-bit on threads that keep Windows' default control word) and rounds with `pack()`. aarch64 uses `double`. NaN payload rules also differ between ARM and x86. `perf.hard_fpu = false` (softfloat everywhere) removes this axis, but the macOS goldens were computed with `hard_fpu = true`, so that run needs a fresh macOS reference.
3. The SSE fast path. It's on by default on aarch64, where it follows ARM's NaN priority. It's off on x86_64, where softfloat runs.
4. APU float code generation. The goldens are known to move with code generation alone ("APU float under `-ffp-contract=fast`"). Windows x86_64 is GCC at x86-64-v3, which has FMA and different contraction than clang arm64, and it runs SSE2 kernels instead of NEON.
5. The cold boot, and only the cold boot, also passes through Windows' worker-thread block I/O (the icount settle is meant to make it deterministic anyway).

Windows ARM64 is the closest host to macOS: it uses `double` helpers and NEON SSE, though not the fork's DSP JIT.

```powershell
. C:\xemu-kit\tools\kit.ps1
$out = "$Res\det-goldens.jsonl"
$cfgs = @(
  @{ name = 'default';            set = @() },
  @{ name = 'interp';             set = @('--set', 'audio.use_dsp_jit=false') },
  @{ name = 'interp-softfpu';     set = @('--set', 'audio.use_dsp_jit=false', '--set', 'perf.hard_fpu=false') }
)
foreach ($c in $cfgs) { foreach ($rep in 1, 2) {
  $lab = "$Ver $($c.name) r$rep"
  py -3 $Det --exe $XNew --label $lab @($c.set) --tag azurik-heavy-f1 --hash-at 300,1200 --out $out
  py -3 $Det --exe $XNew --label $lab @($c.set) --tag azurik-stream-e2 --hash-at 600 -i 'axis ly 1.0' --out $out
  py -3 $Det --exe $XNew --label $lab @($c.set) --tag - --hash-at 900,1800 --out $out
} }
Get-Content $out | ConvertFrom-Json | Format-Table label, tag, hashes, fps, mcyc_per_frame, error -AutoSize
```

Pass (must): within each config, r1 and r2 give identical hashes for every golden, and nothing reports `error`. Then record the cross-host verdict for each config: match or no match against the table. If nothing matches, capture the evidence that lets the Mac find the first divergence:

```powershell
py -3 $Det --exe $XNew --label 'pages interp' --set audio.use_dsp_jit=false --tag azurik-heavy-f1 --hash-at 1,2,5,10,30,100,300 --pages --wav --out "$Res\det-pages.jsonl"
py -3 $Det --exe $XNew --label 'wav default' --tag azurik-heavy-f1 --hash-at 300 --wav --out "$Res\det-pages.jsonl"
```

Bring back `results\det\*.pages` and `*.wav`. On the Mac, the same `--hash-at` list with `xemu-ram-digest pages=true` names the first frame and the pages that differ. Audio buffers versus game state tells you whether the APU or the CPU diverged. Also compare the WAV hashes, which are deterministic under icount.

Also run the heavy-f1 golden once with the debug build (`--exe $XNewDbg`). Its asserts must stay silent and its hashes must equal the release build's for the same config.

### 3.8 `XEMU_SSE_HOST=2` differential [agent, P1, 15 min]

Mode 2 runs the host SSE path and softfloat side by side and aborts on any divergence that isn't NaN-class. It was the gate that turned the host path on for x86_64 (2026-10-18). Roadmap item 5 notes one un-root-caused ±0 sign divergence that only showed under Rosetta.

```powershell
. C:\xemu-kit\tools\kit.ps1
py -3 $Det --exe $XNew --label sse2 --tag azurik-heavy-f1 --hash-at 1200 --env XEMU_SSE_HOST=2 --out "$Res\det-sse.jsonl"
py -3 $Det --exe $XNew --label sse2 --tag azurik-stream-e2 --hash-at 1200 -i 'axis ly 1.0' --env XEMU_SSE_HOST=2 --out "$Res\det-sse.jsonl"
$d = New-Scratch -Name sse2-live
$p = Start-Xemu -Exe $XNew -Dir $d -Stub -Vars @{ XEMU_SSE_HOST = '2'; XEMU_INPUT_PIPE = 'xemu-kit-sse' }
py -3 $Ctl wait 4444 180; py -3 $Ctl loadvm 4444 azurik-stream-e2; py -3 $Inj send xemu-kit-sse 'axis ly 1.0'
Start-Sleep 300; $alive = -not $p.HasExited; Stop-Xemu $p; "alive after 5 min: $alive"
Select-String -LiteralPath (Get-XemuLog $d) -Pattern 'SSE|diverg' | Select-Object -Last 5
```

Pass: no abort, the process is alive after 5 minutes, and on x86_64 the det hash equals the default-config hash from 3.7 for the same frames (softfloat is the default there). Builds before 2026-10-18 ran softfloat by default on x86_64; later ones run the host path, and mode 2 checks it.

### 3.9 Windows ARM64 only [agent, P1, 45 min]

Skip this on x86_64. On ARM64, the fork's aarch64 TCG backend runs on Windows for the first time: pinned guest registers in x8-x15 and x24-x27, the TLB memo in x4-x7, and the hot/cold TB split. The inline x87 and SSE are compiled out there (`TCG_TARGET_HAS_fpu 0`) and the batched icache flush is off.

- Run 2.2, 2.3 and 3.7 first. All goldens again with the debug build (`--exe $XNewDbg`); its pinned-register and TLB-memo checks abort with "a helper without a reload wrote the pinned env field" or "the TLB entry changed under a TLB memo". Pass: no abort, and the same hashes as the release build.
- Sub-page store prefilter (arm b), opt-in and unvalidated on non-Apple aarch64:

```powershell
. C:\xemu-kit\tools\kit.ps1
$d = New-Scratch -Name subpage-arm64
$p = Start-Xemu -Exe $XNew -Dir $d -Stub -Vars @{ XEMU_SUBPAGE_FAST = '1'; XEMU_SUBPAGE_FAST_REFUTE = '1'; XEMU_SUBPAGE_FAST_STATS = '1'; XEMU_INPUT_PIPE = 'xemu-kit-sp' }
py -3 $Ctl wait 4444 180; py -3 $Ctl loadvm 4444 azurik-stream-e2; py -3 $Inj send xemu-kit-sp 'axis ly 1.0'
Start-Sleep 180; py -3 $Ctl loadvm 4444 azurik-transition-f1; py -3 $Inj send xemu-kit-sp 'axis ly 1.0'; Start-Sleep 60
Stop-Xemu $p; Select-String -LiteralPath (Get-XemuLog $d) -Pattern 'subpage|VIOLATION|violation' | Select-Object -Last 10
```

  Pass: no `VIOLATION` and every `violations=` count is 0.
- DSP: the fork's JIT is compiled out on Windows by design, so `XEMU_DSP_JIT_DIFF` does nothing here. Instead, record which engine works and what it costs. Run heavy-f1 (2.3 style, 60 s in the scene) with `-Vars @{ XEMU_APU_PROF = '1' }` twice: once on the default scratch config (upstream's dsp56300 JIT) and once after `Set-TomlKey "$d\xemu.toml" 'audio' 'use_dsp_jit' 'false'` (the interpreter). Read `Select-String -LiteralPath (Get-XemuLog $d) -Pattern 'apu: utilization'`. Pass: utilization well under 100% at about 1500 frames/s, with no crackle on the owner's listen.
- Vulkan on Adreno: if init prints `Error: Device does not support required feature <name>`, record the name. That's a concrete finding: the Apple branch already relaxes that list, and non-Apple relaxed only `shaderTessellationAndGeometryPointSize`. Fall back to OpenGL and continue.
- Run `XEMU_SSE_HOST=2` (3.8) here too. This is gating-audit item I.10.

### 3.10 Renderer debt on a native driver [agent, P1, 45 min]

a) Validation-layer run (needs 1.11):

```powershell
. C:\xemu-kit\tools\kit.ps1
$d = New-Scratch -Name validation
Set-TomlKey "$d\xemu.toml" 'display.vulkan' 'validation_layers' 'true'
Set-TomlKey "$d\xemu.toml" 'display.vulkan' 'assert_on_validation_msg' 'false'
$p = Start-Xemu -Exe $XNew -Dir $d -Stub -Vars @{ XEMU_INPUT_PIPE = 'xemu-kit-val' }
py -3 $Ctl wait 4444 300
foreach ($t in 'azurik-heavy-f1', 'azurik-heavy-w2', 'azurik-stream-e2', 'azurik-transition-f1', 'azurik-movie-e2') {
  py -3 $Ctl loadvm 4444 $t 180
  if ($t -ne 'azurik-heavy-f1' -and $t -ne 'azurik-heavy-w2') { py -3 $Inj send xemu-kit-val 'axis ly 1.0' }
  Start-Sleep 60; py -3 $Inj clear xemu-kit-val; Save-Shot "$d\$t.png"
}
Stop-Xemu $p
Select-String -LiteralPath (Get-XemuLog $d) -Pattern 'Validation layers enabled|validation layers not available'
Select-String -LiteralPath (Get-XemuLog $d) -Pattern 'VUID-[A-Za-z0-9_-]+' -AllMatches | ForEach-Object { $_.Matches.Value } | Group-Object | Sort-Object Count -Descending | Format-Table Count, Name
```

Pass: zero VUIDs. Each VUID is a finding: put the full `[vk]` message in RESULTS.md and the log in results. This covers B1 (occlusion queries), B4 (bind skips), B8 (zeta quarantine), F1 (texture LRU) and the 2026-08 descriptor class.

b) Query-pool capacity guard (B1, audit batch 3): repeat 2.3 for heavy-f1 with `-Vars @{ XEMU_MAX_QUERIES = '64'; XEMU_NV2A_NSPROF = '1' }` and 120 s in the scene. Pass: no crash or abort, the scene renders, and `Select-String -LiteralPath (Get-XemuLog $d) -Pattern 'finish_(queries|reports)_full'` shows `finish_queries_full` firing and no `finish_reports_full` (the report pool keeps its default size; before 2026-10-18 it shrank with the query pool, and its guard fired first).

c) Walk-ins (B3 vertex refinement, B6 surface expiry on monotonic time, B7 fence watchdog, B8, F1): the 3.11 soak covers stream-e2. Also walk `azurik-transition-f1` forward about 4 s (it loads F6), wait 15 s, walk back (it reloads F1) three times, and walk `azurik-movie-e2` forward into its cutscene. Use `py -3 $Inj down <pipe> W` / `up` / `S`. Pass: no garbled geometry, no black freeze at the loads, no magenta, and no `vkWaitForFences timeout` warning.

d) Geometry-shader versus CPU-emulated primitive parity (frontier item 4): native drivers take the geometry-shader path, while MoltenVK emulates on the CPU. Offscreen Vulkan is deterministic under icount, so capture frames to compare with the Mac:

```powershell
. C:\xemu-kit\tools\kit.ps1
py -3 $Det --exe $XNew --label shots --renderer offscreen --shots --tag azurik-heavy-f1 --hash-at 60,300 --out "$Res\det-shots.jsonl"
py -3 $Det --exe $XNew --label shots --renderer offscreen --shots --tag azurik-heavy-w2 --hash-at 60,300 --out "$Res\det-shots.jsonl"
py -3 $Det --exe $XNew --label shots --renderer offscreen --shots --tag azurik-stream-e2 --hash-at 600 -i 'axis ly 1.0' --out "$Res\det-shots.jsonl"
```

Bring back `results\det\*.png`. Small rasterization differences are normal; missing or extra geometry, wrong point sprites or wrong lines are not.

### 3.11 Lockless MMIO and PFIFO soak [agent, P1, 35 min unattended]

There has never been a non-VM soak of the BQL-free MMIO dispatch (PFB, USER, APU-VP, PGRAPH), the lockless DMA_PUT doorbell or the PFIFO untimed wait.

```powershell
. C:\xemu-kit\tools\kit.ps1
$d = New-Scratch -Name soak
$p = Start-Xemu -Exe $XNew -Dir $d -Stub -Vars @{ XEMU_PFIFO_HEARTBEAT = '1'; XEMU_NV2A_NSPROF = '1'; XEMU_INPUT_PIPE = 'xemu-kit-soak' }
py -3 $Ctl wait 4444 180
$tags = 'azurik-heavy-f1', 'azurik-heavy-w2', 'azurik-stream-e2', 'vm-20260704032357', 'azurik-transition-f1'
$log = "$Res\soak-$(Get-Date -Format yyyyMMdd-HHmm).txt"
for ($i = 0; $i -lt 30; $i++) {
  $t = $tags[$i % $tags.Count]
  if ($p.HasExited) { "DIED before cycle $i, exit $($p.ExitCode)" | Tee-Object -Append $log; break }
  $r = py -3 $Ctl loadvm 4444 $t 120
  if ($t -in 'azurik-stream-e2', 'azurik-transition-f1') { py -3 $Inj down xemu-kit-soak W | Out-Null }
  Start-Sleep 50
  py -3 $Inj clear xemu-kit-soak | Out-Null
  $s = Save-Shot "$d\c$i.png"
  $f = (py -3 $Ctl qmp 4444 query-xemu-frame | ConvertFrom-Json).frame
  "$i $t frame=$f | $r | $s" | Tee-Object -Append $log
}
Stop-Xemu $p
Select-String -LiteralPath (Get-XemuLog $d) -Pattern 'vkWaitForFences|rror|assert' | Select-Object -Last 20 | Tee-Object -Append $log
Select-String -LiteralPath (Get-XemuLog $d) -Pattern 'heartbeat' | Select-Object -Last 3 | Tee-Object -Append $log
```

Pass: 30 of 30 cycles, every screenshot PASS, the frame counter rising every cycle, the heartbeat `iters` still rising at the end, and no fence warnings, errors or asserts. A frozen `iters` means PFIFO is stuck; re-run with `XEMU_PFIFO_PUT_LOCK=1` to split the doorbell off. Don't use the machine during the soak. If `Select-String` reports the log as in use while xemu runs, it works after `Stop-Xemu`.

### 3.12 TCG refuters on x86_64 [agent, P1, 20 min]

These are portable TCG optimizations (sub-page code tracking, cross-page chaining, writable-clean TLB bitmaps, keeping translations across loads), checked against ground truth. Run two refuters per launch, and use the walks and loads that exercise them.

```powershell
. C:\xemu-kit\tools\kit.ps1
foreach ($pair in @(@{ XEMU_SUBPAGE_REFUTE = '1'; XEMU_XPAGE_REFUTE = '1'; XEMU_XPAGE_PROF = '1' }, @{ XEMU_TLB_WCLEAN_REFUTE = '1'; XEMU_TB_REVAL_REFUTE = '1' })) {
  $d = New-Scratch -Name refute
  $v = $pair.Clone(); $v['XEMU_INPUT_PIPE'] = 'xemu-kit-ref'
  $p = Start-Xemu -Exe $XNew -Dir $d -Stub -Vars $v
  py -3 $Ctl wait 4444 180
  foreach ($t in 'azurik-stream-e2', 'azurik-transition-f1', 'azurik-heavy-f1', 'azurik-stream-e2') {
    py -3 $Ctl loadvm 4444 $t 120; py -3 $Inj send xemu-kit-ref 'axis ly 1.0'; Start-Sleep 60; py -3 $Inj clear xemu-kit-ref
  }
  if ($pair.ContainsKey('XEMU_TB_REVAL_REFUTE')) {
    # the reval refuter audits the scoped check of incremental checkpoint restores; the first restore after a save takes the full walk
    py -3 $Ctl qmp 4444 xemu-checkpoint-save name=reval
    for ($i = 0; $i -lt 6; $i++) {
      py -3 $Inj send xemu-kit-ref 'axis ly 1.0'; Start-Sleep 10; py -3 $Inj clear xemu-kit-ref
      py -3 $Ctl qmp 4444 xemu-checkpoint-restore name=reval incremental=true
    }
  }
  Stop-Xemu $p                       # the dumps print at a clean quit
  Select-String -LiteralPath (Get-XemuLog $d) -Pattern 'refute|VIOLATION|XPAGE summary|subpage' | Select-Object -Last 12
}
```

Pass: no line containing `VIOLATION`, `tlb-wclean refute ... misses=0 unset=0`, and `xemu: reval refute: N scoped loads ...` with N of at least 5 (the restores minus one), `extra drops 0, unprotected pages 0`. On x86_64, also confirm that `XEMU_SUBPAGE_FAST=1` prints `xemu: XEMU_SUBPAGE_FAST: no inline store-prefilter emitter for this TCG host backend (aarch64 only); ignored`, because the store prefilter is aarch64-only.

### 3.13 Threads, timers and priority [agent, P1, 30 min]

Windows has no equivalent of the macOS QoS bumps (audit A6). On macOS, putting the vCPU at user-interactive QoS took heavy-f1 under load from 37 to 58 fps. These checks show whether Windows needs a `SetThreadPriority` or MMCSS change (audit batch 17).

a) Vblank rate in each window state. In each state the guest must see 60.0 vblanks/s:

```powershell
. C:\xemu-kit\tools\kit.ps1
function Measure-Vblank([int]$Port = 4444, [int]$Secs = 30) {
  $a = (py -3 $Ctl qmp $Port query-xemu-frame | ConvertFrom-Json).frame; Start-Sleep $Secs
  $b = (py -3 $Ctl qmp $Port query-xemu-frame | ConvertFrom-Json).frame; [math]::Round(($b - $a) / $Secs, 2)
}
$d = New-Scratch -Name vblank
$p = Start-Xemu -Exe $XNew -Dir $d -Stub
py -3 $Ctl wait 4444 180; py -3 $Ctl loadvm 4444 azurik-heavy-f1
"visible, focused: $(Measure-Vblank)"     # owner: click the xemu window first
"covered by another window: $(Measure-Vblank)"   # owner: maximize another window over it
"minimized: $(Measure-Vblank)"            # owner: minimize xemu
Stop-Xemu $p
```

Pass: 59.9-60.1 in every state. Windows 11 may ignore timer-resolution requests from minimized or occluded processes, so a drop when minimized is a finding.

b) Host-load sensitivity: run the 4.3 heavy-f1 baseline (2 pairs × 45 s) twice, once idle and once with a CPU burner on every logical core. Then try the burner with xemu's priority raised:

```powershell
$jobs = 1..[Environment]::ProcessorCount | ForEach-Object { Start-Job { while ($true) { } } }
# ... run the bench here ...
Stop-Job $jobs; Remove-Job $jobs
```

For the priority check, start one windowed session (2.3 style), set `(Get-Process -Id $p.Id).PriorityClass = 'AboveNormal'` and compare the nsprof flips/s under the burner. Pass is informational: record the fps for idle, loaded and loaded at AboveNormal. More than about a 10% drop that the priority class recovers makes the case for a priority change.

c) Hybrid CPUs (Intel 12th gen and later): compare nsprof flips/s for heavy-f1 with xemu focused against another app focused while xemu stays visible. Windows' EcoQoS can move background windows to E-cores. Record both.

### 3.14 Snapshots, thumbnails and disc fingerprints [owner + agent, P1, 15 min]

Use the personal setup (`xemu-personal.toml`, play copies):

- Snapshots > create a snapshot named `win-test-1` with `Azurik_qol.iso` in the tray. Pass: stderr has `xemu: disc fingerprint: <path>: <n> bytes, xxh3-128 <hex> (<t> s)` with t under about 2 s.
- Load `win-test-1` from the menu with the same disc in the tray. Pass: no disc dialog.
- A snapshot saved before fingerprints existed always asks on Windows, because Windows has no inode change time. That's expected: check that stderr says why (`... asking`).
- The thumbnails show for every snapshot.
- Delete `win-test-1` afterwards. It lives only in the play copy.

### 3.15 Smaller checks [agent, P2]

- A second xemu with the same `-qmp tcp:127.0.0.1:4444` fails cleanly (address in use) and leaves the first one running. Windows keeps upstream's socket behaviour here. Launch the second one with `Start-Process` directly (same arguments as `Start-Xemu` builds), because `Start-Xemu` refuses busy ports.
- Live pads under icount, then a replay: start with `-Vars @{ XEMU_ICOUNT_LIVE_PADS = '1'; XEMU_INPUT_RECORD = "$d\rec.txt"; XEMU_INPUT_RECORD_PORT = '1' } -Extra '-icount','shift=2,align=on,sleep=on'` and a controller bound to port 1, play 30 s after a `loadvm`, then quit. Replaying the record's last section through `det_golden.py -i` lines under `sleep=off` should reach the same RAM digest. That's exact on macOS and unverified on Windows.
- `[automation]` in the pref-dir config (`%APPDATA%\xemu\xemu\xemu.toml`): only test it if the owner sets up their own Windows play config there. Each key prints an `xemu: [automation] ...: applied` line.

## 4. Performance (P1)

### 4.1 Rules

- The instrument is `scripts\bench-savestate-ab-win.py`: windowed Vulkan, nsprof 5 s intervals, interleaved B/E pairs, a scene gate (draws per flip), a mid-run screenshot and a receipt. Never compare absolute fps across hosts.
- Keep the xemu window visible, unobscured and on the primary screen. Don't use fullscreen (captures come out black). Plug the laptop in, set the power plan to High performance (record it), and don't use the machine during a batch.
- The bench logs the load (`quiet_check.py`) and stops only for a leftover xemu; copy the load it printed into RESULTS.md. Don't wait for an idle machine. (Before 2026-10-18 it refused a busy machine unless given `--no-quiet-check`.)
- `XEMU_NOOP_AB` is a variable nothing reads. Use it as the dummy for A/A runs and cross-binary runs (the script needs an `<ENV_VAR>`).
- The `azurik-*` tags need player 1. The bench doesn't stub pads, so set `XEMU_LOADVM_ADD_PADS=1` (v0.15+) in the session. For v0.14 arms use F5 (`vm-20260704032357`, player 2 only) or a template with a real controller bound to port 1 (4.4).
- If the anchors sit at the 60 fps cap on this machine, windowed fps can't show a speedup. Then compare CPU% (`ab_*.cpu`) and draws/s, and use the deterministic Mcyc/frame metric (4.6) for CPU-side work.

### 4.2 Noise floor and harness check [agent, 10 min]

```powershell
. C:\xemu-kit\tools\kit.ps1
$env:XEMU_LOADVM_ADD_PADS = '1'
py -3 $Bench azurik-heavy-f1 XEMU_NOOP_AB 2 45 "$Res\bench-aa-f1" --exe $XNew
```

Pass: a receipt is printed, no run is DEAD or INVALID, and both arms agree within the spread. INVALID runs with empty logs mean the console problem from 3.1.

### 4.3 v0.15.1 baselines on the anchors [agent, 30 min]

```powershell
. C:\xemu-kit\tools\kit.ps1
$env:XEMU_LOADVM_ADD_PADS = '1'
foreach ($t in 'azurik-heavy-f1', 'azurik-heavy-w2', 'vm-20260704032357') {
  py -3 $Bench $t XEMU_NOOP_AB 3 60 "$Res\bench-base-$t" --exe $XNew
}
```

Record, per anchor: fps mean ± sd over all 6 runs, draws/flip, passes/flip, fence_wait ms/flip, CPU%, and whether it's capped. These are the first Windows baselines: gating-audit D6 says no macOS number transfers.

### 4.4 v0.14 versus v0.15.1 [agent, 20-40 min]

```powershell
. C:\xemu-kit\tools\kit.ps1
py -3 $Bench vm-20260704032357 XEMU_NOOP_AB 3 60 "$Res\bench-v014-v015-F5" --b-exe $XOld --e-exe $XNew
```

For heavy-f1 across versions, connect a controller and bind it to Port 1 once in a personal session. Then build a template with that binding (`Get-TomlKey "$Work\xemu-personal.toml" 'input.bindings' 'port1'` shows the Windows GUID):

```powershell
$t2 = "$Work\xemu-fixtures-pad.toml"; Copy-Item $env:XEMU_TEST_CONFIG $t2 -Force
Set-TomlKey $t2 'input.bindings' 'port1' (Q (Get-TomlKey "$Work\xemu-personal.toml" 'input.bindings' 'port1'))
py -3 $Bench azurik-heavy-f1 XEMU_NOOP_AB 3 60 "$Res\bench-v014-v015-f1" --b-exe $XOld --e-exe $XNew --config-template $t2
```

v0.14 predates `XEMU_BASE_PATH`, so the bench isolates both arms' caches. Each binary gets its own warmup run, which absorbs its cold-cache pass. The receipt refuses the delta when draws per flip differ by more than 15%.

### 4.5 Normal-mode switch A/Bs [agent, 15 min each]

The B arm is the variable set to `0`, and the E arm is `1`. These switches exist because the goldens can't verify normal-mode behaviour, and their defaults were measured only on macOS.

```powershell
. C:\xemu-kit\tools\kit.ps1
$env:XEMU_LOADVM_ADD_PADS = '1'
py -3 $Bench azurik-heavy-f1 XEMU_BQL_SPIN 3 60 "$Res\bench-bqlspin" --exe $XNew              # B no spin, E spin (default)
py -3 $Bench azurik-heavy-f1 XEMU_KI_IDLE_HALT 3 60 "$Res\bench-kiidle" --exe $XNew          # B spin in KiIdleLoop, E halt (default)
py -3 $Bench azurik-heavy-f1 XEMU_PFIFO_STALL_DOORBELL 3 60 "$Res\bench-stalldb" --exe $XNew # B always wake, E default
py -3 $Bench azurik-heavy-f1 XEMU_SSE_HOST 3 60 "$Res\bench-ssehost" --exe $XNew             # x86_64 v0.15.1 only; later builds have it on
```

Pass for each default: E is no slower than B within noise, and CPU% isn't worse. The KiIdle halt depends on Windows condition-variable wake latency and the BQL spin on QueryPerformanceCounter cost, so a regression is a plausible Windows-only finding. For the DSP engine (audit batch 21, most useful on ARM64), the bench can't vary the config between arms. Run the 4.3 heavy-f1 baseline twice instead: once with the default template, and once with `--config-template` pointing at a copy that has `Set-TomlKey <copy> 'audio' 'use_dsp_jit' 'false'`. Set `$env:XEMU_APU_PROF = '1'` in the session for both, and compare fps, CPU% and the `apu: utilization` lines in the run logs.

### 4.6 Deterministic CPU metric [agent, 15 min]

`det_golden.py` reports wall fps and Mcyc/frame from `QueryProcessCycleTime`. Those cycles run at the TSC rate, so unlike macOS they measure CPU time, not core clocks. Use them for CPU-side A/Bs with interleaved pairs:

```powershell
. C:\xemu-kit\tools\kit.ps1
$o = "$Res\det-ab-sse.jsonl"
1..4 | ForEach-Object {
  py -3 $Det --exe $XNew --label base --hash-at 1200 --out $o
  py -3 $Det --exe $XNew --label sse1 --hash-at 1200 --env XEMU_SSE_HOST=1 --out $o
}
Get-Content $o | ConvertFrom-Json | Group-Object label | ForEach-Object {
  [pscustomobject]@{ arm = $_.Name; n = $_.Count
    mcyc = [math]::Round(($_.Group.mcyc_per_frame | Measure-Object -Average).Average, 2)
    fps = [math]::Round(($_.Group.fps | Measure-Object -Average).Average, 1)
    hashes = ($_.Group.hashes | Select-Object -Unique) -join ' | ' } } | Format-Table -AutoSize
```

Pass: one unique hash per arm. Quote the Mcyc/frame delta and how many pairs favour each arm. On v0.15.1 the `sse1` arm's hash differs (its host path dropped the sticky MXCSR flags; later builds raise them and have it on by default, so use this recipe with another variable there), and `--pmc` adds instructions and core cycles per frame.

### 4.7 Upstream xemu as a reference [owner, 10 min, P2]

Upstream v0.8.137 most likely can't load the fork's savestates, because the fork adds vmstate subsections. Try one load and record the error. Then boot `Azurik_qol.iso` in both builds to the same spot (the title attract loop, or a short walk from a fresh save) and read the fps from the menu bar's fps readout (or the debug video window). This is qualitative only. Use a scratch copy of the personal config with `-name xemu-fork-auto` and `volume_limit = 0` (`New-Scratch -Template "$Work\xemu-personal.toml"`, then `Start-Xemu -Exe $XUp`).

### 4.8 Local builds [P2]

Builds go through section 7. The cross-binary A/Bs worth having are the CI release against a native MSYS2 `-O2` build (toolchain effect), native `-O2` against `XEMU_WIN_O3=1`, and native `-O2` against GCC PGO. Run each with `py -3 $Bench azurik-heavy-f1 XEMU_NOOP_AB 4 75 <out> --b-exe <B> --e-exe <E>`, plus 4.6 with `--exe` swapped per arm.

### Numbers to bring back

For every batch: the printed receipt block, plus `meta.json` and `receipt.json` (zip the whole out dir). In RESULTS.md, one row per batch: anchor, arms, B fps mean ± sd, E fps mean ± sd, delta %, sign tally, draws/flip in each arm, fence_wait ms/flip, CPU% in each arm, load, and verdict.

## 5. Upstream CI signal: Roswell tests (P2)

The fork's CI (`.github/workflows/ci.yml`) now calls upstream's `xemu-project/xemu-test` Roswell workflow (`test-roswell.yml@wheel-202610142116`) against the fork's own build artifacts on Ubuntu, Windows and macOS. On master `ca351a6b21` (run 39013910935) the Windows and Ubuntu jobs pass, and macOS fails (being worked on in `fix/roswell-fpu`). CI's Windows job uses Mesa llvmpipe (software GL), `--no-hard-fpu` and `failure_filters/windows_roswell_softfpu.json`. Running it locally adds a real GPU driver.

The test data (`tester.iso`) is built with Docker in CI. The easy route is to download the artifacts of a fresh CI run of the `windows` branch; they expire after 1 day. Pushing the branch (6.3) triggers that run.

```powershell
. C:\xemu-kit\tools\kit.ps1
Set-Location C:\
git clone --recurse-submodules https://github.com/xemu-project/xemu-test.git
Set-Location C:\xemu-test
py -3.12 -m venv .venv; .\.venv\Scripts\Activate.ps1
pip install git+https://github.com/abaire/xemu-pgraph-ci-tools.git
pip install .
$run = gh run list --repo MichaelJSr/xemu-macos --branch windows --workflow CI --limit 1 --json databaseId --jq '.[0].databaseId'
gh run download $run --repo MichaelJSr/xemu-macos -n test-data -D xemutest\data
gh run download $run --repo MichaelJSr/xemu-macos -n roswell-bios -D private
Move-Item private\roswell.bin private\bios.bin -Force
New-Item -ItemType Directory -Force results | Out-Null
python -m xemutest --no-mcpx --sysram 128 --no-hard-fpu --failure-filter failure_filters/windows_roswell_softfpu.json --ci-summary-omit-innermost-passed-subtests -v $XNew private results
deactivate
```

Pass: the same pass/fail set as CI's `results-windows-latest` artifact (`gh run download $run -n results-windows-latest`). Differences on a real GPU, in either direction, are findings. Optionally run it once without `--no-hard-fpu`, and once with `$XUp` as a control.

## 6. Recording results and filing them

### 6.1 RESULTS.md template

Create it at the start (step 1.11) and fill it in as you go:

```powershell
$tmpl = @'
# Windows results: <date>, <machine name>

## Machine
| Item | Value |
|---|---|
| OS | |
| CPU (hybrid?) | |
| RAM | |
| GPU + driver | |
| Display (res, Hz, scaling) | |
| Power plan, Defender exclusion | |
| Arch (x86_64 / arm64) | |

## Builds under test (sha256[:16] of xemu.exe)
| Build | Path | sha256[:16] |
|---|---|---|

## Setup notes (anything that did not work as written)

## P0 smoke (section 2)
| Check | Result | Notes |
|---|---|---|

## Windows paths (section 3)
| Id | Test | Result | Evidence file |
|---|---|---|---|

## Deterministic goldens (3.7)
| Build | Config | heavy-f1 f300 | f1200 | stream-e2 f600 | cold f900 | cold f1800 | r1 = r2? | matches macOS? | Mcyc/frame | fps |
|---|---|---|---|---|---|---|---|---|---|---|

## Performance (section 4)
| Batch | Anchor | Arms | B fps | E fps | Delta | Signs | draws/flip B/E | fence ms/flip | CPU% B/E | Load | Verdict |
|---|---|---|---|---|---|---|---|---|---|---|---|

## Soak and refuters (3.11, 3.12)

## Crashes, hangs, dumps (exit code, time to crash, dump file, top of stack)

## Findings (one line each: severity, symptom, repro, suspected area, evidence)

## Commits pushed to windows
'@
[IO.File]::WriteAllText('C:\xemu-kit\results\RESULTS.md', $tmpl.Replace("`r`n", "`n"))
```

### 6.2 What to bring back

```powershell
$stamp = Get-Date -Format yyyyMMdd
Compress-Archive -Path C:\xemu-kit\results\* -DestinationPath "C:\xemu-kit\windows-results-$stamp.zip" -Force
```

Leave out `results\dumps` if it's large (full dumps are GBs); copy those separately. The zip holds RESULTS.md, machine.json, dxdiag.txt, the det-*.jsonl files, `results\det` (page hashes, WAVs, PNGs), the bench out dirs, the soak log and every interesting `stderr.log` or `xemu.log` (copy them in from `C:\xemu-kit\work\<run>`). `New-Scratch` already deletes each finished run's HDD copy and `det_golden.py` drops a failed run's, so the run dirs left in `work` are small. Once you've copied out what you need, clear them with `Get-ChildItem C:\xemu-kit\work -Directory | Where-Object Name -ne 'shots' | Remove-Item -Recurse -Force` (it keeps the two configs and the `shots` folder).

### 6.3 Filing findings as commits on `windows`

- One logical change per commit, titled `<subsystem>: <short description>` (see `git log --oneline -- <path>` for the prefixes: `nv2a/vk:`, `ui:`, `scripts/win:`, `tcg:`, `target/i386:`, `build:`, `docs:`). The body says why, what and the numbers. End it with the `Co-Authored-By:` trailer the session's harness supplies.
- Docs go in the same commit as the change. Put a README line only for user-visible behaviour (for example a Windows troubleshooting entry). Wrap platform-only code in `#ifdef _WIN32`, because CI builds macOS and Linux too.
- A finding that motivates a fix rides in that fix's commit; measurements with no code change can go in one results commit on `windows`. Add a dated section to `docs/windows-gating-audit.md` ("Windows real-hardware report: <date> (v0.15, <GPU>, <CPU>)") that marks the needs-real-hardware items it settled, and update `docs/roadmap.md` item 5 in the same commit. No measurement logs go in skills.
- Harness fixes (bench-win, `scripts/win/*`) are their own `scripts/win:` commits.

```powershell
Set-Location "$env:USERPROFILE\xemu-macos"
git switch windows
git status --short
git add <changed files>
git commit -m '<subsystem>: <short description>' -m '<why, what, numbers>' -m 'Co-Authored-By: <exact trailer from your system prompt>'
git push -u origin windows
gh run list --repo MichaelJSr/xemu-macos --branch windows --limit 3
```

CI runs on every branch (`branches: ['**']`), so each push builds Windows x86_64 and arm64 and runs the Roswell tests. To test a fix without a local toolchain, push it and wait for the run. Then download the build with `gh run download <id> --repo MichaelJSr/xemu-macos -n xemu-windows-x86_64-release -D C:\xemu-kit\bin\ci-<sha>` (use `-arm64-` on ARM) and rerun the failing step with `-Exe C:\xemu-kit\bin\ci-<sha>\xemu.exe`. Never merge `windows` into `master` and never tag; the owner decides that on the Mac.

## 7. Local builds (optional, P2)

### 7.1 Native MSYS2 MINGW64 build of the branch

This is the README's preferred native route. It was first used on 2026-09-07, and the package list comes from `docs/Campaigns/windows-port-2026-09.md`. The first build takes about 10 min on a 4-core Haswell and needs network for the meson subprojects.

```powershell
. C:\xemu-kit\tools\kit.ps1
$msys = 'C:\msys64\usr\bin\bash.exe'
& $msys -lc 'pacman --noconfirm -Syuu'
& $msys -lc 'pacman --noconfirm -Syuu'
& $msys -lc 'pacman -S --needed --noconfirm --disable-download-timeout git make patch diffutils mingw-w64-x86_64-toolchain mingw-w64-x86_64-clang mingw-w64-x86_64-lld mingw-w64-x86_64-compiler-rt mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja mingw-w64-x86_64-pkgconf mingw-w64-x86_64-python mingw-w64-x86_64-python-yaml mingw-w64-x86_64-python-requests mingw-w64-x86_64-ccache mingw-w64-x86_64-glib2 mingw-w64-x86_64-pixman mingw-w64-x86_64-libepoxy mingw-w64-x86_64-libsamplerate mingw-w64-x86_64-sdl3 mingw-w64-x86_64-libslirp mingw-w64-x86_64-vulkan-headers mingw-w64-x86_64-vulkan-loader mingw-w64-x86_64-vulkan-validation-layers mingw-w64-x86_64-zlib mingw-w64-x86_64-zstd mingw-w64-x86_64-libusb mingw-w64-x86_64-curl mingw-w64-x86_64-openssl mingw-w64-x86_64-gdb'
$env:MSYSTEM = 'MINGW64'; $env:CHERE_INVOKING = '1'
Set-Location $Repo
& $msys -lc './build.sh 2>&1 | tail -n 30'                 # release: -O2, -Dx86_version=3 (same as CI's x86_64 release)
& $msys -lc 'grep -c -- " -O3 " build/compile_commands.json; grep -c fstack-protector build/compile_commands.json'
Copy-Dist local-O2
# incremental after an edit:
& $msys -lc 'ninja -C build qemu-system-i386w.exe && cp build/qemu-system-i386w.exe dist/xemu.exe'
Remove-Item Env:MSYSTEM, Env:CHERE_INVOKING
```

Native `build.sh` writes `dist\xemu.exe` and copies the DLLs it needs (`get_deps.py`). It links SDL3 statically from the meson wrap, which carries the fork's SDL patches (`subprojects/packagefiles`), as CI does; MSYS2's SDL3 package isn't used. CI's x86_64 release adds GCC LTO (`-Db_lto=true` plus `-flto-incremental`). Natively that's `./build.sh -Db_lto=true`: it's slower, so use it only when the build itself is under test. Traps a fresh MSYS2 hits: CRLF (fixed by `core.autocrlf false`), Python 3.13's prefix check and MSYS tar (both handled in build.sh), and a missing `USERPROFILE` when bash is driven from another tool (export it first). On ARM64 hardware, use the release zips. A native CLANGARM64 build is untested.

### 7.2 CI's exact recipe (Docker, cross)

CI doesn't build on Windows. It cross-builds in a Linux container (`.github/workflows/build-windows.yml`) and then makes PDBs with cv2pdb on a Windows runner. To reproduce a CI binary byte for byte, use Docker Desktop (WSL2), ideally on a separate clone:

```powershell
winget install --accept-source-agreements --accept-package-agreements -e --id Docker.DockerDesktop   # start Docker Desktop once
git clone -b windows https://github.com/MichaelJSr/xemu-macos.git C:\xemu-cross
docker run --rm -v 'C:\xemu-cross:/src' -w /src `
  -e CROSSPREFIX=x86_64-w64-mingw32.static- -e CROSSAR=x86_64-w64-mingw32.static-gcc-ar `
  ghcr.io/xemu-project/xemu-win64-toolchain-gcc:sha-2881edd `
  bash -c './build.sh -p win64-cross -Db_lto=true -Dx86_version=3'
# arm64: image ghcr.io/xemu-project/xemu-win64-toolchain:sha-0d06ce8, CROSSPREFIX=aarch64-w64-mingw32.static-, CROSSAR=aarch64-w64-mingw32.static-ar, no extra options
Invoke-WebRequest https://github.com/rainers/cv2pdb/releases/download/v0.54/cv2pdb-0.54.zip -OutFile C:\xemu-cross\cv2pdb.zip
Expand-Archive C:\xemu-cross\cv2pdb.zip C:\xemu-cross\cv2pdb -Force
C:\xemu-cross\cv2pdb\cv2pdb64.exe C:\xemu-cross\dist\xemu.exe     # writes xemu.pdb, strips the exe
```

### 7.3 The exit-crash protocol (windows-port §9)

Only run this if section 2.2 or any native build crashed. Pre-commit the interpretation:

- If A is clean and B crashes, the flags are convicted. Keep `-O3` opt-in and hunt the latent bug from the dump.
- If A also crashes, the flags are cleared. Bisect by reverting whole commits, with 5 or more runs each, never single runs.

```powershell
. C:\xemu-kit\tools\kit.ps1
$msys = 'C:\msys64\usr\bin\bash.exe'; $env:MSYSTEM = 'MINGW64'; $env:CHERE_INVOKING = '1'; Set-Location $Repo
git rev-parse HEAD
& $msys -lc 'rm -rf build && ./build.sh 2>&1 | tail -n 3'; Copy-Dist local-O2                  # arm A (control)
& $msys -lc 'rm -rf build && XEMU_WIN_O3=1 ./build.sh 2>&1 | tail -n 3'; Copy-Dist local-O3     # arm B
Remove-Item Env:MSYSTEM, Env:CHERE_INVOKING
foreach ($arm in 'local-O2', 'local-O3') { 1..6 | ForEach-Object {
  $d = New-Scratch -Name "$arm-$_"; $p = Start-Xemu -Exe "$Kit\bin\$arm\xemu.exe" -Dir $d -Stub
  Start-Sleep 40; $alive = -not $p.HasExited; if ($alive) { Stop-Xemu $p }
  '{0} run {1}: alive={2} exit={3}' -f $arm, $_, $alive, $p.ExitCode } }
```

With 6 clean runs at the observed failure rate of about 5 in 6, P < 1e-4. Open any dump in WinDbg; a `vmovaps` or `vmovdqa` fault on an unaligned operand is the `-O3` vectorizer class. Record the commit, the sha256 of each binary, and both `grep -c` counts (7.1).

### 7.4 GCC PGO on Windows

The committed `pgo/default.profdata` is a clang profile. Windows x86_64 builds with GCC, so `build.sh` drives `.gcda` profiles instead (gating-audit D7; never exercised). `scripts/pgo-train.py` is macOS-only, so this loop replays its corpus:

```powershell
. C:\xemu-kit\tools\kit.ps1
$msys = 'C:\msys64\usr\bin\bash.exe'; $env:MSYSTEM = 'MINGW64'; $env:CHERE_INVOKING = '1'; Set-Location $Repo
& $msys -lc 'rm -rf build pgo && XEMU_PGO=generate ./build.sh 2>&1 | tail -n 5'
$G = Copy-Dist local-pgogen
$d = New-Scratch -Name pgo                         # one scratch for the whole corpus
$p = Start-Xemu -Exe $G -Dir $d -Stub; Start-Sleep 90; Stop-Xemu $p      # boot to the intro
foreach ($t in 'azurik-heavy-f1', 'azurik-heavy-f1', 'azurik-heavy-w2', 'azurik-heavy-w2', 'vm-20260704032357') {
  $p = Start-Xemu -Exe $G -Dir $d -Stub; py -3 $Ctl wait 4444 300; py -3 $Ctl loadvm 4444 $t 180; Start-Sleep 80; Stop-Xemu $p
}
$p = Start-Xemu -Exe $G -Dir $d -Stub -Vars @{ XEMU_INPUT_PIPE = 'xemu-kit-pgo' }
py -3 $Ctl wait 4444 300; py -3 $Ctl loadvm 4444 azurik-stream-e2 180; py -3 $Inj send xemu-kit-pgo 'axis ly 1.0'; Start-Sleep 60; Stop-Xemu $p
py -3 $Det --exe $G --label pgo-train --hash-at 3000
py -3 $Det --exe $G --label pgo-train --tag azurik-stream-e2 --hash-at 1200 -i 'axis ly 1.0'
& $msys -lc 'find pgo -name "*.gcda" | wc -l; XEMU_PGO=use ./build.sh 2>&1 | tail -n 8'
Copy-Dist local-pgouse
Remove-Item Env:MSYSTEM, Env:CHERE_INVOKING
```

Every session must end in a clean QMP `quit` (`Stop-Xemu` sends one) so the counters get written. Then A/B `local-O2` against `local-pgouse` (4.8) and check that the goldens didn't move unexpectedly. Don't commit `.gcda` files: they're tied to the GCC version, and CI's cross GCC differs. A CI-usable Windows profile would need a `generate` binary from CI's own toolchain, trained here, with `GCOV_PREFIX` redirecting its baked-in Linux profile path. Write that up as future work.

## Quick triage on Windows

| Symptom | First check | Next step |
|---|---|---|
| Vulkan init fails, or falls back to OpenGL | `Error: Device does not support required feature ...` or `Failed to find a suitable GPU` in stderr | record the feature; update the driver; continue on OpenGL |
| Black screen with audio | stderr `[vk]` lines; `win32_dxgi present failed` | 2.4 on OpenGL; 3.10a validation layers |
| Crash | exit code, `results\dumps`, the Application event log | WinDbg with the PDB; repeat with `$XNewDbg` |
| Freeze | `XEMU_PFIFO_HEARTBEAT=1` `iters` frozen? GPU TDR event 4101? | `XEMU_PFIFO_PUT_LOCK=1`; the fence warning names its wait site |
| `loadvm` fails with `Unknown section ...usb-hub` | player-1 pad missing | `-Stub`, or `XEMU_LOADVM_ADD_PADS=1` |
| Bench runs INVALID | `ab_*.log` empty, `xemu.log` next to the exe? | give the bench a console of its own (1.10, 3.1) |
| Pipe `cannot open` | `XEMU_INPUT_PIPE` set on that launch? the keyboard bound to port 2? | `query-xemu-input`; `Select-String -LiteralPath (Get-XemuLog $d) -Pattern 'test-input pipe'` |
| QMP refused, or `Start-Xemu` throws `port 4444 or 4445 already in use` | a leftover xemu holds the port (an interrupted call) | `Stop-AutoXemu` (stops only marked runs) |

## Appendices: helper scripts

The helpers live in the repo so they can't get lost:

- `scripts\win\kit.ps1`: session setup for the kit (paths, muted scratch configs, launches with `-config_path`). It is the same file as the kit's `tools\kit.ps1`; dot-source either at the start of every PowerShell session: `. C:\xemu-kit\tools\kit.ps1`.
- `scripts\win\xemu_ctl.py`: QMP and gdbstub control for Windows test runs (loadvm, screendump, frame advance, RAM digest).
- `scripts\win\det_golden.py`: deterministic golden runs on Windows (the macOS `bench-det.py` doesn't run there).

The two Python helpers were tested against fake QMP and gdbstub servers on the Mac only; expect small fixes on their first real run, and commit them to `windows`.
