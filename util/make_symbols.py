#!/usr/bin/env python3
"""Generate the optional userland/debug ksybs symbol file."""

from __future__ import annotations

import argparse
import datetime
import os
import struct
import traceback
from pathlib import Path

from build_paths import build_paths, resolve_build_directory
from mod import run_shell


CACHE_FILE_NAME = ".ksybs_cache.log"


def gen_symbols(source_file: str, target_file: str, force: bool) -> None:
    target = Path(target_file).resolve()
    target.parent.mkdir(parents=True, exist_ok=True)
    cache_path = target.parent / CACHE_FILE_NAME

    if cache_path.exists() and not force:
        cache_line = cache_path.read_text(encoding="utf-8").strip().split("?", 1)
        source_timestamp = datetime.datetime.fromtimestamp(os.path.getmtime(source_file)).strftime(
            "%Y-%m-%d %H:%M:%S.%f"
        )
        if len(cache_line) == 2 and cache_line[0].strip() == source_file and cache_line[1].strip() == source_timestamp:
            print("ksybs is cached. do nothing.")
            return

    symbols = run_shell(f'nm "{source_file}" -C | sort | uniq', None, False).splitlines(False)
    with target.open("wb") as output:
        output.write(struct.pack("Q", 0xF0EAEACC))
        output.write(struct.pack("Q", 1))
        output.write(struct.pack("Q", len(symbols)))

        entries = []
        offset = 0
        for line in symbols:
            if not line.strip():
                continue
            values = line.strip().split(" ")
            address = int(values[0].strip(), 16)
            name = " ".join(values[2:])
            type_name = values[1].strip()
            output.write(struct.pack("Q", address))
            output.write(struct.pack("Q", offset))
            entries.append((name, offset, type_name))
            offset += len(name.encode("utf-8")) + 2

        for name, _, type_name in entries:
            output.write(struct.pack("c", type_name.encode("utf-8")))
            output.write(struct.pack(f"{len(name) + 1}s", name.encode("utf-8")))

    cache_path.write_text(
        source_file
        + "?"
        + datetime.datetime.fromtimestamp(os.path.getmtime(source_file)).strftime("%Y-%m-%d %H:%M:%S.%f"),
        encoding="utf-8",
    )
    print(f"make {target} success")


def main() -> int:
    parser = argparse.ArgumentParser(description="generate optional ksybs debug data")
    parser.add_argument("--build-dir")
    parser.add_argument("-f", "--force", action="store_true")
    parser.add_argument("-o", "--output")
    parser.add_argument("-i", "--input")
    args = parser.parse_args()
    try:
        paths = build_paths(resolve_build_directory(args.build_dir))
        source_file = args.input or str(paths.debug_dir / "system" / "kernel.dbg")
        target_file = args.output or str(paths.rootfs_dir / "data" / "ksybs")
        gen_symbols(source_file, target_file, args.force)
    except Exception:
        traceback.print_exc()
        parser.print_help()
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
