# Testing DaliNira Browser

DaliNira uses standalone C++ regression executables, CTest, and deterministic
local fixtures. Assertions remain enabled in Release test targets. The core
suite must not depend on public internet services or a real user profile.

## Build and run

```bash
./scripts/build-dalinira.sh
ctest --test-dir build-dalinira --output-on-failure
```

The unified build always checks/builds the private WebKitGTK runtime before the
dual-engine application. Do not substitute an engine-specific build based on
which tests or source files appear to have changed.

Run a subsystem by test-name regex or label:

```bash
ctest --test-dir build-dalinira -R 'password|credential-vault' --output-on-failure
ctest --test-dir build-dalinira -R 'tab|step5' --output-on-failure
ctest --test-dir build-dalinira -L security --output-on-failure
ctest --test-dir build-dalinira -L webengine --output-on-failure
```

A test executable can also be run directly for its per-case diagnostics:

```bash
./build-dalinira/dalinira-browser-password-autofill-test
./build-dalinira/dalinira-browser-step5-test
```

Do not encode the current suite count in scripts; use `ctest --test-dir build-dalinira
-N` when an inventory is needed.

## GUI and WebKit tests

Tests are split into tiers so that the default run never stalls a developer's
desktop:

| Tier | How to run | What it does |
|---|---|---|
| Unit / integration | `ctest --test-dir build-dalinira` | Invisible (offscreen or no windows) and light. |
| GUI (label `gui`) | `scripts/safe-gui-test.sh [TEST...]` | Real windows on the desktop display, one test at a time. |
| Manual | `scripts/safe-gui-test.sh --manual TEST` | Window-manager cases: media fullscreen enter/exit. Not a release gate. |
| Live sites | `scripts/safe-gui-test.sh --live TEST` | DuckDuckGo / YouTube. |
| Memory profile | `scripts/safe-gui-test.sh --memory-profile chromium\|webkit` | The developer memory profiler. |

Every tier runs against the private patched WebKitGTK runtime that
`./scripts/build-dalinira.sh` builds. The only exception is CI's push/PR job
(`.github/workflows/ci.yml`): building WebKitGTK there on every push is not
affordable, so it configures with the test-only
`-DDALINIRA_ALLOW_SYSTEM_WEBKIT_FOR_TESTS=ON` and compiles the WebKit backend
against the system WebKitGTK. That flag is OFF by default, cannot be combined
with a package SDK, makes `cmake --install` and CPack fail, and is rejected by
`scripts/check-release-metadata.sh` in any packaging or release file. Results
from such a build say nothing about the shipped runtime.

Never show several bare WebKit views as separate top-level windows at
once. A former manual case of `browser-webkit-backend` did that with three
windows. It hung every time in an endless XEmbed focus exchange: a view in
an inactive top-level window keeps answering the GTK plug's focus request
with a grab that cannot take effect. The test process spun and grew, and
twice the desktop stopped mapping windows until a hard reset. The case was
removed on 2026-10-08. DaliNira itself never shows a WebKit view as a
top-level window. `browser-webkit-lifecycle` covers real multi-window use:
a second BrowserWindow opened as New Window does, next to the first. Both
browse, then the second is closed.

The WebKitGTK 2.54.1 crash investigation is finished. Its stress
reproducer was deleted; only the record and the validated patch remain in
`docs/research/webkit-exit-crash/`. No test tier, CI job, package check or
release step runs a stress loop.

A plain `ctest` reports GUI-tier tests as "Skipped". They run through
`scripts/gui-test-gate.sh`, which only starts them with `DALINIRA_GUI_TESTS=1`.
CI sets that variable on its virtual display; on a desktop, use
`safe-gui-test.sh`. The runner:

- refuses virtual displays and GPU override variables;
- runs one test at a time under a hard timeout, and waits for every engine
  process and any crash handling to finish before the next;
- disables core dumps. Processing one WebKit core dump took systemd-coredump
  ~5.5 GB of memory and stalled the desktop. Crashes are still logged and
  detected; `--keep-cores` keeps the dumps for debugging;
