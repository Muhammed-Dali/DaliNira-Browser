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

DaliNira's WebKit engine runs on its own patched WebKitGTK, which is built from
source in `build-webkit-runtime/` the first time you build (see
[section 4](#4-first-clone-build-once-per-checkout)). Install both groups below:
the DaliNira application dependencies and the WebKitGTK build dependencies.

### Arch Linux / Manjaro
```bash
# Base build tools and compilers
sudo pacman -S --needed base-devel cmake ninja git nodejs pkgconf openssl libpsl

# Qt 6 libraries
sudo pacman -S --needed qt6-base qt6-webengine qt6-svg qt6-imageformats

# WebKitGTK's libraries and its media runtime (see docs/webkit-media-runtime.md).
# The system webkit2gtk-4.1 package brings in every library WebKitGTK links
# against; DaliNira itself loads its private build, not this package.
sudo pacman -S --needed webkit2gtk-4.1 gst-plugins-base gst-plugins-good gst-plugins-bad gst-libav

# Tools for building the private WebKitGTK runtime
sudo pacman -S --needed clang lld glib2-devel gperf python ruby ruby-stdlib unifdef wayland-protocols

# Optional security and media packages
sudo pacman -S --needed libsecret ffmpeg yt-dlp pipewire-pulse
```

### Debian 12 / Ubuntu 24.04
```bash
sudo apt update
# Base build tools and compilers
sudo apt install -y build-essential cmake ninja-build git nodejs pkg-config libssl-dev libpsl-dev

# Qt 6 libraries
sudo apt install -y qt6-base-dev qt6-webengine-dev qt6-svg-dev qt6-image-formats-plugins

# WebKitGTK media runtime (see docs/webkit-media-runtime.md)
sudo apt install -y gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-bad \
  gstreamer1.0-libav gstreamer1.0-gl gstreamer1.0-alsa

# Private WebKitGTK runtime build dependencies (the list the package CI uses)
sudo apt install -y --no-install-recommends \
  ca-certificates curl xz-utils zstd file patch cmake ninja-build gperf bison flex ruby unifdef \
  python3 perl pkg-config bubblewrap xdg-dbus-proxy libseccomp-dev libglib2.0-dev \
  libharfbuzz-dev libcairo2-dev libdrm-dev libgbm-dev libepoxy-dev libfontconfig-dev \
  libfreetype-dev libicu-dev libgcrypt20-dev libhyphen-dev liblcms2-dev libmanette-0.2-dev \
  libxslt1-dev libxml2-dev libxml2-utils libsoup-3.0-dev libgtk-3-dev libsqlite3-dev \
  libsystemd-dev libgudev-1.0-dev libwoff-dev libavif-dev libjpeg-dev libopenjp2-7-dev \
  libpng-dev libtasn1-6-dev libwebp-dev libxt-dev libgstreamer1.0-dev \
  libgstreamer-plugins-base1.0-dev libgstreamer-plugins-bad1.0-dev libenchant-2-dev \
  libsecret-1-dev libegl-dev libgl-dev libgles-dev libwayland-dev wayland-protocols \
  libxkbcommon-dev libatspi2.0-dev libexpat1-dev \
  clang lld libsysprof-capture-4-dev libjxl-dev

# Optional security and media packages
sudo apt install -y libsecret-1-dev ffmpeg yt-dlp pulseaudio
```

On Debian 12, replace the last line of the runtime list with `clang-16 binutils`
and start the first build with
`CC=clang-16 CXX=clang++-16 WEBKIT_RUNTIME_CMAKE_ARGS="-DUSE_SYSTEM_SYSPROF_CAPTURE=OFF -DUSE_JPEGXL=OFF"`
in front of `./scripts/build-dalinira.sh`, as the package CI does. The first
build records these settings in `build-webkit-runtime/env.sh`, so later builds
do not need them.

### Fedora 39+
```bash
# Base build tools and compilers
sudo dnf install -y gcc-c++ cmake ninja-build git nodejs pkgconf-pkg-config openssl-devel libpsl-devel

# Qt 6 libraries
sudo dnf install -y qt6-qtbase-devel qt6-qtwebengine-devel qt6-qtsvg-devel qt6-qtimageformats

# WebKitGTK media runtime (see docs/webkit-media-runtime.md)
sudo dnf install -y gstreamer1-plugins-base gstreamer1-plugins-good gstreamer1-plugins-bad-free \
  gstreamer1-plugin-libav

# Private WebKitGTK runtime build dependencies (the list the package CI uses)
sudo dnf install -y \
  curl xz zstd file patch clang lld cmake ninja-build gperf bison flex gettext ruby rubygems \
  rubygem-json 'rubygem(getoptlong)' unifdef python3 perl 'perl(English)' 'perl(FindBin)' \
  'perl(JSON::PP)' 'perl(bigint)' bubblewrap xdg-dbus-proxy libatomic \
  'pkgconfig(atspi-2)' 'pkgconfig(cairo)' 'pkgconfig(egl)' 'pkgconfig(enchant-2)' \
  'pkgconfig(epoxy)' 'pkgconfig(expat)' 'pkgconfig(fontconfig)' 'pkgconfig(freetype2)' \
  'pkgconfig(gbm)' 'pkgconfig(glib-2.0)' 'pkgconfig(gstreamer-1.0)' \
  'pkgconfig(gstreamer-plugins-bad-1.0)' 'pkgconfig(gstreamer-plugins-base-1.0)' \
  'pkgconfig(gtk+-3.0)' 'pkgconfig(harfbuzz)' 'pkgconfig(icu-uc)' 'pkgconfig(lcms2)' \
  'pkgconfig(libavif)' 'pkgconfig(libdrm)' 'pkgconfig(libgcrypt)' 'pkgconfig(libjpeg)' \
  'pkgconfig(libjxl)' 'pkgconfig(libpng)' 'pkgconfig(libseccomp)' 'pkgconfig(libsecret-1)' \
  'pkgconfig(libsoup-3.0)' 'pkgconfig(libsystemd)' 'pkgconfig(libtasn1)' 'pkgconfig(libwebp)' \
  'pkgconfig(libwoff2dec)' 'pkgconfig(libxslt)' 'pkgconfig(manette-0.2)' 'pkgconfig(sqlite3)' \
  'pkgconfig(sysprof-capture-4)' 'pkgconfig(wayland-client)' 'pkgconfig(wayland-egl)' \
  'pkgconfig(wayland-protocols)' 'pkgconfig(wayland-server)' 'pkgconfig(xt)' hyphen-devel

# Optional security and media packages
sudo dnf install -y libsecret-devel ffmpeg-free yt-dlp
```

`./scripts/bootstrap-webkit-runtime.sh --check` reports any missing tool or
development package without downloading or building anything.

Normal builds require both engines and the private patched WebKitGTK runtime.
A CMake configure that finds neither `build-webkit-runtime/prefix/` nor a
package SDK fails and tells you to run `./scripts/build-dalinira.sh`; it never
falls back to the unpatched system WebKitGTK. For an intentional Chromium-only
diagnostic configuration, pass both `-DDALINIRA_REQUIRE_DUAL_ENGINE=OFF` and
`-DDALINIRA_ENABLE_WEBKIT=OFF`.

`-DDALINIRA_ALLOW_SYSTEM_WEBKIT_FOR_TESTS=ON` is a test-only switch for CI's
push/PR test job: it compiles the WebKit backend against the system WebKitGTK.
It is OFF by default, cannot be combined with a package SDK, and a build made
with it cannot be installed or packaged (see [TESTING.md](TESTING.md)). Do not
use it for development or releases.

---

## 4. First clone: build once per checkout

```bash
git clone https://github.com/Muhammed-Dali/DaliNira-Browser.git
cd DaliNira-Browser
./scripts/build-dalinira.sh
```

On a fresh checkout `build-webkit-runtime/` does not exist yet, so
`build-dalinira.sh` first builds DaliNira's private WebKitGTK runtime through
`scripts/bootstrap-webkit-runtime.sh`. Before it starts, it prints what it is
about to do and asks for confirmation. It then:

1. downloads WebKitGTK 2.54.1 from webkitgtk.org and verifies the SHA-256 pinned in
   `packaging/webkit-runtime/webkitgtk-runtime.env`; a mismatching file is
   rejected and nothing is extracted;
2. applies the validated patch in `packaging/webkit-runtime/patches/`
   (`AcceleratedSurface` timer fix) with no fuzz;
3. records the toolchain in `build-webkit-runtime/env.sh`: clang and lld when
   they are installed, without debug info or LTO, as in the validated runtime;
4. configures, builds and installs WebKitGTK into
   `build-webkit-runtime/prefix/`;
5. continues with the normal DaliNira build in `build-dalinira/`.

Cost of the first runtime build:

- **Disk:** about 5 GB in `build-webkit-runtime/` (0.5 GB source, 4.1 GB
  objects, 0.2 GB installed runtime). The bootstrap refuses to start with less
  than 8 GB free.
- **Time:** typically 1.5 to 3 hours at 3 jobs, depending on the CPU.
- **Memory:** WebKit compile jobs are memory-heavy. The build uses at most 3
  parallel jobs, the same limit as every DaliNira build; on a machine with
  little free memory, use `DALINIRA_BUILD_JOBS=1` or `2`.

The system WebKitGTK is never used as a substitute: if the private runtime
cannot be built, the build stops. An interrupted first build resumes where it
stopped when you run the same command again; a partial tree is never
mistaken for a finished one.

Without a terminal (CI, scripts), confirm up front:
```bash
./scripts/build-dalinira.sh --yes        # or DALINIRA_WEBKIT_BOOTSTRAP=yes
```

To check the dependencies and see the plan without changing anything:
```bash
./scripts/bootstrap-webkit-runtime.sh --check
```

---

## 5. Normal development

After the first build, use one command for every change:
```bash
./scripts/build-dalinira.sh
```

Every invocation checks both engine sides, in this order:

1. **WebKit runtime check.** The helper fingerprints the pinned version and
   checksum, the validated patch, the runtime recipe, the CMake arguments and
   `build-webkit-runtime/env.sh`. Unchanged inputs take the no-work path,
   which takes seconds. Changed source inputs are refreshed from the already
   downloaded, verified tarball, and Ninja rebuilds only the affected objects.
2. **DaliNira build.** The dual-engine application is configured when needed
   and built incrementally in `build-dalinira/`.

Developers do not decide which engine a change affects. Shared UI, DRM, media,
privacy, downloads, settings, localization and engine-specific changes all use
the same command. Parallelism defaults to the machine-safe maximum of three;
`DALINIRA_BUILD_JOBS=1` or `2` may lower it, but values above three are rejected.

### Build directories

| Directory | What it is | Can it be deleted? |
| --------- | ---------- | ------------------ |
| `build-dalinira/` | The disposable application build: one DaliNira binary with the Chromium backend and the WebKit integration. | Yes. The next `build-dalinira.sh` rebuilds it. |
| `build-webkit-runtime/` | The expensive private WebKitGTK build and cache: verified tarball, patched source, objects, `env.sh` and the installed runtime in `prefix/`. | Only to rebuild WebKitGTK from scratch, which repeats the first-time build. |

Both directories are listed in `.gitignore`; never commit them.

### Other configurations

Direct CMake target builds are for specialized debugging after a successful
unified build. They do not replace the runtime-first workflow:
```bash
cmake --build build-dalinira --target dalinira-browser -j3
cmake --build build-dalinira --target dalinira-browser-password-autofill-test -j3
```

**Debug build** (debug symbols and compile commands for language servers):
```bash
cmake -S . -B build-dalinira -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DDALINIRA_WEBKIT_DEVELOPMENT_RUNTIME=$PWD/build-webkit-runtime/prefix
```
This is a specialized configuration. Run `scripts/build-dalinira.sh` for the
normal dual-engine workflow.

**Package build (private WebKitGTK runtime SDK).** Release packages ship
DaliNira's own patched WebKitGTK under `/usr/lib/dalinira-browser/webkit/`
(see [packaging/webkit-runtime/README.md](../packaging/webkit-runtime/README.md)).
That runtime is built separately from the development runtime, for its fixed
install prefix. Build the package SDK first and then point CMake at it:
```bash
packaging/webkit-runtime/build-webkit-runtime.sh \
  --sdk build-webkit-runtime/sdk --build-dir build-webkit-runtime/work --jobs 3
cmake -S . -B build-dalinira -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
  -DCMAKE_INSTALL_BINDIR=lib/dalinira-browser \
  -DDALINIRA_WEBKIT_RUNTIME_SDK=$PWD/build-webkit-runtime/sdk
```
The runtime only works at the prefix it was built for. To run WebKit from
such a build without installing it, use
`scripts/with-staged-install.sh STAGE -- COMMAND`, where `STAGE` is a
`DESTDIR` install.

---

## 6. Running the Built Binary

Execute directly from the build directory:
```bash
./build-dalinira/dalinira-browser                 # engine from settings (Chromium by default)
./build-dalinira/dalinira-browser --engine webkit # start with WebKitGTK
./build-dalinira/dalinira-browser --check-webkit-runtime
```

To open a specific website or internal page on startup:
```bash
./build-dalinira/dalinira-browser https://github.com
./build-dalinira/dalinira-browser dalinira://passwords
```

To run with developer diagnostics and Chromium logging to terminal:
```bash
QTWEBENGINE_CHROMIUM_FLAGS="--enable-logging=stderr --v=1" ./build-dalinira/dalinira-browser
```

---

## 7. Installing to the System

The development build loads WebKit from this checkout's
`build-webkit-runtime/prefix/` and does not install a runtime. Use it from the
build directory. A self-contained installation uses the package build from
[section 5](#other-configurations), which installs the private runtime under
`/usr/lib/dalinira-browser/webkit/`:
```bash
sudo cmake --install build-dalinira
```
Release packages (`.deb`, `.rpm`, Arch, portable archive) are produced by the
workflows in `.github/workflows/packages.yml`.
