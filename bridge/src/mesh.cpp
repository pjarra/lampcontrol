#include "mesh.h"
#include "awox.h"
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <algorithm>
#include <esp_random.h>
#include <freertos/queue.h>
#include <map>
#include <string.h>
#include <time.h>

namespace mesh {
namespace {
const NimBLEUUID SERVICE("00010203-0405-0607-0809-0a0b0c0d1910");
const NimBLEUUID STATUS("00010203-0405-0607-0809-0a0b0c0d1911");
const NimBLEUUID COMMAND("00010203-0405-0607-0809-0a0b0c0d1912");
const NimBLEUUID PAIR("00010203-0405-0607-0809-0a0b0c0d1914");
const uint32_t MIN_GAP_MS = 350;          // closer commands are swallowed by the lamp (upstream notes)
const uint32_t STATUS_EVERY_MS = 10000;   // status query, as the AwoX app polls (every 5 s there)
const int CLOCK_HOUR = 3, CLOCK_MINUTE = 30;

Model* model = nullptr;
SemaphoreHandle_t modelLock = nullptr, textLock = nullptr;
std::string meshName, meshPassword, tzRule;
QueueHandle_t notifications = nullptr;
struct Note { uint8_t length; uint8_t data[20]; };

NimBLEClient* client = nullptr;
NimBLERemoteCharacteristic *pairChar = nullptr, *commandChar = nullptr, *statusChar = nullptr;
uint8_t key[16];
bool ready = false;
volatile bool clocksRequested = false, paused = false, pausedAck = false;
// Latest decrypted notification per (type, address): for working out what else status messages carry.
struct Raw { uint32_t ms; uint8_t type; uint8_t length; uint8_t plain[16]; uint8_t tail[4]; };
std::map<uint32_t, Raw> rawNotes;
std::string connected, statusText = "Starting", clockReport;
uint32_t lastWrite = 0, lastQuery = 0, notesSeen = 0, retryAt = 0, retryDelay = 5000;
int lastClockDay = -1;

// Connection interval 45-75 ms (units of 1.25 ms), timeout 4 s. Wi-Fi shares the radio: at 15 ms
// the bridge answered almost no pings (2026-11-20). Failed connects (HCI 0x3E) were range, not timing.
const uint16_t ITVL_MIN = 36, ITVL_MAX = 60, TIMEOUT = 400;

struct Text { Text() { xSemaphoreTake(textLock, portMAX_DELAY); } ~Text() { xSemaphoreGive(textLock); } };
struct ModelGuard { ModelGuard() { xSemaphoreTake(modelLock, portMAX_DELAY); } ~ModelGuard() { xSemaphoreGive(modelLock); } };
void setStatus(const std::string& s) { Text t; statusText = s; Serial.printf("[mesh] %s\n", s.c_str()); }

// Runs in the NimBLE host task: copy and hand over; decryption happens in the mesh task.
void onNotify(NimBLERemoteCharacteristic*, uint8_t* data, size_t length, bool) {
  Note n;
  n.length = (uint8_t)(length > sizeof n.data ? sizeof n.data : length);
  memcpy(n.data, data, n.length);
  xQueueSend(notifications, &n, 0);
}

bool write(NimBLERemoteCharacteristic* ch, const awox::Bytes& packet) {
  uint32_t wait = millis() - lastWrite;
  if (wait < MIN_GAP_MS) delay(MIN_GAP_MS - wait);
  lastWrite = millis();
  return ch->writeValue(packet.data(), packet.size(), true);
}

void dropConnection() {
  ready = false;
  { ModelGuard g; model->clearQueue(); }  // never send old commands after a reconnect
  memset(key, 0, sizeof key);
  if (client && client->isConnected()) client->disconnect();
  { Text t; connected.clear(); }
}

// Connect to one lamp and log in (0x0C / 0x0D). One retry after 5 s on 0x0E: the chip alternates
// accept/reject (upstream notes; seen on our lamps too).
bool login(const NimBLEAddress& address) {
  uint8_t combined[16];
  if (!awox::combine(meshName, meshPassword, combined)) { setStatus("Mesh credentials invalid"); return false; }
  for (int round = 1; round <= 2; round++) {
    uint32_t t0 = millis();
    if (!client->connect(address)) {
      // 574 = HCI 0x3E: the lamp did not answer the connection request; seen below about -65 dBm.
      setStatus("Bluetooth connection failed (error " + std::to_string(client->getLastError()) + " after " +
                std::to_string(millis() - t0) + " ms; too far from the lamp?)");
      break;
    }
    NimBLERemoteService* service = client->getService(SERVICE);
    pairChar = service ? service->getCharacteristic(PAIR) : nullptr;
    commandChar = service ? service->getCharacteristic(COMMAND) : nullptr;
    statusChar = service ? service->getCharacteristic(STATUS) : nullptr;
    if (!pairChar || !commandChar || !statusChar) { setStatus("Mesh service not found"); break; }
    uint8_t random[8];
    esp_fill_random(random, sizeof random);
    awox::Bytes pair = awox::pairPacket(combined, random);
    if (!pairChar->writeValue(pair.data(), pair.size(), true)) { setStatus("Login write failed"); break; }
    int reply = 0;
    for (int attempt = 0; attempt < 15 && reply == 0 && client->isConnected(); attempt++) {
      delay(400);
      NimBLEAttValue v = pairChar->readValue();
      if (v.length() >= 9 && v.data()[0] == 0x0D) {
        awox::sessionKey(combined, random, v.data() + 1, key);
        reply = 0x0D;
      } else if (v.length() >= 1 && v.data()[0] == 0x0E) reply = 0x0E;
    }
    if (reply == 0x0D) { memset(combined, 0, sizeof combined); lastWrite = millis(); return true; }
    client->disconnect();
    if (reply != 0x0E) { setStatus("No login reply; the lamp may need a power cycle"); break; }
    if (round == 2) { setStatus("Login rejected twice: check the mesh credentials"); break; }
    setStatus("Login rejected (0x0E), retrying in 5 s");
    delay(5000);
  }
  memset(combined, 0, sizeof combined);
  if (client->isConnected()) client->disconnect();
  return false;
}

struct Seen { NimBLEAddress address; int rssi; std::string id, name; };
// Known lamps in range, strongest first.
std::vector<Seen> scan(uint32_t seconds) {
  std::vector<Seen> found;
  NimBLEScan* s = NimBLEDevice::getScan();
  s->setActiveScan(false);
  s->clearResults();
  NimBLEScanResults results = s->start(seconds, false);
  for (int i = 0; i < results.getCount(); i++) {
    NimBLEAdvertisedDevice d = results.getDevice(i);
    if (!d.haveManufacturerData()) continue;
    std::string m = d.getManufacturerData();
    if (m.size() < 8 || (uint8_t)m[0] != 0x60 || (uint8_t)m[1] != 0x01) continue;
    char id[13];
    for (int j = 0; j < 6; j++) snprintf(id + 2 * j, 3, "%02x", (uint8_t)m[2 + j]);
    ModelGuard g;
    if (Lamp* lamp = model->find(id)) found.push_back({d.getAddress(), d.getRSSI(), id, lamp->name});
  }
  s->clearResults();
  std::sort(found.begin(), found.end(), [](const Seen& a, const Seen& b) { return a.rssi > b.rssi; });
  return found;
}

int32_t utcOffset(time_t now) {
  struct tm lt, gt;
  localtime_r(&now, &lt); gmtime_r(&now, &gt);
  int32_t offset = (lt.tm_hour - gt.tm_hour) * 3600 + (lt.tm_min - gt.tm_min) * 60;
  int days = lt.tm_yday - gt.tm_yday;
  if (days == 1 || days < -1) offset += 86400; else if (days == -1 || days > 1) offset -= 86400;
  return offset;
}

// Time zone rule, then the clock, to the directly connected lamp (as the AwoX app and the page do).
bool sendClock() {
  time_t now = time(nullptr);
  bool tzOK = write(commandChar, awox::timezonePacket(key, (uint32_t)now, tzRule));
  bool clockOK = write(commandChar, awox::clockPacket(key, (uint32_t)now, utcOffset(now)));
  if (!tzOK) Serial.println("[mesh] time zone frame not accepted (MTU?)");
  return clockOK;
}

void connectMesh() {
  setStatus("Looking for lamps");
  std::vector<Seen> lamps = scan(4);
  if (lamps.empty()) setStatus("No known lamp in range");
  for (size_t i = 0; i < lamps.size() && i < 3; i++) {
    setStatus("Connecting to " + lamps[i].name + " (RSSI " + std::to_string(lamps[i].rssi) + " dBm, " + lamps[i].address.toString() + ")");
    if (!login(lamps[i].address)) continue;
    statusChar->subscribe(true, onNotify, false);  // no CCCD on this lamp: only registers the callback
    ready = true; retryDelay = 5000;
    { Text t; connected = lamps[i].name; }
    setStatus("Connected (MTU " + std::to_string(client->getMTU()) + ")");
    if (timeKnown()) sendClock();
    write(commandChar, awox::statusQueryPacket(key));
    lastQuery = millis();
    return;
  }
  retryAt = millis() + retryDelay;
  retryDelay = retryDelay * 2 > 60000 ? 60000 : retryDelay * 2;
}

void runClocks() {
  clocksRequested = false;
  dropConnection();
  setStatus("Setting the lamps' clocks");
  std::vector<Seen> lamps = scan(5);
  std::vector<std::string> names;
  { ModelGuard g; for (auto& l : model->lamps) names.push_back(l.name); }
  std::string report;
  struct tm lt; time_t now = time(nullptr); localtime_r(&now, &lt);
  char stamp[20]; strftime(stamp, sizeof stamp, "%d.%m. %H:%M", &lt);
  for (auto& name : names) {
    auto it = std::find_if(lamps.begin(), lamps.end(), [&](const Seen& s) { return s.name == name; });
    std::string result = "not in range";
    if (it != lamps.end()) {
      if (login(it->address)) { result = sendClock() ? std::string("clock set ") + stamp : "clock write failed"; client->disconnect(); }
      else result = "login failed";
    }
    report += name + ": " + result + "\n";
    Serial.printf("[mesh] %s: %s\n", name.c_str(), result.c_str());
    delay(500);
  }
  memset(key, 0, sizeof key);
  { Text t; clockReport = report; }
  retryAt = 0;  // reconnect the mesh right away
}

void task(void*) {
  for (;;) {
    // Nightly clock run; also right after the time first becomes known.
    if (timeKnown()) {
      time_t now = time(nullptr); struct tm lt; localtime_r(&now, &lt);
      if (lastClockDay < 0) lastClockDay = (lt.tm_hour * 60 + lt.tm_min < CLOCK_HOUR * 60 + CLOCK_MINUTE) ? lt.tm_yday - 1 : lt.tm_yday;
      if (lt.tm_yday != lastClockDay && lt.tm_hour * 60 + lt.tm_min >= CLOCK_HOUR * 60 + CLOCK_MINUTE) { lastClockDay = lt.tm_yday; clocksRequested = true; }
      if (clocksRequested) runClocks();
    }
    if (paused) { if (ready) dropConnection(); pausedAck = true; delay(50); continue; }
    if (ready && !client->isConnected()) { setStatus("Connection lost"); dropConnection(); retryAt = millis() + 2000; }
    if (!ready) {
      if ((int32_t)(millis() - retryAt) >= 0) connectMesh();
      delay(100);
      continue;
    }
    Note n;
    while (xQueueReceive(notifications, &n, 0) == pdTRUE) {
      notesSeen++;
      if (n.length >= 17) {
        Raw r = {millis(), n.data[0], n.length, {0}, {0}};
        awox::attDecrypt(key, n.data + 1, r.plain);
        for (int i = 17; i < n.length && i < 21; i++) r.tail[i - 17] = n.data[i];
        uint32_t k = (uint32_t)r.type << 16 | (uint32_t)(r.plain[0] | r.plain[1] << 8);
        Text t; rawNotes[k] = r;
      }
      awox::Status s = awox::parseStatus(key, n.data, n.length);
      if (s.valid) { ModelGuard g; model->statusReport(s.address, s.on, millis()); }
    }
    Command c;
    bool have;
    { ModelGuard g; have = millis() - lastWrite >= MIN_GAP_MS && model->popCommand(c); }
    if (have) {
      awox::Bytes p = c.kind == Kind::Power ? awox::powerPacket(key, c.a == 1, c.dest)
                    : c.kind == Kind::Brightness ? awox::brightnessPacket(key, c.a, c.dest)
                    : c.kind == Kind::Temperature ? awox::temperaturePacket(key, c.a, c.dest)
                    : awox::colorPacket(key, c.a, c.b, c.dest);
      // A failed write may still have reached the lamp: never retry it, reconnect instead.
      if (!write(commandChar, p)) { setStatus("Write failed, reconnecting"); dropConnection(); }
    } else if (millis() - lastQuery >= STATUS_EVERY_MS && millis() - lastWrite >= MIN_GAP_MS) {
      lastQuery = millis();
      if (!write(commandChar, awox::statusQueryPacket(key))) { setStatus("Status query failed, reconnecting"); dropConnection(); }
    }
    delay(10);
  }
}
}  // namespace

bool timeKnown() { return time(nullptr) > 1700000000; }
void requestClocks() { clocksRequested = true; }
void pauseForUpdate() {
  if (!textLock) return;  // never started
  paused = true;
  // Wait until the mesh task is idle: a lamp search or connection attempt (up to ~15 s) keeps the
  // radio busy with Bluetooth, and the download then fails to connect.
  for (int i = 0; i < 400 && !pausedAck; i++) delay(50);
  Text t;
  statusText = "Firmware update running";
}
std::string raw() {
  if (!textLock) return "";
  Text t;
  std::string out = "# ms-ago type addr  decrypted block (16 bytes)  clear tail\n";
  char line[128];
  for (auto& e : rawNotes) {
    const Raw& r = e.second;
    int n = snprintf(line, sizeof line, "%7lu %02x %04x ", (unsigned long)(millis() - r.ms), r.type, (unsigned)(r.plain[0] | r.plain[1] << 8));
    for (int i = 0; i < 16; i++) n += snprintf(line + n, sizeof line - n, "%02x%s", r.plain[i], i == 15 ? " " : "");
    for (int i = 17; i < r.length && i < 21; i++) n += snprintf(line + n, sizeof line - n, "%02x", r.tail[i - 17]);
    out += line; out += "\n";
  }
  return out;
}
// Callable before start() (e.g. without mesh credentials): textLock is null then.
std::string connectedLamp() { if (!textLock) return ""; Text t; return connected; }
std::string message() { if (!textLock) return "Bluetooth not started (mesh credentials missing?)"; Text t; return statusText; }
std::string info() {
  if (!textLock) return "bluetooth: not started\n";
  Text t;
  return "bluetooth: " + statusText + "\nconnected via: " + (connected.empty() ? "-" : connected) +
         "\nstatus messages received: " + std::to_string(notesSeen) + "\nlast clock run:" +
         (clockReport.empty() ? std::string(" none yet\n") : "\n" + clockReport);
}

void start(Model* m, SemaphoreHandle_t lock, const std::string& name, const std::string& password, const std::string& posixTZ) {
  model = m; modelLock = lock; meshName = name; meshPassword = password; tzRule = posixTZ;
  textLock = xSemaphoreCreateMutex();
  notifications = xQueueCreate(32, sizeof(Note));
  NimBLEDevice::init("");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);  // +9 dBm: the lamps did not hear the default +3 dBm (HCI 0x3E)
  client = NimBLEDevice::createClient();
  client->setConnectTimeout(10);
  client->setConnectionParams(ITVL_MIN, ITVL_MAX, 0, TIMEOUT);
  xTaskCreatePinnedToCore(task, "mesh", 8192, nullptr, 1, nullptr, 0);
}
}  // namespace mesh
