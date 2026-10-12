# DaliNira Browser 8.0.2

Released on 13 October 2026.

DaliNira Browser 8.0.2 is a bug-fix release of the 8.0 series. It fixes the
installed packages, which did not find several of DaliNira's own files (see
"Fixes in 8.0.2" below). The notes below describe the whole 8.0 series.

DaliNira now ships two complete browser engines, Chromium (through Qt WebEngine) and WebKitGTK, behind one
shared interface. It also adds a new first-run experience, a substantially
reworked ad and tracker blocker, customizable keyboard shortcuts, auto-hiding
side tabs, tab sleeping, and many interface improvements.

## Two engines, one browser

- Chromium and WebKitGTK are both first-class engines. Choose one during
  onboarding or later in **Settings → System**. Switching engines restarts the
  browser.
- Each engine has its own browsing workspace: open tabs, session, Quick Access
  shortcuts and frequent sites are kept per engine. Bookmarks, theme, language,
  search engine, blocker settings and downloads are shared.
- The toolbar, tabs, menus, popups, find bar, translation, blocker,
  permissions and downloads work the same way in both engines. Automated
  parity tests run the same scenarios against both engines and compare the
  results.
- On Wayland desktops, Chromium runs natively on Wayland and WebKitGTK runs
  through XWayland.

## Privacy and blocking

- The DaliNira blocker now runs natively in both engines. On WebKitGTK it filters
  requests through a web process extension.
- Tracking-parameter removal strips all matching parameters in a single step
  and uses an extended built-in parameter list.
- Link debouncing (`$urlskip`) sends known redirect wrappers straight to their
  destination. Debouncing is skipped when the destination itself would be
  blocked.
- HTTPS is used by default for top-level navigations to public hosts, with a
  one-time fallback to HTTP for sites that do not support it.
- Global Privacy Control (`Sec-GPC` header and `navigator.globalPrivacyControl`)
  is sent in both engines.
- Fingerprinting protection adds per-site, per-session noise to canvas, WebGL,
  audio and several hardware values.
- Third-party cookie blocking applies to WebKitGTK too, through its cookie
  policy and Intelligent Tracking Prevention.
- In Chromium, the HTTP cache and network connections are partitioned by top-level
  site, and `window.name` is cleared on cross-site navigations.

## First run, language and layout

- A new onboarding flow covers the interface language, the engine, the browser
  layout and theme, search engine and privacy options, and importing data from
  other browsers.
- The interface is available in English, Turkish, Arabic and, new in this
  release, French. The translation catalog grew from 209 to 2,128 strings, so
  settings, menus, dialogs and internal pages are now translated as well. The
  language can be changed without restarting, and the browser chrome stays
  left-to-right in every language.
- Four layouts are available: classic tabs, vertical tabs on the left or right,
  and a floating sidebar.

## Side tabs

Vertical side tabs gain an auto-hide mode:

- The side tabs stay visible on the New Tab page and slide away on websites,
  so the page gets the full window width.
- Moving the pointer to the window edge reveals them as an overlay that slides
  in and out in 150 ms; the page is not relaid out. With reduced motion (the
  `ui/reduceMotion` preference or `DALINIRA_REDUCED_MOTION=1`) the panel
  appears and hides without sliding.
- The pin button in the panel keeps the tabs open or lets them hide again. The
  same preference is **Settings → Appearance → Auto-hide side tabs** and the
  *Keep side tabs open* shortcut action (no default key). Left and right layouts stay pinned by
  default; the floating sidebar hides by default.
- Works with the left, right and floating layouts. Fullscreen always hides the
  panel, and it never hides while a tab is being dragged.
- Beside the New Tab page the panel shows a blurred, tinted copy of the page
  background, so it matches the page in both light and dark themes.
- In side layouts, back, forward, reload, the add-site button and the main menu
  move into the panel's top bar.
- WebKitGTK pages report pointer movement to the panel through GTK events, so
  edge reveal works the same in both engines without a polling timer.
- The behavior is covered by a unit test and an engine-parity GUI test on
  Chromium and WebKitGTK.

## Keyboard shortcuts

A new **Settings → Keyboard Shortcuts** page manages the browser's shortcuts:

- Every action has a customizable binding. Actions are grouped as windows and
  tabs, navigation, search and find, page operations, history and bookmarks,
  media and display, and DaliNira tools.
- Conflict detection: when a combination is already in use, the page names
  the action that has it and offers to reassign it. Text-editing keys such as
  Ctrl+C, Ctrl+V and Ctrl+Z, window-manager keys such as Alt+F4, and AltGr
  combinations are reserved.
