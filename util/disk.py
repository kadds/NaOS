#!/usr/bin/env python3
from mod import set_self_dir, run_shell
import sys
import traceback
import os
import argparse

import platform
import sys
import subprocess


system = platform.system()
gdisk_param = '''n
2
2048
43007
8300
n
3
43008

8300
n
1
34
2047
ef02
w
Y
'''

boot_grub = '''root=(hd0,gpt3)
set default=0
set timeout=1
menuentry "NaOS multiboot2" {
multiboot2 /system/kernel
module2 /system/rfsimg rfsimg
boot
}
'''
base_mnt = "../run/image/mnt/"


def mount(output_file):
    current_loop_device = run_shell('losetup -f').strip('\n')
    print("mount device is %s" % (current_loop_device))
    run_shell('losetup --partscan ' +
              current_loop_device + ' "' + output_file + '"')
    run_shell('mount ' + current_loop_device + 'p2 ' + base_mnt + 'boot')
    run_shell('mount ' + current_loop_device + 'p3 ' + base_mnt + 'disk')
    return current_loop_device


def umount(loop_device):
    run_shell('umount ' + base_mnt + 'boot')
    run_shell('umount ' + base_mnt + 'disk')
    run_shell('losetup -d ' + loop_device)


def make_disk(output_file, image_size):

    if image_size < 100:
        image_size = 100

    print("output file is %s, size %d MB" %
          (output_file, image_size))

    # make disk first
    print(run_shell('dd if=/dev/zero of="' + output_file +
                    '" count=' + str(image_size * 2048) + ' bs=512'))

    # partion
    run_shell('gdisk ' + output_file, gdisk_param)
    print(run_shell('gdisk -l ' + output_file))
    current_loop_device = run_shell('losetup -f').strip('\n')
    run_shell('losetup --partscan ' +
              current_loop_device + ' "' + output_file + '"')
    run_shell('mkfs.ext2 ' + current_loop_device + 'p1')
    run_shell('mkfs.vfat -F 16 ' + current_loop_device + 'p2')
    run_shell('mkfs.vfat -F 32 ' + current_loop_device + 'p3')
    run_shell('losetup -d ' + current_loop_device)

    loop_device = mount(output_file)

    run_shell('grub-install --boot-directory=' + base_mnt +
              'boot --target=i386-pc --modules="part_gpt" ' + output_file)
    file = open(base_mnt + 'boot/grub/grub.cfg', 'w+', encoding='utf-8')
    file.write(boot_grub)
    file.close()

    umount(loop_device)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description='ram disk tools: create, mount, umount')

    sub_parser = parser.add_subparsers(
        title='sub operation', dest='subparser_name')
    create_parser = sub_parser.add_parser("create")
    mount_parser = sub_parser.add_parser("mount")
    umount_parser = sub_parser.add_parser("umount")

    create_parser.add_argument(
        "-o", "--output", type=str, default="../run/image/disk.img", help="output target file")
    create_parser.add_argument(
        "-s", "--size", type=int, default=100, help="disk size(Mib)")

    mount_parser.add_argument(
        "-i", "--input", type=str, default="../run/image/disk.img", help="disk file to mount")

    umount_parser.add_argument(
        "-d", "--dev", type=str, default="/dev/loop0", help="loop device to umount")

    args = parser.parse_args()
    if args.subparser_name == None:
        parser.print_help()
        exit(-1)

    try:
        set_self_dir()
        if args.subparser_name == "create":
            filename = args.output
            size = args.size
            make_disk(filename, size)
        elif args.subparser_name == "mount":
            filename = args.input
            mount(filename)
        elif args.subparser_name == "umount":
            name = args.dev
            umount(name)
        else:
            parser.print_help()
            exit(-1)
        print("Ok")

    except Exception:
        traceback.print_exc()
        parser.print_help()
        exit(-1)
