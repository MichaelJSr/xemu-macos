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

The process declares itself DPI-aware before any screen query (otherwise a
scaled display hands back a stretched, mis-cropped capture) and prefers DWM's
extended frame bounds over GetWindowRect, whose rect includes the invisible
resize border. Both still cover the whole window frame, title bar included.

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
    [DllImport("user32.dll")]
    public static extern bool SetProcessDPIAware();
    [DllImport("dwmapi.dll")]
    public static extern int DwmGetWindowAttribute(IntPtr hWnd, int attr,
                                                   out RECT value, int size);
}
"@

# DPI awareness, before the first screen or window query. Without it Windows
# virtualizes this process into a 96-DPI coordinate space: on a scaled display
# GetWindowRect reports logical pixels while CopyFromScreen returns a
# stretched, resampled copy of the desktop - the crop lands in the wrong place
# and the artifact scorer reads interpolated pixels. System-DPI awareness is
# enough for a primary-screen capture; a window on a differently-scaled second
# monitor is still approximate (that would need per-monitor-v2 awareness,
# which cannot be set after the process starts loading UI assemblies).
try { [void][WinRect]::SetProcessDPIAware() } catch { }

$screen = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$mode = "screen"
$origin = $screen.Location
$size = $screen.Size

if ($ProcessId -gt 0) {
    $p = Get-Process -Id $ProcessId -ErrorAction SilentlyContinue
    if ($p -and $p.MainWindowHandle -ne [IntPtr]::Zero) {
        $r = New-Object RECT
        # DWMWA_EXTENDED_FRAME_BOUNDS (9) is the rect the compositor actually
        # draws. GetWindowRect adds the invisible resize border (~7 px a side
        # on Win10+), so cropping to it samples the desktop BEHIND the window
        # along every edge - background pixels the scorer then judges. Fall
        # back to GetWindowRect when DWM declines (composition off, or a
        # window DWM does not track). Either rect is the whole frame: the
        # title bar is still inside the capture, as it always was.
        $got = $false
        try {
            $hr = [WinRect]::DwmGetWindowAttribute($p.MainWindowHandle, 9,
                      [ref]$r, [Runtime.InteropServices.Marshal]::SizeOf([RECT]))
            $got = ($hr -eq 0)
        } catch { $got = $false }
        if (-not $got) {
            $got = [WinRect]::GetWindowRect($p.MainWindowHandle, [ref]$r)
        }
        if ($got) {
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
