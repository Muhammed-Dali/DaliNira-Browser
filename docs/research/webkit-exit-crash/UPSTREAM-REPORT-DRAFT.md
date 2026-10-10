# [GTK] Web process aborts in ~TimerBase(): AcceleratedSurface destroyed on the main run loop with m_releaseUnusedBuffersTimer still active (2.54.1)

**Component:** WebKitGTK · **Version:** 2.54.1 (also expected on main; the code path is unchanged) · **Platform:** Linux, Intel i915/Mesa
**Status:** draft, ready to file. A fix candidate is attached and was validated against the reproducer.

## Summary

A WebKitWebProcess intermittently aborts on its main thread while the UI
process closes tabs and windows:

```
abort()
<WTF crash>                                       libjavascriptcoregtk-4.1 +0x536649
WTF::RunLoop::TimerBase::~TimerBase()              libjavascriptcoregtk-4.1
WTF::RunLoop::Timer::~Timer()                      libwebkit2gtk-4.1 +0x1438c8b
WebKit::AcceleratedSurface::~AcceleratedSurface()  libwebkit2gtk-4.1 +0x1ce335a / +0x1ce35de
CallableWrapper<ThreadSafeWeakPtrControlBlock::strongDeref<WebKit::AcceleratedSurface,
                (DestructionThread)2>()::lambda>::call()   libwebkit2gtk-4.1 +0x1ce373a
<RunLoop dispatch>                                 libjavascriptcoregtk-4.1 +0x25bc86f / +0x25bcb1f
g_main_context_dispatch                            libglib-2.0
WTF::RunLoop::run()
WebKit::WebProcessMain(int, char**)
```

The stack is identical in all three captured crashes. Nothing is printed to
stderr. Frame names come from the `.symtab` sections served by
debuginfod.archlinux.org: libwebkit2gtk-4.1 build-id
`edba0f0fa640969c2b0152af5cd0639025df170e`, libjavascriptcoregtk-4.1
`fa26862a96388babc6b3bac9ebddaa203c400b4d` (Arch webkit2gtk-4.1 2.54.1-1).

## Analysis: a task lost when the compositing run loop stops

