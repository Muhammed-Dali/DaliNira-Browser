#pragma once

#include <QString>

namespace dalinira::engine {

enum class EngineType {
  Chromium,
  WebKit
};

inline QString engineTypeName(EngineType type) {
  switch (type) {
    case EngineType::Chromium:
      return QStringLiteral("Chromium");
    case EngineType::WebKit:
      return QStringLiteral("WebKit");
  }
  return QStringLiteral("Unknown");
}

inline QString engineLayoutEngineName(EngineType type) {
  switch (type) {
    case EngineType::Chromium:
      return QStringLiteral("Blink");
    case EngineType::WebKit:
      return QStringLiteral("WebCore");
  }
  return QStringLiteral("Unknown");
}

inline QString engineJsEngineName(EngineType type) {
  switch (type) {
    case EngineType::Chromium:
      return QStringLiteral("V8");
    case EngineType::WebKit:
      return QStringLiteral("JavaScriptCore");
  }
  return QStringLiteral("Unknown");
}

} // namespace dalinira::engine
