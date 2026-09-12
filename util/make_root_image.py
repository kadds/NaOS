#!/usr/bin/env python3
"""Build the prepared FAT root image used by the mounted-root boot."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import tempfile
from pathlib import Path


SECTOR_SIZE = 512
# mkfs.fat's -C count is in 1024-byte blocks.  This produces 69,632 logical
# 512-byte sectors, matching the prepared ramdisk medium while keeping the
# complete boot image small enough for the default 128 MiB guest.
ROOT_BLOCKS = 34_816
# fatfs reads one filesystem cluster per block-client call.  Eight KiB
# clusters keep executable materialization on the direct MemoryObject data
# path while avoiding one RPC for every 512-byte sector.  The fixed image is
# below the FAT32 cluster-count threshold, so use the matching FAT16 layout.
SECTORS_PER_CLUSTER = 16

# FAT has no Unix symlink representation.  Keep the boot-critical and common
# interactive aliases as dereferenced regular files; omitting the long tail
# avoids multiplying the 1.5 MiB BusyBox image until the root medium no longer
# fits in the fixed prepared medium.
BOOT_APPLET_ALIASES = {
    "cat",
    "echo",
    "ls",
    "poweroff",
    "pwd",
    "sh",
}


def require_command(name: str) -> str:
    command = shutil.which(name)
    if command is None:
        raise RuntimeError(f"required command is unavailable: {name}")
    return command


def materialize_entry(source_root: Path, source: Path, destination: Path) -> None:
    """Copy one tree while turning Unix symlinks into regular FAT files."""
    if source.is_symlink():
        if source.name not in BOOT_APPLET_ALIASES:
            return
        target = Path(source.readlink())
        target = (source_root / target.relative_to("/")) if target.is_absolute() else source.parent / target
        target = target.resolve()
        if not target.is_file():
            raise RuntimeError(f"symlink target is not a file: {source} -> {target}")
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(target, destination)
        return
    if source.is_dir():
        destination.mkdir(parents=True, exist_ok=True)
        for child in sorted(source.iterdir(), key=lambda path: path.name):
            materialize_entry(source_root, child, destination / child.name)
        return
    if source.is_file():
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
        return
    raise RuntimeError(f"unsupported root entry: {source}")


def build_image(source: Path, output: Path) -> None:
    source = source.resolve()
    output = output.resolve()
    if not source.is_dir():
        raise RuntimeError(f"root source is not a directory: {source}")
    output.parent.mkdir(parents=True, exist_ok=True)
    if output.exists():
        output.unlink()

    subprocess.run(
        [
            require_command("mkfs.vfat"),
            "-F16",
            "-S",
            str(SECTOR_SIZE),
            "-s",
            str(SECTORS_PER_CLUSTER),
            "-n",
            "NAOSROOT",
            "-C",
            str(output),
            str(ROOT_BLOCKS),
        ],
        check=True,
    )
    with tempfile.TemporaryDirectory(prefix="naos-root-image-") as staging:
        staging_root = Path(staging)
        for entry in sorted(source.iterdir(), key=lambda path: path.name):
            if entry.is_symlink() and entry.name not in BOOT_APPLET_ALIASES:
                continue
            materialize_entry(source, entry, staging_root / entry.name)
        entries = sorted(staging_root.iterdir(), key=lambda path: path.name)
        if entries:
            subprocess.run(
                [require_command("mcopy"), "-i", str(output), "-s", *map(str, entries), "::/"],
                check=True,
            )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    build_image(args.input, args.output)
    print(f"make {args.output.resolve()} success size {args.output.stat().st_size}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
