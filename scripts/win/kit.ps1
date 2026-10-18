# kit.ps1 - session setup for the xemu-macos Windows test kit.
# Dot-source it at the start of every PowerShell session:  . C:\xemu-kit\tools\kit.ps1
# Reload PATH from the registry, so tools installed after this shell (or Claude Code) started are found.
$env:Path = [Environment]::GetEnvironmentVariable('Path','Machine') + ';' + [Environment]::GetEnvironmentVariable('Path','User')
$Ver     = 'v0.15.1'          # fork release under test (set a newer one if it exists)
# The repo and the kit: $env:XEMU_REPO / $env:XEMU_KIT if set, else found from this file's place
# (the kit's tools\kit.ps1 or the repo's scripts\win\kit.ps1) or as siblings of each other,
# else %USERPROFILE%\xemu-macos and C:\xemu-kit.
function Find-Dir([string[]]$Dirs, [string]$Marker) {
  foreach ($d in $Dirs) { if ($d -and (Test-Path -LiteralPath (Join-Path $d $Marker))) { return $d } }
}
$here    = Split-Path -Parent $PSScriptRoot
$Repo    = Find-Dir @($env:XEMU_REPO, (Split-Path -Parent $here), (Join-Path (Split-Path -Parent $here) 'xemu-macos'),
                      (Join-Path $env:USERPROFILE 'xemu-macos')) 'scripts\win\kit.ps1'
if (-not $Repo) { $Repo = Join-Path $env:USERPROFILE 'xemu-macos' }
$Kit     = Find-Dir @($env:XEMU_KIT, $here, (Join-Path (Split-Path -Parent $Repo) 'xemu-kit'), 'C:\xemu-kit') 'SHA256SUMS.txt'
if (-not $Kit) { $Kit = 'C:\xemu-kit' }
$Work    = Join-Path $Kit 'work'
$Res     = Join-Path $Kit 'results'
$Ctl     = Join-Path $Repo 'scripts\win\xemu_ctl.py'
$Det     = Join-Path $Repo 'scripts\win\det_golden.py'
$Inj     = Join-Path $Repo 'scripts\win\inject_input.py'
$Score   = Join-Path $Repo 'scripts\win\score_shot_win.py'
$Bench   = Join-Path $Repo 'scripts\bench-savestate-ab-win.py'
$XNew    = Join-Path $Kit "bin\$Ver\xemu.exe"
$XNewDbg = Join-Path $Kit "bin\$Ver-dbg\xemu.exe"
$XOld    = Join-Path $Kit 'bin\v0.14\xemu.exe'
$XUp     = Join-Path $Kit 'bin\upstream-0.8.137\xemu.exe'
$env:XEMU_KIT         = $Kit
$env:XEMU_TEST_CONFIG = Join-Path $Work 'xemu-fixtures.toml'
$env:XEMU_BENCH_CACHE = Join-Path $env:LOCALAPPDATA 'xemu-fork-bench'
New-Item -ItemType Directory -Force $Work, $Res, $env:XEMU_BENCH_CACHE | Out-Null

function Q([string]$s) { "'" + $s + "'" }    # TOML literal string: backslashes stay as they are

function Get-TomlKey([string]$Path, [string]$Section, [string]$Key) {
  $cur = ''
  foreach ($l in [IO.File]::ReadAllLines($Path)) {
    if ($l -match '^\s*\[([^\]]+)\]\s*$') { $cur = $Matches[1].Trim(); continue }
    if ($cur -eq $Section -and $l -match ('^\s*' + [regex]::Escape($Key) + '\s*=\s*(.*?)\s*$')) {
      return $Matches[1].Trim([char]39, [char]34)
    }
  }
  return $null
}

