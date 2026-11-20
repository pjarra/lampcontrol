// Lamp bridge: home Wi-Fi <-> AwoX/EGLO Bluetooth mesh, with pages for phones, the Mac and an old Kindle.
//
// Setup happens over USB from the Mac (bridge/setup.py): Wi-Fi and mesh credentials are typed there,
// sent over the serial line and kept only in this board's flash (NVS), never in the repo.
#include "api.h"
#include "awox.h"
#include "mesh.h"
#include "model.h"
#include <Arduino.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <sys/time.h>
#include <time.h>

extern const char INDEX_HTML[] asm("_binary_web_index_html_start");
extern const char KINDLE_HTML[] asm("_binary_web_kindle_html_start");
extern const char SETUP_HTML[] asm("_binary_web_setup_html_start");

namespace {
const char* DEFAULT_TZ = "CET-1CEST,M3.5.0/2,M10.5.0/3";
const char* DEFAULT_LAMPS_URL = "https://pjarra.github.io/lampcontrol/lamps.json";
const char* HOSTNAME = "lampbridge";

Preferences prefs;
WebServer server(80);
Model model;
SemaphoreHandle_t modelLock;
std::string lampsSource = "none", lastError;
volatile bool lampsReloadRequested = true;
bool wifiConfigured = false;

struct Guard { Guard() { xSemaphoreTake(modelLock, portMAX_DELAY); } ~Guard() { xSemaphoreGive(modelLock); } };
// isKey first: the core would log an error line for every missing key.
std::string pref(const char* key, const char* fallback = "") { return prefs.isKey(key) ? prefs.getString(key, fallback).c_str() : fallback; }

std::string readFile(const char* path) {
  File f = LittleFS.open(path, "r");
  if (!f) return "";
  std::string s;
  while (f.available()) s += (char)f.read();
  return s;
}
bool writeFile(const char* path, const std::string& text) {
  File f = LittleFS.open(path, "w");
  if (!f) return false;
  bool ok = f.write((const uint8_t*)text.data(), text.size()) == text.size();
  f.close();
  return ok;
}

// lamps.json: from GitHub Pages (public file; TLS without certificate check, like a plain download),
// cached in flash so the bridge also works without internet.
void reloadLamps() {
  lampsReloadRequested = false;
  std::string url = pref("lamps_url", DEFAULT_LAMPS_URL), body;
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClientSecure tls;
    tls.setInsecure();
    HTTPClient http;
    if (http.begin(tls, (url + "?t=" + std::to_string(millis())).c_str())) {
      if (http.GET() == 200) body = http.getString().c_str();
      http.end();
    }
  }
  std::string warnings, error;
  if (!body.empty()) {
    { Guard g; error = model.loadLamps(body, &warnings); }
    if (error.empty()) { writeFile("/lamps.json", body); lampsSource = "GitHub Pages"; }
  }
  if (body.empty() || !error.empty()) {
    std::string cached = readFile("/lamps.json");
    Guard g;
    if (!cached.empty() && model.loadLamps(cached, &warnings).empty()) lampsSource = "cached copy (download failed)";
  }
  Serial.printf("[lamps] %u lamp(s) from %s%s\n", (unsigned)model.lamps.size(), lampsSource.c_str(),
                warnings.empty() ? "" : (" - " + warnings).c_str());
}

struct Board : api::Platform {
  uint32_t nowMs() override { return millis(); }
  bool timeKnown() override { return mesh::timeKnown(); }
  void setTime(uint64_t ms) override {
    timeval tv = {(time_t)(ms / 1000), (suseconds_t)((ms % 1000) * 1000)};
    settimeofday(&tv, nullptr);
    Serial.println("[time] set from a browser (no internet time yet)");
  }
  std::string connection() override { return mesh::connectedLamp(); }
  std::string message() override { return mesh::message(); }
  bool saveScenes(const std::string& json) override { return writeFile("/scenes.json", json); }
  void requestClocks() override { mesh::requestClocks(); }
  void requestLampsReload() override { lampsReloadRequested = true; }
  std::string info() override {
    char buf[64];
    time_t now = time(nullptr); struct tm lt; localtime_r(&now, &lt);
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &lt);
    std::string s = "firmware: " __DATE__ " " __TIME__ "\n";
    s += "time: " + std::string(mesh::timeKnown() ? buf : "unknown (no internet time yet)") + "\n";
    s += "wifi: " + std::string(WiFi.SSID().c_str()) + ", " + WiFi.localIP().toString().c_str() + ", RSSI " + std::to_string(WiFi.RSSI()) + " dBm\n";
    s += "address: http://" + std::string(HOSTNAME) + ".local/ or http://" + WiFi.localIP().toString().c_str() + "/\n";
    s += "lamps: " + std::to_string(model.lamps.size()) + " from " + lampsSource + "\n";
    s += "scenes: " + std::to_string(model.scenes.size()) + "\n";
    s += mesh::info();
    s += "uptime: " + std::to_string(millis() / 60000) + " min, free memory " + std::to_string(ESP.getFreeHeap() / 1024) + " kB\n";
    return s;
  }
} board;

