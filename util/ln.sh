#!/bin/bash
set -e

r=${1}/bin
mkdir -p "${r}"

for applet in sh ls cat echo pwd true false mkdir rmdir touch rm env cp mv grep sed find xargs test sleep; do
    ln -sf /bin/busybox "${r}/${applet}"
done

# Keep the original NaOS shell entry point available for existing scripts.
ln -sf /bin/nanobox "${r}/nsh"

# Machine teardown is the boot driver's decision, not a test binary's: an
# opt-in /etc/init.sh runs a smoke and then powers the machine off so a QEMU
# run ends on the guest's own schedule instead of the launcher's wall clock.
ln -sf /bin/nanobox "${r}/poweroff"
