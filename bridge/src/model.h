// Lamps, scenes and the pending-command queue. No Bluetooth or network here, so the same code
// runs on the ESP32 and in the host test server.
#pragma once
#include <stdint.h>
#include <string>
#include <vector>

struct Lamp {
  std::string id, name;      // id: 12 hex digits from lamps.json (advertisement bytes)
  uint16_t address = 0;      // mesh address
  int on = -1;               // -1 unknown, 0 off, 1 on (last command or status report)
  int brightness = 60;       // percent, last set
  char mode = 'w';           // 'w' white, 'c' colour
  int kelvin = 2700, hue = 30, saturation = 100;  // degrees / percent
  uint32_t reportedMs = 0;   // last status notification (0 = never)
  bool lightReported = false;  // brightness/white/colour came from the lamp itself (not just set by us)
};

struct SceneLamp { std::string id; bool on = true; int brightness = 50; bool colour = false; int kelvin = 2700, hue = 0, saturation = 100; };
struct Scene { std::string name; std::vector<SceneLamp> lamps; };
// Named light settings (white or colour) offered on the Kindle's details screen; editable in the settings file.
struct Preset { std::string name; bool colour = false; int kelvin = 2700, hue = 0, saturation = 100; };
std::vector<Preset> defaultPresets();

enum class Kind : uint8_t { Power, Brightness, Temperature, Color };
// Wire values: Power a=0/1; Brightness a=2..254; Temperature a=mired; Color a=hue byte, b=saturation byte.
struct Command { uint16_t dest; Kind kind; int a; int b; };

// Same conversions as the web page.
int percentToWire(int percent);            // 1..100 % -> 2..254
int kelvinToMired(int kelvin);             // clamped to 153..454
int hueToByte(int degrees);                // 0..360 -> 0..254
int saturationToByte(int percent);         // 0..100 -> 0..254

class Model {
 public:
  std::vector<Lamp> lamps;
  std::vector<Scene> scenes;
  std::vector<Preset> presets = defaultPresets();

  // lamps.json from the repo ({"lamps":[{name,id,meshAddress}]}). Keeps known state by id.
  std::string loadLamps(const std::string& json, std::string* warnings = nullptr);
  // A settings file ("lampcontrol/1"), or {"scenes":[...]} and/or {"presets":[...]}: replaces the
  // scenes and/or presets present in it. Checked like the page; nothing changes on an error.
  std::string loadScenes(const std::string& json, std::string* warnings = nullptr);
  std::string scenesJson() const;  // scenes and presets, compact, for storage

  Lamp* find(const std::string& id);
  Lamp* findAddress(uint16_t address);
  void power(Lamp& lamp, bool on);
  void brightness(Lamp& lamp, int percent);
  void white(Lamp& lamp, int kelvin);
  void colour(Lamp& lamp, int hue, int saturation);
  void all(bool on);
  bool applyScene(size_t index);
  bool applyPreset(Lamp& lamp, size_t index);
  // "All lamps that are on": the same change for each lamp currently reported on. Returns how many.
  int groupPower(bool on);
  int groupBrightness(int percent);
  int groupWhite(int kelvin);
  int groupColour(int hue, int saturation);
  int groupPreset(size_t index);
  // A scene from what the lamps show now (lamps with an unknown state are left out); replaces a scene
  // of the same name. "" or the reason it failed.
  std::string storeScene(const std::string& name);
  bool deleteScene(size_t index);
  void statusReport(uint16_t address, bool on, uint32_t nowMs);
  // Full report (wire values as in the status message): brightness 2..254, mired, hue/sat 0..254.
  void statusReport(uint16_t address, bool on, bool colour, int brightness, int mired, int hue, int saturation, uint32_t nowMs);

  bool popCommand(Command& out);
  void clearQueue() { queue.clear(); }
  size_t pending() const { return queue.size(); }

 private:
  std::vector<Command> queue;
  void enqueue(const Command& c);
};
