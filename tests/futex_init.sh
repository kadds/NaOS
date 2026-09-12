#!/bin/sh

/bin/naos-smoke-suite --futex
status=$?
echo "futex-init: status=${status}"
# The script, not the smoke, ends the boot; a failed power-off must not
# change the reported status.
/bin/poweroff
exit "$status"
