#!/bin/bash
set -e

# This script minimizes the binary file

Target=$1
To=$2
DBGTarget=${To}.dbg
echo "target is ${Target}"
dir=$(dirname -- "${DBGTarget}")
mkdir -p "${dir}"
mv -- "${Target}" "${DBGTarget}"
strip -s "${DBGTarget}" -o "${Target}"
