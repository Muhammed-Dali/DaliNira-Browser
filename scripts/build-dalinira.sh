#!/usr/bin/env bash
# Authoritative developer build: private WebKitGTK first, then the single
# Chromium + WebKit DaliNira application.
#
# Usage: build-dalinira.sh [--yes]
#   --yes  on a fresh checkout, start the first-time WebKitGTK build without
#          asking (see scripts/bootstrap-webkit-runtime.sh)
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build-dalinira"
RUNTIME_DIR="${ROOT_DIR}/build-webkit-runtime"
RUNTIME_PREFIX="${RUNTIME_DIR}/prefix"
JOBS="${DALINIRA_BUILD_JOBS:-3}"

bootstrap_args=()
while [ $# -gt 0 ]; do
  case "$1" in
    --yes|-y) bootstrap_args+=(--yes) ;;
    -h|--help) awk 'NR > 1 && /^#/ { sub(/^# ?/, ""); print; next } NR > 1 { exit }' "${BASH_SOURCE[0]}"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
  shift
done

case "${JOBS}" in
  1|2|3) ;;
  *) echo "[build] DALINIRA_BUILD_JOBS must be 1, 2, or 3" >&2; exit 2 ;;
esac

# The incremental script writes this stamp only after a successful runtime
# build and install; without it the tree is missing or unfinished.
if [ -f "${RUNTIME_DIR}/.dalinira-build-inputs.sha256" ]; then
  echo "[build 1/2] Checking the private WebKitGTK runtime"
  DALINIRA_BUILD_JOBS="${JOBS}" "${ROOT_DIR}/scripts/build-webkit-runtime-incremental.sh"
else
  echo "[build 1/2] Building the private WebKitGTK runtime for the first time"
  DALINIRA_BUILD_JOBS="${JOBS}" "${ROOT_DIR}/scripts/bootstrap-webkit-runtime.sh" "${bootstrap_args[@]}"
fi

echo "[build 2/2] Configuring the dual-engine DaliNira application"
cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DDALINIRA_ENABLE_WEBKIT=ON \
  -DDALINIRA_REQUIRE_DUAL_ENGINE=ON \
  -DDALINIRA_WEBKIT_DEVELOPMENT_RUNTIME="${RUNTIME_PREFIX}"

echo "[build 2/2] Building Chromium + WebKit integration with ${JOBS} jobs"
cmake --build "${BUILD_DIR}" --target dalinira-browser --parallel "${JOBS}"

echo "[build] complete: ${BUILD_DIR}/dalinira-browser"
