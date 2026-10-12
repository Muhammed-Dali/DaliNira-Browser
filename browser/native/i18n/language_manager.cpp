#include "language_manager.h"
#include "core/application_identity.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLoggingCategory>
#include <QSet>
#include <QThread>

#include <algorithm>

namespace dalinira::i18n {

namespace {
Q_LOGGING_CATEGORY(lcI18n, "dalinira.i18n")
}

LanguageManager &LanguageManager::instance() {
  static LanguageManager manager;
  return manager;
}

LanguageManager::LanguageManager(QObject *parent) : QObject(parent) {
  populateDefaultFallbackLanguages();
}

void LanguageManager::populateDefaultFallbackLanguages() {
  languages_.clear();
  defaultLanguageCode_ = QStringLiteral("en");

  languages_.append({QStringLiteral("en"), QStringLiteral("English"),
                     QStringLiteral("English"), QStringLiteral("ltr")});
  languages_.append({QStringLiteral("tr"), QStringLiteral("Turkish"),
                     QStringLiteral("Türkçe"), QStringLiteral("ltr")});
  languages_.append({QStringLiteral("ar"), QStringLiteral("Arabic"),
                     QStringLiteral("العربية"), QStringLiteral("rtl")});
  languages_.append({QStringLiteral("fr"), QStringLiteral("French"),
                     QStringLiteral("Français"), QStringLiteral("ltr")});
}

QString LanguageManager::resolveFilePath(const QString &preferredPath,
                                        const QString &fallbackSubPath) const {
  if (!preferredPath.isEmpty()) {
    return preferredPath;
  }

  // 1. Try Qt resource path
  const QString qrcPath = QStringLiteral(":/i18n/") + fallbackSubPath;
  if (QFile::exists(qrcPath)) {
    return qrcPath;
  }

  // 2. Try application binary directory
  if (qApp) {
    const QString appDirPath =
        dalinira::application_identity::applicationDirectory() + QStringLiteral("/i18n/") + fallbackSubPath;
    if (QFile::exists(appDirPath)) {
      return appDirPath;
    }
  }

  // 3. Try current working directory
  const QString cwdPath = QStringLiteral("i18n/") + fallbackSubPath;
  if (QFile::exists(cwdPath)) {
    return cwdPath;
  }

  return QStringLiteral(":/i18n/") + fallbackSubPath;
}

void LanguageManager::initialize(const QString &languagesJsonPath,
                                const QString &translationsDir) {
  QWriteLocker locker(&lock_);
  configPath_ = languagesJsonPath;
  translationsDir_ = translationsDir;
  allCatalogsLoaded_ = false;
  allCatalogs_.clear();
  allReverse_.clear();

  locker.unlock();
  loadLanguagesConfig(configPath_);

  determineAndApplyActiveLanguage(/*notify=*/false);

  QWriteLocker locker2(&lock_);
  initialized_ = true;
}

bool LanguageManager::loadLanguagesConfig(const QString &jsonPath) {
  const QString path = resolveFilePath(jsonPath, QStringLiteral("languages.json"));

  QFile file(path);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    qCWarning(lcI18n) << "Could not open languages config at" << path
                      << "- using fallback definitions.";
    QWriteLocker locker(&lock_);
    if (languages_.isEmpty()) {
      populateDefaultFallbackLanguages();
    }
    return false;
  }

  QJsonParseError parseError;
  const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
  file.close();

