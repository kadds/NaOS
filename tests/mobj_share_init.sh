#!/bin/sh

# Opt-in acceptance hook for the MemoryObject shared-page data plane.  The
# default production image contains an intentionally empty /etc/init.sh; this
# script is injected only by the test-specific wrapper for boot_smoke_mobj_share.
# It runs after init has started the system services,
# and the smoke binary owns its own process setup, so no test logic belongs in
# init itself.
#
# The script, not the smoke, ends the boot: once the verdict is out there is
# nothing left to observe, so the machine powers off immediately instead of
# idling until the launcher's wall-clock limit.  A failed power-off must not
# change the reported status.
/bin/naos-smoke-suite --mobj-share
status=$?
echo "mobj-share-init: status=${status}"
/bin/poweroff
exit "$status"
