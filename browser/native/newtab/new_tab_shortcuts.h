#pragma once

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include "core/engine_ui_profile.h"

namespace dalinira::newtab::shortcuts {
inline constexpr int limit = 10;
inline QUrl validatedUrl(const QString &text) {
  if (text.size() > 2048) return {};
  const QString input = text.trimmed();
  QUrl url(input.contains(QLatin1String("://")) ? input : QStringLiteral("https://") + input);
  if (!url.isValid() || url.host().isEmpty() ||
      (url.scheme() != QLatin1String("https") && url.scheme() != QLatin1String("http")) ||
      !url.userName().isEmpty() || !url.password().isEmpty()) return {};
  if (url.path().isEmpty()) url.setPath(QStringLiteral("/"));
  return url;
}
inline QString source() {
  const QString value = ui_profile::getVisualValue(QStringLiteral("browser/topSitesSource"), QStringLiteral("frequent")).toString();
  return value == QLatin1String("custom") || value == QLatin1String("bookmarks") ? value : QStringLiteral("frequent");
}
inline QJsonArray load() {
  const auto stored = QJsonDocument::fromJson(ui_profile::getVisualValue(QStringLiteral("browser/customShortcuts")).toByteArray()).array();
  QJsonArray result;
  for (const auto &value : stored) {
    const auto entry = value.toObject();
    const QUrl url = validatedUrl(entry.value(QStringLiteral("url")).toString());
    const QString name = entry.value(QStringLiteral("name")).toString().trimmed().left(180);
    if (url.isEmpty() || name.isEmpty()) continue;
    result.append(QJsonObject{{QStringLiteral("url"), url.toString(QUrl::FullyEncoded)}, {QStringLiteral("name"), name}});
    if (result.size() == limit) break;
  }
  return result;
}
inline bool add(const QString &name, const QString &text) {
  const QUrl url = validatedUrl(text);
  const QString title = name.trimmed();
  if (url.isEmpty() || title.isEmpty() || title.size() > 180) return false;
  QJsonArray entries = load();
  if (entries.size() >= limit) return false;
  for (const auto &entry : entries)
    if (QUrl(entry.toObject().value(QStringLiteral("url")).toString()) == url) return false;
  entries.append(QJsonObject{{QStringLiteral("name"), title}, {QStringLiteral("url"), url.toString(QUrl::FullyEncoded)}});
  ui_profile::setVisualValue(QStringLiteral("browser/customShortcuts"), QJsonDocument(entries).toJson(QJsonDocument::Compact));
  ui_profile::setVisualValue(QStringLiteral("browser/topSitesSource"), QStringLiteral("custom"));
  return true;
}
inline void remove(const QUrl &url) {
  QJsonArray entries;
  for (const auto &entry : load())
    if (QUrl(entry.toObject().value(QStringLiteral("url")).toString()) != validatedUrl(url.toString())) entries.append(entry);
  ui_profile::setVisualValue(QStringLiteral("browser/customShortcuts"), QJsonDocument(entries).toJson(QJsonDocument::Compact));
}
inline bool edit(const QString &original, const QString &name, const QString &text) {
  const QUrl oldUrl = validatedUrl(original), url = validatedUrl(text);
  const QString title = name.trimmed();
  if (oldUrl.isEmpty() || url.isEmpty() || title.isEmpty() || title.size() > 180) return false;
  QJsonArray entries = load();
  int found = -1;
  for (int i = 0; i < entries.size(); ++i) {
    const QUrl stored(entries[i].toObject().value(QStringLiteral("url")).toString());
    if (stored == oldUrl) found = i;
    else if (stored == url) return false;
  }
  if (found < 0) return false;
  entries[found] = QJsonObject{{QStringLiteral("name"), title}, {QStringLiteral("url"), url.toString(QUrl::FullyEncoded)}};
  ui_profile::setVisualValue(QStringLiteral("browser/customShortcuts"), QJsonDocument(entries).toJson(QJsonDocument::Compact));
  return true;
}
} // namespace dalinira::newtab::shortcuts
