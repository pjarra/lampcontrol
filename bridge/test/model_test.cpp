// Host tests for the protocol port, the model and the API (no Bluetooth, no network).
#include "../src/api.h"
#include "../src/awox.h"
#include <cstdio>
#include <fstream>
#include <sstream>

namespace {
int failures = 0;
void check(bool ok, const std::string& what) {
  if (!ok) { failures++; std::printf("FAIL: %s\n", what.c_str()); }
}
std::string readFile(const std::string& path) { std::ifstream f(path); std::stringstream s; s << f.rdbuf(); return s.str(); }

struct Fake : api::Platform {
  bool known = false; std::string stored; int clocks = 0;
  uint32_t nowMs() override { return 1000; }
  bool timeKnown() override { return known; }
  void setTime(uint64_t) override { known = true; }
  std::string connection() override { return "Kamin"; }
  std::string message() override { return "connected"; }
  bool saveScenes(const std::string& j) override { stored = j; return true; }
  void requestClocks() override { clocks++; }
  void requestLampsReload() override {}
  std::string info() override { return "x: y\n"; }
};
api::Response post(Model& m, Fake& f, const std::string& path, std::map<std::string, std::string> params, std::string body = "") {
  return api::handle(m, f, {"POST", path, body, params});
}
std::vector<Command> drain(Model& m) { std::vector<Command> out; Command c; while (m.popCommand(c)) out.push_back(c); return out; }
}  // namespace

int main(int, char** argv) {
  std::string dir = argv[0];
  dir = dir.substr(0, dir.rfind('/') + 1);
  check(awox::selfTest().empty(), "protocol self-test: " + awox::selfTest());

  // Conversions match the web page.
  check(percentToWire(1) == 5 && percentToWire(50) == 128 && percentToWire(100) == 254, "percent to wire");
  check(kelvinToMired(2700) == 370 && kelvinToMired(6600) == 153 && kelvinToMired(2200) == 454, "kelvin to mired");
  check(hueToByte(360) == 0 && hueToByte(180) == 127 && saturationToByte(100) == 254, "colour bytes");

  Model m;
  Fake f;
  std::string w;
  check(m.loadLamps(readFile(dir + "../../lamps.json"), &w).empty(), "repo lamps.json loads");
  check(m.lamps.size() == 3 && m.find("96203f0f6338") && m.find("96203f0f6338")->address == 0x7780, "Kamin from lamps.json");
  check(!m.loadLamps("{\"lamps\":[{\"name\":\"X\",\"id\":\"9620aaaaaaaa\",\"meshAddress\":\"0x0000\"}]}", &w).empty(), "no lamp with address 0");
  check(m.lamps.size() == 3, "failed load keeps the old lamps");

  // Coalescing: newest brightness wins; power drops pending slider moves; colour replaces white.
  Lamp& kamin = *m.find("96203f0f6338");
  m.brightness(kamin, 10); m.brightness(kamin, 80); m.white(kamin, 3000); m.colour(kamin, 120, 50);
  auto q = drain(m);
  check(q.size() == 2 && q[0].kind == Kind::Brightness && q[0].a == percentToWire(80) && q[1].kind == Kind::Color, "coalesce sliders");
  m.brightness(kamin, 40); m.power(kamin, false);
  q = drain(m);
  check(q.size() == 1 && q[0].kind == Kind::Power && q[0].a == 0 && q[0].dest == 0x7780, "power drops pending brightness");
  m.power(kamin, true); m.all(false);
  q = drain(m);
  check(q.size() == 1 && q[0].dest == awox::BROADCAST && q[0].a == 0, "all off replaces everything pending");

  // API: lamp commands and validation.
  check(post(m, f, "/api/lamp", {{"id", "96203f0f6338"}, {"b", "70"}}).status == 200 && kamin.brightness == 70, "api brightness");
  check(post(m, f, "/api/lamp", {{"id", "96203f0f6338"}, {"b", "0"}}).status == 400, "api rejects brightness 0");
  check(post(m, f, "/api/lamp", {{"id", "nope"}, {"on", "1"}}).status == 400, "api rejects unknown lamp");
  check(post(m, f, "/api/lamp", {{"id", "96203f0f6338"}, {"h", "200"}}).status == 400, "api needs h and s");
  check(api::handle(m, f, {"GET", "/api/lamp", "", {}}).status == 405, "api needs POST for changes");
  drain(m);

  // Scenes: the settings-file format, as exported by the page.
  std::string file = R"({"format":"lampcontrol/1","lamps":[],"scenes":[
    {"name":"Abend","lamps":[{"lamp":"Kamin","id":"96203f0f6338","on":true,"brightness":30,"white":2550},
                             {"lamp":"Standleuchte","id":"9620b155bc38","on":true,"brightness":20,"colour":{"hue":30,"saturation":80}},
                             {"lamp":"Unbekannt","id":"9620ffffffff","on":true,"brightness":20,"white":2700}]},
    {"name":"Aus","lamps":[{"lamp":"Schlafzimmerschrank","on":false}]}]})";
  api::Response r = post(m, f, "/api/scenes", {}, file);
  check(r.status == 200 && m.scenes.size() == 2 && r.body.find("Unbekannt") != std::string::npos, "scene import with warning: " + r.body);
  check(!f.stored.empty() && f.stored.find("\"Abend\"") != std::string::npos, "scenes stored compactly");
  Model reloaded; reloaded.loadLamps(readFile(dir + "../../lamps.json"));
  check(reloaded.loadScenes(f.stored).empty() && reloaded.scenes.size() == 2 && reloaded.scenes[0].lamps.size() == 2, "stored scenes load again");
  check(post(m, f, "/api/scenes", {}, R"({"scenes":[{"name":"X","lamps":[{"id":"96203f0f6338","brightness":300}]}]})").status == 400 &&
        m.scenes.size() == 2, "bad scene rejected, old scenes kept");
  check(post(m, f, "/api/scene", {{"i", "0"}}).status == 200, "apply scene");
  q = drain(m);
  check(q.size() == 6 && q[0].kind == Kind::Power && q[1].a == percentToWire(30) && q[2].kind == Kind::Temperature &&
        q[5].kind == Kind::Color && q[5].a == hueToByte(30), "scene sends power, brightness, light per lamp");
  check(post(m, f, "/api/scene", {{"i", "7"}}).status == 400, "unknown scene");

  // State text for the pages.
  m.statusReport(0x2A37, false, 5000);
  std::string s = api::state(m, f);
  check(s.find("bridge\tKamin\tconnected\n") == 0, "state header");
  check(s.find("lamp\t9620b155bc38\tStandleuchte\t0\t20\tc\t") != std::string::npos, "state lamp line: " + s);
  check(s.find("scene\t1\tAus\n") != std::string::npos, "state scene line");
  check(post(m, f, "/api/time", {{"t", "1793985122000"}}).status == 200 && f.known, "browser time accepted");
  check(post(m, f, "/api/clocks", {}).status == 200 && f.clocks == 1, "clock request");

  std::printf(failures ? "%d FAILED\n" : "all host tests PASS\n", failures);
  return failures ? 1 : 0;
}
