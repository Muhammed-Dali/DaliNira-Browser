#pragma once

#include <QString>
#include "engine/engine_type.h"

namespace dalinira::engine {

class EngineStorageManager {
 public:
  explicit EngineStorageManager(const QString &baseDataDir);

  QString baseDir() const;
  QString sharedDir() const;
  QString engineDir(EngineType type) const;
  QString cookiesPath(EngineType type) const;
  QString cacheDir(EngineType type) const;
  QString siteStorageDir(EngineType type) const;

  void ensureDirectories(EngineType type) const;

 private:
  QString baseDataDir_;
};

} // namespace dalinira::engine
