param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$ArgsFromUser
)

$RepoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $RepoRoot

$BuildDir = Join-Path $RepoRoot "build"
cmake -S $RepoRoot -B $BuildDir
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

cmake --build $BuildDir --config Release
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$exeRelease = Join-Path $BuildDir "Release\local_tts.exe"
$exeFlat = Join-Path $BuildDir "local_tts.exe"

$ExePath = $null
if (Test-Path $exeRelease) {
    $ExePath = $exeRelease
} elseif (Test-Path $exeFlat) {
    $ExePath = $exeFlat
} else {
    Write-Error "local_tts executable not found in expected build locations."
    exit 1
}

if ($ArgsFromUser.Count -eq 0) {
    & $ExePath
} else {
    & $ExePath @ArgsFromUser
}

exit $LASTEXITCODE
