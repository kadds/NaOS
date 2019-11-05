#!/usr/bin/env python3
import os
import struct
from mod import set_self_dir, run_shell


def gen_symbols(file, target_file):
    try:
        os.makedirs(os.path.dirname(os.path.realpath(target_file)))
    except FileExistsError:
        pass
    smps = run_shell("nm \"" + file +
                     "\" | cut -d ' ' -f 1,2,3 | c++filt | sort", None, False)
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

    print("make %s success" % (os.path.realpath(target_file)))


if __name__ == "__main__":
    set_self_dir()
    gen_symbols("../build/debug/system/kernel.dbg",
                "../build/bin/rfsimage/data/ksybs")
