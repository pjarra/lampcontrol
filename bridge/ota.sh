#!/bin/sh
# Update the bridge over Wi-Fi: build, serve the firmware from this Mac, and ask the bridge
# (password-protected) to download and install it. The password lives outside the repo.
set -e
cd "$(dirname "$0")"
PW_FILE="$HOME/.config/lampbridge/ota_password"
BRIDGE=${BRIDGE:-192.168.8.146}
PORT=8777
[ -f "$PW_FILE" ] || { echo "No $PW_FILE: run setup.py once over USB."; exit 1; }
pio run -e esp32dev >/dev/null
MAC_IP=$(route -n get "$BRIDGE" 2>/dev/null | awk '/interface:/ {print $2}' | xargs ipconfig getifaddr)
python3 -m http.server $PORT --bind "$MAC_IP" --directory .pio/build/esp32dev >/tmp/lampbridge-ota.log 2>&1 &
SERVER=$!
trap 'kill $SERVER 2>/dev/null' EXIT
sleep 1
INFO=$(curl -s -m 5 "http://$BRIDGE/api/info")
OLD=$(echo "$INFO" | sed -n 's/^firmware: //p')
OLD_UP=$(echo "$INFO" | sed -n 's/^uptime: \([0-9]*\) min.*/\1/p')
echo "Bridge firmware: ${OLD:-unknown}. Asking it to update from http://$MAC_IP:$PORT/firmware.bin"
curl -s -m 10 -d "key=$(cat "$PW_FILE")" --data-urlencode "url=http://$MAC_IP:$PORT/firmware.bin" "http://$BRIDGE/api/update"; echo
for i in $(seq 60); do
  sleep 3
  INFO=$(curl -s -m 3 "http://$BRIDGE/api/info") || continue
  NEW=$(echo "$INFO" | sed -n 's/^firmware: //p')
  UP=$(echo "$INFO" | sed -n 's/^uptime: \([0-9]*\) min.*/\1/p')
  [ -n "$NEW" ] || continue
  # A new build changes the version; an unchanged build still shows as a restart.
  if [ "$NEW" != "$OLD" ] || { [ -n "$UP" ] && [ -n "$OLD_UP" ] && [ "$UP" -lt "$OLD_UP" ]; } || [ "${UP:-1}" = 0 ]; then
    grep -q "GET /firmware.bin" /tmp/lampbridge-ota.log && { echo "Updated and restarted: firmware $NEW"; exit 0; }
  fi
done
echo "No new firmware after 3 minutes. Download log:"; cat /tmp/lampbridge-ota.log; exit 1
