$RepoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$Python = Join-Path $RepoRoot ".venv\Scripts\python.exe"
if (-not (Test-Path $Python)) { $Python = "py -3.12" }

if ($Python -is [string] -and $Python.StartsWith("py ")) {
    & py -3.12 -m local_tts.cli
} else {
    & $Python -m local_tts.cli
}
