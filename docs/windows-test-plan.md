# xemu-macos on real Windows hardware: test plan

The runbook for Claude Code on the owner's Windows PC. The one run so
far (2026-10-17/18, branch `windows`, frozen since as its comparison
point) is in [windows-gating-audit.md](windows-gating-audit.md), whose
§A lists the Windows-specific code these tests cover; history is in
[Finished/windows-port-2026-09.md](Finished/windows-port-2026-09.md).
Read the kit's `AGENT-START.md` first and fill in
`C:\xemu-kit\results\RESULTS.md` as you go. [owner] needs a person,
[agent] runs alone; P0 first, P1 this session, P2 if time remains.
Never tested here: AMD and Intel GPUs, Windows ARM64, and everything
after v0.15.2, including the 2026-11-24 merges (upstream v0.8.138-23's
device selection, Yuri4Darwin's and willemabc's forks).

## 0. Rules (CLAUDE.md on Windows)

| On the Mac | On Windows |
|---|---|
| personal setup | `C:\xemu-kit\personal`, read-only, for play and smoke tests of the owner's setup; the owner plays on copies in `C:\xemu-kit\play` |
| `../fixtures` | `C:\xemu-kit\fixtures`, read-only, for benchmarks, goldens and savestates (never mix the two sets); reached via `$env:XEMU_TEST_CONFIG`; `SHA256SUMS.txt` is authoritative |
| the pref-dir `xemu.toml` | `%APPDATA%\xemu\xemu\xemu.toml`: always pass `-config_path`; an `xemu.toml` next to an `xemu.exe` under `C:\xemu-kit\bin` is portable mode, never put one there. Its `[automation]` table applies only to that file, so `-config_path`, `XEMU_BASE_PATH`, portable mode and `-name xemu-fork-auto` each disable it (3.15) |
| muted, marked test runs | `[audio] volume_limit = 0` or headless, `-name xemu-fork-auto`, `XEMU_BASE_PATH` = `%LOCALAPPDATA%\xemu-fork-bench`: `Start-Xemu`, the bench and `det_golden.py` do all three |
| commit to `master` | `master` or the owner's branch, never the frozen `windows`; never tag |
| `scripts/bench-det.py` (needs libproc, AF_UNIX, `.app`) | `scripts\win\det_golden.py` |
| `scripts/bench-savestate-ab.sh` | `scripts\bench-savestate-ab-win.py` |
| the monitor socket | `scripts\win\xemu_ctl.py` (QMP, HMP, gdbstub, shared-memory hash) |
| launching a bundle clone | `Start-Xemu` from `scripts\win\kit.ps1` |
| `ninja -C build qemu-system-i386` | `ninja -C build qemu-system-i386w.exe` under MSYS2 MINGW64 |
| APFS clones | plain copies |
| a shell | each Claude Code PowerShell call is a fresh process: dot-source `kit.ps1` in each, keep `Start-Xemu` and `Stop-Xemu` in one call, background anything over 2 min (1.5); `py -3` for every script (MSYS2's python can't run the bench) |
| refuters, load | at most two refuters at once; never `XEMU_X87_REFUTE`, `XEMU_SUBPAGE_FAST_REFUTE` and `XEMU_DSP_JIT_DIFF` together; no fps with `*_REFUTE`, `XEMU_X87_CENSUS`, `XEMU_GUEST_PROF` or `XEMU_MMIO_PROF` set; don't wait for an idle machine, quote the load |

## 1. Setup (P0)

```powershell
# 1.1 [owner] tools and clone
$wg = '--accept-source-agreements', '--accept-package-agreements', '-e'
foreach ($id in 'Git.Git', 'Python.Python.3.12', 'GitHub.cli', '7zip.7zip', 'Microsoft.WinDbg') { winget install @wg --id $id }   # + MSYS2.MSYS2 (7), KhronosGroup.VulkanSDK (3.10a, UAC)
irm https://claude.ai/install.ps1 | iex   # Claude Code in %USERPROFILE%\.local\bin; add it to the user Path if `claude` isn't found
Set-ExecutionPolicy -Scope CurrentUser RemoteSigned -Force; gh auth login   # GitHub.com, HTTPS, authenticate Git = Yes
git config --global core.autocrlf false; git config --global core.longpaths true   # build.sh dies on CRLF checkouts
git config --global user.name 'MichaelJSr'; git config --global user.email 'miky.srouji@gmail.com'
Set-Location $env:USERPROFILE; git clone https://github.com/MichaelJSr/xemu-macos.git; Set-Location xemu-macos
# offline: git clone -b master C:\xemu-kit\repo\xemu-macos.bundle xemu-macos, then in it git remote set-url origin https://github.com/MichaelJSr/xemu-macos.git
git remote add upstream https://github.com/xemu-project/xemu.git; git describe --tags --match 'v*'   # keep spaces out of the path
# 1.2 [agent] the kit: 14 GB plus 25 GB free; layout in WINDOWS-KIT-README.md; robocopy exit 0-7 = success; hashing takes 15 min
robocopy E:\xemu-fork-windows-kit C:\xemu-kit /E /J /R:1 /W:1 /NFL /NDL   # the drive folder's real name
Get-ChildItem C:\xemu-kit -Recurse -Force -File | Where-Object { $_.Name -like '._*' -or $_.Name -eq '.DS_Store' } | Remove-Item -Force
Get-ChildItem C:\xemu-kit -Recurse -File | Unblock-File -ErrorAction SilentlyContinue
$Kit = 'C:\xemu-kit'; $bad = 0; Get-Content -LiteralPath "$Kit\SHA256SUMS.txt" | ForEach-Object { if ($_ -match '^([0-9a-fA-F]{64}) [ *](.+)$') {
  $p = Join-Path $Kit ($Matches[2] -replace '/', '\'); $w = $Matches[1].ToLower()
  if (-not (Test-Path -LiteralPath $p) -or (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.ToLower() -ne $w) { "BAD $p"; $bad++ } } }; "problems: $bad"
Get-ChildItem -LiteralPath "$Kit\personal", "$Kit\fixtures" -Recurse -File | ForEach-Object { $_.IsReadOnly = $true }
# 1.3 [agent] builds: set $Ver to the release under test, here and in kit.ps1
$Kit = 'C:\xemu-kit'; $Ver = 'v0.15.2'; $V = $Ver.TrimStart('v'); $Rel = "$Kit\releases"; $Up = "$Rel\upstream-v0.8.137"
$Arch = if ((Get-CimInstance Win32_Processor | Select-Object -First 1).Architecture -eq 12) { 'arm64' } else { 'x86_64' }
gh release download $Ver --repo MichaelJSr/xemu-macos --dir $Rel --skip-existing --pattern "xemu-$V-windows-$Arch.zip" `
  --pattern "xemu-$V-windows-$Arch-pdb.zip" --pattern "xemu-$V-dbg-windows-$Arch.zip" --pattern "xemu-$V-dbg-windows-$Arch-pdb.zip"
