#!/bin/sh

/bin/virtio-block-smoke
status=$?
echo "virtio block fault smoke exit=${status}"
/bin/reboot
exit "$status"
