// lampctl: full-screen lamp remote for a Kindle 4 (non-touch), talking to the lamp bridge.
//
//   lampctl [bridge-address]          (default 192.168.8.146)
//
// Pages (page buttons switch): Lamps, Status, Help. Status line at the bottom. Buttons, everywhere:
//   up/down select · left/right change the selected value · centre switch/apply · Menu details
//   Back close/return to Lamps · Home reload with a full refresh · keyboard key twice: quit
// Drawn with FBInk (the Kindle's Helvetica); HTTP API and its plain-text format: ../bridge/src/api.h.
#include "fbink.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <signal.h>
#include <fcntl.h>
#include <linux/input.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define FONT_DIR "/usr/java/lib/fonts/"
#define W 600
#define H 800
#define MAXN 40

enum { K_UP = 103, K_DOWN = 108, K_LEFT = 105, K_RIGHT = 106, K_PRESS = 194, K_MENU = 139, K_BACK = 158,
       K_HOME = 102, K_KBD = 29 };
static const int PAGE_FWD[] = {191, 109}, PAGE_BACK[] = {193, 104};

// ---- State from the bridge -------------------------------------------------------------------------
typedef struct { char id[16]; char name[64]; int on; int b; char mode; int k, h, s; bool light; } Lamp;
typedef struct { char idx[8]; char name[64]; } Scene;
typedef struct { char idx[8]; char name[32]; char mode; int k, h, s; } Preset;
static Lamp lamps[MAXN];
static Scene scenes[MAXN];
static Preset presets[MAXN];
static int nLamps, nScenes, nPresets;
static char bridgeHost[64] = "192.168.8.146", via[64], clockText[8], message[128];
static bool reachable;

static int http(const char* method, const char* path, const char* body, char* out, size_t outSize) {
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) return -1;
  struct timeval tv = {6, 0};
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  struct sockaddr_in a = {0};
  a.sin_family = AF_INET;
  a.sin_port = htons(80);
  if (inet_pton(AF_INET, bridgeHost, &a.sin_addr) != 1 || connect(s, (struct sockaddr*)&a, sizeof a) != 0) { close(s); return -1; }
  char req[768];
  int n = body ? snprintf(req, sizeof req, "%s %s HTTP/1.0\r\nHost: %s\r\nContent-Type: application/x-www-form-urlencoded\r\n"
                                           "Content-Length: %zu\r\n\r\n%s", method, path, bridgeHost, strlen(body), body)
               : snprintf(req, sizeof req, "%s %s HTTP/1.0\r\nHost: %s\r\n\r\n", method, path, bridgeHost);
  if (send(s, req, (size_t)n, 0) != n) { close(s); return -1; }
  size_t got = 0;
  for (;;) {
    ssize_t r = recv(s, out + got, outSize - 1 - got, 0);
    if (r <= 0) break;
    got += (size_t)r;
    if (got >= outSize - 1) break;
  }
  close(s);
  out[got] = 0;
  int code = 0;
  if (sscanf(out, "HTTP/%*s %d", &code) != 1) return -1;
  char* start = strstr(out, "\r\n\r\n");
  if (start) memmove(out, start + 4, strlen(start + 4) + 1);
  return code;
}

static void parseState(const char* text) {
  static char copy[12000];
  snprintf(copy, sizeof copy, "%s", text);
  nLamps = nScenes = nPresets = 0;
  via[0] = clockText[0] = 0;
  for (char* line = strtok(copy, "\n"); line; line = strtok(NULL, "\n")) {
    char* f[12] = {0};
    int k = 0;
    for (char *p = line, *t; k < 12 && (t = strsep(&p, "\t")); ) f[k++] = t;
    if (k >= 2 && !strcmp(f[0], "bridge")) snprintf(via, sizeof via, "%s", strcmp(f[1], "-") ? f[1] : "");
    else if (k >= 3 && !strcmp(f[0], "time") && f[1][0] == '1') snprintf(clockText, 6, "%s", f[2]);
    else if (k >= 9 && !strcmp(f[0], "lamp") && nLamps < MAXN) {
      Lamp* l = &lamps[nLamps++];
      snprintf(l->id, sizeof l->id, "%s", f[1]);
      snprintf(l->name, sizeof l->name, "%s", f[2]);
      l->on = f[3][0] == '1' ? 1 : f[3][0] == '0' ? 0 : -1;
      l->b = atoi(f[4]); l->mode = f[5][0]; l->k = atoi(f[6]); l->h = atoi(f[7]); l->s = atoi(f[8]);
      l->light = k >= 11 && f[10][0] == '1';
    } else if (k >= 3 && !strcmp(f[0], "scene") && nScenes < MAXN) {
      snprintf(scenes[nScenes].idx, sizeof scenes[0].idx, "%s", f[1]);
      snprintf(scenes[nScenes].name, sizeof scenes[0].name, "%s", f[2]);
      nScenes++;
    } else if (k >= 7 && !strcmp(f[0], "preset") && nPresets < MAXN) {
      Preset* p = &presets[nPresets++];
      snprintf(p->idx, sizeof p->idx, "%s", f[1]);
      snprintf(p->name, sizeof p->name, "%s", f[2]);
      p->mode = f[3][0]; p->k = atoi(f[4]); p->h = atoi(f[5]); p->s = atoi(f[6]);
    }
  }
}

