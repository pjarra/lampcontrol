// keys: print Kindle button presses as "<code> <value>" (value 1 = press, 0 = release, 2 = repeat).
//   keys [timeout-seconds]   exits with status 1 on timeout (no output)
// Reads all /dev/input/event* devices (keypad, five-way, page/volume buttons).
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/select.h>
#include <unistd.h>

int main(int argc, char** argv) {
  int fds[8], n = 0, maxfd = -1;
  for (int i = 0; i < 8; i++) {
    char path[32];
    snprintf(path, sizeof path, "/dev/input/event%d", i);
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd >= 0) { fds[n++] = fd; if (fd > maxfd) maxfd = fd; }
  }
  if (!n) { fprintf(stderr, "no input devices\n"); return 2; }
  long timeout = argc > 1 ? atol(argv[1]) : -1;
  for (;;) {
    fd_set set; FD_ZERO(&set);
    for (int i = 0; i < n; i++) FD_SET(fds[i], &set);
    struct timeval tv = {timeout, 0};
    int r = select(maxfd + 1, &set, NULL, NULL, timeout >= 0 ? &tv : NULL);
    if (r <= 0) return 1;
    for (int i = 0; i < n; i++) {
      if (!FD_ISSET(fds[i], &set)) continue;
      struct input_event ev;
      while (read(fds[i], &ev, sizeof ev) == (ssize_t)sizeof ev) {
        if (ev.type == EV_KEY && ev.value == 1) { printf("%d\n", ev.code); fflush(stdout); return 0; }
      }
    }
  }
}
