from __future__ import annotations

import json
import os
from pathlib import Path

_DEFAULT_LARGE_DATA_ROOT = Path(r"F:\Local_TTS_Large_Data")


def get_repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def _load_runtime_override(repo_root: Path) -> Path | None:
    config_path = repo_root / "runtime.local.json"
    if not config_path.is_file():
        return None
    data = json.loads(config_path.read_text(encoding="utf-8"))
    value = data.get("large_data_root")
    if isinstance(value, str) and value.strip():
        return Path(value)
    return None


def get_large_data_root() -> Path:
    repo_root = get_repo_root()
    runtime_override = _load_runtime_override(repo_root)
    if runtime_override is not None:
        return runtime_override
    env_override = os.getenv("LOCAL_TTS_LARGE_DATA_ROOT")
    if env_override:
        return Path(env_override)
    return _DEFAULT_LARGE_DATA_ROOT


def describe_paths() -> dict[str, str]:
    return {
        "repo_root": str(get_repo_root()),
        "large_data_root": str(get_large_data_root()),
    }
