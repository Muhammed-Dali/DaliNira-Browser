#!/usr/bin/env bash
# ==============================================================================
# DaliNira Browser - Signed Pacman Repository Publisher
# ==============================================================================
# Bu betik:
# 1. Arch Linux paketlerini (.pkg.tar.zst) GPG ile imzalar (.pkg.tar.zst.sig)
# 2. GPG açık anahtarını (dalinira.gpg) dışa aktarır
# 3. repo-add --sign ile imzalı depo veritabanını (dalinira.db.tar.zst) üretir
# 4. GitHub Releases 'pacman-repo' etiketine tüm imzalı varlıkları yükler
# ==============================================================================

set -euo pipefail

# Varsayılan Yapılandırma
GPG_KEY_DEFAULT="BC741FD0AC804351B0DDBB86FDFEC60C11202588"
GPG_KEY="${GPG_KEY:-$GPG_KEY_DEFAULT}"
RELEASE_TAG="${RELEASE_TAG:-pacman-repo}"
REPO_NAME="dalinira"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
PACKAGING_DIR="${WORKSPACE_ROOT}/packaging/pacman"
DIST_DIR="${WORKSPACE_ROOT}/dist/pacman-repo"

usage() {
  cat <<EOF
Kullanım: $(basename "$0") [SEÇENEKLER] [PAKET_DOSYALARI...]

Seçenekler:
  -k, --key <KEY_ID>       GPG Anahtar ID veya Parmak İzi (Varsayılan: ${GPG_KEY_DEFAULT})
  -t, --tag <TAG_NAME>     GitHub Releases etiket adı (Varsayılan: ${RELEASE_TAG})
  -b, --build              packaging/pacman içinde makepkg ile paketi derle
  -d, --dir <DIR>          Çıktı ve imzalama çalışma dizini (Varsayılan: dist/pacman-repo)
  -h, --help               Bu yardım mesajını göster

Örnekler:
  $(basename "$0")
  $(basename "$0") --build
  $(basename "$0") packaging/pacman/dalinira-8.0.2-1-x86_64.pkg.tar.zst
  $(basename "$0") -k ${GPG_KEY_DEFAULT}
EOF
  exit 0
}

BUILD_PACKAGE=false
CUSTOM_PACKAGES=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    -k|--key)
      GPG_KEY="$2"
      shift 2
      ;;
    -t|--tag)
      RELEASE_TAG="$2"
      shift 2
      ;;
    -b|--build)
      BUILD_PACKAGE=true
      shift
      ;;
    -d|--dir)
      DIST_DIR="$2"
      shift 2
      ;;
    -h|--help)
      usage
      ;;
    *.pkg.tar.zst)
      CUSTOM_PACKAGES+=("$1")
      shift
      ;;
    *)
      echo "Hata: Bilinmeyen parametre: $1" >&2
      usage
      ;;
  esac
done

# ------------------------------------------------------------------------------
# 1. Gerekli Araç ve Yetki Kontrolleri
# ------------------------------------------------------------------------------
echo "==> [1/6] Bağımlılıklar ve GPG anahtarı kontrol ediliyor..."
for cmd in gpg repo-add gh; do
  if ! command -v "$cmd" >/dev/null 2>&1; then
    echo "Hata: '$cmd' komutu bulunamadı. Lütfen ilgili paketi sisteme kurun." >&2
    exit 1
  fi
done

# GPG anahtarının yerel sistemde varlığını doğrula
if ! gpg --list-secret-keys "${GPG_KEY}" >/dev/null 2>&1; then
  echo "Hata: GPG gizli anahtarı (${GPG_KEY}) yerel anahtarlıkta bulunamadı!" >&2
  echo "Lütfen 'gpg --list-secret-keys' çıktısını kontrol edin." >&2
  exit 1
fi
echo "✓ GPG anahtarı doğrulandı: ${GPG_KEY}"

# GitHub CLI oturumunu kontrol et
if ! gh auth status >/dev/null 2>&1; then
  echo "Hata: GitHub CLI oturumu açık değil. Lütfen 'gh auth login' yapın." >&2
  exit 1
fi
echo "✓ GitHub CLI bağlantısı doğrulandı."

# ------------------------------------------------------------------------------
# 2. Paketlerin Hazırlanması
# ------------------------------------------------------------------------------
echo "==> [2/6] Paket dosyaları hazırlanıyor..."
mkdir -p "${DIST_DIR}"

if [[ "${BUILD_PACKAGE}" == "true" ]]; then
  echo "    Paket makepkg ile derleniyor (${PACKAGING_DIR})..."
  (cd "${PACKAGING_DIR}" && makepkg -f --nodeps --noconfirm)
fi

