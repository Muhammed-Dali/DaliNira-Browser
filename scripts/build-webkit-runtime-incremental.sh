#!/usr/bin/env bash
# Internal helper for build-dalinira.sh. It validates and incrementally updates
# the preserved private WebKitGTK tree; it never creates a second runtime tree.
# The first tree is created by scripts/bootstrap-webkit-runtime.sh, which then
# runs this script for the initial configure, build and install.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUNTIME_DIR="${ROOT_DIR}/build-webkit-runtime"
SOURCE_ENV="${ROOT_DIR}/packaging/webkit-runtime/webkitgtk-runtime.env"
RECIPE="${ROOT_DIR}/packaging/webkit-runtime/build-webkit-runtime.sh"
JOBS="${DALINIRA_BUILD_JOBS:-3}"

case "${JOBS}" in
  1|2|3) ;;
  *) echo "[webkit-runtime] DALINIRA_BUILD_JOBS must be 1, 2, or 3" >&2; exit 2 ;;
esac

# shellcheck source=../packaging/webkit-runtime/webkitgtk-runtime.env
source "${SOURCE_ENV}"

SOURCE_DIR="${RUNTIME_DIR}/src/webkitgtk-${WEBKITGTK_VERSION}"
TARBALL="${RUNTIME_DIR}/src/webkitgtk-${WEBKITGTK_VERSION}.tar.xz"
BUILD_DIR="${RUNTIME_DIR}/build"
PREFIX="${RUNTIME_DIR}/prefix"
LOCAL_ENV="${RUNTIME_DIR}/env.sh"
SOURCE_STAMP="${RUNTIME_DIR}/.dalinira-source-inputs.sha256"
CONFIG_STAMP="${RUNTIME_DIR}/.dalinira-build-inputs.sha256"

for path in "${RUNTIME_DIR}" "${SOURCE_DIR}" "${BUILD_DIR}" "${PREFIX}"; do
  [ -d "${path}" ] || {
    echo "[webkit-runtime] missing preserved tree component: ${path}" >&2
    echo "[webkit-runtime] create or repair the tree with scripts/bootstrap-webkit-runtime.sh" >&2
    exit 1
  }
done
for path in "${TARBALL}" "${LOCAL_ENV}"; do
  [ -f "${path}" ] || {
    echo "[webkit-runtime] missing required file: ${path}" >&2
    echo "[webkit-runtime] create or repair the tree with scripts/bootstrap-webkit-runtime.sh" >&2
    exit 1
  }
done

echo "${WEBKITGTK_SHA256}  ${TARBALL}" | sha256sum -c --quiet - || {
  echo "[webkit-runtime] pinned source checksum mismatch: ${TARBALL}" >&2
  exit 1
}

