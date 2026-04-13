# Local_TTS

Local-first C++ transcription for Windows using external `whisper.cpp` and optional external `llama.cpp`.

## Commands
- `./run.ps1`
- `./run.ps1 transcribe <path-to-audio.wav>`
- `./run.ps1 live`
- `./run.ps1 live-debug`
- `./run.ps1 llm-test "<text>"`

## LLM formatting/correction (enabled by default)
- Live mode runs **Whisper first**, then applies optional local llama.cpp formatting/correction.
- Defaults remain deterministic and enabled (`correction_enabled: true`, `correction_mode: "formatted"`).
- Default GGUF model path is:
  `F:\Qwen3.5-27B\small-model-3b\Qwen2.5-3B-Instruct-IQ4_XS.gguf`

## llm-test
- `./run.ps1 llm-test "<text>"` runs only the local LLM correction layer (no microphone/Whisper needed).
- Useful for validating prompt/model/invocation behavior directly during setup or debugging.