- samples CPU, PSS/private memory, WebKit process and window counts, GPU
  engine time (DRM fdinfo), memory pressure and systemd-coredump once per
  second (`scripts/gui-test-monitor.py`). It writes the samples to
  `build-dalinira/gui-test-logs/`, prints the peaks, and stops the test at a limit:
  more than one view's web processes, under 1.5 GB available memory, over
  3 GB PSS, or memory pressure above 20%. A test that legitimately opens
  several WebKit tabs declares its own budget in its ctest `ENVIRONMENT`
  (`DALINIRA_GUI_MAX_WEB_PROCESSES`, e.g. `browser-tab-memory-ux`);
- stops at the first failure, crash, kernel GPU/driver error or KWin stall
  report ("main thread was hanging").

`browser-webkit-lifecycle` is the short WebKit lifecycle regression check,
run in the GUI tier and before a release. In a few seconds, through the real
`BrowserWindow`, it covers:

- opening and closing tabs;
- a hidden background tab;
- tab sleep and wake;
- a second window, opened as New Window does, browsing next to the first;
  closing it ends its tabs' web processes;
- closing the window;
- shutting down the engine.

Any lost web process fails it, and so does a WebKit process left behind.
It covers the lifecycle where WebKitGTK 2.54.1 crashed, which the private
runtime's patch fixes (`packaging/webkit-runtime/`).

The WebKit GUI tests (`browser-webkit-backend`, `browser-webkit-media`) use
`browser/native/tests/webkit_gui_harness.h`:

- one top-level window for the whole run;
- at most one `WebKitEngineView` at a time. A destroyed view's web process
  must exit before the next case starts;
- waits sleep in the event loop instead of spinning;
- the first web process termination or failed check exits immediately, with
  no core dump.

The engine-neutral logic they depend on is in `browser-webkit-engine-logic`,
which runs in the default tier.

On a memory-limited workstation, also cap build parallelism (for example
`cmake --build build-dalinira -j3`): each C++ compile job of this project can take
1–2 GB. With LTO, or with WebKitGTK itself (`packaging/webkit-runtime`), it
can take more. On a 16 GB desktop that already uses ~7 GB, `-j8` exhausted
memory, and zram swap delayed any OOM kill until the machine froze. A
memory-capped scope fails the build instead of freezing the desktop:
`systemd-run --user --scope -p MemoryMax=6G -p MemorySwapMax=0 cmake --build build-dalinira -j3`.

## Test categories

- Core/unit: policies, URL security, address resolution, application identity,
  new-tab assets/backgrounds, download models, media routing and platform
  registries.
- Component: vaults, password UI, translation providers, blocker rules,
  Web Audio, song recognition, media downloads, performance settings and tab
  lifecycle managers.
- Integration/WebEngine: BrowserWindow commands, site permissions, menus,
  session restoration, page translation, live view transfer, fullscreen,
  omnibox behavior and the phase 22d browser bridge.
- Security: credential vault/autofill, origin canonicalization, private-mode
  boundaries, URL sanitization and encrypted translation secrets.
- Golden/script checks: DALI generation and manifest scripts are invoked by
  their build targets and repository tooling rather than being separate native
  CTest executables.

CTest labels currently provide `security`, `passwords`, `audio`, `integration`
and `webengine` entry points. A test may have more than one label.

## Registered standalone regressions

The following older standalone programs are first-class CTest suites in
addition to the original browser suites:

- `adaptive_audio_dsp_test.cpp` — adaptive audio state and persistence.
- `dalinira_blocker_test.cpp` — blocker policy, UI, rules and migrations.
- `blocker_filter_update_test.cpp` — remote filter-list updates against a
  loopback HTTP fixture: validation, ETag/Last-Modified, failures, crash
  recovery, scheduling, and one shared update applied by the Chromium
  interceptor (offscreen page) and the WebKit network filter socket.
- `blocker_filter_syntax_test.cpp` — `$removeparam`, `$redirect`/`$rewrite`
  and redirect exceptions, `$badfilter`, `$method` and the response-phase
  rules neither engine can enforce, through the shared rule model, a real
  Chromium page and the WebKit filter socket. `browser-webkit-backend` (GUI
  tier) repeats the end-to-end part in a real WebKit page.
- `internal_tab_test.cpp` — native-tab/session and private-session isolation.
- `new_tab_background_store_test.cpp` — image validation and size limits.
- `page_translator_test.cpp` and `translate_service_test.cpp` — translator
  lifecycle, provider behavior, vault-backed secrets and local fake replies.
