# Local_TTS

Minimal local-first C++ transcription app for Windows, using external `whisper.cpp` outside this repo.

Large assets stay outside git at `F:\Local_TTS_Large_Data` (or your configured large-data root).

## Commands
- Print resolved paths JSON:
  - `./run.ps1`
- Transcribe a WAV file:
  - `./run.ps1 transcribe <path-to-audio.wav>`
- Start resident live dictation mode (Windows-only):
  - `./run.ps1 live`

## Live mode (first pass)
- Windows-only hidden tray app.
- Hold `Ctrl+Alt` to record microphone audio.
- Release either key to stop recording and transcribe locally.
- Transcript is inserted into the target app using clipboard + `Ctrl+V`.
- Transcript is appended to:
  - `<large_data_root>/output/live_transcripts/session.txt`
