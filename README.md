# Local_TTS

Minimal local-first C++ transcription app for Windows, using external `whisper.cpp` and optional external `llama.cpp` outside this repo.

## Commands
- Print resolved paths JSON:
  - `./run.ps1`
- Transcribe a WAV file:
  - `./run.ps1 transcribe <path-to-audio.wav>`
- Start resident live dictation mode (Windows-only, hidden tray behavior):
  - `./run.ps1 live`
- Start live dictation with console diagnostics:
  - `./run.ps1 live-debug`
- Directly test local LLM correction:
  - `./run.ps1 llm-test "<text>"`

## Correction layer
- The correction layer runs on finalized Whisper text only.
- It is **disabled by default** (`correction_enabled: false`).
- Enable it in `runtime.local.json` by setting:
  - `correction_enabled: true`
  - `llama_cpp_root`
  - `llama_model_path`
- Decoding defaults stay deterministic (`temperature=0.0`, `top_k=1`, `top_p=0.0`, `min_p=0.0`).

## Live behavior
- Hold `Ctrl+Alt` to record; release to transcribe and insert.
- `live-debug` prints machine-readable stage markers to the console.
- Insertion remains non-disruptive: clipboard Unicode text + `SendInput` Ctrl+V with no restore/resize/move behavior.