function Set-TomlKey([string]$Path, [string]$Section, [string]$Key, [string]$Value) {
  $lines = [System.Collections.Generic.List[string]]::new()
  $lines.AddRange([string[]][IO.File]::ReadAllLines($Path))
  $cur = ''; $start = -1; $end = -1
  for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match '^\s*\[([^\]]+)\]\s*$') {
      if ($start -ge 0 -and $end -lt 0) { $end = $i }
      $cur = $Matches[1].Trim()
      if ($cur -eq $Section) { $start = $i; $end = -1 }
      continue
    }
    if ($cur -eq $Section -and $lines[$i] -match ('^\s*' + [regex]::Escape($Key) + '\s*=')) {
      $lines[$i] = "$Key = $Value"
      [IO.File]::WriteAllLines($Path, $lines.ToArray()); return
    }
  }
  if ($start -lt 0) { $lines.Add(''); $lines.Add("[$Section]"); $lines.Add("$Key = $Value") }
  else { if ($end -lt 0) { $end = $lines.Count }; $lines.Insert($end, "$Key = $Value") }
  [IO.File]::WriteAllLines($Path, $lines.ToArray())
}

function Remove-TomlKey([string]$Path, [string]$Section, [string]$Key) {
  $cur = ''
  $out = foreach ($l in [IO.File]::ReadAllLines($Path)) {
    if ($l -match '^\s*\[([^\]]+)\]\s*$') { $cur = $Matches[1].Trim(); $l; continue }
    if ($cur -eq $Section -and $l -match ('^\s*' + [regex]::Escape($Key) + '\s*=')) { continue }
    $l
  }
  [IO.File]::WriteAllLines($Path, [string[]]$out)
}

# A run dir with copies of the template's HDD and EEPROM and a muted scratch config.
# It first deletes the HDD copy (about 2.2 GB) of every earlier run dir under $Work whose
# xemu has exited and that is more than 2 min old; logs, configs and screenshots stay.
function New-Scratch([string]$Name = 'run', [string]$Template = $env:XEMU_TEST_CONFIG) {
  $live = @(Get-CimInstance Win32_Process -Filter "Name='xemu.exe'" | ForEach-Object { $_.CommandLine })
  $old = (Get-Date).AddMinutes(-2)
  Get-ChildItem -LiteralPath $Work -Directory | ForEach-Object {
    $h = Join-Path $_.FullName 'hdd.qcow2'
    if ((Test-Path -LiteralPath $h) -and (Get-Item -LiteralPath $h).LastWriteTime -lt $old -and
        -not ($live -like "*$($_.FullName)\*")) { Remove-Item -LiteralPath $h -Force }
  }
  $d = Join-Path $Work ('{0}-{1:MMdd-HHmmss}' -f $Name, (Get-Date))
  New-Item -ItemType Directory -Force $d | Out-Null
  Copy-Item -LiteralPath (Get-TomlKey $Template 'sys.files' 'hdd_path') (Join-Path $d 'hdd.qcow2')
  Copy-Item -LiteralPath (Get-TomlKey $Template 'sys.files' 'eeprom_path') (Join-Path $d 'eeprom.bin')
  Copy-Item -LiteralPath $Template (Join-Path $d 'xemu.toml')
  Get-ChildItem -LiteralPath $d -File | ForEach-Object { $_.IsReadOnly = $false }   # copies of the read-only fixtures are read-only
  (Get-Item -LiteralPath (Join-Path $d 'hdd.qcow2')).LastWriteTime = Get-Date   # the 2-minute guard above must see a fresh copy
  $cfg = Join-Path $d 'xemu.toml'
  Set-TomlKey $cfg 'sys.files' 'hdd_path' (Q (Join-Path $d 'hdd.qcow2'))
  Set-TomlKey $cfg 'sys.files' 'eeprom_path' (Q (Join-Path $d 'eeprom.bin'))
  Set-TomlKey $cfg 'general' 'screenshot_dir' (Q $d)
  Set-TomlKey $cfg 'audio' 'volume_limit' '0'
  Set-TomlKey $cfg 'display.window' 'fullscreen_on_startup' 'false'
  return $d
}

