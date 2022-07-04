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

    fileList = {}
    try:
        os.makedirs(os.path.dirname(os.path.realpath(target_file)))
    except FileExistsError:
        pass
    for dirs in os.walk(base_dir):
        if len(dirs[2]) != 0:
            basefile = dirs[0]
            for file in dirs[2]:
                name = basefile.split(base_dir)[1] + "/" + file
                full_path = basefile + "/" + file
                time = datetime.datetime.fromtimestamp(
                    os.path.getmtime(full_path)).strftime("%Y-%m-%d %H:%M:%S.%f")
                fileList[name] = time

    # read cache
    cache_count = 0
    if os.path.exists(cache_file_name) and not force:
        cache_file = open(cache_file_name, "r")
        cache_line = cache_file.readlines()
        if len(cache_line) > 0:
            target = cache_line[0].strip()
            if target == target_file:
                for it in cache_line:
                    line = it.split("?")
                    if len(line) <= 1:
                        continue
                    filename = line[0].strip()
                    time = line[1].strip()
                    if filename in fileList:
                        if fileList[filename] == time:
                            cache_count += 1
        cache_file.close()
        if cache_count == len(fileList):
            print("%d file(s) is cached. do nothing." % (cache_count))
            return None

    output = open(target_file, 'wb')
    output.write(struct.pack("Q", 0xF5EEEE5F))  # magic
    output.write(struct.pack("Q", 1))  # version
    output.write(struct.pack("Q", len(fileList)))  # file count

    cache_file = open(cache_file_name, "w")
    cache_file.write(target_file + "\n")
    for (file, time) in fileList.items():
        output.write(struct.pack(str(len(file) + 1) + "s",
                                 file.encode('utf-8')))  # file path
        cur_file = open(base_dir + "/" + file, 'rb')
        data = cur_file.read()
        p = output.tell()
        output.write(struct.pack("Q", len(data)))  # file length
        output.write(data)
        cache_file.write(file)
        cache_file.write("?")
        cache_file.write(str(time))
        cache_file.write("?")
        cache_file.write(str(p))
        cache_file.write("\n")

    output.close()
    cache_file.close()
    print("handle %d file(s)" % len(fileList))
    print("make %s success" % os.path.realpath(target_file))


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
        pack_image(args.input, args.output, args.force)
    except Exception:
        traceback.print_exc()
        parser.print_help()
        exit(-1)
