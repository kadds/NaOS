#!/usr/bin/env python3
import sys
import os
from mod import run_shell, set_self_dir


if __name__ == "__main__":
    set_self_dir()
    base = "../build/"
    os.mkdir(base)
