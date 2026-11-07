#!/usr/bin/env python3
"""Run the opt-in Virtio Block MSI-X and data-path smoke on a private disk."""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path


REPOSITORY_ROOT = Path(__file__).resolve().parent.parent
DISK_BYTES = 32 * 1024 * 1024


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--timeout", required=True, type=int)
    parser.add_argument("--terminal-fault", action="store_true")
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error("--timeout must be positive")

    build_dir = args.build_dir.resolve()
    image_dir = build_dir / "image"
    image_dir.mkdir(parents=True, exist_ok=True)
    disk_image = image_dir / f"virtio-blk-msix-smoke-{os.getpid()}.img"
    try:
        descriptor = os.open(disk_image, os.O_CREAT | os.O_EXCL | os.O_RDWR, 0o600)
        try:
            os.ftruncate(descriptor, DISK_BYTES)
        finally:
            os.close(descriptor)

        command = [
            sys.executable,
            str(REPOSITORY_ROOT / "tests" / "boot_smoke.py"),
            "--build-dir",
            str(build_dir),
            "--timeout",
            str(args.timeout),
            "--no-gdb",
            "--intel-iommu-ir",
            "--acpi-ssdt",
            str(build_dir / "acpi-test-fixtures" / "qemu_virtio_blk_ssdt.aml"),
            "--init-script",
            str(
                REPOSITORY_ROOT
                / "tests"
                / ("virtio_block_fault_init.sh" if args.terminal_fault else "virtio_block_init.sh")
            ),
            "--qemu-drive",
            f"file={disk_image},format=raw,if=none,id=blk0",
            "--qemu-device",
            "virtio-blk-pci,drive=blk0,disable-legacy=on,iommu_platform=on",
            "--expect-qemu-exit",
            "--expect-marker",
            "init: ttyd ready",
            "--expect-marker",
            "PCI device resource attestation bound address=0000:00:03.0",
            "--expect-marker",
            "Virtio block provider registered address=0000:00:03.0",
            "--expect-marker",
            "Virtio block online",
            "--expect-marker",
            "reboot: submitting PlatformPowerControl reboot request",
        ]
        if args.terminal_fault:
            command.extend(
                [
                    "--qemu-trace-event",
                    "vtd_dmar_fault",
                    "--expect-marker",
                    "Virtio test injected unmapped DMA IOVA=0x20000000",
                    "--expect-marker",
                    "Virtio DMA fault iova=0x20000000",
                    "--expect-marker",
                    "Virtio block fault smoke received the expected BlockDevice EIO",
                    "--expect-marker",
                    "Virtio failure path dropped its MSI-X route capability",
                    "--expect-marker",
                    "Virtio block fault smoke waiting for catalog cleanup",
                    "--expect-marker",
                    "removed PCI block provider after driver exit address=0000:00:03.0 factory_id=1",
                    "--expect-marker",
                    "Virtio block fault smoke received a catalog snapshot after driver exit",
                    "--expect-marker",
                    "Virtio block fault smoke observed its provider missing from the catalog",
                    "--expect-marker",
                    "Virtio block fault smoke verified terminal DMA failure and catalog removal",
                    "--expect-marker",
                    "PCI DMA domain teardown complete segment=0 bus=0 device=3 function=0",
                    "--expect-marker",
                    "PCI MSI route teardown complete segment=0 bus=0 device=3 function=0",
                    "--expect-qemu-trace-regex",
                    r"vtd_dmar_fault sid 0x18 fault [0-9]+ addr 0x20000000 write 0",
                ]
            )
        else:
            command.extend(
                [
                    "--qemu-trace-event",
                    "vtd_ir_remap_msi_req",
                    "--expect-marker",
                    "Virtio block smoke verified write, flush and read-back through BlockDevice",
                    "--expect-qemu-trace-regex",
                    r"vtd_ir_remap_msi_req addr 0xfee000[0-9a-f]+ data 0x0",
                ]
            )
        command.extend(["q", "--machine", "q35", "--iso", "-m", "512", "-n", "--no-reboot"])
        return subprocess.run(command, cwd=REPOSITORY_ROOT, check=False).returncode
    finally:
        disk_image.unlink(missing_ok=True)


if __name__ == "__main__":
    raise SystemExit(main())
