# Bundle the VC++ runtime app-local, taken from the Visual Studio REDIST folder that matches
# the toolchain - NEVER from this machine's System32.
#
# Why: System32's version drifts with Windows updates and with the CI runner image. That is
# exactly how a 14.51 CRT once shipped and then failed to initialise on older classroom
# machines ("no Qt platform plugin could be initialized"). Taking it from the redist folder
# keeps the compiler and its runtime a matched, licensed pair, the way windeployqt
# --compiler-runtime does it.
#
# The version actually bundled is written to runtime.txt next to the exe (the app echoes it
# into startup.log), and anything newer than the ceiling fails the build so it gets looked at
# deliberately instead of silently. See docs 2.60.
param([Parameter(Mandatory = $true)][string]$Stage)

$ErrorActionPreference = 'Stop'

$files = @('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')
$ceiling = [version]'14.44'

# Candidate redist roots: the environment first, then the usual VS install locations.
$roots = @()
if ($env:VCINSTALLDIR) { $roots += (Join-Path $env:VCINSTALLDIR 'Redist\MSVC') }
$bases = @()
foreach ($p in @("$env:ProgramFiles\Microsoft Visual Studio\2022",
                 "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022",
                 'D:\Microsoft Visual Studio\2022')) {
    if ($p) { $bases += $p }
}
foreach ($base in $bases) {
    if (Test-Path $base) {
        Get-ChildItem $base -Directory -ErrorAction SilentlyContinue | ForEach-Object {
            $roots += (Join-Path $_.FullName 'VC\Redist\MSVC')
        }
    }
}

$dirs = @()
foreach ($root in $roots) {
    if (-not (Test-Path $root)) { continue }
    Get-ChildItem $root -Directory -ErrorAction SilentlyContinue | ForEach-Object {
        $crt = Join-Path $_.FullName 'x64\Microsoft.VC143.CRT'
        if (Test-Path (Join-Path $crt 'msvcp140.dll')) {
            $dirs += [pscustomobject]@{ Dir = $crt; Ver = [version]$_.Name }
        }
    }
}
if ($dirs.Count -eq 0) {
    throw "no Microsoft.VC143.CRT found under: $($roots -join '; ')"
}

# Newest = the toolset this build actually used.
$pick = $dirs | Sort-Object Ver -Descending | Select-Object -First 1

$bundled = @()
foreach ($name in $files) {
    $src = Join-Path $pick.Dir $name
    $ver = [version](Get-Item $src).VersionInfo.FileVersion
    if ($ver -gt $ceiling) {
        throw ("$name is $ver, newer than the verified ceiling $ceiling. Bump the ceiling " +
               "deliberately (tools/bundle-runtime.ps1 + docs 2.60) and re-verify on an old " +
               "Windows 10 build before shipping.")
    }
    Copy-Item $src (Join-Path $Stage $name) -Force
    $bundled += "$name $ver"
}

$text = "VC runtime bundled from: $($pick.Dir)`r`n" + ($bundled -join "`r`n") + "`r`n"
Set-Content -Path (Join-Path $Stage 'runtime.txt') -Value $text -Encoding ASCII
"bundled VC runtime: " + ($bundled -join ', ')
