#include "api.h"
#include <stdlib.h>

namespace api {
namespace {
// Integer parameter within [lo, hi]; false if missing or invalid.
bool intParam(const Request& r, const char* name, long lo, long hi, long& out) {
  auto it = r.params.find(name);
  if (it == r.params.end() || it->second.empty()) return false;
  char* end = nullptr;
  long v = strtol(it->second.c_str(), &end, 10);
  if (*end != '\0' || v < lo || v > hi) return false;
  out = v;
  return true;
}
bool has(const Request& r, const char* name) { return r.params.count(name) > 0; }
Response ok(const std::string& body = "ok") { return {200, body}; }
Response bad(const std::string& body) { return {400, body}; }
std::string num(long v) { return std::to_string(v); }
}  // namespace

std::string state(Model& model, Platform& platform) {
  std::string conn = platform.connection();
  std::string out = "bridge\t" + (conn.empty() ? std::string("-") : conn) + "\t" + platform.message() + "\n";
  out += std::string("time\t") + (platform.timeKnown() ? "1\t" + platform.localTime() : "0\t") + "\n";
  for (auto& l : model.lamps) {
    // Without a lamp connection the last known state may be stale: report it as unknown.
    out += "lamp\t" + l.id + "\t" + l.name + "\t" + (l.on < 0 || conn.empty() ? "?" : l.on ? "1" : "0") + "\t" + num(l.brightness) +
           "\t" + std::string(1, l.mode) + "\t" + num(l.kelvin) + "\t" + num(l.hue) + "\t" + num(l.saturation) + "\t" +
           (l.reportedMs ? "1" : "0") + "\t" + (l.lightReported ? "1" : "0") + "\n";
  }
  for (size_t i = 0; i < model.scenes.size(); i++) out += "scene\t" + num((long)i) + "\t" + model.scenes[i].name + "\n";
  for (size_t i = 0; i < model.presets.size(); i++) {
    const Preset& p = model.presets[i];
    out += "preset\t" + num((long)i) + "\t" + p.name + "\t" + (p.colour ? "c" : "w") + "\t" + num(p.kelvin) + "\t" +
           num(p.hue) + "\t" + num(p.saturation) + "\n";
  }
  return out;
}

Response handle(Model& model, Platform& platform, const Request& r) {
  const std::string& p = r.path;
  bool post = r.method == "POST";
  if (p == "/api/state") return ok(state(model, platform));
  if (p == "/api/info")  // the caller holds the model lock, so the counters are read here
    return ok(platform.info() + "commands sent: " + num((long)model.commandsSent) + ", re-sent after no confirmation: " +
              num((long)model.commandsResent) + ", given up: " + num((long)model.gaveUp) + "\n");
  if (p == "/api/raw") return ok(platform.raw());
  if (!post) return {405, "use POST"};
  // Light commands are refused, not queued, while the bridge has no lamp connection: a queued
  // "on" must not fire hours later when the connection comes back.
  bool lightCommand = p == "/api/lamp" || p == "/api/all" || p == "/api/scene" || p == "/api/group" || p == "/api/scene/store";
  if (lightCommand && platform.connection().empty()) return {503, "Not sent: the bridge has no connection to the lamps."};

  if (p == "/api/lamp") {
    auto it = r.params.find("id");
    Lamp* lamp = it == r.params.end() ? nullptr : model.find(it->second);
    if (!lamp) return bad("unknown lamp");
    long v, h, s;
    if (has(r, "on")) { if (!intParam(r, "on", 0, 1, v)) return bad("on must be 0 or 1"); model.power(*lamp, v == 1); }
    else if (has(r, "b")) { if (!intParam(r, "b", 1, 100, v)) return bad("b must be 1-100"); model.brightness(*lamp, (int)v); }
    else if (has(r, "k")) { if (!intParam(r, "k", 2200, 6600, v)) return bad("k must be 2200-6600"); model.white(*lamp, (int)v); }
    else if (has(r, "h")) {
      if (!intParam(r, "h", 0, 360, h) || !intParam(r, "s", 0, 100, s)) return bad("h must be 0-360 and s 0-100");
      model.colour(*lamp, (int)h, (int)s);
    } else if (has(r, "preset")) {
      if (!intParam(r, "preset", 0, 99, v) || !model.applyPreset(*lamp, (size_t)v)) return bad("unknown preset");
    } else return bad("nothing to do");
    return ok(state(model, platform));
  }
  if (p == "/api/group") {  // all lamps that are on
    long v, h, s;
    int n;
    if (has(r, "on")) { if (!intParam(r, "on", 0, 0, v)) return bad("only on=0 (switch them off)"); n = model.groupPower(false); }
    else if (has(r, "b")) { if (!intParam(r, "b", 1, 100, v)) return bad("b must be 1-100"); n = model.groupBrightness((int)v); }
    else if (has(r, "k")) { if (!intParam(r, "k", 2200, 6600, v)) return bad("k must be 2200-6600"); n = model.groupWhite((int)v); }
    else if (has(r, "h")) {
      if (!intParam(r, "h", 0, 360, h) || !intParam(r, "s", 0, 100, s)) return bad("h must be 0-360 and s 0-100");
      n = model.groupColour((int)h, (int)s);
    } else if (has(r, "preset")) {
      if (!intParam(r, "preset", 0, 99, v) || v >= (long)model.presets.size()) return bad("unknown preset");
      n = model.groupPreset((size_t)v);
    } else return bad("nothing to do");
    if (n == 0) return bad("No lamp is on.");
    return ok(state(model, platform));
  }
  if (p == "/api/scene/store") {
    auto it = r.params.find("name");
    std::string why = model.storeScene(it == r.params.end() ? "" : it->second);
    if (!why.empty()) return bad(why);
    if (!platform.saveScenes(model.scenesJson())) return {500, "Stored, but could not be saved on the bridge."};
    return ok(state(model, platform));
  }
  if (p == "/api/scene/delete") {
    long i;
    if (!intParam(r, "i", 0, 999, i) || !model.deleteScene((size_t)i)) return bad("unknown scene");
    if (!platform.saveScenes(model.scenesJson())) return {500, "Deleted, but could not be saved on the bridge."};
    return ok(state(model, platform));
  }
  if (p == "/api/all") {
    long v;
    if (!intParam(r, "on", 0, 1, v)) return bad("on must be 0 or 1");
    model.all(v == 1);
    return ok(state(model, platform));
  }
  if (p == "/api/scene") {
    long i;
    if (!intParam(r, "i", 0, 999, i) || !model.applyScene((size_t)i)) return bad("unknown scene");
    return ok(state(model, platform));
  }
  if (p == "/api/time") {
    auto it = r.params.find("t");
    unsigned long long t = it == r.params.end() ? 0 : strtoull(it->second.c_str(), nullptr, 10);
    if (t < 1700000000000ULL || t > 4102444800000ULL) return bad("t must be ms since 1970");
    if (!platform.timeKnown()) platform.setTime(t);
    return ok(platform.timeKnown() ? "time known" : "time not set");
  }
  if (p == "/api/scenes") {
    std::string warnings;
    std::string error = model.loadScenes(r.body, &warnings);
    if (!error.empty()) return bad("Not imported: " + error);
    if (!platform.saveScenes(model.scenesJson())) return {500, "Imported, but could not be stored on the bridge."};
    return ok("Imported: " + num((long)model.scenes.size()) + " scene(s), " + num((long)model.presets.size()) + " preset(s)." +
              (warnings.empty() ? "" : " (" + warnings + ")"));
  }
  if (p == "/api/update") {
    auto k = r.params.find("key"), u = r.params.find("url");
    std::string why = platform.requestUpdate(k == r.params.end() ? "" : k->second, u == r.params.end() ? "" : u->second);
    return why.empty() ? ok("Updating: downloading the firmware, then restarting.") : Response{403, why};
  }
  if (p == "/api/clocks") { platform.requestClocks(); return ok("Setting the clocks of all lamps, one after another (about 15 s per lamp)."); }
  if (p == "/api/lamps/reload") { platform.requestLampsReload(); return ok("Reloading lamps.json."); }
  return {404, "not found"};
}
}  // namespace api
