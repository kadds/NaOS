#!/usr/bin/env python3
"""Create and edit NaOS disk images directly, without loop devices or sudo."""

from __future__ import annotations

import argparse
import json
import os
import posixpath
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Sequence


from build_paths import build_paths, resolve_build_directory


SECTOR_SIZE = 512
PARTITION_NUMBER = 1
FSTOOL_INSTALL_HINT = "cargo install --git https://github.com/KarpelesLab/fstool --locked --force fstool"


class DiskError(RuntimeError):
    """A disk-image lifecycle error suitable for command-line output."""


def image_path(image_file: str | Path) -> Path:
    """Resolve an image path without changing the caller's working directory."""
    return Path(image_file).expanduser().resolve()


def command_text(command: Sequence[str]) -> str:
    return shlex.join(command)


def run(
    command: Sequence[str], *, input_text: str | None = None, check: bool = True
) -> subprocess.CompletedProcess[str]:
    """Run a text command without shell interpolation and report useful failures."""
    print(f"> {command_text(command)}")
    process = subprocess.run(
        command,
        input=input_text,
        text=True,
        capture_output=True,
        check=False,
    )
    if check and process.returncode != 0:
        details = (process.stderr or process.stdout).strip()
        suffix = f": {details}" if details else ""
        raise DiskError(f"command failed ({process.returncode}): {command_text(command)}{suffix}")
    return process


def require_command(name: str, package_hint: str) -> str:
    command = shutil.which(name)
    if command is None:
        raise DiskError(f"required command '{name}' is unavailable; install {package_hint} and retry")
    return command


def fstool_command() -> str:
    configured = os.environ.get("NAOS_FSTOOL_EXECUTABLE")
    if configured:
        if Path(configured).is_file() and os.access(configured, os.X_OK):
            return configured
        raise DiskError(f"NAOS_FSTOOL_EXECUTABLE is not executable: {configured}")
    return require_command("fstool", FSTOOL_INSTALL_HINT)


def partition_geometry(image_file: str | Path, partition: int = PARTITION_NUMBER) -> tuple[int, int]:
    """Return a partition's byte offset and byte length from its image MBR/GPT."""
    image = image_path(image_file)
    output = run([require_command("sfdisk", "util-linux"), "--json", str(image)]).stdout
    try:
        table = json.loads(output)["partitiontable"]
        partitions = table["partitions"]

        def partition_number(node: str) -> int:
            match = re.search(r"(\d+)$", node)
            if match is None:
                raise ValueError(f"partition node has no numeric suffix: {node}")
            return int(match.group(1))

        entry = next(
            item
            for item in partitions
            if partition_number(item["node"]) == partition
        )
        return int(entry["start"]) * SECTOR_SIZE, int(entry["size"]) * SECTOR_SIZE
    except (KeyError, StopIteration, TypeError, ValueError) as error:
        raise DiskError(f"partition {partition} is not available in {image}") from error


def image_target(image_file: str | Path, partition: int = PARTITION_NUMBER) -> str:
    return f"{image_path(image_file)}:{partition}"


def filesystem_kind(image_file: str | Path, partition: int = PARTITION_NUMBER) -> str | None:
    """Probe a filesystem through fstool; an unformatted partition returns None."""
    process = run([fstool_command(), "info", image_target(image_file, partition)], check=False)
    if process.returncode != 0:
        return None
    match = re.search(r"^fs kind:\s+(\S+)", process.stdout, flags=re.MULTILINE)
    return match.group(1).lower() if match else None


def copy_into_partition(image_file: Path, source_file: Path, offset: int, size: int) -> None:
    """Copy a freshly formatted filesystem into an image partition using regular-file I/O."""
    if source_file.stat().st_size != size:
        raise DiskError(
            f"fstool produced {source_file.stat().st_size} bytes, expected exactly {size} bytes for p1"
        )
    if image_file.stat().st_size < offset + size:
        raise DiskError(f"partition p1 extends beyond image size: {image_file}")
    with source_file.open("rb") as source, image_file.open("r+b") as destination:
        destination.seek(offset)
        remaining = size
        while remaining:
            chunk = source.read(min(1024 * 1024, remaining))
            if not chunk:
                raise DiskError("short read from fstool's formatted image")
            destination.write(chunk)
            remaining -= len(chunk)
        destination.flush()
        os.fsync(destination.fileno())


