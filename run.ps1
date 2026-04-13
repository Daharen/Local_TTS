$RepoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $RepoRoot

$BuildDir = Join-Path $RepoRoot "build"
cmake -S $RepoRoot -B $BuildDir
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

cmake --build $BuildDir --config Release
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$exeRelease = Join-Path $BuildDir "Release\local_tts.exe"
$exeFlat = Join-Path $BuildDir "local_tts.exe"

if (Test-Path $exeRelease) {
    & $exeRelease
} elseif (Test-Path $exeFlat) {
    & $exeFlat
} else {
    Write-Error "local_tts executable not found in expected build locations."
    exit 1
}
