#!/usr/bin/env bash
# Builds DaliNira's private WebKitGTK runtime: the upstream WebKitGTK release
# pinned in webkitgtk-runtime.env plus the patches in patches/, configured for
# an application-private prefix (default /usr/lib/dalinira-browser/webkit).
#
# WebKitGTK compiles absolute paths into libwebkit2gtk (the WebKitWebProcess /
# WebKitNetworkProcess directory, the injected bundle, the locale directory),
# so the runtime only works when installed at the prefix it was built for.
# Every library and helper gets DT_RPATH=<prefix>/lib, so the helpers load the
# private libwebkit2gtk and libjavascriptcoregtk, never the system copies,
# whatever LD_LIBRARY_PATH says.
#
# Output (--sdk DIR), consumed by DaliNira's CMake (-DDALINIRA_WEBKIT_RUNTIME_SDK=DIR):
#   root/<prefix>/...                 full `ninja install` of WebKitGTK (DESTDIR)
#   pkgconfig/*.pc                    build-time .pc files pointing into root/
#   legal/                            WebKitGTK license texts, patches, BUILD-INFO
#   dalinira-webkit-runtime.cmake     version, prefix and paths for CMake
# DaliNira's install rules copy only the runtime part of root/ into packages.
#
# The build follows the configuration validated on 2026-10-08 (0 crashes in
# 500 window lifetimes): Release, no debug info, no LTO, no documentation or
# introspection. Debug and LTO flags in CFLAGS/CXXFLAGS/LDFLAGS are dropped.
# WEBKIT_RUNTIME_CMAKE_ARGS adds distribution-specific CMake options (CI uses
# it where a dependency is too old, e.g. -DUSE_JPEGXL=OFF on Debian 12).
#
# Usage: build-webkit-runtime.sh --sdk DIR [--build-dir DIR] [--tarball FILE]
#                                [--prefix PATH] [--jobs N]
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=webkitgtk-runtime.env
source "${here}/webkitgtk-runtime.env"

prefix=/usr/lib/dalinira-browser/webkit
sdk=""
build_dir=""
tarball=""
jobs="$(nproc)"

while [ $# -gt 0 ]; do
  case "$1" in
    --sdk) sdk="$2"; shift 2 ;;
    --build-dir) build_dir="$2"; shift 2 ;;
    --tarball) tarball="$2"; shift 2 ;;
    --prefix) prefix="$2"; shift 2 ;;
    --jobs) jobs="$2"; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done