gh release download v0.14 --repo MichaelJSr/xemu-macos --dir $Rel --skip-existing --pattern "xemu-0.14-windows-$Arch.zip"
gh release download v0.8.137 --repo xemu-project/xemu --dir $Up --skip-existing --pattern "xemu-0.8.137-windows-$Arch.zip"
Get-ChildItem $Rel -Recurse -Filter *.zip | Unblock-File
foreach ($z in "xemu-$V-windows-$Arch", "xemu-$V-windows-$Arch-pdb") { Expand-Archive "$Rel\$z.zip" "$Kit\bin\$Ver" -Force }
foreach ($z in "xemu-$V-dbg-windows-$Arch", "xemu-$V-dbg-windows-$Arch-pdb") { Expand-Archive "$Rel\$z.zip" "$Kit\bin\$Ver-dbg" -Force }
Expand-Archive "$Rel\xemu-0.14-windows-$Arch.zip" "$Kit\bin\v0.14" -Force; Expand-Archive "$Up\xemu-0.8.137-windows-$Arch.zip" "$Kit\bin\upstream-0.8.137" -Force
Get-ChildItem "$Kit\bin\*\xemu.exe" | ForEach-Object { '{0}  {1}' -f (Get-FileHash $_ -Algorithm SHA256).Hash.Substring(0, 16), $_.FullName }   # record these
# 1.4 [agent] the two configs (the originals stay untouched) and a helper check
. C:\xemu-kit\tools\kit.ps1; New-Item -ItemType Directory -Force "$Work\shots", "$Kit\play" | Out-Null; $F = "$Kit\fixtures"; $P = "$Kit\personal"
$cf = "$Work\xemu-fixtures.toml"; Copy-Item -LiteralPath "$F\xemu.toml" $cf -Force; (Get-Item $cf).IsReadOnly = $false
Set-TomlKey $cf 'sys.files' 'bootrom_path' (Q "$F\mcpx_1.0.bin"); Set-TomlKey $cf 'sys.files' 'flashrom_path' (Q "$F\Complex_4627v1.03.bin"); Set-TomlKey $cf 'sys.files' 'eeprom_path' (Q "$F\eeprom.bin")
Set-TomlKey $cf 'sys.files' 'hdd_path' (Q "$F\xbox_hdd.qcow2"); Set-TomlKey $cf 'sys.files' 'dvd_path' (Q "$F\Azurik_qol.iso"); Set-TomlKey $cf 'general' 'games_dir' (Q $F)
Set-TomlKey $cf 'general' 'show_welcome' 'false'; Set-TomlKey $cf 'display' 'renderer' "'VULKAN'"; Set-TomlKey $cf 'input' 'auto_bind' 'false'
foreach ($f in 'xbox_hdd.qcow2', 'eeprom.bin') { if (-not (Test-Path -LiteralPath "$Kit\play\$f")) { Copy-Item -LiteralPath "$P\$f" "$Kit\play" } }   # never overwrite play copies
Get-ChildItem -LiteralPath "$Kit\play" -File | ForEach-Object { $_.IsReadOnly = $false }
$bios = Get-ChildItem -LiteralPath "$P\BIOS" -File | Sort-Object { $_.Name -ne 'Complex_4627v1.03.bin' } | Select-Object -First 1
$rom = Get-ChildItem -LiteralPath "$P\Boot ROM image" -File | Sort-Object { $_.Name -ne 'mcpx_1.0.bin' } | Select-Object -First 1
$macDvd = Get-TomlKey "$P\xemu.toml" 'sys.files' 'dvd_path'; $dvd = if ($macDvd) { Join-Path "$P\games" ([IO.Path]::GetFileName($macDvd)) } else { '' }
if (-not $dvd -or -not (Test-Path -LiteralPath $dvd)) { $dvd = "$P\games\Azurik_qol.iso" }
$cp = "$Work\xemu-personal.toml"; Copy-Item -LiteralPath "$P\xemu.toml" $cp -Force; (Get-Item $cp).IsReadOnly = $false
Set-TomlKey $cp 'sys.files' 'bootrom_path' (Q $rom.FullName); Set-TomlKey $cp 'sys.files' 'flashrom_path' (Q $bios.FullName); Set-TomlKey $cp 'sys.files' 'dvd_path' (Q $dvd)
Set-TomlKey $cp 'sys.files' 'eeprom_path' (Q "$Kit\play\eeprom.bin"); Set-TomlKey $cp 'sys.files' 'hdd_path' (Q "$Kit\play\xbox_hdd.qcow2"); Set-TomlKey $cp 'general' 'games_dir' (Q "$P\games")
Set-TomlKey $cp 'display.ui' 'auto_scale' 'true'; Remove-TomlKey $cp 'display.ui' 'scale'; Remove-TomlKey $cp 'display.window' 'startup_size'   # the Mac's Retina scale doesn't fit
foreach ($c in $cf, $cp) {
  Set-TomlKey $c 'general' 'screenshot_dir' (Q "$Work\shots"); Set-TomlKey $c 'general.updates' 'check' 'false'; Set-TomlKey $c 'audio' 'volume_limit' '0'   # raise it in Settings > Audio to play
  Set-TomlKey $c 'display.window' 'fullscreen_on_startup' 'false'; Remove-TomlKey $c 'display.vulkan' 'preferred_physical_device'   # the Mac's GPU; gone since v0.8.138-23
  if ((Get-TomlKey $c 'input.bindings' 'port1') -ne 'keyboard') { Remove-TomlKey $c 'input.bindings' 'port1' } }   # the Mac pad's GUID; bind yours in Settings > Input
