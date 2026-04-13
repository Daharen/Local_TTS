$ErrorActionPreference = 'Stop'

$RepoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $RepoRoot

function Get-RuntimeConfig {
    $path = Join-Path $RepoRoot 'runtime.local.json'
    if (Test-Path $path) {
        try { return (Get-Content $path -Raw | ConvertFrom-Json) } catch { return $null }
    }
    return $null
}

$config = Get-RuntimeConfig

$defaultLargeDataRoot = 'F:\Local_TTS_Large_Data'
$largeDataRoot = if ($config -and $config.large_data_root) { $config.large_data_root } elseif ($env:LOCAL_TTS_LARGE_DATA_ROOT) { $env:LOCAL_TTS_LARGE_DATA_ROOT } else { $defaultLargeDataRoot }

$defaultWhisperCppRoot = Join-Path $largeDataRoot 'external\whisper.cpp'
$whisperCppRoot = if ($config -and $config.whisper_cpp_root) { $config.whisper_cpp_root } elseif ($env:LOCAL_TTS_WHISPER_CPP_ROOT) { $env:LOCAL_TTS_WHISPER_CPP_ROOT } else { $defaultWhisperCppRoot }

$defaultModelPath = Join-Path $largeDataRoot 'models\whisper.cpp\ggml-base.en.bin'
$modelPath = if ($config -and $config.whisper_model_path) { $config.whisper_model_path } elseif ($env:LOCAL_TTS_WHISPER_MODEL_PATH) { $env:LOCAL_TTS_WHISPER_MODEL_PATH } else { $defaultModelPath }

New-Item -ItemType Directory -Force -Path $largeDataRoot | Out-Null
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $whisperCppRoot) | Out-Null

if (!(Test-Path (Join-Path $whisperCppRoot '.git'))) {
    if (Test-Path $whisperCppRoot) {
        Write-Host "Reusing existing folder at $whisperCppRoot"
    } else {
        git clone https://github.com/ggerganov/whisper.cpp.git $whisperCppRoot
    }
} else {
    Write-Host "whisper.cpp already present at $whisperCppRoot"
}

$buildDir = Join-Path $whisperCppRoot 'build'
cmake -S $whisperCppRoot -B $buildDir
cmake --build $buildDir --config Release

New-Item -ItemType Directory -Force -Path (Split-Path -Parent $modelPath) | Out-Null
if (!(Test-Path $modelPath)) {
    Invoke-WebRequest `
        -Uri 'https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.en.bin?download=true' `
        -OutFile $modelPath
}

Write-Host "whisper_cpp_root: $whisperCppRoot"
Write-Host "whisper_model_path: $modelPath"
