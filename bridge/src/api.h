// HTTP API, shared by the ESP32 web server and the host test server. Plain text, tab-separated,
// because the Kindle's browser (WebKit 528) has no JSON object.
//
//   GET  /api/state            bridge\t<connected lamp or ->\t<message>
//                              time\t<1 if the clock is known, else 0>\t<local HH:MM:SS, if known>
//                              lamp\t<id>\t<name>\t<on 1|0|?>\t<brightness %>\t<w|c>\t<kelvin>\t<hue>\t<sat %>\t<reported 1|0>\t<light reported 1|0>
//                              (light reported: brightness/white/colour come from the lamp's own status)
//                              scene\t<index>\t<name>
//   Light commands answer 503 while the bridge has no lamp connection (nothing is queued).
//   POST /api/lamp   id=<id> and one of: on=0|1, b=<1..100>, k=<kelvin>, h=<0..360>&s=<0..100>
//   POST /api/all    on=0|1
//   POST /api/scene  i=<index>
//   POST /api/time   t=<ms since 1970>   (from a browser, when the bridge has no internet time yet)
//   POST /api/scenes <settings file as the request body, text/plain>
//   POST /api/clocks             set every lamp's clock now
//   POST /api/lamps/reload       fetch lamps.json again
//   GET  /api/info               diagnostics, one "key: value" per line
//   GET  /api/raw                last decrypted status message per lamp (decoding experiments)
//   POST /api/update key=<update password>&url=http://<mac>/firmware.bin   (bridge/ota.sh)
#pragma once
#include "model.h"
#include <map>
#include <string>

namespace api {
struct Request { std::string method, path, body; std::map<std::string, std::string> params; };
struct Response { int status; std::string body; };

// What only the platform can do.
struct Platform {
  virtual ~Platform() {}
  virtual uint32_t nowMs() = 0;
  virtual bool timeKnown() = 0;
  virtual std::string localTime() = 0;    // "HH:MM:SS" (the Kindle's own clock is unreliable)
  virtual void setTime(uint64_t unixMs) = 0;
  virtual std::string connection() = 0;   // name of the lamp the bridge is connected to, or ""
  virtual std::string message() = 0;      // short status line
  virtual bool saveScenes(const std::string& compactJson) = 0;
  virtual void requestClocks() = 0;
  virtual void requestLampsReload() = 0;
  virtual std::string info() = 0;
  virtual std::string raw() = 0;          // decoding aid: last decrypted status messages
  // Firmware update: the bridge downloads url itself (outgoing connections work reliably, unlike
  // waiting for an invitation while Wi-Fi power saving is on). Returns "" if accepted, else why not.
  virtual std::string requestUpdate(const std::string& key, const std::string& url) = 0;
};

Response handle(Model& model, Platform& platform, const Request& request);
std::string state(Model& model, Platform& platform);
}  // namespace api
