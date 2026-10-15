# DaliNira M3-B5: final validation report

2026-10-15, Europe/Istanbul. Validated on top of HEAD `270323fd`; committed as the M3-B5 commit. Not pushed. M3-B6 not started.

**Verdict: M3-B5 PASS.** The real Gecko New Tab, the X11 keyboard-focus fix and the Ctrl+W document-race fix pass the automated suites, and both real-X11 GUI tests pass. The limitations below still apply.

## 1. What M3-B5 delivers

- **Native Gecko New Tab.** `about:dalinira-newtab` runs as an unprivileged content document. Authority is bound per tab and per document, actions are validated, and bundles are immutable. Details are in the earlier M3-B5 report (local evidence: `scratch/m3-b5-validation-20261015-184319/M3-B5-REPORT.md`); none of it changed in this phase.
- **X11 keyboard-focus fix.**
  - Cause: Qt (6.12) never registers the embedded `QXcbForeignWindow` as an xcb event listener. When X focus moved into Gecko, Qt saw only the top-level's `FocusOut(NotifyInferior)` and deactivated the window 100 ms later. After that, `setFocus()` on the address bar emitted no focus change, so the page kept the keyboard.
  - Fix: `GeckoX11Embedder` keeps exactly that FocusOut from Qt. It must be on the owner top-level, with detail Inferior, and the X server must report focus on the embedded window or inside it.
  - `GeckoPresentationSurface::focusPage` acts only for the view that holds Qt's focus now, so late activation replies cannot take the keyboard.
- **Ctrl+W document-race fix.**
  - Cause, from the timed X11 trace: Ctrl+T creates the tab on about:blank, then swaps in the New Tab document. A Ctrl+W landing after that swap but before Qt observed it named the old document. The host correctly refused it as `stale-document`, and `requestClose` dropped the user's close.
  - Fix: `GeckoSessionView::documentChanged`. The plain close is retried **once**, only for the document the host named and only after Qt observes it. It is the same guarded request: the host still enforces beforeunload on that exact document, and a discard confirmation binds to it.
  - Still denied without a retry: any other document, nothing observed within 2 s, a second refusal, a lost or replaced view, or a generation change. The host-side check is unchanged.

## 2. Real-display (X11) results

All runs had explicit authorization. Each script ran exactly once; the test was never rerun automatically after a failure.

| Time | Test | Result |
|---|---|---|
| 19:46 | browser-gecko-newtab-x11 (before the fix) | FAIL: address-bar focus regression (the original report) |
| 20:20 | browser-gecko-newtab-x11 | **PASS** (6.8 s) |
| 20:21 | browser-gecko-x11-embedding | FAIL at startup: retest script made the IPC socket path 106 > 103 bytes. Fixed with a short temp template |
| 20:26 | browser-gecko-x11-embedding | FAIL at close: stale M3-B4 assertion (host survives window close). Updated to M3-B4.1 |
| 20:30 | browser-gecko-x11-embedding | FAIL at Ctrl+W: intermittent; trace added |
| 20:37 | browser-gecko-x11-embedding | FAIL at Ctrl+W: trace proved the stale-document race. Product fix |
| 20:49 | browser-gecko-x11-embedding | FAIL at shutdown: assertion stricter than the M3-B4.1 contract. Corrected |
| **20:54** | **browser-gecko-x11-embedding** | **PASS (6.46 s)** |

### Final embedding run (20:54): checks passed on the real display

- **Embedding:** the host window is inside the Qt top-level, viewable, unmanaged, with the right geometry. The surface follows the selected tab, and cross-site pages run in separate sandboxed processes (Seccomp 2, NoNewPrivs 1). The surface is hidden under DaliNira pages.
- **Keyboard:** the page and the address bar take the X11 keyboard in turn. After a 400 ms dwell past Qt's focus-in delay, the window is still active and the view still focused.
- **Shortcuts from the focused page:** Ctrl+Tab (the page keeps the keyboard), Ctrl+L (address bar plus Qt X focus), Settings and tab switches. **Ctrl+T opens exactly one tab, and Ctrl+W closes exactly that tab.** No stale-close trace was printed.
- **Page keys:** typing, Ctrl+C and **Escape** reach the page; Escape triggers no browser action and the page keeps the keyboard.
- **No NVIDIA device** in the host process tree.
- **M3-B4.1 shutdown:**
  - the window stays open right after `close()`;
  - it closes only once the guarded shutdown is acknowledged;
  - the session is `Stopping` (or the host has already exited);
  - the host stops within 15 s;
  - every recorded host and content PID exits.

