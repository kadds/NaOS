#!/usr/bin/env python3
import os
import struct
from mod import set_self_dir, run_shell
import argparse
import traceback
import datetime

cache_file_name = "ksybs_cache.log"


def gen_symbols(file, target_file, force):
    try:
        os.makedirs(os.path.dirname(os.path.realpath(target_file)))
    except FileExistsError:
        pass

    if os.path.exists(cache_file_name) and not force:
        cache_file = open(cache_file_name, "r")
        cache_line = cache_file.readlines()
        if len(cache_line) == 1:
            line = cache_line[0].split("?")
            filename = line[0].strip()
            if len(line) > 1 and filename == file:
                time = line[1].strip()
                if time == datetime.datetime.fromtimestamp(
                        os.path.getmtime(file)).strftime("%Y-%m-%d %H:%M:%S.%f"):
                    print("ksybs is cached. do nothing.")
                    cache_file.close()
                    return None

        cache_file.close()

    smps = run_shell("nm \"" + file +
                     "\" | cut -d ' ' -f 1,2,3 | c++filt |sort | uniq", None, False)
    lines = smps.splitlines(False)
    output = open(target_file, 'wb')
    output.write(struct.pack("Q", 0xF0EAEACC))  # magic
    output.write(struct.pack("Q", 1))  # version
    output.write(struct.pack("Q", len(lines)))  # list count

    print("revice %d symbols" % len(lines))
    offset = 0
    list = []
    for line in lines:
        v = line.split(" ")
        addr = v[0].strip()
        name = " ".join(v[2:])
        type_name = v[1].strip()

        output.write(struct.pack("Q", int(addr, 16)))
        output.write(struct.pack("Q", offset))
        list.append((name,  offset, type_name))
        offset += len(name.encode("utf-8")) + 2

    for it in list:
        output.write(struct.pack("c", it[2].encode("utf-8")))  # type
        output.write(struct.pack(str(len(it[0]) + 1) + "s",
                                 it[0].encode('utf-8')))  # name

    output.close()

    cache_file = open(cache_file_name, "w")
    cache_file.write(file + "?" + datetime.datetime.fromtimestamp(
        os.path.getmtime(file)).strftime("%Y-%m-%d %H:%M:%S.%f"))
    cache_file.close()

    print("make %s success" % (os.path.realpath(target_file)))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description='symbol tools: generate kernel debug file (ksybs)')
    parser.add_argument(
        "-f", "--force",  action='store_true', help="force generation")
    parser.add_argument("-o", "--output", type=str,
                        default="../build/bin/rfsroot/data/ksybs", help="output ksybs file")
    parser.add_argument("-i", "--input", type=str,
                        default="../build/debug/system/kernel.dbg", help="kernel file with debug info")
    args = parser.parse_args()
    try:
        set_self_dir()
        gen_symbols(args.input, args.output, args.force)
    except Exception:
        traceback.print_exc()
        parser.print_help()
        exit(-1)
