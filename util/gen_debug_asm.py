#!/usr/bin/env python3
import sys
import os
from mod import set_self_dir, run_shell

detail_cmd = "objdump -d --section=.text -C -l -m"
sp_cmd = "objdump -d --section=.text -C -m"

if __name__ == "__main__":
    targets = []
    set_self_dir()
    path = "../build/debug/"
    if len(sys.argv) <= 1:
        print("no args")
    else:
        fileMap = {}
        cmd = sp_cmd
        targets = []
        if sys.argv[1] == "-d":
            cmd = detail_cmd
            print("detail mode: show line info")
            targets = sys.argv[2:]
        else:
            targets = sys.argv[1:]

        for dirs in os.walk(path):
            if len(dirs[2]) != 0:
                basefile = dirs[0]
                for file in dirs[2]:
                    if os.path.splitext(file)[1] == ".dbg":
                        fileMap.update({os.path.splitext(file)[
                            0]: basefile + "/" + file})

        for target in targets:
            tp = fileMap.get(target, "")
            if tp != "":
                print("find target at ", tp)
                run_shell(cmd + "i386:x86-64 " + tp + " > " +
                          os.path.split(tp)[0] + "/" + target + ".dbg.S")
