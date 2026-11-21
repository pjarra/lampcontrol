// Host test server: the bridge's real API and pages on the Mac, with simulated lamps instead of
// Bluetooth. Commands leave the queue every 350 ms like on the ESP32 and are printed.
//
//   make -C test server && test/host_server [--lan] [port]     (default 127.0.0.1:8780)
#include "../src/api.h"
#include "../src/awox.h"
#include <arpa/inet.h>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <netinet/in.h>
#include <sstream>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace {
const auto START = std::chrono::steady_clock::now();
uint32_t now() { return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - START).count(); }
std::string readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream s; s << f.rdbuf();
  return s.str();
}
std::string urlDecode(const std::string& s) {
  std::string out;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '+') out += ' ';
    else if (s[i] == '%' && i + 2 < s.size()) { out += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16); i += 2; }
    else out += s[i];
  }
  return out;
}
void parseForm(const std::string& s, std::map<std::string, std::string>& out) {
  std::stringstream ss(s);
  std::string pair;
  while (std::getline(ss, pair, '&')) {
    auto eq = pair.find('=');
    if (eq != std::string::npos) out[urlDecode(pair.substr(0, eq))] = urlDecode(pair.substr(eq + 1));
  }
}

struct Sim : api::Platform {
  bool known = false;
  std::string scenesStored;
  uint32_t nowMs() override { return now(); }
  bool timeKnown() override { return known; }
  std::string localTime() override {
    char buf[16]; time_t now = time(nullptr); struct tm lt; localtime_r(&now, &lt);
    strftime(buf, sizeof buf, "%H:%M:%S", &lt); return buf;
  }
  void setTime(uint64_t ms) override { known = true; std::printf("time set from a browser: %llu\n", (unsigned long long)ms); }
  std::string connection() override { return "Kamin (simulated)"; }
  std::string message() override { return "Simulated lamps (host test server)"; }
  bool saveScenes(const std::string& json) override { scenesStored = json; return true; }
  void requestClocks() override { std::printf("clocks requested\n"); }
  void requestLampsReload() override { std::printf("lamps reload requested\n"); }
  std::string raw() override { return "# simulated: no status messages\n"; }
  std::string requestUpdate(const std::string&, const std::string&) override { return "not on the host server"; }
  std::string info() override { return "mode: host test server\nscenes stored: " + std::to_string(scenesStored.size()) + " bytes\n"; }
};

const char* kindName(Kind k) {
  return k == Kind::Power ? "power" : k == Kind::Brightness ? "brightness" : k == Kind::Temperature ? "temperature" : "colour";
}
}  // namespace

int main(int argc, char** argv) {
  bool lan = false;
  int port = 8780;
  for (int i = 1; i < argc; i++) { std::string a = argv[i]; if (a == "--lan") lan = true; else port = atoi(argv[i]); }
  std::string root = argv[0];
  root = root.substr(0, root.rfind('/') + 1) + "../web/";

  std::string self = awox::selfTest();
  if (!self.empty()) { std::printf("protocol self-test failed: %s\n", self.c_str()); return 1; }
  Model model;
  Sim sim;
  std::string warnings, err = model.loadLamps(readFile(root + "../../lamps.json"), &warnings);
  if (!err.empty()) { std::printf("lamps.json: %s\n", err.c_str()); return 1; }
  if (!warnings.empty()) std::printf("lamps.json warnings: %s\n", warnings.c_str());

  int server = socket(AF_INET, SOCK_STREAM, 0), yes = 1;
  setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
  sockaddr_in addr{};
  addr.sin_family = AF_INET; addr.sin_port = htons(port);
  addr.sin_addr.s_addr = lan ? INADDR_ANY : htonl(INADDR_LOOPBACK);
  if (bind(server, (sockaddr*)&addr, sizeof addr) || listen(server, 8)) { perror("bind"); return 1; }
  std::printf("Serving on %s:%d (pages from %s)\n", lan ? "0.0.0.0" : "127.0.0.1", port, root.c_str());
  std::fflush(stdout);

  uint32_t lastCommand = 0;
  for (;;) {
    // Drain the queue like the bridge: one command per 350 ms; a simulated status report follows.
    Command c;
    if (now() - lastCommand >= 350 && model.popCommand(c)) {
      lastCommand = now();
      std::printf("send %-11s to 0x%04X: %d %d\n", kindName(c.kind), c.dest, c.a, c.b);
      if (c.kind == Kind::Power) {
        if (c.dest == awox::BROADCAST) for (auto& l : model.lamps) model.statusReport(l.address, c.a, now());
        else model.statusReport(c.dest, c.a, now());
      }
      std::fflush(stdout);
    }
    fd_set set; FD_ZERO(&set); FD_SET(server, &set);
    timeval tv{0, 50000};
    if (select(server + 1, &set, nullptr, nullptr, &tv) <= 0) continue;
    int client = accept(server, nullptr, nullptr);
    if (client < 0) continue;
    timeval rt{2, 0};
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &rt, sizeof rt);
    std::string raw;
    char buf[4096];
    size_t headerEnd = std::string::npos, length = 0;
    for (;;) {
      ssize_t n = recv(client, buf, sizeof buf, 0);
      if (n <= 0) break;
      raw.append(buf, n);
      if (headerEnd == std::string::npos && (headerEnd = raw.find("\r\n\r\n")) != std::string::npos) {
        auto cl = raw.find("Content-Length:");
        if (cl == std::string::npos) cl = raw.find("content-length:");
        if (cl != std::string::npos && cl < headerEnd) length = strtoul(raw.c_str() + cl + 15, nullptr, 10);
      }
      if (headerEnd != std::string::npos && raw.size() >= headerEnd + 4 + length) break;
    }
    api::Request req;
    std::stringstream first(raw.substr(0, raw.find("\r\n")));
    std::string target;
    first >> req.method >> target;
    req.path = target.substr(0, target.find('?'));
    if (target.find('?') != std::string::npos) parseForm(target.substr(target.find('?') + 1), req.params);
    if (headerEnd != std::string::npos) req.body = raw.substr(headerEnd + 4);
    bool form = raw.find("application/x-www-form-urlencoded") != std::string::npos;
    if (form) parseForm(req.body, req.params);

    int status = 200;
    std::string body, type = "text/plain; charset=utf-8";
    if (req.path.rfind("/api/", 0) == 0) {
      api::Response res = api::handle(model, sim, req);
      status = res.status; body = res.body;
      if (req.method == "POST") std::printf("%s %s -> %d %s\n", req.method.c_str(), req.path.c_str(), status, status == 200 ? "" : body.c_str());
    } else {
      std::string file = req.path == "/" ? "index.html" : req.path == "/k" ? "kindle.html" : req.path == "/setup" ? "setup.html" : "";
      body = file.empty() ? "" : readFile(root + file);
      if (body.empty()) { status = 404; body = "not found"; } else type = "text/html; charset=utf-8";
    }
    std::string head = "HTTP/1.1 " + std::to_string(status) + " X\r\nContent-Type: " + type +
                       "\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    std::string all = head + body;
    send(client, all.data(), all.size(), 0);
    close(client);
    std::fflush(stdout);
  }
}
