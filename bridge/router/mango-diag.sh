#!/bin/bash
# Read-only check of the Mango's internet path (iPhone tethering). Run while joined to "Mango";
# asks for the router password; writes ~/mango-diag.txt. No passwords end up in the file.
OUT=~/mango-diag.txt
ssh -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no -o LogLevel=ERROR root@192.168.8.1 'sh -s' > "$OUT" 2>&1 <<'REMOTE'
echo "== interfaces"; ip -4 addr | grep -E "^[0-9]|inet"
echo "== routes"; ip route
echo "== tether status"; ifstatus tether 2>&1 | grep -E '"up"|"pending"|"available"|"device"|"address"|"nexthop"|"errors"|"code"'
echo "== iPhone USB"; ls /sys/class/net; for d in /sys/class/net/*; do readlink "$d/device/driver" 2>/dev/null | grep -q ipheth && echo "ipheth: $(basename $d) carrier=$(cat $d/carrier 2>/dev/null)"; done
echo "== firewall zones"; uci show firewall | grep -E "@zone\[[0-9]+\]\.(name|network)"
echo "== dns"; cat /tmp/resolv.conf.d/resolv.conf.auto 2>/dev/null
echo "== ping"; ping -c 2 -W 3 1.1.1.1 2>&1 | tail -2
echo "== name lookup"; nslookup openwrt.org 127.0.0.1 2>&1 | tail -3
echo "== log"; logread | grep -iE "usbmuxd|ipheth|tether|udhcpc|netifd|eth1" | tail -40
REMOTE
echo "Saved to $OUT ($(wc -l < "$OUT") lines). Switch back to the iPhone hotspot and tell Claude."
