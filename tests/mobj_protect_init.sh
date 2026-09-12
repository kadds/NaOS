#!/bin/sh

# Opt-in hook for the MemoryObject page-table protection witness.  This case
# deliberately stores through a read-only mapping, so the kernel reports a
# user-space fault and delivers SIGSEGV; the boot predicate classifies that
# line as a fault, which is why this script is not wired into the gated ctest
# graph.  Drive it explicitly and read the verdict from the kernel log:
#
#   python3 util/run.py --build-dir build \
#       --init-script tests/mobj_protect_init.sh q --iso -n --no-reboot
#   grep "mobj-protect" build/kernel_out.log
#
# The expected pair is the kernel's "exception 14 occurred at ... pid ..."
# line followed by "mobj-protect: PASS denied=1 witness=1 unchanged=1".
/bin/naos-smoke-suite --mobj-protect
status=$?
echo "mobj-protect-init: status=${status}"
/bin/poweroff
exit "$status"