def format_partition(image_file: str | Path, filesystem: str) -> None:
    """Format p1 as FAT32 or exFAT without opening a block device."""
    if filesystem not in {"fat32", "exfat"}:
        raise DiskError(f"unsupported filesystem: {filesystem}")
    image = image_path(image_file)
    if not image.is_file():
        raise DiskError(f"disk image does not exist: {image}; run 'disk.py create' first")
    offset, size = partition_geometry(image)
    current = filesystem_kind(image)
    accepted = {"fat32", "vfat"} if filesystem == "fat32" else {"exfat"}
    if current in accepted:
        print(f"p1 is already {filesystem}; leaving it unchanged")
        return
    if current is not None:
        raise DiskError(f"refusing to replace existing {current} filesystem on p1")

    with tempfile.TemporaryDirectory(prefix="naos-fstool-format-") as directory:
        formatted = Path(directory) / "filesystem.img"
        run(
            [
                fstool_command(),
                "create",
                "--type",
                filesystem,
                "--size",
                f"{size}B",
                "--output",
                str(formatted),
            ]
        )
        copy_into_partition(image, formatted, offset, size)
    print(f"formatted p1 as {filesystem} at byte offset {offset} ({size} bytes)")


def mkfat(image_file: str | Path) -> None:
    """Format p1 as FAT32 through fstool."""
    format_partition(image_file, "fat32")


def mkexfat(image_file: str | Path) -> None:
    """Format p1 as exFAT through fstool."""
    format_partition(image_file, "exfat")


def add_file(
    image_file: str | Path,
    host_file: str | Path,
    guest_path: str,
    *,
    replace: bool = False,
) -> None:
    """Copy a host file into an image partition and flush it through fstool."""
    if not guest_path.startswith("/"):
        raise DiskError(f"guest path must be absolute: {guest_path}")
    source = Path(host_file).expanduser().resolve()
    if not source.is_file():
        raise DiskError(f"host source is not a file: {source}")
    if replace and entry_exists(image_file, guest_path):
        run([fstool_command(), "rm", image_target(image_file), guest_path])
    run([fstool_command(), "add", image_target(image_file), str(source), guest_path])


def read_file(image_file: str | Path, guest_path: str) -> bytes:
    """Read a file from an image partition without mounting it."""
    if not guest_path.startswith("/"):
        raise DiskError(f"guest path must be absolute: {guest_path}")
    command = [fstool_command(), "cat", image_target(image_file), guest_path]
    print(f"> {command_text(command)}")
    process = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if process.returncode != 0:
        details = process.stderr.decode(errors="replace").strip()
        suffix = f": {details}" if details else ""
        raise DiskError(f"command failed ({process.returncode}): {command_text(command)}{suffix}")
    return process.stdout


def create(image_file: str | Path, size: str = "64M") -> None:
    """Create a raw image with a DOS partition table and a single p1 slot."""
    image = image_path(image_file)
    if image.exists():
        print(f"disk image already exists: {image}")
        return
    qemu_img = require_command("qemu-img", "qemu-img")
    sfdisk = require_command("sfdisk", "util-linux")
    image.parent.mkdir(parents=True, exist_ok=True)
    run([qemu_img, "create", "-f", "raw", str(image), size])
    try:
        run(
            [sfdisk, "--no-reread", "--no-tell-kernel", str(image)],
            input_text="label: dos\nunit: sectors\n\nstart=2048, type=c, bootable\n",
        )
    except DiskError:
        image.unlink(missing_ok=True)
        raise
    print(f"created {image} ({size}) with DOS partition p1")


