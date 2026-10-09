# WebKit media runtime

DaliNira's WebKit engine is WebKitGTK (`webkit2gtk-4.1`). WebKitGTK plays HTML5
audio/video through GStreamer, and GStreamer plugins are separate packages. The
WebKitGTK packages on Arch, Debian, Ubuntu and Fedora only *recommend* them, so
a system can have WebKit without a working media runtime. DaliNira's packages
therefore depend on the runtime explicitly: installing DaliNira is the only
step, and YouTube, HTML5 video and HTML5 audio work on first launch.

## What WebKitGTK 2.52 needs

These are the elements WebKitGTK's media player, its MSE append pipeline and its
video sink create, plus the decoders and MSE parsers for the formats YouTube
serves. The list was checked against the `webkitgtk-2.52.6` sources
(`GStreamerRegistryScanner.cpp`, `mse/AppendPipeline.cpp`,
`GStreamerCommon.cpp`, `GLVideoSinkGStreamer.cpp`). `WebKitMediaRuntime`
(`browser/native/engine/webkit/webkit_media_runtime.cpp`) checks the same list
against the live GStreamer registry.

| Element | GStreamer plugin | Upstream module | Why |
|---|---|---|---|
| `autoaudiosink` | autodetect | gst-plugins-good | Audio output. **Fatal if missing:** WebKit hits `RELEASE_ASSERT` in `MediaPlayerPrivateGStreamer::createAudioSink` and the page goes blank. |
| `playbin`, `parsebin` | playback | gst-plugins-base | Media pipeline, MSE demuxing |
| `appsrc`, `appsink` | app | gst-plugins-base | MSE / network sources |
| `audioconvert`, `audioresample`, `volume` | (same names) | gst-plugins-base | Audio path |
| `videoconvert`, `videoscale` | videoconvertscale | gst-plugins-base | Video path |
| `glupload`, `glcolorconvert` | opengl | gst-plugins-base | GPU video sink |
| `typefind` | coreelements | gstreamer | Container detection |
| `scaletempo` | audiofx | gst-plugins-good | `playbackRate` changes |
| `matroskademux` | matroska | gst-plugins-good | WebM: YouTube VP9 + Opus (MSE creates it directly) |
| `qtdemux` | isomp4 | gst-plugins-good | MP4: YouTube H.264/AAC (MSE creates it directly) |
| `aacparse` | audioparsers | gst-plugins-good | AAC streams (MSE creates it directly) |
| `vp9dec` or `avdec_vp9` | vpx / libav | gst-plugins-good / gst-libav | VP9 |
| `opusdec` | opus | gst-plugins-base | Opus |
| `avdec_h264` or `openh264dec` or `vah264dec` | libav / openh264 / va | gst-libav / Fedora: gstreamer1-plugin-openh264 | H.264 |
| `avdec_aac` or `fdkaacdec` or `faad` | libav / fdkaac / faad | gst-libav / gst-plugins-bad | AAC |
| `opusparse` | opusparse | gst-plugins-bad | **MSE Opus:** in MSE mode WebKit advertises Opus only when an Opus parser exists, so without it YouTube gets no Opus audio |
| `vp9parse`, `h264parse` | videoparsersbad | gst-plugins-bad | MSE track metadata for VP9 and H.264; without them WebKit falls back to `identity` and warns of "problems processing some MSE streams" |

gst-plugins-bad is therefore a hard dependency, not an optional extra. It also
adds AV1 parsing and VA-API hardware decoders, which WebKit uses when present.

Only the missing audio sink is fatal. Every other gap only makes the affected
format report "unsupported" (`canPlayType() == ""`), and sites then fall back to
other formats. DaliNira logs any gap once at startup. If the fatal gap is
present, it disables media in the WebKit settings and blocks the legacy
`Audio()` factory, so pages keep working and there is no web process crash or
restart loop. Media is never faked: pages see media as unsupported. Once the
runtime is installed, the next start enables media normally.

### Checking an installation

```
dalinira-browser --check-media-runtime
```

This prints every missing element with the package that provides it and needs
no display. Exit status: `0` complete, `2` playable but some formats
unsupported, `1` HTML5 media disabled. The release workflow runs it after
installing each native package into a clean container.

