# Local_TTS

Local-first C++ transcription for Windows using external `whisper.cpp` and optional external `llama.cpp`.

## LLM correction backend
- Correction now prefers a resident local `llama.cpp` server backend to avoid per-request model/process startup.
- `llama.cpp` and GGUF model paths remain external/config-driven (`llama_cpp_root`, `llama_model_path`); the repo does not store model files.
- Resident mode is enabled by default and attempts GPU offload (`correction_resident_gpu_layers`) so the model can remain loaded between corrections.
- If resident startup or request handling fails, correction falls back to one-shot subprocess behavior automatically.
- Resident/backend tuning is controlled through runtime config fields:
  - `correction_backend_mode`
  - `correction_resident_enabled`
  - `correction_resident_host`
  - `correction_resident_port`
  - `correction_resident_ctx_size`
  - `correction_resident_gpu_layers`
  - `correction_resident_threads`
  - `correction_resident_startup_timeout_ms`
  - `correction_resident_request_timeout_ms`

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
