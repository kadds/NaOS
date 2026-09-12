#!/usr/bin/env python3
import pathlib
import sys
import tempfile
from types import SimpleNamespace


ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "util"))

from build_paths import build_paths
import run
from run import QemuResult, build_qemu_argv


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
        "timeout": 1,
        "init_script": None,
        "build_dir": str(ROOT / "build-test"),
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
    paths = build_paths(ROOT / "build-test")

    # The boot contract is module-based: fixed early modules plus the
    # prepared root image must be present.
    with tempfile.TemporaryDirectory() as directory:
        module_paths = build_paths(pathlib.Path(directory))
        module_paths.system_dir.mkdir(parents=True, exist_ok=True)
        for name in ("kernel", "vfsd", "ramdiskd", "rootfsd", "init", "root.img"):
            module_paths.system_dir.joinpath(name).write_bytes(b"ELF")
        run.require_boot_artifacts(module_paths)
        module_paths.system_dir.joinpath("rootfsd").unlink()
        try:
            run.require_boot_artifacts(module_paths)
        except run.RunError:
            pass
        else:
            raise AssertionError("missing rootfsd boot module must reject the boot")
        module_paths.system_dir.joinpath("rootfsd").write_bytes(b"ELF")

        original_subprocess_run = run.subprocess.run
        try:
            run.subprocess.run = lambda *_args, **_kwargs: None
            run.prepare_iso(module_paths)
        finally:
            run.subprocess.run = original_subprocess_run
        for name in ("kernel", "vfsd", "ramdiskd", "rootfsd", "init", "root.img"):
            assert module_paths.iso_dir.joinpath(name).is_file()

        # A negative boot may intentionally omit one named early module. The
        # runner removes its GRUB module entry instead of leaving a stale copy
        # in the build-local ISO.
        module_paths.system_dir.joinpath("vfsd").unlink()
        run.require_boot_artifacts(module_paths, ("vfsd",))
        run.prepare_iso(module_paths, ("vfsd",))
        assert not module_paths.iso_dir.joinpath("vfsd").exists()
        grub_config = module_paths.iso_dir / "boot" / "grub" / "grub.cfg"
        assert "module2 /vfsd vfsd" not in grub_config.read_text(encoding="utf-8")

    default = build_qemu_argv(options(), paths)
    assert "-s" in default
    assert "-S" not in default
    assert "-serial" in default
    assert f"file:{paths.serial_log}" in default
    assert str(paths.iso_image) in default
    expected_cpu = "host" if run.kvm_available() else "Haswell-v4,pdpe1gb"
    assert default[default.index("-cpu") + 1] == expected_cpu
    if run.kvm_available():
        assert default[default.index("-accel") + 1] == "kvm"
    else:
        assert "-accel" not in default

    waiting = build_qemu_argv(options(wait_gdb=True), paths)
    assert "-S" in waiting
    assert_common_gdb(waiting, 1234)

    explicit_port = build_qemu_argv(options(gdb_port=12345), paths)
    assert "-s" not in explicit_port
    assert_common_gdb(explicit_port, 12345)

    debug = build_qemu_argv(options(qemu_debug=True, no_reboot=True), paths)
    assert "-d" in debug
    assert "int,guest_errors,unimp,cpu_reset" in debug
    assert "-D" in debug
    assert debug[debug.index("-D") + 1] == str(paths.qemu_debug_log)
    assert "-no-reboot" in debug
    # -no-shutdown would park QEMU on a guest power-off, so the harness that
    # ends opt-in smoke runs through ACPI S5 must not add it.
    assert "-no-shutdown" not in debug

    with tempfile.TemporaryDirectory() as directory:
        monitor = pathlib.Path(directory) / "qemu.monitor"
        monitored = build_qemu_argv(options(monitor=str(monitor)), paths)
        assert f"unix:{monitor},server=on,wait=off" in monitored
        monitor.touch()
        try:
            build_qemu_argv(options(monitor=str(monitor)), paths)
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
        argv = build_qemu_argv(options(iso=iso, uefi=uefi, gdb_port=23456, wait_gdb=True), paths)
        assert_common_gdb(argv, 23456)
        assert "-S" in argv

    original_run_qemu = run.run_qemu
    try:
        run.run_qemu = lambda _args, _paths: QemuResult(124, "", "", True)
        assert run.run_checked_qemu(options(timeout=1), paths) == 124
    finally:
        paths.serial_log.unlink(missing_ok=True)
        run.run_qemu = original_run_qemu

    with tempfile.TemporaryDirectory() as directory:
        temporary_build = pathlib.Path(directory)
        temporary_paths = build_paths(temporary_build)
        temporary_paths.system_dir.mkdir(parents=True, exist_ok=True)
        temporary_paths.rootfs_dir.joinpath("etc").mkdir(parents=True, exist_ok=True)
        temporary_paths.system_dir.joinpath("root.img").write_bytes(b"original")
        script = temporary_build / "init.sh"
        script.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        events = []
        original_subprocess_run = run.subprocess.run
        original_prepare_iso = run.prepare_iso
        original_checked_qemu = run.run_checked_qemu
        try:
            def record_subprocess(command, **_kwargs):
                events.append("root-image" if str(run.MAKE_ROOT_IMAGE_SCRIPT) in command else "unexpected")

            run.subprocess.run = record_subprocess
            run.prepare_iso = lambda _paths: events.append("stage")
            run.run_checked_qemu = lambda _args, _paths: events.append("qemu") or 0
            script_options = options(init_script=script, build_dir=str(temporary_build))
            assert run.run_with_init_script(script_options, temporary_paths) == 0
            assert events == ["root-image", "stage", "qemu"], events
            assert temporary_paths.system_dir.joinpath("root.img").read_bytes() == b"original"
        finally:
            run.subprocess.run = original_subprocess_run
            run.prepare_iso = original_prepare_iso
            run.run_checked_qemu = original_checked_qemu

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
