#!/usr/bin/env python3
"""Rootless FAT32/exFAT image read/write smoke for util.disk."""

from __future__ import annotations

import sys
import tempfile
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(PROJECT_ROOT / "util"))

import disk  # noqa: E402


def check_filesystem(filesystem: str, temporary_root: Path) -> None:
    image = temporary_root / f"{filesystem}.img"
    source = temporary_root / f"{filesystem}.txt"
    source.write_text(f"NaOS rootless {filesystem} smoke\n", encoding="utf-8")

    disk.create(image, "64M")
    disk.format_partition(image, filesystem)
    disk.format_partition(image, filesystem)
    disk.mount(image)
    disk.ensure_directory(image, "/boot")
    destination = f"/boot/{source.name}"
    disk.add_file(image, source, destination)
    assert disk.read_file(image, destination) == source.read_bytes()
    source.write_text(f"NaOS direct-image replacement {filesystem}\n", encoding="utf-8")
    disk.add_file(image, source, destination, replace=True)
    assert disk.read_file(image, destination) == source.read_bytes()
    disk.umount(image)


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="naos-fstool-test-") as directory:
        temporary_root = Path(directory)
        check_filesystem("fat32", temporary_root)
        check_filesystem("exfat", temporary_root)
    print("fstool rootless FAT32/exFAT image smoke: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