# Launch a marked, muted xemu on <Dir>\xemu.toml with QMP and HMP on loopback.
# -Stub runs scripts\harness-config.py (mute, auto_bind off, player-1/2 pad stubs).
function Start-Xemu([string]$Exe, [string]$Dir, [hashtable]$Vars = @{}, [string[]]$Extra = @(),
                    [int]$Qmp = 4444, [int]$Mon = 4445, [switch]$Stub) {
  # A leftover xemu on these ports would make the new one exit and xemu_ctl drive the old one.
  if (Get-NetTCPConnection -LocalPort $Qmp, $Mon -State Listen -ErrorAction SilentlyContinue) {
    throw "port $Qmp or $Mon already in use (leftover xemu?); run Stop-AutoXemu"
  }
  $cfg = Join-Path $Dir 'xemu.toml'
  $argv = @('-name', 'xemu-fork-auto', '-config_path', $cfg,
            '-qmp', "tcp:127.0.0.1:$Qmp,server=on,wait=off",
            '-monitor', "tcp:127.0.0.1:$Mon,server,nowait")
  if ($Stub) { $argv += @(py -3 -I (Join-Path $Repo 'scripts\harness-config.py') $cfg) }
  $argv += $Extra
  $set = @{ XEMU_BASE_PATH = $env:XEMU_BENCH_CACHE }
  foreach ($k in $Vars.Keys) { $set[$k] = [string]$Vars[$k] }
  $saved = @{}
  foreach ($k in $set.Keys) {
    $saved[$k] = [Environment]::GetEnvironmentVariable($k, 'Process')
    [Environment]::SetEnvironmentVariable($k, $set[$k], 'Process')
  }
  try {
    $proc = Start-Process -FilePath $Exe -ArgumentList $argv -WorkingDirectory $Dir -PassThru `
      -RedirectStandardOutput (Join-Path $Dir 'stdout.log') -RedirectStandardError (Join-Path $Dir 'stderr.log')
    $null = $proc.Handle   # keep a handle open so ExitCode survives the exit
    $proc
  } finally {
    foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k], 'Process') }
  }
}

function Stop-Xemu([System.Diagnostics.Process]$P, [int]$Qmp = 4444) {
  if (-not $P.HasExited) { py -3 $Ctl qmp $Qmp quit 2>$null | Out-Null }
  if (-not $P.WaitForExit(30000)) { Stop-Process -Id $P.Id -Force; 'killed: no clean quit within 30 s' }
}

function Save-Shot([string]$Path, [int]$Qmp = 4444) {
  py -3 $Ctl qmp $Qmp screendump "filename=$Path" format=png | Out-Null
  py -3 $Score $Path
}

# The run's log: stderr.log, or xemu.log when xemu found no console to attach to (ui/xemu.c
# then sends both streams to xemu.log in its working directory, created at startup). Decide by
# xemu.log, not by stderr.log's size: the CRT buffers a redirected stderr in 4 KiB blocks, so
# stderr.log can be empty while xemu runs (nsprof and the heartbeat flush it; exit does too).
function Get-XemuLog([string]$Dir) {
  $x = Join-Path $Dir 'xemu.log'
  if (Test-Path -LiteralPath $x) { $x } else { Join-Path $Dir 'stderr.log' }
}

function Show-Nsprof([string]$Dir, [int]$Last = 12) {
  Select-String -LiteralPath (Get-XemuLog $Dir) -Pattern 'nsprof: .*interval|nsprof:\s+(draws|renderpass|fence_wait)\b' -ErrorAction SilentlyContinue |
    Select-Object -Last $Last | ForEach-Object { $_.Line }
}

# Copy a fresh build's dist\ to C:\xemu-kit\bin\<Name> (contents, not a nested dist folder); returns the exe path.
function Copy-Dist([string]$Name) {
  $dst = Join-Path $Kit "bin\$Name"
  New-Item -ItemType Directory -Force $dst | Out-Null
  Copy-Item -Recurse -Force (Join-Path $Repo 'dist\*') $dst
  Join-Path $dst 'xemu.exe'
}

# Stop only automated (marked) xemu processes, never the owner's own.
function Stop-AutoXemu {
  Get-CimInstance Win32_Process -Filter "Name='xemu.exe'" | Where-Object CommandLine -like '*xemu-fork-auto*' |
    ForEach-Object { Stop-Process -Id $_.ProcessId -Force; "stopped $($_.ProcessId)" }
}
