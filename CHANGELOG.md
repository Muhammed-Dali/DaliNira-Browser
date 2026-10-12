# Changelog

All notable release-facing changes to DaliNira Browser are documented here.

## [8.0.1] - 2026-10-12

8.0.1 is the first published release of the 8.0 series. The v8.0.0 tag was
created, but its release build failed before anything was published; 8.0.1
contains all of 8.0.0 below plus these fixes.

### Fixed

- Debian 12 and Ubuntu 24.04 (Qt 6.4) build again: Qt 6.8-only client-hints code is version-guarded and `QTimeZone::UTC` became `QTimeZone::utc()`; CMake accepts FindSQLite3's pre-4.1 target name (`SQLite::SQLite3`).
- Fedora: the private WebKitGTK runtime check asked `dladdr()` for a function address, which in a non-PIE executable names the browser itself, so the WebKit engine was disabled although the private libraries were loaded. The check now asks the dynamic loader (`dlopen(RTLD_NOLOAD)` + `dlinfo`).
- The launcher of Chromium's helper processes no longer links WebKitGTK, X11, libpsl and libsecret; without `--as-needed` (Fedora) these were unresolvable NEEDED entries.
- Private WebKitGTK runtime build: the generated `WebKitDirectoryInputStreamData.h` is built before the parallel build (WebKitGTK 2.54.1 does not order it before its use in WebKitNetworkProcess), and the DT_RPATH check no longer fails at random under `pipefail`.
- Release packaging: Debian 12 builds the runtime with clang-18 (clang-16 rejects WebKitGTK 2.54's ANGLE), the runtime and native package jobs install gettext and libavif, Arch CI jobs build without a `-debug` split package, the Arch source job installs git before checkout, and fresh-install tests keep `/usr/share/doc` so the license files are checked.
- Tests: the popup-blocking integration test waits for its own opener page, and the hover-card test accepts a shortened title with the full title in its tooltip.

## [8.0.0] - 2026-10-11 (tagged, not published)

### Added

- **Dual-engine architecture**: Chromium (Qt WebEngine) and WebKitGTK behind a shared `EngineView`/`EngineBackend` abstraction, selectable in onboarding and Settings → System, with a separate tab/session workspace per engine.
- **Onboarding**: language, engine, layout and theme, search engine and privacy options, and browser data import.
- **Layouts**: vertical tabs on the left or right and a floating sidebar, alongside the classic tab strip.
- **Auto-hiding side tabs**: visible on New Tab, hidden on websites, revealed by hovering the window edge, with a pin toggle (Settings → Appearance → Auto-hide side tabs); left, right and floating layouts; navigation controls and the main menu move into the panel; a background-aware glass look beside New Tab; event-driven pointer tracking on WebKitGTK; parity-tested on both engines.
- **Keyboard Shortcuts** (Settings → Keyboard Shortcuts): customizable bindings with conflict detection and reassignment, reset one or reset all, live updates in every window, reserved text-editing keys, DaliNira's own key names (e.g. Ctrl+⇧+Y), and the same behavior on Chromium and WebKitGTK.
- **Private WebKitGTK runtime**: the WebKit engine runs on DaliNira's own build of WebKitGTK 2.54.1 under `/usr/lib/dalinira-browser/webkit/`, with a fix for an `AcceleratedSurface` web-process crash (unpatched 3/40 window lifetimes crashed; patched 0/500, packaged runtime 0/500). License texts, the patch and a source notice are installed; the corresponding source is published with the release.
- **Native window movement**: dragging by the tab strip or title area uses the window manager's move, so KDE Plasma snaps, tiles and maximizes DaliNira like other windows; new windows open at a screen-based size, centered.
- **Unified developer build**: `./scripts/build-dalinira.sh` checks or builds the private runtime (first run: pinned download, SHA-256 check, patch, at most 3 jobs) and then builds the dual-engine application.
- **Tab sleeping**: two-stage light/full sleep, unsaved-input protection, a sleeping-tab indicator, Wake actions, and per-tab memory in the hover card where it can be measured exactly.
- **Privacy**: Global Privacy Control, HTTPS by default with a one-time fallback, link debouncing (`$urlskip`), fingerprinting noise, WebKitGTK third-party cookie blocking with ITP, and Chromium cache/connection partitioning.
- **Find in page**: redesigned find bar, toolbar button, Ctrl+G / Ctrl+Shift+G.
- **Shortcut row**: drag-and-drop reordering of Quick Access shortcuts.
- **Feedback**: Settings → About links to the GitHub bug-report and feature-request forms, plus a **Send Email** action that opens a reviewable draft in the user's mail application (version and engine only); no browsing data is attached.
- French interface translation; live language switching for all four interface languages (English, Turkish, Arabic, French).
- Engine-parity, onboarding, window-platform, and feedback-link regression suites; a localization audit; a GUI test safety runner.
- Packaging and CI jobs that build and fresh-install the portable archive and the `.deb`, `.rpm` and Arch packages.

### Known issues

- Builds against Qt older than 6.8 (Debian 12 and Ubuntu 24.04 `.deb`, the portable archive and the Arch binary packages that repackage it) have no Secure DNS setting, no saved-site-permissions list, keep the OS version in client hints and detect light/dark only through the palette. The Fedora `.rpm` and the Arch source package have all features.
- WebKitGTK 2.54.1 (upstream): when the `<video>` element itself is made fullscreen (native video controls, or `requestFullscreen()` on the video), playback stops advancing until fullscreen ends. Container fullscreen, as on YouTube, windowed playback and the Chromium engine are not affected.

### Changed

- The DaliNira blocker runs natively in both engines. Tracking parameters are now stripped in a single pass using an extended built-in list.
- The page-translation popup no longer blocks the page.
- Refined New Tab page, tab strip, toolbar icons, context menus, and settings pages.

### Fixed

- WebKitGTK X11 error handling, keyboard focus handover, and stacking of native overlays.
- Clean shutdown of retained WebKitGTK web processes such as service workers.
- Windows opened for links, popups and detached tabs are released on close, together with their WebKit tabs and web processes.
- Ad-blocking scripts on page-initiated cross-site navigations in Chromium.
- New Tab page no longer scrolls.
- Settings → About names the engine actually running (WebKitGTK and its version under WebKit) and shows the product name correctly.
- The WebKit New Tab page uses the profile's search engine, as the address bar does.
- Translation target and preferred languages default to the interface language instead of Turkish.
- Onboarding icons load from the application resources on every installation.
- The tab-sleep timing row in Settings → Performance no longer squeezes its label.

## [7.2.1] - 2026-09-23

### Fixed

- **TikTok Media Page URL Resolver**: Improved candidate extraction to reliably handle modern DOM layouts, photo slideshows (`/photo/`), video ID patterns, rehydration data payloads, and explicit media anchors.
- **Platform Feed URL Protection**: Added `isGenericPlatformFeedUrl` checks across the browser UI and download manager to prevent generic home/feed pages (e.g. `tiktok.com`, `tiktok.com/foryou`, `tiktok.com/explore`) from incorrectly triggering single-media download suggestions and auto-analysis.
- **Adult Content Protection**: Fixed false-positive adult content classifications on download manager requests and non-navigation file transfers.
- **Search Suggestions / New Tab**: Corrected search suggestion queries and New Tab omnibox search handling for smooth suggestion navigation.

### Added

- Added automated regression tests for modern TikTok media extraction, slideshow resolution, and generic platform feed URL detection across the native download infrastructure.

## [7.2.0] - 2026-09-21

### Added

- Added the **DaliNira Bağlantılı** tab style with a connected active-tab accent, made it the default for new profiles, and exposed persistent tab-style selection in Settings.
- Added Linux Secret Service-backed device keyring integration for stronger local credential-vault protection and migration coverage.
- Added focused contributor, architecture, build, testing, debugging, security, and issue-reporting documentation.

### Changed

- Refined tab geometry, drag/layout behavior, loading indicators, toolbar icons, keyboard shortcuts, and Reload/Stop state transitions for clearer navigation feedback.
- Expanded New Tab customization with separate download/protection card controls, counter scopes, improved search-history suggestions, and persistent custom backgrounds synchronized across open New Tab pages.
- Improved page translation provider handling, dynamic-content translation, translated-content restoration, target-language selection, and protected translation-secret storage.
- Refined settings, downloads, local media playback, audio effects, and Pulse song-recognition behavior.
- Split large browser-window and settings implementations into smaller subsystem-focused modules to make maintenance and review easier.

### Fixed

- Fixed custom New Tab background upload, preview, immediate application, replacement, removal, restart persistence, and safe PNG/JPEG/WebP validation without exposing unrestricted local-file access.
- Prevented DaliNira Blocker from breaking normal YouTube playback by narrowing ad-stream matching and suppressing destabilizing YouTube procedural/scriptlet behavior.
- Hardened credential autofill, save prompts, vault migration, origin validation, private-profile isolation, and sensitive-memory cleanup.
- Corrected blocker accounting and ruleset updates, session restoration, translation lifecycle handling, and several asynchronous WebEngine lifetime paths.
- Expanded deterministic regression coverage across passwords, translation, blocker, media, audio, tabs, performance settings, omnibox, and internal pages.

## [7.1.2] - 2026-09-13

### Added

- Added internal media player support (`dalinira://player`) and local media routing for downloaded audio and video files.
- Enabled DALI Web Audio DSP processing and equalizer effects for internal playback of downloaded media.
- Added launcher wrapper and taskbar icon grouping integration via `StartupWMClass=DaliNiraBrowser` and default browser prompt handling.

### Fixed

- Resolved download manager progress delta and status reporting for streaming downloads.
- Improved persistence of download jobs, thumbnails, and origin metadata.

## [7.1.1] - 2026-09-13

### Fixed

- Corrected New Tab asset discovery for installed Linux packages whose executable lives under `/usr/lib/dalinira-browser` while runtime assets live under `/usr/share/dalinira-browser`.
- Added embedded fallbacks for the New Tab logo, background, search-engine artwork, and interface icons so a missing external asset can no longer produce broken-image placeholders.
- Added installed, portable, and build-tree asset-layout regression coverage.

## [7.1.0] - 2026-09-13

### Changed

- Set DuckDuckGo as default search engine and enhanced the New Tab experience with custom search history, quick engine switching, and privacy metrics.
- Hardened password autofill and New Tab integration test suites for high reliability across Linux CI distributions (Ubuntu, Debian, Fedora, Arch Linux).

## [7.0.1] - 2026-09-11

### Fixed

- Removed a late-callback lifetime hazard from the Qt WebEngine JavaScript test
  helper and made JavaScript polling honor one bounded deadline.
- Made the new-tab suggestion selection retry path tolerate transient renderer
  timing without bypassing its final correctness assertions.
- Made the changed-resource download resume regression deterministic while
  preserving strict checks for progress, pause/resume state transitions,
  notifications, and byte-for-byte replacement content.

## [7.0.0] - 2026-09-11

### Added

- **GeneralDownloadManager Engine**: High-performance adaptive multi-connection segmented download pipeline with 1 → 2 → 4 → 8 parallel streams, dynamic work stealing, chunk pre-allocation, pause/resume/cancel/part-level retry, and 429 throttling backoff.
- **Download Management UI**: Dedicated toolbar download popup (`DownloadToolbarUI`) with live speed metering and an updated internal downloads page (`dalinira://downloads`).
- **Smart Omnibox Navigation**: Composite navigation ranking aggregating history, bookmarks, and frequent sites with domain normalizer and real-time search suggestions (DuckDuckGo, Google, Brave, Bing).
- **Granular Site Permissions**: `SiteControlsBubble` and `SitePermissionPromptBubble` providing per-origin toggles for Camera, Microphone, Geolocation, Notifications, Popups, Insecure Content, and JavaScript optimization.
- **Encrypted Local Credential Vault**: Zero-cloud password management with AES-256-GCM encryption, PBKDF2-HMAC-SHA256 key derivation, in-page autofill controller, save bubbles, and vault unlock dialogs.
- **Hardware-Accelerated WebEngine**: Zero-copy video decoding pipelines, early GPU runtime flags, subprocess memory policy, tab memory hover cards, and tab memory pressure monitor.
- **AutoEQ Presets Integration**: Bundled 1,757 calibrated AutoEQ headphone correction profiles for audiophile hardware in `browser/resources/eq-presets/autoeq` and CMake install rules.
- **CMake & Packaging Integration**: Complete `GNUInstallDirs` support, unified `.desktop` specification, full hicolor icon resolution suite (16px to 1024px), and updated PKGBUILDs for Arch Linux and AUR.
- **DaliNira Listen**: PipeWire/PulseAudio discovery, ffmpeg-backed 16 kHz mono capture, live input levels, persistent settings, dedicated `dalinira://listen` pages, and single-instance tab integration.
- **Media Downloader**: yt-dlp analysis, metadata and thumbnails, selectable audio/video formats, adaptive-stream merging, conversion, embedded metadata and cover art, subtitles, ranges, playlists, queues, retry, cancellation, history, and export.
- **Desktop Tab Architecture**: Detach/attach, cross-window drag sessions, groups, search, animations, hover cards, audible/fullscreen state, memory-pressure handling, and lifecycle-safe restore.
- **Languages & Translation**: Centralized English, Turkish, and Arabic catalogs, runtime switching, system-locale detection, persistent preferences, RTL/LTR metadata, preferred-language ordering, spell check, custom dictionary, and page translation providers.
- **Official Website**: Security-first static English, Turkish, and Arabic site with responsive RTL support, real product screenshots, local-only assets, CSP/security headers, SEO, and no tracking.

### Changed

- Updated version metadata to 7.0.0 across CMake, application runtime, package manifests, and Arch/AUR packaging.
- Enhanced resource search paths to reliably locate AutoEQ presets and AdBlock rulesets in portable, build, and system-installed `/usr/share/dalinira-browser/` environments.
- Enforced strict assertions (`-UNDEBUG`) across all automated test suites.
- Expanded the verified suite to 29 CTest targets covering blocker accounting, song finding, downloads, permissions, omnibox behavior, translation, i18n, tab lifecycle, performance, credentials, and security.

### Fixed

- Made blocker hit statistics tab-scoped and thread-safe, prevented duplicate cosmetic counts, preserved SPA counters, corrected subdomain and initiator handling, and cleaned state on tab close.
- Corrected cosmetic blocking totals shown in browser UI and expanded Facebook, Reels, article, and sponsored-content filtering.
- Preserved the Qt 6.4 build baseline by guarding newer WebEngine and color-scheme APIs and fixing complete-type and permission-bubble declarations.

## [6.1.2] - 2026-08-24

### Changed

- Displays the installed application name as `DaliNira` in desktop launchers.
- Installs the verified DaliNira bee-shield icon under the `dalinira` icon identity.
- Standardized package manifests for Arch Linux, AUR, and the pacman repository.

## [6.0.1] - 2026-08-24

### Changed

- Clarified that DaliNira Blocker is DaliNira Browser's built-in advertising and
  tracker protection engine, while third-party filter and ruleset resources
  retain separate attribution in `NOTICE.txt`.
- Added the Qt image-format runtime dependency required for bundled WebP
  handling in Arch Linux packages.
- Stabilized GitHub CI and release builds with an Xvfb-backed Qt test
  environment and explicit release re-run support.

### Fixed

- Corrected release automation and Arch package metadata discovered during the
  6.0.0 publication verification.

## [6.0.0] - 2026-08-24

### Changed

- Rebuilt DaliNira as a native Qt 6 and C++20 desktop browser.
- Reorganized the browser into modular core, tabs, settings, audio, blocker,
  password, Pulse, sidebar, and session components.
- Migrated the Linux desktop integration and Arch packaging to the
  `dalinira-browser` executable and product identity.

### Added

- DaliNira Blocker with local request filtering, cosmetic filtering, rule-set
  controls, per-site controls, and inspection tools.
- DaliNira Pulse music recognition using system audio or microphone capture.
- An encrypted local credential vault and password-management interface.
- Custom new-tab, audio effects, 32-band equalizer presets, tab workspaces,
  and redesigned browser settings.

### Migration note

DaliNira Browser 6.0.0 is a major architectural transition from
DaliNira-WebMedia 5.5.2. Existing user-data migration is not guaranteed.
