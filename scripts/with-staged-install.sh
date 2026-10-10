#!/usr/bin/env bash
# Runs a command as if a staged DaliNira package were installed, without root
# and without touching the system: in a private mount namespace (bubblewrap),
# the package's files are mounted at their real install paths. Everything
# else (display, GPU, D-Bus, audio, the user's home) is the real system;
# /usr/share/doc is replaced by an empty tmpfs so the package's
# documentation directory can be mounted there. Used to validate a built package, the
# private WebKitGTK runtime above all, whose paths are compiled in.
#
#   scripts/with-staged-install.sh ROOT -- COMMAND [ARG...]
#
# ROOT is an extracted package: ROOT/usr/lib/dalinira-browser, ...
# (`bsdtar -xf dalinira-browser-*.pkg.tar.zst -C ROOT`, or `DESTDIR=ROOT cmake --install`).
set -euo pipefail

root="${1:?usage: with-staged-install.sh ROOT -- COMMAND...}"
shift
[ "${1:-}" = "--" ] && shift
[ $# -gt 0 ] || { echo "no command" >&2; exit 2; }
root="$(cd "$root" && pwd)"
[ -x "$root/usr/lib/dalinira-browser/dalinira-browser" ] || { echo "not a staged DaliNira install: $root" >&2; exit 2; }

# Mount points must already exist: an unprivileged namespace cannot create
# them in root-owned directories.
for dir in /usr/lib/dalinira-browser /usr/share/dalinira-browser; do
  [ -d "$dir" ] || { echo "$dir must exist on the host (an installed DaliNira or an empty directory)" >&2; exit 2; }
done
args=(--dev-bind / / --tmpfs /usr/share/doc)
for dir in usr/lib/dalinira-browser usr/share/dalinira-browser usr/share/doc/dalinira-browser; do
  [ -d "$root/$dir" ] && args+=(--ro-bind "$root/$dir" "/$dir")
done
# The launcher: a symlink into the application directory, or the CMake
# launcher script. A host symlink already points into the mounted directory.
if [ -L /usr/bin/dalinira-browser ]; then
  [ "$(readlink /usr/bin/dalinira-browser)" = "../lib/dalinira-browser/dalinira-browser" ] \
    || { echo "unexpected /usr/bin/dalinira-browser symlink on the host" >&2; exit 2; }
elif [ -e "$root/usr/bin/dalinira-browser" ]; then
  args+=(--ro-bind "$root/usr/bin/dalinira-browser" /usr/bin/dalinira-browser)
fi
args+=(--setenv DALINIRA_STAGED_INSTALL "$root")

exec bwrap "${args[@]}" -- "$@"