static bool refresh(void) {
  static char buf[12000];
  int code = http("GET", "/api/state", NULL, buf, sizeof buf);
  reachable = code == 200;
  if (reachable) parseState(buf);
  return reachable;
}

// POST; on success the reply is the new state. On failure the bridge's reason becomes the message.
static bool post(const char* path, const char* body) {
  static char buf[12000];
  int code = http("POST", path, body, buf, sizeof buf);
  if (code == 200) { parseState(buf); reachable = true; message[0] = 0; return true; }
  if (code < 0) { reachable = false; snprintf(message, sizeof message, "Bridge not reachable: not sent"); }
  else { buf[strcspn(buf, "\r\n")] = 0; snprintf(message, sizeof message, "%.120s", buf[0] ? buf : "Bridge error"); }
  return false;
}

static void urlencode(const char* in, char* out, size_t size) {
  size_t o = 0;
  for (const unsigned char* p = (const unsigned char*)in; *p && o + 4 < size; p++) {
    if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.') out[o++] = (char)*p;
    else o += (size_t)snprintf(out + o, size - o, "%%%02X", *p);
  }
  out[o] = 0;
}

// ---- Drawing ---------------------------------------------------------------------------------------
static int fb = -1;
static FBInkConfig cfg;
static int renders;

static void text(const char* s, int x, int y, int px, bool bold, bool white) {
  FBInkOTConfig ot = {0};
  ot.margins.top = (short)y;
  ot.margins.left = (short)x;
  ot.size_px = (unsigned short)px;
  ot.style = bold ? FNT_BOLD : FNT_REGULAR;
  FBInkConfig c = cfg;
  c.no_refresh = true;
  // White text: black-on-white inverted (its box is black, matching the selection bar); a white
  // fg_color with is_bgless rendered nothing on the K4.
  c.fg_color = FG_BLACK;
  c.bg_color = BG_WHITE;
  c.is_inverted = white;
  c.is_bgless = !white;
  fbink_update_pen_colors(&c);  // FBInk takes colours at init; changes need this
  fbink_print_ot(fb, s, &ot, &c, NULL);
}
static void box(int x, int y, int w, int h, bool black) {
  FBInkConfig c = cfg;
  c.no_refresh = true;
  c.bg_color = black ? BG_BLACK : BG_WHITE;
  fbink_update_pen_colors(&c);
  FBInkRect r = {(unsigned short)x, (unsigned short)y, (unsigned short)w, (unsigned short)h};
  fbink_cls(fb, &c, &r, false);
}
static void show(bool flash) {
  FBInkConfig c = cfg;
  c.is_flashing = flash || (++renders % 15 == 0);  // a flash now and then clears e-ink ghosting
  c.wfm_mode = WFM_GC16;
  fbink_refresh(fb, 0, 0, W, H, &c);
}

enum { ROW = 64, LIST_TOP = 16, FOOTER = 52 };
// A list row: label left, value at x=330; selected rows are white on black.
static void row(int y, const char* label, const char* value, bool selected, bool bold) {
  if (selected) box(10, y, W - 20, ROW - 6, true);
  else box(10, y + ROW - 7, W - 20, 1, true);
  text(label, 26, y + 14, 32, bold, selected);
  if (value && value[0]) text(value, 330, y + 18, 26, false, selected);
}
static void heading(int y, const char* s) { text(s, 26, y + 8, 18, true, false); }

