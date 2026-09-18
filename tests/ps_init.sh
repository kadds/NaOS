#!/bin/sh
/bin/busybox ps
status=$?
if [ "$status" -eq 0 ]; then
    /bin/busybox ps -T
    status=$?
fi
echo "ps-smoke: status=${status}"
exit "$status"
