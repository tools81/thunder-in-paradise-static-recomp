<#
  Builds the release packages in dist\:

    ThunderInParadise-<version>-portable.zip   unzip anywhere and run (also what
                                               the in-game updater downloads)
    ThunderInParadise-<version>-setup.exe      single-file installer (Inno Setup 6)

  The executable is built WITHOUT the translated game code (CMake option
  RECOMP_INTERP_ONLY): it reads the game's program from the player's disc
  and runs it in the 68000 interpreter, so the packages contain nothing of
  the game. Output and saves are identical to the recompiled build.

  Needs: CMake + Visual Studio 2022 C++ build tools, Inno Setup 6
  (winget install JRSoftware.InnoSetup) for the installer.

    powershell -ExecutionPolicy Bypass -File tools\package.ps1 [-SkipInstaller]
#>
param([switch]$SkipInstaller)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$ver = (Select-String -Path runtime\version.h -Pattern '#define APP_VERSION "([^"]+)"').Matches[0].Groups[1].Value
Write-Host "Packaging Thunder in Paradise $ver"

# 1. executable (interpreter-only build)
cmake -S . -B build\release -G "Visual Studio 17 2022" -A x64 -DRECOMP_INTERP_ONLY=ON | Out-Null
if ($LASTEXITCODE) { throw 'cmake configure failed' }
cmake --build build\release --config Release --target game --parallel
if ($LASTEXITCODE) { throw 'build failed' }

# 2. staging folder: exactly what the user gets (no saves\, no game files)
$stage = 'dist\stage'
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item build\release\Release\thunder.exe $stage
Copy-Item installer\README.txt, installer\THIRD-PARTY.txt $stage

# 3. portable zip (files at the top level: the updater unpacks it over the install folder)
$zip = "dist\ThunderInParadise-$ver-portable.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path "$stage\*" -DestinationPath $zip -CompressionLevel Optimal
Write-Host "  $zip"

# 4. installer
if (-not $SkipInstaller) {
    $iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
              "$env:ProgramFiles\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $iscc) { Write-Warning 'Inno Setup 6 not found (winget install JRSoftware.InnoSetup): installer skipped' }
    else {
        & $iscc /Q "/DAppVersion=$ver" "/DDistDir=$root\$stage" "/DOutDir=$root\dist" installer\thunder.iss
        if ($LASTEXITCODE) { throw 'ISCC failed' }
        Write-Host "  dist\ThunderInParadise-$ver-setup.exe"
    }
}