// ---- Values ----------------------------------------------------------------------------------------
static const char* hueName(int h);
static const char* presetName(char mode, int k, int h, int s) {
  for (int i = 0; i < nPresets; i++) {
    const Preset* p = &presets[i];
    if (p->mode != mode) continue;
    if (mode == 'w' && abs(p->k - k) <= 60) return p->name;
    if (mode == 'c' && abs(p->h - h) <= 4 && abs(p->s - s) <= 4) return p->name;
  }
  return NULL;
}
static void lightText(const Lamp* l, char* out, size_t size) {
  const char* p = presetName(l->mode, l->k, l->h, l->s);
  if (p) snprintf(out, size, "%s", p);
  else if (l->mode == 'c') snprintf(out, size, "%s %d %%", hueName(l->h), l->s);
  else snprintf(out, size, "%d K", l->k);
}
static void lampValue(const Lamp* l, char* out, size_t size) {
  if (l->on < 0) { snprintf(out, size, "?"); return; }
  if (l->on == 0) { snprintf(out, size, "off"); return; }
  if (!l->light) { snprintf(out, size, "on"); return; }
  char light[40];
  lightText(l, light, sizeof light);
  snprintf(out, size, "%d %%  %s", l->b, light);
}
static const int B_STEPS[] = {10, 20, 30, 45, 60, 80, 100};    // 5 % turned the Sofa lamp red
static const int K_STEPS[] = {2200, 2700, 3000, 3500, 4000, 5000, 6500};
static const int S_STEPS[] = {25, 50, 75, 100};
// Twelve named hues, 30° apart (the lamps report hue in 254 steps, so match within ±8°).
static const char* HUES[12] = {"red", "orange", "yellow", "lime", "green", "mint", "cyan", "sky", "blue", "violet", "purple", "pink"};
static int hueIndex(int h) { return ((h + 15) % 360) / 30; }
static const char* hueName(int h) { return HUES[hueIndex(h)]; }
static int step(const int* steps, int n, int current, int dir) {
  if (dir > 0) { for (int i = 0; i < n; i++) if (steps[i] > current) return steps[i]; return steps[n - 1]; }
  for (int i = n - 1; i >= 0; i--) if (steps[i] < current) return steps[i];
  return steps[0];
}

// ---- Views -----------------------------------------------------------------------------------------
typedef enum { V_LIST, V_DETAILS, V_KEYBOARD, V_CONFIRM } View;
typedef enum { R_SCENE, R_LAMP, R_GROUP, R_STORE, R_ALLOFF } RowKind;
typedef struct { RowKind kind; int index; } Row;
static View view = V_LIST;
static int page;              // 0 Lamps, 1 Status, 2 Help
static int sel, first;        // list selection and scroll
static Row rows[MAXN * 2 + 4];
static int nRows;
static int detSel;            // details: 0 power, 1 brightness, 2 white, 3 colour, 4 saturation, 5 preset
enum { D_POWER, D_BRIGHT, D_WHITE, D_COLOUR, D_SAT, D_PRESET, D_ROWS };
static Row detRow;            // what the details screen is for (lamp or group)
static int presetCursor;      // details: preset shown on the preset row
static char kbText[40];
static int kbR, kbC;
static long quitArmedUntil;
// Sleep: the Kindle's power manager (powerd) starts its screensaver 10 minutes after its idle timer was
// last reset (we reset it on every button press), or when the power button is pressed; on battery it
// suspends a minute later, which also turns Wi-Fi off. While the screensaver is on, powerd keeps the
// buttons to itself, so only the power button wakes the Kindle. The remote follows powerd's events
// (lipc-wait-event in a child process): it shows "Asleep" on the way down, and on the way up it ends
// the screensaver itself (the Kindle's own interface would normally do that) and reconnects.
static bool asleepShown;
static int powerFd = -1;
static pid_t powerPid = -1;
static void touchSleepTimer(void) {
  if (system("lipc-set-prop com.lab126.powerd touchScreenSaverTimeout 1 >/dev/null 2>&1") != 0) { /* no powerd: fine */ }
}
static void watchPower(void) {
  int p[2];
  if (pipe(p) < 0) return;
  powerPid = fork();
  if (powerPid == 0) {
    dup2(p[1], 1);
    close(p[0]);
    close(p[1]);
    execlp("lipc-wait-event", "lipc-wait-event", "-m", "com.lab126.powerd",
           "goingToScreenSaver,outOfScreenSaver,resuming,outOfShutdown", (char*)NULL);
    _exit(1);
  }
  close(p[1]);
  if (powerPid < 0) { close(p[0]); return; }
  powerFd = p[0];
  fcntl(powerFd, F_SETFL, O_NONBLOCK);
}
static bool kindleActive(void) {
  FILE* f = popen("lipc-get-prop -s com.lab126.powerd state 2>/dev/null", "r");
  if (!f) return true;
  char state[32] = "";
  if (!fgets(state, sizeof state, f)) state[0] = 0;
  pclose(f);
  return !strncmp(state, "active", 6);
}

