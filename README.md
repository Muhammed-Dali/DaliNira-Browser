<p align="center">
  <img src="docs/images/dalinira-icon.png" width="112" height="112" alt="DaliNira Browser logo">
</p>

<h1 align="center">DaliNira Browser</h1>

<p align="center">
  A native Linux desktop browser with two engines — Chromium and WebKitGTK — built-in ad and tracker blocking, and a local-first design.
</p>

<p align="center">
  <a href="https://github.com/Muhammed-Dali/DaliNira-Browser/releases/tag/v8.0.0"><img src="https://img.shields.io/badge/release-v8.0.0-007ACC.svg?style=flat-square" alt="Release v8.0.0"></a>
  <a href="https://github.com/Muhammed-Dali/DaliNira-Browser/actions/workflows/ci.yml"><img src="https://github.com/Muhammed-Dali/DaliNira-Browser/actions/workflows/ci.yml/badge.svg" alt="CI status"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-GPL--3.0--only-success.svg?style=flat-square" alt="License GPL-3.0-only"></a>
  <img src="https://img.shields.io/badge/platform-Linux%20x86__64-FCC624.svg?style=flat-square&logo=linux&logoColor=black" alt="Platform: Linux x86_64">
  <img src="https://img.shields.io/badge/Qt-6-41CD52.svg?style=flat-square&logo=qt" alt="Qt 6">
  <img src="https://img.shields.io/badge/C%2B%2B-20-00599C.svg?style=flat-square&logo=c%2B%2B" alt="C++20">
</p>

<p align="center">
  <a href="https://github.com/Muhammed-Dali/DaliNira-Browser/releases">Download</a> ·
  <a href="https://muhammed-dali.github.io/DaliNira-Browser/">Website</a> ·
  <a href="docs/RELEASE_NOTES_v8.0.0.md">Release notes</a> ·
  <a href="CHANGELOG.md">Changelog</a>
</p>

<p align="center">
  <img src="docs/images/dalinira-browser.png" width="100%" alt="DaliNira main window with the New Tab page, bookmarks bar, Quick Access shortcuts and clock">
</p>

## What is DaliNira?

DaliNira is an independent desktop browser written in C++20 and Qt 6. It brings
together features that usually need several extensions — ad and tracker
blocking, page translation, a download manager, a password vault and an audio
equalizer — in one native application. Your data stays on your computer, and
no account is needed.

Version 8.0.0 adds a second browser engine. DaliNira can run on **Chromium**
(through Qt WebEngine) or on **WebKitGTK**, with the same interface and
features on both.

## Highlights

- **Two engines, one interface:** Chromium and WebKitGTK behind a shared engine
  layer, each with its own tabs and session.
- **Built-in ad and tracker blocking:** a native filter engine with EasyList,
  EasyPrivacy, regional lists, cosmetic filtering, tracking-parameter removal
  and link debouncing.
- **Privacy defaults:** HTTPS by default, Global Privacy Control,
  fingerprinting noise and third-party cookie blocking.
- **Page translation** with Google, Google Cloud, DeepL or LibreTranslate.
- **Tab sleeping:** inactive tabs sleep in two stages to save memory, and
  pages with unsaved input are protected.
- **Four interface languages:** English, Turkish, Arabic and French, switchable
  without a restart.
- **Customizable layout:** classic tabs, vertical tabs on either side, or a
  floating sidebar; New Tab backgrounds, clock styles and Quick Access
  shortcuts.
- **Utilities:** a parallel download manager, an encrypted local password vault,
  a 32-band web-audio equalizer with 1,757 AutoEQ headphone presets, and Pulse
  song recognition.

## Two engines

Choose the engine in the first-run setup, or later in **Settings → System**.
Switching restarts the browser and opens that engine's own set of tabs.

|                     | Chromium (Qt WebEngine)                 | WebKitGTK                                   |
|---------------------|-----------------------------------------|---------------------------------------------|
| Interface, menus, find bar, translation, blocker, downloads | Shared | Shared |
| Tabs and session    | Own workspace                           | Own workspace                               |
| Wayland             | Native Wayland                          | Through XWayland                            |
| Cache partitioning  | HTTP cache per top-level site¹          | Not available (single cache)                |
| Light tab sleep     | Page frozen                             | Hidden page throttled                       |

¹ A third-party frame that is open in another tab can reuse that tab's renderer
process, and Chromium's in-memory resource cache there is not partitioned.

Bookmarks, theme, language, search engine, blocker settings and downloads are
shared by both engines. Automated parity tests run the same scenarios on both
engines and compare the results.

