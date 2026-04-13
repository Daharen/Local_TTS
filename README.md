# Local_TTS

Local-first speech experimentation repository.

This repo stores only source code, lightweight config, and small scripts. Large files (models, corpora, generated artifacts, dumps) must stay outside git at `F:\Local_TTS_Large_Data`.

Large data root resolution priority:
1. `runtime.local.json`
2. `LOCAL_TTS_LARGE_DATA_ROOT` environment variable
3. fallback `F:\Local_TTS_Large_Data`
