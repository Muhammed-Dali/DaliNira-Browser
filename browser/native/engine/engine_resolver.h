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
        const auto requested = parseEngineKey(eng);
        if (!requested) {
          qWarning() << "Unknown engine option:" << eng << "- falling back to default Chromium.";
          return resolution(EngineType::Chromium, EngineResolutionSource::CliOverride);
        }
#if !defined(DALINIRA_BUILD_WEBKIT)
        if (*requested == EngineType::WebKit) {
          qWarning() << "WebKit backend requested via CLI, but DaliNira was compiled without WebKit support. Falling back to Chromium.";
          return resolution(EngineType::Chromium, EngineResolutionSource::CliOverride);
        }
#endif
        return resolution(*requested, EngineResolutionSource::CliOverride);
      }
    }

    // 2. Otherwise persisted DaliNira engine preference
    if (const auto persisted = parseEngineKey(getPersistedEngine())) {
#if !defined(DALINIRA_BUILD_WEBKIT)
      if (*persisted == EngineType::WebKit) {
        qWarning() << "WebKit backend persisted in settings, but DaliNira was compiled without WebKit support. Falling back to Chromium.";
        return resolution(EngineType::Chromium, EngineResolutionSource::Settings);
      }
#endif
      return resolution(*persisted, EngineResolutionSource::Settings);
    }

    // 3. Otherwise Chromium default
    return resolution(EngineType::Chromium, EngineResolutionSource::Default);
  }

  // As resolve(), without settings or build options; an unknown --engine
  // value is skipped instead of meaning Chromium.
  static EngineResolution resolvePure(const QString &persistedEngine, const QStringList &args) {
    for (int i = 1; i < args.size(); ++i) {
      if (args[i] == QLatin1String("--engine") && i + 1 < args.size()) {
        if (const auto requested = parseEngineKey(args[i + 1]))
          return resolution(*requested, EngineResolutionSource::CliOverride);
      }
    }
    if (const auto persisted = parseEngineKey(persistedEngine))
      return resolution(*persisted, EngineResolutionSource::Settings);
    return resolution(EngineType::Chromium, EngineResolutionSource::Default);
  }

  static QString getPersistedEngine() {
    QSettings settings(QString::fromLatin1(dalinira::application_identity::kOrganizationName),
                       QString::fromLatin1(dalinira::application_identity::kApplicationName));
    return settings.value(QStringLiteral("browser/engine"), engineKey(EngineType::Chromium)).toString().trimmed().toLower();
  }

  static void setPersistedEngine(const QString &engine) {
    QSettings settings(QString::fromLatin1(dalinira::application_identity::kOrganizationName),
                       QString::fromLatin1(dalinira::application_identity::kApplicationName));
    settings.setValue(QStringLiteral("browser/engine"), engine.trimmed().toLower());
    settings.sync();
  }

 private:
  static EngineResolution resolution(EngineType engine, EngineResolutionSource source) {
    return {engine, source == EngineResolutionSource::CliOverride, source, engineKey(engine)};
  }
};

} // namespace dalinira::engine
