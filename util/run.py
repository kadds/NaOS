#!/usr/bin/env python3
import sys
import os
import argparse
import traceback
from disk import mount_point
from mod import set_self_dir, run_shell, run_shell_input

'''
How to build VMbox image?
VBoxManage internalcommands createrawvmdk -filename run/image/disk.vmdk -rawdisk run/image/disk.img
'''

ovmf_path = '/usr/share/ovmf/x64/OVMF_CODE.fd'


qemu = 'qemu-system-x86_64 -drive file=../run/image/disk.img,format=raw,index=0 -m 64 -s -smp 4,sockets=1,cores=4 -serial file:../run/kernel_out.log'
qemu_uefi = 'qemu-system-x86_64 -drive file=' + ovmf_path + ',format=raw,readonly,if=pflash -drive file=../run/image/disk.img,format=raw,index=0 -m 64 -s -smp 4,sockets=1,cores=4 -serial file:../run/kernel_out.log'
qemu_headless_str = ' -nographic -vnc :0'

bochs = 'bochs -f ../run/cfg/bochs/bochsrc.txt'
vbox = 'VBoxManage startvm boot'

if __name__ == "__main__":
    set_self_dir()

    parser = argparse.ArgumentParser(
        description='run tools: run kernel')
    parser.add_argument(
        "-n", "--nographic",  action='store_true', help="try don't show window")
    parser.add_argument(
        "-u", "--uefi",  action='store_true', help="open qemu with uefi firmware")
    parser.add_argument("emulator_name", type=str,
                        choices=["q", "b", "v"],  help="q: run qemu\nb: run bochs\nv: run virtual box")
    args = parser.parse_args()
    base_mnt = None
    if args.uefi:
        base_mnt = mount_point("../run/image/disk.img", 2)
    else:
        base_mnt = mount_point("../run/image/disk.img", 1)

    if base_mnt == "" or base_mnt == None:
        print("Mount disk before run.\n    try 'python disk.py mount'")
        exit(-1)

    try:
        run_shell('cp -R ../build/bin/system/* ' + base_mnt + '/boot/')
        run_shell('sync')
        tp = args.emulator_name
        if tp == 'q':
            if args.uefi:
                print('run qemu uefi')
                if args.nographic:
                    run_shell(qemu_uefi + qemu_headless_str)
                else:
                    run_shell(qemu_uefi)
            else:
                print("run qemu")
                if args.nographic:
                    run_shell(qemu + qemu_headless_str)
                else:
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
