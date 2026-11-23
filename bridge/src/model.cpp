#include "model.h"
#include "awox.h"
#include <ArduinoJson.h>
#include <math.h>
#include <stdlib.h>

namespace {
int clampInt(long v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : (int)v; }
int roundInt(double v) { return (int)floor(v + 0.5); }
std::string lower(std::string s) { for (auto& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a'); return s; }
// Names end up in a tab-separated API: no control characters.
std::string clean(const std::string& s, size_t max) {
  std::string out;
  for (char c : s) { if ((unsigned char)c >= 0x20) out += c; else out += ' '; }
  return out.size() > max ? out.substr(0, max) : out;
}
void warn(std::string* w, const std::string& text) { if (w) { if (!w->empty()) *w += "; "; *w += text; } }
}  // namespace

int percentToWire(int percent) { return 2 + clampInt(roundInt(clampInt(percent, 0, 100) * 2.52), 0, 252); }
int kelvinToMired(int kelvin) { return clampInt(roundInt(1e6 / (kelvin > 0 ? kelvin : 2700)), awox::MIRED_MIN, awox::MIRED_MAX); }
int hueToByte(int degrees) { return clampInt(roundInt((((degrees % 360) + 360) % 360) * 254.0 / 360), 0, 254); }
int saturationToByte(int percent) { return clampInt(roundInt(clampInt(percent, 0, 100) * 2.54), 0, 254); }

std::string Model::loadLamps(const std::string& json, std::string* warnings) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) return "lamps.json is not valid JSON";
  JsonArrayConst list = doc["lamps"].as<JsonArrayConst>();
  if (list.isNull()) return "lamps.json has no \"lamps\" list";
  std::vector<Lamp> next;
  for (JsonObjectConst l : list) {
    std::string id = lower(l["id"] | ""), name = clean(l["name"] | "", 40), addr = l["meshAddress"] | "";
    long address = strtol(addr.c_str(), nullptr, 16);
    if (id.size() != 12 || name.empty()) { warn(warnings, "skipped an entry without id or name"); continue; }
    if (address <= 0 || address >= 0xFFFF || !awox::addressOK((uint16_t)address)) {
      warn(warnings, name + ": no usable mesh address (" + addr + ")"); continue;
    }
    Lamp lamp;
    if (Lamp* old = find(id)) lamp = *old;  // keep the known state
    lamp.id = id; lamp.name = name; lamp.address = (uint16_t)address;
    next.push_back(lamp);
  }
  if (next.empty()) return "lamps.json lists no usable lamp";
  lamps = next;
  return "";
}

std::vector<Preset> defaultPresets() {
  auto white = [](const char* n, int k) { Preset p; p.name = n; p.kelvin = k; return p; };
  auto colour = [](const char* n, int h, int s) { Preset p; p.name = n; p.colour = true; p.hue = h; p.saturation = s; return p; };
  return {white("Evening", 2200), white("Warm", 2700), white("Neutral", 4000), white("Daylight", 5500),
          colour("Blue night", 230, 70), colour("Red", 5, 90)};
}