**Natural termination.** These process checks ran inside the test before it called `EngineBackend::shutdown()` and exited normally. The test's temp directory was removed by its normal exit. `safe-gui-test` reported no surviving processes and did no forced cleanup; it terminates process trees only on failure or timeout, and neither happened. The monitor's last sample shows zero test PSS.

**Resources (final run):** peak PSS 736 MB, minimum available memory 9,484 MB, peak GPU 1.6% on i915, NVIDIA not opened, PSI memory 0%, no systemd-coredump. No kernel log lines, no coredumps.

**New Tab X11 (20:20):**
- New Tab DOM/assets with unprivileged content; malformed, traversal and stale bundles rejected.
- Address bar and page take the keyboard in turn, after the dwell and via Ctrl+L typed into the page.
- Custom-provider search, speed dial, history, reload, appearance and unsupported-provider handling.
- Two tab switches with activation acknowledgements; guarded window shutdown.
- Sandbox level 6, Fission, WebRender, hwCompositing available (Mesa Intel UHD 630, iris).

## 3. Automated validation (headless, no display)

All runs used private HOME/XDG/TMP, an isolated D-Bus, core limit 0, an RSS budget, a GPU file-descriptor watch and an inotify watch on the GRE.

| Suite | Result | Evidence |
|---|---|---|
| Adapter, including the X11 focus-handoff regression | PASS (10) | s-adapter-204451 |
| BrowserWindow, shortcuts and M3-B4.1 (all fail-closed faults; new stale-close retry test) | PASS (32) | f-window-205133 |
| Real headless New Tab, including Ctrl+T/Ctrl+W bound and loading, and window-close shutdown | PASS (18) | f-newtab-205114 |
| New Tab security (CSP, actor, principal, replay, foreign/stale rejection) | PASS (20) | f-security-205123 |
| Real host (sandbox/Fission invariants) | PASS (9) | s-host-204528 |
| Chromium/WebKit, Gecko ON (bubblewrap, no GPU nodes) | PASS 10/10 | regressions-s-on-204530 |
| Chromium/WebKit, Gecko OFF | PASS 10/10 | regressions-s-off-204536 |
| Negative control: stale-close retry disabled | FAILS as expected (line 753) | window-stale-negative-* |
| GUI gate without authorization | SKIP 77 for both GUI tests | — |

**Builds.** The Gecko ON application, the Gecko tests and the ten regression targets build. The Gecko OFF application and regression targets build, compiling no Gecko engine or controller sources. Logs: build-on*.log, build-off*.log, build-stale-close.log.

**Security and safety checks unchanged:**
- M3-B4.1 fail-closed lifecycle: requestClose, guarded shutdown, quit denial, presentation guards.
- M3-B5 internal-page authority, document identity and action validation.
- IPC authentication and protocol v3, URL restrictions, and the host's `stale-document` checks.
- No X key grabs and no input injection into the desktop.

## 4. Integrity

- **GRE runtime:** 9,243 entries and the six selected SHA-256 hashes are unchanged (runtime-final.json, verified after every run).
- **Gecko source:** HEAD `2ef09bbf`, clean. dng-poc untouched.
- **DaliNira:** HEAD `270323fd`, staging area empty, `git diff --check` passes.
- **Profiles and processes:** no real user profile was used, and no unrelated process was signalled.

## 5. Changed files (37, uncommitted)

```
.vscode/c_cpp_properties.json
.vscode/settings.json
CMakeLists.txt
browser/native/browser_window.cpp
browser/native/core/browser_profile_service.cpp
browser/native/core/browser_profile_service.h
browser/native/engine/gecko/gecko_engine_view.cpp        (+ Ctrl+W stale-close retry)
browser/native/engine/gecko/gecko_engine_view.h
browser/native/engine/gecko/gecko_host_process.cpp
browser/native/engine/gecko/gecko_ipc_server.cpp
browser/native/engine/gecko/gecko_ipc_server.h
browser/native/engine/gecko/gecko_presentation_surface.cpp  (focus phase)
browser/native/engine/gecko/gecko_runtime.cpp
browser/native/engine/gecko/gecko_session.cpp            (+ documentChanged)
browser/native/engine/gecko/gecko_session.h              (+ documentChanged)
browser/native/engine/gecko/gecko_x11_embedder.cpp       (focus phase)
browser/native/engine/gecko/gecko_x11_embedder.h         (focus phase)
browser/native/newtab/gecko_new_tab_controller.cpp       (new)
browser/native/newtab/gecko_new_tab_controller.h         (new)
browser/native/newtab/new_tab_html.cpp
browser/native/newtab/new_tab_html.h
browser/native/tests/gecko_adapter_test.cpp              (+ focus regression)
browser/native/tests/gecko_browser_window_test.cpp       (+ X11 focus gate, Ctrl+W, stale-close, shutdown contract)
browser/native/tests/gecko_x11_embedding_test.cpp        (focus phase: dwell, Ctrl+W, Escape, trace, socket path, M3-B4.1 close)
browser/resources/gecko-host/chrome/content/DaliNiraNewTabChild.sys.mjs   (new)
browser/resources/gecko-host/chrome/content/DaliNiraNewTabParent.sys.mjs  (new)
browser/resources/gecko-host/chrome/content/NewTabAbout.sys.mjs           (new)
browser/resources/gecko-host/chrome/content/host.js
browser/resources/gecko-host/chrome/content/host.xhtml
browser/resources/gecko-host/chrome/content/ipc.js
browser/resources/gecko-host/chrome/content/newtab-bootstrap.js           (new)
browser/resources/gecko-host/chrome/content/newtab-presentation.js        (new)
browser/resources/gecko-host/chrome/content/newtab.js                     (new)
browser/resources/gecko-host/chrome/content/tabs.js
tools/gecko_newtab_gui_fixture.py                        (new)
tools/package_gecko_host.py                              (new)
tools/test_gecko_newtab_security.py                      (new)
```