void sendText(int status, const std::string& body) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(status, "text/plain; charset=utf-8", body.c_str());
}
void sendPage(const char* html) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html; charset=utf-8", html);
}
void handleApi() {
  api::Request r;
  r.method = server.method() == HTTP_POST ? "POST" : "GET";
  r.path = server.uri().c_str();
  for (int i = 0; i < server.args(); i++) {
    if (server.argName(i) == "plain") r.body = server.arg(i).c_str();
    else r.params[server.argName(i).c_str()] = server.arg(i).c_str();
  }
  api::Response res;
  { Guard g; res = api::handle(model, board, r); }
  sendText(res.status, res.body);
}

// Serial setup console (bridge/setup.py). Values arrive hex-encoded; secrets are never echoed.
//   set <key> <hex>   keys: wifi_ssid wifi_pass mesh_name mesh_pass tz lamps_url lamps_json
//   info | restart | forget
std::string line;
std::string unhex(const std::string& h) {
  std::string out;
  for (size_t i = 0; i + 1 < h.size(); i += 2) out += (char)strtol(h.substr(i, 2).c_str(), nullptr, 16);
  return out;
}
void console() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c != '\n' && c != '\r') { if (line.size() < 8192) line += c; continue; }
    if (line.empty()) continue;
    std::string cmd = line; line.clear();
    if (cmd.rfind("set ", 0) == 0) {
      size_t sp = cmd.find(' ', 4);
      std::string k = cmd.substr(4, sp - 4), v = sp == std::string::npos ? "" : unhex(cmd.substr(sp + 1));
      const char* keys[] = {"wifi_ssid", "wifi_pass", "mesh_name", "mesh_pass", "tz", "lamps_url"};
      bool known = false;
      for (const char* x : keys) if (k == x) { prefs.putString(x, v.c_str()); known = true; }
      if (k == "lamps_json") { known = writeFile("/lamps.json", v); }
      Serial.printf(known ? "ok %s\n" : "error unknown key %s\n", k.c_str());
    } else if (cmd == "info") {
      Serial.printf("ok wifi=%s mesh=%s ip=%s\n", pref("wifi_ssid").empty() ? "missing" : "set",
                    pref("mesh_name").empty() || pref("mesh_pass").empty() ? "missing" : "set", WiFi.localIP().toString().c_str());
    } else if (cmd == "forget") {
      prefs.clear(); LittleFS.remove("/scenes.json");
      Serial.println("ok forgotten");
    } else if (cmd == "restart") {
      Serial.println("ok restarting"); delay(200); ESP.restart();
    } else Serial.println("error unknown command");
  }
}
}  // namespace

void setup() {
  Serial.setRxBufferSize(4096);  // setup.py sends lamps.json as one long line
  Serial.begin(115200);
  delay(300);
  Serial.println("\n[bridge] starting");
  modelLock = xSemaphoreCreateMutex();
  prefs.begin("bridge", false);
  if (!LittleFS.begin(true)) Serial.println("[bridge] flash file system unavailable");

  std::string self = awox::selfTest();
  Serial.printf("[bridge] protocol self-test %s\n", self.empty() ? "PASS" : ("FAILED: " + self).c_str());

  std::string cachedLamps = readFile("/lamps.json"), scenes = readFile("/scenes.json");
  if (!cachedLamps.empty() && model.loadLamps(cachedLamps).empty()) lampsSource = "cached copy";
  if (!scenes.empty()) model.loadScenes(scenes);

  std::string ssid = pref("wifi_ssid"), wifiPass = pref("wifi_pass");
  wifiConfigured = !ssid.empty();
  if (wifiConfigured) {
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(HOSTNAME);
    WiFi.setAutoReconnect(true);
    WiFi.begin(ssid.c_str(), wifiPass.c_str());
  } else Serial.println("[bridge] no Wi-Fi configured: run bridge/setup.py on the Mac");

  std::string tz = pref("tz", DEFAULT_TZ);
  configTzTime(tz.c_str(), "pool.ntp.org", "time.google.com");

  server.on("/", [] { sendPage(INDEX_HTML); });
  server.on("/k", [] { sendPage(KINDLE_HTML); });
  server.on("/setup", [] { sendPage(SETUP_HTML); });
  server.onNotFound([] {
    if (server.uri().startsWith("/api/")) handleApi(); else sendText(404, "not found");
  });

  std::string meshName = pref("mesh_name"), meshPass = pref("mesh_pass");
  uint8_t probe[16];
  if (self.empty() && awox::combine(meshName, meshPass, probe)) mesh::start(&model, modelLock, meshName, meshPass, tz);
  else Serial.println("[bridge] mesh credentials missing: run bridge/setup.py on the Mac");
  memset(probe, 0, sizeof probe);
}

void loop() {
  static bool online = false;
  console();
  if (!wifiConfigured) { delay(20); return; }
  if (WiFi.status() == WL_CONNECTED && !online) {
    online = true;
    Serial.printf("[bridge] Wi-Fi connected: http://%s/ (http://%s.local/)\n", WiFi.localIP().toString().c_str(), HOSTNAME);
    MDNS.begin(HOSTNAME);
    MDNS.addService("http", "tcp", 80);
    server.begin();
  } else if (WiFi.status() != WL_CONNECTED && online) {
    online = false;
    Serial.println("[bridge] Wi-Fi lost, reconnecting");
  }
  if (online) {
    server.handleClient();
    if (lampsReloadRequested) reloadLamps();
  }
  delay(2);
}
