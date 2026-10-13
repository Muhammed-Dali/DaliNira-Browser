// One engine identity for every engine-scoped name: engineKey() is the key
// persisted as `browser/engine`, given to --engine and used for the
// per-engine scopes (sessions, UI settings, New Tab data); engineTypeFromKey()
// and parseEngineKey() read such a key back.
//
// Besides the API itself, every conversion that now goes through it is
// compared with the expression it replaced (copied verbatim below as
// legacy*), over keys that differ in case, whitespace and Unicode, empty and
// unknown keys. SessionStore and BrowserProfileService normalization are
// covered with the real classes by browser-engine-workspace.

#include <QCoreApplication>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <cstdlib>
#include <iostream>
#include <utility>

#include "core/application_identity.h"
#include "core/engine_ui_profile.h"
#include "engine/engine_backend.h"
#include "engine/engine_resolver.h"
#include "engine/engine_type.h"

#define CHECK(condition)                                                                    \
  do {                                                                                      \
    if (!(condition)) {                                                                     \
      std::cout << "[FAIL] " << __FILE__ << ":" << __LINE__ << ": " #condition << std::endl; \
      std::_Exit(1);                                                                        \
    }                                                                                       \
  } while (false)

namespace {

using dalinira::engine::EngineBackend;
using dalinira::engine::EngineResolution;
using dalinira::engine::EngineResolutionSource;
using dalinira::engine::EngineResolver;
using dalinira::engine::EngineType;

const QList<EngineType> kEngines{EngineType::Chromium, EngineType::WebKit};

// Keys as settings, command lines and callers may spell them.
const QStringList kKeys{
    QStringLiteral("chromium"), QStringLiteral("webkit"), QStringLiteral("Chromium"),
    QStringLiteral("WEBKIT"), QStringLiteral("WebKit"), QStringLiteral(" webkit "),
    QStringLiteral("\twebkit\n"), QStringLiteral("  CHROMIUM"), QString(), QStringLiteral(""),
    QStringLiteral("   "), QStringLiteral("gecko"), QStringLiteral("Gecko"), QStringLiteral("blink"),
    QStringLiteral("webkit2"), QStringLiteral("chrome"), QStringLiteral("web kit"),
    QStringLiteral("WebKit (WebCore)"), QStringLiteral("unknown"),
    // Fullwidth letters stay distinct; KELVIN SIGN lowercases to "k".
    QStringLiteral("ｗｅｂｋｉｔ"), QStringLiteral("WEBKIT"),
    QStringLiteral("WEBKİT")};

// --- The conversions B0 replaced, verbatim -----------------------------------

// main.cpp (session scope, onboarding), BrowserWindow (onboarding restart,
// currentEngineScope), settings status, OnboardingManager::selectedEngine.
QString legacyRunningEngine(EngineType type) {
  return (type == EngineType::WebKit) ? QStringLiteral("webkit") : QStringLiteral("chromium");
}

// SessionStore::normalizeEngine.
QString legacySessionEngine(const QString &engine) {
  const QString lower = engine.toLower().trimmed();
  if (lower == QLatin1String("webkit")) return QStringLiteral("webkit");
  return QStringLiteral("chromium");
}

// EngineStep::selectEngine.
QString legacyOnboardingEngine(const QString &engine) {
  return (engine.trimmed().toLower() == QLatin1String("webkit"))
      ? QStringLiteral("webkit") : QStringLiteral("chromium");
}

// BrowserProfileService's normalizeEngineScope.
QString legacyProfileScope(const QString &engine, EngineType selected) {
  if (!engine.isEmpty()) {
    const QString lower = engine.toLower().trimmed();
    if (lower == QLatin1String("webkit") || lower == QLatin1String("chromium")) {
      return lower;
    }
  }
  if (selected == EngineType::WebKit) {
    return QStringLiteral("webkit");
  }
  return QStringLiteral("chromium");
}

// engine_ui_profile.h profilePrefix.
QString legacyUiPrefix(EngineType selected) {
  return (selected == EngineType::WebKit) ? QStringLiteral("ui_webkit/") : QStringLiteral("ui_chromium/");
}

// EngineResolver::resolvePure.
EngineResolution legacyResolvePure(const QString &persistedEngine, const QStringList &args) {
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

bool sameResolution(const EngineResolution &a, const EngineResolution &b) {
  return a.engine == b.engine && a.isCliOverride == b.isCliOverride && a.source == b.source &&
         a.engineName == b.engineName;
}

QStringList commandLine(const QStringList &arguments) {
  return QStringList{QStringLiteral("dalinira-browser")} + arguments;
}

// --- Checks -------------------------------------------------------------------

void checkKeys() {
  CHECK(dalinira::engine::engineKey(EngineType::Chromium) == QStringLiteral("chromium"));
  CHECK(dalinira::engine::engineKey(EngineType::WebKit) == QStringLiteral("webkit"));
  for (const EngineType type : kEngines) {
    // The key is the display name in lower case, and reads back to its engine.
    CHECK(dalinira::engine::engineKey(type) == dalinira::engine::engineTypeName(type).toLower());
    CHECK(dalinira::engine::parseEngineKey(dalinira::engine::engineKey(type)) == type);
    CHECK(dalinira::engine::engineTypeFromKey(dalinira::engine::engineKey(type)) == type);
  }

  CHECK(dalinira::engine::parseEngineKey(QStringLiteral(" WebKit\t")) == EngineType::WebKit);
  CHECK(dalinira::engine::parseEngineKey(QStringLiteral("CHROMIUM")) == EngineType::Chromium);
  CHECK(dalinira::engine::parseEngineKey(QStringLiteral("WEBKIT")) == EngineType::WebKit);
  for (const QString &unknown : {QString(), QStringLiteral("  "), QStringLiteral("gecko"),
                                 QStringLiteral("blink"), QStringLiteral("webkit2"), QStringLiteral("web kit")}) {
    CHECK(!dalinira::engine::parseEngineKey(unknown).has_value());
    // Chromium stays the default engine for any key it does not know.
    CHECK(dalinira::engine::engineTypeFromKey(unknown) == EngineType::Chromium);
  }
  std::cout << "PASS engine keys" << std::endl;
}

void checkReplacedConversions() {
  for (const EngineType type : kEngines) {
    CHECK(dalinira::engine::engineKey(type) == legacyRunningEngine(type));
    EngineBackend::setSelectedEngineType(type);
    CHECK(dalinira::ui_profile::profilePrefix() == legacyUiPrefix(type));
    CHECK(dalinira::ui_profile::scopedKey(QStringLiteral("appearance/accent")) ==
          legacyUiPrefix(type) + QStringLiteral("appearance/accent"));
  }
  EngineBackend::setSelectedEngineType(EngineType::Chromium);

  for (const QString &key : kKeys) {
    // SessionStore::normalizeEngine and EngineStep::selectEngine.
    const QString normalized = dalinira::engine::engineKey(dalinira::engine::engineTypeFromKey(key));
    CHECK(normalized == legacySessionEngine(key));
    CHECK(normalized == legacyOnboardingEngine(key));
    // BrowserProfileService: an unknown key means the running engine.
    for (const EngineType selected : kEngines) {
      CHECK(dalinira::engine::engineKey(dalinira::engine::parseEngineKey(key).value_or(selected)) ==
            legacyProfileScope(key, selected));
    }
  }
  std::cout << "PASS replaced conversions match their previous results" << std::endl;
}

void checkResolvePure() {
  QList<QStringList> commandLines{commandLine({}), commandLine({QStringLiteral("--engine")}),
                                  commandLine({QStringLiteral("https://example.org/"), QStringLiteral("--engine")})};
  for (const QString &key : kKeys) {
    commandLines.append(commandLine({QStringLiteral("--engine"), key}));
    commandLines.append(commandLine({QStringLiteral("https://example.org/"), QStringLiteral("--engine"), key}));
    // A later --engine is still honoured after one with an unknown value.
    commandLines.append(commandLine({QStringLiteral("--engine"), key, QStringLiteral("--engine"), QStringLiteral("webkit")}));
    commandLines.append(commandLine({QStringLiteral("--engine"), QStringLiteral("gecko"), QStringLiteral("--engine"), key}));
  }
  int compared = 0;
  for (const QString &persisted : kKeys) {
    for (const QStringList &arguments : std::as_const(commandLines)) {
      CHECK(sameResolution(EngineResolver::resolvePure(persisted, arguments), legacyResolvePure(persisted, arguments)));
      ++compared;
    }
  }
  std::cout << "PASS resolvePure matches its previous result for " << compared << " inputs" << std::endl;
}

// resolve() reads `browser/engine` from DaliNira's own settings file, which
// main() has redirected into a temporary directory.
void checkResolve() {
  const QStringList plain = commandLine({});
#if defined(DALINIRA_BUILD_WEBKIT)
  const EngineType webkitOrFallback = EngineType::WebKit;
#else
  const EngineType webkitOrFallback = EngineType::Chromium;
#endif

  // Nothing stored reads as the stored default, "chromium": a Settings
  // resolution. Only an unknown stored key resolves as Default (below).
  CHECK(EngineResolver::getPersistedEngine() == QStringLiteral("chromium"));
  {
    const auto r = EngineResolver::resolve(plain);
    CHECK(r.engine == EngineType::Chromium && !r.isCliOverride &&
          r.source == EngineResolutionSource::Settings && r.engineName == QStringLiteral("chromium"));
  }

  // A stored key is normalized when written.
  EngineResolver::setPersistedEngine(QStringLiteral(" WebKit "));
  CHECK(EngineResolver::getPersistedEngine() == QStringLiteral("webkit"));
  {
    const auto r = EngineResolver::resolve(plain);
    CHECK(r.engine == webkitOrFallback && !r.isCliOverride && r.source == EngineResolutionSource::Settings &&
          r.engineName == dalinira::engine::engineKey(webkitOrFallback));
  }

  // The command line wins over the stored engine.
  {
    const auto r = EngineResolver::resolve(commandLine({QStringLiteral("--engine"), QStringLiteral("CHROMIUM")}));
    CHECK(r.engine == EngineType::Chromium && r.isCliOverride &&
          r.source == EngineResolutionSource::CliOverride && r.engineName == QStringLiteral("chromium"));
  }
  // An unknown --engine value starts Chromium (resolvePure ignores it instead).
  {
    const auto r = EngineResolver::resolve(commandLine({QStringLiteral("--engine"), QStringLiteral("gecko")}));
    CHECK(r.engine == EngineType::Chromium && r.isCliOverride &&
          r.source == EngineResolutionSource::CliOverride && r.engineName == QStringLiteral("chromium"));
  }

  EngineResolver::setPersistedEngine(QStringLiteral("chromium"));
  {
    const auto r = EngineResolver::resolve(commandLine({QStringLiteral("--engine"), QStringLiteral(" webkit ")}));
    CHECK(r.engine == webkitOrFallback && r.isCliOverride &&
          r.source == EngineResolutionSource::CliOverride && r.engineName == dalinira::engine::engineKey(webkitOrFallback));
  }
  {
    const auto r = EngineResolver::resolve(plain);
    CHECK(r.engine == EngineType::Chromium && r.source == EngineResolutionSource::Settings);
  }

  // An unknown stored key counts as nothing stored.
  EngineResolver::setPersistedEngine(QStringLiteral("gecko"));
  CHECK(EngineResolver::getPersistedEngine() == QStringLiteral("gecko"));
  {
    const auto r = EngineResolver::resolve(plain);
    CHECK(r.engine == EngineType::Chromium && !r.isCliOverride &&
          r.source == EngineResolutionSource::Default && r.engineName == QStringLiteral("chromium"));
  }
  std::cout << "PASS resolve precedence, normalization and fallback" << std::endl;
}

}  // namespace

int main(int argc, char *argv[]) {
  // Settings are written only below a temporary directory: point the user
  // configuration there before Qt resolves any settings path.
  QTemporaryDir settingsRoot;
  CHECK(settingsRoot.isValid());
  qputenv("XDG_CONFIG_HOME", settingsRoot.path().toLocal8Bit());
  QCoreApplication app(argc, argv);
  QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settingsRoot.path());
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsRoot.path());
  {
    // The settings file EngineResolver uses; stop before writing anywhere else.
    const QSettings resolverSettings(QString::fromLatin1(dalinira::application_identity::kOrganizationName),
                                     QString::fromLatin1(dalinira::application_identity::kApplicationName));
    CHECK(resolverSettings.fileName().startsWith(settingsRoot.path() + QLatin1Char('/')));
  }

  std::cout << "=== ENGINE IDENTITY ===" << std::endl;
  checkKeys();
  checkReplacedConversions();
  checkResolvePure();
  checkResolve();
  std::cout << "=== ALL ENGINE IDENTITY TESTS PASSED ===" << std::endl;
  return 0;
}
