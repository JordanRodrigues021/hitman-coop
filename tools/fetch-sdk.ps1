# Downloads the pinned ZHMModSDK release files from the SDK's official GitHub releases, and the
# DirectX-Headers the SDK's headers include, checks their SHA-256, and extracts them under _deps\.
# Used by the build and by CI.
param(
    [string]$OutDir = (Join-Path $PSScriptRoot "..\_deps")
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$SdkVersion = "v4.1.1"
$SdkUrl = "https://github.com/OrfeasZ/ZHMModSDK/releases/download/$SdkVersion"

# DirectX-Headers is what vcpkg gives the SDK's own build; only its headers are used.
$DxHeadersVersion = "v1.614.0"

$Files = @(
    @{ Name = "DevPkg-ZHMModSDK.zip"; Url = "$SdkUrl/DevPkg-ZHMModSDK.zip"; Sha256 = "fcf42dabe1f5e0015fe50b4cfe50244635ed1d556bdebf8db642e64f4d9fd683"; Dir = "devpkg" },
    @{ Name = "ZHMModSDK-Release.zip"; Url = "$SdkUrl/ZHMModSDK-Release.zip"; Sha256 = "3623ff63e5d388a3b930651af02f0220b425e87ae4bbe6fd48d13b7511aa821a"; Dir = "sdk-release" },
    @{ Name = "DirectX-Headers-$DxHeadersVersion.zip"; Url = "https://github.com/microsoft/DirectX-Headers/archive/refs/tags/$DxHeadersVersion.zip"; Sha256 = "9305df61de9916cc206b3b466fcb0afa4312664ef512f93939242adcfa029554"; Dir = "directx-headers" }
)

New-Item -ItemType Directory -Force $OutDir | Out-Null

foreach ($File in $Files) {
    $Zip = Join-Path $OutDir $File.Name
    $Target = Join-Path $OutDir $File.Dir

    if (-not (Test-Path $Zip)) {
        Write-Host "Downloading $($File.Url)"
        Invoke-WebRequest -Uri $File.Url -OutFile $Zip -UseBasicParsing
    }

    $Hash = (Get-FileHash -Algorithm SHA256 $Zip).Hash.ToLowerInvariant()

    if ($Hash -ne $File.Sha256) {
        Remove-Item $Zip -Force
        throw "SHA-256 mismatch for $($File.Name): got $Hash, expected $($File.Sha256). The file was deleted."
    }

    if (-not (Test-Path $Target)) {
        Expand-Archive -Path $Zip -DestinationPath $Target
    }

    Write-Host "OK  $($File.Name) -> $Target"
}
