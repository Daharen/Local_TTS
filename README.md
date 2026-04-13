# Local_TTS

Minimal local-first C++ transcription app for Windows, using external `whisper.cpp` and optional external `llama.cpp` outside this repo.

Large assets stay outside git at `F:\Local_TTS_Large_Data` (or your configured large-data root).

## Commands
- Print resolved paths JSON:
  - `./run.ps1`
- Transcribe a WAV file:
  - `./run.ps1 transcribe <path-to-audio.wav>`
- Start resident live dictation mode (Windows-only):
  - `./run.ps1 live`

## Live mode
- Windows-only hidden tray app.
- Hold `Ctrl+Alt` to record microphone audio.
- Release either key to stop recording and transcribe locally.
- Text insertion uses clipboard + `Ctrl+V` and avoids restore/resize/move window behavior.
- If focus cannot be safely returned, paste is skipped and logged.
- Optional post-transcription correction layer (disabled by default) runs only on finalized Whisper text.
- Correction uses external `llama.cpp` with a local GGUF model.
- Default correction model path:
  - `F:\Qwen3.5-27B\small-model-3b\Qwen2.5-3B-Instruct-IQ4_XS.gguf`
- Transcript log path:
  - `<large_data_root>/output/live_transcripts/session.txt`
