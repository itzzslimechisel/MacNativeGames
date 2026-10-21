# Windows counterpart of publish_native.sh (docs/windows-port-plan.md C10): builds the shader translation service
# (totk_xlate --service, MSL or SPIR-V chain) from vendor/ryujinx + patches/*.patch + this directory's sources.
#   build_win.ps1                 -> build/totk_xlate-win/totk_xlate.dll (JIT build; run with dotnet)
#   build_win.ps1 -Aot -Rid win-x64   -> build/totk_xlate-win/aot-win-x64/totk_xlate.exe (NativeAOT, needs the MSVC linker)
# Needs the .NET 10 SDK (-Dotnet <path to dotnet.exe>; default: dotnet on PATH, then %LOCALAPPDATA%\dotnet10) and git
# (patches are applied with git apply).
param([string]$Dotnet = "", [switch]$Aot, [string]$Rid = "win-x64")
$ErrorActionPreference = "Continue"   # native tools write progress to stderr; failures are caught by exit code
$here = $PSScriptRoot
$repo = (Resolve-Path "$here\..\..\..\..").Path
if (-not $Dotnet) {
    $cand = Get-Command dotnet -ErrorAction SilentlyContinue
    if ($cand -and (& $cand.Source --list-sdks | Select-String '^10\.')) { $Dotnet = $cand.Source }
    elseif (Test-Path "$env:LOCALAPPDATA\dotnet10\dotnet.exe") { $Dotnet = "$env:LOCALAPPDATA\dotnet10\dotnet.exe" }
    else { throw "no .NET 10 SDK found (pass -Dotnet)" }
}
$work = "$repo\build\totk_xlate_work"
$out = "$repo\build\totk_xlate-win"
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory -Force "$work\ryujinx-src", "$work\xlate" | Out-Null
Copy-Item -Recurse "$repo\vendor\ryujinx\*" "$work\ryujinx-src\"
Get-ChildItem "$work\ryujinx-src\src" -Directory -Recurse -Depth 1 | Where-Object { $_.Name -in 'obj', 'bin' } | Remove-Item -Recurse -Force
Push-Location "$work\ryujinx-src"
try {
    foreach ($p in Get-ChildItem "$here\patches\*.patch" | Sort-Object Name) {
        git apply --whitespace=nowarn --ignore-whitespace $p.FullName   # line endings differ between checkouts
        if ($LASTEXITCODE) { throw "patch $($p.Name) failed" }
    }
} finally { Pop-Location }
Copy-Item "$here\*.cs", "$here\totk_xlate.csproj" "$work\xlate\"
if ($Aot) {
    & $Dotnet publish "$work\xlate\totk_xlate.csproj" -c Release -r $Rid -p:PublishAot=true -p:InvariantGlobalization=true `
        "-p:RyuSrc=$work\ryujinx-src" -o "$out\aot-$Rid"
} else {
    & $Dotnet build "$work\xlate\totk_xlate.csproj" -c Release "-p:RyuSrc=$work\ryujinx-src" -o $out
}
if ($LASTEXITCODE) { throw "dotnet failed ($LASTEXITCODE)" }
Write-Host "built $out"