## Features

### Ad and tracker protection

<p align="center">
  <img src="docs/images/dalinira-blocker.png" width="100%" alt="DaliNira Blocker page with the Basic, Optimal and Complete filtering modes">
</p>

The DaliNira blocker is a native C++ filter engine that works in both
engines. It applies EasyList, EasyPrivacy, Peter Lowe's list, AdGuard and
regional lists locally. It hides empty ad containers, removes tracking
parameters such as `utm_*`, `fbclid` and `gclid`, and sends known redirect
wrappers straight to their destination. Protection levels, list selection and
per-site exceptions are available on `dalinira://blocker`.

### Find in page and translation

Press **Ctrl+F** or use the toolbar button to search the page. Enter and
Shift+Enter, F3 or Ctrl+G move between matches. When a page is in another
language, the translation popup offers to translate it in place, without
blocking the page.

### Tab sleeping

Background tabs first go into light sleep, then release their memory after a
longer idle period or under memory pressure. You can also put a tab to sleep
or wake it from the tab menu. Tabs with unsaved form input are never released
automatically. The hover card shows a tab's memory use when it can be measured
exactly.

### Settings, languages and customization

<p align="center">
  <img src="docs/images/settings.png" width="100%" alt="DaliNira Settings with the category sidebar, open on the On startup page">
</p>

All settings are on one page (`dalinira://settings`), grouped in a sidebar
from startup and appearance to privacy, languages and the browser engine. The
whole interface is translated into English, Turkish, Arabic and French, and you
can change the language without restarting. Appearance settings cover the
theme, tab style, browser layout and New Tab page.

### Downloads, passwords and media

<p align="center">
  <img src="docs/images/downloads.png" width="49%" alt="Downloads page with the link box, active downloads and download history">
  <img src="docs/images/password-manager.png" width="49%" alt="Password Manager opt-in screen with the local vault notice">
</p>
<p align="center">
  <img src="docs/images/audio-effects.png" width="49%" alt="Audio Effects page with the 32-band equalizer">
  <img src="docs/images/dalinira-pulse.png" width="49%" alt="DaliNira Pulse ready to listen, with the audio source selector">
</p>

- **Downloads:** segmented parallel downloads with resume and retries, plus
  media analysis for supported sites (`dalinira://downloads`).
- **Password vault:** credentials are encrypted locally with AES-256-GCM and a
  PBKDF2-HMAC-SHA256 key. The vault is never synchronized to a server
  (`dalinira://passwords`).
- **Audio effects:** a 32-band equalizer, compressor, limiter, stereo widener,
  reverb and 1,757 AutoEQ headphone presets for web media
  (`dalinira://audio-effects`).
- **Pulse:** identifies the song playing on your computer. Pulse builds an audio
  fingerprint locally and sends only that fingerprint to Shazam's recognition
  service.

### Feedback

**Settings → About DaliNira → Feedback** links to the GitHub bug-report and
feature-request forms. **Send Email** opens a draft in your own mail
application, addressed to the project, with a short problem-report template,
the DaliNira version and the active engine; nothing is sent until you press
Send. None of these include the current page, browsing history, account data or
logs.

## Privacy principles

- **Local first:** history, bookmarks, passwords, settings and audio presets
  are stored in your user profile on your computer.
- **No account and no built-in telemetry.**
- **Network services are used only for the features that need them:** search
  suggestions from your chosen search engine, translation from the provider
  you select, Pulse recognition, and filter-list updates.
- **Private windows** use a separate profile that is not written to disk.

See [SECURITY.md](SECURITY.md) for the security model and how to report
vulnerabilities.

## Installation