std::string Model::loadScenes(const std::string& json, std::string* warnings) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) return "The file is not valid JSON (check commas and quotes).";
  JsonArrayConst list = doc["scenes"].as<JsonArrayConst>(), presetList = doc["presets"].as<JsonArrayConst>();
  if (list.isNull() && presetList.isNull()) return "The file has neither a \"scenes\" nor a \"presets\" list.";
  std::vector<Preset> nextPresets;
  std::string error;
  for (JsonObjectConst p : presetList) {
    Preset x;
    x.name = clean(p["name"] | "", 20);
    std::string where = "Preset \"" + x.name + "\"";
    if (x.name.empty()) error = "A preset needs a name";
    if (!p["colour"].isNull()) {
      long h = p["colour"]["hue"] | -1L, sat = p["colour"]["saturation"] | 100L;
      if (h < 0 || h > 360 || sat < 0 || sat > 100) error = where + ": colour needs hue 0-360 and saturation 0-100";
      x.colour = true; x.hue = (int)h; x.saturation = (int)sat;
    } else {
      long k = p["white"] | 0L;
      if (k < 2200 || k > 6600) error = where + ": needs \"white\" (2200-6600 K) or \"colour\"";
      x.kelvin = (int)k;
    }
    nextPresets.push_back(x);
  }
  if (!presetList.isNull() && nextPresets.size() > 20) error = "At most 20 presets";
  std::vector<Scene> next;
  for (JsonObjectConst s : list) {
    Scene scene;
    scene.name = clean(s["name"] | "", 30);
    std::string where = "Scene \"" + scene.name + "\"";
    for (JsonObjectConst sl : s["lamps"].as<JsonArrayConst>()) {
      std::string id = lower(sl["id"] | ""), name = sl["lamp"] | "";
      Lamp* lamp = find(id);
      if (!lamp) for (auto& l : lamps) if (lower(l.name) == lower(name)) lamp = &l;
      if (!lamp) { warn(warnings, where + ": lamp \"" + (name.empty() ? id : name) + "\" skipped"); continue; }
      SceneLamp x;
      x.id = lamp->id;
      x.on = sl["on"] | true;
      if (x.on) {
        long b = sl["brightness"] | 50L;
        if (b < 0 || b > 100) error = where + ", " + lamp->name + ": brightness must be 0-100";
        x.brightness = clampInt(b, 1, 100);
        if (!sl["colour"].isNull()) {
          long h = sl["colour"]["hue"] | 0L, sat = sl["colour"]["saturation"] | 100L;
          if (h < 0 || h > 360 || sat < 0 || sat > 100) error = where + ", " + lamp->name + ": colour out of range";
          x.colour = true; x.hue = (int)h; x.saturation = (int)sat;
        } else {
          long k = sl["white"] | 2700L;
          if (k < 2200 || k > 6600) error = where + ", " + lamp->name + ": white must be 2200-6600 K";
          x.kelvin = (int)k;
        }
      }
      scene.lamps.push_back(x);
    }
    if (scene.name.empty() || scene.lamps.empty()) error = where + ": needs a name and at least one lamp";
    next.push_back(scene);
  }
  if (!error.empty()) return error;
  if (!list.isNull()) scenes = next;
  if (!presetList.isNull()) presets = nextPresets.empty() ? defaultPresets() : nextPresets;
  return "";
}

std::string Model::scenesJson() const {
  JsonDocument doc;
  JsonArray list = doc["scenes"].to<JsonArray>();
  for (auto& s : scenes) {
    JsonObject o = list.add<JsonObject>();
    o["name"] = s.name;
    JsonArray ls = o["lamps"].to<JsonArray>();
    for (auto& l : s.lamps) {
      JsonObject x = ls.add<JsonObject>();
      x["id"] = l.id; x["on"] = l.on;
      if (!l.on) continue;
      x["brightness"] = l.brightness;
      if (l.colour) { x["colour"]["hue"] = l.hue; x["colour"]["saturation"] = l.saturation; }
      else x["white"] = l.kelvin;
    }
  }
  JsonArray pl = doc["presets"].to<JsonArray>();
  for (auto& p : presets) {
    JsonObject o = pl.add<JsonObject>();
    o["name"] = p.name;
    if (p.colour) { o["colour"]["hue"] = p.hue; o["colour"]["saturation"] = p.saturation; }
    else o["white"] = p.kelvin;
  }
  std::string out;
  serializeJson(doc, out);
  return out;
}

Lamp* Model::find(const std::string& id) {
  std::string want = lower(id);
  for (auto& l : lamps) if (l.id == want) return &l;
  return nullptr;
}
Lamp* Model::findAddress(uint16_t address) {
  for (auto& l : lamps) if (l.address == address) return &l;
  return nullptr;
}

// Latest value wins per lamp and kind; power drops pending slider moves; white and colour replace each other.
void Model::enqueue(const Command& c) {
  for (size_t i = 0; i < queue.size();) {
    const Command& q = queue[i];
    bool sameLamp = q.dest == c.dest || c.dest == awox::BROADCAST;
    bool drop = sameLamp && (q.kind == c.kind || c.kind == Kind::Power ||
                             (c.kind == Kind::Temperature && q.kind == Kind::Color) ||
                             (c.kind == Kind::Color && q.kind == Kind::Temperature));
    if (drop) queue.erase(queue.begin() + i); else i++;
  }
  queue.push_back(c);
}
// Each setter records the wish (Lamp::Want) and, unless send is false, queues the command.
static void wish(Lamp& l) { if (!l.want.active) { l.want = Lamp::Want(); l.want.active = true; } l.want.retries = 0; }
static Command powerCmd(const Lamp& l, bool on) { return {l.address, Kind::Power, on ? 1 : 0, 0}; }
static Command brightnessCmd(const Lamp& l, int pct) { return {l.address, Kind::Brightness, percentToWire(pct), 0}; }
static Command whiteCmd(const Lamp& l, int k) { return {l.address, Kind::Temperature, kelvinToMired(k), 0}; }
static Command colourCmd(const Lamp& l, int h, int s) { return {l.address, Kind::Color, hueToByte(h), saturationToByte(s)}; }