Select-String -LiteralPath $cf, $cp -Pattern '_path|games_dir|volume_limit|renderer|port[1-4]|mem_limit'
py -3 $Ctl --help; py -3 $Det --help; py -3 $Bench --dry-run "$Res\bench-dryrun"; py -3 "$Repo\scripts\win\quiet_check.py" --info
py -3 -I "$Repo\scripts\list_snapshots.py" "$Kit\fixtures\xbox_hdd.qcow2"
```

| Step | Pass, and notes |
|---|---|
| 1.1 | first the GPU vendor's current driver and a reboot (on a laptop, plug in and give each `xemu.exe` High performance in Settings > Graphics). No `winget`: update App Installer from the Store or `Add-AppxPackage -RegisterByFamilyName -MainPackage Microsoft.DesktopAppInstaller_8wekyb3d8bbwe`, then a new window. Record OS, CPU (`Win32_Processor.Architecture` 9 = x86_64, 12 = ARM64; hybrid?), RAM, GPUs and drivers, display modes and power plan in `results\machine.json`, with `dxdiag /t` and `vulkaninfo --summary`. Pass: `vulkan-1.dll` in `System32`, no "Microsoft Basic Display Adapter"; every tool prints its version; `gh auth status` logged in |
| 1.2 | `problems: 0`. The fixtures hash to `xbox_hdd.qcow2` 9e31cd901452ed87..., `Azurik_qol.iso` c167168e..., `eeprom.bin` a1784937..., `mcpx_1.0.bin` e99e3a77..., `Complex_4627v1.03.bin` 1de4c87e...; the `xbox_hdd.qcow2` line in `fixtures\README.md` (1c6f030a...) is stale |
| 1.3 | arm64 zips on Windows on ARM. Releases from v0.16.5 have no `-dbg` or `-pdb` zips: download them from the release's CI run (`gh run download <id> -n xemu-windows-<arch>-debug-pdb`, and `-release-pdb`). `-dbg` has asserts: det runs and crash triage, never fps. v0.14 lacks headless mode, `XEMU_BASE_PATH`, `XEMU_LOADVM_ADD_PADS` and the QMP digest commands (4.4 only) |
| 1.4 | every `_path` exists, `volume_limit = 0` in both; the fixture config keeps its keyboard pad (WASD = 26/4/22/7, A = 81, B = 79, Start = 42) and `mem_limit`, the personal one the macOS `mem_limit` (its savestates need it); the helpers print help, the dry run has no traceback; the snapshots include `azurik-heavy-f1`, `azurik-heavy-w2`, `azurik-stream-e2`, `azurik-transition-f1`, `azurik-movie-e2`, `vm-20260704032357` (F5) |
| `kit.ps1` | the kit's `tools\kit.ps1` is `scripts\win\kit.ps1` (`$env:XEMU_KIT`, `$env:XEMU_REPO` override where it looks). It sets `$Kit $Repo $Work $Res $XNew $XNewDbg $XOld $XUp $Ctl $Det $Inj $Score $Bench`, `XEMU_TEST_CONFIG`, `XEMU_BENCH_CACHE`. `New-Scratch [-Name] [-Template]` makes a muted run dir and deletes finished runs' HDD copies; `Start-Xemu -Exe -Dir [-Stub] [-Vars @{}] [-Extra] [-Qmp 4444] [-Mon 4445]` launches marked with QMP and HMP on loopback (`-Stub`: player 1/2 pad stubs; refuses busy ports); `Stop-Xemu` sends `quit`, kills after 30 s; `Stop-AutoXemu` stops marked runs only; `Get-XemuLog` names `stderr.log` (or `xemu.log` from a v0.15.1 or older build with no console); also `Save-Shot`, `Show-Nsprof`, `Q`, `Get-/Set-/Remove-TomlKey`, `Copy-Dist` |
| 1.5 dumps [owner] | elevated: `HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\xemu.exe` with `DumpFolder` = `C:\xemu-kit\results\dumps` (ExpandString), `DumpCount` = 20, `DumpType` = 2 (DWord). `windbg -z <dump> -y <dir of xemu.exe and its PDB>`, `!analyze -v`, `~*k`. Crashes and TDRs: `Get-WinEvent` on the Application log (`Application Error`) and System event 4101. Optional (record it in RESULTS.md): `Add-MpPreference -ExclusionPath 'C:\xemu-kit'` |
| 1.5 memories [owner] | copy `C:\xemu-kit\claude-memory\*.md` to `%USERPROFILE%\.claude\projects\<dir>\memory\`, `<dir>` being the repo path with each non-alphanumeric character turned into `-` (`C:\Users\mike\xemu-macos` gives `C--Users-mike-xemu-macos`; move them if the first `claude` run made another name). Start `claude --add-dir C:\xemu-kit` in the repo; where `windows-session` says to commit to `windows`, section 0 wins. No Git Bash: set `CLAUDE_CODE_GIT_BASH_PATH` to `C:\Program Files\Git\bin\bash.exe` |
| 1.5 long blocks | foreground calls time out after 2 min (10 at most): write the block to `C:\xemu-kit\work\step-<id>.ps1`, run `powershell -NoProfile -ExecutionPolicy Bypass -File <it> *> C:\xemu-kit\results\step-<id>.log` with `run_in_background`, read the log at the end; `Stop-AutoXemu` after an interrupted call. v0.15.1 and older launched with no console write `xemu.log` and leave the bench's `ab_*.log` empty: run the bench from `Start-Process powershell -WindowStyle Minimized -Wait ...` |

## 2. Smoke (P0)

2.1 [owner, 20 min]: `. C:\xemu-kit\tools\kit.ps1; & $XNew -config_path
"$Work\xemu-personal.toml"`.

| Check | Pass when |
|---|---|
| Title bar | names the release under test |
| Dashboard | the HDD dashboard boots with the tray empty (Machine > Eject Disc); `NevolutionX.iso` (Machine > Load Disc...) boots to its menu |
| Games | `Azurik_qol.iso` reaches intro, title and in-game with no black screen, pink tiles or missing HUD; `Azurik - Rise of Perathia (USA).xiso.iso` and `Vexx (USA).xiso.iso` reach their titles |
| Renderer | Vulkan; `Selected physical device:` is the GPU driving the display (on a laptop the discrete one; else set `[display.vulkan] force_physical_device` to the exact name from `Available physical devices`; xemu never writes it) |
| Presenter | `DXGI Presentation Helper successfully initialized`, not `win32_dxgi present failed or unavailable` |
| Audio, pad | music without crackle at a raised volume, the log names the backend; the pad on Port 1 drives Azurik, rumbles and survives a replug |
| Window | View > Fullscreen with `fullscreen_exclusive` on and off, alt-tab without a black screen; crisp auto-scale at 125-200%; resizing in each fit mode |
| Snapshots | thumbnails show; an own snapshot loads (older ones ask for the disc, 3.14) |
| Video overlay | a game that plays FMV through PVIDEO (Midtown Madness 3), if the owner has one, shows its movies on Vulkan |
| 2026-11-24 features | Machine > Game Library..., Game Patches... and Xbox Hard Drive... open; in the last (play HDD), Add files with a non-ASCII file name, then Copy to PC brings it back intact (the `_wfopen` path) and dates show; `display.ui.show_fps` shows FPS (the CPU/GPU load is macOS-only); `display.filtering` fsr, bicubic and lanczos render |
| 2.2 [agent] five cold launches | 5 of 5 alive at 40 s. On a crash: the exit code (`-1073741819` = 0xC0000005, `-1073740791` = 0xC0000409), the dump, the log's last 30 lines; repeat with `$XNewDbg`, then 7.3 |
| 2.3 [agent] fixture savestates | per tag `loaded <tag>`, a PASS shot, draws per flip in band (heavy-f1 576-690, W2 306-323, F5 about 471, stream-e2 about 207), no `vkWaitForFences` warning, the display's GPU selected (else `Set-TomlKey "$Work\xemu-fixtures.toml" 'display.vulkan' 'force_physical_device' (Q '<exact name from Available physical devices>')` and rerun 2.3: every det and bench run copies that template). `Unknown section ... 1.3/usb-hub`: no player-1 stub (`-Stub`, or `XEMU_LOADVM_ADD_PADS=1`) |
| 2.4 [agent] Vulkan, OpenGL | both render heavy-f1; record flips/s. An OpenGL crash is a P0 finding |

```powershell
. C:\xemu-kit\tools\kit.ps1
1..5 | ForEach-Object { $d = New-Scratch -Name "cold$_"; $p = Start-Xemu -Exe $XNew -Dir $d -Stub; Start-Sleep 40   # 2.2
  $alive = -not $p.HasExited; if ($alive) { Save-Shot "$d\boot.png"; Stop-Xemu $p }; "run $_ alive=$alive exit=$($p.ExitCode)" }
