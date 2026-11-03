# Assembles the player downloads, one installer for every game version HITMAN Co-op supports:
#   files\        the build for the current game (3.290.x): _build, ZHMModSDK 4.2.1's loader
#   files-3.280\  the build for game 3.280.x: _build-3280 (-DCOOP_SDK=4.1.1), ZHMModSDK 4.1.1's loader
# installer.ps1 / installer.sh install the one for the game they find. Out come, in _dist:
#   HitmanCoop-<v>.zip              Windows: extract, run Install.bat
#   HitmanCoop-Setup-<v>.exe        Windows: the same files in a one-file windowed setup
#   HitmanCoop-<v>-linux.tar.gz     Linux / Steam Deck (Proton): extract, bash installer.sh
#   update-<v>\                     the self-updater's files: update.txt + the 3.290 files, and
#                                   update-3.280.txt + game-3.280\ (see BUILDING.md, "Releasing")
#   tools\package.ps1 [-Version 0.13.94] [-No3280]   (the version defaults to the one in plugin/Src/CoopText.h)
param(
    [string]$Version = "",
    [string]$OutDir = (Join-Path $PSScriptRoot "..\_dist"),
    # Leaves the 3.280 build out (a quick build of the current one only).
    [switch]$No3280,
    # Where the mod's updater downloads this version's files from (see plugin/Src/Updater.h).
    [string]$UpdateBase = "https://raw.githubusercontent.com/JordanRodrigues021/hitman-coop/updates/"
)

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
if (-not $Version) {
    $Version = [regex]::Match((Get-Content (Join-Path $Root "plugin\Src\CoopText.h") -Raw), 'c_ModVersion = "([^"]+)"').Groups[1].Value
    if (-not $Version) { throw "No c_ModVersion in plugin\Src\CoopText.h: pass -Version." }
}
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path

# Each build: where it was built, its SDK's release files, and the folder it goes in.
$Builds = @(
    @{ Game = "3.290"; BuildDir = "_build"; Sdk = "_deps\sdk-release"; SdkName = "v4.2.1"; Folder = "files" }
)
if (-not $No3280) {
    $Builds += @{ Game = "3.280"; BuildDir = "_build-3280"; Sdk = "_deps\old-4.1.1\sdk-release"; SdkName = "v4.1.1"; Folder = "files-3.280" }
}

function Read-Text([string]$Path) { [System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($Path)) }

foreach ($B in $Builds) {
    $B.ModDll = Join-Path $Root "$($B.BuildDir)\install\mods\HitmanCoop.dll"
    $B.Built = Join-Path $Root "$($B.BuildDir)\plugin\RelWithDebInfo\HitmanCoop.dll"
    $B.SdkRoot = Join-Path $Root $B.Sdk

    if (-not (Test-Path $B.ModDll)) { throw "The game $($B.Game) build isn't installed at $($B.ModDll). Build and install $($B.BuildDir) first (see BUILDING.md)." }
    if (-not (Test-Path (Join-Path $B.SdkRoot "dinput8.dll"))) { throw "The SDK release for game $($B.Game) isn't at $($B.SdkRoot). Run tools\fetch-sdk.ps1 first." }

    # 0.12.1 shipped 0.12.0's DLL (built, but the install step skipped): each must say this version,
    # be the one just built, and be the build for its game.
    $Text = Read-Text $B.ModDll
    if (-not $Text.Contains($Version)) { throw "$($B.ModDll) isn't version $Version. Build and install $($B.BuildDir) first." }
    if (-not $Text.Contains("HitmanCoop build for game $($B.Game)")) { throw "$($B.ModDll) isn't the build for game $($B.Game) (configure $($B.BuildDir) with the matching -DCOOP_SDK)." }
    if ((Test-Path $B.Built) -and (Get-FileHash $B.Built).Hash -ne (Get-FileHash $B.ModDll).Hash) {
        throw "$($B.ModDll) differs from the last build at $($B.Built). Run the install step for $($B.BuildDir)."
    }
}

$NetTest = Join-Path $Root "_build\install\HitmanCoop-NetTest.exe"
if (-not (Test-Path $NetTest)) { throw "Connection test not found at $NetTest. Build and install first (see BUILDING.md)." }
if (-not (Read-Text $NetTest).Contains($Version)) { throw "$NetTest isn't version $Version. Build and install first." }

# The installer scripts must keep their line endings: installer.sh with CR in it doesn't run.
$Sh = Join-Path $Root "installer\installer.sh"
if ([System.IO.File]::ReadAllText($Sh).Contains("`r")) { throw "installer\installer.sh has Windows line endings: it must be LF only." }

$Name = "HitmanCoop-$Version"
$Stage = Join-Path $OutDir $Name
Remove-Item $Stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Join-Path $Stage "licenses") | Out-Null

foreach ($B in $Builds) {
    $Files = Join-Path $Stage $B.Folder
    New-Item -ItemType Directory -Force (Join-Path $Files "mods") | Out-Null

    # Mod loader and SDK runtime files. The SDK's sample mods are left out.
    foreach ($Item in Get-ChildItem $B.SdkRoot) {
        if ($Item.Name -in @("mods", "licenses", "LICENSE")) { continue }
        if ($Item.Extension -eq ".pdb") { continue }
        Copy-Item $Item.FullName $Files -Recurse
    }

    Copy-Item $B.ModDll (Join-Path $Files "mods\HitmanCoop.dll")
}