void Model::power(Lamp& lamp, bool on) {
  lamp.on = on; wish(lamp); lamp.want.on = on;
  enqueue(powerCmd(lamp, on));
}
void Model::brightness(Lamp& lamp, int percent) {
  lamp.brightness = clampInt(percent, 1, 100); wish(lamp); lamp.want.brightness = lamp.brightness;
  enqueue(brightnessCmd(lamp, lamp.brightness));
}
void Model::white(Lamp& lamp, int kelvin) {
  lamp.mode = 'w'; lamp.kelvin = clampInt(kelvin, 2200, 6600);
  wish(lamp); lamp.want.mode = 'w'; lamp.want.kelvin = lamp.kelvin;
  enqueue(whiteCmd(lamp, lamp.kelvin));
}
void Model::colour(Lamp& lamp, int hue, int saturation) {
  lamp.mode = 'c'; lamp.hue = clampInt(hue, 0, 360); lamp.saturation = clampInt(saturation, 0, 100);
  wish(lamp); lamp.want.mode = 'c'; lamp.want.hue = lamp.hue; lamp.want.saturation = lamp.saturation;
  enqueue(colourCmd(lamp, lamp.hue, lamp.saturation));
}
void Model::all(bool on) {
  for (auto& l : lamps) { l.on = on; wish(l); l.want.on = on; }
  enqueue({awox::BROADCAST, Kind::Power, on ? 1 : 0, 0});
}

static bool near(int a, int b, int tolerance) { return a - b <= tolerance && b - a <= tolerance; }
static bool hueNear(int a, int b) { int d = ((a - b) % 360 + 360) % 360; return d <= 4 || d >= 356; }

// A scene skips what a lamp has already reported (fewer commands: a scene is quicker and less gets
// lost); the check after sending covers the rest.
bool Model::applyScene(size_t index) {
  if (index >= scenes.size()) return false;
  for (auto& sl : scenes[index].lamps) {
    Lamp* lamp = find(sl.id);
    if (!lamp) continue;
    Lamp& l = *lamp;
    bool known = l.reportedMs && !l.want.active;  // the shown state is the lamp's own report
    bool lightKnown = known && l.lightReported && l.on == 1;
    if (!known || l.on != (sl.on ? 1 : 0)) power(l, sl.on);
    else { wish(l); l.want.on = sl.on; }
    if (!sl.on) continue;
    if (!lightKnown || !near(l.brightness, sl.brightness, 1)) brightness(l, sl.brightness);
    else { wish(l); l.want.brightness = sl.brightness; }
    if (sl.colour) {
      if (!lightKnown || l.mode != 'c' || !hueNear(l.hue, sl.hue) || !near(l.saturation, sl.saturation, 2)) colour(l, sl.hue, sl.saturation);
      else { l.want.mode = 'c'; l.want.hue = sl.hue; l.want.saturation = sl.saturation; }
    } else {
      if (!lightKnown || l.mode != 'w' || !near(l.kelvin, sl.kelvin, 60)) white(l, sl.kelvin);
      else { l.want.mode = 'w'; l.want.kelvin = sl.kelvin; }
    }
  }
  return true;
}
bool Model::applyPreset(Lamp& lamp, size_t index) {
  if (index >= presets.size()) return false;
  const Preset& p = presets[index];
  if (p.colour) colour(lamp, p.hue, p.saturation); else white(lamp, p.kelvin);
  return true;
}
int Model::groupPower(bool on) { int n = 0; for (auto& l : lamps) if (l.on == 1) { power(l, on); n++; } return n; }
int Model::groupBrightness(int percent) { int n = 0; for (auto& l : lamps) if (l.on == 1) { brightness(l, percent); n++; } return n; }
int Model::groupWhite(int kelvin) { int n = 0; for (auto& l : lamps) if (l.on == 1) { white(l, kelvin); n++; } return n; }
int Model::groupColour(int hue, int saturation) { int n = 0; for (auto& l : lamps) if (l.on == 1) { colour(l, hue, saturation); n++; } return n; }
int Model::groupPreset(size_t index) {
  if (index >= presets.size()) return 0;
  int n = 0;
  for (auto& l : lamps) if (l.on == 1) { applyPreset(l, index); n++; }
  return n;
}

