#!/usr/bin/env bash
# First-time setup of the private WebKitGTK development runtime in
# build-webkit-runtime/. build-dalinira.sh runs it automatically when that tree
# has not been built yet; it can also be run on its own.
#
# It downloads the WebKitGTK release pinned in
# packaging/webkit-runtime/webkitgtk-runtime.env, verifies its SHA-256,
# applies packaging/webkit-runtime/patches/*.patch and hands the tree to
# build-webkit-runtime-incremental.sh for the configure, build and install.
# The system WebKitGTK is never used as a substitute.
#
# Every step is resumable: an interrupted bootstrap continues where it
# stopped, and a finished tree is left to the incremental script.
#
# Usage: bootstrap-webkit-runtime.sh [--yes] [--check]
#   --yes    start without asking (also DALINIRA_WEBKIT_BOOTSTRAP=yes)
#   --check  only run the preflight checks and print the plan
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUNTIME_DIR="${ROOT_DIR}/build-webkit-runtime"
SOURCE_ENV="${ROOT_DIR}/packaging/webkit-runtime/webkitgtk-runtime.env"
INCREMENTAL="${ROOT_DIR}/scripts/build-webkit-runtime-incremental.sh"
JOBS="${DALINIRA_BUILD_JOBS:-3}"
# Measured tree: 48 MB tarball, 0.5 GB source, 4.1 GB objects, 0.2 GB prefix.
MIN_FREE_GB=8

log() { printf '[webkit-bootstrap] %s\n' "$*"; }
die() { printf '[webkit-bootstrap] %s\n' "$*" >&2; exit 1; }

assume_yes=0
check_only=0
case "${DALINIRA_WEBKIT_BOOTSTRAP:-}" in yes|1) assume_yes=1 ;; esac
while [ $# -gt 0 ]; do
  case "$1" in
    --yes|-y) assume_yes=1 ;;
    --check) check_only=1 ;;
    -h|--help) awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
  shift
done

case "${JOBS}" in
  1|2|3) ;;
  *) die "DALINIRA_BUILD_JOBS must be 1, 2, or 3" ;;
esac

# shellcheck source=../packaging/webkit-runtime/webkitgtk-runtime.env
source "${SOURCE_ENV}"

SOURCE_DIR="${RUNTIME_DIR}/src/webkitgtk-${WEBKITGTK_VERSION}"
TARBALL="${RUNTIME_DIR}/src/webkitgtk-${WEBKITGTK_VERSION}.tar.xz"
LOCAL_ENV="${RUNTIME_DIR}/env.sh"

# A finished tree only needs the normal incremental check.
if [ -f "${RUNTIME_DIR}/.dalinira-build-inputs.sha256" ]; then
  if [ "${check_only}" -eq 1 ]; then
    log "build-webkit-runtime/ is already built; ./scripts/build-dalinira.sh checks it incrementally"
    exit 0
  fi
  log "build-webkit-runtime/ is already built; running the incremental check"
  exec env DALINIRA_BUILD_JOBS="${JOBS}" "${INCREMENTAL}"
fi

