#pragma once

#include "engine/engine_type.h"
#include <QByteArray>
#include <QList>
#include <QNetworkCookie>
#include <QString>
#include <QUrl>

namespace dalinira::engine {

struct EngineDownloadContext {
  EngineType sourceEngine = EngineType::Chromium;
  uint64_t sourceTabId = 0;
  QUrl url;
  QUrl pageUrl;
  QUrl originalUrl;
  QString suggestedName;
  QString mimeType;
  qint64 totalBytes = -1;
  QByteArray userAgent;
  QByteArray referrer;
  QList<QNetworkCookie> cookies;
  bool isPrivate = false;
  quint64 navigationGeneration = 0;
};

using WebKitDownloadContext = EngineDownloadContext;

}  // namespace dalinira::engine

Q_DECLARE_METATYPE(dalinira::engine::EngineDownloadContext)
