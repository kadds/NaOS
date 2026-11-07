#!/usr/bin/env python3
"""Run a boot smoke and assert its test-owned serial-log contract."""

from __future__ import annotations

import argparse
import re
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
    parser.add_argument("--gdb-port")
    parser.add_argument("--no-gdb", action="store_true")
    parser.add_argument("--intel-iommu", action="store_true")
    parser.add_argument("--intel-iommu-ir", action="store_true")
    parser.add_argument("--qemu-device", action="append", default=[])
    parser.add_argument("--qemu-drive", action="append", default=[])
    parser.add_argument("--qemu-trace-event", action="append", default=[])
    parser.add_argument("--acpi-ssdt", type=Path)
    parser.add_argument("--init-script", type=Path)
    parser.add_argument("--expect-qemu-exit", action="store_true")
    parser.add_argument("--check-boot", action="store_true")
    parser.add_argument("--expect-marker", action="append", default=[])
    parser.add_argument("--expect-marker-order", action="append", nargs="+", default=[])
    parser.add_argument("--forbid-marker", action="append", default=[])
    parser.add_argument(
        "--expect-marker-count",
        action="append",
        nargs=2,
        metavar=("MARKER", "COUNT"),
        default=[],
    )
    parser.add_argument("--expect-qemu-trace-regex", action="append", default=[])
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
    if args.gdb_port is not None:
        command.extend(["--gdb-port", args.gdb_port])
    if args.no_gdb:
        command.append("--no-gdb")
    if args.intel_iommu:
        command.append("--intel-iommu")
    if args.intel_iommu_ir:
        command.append("--intel-iommu-ir")
    for device in args.qemu_device:
        command.extend(["--qemu-device", device])
    for drive in args.qemu_drive:
        command.extend(["--qemu-drive", drive])
    for event in args.qemu_trace_event:
        command.extend(["--qemu-trace-event", event])
    if args.acpi_ssdt is not None:
        command.extend(["--acpi-ssdt", str(args.acpi_ssdt)])
    if args.require_absent:
        command.extend(["--allow-missing-artifact", args.require_absent])
    if args.init_script is not None:
        command.extend(["--init-script", str(args.init_script)])
    command.extend([args.emulator_name, *args.qemu_args])
    result = subprocess.run(command, cwd=REPOSITORY_ROOT, check=False)
    if args.expect_qemu_exit and result.returncode != 0:
        print(
            f"boot smoke: expected QEMU to exit cleanly after the guest transition, got {result.returncode}",
            file=sys.stderr,
        )
        return 1
    if result.returncode not in (0, 124):
        return result.returncode

    if (
        not args.check_boot
        and not args.expect_marker
        and not args.expect_marker_order
        and not args.expect_marker_count
        and not args.forbid_marker
        and not args.expect_qemu_trace_regex
    ):
        return result.returncode

    serial_log = Path(args.build_dir).resolve() / "kernel_out.log"
    try:
        serial = serial_log.read_text(encoding="utf-8", errors="replace")
    except OSError as error:
        print(f"boot smoke: cannot read {serial_log}: {error}", file=sys.stderr)
        return 1
    if args.expect_qemu_exit and any(marker in serial for marker in ("PANIC:", "Kernel Oops", "exception ")):
        print(f"boot smoke: unexpected kernel fault during transition test in {serial_log}", file=sys.stderr)
        return 1
    if args.check_boot and not boot_succeeded(serial):
        print(f"boot smoke: service-start sequence missing from {serial_log}", file=sys.stderr)
        return 1
    missing = [marker for marker in args.expect_marker if marker not in serial]
    if missing:
        print(f"boot smoke: missing marker(s) {missing!r} in {serial_log}", file=sys.stderr)
        return 1
    for ordered_markers in args.expect_marker_order:
        positions = [serial.find(marker) for marker in ordered_markers]
        if any(position < 0 for position in positions):
            print(
                f"boot smoke: ordered marker group has missing marker(s) {ordered_markers!r} in {serial_log}",
                file=sys.stderr,
            )
            return 1
        if positions != sorted(positions):
            print(
                f"boot smoke: marker order violated for {ordered_markers!r} in {serial_log}",
                file=sys.stderr,
            )
            return 1
    forbidden = [marker for marker in args.forbid_marker if marker in serial]
    if forbidden:
        print(f"boot smoke: forbidden marker(s) {forbidden!r} in {serial_log}", file=sys.stderr)
        return 1
    for marker, expected_text in args.expect_marker_count:
        try:
            expected_count = int(expected_text)
        except ValueError:
            print(f"boot smoke: invalid marker count {expected_text!r} for {marker!r}", file=sys.stderr)
            return 2
        actual_count = serial.count(marker)
        if actual_count != expected_count:
            print(
                f"boot smoke: marker {marker!r} occurred {actual_count} times; expected {expected_count}",
                file=sys.stderr,
            )
            return 1
    if args.expect_qemu_trace_regex:
        trace_log = Path(args.build_dir).resolve() / "qemu-trace.log"
        try:
            trace = trace_log.read_text(encoding="utf-8", errors="replace")
        except OSError as error:
            print(f"boot smoke: cannot read {trace_log}: {error}", file=sys.stderr)
            return 1
        missing = [pattern for pattern in args.expect_qemu_trace_regex if re.search(pattern, trace) is None]
        if missing:
            print(f"boot smoke: missing QEMU trace pattern(s) {missing!r} in {trace_log}", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
