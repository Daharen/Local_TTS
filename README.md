# Local_TTS

Local-first C++ transcription for Windows using external `whisper.cpp` and optional external `llama.cpp`.

## Commands
- `./run.ps1`
- `./run.ps1 transcribe <path-to-audio.wav>`
- `./run.ps1 live`
- `./run.ps1 live-debug`
- `./run.ps1 llm-test "<text>"`

## Live mode dashboard
- When dashboard support is enabled, the tray menu includes `Dashboard` and `Exit`.
- `Dashboard` currently opens a blank Win32 verification window titled `Local TTS Dashboard`.
- Richer dashboard contents will be added in a later pass.
- Live dictation remains non-blocking and keeps running even if dashboard creation or diagnostics updates fail.
- No additional GUI framework dependency is introduced.
- Dashboard support is optional and disabled by default at build time.
- Enable it explicitly with `-DLOCAL_TTS_ENABLE_DASHBOARD=ON`.

## Runtime config
- `runtime.repo.json` is the committed central settings file for normal tuning.
- `runtime.local.json` is an optional local override file (same schema, git-ignored).
- `runtime.local.json.example` mirrors the full schema.
- Prefer editing config files instead of changing code for runtime tuning.

Resolution order for effective values:
1. Environment variables
2. `runtime.local.json`
3. `runtime.repo.json`
4. Centralized C++ fallback defaults
