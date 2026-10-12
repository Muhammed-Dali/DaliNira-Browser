# Native Debian/Ubuntu (.deb) and Fedora (.rpm) packages of DaliNira.
#
# Built by .github/workflows/release.yml inside each distribution's own
# container, so library dependencies match that release:
#
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
#         -DCMAKE_INSTALL_BINDIR=lib/dalinira-browser -DDALINIRA_PACKAGE_DISTRO=debian12 \
#         -DDALINIRA_WEBKIT_RUNTIME_SDK=/path/to/distribution-sdk
#   cmake --build build
#   cd build && cpack -G DEB        # or -G RPM for fedora
#
# Shared libraries the binaries link (Qt, QtWebEngine, GStreamer core,
# OpenSSL, libpsl, and the libraries of the private WebKitGTK runtime: GTK,
# libsoup, ICU, ...) become dependencies automatically: dpkg-shlibdeps for
# .deb, rpmbuild's soname requirements for .rpm. WebKitGTK itself is not a
# dependency: release packages ship DaliNira's private, patched build of it
# under /usr/lib/dalinira-browser/webkit (packaging/webkit-runtime). What neither tool can see is
# declared below: plugins that are loaded at runtime, above all the GStreamer
# plugins WebKitGTK needs for HTML5 audio/video and YouTube. The distributions'
# WebKitGTK packages only recommend or suggest them, so without these entries
# a system could have DaliNira without working media.
# docs/webkit-media-runtime.md lists which element comes from which package.
#
# The Arch Linux / AUR equivalents are packaging/*/PKGBUILD `depends`.

set(DALINIRA_PACKAGE_DISTRO "" CACHE STRING
    "Distribution the cpack package targets: debian12, ubuntu2404 or fedora")
if(NOT DALINIRA_PACKAGE_DISTRO STREQUAL "" AND DALINIRA_BUILD_WEBKIT AND
   NOT DALINIRA_WEBKIT_RUNTIME_SDK)
  message(FATAL_ERROR
    "Distribution packages require DALINIRA_WEBKIT_RUNTIME_SDK; a developer runtime path must never enter a package")
endif()

set(CPACK_PACKAGE_NAME "dalinira-browser")
set(CPACK_PACKAGE_VENDOR "Muhammed Dali")
set(CPACK_PACKAGE_CONTACT "Muhammed Dali <dali.nira.12.1453kerinyoli@gmail.com>")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Native Qt/C++ desktop browser with integrated privacy and media tools")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/Muhammed-Dali/DaliNira-Browser")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGING_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")
set(CPACK_STRIP_FILES ON)

# Runtime plugins, per packaging family (verified against the Debian 12,
# Ubuntu 24.04 and Fedora 43 file lists):
#   GStreamer plugins-base: playbin/parsebin, appsrc/appsink, audio/video
#     conversion, opusdec (Debian/Ubuntu: OpenGL video sink and ALSA output
#     are split into gstreamer1.0-gl and gstreamer1.0-alsa);
#   plugins-good: autoaudiosink (WebKit aborts the web process without it),
#     pulsesink, WebM/MP4 demuxers, AAC parser, VP9 decoder, scaletempo;
#   plugins-bad: opusparse (MSE Opus, YouTube audio), vp9parse/h264parse;
#   libav: H.264 and AAC decoders (Fedora's FFmpeg build lacks H.264, which
#     comes from openh264 in the Cisco repository, hence only recommended).
#   Qt: image format plugins (WebP/AVIF), the QtWebEngineProcess helper. The
#     SVG icon engine ships with the Qt SVG library itself (libqt6svg6,
#     qt6-qtsvg), which the soname dependencies already pull in.
#   ffmpeg: the media download service runs the ffmpeg tool. Fedora requires
#     the file, so either Fedora's ffmpeg-free or RPM Fusion's ffmpeg works.
#   Suggested only: the Qt Wayland platform plugin, for the Chromium engine's
#     opt-in native Wayland mode (QT_QPA_PLATFORM=wayland); DaliNira uses
#     XWayland by default and WebKit always does.

