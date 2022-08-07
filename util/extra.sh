#!/bin/bash
# This script minimizes the kernel

Debug=$4
Type=$3
Target=$1
To=$2
DBGTarget=${To}.dbg
STarget=${To}.dbg.S
echo "target is "$Target
dir=`dirname ${DBGTarget}`
mkdir -p ${dir}
mv ${Target} ${DBGTarget}
if [ "$Debug"x == "Debug"x ]; then
   objcopy -O binary -R .note -R .comment -R .dyn*  -R .interp  -R .debug* -K .unpaged.* -S ${DBGTarget} ${Target}
else
   objcopy -O binary -R .note -R .comment -R .dyn*  -R .interp -R .debug* -K .unpaged.* -S -g ${DBGTarget} ${Target}
fi
