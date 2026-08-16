#!/usr/bin/env python3
import pathlib
import sys
import tempfile
from types import SimpleNamespace


ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "util"))

from run import build_qemu_argv


def options(**overrides):
    values = {
        "iso": True,
        "uefi": False,
        "nographic": True,
        "memory": "128",
        "cores": "2",
        "wait_gdb": False,
        "gdb_port": None,
        "qemu_debug": False,
        "monitor": None,
        "no_reboot": False,
    }
    values.update(overrides)
    return SimpleNamespace(**values)


def assert_common_gdb(argv, port):
    if "-s" in argv:
        assert port == 1234
    else:
        assert "-gdb" in argv
        assert f"tcp::{port}" in argv


def main() -> int:
    default = build_qemu_argv(options())
    assert "-s" in default
    assert "-S" not in default
    assert "-serial" in default

    waiting = build_qemu_argv(options(wait_gdb=True))
    assert "-S" in waiting
    assert_common_gdb(waiting, 1234)

    explicit_port = build_qemu_argv(options(gdb_port=12345))
    assert "-s" not in explicit_port
    assert_common_gdb(explicit_port, 12345)

    debug = build_qemu_argv(options(qemu_debug=True, no_reboot=True))
    assert "-d" in debug
    assert "int,guest_errors,unimp,cpu_reset" in debug
    assert "-D" in debug
    assert debug[debug.index("-D") + 1].endswith("run/qemu.log")
    assert "-no-reboot" in debug
    assert "-no-shutdown" in debug

    with tempfile.TemporaryDirectory() as directory:
        monitor = pathlib.Path(directory) / "qemu.monitor"
        monitored = build_qemu_argv(options(monitor=str(monitor)))
        assert f"unix:{monitor},server=on,wait=off" in monitored
        monitor.touch()
        try:
            build_qemu_argv(options(monitor=str(monitor)))
        except FileExistsError:
            pass
        else:
            raise AssertionError("existing monitor socket must be rejected")

    for bad_port in (0, 65536, "not-a-port"):
        try:
            build_qemu_argv(options(gdb_port=bad_port))
        except ValueError:
            pass
        else:
            raise AssertionError(f"invalid GDB port accepted: {bad_port}")

    for iso, uefi in ((True, False), (False, False), (True, True), (False, True)):
        argv = build_qemu_argv(options(iso=iso, uefi=uefi, gdb_port=23456, wait_gdb=True))
        assert_common_gdb(argv, 23456)
        assert "-S" in argv

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
