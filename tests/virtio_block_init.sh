#!/bin/sh

/bin/virtio-block-smoke
status=$?
echo "virtio block smoke exit=${status}"
/bin/reboot
exit "$status"