## Runtime dependencies per distribution

### Arch Linux / CachyOS / AUR

`packaging/*/PKGBUILD` `depends` (in addition to Qt/FFmpeg/OpenSSL):

```
webkit2gtk-4.1 gst-plugins-base gst-plugins-good gst-plugins-bad gst-libav
```

Arch splits the plugins differently from upstream. Most gst-plugins-base
elements, including the OpenGL sink, live in `gst-plugins-base-libs`, and
`vp9parse`/`h264parse` live in `gst-plugins-bad-libs`. `webkit2gtk-4.1` already
depends on both. `gst-plugins-base` adds `alsasink` and `opusdec`.
`gst-plugins-good` adds `autoaudiosink`, `pulsesink`, the demuxers and
`vp9dec`. `gst-plugins-bad` adds `opusparse`. `gst-plugin-pipewire` is optional:
`autoaudiosink` works through PipeWire's PulseAudio server. `libpsl` is linked
directly. The source package (`packaging/archlinux`) also depends on
`libsecret`, which its build links; the Debian-built release archive that the
binary packages repack does not link it. `qt6-wayland` is an `optdepends` for
the Chromium engine's opt-in native Wayland mode.

### Debian 12 / Ubuntu 24.04

Native `.deb` packages, one per release, built with CPack
(`packaging/linux/cpack.cmake`). Library dependencies (Qt, QtWebEngine,
WebKitGTK, GStreamer core, OpenSSL, libpsl) come from `dpkg-shlibdeps`. The
runtime plugins are declared explicitly:

```
Depends: gstreamer1.0-plugins-base, gstreamer1.0-plugins-good, gstreamer1.0-plugins-bad,
 gstreamer1.0-libav, gstreamer1.0-gl, gstreamer1.0-alsa, libqt6webenginecore6-bin,
 qt6-image-formats-plugins, ffmpeg
Recommends: gstreamer1.0-pipewire
Suggests: qt6-wayland
```

Debian and Ubuntu split the OpenGL plugin into `gstreamer1.0-gl` and ALSA into
`gstreamer1.0-alsa`. `pulsesink` is part of `gstreamer1.0-plugins-good`. The Qt
xcb platform plugin is in `libqt6gui6` (`libqt6gui6t64` on Ubuntu), which the
library dependencies already bring in. Install with
`sudo apt install ./dalinira-browser_<version>-1.<release>_amd64.deb` (package
version `<version>-1~<release>`; the file name has no `~`, which GitHub release
assets rename).

### Fedora

A native `.rpm`, built with CPack in a Fedora container. Library requirements
come from rpmbuild's automatic soname dependencies. Runtime plugins:

```
Requires: gstreamer1-plugins-base, gstreamer1-plugins-good, gstreamer1-plugins-bad-free,
 gstreamer1-plugin-libav, qt6-qtimageformats, /usr/bin/ffmpeg
Recommends: gstreamer1-plugin-openh264
Suggests: qt6-qtwayland
```

`/usr/bin/ffmpeg` is a file requirement, so Fedora's `ffmpeg-free` or RPM
Fusion's `ffmpeg` (which replaces it) both satisfy it.

Fedora's `gstreamer1-plugins-base` includes the OpenGL and ALSA plugins, and
`gstreamer1-plugins-bad-free` includes `opusparse` and the video parsers.
`gstreamer1-plugin-libav` is built on `ffmpeg-free`, which has no H.264 decoder,
so H.264 comes from `gstreamer1-plugin-openh264` in the Cisco repository
(enabled by default on Fedora Workstation). It is a weak dependency so the
install never fails without that repository. YouTube then plays VP9, and
`--check-media-runtime` exits 2. Install with
`sudo dnf install ./dalinira-browser-<version>-1.<dist>.x86_64.rpm`.

The package placements above were checked against the Debian bookworm,
Ubuntu noble and Fedora 43/44 package lists.

### Validation in CI

`.github/workflows/packages.yml` builds every package in its distribution's
container (the release workflow calls it) and installs each one into a fresh
container with the package manager alone, then runs
`packaging/linux/validate-installed-package.sh`: installed layout and desktop
entry, shared-library resolution of every installed ELF file, `--version`,
`--check-media-runtime` (0 required; Fedora also accepts 2) and an offscreen
start of the browser.

