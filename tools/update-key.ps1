# The key that signs the self-updater's update.txt (ECDSA P-256). The mod only takes an update whose
# update.txt is signed with it (plugin/Src/UpdateKey.h has the public half), so nobody who gets at
# the GitHub repo or its file server can push files into players' games.
#
#   tools\update-key.ps1 -Create         makes the key (once) in .signing\ and prints UpdateKey.h
#   tools\update-key.ps1 -Sign <file>    writes <file>.sig (used by tools\package.ps1)
#   tools\update-key.ps1 -Verify <file>  checks <file>.sig
#
# .signing\ never leaves this PC (tools\sync-github.ps1 skips it). Keep a backup of it somewhere
# safe: without the key, released mods can't update themselves any more.
param([switch]$Create, [string]$Sign = "", [string]$Verify = "")

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Core
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$KeyFile = Join-Path $Root ".signing\update-key.blob"

function Get-Key {
    if (-not (Test-Path $KeyFile)) { throw "No signing key at $KeyFile (tools\update-key.ps1 -Create makes one; restore it from your backup if you had one)." }
    return [System.Security.Cryptography.CngKey]::Import([IO.File]::ReadAllBytes($KeyFile), [System.Security.Cryptography.CngKeyBlobFormat]::EccPrivateBlob)
}

if ($Create) {
    if (Test-Path $KeyFile) { throw "$KeyFile already exists: one key for every release. Delete it only if you mean to start over (old mods then can't update)." }
    New-Item -ItemType Directory -Force (Split-Path $KeyFile) | Out-Null
    $Params = New-Object System.Security.Cryptography.CngKeyCreationParameters
    $Params.ExportPolicy = [System.Security.Cryptography.CngExportPolicies]::AllowPlaintextExport
    $Key = [System.Security.Cryptography.CngKey]::Create([System.Security.Cryptography.CngAlgorithm]::ECDsaP256, $null, $Params)
    [IO.File]::WriteAllBytes($KeyFile, $Key.Export([System.Security.Cryptography.CngKeyBlobFormat]::EccPrivateBlob))
}

if ($Create -or (-not $Sign -and -not $Verify)) {
    $Public = (Get-Key).Export([System.Security.Cryptography.CngKeyBlobFormat]::EccPublicBlob)
    $Hex = ($Public | ForEach-Object { "0x{0:x2}" -f $_ }) -join ", "
    Write-Output "// The public half of the update signing key (tools\update-key.ps1): a BCRYPT_ECCPUBLIC_BLOB."
    Write-Output "constexpr unsigned char c_UpdateKey[$($Public.Length)] = { $Hex };"
    exit 0
}

$Ecdsa = New-Object System.Security.Cryptography.ECDsaCng (Get-Key)
$Ecdsa.HashAlgorithm = [System.Security.Cryptography.CngAlgorithm]::Sha256

if ($Sign) {
    $Signature = $Ecdsa.SignData([IO.File]::ReadAllBytes((Resolve-Path $Sign).Path))
    [IO.File]::WriteAllText("$((Resolve-Path $Sign).Path).sig", [Convert]::ToBase64String($Signature) + "`n", (New-Object System.Text.UTF8Encoding $false))
    Write-Output "Signed $Sign"
}

if ($Verify) {
    $Path = (Resolve-Path $Verify).Path
    $Ok = $Ecdsa.VerifyData([IO.File]::ReadAllBytes($Path), [Convert]::FromBase64String([IO.File]::ReadAllText("$Path.sig").Trim()))
    if (-not $Ok) { throw "$Verify.sig doesn't match." }
    Write-Output "$Verify is signed"
}
