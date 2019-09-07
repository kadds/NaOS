#!/bin/bash
Debug=$4
Type=$3
Target=$1
To=$2
DBGTarget=${To}.dbg
STarget=${To}.dbg.S
echo "target is "$Target

mv ${Target} ${DBGTarget}
if [ "$Debug"x == "Debug"x ]; then
    objcopy -O binary -R .note -R .comment -S ${DBGTarget} ${Target}
else
    objcopy -O binary -R .note -R .comment -g -S ${DBGTarget} ${Target}
fi