def mount(image_file: str | Path) -> None:
    """Validate p1 for direct-image access; no host mount is performed."""
    image = image_path(image_file)
    kind = filesystem_kind(image)
    if kind is None:
        raise DiskError(f"p1 is not a recognized filesystem in {image}; run mkfat or mkexfat first")
    print(f"direct image mode: p1 is {kind}; no loop device or host mount is needed")


def directory_exists(image_file: str | Path, guest_path: str) -> bool:
    """Return whether ``guest_path`` names a directory in the image."""
    if not guest_path.startswith("/"):
        raise DiskError(f"guest path must be absolute: {guest_path}")
    process = run(
        [fstool_command(), "ls", image_target(image_file), guest_path],
        check=False,
    )
    return process.returncode == 0


def ensure_directory(image_file: str | Path, guest_path: str) -> None:
    """Create each missing component of an absolute image directory path."""
    if not guest_path.startswith("/"):
        raise DiskError(f"guest path must be absolute: {guest_path}")
    current = ""
    for component in guest_path.strip("/").split("/"):
        if not component:
            continue
        current += f"/{component}"
        if directory_exists(image_file, current):
            continue
        run(
            [fstool_command(), "shell", image_target(image_file)],
            input_text=f"mkdir {shlex.quote(current)}\nquit\n",
        )
        if not directory_exists(image_file, current):
            raise DiskError(f"failed to create directory {current} in {image_path(image_file)}")


def entry_exists(image_file: str | Path, guest_path: str) -> bool:
    """Return whether a file-system entry exists, without reading its data."""
    if not guest_path.startswith("/"):
        raise DiskError(f"guest path must be absolute: {guest_path}")
    normalized = posixpath.normpath(guest_path)
    if normalized == "/":
        return True
    parent = posixpath.dirname(normalized) or "/"
    name = posixpath.basename(normalized)
    process = run(
        [fstool_command(), "ls", image_target(image_file), parent],
        check=False,
    )
    if process.returncode != 0:
        return False
    for line in process.stdout.splitlines():
        fields = line.split("\t", 2)
        if len(fields) == 3 and fields[2] == name:
            return True
    return False


def umount(image_file: str | Path) -> None:
    """Finalize direct-image access; fstool mutations are already flushed."""
    print(f"direct image mode: no host mount to detach for {image_path(image_file)}")


def add_image_option(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--build-dir",
        help="CMake build directory owning the image (default: build or the only build-* directory)",
    )
    parser.add_argument(
        "-i",
        "--input",
        help="raw disk image (default: <build-dir>/image/disk.img)",
    )


def image_argument(args: argparse.Namespace) -> str:
    """Resolve an explicit image or the selected build-local disk image."""
    if args.input:
        return args.input
    return str(build_paths(resolve_build_directory(args.build_dir)).disk_image)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    create_parser = subparsers.add_parser("create", help="create disk.img and a DOS p1 partition")
    add_image_option(create_parser)
    create_parser.add_argument("--size", default="64M", help="raw image size for a new disk (default: 64M)")

    commands = (
        ("mkfat", "format p1 as FAT32"),
        ("mkexfat", "format p1 as exFAT"),
        ("add", "copy a host file into p1"),
        ("cat", "print a file from p1"),
        ("mount", "validate p1 in direct-image mode"),
        ("umount", "flush/no-op direct-image detach"),
    )
    for name, help_text in commands:
        subparser = subparsers.add_parser(name, help=help_text)
        add_image_option(subparser)
        if name == "add":
            subparser.add_argument("source")
            subparser.add_argument("destination")
        elif name == "cat":
            subparser.add_argument("path")
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    try:
        image = image_argument(args)
        if args.command == "create":
            create(image, args.size)
        elif args.command == "mkfat":
            mkfat(image)
        elif args.command == "mkexfat":
            mkexfat(image)
        elif args.command == "add":
            add_file(image, args.source, args.destination)
        elif args.command == "cat":
            sys.stdout.buffer.write(read_file(image, args.path))
        elif args.command == "mount":
            mount(image)
        elif args.command == "umount":
            umount(image)
        return 0
    except (DiskError, OSError, ValueError) as error:
        print(f"disk: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
