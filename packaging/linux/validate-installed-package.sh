#!/usr/bin/env bash
# Validates an installed DaliNira package (.pkg.tar.zst, .deb or .rpm) in a
# fresh system that received it from its package manager alone. Used by the
# fresh-install CI jobs (.github/workflows/packages.yml); also runs against a
# staged install (--root DIR) on a development machine.
#
#   validate-installed-package.sh --version 8.0.1 [--allow-partial-media] [--root DIR] [--no-start]
#                                 [--system-webkit]
#
# Checks, in order:
#   1. layout: launcher, executables, WebKit extension, desktop entry, icons;
#   2. every ELF file DaliNira installs resolves all of its shared libraries;
#   3. `dalinira-browser --version` runs and reports the expected version;
#   3b. the private, patched WebKitGTK runtime (packaging/webkit-runtime) is
#      installed and is what the browser and WebKit's helper processes load,
#      not a system WebKitGTK (`--check-webkit-runtime`, ldd); --system-webkit
#      skips this for development builds without a private runtime;
#   4. `dalinira-browser --check-media-runtime`: 0 (complete) is required;
#      --allow-partial-media also accepts 2 (playable, some formats missing,
#      e.g. Fedora without the Cisco openh264 repository). 1 (HTML5 media
#      disabled) always fails: the package must have pulled the runtime in;
#   5. the browser starts (Chromium engine, Qt offscreen platform, no GPU, no
#      display server) and is still running after a few seconds.
#
# No Xvfb, no GPU and no WebKit window: WebKit needs an X display, so its
# runtime is covered by steps 2 and 4.

set -euo pipefail

root=""
expected_version=""
allow_partial_media=0
start_check=1
private_webkit=1
while [ $# -gt 0 ]; do
  case "$1" in
    --root) root="${2%/}"; shift 2 ;;
    --version) expected_version="$2"; shift 2 ;;
    --allow-partial-media) allow_partial_media=1; shift ;;
    --no-start) start_check=0; shift ;;
    --system-webkit) private_webkit=0; shift ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done
[ -n "$expected_version" ] || { echo "--version is required" >&2; exit 2; }

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok: $*"; }

launcher="${root}/usr/bin/dalinira-browser"
appdir="${root}/usr/lib/dalinira-browser"

# 1. Layout and desktop integration.
for file in \
    "$launcher" \
    "$appdir/dalinira-browser" \
    "$appdir/dalinira-webengine-process" \
    "$appdir/browser_policy.json" \
    "$appdir/webkit-extensions/libdalinira-webkit-extension.so" \
    "${root}/usr/share/applications/dalinira.desktop" \
    "${root}/usr/share/icons/hicolor/256x256/apps/dalinira.png" \
    "${root}/usr/share/icons/hicolor/48x48/apps/dalinira.png"; do
  [ -s "$file" ] || fail "missing or empty: $file"
done
[ -x "$launcher" ] || fail "launcher is not executable: $launcher"
desktop="${root}/usr/share/applications/dalinira.desktop"
grep -qx 'Exec=/usr/bin/dalinira-browser %U' "$desktop" || fail "desktop entry Exec line"
grep -qx 'Icon=dalinira' "$desktop" || fail "desktop entry Icon line"
grep -q '^MimeType=.*x-scheme-handler/https' "$desktop" || fail "desktop entry URL handler MIME types"
if command -v desktop-file-validate >/dev/null; then
  desktop-file-validate "$desktop" || fail "desktop-file-validate"
fi
pass "layout, desktop entry and icons"

# 2. Shared libraries of everything DaliNira installs as ELF.
missing=0
while IFS= read -r -d '' file; do
  head -c 4 "$file" | grep -q $'\x7fELF' || continue
  if ldd "$file" 2>&1 | grep -q 'not found'; then
    echo "unresolved libraries in $file:" >&2
    ldd "$file" | grep 'not found' >&2
    missing=1
  fi
done < <(find "$appdir" -type f -print0)
[ "$missing" -eq 0 ] || fail "shared libraries are missing"
pass "all shared libraries resolve"

# 3. The launcher runs the browser binary.
reported="$("$launcher" --version)"
[ "$reported" = "DaliNira Browser ${expected_version}" ] || fail "--version reported '$reported'"
pass "$reported"

