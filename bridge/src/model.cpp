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

std::string Model::loadScenes(const std::string& json, std::string* warnings) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) return "The file is not valid JSON (check commas and quotes).";
  JsonArrayConst list = doc["scenes"].as<JsonArrayConst>();
  if (list.isNull()) return "The file has no \"scenes\" list.";
  std::vector<Scene> next;
  std::string error;
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
  scenes = next;
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
void Model::power(Lamp& lamp, bool on) { lamp.on = on; enqueue({lamp.address, Kind::Power, on ? 1 : 0, 0}); }
void Model::brightness(Lamp& lamp, int percent) {
  lamp.brightness = clampInt(percent, 1, 100);
  enqueue({lamp.address, Kind::Brightness, percentToWire(lamp.brightness), 0});
}
void Model::white(Lamp& lamp, int kelvin) {
  lamp.mode = 'w'; lamp.kelvin = clampInt(kelvin, 2200, 6600);
  enqueue({lamp.address, Kind::Temperature, kelvinToMired(lamp.kelvin), 0});
}
void Model::colour(Lamp& lamp, int hue, int saturation) {
  lamp.mode = 'c'; lamp.hue = clampInt(hue, 0, 360); lamp.saturation = clampInt(saturation, 0, 100);
  enqueue({lamp.address, Kind::Color, hueToByte(lamp.hue), saturationToByte(lamp.saturation)});
}
void Model::all(bool on) {
  for (auto& l : lamps) l.on = on;
  enqueue({awox::BROADCAST, Kind::Power, on ? 1 : 0, 0});
}
bool Model::applyScene(size_t index) {
  if (index >= scenes.size()) return false;
  for (auto& sl : scenes[index].lamps) {
    Lamp* lamp = find(sl.id);
    if (!lamp) continue;
    power(*lamp, sl.on);
    if (!sl.on) continue;
    brightness(*lamp, sl.brightness);
    if (sl.colour) colour(*lamp, sl.hue, sl.saturation); else white(*lamp, sl.kelvin);
  }
  return true;
}
void Model::statusReport(uint16_t address, bool on, uint32_t nowMs) {
  if (Lamp* l = findAddress(address)) { l->on = on; l->reportedMs = nowMs ? nowMs : 1; }
}
bool Model::popCommand(Command& out) {
  if (queue.empty()) return false;
  out = queue.front();
  queue.erase(queue.begin());
  return true;
}
