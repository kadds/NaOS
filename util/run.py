#!/usr/bin/env python3
"""Build boot artifacts and run NaOS with build-local emulator state."""

from __future__ import annotations

import argparse
import fcntl
import shlex
import shutil
import subprocess
import sys
import tempfile
from contextlib import contextmanager
from dataclasses import dataclass
from pathlib import Path

import disk
from build_paths import BuildDirectoryError, BuildPaths, build_paths, resolve_build_directory
from mod import run_shell, run_shell_input


REPOSITORY_ROOT = Path(__file__).resolve().parent.parent
MAKE_ROOT_IMAGE_SCRIPT = REPOSITORY_ROOT / "util" / "make_root_image.py"
OVMF_PATH = "/usr/share/ovmf/x64/OVMF_CODE.fd"

QEMU_GRAPHIC_ARGS = ["-display", "gtk"]
QEMU_UEFI_ARGS = ["-drive", f"file={OVMF_PATH},format=raw,readonly=on,if=pflash"]
VBOX_COMMAND = "VBoxManage startvm boot"
KVM_DEVICE = Path("/dev/kvm")

# Fixed Multiboot authority tokens.  The source filenames intentionally keep
# the concrete implementation names: blockd is currently ramdiskd and rootfsd
# is currently exfatd.
BOOT_MODULE_SOURCES = (
    ("vfsd", "vfsd"),
    ("blockd", "ramdiskd"),
    ("rootfsd", "rootfsd"),
    ("init", "init"),
    ("rootimage", "root.img"),
)


class RunError(RuntimeError):
    """A build or emulator error suitable for the CLI."""


@dataclass
class QemuResult:
    """The bounded QEMU result used by the launcher."""

    returncode: int
    stdout: str
    stderr: str
    timed_out: bool


def kvm_available() -> bool:
    """Return whether the host exposes a usable KVM device node."""
    return KVM_DEVICE.is_char_device()


def positive_timeout(value: str) -> int:
    """Parse a positive QEMU timeout in seconds."""
    try:
        timeout = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("timeout must be a positive integer") from error
    if timeout <= 0:
        raise argparse.ArgumentTypeError("timeout must be a positive integer")
    return timeout


def _gdb_port(value: str | None) -> int:
    if value is None:
        return 1234
    try:
        port = int(value)
    except (TypeError, ValueError) as error:
        raise ValueError(f"invalid GDB port: {value}") from error
    if not 1 <= port <= 65535:
        raise ValueError(f"GDB port must be between 1 and 65535: {port}")
    return port


def build_qemu_argv(args: argparse.Namespace, paths: BuildPaths | None = None) -> list[str]:
    """Build one QEMU argv for ISO, disk, and UEFI runs."""
    paths = paths or build_paths(getattr(args, "build_dir", REPOSITORY_ROOT / "build"))
    cores = int(args.cores)
    if cores < 1:
        raise ValueError("CPU core count must be positive")

    argv = [
        "qemu-system-x86_64",
        "-serial",
        f"file:{paths.serial_log}",
        "-cpu",
        "host" if kvm_available() else "Haswell-v4,pdpe1gb",
        "-smp",
        f"{cores},sockets=1,cores={cores}",
        "-m",
        str(args.memory),
    ]
    if kvm_available():
        argv[1:1] = ["-accel", "kvm"]

    port = _gdb_port(args.gdb_port)
    if args.gdb_port is None:
        argv.append("-s")
    else:
        argv.extend(["-gdb", f"tcp::{port}"])
    if args.wait_gdb:
        argv.append("-S")

    if args.uefi:
        argv.extend(QEMU_UEFI_ARGS)
    # -nographic muxes the serial port onto stdio and conflicts with an
    # explicit `-serial file:` capture; keep headless mode display-only.
    if args.nographic:
        argv.extend(["-display", "none", "-monitor", "none"])
    else:
        argv.extend(QEMU_GRAPHIC_ARGS)
    if args.iso:
        argv.extend(["-cdrom", str(paths.iso_image)])
    else:
        argv.extend(["-drive", f"file={paths.disk_image},format=raw,index=1"])

    if args.qemu_debug:
        argv.extend(["-d", "int,guest_errors,unimp,cpu_reset", "-D", str(paths.qemu_debug_log)])
    if args.monitor is not None:
        monitor_path = Path(args.monitor).expanduser().resolve()
        if monitor_path.exists():
            raise FileExistsError(f"QEMU monitor path already exists: {monitor_path}")
        monitor_path.parent.mkdir(parents=True, exist_ok=True)
        argv.extend(["-monitor", f"unix:{monitor_path},server=on,wait=off"])
    if args.no_reboot:
        # Keep -no-reboot so a guest reset parks the machine, but let a guest
        # power-off (ACPI S5) exit QEMU: opt-in smoke runs end by powering off
        # and would otherwise idle until the launcher's wall-clock limit.
        argv.extend(["-no-reboot"])
    return argv


