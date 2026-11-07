#!/usr/bin/env python3
"""Exercise Q35 CPU hotplug through the guest's ACPI SCI/GPE path."""

from __future__ import annotations

import argparse
import os
import re
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

from boot_smoke import BOOT_MARKERS, boot_succeeded


REPOSITORY_ROOT = Path(__file__).resolve().parent.parent
RUNNER = REPOSITORY_ROOT / "util" / "run.py"
GPE_READY_MARKER = "AcpiFixedHardware SCI owner claimed; enabled AML GPE handlers="
GPE_HANDLER_MARKER = "SCI GPE handler completed number=0x2 "
SCI_ACK_MARKER = "SCI acknowledgement sequence="


def read_serial(serial_log: Path) -> str:
    try:
        return serial_log.read_text(encoding="utf-8", errors="replace")
    except FileNotFoundError:
        return ""


def wait_for_marker(
    serial_log: Path,
    marker: str,
    process: subprocess.Popen[bytes],
    timeout: float,
    fresh_after_ns: int,
) -> str:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            is_fresh = serial_log.stat().st_mtime_ns >= fresh_after_ns
        except FileNotFoundError:
            is_fresh = False
        serial = read_serial(serial_log) if is_fresh else ""
        if marker in serial:
            return serial
        return_code = process.poll()
        if return_code is not None:
            raise RuntimeError(f"QEMU runner exited with {return_code} before marker {marker!r}")
        time.sleep(0.1)
    raise TimeoutError(f"timed out waiting for serial marker {marker!r}")


def connect_monitor(path: Path, process: subprocess.Popen[bytes], timeout: float) -> socket.socket:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"QEMU runner exited with {process.returncode} before monitor opened")
        monitor = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        monitor.settimeout(1.0)
        try:
            monitor.connect(str(path))
            return monitor
        except OSError:
            monitor.close()
            time.sleep(0.1)
    raise TimeoutError(f"timed out connecting to QEMU monitor {path}")


def read_monitor_prompt(monitor: socket.socket, timeout: float = 5.0) -> bytes:
    monitor.settimeout(timeout)
    response = bytearray()
    while b"(qemu)" not in response:
        chunk = monitor.recv(4096)
        if not chunk:
            break
        response.extend(chunk)
    return bytes(response)


def issue_hmp(monitor: socket.socket, command: str) -> bytes:
    monitor.sendall(command.encode("ascii") + b"\n")
    response = read_monitor_prompt(monitor)
    lowered = response.lower()
    if b"error:" in lowered or b"failed" in lowered or b"not found" in lowered:
        raise RuntimeError(f"QEMU rejected HMP command {command!r}: {response.decode(errors='replace')}")
    return response


def hotpluggable_cpu(monitor: socket.socket) -> tuple[str, int, int, int]:
    response = issue_hmp(monitor, "info hotpluggable-cpus").decode("utf-8", errors="replace")
    response = re.sub(r"\x1b\[[0-?]*[ -/]*[@-~]", "", response)
    response = response.replace("\x08", "")
    for block in re.split(r"(?=\btype:\s*)", response):
        cpu_type = re.search(r'\btype:\s*"?([A-Za-z0-9_.-]+)"?', block)
        socket_id = re.search(r'\bsocket-id:\s*"?(\d+)"?', block)
        core_id = re.search(r'\bcore-id:\s*"?(\d+)"?', block)
        thread_id = re.search(r'\bthread-id:\s*"?(\d+)"?', block)
        if not all((cpu_type, socket_id, core_id, thread_id)):
            continue
        if "qom_path:" in block or "qom-path:" in block:
            continue
        return (
            cpu_type.group(1),
            int(socket_id.group(1)),
            int(core_id.group(1)),
            int(thread_id.group(1)),
        )
    raise RuntimeError(f"QEMU exposes no unpopulated CPU slot: {response}")


def terminate_runner(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        return
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--timeout", type=int, default=60)
    parser.add_argument("--acpi-ssdt", required=True, type=Path)
    args = parser.parse_args()

    build_dir = Path(args.build_dir).resolve()
    serial_log = build_dir / "kernel_out.log"
    monitor_path = build_dir / f"q35-acpi-gpe-{os.getpid()}.monitor"
    runner_log = build_dir / "q35-acpi-gpe-runner.log"
    command = [
        sys.executable,
        str(RUNNER),
        "--build-dir",
        str(build_dir),
        "--timeout",
        str(args.timeout),
        "--no-gdb",
        "--monitor",
        str(monitor_path),
        "--max-cpus",
        "3",
        "--acpi-ssdt",
        str(args.acpi_ssdt.resolve()),
        "q",
        "--machine",
        "q35",
        "-m",
        "512",
        "-n",
    ]

    monitor = None
    succeeded = False
    launched_at_ns = time.time_ns()
    with runner_log.open("wb") as output:
        process = subprocess.Popen(
            command,
            cwd=REPOSITORY_ROOT,
            stdout=output,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        try:
            serial = wait_for_marker(serial_log, GPE_READY_MARKER, process, 90.0, launched_at_ns)
            ready_line = next(line for line in serial.splitlines() if GPE_READY_MARKER in line)
            mask_text = ready_line.split("gpe0_mask=", 1)[1].split()[0]
            gpe0_mask = int(mask_text, 16)
            if gpe0_mask & 0x4 == 0:
                raise RuntimeError(f"Q35 CPU hotplug GPE.2 is not enabled: {ready_line}")
            serial = wait_for_marker(serial_log, BOOT_MARKERS[-1], process, 30.0, launched_at_ns)

            monitor = connect_monitor(monitor_path, process, 10.0)
            read_monitor_prompt(monitor)
            cpu_type, socket_id, core_id, thread_id = hotpluggable_cpu(monitor)
            issue_hmp(
                monitor,
                f"device_add {cpu_type},id=naos_acpi_gpe_cpu,socket-id={socket_id},"
                f"core-id={core_id},thread-id={thread_id}",
            )
            wait_for_marker(serial_log, GPE_HANDLER_MARKER, process, 15.0, launched_at_ns)
            wait_for_marker(serial_log, SCI_ACK_MARKER, process, 5.0, launched_at_ns)

            serial = read_serial(serial_log)
            if not boot_succeeded(serial):
                raise RuntimeError("normal service startup did not complete during SCI/GPE smoke")
            if "SCI event sequence=" not in serial:
                raise RuntimeError("SCI readiness fired without a drained event record")
            if "SCI acknowledgement sequence=" not in serial:
                raise RuntimeError("SCI GPE handler ran without a matching acknowledgement")

            issue_hmp(monitor, "quit")
            return_code = process.wait(timeout=15)
            if return_code != 0:
                raise RuntimeError(f"QEMU runner exited with {return_code} after clean monitor quit")
            succeeded = True
            return 0
        except (OSError, RuntimeError, TimeoutError, StopIteration) as error:
            print(f"ACPI SCI/GPE smoke failed: {error}", file=sys.stderr)
            return 1
        finally:
            if monitor is not None:
                monitor.close()
            if not succeeded:
                terminate_runner(process)


if __name__ == "__main__":
    raise SystemExit(main())
