# Lamp Control

Static web page to control EGLO/AwoX Bluetooth mesh lamps (`EGLO_ZM_RGB_TW`) from
Chrome or, on iPhone, [Bluefy](https://apps.apple.com/app/bluefy-web-ble-browser/id1492822055).
No server, no account: the page talks to the lamps directly via Web Bluetooth.

- Power, brightness, white temperature, colour
- Schedules stored in the lamp (wake-up light, evening scene, off), which run with the phone off
- Mesh credentials are entered at runtime and never stored or sent anywhere

Protocol (non-Telink AwoX variant) adapted from
[awox-mesh](https://github.com/paul-schoettl/awox-mesh), MIT; see `LICENSE.txt`.

## Adding lamps

Browsers cannot read Bluetooth advertisements, so lamps are discovered on the Mac:

```bash
swift tools/scan-lamps.swift        # passive 60 s scan; asks for a name for each new lamp
git commit -am "Add lamp" && git push
```

This updates `lamps.json`, which the page loads on start, on every device.

## Settings file and reading the lamps

Wake-up lights, switching times and scenes can be exported and imported as one editable JSON file
(page → *Settings file*). On the Mac, read what the lamps really hold into the same format:

```bash
swift tools/lampsync.swift            # asks for the mesh credentials; writes lampcontrol-settings.json
```

Import that file in the page on any device, adjust if needed, then *Save all to lamp* per lamp.
Settings files stay out of git (`.gitignore`). `tools/readback.swift` is the raw diagnostic version.

## Development

Local: `python3 -m http.server 8765 --bind 127.0.0.1`, then open <http://localhost:8765> in Chrome.
`tools/` also holds PacketLogger capture helpers; see `CAPTURE.md`.
