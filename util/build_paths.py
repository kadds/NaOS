#!/usr/bin/env python3
"""Resolve build-local NaOS artifacts and emulator state."""

from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parent.parent


class BuildDirectoryError(ValueError):
    """Raised when the build directory cannot be selected unambiguously."""


@dataclass(frozen=True)
class BuildPaths:
    """All generated artifacts and emulator state belonging to one build."""

    build_dir: Path

    @property
    def system_dir(self) -> Path:
        return self.build_dir / "bin" / "system"

    @property
    def rootfs_dir(self) -> Path:
        return self.build_dir / "bin" / "rfsroot"

    @property
    def debug_dir(self) -> Path:
        return self.build_dir / "debug"

    @property
    def iso_dir(self) -> Path:
        return self.build_dir / "iso"

    @property
    def image_dir(self) -> Path:
        return self.build_dir / "image"

    @property
    def iso_image(self) -> Path:
        return self.image_dir / "naos.iso"

    @property
    def disk_image(self) -> Path:
        return self.image_dir / "disk.img"

    @property
    def serial_log(self) -> Path:
        return self.build_dir / "kernel_out.log"

    @property
    def qemu_debug_log(self) -> Path:
        return self.build_dir / "qemu.log"

    @property
    def boot_lock(self) -> Path:
        return self.build_dir / "boot.lock"


def build_paths(build_dir: str | Path) -> BuildPaths:
    """Return absolute paths rooted at *build_dir*."""
    return BuildPaths(Path(build_dir).expanduser().resolve())


def resolve_build_directory(build_dir: str | Path | None = None) -> Path:
    """Select a build directory, rejecting ambiguous implicit selection.

    An explicit argument or ``NAOS_BUILD_DIR`` always wins.  Without either,
    an existing single build directory is accepted; multiple build trees must
    be selected explicitly so parallel validation cannot share state.
    """
    requested = build_dir or os.environ.get("NAOS_BUILD_DIR")
    if requested:
        return Path(requested).expanduser().resolve()

    candidates = []
    default_build = REPOSITORY_ROOT / "build"
    if default_build.is_dir():
        candidates.append(default_build)
    candidates.extend(sorted(path for path in REPOSITORY_ROOT.glob("build-*") if path.is_dir()))

    if len(candidates) == 1:
        return candidates[0].resolve()
    if not candidates:
        return default_build.resolve()

    names = ", ".join(str(path.relative_to(REPOSITORY_ROOT)) for path in candidates)
    raise BuildDirectoryError(
        f"multiple build directories found ({names}); pass --build-dir or set NAOS_BUILD_DIR"
    )