static void buildRows(void) {
  nRows = 0;
  for (int i = 0; i < nScenes; i++) rows[nRows++] = (Row){R_SCENE, i};
  for (int i = 0; i < nLamps; i++) rows[nRows++] = (Row){R_LAMP, i};
  rows[nRows++] = (Row){R_GROUP, 0};
  rows[nRows++] = (Row){R_STORE, 0};
  rows[nRows++] = (Row){R_ALLOFF, 0};
  if (sel >= nRows) sel = nRows - 1;
}
static int lampsOn(void) { int n = 0; for (int i = 0; i < nLamps; i++) if (lamps[i].on == 1) n++; return n; }

static void footer(void) {
  box(0, H - FOOTER, W, 1, true);
  char line[200];
  long now = time(NULL);
  if (asleepShown) snprintf(line, sizeof line, "Asleep  ·  press the power button to wake");
  else if (now < quitArmedUntil) snprintf(line, sizeof line, "Press the keyboard key again to quit");
  else if (message[0]) snprintf(line, sizeof line, "%s", message);
  else if (!reachable) snprintf(line, sizeof line, "Bridge not reachable  ·  page %d/3", page + 1);
  else if (!via[0]) snprintf(line, sizeof line, "NO LAMP CONNECTION: nothing is sent  ·  %s", clockText);
  else snprintf(line, sizeof line, "via %s  ·  %s  ·  page %d/3", via, clockText[0] ? clockText : "--:--", page + 1);
  text(line, 20, H - FOOTER + 14, 20, false, false);
}

static void drawLamps(void) {
  buildRows();
  int area = H - FOOTER - LIST_TOP - 10;
  // Rows plus headings must fit; scroll so the selection stays visible.
  int fit = (area - 2 * 34) / ROW;
  if (sel < first) first = sel;
  if (sel >= first + fit) first = sel - fit + 1;
  int y = LIST_TOP;
  int lastSection = -1;
  for (int i = first; i < nRows && i < first + fit; i++) {
    Row r = rows[i];
    int section = r.kind == R_SCENE ? 0 : r.kind == R_LAMP || r.kind == R_GROUP ? 1 : 2;
    if (section != lastSection && section < 2) { heading(y, section == 0 ? "SCENES" : "LAMPS"); y += 34; }
    lastSection = section;
    bool s = i == sel;
    char value[64] = "";
    if (r.kind == R_SCENE) row(y, scenes[r.index].name, "", s, false);
    else if (r.kind == R_LAMP) { lampValue(&lamps[r.index], value, sizeof value); row(y, lamps[r.index].name, value, s, false); }
    else if (r.kind == R_GROUP) { snprintf(value, sizeof value, "%d on", lampsOn()); row(y, "Lamps that are on", value, s, false); }
    else if (r.kind == R_STORE) row(y, "Store current as scene", "", s, false);
    else row(y, "All off", "", s, true);
    y += ROW;
  }
  if (first > 0) text("more above", 470, 0, 16, false, false);
  if (first + fit < nRows) text("more below", 470, H - FOOTER - 22, 16, false, false);
}

static void drawStatus(void) {
  text("Status", 26, 14, 40, true, false);
  int y = 80;
  text("Lamp", 26, y, 20, true, false); text("Power", 290, y, 20, true, false);
  text("Bright.", 370, y, 20, true, false); text("Light", 460, y, 20, true, false);
  y += 34;
  for (int i = 0; i < nLamps; i++) {
    const Lamp* l = &lamps[i];
    char b[16] = "-", light[40] = "-";
    if (l->light) { snprintf(b, sizeof b, "%d %%", l->b); lightText(l, light, sizeof light); }
    text(l->name, 26, y, 22, false, false);
    text(l->on < 0 ? "?" : l->on ? "on" : "off", 290, y, 22, false, false);
    text(b, 370, y, 22, false, false);
    text(light, 460, y, 22, false, false);
    box(26, y + 40, W - 52, 1, true);
    y += 50;
  }
  y += 16;
  char line[160];
  snprintf(line, sizeof line, "Bridge %s: %s", bridgeHost, !reachable ? "not reachable" : via[0] ? "connected" : "no lamp connection");
  text(line, 26, y, 20, false, false);
  if (via[0]) { snprintf(line, sizeof line, "Connected through %s", via); text(line, 26, y + 30, 20, false, false); }
  snprintf(line, sizeof line, "%d scene(s), %d preset(s)", nScenes, nPresets);
  text(line, 26, y + 60, 20, false, false);
  text("Values come from the lamps' own status reports.", 26, y + 100, 18, false, false);
}

