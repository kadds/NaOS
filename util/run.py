#!/usr/bin/env python3
import sys
import os
import argparse
import traceback

from mod import set_self_dir, run_shell, run_shell_input
# -monitor stdio
qemu = 'qemu-system-x86_64 -hda ../run/image/disk.img -m 24 -s -smp 2,sockets=1,cores=2 -serial file:kernel_out.log'
qemu_headless = qemu + ' -nographic -vnc :0'
bochs = 'bochs -f ../run/cfg/bochs/bochsrc.txt'
vbox_image = 'VBoxManage internalcommands createrawvmdk -filename ../run/image/disk.vmdk -rawdisk /dev/loop0'
vbox = 'VBoxManage startvm boot'

if __name__ == "__main__":
    set_self_dir()

    parser = argparse.ArgumentParser(
        description='run tools: run kernel')
    parser.add_argument(
        "-n", "--nographic",  action='store_true', help="try don't show window")
    parser.add_argument("emulator_name", type=str,
                        choices=["q", "b", "v"],  help="q: run qemu\nb: run bochs\nv: run virtual box")

    args = parser.parse_args()
    if os.path.exists("../run/image/mnt/boot/grub/grub.cfg") == False:
        print("Can't run kernel at 'run/image/mnt/'. The directory isn't mount. Mount disk before run.\n    try 'sudo python disk.py mount'")
        exit(-1)

    try:
        run_shell('cp -R ../build/bin/system/ ../run/image/mnt/disk')
        run_shell('sync')
        tp = args.emulator_name
        if tp == 'q':
            if args.nographic:
                print('run qemu without graphic')
                run_shell(qemu_headless)
            else:
                print("run qemu")
                run_shell(qemu)

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
