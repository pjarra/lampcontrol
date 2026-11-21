// sendkey: inject button presses for testing the remote over SSH.
//   sendkey <device-number> <code> [<code> ...]     e.g. sendkey 1 108 108 194  (down, down, centre)
#include <fcntl.h>
#include <linux/input.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

static void emit(int fd, int type, int code, int value) {
  struct input_event ev;
  memset(&ev, 0, sizeof ev);
  ev.type = (unsigned short)type; ev.code = (unsigned short)code; ev.value = value;
  if (write(fd, &ev, sizeof ev) != (ssize_t)sizeof ev) perror("write");
}
int main(int argc, char** argv) {
  if (argc < 3) { fprintf(stderr, "usage: sendkey <event-number> <code>...\n"); return 2; }
  char path[32];
  snprintf(path, sizeof path, "/dev/input/event%s", argv[1]);
  int fd = open(path, O_WRONLY);
  if (fd < 0) { perror(path); return 1; }
  for (int i = 2; i < argc; i++) {
    int code = atoi(argv[i]);
    emit(fd, EV_KEY, code, 1); emit(fd, EV_SYN, SYN_REPORT, 0);
    emit(fd, EV_KEY, code, 0); emit(fd, EV_SYN, SYN_REPORT, 0);
    usleep(1200000);  // let the remote redraw between presses
  }
  close(fd);
  return 0;
}
