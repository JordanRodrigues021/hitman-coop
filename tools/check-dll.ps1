# Checks that the built HitmanCoop.dll will load next to the shipped ZHMModSDK, without the game:
# - it exports the three functions the SDK's mod loader looks up,
# - every symbol it imports from ZHMModSDK.dll is exported by the ZHMModSDK.dll players get,
# - it only depends on DLLs that every Windows install has (no Visual C++ runtime needed).
param(
    [string]$ModDll = (Join-Path $PSScriptRoot "..\_build\install\mods\HitmanCoop.dll"),
    [string]$SdkDll = (Join-Path $PSScriptRoot "..\_deps\sdk-release\ZHMModSDK.dll")
)

$ErrorActionPreference = "Stop"

function Find-Dumpbin {
    $VsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    foreach ($Vs in & $VsWhere -products * -all -property installationPath) {
        $Found = Get-ChildItem (Join-Path $Vs "VC\Tools\MSVC") -Recurse -Filter dumpbin.exe -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match "Hostx64\\x64" } | Select-Object -First 1
        if ($Found) { return $Found.FullName }
    }
    throw "dumpbin.exe not found. Install Visual Studio (Build Tools) with the C++ workload."
}

function Get-Section([string[]]$Lines, [string]$Start) {
    # dumpbin /imports prints one block per DLL: "    NAME.dll", a header, then indented symbol lines.
    $In = $false
    foreach ($Line in $Lines) {
        if ($Line -match '^\s{4}(\S+\.dll)\s*$') { $In = ($Matches[1] -ieq $Start); continue }
        if ($In -and $Line -match '^\s+[0-9A-F]+\s+(\S+)\s*$') { $Matches[1] }
    }
}

$Dumpbin = Find-Dumpbin
$Failures = 0
function Check([bool]$Ok, [string]$What) {
    if ($Ok) { Write-Host "PASS  $What" } else { Write-Host "FAIL  $What"; $script:Failures++ }
}

$ModExports = & $Dumpbin /nologo /exports $ModDll
foreach ($Name in "GetPluginInterface", "CompiledSdkVersion", "CompiledSdkAbiVersion") {
    Check ([bool]($ModExports | Select-String "^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]+\s+$Name(\s|$)")) "exports $Name"
}

$Deps = (& $Dumpbin /nologo /dependents $ModDll) | Where-Object { $_ -match '^\s+\S+\.dll\s*$' } | ForEach-Object { $_.Trim() }
$Runtime = $Deps | Where-Object { $_ -match '^(vcruntime|msvcp|ucrtbase|api-ms-win-crt|concrt)' }
Check (-not $Runtime) "no Visual C++ runtime dependency ($($Deps -join ', '))"

# The SDK's own mods import ZHMModSDK, KERNEL32, USER32, SHELL32 and IMM32 (ImGui) too. WINHTTP
# and bcrypt (Windows' own, on every PC) are the self-updater's download and SHA-256.
$Allowed = "ZHMModSDK.dll", "KERNEL32.dll", "USER32.dll", "WS2_32.dll", "VERSION.dll", "ADVAPI32.dll", "SHELL32.dll", "IMM32.dll", "ole32.dll", "WINHTTP.dll", "bcrypt.dll"
$Unknown = $Deps | Where-Object { $_ -notin $Allowed }
Check (-not $Unknown) "depends only on the SDK and Windows DLLs$(if ($Unknown) { ': unexpected ' + ($Unknown -join ', ') })"

$Imported = @(Get-Section (& $Dumpbin /nologo /imports $ModDll) "ZHMModSDK.dll")
$SdkExports = @{}
foreach ($Line in & $Dumpbin /nologo /exports $SdkDll) {
    if ($Line -match '^\s+\d+\s+[0-9A-F]+\s+[0-9A-F]+\s+(\S+)') { $SdkExports[$Matches[1]] = $true }
}

$Missing = $Imported | Where-Object { -not $SdkExports.ContainsKey($_) }
Check ($Imported.Count -gt 0 -and -not $Missing) "all $($Imported.Count) SDK imports exported by the shipped ZHMModSDK.dll ($($SdkExports.Count) exports)"
foreach ($Name in $Missing) { Write-Host "      missing: $Name" }

# The loader refuses mods built for another SDK ABI. Compare with one of the SDK's own mods,
# reading both without running them (DONT_RESOLVE_DLL_REFERENCES skips DllMain and imports).
Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public static class ZhmModProbe {
    [DllImport("kernel32", SetLastError = true, CharSet = CharSet.Unicode)] static extern IntPtr LoadLibraryExW(string p, IntPtr h, uint f);
    [DllImport("kernel32")] static extern IntPtr GetProcAddress(IntPtr m, string n);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate IntPtr StrFn();
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)] delegate int IntFn();
    public static string Read(string p_Path) {
        IntPtr s_Module = LoadLibraryExW(p_Path, IntPtr.Zero, 1);
        if (s_Module == IntPtr.Zero) return null;
        IntPtr s_Ver = GetProcAddress(s_Module, "CompiledSdkVersion"), s_Abi = GetProcAddress(s_Module, "CompiledSdkAbiVersion");
        if (s_Ver == IntPtr.Zero || s_Abi == IntPtr.Zero) return null;
        return Marshal.PtrToStringAnsi(((StrFn)Marshal.GetDelegateForFunctionPointer(s_Ver, typeof(StrFn)))()) + " ABI " +
            ((IntFn)Marshal.GetDelegateForFunctionPointer(s_Abi, typeof(IntFn)))();
    }
}
'@
$Reference = Get-ChildItem (Join-Path (Split-Path $SdkDll) "mods") -Filter *.dll | Select-Object -First 1
$Ours = [ZhmModProbe]::Read((Resolve-Path $ModDll).Path)
$Theirs = if ($Reference) { [ZhmModProbe]::Read($Reference.FullName) }
Check ($Ours -and $Ours -eq $Theirs) "built for SDK $Ours (shipped $($Reference.Name): SDK $Theirs)"

if ($Failures) { throw "$Failures check(s) failed." }
Write-Host "HitmanCoop.dll matches the shipped ZHMModSDK.dll."
