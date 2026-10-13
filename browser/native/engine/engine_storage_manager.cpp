#include "engine/engine_storage_manager.h"
#include <QDir>
#include <QFileInfo>

namespace dalinira::engine {

EngineStorageManager::EngineStorageManager(const QString &baseDataDir)
    : baseDataDir_(baseDataDir) {}

QString EngineStorageManager::baseDir() const {
  return baseDataDir_;
}

QString EngineStorageManager::sharedDir() const {
  return baseDataDir_ + QStringLiteral("/shared");
}

QString EngineStorageManager::engineDir(EngineType type) const {
  switch (type) {
    case EngineType::Chromium:
      return baseDataDir_ + QStringLiteral("/chromium");
    case EngineType::WebKit:
      return baseDataDir_ + QStringLiteral("/webkit");
    case EngineType::Gecko:
      return baseDataDir_ + QStringLiteral("/gecko");
  }
  return baseDataDir_ + QStringLiteral("/unknown");
}

QString EngineStorageManager::cookiesPath(EngineType type) const {
  return engineDir(type) + QStringLiteral("/cookies/cookies.db");
}

QString EngineStorageManager::cacheDir(EngineType type) const {
  return engineDir(type) + QStringLiteral("/cache");
}

QString EngineStorageManager::siteStorageDir(EngineType type) const {
  return engineDir(type) + QStringLiteral("/site-storage");
}

void EngineStorageManager::ensureDirectories(EngineType type) const {
  QDir().mkpath(sharedDir());
  QDir().mkpath(engineDir(type));
  QDir().mkpath(QFileInfo(cookiesPath(type)).dir().absolutePath());
  QDir().mkpath(cacheDir(type));
  QDir().mkpath(siteStorageDir(type));
}

} // namespace dalinira::engine
