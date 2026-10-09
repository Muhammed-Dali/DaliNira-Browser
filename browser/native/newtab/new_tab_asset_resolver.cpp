#include "new_tab_asset_resolver.h"

#include <QDir>
#include <QFile>
#include <QStringList>

QString resolveNewTabAssetsDirectory(const QString &applicationDirectory) {
  const QDir appDirectory(applicationDirectory);
  const QStringList candidates{
      appDirectory.absoluteFilePath(QStringLiteral("assets/new-tab")),
      appDirectory.absoluteFilePath(QStringLiteral("../share/dalinira-browser/new-tab")),
      appDirectory.absoluteFilePath(QStringLiteral("../../share/dalinira-browser/new-tab"))};

  for (const QString &candidate : candidates) {
    if (QDir(candidate).exists()) return QDir::cleanPath(candidate);
  }
  return QDir::cleanPath(candidates.constFirst());
}

bool newTabStaticAsset(const QString &requestPath, NewTabStaticAsset *asset) {
  struct Entry {
    const char *path;
    const char *mime;
    const char *resource;
  };
  static constexpr Entry entries[] = {
      {"/dalinira-flow-blue.png", "image/png", ":/new-tab/dalinira-flow-blue.png"},
      {"/dalinira-browser.png", "image/png", ":/assets/icons/dalinira-browser-128.png"},
      {"/google.ico", "image/x-icon", ":/search-engines/google.ico"},
      {"/duckduckgo.ico", "image/x-icon", ":/search-engines/duckduckgo.ico"},
      {"/startpage.ico", "image/x-icon", ":/search-engines/startpage.ico"},
      {"/mojeek.ico", "image/x-icon", ":/search-engines/mojeek.ico"},
      {"/icons/appearance.svg", "image/svg+xml", ":/browser-icons/appearance.svg"},
      {"/icons/search.svg", "image/svg+xml", ":/browser-icons/search.svg"},
      {"/icons/grid.svg", "image/svg+xml", ":/browser-icons/grid.svg"},
      {"/icons/clock.svg", "image/svg+xml", ":/browser-icons/clock.svg"},
      {"/icons/cards.svg", "image/svg+xml", ":/browser-icons/cards.svg"},
      {"/icons/close.svg", "image/svg+xml", ":/browser-icons/close.svg"},
      {"/icons/settings.svg", "image/svg+xml", ":/browser-icons/settings.svg"},
  };
  for (const Entry &entry : entries) {
    if (requestPath != QLatin1String(entry.path)) continue;
    if (asset) {
      asset->mimeType = QByteArray(entry.mime);
      asset->fileName = QString::fromLatin1(entry.path).mid(1);
      asset->embeddedResource = QString::fromLatin1(entry.resource);
    }
    return true;
  }
  return false;
}

QByteArray readNewTabStaticAsset(const NewTabStaticAsset &asset, const QString &assetsDirectory) {
  QFile file(QDir(assetsDirectory).filePath(asset.fileName));
  if (!assetsDirectory.isEmpty() && file.open(QIODevice::ReadOnly)) return file.readAll();
  QFile embedded(asset.embeddedResource);
  return embedded.open(QIODevice::ReadOnly) ? embedded.readAll() : QByteArray();
}
