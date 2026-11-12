# HITMAN Co-op installer. Installs the ZHMModSDK mod loader (if needed) and the HitmanCoop mod
# into a HITMAN World of Assassination install, and can undo it again.
#
#   installer.ps1                  find the game and install
#   installer.ps1 -GamePath "D:\Games\HITMAN 3"
#   installer.ps1 -Uninstall
#   installer.ps1 -ListGames       print the detected installs, one per line (used by the setup window)
#
# Created by Jordan Rodrigues.
param(
    [string]$GamePath = "",
    [switch]$Uninstall,
    [switch]$NoFirewall,
    [switch]$Firewall,
    [switch]$ListGames,
    [switch]$Quiet
)

$ErrorActionPreference = "Stop"
$ModName = "HitmanCoop"
$Payload = Join-Path $PSScriptRoot "files"   # chosen for the game in Install-Mod (Select-Payload)
$ManifestName = "HitmanCoop-install.json"
$LogFile = $null            # Retail\HitmanCoop-install.log once the game is known

# The download has a build for each game version it supports: files\ for the current game (3.290.x),
# files-3.280\ for players who can't update past 3.280.
function Select-Payload([string]$Version) {
    $Older = Join-Path $PSScriptRoot "files-3.280"
    if ($Version -like "3.280.*" -and (Test-Path (Join-Path $Older "mods\$ModName.dll"))) { return $Older }
    return (Join-Path $PSScriptRoot "files")
}

function Say([string]$Text) {
    Write-Host $Text
    if ($LogFile) { try { Add-Content -Path $LogFile -Value $Text -Encoding utf8 } catch {} }
}

# The Microsoft Store / Game Pass build keeps its files in a protected package (Content\Retail):
# no mod loader can be added to it.
function Test-StoreBuild([string]$Dir) {
    return $Dir -and (Test-Path (Join-Path $Dir "Content\Retail\HITMAN3.exe"))
}

function Find-StoreBuilds {
    $Found = @()
    foreach ($Drive in [IO.DriveInfo]::GetDrives() | Where-Object { $_.DriveType -eq 'Fixed' -and $_.IsReady }) {
        $Root = Join-Path $Drive.RootDirectory.FullName "XboxGames"
        if (-not (Test-Path $Root)) { continue }
        foreach ($Dir in Get-ChildItem $Root -Directory -Filter "HITMAN*" -ErrorAction SilentlyContinue) {
            if (Test-StoreBuild $Dir.FullName) { $Found += $Dir.FullName }
        }
    }
    return $Found
}

# Version and store of an install, for the log and the messages.
function Get-GameInfo([string]$Game) {
    $Exe = Join-Path $Game "Retail\HITMAN3.exe"
    $Version = "unknown"
    try {
        $Info = (Get-Item $Exe).VersionInfo
        # The numbers, as the mod reads them ("3.290.1"); the text fields can be formatted any way.
        if ($Info.FileMajorPart -or $Info.FileMinorPart) { $Version = "$($Info.FileMajorPart).$($Info.FileMinorPart).$($Info.FileBuildPart)" }
        elseif ($Info.ProductVersion) { $Version = $Info.ProductVersion.Trim() }
        else { $Version = $Info.FileVersion.Trim() }
    } catch {}

    $Store = "unknown store"
    if ($Game -match '\\steamapps\\') { $Store = "Steam" }
    elseif (Test-Path (Join-Path $Game ".egstore")) { $Store = "Epic Games" }

    return @{ Version = $Version; Store = $Store }
}

# Version of a file (the mod loader), or $null.
function Get-FileVersion([string]$Path) {
    if (-not (Test-Path $Path)) { return $null }
    try {
        $Info = (Get-Item $Path).VersionInfo
        return [version]::new($Info.FileMajorPart, $Info.FileMinorPart, $Info.FileBuildPart, $Info.FilePrivatePart)
    } catch { return $null }
}

function Test-GameDir([string]$Dir) {
    return $Dir -and (Test-Path (Join-Path $Dir "Retail\HITMAN3.exe"))
}

