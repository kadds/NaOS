#!/usr/bin/env python3

import platform, sys, subprocess, os
from mod import run_shell, set_self_dir

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

boot_grub = '''menuentry "NaOS" {
multiboot (hd0,gpt2)/loader-multiboot
boot
}
'''
base_mnt = "../run/image/mnt/"

def mount(output_file):
    current_loop_device = run_shell(['losetup -f']).strip('\n')
    print("mount device is %s" % (current_loop_device))
    run_shell(['losetup --partscan ' + current_loop_device + ' "' + output_file + '"'])
    print(run_shell(['mount ' + current_loop_device + 'p2 ' + base_mnt + 'boot']))
    print(run_shell(['mount ' + current_loop_device + 'p3 ' + base_mnt + 'disk']))
    return current_loop_device
def umount(loop_device):
    print(run_shell(['umount ' + base_mnt + 'boot']))
    print(run_shell(['umount ' + base_mnt + 'disk']))
    print(run_shell(['losetup -d ' + loop_device]))

def make_disk():
    output_file = "../run/image/disk.img"

    if len(sys.argv) > 1:
        image_size = int(sys.argv[1])
    else:
        image_size = 100
        
    if image_size < 100:
        image_size = 100

    print("System is %s, output file is %s, size %d MB" % (system, output_file, image_size))

    if system == "Linux":
        #make disk first
        print(run_shell('dd if=/dev/zero of="' + output_file + '" count=' + str(image_size * 2048) + ' bs=512'))

        # partion
        run_shell(['gdisk ' + output_file], gdisk_param)
        print(run_shell(['gdisk -l ' + output_file]))
        current_loop_device = run_shell(['losetup -f']).strip('\n')
        run_shell(['losetup --partscan ' + current_loop_device + ' "' + output_file + '"'])
        run_shell(['mkfs.ext2 ' + current_loop_device + 'p1'])
        run_shell(['mkfs.vfat -F 16 ' + current_loop_device + 'p2'])
        run_shell(['mkfs.vfat -F 32 ' + current_loop_device + 'p3'])
        run_shell(['losetup -d ' + current_loop_device])

        loop_device = mount(output_file)

        run_shell('grub-install --boot-directory=' + base_mnt + 'boot --target=i386-pc --modules="part_gpt" ' + output_file)
        file = open(base_mnt + 'boot/grub/grub.cfg', 'w+', encoding='utf-8')
        file.write(boot_grub)
        file.close()

        umount(loop_device)
        print("Ok")
    else:
        print("Unsupport system.")

if __name__ == "__main__":
    set_self_dir()
    make_disk()