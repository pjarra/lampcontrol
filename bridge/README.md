# Lamp bridge (ESP32)

An ESP32 (WROOM-32) between the home Wi-Fi and the lamps' Bluetooth mesh. Any browser at home controls
the lamps through it: no Bluefy, no Bluetooth picker. It also serves an e-ink page for an old Kindle.

- `/` lamps with brightness, white and colour, scenes, all on/off (phone, Mac)
- `/k` Kindle page: 600 × 706, black and white, big buttons, old JavaScript only
- `/setup` status, *Set all clocks now*, scene import (the page's settings file), reload lamps.json

The bridge keeps one Bluetooth connection to the nearest lamp and reaches the others through the mesh.
It asks the lamps for their on/off state every 10 s. Every night at 03:30, and on request, it connects
to each lamp in turn and sets its clock (time from the internet, or from a browser if there is none),
so schedules survive power cuts. Schedules themselves are still edited in the Lamp Control page.

## First start

```bash
cd bridge
pio run -t upload        # board on USB with a data cable (the Kindle's cable works)
python3 setup.py         # asks for Wi-Fi and mesh credentials; passwords hidden
```

`setup.py` sends the credentials over the USB cable; they are kept only in the board's flash, never on
the Mac or in the repo. Run it again to change them (empty answers keep the current values). It then
shows the bridge's address, e.g. `http://192.168.8.50/` or `http://lampbridge.local/`. The Kindle
cannot resolve `.local` names: use the number, and give the bridge a fixed address in the router.

Power it from any USB charger afterwards. Lamps come from `lamps.json` on GitHub Pages at every start
(cached on the board), so `tools/scan-lamps.swift` stays the way to add lamps.

## Updates over Wi-Fi

```bash
./ota.sh                 # builds, serves the firmware from this Mac, the bridge downloads and installs it
```

Protected by a password that `setup.py` creates in `~/.config/lampbridge/ota_password` (outside the repo)
and stores on the board. The bridge pulls the file itself: waiting for an ArduinoOTA invitation failed
while Bluetooth forces Wi-Fi power saving on. A failed update is shown on `/setup` after the restart.

## Without the board

```bash
make -C test             # protocol self-test (same vectors as protocol.js), model and API tests
make -C test serve       # pages with simulated lamps on http://127.0.0.1:8780 (LAN=1 for the Kindle)
```

## Notes

- API: plain text, tab-separated (the Kindle's browser has no JSON); see `src/api.h`.
- Anyone on the home Wi-Fi can use the pages; there is no login.
- Status reports use the same decoding as `tools/lampsync.swift` (verified on macOS, not yet on the ESP32).
- If the time-zone frame fails on a small Bluetooth MTU, the clock frame still sets the time (UTC offset only).
