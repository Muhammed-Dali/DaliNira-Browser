# DaliNira Browser 8.0.0

Released on 5 October 2026.

DaliNira Browser 8.0.0 is a major release. DaliNira now ships two complete
browser engines, Chromium (through Qt WebEngine) and WebKitGTK, behind one
shared interface. It also adds a new first-run experience, a substantially
reworked ad and tracker blocker, tab sleeping, and many interface improvements.

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
  release, French. The translation catalog grew from 209 to 2,031 strings, so
  settings, menus, dialogs and internal pages are now translated as well. The
  language can be changed without restarting, and the browser chrome stays
  left-to-right in every language.
- Four layouts are available: classic tabs, vertical tabs on the left or right,
  and a floating sidebar.

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

## Stability and quality

- A shared engine abstraction (`EngineView` / `EngineBackend`) with separate
  Chromium and WebKitGTK backends.
- WebKitGTK shutdown now waits for retained web processes, such as service
  workers, to exit cleanly.
- Fixed X11 error handling, focus handover and native overlay stacking for the
  embedded WebKitGTK view.
- New regression suites: engine parity (popups, scripts, media, layout, privacy,
  network, fingerprinting, find in page, translation), onboarding, window
  platform and feedback links. A localization audit keeps all four catalogs
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
- WebKitGTK media playback needs the GStreamer plugins listed in
  [docs/webkit-media-runtime.md](webkit-media-runtime.md).

## Known limitations

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

## Platform status

Linux x86_64 is the supported platform. The portable archive ships as
`dalinira-browser-8.0.0-linux-x86_64.tar.zst`.
