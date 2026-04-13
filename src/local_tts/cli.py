import json

from local_tts.paths import describe_paths


if __name__ == "__main__":
    print(json.dumps(describe_paths(), indent=2))
