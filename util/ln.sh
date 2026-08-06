#!/bin/bash
set -e

r=${1}/bin
mkdir -p "${r}"

for applet in sh ls cat echo pwd true false mkdir rmdir touch rm env cp mv grep sed find xargs test sleep; do
    ln -sf /bin/busybox "${r}/${applet}"
done

# Keep the original NaOS shell entry point available for existing scripts.
ln -sf /bin/nanobox "${r}/nsh"
