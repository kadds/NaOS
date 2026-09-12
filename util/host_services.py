#!/usr/bin/env python3
"""Build and run the Linux std service process-boundary smoke.

This runner only starts local binaries and does not inspect repository source.
It is suitable for CTest and deliberately has no QEMU dependency.
"""

from __future__ import annotations

import argparse
import os
import subprocess
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cargo", default="cargo")
    parser.add_argument("--target-dir", type=Path, default=None)
    args = parser.parse_args()

    repository = Path(__file__).resolve().parent.parent
    target_dir = args.target_dir or repository / "target"
    environment = os.environ.copy()
    environment["CARGO_TARGET_DIR"] = str(target_dir)

    build = [
        args.cargo,
        "build",
        "--locked",
        "--package",
        "ramdiskd",
        "--package",
        "exfatd",
        "--package",
        "vfsd",
        "--package",
        "naos-host-smoke",
        "--bins",
    ]
    subprocess.run(build, cwd=repository, env=environment, check=True)

    debug = target_dir / "debug"
    smoke = [
        str(debug / "naos-host-smoke"),
        str(debug / "ramdiskd"),
        str(debug / "exfatd"),
        str(debug / "vfsd"),
    ]
    subprocess.run(smoke, cwd=repository, env=environment, check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