Copy-Item $NetTest (Join-Path $Stage "Test-Connection.exe")
foreach ($File in "Install.bat", "Uninstall.bat", "installer.ps1", "installer.sh", "README.txt") {
    Copy-Item (Join-Path $Root "installer\$File") $Stage
}

$Main = $Builds[0]
Copy-Item (Join-Path $Root "LICENSE") (Join-Path $Stage "licenses\HitmanCoop-LICENSE.txt")
if (Test-Path (Join-Path $Main.SdkRoot "LICENSE")) { Copy-Item (Join-Path $Main.SdkRoot "LICENSE") (Join-Path $Stage "licenses\ZHMModSDK-LICENSE.txt") }
if (Test-Path (Join-Path $Main.SdkRoot "licenses")) { Copy-Item (Join-Path $Main.SdkRoot "licenses") (Join-Path $Stage "licenses\ZHMModSDK-third-party") -Recurse }
$Sources = @("HITMAN Co-op, created by Jordan Rodrigues, is free software under the GNU GPL v3 or later.")
foreach ($B in $Builds) {
    $Sources += "ZHMModSDK $($B.SdkName) by OrfeasZ (GPL-3.0), in $($B.Folder) (game $($B.Game)): https://github.com/OrfeasZ/ZHMModSDK/tree/$($B.SdkName)"
}
Set-Content (Join-Path $Stage "licenses\SOURCES.txt") $Sources

# Windows: the zip, for people who prefer extracting and running Install.bat.
$Zip = Join-Path $OutDir "$Name.zip"
Remove-Item $Zip -Force -ErrorAction SilentlyContinue
Compress-Archive -Path $Stage -DestinationPath $Zip

# Linux / Steam Deck: the same, as a tarball (installer.sh keeps its LF endings either way).
$Tar = Join-Path $OutDir "$Name-linux.tar.gz"
Remove-Item $Tar -Force -ErrorAction SilentlyContinue
& (Join-Path $env:WINDIR "System32\tar.exe") -c -z -f $Tar -C $OutDir $Name
if ($LASTEXITCODE -ne 0) { throw "Making the Linux tarball failed." }

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

foreach ($File in $Zip, $Setup, $Tar) {
    $Hash = (Get-FileHash -Algorithm SHA256 $File).Hash.ToLowerInvariant()
    # LF only, no BOM: "sha256sum -c" read the file name with the CR of a Windows line ending.
    [System.IO.File]::WriteAllText("$File.sha256", "$Hash  $(Split-Path -Leaf $File)`n", (New-Object System.Text.UTF8Encoding $false))
    Write-Host "Packaged $File ($([math]::Round((Get-Item $File).Length / 1MB, 1)) MB)"
    Write-Host "SHA-256 $Hash"
}

# The self-update: for each build a manifest (version, each file the installer puts in the game's
# Retail folder with its size and SHA-256, what's new) and those files under their own names. The
# mod reads the one for the game it runs on (update.txt, or update-3.280.txt on game 3.280), so a
# player whose game moves on gets the right build by itself. The manifests go on the release and
# the updates branch; the files on the updates branch (the 3.280 ones in game-3.280/).
$WhatsNew = Join-Path $Root "installer\WHATSNEW.txt"
if (-not (Test-Path $WhatsNew)) { throw "installer\WHATSNEW.txt (what's new in $Version, shown in game after an update) is missing." }
$Notes = (Get-Content $WhatsNew -Raw -Encoding UTF8).Trim()
if ($Notes -notmatch [regex]::Escape($Version)) { throw "installer\WHATSNEW.txt doesn't mention ${Version}: write what's new in it first." }

$Update = Join-Path $OutDir "update-$Version"
Remove-Item $Update -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $Update | Out-Null

foreach ($B in $Builds) {
    $Main = $B.Game -eq "3.290"
    $Into = if ($Main) { $Update } else { Join-Path $Update "game-$($B.Game)" }
    $Base = if ($Main) { $UpdateBase } else { "$($UpdateBase)game-$($B.Game)/" }
    $Manifest = if ($Main) { "update.txt" } else { "update-$($B.Game).txt" }
    New-Item -ItemType Directory -Force $Into | Out-Null
    $Files = (Resolve-Path (Join-Path $Stage $B.Folder)).Path.TrimEnd("\")
    $Lines = @("version $Version", "base $Base")
    $Names = @{}

    foreach ($Item in Get-ChildItem $Files -Recurse -File) {
        $Rel = $Item.FullName.Substring($Files.Length + 1).Replace("\", "/")
        if ($Rel -notmatch '^[A-Za-z0-9_./-]+$') { throw "Bad update path '$Rel' for $($Item.FullName)." }
        if ($Names.ContainsKey($Item.Name)) { throw "Two update files are called $($Item.Name): release assets need unique names." }
        $Names[$Item.Name] = $true
        Copy-Item $Item.FullName (Join-Path $Into $Item.Name)
        $Lines += "file $Rel $($Item.Length) $((Get-FileHash -Algorithm SHA256 $Item.FullName).Hash.ToLowerInvariant())"
    }

    $Lines += "notes"
    $Text = ($Lines -join "`n") + "`n" + ($Notes -replace "`r", "") + "`n"
    [System.IO.File]::WriteAllText((Join-Path $Update $Manifest), $Text, (New-Object System.Text.UTF8Encoding $false))
    Write-Host "Update for game $($B.Game): $Manifest + $($Names.Count) files in $Into"
}
