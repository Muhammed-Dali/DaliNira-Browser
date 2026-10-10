# WebKitGTK 2.54.1 web-process crash: historical record

During DaliNira 8.0.0 validation (2026-10-06 to 2026-10-08), a
WebKitWebProcess occasionally aborted while WebKit tabs and windows closed.
That was about 2 % of window lifetimes in a multi-tab flow. The crash was in
WebKitGTK itself:

- `AcceleratedSurface::willDestroyCompositingRunLoop()` sent the reset of
  `m_releaseUnusedBuffersTimer` to the compositing run loop as a task.
- The ThreadedCompositor stopped that run loop right afterwards, and the
  pending task was dropped.
- The surface, with its still-active RunLoop timer, was then destroyed on the
  main run loop, and `~TimerBase()` aborted.
- Pages that become hidden while open (background tabs) arm that timer.

The fix destroys the timer synchronously on its own run loop. It is
`upstream-AcceleratedSurface-sync-timer-destroy.patch` here. DaliNira ships
it in its private WebKitGTK runtime as
`packaging/webkit-runtime/patches/0001-*` (see
`packaging/webkit-runtime/README.md`).

## Validation (final evidence, not to be repeated)

| Build | Window lifetimes | Web-process crashes |
|---|---|---|
| WebKitGTK 2.54.1, unpatched (control) | 40 | **3** |
| WebKitGTK 2.54.1, patched (local build) | 500, with 2,000 web processes | **0** |
| DaliNira's packaged private runtime (staged install) | 500, with 2,000 web processes | **0** |

- With the patch there was no deadlock or hang: every run ended on its own
  within its time limit.
- In an interleaved A/B, the patch caused no CPU, GPU or heap-memory
  regression.
- In the packaged-runtime run, `/proc/<pid>/maps` showed that every web
  process ran on the private patched libraries.

This is complete. The stress reproducer and the bisection tools used to get
here were deleted on 2026-10-08, and the validation is not part of DaliNira
development or release testing. Releases rely on the normal test tiers. The
short functional check for this lifecycle is the GUI test
`browser-webkit-lifecycle`: tabs, background tab, sleep/wake, a second
window, window close and shutdown, with no WebKit process lost or left
behind.

## Other findings from the investigation

- **WebKitGTK 2.54.1 `<video>` fullscreen (upstream, not fixed):** a video
  that is itself the fullscreen element stops advancing until fullscreen
  ends. Plain WebKitGTK shows the same on X11 and Wayland. Container
  fullscreen (YouTube) and windowed playback are not affected. It is a
  documented known issue in the 8.0.0 release notes, and the parity test
  records it as `[KNOWN]`.
- **WebKitGTK 2.52.6 exit-time heap corruption:** most likely upstream bug
  305909, fixed in 2.54.1.
- **DaliNira fixes made along the way:**
  - step5 test synchronization;
  - stale WebKit favicon;
  - bookmark-bar widget leak;
  - Chromium tabs' loading state;
  - favicon parity between the engines.
- **Removed manual case:** several bare WebKit views shown as separate
  top-level windows at once caused an endless XEmbed focus exchange, and
  twice the desktop stopped responding. That test case was removed. See
  `docs/TESTING.md`.

## Files

| File | Purpose |
|---|---|
| `upstream-AcceleratedSurface-sync-timer-destroy.patch` | The fix, as validated (applies to 2.54.1 and main). |
| `UPSTREAM-REPORT-DRAFT.md` | Draft upstream reports, for the crash and the `<video>` fullscreen freeze. Not filed yet. |
