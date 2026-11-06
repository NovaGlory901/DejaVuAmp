# Builds, tests and installs DejaVuAmp on Windows (MSYS2 UCRT64 toolchain). Run from anywhere:
#
#   .\build.ps1                      build + run the tests (output in ..\build-windows)
#   .\build.ps1 -Install             ... and install into %USERPROFILE%\DejaVuAmp plus a Start menu entry
#   .\build.ps1 -Install -SkipTests  faster
#   .\build.ps1 -Uninstall           remove the program, its DLLs and the Start menu entry (your songs, equalizer presets, themes and settings stay)
#
# Options: -Prefix DIR (another install folder), -Msys2 DIR (MSYS2 is not in C:\msys64)
# One-time setup, in an MSYS2 UCRT64 shell:
#   pacman -S --needed mingw-w64-ucrt-x86_64-{toolchain,cmake,ninja,SDL2,ffmpeg,pkgconf,angleproject}
[CmdletBinding()]
param(
    [switch]$Install,
    [switch]$Uninstall,
    [switch]$SkipTests,
    [string]$Prefix = (Join-Path $env:USERPROFILE 'DejaVuAmp'),
    [string]$Msys2 = 'C:\msys64'
)
$ErrorActionPreference = 'Stop'

$src = Split-Path -Parent $PSScriptRoot
$build = Join-Path $src 'build-windows'
$bin = Join-Path $Msys2 'ucrt64\bin'
$exe = Join-Path $Prefix 'DejaVuAmp.exe'
$shortcut = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\DejaVuAmp.lnk'

function Stop-IfRunning {
    $running = Get-Process DejaVuAmp -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $exe }
    if ($running) { throw "DejaVuAmp is running from $Prefix. Close its window first, then run this again." }
}
function Step($what) { Write-Host "== $what ==" -ForegroundColor Cyan }
function Check($what) { if ($LASTEXITCODE -ne 0) { throw "$what failed (exit code $LASTEXITCODE)" } }

if ($Uninstall) {
    Stop-IfRunning
    Remove-Item $exe -ErrorAction SilentlyContinue
    Get-ChildItem $Prefix -Filter *.dll -File -ErrorAction SilentlyContinue | Remove-Item
    Remove-Item (Join-Path $Prefix 'README.md') -ErrorAction SilentlyContinue
    # the Start menu entry goes only if it belongs to this install (another copy may have its own)
    if (Test-Path $shortcut) {
        $target = (New-Object -ComObject WScript.Shell).CreateShortcut($shortcut).TargetPath
        if ($target -eq $exe) { Remove-Item $shortcut }
    }
    Write-Host "Removed the program, its DLLs and this install's Start menu entry. Your folders in $Prefix are left alone."
    return
}

if (-not (Test-Path (Join-Path $bin 'g++.exe'))) { throw "No MSYS2 UCRT64 toolchain at $bin. See the setup line at the top of this script (or pass -Msys2)." }
$env:PATH = "$bin;$env:PATH"      # for the build, the tests, and the DLLs the tests load

Step 'configuring'
cmake -S $src -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release
Check 'configure'
Step 'building'
cmake --build $build
Check 'build'
if (-not (Test-Path (Join-Path $build 'DejaVuAmp.exe'))) { throw 'DejaVuAmp.exe was not built (SDL2 or the ANGLE package is missing: see the setup line at the top).' }

if (-not $SkipTests) {
    Step 'testing (the window test opens a window for a moment)'
    ctest --test-dir $build --output-on-failure
    Check 'tests'
}

if ($Install) {
    Stop-IfRunning
    Step "installing into $Prefix"
    cmake --install $build --prefix $Prefix | Out-Null
    Check 'install'
    if ($PSBoundParameters.ContainsKey('Prefix')) {
        Write-Host "DejaVuAmp is installed: $exe (no Start menu entry for a custom -Prefix)" -ForegroundColor Green
        return
    }
    $link = (New-Object -ComObject WScript.Shell).CreateShortcut($shortcut)
    $link.TargetPath = $exe
    $link.WorkingDirectory = $Prefix
    $link.Description = 'Music player with an equalizer'
    $link.Save()
    Write-Host "DejaVuAmp is installed: $exe" -ForegroundColor Green
    Write-Host "Start menu: DejaVuAmp.  Folders to fill: $Prefix\music, $Prefix\eq-presets, $Prefix\themes"
}
