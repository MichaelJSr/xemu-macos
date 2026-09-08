<#
capture_screen.ps1 - primary-screen / window capture for the Windows bench
harness (scripts/bench-savestate-ab-win.py).

Windows has no `screencapture -l <windowID>`: flip-model swapchains (what
xemu's Vulkan present uses) commonly return black from PrintWindow /
BitBlt against the window DC, so the reliable capture is the desktop
composition itself - System.Drawing Graphics.CopyFromScreen of the region
the window occupies. That means the window MUST be visible and unobscured
on the primary screen while a bench runs; an occluded window scores the
pixels of whatever covers it (recorded in the verdict as a screen-mode
capture).

Usage:
  capture_screen.ps1 -Out shot.png [-ProcessId 1234] [-Thumb shot.small.png]
                     [-ThumbWidth 320]

-ProcessId  crop to that process's main window rect (falls back to the
            whole primary screen, reported as mode=screen).
-Thumb      also write a downscaled copy; the pure-Python artifact scorer
            (score_shot_win.py) decodes that instead of a 1080p PNG, which
            keeps post-run scoring off the benchmark's CPU budget.

Prints one line: capture: <w>x<h> mode=<window|screen> -> <path>
#>
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [int]$ProcessId = 0,
    [string]$Thumb = "",
    [int]$ThumbWidth = 320
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

Add-Type @"
using System;
using System.Runtime.InteropServices;
public struct RECT { public int Left, Top, Right, Bottom; }
public class WinRect {
    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);
}
"@

$screen = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$mode = "screen"
$origin = $screen.Location
$size = $screen.Size

if ($ProcessId -gt 0) {
    $p = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
    if ($p -and $p.MainWindowHandle -ne [IntPtr]::Zero) {
        $r = New-Object RECT
        if ([WinRect]::GetWindowRect($p.MainWindowHandle, [ref]$r)) {
            $w = $r.Right - $r.Left
            $h = $r.Bottom - $r.Top
            if ($w -gt 64 -and $h -gt 64) {
                # Clamp into the primary screen: CopyFromScreen on an
                # off-screen region yields black, which would read as a
                # UNIFORM_FRAME artifact.
                $l = [Math]::Max($r.Left, $screen.Left)
                $t = [Math]::Max($r.Top, $screen.Top)
                $w = [Math]::Min($r.Right, $screen.Right) - $l
                $h = [Math]::Min($r.Bottom, $screen.Bottom) - $t
                if ($w -gt 64 -and $h -gt 64) {
                    $origin = New-Object System.Drawing.Point($l, $t)
                    $size = New-Object System.Drawing.Size($w, $h)
                    $mode = "window"
                }
            }
        }
    }
}

$bmp = New-Object System.Drawing.Bitmap $size.Width, $size.Height
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($origin, [System.Drawing.Point]::Empty, $size)
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)

if ($Thumb -ne "") {
    $tw = [Math]::Min($ThumbWidth, $size.Width)
    $th = [Math]::Max(1, [int]($size.Height * $tw / $size.Width))
    $small = New-Object System.Drawing.Bitmap $tw, $th
    $sg = [System.Drawing.Graphics]::FromImage($small)
    # Bilinear averages the source block (no bicubic overshoot, which can
    # manufacture out-of-gamut pixels the magenta test would flag).
    $sg.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBilinear
    $sg.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $sg.DrawImage($bmp, 0, 0, $tw, $th)
    $small.Save($Thumb, [System.Drawing.Imaging.ImageFormat]::Png)
    $sg.Dispose(); $small.Dispose()
}

$g.Dispose(); $bmp.Dispose()
Write-Output ("capture: {0}x{1} mode={2} -> {3}" -f $size.Width, $size.Height, $mode, $Out)