### Portable archive

`dalinira-browser-<version>-linux-x86_64.tar.zst` has no dependency metadata.
The native packages above are the supported installation on Debian, Ubuntu and
Fedora. With the archive, install the listed runtime packages yourself.
`--check-media-runtime` names anything missing.

## Bundling instead of depending

Bundling GStreamer plugins inside DaliNira was considered and rejected.
Plugins are loaded into WebKit's web process, and they must match the system
`libgstreamer-1.0` and the `gst-plugins-base` libraries that
`libwebkit2gtk-4.1` itself links against. A bundled plugin built for a
different GStreamer version crashes the web process; the old
`DALINIRA_DEV_GST_FALLBACK` plugins did exactly that. Bundling safely means
bundling GStreamer and WebKitGTK together, which is what Flatpak does with the
GNOME runtime. For native packages, the package manager dependencies above are
the correct and safe mechanism.

## Verification

`browser-webkit-media` (`browser/native/tests/webkit_media_test.cpp`) is in the
GUI tier: run it with `scripts/safe-gui-test.sh browser-webkit-media`. It uses
one window and one view at a time (see docs/TESTING.md).

- Evaluates the requirement policy with and without each fatal and non-fatal gap.
- Checks that the WebKit profile settings follow the detected runtime.
- Without the runtime: media reports unsupported, `Audio()` throws
  NotSupportedError, and the page stays alive.
- With the runtime, over HTTP with byte ranges:
  - VP9/Opus playback, with frames decoded and a WebKitWebProcess sink-input
    present on the sound server;
  - pause and resume, and seeking backward and forward;
  - MSE SourceBuffer playback;
  - playback continuing in a background tab;
  - closing a playing tab, with its web process exiting cleanly;
  - shutting down while playing, with every WebKit process exiting.
- `--manual` adds fullscreen enter/exit, which goes through the window manager.
- `--live` (`DALINIRA_LIVE_MEDIA_TEST=1`) adds a live YouTube run: MSE
  playback, pause, seek, tab switch and close (plus fullscreen with `--manual`).

## Video import and Web Audio latency

The WebKit profile defaults `WEBKIT_GST_DMABUF_SINK_DISABLED=1` before starting
web processes, unless the caller explicitly sets it. This selects the
GStreamer GL video sink's `glupload` / `glcolorconvert` path rather than direct
DMA-BUF video-plane import. Hardware decoding and page compositing remain
enabled. It does **not** set `WEBKIT_DISABLE_DMABUF_RENDERER`. The reference
implementation is [WebKitGTK 2.52.6's GL video sink](https://github.com/WebKit/WebKit/blob/webkitgtk-2.52.6/Source/WebCore/platform/graphics/gstreamer/GLVideoSinkGStreamer.cpp).

Effects request interactive output latency on WebKit; Chromium retains its
existing playback latency hint. WebKitGTK 2.52's
[Web Audio destination](https://github.com/WebKit/WebKit/blob/webkitgtk-2.52.6/Source/WebCore/platform/audio/gstreamer/AudioDestinationGStreamer.cpp)
uses a separate pipeline and sets the auto-detected audio sink's buffer to
100 ms, independent of that hint. DaliNira's web-process extension observes
GStreamer's bin additions and caps only audio sinks in `audio-destination`
pipelines, before PLAYING, at 30 ms buffer time and 10 ms latency time. It
preserves smaller buffers, normal media sinks, automatic device selection,
and pipeline clock synchronization. These values are output buffering
requests, not a guarantee of total measured A/V delay. The extension's media
policy also loads when network filtering is unavailable.

`browser-webkit-engine-logic` checks the scoped sink policy without opening an
audio device. `browser-web-audio-engine-unit` checks engine-specific hints;
`browser-webkit-media` and `browser-web-audio-engine-gui` check real local media
and effects lifecycle. Before release, use HD YouTube on the affected GPU and
audio device to check color/flicker and lip-sync with effects off/on, seeking,
pause/resume, background tabs, and fullscreen.
