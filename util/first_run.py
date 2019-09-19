#!/usr/bin/env python3
import sys
import os
from mod import run_shell, set_self_dir


if __name__ == "__main__":
    set_self_dir()
    base = "../build/"
    os.mkdir(base)
    os.mkdir(base + "debug/")
    os.mkdir(base + "debug/loader/")
    os.mkdir(base + "debug/system/")
    os.mkdir(base + "debug/rfsimage/")