foreach ($t in 'azurik-heavy-f1', 'azurik-heavy-w2', 'azurik-stream-e2', 'vm-20260704032357') {   # 2.3
  $d = New-Scratch -Name "load-$t"; $p = Start-Xemu -Exe $XNew -Dir $d -Stub -Vars @{ XEMU_NV2A_NSPROF = '1' }
  py -3 $Ctl wait 4444 180; py -3 $Ctl loadvm 4444 $t; Start-Sleep 25; Save-Shot "$d\$t.png"; py -3 $Ctl qmp 4444 query-xemu-display
  Stop-Xemu $p; Show-Nsprof $d; Select-String -LiteralPath (Get-XemuLog $d) -Pattern 'Selected physical device' }
foreach ($r in 'VULKAN', 'OPENGL') { $d = New-Scratch -Name "renderer-$r"; Set-TomlKey "$d\xemu.toml" 'display' 'renderer' "'$r'"   # 2.4
  $p = Start-Xemu -Exe $XNew -Dir $d -Stub -Vars @{ XEMU_NV2A_NSPROF = '1' }; py -3 $Ctl wait 4444 180; py -3 $Ctl loadvm 4444 azurik-heavy-f1
  Start-Sleep 30; Save-Shot "$d\$r.png"; Stop-Xemu $p; Show-Nsprof $d }
```

## 3. The fork's Windows-specific paths (P0 to 3.6, then P1)

Steps without code below use this pattern; `py -3 $Inj send <pipe>
'axis ly 1.0'` walks and `clear <pipe>` stops.

```powershell
. C:\xemu-kit\tools\kit.ps1
$d = New-Scratch -Name <id>; $p = Start-Xemu -Exe $XNew -Dir $d -Stub -Vars @{ <vars> } -Extra <args>
py -3 $Ctl wait 4444 180; py -3 $Ctl loadvm 4444 <tag>; Start-Sleep <s>; Save-Shot "$d\<id>.png"
Stop-Xemu $p; Select-String -LiteralPath (Get-XemuLog $d) -Pattern '<pass line>'
```

| Step | Launch and drive | Pass |
|---|---|---|
| 3.1 logs | `XEMU_PFIFO_HEARTBEAT=1`, `XEMU_NV2A_NSPROF=1`; heavy-f1 20 s; end with `Stop-Process -Id $p.Id -Force` | `xemu_version:` and `xemu_commit:` on top, the last heartbeat or nsprof line within about 5 s of the kill, no `xemu.log` |
| 3.2 headless | `XEMU_HEADLESS=1`, then `null`; heavy-f1; `(Get-Process -Id $p.Id).MainWindowHandle`; `qmp 4444 screendump "filename=$d\x.png" format=png` | `xemu: headless mode: offscreen` (`null`); handle 0, no taskbar button, focus steal or Volume Mixer entry; the offscreen shot shows the scene, null returns a PNG; the saved `display.renderer` is never `NULL` |
| 3.3 QMP (code) | heavy-f1 under icount, checkpoint, 300 frames, digests, dump, incremental restore, 300 frames | replay same and dump matches True, `verify=true` reports 0 missed pages, every command returns; sha256 of 128 MiB 50-150 ms with SHA instructions (655 ms on Haswell, without) |
| 3.4 input pipe | `XEMU_INPUT_PIPE=xemu-kit-pipe`, another window focused; stream-e2; `$Inj down xemu-kit-pipe W`; `query-xemu-input`; `$Inj send xemu-kit-pipe '+30 axis ly 1.0' '+150 axis ly 0'`; 100 × `$Inj tap xemu-kit-pipe D 20` | `xemu: test-input pipe listening on \\.\pipe\xemu-kit-pipe`; keys has 26, port 2 `ly` > 0; the character walks; both lines `queued` with frames; 0 of 100 writer failures |
| 3.5 shared RAM | `XEMU_RAM_SHM=xemu-kit-shm`, `XEMU_HEADLESS=null`, `-S`; heavy-f1, `xemu-frame-advance frames=60`; `$Ctl shmhash xemu-kit-shm <ram-size>` against `xemu-ram-digest`; a second xemu on the name (`-Qmp 4446 -Mon 4447`); a third after both quit | `xemu: guest RAM in shared memory xemu-kit-shm`, equal hashes; the second says the name is held and keeps private RAM; the third gets the name |
| 3.6 gdbstub | `-Extra '-gdb', 'tcp:127.0.0.1:1234'`; heavy-f1; `$Ctl gdbcheck 1234 8c230 30` (Azurik's player tick) | `stop=T05 ... eip=0x8c230 ... PASS` within seconds; `query-xemu-frame` rises after detach |
| 3.7 goldens (code, 45 min) | `det_golden.py` repeats `bench-det.py`'s recipe: fixtures, player 1 stubbed, muted, `XEMU_HEADLESS=null`, `-S -icount shift=2,align=off,sleep=off`, `loadvm`, `xemu-frame-advance`, the first 16 hex digits of the RAM sha256 | r1 = r2 for every golden, no `error`, the `-dbg` asserts silent. Windows never matches macOS (`scripts/det-goldens.json`: heavy-f1 f300 `6a553d8c7727c6c6` f1200 `037271ebadb297c5`, stream-e2 f600 `1e98a85ee122f87c`, cold f900 `9ffd0e1d2e1f36ea` f1800 `85dec4a26bcb49e9`); gating audit §V has the Windows hashes and the causes. For a divergence run the two `det-pages` lines, bring back `results\det\*.pages` and `*.wav`, and find the frame with `xemu-ram-digest pages=true` on the Mac; the WAV hashes are deterministic under icount and separate an APU divergence from a CPU one |
| 3.8 SSE differential | `det_golden.py --env XEMU_SSE_HOST=2` on heavy-f1 `--hash-at 1200` and the stream-e2 walk; `XEMU_SSE_HOST=2` walking stream-e2 300 s | no abort; alive at 5 min; record the hashes (they differ from the default since `57cfe4fb51`) |
| 3.9 ARM64 only | 2.2, 2.3, 3.7, then the goldens with `--exe $XNewDbg`; `XEMU_SUBPAGE_FAST=1`, `XEMU_SUBPAGE_FAST_REFUTE=1`, `XEMU_SUBPAGE_FAST_STATS=1` walking stream-e2 180 s and transition-f1 60 s; heavy-f1 60 s with `XEMU_APU_PROF=1`, default and `audio.use_dsp_jit = false`; 3.8 | no "a helper without a reload wrote the pinned env field" or "the TLB entry changed under a TLB memo" abort, release hashes; no `VIOLATION`, all `violations=` 0; `apu: utilization` well under 100%; record any `Error: Device does not support required feature <name>`, continue on OpenGL |
| 3.10a validation (code) | the Vulkan SDK's layers, five scenes, 60 s each | zero VUIDs; record each with its `[vk]` message |
| 3.10b query guard | `XEMU_MAX_QUERIES=64`, `XEMU_NV2A_NSPROF=1`; heavy-f1 120 s | renders, no abort; `finish_queries_full` fires, `finish_reports_full` doesn't |
| 3.10c walk-ins | walk transition-f1 forward about 4 s (loads F6), wait 15 s, walk back (F1), three times; walk movie-e2 into its cutscene | no garbled geometry, black freeze, magenta or `vkWaitForFences timeout` |
| 3.10d primitives (code) | offscreen det frames, to compare with the Mac's CPU-emulated primitives | small rasterization differences only; no missing geometry, wrong point sprites or lines |
| 3.11 soak (code, 35 min) | 30 cycles of `loadvm` over five anchors, walking two, 50 s each | 30 of 30, every shot PASS, the frame counter and heartbeat `iters` rising to the end, no fence warnings, errors or asserts; frozen `iters` means PFIFO is stuck (rerun with `XEMU_PFIFO_PUT_LOCK=1`) |
| 3.12 refuters (code) | two pairs, walks and loads; then six incremental restores | no `VIOLATION`; `tlb-wclean refute ... misses=0 unset=0`; `xemu: reval refute: N scoped loads ...` with N of at least 5 and `extra drops 0, unprotected pages 0`. On x86_64 `XEMU_SUBPAGE_FAST=1` prints `xemu: XEMU_SUBPAGE_FAST: no inline store-prefilter emitter for this TCG host backend (aarch64 only); ignored` |
| 3.13a vblank | heavy-f1; `query-xemu-frame` over 30 s focused, covered, minimized (the owner sets each) | 59.9-60.1 in every state |
| 3.13b host load | the 4.3 heavy-f1 baseline (2 × 45 s) idle and under `Start-Job { while ($true) { } }` per core; then with `(Get-Process -Id $p.Id).PriorityClass = 'AboveNormal'` | record all three; a drop over about 10% that the priority recovers argues for more than `f96f00ba9d` (MMCSS, audit batch 17) |
| 3.13c hybrid CPU | nsprof flips/s on heavy-f1, xemu focused against another app focused | record both (EcoQoS can move background windows to E-cores) |
| 3.14 snapshots [owner] | personal setup: save `win-test-1` with `Azurik_qol.iso` in, load it, load a pre-fingerprint snapshot, delete `win-test-1` | `xemu: disc fingerprint: <path>: <n> bytes, xxh3-128 <hex> (<t> s)`, t under about 2 s; no dialog; the old one asks with `... asking` on stderr; thumbnails show |
| 3.15 P2 | a second xemu on `-qmp tcp:127.0.0.1:4444` (via `Start-Process`); `XEMU_ICOUNT_LIVE_PADS=1`, `XEMU_INPUT_RECORD=$d\rec.txt`, `XEMU_INPUT_RECORD_PORT=1`, `-icount shift=2,align=on,sleep=on`, 30 s of play, replay through `det_golden.py -i`; `[automation]` keys only if the owner keeps a play config in the pref dir | the second fails cleanly, the first runs on; the replay reaches the same RAM digest; each set key prints `xemu: [automation] <key> = "...": applied` |

```powershell
. C:\xemu-kit\tools\kit.ps1   # 3.3
$d = New-Scratch -Name qmp; $p = Start-Xemu -Exe $XNew -Dir $d -Stub -Vars @{ XEMU_HEADLESS = 'null' } -Extra '-S', '-icount', 'shift=2,align=off,sleep=off'
py -3 $Ctl wait 4444 180; py -3 $Ctl loadvm 4444 azurik-heavy-f1; py -3 $Ctl qmp 4444 xemu-checkpoint-save name=base; py -3 $Ctl qmp 4444 xemu-frame-advance frames=300
$a = py -3 $Ctl qmp 4444 xemu-ram-digest | ConvertFrom-Json; py -3 $Ctl qmp 4444 xemu-ram-digest algorithm=xxh3-128 pages=true | ConvertFrom-Json | Select-Object frame, 'ram-size', 'elapsed-us'
py -3 $Ctl qmp 4444 xemu-dump-ram "filename=$d\ram.bin"; $h = (Get-FileHash "$d\ram.bin" -Algorithm SHA256).Hash.ToLower()
py -3 $Ctl qmp 4444 xemu-checkpoint-restore name=base incremental=true verify=true; py -3 $Ctl qmp 4444 xemu-frame-advance frames=300
$b = py -3 $Ctl qmp 4444 xemu-ram-digest | ConvertFrom-Json; py -3 $Ctl qmp 4444 query-xemu-display; py -3 $Ctl qmp 4444 query-xemu-input; Stop-Xemu $p
'f300 {0}; replay same = {1}; dump matches = {2}; {3} us' -f $a.sha256.Substring(0, 16), ($a.sha256 -eq $b.sha256), ($h -eq $a.sha256), $a.'elapsed-us'
. C:\xemu-kit\tools\kit.ps1; $out = "$Res\det-goldens.jsonl"   # 3.7
$cfgs = @{ default = @(); interp = @('--set', 'audio.use_dsp_jit=false'); 'interp-softfpu' = @('--set', 'audio.use_dsp_jit=false', '--set', 'perf.hard_fpu=false') }
foreach ($c in $cfgs.Keys) { foreach ($rep in 1, 2) { $l = "$Ver $c r$rep"
  py -3 $Det --exe $XNew --label $l @($cfgs[$c]) --tag azurik-heavy-f1 --hash-at 300,1200 --out $out
  py -3 $Det --exe $XNew --label $l @($cfgs[$c]) --tag azurik-stream-e2 --hash-at 600 -i 'axis ly 1.0' --out $out
  py -3 $Det --exe $XNew --label $l @($cfgs[$c]) --tag - --hash-at 900,1800 --out $out } }
