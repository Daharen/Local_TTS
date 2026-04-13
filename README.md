# Local_TTS

Local_TTS is a local-first speech experimentation repository.

The bootstrap/runtime baseline is now C++-first. The repo contains source code, config, and scripts only.

Large files do not belong in git; keep them at `F:\Local_TTS_Large_Data`.

Large-data root resolution priority:
1. `runtime.local.json`
2. `LOCAL_TTS_LARGE_DATA_ROOT`
3. fallback `F:\Local_TTS_Large_Data`
