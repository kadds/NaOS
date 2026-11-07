#!/bin/sh

echo "smoke: rust-allocator-quota: start"
/bin/rust-smoke-suite --allocator-quota-only
status=$?
if [ "$status" -eq 0 ]; then
    echo "smoke: rust-allocator-quota: pass"
else
    echo "smoke: rust-allocator-quota: fail ($status)"
fi
exit "$status"