function Find-SteamGames {
    $Found = @()
    $SteamPath = $null

    try { $SteamPath = (Get-ItemProperty -Path "HKCU:\Software\Valve\Steam" -Name SteamPath -ErrorAction Stop).SteamPath } catch {}
    if (-not $SteamPath) {
        try { $SteamPath = (Get-ItemProperty -Path "HKLM:\SOFTWARE\WOW6432Node\Valve\Steam" -Name InstallPath -ErrorAction Stop).InstallPath } catch {}
    }

    if (-not $SteamPath) { return $Found }

    # Library paths from the vdf first: they keep the folder's real capitalisation.
    $Libraries = @()
    $Vdf = Join-Path $SteamPath "steamapps\libraryfolders.vdf"

    if (Test-Path $Vdf) {
        foreach ($Match in [regex]::Matches((Get-Content $Vdf -Raw), '"path"\s+"([^"]+)"')) {
            $Libraries += $Match.Groups[1].Value -replace '\\\\', '\'
        }
    }

    $Libraries += $SteamPath

    foreach ($Library in $Libraries) {
        $Dir = Join-Path $Library "steamapps\common\HITMAN 3"
        if (Test-GameDir $Dir) { $Found += (Resolve-Path $Dir).Path }
    }

    return $Found
}

# Steam stores its own path in lower case, so the same folder can turn up twice.
function Get-UniquePaths([string[]]$Paths) {
    $Seen = @{}
    $Unique = @()

    foreach ($Path in $Paths) {
        if (-not $Path) { continue }
        $Key = ([IO.Path]::GetFullPath($Path)).TrimEnd('\').ToLowerInvariant()
        if (-not $Seen.ContainsKey($Key)) { $Seen[$Key] = $true; $Unique += $Path }
    }

    return $Unique
}

function Find-EpicGames {
    $Found = @()
    $Manifests = "$env:ProgramData\Epic\EpicGamesLauncher\Data\Manifests"

    if (-not (Test-Path $Manifests)) { return $Found }

    foreach ($Item in Get-ChildItem $Manifests -Filter *.item) {
        try {
            $Json = Get-Content $Item.FullName -Raw | ConvertFrom-Json

            if ($Json.DisplayName -like "HITMAN*" -and (Test-GameDir $Json.InstallLocation)) {
                $Found += $Json.InstallLocation
            }
        } catch {}
    }

    return $Found
}

# Epic installs made with Legendary or Heroic instead of the Epic launcher.
function Find-LegendaryGames {
    $Found = @()
    foreach ($Json in @("$env:USERPROFILE\.config\legendary\installed.json", "$env:APPDATA\heroic\legendaryConfig\legendary\installed.json")) {
        if (-not (Test-Path $Json)) { continue }
        try {
            $Installed = Get-Content $Json -Raw | ConvertFrom-Json
            foreach ($App in $Installed.PSObject.Properties) {
                if ($App.Value.title -like "HITMAN*" -and (Test-GameDir $App.Value.install_path)) { $Found += $App.Value.install_path }
            }
        } catch {}
    }
    return $Found
}

function Find-AllGames {
    return @(Get-UniquePaths (@(Find-SteamGames) + @(Find-EpicGames) + @(Find-LegendaryGames)))
}

function Select-GameFolder {
    Add-Type -AssemblyName System.Windows.Forms
    $Dialog = New-Object System.Windows.Forms.FolderBrowserDialog
    $Dialog.Description = "Select your HITMAN World of Assassination folder (the one that contains 'Retail')"

    if ($Dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { return $Dialog.SelectedPath }

    return ""
}

function Resolve-GamePath {
    if ($GamePath) {
        # Accept the Retail folder itself too.
        if ((Split-Path $GamePath -Leaf) -ieq "Retail") { $GamePath = Split-Path $GamePath -Parent }
        if (Test-GameDir $GamePath) { return (Resolve-Path $GamePath).Path }
        if (Test-StoreBuild $GamePath) { throw "This is the Microsoft Store / Game Pass version of HITMAN. Its files are locked by Windows, so no mods can be installed into it. HITMAN Co-op works with the Steam and Epic versions." }
        throw "No Retail\HITMAN3.exe under '$GamePath'."
    }

    # @(): a single install comes back as a plain string, and [0] of that is its first letter.
    $Candidates = @(Find-AllGames)

    if ($Candidates.Count -eq 1) { return $Candidates[0] }

    if ($Candidates.Count -gt 1) {
        Say "Found more than one HITMAN install:"
        for ($i = 0; $i -lt $Candidates.Count; $i++) { Say "  [$($i + 1)] $($Candidates[$i])" }
        $Choice = Read-Host "Which one? (1-$($Candidates.Count))"
        $Index = [int]$Choice - 1
        if ($Index -ge 0 -and $Index -lt $Candidates.Count) { return $Candidates[$Index] }
        throw "No install selected."
    }

    if (@(Find-StoreBuilds).Count -gt 0) {
        Say "Found only the Microsoft Store / Game Pass version of HITMAN, which can't be modded (Windows locks its files). HITMAN Co-op works with the Steam and Epic versions."
    }

    Say "Couldn't find HITMAN automatically. Please pick the game folder."
    $Picked = Select-GameFolder
    if ((Split-Path $Picked -Leaf) -ieq "Retail") { $Picked = Split-Path $Picked -Parent }
    if (Test-GameDir $Picked) { return $Picked }

    throw "That folder doesn't contain Retail\HITMAN3.exe."
}

function Enable-ModInIni([string]$Ini) {
    $Lines = @()
    if (Test-Path $Ini) { $Lines = @(Get-Content $Ini) }

    if (-not ($Lines | Where-Object { $_.Trim() -ieq "[$ModName]" })) {
        $Lines += "[$ModName]"
        Set-Content -Path $Ini -Value $Lines -Encoding ascii
    }
}

function Disable-ModInIni([string]$Ini) {
    if (-not (Test-Path $Ini)) { return }

    $Out = @()
    $Skipping = $false

    foreach ($Line in Get-Content $Ini) {
        $Trim = $Line.Trim()

        if ($Trim.StartsWith("[")) { $Skipping = $Trim -ieq "[$ModName]" }
        if (-not $Skipping) { $Out += $Line }
    }

    Set-Content -Path $Ini -Value $Out -Encoding ascii
}

function Add-FirewallRule([string]$Exe) {
    $Rule = "HITMAN Co-op (UDP in)"
    $NetshArgs = "advfirewall firewall add rule name=`"$Rule`" dir=in action=allow protocol=UDP program=`"$Exe`" enable=yes"

    try {
        Start-Process -FilePath "netsh.exe" -ArgumentList $NetshArgs -Verb RunAs -Wait -WindowStyle Hidden
        Say "Added a Windows Firewall rule so friends can connect when you host."
    } catch {
        Say "Skipped the firewall rule (Windows will ask the first time you host instead)."
    }
}

function Install-Mod([string]$Game) {
    # Never anywhere but a real game folder.
    if (-not (Test-GameDir $Game)) { throw "No Retail\HITMAN3.exe under '$Game'." }
    $Retail = Join-Path $Game "Retail"

    if (Get-Process -Name "HITMAN3" -ErrorAction SilentlyContinue) {
        throw "HITMAN is running. Close the game and run the installer again."
    }

    $script:LogFile = Join-Path $Retail "HitmanCoop-install.log"
    $Info = Get-GameInfo $Game
    Say "---- $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') installing HITMAN Co-op"
    Say "Game: HITMAN World of Assassination $($Info.Version) ($($Info.Store)) in $Game"

    # The build for this game version: each needs its own mod loader (ZHMModSDK) and DLL.
    $script:Payload = Select-Payload $Info.Version

    if (-not (Test-Path (Join-Path $Payload "mods\$ModName.dll"))) {
        throw "The installer's 'files' folder is missing. Extract the whole zip before running the installer."
    }

    $Build = if ($Payload -like "*files-3.280") { "3.280" } else { "3.290" }
    Say "Installing the HITMAN Co-op build for game $Build.x."

    if ($Info.Version -notlike "$Build*") {
        Say "Note: there's no HITMAN Co-op build for game $($Info.Version) yet; the $Build one is installed. Co-op stays off in the game (it says so) until you update the game; the game itself runs normally."
    }

    # An existing mod loader: ours replaces it (the mod is built for this one), and it is backed up
    # and put back on uninstall.
    $Theirs = Get-FileVersion (Join-Path $Retail "ZHMModSDK.dll")
    $Ours = Get-FileVersion (Join-Path $Payload "ZHMModSDK.dll")
    if ($Theirs -and $Ours -and $Theirs -ne $Ours) {
        Say "Found mod loader (ZHMModSDK) $Theirs; HITMAN Co-op needs $Ours, so it is installed instead. The old one is backed up and comes back if you uninstall."
        if ($Theirs -gt $Ours) { Say "Your other SDK mods were made for $Theirs and may not load with $Ours until you uninstall HITMAN Co-op." }
    }

    $Stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $Backup = Join-Path $Retail "HitmanCoop-backup\$Stamp"
    $Added = @()
    $Replaced = @()
    $Hashes = [ordered]@{}

    # What an earlier install of ours put here (the setup's and this script's manifests): replaced
    # without a backup. Backing those up made the uninstall after an update put the old version's
    # mod loader back instead of the player's own.
    $ManifestPath = Join-Path $Retail $ManifestName
    $Previous = $null
    if (Test-Path $ManifestPath) { try { $Previous = Get-Content $ManifestPath -Raw | ConvertFrom-Json } catch {} }
    function Test-Ours([string]$Rel) {
        if (-not $Previous) { return $false }
        if (@($Previous.added) -contains $Rel) { return $true }
        if ($Previous.files -and ($Previous.files.PSObject.Properties.Name -contains $Rel)) { return $true }
        foreach ($Dir in @($Previous.backups | Where-Object { $_ })) { if (Test-Path (Join-Path (Join-Path $Retail $Dir) $Rel)) { return $true } }
        return $false
    }

    foreach ($File in Get-ChildItem $Payload -Recurse -File) {
        $Relative = $File.FullName.Substring($Payload.Length + 1)
        $Dest = Join-Path $Retail $Relative

        New-Item -ItemType Directory -Force (Split-Path $Dest -Parent) | Out-Null

        $Hashes[$Relative] = (Get-FileHash $File.FullName).Hash.ToLowerInvariant()

        # Already our file from an earlier install: nothing to back up.
        if ((Test-Path $Dest) -and (Get-FileHash $Dest).Hash -eq (Get-FileHash $File.FullName).Hash) {
            continue
        }

        if ((Test-Path $Dest) -and (Test-Ours $Relative)) {
            # One of ours, an older version: just replaced.
        } elseif (Test-Path $Dest) {
            $BackupFile = Join-Path $Backup $Relative
            New-Item -ItemType Directory -Force (Split-Path $BackupFile -Parent) | Out-Null
            Copy-Item $Dest $BackupFile -Force
            $Replaced += $Relative
        } elseif (-not (Test-Ours $Relative)) {
            $Added += $Relative
        }

        Copy-Item $File.FullName $Dest -Force
    }

    Enable-ModInIni (Join-Path $Retail "mods.ini")

    # Everything really there (an antivirus can quietly remove or block dinput8.dll).
    foreach ($File in Get-ChildItem $Payload -Recurse -File) {
        $Dest = Join-Path $Retail $File.FullName.Substring($Payload.Length + 1)
        if (-not (Test-Path $Dest) -or (Get-FileHash $Dest).Hash -ne (Get-FileHash $File.FullName).Hash) {
            throw "Couldn't put $($File.Name) into the game folder. If an antivirus blocked it, allow it (it is the ZHMModSDK mod loader) and install again."
        }
    }

    if (-not (Get-Content (Join-Path $Retail "mods.ini") | Where-Object { $_.Trim() -ieq "[$ModName]" })) {
        throw "Couldn't turn the mod on in Retail\mods.ini."
    }

    # Remember what we did, so uninstall can put things back (the same file the setup writes).
    $Manifest = [ordered]@{
        installed = $Stamp
        version   = (Split-Path -Leaf $PSScriptRoot) -replace '^HitmanCoop-', ''
        build     = $Build
        added     = @($Added + @(if ($Previous) { $Previous.added }) | Where-Object { $_ } | Select-Object -Unique)
        backups   = @(@(if ($Previous) { $Previous.backups }) + @(if ($Replaced.Count) { "HitmanCoop-backup\$Stamp" }) | Where-Object { $_ })
        files     = $Hashes
    }

    $Manifest | ConvertTo-Json | Set-Content -Path $ManifestPath -Encoding utf8

    # Only direct (IP address) connections need this; join codes go out through the co-op server.
    if ($Firewall) {
        Add-FirewallRule (Join-Path $Retail "HITMAN3.exe")
    } elseif (-not $NoFirewall -and -not $Quiet) {
        $Answer = Read-Host "Allow incoming direct connections through Windows Firewall? Join codes work without this. (y/N)"
        if ($Answer -match '^[yY]') { Add-FirewallRule (Join-Path $Retail "HITMAN3.exe") }
    }

    Say ""
    Say "Installed HITMAN Co-op into $Game"
    Say "Start the game, press ~ (or ^ on some keyboards) and click CO-OP to host or join."
    Say "Works with the game online or offline: co-op never uses IOI's servers."
}

function Uninstall-Mod([string]$Game) {
    if (-not (Test-GameDir $Game)) { throw "No Retail\HITMAN3.exe under '$Game'." }
    $Retail = Join-Path $Game "Retail"
    $ManifestPath = Join-Path $Retail $ManifestName

    if (Get-Process -Name "HITMAN3" -ErrorAction SilentlyContinue) {
        throw "HITMAN is running. Close the game and run the uninstaller again."
    }

    Disable-ModInIni (Join-Path $Retail "mods.ini")
    Remove-Item (Join-Path $Retail "mods\$ModName.dll") -Force -ErrorAction SilentlyContinue

    # Keep the SDK loader if other SDK mods are still enabled.
    $OtherMods = @()
    $Ini = Join-Path $Retail "mods.ini"
    if (Test-Path $Ini) {
        $OtherMods = @(Get-Content $Ini | Where-Object { $_.Trim() -match '^\[(.+)\]$' -and $_.Trim() -ine "[sdk]" })
    }

    if (Test-Path $ManifestPath) {
        $Manifest = Get-Content $ManifestPath -Raw | ConvertFrom-Json

        # An empty mods.ini only held our section.
        if ((Test-Path $Ini) -and -not (Get-Content $Ini -Raw)) { Remove-Item $Ini -Force }

        # Files we added (the SDK loader, if it wasn't there before) are removed, unless other SDK
        # mods (added since) still need the loader...
        # (A loader of their own that we replaced comes back from the backup below, with what it had.)
        $HadLoader = @($Manifest.backups | Where-Object { $_ }).Count -gt 0

        if ($OtherMods.Count -gt 0 -and -not $HadLoader) {
            Say "Other SDK mods are enabled, so the mod loader stays."
            Remove-Item (Join-Path $Retail "mods\$ModName.dll") -Force -ErrorAction SilentlyContinue
        } else {
            foreach ($Relative in $Manifest.added) {
                Remove-Item (Join-Path $Retail $Relative) -Force -ErrorAction SilentlyContinue
            }
        }

        # ...and files we replaced get their backups back, oldest first, so the newest of the
        # player's own files wins (only their files are ever backed up).
        foreach ($BackupDir in @($Manifest.backups | Where-Object { $_ }) | Sort-Object) {
            $Source = Join-Path $Retail $BackupDir
            if (-not (Test-Path $Source)) { continue }

            foreach ($File in Get-ChildItem $Source -Recurse -File) {
                $Relative = $File.FullName.Substring($Source.Length + 1)
                Copy-Item $File.FullName (Join-Path $Retail $Relative) -Force
            }
        }

        Remove-Item $ManifestPath -Force
        # Put back, so no longer needed.
        Remove-Item (Join-Path $Retail "HitmanCoop-backup") -Recurse -Force -ErrorAction SilentlyContinue
    }

    Remove-Item (Join-Path $Retail "HitmanCoop-install.log") -Force -ErrorAction SilentlyContinue
    # What the mod wrote while it ran (its settings, HitmanCoop.ini, stay for next time).
    foreach ($Name in "HitmanCoop.log", "HitmanCoop-previous.log", "HitmanCoop-crash.txt", "HitmanCoop-crash-previous.txt", "HitmanCoop-whatsnew.txt") {
        Remove-Item (Join-Path $Retail "mods\$Name") -Force -ErrorAction SilentlyContinue
    }
    Get-ChildItem $Retail -Recurse -Filter *.hcold -File -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue
    $ModsDir = Join-Path $Retail "mods"
    if ((Test-Path $ModsDir) -and -not (Get-ChildItem $ModsDir -Force -ErrorAction SilentlyContinue)) { Remove-Item $ModsDir -Force }
    Say "Removed HITMAN Co-op from $Game"
}

if ($ListGames) {
    Find-AllGames | ForEach-Object { Write-Output $_ }
    exit 0
}

try {
    $Game = Resolve-GamePath

    if ($Uninstall) { Uninstall-Mod $Game } else { Install-Mod $Game }
    exit 0
} catch {
    Say ""
    Say "ERROR: $($_.Exception.Message)"
    exit 1
}
