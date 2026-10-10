# Contributing to DaliNira Browser

Thank you for your interest in contributing to DaliNira Browser. This document outlines practical guidelines to help you understand the codebase, build and test changes, and submit high-quality contributions.

---

## 1. Development Environment

DaliNira Browser is a native C++20 desktop browser built on Qt 6 and Qt WebEngine:

- **Operating System:** Linux (x86_64, Wayland / X11).
- **C++ Standard:** C++20 (`gcc >= 11` or `clang >= 14`).
- **Build System:** CMake 3.20+ with Ninja.
- **Framework:** Qt 6.4+ (Core, Gui, Widgets, WebEngineWidgets, Network, Svg, Concurrent, DBus).
- **Tooling:** Node.js (for DALI DSP JavaScript generation), OpenSSL, libpsl, pkg-config.
- **Optional:** `libsecret-1` (for Linux FreeDesktop Secret Service vault key binding), `ffmpeg` (media downloads/playback), `pulseaudio`/`pipewire-pulse` (Pulse song recognition).

For distribution-specific package installation commands and dependencies, refer to [docs/BUILDING.md](docs/BUILDING.md).

---

## 2. Quick Start: Clone, Build, and Run

```bash
# Clone the repository
git clone https://github.com/Muhammed-Dali/DaliNira-Browser.git
cd DaliNira-Browser

# Build both engines. The first run also builds DaliNira's private WebKitGTK
# runtime once (about 5 GB of disk, typically 1.5 to 3 hours, asks first).
./scripts/build-dalinira.sh

# Launch the browser
./build-dalinira/dalinira-browser
```

Install the dependencies listed in [docs/BUILDING.md](docs/BUILDING.md) first;
`./scripts/bootstrap-webkit-runtime.sh --check` reports anything missing.
After the first build, `./scripts/build-dalinira.sh` is the only build command
you need: it checks the WebKit runtime (a no-work step when nothing changed)
and rebuilds DaliNira incrementally, with at most 3 parallel jobs.

To run with full terminal diagnostics and logging enabled:
```bash
QTWEBENGINE_CHROMIUM_FLAGS="--enable-logging=stderr --v=1" ./build-dalinira/dalinira-browser
```

---

## 3. Workflow & Contribution Rules

### Keep Changes Focused
- Address **one specific issue or feature per pull request**.
- Avoid massive unrelated refactorings or reformatting lines outside your feature area.
- Do not mix stylistic changes with functional bug fixes.

### Preserve Architecture & Avoid Duplication
- **Do not reinvent existing systems.** Check existing subsystems before implementing new managers:
  - Download handling belongs in `GeneralDownloadManager` / `MediaDownloadService` (`browser/native/downloads/`).
  - History, bookmarks, and site permissions belong in `BrowserProfileService` (`browser/native/core/`).
  - Audio processing belongs in `WebAudioEffectsController` and `EqPresetRepository` (`browser/native/audio/` and `browser/native/eq/`).
  - Passwords belong in `CredentialVaultManager` and `CredentialAutofillController` (`browser/native/passwords/`).
- Refer to [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) to understand current ownership and lifecycles.

### Code Style & Conventions
- **C++ Standard:** Modern C++20 with standard RAII idiom.
- **Indentation:** 2 spaces, no tabs.
- **Qt Conventions:** Use `QStringLiteral`, `QLatin1String`, `QLatin1Char`, and `QPointer` for safe widget/page tracking.
- **Naming:** CamelCase for classes (`BrowserWindow`, `CredentialVault`), lowerCamelCase for methods (`addNewTab()`, `canonicalHttpsOrigin()`), trailing underscore for private members (`view_`, `vaultManager_`).
- **No Sensitive Logging:** Never write passwords, master passwords, crypto keys, device secrets, fill tokens, or raw microphone data to logs, stderr, stdout, or crash dumps.

---

## 4. Testing Your Changes

Every change must be validated against automated tests before opening a pull request.

### Targeted Testing
While developing, run only the targeted test executable for the affected subsystem:
```bash
# Example: Running password manager tests
./build-dalinira/dalinira-browser-password-autofill-test

# Example: Running download manager tests
./build-dalinira/dalinira-browser-general-download-manager-test
```

CTest regexes and labels are also available, for example `ctest --test-dir
build-dalinira -L security --output-on-failure` and `ctest --test-dir build-dalinira -R
'tab|step5' --output-on-failure`. Tests must use temporary settings/profile
paths and deterministic local fixtures; they must never modify a contributor's
normal DaliNira profile or require public internet access.

### Full CTest Suite
Before submitting, run the complete test suite:
```bash
ctest --test-dir build-dalinira --output-on-failure
```
All registered test suites must pass. For detailed testing practices, CI headless environments, and test categories, read [docs/TESTING.md](docs/TESTING.md).

---

## 5. Security-Sensitive Areas

Changes to the following subsystems receive heightened security scrutiny:

1. **Password Manager & Vault (`browser/native/passwords/`):**
   - Must never weaken AES-256-GCM encryption or PBKDF2-HMAC-SHA256 (600k) key derivation.
   - Master wrap keys must use `DeviceKeyring` binding when available.
   - Passwords in memory must be wiped with `CredentialSecret::wipe()` immediately after use.
   - Never persist plaintext passwords to disk or temporary files.
2. **Credential Autofill & Script Injection:**
   - All injected scripts must execute strictly in `QWebEngineScript::ApplicationWorld`.
   - Native autofill actions must require verified, single-use fill tokens.
   - Canonical origin normalization must strictly match schemes and ports (`https://` only, never `http://`).
3. **Private Browsing Isolation:**
   - Private windows must use ephemeral, isolated directories for storage.
   - Normal profile data (history, bookmarks, persistent passwords) must never leak into or out of a private browsing session.
4. **Qt WebEngine Lifetime:**
   - WebEngine objects (`QWebEnginePage`, `QWebEngineView`) destruction is asynchronous. Use `QPointer` guards and avoid raw pointer dereferences across event loop iterations or JavaScript callbacks.

---

## 6. Reporting Issues

### Bug Reports
When submitting a bug report via [GitHub Issues](https://github.com/Muhammed-Dali/DaliNira-Browser/issues), please include:
- Exact DaliNira Browser version / Git commit hash.
- Linux distribution, desktop environment (KDE Plasma, GNOME, etc.), and display server (Wayland / X11).
- Clear, numbered reproduction steps.
- Expected behavior vs actual behavior observed.
- Relevant terminal logs (with all personal information and credentials redacted).

### Performance Reports
For reporting performance issues (CPU spikes, memory leaks, GPU frame drops):
- Hardware specs (CPU, RAM, GPU, graphics driver).
- Measurement method (e.g. `htop`, `ps`, `nvidia-smi`, `radeontop`).
- Specific URLs or workloads where the issue occurs.
