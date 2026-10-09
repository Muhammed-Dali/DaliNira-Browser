#!/usr/bin/env bash
# Serves website/ on http://127.0.0.1:<port>/ for a local preview, laid out as
# GitHub Pages serves it (/, /tr/, /ar/, /assets/...).
#
# Opening website/index.html directly (file://) is not a reliable preview:
# sandboxed browsers (Flatpak, Snap) receive only that single file through the
# document portal, so the stylesheet, script and images next to it are missing.
#
# Usage: scripts/preview-website.sh [port]   (default 8000; Ctrl+C stops it)
set -euo pipefail
cd "$(dirname "$0")/../website"
port="${1:-8000}"
echo "DaliNira website preview: http://127.0.0.1:${port}/  (TR: /tr/  AR: /ar/)"
exec python3 -m http.server "$port" --bind 127.0.0.1
