# Local_TTS

Local-first C++ transcription for Windows using external `whisper.cpp` and optional external `llama.cpp`.

## Commands
- `./run.ps1`
- `./run.ps1 transcribe <path-to-audio.wav>`
- `./run.ps1 live`
- `./run.ps1 live-debug`
- `./run.ps1 llm-test "<text>"`

## Live mode dashboard
- Dashboard support is enabled by default at build time.
- You can still disable it explicitly with `-DLOCAL_TTS_ENABLE_DASHBOARD=OFF`.
- When enabled, the tray menu includes `Dashboard` and `Exit`.
- `Dashboard` opens a Win32 diagnostics window with live dictation timing/session state and recent pipeline events.
- The dashboard is observational only (best-effort): live dictation remains non-blocking even if dashboard open/update paths fail.
- No additional GUI framework dependency is introduced.

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
