#pragma once

#include <QString>
#include <QVariant>
#include <QSettings>
#include "engine/engine_backend.h"

namespace dalinira::ui_profile {

inline QString profilePrefix(::dalinira::engine::EngineType engine) {
  return QStringLiteral("ui_") + ::dalinira::engine::engineKey(engine) + QLatin1Char('/');
}

inline QString profilePrefix() {
  return profilePrefix(::dalinira::engine::EngineBackend::selectedEngineType());
}

inline QString scopedKey(const QString &baseKey) {
  // A key already scoped to an engine keeps that scope.
  for (const auto engine : {::dalinira::engine::EngineType::Chromium, ::dalinira::engine::EngineType::WebKit,
                            ::dalinira::engine::EngineType::Gecko}) {
    if (baseKey.startsWith(profilePrefix(engine))) return baseKey;
  }
  return profilePrefix() + baseKey;
}

inline QVariant getVisualValue(const QString &key, const QVariant &defaultValue = QVariant()) {
  QSettings s;
  const QString scoped = scopedKey(key);
  if (s.contains(scoped)) {
    return s.value(scoped);
  }
  // Unscoped values predate engine profiles and belong to the original
  // Chromium UI. Never leak them into another engine's appearance profile.
  if (::dalinira::engine::EngineBackend::selectedEngineType() ==
          ::dalinira::engine::EngineType::Chromium && s.contains(key)) {
    return s.value(key);
  }
  return defaultValue;
}

inline void setVisualValue(const QString &key, const QVariant &value) {
  QSettings s;
  s.setValue(scopedKey(key), value);
}

inline void removeVisualValue(const QString &key) {
  QSettings s;
  s.remove(scopedKey(key));
}

} // namespace dalinira::ui_profile
