#!/usr/bin/env python3
import json
import pathlib
import sys


EXPECTED_SCOPES = {
    "Directory": 3,
    "File": 2,
    "MemoryObject": 7,
    "Process": 9,
    "SharedRing": 8,
    "Stream": 1,
    "TtyControl": 4,
}


def main() -> int:
    generated = pathlib.Path(sys.argv[1])
    for protocol, expected_scope in EXPECTED_SCOPES.items():
        metadata = json.loads((generated / f"{protocol}_metadata.json").read_text(encoding="utf-8"))
        if metadata["scope"] != expected_scope:
            raise AssertionError(f"{protocol}: expected scope {expected_scope}, got {metadata['scope']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
