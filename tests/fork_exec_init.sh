#!/bin/sh

/bin/busybox sleep 12
status=$?
/bin/naos-smoke-suite --mlibc-tls
status=$?
echo "fork-exec-init: external-status=${status}"
# The script, not the smoke, ends the boot; a failed power-off must not
# change the reported status.
/bin/poweroff
exit "$status"