  if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
    qCWarning(lcI18n) << "Failed to parse languages JSON:" << parseError.errorString()
                      << "at" << path;
    QWriteLocker locker(&lock_);
    if (languages_.isEmpty()) {
      populateDefaultFallbackLanguages();
    }
    return false;
  }

  const QJsonObject root = doc.object();
  const QString defaultCode = root.value(QStringLiteral("default")).toString().trimmed();
  const QJsonArray langsArray = root.value(QStringLiteral("languages")).toArray();

  QList<LanguageInfo> parsedList;
  QSet<QString> seenCodes;

  for (const QJsonValue &val : langsArray) {
    if (!val.isObject()) continue;
    const QJsonObject obj = val.toObject();
    const QString code = obj.value(QStringLiteral("code")).toString().trimmed().toLower();
    const QString name = obj.value(QStringLiteral("name")).toString().trimmed();
    const QString nativeName = obj.value(QStringLiteral("nativeName")).toString().trimmed();
    QString direction = obj.value(QStringLiteral("direction")).toString().trimmed().toLower();

    if (code.isEmpty() || seenCodes.contains(code)) continue;
    if (direction != QLatin1String("rtl") && direction != QLatin1String("ltr")) {
      direction = QStringLiteral("ltr");
    }

    LanguageInfo info;
    info.code = code;
    info.name = name.isEmpty() ? code : name;
    info.nativeName = nativeName.isEmpty() ? info.name : nativeName;
    info.direction = direction;

    parsedList.append(info);
    seenCodes.insert(code);
  }

  if (parsedList.isEmpty()) {
    qCWarning(lcI18n) << "No valid languages found in" << path;
    return false;
  }

  QWriteLocker locker(&lock_);
  languages_ = parsedList;
  if (!defaultCode.isEmpty() && seenCodes.contains(defaultCode)) {
    defaultLanguageCode_ = defaultCode;
  } else {
    defaultLanguageCode_ = parsedList.first().code;
  }

  return true;
}

bool LanguageManager::loadCatalog(const QString &code,
                                  QHash<QString, QString> &outCatalog) const {
  outCatalog.clear();
  const QString filename = code + QStringLiteral(".json");
  const QString subPath = QStringLiteral("translations/") + filename;

  QString targetPath;
  if (!translationsDir_.isEmpty()) {
    targetPath = translationsDir_ + QStringLiteral("/") + filename;
  }
  targetPath = resolveFilePath(targetPath, subPath);

  QFile file(targetPath);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
    qCWarning(lcI18n) << "Could not open translation catalog for" << code << "at" << targetPath;
    return false;
  }

  QJsonParseError parseError;
  const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
  file.close();

  if (parseError.error != QJsonParseError::NoError) {
    qCWarning(lcI18n) << "Failed to parse translation catalog for" << code
                      << ":" << parseError.errorString();
    return false;
  }

  if (doc.isObject()) {
    const QJsonObject obj = doc.object();
    for (auto it = obj.begin(); it != obj.end(); ++it) {
      if (it.value().isString()) {
        outCatalog.insert(it.key(), it.value().toString());
      }
    }
    return true;
  }

  // Handle array of translated entries (Helium format parity)
  if (doc.isArray()) {
    const QJsonArray arr = doc.array();
    for (const QJsonValue &val : arr) {
      if (!val.isObject()) continue;
      const QJsonObject entry = val.toObject();
      const QString name = entry.value(QStringLiteral("name")).toString();
      const QString message = entry.value(QStringLiteral("message")).toString();
      if (!name.isEmpty() && !message.isEmpty()) {
        outCatalog.insert(name, message);
      }
    }
    return true;
  }

  return false;
}

QList<LanguageInfo> LanguageManager::supportedLanguages() const {
  QReadLocker locker(&lock_);
  return languages_;
}

QString LanguageManager::defaultLanguageCode() const {
  QReadLocker locker(&lock_);
  return defaultLanguageCode_;
}

bool LanguageManager::isLanguageSupported(const QString &code) const {
  QReadLocker locker(&lock_);
  for (const auto &info : languages_) {
    if (info.code.compare(code, Qt::CaseInsensitive) == 0) return true;
  }
  return false;
}

LanguageInfo LanguageManager::languageInfo(const QString &code) const {
  QReadLocker locker(&lock_);
  for (const auto &info : languages_) {
    if (info.code.compare(code, Qt::CaseInsensitive) == 0) return info;
  }
  return {};
}

QString LanguageManager::systemLocaleLanguageCode() const {
  // Deterministic override for tests and controlled GUI validation runs. It
  // replaces the *system* language only; an explicit user preference still
  // wins, so live switching keeps working.
  const QString forced = qEnvironmentVariable("DALINIRA_UI_LANGUAGE").trimmed().toLower();
  if (!forced.isEmpty() && isLanguageSupported(forced)) {
    return forced;
  }

  const QLocale sys;
  const QString name = sys.name().toLower();
  const auto lang = sys.language();

  if (lang == QLocale::Turkish || name.startsWith(QLatin1String("tr"))) {
    return QStringLiteral("tr");
  }
  if (lang == QLocale::Arabic || name.startsWith(QLatin1String("ar"))) {
    return QStringLiteral("ar");
  }
  if (lang == QLocale::English || name.startsWith(QLatin1String("en"))) {
    return QStringLiteral("en");
  }
  if (lang == QLocale::French || name.startsWith(QLatin1String("fr"))) {
    return QStringLiteral("fr");
  }

  QReadLocker locker(&lock_);
  return defaultLanguageCode_.isEmpty() ? QStringLiteral("en") : defaultLanguageCode_;
}

