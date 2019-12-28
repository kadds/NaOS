#!/usr/bin/env python3
from mod import set_self_dir, run_shell, run_shell_input
import sys
import traceback
import os
import argparse

import platform
import sys
import subprocess
import configparser

system = platform.system()

fdisk_mbr= '''o
n
p
1
4096
{}
N
n
p
2
{}
{}
N
p
w
'''

boot_grub = '''root=(hd0,msdos2)
set default=0
set timeout=1
menuentry "NaOS multiboot2" {
multiboot2 /system/kernel
module2 /system/rfsimg rfsimg
boot
}
'''
cfg_file_name = "./disk_mount_status.log"

def get_mount_loop_device(image_file):
    image_file = os.path.realpath(image_file)
    config = configparser.ConfigParser()
    config.read(cfg_file_name)
    if config.has_section(image_file):
        dev = config.get(image_file, "loop_device")
        if dev == "":
            return ""
        point = run_shell('udisksctl info -b ' + dev  + ' | grep BackingFile | awk \'{print $2}\'', print_command = False).strip('\n')
        if point != image_file:
            config.set(image_file, "loop_device", "")
            config.write(open(cfg_file_name, 'w'))
            return ""
        return dev
    return ""

def mount(image_file):
    image_file = os.path.realpath(image_file)
    current_loop_device = get_mount_loop_device(image_file)
    if current_loop_device == "":
        current_loop_device = run_shell('udisksctl loop-setup -f ' + image_file + " | awk '{print $5}'").strip('\n')
        config = configparser.ConfigParser()
        config.read(cfg_file_name)
        if config.has_section(image_file) == False:
            config.add_section(image_file)
        config.set(image_file, "loop_device", "")
        assert current_loop_device != ''
        current_loop_device = current_loop_device[0:-1]
        config.set(image_file, "loop_device", current_loop_device)
        config.write(open(cfg_file_name, 'w'))

    print(current_loop_device)


def mount_point(image_file, pat):
    loop_device = get_mount_loop_device(image_file)
    if loop_device != "":
        point = run_shell('udisksctl info -b ' + loop_device  + 'p' + str(pat) + ' | grep MountPoints | awk \'{print $2}\'').strip('\n')
        return point
    return None


def umount(image_file):
    loop_device = get_mount_loop_device(image_file)
    if loop_device != "":
        print(run_shell('udisksctl loop-delete -b ' + loop_device))
        print(run_shell('udisksctl unmount -b ' + loop_device + 'p1'))
        print(run_shell('udisksctl unmount -b ' + loop_device + 'p2'))
        print(run_shell('udisksctl unmount -b ' + loop_device + 'p3'))

def confirm(msg):
    if input(msg + " [y/N]:") == "y":
        return True
    return False


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description='ram disk tools: create, mount, umount')

    sub_parser = parser.add_subparsers(
        title='sub operation', dest='subparser_name')
    mount_parser = sub_parser.add_parser("mount")
    umount_parser = sub_parser.add_parser("umount")
    getpoint_parser = sub_parser.add_parser("getpoint")
    getloop_parser = sub_parser.add_parser("getloop")

    mount_parser.add_argument(
        "-i", "--input", type=str, default="../run/image/disk.img", help="disk file to mount")

    umount_parser.add_argument(
        "-i", "--input", type=str, default="../run/image/disk.img", help="disk file to unmount")
    getpoint_parser.add_argument(
        "-i", "--input", type=str, default="../run/image/disk.img", help="disk file to get mount point")
    getloop_parser.add_argument(
        "-i", "--input", type=str, default="../run/image/disk.img", help="disk file to get loop device")

    args = parser.parse_args()
    if args.subparser_name == None:
        parser.print_help()
        exit(-1)

    try:
        set_self_dir()
        if args.subparser_name == "mount":
            mount(args.input)
        elif args.subparser_name == "umount":
            umount(args.input)
        elif args.subparser_name == "getpoint":
            print(mount_point(args.input, 2))
            print(mount_point(args.input, 3))
        elif args.subparser_name == "getloop":
            loop = get_mount_loop_device(args.input)
            if loop == "":
                print("No loop device")
            else :
                print(loop)
        else:
            parser.print_help()
            exit(-1)
        print("Ok")

    except Exception:
        traceback.print_exc()
        parser.print_help()
        exit(-1)
