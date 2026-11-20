// lampctl: full-screen lamp remote for a Kindle 4 (non-touch), talking to the lamp bridge.
//
//   lampctl [bridge-address]          (default 192.168.8.146)
//
// Draws scenes, lamps and "All off" with FBInk (TrueType, the Kindle's own Helvetica) and reads the
// buttons from /dev/input. Up/down (and the page buttons) move the selection, the 5-way centre
// switches (scene: apply, lamp: on/off), left/right step a lamp's brightness, Menu redraws with a
// full refresh, the keyboard key quits. The state is fetched every 30 s and after every action.
// HTTP API and its plain-text format: ../bridge/src/api.h.
#include "fbink.h"
#include <arpa/inet.h>
#include <errno.h>
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
#define MAX_ITEMS 40

enum { KEY_UP_ = 103, KEY_DOWN_ = 108, KEY_LEFT_ = 105, KEY_RIGHT_ = 106, KEY_PRESS = 194, KEY_MENU_ = 139,
       KEY_BACK_ = 158, KEY_HOME_ = 102, KEY_KBD_ = 29 };
// Page buttons (left/right side, forward/back); codes vary between K4 units, all move the selection.
static const int PAGE_FWD[] = {191, 109}, PAGE_BACK[] = {193, 104};

typedef enum { SCENE, LAMP, ALL_OFF } Kind;
// brightnessKnown: the bridge only knows a lamp's brightness after setting it; until this remote
// changes it, only on/off is shown.
typedef struct { Kind kind; char id[16]; char name[64]; int on; int brightness; bool brightnessKnown; } Item;

static Item items[MAX_ITEMS];
static int count = 0, selected = 0, first = 0, renders = 0;
static char bridgeHost[64] = "192.168.8.146", status[96] = "", via[64] = "";
static int fb = -1;
static FBInkConfig cfg;

// ---- HTTP (plain sockets; the bridge closes the connection after each response) -----------------
static int http(const char* method, const char* path, const char* body, char* out, size_t outSize) {
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s < 0) return -1;
  struct timeval tv = {5, 0};
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  struct sockaddr_in a = {0};
  a.sin_family = AF_INET;
  a.sin_port = htons(80);
  if (inet_pton(AF_INET, bridgeHost, &a.sin_addr) != 1 || connect(s, (struct sockaddr*)&a, sizeof a) != 0) { close(s); return -1; }
  char req[512];
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
  char* bodyStart = strstr(out, "\r\n\r\n");
  if (bodyStart) memmove(out, bodyStart + 4, strlen(bodyStart + 4) + 1);
  return code;
}

// ---- State: "bridge\t<via>\t<msg>", "lamp\t<id>\t<name>\t<on>\t<b>...", "scene\t<i>\t<name>" ---------
static void parseState(char* text) {
  Item next[MAX_ITEMS];
  int n = 0, scenes = 0;
  // Scenes first, then lamps, then "All off" (the bridge lists lamps before scenes).
  for (int pass = 0; pass < 2; pass++) {
    char copy[8192];
    snprintf(copy, sizeof copy, "%s", text);
    for (char* line = strtok(copy, "\n"); line && n < MAX_ITEMS - 1; line = strtok(NULL, "\n")) {
      char* f[10] = {0};
      int k = 0;
      for (char *p = line, *t; k < 10 && (t = strsep(&p, "\t")); ) f[k++] = t;
      if (pass == 0 && k >= 3 && !strcmp(f[0], "scene")) {
        Item it = {.kind = SCENE};
        snprintf(it.id, sizeof it.id, "%s", f[1]);
        snprintf(it.name, sizeof it.name, "%s", f[2]);
        next[n++] = it; scenes++;
      } else if (pass == 1 && k >= 5 && !strcmp(f[0], "lamp")) {
        Item it = {.kind = LAMP};
        snprintf(it.id, sizeof it.id, "%s", f[1]);
        snprintf(it.name, sizeof it.name, "%s", f[2]);
        it.on = f[3][0] == '1' ? 1 : f[3][0] == '0' ? 0 : -1;
        it.brightness = atoi(f[4]);
        next[n++] = it;
      } else if (pass == 0 && k >= 3 && !strcmp(f[0], "bridge")) {
        snprintf(via, sizeof via, "%s", strcmp(f[1], "-") ? f[1] : "");
      }
    }
  }
  Item all = {.kind = ALL_OFF};
  snprintf(all.name, sizeof all.name, "All off");
  next[n++] = all;
  for (int i = 0; i < n; i++)
    for (int j = 0; j < count; j++)
      if (next[i].kind == LAMP && items[j].kind == LAMP && !strcmp(next[i].id, items[j].id)) next[i].brightnessKnown = items[j].brightnessKnown;
  // Keep the selection on the same entry if the list changed.
  Item old = count ? items[selected] : (Item){0};
  memcpy(items, next, sizeof(Item) * (size_t)n);
  count = n;
  if (selected >= count) selected = count - 1;
  for (int i = 0; i < count; i++)
    if (items[i].kind == old.kind && !strcmp(items[i].id, old.id) && !strcmp(items[i].name, old.name)) selected = i;
  (void)scenes;
}