DaliNira supports Linux x86_64. Every release on the
[Releases page](https://github.com/Muhammed-Dali/DaliNira-Browser/releases)
includes:

| Package                                         | Distribution       |
|-------------------------------------------------|--------------------|
| `dalinira-browser_8.0.0-1.debian12_amd64.deb`   | Debian 12          |
| `dalinira-browser_8.0.0-1.ubuntu2404_amd64.deb` | Ubuntu 24.04       |
| `dalinira-browser-8.0.0-1.fcNN.x86_64.rpm`      | Fedora             |
| `dalinira-browser-8.0.0-linux-x86_64.tar.zst`   | Portable archive   |

Install a downloaded package with your package manager, for example
`sudo apt install ./dalinira-browser_8.0.0-1.debian12_amd64.deb` or
`sudo dnf install ./dalinira-browser-8.0.0-1.fc*.x86_64.rpm`.

### Arch Linux

From the AUR:

```bash
yay -S dalinira-bin
```

Or from the signed DaliNira pacman repository (key
`BC741FD0AC804351B0DDBB86FDFEC60C11202588`):

```bash
curl -sL https://github.com/Muhammed-Dali/DaliNira-Browser/releases/download/pacman-repo/dalinira.gpg | sudo pacman-key --add -
sudo pacman-key --lsign-key BC741FD0AC804351B0DDBB86FDFEC60C11202588
echo -e "\n[dalinira]\nSigLevel = Required DatabaseOptional\nServer = https://github.com/Muhammed-Dali/DaliNira-Browser/releases/download/pacman-repo" | sudo tee -a /etc/pacman.conf
sudo pacman -Syu dalinira
```

### Portable archive

```bash
tar -I zstd -xvf dalinira-browser-8.0.0-linux-x86_64.tar.zst
sudo cp -r usr/* /usr/
```

The WebKitGTK engine needs the GStreamer plugins listed in
[docs/webkit-media-runtime.md](docs/webkit-media-runtime.md) to play media.

## Building from source

Requirements: a C++20 compiler, CMake, Ninja, Node.js, Qt 6 (Base,
WebEngine, SVG, Image Formats), OpenSSL, libpsl, and — for the WebKitGTK engine —
WebKitGTK 4.1 and GStreamer.

**Arch Linux**

```bash
sudo pacman -S --needed base-devel cmake ninja git nodejs pkgconf openssl libpsl libsecret ffmpeg \
  qt6-base qt6-webengine qt6-svg qt6-imageformats \
  webkit2gtk-4.1 gst-plugins-base gst-plugins-good gst-plugins-bad gst-libav
```

**Debian 12 / Ubuntu 24.04**

```bash
sudo apt install build-essential cmake ninja-build git nodejs pkg-config libssl-dev libpsl-dev ffmpeg \
  qt6-base-dev qt6-webengine-dev qt6-svg-dev qt6-image-formats-plugins \
  libwebkit2gtk-4.1-dev libgstreamer1.0-dev gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad gstreamer1.0-libav
```

**Fedora**

```bash
sudo dnf install gcc-c++ cmake ninja-build git nodejs pkgconf-pkg-config openssl-devel libpsl-devel ffmpeg-free \
  qt6-qtbase-devel qt6-qtwebengine-devel qt6-qtsvg-devel qt6-qtimageformats \
  webkit2gtk4.1-devel gstreamer1-devel
```

Then configure, build and run:

```bash
git clone https://github.com/Muhammed-Dali/DaliNira-Browser.git
cd DaliNira-Browser
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
./build/dalinira-browser                 # engine from settings (Chromium by default)
./build/dalinira-browser --engine webkit # start with WebKitGTK
```

If WebKitGTK is not installed, the build continues with the Chromium engine
only. To build without WebKitGTK on purpose, pass `-DDALINIRA_ENABLE_WEBKIT=OFF`.

### Tests

```bash
ctest --test-dir build --output-on-failure    # non-GUI tests
scripts/safe-gui-test.sh                      # GUI and engine-parity tests, one at a time
python3 scripts/i18n_audit.py                 # localization catalogs
scripts/check-release-metadata.sh             # version consistency
```

GUI tests open real browser windows. Run them on a desktop session, not on a
virtual display; `scripts/safe-gui-test.sh` explains the safety rules it
enforces.

## Documentation

- [Architecture](docs/ARCHITECTURE.md) — subsystems, startup sequence, ownership
- [Building](docs/BUILDING.md) — dependencies and build options
- [Testing](docs/TESTING.md) — test tiers and how to run them
- [Debugging](docs/DEBUGGING.md) — diagnostics and logging
- [WebKitGTK media runtime](docs/webkit-media-runtime.md)
- [Contributing](CONTRIBUTING.md) · [Security policy](SECURITY.md)

## License

DaliNira Browser is licensed under the [GNU General Public License v3.0](LICENSE).

The DaliNira blocker is an independent native filter engine. It does not
embed the uBlock Origin extension, but it uses compatible pre-generated
rulesets from EasyList, EasyPrivacy, uAssets, AdGuard, Peter Lowe's list and
other community projects. See [NOTICE.txt](browser/resources/adblock/NOTICE.txt)
and the [ruleset attribution manifest](browser/resources/adblock/rulesets/ruleset-attributions.json).
The AutoEQ presets come from the AutoEQ project by Jaakko Pasanen.