# 3b. The private WebKitGTK runtime.
if [ "$private_webkit" -eq 1 ]; then
  webkit_runtime="$appdir/webkit"
  helpers="$webkit_runtime/libexec/webkit2gtk-4.1"
  for file in \
      "$webkit_runtime/lib/libwebkit2gtk-4.1.so.0" \
      "$webkit_runtime/lib/libjavascriptcoregtk-4.1.so.0" \
      "$webkit_runtime/lib/webkit2gtk-4.1/injected-bundle/libwebkit2gtkinjectedbundle.so" \
      "$helpers/WebKitWebProcess" "$helpers/WebKitNetworkProcess" "$helpers/WebKitGPUProcess" \
      "$webkit_runtime/BUILD-INFO" \
      "${root}/usr/share/doc/dalinira-browser/webkitgtk/LICENSE.WebKitGTK" \
      "${root}/usr/share/doc/dalinira-browser/webkitgtk/SOURCE.md" \
      "${root}/usr/share/doc/dalinira-browser/webkitgtk/COPYRIGHT.debian"; do
    [ -s "$file" ] || fail "private WebKit runtime file missing or empty: $file"
  done
  ls "${root}"/usr/share/doc/dalinira-browser/webkitgtk/patches/*.patch >/dev/null 2>&1 \
    || fail "WebKit runtime patches missing from the license directory"
  if [ -z "$root" ]; then
    # The loader must resolve WebKitGTK to the private copy for the browser
    # and for every helper process (their DT_RPATH points there).
    private_lib="/usr/lib/dalinira-browser/webkit/lib"
    for elf in "$appdir/dalinira-browser" "$helpers/WebKitWebProcess" "$helpers/WebKitNetworkProcess" \
               "$helpers/WebKitGPUProcess" "$webkit_runtime/lib/libwebkit2gtk-4.1.so.0"; do
      for soname in libwebkit2gtk-4.1.so.0 libjavascriptcoregtk-4.1.so.0; do
        [ "$(basename "$elf")" = "$soname" ] && continue
        resolved="$(ldd "$elf" | awk -v so="$soname" '$1 == so { print $3 }')"
        case "$resolved" in
          "$private_lib"/*) ;;
          *) fail "$elf resolves $soname to '${resolved:-nothing}', not $private_lib" ;;
        esac
      done
    done
    set +e
    "$launcher" --check-webkit-runtime
    runtime_status=$?
    set -e
    [ "$runtime_status" -eq 0 ] || fail "--check-webkit-runtime exit $runtime_status"
    # The web-process sandbox executes these by absolute path; without them
    # WebKitGTK cannot start a web process. Package dependencies install them.
    for sandbox_tool in /usr/bin/bwrap /usr/bin/xdg-dbus-proxy; do
      [ -x "$sandbox_tool" ] || fail "WebKit sandbox helper missing: $sandbox_tool (package dependency)"
    done
  fi
  pass "private WebKitGTK runtime ($(sed -n 's/^webkitgtk_version=//p' "$webkit_runtime/BUILD-INFO"), $(grep -c '^patch=' "$webkit_runtime/BUILD-INFO") patch(es))"
fi

# 4. WebKit HTML5 media runtime (GStreamer plugins from package dependencies).
set +e
"$launcher" --check-media-runtime
media_status=$?
set -e
case "$media_status" in
  0) pass "media runtime complete" ;;
  2) [ "$allow_partial_media" -eq 1 ] || fail "media runtime incomplete (exit 2)"
     pass "media runtime playable, some formats unsupported (allowed here)" ;;
  *) fail "media runtime check exit $media_status (HTML5 media would be disabled)" ;;
esac

# 5. The browser starts and keeps running (Chromium engine, offscreen).
if [ "$start_check" -eq 1 ]; then
  home="$(mktemp -d)"
  set +e
  env -u DISPLAY -u WAYLAND_DISPLAY \
    HOME="$home" XDG_CONFIG_HOME="$home/config" XDG_DATA_HOME="$home/data" XDG_CACHE_HOME="$home/cache" \
    QT_QPA_PLATFORM=offscreen QTWEBENGINE_DISABLE_SANDBOX=1 \
    QTWEBENGINE_CHROMIUM_FLAGS="--no-sandbox --disable-gpu" \
    timeout -s TERM 20 "$launcher" --engine chromium > "$home/start.log" 2>&1
  start_status=$?
  set -e
  # 124: still running when the timeout ended it, i.e. it started and stayed up.
  if [ "$start_status" -ne 124 ]; then
    tail -n 40 "$home/start.log" >&2
    rm -rf "$home"
    fail "browser exited with status $start_status during startup"
  fi
  rm -rf "$home"
  pass "browser starts and keeps running (offscreen, Chromium engine)"
fi

echo "PASS: DaliNira ${expected_version} package validated"
