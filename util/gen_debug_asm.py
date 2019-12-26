#!/usr/bin/env python3
import sys
import os
import argparse
import traceback
from mod import set_self_dir, run_shell

if __name__ == "__main__":
    try:
        targets = []
        set_self_dir()
        path = "../build/debug/"
        fileMap = {}

        choices = []
        for dirs in os.walk(path):
            if len(dirs[2]) != 0:
                basefile = dirs[0]
                for file in dirs[2]:
                    if os.path.splitext(file)[1] == ".dbg":
                        filename = os.path.splitext(file)[0]
                        fileMap.update({filename: basefile + "/" + file})
                        choices.append(filename)

        parser = argparse.ArgumentParser(
            description='debug tools: generate kernel debug file (ksybs)')
        parser.add_argument(
            "-d", "--detail",  action='store_true', help="show source file and line information")
        parser.add_argument(
            "-s", "--source",  action='store_true', help="show source code")
        parser.add_argument("component", type=str, nargs="+",
                            choices=choices,  help="the component want to decompile")
        args = parser.parse_args()

        cmd = "objdump "

        if args.detail:
            cmd += "-l "
        if args.source:
            cmd += "-S "
        cmd += "-d --section=.text -C -l -m"   

        targets = args.component
        for target in targets:
            tp = fileMap.get(target, "")
            if tp != "":
                print("find target at ", tp)
                run_shell(cmd + "i386:x86-64 " + tp + " > " +
                          os.path.split(tp)[0] + "/" + target + ".dbg.S")
        print("%d target(s) is generated." % (len(targets)))
    except Exception:
        traceback.print_exc()
        parser.print_help()
        exit(-1)
