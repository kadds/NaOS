#!/usr/bin/env python3
import platform, sys, subprocess, os

def run_shell(shell_params, input=None):
    print("run shell: %s" % (shell_params))
    r = subprocess.Popen(shell_params, text=True, shell=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, stdin=subprocess.PIPE).communicate(input=input)
    if r[0] != "":
        if r[1] != "":
            return r[0] + "\n" + r[1]
        else:
            return r[0]
    else:
        if r[1] != "":
            return r[1]
        else:
            return ""
def run_shell_input(shell_params, input=None):
    print("run shell: %s" % (shell_params))
    r = subprocess.Popen(shell_params, text=True, shell=True, stdout=sys.stdout, stderr=sys.stderr, stdin=sys.stdin)
    r.wait()
    
def set_self_dir():
    os.chdir(os.path.abspath(os.path.dirname(sys.argv[0])))
    print('set working dir: %s' % os.path.abspath(os.path.dirname(sys.argv[0])))