shopt -s nullglob
PATCHES=("${ROOT_DIR}"/packaging/webkit-runtime/patches/*.patch)
shopt -u nullglob
[ "${#PATCHES[@]}" -gt 0 ] || die "no validated patches in packaging/webkit-runtime/patches"

# ---- Preflight: report every missing tool at once, before any download ----
missing=()
for tool in cmake ninja tar xz sha256sum patch pkg-config perl python3 ruby gperf unifdef \
            glib-mkenums gdbus-codegen; do
  command -v "${tool}" >/dev/null || missing+=("${tool}")
done
command -v curl >/dev/null || [ -f "${TARBALL}" ] || missing+=("curl")
if [ -z "${CXX:-}" ] && ! command -v clang++ >/dev/null && ! command -v c++ >/dev/null; then
  missing+=("a C++ compiler (clang++ recommended)")
fi
missing_pc=()
for module in glib-2.0 gtk+-3.0 libsoup-3.0 gstreamer-1.0 gstreamer-plugins-base-1.0 \
              icu-uc harfbuzz libxslt sqlite3 libsecret-1 enchant-2 libseccomp epoxy \
              wayland-client wayland-protocols libwebp lcms2 libtasn1 libwoff2dec libavif \
              atspi-2 gbm; do
  pkg-config --exists "${module}" 2>/dev/null || missing_pc+=("${module}")
done
if [ "${#missing[@]}" -gt 0 ] || [ "${#missing_pc[@]}" -gt 0 ]; then
  [ "${#missing[@]}" -eq 0 ] || log "missing tools: ${missing[*]}"
  [ "${#missing_pc[@]}" -eq 0 ] || log "missing development packages (pkg-config): ${missing_pc[*]}"
  die "install the WebKitGTK build dependencies listed in docs/BUILDING.md, then run this again"
fi

free_gb="$(df -P -BG "${ROOT_DIR}" | awk 'NR == 2 { sub(/G$/, "", $4); print $4 }')"
resuming=0
[ -d "${RUNTIME_DIR}/build" ] && resuming=1
if [ "${resuming}" -eq 0 ] && [ "${free_gb}" -lt "${MIN_FREE_GB}" ]; then
  die "only ${free_gb} GB free in ${ROOT_DIR}; the first WebKitGTK build needs about ${MIN_FREE_GB} GB"
fi

if [ -n "${CC:-}${CXX:-}" ]; then
  compiler="${CXX:-c++} (from CC/CXX)"
elif command -v clang++ >/dev/null; then
  compiler="clang++"
else
  compiler="c++"
fi
mem_gb="$(awk '/^MemAvailable:/ { printf "%d", $2 / 1048576 }' /proc/meminfo 2>/dev/null || echo "?")"

cat <<EOF

  DaliNira private WebKitGTK runtime: first-time build$([ "${resuming}" -eq 0 ] || echo " (resuming)")
  ----------------------------------------------------
  DaliNira's WebKit engine runs on its own patched WebKitGTK, not on the
  system copy. This checkout does not have that runtime yet, so it is built
  once now in build-webkit-runtime/. Later builds only check it and rebuild
  what changed.

  Source       WebKitGTK ${WEBKITGTK_VERSION} from ${WEBKITGTK_URL}
  SHA-256      ${WEBKITGTK_SHA256}
  Patches      $(for p in "${PATCHES[@]}"; do basename "${p}"; done | paste -sd' ')
  Compiler     ${compiler}
  Jobs         ${JOBS} (DALINIRA_BUILD_JOBS=1 or 2 lowers it; 3 is the maximum)
  Disk         about 5 GB in build-webkit-runtime/ (${free_gb} GB free)
  Memory       ${mem_gb} GB available; WebKit compile jobs are memory-heavy
  Time         typically 1.5 to 3 hours at 3 jobs, depending on the CPU

  The build can be interrupted and resumed: run the same command again.
  Keep build-webkit-runtime/ afterwards; deleting it means building again.

EOF

if [ "${check_only}" -eq 1 ]; then
  log "preflight passed; nothing was changed (--check)"
  exit 0
fi

if [ "${assume_yes}" -eq 0 ]; then
  if [ -t 0 ]; then
    read -r -p "Start the WebKitGTK build now? [y/N] " answer
    case "${answer}" in y|Y|yes|YES) ;; *) die "cancelled; nothing was built" ;; esac
  else
    die "not started: no terminal to confirm. Run ./scripts/build-dalinira.sh --yes (or set DALINIRA_WEBKIT_BOOTSTRAP=yes) to start the first-time build"
  fi
fi

# ---- Source: the pinned, checksum-verified release tarball ----
mkdir -p "${RUNTIME_DIR}/src"
if [ -f "${TARBALL}" ] && ! echo "${WEBKITGTK_SHA256}  ${TARBALL}" | sha256sum -c --quiet - 2>/dev/null; then
  log "discarding a tarball that does not match the pinned checksum"
  rm -f "${TARBALL}"
fi
if [ ! -f "${TARBALL}" ]; then
  log "downloading ${WEBKITGTK_URL}"
  curl -fL --retry 3 -o "${TARBALL}.part" "${WEBKITGTK_URL}"
  echo "${WEBKITGTK_SHA256}  ${TARBALL}.part" | sha256sum -c --quiet - || {
    rm -f "${TARBALL}.part"
    die "the downloaded tarball does not match the pinned SHA-256; nothing was extracted"
  }
  mv "${TARBALL}.part" "${TARBALL}"
fi
log "verified webkitgtk-${WEBKITGTK_VERSION}.tar.xz (SHA-256 ${WEBKITGTK_SHA256})"

# Extract and patch next to the final location, then move it into place, so
# an existing source directory is always complete and patched.
if [ ! -d "${SOURCE_DIR}" ]; then
  extract_dir="$(mktemp -d "${RUNTIME_DIR}/.bootstrap-extract.XXXXXX")"
  trap 'rm -rf "${extract_dir}"' EXIT
  log "extracting webkitgtk-${WEBKITGTK_VERSION}"
  tar -xf "${TARBALL}" -C "${extract_dir}"
  [ -d "${extract_dir}/webkitgtk-${WEBKITGTK_VERSION}" ] || die "unexpected source archive layout"
  for patch_file in "${PATCHES[@]}"; do
    log "applying $(basename "${patch_file}")"
    patch -d "${extract_dir}/webkitgtk-${WEBKITGTK_VERSION}" -p1 --forward --fuzz=0 --silent < "${patch_file}"
  done
  mv "${extract_dir}/webkitgtk-${WEBKITGTK_VERSION}" "${SOURCE_DIR}"
  rm -rf "${extract_dir}"
  trap - EXIT
fi
mkdir -p "${RUNTIME_DIR}/build" "${RUNTIME_DIR}/prefix"

# ---- Toolchain: recorded once, read by every later configure ----
# The validated runtime was built with clang and lld, without debug info or
# LTO (packaging/webkit-runtime/build-webkit-runtime.sh drops the same flags).
if [ ! -f "${LOCAL_ENV}" ]; then
  strip_flags() {
    local out=() f
    for f in $1; do
      case "$f" in -g|-g[0-9]|-ggdb*|-gdwarf*|-gsplit-dwarf|-flto|-flto=*|-ffat-lto-objects) ;; *) out+=("$f") ;; esac
    done
    echo "${out[*]-}"
  }
  cc="${CC:-}"
  cxx="${CXX:-}"
  ldflags="$(strip_flags "${LDFLAGS:-}")"
  if [ -z "${cc}${cxx}" ] && command -v clang >/dev/null && command -v clang++ >/dev/null; then
    cc=clang
    cxx=clang++
    if command -v ld.lld >/dev/null; then
      case " ${ldflags} " in *" -fuse-ld="*) ;; *) ldflags="${ldflags:+${ldflags} }-fuse-ld=lld" ;; esac
    fi
  fi
  {
    echo "# Toolchain of this private WebKitGTK tree, written by"
    echo "# scripts/bootstrap-webkit-runtime.sh. Sourced before every configure."
    [ -z "${cc}" ] || printf 'export CC=%q\n' "${cc}"
    [ -z "${cxx}" ] || printf 'export CXX=%q\n' "${cxx}"
    [ -z "${CFLAGS:-}" ] || printf 'export CFLAGS=%q\n' "$(strip_flags "${CFLAGS}")"
    [ -z "${CXXFLAGS:-}" ] || printf 'export CXXFLAGS=%q\n' "$(strip_flags "${CXXFLAGS}")"
    [ -z "${ldflags}" ] || printf 'export LDFLAGS=%q\n' "${ldflags}"
    [ -z "${WEBKIT_RUNTIME_CMAKE_ARGS:-}" ] || printf 'export WEBKIT_RUNTIME_CMAKE_ARGS=%q\n' "${WEBKIT_RUNTIME_CMAKE_ARGS}"
  } > "${LOCAL_ENV}"
  log "recorded the toolchain in build-webkit-runtime/env.sh"
fi

# ---- Configure, build and install through the normal incremental path ----
log "configuring and building with ${JOBS} jobs; WebKitGTK progress follows"
DALINIRA_BUILD_JOBS="${JOBS}" "${INCREMENTAL}"
log "private WebKitGTK runtime ready in build-webkit-runtime/prefix"
