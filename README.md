# Local_TTS

Minimal local-first C++ transcription scaffold for Windows.

This first runnable integration is **file-based transcription** only. It invokes `whisper.cpp` as an **external C++ dependency** stored outside this repo.

Large assets stay outside git at:
- `F:\Local_TTS_Large_Data`

## Local workflow
1. Run setup (clones/builds `whisper.cpp`, downloads model outside repo):
   - `./setup_whisper_cpp.ps1`
2. Build and run Local_TTS (prints resolved paths):
   - `./run.ps1`
3. Transcribe a WAV file locally:
   - `./run.ps1 transcribe <path-to-audio.wav>`

## Notes
- No microphone capture yet.
- No punctuation cleanup/rewrite layer.
- No GUI.
