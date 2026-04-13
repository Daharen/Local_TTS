# Local_TTS

Local-first C++ transcription for Windows using external `whisper.cpp` and optional external `llama.cpp`.

## Commands
- `./run.ps1`
- `./run.ps1 transcribe <path-to-audio.wav>`
- `./run.ps1 live`
- `./run.ps1 live-debug`
- `./run.ps1 llm-test "<text>"`

## Whisper-first + local LLM cleanup
- Pipeline remains **Whisper first**, then optional local llama.cpp correction/formatting.
- Short transcripts still run one-shot correction.
- Long transcripts are segmented and corrected chunk-by-chunk, then deterministically merged.
- Long-form dictation no longer depends on one tiny `-n 128` rewrite.

## Correction config knobs
- `correction_max_output_tokens` (default `512`)
- `correction_segment_max_chars` (default `1600`)
- `correction_segment_overlap_chars` (default `200`)
- `correction_force_segmentation_threshold_chars` (default `1800`)
