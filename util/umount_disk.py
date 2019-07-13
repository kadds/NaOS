from make_disk import umount 
import sys, os
from mod import set_self_dir
if __name__ == "__main__":
    dev = ''
    if len(sys.argv) <= 1:
        dev = '/dev/loop0'
    else:
        dev = sys.argv[1]
    set_self_dir()
    umount(dev)