QString LanguageManager::languagePreference() const {
  QSettings settings;
  const QString pref =
      settings.value(QStringLiteral("i18n/language"), QStringLiteral("system")).toString().trimmed().toLower();
  if (pref.isEmpty()) {
    return QStringLiteral("system");
  }
  return pref;
}

void LanguageManager::setLanguagePreference(const QString &preference) {
  const QString cleanPref = preference.trimmed().toLower();
  {
    QSettings settings;
    settings.setValue(QStringLiteral("i18n/language"), cleanPref);
    settings.sync();
  }
  determineAndApplyActiveLanguage(/*notify=*/true);
}

void LanguageManager::determineAndApplyActiveLanguage(bool notify) {
  QString chosenCode;
  const QString pref = languagePreference();

  if (pref == QLatin1String("system") || pref.isEmpty()) {
    chosenCode = systemLocaleLanguageCode();
  } else if (isLanguageSupported(pref)) {
    chosenCode = pref;
  } else {
    chosenCode = defaultLanguageCode();
  }

  if (chosenCode.isEmpty()) {
    chosenCode = QStringLiteral("en");
  }

  QHash<QString, QString> newCatalog;
  loadCatalog(chosenCode, newCatalog);

  QHash<QString, QString> enCatalog;
  if (chosenCode == QStringLiteral("en")) {
    enCatalog = newCatalog;
  } else {
    loadCatalog(QStringLiteral("en"), enCatalog);
  }

  QStringList keys = newCatalog.keys();
  std::sort(keys.begin(), keys.end());
  QHash<QString, QString> reverse;
  reverse.reserve(keys.size());
  for (const QString &key : std::as_const(keys)) {
    const QString &value = newCatalog[key];
    if (!value.isEmpty() && !reverse.contains(value)) reverse.insert(value, key);
  }

  {
    QWriteLocker locker(&lock_);
    activeLanguageCode_ = chosenCode;
    currentCatalog_ = std::move(newCatalog);
    fallbackCatalog_ = std::move(enCatalog);
    currentReverse_ = std::move(reverse);
    initialized_ = true;
  }

  applyLayoutDirection();

  if (notify) {
    emit languageChanged(chosenCode);
  }
}

QString LanguageManager::activeLanguageCode() const {
  ensureInitialized();
  QReadLocker locker(&lock_);
  return activeLanguageCode_;
}

LanguageInfo LanguageManager::activeLanguage() const {
  ensureInitialized();
  QReadLocker locker(&lock_);
  for (const auto &info : languages_) {
    if (info.code == activeLanguageCode_) return info;
  }
  return {activeLanguageCode_, activeLanguageCode_, activeLanguageCode_, QStringLiteral("ltr")};
}

bool LanguageManager::isRtl() const {
  // DaliNira deliberately keeps its browser chrome and internal-page
  // structure stable in every locale. Arabic shaping/bidi is handled by Qt
  // in individual text runs; it must not mirror the application layout.
  return false;
}

Qt::LayoutDirection LanguageManager::layoutDirection() const {
  return Qt::LeftToRight;
}

void LanguageManager::applyLayoutDirection() {
  // Browser chrome never mirrors, in any language. Only the GUI thread may
  // touch the application object (lazy initialization can happen elsewhere).
  if (qApp && QThread::currentThread() == qApp->thread()) {
    qApp->setLayoutDirection(Qt::LeftToRight);
  }
}

void LanguageManager::reloadTranslations() {
  determineAndApplyActiveLanguage(/*notify=*/true);
}