static void drawHelp(void) {
  text("Buttons", 26, 14, 40, true, false);
  const char* lines[] = {
    "Up / down", "select",
    "Left / right", "change the value (brightness on the list)",
    "Centre", "switch a lamp, apply a scene",
    "Menu", "details: brightness, white, colour, presets; scene: delete",
    "Back", "close details, back to the list",
    "Page buttons", "Lamps, Status, Help pages",
    "Home", "reload, full screen refresh",
    "Keyboard key", "twice: quit to the Kindle",
  };
  int y = 90;
  for (int i = 0; i < 16; i += 2) {
    text(lines[i], 26, y, 24, true, false);
    text(lines[i + 1], 26, y + 30, 22, false, false);
    y += 72;
  }
}

static const char* detTitle(void) { return detRow.kind == R_GROUP ? "Lamps that are on" : lamps[detRow.index].name; }
static void drawDetails(void) {
  text(detTitle(), 26, 14, 40, true, false);
  const Lamp* l = detRow.kind == R_LAMP ? &lamps[detRow.index] : NULL;
  char v[D_ROWS][48];
  if (l) {
    bool colour = l->light && l->mode == 'c';
    snprintf(v[D_POWER], 48, "%s", l->on < 0 ? "?" : l->on ? "on" : "off");
    if (l->light) snprintf(v[D_BRIGHT], 48, "%d %%", l->b); else snprintf(v[D_BRIGHT], 48, "-");
    if (l->light && !colour) snprintf(v[D_WHITE], 48, "%d K", l->k); else snprintf(v[D_WHITE], 48, "-");
    if (colour) { snprintf(v[D_COLOUR], 48, "%s", hueName(l->h)); snprintf(v[D_SAT], 48, "%d %%", l->s); }
    else { snprintf(v[D_COLOUR], 48, "-"); snprintf(v[D_SAT], 48, "-"); }
  } else {
    snprintf(v[D_POWER], 48, "%d on (centre: off)", lampsOn());
    for (int i = D_BRIGHT; i <= D_SAT; i++) snprintf(v[i], 48, "left / right");
  }
  snprintf(v[D_PRESET], 48, "%s", nPresets ? presets[presetCursor % nPresets].name : "none");
  const char* labels[D_ROWS] = {"Power", "Brightness", "White", "Colour", "Saturation", "Preset"};
  int y = 90;
  for (int i = 0; i < D_ROWS; i++) { row(y, labels[i], v[i], i == detSel, false); y += ROW; }
  text("Left/right change at once", 26, y + 16, 20, false, false);
  text("Centre on White or Colour switches between them", 26, y + 44, 20, false, false);
  text("Back closes", 26, y + 72, 20, false, false);
}