py -3 $Det --exe $XNewDbg --label dbg --tag azurik-heavy-f1 --hash-at 300,1200 --out $out
Get-Content $out | ConvertFrom-Json | Format-Table label, tag, hashes, fps, mcyc_per_frame, error -AutoSize
py -3 $Det --exe $XNew --label 'pages interp' --set audio.use_dsp_jit=false --tag azurik-heavy-f1 --hash-at 1,2,5,10,30,100,300 --pages --wav --out "$Res\det-pages.jsonl"   # divergence only
py -3 $Det --exe $XNew --label 'wav default' --tag azurik-heavy-f1 --hash-at 300 --wav --out "$Res\det-pages.jsonl"
. C:\xemu-kit\tools\kit.ps1; $d = New-Scratch -Name validation   # 3.10a and 3.10d
Set-TomlKey "$d\xemu.toml" 'display.vulkan' 'validation_layers' 'true'; Set-TomlKey "$d\xemu.toml" 'display.vulkan' 'assert_on_validation_msg' 'false'
$p = Start-Xemu -Exe $XNew -Dir $d -Stub -Vars @{ XEMU_INPUT_PIPE = 'xemu-kit-val' }; py -3 $Ctl wait 4444 300
foreach ($t in 'azurik-heavy-f1', 'azurik-heavy-w2', 'azurik-stream-e2', 'azurik-transition-f1', 'azurik-movie-e2') {
  py -3 $Ctl loadvm 4444 $t 180; if ($t -notlike 'azurik-heavy-*') { py -3 $Inj send xemu-kit-val 'axis ly 1.0' }; Start-Sleep 60; py -3 $Inj clear xemu-kit-val; Save-Shot "$d\$t.png" }
