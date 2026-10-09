#pragma once

#include <QString>
#include <QStringList>
#include <QSettings>
#include <QDebug>
#include "engine/engine_backend.h"
#include "core/application_identity.h"

namespace dalinira::engine {

enum class EngineResolutionSource {
  CliOverride,
  Settings,
  Default
};

struct EngineResolution {
  EngineType engine = EngineType::Chromium;
  bool isCliOverride = false;
  EngineResolutionSource source = EngineResolutionSource::Default;
  QString engineName;
};

class EngineResolver {
 public:
  static EngineResolution resolve(const QStringList &args) {
    // 1. Explicit CLI override takes precedence for development/testing
    for (int i = 1; i < args.size(); ++i) {
      if (args[i] == QLatin1String("--engine") && i + 1 < args.size()) {
        const QString eng = args[i + 1].trimmed().toLower();
        if (eng == QLatin1String("webkit")) {
#if defined(DALINIRA_BUILD_WEBKIT)
          return {EngineType::WebKit, true, EngineResolutionSource::CliOverride, QStringLiteral("webkit")};
#else
          qWarning() << "WebKit backend requested via CLI, but DaliNira was compiled without WebKit support. Falling back to Chromium.";
          return {EngineType::Chromium, true, EngineResolutionSource::CliOverride, QStringLiteral("chromium")};
#endif
        } else if (eng == QLatin1String("chromium")) {
          return {EngineType::Chromium, true, EngineResolutionSource::CliOverride, QStringLiteral("chromium")};
        } else {
          qWarning() << "Unknown engine option:" << eng << "- falling back to default Chromium.";
          return {EngineType::Chromium, true, EngineResolutionSource::CliOverride, QStringLiteral("chromium")};
        }
      }
    }

    // 2. Otherwise persisted DaliNira engine preference
    const QString persisted = getPersistedEngine();
    if (persisted == QLatin1String("webkit")) {
#if defined(DALINIRA_BUILD_WEBKIT)
      return {EngineType::WebKit, false, EngineResolutionSource::Settings, QStringLiteral("webkit")};
#else
      qWarning() << "WebKit backend persisted in settings, but DaliNira was compiled without WebKit support. Falling back to Chromium.";
      return {EngineType::Chromium, false, EngineResolutionSource::Settings, QStringLiteral("chromium")};
#endif
    } else if (persisted == QLatin1String("chromium")) {
      return {EngineType::Chromium, false, EngineResolutionSource::Settings, QStringLiteral("chromium")};
    }

    // 3. Otherwise Chromium default
    return {EngineType::Chromium, false, EngineResolutionSource::Default, QStringLiteral("chromium")};
  }

  static EngineResolution resolvePure(const QString &persistedEngine, const QStringList &args) {
    for (int i = 1; i < args.size(); ++i) {
      if (args[i] == QLatin1String("--engine") && i + 1 < args.size()) {
        const QString eng = args[i + 1].trimmed().toLower();
        if (eng == QLatin1String("webkit")) {
          return {EngineType::WebKit, true, EngineResolutionSource::CliOverride, QStringLiteral("webkit")};
        } else if (eng == QLatin1String("chromium")) {
          return {EngineType::Chromium, true, EngineResolutionSource::CliOverride, QStringLiteral("chromium")};
        }
      }
    }
    const QString persisted = persistedEngine.trimmed().toLower();
    if (persisted == QLatin1String("webkit")) {
      return {EngineType::WebKit, false, EngineResolutionSource::Settings, QStringLiteral("webkit")};
    } else if (persisted == QLatin1String("chromium")) {
      return {EngineType::Chromium, false, EngineResolutionSource::Settings, QStringLiteral("chromium")};
    }
    return {EngineType::Chromium, false, EngineResolutionSource::Default, QStringLiteral("chromium")};
  }

  static QString getPersistedEngine() {
    QSettings settings(QString::fromLatin1(dalinira::application_identity::kOrganizationName),
                       QString::fromLatin1(dalinira::application_identity::kApplicationName));
    return settings.value(QStringLiteral("browser/engine"), QStringLiteral("chromium")).toString().trimmed().toLower();
  }

  static void setPersistedEngine(const QString &engine) {
    QSettings settings(QString::fromLatin1(dalinira::application_identity::kOrganizationName),
                       QString::fromLatin1(dalinira::application_identity::kApplicationName));
    settings.setValue(QStringLiteral("browser/engine"), engine.trimmed().toLower());
    settings.sync();
  }
};

} // namespace dalinira::engine