// On-screen keyboard for scene names (the K4 has no keyboard).
static const char* KB[5][10] = {
  {"A", "B", "C", "D", "E", "F", "G", "H", "I", "J"},
  {"K", "L", "M", "N", "O", "P", "Q", "R", "S", "T"},
  {"U", "V", "W", "X", "Y", "Z", "Ä", "Ö", "Ü", "ß"},
  {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"},
  {"Space", "Delete", "OK", 0},
};
static int kbCols(int r) { int n = 0; while (n < 10 && KB[r][n]) n++; return n; }
static void drawKeyboard(void) {
  text("Scene name", 26, 14, 36, true, false);
  box(20, 128, W - 40, 2, true);
  char shown[48];
  snprintf(shown, sizeof shown, "%s_", kbText);
  text(shown, 30, 80, 34, false, false);
  int y = 160;
  for (int r = 0; r < 5; r++) {
    int n = kbCols(r), w = (W - 40) / n;
    for (int c = 0; c < n; c++) {
      int x = 20 + c * w;
      bool s = r == kbR && c == kbC;
      if (s) box(x + 2, y, w - 4, 60, true);
      else box(x + 2, y + 58, w - 4, 1, true);
      text(KB[r][c], x + (n == 10 ? 16 : 30), y + 14, n == 10 ? 30 : 26, n != 10, s);
    }
    y += 72;
  }
  text("Letters after the first are typed in lower case.", 26, y + 20, 18, false, false);
  text("Centre types  ·  Back cancels  ·  same name replaces", 26, y + 48, 18, false, false);
}
static void kbType(const char* key) {
  size_t len = strlen(kbText);
  if (!strcmp(key, "Delete")) {
    if (len) { len--; while (len && ((unsigned char)kbText[len] & 0xC0) == 0x80) len--; kbText[len] = 0; }  // UTF-8
    return;
  }
  char piece[8];
  if (!strcmp(key, "Space")) snprintf(piece, sizeof piece, " ");
  else if (len && kbText[len - 1] != ' ') {
    // Lower case after the first letter of a word (ASCII and the three umlauts).
    if (!strcmp(key, "Ä")) snprintf(piece, sizeof piece, "ä");
    else if (!strcmp(key, "Ö")) snprintf(piece, sizeof piece, "ö");
    else if (!strcmp(key, "Ü")) snprintf(piece, sizeof piece, "ü");
    else { snprintf(piece, sizeof piece, "%s", key); if (piece[1] == 0) piece[0] = (char)tolower((unsigned char)piece[0]); }
  } else snprintf(piece, sizeof piece, "%s", key);
  if (len + strlen(piece) < 30) strcat(kbText, piece);
}

static void drawConfirm(void) {
  text("Delete scene?", 26, 14, 40, true, false);
  text(scenes[rows[sel].index].name, 26, 100, 34, false, false);
  text("Centre: delete  ·  Back: keep", 26, 180, 24, false, false);
}

static void render(bool flash) {
  box(0, 0, W, H, false);
  if (view == V_DETAILS) drawDetails();
  else if (view == V_KEYBOARD) drawKeyboard();
  else if (view == V_CONFIRM) drawConfirm();
  else if (page == 0) drawLamps();
  else if (page == 1) drawStatus();
  else drawHelp();
  footer();
  show(flash);
}

// ---- Actions ---------------------------------------------------------------------------------------
static void lampCmd(int i, const char* what) {
  char body[96];
  snprintf(body, sizeof body, "id=%s&%s", lamps[i].id, what);
  post("/api/lamp", body);
}
// Details open on the preset that matches the lamp's current light, if any.
static void openDetails(Row r) {
  view = V_DETAILS; detRow = r; detSel = 1; presetCursor = 0;
  if (r.kind != R_LAMP) return;
  const Lamp* l = &lamps[r.index];
  const char* name = presetName(l->mode, l->k, l->h, l->s);
  for (int i = 0; name && i < nPresets; i++) if (!strcmp(presets[i].name, name)) presetCursor = i;
}
static void onList(int key) {
  Row r = rows[sel];
  if (key == K_UP && sel > 0) sel--;
  else if (key == K_DOWN && sel < nRows - 1) sel++;
  else if (key == K_PRESS) {
    char body[64];
    if (r.kind == R_SCENE) { snprintf(body, sizeof body, "i=%s", scenes[r.index].idx); post("/api/scene", body); }
    else if (r.kind == R_LAMP) lampCmd(r.index, lamps[r.index].on == 1 ? "on=0" : "on=1");
    else if (r.kind == R_GROUP) openDetails(r);
    else if (r.kind == R_STORE) { view = V_KEYBOARD; kbR = 4; kbC = 2; snprintf(kbText, sizeof kbText, "Scene %d", nScenes + 1); }
    else post("/api/all", "on=0");
  } else if ((key == K_LEFT || key == K_RIGHT) && (r.kind == R_LAMP || r.kind == R_GROUP)) {
    int dir = key == K_RIGHT ? 1 : -1;
    char what[32];
    if (r.kind == R_LAMP) {
      Lamp* l = &lamps[r.index];
      if (l->on != 1 && dir > 0) lampCmd(r.index, "on=1");
      snprintf(what, sizeof what, "b=%d", step(B_STEPS, 7, l->b, dir));
      lampCmd(r.index, what);
    } else {
      // The lamps that are on may differ: step from the brightest of them.
      int b = 0;
      for (int i = 0; i < nLamps; i++) if (lamps[i].on == 1 && lamps[i].b > b) b = lamps[i].b;
      snprintf(what, sizeof what, "b=%d", step(B_STEPS, 7, b ? b : 60, dir));
      post("/api/group", what);
    }
  } else if (key == K_MENU && (r.kind == R_LAMP || r.kind == R_GROUP)) openDetails(r);
  else if (key == K_MENU && r.kind == R_SCENE) view = V_CONFIRM;
}
// Current value for the details screen: the lamp's, or for the group the first lamp that is on.
static const Lamp* detLamp(char mode) {
  if (detRow.kind == R_LAMP) return &lamps[detRow.index];
  for (int i = 0; i < nLamps; i++) if (lamps[i].on == 1 && (!mode || lamps[i].mode == mode)) return &lamps[i];
  return NULL;
}
static void onDetails(int key) {
  bool group = detRow.kind == R_GROUP;
  if (key == K_BACK) { view = V_LIST; return; }
  if (key == K_UP && detSel > 0) { detSel--; return; }
  if (key == K_DOWN && detSel < D_ROWS - 1) { detSel++; return; }
  if (key != K_PRESS && key != K_LEFT && key != K_RIGHT) return;
  int dir = key == K_LEFT ? -1 : key == K_RIGHT ? 1 : 0;
  char what[48];
  const Lamp* l = detLamp(0);
  const Lamp* white = detLamp('w');
  const Lamp* colour = detLamp('c');
  bool inColour = l && l->light && l->mode == 'c';
  int hue = colour && colour->light && colour->mode == 'c' ? colour->h : 0;
  int sat = colour && colour->light && colour->mode == 'c' ? colour->s : 100;
  switch (detSel) {
    case D_POWER:
      if (group) post("/api/group", "on=0");
      else lampCmd(detRow.index, l->on == 1 ? "on=0" : "on=1");
      return;
    case D_BRIGHT: {
      if (!dir) return;
      int b = 0;
      if (group) { for (int i = 0; i < nLamps; i++) if (lamps[i].on == 1 && lamps[i].b > b) b = lamps[i].b; }
      else b = l->b;
      snprintf(what, sizeof what, "b=%d", step(B_STEPS, 7, b ? b : 60, dir));
      break;
    }
    case D_WHITE: {
      // From colour mode, the first press (or centre) switches to white at the last temperature.
      int k = white && white->light && white->mode == 'w' ? white->k : 2700;
      if (dir && !(inColour && !group)) k = step(K_STEPS, 7, k, dir);
      snprintf(what, sizeof what, "k=%d", k);
      break;
    }
    case D_COLOUR:
      // From white mode, the first press (or centre) switches to colour.
      if (dir && (inColour || group)) hue = ((hueIndex(hue) + dir + 12) % 12) * 30;
      snprintf(what, sizeof what, "h=%d&s=%d", hue, sat);
      break;
    case D_SAT:
      if (!dir || (!group && !inColour)) return;  // saturation only applies to a colour
      snprintf(what, sizeof what, "h=%d&s=%d", hue, step(S_STEPS, 4, sat, dir));
      break;
    default:
      if (!nPresets) return;
      if (dir) presetCursor = (presetCursor + dir + nPresets) % nPresets;
      snprintf(what, sizeof what, "preset=%s", presets[presetCursor].idx);
  }
  if (group) post("/api/group", what); else lampCmd(detRow.index, what);
}
static void onKeyboard(int key) {
  int n = kbCols(kbR);
  if (key == K_BACK) { view = V_LIST; return; }
  if (key == K_LEFT) kbC = (kbC + n - 1) % n;
  else if (key == K_RIGHT) kbC = (kbC + 1) % n;
  else if (key == K_UP || key == K_DOWN) {
    kbR = (kbR + (key == K_DOWN ? 1 : 4)) % 5;
    int m = kbCols(kbR);
    if (kbC >= m) kbC = m - 1;
  } else if (key == K_PRESS) {
    if (!strcmp(KB[kbR][kbC], "OK")) {
      char enc[128], body[160];
      urlencode(kbText, enc, sizeof enc);
      snprintf(body, sizeof body, "name=%s", enc);
      if (post("/api/scene/store", body)) { snprintf(message, sizeof message, "Scene \"%s\" stored", kbText); view = V_LIST; }
    } else kbType(KB[kbR][kbC]);
  }
}
static void onConfirm(int key) {
  if (key == K_PRESS) {
    char body[32];
    snprintf(body, sizeof body, "i=%s", scenes[rows[sel].index].idx);
    if (post("/api/scene/delete", body) && sel > 0) sel--;
    view = V_LIST;
  } else if (key == K_BACK) view = V_LIST;
}

// ---- Input and main loop ---------------------------------------------------------------------------
static int inputs[8], nInputs, maxFd = -1;
enum { E_SLEEP = 0x1000, E_WAKE };  // powerd events, returned like keys
static int nextKey(int timeoutSec) {
  for (;;) {
    fd_set set;
    FD_ZERO(&set);
    for (int i = 0; i < nInputs; i++) FD_SET(inputs[i], &set);
    if (powerFd >= 0) FD_SET(powerFd, &set);
    struct timeval tv = {timeoutSec, 0};
    if (select((powerFd > maxFd ? powerFd : maxFd) + 1, &set, NULL, NULL, &tv) <= 0) return 0;
    if (powerFd >= 0 && FD_ISSET(powerFd, &set)) {
      char buf[256];
      ssize_t n = read(powerFd, buf, sizeof buf - 1);
      if (n <= 0) { close(powerFd); powerFd = -1; continue; }  // watcher gone: no sleep handling
      buf[n] = 0;
      // Several events can arrive at once; the last one counts.
      const char* down = NULL;
      const char* up = NULL;
      for (const char* e = buf; (e = strstr(e, "goingToScreenSaver")); e++) down = e;
      const char* names[] = {"outOfScreenSaver", "resuming", "outOfShutdown"};
      for (int i = 0; i < 3; i++)
        for (const char* e = buf; (e = strstr(e, names[i])); e++) if (!up || e > up) up = e;
      if (up && (!down || up > down)) return E_WAKE;
      if (down) return E_SLEEP;
      continue;
    }
    for (int i = 0; i < nInputs; i++) {
      if (!FD_ISSET(inputs[i], &set)) continue;
      struct input_event ev;
      while (read(inputs[i], &ev, sizeof ev) == (ssize_t)sizeof ev)
        if (ev.type == EV_KEY && ev.value == 1) return ev.code;
    }
  }
}
static bool isIn(int key, const int* list) { return key == list[0] || key == list[1]; }

int main(int argc, char** argv) {
  if (argc > 1) snprintf(bridgeHost, sizeof bridgeHost, "%s", argv[1]);
  for (int i = 0; i < 8; i++) {
    char path[32];
    snprintf(path, sizeof path, "/dev/input/event%d", i);
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd >= 0) { inputs[nInputs++] = fd; if (fd > maxFd) maxFd = fd; }
  }
  fb = fbink_open();
  cfg.is_quiet = true;
  if (fb < 0 || fbink_init(fb, &cfg) < 0) { fprintf(stderr, "lampctl: no framebuffer\n"); return 1; }
  fbink_add_ot_font(FONT_DIR "Helvetica_LT_65_Medium.ttf", FNT_REGULAR);
  fbink_add_ot_font(FONT_DIR "Helvetica_LT_75_Bold.ttf", FNT_BOLD);

  refresh();
  touchSleepTimer();
  watchPower();
  render(true);
  char lastClock[8] = "";
  for (;;) {
    int key = nextKey(20);
    bool flash = false;
    if (key == E_SLEEP) {
      asleepShown = true;
      view = V_LIST;
      render(false);
      continue;
    }
    if (key == E_WAKE) {
      if (!asleepShown) continue;  // e.g. the screensaver ended because we ended it below
      asleepShown = false;
      if (!kindleActive()) {
        if (system("lipc-set-prop com.lab126.powerd wakeUp 1 >/dev/null 2>&1") != 0) { /* nothing better to do */ }
      }
      // After a suspend Wi-Fi needs a few seconds to come back.
      snprintf(message, sizeof message, "Waking up: reconnecting…");
      render(true);
      for (int i = 0; i < 30 && !refresh(); i++) sleep(1);
      message[0] = 0;
      touchSleepTimer();
      render(false);
      continue;
    }
    if (key == 0) {
      if (asleepShown) continue;
      // Idle: reload; redraw only if something visible changed (e-ink redraws are slow and ghost).
      static Lamp before[MAXN];
      memcpy(before, lamps, sizeof lamps);
      int n0 = nLamps, s0 = nScenes;
      bool r0 = reachable;
      refresh();
      if (!memcmp(before, lamps, sizeof lamps) && n0 == nLamps && s0 == nScenes && r0 == reachable && !strcmp(lastClock, clockText)) continue;
      snprintf(lastClock, sizeof lastClock, "%s", clockText);
      if (view == V_KEYBOARD || view == V_CONFIRM) continue;  // don't disturb typing
      render(false);
      continue;
    }
    touchSleepTimer();
    if (asleepShown) { asleepShown = false; render(false); }  // a key got through: evidently awake
    if (key == K_KBD) {
      long now = time(NULL);
      if (now < quitArmedUntil) break;
      quitArmedUntil = now + 4;
      render(false);
      continue;
    }
    quitArmedUntil = 0;
    if (key != K_PRESS && key != K_LEFT && key != K_RIGHT) message[0] = 0;
    if (key == K_HOME) { refresh(); flash = true; }
    else if (view == V_DETAILS) onDetails(key);
    else if (view == V_KEYBOARD) onKeyboard(key);
    else if (view == V_CONFIRM) onConfirm(key);
    else if (isIn(key, PAGE_FWD)) page = (page + 1) % 3;
    else if (isIn(key, PAGE_BACK)) page = (page + 2) % 3;
    else if (key == K_BACK) { if (page) page = 0; else refresh(); }
    else if (page == 0) onList(key);
    else if (key == K_UP || key == K_DOWN) continue;  // nothing to select on Status/Help
    render(flash);
  }
  box(0, 0, W, H, false);
  FBInkConfig c = cfg;
  c.is_flashing = true;
  fbink_refresh(fb, 0, 0, W, H, &c);
  fbink_free_ot_fonts();
  fbink_close(fb);
  if (powerPid > 0) kill(powerPid, SIGTERM);
  return 0;
}
