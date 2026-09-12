#!/usr/bin/env python3
"""Build NaOS Rust artifacts with a content-aware Cargo target directory.

Cargo tracks ordinary crate sources, but build-std can reuse a previously
compiled custom std even after a file under the Rust fork changes.  The CMake
Rust target calls this wrapper so any change in the workspace, IDL, custom
std, or vendored async runtime invalidates the exact target directory before
Cargo is invoked.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import subprocess
import sys
from pathlib import Path


def input_files(root: Path) -> list[Path]:
    if root.is_file():
        return [root]
    if not root.is_dir():
        raise FileNotFoundError(root)
    return sorted(path for path in root.rglob("*") if path.is_file())


def fingerprint(roots: list[Path]) -> str:
    digest = hashlib.sha256()
    for root in roots:
        root = root.resolve()
        digest.update(b"root\0")
        digest.update(str(root).encode())
        digest.update(b"\0")
        for path in input_files(root):
            relative = path.relative_to(root) if root.is_dir() else Path(path.name)
            digest.update(str(relative).encode())
            digest.update(b"\0")
            digest.update(path.read_bytes())
            digest.update(b"\0")
    return digest.hexdigest()


def parse_args(argv: list[str]) -> tuple[argparse.Namespace, list[str]]:
    try:
        separator = argv.index("--")
    except ValueError as error:
        raise SystemExit("naos_rust_build.py requires `--` before Cargo arguments") from error

    parser = argparse.ArgumentParser()
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--target-dir", type=Path, required=True)
    parser.add_argument("--fingerprint-file", type=Path, required=True)
    parser.add_argument("--input-root", type=Path, action="append", required=True)
    return parser.parse_args(argv[:separator]), argv[separator + 1 :]


def main(argv: list[str]) -> int:
    args, cargo = parse_args(argv)
    if not cargo:
        raise SystemExit("naos_rust_build.py received no Cargo command")

    current = fingerprint(args.input_root)
    previous = None
    if args.fingerprint_file.is_file():
        previous = args.fingerprint_file.read_text(encoding="ascii").strip()

    if previous != current and args.target_dir.exists():
        subprocess.run(
            [cargo[0], "clean", "--target-dir", os.fspath(args.target_dir)],
            cwd=args.workspace,
            check=True,
        )

    subprocess.run(cargo, cwd=args.workspace, check=True)

    args.fingerprint_file.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.fingerprint_file.with_name(args.fingerprint_file.name + ".tmp")
    temporary.write_text(current + "\n", encoding="ascii")
    temporary.replace(args.fingerprint_file)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