std::string Model::storeScene(const std::string& rawName) {
  std::string name = clean(rawName, 30);
  while (!name.empty() && name.back() == ' ') name.pop_back();
  if (name.empty()) return "The scene needs a name.";
  Scene scene;
  scene.name = name;
  for (auto& l : lamps) {
    if (l.on < 0) continue;  // unknown: leave this lamp out of the scene
    SceneLamp x;
    x.id = l.id; x.on = l.on == 1; x.brightness = l.brightness;
    x.colour = l.mode == 'c'; x.kelvin = l.kelvin; x.hue = l.hue; x.saturation = l.saturation;
    scene.lamps.push_back(x);
  }
  if (scene.lamps.empty()) return "No lamp state is known yet.";
  for (auto& s : scenes) if (lower(s.name) == lower(name)) { s = scene; return ""; }
  if (scenes.size() >= 20) return "At most 20 scenes: delete one first.";
  scenes.push_back(scene);
  return "";
}
bool Model::deleteScene(size_t index) {
  if (index >= scenes.size()) return false;
  scenes.erase(scenes.begin() + (long)index);
  return true;
}

void Model::statusReport(uint16_t address, bool on, uint32_t nowMs) {
  Lamp* l = findAddress(address);
  if (!l) return;
  l->reportedMs = nowMs ? nowMs : 1;
  if (judge(*l, on, false, false, 0, 0, 0, 0, nowMs)) l->on = on;
}
void Model::statusReport(uint16_t address, bool on, bool colour, int brightness, int mired, int hue, int saturation, uint32_t nowMs) {
  Lamp* l = findAddress(address);
  if (!l) return;
  l->reportedMs = nowMs ? nowMs : 1;
  int pct = clampInt(roundInt((brightness - 2) / 2.52), 1, 100);
  int k = clampInt(roundInt(1e6 / (mired > 0 ? mired : 370) / 50) * 50, 2200, 6600);
  int h = clampInt(roundInt(hue * 360.0 / 254), 0, 360), s = clampInt(roundInt(saturation / 2.54), 0, 100);
  if (!judge(*l, on, true, colour, pct, k, h, s, nowMs)) return;
  l->on = on;
  l->brightness = pct;
  if (colour) { l->mode = 'c'; l->hue = h; l->saturation = s; }
  else { l->mode = 'w'; l->kelvin = k; }
  l->lightReported = true;
}

const uint32_t SETTLE_MS = 1000;    // a report this soon after a command may predate it
const uint32_t GIVE_UP_MS = 30000;  // no confirmation this long after the last send: stop waiting
const int MAX_RETRIES = 3;

bool Model::pendingFor(uint16_t address) const {
  for (auto& c : queue) if (c.dest == address || c.dest == awox::BROADCAST) return true;
  return false;
}
bool Model::judge(Lamp& l, bool on, bool hasLight, bool colour, int pct, int k, int h, int s, uint32_t nowMs) {
  Lamp::Want& w = l.want;
  if (!w.active) return true;
  if (pendingFor(l.address)) return false;              // more to come; judge afterwards
  if (!w.sentMs) { w = Lamp::Want(); return true; }      // nothing was sent (all skipped): report is the truth
  if (nowMs - w.sentMs < SETTLE_MS) return false;
  if (nowMs - w.sentMs > GIVE_UP_MS) { w = Lamp::Want(); return true; }
  std::vector<Command> missing;
  bool unsure = false;  // a part that only a full report can confirm
  if (w.on >= 0 && (on ? 1 : 0) != w.on) missing.push_back(powerCmd(l, w.on == 1));
  else if (on) {
    if (!hasLight) unsure = w.brightness >= 0 || w.mode;
    else {
      if (w.brightness >= 0 && !near(pct, w.brightness, 2)) missing.push_back(brightnessCmd(l, w.brightness));
      if (w.mode == 'w' && (colour || !near(k, w.kelvin, 100))) missing.push_back(whiteCmd(l, w.kelvin));
      if (w.mode == 'c' && (!colour || !hueNear(h, w.hue) || !near(s, w.saturation, 3))) missing.push_back(colourCmd(l, w.hue, w.saturation));
    }
  }
  if (missing.empty()) {
    if (unsure) return false;  // wait for a full report (or give up after GIVE_UP_MS)
    w = Lamp::Want();
    return true;
  }
  if (w.retries >= MAX_RETRIES) { w = Lamp::Want(); gaveUp++; return true; }  // show what the lamp really does
  w.retries++;
  commandsResent += (uint32_t)missing.size();
  for (auto& c : missing) enqueue(c);
  return false;
}
void Model::sent(const Command& c, uint32_t nowMs) {
  commandsSent++;
  for (auto& l : lamps)
    if (l.want.active && (c.dest == l.address || c.dest == awox::BROADCAST)) l.want.sentMs = nowMs ? nowMs : 1;
}
bool Model::verifying() const {
  for (auto& l : lamps) if (l.want.active && l.want.sentMs) return true;
  return false;
}

bool Model::popCommand(Command& out) {
  if (queue.empty()) return false;
  out = queue.front();
  queue.erase(queue.begin());
  return true;
}
