# Screenshot for the game tests: one process's main window (PrintWindow, works when covered) or the
# whole primary screen, saved as a PNG at most -MaxWidth pixels wide.
#   shot.ps1 -Out a.png [-Process HITMAN3] [-Screen] [-MaxWidth 1280]
param(
    [Parameter(Mandatory)] [string]$Out,
    [string]$Process = "",
    [switch]$Screen,
    [int]$MaxWidth = 1280
)

Add-Type -AssemblyName System.Drawing, System.Windows.Forms
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class ShotNative {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
'@
[ShotNative]::SetProcessDPIAware() | Out-Null

if ($Screen -or -not $Process) {
    $Bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    $Bmp = New-Object System.Drawing.Bitmap $Bounds.Width, $Bounds.Height
    $G = [System.Drawing.Graphics]::FromImage($Bmp)
    $G.CopyFromScreen($Bounds.Location, [System.Drawing.Point]::Empty, $Bounds.Size)
} else {
    $P = Get-Process -Name $Process -ErrorAction Stop | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
    if (-not $P) { throw "No window for $Process" }
    $R = New-Object ShotNative+RECT
    [ShotNative]::GetWindowRect($P.MainWindowHandle, [ref]$R) | Out-Null
    $Bmp = New-Object System.Drawing.Bitmap ($R.R - $R.L), ($R.B - $R.T)
    $G = [System.Drawing.Graphics]::FromImage($Bmp)
    $Hdc = $G.GetHdc()
    [ShotNative]::PrintWindow($P.MainWindowHandle, $Hdc, 2) | Out-Null
    $G.ReleaseHdc($Hdc)
}

$G.Dispose()

if ($Bmp.Width -gt $MaxWidth) {
    $H = [int]($Bmp.Height * $MaxWidth / $Bmp.Width)
    $Small = New-Object System.Drawing.Bitmap $Bmp, $MaxWidth, $H
    $Bmp.Dispose()
    $Bmp = $Small
}

$Bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$Bmp.Dispose()
Write-Output $Out
