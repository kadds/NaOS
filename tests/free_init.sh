#!/bin/sh

/bin/busybox free
status=$?
echo "free-smoke: status=${status}"
exit "$status"
