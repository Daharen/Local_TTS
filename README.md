# Local_TTS

Local-first C++ transcription for Windows using external `whisper.cpp` and optional external `llama.cpp`.

## LLM correction backend
- Correction defaults to one-shot mode (`correction_backend_mode: "oneshot"`) as the current stable path.
- `llama.cpp` and GGUF model paths remain external/config-driven (`llama_cpp_root`, `llama_model_path`); the repo does not store model files.
- Resident mode remains available as an override (`correction_backend_mode: "resident"`).
- Resident requests are now explicitly non-streaming and run under a strict total budget. On startup/probe/request timeout or failure, resident state is reset and correction automatically falls back to one-shot.
- Correction diagnostics surface backend attribution and resident startup/probe/request/fallback/reset details with compact, bounded fields.
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
  - `correction_resident_total_budget_ms`
  - `correction_resident_per_attempt_timeout_ms`

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
- `pipeline_debug_enabled` controls an additional common debug log written to `<large_data_root>/output/debug/pipeline.common.log`.

Resolution order for effective values:
1. Environment variables
2. `runtime.local.json`
3. `runtime.repo.json`
4. Centralized C++ fallback defaults
