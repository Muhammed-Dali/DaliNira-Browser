# Private WebKitGTK runtime

DaliNira's WebKit engine runs on its own build of WebKitGTK, not on the
system's `webkit2gtk-4.1`. The build is the upstream release plus the patches in
`patches/`. It is installed under `/usr/lib/dalinira-browser/webkit/` and only
DaliNira loads it. Nothing in `/usr/lib`, the system WebKitGTK, GTK or Mesa is
replaced, and no environment variable is involved.

Why: WebKitGTK 2.54.1 aborts a web process in about 2 % of DaliNira window
lifetimes (`AcceleratedSurface` destroys an active `RunLoop` timer on the wrong
thread). `patches/0001-…` fixes it. It was validated with 0 crashes in 500
window lifetimes, against an unpatched control of 3 crashes in 40 (see
`docs/research/webkit-exit-crash/`). There is no upstream fix yet.

## Layout

```
/usr/lib/dalinira-browser/dalinira-browser            DT_RPATH=/usr/lib/dalinira-browser/webkit/lib
/usr/lib/dalinira-browser/webkit-extensions/…         DaliNira's web process extension
/usr/lib/dalinira-browser/webkit/
  BUILD-INFO                                          version, source/patch checksums, build IDs
  lib/libwebkit2gtk-4.1.so.0[.24.4]                   DT_RPATH=<prefix>/lib
  lib/libjavascriptcoregtk-4.1.so.0[.11.8]
  lib/webkit2gtk-4.1/injected-bundle/libwebkit2gtkinjectedbundle.so
  libexec/webkit2gtk-4.1/WebKitWebProcess             DT_RPATH=<prefix>/lib
  libexec/webkit2gtk-4.1/WebKitNetworkProcess
  libexec/webkit2gtk-4.1/WebKitGPUProcess
  share/locale/*/LC_MESSAGES/WebKitGTK-4.1.mo
/usr/share/doc/dalinira-browser/webkitgtk/            LICENSE.WebKitGTK, SOURCE.md, BUILD-INFO, patches/
```

How the private copy is used, and the system copy never is:

- WebKitGTK compiles absolute paths into `libwebkit2gtk`: the helper-process
  directory (`PKGLIBEXECDIR`), the injected bundle (`PKGLIBDIR`) and the
  locale directory. The runtime is therefore built for exactly this prefix,
  and DaliNira's CMake refuses an SDK built for another one.
- The browser binary and every runtime ELF carry `DT_RPATH` (not
  `DT_RUNPATH`; both WebKit and DaliNira link with `--disable-new-dtags`).
  `DT_RPATH` is searched before `LD_LIBRARY_PATH`, so a user's environment
  cannot swap in the system library. It is per binary, so no other program
  is affected.
- The helpers load the private library through their own `DT_RPATH`, so
  they do not depend on what they inherit from the UI process.
- At start-up, `WebKitPrivateRuntime::probe()` checks with `dladdr()` which
  `libwebkit2gtk` and `libjavascriptcoregtk` the process mapped, and that the
  helpers exist. If a release build is not on its private runtime (a broken
  install), it starts with Chromium instead of running WebKit on a system
  copy. `dalinira-browser --check-webkit-runtime` prints the same report.
- The normal development build (`scripts/build-dalinira.sh`, application in
  `build-dalinira/`) always uses the development runtime in
  `build-webkit-runtime/prefix/`. On a fresh checkout it first builds that
  runtime through `scripts/bootstrap-webkit-runtime.sh` (same pinned tarball,
  checksum and patches); it never falls back to the system WebKitGTK. A
  CMake configure without that prefix or an SDK fails. Only CI's push/PR test
  job opts into the test-only `DALINIRA_ALLOW_SYSTEM_WEBKIT_FOR_TESTS=ON`,
  whose builds cannot be installed or packaged. Package builds always pass
  `DALINIRA_WEBKIT_RUNTIME_SDK` explicitly.

What is not bundled: GTK, GLib, libsoup, GStreamer and its plugins, Mesa, ICU
and the other libraries WebKitGTK links. They come from the distribution as
package dependencies (for .deb/.rpm through dpkg-shlibdeps and rpm's soname
requirements; for Arch Linux they are listed in the PKGBUILDs). Also not
shipped: headers, pkg-config files, development symlinks, `WebKitWebDriver`,
`jsc`, introspection data and documentation.

