#!/usr/bin/env python3
import pathlib
import sys


def main() -> int:
    source_dir = pathlib.Path(sys.argv[1])
    generated_dir = pathlib.Path(sys.argv[2])
    source_json = sorted(source_dir.rglob("*.json"))
    if source_json:
        raise AssertionError(f"generated JSON must not be in IDL source tree: {source_json[0]}")
    if not (generated_dir / "Directory.abi.json").is_file():
        raise AssertionError(f"missing generated ABI manifest: {generated_dir / 'Directory.abi.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
