#include "dalinira_blocker_redirect_resources.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QMutexLocker>
#include <QRegularExpression>

namespace {
constexpr qint64 kMaximumResourceBytes = 1024 * 1024;

// Short names used by filter lists for the same packaged files: uBlock
// Origin's resource aliases and Adblock Plus's $rewrite=abp-resource: names.
// They only ever select one of the packaged files.
const QHash<QString, QString> &aliases() {
  static const QHash<QString, QString> table = {
      {QStringLiteral("noopjs"), QStringLiteral("noop.js")},
      {QStringLiteral("nooptext"), QStringLiteral("noop.txt")},
      {QStringLiteral("noopcss"), QStringLiteral("noop.css")},
      {QStringLiteral("noopjson"), QStringLiteral("noop.json")},
      {QStringLiteral("noopframe"), QStringLiteral("noop.html")},
      {QStringLiteral("noopmp3-0.1s"), QStringLiteral("noop-0.1s.mp3")},
      {QStringLiteral("noopmp4-1s"), QStringLiteral("noop-1s.mp4")},
      {QStringLiteral("noop-vmap1.0.xml"), QStringLiteral("noop-vmap1.xml")},
      {QStringLiteral("abp-resource:blank-js"), QStringLiteral("noop.js")},
      {QStringLiteral("abp-resource:blank-text"), QStringLiteral("noop.txt")},
      {QStringLiteral("abp-resource:blank-css"), QStringLiteral("noop.css")},
      {QStringLiteral("abp-resource:blank-html"), QStringLiteral("noop.html")},
      {QStringLiteral("abp-resource:blank-mp3"), QStringLiteral("noop-0.1s.mp3")},
      {QStringLiteral("abp-resource:blank-mp4"), QStringLiteral("noop-1s.mp4")},
      {QStringLiteral("abp-resource:1x1-transparent-gif"), QStringLiteral("1x1.gif")},
      {QStringLiteral("abp-resource:2x2-transparent-png"), QStringLiteral("2x2.png")},
      {QStringLiteral("abp-resource:32x32-transparent-png"), QStringLiteral("32x32.png")},
  };
  return table;
}
}  // namespace

DaliNiraBlockerRedirectResources::DaliNiraBlockerRedirectResources(const QString &directory)
    : directory_(directory) {
  // Plain files with plain names only: no links, no subdirectories.
  static const QRegularExpression safeName(QStringLiteral("^[A-Za-z0-9_][A-Za-z0-9_.-]{0,63}$"));
  const QDir dir(directory_);
  if (directory_.isEmpty() || !dir.exists()) return;
  for (const QFileInfo &info : dir.entryInfoList(QDir::Files | QDir::NoSymLinks)) {
    if (safeName.match(info.fileName()).hasMatch() && info.size() <= kMaximumResourceBytes)
      files_.insert(info.fileName());
  }
}

QString DaliNiraBlockerRedirectResources::canonicalName(const QString &token) const {
  const QString name = token.trimmed();
  const QString resolved = aliases().value(name.toLower(), name);
  return files_.contains(resolved) ? resolved : QString();
}

const DaliNiraBlockerRedirectResources::Resource &DaliNiraBlockerRedirectResources::load(
    const QString &canonicalName) const {
  Resource &resource = resources_[canonicalName];
  if (resource.loaded) return resource;
  resource.loaded = true;
  QFile file(QDir(directory_).filePath(canonicalName));
  if (!file.open(QIODevice::ReadOnly) || file.size() > kMaximumResourceBytes) return resource;
  resource.bytes = file.readAll();
  const QString mime = QMimeDatabase().mimeTypeForFile(canonicalName, QMimeDatabase::MatchExtension).name();
  resource.mimeType = mime.isEmpty() ? QStringLiteral("application/octet-stream") : mime;
  resource.dataUrl = QStringLiteral("data:%1;base64,%2")
                         .arg(resource.mimeType, QString::fromLatin1(resource.bytes.toBase64()));
  return resource;
}

QString DaliNiraBlockerRedirectResources::dataUrl(const QString &canonicalName) const {
  if (!files_.contains(canonicalName)) return {};
  QMutexLocker locker(&mutex_);
  return load(canonicalName).dataUrl;
}

QByteArray DaliNiraBlockerRedirectResources::content(const QString &canonicalName, QString *mimeType, bool *ok) const {
  *ok = false;
  if (!files_.contains(canonicalName)) return {};
  QMutexLocker locker(&mutex_);
  const Resource &resource = load(canonicalName);
  *ok = !resource.dataUrl.isEmpty();
  *mimeType = resource.mimeType;
  return resource.bytes;
}
