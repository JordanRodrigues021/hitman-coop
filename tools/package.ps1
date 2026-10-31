# Assembles the player download: installer + ZHMModSDK loader + HitmanCoop.dll, zipped.
param(
    [string]$ModDll = (Join-Path $PSScriptRoot "..\_build\install\mods\HitmanCoop.dll"),
    [string]$NetTest = (Join-Path $PSScriptRoot "..\_build\install\HitmanCoop-NetTest.exe"),
    [string]$SdkRelease = (Join-Path $PSScriptRoot "..\_deps\sdk-release"),
    [string]$OutDir = (Join-Path $PSScriptRoot "..\_dist"),
    [string]$Version = "0.13.0",
    # Where the mod's updater downloads this version's files from (see plugin/Src/Updater.h).
    [string]$UpdateBase = ""
)

$ErrorActionPreference = "Stop"
$Root = Resolve-Path (Join-Path $PSScriptRoot "..")

if (-not (Test-Path $ModDll)) { throw "Mod DLL not found at $ModDll. Build and install first (see BUILDING.md)." }
if (-not (Test-Path $NetTest)) { throw "Connection test not found at $NetTest. Build and install first (see BUILDING.md)." }
if (-not (Test-Path $SdkRelease)) { throw "SDK release not found at $SdkRelease. Run tools\fetch-sdk.ps1 first." }

# 0.12.1 shipped 0.12.0's DLL (built, but the install step skipped): each must say this version,
# and the mod DLL must be the one just built.
foreach ($Bin in $ModDll, $NetTest) {
    $Text = [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($Bin))
    if (-not $Text.Contains($Version)) { throw "$Bin isn't version $Version. Build and install first (see BUILDING.md)." }
}
$Built = Join-Path $PSScriptRoot "..\_build\plugin\RelWithDebInfo\HitmanCoop.dll"
if ((Test-Path $Built) -and (Get-FileHash $Built).Hash -ne (Get-FileHash $ModDll).Hash) {
    throw "$ModDll differs from the last build at $Built. Run the install step (see BUILDING.md)."
}

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
    "ZHMModSDK v4.2.1 by OrfeasZ (GPL-3.0) source: https://github.com/OrfeasZ/ZHMModSDK/tree/v4.2.1"
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
    # LF only, no BOM: "sha256sum -c" read the file name with the CR of a Windows line ending.
    [System.IO.File]::WriteAllText("$File.sha256", "$Hash  $(Split-Path -Leaf $File)`n", (New-Object System.Text.UTF8Encoding $false))
    Write-Host "Packaged $File"
    Write-Host "SHA-256 $Hash"
}

# The self-update: update.txt (version, each file the installer puts in the game's Retail folder
# with its size and SHA-256, what's new) and those files under their own names, all uploaded to
# the release. The mod reads update.txt from the latest release.
if (-not $UpdateBase) { $UpdateBase = "https://github.com/JordanRodrigues021/hitman-coop/releases/download/v$Version/" }
$WhatsNew = Join-Path $Root "installer\WHATSNEW.txt"
if (-not (Test-Path $WhatsNew)) { throw "installer\WHATSNEW.txt (what's new in $Version, shown in game after an update) is missing." }
$Notes = (Get-Content $WhatsNew -Raw -Encoding UTF8).Trim()
if ($Notes -notmatch [regex]::Escape($Version)) { throw "installer\WHATSNEW.txt doesn't mention ${Version}: write what's new in it first." }

$Update = Join-Path $OutDir "update-$Version"
Remove-Item $Update -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $Update | Out-Null
$Files = (Resolve-Path (Join-Path $Stage "files")).Path.TrimEnd("\")
$Lines = @("version $Version", "base $UpdateBase")
$Names = @{}

foreach ($Item in Get-ChildItem $Files -Recurse -File) {
    $Rel = $Item.FullName.Substring($Files.Length + 1).Replace("\", "/")
    if (-not (Test-Path (Join-Path $Files $Rel)) -or $Rel -notmatch '^[A-Za-z0-9_./-]+$') { throw "Bad update path '$Rel' for $($Item.FullName)." }
    if ($Names.ContainsKey($Item.Name)) { throw "Two update files are called $($Item.Name): release assets need unique names." }
    $Names[$Item.Name] = $true
    Copy-Item $Item.FullName (Join-Path $Update $Item.Name)
    $Lines += "file $Rel $($Item.Length) $((Get-FileHash -Algorithm SHA256 $Item.FullName).Hash.ToLowerInvariant())"
}

$Lines += "notes"
$Text = ($Lines -join "`n") + "`n" + ($Notes -replace "`r", "") + "`n"
[System.IO.File]::WriteAllText((Join-Path $Update "update.txt"), $Text, (New-Object System.Text.UTF8Encoding $false))
Write-Host "Update files in $Update ($($Names.Count) files + update.txt)"