## 6. Remaining limitations

1. **New Tab X11 test not rerun after the later changes.** browser-gecko-newtab-x11 passed at 20:20, before the Ctrl+W product fix and before the stricter shutdown assertion in its shared code. Those paths pass headless against real Gecko and passed in the final X11 embedding run, but that test itself was not rerun on the display.
2. **Synthetic keyboard input.** Keys were typed through Gecko's own test input path (nsITextInputProcessor), not a physical keyboard. Physical mouse and assistive-technology activation are not proven.
3. **Black screenshots.** XWayland window grabs are black, so screenshots do not show visual parity. Hardware WebRender was reported by Gecko itself (Mesa iris).
4. **Stale-close retry is narrow by design.** It covers one unobserved document change within 2 s. Longer or repeated changes still deny the close, which is fail-closed; the user can press Ctrl+W again.
5. **The fix depends on Qt behaviour.** It relies on Qt xcb's FocusOut handling (checked against the Qt 6.12.0 source). A Qt that starts registering foreign windows would make the filter a no-op, not a fault, because the predicate requires focus inside the embedded window.
6. **Limitations carried over from the M3-B5 report:**
   - Not enabled: managed-background import, favicons, autocomplete.
   - Bing/Brave/unknown providers are rejected.
   - One content-process crash can affect the tabs sharing it.
   - The about origin is shared; per-tab authority comes from the native WindowGlobal checks.
   - No compromised-renderer claim. 32-tab capacity was not measured.
7. **Platform scope.** Experimental engine: X11/XWayland only, one window, `--engine gecko` only.

## 7. Non-blocking follow-up items (final review)

None of these affects security, the M3-B4.1 lifecycle or the validated behaviour. They are left for later work.

1. **Partial-file cleanup.** If a write or `fsync` fails, `GeckoSession::publishNewTab` leaves the partially written private bundle file behind. The host already rejects such a file (strict UTF-8/JSON/schema/SHA-256 checks), and the file stays inside the private 0600 run directory, which is removed at session exit. It should still be unlinked immediately on failure.
2. **Reproducible `omni.ja` timestamps.** `tools/package_gecko_host.py` writes each source file's mtime into the archive. Entry order is sorted, but the archive is not byte-for-byte reproducible across checkouts. Fixed entry timestamps (for example `SOURCE_DATE_EPOCH`) would make it reproducible.
3. **Code readability.** Much of the M3-B5 code (`gecko_new_tab_controller.cpp`, parts of `gecko_engine_view.cpp`, `gecko_session.cpp`, `gecko_ipc_server.cpp`, `newtab.js`, the actor modules and the `--real-newtab` test path) is written as dense one-line statements, unlike the surrounding Gecko code. Reformatting it to the project's normal style, without changing behaviour, is recommended before further feature work.

## Evidence index

The evidence is kept in the local, git-ignored workspace directory `scratch/m3-b5-focus-fix-20261015/` (and `scratch/m3-b5-validation-20261015-184319/` for the earlier M3-B5 runs). It is not part of the repository.


- Focus-fix and diagnosis log: `M3-B5-FOCUS-FIX-REPORT.md`
- X11 runs: `approved-run-20261015-202049.log`, `approved-embedding-run*-*.log`, `gui-authorized-run/` (CTest log, monitor CSV/summary, host logs, runtime.json)
- Headless runs: `s-*`, `f-*`, `regressions-s-*`, `window-stale*`, `newtab-ctrlw*` (each with output.log and result.json)
- Prepared GUI scripts: `run-x11-retest-on-approval.sh`, `run-x11-embedding-retest-on-approval.sh`