Stop-Xemu $p; Select-String -LiteralPath (Get-XemuLog $d) -Pattern 'Validation layers enabled|validation layers not available'
Select-String -LiteralPath (Get-XemuLog $d) -Pattern 'VUID-[A-Za-z0-9_-]+' -AllMatches | ForEach-Object { $_.Matches.Value } | Group-Object | Sort-Object Count -Descending
foreach ($t in 'azurik-heavy-f1', 'azurik-heavy-w2') { py -3 $Det --exe $XNew --label shots --renderer offscreen --shots --tag $t --hash-at 60,300 --out "$Res\det-shots.jsonl" }
py -3 $Det --exe $XNew --label shots --renderer offscreen --shots --tag azurik-stream-e2 --hash-at 600 -i 'axis ly 1.0' --out "$Res\det-shots.jsonl"   # bring back results\det\*.png
. C:\xemu-kit\tools\kit.ps1; $log = "$Res\soak-$(Get-Date -Format yyyyMMdd-HHmm).txt"   # 3.11, nobody at the machine
$d = New-Scratch -Name soak; $p = Start-Xemu -Exe $XNew -Dir $d -Stub -Vars @{ XEMU_PFIFO_HEARTBEAT = '1'; XEMU_NV2A_NSPROF = '1'; XEMU_INPUT_PIPE = 'xemu-kit-soak' }
py -3 $Ctl wait 4444 180; $tags = 'azurik-heavy-f1', 'azurik-heavy-w2', 'azurik-stream-e2', 'vm-20260704032357', 'azurik-transition-f1'
for ($i = 0; $i -lt 30; $i++) { $t = $tags[$i % 5]; if ($p.HasExited) { "DIED before $i, exit $($p.ExitCode)" | Tee-Object -Append $log; break }
  $r = py -3 $Ctl loadvm 4444 $t 120; if ($t -in 'azurik-stream-e2', 'azurik-transition-f1') { py -3 $Inj down xemu-kit-soak W | Out-Null }
  Start-Sleep 50; py -3 $Inj clear xemu-kit-soak | Out-Null; $s = Save-Shot "$d\c$i.png"
  "$i $t frame=$((py -3 $Ctl qmp 4444 query-xemu-frame | ConvertFrom-Json).frame) | $r | $s" | Tee-Object -Append $log }
Stop-Xemu $p; Select-String -LiteralPath (Get-XemuLog $d) -Pattern 'vkWaitForFences|rror|assert|heartbeat' | Select-Object -Last 20 | Tee-Object -Append $log
. C:\xemu-kit\tools\kit.ps1   # 3.12
foreach ($v in @(@{ XEMU_SUBPAGE_REFUTE = '1'; XEMU_XPAGE_REFUTE = '1'; XEMU_XPAGE_PROF = '1' }, @{ XEMU_TLB_WCLEAN_REFUTE = '1'; XEMU_TB_REVAL_REFUTE = '1' })) {
  $d = New-Scratch -Name refute; $v['XEMU_INPUT_PIPE'] = 'xemu-kit-ref'; $p = Start-Xemu -Exe $XNew -Dir $d -Stub -Vars $v; py -3 $Ctl wait 4444 180
  foreach ($t in 'azurik-stream-e2', 'azurik-transition-f1', 'azurik-heavy-f1', 'azurik-stream-e2') { py -3 $Ctl loadvm 4444 $t 120; py -3 $Inj send xemu-kit-ref 'axis ly 1.0'; Start-Sleep 60; py -3 $Inj clear xemu-kit-ref }
  if ($v.ContainsKey('XEMU_TB_REVAL_REFUTE')) { py -3 $Ctl qmp 4444 xemu-checkpoint-save name=reval   # the first restore takes the full walk
    1..6 | ForEach-Object { py -3 $Inj send xemu-kit-ref 'axis ly 1.0'; Start-Sleep 10; py -3 $Inj clear xemu-kit-ref; py -3 $Ctl qmp 4444 xemu-checkpoint-restore name=reval incremental=true } }
  Stop-Xemu $p; Select-String -LiteralPath (Get-XemuLog $d) -Pattern 'refute|VIOLATION|XPAGE summary|subpage' | Select-Object -Last 12 }
```

## 4. Performance (P1, nobody at the machine)

```powershell
. C:\xemu-kit\tools\kit.ps1; $env:XEMU_LOADVM_ADD_PADS = '1'   # the azurik-* tags need player 1
py -3 $Bench azurik-heavy-f1 XEMU_NOOP_AB 2 45 "$Res\bench-aa-f1" --exe $XNew   # 4.2
foreach ($t in 'azurik-heavy-f1', 'azurik-heavy-w2', 'vm-20260704032357') { py -3 $Bench $t XEMU_NOOP_AB 3 60 "$Res\bench-base-$t" --exe $XNew }   # 4.3
py -3 $Bench vm-20260704032357 XEMU_NOOP_AB 3 60 "$Res\bench-v014-F5" --b-exe $XOld --e-exe $XNew   # 4.4
foreach ($s in 'XEMU_BQL_SPIN', 'XEMU_KI_IDLE_HALT', 'XEMU_PFIFO_STALL_DOORBELL') { py -3 $Bench azurik-heavy-f1 $s 3 60 "$Res\bench-$s" --exe $XNew }   # 4.5: B = 0, E = 1
$o = "$Res\det-ab-hardfpu.jsonl"; 1..4 | ForEach-Object { py -3 $Det --exe $XNew --label base --hash-at 1200 --out $o; py -3 $Det --exe $XNew --label softfpu --hash-at 1200 --set perf.hard_fpu=false --out $o }   # 4.6
Get-Content $o | ConvertFrom-Json | Group-Object label | ForEach-Object { '{0}: n={1} mcyc={2:N2} fps={3:N1} hashes={4}' -f $_.Name, $_.Count,
  ($_.Group.mcyc_per_frame | Measure-Object -Average).Average, ($_.Group.fps | Measure-Object -Average).Average, (($_.Group.hashes | Select-Object -Unique) -join ' | ') }
