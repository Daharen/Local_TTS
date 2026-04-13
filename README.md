# Local_TTS

Local-first C++ transcription for Windows using external `whisper.cpp` and optional external `llama.cpp`.

## Commands
- `./run.ps1`
- `./run.ps1 transcribe <path-to-audio.wav>`
- `./run.ps1 live`
- `./run.ps1 live-debug`
- `./run.ps1 llm-test "<text>"`

## LLM formatting layer (enabled by default)
- Finalized Whisper text now goes through a default-on formatting layer (`correction_enabled: true`).
- The layer does conservative grammar/punctuation cleanup plus readability formatting.
- It may add line breaks, paragraph breaks, indentation, and light structure when warranted.
- `correction_mode: "formatted"` (default) keeps structure minimal and readability-focused.
- `correction_mode: "notes"` is more willing to emit bullet/list structure when content supports it.
- Switch modes in `runtime.local.json` (or `LOCAL_TTS_CORRECTION_MODE`).

## Diagnostics
- Direct formatting test: `./run.ps1 llm-test "<text>"`
- Live debug mode: `./run.ps1 live-debug`
- Live mode remains non-disruptive to target windows (no restore/resize/move behavior).