[ -n "$sdk" ] || { echo "--sdk DIR is required" >&2; exit 2; }
case "$prefix" in /*) ;; *) echo "--prefix must be absolute" >&2; exit 2 ;; esac
sdk="$(mkdir -p "$sdk" && cd "$sdk" && pwd)"
build_dir="${build_dir:-${sdk}.build}"
mkdir -p "$build_dir"
build_dir="$(cd "$build_dir" && pwd)"

log() { printf '[webkit-runtime] %s\n' "$*"; }

# ---- Source: the pinned release tarball, checksum-verified ----
if [ -z "$tarball" ]; then
  tarball="${build_dir}/webkitgtk-${WEBKITGTK_VERSION}.tar.xz"
  if [ ! -f "$tarball" ]; then
    log "downloading ${WEBKITGTK_URL}"
    curl -fL --retry 3 -o "${tarball}.part" "$WEBKITGTK_URL"
    mv "${tarball}.part" "$tarball"
  fi
fi
echo "${WEBKITGTK_SHA256}  ${tarball}" | sha256sum -c --quiet - \
  || { echo "checksum mismatch: ${tarball}" >&2; exit 1; }

src="${build_dir}/webkitgtk-${WEBKITGTK_VERSION}"
patches=("${here}"/patches/*.patch)
patch_stamp="$(cat "${patches[@]}" | sha256sum | cut -d' ' -f1)"
if [ ! -f "${src}/.dalinira-patched" ] || [ "$(cat "${src}/.dalinira-patched")" != "$patch_stamp" ]; then
  log "extracting and patching webkitgtk-${WEBKITGTK_VERSION}"
  rm -rf "$src"
  tar -xf "$tarball" -C "$build_dir"
  for p in "${patches[@]}"; do
    log "applying $(basename "$p")"
    patch -d "$src" -p1 --forward --fuzz=0 < "$p"
  done
  echo "$patch_stamp" > "${src}/.dalinira-patched"
fi

# ---- Configure and build ----
strip_flags() {
  local out=() f
  for f in $1; do
    case "$f" in -g|-g[0-9]|-ggdb*|-gdwarf*|-gsplit-dwarf|-flto|-flto=*|-ffat-lto-objects) ;; *) out+=("$f") ;; esac
  done
  echo "${out[*]-}"
}
export CFLAGS="$(strip_flags "${CFLAGS:--O2}")"
export CXXFLAGS="$(strip_flags "${CXXFLAGS:--O2}")"
export LDFLAGS="$(strip_flags "${LDFLAGS:-}")"
# WebKit's log macros embed __FILE__, so without this the shipped libraries
# would carry the build machine's absolute source path (thousands of strings,
# e.g. a home directory). Map it to a relative one, as reproducible builds do;
# only string constants change.
export CFLAGS="${CFLAGS} -ffile-prefix-map=${build_dir}=."
export CXXFLAGS="${CXXFLAGS} -ffile-prefix-map=${build_dir}=."

obj="${build_dir}/build"
log "configuring for prefix ${prefix} (CC=${CC:-default} CXX=${CXX:-default})"
cmake -S "$src" -B "$obj" -G Ninja \
  -DPORT=GTK \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$prefix" \
  -DCMAKE_INSTALL_LIBDIR=lib \
  -DCMAKE_INSTALL_LIBEXECDIR=libexec \
  -DCMAKE_SKIP_RPATH=OFF \
  -DCMAKE_INSTALL_RPATH="${prefix}/lib" \
  -DUSE_GTK4=OFF \
  -DENABLE_DOCUMENTATION=OFF \
  -DENABLE_INTROSPECTION=OFF \
  -DENABLE_MINIBROWSER=OFF \
  -DENABLE_SPEECH_SYNTHESIS=OFF \
  -DUSE_FLITE=OFF \
  -DUSE_LIBBACKTRACE=OFF \
  ${WEBKIT_RUNTIME_CMAKE_ARGS:-} \
  > "${build_dir}/configure.log"

log "building with ${jobs} jobs (log: ${build_dir}/build.log)"
# WebKitGTK 2.54.1 misses a dependency: WebKitNetworkProcess may compile the
# unified source that includes this generated header before the WebKitShared
# target has generated it. Generate it first.
ninja -C "$obj" WebKitGTK/DerivedSources/WebKitDirectoryInputStreamData.h > "${build_dir}/build.log"
ninja -C "$obj" -j "$jobs" >> "${build_dir}/build.log"

log "installing into ${sdk}/root"
rm -rf "${sdk}/root"
DESTDIR="${sdk}/root" ninja -C "$obj" install > "${build_dir}/install.log"

stage="${sdk}/root${prefix}"
libexec="${stage}/libexec/webkit2gtk-4.1"
for f in "${stage}/lib/libwebkit2gtk-4.1.so.0" "${stage}/lib/libjavascriptcoregtk-4.1.so.0" \
         "${libexec}/WebKitWebProcess" "${libexec}/WebKitNetworkProcess" "${libexec}/WebKitGPUProcess" \
         "${stage}/lib/webkit2gtk-4.1/injected-bundle/libwebkit2gtkinjectedbundle.so"; do
  [ -e "$f" ] || { echo "missing after install: $f" >&2; exit 1; }
  # Read the section first: under pipefail, `readelf | grep -q` fails at
  # random when grep exits before readelf has written everything (SIGPIPE).
  dynamic="$(readelf -d "$f")"
  if ! grep -Eq "\((RPATH)\).*\[${prefix}/lib\]" <<< "$dynamic"; then
    echo "expected DT_RPATH ${prefix}/lib in $f" >&2
    readelf -d "$f" | grep -E 'RPATH|RUNPATH' >&2 || true
    exit 1
  fi
done

# Shipped ELF files carry no symbol tables (build IDs stay).
for f in "${stage}"/lib/lib*-4.1.so.*.* "${stage}/lib/webkit2gtk-4.1/injected-bundle/libwebkit2gtkinjectedbundle.so" \
         "${libexec}/WebKitWebProcess" "${libexec}/WebKitNetworkProcess" "${libexec}/WebKitGPUProcess"; do
  [ -L "$f" ] || "${STRIP:-strip}" --strip-unneeded "$f"
done

# ---- Build-time pkg-config files pointing into the SDK ----
rm -rf "${sdk}/pkgconfig"
mkdir -p "${sdk}/pkgconfig"
for pc in "${stage}"/lib/pkgconfig/*.pc; do
  sed "s|${prefix}|${stage}|g" "$pc" > "${sdk}/pkgconfig/$(basename "$pc")"
done

# ---- Legal files and build record ----
legal="${sdk}/legal"
rm -rf "$legal"
mkdir -p "${legal}/patches"
cp "${patches[@]}" "${legal}/patches/"
# Same collection Arch Linux's webkit2gtk-4.1 package ships: every license and
# copying file in the release tarball, concatenated with its path.
(cd "$src" && find . -type f \( -name 'COPYING*' -o -name 'LICENSE*' -o -name 'NOTICE*' \) -print0 \
   | LC_ALL=C sort -z \
   | while IFS= read -r -d '' f; do printf '### %s ###\n\n' "${f#./}"; cat "$f"; printf '\n\n'; done) \
  > "${legal}/LICENSE.WebKitGTK"
# Per-file copyright holders and licenses of this exact release, as audited
# by Debian (webkit2gtk 2.54.1-1, debian/copyright), and xdgmime's license
# grant, which is only stated in its README and file headers.
cp "${here}/legal/copyright.debian-webkit2gtk-${WEBKITGTK_VERSION}" "${legal}/COPYRIGHT.debian"
cp "${src}/Source/ThirdParty/xdgmime/README" "${legal}/README.xdgmime"
cp "${here}/SOURCE.md" "${legal}/SOURCE.md"

build_id() { readelf -n "$1" | sed -n 's/.*Build ID: //p'; }
{
  echo "name=DaliNira private WebKitGTK runtime"
  echo "webkitgtk_version=${WEBKITGTK_VERSION}"
  echo "runtime_revision=${DALINIRA_WEBKIT_RUNTIME_REVISION}"
  echo "source_url=${WEBKITGTK_URL}"
  echo "source_sha256=${WEBKITGTK_SHA256}"
  for p in "${patches[@]}"; do
    echo "patch=$(basename "$p") sha256=$(sha256sum "$p" | cut -d' ' -f1)"
  done
  echo "prefix=${prefix}"
  echo "built_on=$( . /etc/os-release && echo "${PRETTY_NAME:-$ID}")"
  echo "compiler=$("${CXX:-c++}" --version | head -n1)"
  echo "libwebkit2gtk_build_id=$(build_id "${stage}/lib/libwebkit2gtk-4.1.so.0")"
  echo "libjavascriptcoregtk_build_id=$(build_id "${stage}/lib/libjavascriptcoregtk-4.1.so.0")"
} > "${legal}/BUILD-INFO"
cp "${legal}/BUILD-INFO" "${stage}/BUILD-INFO"

cat > "${sdk}/dalinira-webkit-runtime.cmake" <<EOF
# Generated by packaging/webkit-runtime/build-webkit-runtime.sh
set(DALINIRA_WEBKIT_RUNTIME_VERSION "${WEBKITGTK_VERSION}")
set(DALINIRA_WEBKIT_RUNTIME_REVISION "${DALINIRA_WEBKIT_RUNTIME_REVISION}")
set(DALINIRA_WEBKIT_RUNTIME_PREFIX "${prefix}")
set(DALINIRA_WEBKIT_RUNTIME_STAGE "\${CMAKE_CURRENT_LIST_DIR}/root${prefix}")
set(DALINIRA_WEBKIT_RUNTIME_PKGCONFIG_DIR "\${CMAKE_CURRENT_LIST_DIR}/pkgconfig")
set(DALINIRA_WEBKIT_RUNTIME_LEGAL_DIR "\${CMAKE_CURRENT_LIST_DIR}/legal")
EOF

log "done: $(cat "${legal}/BUILD-INFO" | tr '\n' ' ')"