shopt -s nullglob
PATCHES=("${ROOT_DIR}"/packaging/webkit-runtime/patches/*.patch)
shopt -u nullglob
[ "${#PATCHES[@]}" -gt 0 ] || { echo "[webkit-runtime] no validated patches found" >&2; exit 1; }

PATCH_HASH="$(cat "${PATCHES[@]}" | sha256sum | cut -d' ' -f1)"
SOURCE_FINGERPRINT="$(printf '%s\n' \
  "version=${WEBKITGTK_VERSION}" \
  "sha256=${WEBKITGTK_SHA256}" \
  "patches=${PATCH_HASH}" | sha256sum | cut -d' ' -f1)"

source_changed=0
if [ ! -f "${SOURCE_STAMP}" ]; then
  # Adopt the already validated tree without extracting it again. Reverse
  # dry-runs prove that every current patch is present in the source.
  for patch_file in "${PATCHES[@]}"; do
    patch -d "${SOURCE_DIR}" -p1 -R --dry-run --silent < "${patch_file}" || {
      echo "[webkit-runtime] existing source does not contain $(basename "${patch_file}")" >&2
      exit 1
    }
  done
  printf '%s\n' "${SOURCE_FINGERPRINT}" > "${SOURCE_STAMP}"
elif [ "$(cat "${SOURCE_STAMP}")" != "${SOURCE_FINGERPRINT}" ]; then
  # Patch/version changes are exceptional. Refresh the derived source in
  # place from the already downloaded, checksum-verified tarball, preserving
  # the existing object directory so Ninja rebuilds only affected objects.
  command -v rsync >/dev/null || {
    echo "[webkit-runtime] rsync is required to refresh changed runtime source inputs" >&2
    exit 1
  }
  refresh_dir="$(mktemp -d "${RUNTIME_DIR}/.source-refresh.XXXXXX")"
  trap 'rm -rf "${refresh_dir}"' EXIT
  tar -xf "${TARBALL}" -C "${refresh_dir}"
  refreshed_source="${refresh_dir}/webkitgtk-${WEBKITGTK_VERSION}"
  [ -d "${refreshed_source}" ] || { echo "[webkit-runtime] unexpected source archive layout" >&2; exit 1; }
  for patch_file in "${PATCHES[@]}"; do
    patch -d "${refreshed_source}" -p1 --forward --fuzz=0 < "${patch_file}"
  done
  rsync -a --delete "${refreshed_source}/" "${SOURCE_DIR}/"
  printf '%s\n' "${SOURCE_FINGERPRINT}" > "${SOURCE_STAMP}"
  source_changed=1
fi

CONFIG_FINGERPRINT="$({
  sha256sum "${SOURCE_ENV}" "${RECIPE}" "${BASH_SOURCE[0]}" "${LOCAL_ENV}"
  printf '%s\n' \
    'generator=Ninja' 'port=GTK' 'type=Release' 'libdir=lib' 'libexecdir=lib' \
    'gtk4=OFF' 'documentation=OFF' 'introspection=OFF' 'minibrowser=OFF' \
    'speech-synthesis=OFF' 'flite=OFF' 'libbacktrace=OFF' \
    "prefix=${PREFIX}" "source=${SOURCE_FINGERPRINT}"
} | sha256sum | cut -d' ' -f1)"

configure_needed=${source_changed}
if [ ! -f "${CONFIG_STAMP}" ] || [ "$(cat "${CONFIG_STAMP}")" != "${CONFIG_FINGERPRINT}" ] ||
   [ ! -f "${BUILD_DIR}/CMakeCache.txt" ]; then
  configure_needed=1
fi

# env.sh holds the tree's toolchain settings: the compiler and flags the first
# configure recorded, distribution-specific WEBKIT_RUNTIME_CMAKE_ARGS, and any
# local tool paths. It is part of the fingerprint above.
# shellcheck source=/dev/null
source "${LOCAL_ENV}"

if [ "${configure_needed}" -eq 1 ]; then
  echo "[webkit-runtime] configuring existing tree"
  cmake -S "${SOURCE_DIR}" -B "${BUILD_DIR}" -G Ninja \
    -DPORT=GTK \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${PREFIX}" \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DCMAKE_INSTALL_LIBEXECDIR=lib \
    -DCMAKE_SKIP_RPATH=OFF \
    -DCMAKE_INSTALL_RPATH="${PREFIX}/lib" \
    -DUSE_GTK4=OFF \
    -DENABLE_DOCUMENTATION=OFF \
    -DENABLE_INTROSPECTION=OFF \
    -DENABLE_MINIBROWSER=OFF \
    -DENABLE_SPEECH_SYNTHESIS=OFF \
    -DUSE_FLITE=OFF \
    -DUSE_LIBBACKTRACE=OFF \
    ${WEBKIT_RUNTIME_CMAKE_ARGS:-}
fi

plan_file="$(mktemp)"
trap 'rm -f "${plan_file}"; if [ -n "${refresh_dir:-}" ]; then rm -rf "${refresh_dir}"; fi' EXIT
cmake --build "${BUILD_DIR}" -- -n > "${plan_file}"

# WebKit's forwarding-header phony target makes Ninja propose a harmless
# libwebkit/helper relink even in a clean tree. Treat precisely that known
# cascade as no compile work; any other planned action runs the real build.
unexpected_plan="$(sed -E \
  -e '/^ninja: Entering directory /d' \
  -e '/^ninja: no work to do\.$/d' \
  -e '/^[[][^]]+[]] cd .*generate-forwarding-headers\.pl /d' \
  -e '/^[[][^]]+[]] Linking CXX shared library lib\/libwebkit2gtk-4\.1\.so/d' \
  -e '/^[[][^]]+[]] Creating library symlink lib\/libwebkit2gtk-4\.1\.so/d' \
  -e '/^[[][^]]+[]] Linking CXX executable bin\/WebKit(Web|Network|GPU)Process$/d' \
  -e '/^[[][^]]+[]] Linking CXX shared module lib\/libwebkit2gtkinjectedbundle\.so$/d' \
  -e '/^[[:space:]]*$/d' "${plan_file}")"

if [ "${configure_needed}" -eq 0 ] && [ -z "${unexpected_plan}" ]; then
  echo "[webkit-runtime] no changed compile inputs; incremental check complete (no work)"
  exit 0
fi

echo "[webkit-runtime] building changed inputs with ${JOBS} jobs"
# A generated header WebKitGTK 2.54.1 forgets to order before one of its
# users (see packaging/webkit-runtime/build-webkit-runtime.sh).
ninja -C "${BUILD_DIR}" WebKitGTK/DerivedSources/WebKitDirectoryInputStreamData.h > /dev/null
cmake --build "${BUILD_DIR}" --parallel "${JOBS}"
echo "[webkit-runtime] synchronizing ${PREFIX}"
cmake --install "${BUILD_DIR}"
printf '%s\n' "${CONFIG_FINGERPRINT}" > "${CONFIG_STAMP}"
echo "[webkit-runtime] complete"
