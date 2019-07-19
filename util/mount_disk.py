#!/usr/bin/env python3
from make_disk import mount
from mod import set_self_dir
import sys
import os

if __name__ == "__main__":
    if len(sys.argv) <= 1:
        file = '../run/image/disk.img'
    else:
        file = sys.argv[1]
    set_self_dir()
    mount(file)