```

| Step | Pass or record |
|---|---|
| 4.1 rules | `scripts\bench-savestate-ab-win.py` runs windowed Vulkan with 5 s nsprof intervals, interleaved B/E pairs, a draws-per-flip gate, a mid-run shot and a receipt. Never compare absolute fps across hosts. Keep the window visible on the primary screen and not fullscreen (captures come out black), High performance power; copy the printed load into RESULTS.md. `XEMU_NOOP_AB` is read by nothing (the A/A and cross-binary dummy). At the 60 fps cap compare CPU% (`ab_*.cpu`), draws/s and 4.6 instead. Bring back each receipt, `meta.json` and `receipt.json` |
| 4.2 A/A | a receipt, no DEAD or INVALID run (INVALID with empty logs: the console problem in 1.5), arms within the spread |
| 4.3 baselines | per anchor: fps mean ± sd over 6 runs, draws/flip, passes/flip, fence_wait ms/flip, CPU%, capped or not; compare with gating audit §V |
| 4.4 v0.14 | the bench isolates both caches and refuses the delta when draws per flip differ by over 15%; heavy-f1 needs a real pad: copy the personal `input.bindings` `port1` into a copy of `$env:XEMU_TEST_CONFIG` and pass `--config-template` |
| 4.5 switches | each default (E) no slower than B within noise, CPU% no worse. DSP engine: the 4.3 heavy-f1 baseline with `$env:XEMU_APU_PROF = '1'` on the default template and on one with `audio.use_dsp_jit = false`; compare fps, CPU%, `apu: utilization` |
| 4.6 det metric | `det_golden.py` Mcyc/frame comes from `QueryProcessCycleTime` (TSC rate: CPU time, not core clocks; `--pmc` adds instructions and core cycles; the 4.5 switches do nothing under icount). One hash per arm (softfpu's differs by design); quote the delta and the sign tally |
| 4.7 upstream [owner] P2 | v0.8.137 most likely can't load the fork's savestates: record the error, then compare menu-bar fps at one spot of `Azurik_qol.iso` (`New-Scratch -Template "$Work\xemu-personal.toml"`, `Start-Xemu -Exe $XUp`) |
| 4.8 local builds P2 | CI release against a native build of one commit, native `-O2` (`-Doptimization=2 -Dstack_protector=auto`) against `-O3`, native against GCC PGO: `py -3 $Bench azurik-heavy-f1 XEMU_NOOP_AB 4 75 <out> --b-exe <B> --e-exe <E>` plus 4.6 with `--exe` per arm |

## 5. Roswell tests on a real GPU (P2)

`ci.yml` runs the fork's Roswell workflow
(`MichaelJSr/xemu-test/.github/workflows/test-roswell.yml`, pinned by
commit; tests from `MichaelJSr/xemu-test` branch `xemu-macos`). Windows
(Mesa llvmpipe) has a default leg (`--no-hard-fpu`,
`failure_filters/windows_roswell_softfpu.json`) and a hard-FPU leg
(`failure_filters/windows_roswell_hardfpu.json`); artifacts expire
after 1 day. Pass: the pass/fail set of `results-windows-latest-default`
(`-hard-fpu` for a run without `--no-hard-fpu`, optional, as is `$XUp`
as a control); a real-GPU difference either way is a finding. Also on
Vulkan, default and with `$env:XEMU_VK_CLEAR_IN_PASS = '0'`: the same
set, FPU and MMX text frames near 2 render passes a flip (about 1700
and 2471 with `0`).

```powershell
. C:\xemu-kit\tools\kit.ps1; Set-Location C:\; git clone --recurse-submodules -b xemu-macos https://github.com/MichaelJSr/xemu-test.git; Set-Location C:\xemu-test
py -3.12 -m venv .venv; .\.venv\Scripts\Activate.ps1; pip install git+https://github.com/abaire/xemu-pgraph-ci-tools.git; pip install .
$run = gh run list --repo MichaelJSr/xemu-macos --branch master --workflow CI --limit 1 --json databaseId --jq '.[0].databaseId'
gh run download $run --repo MichaelJSr/xemu-macos -n test-data -D xemutest\data; gh run download $run --repo MichaelJSr/xemu-macos -n roswell-bios -D private
Move-Item private\roswell.bin private\bios.bin -Force; New-Item -ItemType Directory -Force results | Out-Null
python -m xemutest --no-mcpx --sysram 128 --no-hard-fpu --failure-filter failure_filters/windows_roswell_softfpu.json --ci-summary-omit-innermost-passed-subtests -v $XNew private results; deactivate
```

## 6. Results and commits (P0)

| Item | Do |
|---|---|
| RESULTS.md | LF endings. Sections: Machine; Builds (path, sha256[:16]); Setup notes; P0 smoke; Windows paths (id, result, evidence); Goldens (build, config, hashes, r1 = r2, Mcyc/frame, fps); Performance (batch, anchor, arms, B/E fps, delta, signs, draws/flip, fence ms/flip, CPU%, load, verdict); Soak and refuters; Crashes (exit code, time, dump, stack top); Findings (severity, symptom, repro, area, evidence); Commits |
| Bring back | zip `results\*` (dumps apart) with the interesting run logs, then delete the run dirs in `work` except `shots` |
| Commits | one logical change, `<subsystem>: <short description>` (`nv2a/vk:`, `ui:`, `scripts/win:`, `tcg:`, `target/i386:`, `build:`, `docs:`), a why-what-numbers body, the harness's `Co-Authored-By:` trailer, docs in the same commit, platform-only code in `#ifdef _WIN32`. Harness fixes (`scripts/win/*`, bench-win) are their own `scripts/win:` commits. Measurements alone: one commit that updates `windows-gating-audit.md` (§T statuses, §W, and the run's machine, defects and baselines as a new dated section after §W) and the roadmap's real-hardware item |
| CI builds | `git push origin HEAD`, then `gh run list --repo MichaelJSr/xemu-macos --branch (git branch --show-current) --limit 3`; every pushed branch builds; `gh run download <id> --repo MichaelJSr/xemu-macos -n xemu-windows-x86_64-release -D C:\xemu-kit\bin\ci-<sha>` (`-arm64-` on ARM) tests a fix without a toolchain |

## 7. Local builds (P2)

