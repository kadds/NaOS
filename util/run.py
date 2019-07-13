#!/usr/bin/env python3
import sys, os
from mod import set_self_dir, run_shell, run_shell_input
qemu = 'qemu-system-x86_64 -hda ../run/image/disk.img -m 1024 -s &'
bochs = 'bochs -f ../run/cfg/bochs/bochsrc.txt'
vbox_image = 'VBoxManage internalcommands createrawvmdk -filename ../run/image/disk.vmdk -rawdisk /dev/loop0'
vbox = 'VBoxManage startvm boot'
if __name__ == "__main__":
    set_self_dir()
    run_shell('cp -R ../build/bin/loader/* ../run/image/mnt/boot')
    run_shell('cp -R ../build/bin/system/ ../run/image/mnt/disk')
    run_shell('sync')
    if len(sys.argv) >= 2:
        tp = sys.argv[1]
        if tp == 'q':
            print("run qemu")
            run_shell(qemu)
        elif tp == 'b':
            print('run bochs')
            run_shell_input(bochs)
        elif tp == 'v':
            print('run VBox')
            run_shell(vbox)
        else:
            print('error lauch type "%s" must be b(bochs), q(qemu) or v(virtual box)' % (tp))
    else:
        print('null lauch type')

    