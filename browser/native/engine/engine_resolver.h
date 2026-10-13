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
        if (!isBuilt(*requested)) {
          qWarning().noquote() << QStringLiteral("%1 backend requested via CLI, but DaliNira was compiled without %1 support. Falling back to Chromium.")
                                      .arg(engineTypeName(*requested));
          return resolution(EngineType::Chromium, EngineResolutionSource::CliOverride);
        }
        return resolution(*requested, EngineResolutionSource::CliOverride);
      }
    }

    // 2. Otherwise persisted DaliNira engine preference
    if (const auto persisted = parseEngineKey(getPersistedEngine())) {
      if (!isBuilt(*persisted)) {
        qWarning().noquote() << QStringLiteral("%1 backend persisted in settings, but DaliNira was compiled without %1 support. Falling back to Chromium.")
                                    .arg(engineTypeName(*persisted));
        return resolution(EngineType::Chromium, EngineResolutionSource::Settings);
      }
      return resolution(*persisted, EngineResolutionSource::Settings);
    }

    // 3. Otherwise Chromium default
    return resolution(EngineType::Chromium, EngineResolutionSource::Default);
  }

  // Whether this build has the engine's backend. The Gecko backend does not
  // exist yet: DALINIRA_BUILD_GECKO is never defined (CMake refuses
  // DALINIRA_ENABLE_GECKO=ON), so Gecko always falls back to Chromium.
  static constexpr bool isBuilt(EngineType engine) {
    switch (engine) {
      case EngineType::Chromium:
        return true;
      case EngineType::WebKit:
#if defined(DALINIRA_BUILD_WEBKIT)
        return true;
#else
        return false;
#endif
      case EngineType::Gecko:
#if defined(DALINIRA_BUILD_GECKO)
        return true;
#else
        return false;
#endif
    }
    return false;
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
