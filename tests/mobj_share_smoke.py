#!/usr/bin/env python3
"""Run the opt-in MemoryObject smoke and require its final verdict."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parent.parent
RUNNER = REPOSITORY_ROOT / "util" / "run.py"
INIT_SCRIPT = REPOSITORY_ROOT / "tests" / "mobj_share_init.sh"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--timeout", type=int, required=True)
    parser.add_argument("emulator_name", choices=["q"])
    parser.add_argument("qemu_args", nargs=argparse.REMAINDER)
    args = parser.parse_args()

    command = [
        sys.executable,
        str(RUNNER),
        "--build-dir",
        args.build_dir,
        "--timeout",
        str(args.timeout),
        "--init-script",
        str(INIT_SCRIPT),
        args.emulator_name,
        *args.qemu_args,
    ]
    result = subprocess.run(command, cwd=REPOSITORY_ROOT, check=False)
    # A bounded normal boot returns the launcher's timeout status. An opt-in
    # smoke normally powers QEMU off itself and returns zero.
    if result.returncode not in (0, 124):
        return result.returncode

    serial_log = Path(args.build_dir).resolve() / "kernel_out.log"
    try:
        serial = serial_log.read_text(encoding="utf-8", errors="replace")
    except OSError as error:
        print(f"mobj-share smoke: cannot read {serial_log}: {error}", file=sys.stderr)
        return 1
    if "mobj-share: PASS" not in serial:
        print(f"mobj-share smoke: missing final PASS marker in {serial_log}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
