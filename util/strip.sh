#!/bin/bash
# This script minimizes the binary file

Target=$1
To=$2
DBGTarget=${To}.dbg
STarget=${To}.dbg.S
echo "target is "$Target
dir=`dirname ${DBGTarget}`
mkdir -p ${dir}
mv ${Target} ${DBGTarget}
strip -d -S ${DBGTarget} -o ${Target}