## Validation

- **Before a release:** the GUI tier, in particular `browser-webkit-lifecycle`.
  It is a few seconds of tabs, background tab, sleep/wake, window close and
  shutdown, with no WebKit process lost or left behind. Run it against the
  staged package
  (`scripts/with-staged-install.sh STAGE -- scripts/safe-gui-test.sh …`).
  Check the loaded libraries with `verify-runtime-maps.py` and
  `dalinira-browser --check-webkit-runtime`.
- **The patch's statistical validation is research evidence**, not a release
  step: 3/40 unpatched vs 0/500 patched, and 0/500 again on the packaged
  runtime. It is recorded in `docs/research/webkit-exit-crash/README.md`.
  The stress tool was deleted and this 500-run validation must not be repeated
  as part of DaliNira development or release validation.

## Building

```
packaging/webkit-runtime/build-webkit-runtime.sh --sdk DIR [--build-dir DIR] [--tarball FILE] [--jobs N]
cmake -S . -B build-dalinira … -DDALINIRA_WEBKIT_RUNTIME_SDK=DIR
```

For local work, keep all expensive WebKit output below
`build-webkit-runtime/`; the normal dual-engine application build belongs in
the disposable `build-dalinira/` directory. The runtime directory is not a
second DaliNira application build. The development runtime there is created
once by `scripts/bootstrap-webkit-runtime.sh` (run automatically by
`scripts/build-dalinira.sh`) and then updated incrementally; see
`docs/BUILDING.md`.

The script verifies the pinned tarball (`webkitgtk-runtime.env`), applies the
patches with no fuzz, builds a Release configuration like Arch Linux's
`webkit2gtk-4.1` (without documentation, introspection, MiniBrowser and speech
synthesis), installs into `DIR/root`, and writes the build-time `.pc` files,
`legal/` and `dalinira-webkit-runtime.cmake`. Debug and LTO flags are dropped.
The build directory is mapped to a relative path (`-ffile-prefix-map`), so the
`__FILE__` strings in WebKit's log messages carry no build-machine path.
Distribution-specific CMake options can be passed in
`WEBKIT_RUNTIME_CMAKE_ARGS`; CI uses this for Debian 12. A full build takes
about 50 minutes on 8 cores and needs about 4 GB of disk for the build
directory. The tree is never shipped.

The runtime is built per distribution family, because its library
dependencies (ICU and libxml2 sonames, for example) differ:

| Package | Runtime built on |
|---|---|
| Arch source (`packaging/archlinux`) | inside `makepkg` |
| Arch binary (`packaging/pacman`, AUR `dalinira`, `dalinira-bin`) | Arch Linux: the source package's runtime, released as `dalinira-webkit-runtime-<v>-arch-x86_64.tar.zst` |
| portable archive, Debian 12 `.deb` | Debian 12 |
| Ubuntu 24.04 `.deb` | Ubuntu 24.04 |
| Fedora `.rpm` | Fedora |

## Licensing

See `SOURCE.md`, which is installed with the licenses. In short:

- `libwebkit2gtk` and `libjavascriptcoregtk` contain LGPL-2.1-or-later code
  (WebCore, JavaScriptCore) and BSD-licensed code (WebKit, WTF), plus
  third-party components under their own licenses (Skia, ANGLE, pdf.js,
  xdgmime, simdutf, fast_float, …).
- Every package ships `LICENSE.WebKitGTK`. Like Arch Linux's
  `webkit2gtk-4.1`, it holds every `COPYING*`, `LICENSE*` and `NOTICE*` file
  of the release tarball. Each package also ships the patches, `SOURCE.md` and
  `BUILD-INFO`.
- Corresponding source (LGPL-2.1 section 6): every release publishes
  `dalinira-webkit-runtime-source-<v>.tar` next to the binaries. It holds the
  upstream tarball, the patches and this recipe. The Arch source package
  builds from the same files. The modified file's change is described, with
  its date, in the patch header.
- DaliNira (GPL-3.0-only) links WebKitGTK dynamically, and users can replace
  the private libraries with their own build (`SOURCE.md`).
