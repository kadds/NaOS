# This program packs files to rfs format
# rfs format: | magic | version | file counts | file0 | file1 | filex... |
# each of files: | file size | file contents |
# 

from mod import set_self_dir, run_shell
import sys
import traceback
import os
import argparse

import platform
import sys
import subprocess
import struct
import time
import datetime

cache_file_name = "pack_cache.log"


def pack_image(base_dir, target_file, force):
    try:
        os.makedirs(os.path.dirname(target_file))
    except FileExistsError:
        pass
    run_shell('tar --format=gnu -cvf "' + target_file + '" "' + base_dir + '"')

    output = open(target_file, 'r')
    print("make %s success size %d" % (os.path.realpath(target_file), os.path.getsize(target_file)))
    output.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description='image pack tools: Packaging root fs image')
    parser.add_argument(
        "-f", "--force",  action='store_true', help="force image generation")
    parser.add_argument("-o", "--output", type=str,
                        default="../build/bin/system/rfsimg", help="output image file")
    parser.add_argument("-i", "--input", type=str,
                        default="../build/bin/rfsroot", help="directory to be packaged")
    args = parser.parse_args()
    try:
        set_self_dir() 
        base_dir_abs = os.path.realpath(args.input)
        target_file_abs = os.path.realpath(args.output)

        os.chdir(base_dir_abs)
        pack_image('./', target_file_abs, args.force)
    except Exception:
        traceback.print_exc()
        parser.print_help()
        exit(-1)
