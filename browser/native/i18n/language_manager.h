#pragma once

#include <QHash>
#include <QList>
#include <QLocale>
#include <QObject>
#include <QReadWriteLock>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVariantMap>

namespace dalinira::i18n {

struct LanguageInfo {
  QString code;
  QString name;
  QString nativeName;
  QString direction;  // "ltr" or "rtl"

  bool isRtl() const {
    return direction.compare(QLatin1String("rtl"), Qt::CaseInsensitive) == 0;
  }
};

class LanguageManager : public QObject {
  Q_OBJECT

 public:
  static LanguageManager &instance();

  // Initialization & Configuration
  void initialize(const QString &languagesJsonPath = QString(),
                  const QString &translationsDir = QString());
  bool loadLanguagesConfig(const QString &jsonPath = QString());
  bool loadCatalog(const QString &code, QHash<QString, QString> &outCatalog) const;

  // Language Information
  QList<LanguageInfo> supportedLanguages() const;
  QString defaultLanguageCode() const;
  bool isLanguageSupported(const QString &code) const;
  LanguageInfo languageInfo(const QString &code) const;

  // Preference & Resolution
  QString systemLocaleLanguageCode() const;
  QString languagePreference() const;
  void setLanguagePreference(const QString &preference);

  QString activeLanguageCode() const;
  LanguageInfo activeLanguage() const;
  bool isRtl() const;
  Qt::LayoutDirection layoutDirection() const;

  // Translation Lookups
  QString translate(const QString &key, const QString &fallback = QString()) const;
  QString translate(const QString &key, const QVariantMap &replacements) const;
  // Value of `key` in a specific supported language (tests, cross-language
  // matching). Falls back to English, then to the key itself.
  QString translateIn(const QString &code, const QString &key) const;
  // Every distinct value `key` has across the supported catalogs.
  QStringList translationsOf(const QString &key) const;
  // Reverse lookup used by live widget retranslation: the catalog key whose
  // value is exactly `text`, preferring the active language. Empty when the
  // text is not a catalog value (user data, composed strings).
  QString keyForText(const QString &text) const;

  // Helper formatting
  QString formatSystemLanguageLabel() const;

  // Apply layout direction to qApp
  void applyLayoutDirection();
  void reloadTranslations();

  // Diagnostics & testing
  QHash<QString, QString> currentCatalog() const;
  QHash<QString, QString> fallbackCatalog() const;

 signals:
  void languageChanged(const QString &languageCode);

 private:
  explicit LanguageManager(QObject *parent = nullptr);
  ~LanguageManager() override = default;

  LanguageManager(const LanguageManager &) = delete;
  LanguageManager &operator=(const LanguageManager &) = delete;

  void determineAndApplyActiveLanguage(bool notify = true);
  QString resolveFilePath(const QString &preferredPath, const QString &fallbackSubPath) const;
  void populateDefaultFallbackLanguages();
  void ensureInitialized() const;
  void ensureAllCatalogs() const;

  mutable QReadWriteLock lock_;
  QString configPath_;
  QString translationsDir_;
  QString defaultLanguageCode_{QStringLiteral("en")};
  QList<LanguageInfo> languages_;
  QString activeLanguageCode_{QStringLiteral("en")};

  QHash<QString, QString> currentCatalog_;
  QHash<QString, QString> fallbackCatalog_;
  QHash<QString, QString> currentReverse_;
  // Lazily loaded, immutable after load: every supported catalog and the
  // value->key index across all of them.
  mutable QHash<QString, QHash<QString, QString>> allCatalogs_;
  mutable QHash<QString, QString> allReverse_;
  mutable bool allCatalogsLoaded_{false};
  bool initialized_{false};
};

}  // namespace dalinira::i18n

namespace dalinira {
using LanguageInfo = dalinira::i18n::LanguageInfo;
using LanguageManager = dalinira::i18n::LanguageManager;
}  // namespace dalinira
