/* Checks that the current X display maps a new top-level window.
 *
 * Used by scripts/safe-gui-test.sh before any GUI/engine test: when the
 * desktop's window manager does not map windows, GUI tests cannot run and the
 * runner stops instead of working around it. Plain Xlib, no GL/EGL.
 * Exit status: 0 mapped, 1 no display, 2 not mapped within the timeout. */
#include <X11/Xlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/time.h>

static double now_ms(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

int main(int argc, char **argv) {
  const double timeout_ms = argc > 1 ? atof(argv[1]) : 5000.0;
  Display *display = XOpenDisplay(NULL);
  if (!display) {
    fprintf(stderr, "x11-map-probe: cannot open display\n");
    return 1;
  }
  Window window = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, 64, 64, 0, 0, 0);
  XStoreName(display, window, "DaliNira GUI test map probe");
  XSelectInput(display, window, StructureNotifyMask);
  const double start = now_ms();
  XMapWindow(display, window);
  XFlush(display);
  int mapped = 0;
  while (!mapped && now_ms() - start < timeout_ms) {
    while (XPending(display)) {
      XEvent event;
      XNextEvent(display, &event);
      if (event.type == MapNotify) mapped = 1;
    }
    if (!mapped) {
      struct timeval pause = {0, 10000};
      select(0, NULL, NULL, NULL, &pause);
    }
  }
  if (mapped) printf("x11-map-probe: window mapped after %.0f ms\n", now_ms() - start);
  else fprintf(stderr, "x11-map-probe: window was not mapped within %.0f ms\n", timeout_ms);
  XDestroyWindow(display, window);
  XCloseDisplay(display);
  return mapped ? 0 : 2;
}