- Reset one shortcut to its default, or reset all shortcuts at once.
- Changes take effect immediately in every open window, without a restart.
- The same bindings work on Chromium and WebKitGTK pages; on WebKitGTK,
  browser shortcuts take priority over the page as they do in Chromium.
- Shortcuts are shown with DaliNira's own key names, so they match the
  interface language instead of the desktop's locale.
- New defaults include Ctrl+1 to Ctrl+8, Ctrl+9 and Ctrl+PgUp / Ctrl+PgDn.

## Browsing

- **Tab sleeping:** inactive tabs are put to light sleep first and released
  fully later or under memory pressure. Tabs with unsaved form input are not
  released automatically. Sleeping tabs are marked, can be woken from the tab
  menu, and the tab hover card shows per-tab memory where it can be measured
  exactly.
- **Find in page:** redesigned find bar with a toolbar button, a match counter,
  icon controls and keyboard shortcuts (Ctrl+F, Enter / Shift+Enter, F3 /
  Shift+F3, Ctrl+G / Ctrl+Shift+G, Escape).
- **Page translation:** the translation popup no longer blocks the page while it
  is open. Translation providers: Google, Google Cloud, DeepL and
  LibreTranslate.
- **Shortcut row:** Quick Access shortcuts under the address bar can be
  reordered by drag and drop.
- **Feedback:** Settings → About links to the GitHub bug-report and
  feature-request forms. **Send Email** opens a draft to the project in your
  own mail application with a short report template, the DaliNira version and
  the active engine; you review it and press Send yourself. None of these carry
  the current page, browsing history, browser state or logs.
- Refined New Tab page, tab strip, toolbar icons, context menus and settings
  pages.

## Window behavior

- Dragging the window by the tab strip or the title area now hands the move to
  the window manager. On KDE Plasma (X11, XWayland and Wayland) the window
  snaps to the left or right half, maximizes at the top edge and is restored
  by dragging it away, like other windows; the same applies to other window
  managers that support native moves.
- A new window without saved geometry opens at about two thirds of the
  screen's width and four fifths of its height, within 1350 x 900, centered;
  saved size, position and maximized state are restored as before.

## Stability and quality

- A shared engine abstraction (`EngineView` / `EngineBackend`) with separate
  Chromium and WebKitGTK backends.
- WebKitGTK shutdown now waits for retained web processes, such as service
  workers, to exit cleanly.
- Windows opened for links, popups and detached tabs are now released when
  they close, together with their WebKit tabs and web processes. The GUI test
  `browser-webkit-lifecycle` checks the WebKit process lifecycle: tabs,
  background tabs, sleep and wake, a second window, window close and shutdown.
- Fixed X11 error handling, focus handover and native overlay stacking for the
  embedded WebKitGTK view.
- New regression suites: engine parity (popups, scripts, media, layout, privacy,
  network, fingerprinting, find in page, translation, keyboard shortcuts, side
  tabs), onboarding, window platform, feedback links, keyboard shortcuts and
  side-tab auto-hide. A localization audit keeps all four catalogs
  complete and consistent.
- GUI tests run separately from the default test run, through a safety runner
  with resource monitoring.
- Fixed in the final release preparation: Settings → About now names the
  running engine (WebKitGTK and its version under WebKit); the WebKit New Tab
  page uses the profile's search engine; the translation target and preferred
  languages default to the interface language instead of Turkish; onboarding
  icons load from the application resources.

## Packaging

- Release builds produce a portable archive, `.deb` packages for Debian 12 and
  Ubuntu 24.04, an `.rpm` package for Fedora, and Arch Linux packages. CI
  installs each package into a fresh container and checks it.
- The WebKit engine runs on DaliNira's own build of WebKitGTK 2.54.1. It is
  installed under `/usr/lib/dalinira-browser/webkit/` and carries a fix for a
  web process crash in WebKitGTK 2.54.1: a compositor timer was destroyed on
  the wrong thread when a background tab closed. Only DaliNira uses this
  copy. The fix was validated before release: the unpatched WebKitGTK 2.54.1
  crashed in 3 of 40 window lifetimes; the patched build and the packaged
  private runtime each ran 500 lifetimes with 2,000 web processes without a
  crash. The patch caused no deadlock and no CPU, GPU or heap-memory
  regression in an interleaved comparison. No upstream fix exists yet. The system's WebKitGTK is neither needed nor changed, and no setup is
  required. `dalinira-browser --check-webkit-runtime` shows which WebKitGTK
  is in use. Its licenses, the patch and the source notice are installed in
  `/usr/share/doc/dalinira-browser/webkitgtk/`. The complete source is
  published with each release (`dalinira-webkit-runtime-source-8.0.1.tar`).
- WebKitGTK media playback needs the GStreamer plugins listed in
  [docs/webkit-media-runtime.md](webkit-media-runtime.md).

