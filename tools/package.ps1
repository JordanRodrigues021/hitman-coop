# Assembles the player download: installer + ZHMModSDK loader + HitmanCoop.dll, zipped.
param(
    [string]$ModDll = (Join-Path $PSScriptRoot "..\_build\install\mods\HitmanCoop.dll"),
    [string]$NetTest = (Join-Path $PSScriptRoot "..\_build\install\HitmanCoop-NetTest.exe"),
    [string]$SdkRelease = (Join-Path $PSScriptRoot "..\_deps\sdk-release"),
    [string]$OutDir = (Join-Path $PSScriptRoot "..\_dist"),
    [string]$Version = "0.12.0"
)

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")

if (-not (Test-Path $ModDll)) { throw "Mod DLL not found at $ModDll. Build and install first (see BUILDING.md)." }
if (-not (Test-Path $NetTest)) { throw "Connection test not found at $NetTest. Build and install first (see BUILDING.md)." }
if (-not (Test-Path $SdkRelease)) { throw "SDK release not found at $SdkRelease. Run tools\fetch-sdk.ps1 first." }

# The release zip may or may not wrap everything in one top-level folder.
$SdkRoot = $SdkRelease
$Top = @(Get-ChildItem $SdkRelease)
if ($Top.Count -eq 1 -and $Top[0].PSIsContainer) { $SdkRoot = $Top[0].FullName }

if (-not (Test-Path (Join-Path $SdkRoot "dinput8.dll"))) { throw "dinput8.dll not found in the SDK release at $SdkRoot." }

$Name = "HitmanCoop-$Version"
$Stage = Join-Path $OutDir $Name
Remove-Item $Stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Join-Path $Stage "files\mods") | Out-Null
New-Item -ItemType Directory -Force (Join-Path $Stage "licenses") | Out-Null

# Mod loader and SDK runtime files. The SDK's sample mods are left out.
foreach ($Item in Get-ChildItem $SdkRoot) {
    if ($Item.Name -in @("mods", "licenses", "LICENSE")) { continue }
    if ($Item.Extension -eq ".pdb") { continue }
    Copy-Item $Item.FullName (Join-Path $Stage "files") -Recurse
}

Copy-Item $ModDll (Join-Path $Stage "files\mods\HitmanCoop.dll")
Copy-Item $NetTest (Join-Path $Stage "Test-Connection.exe")

Copy-Item (Join-Path $Root "installer\Install.bat") $Stage
Copy-Item (Join-Path $Root "installer\Uninstall.bat") $Stage
Copy-Item (Join-Path $Root "installer\installer.ps1") $Stage
Copy-Item (Join-Path $Root "installer\README.txt") $Stage

Copy-Item (Join-Path $Root "LICENSE") (Join-Path $Stage "licenses\HitmanCoop-LICENSE.txt")
if (Test-Path (Join-Path $SdkRoot "LICENSE")) { Copy-Item (Join-Path $SdkRoot "LICENSE") (Join-Path $Stage "licenses\ZHMModSDK-LICENSE.txt") }
if (Test-Path (Join-Path $SdkRoot "licenses")) { Copy-Item (Join-Path $SdkRoot "licenses") (Join-Path $Stage "licenses\ZHMModSDK-third-party") -Recurse }
Set-Content (Join-Path $Stage "licenses\SOURCES.txt") @(
    "HITMAN Co-op, created by Jordan Rodrigues, is free software under the GNU GPL v3 or later.",
    "ZHMModSDK v4.1.1 by OrfeasZ (GPL-3.0) source: https://github.com/OrfeasZ/ZHMModSDK/tree/v4.1.1"
)

# The zip: for people who prefer extracting and running Install.bat.
$Zip = Join-Path $OutDir "$Name.zip"
Remove-Item $Zip -Force -ErrorAction SilentlyContinue
Compress-Archive -Path $Stage -DestinationPath $Zip

# The one-file windowed setup: the same files embedded in HitmanCoop-Setup.exe.
$SetupSrc = Join-Path $Root "installer\setup"
$Payload = Join-Path $OutDir "payload.zip"
Remove-Item $Payload -Force -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $Stage "*") -DestinationPath $Payload

$Icon = Join-Path $SetupSrc "setup.ico"
if (-not (Test-Path $Icon)) { & python (Join-Path $SetupSrc "make_icon.py") $Icon }

$Csc = Join-Path $env:WINDIR "Microsoft.NET\Framework64\v4.0.30319\csc.exe"
$FxDir = Split-Path $Csc
$Setup = Join-Path $OutDir "HitmanCoop-Setup-$Version.exe"
& $Csc /nologo /target:winexe /optimize+ /out:$Setup "/win32icon:$Icon" "/win32manifest:$(Join-Path $SetupSrc 'setup.manifest')" `
    "/resource:$Payload,payload.zip" /r:System.Windows.Forms.dll /r:System.Drawing.dll `
    "/r:$FxDir\System.IO.Compression.dll" "/r:$FxDir\System.IO.Compression.FileSystem.dll" (Join-Path $SetupSrc "Setup.cs")
if ($LASTEXITCODE -ne 0) { throw "Building the setup exe failed." }
Remove-Item $Payload -Force

foreach ($File in $Zip, $Setup) {
    $Hash = (Get-FileHash -Algorithm SHA256 $File).Hash.ToLowerInvariant()
    Set-Content "$File.sha256" "$Hash  $(Split-Path -Leaf $File)"
    Write-Host "Packaged $File"
    Write-Host "SHA-256 $Hash"
}