The assertion added in 316457@main (bug 318088, "Assert RunLoop::Timer is
stopped and destroyed on its own run loop's thread") exposes this race.
`willDestroyCompositingRunLoop()` is not skipped. It is always called, but it
only *dispatches* the timer destruction. The compositing thread can exit
before that task runs, and `RunLoop` then drops the task without running it.

Lifecycle in webkitglib/2.54 (`AcceleratedSurface.cpp`, `ThreadedCompositor.cpp`;
the same on main):

1. `ThreadedCompositor` creates its `WorkQueue("org.webkit.ThreadedCompositor")`
   and calls `AcceleratedSurface::didCreateCompositingRunLoop(runLoop)`. That
   creates `m_releaseUnusedBuffersTimer` bound to the **compositing** run loop.
2. The page is hidden (background tab, or the window is closing).
   `ThreadedCompositor::suspend()` → `AcceleratedSurface::visibilityDidChange(false)`
   on the main thread dispatches `startOneShot(10_s)` to the compositing run
   loop. The task runs and the **timer is now active** for 10 s.
3. The page is destroyed within those 10 s. `LayerTreeHost::~LayerTreeHost()`
   → `ThreadedCompositor::invalidate()` (main thread): `dispatchSync(...)`, then
   `m_surface->willDestroyCompositingRunLoop()`. That clears
   `m_compositingRunLoop` and **dispatches**
   `[protectedThis = Ref { *this }] { protectedThis->m_releaseUnusedBuffersTimer = nullptr; }`.
   Then `m_surface = nullptr`, so this lambda now holds the last reference.
4. The `ThreadedCompositor` (and with it the `WorkQueue`) is destroyed right
   after. `WorkQueue::platformInvalidate()` → `RunLoop::stop()`. In
   `RunLoopGLib`, `stop()` sets `m_shouldStop`, and the thread leaves
   `while (!m_shouldStop)` without necessarily performing work that was
   dispatched but not yet taken. `RunLoop::dispatch()`, unlike
   `WorkQueue::dispatch()`, does not keep the queue alive.
5. When the thread exits, `RunLoop::Holder::~Holder()` → `threadWillExit()`
   clears `m_currentIteration` / `m_nextIteration`. The pending lambda is
   **destroyed without running**, and its `protectedThis` drops the last
   reference on the compositing thread.
6. `AcceleratedSurface` is `ThreadSafeRefCountedAndCanMakeThreadSafeWeakPtr<…,
   WTF::DestructionThread::MainRunLoop>`. `strongDeref()` therefore schedules
   `delete` on the main run loop. That is the
   `strongDeref<AcceleratedSurface, (DestructionThread)2>` frame.
7. `~AcceleratedSurface()` (`= default`) on the **main** thread destroys
   `m_releaseUnusedBuffersTimer`. The timer is still active and is not on its
   own run loop, so `~TimerBase()` hits the assertion and aborts.

Why it is intermittent:
- If the page was never hidden, or the 10 s timer already fired, the timer
  is inactive. Destroying it on main is then allowed, and the same lost task
  is harmless.
- If the compositing thread performs the task before it sees `m_shouldStop`,
  the timer is destroyed on its own thread, and nothing goes wrong.

Runtime evidence (normal environment, 4 crashes in 180 window lifetimes,
each with a SIGABRT handler listing `/proc/self/task` at the abort):
- The stack is the one above every time. The `strongDeref<…, MainRunLoop>`
  lambda shows that the last reference was released off the main thread. If
  the nulling task had run, the timer would be null and there would be no
  assertion. So the dispatched task was destroyed unrun.
- 3/4: no `ThreadedCompositor` thread existed any more at the abort.
- 1/4: two `ThreadedCompositor` threads were still alive. That is consistent
  with other pages in the same web process, but thread names cannot tie them
  to the destroyed surface.

`NonCompositedFrameRenderer` uses the same API with
`RunLoop::mainSingleton()` as the "compositing" run loop. There,
`willDestroyCompositingRunLoop()` is called on that run loop itself.

## Proposed fix

Destroy the timer **synchronously** on its run loop in
`willDestroyCompositingRunLoop()`. Use a direct reset when already on that
run loop (the `NonCompositedFrameRenderer` case: blocking there would
deadlock). This mirrors the `dispatchSync` that `ThreadedCompositor::invalidate()`
already does immediately before the call, so it adds no new blocking pattern.
`BinarySemaphore` instead of `RunLoop::dispatchSync`-style helpers keeps the
patch local to the surface:

```diff
--- a/Source/WebKit/WebProcess/WebPage/CoordinatedGraphics/AcceleratedSurface.cpp
+++ b/Source/WebKit/WebProcess/WebPage/CoordinatedGraphics/AcceleratedSurface.cpp
@@ -44,6 +44,7 @@
 #include <wtf/SafeStrerror.h>
 #include <wtf/SystemTracing.h>
 #include <wtf/TZoneMallocInlines.h>
+#include <wtf/threads/BinarySemaphore.h>
@@ willDestroyCompositingRunLoop()
     // m_releaseUnusedBuffersTimer is owned by the compositing run loop and its destructor stops it, so
-    // destroy it on that run loop's thread rather than here on the main thread.
+    // destroy it on that run loop's thread rather than here on the main thread. Do it synchronously:
+    // the compositing run loop is stopped right after this (its WorkQueue goes away with the
+    // ThreadedCompositor), and a task dispatched to it may then never run; it is dropped when the
+    // thread exits, the surface is destroyed on the main run loop and the still-active timer with it.
     if (RefPtr compositingRunLoop = std::exchange(m_compositingRunLoop, nullptr)) {
-        compositingRunLoop->dispatch([protectedThis = Ref { *this }] {
-            protectedThis->m_releaseUnusedBuffersTimer = nullptr;
-        });
+        if (compositingRunLoop->isCurrent())
+            m_releaseUnusedBuffersTimer = nullptr;
+        else {
+            BinarySemaphore semaphore;
+            compositingRunLoop->dispatch([this, &semaphore] {
+                m_releaseUnusedBuffersTimer = nullptr;
+                semaphore.signal();
+            });
+            semaphore.wait();
+        }
     }
```

The full patch is `upstream-AcceleratedSurface-sync-timer-destroy.patch`. It
applies cleanly to the webkitgtk-2.54.1 tag and to main (offset 4 lines).
It was tested against a local build; the system WebKitGTK was not
modified (see *Patch validation* below). Alternatives would be to reset the timer inside
`ThreadedCompositor::invalidate()`'s existing `dispatchSync` block, or to
drain the run loop in `threadWillExit()`. The first spreads surface
internals into the compositor. The second changes `RunLoop` semantics for
every client.

## Patch validation (2026-10-08)

**Status: viable fix candidate.**

**Build:**
- webkitgtk-2.54.1 release tarball (sha256 `ea0bbb02…397d`).
- Arch's CMake options and compiler flags: clang 23, lld, `-O2`,
  `_FORTIFY_SOURCE=2`, `-fcf-protection=none`.
- Differences from Arch: no `-g`, and no documentation, introspection or
  MiniBrowser.
- Installed to a private prefix and run through `LD_LIBRARY_PATH`. The UI,
  network and web processes were checked to map only the local libraries.

**Same-build control:** the unpatched build gives the same abort, with the
identical `~Timer` ← `~AcceleratedSurface` ←
`strongDeref<…, MainRunLoop>` stack. It hit **3 crashes in 40** window
lifetimes, and in all 3 no ThreadedCompositor thread was left.

**Patched, same reproducer, normal environment:**

| Check | Result |
|---|---|
| Crash reproducer | **0 crashes in 500** window lifetimes (2000 web processes) |
| Hangs / deadlock | none; no run hit the hard timeout, no web process outlived its run, and each 20-lifetime run took 65-69 s (unpatched local: 70-71 s) |
| Embedder WebKit GUI tests | pass: browsing and step5 flows, tab open/close, background tabs and tab sleep, popups, layout, scripts, site data, media, engine switch, shutdown |
| `<video>` element fullscreen | unchanged: the separate 2.54.1 freeze, same as unpatched |
| Container fullscreen playback (plain WebKitGTK) | plays (0 of 7 checks stalled) |
| CPU, interleaved A/B, 6 pairs | windowed 31.9 % → 32.1 %, web process 20.3 % → 20.6 %, fullscreen 32.4 % → 32.3 % (unpatched → patched, % of one core) |
| GPU render engine | 7.3 % windowed / 17.1 % fullscreen, both builds |
| rAF | 62 fps, both builds |
| Heap/anon PSS | equal |

Sample-size estimate: if the patch did nothing, 0 crashes in 500 would be
very unlikely. At the lowest rate measured (≈2 %) the chance is
e^-10 ≈ 5·10⁻⁵, and at the same-build control rate (7.5 %) it is far
lower.

Smaller notes:
- The `libwebkit2gtk` file mapping is about 3 MB higher in the patched
  web process. That is resident library text, a layout effect; heap and
  anonymous memory are equal.
- Earlier, non-interleaved CPU samples swung between ~20 % and ~40 % in
  both builds because of desktop load.

This fits what we measured:
- Crashes need pages that become **hidden while open** (background tabs):
  7/300 window lifetimes with a multi-tab flow vs 0/300 with a single tab.
- **Accelerated compositing off** (`WEBKIT_HARDWARE_ACCELERATION_POLICY_NEVER`)
  means no `AcceleratedSurface`, and gave **0/260** against **7/240**
  interleaved with the default policy.
- CPU painting with GPU compositing (`WEBKIT_SKIA_ENABLE_CPU_RENDERING=1`)
  still crashes, **6/260**.

## Frequency

About 2-3% of window lifetimes in the reproducer flow:
- 3/120 and 3/140 in the normal environment;
- 7/240 in the interleaved policy comparison;
- 2/120 with `libc_malloc_debug` and `malloc.check=3`.

Each window lifetime: a custom-scheme page (`dalinira://newtab/`,
`webkit_web_context_register_uri_scheme`) → an http page with a favicon → an
http page without one → back to the custom page → the same page opened in
two background views that are closed again → a link followed on the custom
page → the toplevel destroyed.

## Environment

| Component | Version |
|---|---|
| WebKitGTK | 2.54.1 (webkit2gtk-4.1 2.54.1-1) |
| GTK / GLib / libsoup | 3.24.52 / 2.90.1 / 3.8.0 |
| GStreamer | 1.28.7 |
| Mesa | 26.2.4 (iris), Intel UHD 630 (CoffeeLake-H GT2, 8086:3e9b), i915, Linux 7.2.9-1-cachyos |
| OS | CachyOS (Arch-based); KDE Plasma 6 Wayland (KWin 6.7.5), Xwayland 24.1.13 |
| Second GPU | NVIDIA GTX 1660 Ti Mobile. Not involved: the web processes open only the i915 render node, and the crash also occurs with a Mesa-only EGL vendor. |

**Display:** the embedder (a Qt 6 browser, DaliNira) runs WebKitGTK with
`GDK_BACKEND=x11` on Xwayland, so all crash observations are X11. The
WebKitWebView is in a GtkPlug. On 2.52.6 the embedder's earlier crash also
reproduced with a `GtkOffscreenWindow` instead; that was not repeated on
2.54.1. Native-Wayland GTK was not tried for this crash.

**Embedder code:** the embedder's web extension holds only a network filter.
On 2.52.6 the same flow still crashed with **no web extension loaded**
(3/20). On 2.54.1 the extension was loaded during these runs.

## Reproducer status

- **Reliable:** through the embedder (DaliNira's step5 GUI test loop).
- **Plain PyGObject WebKitGTK:** a similar flow (custom scheme, favicon
  database, ITP, two views, toplevel destroyed 150 ms after a second view
  starts) has **not** reproduced it (0/120). The race window seems to depend
  on main-thread timing that the embedder's Qt main loop provides.
- Happy to test patches: we build the embedder against system WebKitGTK and
  can rebuild WebKitGTK from source.

## Related

- Bug 318088 / 316457@main added the assertion.
- On WebKitGTK 2.52.6 the same embedder flow showed glibc heap corruption at
  exit instead (`corrupted double-linked list`, or SIGSEGV in `unlink_chunk`,
  14/40). That matches bug 305909 (crash on exit in
  `PlatformDisplay::terminateEGLDisplay`), whose fix (321775@main) was
  cherry-picked to webkitglib/2.54 and ships in 2.54.1. That signature is
  gone in 2.54.1.

---

# Second report: [GTK][GStreamer] video stops advancing while a `<video>` element is fullscreen (2.54.1 regression)

**Status:** draft, ready to file. Reproduces with plain WebKitGTK and no
embedder code (a short PyGObject page with the steps below; DaliNira's
throw-away script for it was not kept).

**Steps:** a `<video muted autoplay>` playing a local 4 s VP9/Opus WebM (with
an HTTP Range server), then `video.requestFullscreen()` from a page button.

**Result:** within 1.5 s `currentTime` stops advancing, with `paused=false`
and `readyState` 2 (`HAVE_CURRENT_DATA`). After `document.exitFullscreen()`
it advances again. 3/3 rounds on X11 (GDK_BACKEND=x11) and 3/3 on native
Wayland.

**Not affected:**
- `requestFullscreen()` on a container `<div>` holding the video: 0/3;
- resizing the window to the screen size without fullscreen: 0/3.

**Still affected:** `HardwareAccelerationPolicy.NEVER` (3/3).

**Pipeline in both cases:** `vp9dec` (libvpx) → `glupload` →
`glcolorconvert` → `webkitglvideosink`; `WEBKIT_GST_DMABUF_SINK_DISABLED`
makes no difference.

**Regression:** on 2.52.6 the same page played in element fullscreen; there,
video stalled after *leaving* element fullscreen instead. Environment as
above. Possibly in the family of bug 254480.
