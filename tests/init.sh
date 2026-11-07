#!/bin/sh

failed=0

run_one() {
    name="$1"
    shift
    echo "smoke: ${name}: start"
    "$@"
    status=$?
    if [ "$status" -eq 0 ]; then
        echo "smoke: ${name}: pass"
    else
        echo "smoke: ${name}: fail (${status})"
        failed=1
    fi
}

run_one service-directory-scope /bin/naos-smoke-suite --service-directory-scope
run_one naos-smoke-suite /bin/naos-smoke-suite
run_one rust-smoke-suite /bin/rust-smoke-suite
run_one virtio-block-smoke /bin/virtio-block-smoke

if [ "$failed" -eq 0 ]; then
    echo "smoke: all tests passed"
else
    echo "smoke: one or more tests failed"
fi
exit "$failed"
