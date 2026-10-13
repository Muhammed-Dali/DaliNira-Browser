#pragma once

#include <QString>
#include <optional>

namespace dalinira::engine {

enum class EngineType {
  Chromium,
  WebKit,
  Gecko
};

// The engine's stable key: the persisted `browser/engine` setting, the
// --engine value, and the name of its workspace scopes (sessions, UI
// settings, New Tab data). Chromium is the default engine.
inline QString engineKey(EngineType type) {
  switch (type) {
    case EngineType::Chromium:
      return QStringLiteral("chromium");
    case EngineType::WebKit:
      return QStringLiteral("webkit");
    case EngineType::Gecko:
      return QStringLiteral("gecko");
  }
  return QStringLiteral("chromium");
}

// The engine a key names, ignoring case and surrounding whitespace; none for
// an empty or unknown key.
inline std::optional<EngineType> parseEngineKey(const QString &key) {
  const QString normalized = key.trimmed().toLower();
  if (normalized == engineKey(EngineType::Chromium)) return EngineType::Chromium;
  if (normalized == engineKey(EngineType::WebKit)) return EngineType::WebKit;
  if (normalized == engineKey(EngineType::Gecko)) return EngineType::Gecko;
  return std::nullopt;
}

// As parseEngineKey(), with Chromium for an empty or unknown key.
inline EngineType engineTypeFromKey(const QString &key) {
  return parseEngineKey(key).value_or(EngineType::Chromium);
}

inline QString engineTypeName(EngineType type) {
  switch (type) {
    case EngineType::Chromium:
      return QStringLiteral("Chromium");
    case EngineType::WebKit:
      return QStringLiteral("WebKit");
    case EngineType::Gecko:
      return QStringLiteral("Gecko");
  }
  return QStringLiteral("Unknown");
}

inline QString engineLayoutEngineName(EngineType type) {
  switch (type) {
    case EngineType::Chromium:
      return QStringLiteral("Blink");
    case EngineType::WebKit:
      return QStringLiteral("WebCore");
    case EngineType::Gecko:
      return QStringLiteral("Gecko");
  }
  return QStringLiteral("Unknown");
}

inline QString engineJsEngineName(EngineType type) {
  switch (type) {
    case EngineType::Chromium:
      return QStringLiteral("V8");
    case EngineType::WebKit:
      return QStringLiteral("JavaScriptCore");
    case EngineType::Gecko:
      return QStringLiteral("SpiderMonkey");
  }
  return QStringLiteral("Unknown");
}

} // namespace dalinira::engine
