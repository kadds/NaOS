#!/bin/sh
# Generates fat32-fixture.img.gz in the requested build output directory.
#
# The fixture is a genuine FAT32 volume: 512-byte sectors, 1 sector per
# cluster and 68528 data clusters (>= 65525, so every FAT implementation
# classifies it as FAT32 — ADR appendix A "FAT32" column).
#
# Host requirements: dosfstools (mkfs.vfat) and mtools (mcopy).
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
OUTPUT_DIR=${1:-"$SCRIPT_DIR"}
mkdir -p "$OUTPUT_DIR"

IMAGE="$OUTPUT_DIR/fat32-fixture.img"
COMPRESSED="$IMAGE.gz"
BIN_FIXTURE="$OUTPUT_DIR/.BIN.DAT"

rm -f "$IMAGE" "$COMPRESSED" "$BIN_FIXTURE"

mkfs.vfat -C -F32 -S512 -s1 -n NAOSFAT32 "$IMAGE" 34816 >/dev/null
printf '\000\001\002\003\336\255\276\357' >"$BIN_FIXTURE"

mcopy -i "$IMAGE" "$SCRIPT_DIR/stage/README.TXT" ::/README.TXT
mcopy -i "$IMAGE" "$BIN_FIXTURE" ::/BIN.DAT
mcopy -i "$IMAGE" -s "$SCRIPT_DIR/stage/docs" ::/docs

gzip -9n -k "$IMAGE"
rm -f "$BIN_FIXTURE"
ls -l "$IMAGE" "$COMPRESSED"
