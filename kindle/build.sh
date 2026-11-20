#!/bin/sh
# Build the Kindle remote (static ARM/musl binaries; the K4's kernel 2.6.31 is too old for current glibc).
#   FBINK=/path/to/FBInk-v1.25.0 ./build.sh     (FBInk built once: make static KINDLE=true CC="zig cc -target arm-linux-musleabi" ...)
set -e
: "${FBINK:?set FBINK to an FBInk source tree with Release/libfbink.a}"
zig cc -target arm-linux-musleabi -static -Os -s -std=gnu11 -Wall -Wextra -I"$FBINK" -o lampctl lampctl.c "$FBINK/Release/libfbink.a" -lm
zig cc -target arm-linux-musleabi -static -Os -s -o keys keys.c
echo "Copy lampctl and lamps.sh to /mnt/us/lamps/ on the Kindle."