def _is_allowed_missing(token: str, source: str, allowed: set[str]) -> bool:
    return token in allowed or source in allowed


def require_boot_artifacts(paths: BuildPaths, allow_missing: tuple[str, ...] = ()) -> None:
    """Reject a boot before QEMU when required build outputs are absent."""
    allowed = set(allow_missing)
    missing = [
        path
        for path in [paths.system_dir / "kernel"]
        + [
            paths.system_dir / source
            for token, source in BOOT_MODULE_SOURCES
            if not _is_allowed_missing(token, source, allowed)
        ]
        if not path.is_file()
    ]
    if missing:
        joined = ", ".join(str(path) for path in missing)
        raise RunError(f"missing NaOS build artifact(s): {joined}; build the system first")


def prepare_iso(paths: BuildPaths, allow_missing: tuple[str, ...] = ()) -> None:
    """Stage the current build and create an ISO entirely below its build dir."""
    allowed = set(allow_missing)
    paths.iso_dir.mkdir(parents=True, exist_ok=True)
    paths.image_dir.mkdir(parents=True, exist_ok=True)
    shutil.copytree(
        REPOSITORY_ROOT / "run/iso/boot",
        paths.iso_dir / "boot",
        dirs_exist_ok=True,
    )

    for name in ("kernel",):
        source = paths.system_dir / name
        if not source.is_file():
            raise FileNotFoundError(f"{source} does not exist; build NaOS before starting QEMU")
        shutil.copy2(source, paths.iso_dir / name)

    omitted_modules: list[tuple[str, str]] = []
    for token, source_name in BOOT_MODULE_SOURCES:
        source = paths.system_dir / source_name
        if not source.is_file():
            if not _is_allowed_missing(token, source_name, allowed):
                raise FileNotFoundError(f"{source} does not exist; build NaOS before starting QEMU")
            (paths.iso_dir / source_name).unlink(missing_ok=True)
            omitted_modules.append((token, source_name))
            continue
        shutil.copy2(source, paths.iso_dir / source_name)

    if omitted_modules:
        grub_config = paths.iso_dir / "boot" / "grub" / "grub.cfg"
        content = grub_config.read_text(encoding="utf-8")
        for token, source_name in omitted_modules:
            content = content.replace(f"    module2 /{source_name} {token}\n", "")
        grub_config.write_text(content, encoding="utf-8")

    # Obsolete prefixed names must never survive from an older build tree.
    for obsolete in ("naos-vfsd", "naos-ramdiskd", "naos-blockd", "naos-exfatd", "exfatd"):
        (paths.iso_dir / obsolete).unlink(missing_ok=True)

    subprocess.run(
        ["grub-mkrescue", "-o", str(paths.iso_image), str(paths.iso_dir)],
        check=True,
    )


