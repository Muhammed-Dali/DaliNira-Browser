#pragma once

#include <QString>
#include <QVariant>
#include <QSettings>
#include "engine/engine_backend.h"

namespace dalinira::ui_profile {

inline QString profilePrefix() {
  const auto eng = ::dalinira::engine::EngineBackend::selectedEngineType();
  return (eng == ::dalinira::engine::EngineType::WebKit) ? QStringLiteral("ui_webkit/") : QStringLiteral("ui_chromium/");
}

inline QString scopedKey(const QString &baseKey) {
  if (baseKey.startsWith(QLatin1String("ui_webkit/")) || baseKey.startsWith(QLatin1String("ui_chromium/"))) {
    return baseKey;
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
  // Chromium UI. Never leak them into a fresh WebKit appearance profile.
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
