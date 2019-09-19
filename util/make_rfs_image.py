#!/usr/bin/env python3
import os
import struct
from mod import set_self_dir


def pack_image(base_dir, target_file):
    fileList = []

    for dirs in os.walk(base_dir):
        if len(dirs[2]) != 0:
            basefile = dirs[0]
            for file in dirs[2]:
                fileList.append(basefile.split(base_dir)[1] + "/" + file)
    print(fileList)

    output = open(target_file, 'wb')
    output.write(struct.pack("Q", 0xF5EEEE5F))  # magic
    output.write(struct.pack("Q", 1))  # version
    output.write(struct.pack("Q", len(fileList)))  # file count

    for file in fileList:
        output.write(struct.pack(str(len(file) + 1) + "s",
                                 file.encode('utf-8')))  # file path
        cur_file = open(base_dir + "/" + file, 'rb')
        data = cur_file.read()
        output.write(struct.pack("L", len(data)))  # file length
        output.write(data)

    output.close()
    print("handle %d file(s)" % len(fileList))
    print("make %s success" % os.path.realpath(target_file))


if __name__ == "__main__":
    set_self_dir()
    pack_image("../build/bin/rfsimage", "../build/bin/system/rfsimg")
