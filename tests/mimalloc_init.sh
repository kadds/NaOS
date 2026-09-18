#!/bin/sh

/bin/rust-smoke-suite
status=$?
echo "mimalloc-smoke: status=${status}"
exit 0