## For developers

- `./scripts/build-dalinira.sh` is the single build command. Each run first
  checks the private WebKitGTK runtime in `build-webkit-runtime/`, which is a
  no-work step when nothing changed, and then builds the dual-engine
  application in `build-dalinira/`.
- On a fresh checkout the same command builds the private runtime once: it
  downloads WebKitGTK 2.54.1, verifies the pinned SHA-256, applies the patch,
  and builds with at most 3 jobs after asking for confirmation. The first
  build takes about 5 GB of disk and typically 1.5 to 3 hours. See
  [BUILDING.md](BUILDING.md).
- A normal build fails instead of producing a Chromium-only browser.

## Fixes in 8.0.2

- Installed packages start DaliNira through `/usr/bin/dalinira-browser`, a symbolic link to `/usr/lib/dalinira-browser/dalinira-browser`. DaliNira looked for the files it installs next to its executable in `/usr/bin`, so the WebKit engine started without its web-process extension: the tab hover card kept showing "Memory usage: measuring…", network-level ad and tracker blocking was off, and the Web Audio output tuning was missing. The browser policy file, blocker lists, EQ presets, translations and New Tab assets were affected the same way. DaliNira now resolves its own executable before looking for these files. Development builds started directly from the build directory were not affected.
- The pacman repository workflow polls the release commit's CI run once a minute instead of every few seconds, which exhausted the workflow's GitHub API rate limit.

## Fixes in 8.0.1

- Debian 12 and Ubuntu 24.04 (Qt 6.4) build again: Qt 6.8-only client-hints code is version-guarded and `QTimeZone::UTC` became `QTimeZone::utc()`; CMake accepts FindSQLite3's pre-4.1 target name (`SQLite::SQLite3`).
- Fedora: the private WebKitGTK runtime check asked `dladdr()` for a function address, which in a non-PIE executable names the browser itself, so the WebKit engine was disabled although the private libraries were loaded. The check now asks the dynamic loader (`dlopen(RTLD_NOLOAD)` + `dlinfo`).
- The launcher of Chromium's helper processes no longer links WebKitGTK, X11, libpsl and libsecret; without `--as-needed` (Fedora) these were unresolvable NEEDED entries.
- Private WebKitGTK runtime build: the generated `WebKitDirectoryInputStreamData.h` is built before the parallel build (WebKitGTK 2.54.1 does not order it before its use in WebKitNetworkProcess), and the DT_RPATH check no longer fails at random under `pipefail`.
- Release packaging: Debian 12 builds the runtime with clang-18 (clang-16 rejects WebKitGTK 2.54's ANGLE), the runtime and native package jobs install gettext and libavif, Arch CI jobs build without a `-debug` split package, the Arch source job installs git before checkout, and fresh-install tests keep `/usr/share/doc` so the license files are checked.
- Tests: the popup-blocking integration test waits for its own opener page, and the hover-card test accepts a shortened title with the full title in its tooltip.

## Known limitations

- Packages built against Qt older than 6.8 have fewer privacy and settings
  features: the Debian 12 and Ubuntu 24.04 `.deb` packages and the portable
  archive are built against Qt 6.4, and the Arch binary packages (pacman
  repository, AUR `dalinira-bin`) repackage the portable archive. These builds
  have no Secure DNS setting (Qt 6.6), no list of saved site permissions (Qt 6.8;
  permission prompts still work), do not remove the OS version from client
  hints (Qt 6.8), and follow the system light/dark theme only through the
  palette (Qt 6.5). The Fedora `.rpm` and the Arch source package
  (`packaging/archlinux`) are built against current Qt and have all features.
- WebKitGTK uses a single network cache and connection pool; it offers no API to
  partition them by site.
- In Chromium, strict site isolation lets a third-party frame reuse the renderer
  process it already has in another open tab, and Chromium's in-memory resource
  cache in that process is not partitioned. The HTTP disk cache and network
  connections stay partitioned by top-level site.
- In WebKitGTK, light sleep throttles hidden pages but does not freeze them. Full
  sleep releases the page in both engines.
- Per-site cookie exceptions are not available on WebKitGTK, whose cookie
  policy is global.
- WebKitGTK 2.54.1 known issue (upstream): a video shown fullscreen *as the
  `<video>` element itself* stops advancing until fullscreen ends. This
  happens with the native video controls' fullscreen button and with pages
  that call `requestFullscreen()` on the video element. Sites that make
  their player container fullscreen, as YouTube does, are not affected, and
  windowed playback is not affected. Chromium is not affected.

## Platform status

Linux x86_64 is the supported platform. The portable archive ships as
`dalinira-browser-8.0.1-linux-x86_64.tar.zst`.