QString LanguageManager::translate(const QString &key, const QString &fallback) const {
  if (key.isEmpty()) return {};
  ensureInitialized();

  QReadLocker locker(&lock_);
  auto it = currentCatalog_.constFind(key);
  if (it != currentCatalog_.constEnd() && !it.value().isEmpty()) {
    return it.value();
  }

  auto itFb = fallbackCatalog_.constFind(key);
  if (itFb != fallbackCatalog_.constEnd() && !itFb.value().isEmpty()) {
    return itFb.value();
  }

  if (!fallback.isEmpty()) {
    return fallback;
  }

  return key;
}

QString LanguageManager::translate(const QString &key,
                                   const QVariantMap &replacements) const {
  QString text = translate(key);
  for (auto it = replacements.constBegin(); it != replacements.constEnd(); ++it) {
    const QString placeholder1 = QStringLiteral("{%1}").arg(it.key());
    const QString placeholder2 = QStringLiteral("{{%1}}").arg(it.key());
    const QString val = it.value().toString();
    text.replace(placeholder1, val);
    text.replace(placeholder2, val);
  }
  return text;
}

void LanguageManager::ensureInitialized() const {
  {
    QReadLocker locker(&lock_);
    if (initialized_) return;
  }
  // Components created before main() configured the manager (tests, static
  // helpers) still get real catalogs instead of raw keys.
  const_cast<LanguageManager *>(this)->initialize();
}

void LanguageManager::ensureAllCatalogs() const {
  {
    QReadLocker locker(&lock_);
    if (allCatalogsLoaded_) return;
  }
  QHash<QString, QHash<QString, QString>> catalogs;
  QHash<QString, QString> reverse;
  const QList<LanguageInfo> languages = supportedLanguages();
  for (const LanguageInfo &info : languages) {
    QHash<QString, QString> catalog;
    if (!loadCatalog(info.code, catalog)) continue;
    QStringList keys = catalog.keys();
    std::sort(keys.begin(), keys.end());
    for (const QString &key : std::as_const(keys)) {
      const QString &value = catalog[key];
      if (!value.isEmpty() && !reverse.contains(value)) reverse.insert(value, key);
    }
    catalogs.insert(info.code, std::move(catalog));
  }
  QWriteLocker locker(&lock_);
  if (allCatalogsLoaded_) return;
  allCatalogs_ = std::move(catalogs);
  allReverse_ = std::move(reverse);
  allCatalogsLoaded_ = true;
}

QString LanguageManager::translateIn(const QString &code, const QString &key) const {
  ensureInitialized();
  ensureAllCatalogs();
  QReadLocker locker(&lock_);
  const QString lower = code.toLower();
  const QString value = allCatalogs_.value(lower).value(key);
  if (!value.isEmpty()) return value;
  const QString english = allCatalogs_.value(QStringLiteral("en")).value(key);
  return english.isEmpty() ? key : english;
}

QStringList LanguageManager::translationsOf(const QString &key) const {
  ensureInitialized();
  ensureAllCatalogs();
  QReadLocker locker(&lock_);
  QStringList values;
  for (auto it = allCatalogs_.cbegin(); it != allCatalogs_.cend(); ++it) {
    const QString value = it.value().value(key);
    if (!value.isEmpty() && !values.contains(value)) values.append(value);
  }
  return values;
}

QString LanguageManager::keyForText(const QString &text) const {
  if (text.isEmpty()) return {};
  ensureInitialized();
  {
    QReadLocker locker(&lock_);
    const auto current = currentReverse_.constFind(text);
    if (current != currentReverse_.cend()) return current.value();
  }
  ensureAllCatalogs();
  QReadLocker locker(&lock_);
  return allReverse_.value(text);
}

QString LanguageManager::formatSystemLanguageLabel() const {
  const QString sysCode = systemLocaleLanguageCode();
  const LanguageInfo sysInfo = languageInfo(sysCode);
  const QString name = !sysInfo.nativeName.isEmpty() ? sysInfo.nativeName : sysCode;
  return translate(QStringLiteral("settings.language.system_default_format")).arg(name);
}

QHash<QString, QString> LanguageManager::currentCatalog() const {
  QReadLocker locker(&lock_);
  return currentCatalog_;
}

QHash<QString, QString> LanguageManager::fallbackCatalog() const {
  QReadLocker locker(&lock_);
  return fallbackCatalog_;
}

}  // namespace dalinira::i18n
