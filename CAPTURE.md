# Capturing the AwoX app's Bluetooth traffic

Goal: see exactly how the AwoX app stores "turn off after …" for the wake-up light
(and anything else it does with schedules), so the web page can do the same.

## One-time setup

1. **iPhone: Bluetooth logging profile**
   - On the iPhone, open <https://developer.apple.com/bug-reporting/profiles-and-logs/>,
     sign in with your Apple ID (a free account is enough), choose **Bluetooth** for iOS
     and download the profile.
   - Settings → General → VPN & Device Management → install the downloaded profile.
   - If PacketLogger later shows no traffic, restart the iPhone once.
   - The profile expires after a few days; that is expected.
2. **Mac: PacketLogger**
   - <https://developer.apple.com/download/all/> → search "Additional Tools for Xcode",
     download the version matching your macOS, open the .dmg, copy
     `Hardware/PacketLogger.app` to Applications.
3. Connect the iPhone to the Mac with a cable, unlock it, and tap **Trust** if asked.

## Capture

Close Lamp Control in Chrome and Bluefy first; a lamp accepts only one connection.

1. PacketLogger → **File → New iOS Trace**. Packets should start scrolling.
2. On the iPhone, **force-quit the AwoX app, then open it** and let it connect.
   This step matters: the login exchange must be in the capture, or the rest
   cannot be decrypted.
3. In the app, on the **Standleuchte**, create a wake-up schedule with
   **turn off after = 10 min**. Note the exact settings (time, fade, weekdays).
4. Change only **turn off after** to **20 min** and save again.
   (Two values that differ in one setting make the field easy to find.)
5. Optional, same idea: an off/sleep schedule with a fade, or any other schedule
   option you would like to have.
6. Write down the clock time of each save (to the minute).
7. Stop the trace, **File → Save** as e.g. `awox-wakeup.pklg` into this folder
   (`~/Src/Lampcontrol`).

Then tell Claude the file name and your notes (settings + times). The mesh
credentials are needed to decrypt; enter them at runtime, they are not stored.

## Note

The capture contains all Bluetooth traffic of the iPhone during that time
(e.g. headphones). Keep it private; delete it once analysed.

## Restarting the local page

```bash
cd ~/Src/Lampcontrol && python3 -m http.server 8765 --bind 127.0.0.1
```
Then open <http://localhost:8765> in Chrome.
