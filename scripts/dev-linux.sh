#!/usr/bin/env bash
set -e

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build-dalinira"

"$ROOT_DIR/scripts/build-dalinira.sh"

# Wayland / XCB display environment setup
if [ -z "$QT_QPA_PLATFORM" ] && [ -n "$DISPLAY" ] && [ "$XDG_SESSION_TYPE" = "wayland" ]; then
  export QT_QPA_PLATFORM=xcb
fi
if [ -z "$GDK_BACKEND" ] && [ -n "$DISPLAY" ]; then
  export GDK_BACKEND=x11
fi

# Isolated development profile
DEV_CONFIG_DIR="$BUILD_DIR/dev-profile/config"
mkdir -p "$DEV_CONFIG_DIR"
if [ -d "$HOME/.config/DaliNira" ] && [ ! -d "$DEV_CONFIG_DIR/DaliNira" ]; then
  cp -r "$HOME/.config/DaliNira" "$DEV_CONFIG_DIR/" 2>/dev/null || true
fi
export XDG_CONFIG_HOME="$DEV_CONFIG_DIR"

echo "[START] Tarayıcı başlatılıyor: $BUILD_DIR/dalinira-browser"
cd "$BUILD_DIR"
exec "$BUILD_DIR/dalinira-browser" "$@"