```powershell
# 7.1 native MSYS2 MINGW64, about 10 min cold on a 4-core Haswell; the meson subprojects need network
. C:\xemu-kit\tools\kit.ps1; $msys = 'C:\msys64\usr\bin\bash.exe'
& $msys -lc 'pacman --noconfirm -Syuu'; & $msys -lc 'pacman --noconfirm -Syuu'
& $msys -lc 'pacman -S --needed --noconfirm --disable-download-timeout git make patch diffutils mingw-w64-x86_64-toolchain mingw-w64-x86_64-clang mingw-w64-x86_64-lld mingw-w64-x86_64-compiler-rt mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja mingw-w64-x86_64-pkgconf mingw-w64-x86_64-python mingw-w64-x86_64-python-yaml mingw-w64-x86_64-python-requests mingw-w64-x86_64-ccache mingw-w64-x86_64-glib2 mingw-w64-x86_64-pixman mingw-w64-x86_64-libepoxy mingw-w64-x86_64-sdl3 mingw-w64-x86_64-libslirp mingw-w64-x86_64-vulkan-headers mingw-w64-x86_64-vulkan-loader mingw-w64-x86_64-vulkan-validation-layers mingw-w64-x86_64-zlib mingw-w64-x86_64-zstd mingw-w64-x86_64-libusb mingw-w64-x86_64-curl mingw-w64-x86_64-openssl mingw-w64-x86_64-gdb'
$env:MSYSTEM = 'MINGW64'; $env:CHERE_INVOKING = '1'; Set-Location $Repo
& $msys -lc './build.sh 2>&1 | tail -n 30'; Copy-Dist local-O3   # -O3, no stack protector, -Dx86_version=3
& $msys -lc 'grep -c -- " -O3 " build/compile_commands.json; grep -c fstack-protector build/compile_commands.json'
& $msys -lc 'ninja -C build qemu-system-i386w.exe && cp build/qemu-system-i386w.exe dist/xemu.exe'   # incremental
# 7.2 CI's exact x86_64 binary (.github/workflows/build-windows.yml: a Linux-container cross build, then cv2pdb on Windows)
winget install --accept-source-agreements --accept-package-agreements -e --id Docker.DockerDesktop   # start Docker Desktop once
git clone -b master https://github.com/MichaelJSr/xemu-macos.git C:\xemu-cross
docker run --rm -v 'C:\xemu-cross:/src' -w /src -e CROSSPREFIX=x86_64-w64-mingw32.static- -e CROSSAR=x86_64-w64-mingw32.static-gcc-ar ghcr.io/xemu-project/xemu-win64-toolchain-gcc:sha-2881edd bash -c './build.sh -p win64-cross -Db_lto=true -Dx86_version=3'
# arm64: ghcr.io/xemu-project/xemu-win64-toolchain:sha-0d06ce8, CROSSPREFIX=aarch64-w64-mingw32.static-, CROSSAR=aarch64-w64-mingw32.static-ar, no options
Invoke-WebRequest https://github.com/rainers/cv2pdb/releases/download/v0.54/cv2pdb-0.54.zip -OutFile C:\xemu-cross\cv2pdb.zip
Expand-Archive C:\xemu-cross\cv2pdb.zip C:\xemu-cross\cv2pdb -Force; C:\xemu-cross\cv2pdb\cv2pdb64.exe C:\xemu-cross\dist\xemu.exe   # writes xemu.pdb, strips the exe
# 7.3 exit-crash protocol, only if 2.2 or a native build crashed
. C:\xemu-kit\tools\kit.ps1; $msys = 'C:\msys64\usr\bin\bash.exe'; $env:MSYSTEM = 'MINGW64'; $env:CHERE_INVOKING = '1'; Set-Location $Repo; git rev-parse HEAD
& $msys -lc 'rm -rf build && ./build.sh -Doptimization=2 -Dstack_protector=auto 2>&1 | tail -n 3'; Copy-Dist local-O2   # arm A
& $msys -lc 'rm -rf build && ./build.sh 2>&1 | tail -n 3'; Copy-Dist local-O3   # arm B
foreach ($arm in 'local-O2', 'local-O3') { 1..6 | ForEach-Object { $d = New-Scratch -Name "$arm-$_"; $p = Start-Xemu -Exe "$Kit\bin\$arm\xemu.exe" -Dir $d -Stub
  Start-Sleep 40; $alive = -not $p.HasExited; if ($alive) { Stop-Xemu $p }; "$arm run $_ alive=$alive exit=$($p.ExitCode)" } }
# 7.4 GCC PGO: replays pgo-train.py's corpus (macOS-only); every session must end in a QMP quit to write the counters
. C:\xemu-kit\tools\kit.ps1; $msys = 'C:\msys64\usr\bin\bash.exe'; $env:MSYSTEM = 'MINGW64'; $env:CHERE_INVOKING = '1'; Set-Location $Repo
& $msys -lc 'rm -rf build pgo && XEMU_PGO=generate ./build.sh 2>&1 | tail -n 5'; $G = Copy-Dist local-pgogen
$d = New-Scratch -Name pgo; $p = Start-Xemu -Exe $G -Dir $d -Stub; Start-Sleep 90; Stop-Xemu $p   # boot to the intro
foreach ($t in 'azurik-heavy-f1', 'azurik-heavy-f1', 'azurik-heavy-w2', 'azurik-heavy-w2', 'vm-20260704032357') {
  $p = Start-Xemu -Exe $G -Dir $d -Stub; py -3 $Ctl wait 4444 300; py -3 $Ctl loadvm 4444 $t 180; Start-Sleep 80; Stop-Xemu $p }
$p = Start-Xemu -Exe $G -Dir $d -Stub -Vars @{ XEMU_INPUT_PIPE = 'xemu-kit-pgo' }
py -3 $Ctl wait 4444 300; py -3 $Ctl loadvm 4444 azurik-stream-e2 180; py -3 $Inj send xemu-kit-pgo 'axis ly 1.0'; Start-Sleep 60; Stop-Xemu $p
py -3 $Det --exe $G --label pgo-train --hash-at 3000; py -3 $Det --exe $G --label pgo-train --tag azurik-stream-e2 --hash-at 1200 -i 'axis ly 1.0'
& $msys -lc 'find pgo -name "*.gcda" | wc -l; XEMU_PGO=use ./build.sh 2>&1 | tail -n 8'; Copy-Dist local-pgouse   # A/B it against local-O3 (4.8)
```

| Step | Notes |
|---|---|
| 7.1 | `build.sh` writes `dist\xemu.exe` with its DLLs (`get_deps.py`) and links SDL3 statically from the meson wrap with the fork's `subprojects/packagefiles` patches, as CI does (MSYS2's SDL3 package is not used). CI's x86_64 release adds GCC LTO (`-Db_lto=true` plus `-flto-incremental`); natively `./build.sh -Db_lto=true` is slow, so use it only when the build itself is under test. Export `USERPROFILE` when another tool drives bash. On ARM64 use the release zips (native CLANGARM64 is untested) |
| 7.3 | decide first: A clean and B crashing convicts `-O3` (make `-O2` the default again, hunt the bug in the dump); both crashing clears the flags (bisect whole commits, 5+ runs each, never single runs). Six clean runs at a 5-in-6 failure rate give P < 1e-4; a `vmovaps` or `vmovdqa` fault on an unaligned operand is the `-O3` vectorizer class |
| 7.4 | GCC builds use `.gcda` profiles (the committed `pgo/default.profdata` is clang's). First run 2026-10-18: 1187 profiles, det heavy-f1 core cycles -3.1%, instructions -3.9%, goldens unchanged. Don't commit `.gcda` files: CI's cross GCC can't read them (a CI profile needs a CI-toolchain `generate` binary trained here with `GCOV_PREFIX`) |

## Quick triage

| Symptom | First check | Next step |
|---|---|---|
| Vulkan init fails or falls back to OpenGL | `Error: Device does not support required feature ...`, `Failed to find a suitable GPU`, `No supported Vulkan device found for the GPU driving the display. To override, set display.vulkan.force_physical_device.`, `Forced device "<name>" not found or unsupported` | record it; update the driver; set or fix `force_physical_device`; continue on OpenGL |
| Black screen with audio | `[vk]` lines; `win32_dxgi present failed` | 2.4 on OpenGL; 3.10a |
| Crash | exit code, `results\dumps`, the Application log | WinDbg with the PDB; repeat with `$XNewDbg` |
| Freeze | heartbeat `iters` frozen? TDR event 4101? | `XEMU_PFIFO_PUT_LOCK=1`; the fence warning names its wait site |
| `loadvm`: `Unknown section ...usb-hub` | no player-1 pad | `-Stub` or `XEMU_LOADVM_ADD_PADS=1` |
| Bench runs INVALID | `ab_*.log` empty, `xemu.log` present? | give the bench a console (1.5) |
| Pipe `cannot open` | `XEMU_INPUT_PIPE` set on that launch? the keyboard bound to port 2? | `query-xemu-input`; grep the log for `test-input pipe` |
| `port 4444 or 4445 already in use` | a leftover xemu | `Stop-AutoXemu` |