PACKAGES=()
if [[ ${#CUSTOM_PACKAGES[@]} -gt 0 ]]; then
  for p in "${CUSTOM_PACKAGES[@]}"; do
    if [[ -f "$p" ]]; then
      cp -f "$p" "${DIST_DIR}/"
      PACKAGES+=("${DIST_DIR}/$(basename "$p")")
    else
      echo "Uyarı: Belirtilen paket bulunamadı: $p" >&2
    fi
  done
else
  # packaging/pacman veya dist dizininden paketleri topla
  while IFS= read -r -d '' pkg; do
    cp -f "${pkg}" "${DIST_DIR}/"
    PACKAGES+=("${DIST_DIR}/$(basename "${pkg}")")
  done < <(find "${PACKAGING_DIR}" -maxdepth 1 -name "*.pkg.tar.zst" -print0 2>/dev/null)
fi

# Eğer yerelde paket yoksa, mevcut GitHub Releases 'pacman-repo' etiketinden en son paketi çek
if [[ ${#PACKAGES[@]} -eq 0 ]]; then
  echo "    Yerelde paket bulunamadı, GitHub Releases '${RELEASE_TAG}' üzerinden mevcut paket indiriliyor..."
  if gh release view "${RELEASE_TAG}" >/dev/null 2>&1; then
    latest_pkg="$(gh release view "${RELEASE_TAG}" --json assets -q '.assets[] | select(.name | test("^dalinira-[0-9].*\\.pkg\\.tar\\.zst$")) | .name' | sort -V | tail -n 1)"
    if [[ -n "${latest_pkg}" ]]; then
      echo "    İndiriliyor: ${latest_pkg}..."
      gh release download "${RELEASE_TAG}" --pattern "${latest_pkg}" --dir "${DIST_DIR}" --clobber
      PACKAGES+=("${DIST_DIR}/${latest_pkg}")
    fi
  fi
fi

if [[ ${#PACKAGES[@]} -eq 0 ]]; then
  echo "Hata: İmzalanacak hiçbir .pkg.tar.zst paketi bulunamadı!" >&2
  echo "Lütfen bir paket yolu belirtin veya '--build' parametresini kullanın." >&2
  exit 1
fi

echo "✓ İşlenecek paket(ler):"
for pkg in "${PACKAGES[@]}"; do
  echo "    - $(basename "${pkg}")"
done

# ------------------------------------------------------------------------------
# 3. Paketleri GPG ile İmzalama (.sig)
# ------------------------------------------------------------------------------
echo "==> [3/6] Paketler GPG ile imzalanıyor (.sig)..."
for pkg in "${PACKAGES[@]}"; do
  sig_file="${pkg}.sig"
  echo "    İmzalanıyor: $(basename "${pkg}") -> $(basename "${sig_file}")"
  rm -f "${sig_file}"
  gpg --batch --yes --detach-sign --default-key "${GPG_KEY}" --output "${sig_file}" "${pkg}"
  # İmzayı doğrula
  gpg --batch --verify "${sig_file}" "${pkg}" >/dev/null 2>&1 || {
    echo "Hata: İmza doğrulanamadı: ${sig_file}" >&2
    exit 1
  }
done
echo "✓ Tüm paketler başarıyla imzalandı."

# ------------------------------------------------------------------------------
# 4. GPG Açık Anahtarını (dalinira.gpg) Dışa Aktarma
# ------------------------------------------------------------------------------
echo "==> [4/6] Kullanıcılar için GPG açık anahtarı (dalinira.gpg) oluşturuluyor..."
PUBLIC_KEY_FILE="${DIST_DIR}/dalinira.gpg"
gpg --batch --yes --armor --export "${GPG_KEY}" > "${PUBLIC_KEY_FILE}"
echo "✓ Açık anahtar hazırlandı: ${PUBLIC_KEY_FILE}"

# ------------------------------------------------------------------------------
# 5. Pacman Repo Veritabanını Oluşturma ve İmzalama (repo-add --sign)
# ------------------------------------------------------------------------------
echo "==> [5/6] İmzalı depo veritabanı (repo-add --sign) oluşturuluyor..."
DB_ZST="${DIST_DIR}/${REPO_NAME}.db.tar.zst"
FILES_ZST="${DIST_DIR}/${REPO_NAME}.files.tar.zst"

# Eski geçici db dosyalarını temizle
rm -f "${DIST_DIR}/${REPO_NAME}".db* "${DIST_DIR}/${REPO_NAME}".files*

# repo-add --sign ile dalinira.db.tar.zst oluştur
repo-add --sign --key "${GPG_KEY}" "${DB_ZST}" "${DIST_DIR}"/*.pkg.tar.zst

# Pacman HTTP sunucularının hem doğrudan .db hem de .tar.zst uzantılarına yanıt verebilmesi için
# repo-add'in oluşturduğu sembolik bağları bağımsız dosyalara dönüştür
rm -f "${DIST_DIR}/${REPO_NAME}.db" "${DIST_DIR}/${REPO_NAME}.db.sig" \
      "${DIST_DIR}/${REPO_NAME}.files" "${DIST_DIR}/${REPO_NAME}.files.sig" \
      "${DIST_DIR}/${REPO_NAME}.db.tar.gz" "${DIST_DIR}/${REPO_NAME}.db.tar.gz.sig" \
      "${DIST_DIR}/${REPO_NAME}.files.tar.gz" "${DIST_DIR}/${REPO_NAME}.files.tar.gz.sig"

cp -f "${DB_ZST}" "${DIST_DIR}/${REPO_NAME}.db"
cp -f "${DB_ZST}.sig" "${DIST_DIR}/${REPO_NAME}.db.sig"
cp -f "${FILES_ZST}" "${DIST_DIR}/${REPO_NAME}.files"
cp -f "${FILES_ZST}.sig" "${DIST_DIR}/${REPO_NAME}.files.sig"

# Geriye dönük uyumluluk (tar.gz uzantısı arayan istemciler için)
cp -f "${DB_ZST}" "${DIST_DIR}/${REPO_NAME}.db.tar.gz"
cp -f "${DB_ZST}.sig" "${DIST_DIR}/${REPO_NAME}.db.tar.gz.sig"
cp -f "${FILES_ZST}" "${DIST_DIR}/${REPO_NAME}.files.tar.gz"
cp -f "${FILES_ZST}.sig" "${DIST_DIR}/${REPO_NAME}.files.tar.gz.sig"

echo "✓ Veritabanı ve imzalar oluşturuldu:"
ls -lh "${DIST_DIR}/${REPO_NAME}".*

# ------------------------------------------------------------------------------
# 6. GitHub Releases Üzerine Yükleme (gh release upload)
# ------------------------------------------------------------------------------
echo "==> [6/6] GitHub Releases '${RELEASE_TAG}' etiketine varlıklar yükleniyor..."
if ! gh release view "${RELEASE_TAG}" >/dev/null 2>&1; then
  echo "    '${RELEASE_TAG}' release etiketi bulunamadı, oluşturuluyor..."
  gh release create "${RELEASE_TAG}" \
    --title "DaliNira Pacman Repository" \
    --notes "Official signed Arch Linux pacman repository assets for DaliNira Browser."
fi

UPLOAD_FILES=(
  "${PUBLIC_KEY_FILE}"
  "${DIST_DIR}/${REPO_NAME}.db.tar.zst"
  "${DIST_DIR}/${REPO_NAME}.db.tar.zst.sig"
  "${DIST_DIR}/${REPO_NAME}.db.tar.gz"
  "${DIST_DIR}/${REPO_NAME}.db.tar.gz.sig"
  "${DIST_DIR}/${REPO_NAME}.db"
  "${DIST_DIR}/${REPO_NAME}.db.sig"
  "${DIST_DIR}/${REPO_NAME}.files.tar.zst"
  "${DIST_DIR}/${REPO_NAME}.files.tar.zst.sig"
  "${DIST_DIR}/${REPO_NAME}.files.tar.gz"
  "${DIST_DIR}/${REPO_NAME}.files.tar.gz.sig"
  "${DIST_DIR}/${REPO_NAME}.files"
  "${DIST_DIR}/${REPO_NAME}.files.sig"
)

# Paketleri ve paket imzalarını listeye ekle
for pkg in "${DIST_DIR}"/*.pkg.tar.zst; do
  UPLOAD_FILES+=("${pkg}")
  if [[ -f "${pkg}.sig" ]]; then
    UPLOAD_FILES+=("${pkg}.sig")
  fi
done

echo "    Yüklenen dosya sayısı: ${#UPLOAD_FILES[@]}"
gh release upload "${RELEASE_TAG}" "${UPLOAD_FILES[@]}" --clobber

REPO_SLUG="$(gh repo view --json nameWithOwner -q .nameWithOwner)"
echo ""
echo "=========================================================================="
echo "✓ İmzalı Arch Linux Pacman Deposu Başarıyla Güncellendi!"
echo "--------------------------------------------------------------------------"
echo "Depo Release URL: https://github.com/${REPO_SLUG}/releases/tag/${RELEASE_TAG}"
echo "GPG Açık Anahtar: https://github.com/${REPO_SLUG}/releases/download/${RELEASE_TAG}/dalinira.gpg"
echo "GPG Parmak İzi:   ${GPG_KEY}"
echo "=========================================================================="