def prepare_disk_image(paths: BuildPaths) -> None:
    """Refresh /boot directly in the build-local prepared disk image."""
    if not paths.disk_image.is_file():
        disk.create(paths.disk_image)
        disk.mkexfat(paths.disk_image)
    disk.mount(paths.disk_image)
    disk.ensure_directory(paths.disk_image, "/boot")
    for source in sorted(paths.system_dir.iterdir()):
        if source.is_file():
            disk.add_file(paths.disk_image, source, f"/boot/{source.name}", replace=True)
    config = (REPOSITORY_ROOT / "run/iso/boot/grub/grub.cfg").read_text(encoding="utf-8")
    for source_name in ("kernel", "vfsd", "ramdiskd", "rootfsd", "init", "root.img"):
        config = config.replace(f"/{source_name}", f"/boot/{source_name}")
    disk.install_bios_grub(paths.disk_image, config.encode("utf-8"))
    disk.umount(paths.disk_image)


def prepare_bochs_config(paths: BuildPaths) -> Path:
    """Create a build-local Bochs config with build-local state paths."""
    template = REPOSITORY_ROOT / "run/cfg/bochs/bochsrc.txt"
    config = paths.build_dir / "bochsrc.txt"
    paths.build_dir.mkdir(parents=True, exist_ok=True)
    content = template.read_text(encoding="utf-8")
    replacements = {
        'path="./../run/image/disk.img"': f'path="{paths.disk_image}"',
        "log: ../run/bochsout.txt": f"log: {paths.build_dir / 'bochsout.txt'}",
        "dev=kernel_out.log": f"dev={paths.serial_log}",
    }
    for old, new in replacements.items():
        content = content.replace(old, new)
    config.write_text(content, encoding="utf-8")
    return config


@contextmanager
def exclusive_boot(paths: BuildPaths):
    """Serialize QEMU and all mutable boot preparation for one build tree."""
    paths.build_dir.mkdir(parents=True, exist_ok=True)
    with paths.boot_lock.open("a+") as boot_lock:
        fcntl.flock(boot_lock.fileno(), fcntl.LOCK_EX)
        try:
            yield
        finally:
            fcntl.flock(boot_lock.fileno(), fcntl.LOCK_UN)


def run_qemu(args: argparse.Namespace, paths: BuildPaths) -> QemuResult:
    """Run QEMU with an optional wall-clock bound."""
    paths.serial_log.parent.mkdir(parents=True, exist_ok=True)
    paths.serial_log.write_text("", encoding="utf-8")
    process = subprocess.Popen(
        build_qemu_argv(args, paths),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    try:
        stdout, stderr = process.communicate(timeout=args.timeout)
        return QemuResult(process.returncode, stdout, stderr, False)
    except subprocess.TimeoutExpired:
        process.kill()
        stdout, stderr = process.communicate()
        return QemuResult(124, stdout, stderr, True)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--build-dir",
        help="CMake build directory owning all boot artifacts and emulator state",
    )
    parser.add_argument("-n", "--nographic", action="store_true", help="run without a graphical display")
    parser.add_argument("-u", "--uefi", action="store_true", help="boot QEMU with UEFI firmware")
    parser.add_argument("--iso", action="store_true", help="build and boot an ISO from this build")
    parser.add_argument("-m", "--memory", default="128", help="guest memory in MiB")
    parser.add_argument("-c", "--cores", default="2", help="guest CPU core count")
    parser.add_argument("--wait-gdb", action="store_true", help="pause QEMU until GDB connects")
    parser.add_argument("--gdb-port", help="GDB TCP port; replaces the default -s/1234 listener")
    parser.add_argument("--qemu-debug", action="store_true", help="write QEMU debug output into the build dir")
    parser.add_argument("--monitor", help="QEMU monitor Unix socket path")
    parser.add_argument("--no-reboot", action="store_true", help="keep QEMU stopped after guest shutdown or reset")
    parser.add_argument("--timeout", type=positive_timeout, help="stop QEMU after this many seconds")
    parser.add_argument(
        "--init-script",
        type=Path,
        help="temporarily install this opt-in /etc/init.sh and repack the build-local rootfs",
    )
    parser.add_argument(
        "--allow-missing-artifact",
        action="append",
        default=[],
        help="allow a named boot module to be absent (for a test-specific negative boot)",
    )
    parser.add_argument("emulator_name", choices=["q", "b", "v"], help="q: QEMU, b: Bochs, v: VirtualBox")
    args = parser.parse_args()
    return args


