// One engine identity for every engine-scoped name: engineKey() is the key
// persisted as `browser/engine`, given to --engine and used for the
// per-engine scopes (sessions, UI settings, New Tab data); engineTypeFromKey()
// and parseEngineKey() read such a key back. Three engines are known:
// Chromium, WebKit and Gecko, whose backend is not built yet.
//
// For every key that does not name Gecko, each conversion is compared with
// the expression it replaced (copied verbatim below as legacy*), over keys
// that differ in case, whitespace and Unicode, empty and unknown keys: the
// Chromium and WebKit results are unchanged. Keys naming Gecko are checked
// against their own expected results. SessionStore, BrowserProfileService and
// EngineStep are covered with the real classes by browser-engine-workspace
// and browser-onboarding.

#include <QCoreApplication>
#include <QSet>
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

const QList<EngineType> kEngines{EngineType::Chromium, EngineType::WebKit, EngineType::Gecko};

// Keys as settings, command lines and callers may spell them.
const QStringList kKeys{
    QStringLiteral("chromium"), QStringLiteral("webkit"), QStringLiteral("Chromium"),
    QStringLiteral("WEBKIT"), QStringLiteral("WebKit"), QStringLiteral(" webkit "),
    QStringLiteral("\twebkit\n"), QStringLiteral("  CHROMIUM"), QString(), QStringLiteral(""),
    QStringLiteral("   "), QStringLiteral("gecko"), QStringLiteral("Gecko"), QStringLiteral(" GECKO\t"),
    QStringLiteral("geckoview"), QStringLiteral("firefox"), QStringLiteral("blink"),
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

bool namesGecko(const QString &key) {
  return dalinira::engine::parseEngineKey(key) == EngineType::Gecko;
}

// Whether the stored key or any --engine value names Gecko.
bool involvesGecko(const QString &persisted, const QStringList &args) {
  if (namesGecko(persisted)) return true;
  for (int i = 1; i + 1 < args.size(); ++i) {
    if (args[i] == QLatin1String("--engine") && namesGecko(args[i + 1])) return true;
  }
  return false;
}

EngineResolution expected(EngineType engine, EngineResolutionSource source) {
  return {engine, source == EngineResolutionSource::CliOverride, source, dalinira::engine::engineKey(engine)};
}

void checkKeys() {
  CHECK(dalinira::engine::engineKey(EngineType::Chromium) == QStringLiteral("chromium"));
  CHECK(dalinira::engine::engineKey(EngineType::WebKit) == QStringLiteral("webkit"));
  CHECK(dalinira::engine::engineKey(EngineType::Gecko) == QStringLiteral("gecko"));
  CHECK(dalinira::engine::engineTypeName(EngineType::Gecko) == QStringLiteral("Gecko"));
  CHECK(dalinira::engine::engineLayoutEngineName(EngineType::Gecko) == QStringLiteral("Gecko"));
  CHECK(dalinira::engine::engineJsEngineName(EngineType::Gecko) == QStringLiteral("SpiderMonkey"));
  QSet<QString> keys;
  for (const EngineType type : kEngines) {
    // The key is the display name in lower case, and reads back to its engine.
    CHECK(dalinira::engine::engineKey(type) == dalinira::engine::engineTypeName(type).toLower());
    CHECK(dalinira::engine::parseEngineKey(dalinira::engine::engineKey(type)) == type);
    CHECK(dalinira::engine::engineTypeFromKey(dalinira::engine::engineKey(type)) == type);
    keys.insert(dalinira::engine::engineKey(type));
  }
  CHECK(keys.size() == kEngines.size());

  CHECK(dalinira::engine::parseEngineKey(QStringLiteral(" WebKit\t")) == EngineType::WebKit);
  CHECK(dalinira::engine::parseEngineKey(QStringLiteral("CHROMIUM")) == EngineType::Chromium);
  CHECK(dalinira::engine::parseEngineKey(QStringLiteral(" GECKO\n")) == EngineType::Gecko);
  CHECK(dalinira::engine::parseEngineKey(QStringLiteral("WEB\u212AIT")) == EngineType::WebKit);
  for (const QString &unknown : {QString(), QStringLiteral("  "), QStringLiteral("blink"), QStringLiteral("webkit2"),
                                 QStringLiteral("web kit"), QStringLiteral("firefox"), QStringLiteral("geckoview")}) {
    CHECK(!dalinira::engine::parseEngineKey(unknown).has_value());
    // Chromium stays the default engine for any key it does not know.
    CHECK(dalinira::engine::engineTypeFromKey(unknown) == EngineType::Chromium);
  }
  std::cout << "PASS engine keys" << std::endl;
}

void checkConversions() {
  for (const EngineType type : kEngines) {
    const bool gecko = type == EngineType::Gecko;
    const QString prefix = gecko ? QStringLiteral("ui_gecko/") : legacyUiPrefix(type);
    CHECK(dalinira::engine::engineKey(type) == (gecko ? QStringLiteral("gecko") : legacyRunningEngine(type)));
    EngineBackend::setSelectedEngineType(type);
    CHECK(dalinira::ui_profile::profilePrefix() == prefix);
    CHECK(dalinira::ui_profile::scopedKey(QStringLiteral("appearance/accent")) == prefix + QStringLiteral("appearance/accent"));
    // A key already scoped to an engine keeps that scope.
    for (const EngineType other : kEngines) {
      const QString scoped = dalinira::ui_profile::profilePrefix(other) + QStringLiteral("tabStyle");
      CHECK(dalinira::ui_profile::scopedKey(scoped) == scoped);
    }
  }
  EngineBackend::setSelectedEngineType(EngineType::Chromium);

  int unchanged = 0;
  for (const QString &key : kKeys) {
    // SessionStore::normalizeEngine: Gecko has its own workspace.
    const QString session = dalinira::engine::engineKey(dalinira::engine::engineTypeFromKey(key));
    if (namesGecko(key)) {
      CHECK(session == QStringLiteral("gecko"));
    } else {
      CHECK(session == legacySessionEngine(key));
      ++unchanged;
    }
    // BrowserProfileService: an unknown key means the running engine.
    for (const EngineType selected : kEngines) {
      const auto parsed = dalinira::engine::parseEngineKey(key);
      const QString scope = dalinira::engine::engineKey(parsed.value_or(selected));
      if (namesGecko(key)) {
        CHECK(scope == QStringLiteral("gecko"));
      } else if (selected != EngineType::Gecko) {
        CHECK(scope == legacyProfileScope(key, selected));
        ++unchanged;
      } else {
        CHECK(scope == (parsed ? dalinira::engine::engineKey(*parsed) : QStringLiteral("gecko")));
      }
    }
  }
  std::cout << "PASS conversions; " << unchanged << " Chromium/WebKit results match their previous ones" << std::endl;
}

// The appearance settings of each engine stay apart; settings from before
// engine profiles belong to Chromium only.
void checkUiProfiles() {
  QSettings settings;
  settings.setValue(QStringLiteral("legacyVisualIsolationProbe"), QStringLiteral("chromium-legacy"));
  EngineBackend::setSelectedEngineType(EngineType::Chromium);
  CHECK(dalinira::ui_profile::getVisualValue(QStringLiteral("legacyVisualIsolationProbe")).toString() ==
        QStringLiteral("chromium-legacy"));
  EngineBackend::setSelectedEngineType(EngineType::Gecko);
  CHECK(dalinira::ui_profile::getVisualValue(QStringLiteral("legacyVisualIsolationProbe"),
                                             QStringLiteral("gecko-default")).toString() == QStringLiteral("gecko-default"));
  dalinira::ui_profile::setVisualValue(QStringLiteral("tabStyle"), QStringLiteral("gecko-style"));
  CHECK(settings.value(QStringLiteral("ui_gecko/tabStyle")).toString() == QStringLiteral("gecko-style"));
  for (const EngineType other : {EngineType::Chromium, EngineType::WebKit}) {
    EngineBackend::setSelectedEngineType(other);
    CHECK(dalinira::ui_profile::getVisualValue(QStringLiteral("tabStyle"), QStringLiteral("none")).toString() ==
          QStringLiteral("none"));
    dalinira::ui_profile::setVisualValue(QStringLiteral("tabStyle"), dalinira::engine::engineKey(other));
  }
  EngineBackend::setSelectedEngineType(EngineType::Gecko);
  CHECK(dalinira::ui_profile::getVisualValue(QStringLiteral("tabStyle")).toString() == QStringLiteral("gecko-style"));
  EngineBackend::setSelectedEngineType(EngineType::Chromium);
  settings.remove(QStringLiteral("legacyVisualIsolationProbe"));
  std::cout << "PASS UI profiles of the three engines stay apart" << std::endl;
}

void checkResolvePure() {
  QList<QStringList> commandLines{commandLine({}), commandLine({QStringLiteral("--engine")}),
                                  commandLine({QStringLiteral("https://example.org/"), QStringLiteral("--engine")})};
  for (const QString &key : kKeys) {
    commandLines.append(commandLine({QStringLiteral("--engine"), key}));
    commandLines.append(commandLine({QStringLiteral("https://example.org/"), QStringLiteral("--engine"), key}));
    // A later --engine is still honoured after one with an unknown value.
    commandLines.append(commandLine({QStringLiteral("--engine"), key, QStringLiteral("--engine"), QStringLiteral("webkit")}));
    commandLines.append(commandLine({QStringLiteral("--engine"), QStringLiteral("blink"), QStringLiteral("--engine"), key}));
  }
  int unchanged = 0, gecko = 0;
  for (const QString &persisted : kKeys) {
    for (const QStringList &arguments : std::as_const(commandLines)) {
      const EngineResolution actual = EngineResolver::resolvePure(persisted, arguments);
      if (involvesGecko(persisted, arguments)) {
        ++gecko;
        continue;
      }
      CHECK(sameResolution(actual, legacyResolvePure(persisted, arguments)));
      ++unchanged;
    }
  }
  // resolvePure ignores build options: a Gecko key resolves to Gecko.
  const QStringList plain = commandLine({});
  CHECK(sameResolution(EngineResolver::resolvePure(QStringLiteral("gecko"), plain),
                       expected(EngineType::Gecko, EngineResolutionSource::Settings)));
  CHECK(sameResolution(EngineResolver::resolvePure(QStringLiteral("chromium"),
                                                   commandLine({QStringLiteral("--engine"), QStringLiteral(" GECKO ")})),
                       expected(EngineType::Gecko, EngineResolutionSource::CliOverride)));
  CHECK(sameResolution(EngineResolver::resolvePure(QStringLiteral("gecko"),
                                                   commandLine({QStringLiteral("--engine"), QStringLiteral("webkit")})),
                       expected(EngineType::WebKit, EngineResolutionSource::CliOverride)));
  CHECK(sameResolution(EngineResolver::resolvePure(QStringLiteral("webkit"),
                                                   commandLine({QStringLiteral("--engine"), QStringLiteral("blink"),
                                                                QStringLiteral("--engine"), QStringLiteral("gecko")})),
                       expected(EngineType::Gecko, EngineResolutionSource::CliOverride)));
  CHECK(sameResolution(EngineResolver::resolvePure(QStringLiteral("gecko"),
                                                   commandLine({QStringLiteral("--engine"), QStringLiteral("blink")})),
                       expected(EngineType::Gecko, EngineResolutionSource::Settings)));
  std::cout << "PASS resolvePure: " << unchanged << " inputs match their previous result, "
            << gecko << " involve Gecko" << std::endl;
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
#if defined(DALINIRA_BUILD_GECKO)
  const EngineType geckoOrFallback = EngineType::Gecko;
#else
  const EngineType geckoOrFallback = EngineType::Chromium;
#endif
  CHECK(EngineResolver::isBuilt(EngineType::Chromium));
  CHECK(EngineResolver::isBuilt(EngineType::WebKit) == (webkitOrFallback == EngineType::WebKit));
  CHECK(EngineResolver::isBuilt(EngineType::Gecko) == (geckoOrFallback == EngineType::Gecko));

  // Nothing stored reads as the stored default, "chromium": a Settings
  // resolution. Only an unknown stored key resolves as Default (below).
  CHECK(EngineResolver::getPersistedEngine() == QStringLiteral("chromium"));
  CHECK(sameResolution(EngineResolver::resolve(plain), expected(EngineType::Chromium, EngineResolutionSource::Settings)));

  // A stored key is normalized when written.
  EngineResolver::setPersistedEngine(QStringLiteral(" WebKit "));
  CHECK(EngineResolver::getPersistedEngine() == QStringLiteral("webkit"));
  CHECK(sameResolution(EngineResolver::resolve(plain), expected(webkitOrFallback, EngineResolutionSource::Settings)));

  // The command line wins over the stored engine.
  CHECK(sameResolution(EngineResolver::resolve(commandLine({QStringLiteral("--engine"), QStringLiteral("CHROMIUM")})),
                       expected(EngineType::Chromium, EngineResolutionSource::CliOverride)));
  // An unknown --engine value starts Chromium (resolvePure ignores it instead).
  CHECK(sameResolution(EngineResolver::resolve(commandLine({QStringLiteral("--engine"), QStringLiteral("blink")})),
                       expected(EngineType::Chromium, EngineResolutionSource::CliOverride)));
  // --engine gecko starts Gecko where it is built, Chromium elsewhere.
  CHECK(sameResolution(EngineResolver::resolve(commandLine({QStringLiteral("--engine"), QStringLiteral(" Gecko ")})),
                       expected(geckoOrFallback, EngineResolutionSource::CliOverride)));

  EngineResolver::setPersistedEngine(QStringLiteral("chromium"));
  CHECK(sameResolution(EngineResolver::resolve(commandLine({QStringLiteral("--engine"), QStringLiteral(" webkit ")})),
                       expected(webkitOrFallback, EngineResolutionSource::CliOverride)));
  CHECK(sameResolution(EngineResolver::resolve(plain), expected(EngineType::Chromium, EngineResolutionSource::Settings)));

  // A stored Gecko is kept, but starts Chromium: Gecko is experimental, only
  // --engine gecko starts it (and Settings never stores it).
  EngineResolver::setPersistedEngine(QStringLiteral(" GECKO "));
  CHECK(EngineResolver::getPersistedEngine() == QStringLiteral("gecko"));
  CHECK(sameResolution(EngineResolver::resolve(plain), expected(EngineType::Chromium, EngineResolutionSource::Settings)));
  CHECK(sameResolution(EngineResolver::resolve(commandLine({QStringLiteral("--engine"), QStringLiteral("webkit")})),
                       expected(webkitOrFallback, EngineResolutionSource::CliOverride)));

  // An unknown stored key counts as nothing stored.
  EngineResolver::setPersistedEngine(QStringLiteral("blink"));
  CHECK(EngineResolver::getPersistedEngine() == QStringLiteral("blink"));
  CHECK(sameResolution(EngineResolver::resolve(plain), expected(EngineType::Chromium, EngineResolutionSource::Default)));
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
  QCoreApplication::setOrganizationName(QStringLiteral("DaliNiraTests"));
  QCoreApplication::setApplicationName(QStringLiteral("EngineIdentity"));
  QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settingsRoot.path());
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsRoot.path());
  {
    // The settings files the test writes; stop before writing anywhere else.
    const QString prefix = settingsRoot.path() + QLatin1Char('/');
    CHECK(QSettings().fileName().startsWith(prefix));
    CHECK(QSettings(QString::fromLatin1(dalinira::application_identity::kOrganizationName),
                    QString::fromLatin1(dalinira::application_identity::kApplicationName)).fileName().startsWith(prefix));
  }

  std::cout << "=== ENGINE IDENTITY ===" << std::endl;
  checkKeys();
  checkConversions();
  checkUiProfiles();
  checkResolvePure();
  checkResolve();
  std::cout << "=== ALL ENGINE IDENTITY TESTS PASSED ===" << std::endl;
  return 0;
}
