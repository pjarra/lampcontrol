#!/bin/sh
# Start the lamp remote full screen: pause the Kindle's own interface (it would otherwise draw over the
# remote and react to the buttons), keep the screen awake, and bring the interface back on exit.
DIR=$(dirname "$0")
# Only one remote: a second one would react to every button press as well.
pidof lampctl >/dev/null && { echo "lampctl already running"; exit 0; }
BRIDGE=${1:-192.168.8.146}
cp "$DIR/lampctl" /tmp/lampctl && chmod +x /tmp/lampctl
lipc-set-prop -- com.lab126.powerd preventScreenSaver 1
/etc/init.d/framework stop >/dev/null 2>&1
sleep 2
/tmp/lampctl "$BRIDGE"
/etc/init.d/framework start >/dev/null 2>&1 &