def print_process_output(result: QemuResult) -> None:
    """Print launcher diagnostics when QEMU exits unsuccessfully."""
    if result.stdout:
        print("QEMU stdout:", file=sys.stderr)
        print(result.stdout.rstrip(), file=sys.stderr)
    if result.stderr:
        print("QEMU stderr:", file=sys.stderr)
        print(result.stderr.rstrip(), file=sys.stderr)


def run_checked_qemu(args: argparse.Namespace, paths: BuildPaths) -> int:
    """Run QEMU; test-specific wrappers own any serial-log assertions."""
    result = run_qemu(args, paths)
    if result.returncode != 0 and not result.timed_out:
        print_process_output(result)
        raise RunError(f"QEMU exited with status {result.returncode}")
    if result.timed_out:
        print(f"QEMU reached the {args.timeout}s limit; inspect {paths.serial_log}.")
        return result.returncode
    return 0


def run_with_init_script(args: argparse.Namespace, paths: BuildPaths) -> int:
    """Run an opt-in init script while restoring the normal build artifacts."""
    script = args.init_script.expanduser().resolve()
    target = paths.rootfs_dir / "etc" / "init.sh"
    root_image = paths.system_dir / "root.img"
    if not script.is_file():
        raise RunError(f"init script does not exist: {script}")
    if not target.parent.is_dir() or not root_image.is_file():
        raise RunError(f"temporary init-script boot requires {paths.rootfs_dir} and root.img")

    with tempfile.TemporaryDirectory(prefix="naos-run-") as temporary:
        temporary_path = Path(temporary)
        target_backup = temporary_path / "init.sh"
        root_backup = temporary_path / "root.img"
        target_existed = target.is_file()
        if target_existed:
            shutil.copy2(target, target_backup)
        shutil.copy2(root_image, root_backup)
        try:
            shutil.copy2(script, target)
            subprocess.run(
                [
                    sys.executable,
                    str(MAKE_ROOT_IMAGE_SCRIPT),
                    "--input",
                    str(paths.rootfs_dir),
                    "--output",
                    str(root_image),
                ],
                cwd=REPOSITORY_ROOT,
                check=True,
            )
            # Stage only after the root image has been repacked.  The
            # ISO/disk image is the artifact QEMU actually boots; staging it
            # before this point silently drops the opt-in init script.
            if args.iso:
                allow_missing = tuple(getattr(args, "allow_missing_artifact", ()))
                if allow_missing:
                    prepare_iso(paths, allow_missing)
                else:
                    prepare_iso(paths)
            else:
                prepare_disk_image(paths)
            return run_checked_qemu(args, paths)
        finally:
            if target_existed:
                shutil.copy2(target_backup, target)
            else:
                target.unlink(missing_ok=True)
            shutil.copy2(root_backup, root_image)


def main() -> int:
    args = parse_arguments()
    try:
        paths = build_paths(resolve_build_directory(args.build_dir))
        if args.init_script is not None and args.emulator_name != "q":
            raise RunError("--init-script is only supported with QEMU")

        if args.emulator_name == "q":
            allowed_missing = tuple(args.allow_missing_artifact)
            known_modules = {name for token, name in BOOT_MODULE_SOURCES} | {
                token for token, _ in BOOT_MODULE_SOURCES
            }
            unknown = sorted(set(allowed_missing) - known_modules)
            if unknown:
                raise RunError(f"unknown boot module in --allow-missing-artifact: {', '.join(unknown)}")
            require_boot_artifacts(paths, allowed_missing)
            with exclusive_boot(paths):
                if args.init_script is not None:
                    return run_with_init_script(args, paths)
                if args.iso:
                    prepare_iso(paths, allowed_missing)
                else:
                    prepare_disk_image(paths)
                return run_checked_qemu(args, paths)

        if args.emulator_name == "b":
            config = prepare_bochs_config(paths)
            run_shell_input(f"bochs -f {shlex.quote(str(config))}")
        else:
            run_shell(VBOX_COMMAND)
        return 0
    except (BuildDirectoryError, RunError, FileExistsError, FileNotFoundError, OSError, ValueError) as error:
        print(f"run: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