# ---- Debian 12 and Ubuntu 24.04 (identical package names) ----
set(CPACK_DEBIAN_PACKAGE_SECTION "web")
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")
# File name set per distribution below.
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
# The private WebKit web-process extension is not a public library.
set(CPACK_DEBIAN_PACKAGE_GENERATE_SHLIBS OFF)
# The private WebKitGTK libraries, relative to the package root (CPack runs
# dpkg-shlibdeps there): found in the package, so no system WebKitGTK
# dependency; their own library dependencies still become Depends.
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS_PRIVATE_DIRS "lib/dalinira-browser/webkit/lib")
if(NOT CMAKE_INSTALL_PREFIX STREQUAL "/")
  string(REGEX REPLACE "^/" "" _dalinira_prefix_rel "${CMAKE_INSTALL_PREFIX}")
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS_PRIVATE_DIRS "${_dalinira_prefix_rel}/lib/dalinira-browser/webkit/lib")
endif()
# bubblewrap, xdg-dbus-proxy: the private runtime's web-process sandbox runs
# /usr/bin/bwrap and /usr/bin/xdg-dbus-proxy, which dpkg-shlibdeps cannot see
# (libwebkit2gtk-4.1-0 depends on them for the same reason).
set(CPACK_DEBIAN_PACKAGE_DEPENDS
    "bubblewrap, xdg-dbus-proxy, gstreamer1.0-plugins-base, gstreamer1.0-plugins-good, gstreamer1.0-plugins-bad, gstreamer1.0-libav, gstreamer1.0-gl, gstreamer1.0-alsa, libqt6webenginecore6-bin, qt6-image-formats-plugins, ffmpeg")
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "gstreamer1.0-pipewire")
set(CPACK_DEBIAN_PACKAGE_SUGGESTS "qt6-wayland")

# ---- Fedora ----
set(CPACK_RPM_PACKAGE_LICENSE "GPL-3.0-only")
set(CPACK_RPM_PACKAGE_GROUP "Applications/Internet")
set(CPACK_RPM_PACKAGE_ARCHITECTURE "x86_64")
set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
set(CPACK_RPM_PACKAGE_AUTOREQ ON)
# Don't advertise the private WebKit extension module or the private
# WebKitGTK libraries as provided libraries.
set(CPACK_RPM_PACKAGE_AUTOPROV OFF)
# ... and don't require the system's copies of the private WebKitGTK
# libraries: the package ships them (rpm's dependency generator honours this).
set(CPACK_RPM_SPEC_MORE_DEFINE "%global __requires_exclude ^lib(webkit2gtk|javascriptcoregtk)-4\\.1\\.so")
# bubblewrap, xdg-dbus-proxy: see the Debian dependencies above.
set(CPACK_RPM_PACKAGE_REQUIRES
    "bubblewrap, xdg-dbus-proxy, gstreamer1-plugins-base, gstreamer1-plugins-good, gstreamer1-plugins-bad-free, gstreamer1-plugin-libav, qt6-qtimageformats, /usr/bin/ffmpeg")
set(CPACK_RPM_PACKAGE_RECOMMENDS "gstreamer1-plugin-openh264")
set(CPACK_RPM_PACKAGE_SUGGESTS "qt6-qtwayland")
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
    /usr/share/applications /usr/share/icons /usr/share/icons/hicolor)

if(DALINIRA_PACKAGE_DISTRO MATCHES "^(debian12|ubuntu2404)$")
  # Package version 8.0.2-1~debian12 ("~" sorts before a distribution's own
  # 8.0.2-1). The file name avoids "~", which GitHub release assets rename:
  # dalinira-browser_8.0.2-1.debian12_amd64.deb
  set(CPACK_DEBIAN_PACKAGE_RELEASE "1~${DALINIRA_PACKAGE_DISTRO}")
  set(CPACK_DEBIAN_FILE_NAME "${CPACK_PACKAGE_NAME}_${CPACK_PACKAGE_VERSION}-1.${DALINIRA_PACKAGE_DISTRO}_amd64.deb")
elseif(DALINIRA_PACKAGE_DISTRO STREQUAL "fedora")
  set(CPACK_RPM_PACKAGE_RELEASE "1")
  set(CPACK_RPM_PACKAGE_RELEASE_DIST ON)  # adds %{?dist}, e.g. .fc43
elseif(NOT DALINIRA_PACKAGE_DISTRO STREQUAL "")
  message(FATAL_ERROR "DALINIRA_PACKAGE_DISTRO must be debian12, ubuntu2404 or fedora")
else()
  set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
endif()

include(CPack)
