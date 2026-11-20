#!/bin/bash
# Second step after flashing plain OpenWrt on the GL-MT300N-V2 ("Mango"), from the Mac while joined
# to the temporary Wi-Fi "Mango-setup". Asks for the real Wi-Fi password and a new router password
# (both hidden), sends them over SSH on stdin (never on a command line or into a file), and sets up:
#   - Wi-Fi "Mango" (WPA2, channel 1, 20 MHz), address 192.168.8.1 as before
#   - iPhone USB tethering as the internet connection (usbmuxd + ipheth)
#   - a fixed address for the lamp bridge (192.168.8.146)
# Why: GL.iNet's closed MediaTek driver drops frames for Wi-Fi clients in power save, which ESP32
# boards with Bluetooth must use (2026-11-20). OpenWrt uses the open mt76 (mt7603) driver.
set -euo pipefail

# ROUTER=fdd3:cb18:b67b::1 when the Mac has no IPv4 address on the router's network (IPv6 works).
ROUTER=${ROUTER:-192.168.8.1}
BRIDGE_MAC=68:09:47:9f:6f:2c
# Fresh install: new host key, no root password yet. Keep it out of the normal known_hosts file.
SSH=(ssh -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no -o LogLevel=ERROR root@$ROUTER)

read -rsp "Wi-Fi password for \"Mango\" (hidden): " WIFI_KEY; echo
[ ${#WIFI_KEY} -ge 8 ] || { echo "WPA2 needs at least 8 characters."; exit 1; }
read -rsp "New router password for root (hidden): " ROOT_PW; echo
read -rsp "Router password again: " ROOT_PW2; echo
[ "$ROOT_PW" = "$ROOT_PW2" ] && [ -n "$ROOT_PW" ] || { echo "Passwords differ or are empty."; exit 1; }

echo "Configuring the router…"
# First copy the script, then run it with the two secrets on stdin (one input stream per call).
"${SSH[@]}" 'cat > /tmp/lampcontrol-setup.sh' <<'REMOTE'
set -e
IFS= read -r WIFI_KEY
IFS= read -r ROOT_PW
printf '%s\n%s\n' "$ROOT_PW" "$ROOT_PW" | passwd root >/dev/null
# OpenWrt 25.12 keeps the netmask in ipaddr: without /24 the LAN is a /32 and DHCP hands out nothing.
uci set network.lan.ipaddr='192.168.8.1/24'
uci set wireless.radio0.country='DE'
uci set wireless.radio0.channel='1'
uci set wireless.radio0.htmode='HT20'
uci set wireless.default_radio0.ssid='Mango'
uci set wireless.default_radio0.encryption='psk2'
uci set wireless.default_radio0.key="$WIFI_KEY"
# iPhone tethering: an extra internet interface in the wan firewall zone.
uci set network.tether='interface'
uci set network.tether.proto='dhcp'
uci set network.tether.device='eth1'
zone=$(uci show firewall | sed -n "s/^firewall\.\(@zone\[[0-9]*\]\)\.name='wan'$/\1/p")
uci add_list "firewall.$zone.network=tether"
# Fixed address for the lamp bridge, so bookmarks (Kindle) keep working.
host=$(uci add dhcp host)
uci set "dhcp.$host.name=lampbridge"
uci set "dhcp.$host.mac=$BRIDGE_MAC"
uci set "dhcp.$host.ip=192.168.8.146"
uci commit
/etc/init.d/usbmuxd enable
/etc/init.d/usbmuxd start
rm -f /tmp/lampcontrol-setup.sh
echo "done"
REMOTE
printf '%s\n%s\n' "$WIFI_KEY" "$ROOT_PW" | "${SSH[@]}" "BRIDGE_MAC=$BRIDGE_MAC sh /tmp/lampcontrol-setup.sh"
unset WIFI_KEY ROOT_PW ROOT_PW2

cat <<'TEXT'

Now the iPhone: plug it into the Mango's USB port, unlock it, tap "Trust" if asked, and switch on
Personal Hotspot. Waiting up to 90 s for the tethering interface…
TEXT
echo "(The router now asks for the password you just set.)"
ssh -o UserKnownHostsFile=/dev/null -o StrictHostKeyChecking=no -o LogLevel=ERROR root@$ROUTER 'sh -s' <<'REMOTE'
for i in $(seq 90); do
  for dev in /sys/class/net/*; do
    if readlink "$dev/device/driver" 2>/dev/null | grep -q ipheth; then
      name=$(basename "$dev")
      [ "$(uci get network.tether.device)" = "$name" ] || { uci set network.tether.device="$name"; uci commit network; }
      echo "iPhone found as $name. Switching to the final Wi-Fi \"Mango\" now: join it again on the Mac."
      ( sleep 2; /etc/init.d/network reload; wifi reload ) >/dev/null 2>&1 &
      exit 0
    fi
  done
  sleep 1
done
echo "No iPhone tethering interface yet. Wi-Fi switches to \"Mango\" anyway; tethering starts once the phone trusts the router."
( sleep 2; /etc/init.d/network reload; wifi reload ) >/dev/null 2>&1 &
REMOTE
