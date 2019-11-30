#!/usr/bin/env python3
import platform
import sys
import subprocess
import os
import signal
import argparse


def run_shell(shell_params, input=None, print_command=True):
    if print_command:
        print("> %s" % (shell_params))
    r = subprocess.Popen(shell_params, universal_newlines=True, shell=True, stdout=subprocess.PIPE,
                         stdin=subprocess.PIPE).communicate(input=input)
    if r[1] != None:
        raise Exception(r[1])
    return r[0]


def run_shell_input(shell_params, input=None, print_command=False):
    if print_command:
        print("> %s" % (shell_params))
    r = subprocess.Popen(shell_params, universal_newlines=True, shell=True,
                         stdout=sys.stdout, stderr=sys.stderr, stdin=sys.stdin)

    def exit(signum, frame):
        print("kill")
        print(r)
        r.send_signal(signal.SIGINT)

    signal.signal(signal.SIGINT, exit)
    signal.signal(signal.SIGTERM, exit)

    r.wait()


def set_self_dir():
    check_version()
    os.chdir(os.path.abspath(os.path.dirname(sys.argv[0])))
    print('working dir: %s' %
          os.path.abspath(os.path.dirname(sys.argv[0])))


def check_version():
    assert (sys.version_info.major >= 3)
