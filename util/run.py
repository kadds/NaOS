#!/usr/bin/env python3
import sys
import os
import argparse
import shutil
import subprocess
import traceback
from pathlib import Path
from disk import mount_point
from mod import set_self_dir, run_shell, run_shell_input

'''
How to build VMbox image?
VBoxManage internalcommands createrawvmdk -filename run/image/disk.vmdk -rawdisk run/image/disk.img
'''

# set ovmf_path if boot from UEFI
ovmf_path = '/usr/share/ovmf/x64/OVMF_CODE.fd'

qemu_graphic_args = ["-display", "gtk"]
qemu_headless_args = ["-nographic"]
qemu_uefi_args = ["-drive", f"file={ovmf_path},format=raw,readonly=on,if=pflash"]

bochs = "bochs -f ../run/cfg/bochs/bochsrc.txt"
vbox = "VBoxManage startvm boot"


def _gdb_port(value):
    if value is None:
        return 1234
    try:
        port = int(value)
    except (TypeError, ValueError) as error:
        raise ValueError(f"invalid GDB port: {value}") from error
    if not 1 <= port <= 65535:
        raise ValueError(f"GDB port must be between 1 and 65535: {port}")
    return port


def build_qemu_argv(args):
    """Build one QEMU argv for ISO, disk, and UEFI runs."""
    cores = int(args.cores)
    if cores < 1:
        raise ValueError("CPU core count must be positive")

    argv = [
        "qemu-system-x86_64",
        "-serial",
        "file:../run/kernel_out.log",
        "-cpu",
        "Haswell-v4,pdpe1gb",
        "-smp",
        f"{cores},sockets=1,cores={cores}",
        "-m",
        str(args.memory),
    ]

    port = _gdb_port(args.gdb_port)
    if args.gdb_port is None:
        argv.append("-s")
    else:
        argv.extend(["-gdb", f"tcp::{port}"])
    if args.wait_gdb:
        argv.append("-S")

    if args.uefi:
        argv.extend(qemu_uefi_args)
    argv.extend(qemu_headless_args if args.nographic else qemu_graphic_args)
    if args.iso:
        argv.extend(["-cdrom", "../run/image/naos.iso"])
    else:
        argv.extend(["-drive", "file=../run/image/disk.img,format=raw,index=1"])

    if args.qemu_debug:
        argv.extend(["-d", "int,guest_errors,unimp,cpu_reset", "-D", "../run/qemu.log"])
    if args.monitor is not None:
        monitor_path = Path(args.monitor)
        if monitor_path.exists():
            raise FileExistsError(f"QEMU monitor path already exists: {monitor_path}")
        argv.extend(["-monitor", f"unix:{monitor_path},server=on,wait=off"])
    if args.no_reboot:
        argv.extend(["-no-reboot", "-no-shutdown"])
    return argv


def prepare_iso():
    system_dir = "../build/bin/system"
    iso_dir = "../run/iso"
    image_dir = "../run/image"

    os.makedirs(iso_dir, exist_ok=True)
    os.makedirs(image_dir, exist_ok=True)

    for name in ("kernel", "rfsimg"):
        source = os.path.join(system_dir, name)
        if not os.path.isfile(source):
            raise FileNotFoundError(
                source + " does not exist; build NaOS before starting QEMU"
            )
        shutil.copy2(source, os.path.join(iso_dir, name))

    subprocess.run(
        ["grub-mkrescue", "-o", os.path.join(image_dir, "naos.iso"), iso_dir],
        check=True,
    )


if __name__ == "__main__":
    set_self_dir()

    parser = argparse.ArgumentParser(
        description='run tools: run kernel')
    parser.add_argument(
        "-n", "--nographic", action='store_true', help="try don't show GUI")
    parser.add_argument(
        "-u", "--uefi", action='store_true', help="open qemu with uefi firmware")
    parser.add_argument(
        "--iso", action='store_true', help="iso")
    parser.add_argument("emulator_name", type=str,
                        choices=["q", "b", "v"], help="q: run qemu\nb: run bochs\nv: run virtual box")
    parser.add_argument(
        "-m", "--memory", help="memory max(M)", default='128')
    parser.add_argument(
        "-c", "--cores", help="cpu cores", default='2')
    parser.add_argument(
        "--wait-gdb", action='store_true', help="pause QEMU until GDB connects")
    parser.add_argument(
        "--gdb-port", help="GDB TCP port; replaces the default -s/1234 listener")
    parser.add_argument(
        "--qemu-debug", action='store_true', help="write QEMU internal debug output")
    parser.add_argument(
        "--monitor", help="QEMU monitor Unix socket path")
    parser.add_argument(
        "--no-reboot", action='store_true', help="keep QEMU stopped after guest shutdown or reset")

    args = parser.parse_args()

    try:
        if args.iso:
            prepare_iso()
        else:
            if args.uefi:
                base_mnt = mount_point("../run/image/disk.img", 2)
            else:
                base_mnt = mount_point("../run/image/disk.img", 1)

            if base_mnt == "" or base_mnt is None:
                print("Mount disk before run.\n    try 'python disk.py mount'")
                exit(-1)

            run_shell('mkdir -p ' + base_mnt + '/boot/')
            run_shell('cp -R ../build/bin/system/* ' + base_mnt + '/boot/')
            run_shell('sync')

        tp = args.emulator_name
        if tp == 'q':
            subprocess.run(build_qemu_argv(args), check=True)

        elif tp == 'b':
            print('run bochs')
            run_shell_input(bochs)
        elif tp == 'v':
            print('run VBox')
            run_shell(vbox)
    except Exception:
        traceback.print_exc()
        parser.print_help()
        exit(-1)
