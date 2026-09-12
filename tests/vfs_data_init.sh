#!/bin/sh

# Opt-in process-boundary smoke.  The default production image contains an
# intentionally empty /etc/init.sh; this script is injected only by the
# test-specific boot wrapper and exercises the mounted userspace FAT data plane.
# The hook runs only after init has acquired the committed root route. The
# wrapper checks the final smoke verdict in the serial log.
#
# The script, not the smoke, ends the boot: once the verdict is out there is
# nothing left to observe, so the machine powers off instead of idling until
# the launcher's wall-clock limit.  A failed power-off must not change the
# reported status.
/bin/naos-smoke-suite --vfs-data
status=$?
echo "vfs-data-init: status=${status}"
/bin/poweroff
exit "$status"
