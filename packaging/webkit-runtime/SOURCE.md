# DaliNira private WebKitGTK runtime: source and licenses

DaliNira's WebKit engine uses a private copy of WebKitGTK, installed under
`/usr/lib/dalinira-browser/webkit/`. Only DaliNira loads it; the system's own
WebKitGTK, if any, is not touched.

## What it is

- Upstream WebKitGTK 2.54.1, unmodified except for the patch listed below.
  - Release tarball: <https://webkitgtk.org/releases/webkitgtk-2.54.1.tar.xz>
  - SHA-256: `ea0bbb02dbdbc596874a4e7ad35b66645b3e0a232bd0e4081de5ed92eb0a397d`
- One DaliNira change, in `patches/` next to this file:
  `0001-AcceleratedSurface-destroy-release-buffers-timer-synchronously.patch`.
  It modifies `Source/WebKit/WebProcess/WebPage/CoordinatedGraphics/AcceleratedSurface.cpp`
  so the web process no longer aborts when a hidden page's compositor is
  torn down. The patch header gives the date of the change and the reason.
- `BUILD-INFO` records the exact source checksum, patch checksums, build
  machine, compiler and the build IDs of the shipped libraries.

## Licenses

WebKitGTK is free software. The libraries are covered mainly by the GNU
Lesser General Public License version 2.1 or later (WebCore, JavaScriptCore)
and by BSD-style licenses (WebKit, WTF). They include third-party code such
as Skia, ANGLE, pdf.js, xdgmime, simdutf and fast_float, under their own
licenses. `LICENSE.WebKitGTK` contains every license and copying file from
the release tarball, each headed by its path in the source tree.
`COPYRIGHT.debian` lists the copyright holders and licenses per source file
of this release, as audited by Debian's webkit2gtk 2.54.1-1 package.
`README.xdgmime` states the xdgmime license (AFL-2.0 or LGPL-2.0-or-later).

## Corresponding source

The complete corresponding source of this runtime is:

1. the upstream release tarball above;
2. the patches in `patches/`;
3. the build recipe, `packaging/webkit-runtime/` in the DaliNira source
   repository at the matching release tag
   (<https://github.com/Muhammed-Dali/DaliNira-Browser>).

Each DaliNira release also publishes the tarball, the patches and the recipe
as release assets next to the binary packages, at
<https://github.com/Muhammed-Dali/DaliNira-Browser/releases>.

## Rebuilding or replacing the library

DaliNira links WebKitGTK dynamically. To use a modified WebKitGTK, rebuild it
with `packaging/webkit-runtime/build-webkit-runtime.sh` (after changing the
source or the patches) and replace the files under
`/usr/lib/dalinira-browser/webkit/`. The build must use the prefix
`/usr/lib/dalinira-browser/webkit`, because WebKitGTK compiles in the paths
of its helper processes.
