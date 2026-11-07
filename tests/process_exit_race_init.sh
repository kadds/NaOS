#!/bin/sh

i=0
while [ "$i" -lt 30 ]; do
    /bin/ls || {
        echo "process-exit-race: ls failed at iteration $i"
        exit 1
    }
    i=$((i + 1))
done

echo "process-exit-race: completed 30 ls processes"
exit 0
