#!/usr/bin/env python3
"""Run a boot smoke and assert its test-owned serial-log contract."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parent.parent
RUNNER = REPOSITORY_ROOT / "util" / "run.py"

BOOT_MARKERS = (
    "vfsd: INFO: mount authority ready service=",
    "vfsd: INFO: rootfsd worker bootstrap requested pid=",
    "exfatd: INFO: mount committed mount=",
    "vfsd: INFO: root route ready service=",
    "vfsd: INFO: ready service=",
    "vfsd: INFO: init spawned pid=",
    "init: committed root route ready",
    "init: user shell started",
)


def boot_succeeded(serial: str) -> bool:
    """Check required startup markers and only their causal ordering."""
    if any(marker in serial for marker in ("PANIC:", "Kernel Oops", "exception ")):
        return False
    positions = {marker: serial.find(marker) for marker in BOOT_MARKERS}
    if not all(position >= 0 for position in positions.values()):
        return False

    ordered_pairs = (
        (BOOT_MARKERS[0], BOOT_MARKERS[1]),
        (BOOT_MARKERS[1], BOOT_MARKERS[2]),
        (BOOT_MARKERS[1], BOOT_MARKERS[3]),
        (BOOT_MARKERS[3], BOOT_MARKERS[4]),
        (BOOT_MARKERS[4], BOOT_MARKERS[5]),
        (BOOT_MARKERS[5], BOOT_MARKERS[6]),
        (BOOT_MARKERS[6], BOOT_MARKERS[7]),
    )
    if any(positions[before] >= positions[after] for before, after in ordered_pairs):
        return False
    return "init: exfatd started" not in serial and "init: exfatd spawn failed" not in serial


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--timeout", type=int, required=True)
    parser.add_argument("--init-script", type=Path)
    parser.add_argument("--check-boot", action="store_true")
    parser.add_argument("--expect-marker", action="append", default=[])
    parser.add_argument("--require-absent")
    parser.add_argument("emulator_name", choices=["q"])
    parser.add_argument("qemu_args", nargs=argparse.REMAINDER)
    args = parser.parse_args()

    if args.require_absent:
        artifact = Path(args.build_dir).resolve() / "bin" / "system" / args.require_absent
        if artifact.is_file():
            print(f"Skipping negative boot: {artifact} is present")
            return 0

    command = [
        sys.executable,
        str(RUNNER),
        "--build-dir",
        args.build_dir,
        "--timeout",
        str(args.timeout),
    ]
    if args.require_absent:
        command.extend(["--allow-missing-artifact", args.require_absent])
    if args.init_script is not None:
        command.extend(["--init-script", str(args.init_script)])
    command.extend([args.emulator_name, *args.qemu_args])
    result = subprocess.run(command, cwd=REPOSITORY_ROOT, check=False)
    if result.returncode not in (0, 124):
        return result.returncode

    if not args.check_boot and not args.expect_marker:
        return result.returncode

    serial_log = Path(args.build_dir).resolve() / "kernel_out.log"
    try:
        serial = serial_log.read_text(encoding="utf-8", errors="replace")
    except OSError as error:
        print(f"boot smoke: cannot read {serial_log}: {error}", file=sys.stderr)
        return 1
    if args.check_boot and not boot_succeeded(serial):
        print(f"boot smoke: service-start sequence missing from {serial_log}", file=sys.stderr)
        return 1
    missing = [marker for marker in args.expect_marker if marker not in serial]
    if missing:
        print(f"boot smoke: missing marker(s) {missing!r} in {serial_log}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
