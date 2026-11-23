#!/bin/sh
# Mango watchdog, run by cron every minute (installed by mango-watch-install.sh).
# The router's log lives in RAM and is gone after a restart, so when the Wi-Fi access point or the
# iPhone internet disappears, the log is saved to flash first (/root/crashlogs, the last 6), then:
#   access point gone 2 min: reload Wi-Fi; still gone after 6 min: restart the router
#   internet gone 3 min: reconnect the iPhone tether once (never a restart: the iPhone leaves the house)
STATE=/tmp/mango-watch
LOGS=/root/crashlogs
mkdir -p "$STATE" "$LOGS"
# Booting takes ~2 minutes until Wi-Fi is up: leave it alone for the first 5.
[ "$(cut -d. -f1 /proc/uptime)" -lt 300 ] && exit 0

save() {
  f="$LOGS/$(date +%Y%m%d-%H%M)-$1.txt"
  { date; uptime; free; echo; iwinfo; ip addr; ip route; echo; logread; echo; dmesg; } > "$f" 2>&1
  ls -t "$LOGS"/* 2>/dev/null | tail -n +7 | xargs -r rm -f
}
count() { n=$(( $(cat "$STATE/$1" 2>/dev/null || echo 0) + 1 )); echo "$n" > "$STATE/$1"; echo "$n"; }

if [ "$(cat /sys/class/net/phy0-ap0/operstate 2>/dev/null)" = up ]; then
  rm -f "$STATE/wifi"
else
  n=$(count wifi)
  [ "$n" = 2 ] && { save wifi; logger -t mango-watch "access point gone: reloading Wi-Fi"; wifi; }
  [ "$n" = 6 ] && { save wifi-reboot; logger -t mango-watch "access point still gone: restarting"; sync; reboot; }
fi

if ip route | grep -q '^default .*dev eth1' && ping -c 2 -W 3 1.1.1.1 >/dev/null 2>&1; then
  rm -f "$STATE/net"
else
  n=$(count net)
  # Only while the iPhone is plugged in (eth1 exists): otherwise it is simply away.
  [ "$n" = 3 ] && [ -e /sys/class/net/eth1 ] && { save internet; logger -t mango-watch "no internet via the iPhone: reconnecting"; ifup tether; }
fi