static bool refreshState(void) {
  char buf[8192];
  int code = http("GET", "/api/state", NULL, buf, sizeof buf);
  if (code != 200) { snprintf(status, sizeof status, "Bridge not reachable"); return false; }
  parseState(buf);
  time_t now = time(NULL);
  struct tm lt;
  localtime_r(&now, &lt);
  if (via[0]) snprintf(status, sizeof status, "via %s  ·  updated %02d:%02d", via, lt.tm_hour, lt.tm_min);
  else snprintf(status, sizeof status, "bridge has no lamp connection  ·  %02d:%02d", lt.tm_hour, lt.tm_min);
  return true;
}

static bool post(const char* path, const char* body) {
  char buf[8192];
  int code = http("POST", path, body, buf, sizeof buf);
  if (code == 200) { parseState(buf); return true; }
  snprintf(status, sizeof status, code < 0 ? "Bridge not reachable" : "Bridge error %d", code);
  return false;
}

// ---- Drawing -------------------------------------------------------------------------------------
enum { TOP = 120, ROW = 76, HEADER = 34, BOTTOM = 40 };

static void text(const char* s, int x, int y, int px, bool bold, bool white) {
  FBInkOTConfig ot = {0};
  ot.margins.top = (short)y;
  ot.margins.left = (short)x;
  ot.size_px = (unsigned short)px;
  ot.style = bold ? FNT_BOLD : FNT_REGULAR;
  FBInkConfig c = cfg;
  c.no_refresh = true;
  // White text: draw black-on-white inverted (its box is black, matching the selection bar).
  // A white fg_color with is_bgless rendered nothing on the K4.
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

static void stateText(const Item* it, char* out, size_t size) {
  if (it->kind != LAMP) { out[0] = 0; return; }
  if (it->on == 1 && it->brightnessKnown) snprintf(out, size, "on  %d %%", it->brightness);
  else if (it->on == 1) snprintf(out, size, "on");
  else snprintf(out, size, it->on == 0 ? "off" : "?");
}

// Rows that fit; the list scrolls so that the selection stays visible.
static int visibleRows(void) { return (H - TOP - BOTTOM - 2 * HEADER) / ROW; }

static void render(bool flash) {
  box(0, 0, W, H, false);
  text("Lamps", 30, 28, 52, true, false);
  text(status, 30, 92, 20, false, false);
  int rows = visibleRows();
  if (selected < first) first = selected;
  if (selected >= first + rows) first = selected - rows + 1;
  int y = TOP;
  Kind section = (Kind)-1;
  for (int i = first; i < count && i < first + rows; i++) {
    const Item* it = &items[i];
    Kind sec = it->kind == ALL_OFF ? LAMP : it->kind;
    if (sec != section) {
      section = sec;
      if (it->kind != ALL_OFF) { text(sec == SCENE ? "SCENES" : "LAMPS", 30, y + 6, 20, true, false); y += HEADER; }
    }
    bool sel = i == selected;
    if (sel) box(14, y, W - 28, ROW - 8, true);
    else box(14, y + ROW - 9, W - 28, 1, true);  // thin separator
    text(it->name, 34, y + 16, 36, it->kind == ALL_OFF, sel);
    char st[32];
    stateText(it, st, sizeof st);
    if (st[0]) text(st, 400, y + 20, 30, false, sel);
    y += ROW;
  }
  if (first > 0) text("more above", 450, TOP - 30, 20, false, false);
  if (first + rows < count) text("more below", 450, H - BOTTOM - 8, 20, false, false);
  text("left/right: brightness   centre: switch   keyboard key: quit", 30, H - 30, 17, false, false);
  // Partial refresh normally; a flashing full refresh every 15 renders (or on Menu) clears e-ink ghosting.
  FBInkConfig c = cfg;
  c.is_flashing = flash || (++renders % 15 == 0);
  c.wfm_mode = WFM_GC16;
  fbink_refresh(fb, 0, 0, W, H, &c);
}

// ---- Actions ---------------------------------------------------------------------------------------
static const int STEPS[] = {5, 10, 30, 60, 100};
static void brightnessStep(Item* it, int dir) {
  int n = sizeof STEPS / sizeof STEPS[0], target = it->brightness;
  if (dir > 0) { target = STEPS[n - 1]; for (int i = 0; i < n; i++) if (STEPS[i] > it->brightness) { target = STEPS[i]; break; } }
  else { target = STEPS[0]; for (int i = n - 1; i >= 0; i--) if (STEPS[i] < it->brightness) { target = STEPS[i]; break; } }
  char body[64];
  if (it->on != 1 && dir > 0) { snprintf(body, sizeof body, "id=%s&on=1", it->id); post("/api/lamp", body); }
  snprintf(body, sizeof body, "id=%s&b=%d", it->id, target);
  char id[16];
  snprintf(id, sizeof id, "%s", it->id);
  if (post("/api/lamp", body))  // the post re-parsed the list: find the lamp again
    for (int i = 0; i < count; i++) if (items[i].kind == LAMP && !strcmp(items[i].id, id)) items[i].brightnessKnown = true;
}

static void activate(Item* it) {
  char body[64];
  if (it->kind == SCENE) { snprintf(body, sizeof body, "i=%s", it->id); post("/api/scene", body); }
  else if (it->kind == LAMP) { snprintf(body, sizeof body, "id=%s&on=%d", it->id, it->on == 1 ? 0 : 1); post("/api/lamp", body); }
  else post("/api/all", "on=0");
}

// ---- Input -----------------------------------------------------------------------------------------
static int inputs[8], inputCount = 0, maxFd = -1;
// Next key press, or 0 after timeoutSec without one.
static int nextKey(int timeoutSec) {
  for (;;) {
    fd_set set;
    FD_ZERO(&set);
    for (int i = 0; i < inputCount; i++) FD_SET(inputs[i], &set);
    struct timeval tv = {timeoutSec, 0};
    int r = select(maxFd + 1, &set, NULL, NULL, &tv);
    if (r <= 0) return 0;
    for (int i = 0; i < inputCount; i++) {
      if (!FD_ISSET(inputs[i], &set)) continue;
      struct input_event ev;
      while (read(inputs[i], &ev, sizeof ev) == (ssize_t)sizeof ev)
        if (ev.type == EV_KEY && ev.value == 1) return ev.code;
    }
  }
}
static bool isIn(int key, const int* list, int n) { for (int i = 0; i < n; i++) if (list[i] == key) return true; return false; }

int main(int argc, char** argv) {
  if (argc > 1) snprintf(bridgeHost, sizeof bridgeHost, "%s", argv[1]);
  for (int i = 0; i < 8; i++) {
    char path[32];
    snprintf(path, sizeof path, "/dev/input/event%d", i);
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd >= 0) { inputs[inputCount++] = fd; if (fd > maxFd) maxFd = fd; }
  }
  fb = fbink_open();
  cfg.is_quiet = true;
  if (fb < 0 || fbink_init(fb, &cfg) < 0) { fprintf(stderr, "lampctl: no framebuffer\n"); return 1; }
  fbink_add_ot_font(FONT_DIR "Helvetica_LT_65_Medium.ttf", FNT_REGULAR);
  fbink_add_ot_font(FONT_DIR "Helvetica_LT_75_Bold.ttf", FNT_BOLD);

  refreshState();
  render(true);
  for (;;) {
    int key = nextKey(30);
    bool flash = false;
    if (key == 0 || key == KEY_BACK_ || key == KEY_HOME_) refreshState();
    else if (key == KEY_UP_ || isIn(key, PAGE_BACK, 2)) { if (selected > 0) selected--; }
    else if (key == KEY_DOWN_ || isIn(key, PAGE_FWD, 2)) { if (selected < count - 1) selected++; }
    else if (key == KEY_PRESS && count) activate(&items[selected]);
    else if ((key == KEY_LEFT_ || key == KEY_RIGHT_) && count && items[selected].kind == LAMP)
      brightnessStep(&items[selected], key == KEY_RIGHT_ ? 1 : -1);
    else if (key == KEY_MENU_) { refreshState(); flash = true; }
    else if (key == KEY_KBD_) break;
    else continue;
    render(flash);
  }
  box(0, 0, W, H, false);
  FBInkConfig c = cfg;
  c.is_flashing = true;
  fbink_refresh(fb, 0, 0, W, H, &c);
  fbink_free_ot_fonts();
  fbink_close(fb);
  return 0;
}
