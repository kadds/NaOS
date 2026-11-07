#!/usr/bin/env python3
"""Generate the Rust fork's machine-local bootstrap.toml configuration."""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parent.parent
RUST_SOURCE = REPOSITORY_ROOT / "naos" / "third_party" / "rust-naos"
DEFAULT_TOOLCHAIN = "1.96.0"


def rustup_rustc(toolchain: str) -> Path:
    rustup = shutil.which("rustup")
    if rustup is None:
        raise SystemExit("rustup was not found on PATH")

    result = subprocess.run(
        [rustup, "which", "--toolchain", toolchain, "rustc"],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        if result.stderr.strip():
            print(result.stderr.strip(), file=sys.stderr)
        raise SystemExit(
            f"Rust toolchain {toolchain!r} is unavailable; install it with "
            f"`rustup toolchain install {toolchain} --profile minimal`"
        )

    rustc = Path(result.stdout.strip()).resolve()
    if not rustc.is_file():
        raise SystemExit(f"rustup returned a missing rustc path: {rustc}")
    return rustc


def install_config(rust_root: Path, generated_config: Path) -> Path:
    destination = rust_root / "bootstrap.toml"
    temporary_path: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(dir=rust_root, prefix="bootstrap.", suffix=".tmp", delete=False) as stream:
            temporary_path = Path(stream.name)
            stream.write(generated_config.read_bytes())
        temporary_path.chmod(0o644)
        os.replace(temporary_path, destination)
    finally:
        if temporary_path is not None and temporary_path.exists():
            temporary_path.unlink()
    return destination


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--toolchain",
        default=DEFAULT_TOOLCHAIN,
        help=f"installed rustup bootstrap toolchain (default: {DEFAULT_TOOLCHAIN})",
    )
    parser.add_argument(
        "--build-dir",
        default="build-debug/rust-naos",
        help="Rust fork build directory, relative to the NaOS repository root by default",
    )
    parser.add_argument("--jobs", type=int, default=2, help="Rust bootstrap parallel jobs (default: 2)")
    args = parser.parse_args()

    if args.jobs < 1:
        parser.error("--jobs must be at least 1")

    rust_root = RUST_SOURCE.resolve()
    configure = rust_root / "configure"
    if not configure.is_file():
        raise SystemExit(f"Rust fork is missing; initialize its submodule first: {rust_root}")

    rustc = rustup_rustc(args.toolchain)
    local_rust_root = rustc.parent.parent
    build_dir = Path(args.build_dir).expanduser()
    if not build_dir.is_absolute():
        build_dir = REPOSITORY_ROOT / build_dir
    build_dir = build_dir.resolve()

    configure_args = [
        str(configure),
        "--enable-local-rust",
        f"--local-rust-root={local_rust_root}",
        "--enable-local-rebuild",
        "--disable-extended",
        "--set",
        f"build.build-dir={build_dir}",
        "--set",
        f"build.jobs={args.jobs}",
        "--set",
        "llvm.download-ci-llvm=true",
        "--set",
        "rust.deny-warnings=false",
    ]

    with tempfile.TemporaryDirectory(prefix="naos-rust-bootstrap-") as temporary_dir:
        subprocess.run(configure_args, cwd=temporary_dir, check=True)
        generated_config = Path(temporary_dir) / "bootstrap.toml"
        if not generated_config.is_file():
            raise SystemExit(f"Rust configure did not produce {generated_config}")
        destination = install_config(rust_root, generated_config)

    print(f"Wrote {destination}")
    print(f"Rust bootstrap toolchain: {local_rust_root}")
    print(f"Rust build directory: {build_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
