#!/usr/bin/env bash
# Every place that states DaliNira's version must agree: CMake (binary,
# --version, .deb/.rpm), package.json (release tag check), the Arch/AUR
# PKGBUILDs and all committed .SRCINFO files. Runs in CI on every push.
set -euo pipefail
cd "$(dirname "$0")/.."

cmake_version="$(sed -n 's/^project(DaliNiraBrowser VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)"
[ -n "$cmake_version" ] || { echo "cannot read the CMake project version" >&2; exit 1; }
status=0
check() {
  if [ "$2" != "$cmake_version" ]; then
    echo "version mismatch: $1 has '$2', CMakeLists.txt has '$cmake_version'" >&2
    status=1
  fi
}
check package.json "$(sed -n 's/^  "version": "\([^"]*\)".*/\1/p' package.json)"
for pkgbuild in packaging/archlinux/PKGBUILD packaging/pacman/PKGBUILD packaging/aur/*/PKGBUILD; do
  check "$pkgbuild" "$(sed -n 's/^pkgver=//p' "$pkgbuild")"
done
for srcinfo in packaging/archlinux/.SRCINFO packaging/pacman/.SRCINFO packaging/aur/*/.SRCINFO; do
  check "$srcinfo" "$(sed -n 's/^[[:space:]]*pkgver = //p' "$srcinfo")"
  # The AUR metadata must describe the PKGBUILD next to it.
  for field in depends optdepends makedepends provides conflicts replaces source; do
    from_pkgbuild="$(cd "$(dirname "$srcinfo")" && bash -c 'source ./PKGBUILD; printf "%s\n" "${'"$field"'[@]}"' | sort)"
    from_srcinfo="$(sed -n "s/^[[:space:]]*${field} = //p" "$srcinfo" | sort)"
    if [ "$from_pkgbuild" != "$from_srcinfo" ]; then
      echo "$srcinfo: '$field' differs from its PKGBUILD (regenerate with makepkg --printsrcinfo)" >&2
      status=1
    fi
  done
done
# The private WebKitGTK runtime: the Arch source recipe builds the release
# pinned in packaging/webkit-runtime/webkitgtk-runtime.env, with its checksum.
# shellcheck source=../packaging/webkit-runtime/webkitgtk-runtime.env
source packaging/webkit-runtime/webkitgtk-runtime.env
if [ "$(sed -n 's/^_webkitver=//p' packaging/archlinux/PKGBUILD)" != "$WEBKITGTK_VERSION" ]; then
  echo "packaging/archlinux/PKGBUILD: _webkitver differs from webkitgtk-runtime.env ($WEBKITGTK_VERSION)" >&2
  status=1
fi
if ! grep -q "'${WEBKITGTK_SHA256}'" packaging/archlinux/PKGBUILD; then
  echo "packaging/archlinux/PKGBUILD: the WebKitGTK checksum differs from webkitgtk-runtime.env" >&2
  status=1
fi

# Packaged file names stay ASCII: makepkg's bsdtar cannot translate others
# (e.g. a zero-width space) under every locale, and they are invisible.
non_ascii="$(find browser/resources browser/assets i18n -print0 | LC_ALL=C grep -zP '[^\x20-\x7e]' | tr '\0' '\n' || true)"
if [ -n "$non_ascii" ]; then
  echo "non-ASCII file names in packaged directories:" >&2
  echo "$non_ascii" >&2
  status=1
fi

# Stage 1 pre-push contact audit. Keep real project contact data out of tests
# and keep sample identities out of release metadata.
project_contact='dali.nira.12.1453kerinyoli'"@gmail.com"
sample_email='user'"@example.com"

for retired_contact in \
  'muhammeddali1453'"@gmail.com" \
  'muhammet'"@gmail.com" \
  'support'"@dalinira.app"; do
  retired_matches="$(git grep -n -F -e "$retired_contact" -- . || true)"
  if [ -n "$retired_matches" ]; then
    echo "retired email address remains in release files: $retired_contact" >&2
    echo "$retired_matches" >&2
    status=1
  fi
done

expected_contact_files=(
  .github/workflows/aur-publish.yml
  packaging/archlinux/PKGBUILD
  packaging/aur/dalinira-bin/PKGBUILD
  packaging/aur/dalinira/PKGBUILD
  packaging/linux/cpack.cmake
  packaging/pacman/PKGBUILD
)
for contact_file in "${expected_contact_files[@]}"; do
  if ! grep -Fq "$project_contact" "$contact_file"; then
    echo "$contact_file: missing the DaliNira project contact email" >&2
    status=1
  fi
done

while IFS= read -r contact_file; do
  [ -z "$contact_file" ] && continue
  case "$contact_file" in
    .github/workflows/aur-publish.yml|packaging/archlinux/PKGBUILD|packaging/aur/dalinira-bin/PKGBUILD|packaging/aur/dalinira/PKGBUILD|packaging/linux/cpack.cmake|packaging/pacman/PKGBUILD) ;;
    # Authorship of DaliNira's WebKitGTK patches (their From: header).
    packaging/webkit-runtime/patches/*.patch) ;;
    # Settings > About > Feedback > Send Email: the one in-app destination.
    browser/native/settings/feedback_links.cpp) ;;
    *)
      echo "$contact_file: real project email is outside approved contact metadata" >&2
      status=1
      ;;
  esac
done < <(git grep -l -F -e "$project_contact" -- . || true)

while IFS= read -r sample_file; do
  [ -z "$sample_file" ] && continue
  case "$sample_file" in
    SECURITY.md|browser/native/tests/*) ;;
    *)
      echo "$sample_file: sample email is outside tests or documentation examples" >&2
      status=1
      ;;
  esac
done < <(git grep -l -F -e "$sample_email" -- . || true)

# The test-only system-WebKit switch must never reach a package or release
# build. It is allowed in CMakeLists.txt, the CI test workflow and Markdown
# documentation that explains it; recipes, scripts and release workflows fail.
system_webkit_flag='DALINIRA_ALLOW_SYSTEM_WEBKIT_FOR_TESTS'
system_webkit_matches="$(grep -rl --exclude='*.md' -F -e "$system_webkit_flag" -- \
  packaging scripts .github/workflows/packages.yml .github/workflows/release.yml \
  .github/workflows/aur-publish.yml .github/workflows/pacman-repo.yml 2>/dev/null | grep -v '^scripts/check-release-metadata\.sh$' || true)"
if [ -n "$system_webkit_matches" ]; then
  echo "$system_webkit_flag is test-only but appears in release/packaging files:" >&2
  echo "$system_webkit_matches" >&2
  status=1
fi

[ "$status" -eq 0 ] && echo "release metadata and contact email audit consistent: ${cmake_version}"
exit "$status"