- `performance_settings_test.cpp` — Settings performance controls and keys.
- `state_transfer_test.cpp` — live WebEngine view transfer/rollback stress.
- `tab_audible_and_fullscreen_test.cpp` — audible metadata, hover data and
  fullscreen restoration.
- `tab_manager_test.cpp`, `tab_performance_manager_test.cpp` and
  `tab_throbber_test.cpp` — ownership, cleanup and loading lifecycle.
- `yt_dlp_update_manager_test.cpp` — managed-tool updates using two local
  synthetic executables; no GitHub access is performed.

`discard_restore_harness_test.cpp` remains a specialized developer benchmark,
not a CTest. It mixes deterministic policy checks with renderer memory and
restore-latency measurements, so it is unsuitable for pass/fail CI. The
`yt_dlp_version_fixture.cpp` program and `suggestion_test_transport.h` are test
helpers, not independent suites.

The scripts in `browser/scripts/tmp-*-runtime-test.js` are manual
DevTools-protocol smoke helpers for a developer-launched new-tab page. They are
kept aligned with the current `search-suggestions` DOM contract, but are not
registered because they require an externally supplied debugging endpoint.

## Isolation and network policy

- Put profile storage, vaults, histories, downloads, sessions and QSettings in
  `QTemporaryDir` paths. Set the QSettings format/path before constructing the
  first settings-backed object.
- Use off-the-record or explicitly temporary `QWebEngineProfile` storage.
- Never read or write the normal DaliNira profile, credential vault or desktop
  settings from a test.
- Use loopback servers, fake `QNetworkAccessManager` replies and checked-in
  fixtures. URLs such as `example.com` used only as values do not perform
  network access.
- Never print credential plaintext, master passwords, device secrets, fill
  tokens, encryption keys or raw private audio in failure diagnostics.

## Headless Qt WebEngine

CTest supplies the required offscreen/X11 and Chromium flags per suite. For a
direct WebEngine test invocation, the usual local equivalent is:

```bash
QT_QPA_PLATFORM=offscreen \
QTWEBENGINE_DISABLE_SANDBOX=1 \
QTWEBENGINE_CHROMIUM_FLAGS='--no-sandbox --disable-gpu' \
./build-dalinira/dalinira-browser-step5-test
```

Disabling Chromium's sandbox is for isolated test processes in containerized
CI only; it is not a browser runtime recommendation. Some managed containers
deny namespace/process operations even with these flags. In that case run the
same CTest command on the host/CI executor rather than weakening assertions.

Offscreen widgets are not compositor-mapped. Prefer command results, model
state, signals and explicit hidden state over assuming `isVisible()` is true.
Use signals or bounded event loops for asynchronous behavior; do not add long
arbitrary sleeps.

## Optional sanitizers

GCC and Clang developer builds may opt in without changing normal Release
builds:

```bash
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DDALINIRA_ENABLE_ASAN=ON
cmake -S . -B build-ubsan -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DDALINIRA_ENABLE_UBSAN=ON
```

ASan and UBSan flags apply to DaliNira targets. Qt/Chromium libraries and
WebEngine helper processes are not rebuilt with those flags, so begin with
native unit/component suites. Treat WebEngine sanitizer execution as a
developer diagnostic, not a mandatory CI gate.

## Manual desktop-only checks

Automation does not reliably prove compositor dialogs, a physical microphone
or system-audio capture, GPU/video decode, Secret Service integration, or live
third-party services. Before a release, limit manual testing to affected gaps:

- invoke Print and Save as PDF from a real desktop dialog and confirm the
  browser remains alive until asynchronous printing completes;
- enter/exit HTML5 fullscreen and capture a visible-page screenshot under the
  desktop compositor;
- perform one real microphone/system-audio recognition attempt when audio code
  changed;
- verify Secret Service unlock/persistence when keyring integration changed.

## Adding a regression

Place a case in the closest existing suite and name the broken contract. Drive
the real command boundary when practical, then assert the expected observable
state and the important negative state (for example, one Ctrl+W activation
reduces the tab count by exactly one). Add a new executable only when the test
has genuinely distinct dependencies or lifecycle requirements.
