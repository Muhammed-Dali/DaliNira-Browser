# Building DaliNira Browser

This guide details the system prerequisites, dependencies, compilation steps, and optional feature integrations for building DaliNira Browser from source on Linux.

---

## 1. System Requirements

- **Operating System:** Linux x86_64 (Wayland or X11).
- **Compiler:** C++20 compliant compiler:
  - GCC 11.2 or newer, or
  - Clang 14 or newer
- **Build Tools:** CMake 3.20 or newer, Ninja build system, pkg-config.

---

## 2. Dependency Breakdown

### Required Build Dependencies
These libraries and tools must be present to configure and compile DaliNira Browser:

| Component | Minimum Version | Purpose in DaliNira |
| --------- | --------------- | ----------------- |
| **Qt 6** | >= 6.4 | Core GUI, Widgets, WebEngine, Network, SVG, DBus, Concurrent |
| **Qt WebEngine** | >= 6.4 | Chromium-based browser engine and rendering pipeline |
| **OpenSSL** | >= 1.1.1 / 3.0 | AES-256-GCM vault encryption, PBKDF2 key derivation, TLS |
| **libpsl** | >= 0.21 | Public Suffix List domain validation and cookie isolation |
| **Node.js** | >= 16 | Compiles DALI Lang Web Audio DSP modules into JavaScript |
| **PkgConfig** | Any modern | Locates system libraries during CMake configuration |

### Optional Build Dependencies
| Library | Feature Affected | Fallback Behavior if Missing |
| ------- | ---------------- | ---------------------------- |
| **libsecret-1** (`libsecret-dev` / `libsecret-devel`) | Hardware-bound credential vault (`DeviceKeyring`) | Automatically detected. If absent, DaliNira compiles with standard PBKDF2 master password derivation without OS session keyring binding. |

### Optional Runtime Tools
| Tool | Feature Affected | Fallback Behavior if Missing |
| ---- | ---------------- | ---------------------------- |
| **ffmpeg** | Media Downloads / Player (`dalinira://player`) | Downloaded audio/video streams cannot be muxed or converted. Direct video streams play if supported natively. |
| **yt-dlp** | Video Link Extraction (`MediaDownloadService`) | Media download analysis will notify user that yt-dlp is required. |
| **pulseaudio** / **pipewire-pulse** | DaliNira Pulse (`dalinira://pulse`) | System audio stream capture cannot record system output; microphone capture works if ALSA/Pulse device is present. |
| **qtwebengine_dictionaries** | Spell Checking | Spell checking is disabled with a non-fatal terminal warning. |
| **VA-API / GPU Drivers** (`libva-intel-driver`, `mesa-va-drivers`) | Hardware Video Decoding | Falls back to CPU software decoding automatically. |

---

## 3. Installing Dependencies by Distribution

### Arch Linux / Manjaro
```bash
# Base build tools and compilers
sudo pacman -S --needed base-devel cmake ninja git nodejs pkgconf openssl libpsl

# Qt 6 libraries
sudo pacman -S --needed qt6-base qt6-webengine qt6-svg qt6-imageformats

# WebKitGTK engine and its media runtime (see docs/webkit-media-runtime.md)
sudo pacman -S --needed webkit2gtk-4.1 gst-plugins-base gst-plugins-good gst-plugins-bad gst-libav

# Optional security and media packages
sudo pacman -S --needed libsecret ffmpeg yt-dlp pipewire-pulse
```

### Ubuntu 24.04+ / Debian 13+
```bash
sudo apt update
# Base build tools and compilers
sudo apt install -y build-essential cmake ninja-build git nodejs pkg-config libssl-dev libpsl-dev

# Qt 6 libraries
sudo apt install -y qt6-base-dev qt6-webengine-dev libqt6svg6-dev libqt6webenginewidgets6

# WebKitGTK engine and its media runtime (see docs/webkit-media-runtime.md)
sudo apt install -y libwebkit2gtk-4.1-dev libgstreamer1.0-dev gstreamer1.0-plugins-base \
  gstreamer1.0-plugins-good gstreamer1.0-plugins-bad gstreamer1.0-libav

# Optional security and media packages
sudo apt install -y libsecret-1-dev ffmpeg yt-dlp pulseaudio
```

### Fedora 39+
```bash
# Base build tools and compilers
sudo dnf install -y gcc-c++ cmake ninja-build git nodejs pkgconf-pkg-config openssl-devel libpsl-devel

# Qt 6 libraries
sudo dnf install -y qt6-qtbase-devel qt6-qtwebengine-devel qt6-qtsvg-devel

# WebKitGTK engine and its media runtime (see docs/webkit-media-runtime.md)
sudo dnf install -y webkit2gtk4.1-devel gstreamer1-devel gstreamer1-plugins-good gstreamer1-plugins-bad-free \
  gstreamer1-plugin-libav

# Optional security and media packages
sudo dnf install -y libsecret-devel ffmpeg-free yt-dlp
```

Without WebKitGTK the build continues with the Chromium engine only;
`-DDALINIRA_ENABLE_WEBKIT=OFF` disables the WebKitGTK engine explicitly.

---

## 4. Compilation Instructions

### Step 1: Clone Repository
```bash
git clone https://github.com/Muhammed-Dali/DaliNira-Browser.git
cd DaliNira-Browser
```

### Step 2: Configure with CMake

#### Standard Release Build (Recommended)
Optimized build with strict assertions enabled:
```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
```

#### Debug Build (For Development & Profiling)
Generates debug symbols and export compile commands for language servers:
```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

### Step 3: Build the Targets

Compile using all available CPU cores:
```bash
cmake --build build -j$(nproc)
```

To build only the main browser executable:
```bash
cmake --build build --target dalinira-browser -j$(nproc)
```

To build a specific test executable:
```bash
cmake --build build --target dalinira-browser-password-autofill-test -j$(nproc)
```

---

## 5. Running the Built Binary

Execute directly from the build directory:
```bash
./build/dalinira-browser
```

To open a specific website or internal page on startup:
```bash
./build/dalinira-browser https://github.com
./build/dalinira-browser dalinira://passwords
```

To run with developer diagnostics and Chromium logging to terminal:
```bash
QTWEBENGINE_CHROMIUM_FLAGS="--enable-logging=stderr --v=1" ./build/dalinira-browser
```

---

## 6. Installing to System (Optional)

To install DaliNira Browser to the standard system prefix (`/usr/local` by default):
```bash
sudo cmake --install build
```

To specify a custom prefix (e.g. `/usr`):
```bash
cmake -S . -B build -G Ninja -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
sudo cmake --install build
```
