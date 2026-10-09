#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLineEdit>
#include <QLabel>
#include <QFontMetrics>
#include <QLocale>
#include <QSettings>
#include <QTemporaryDir>
#include <cassert>
#include <iostream>

#include "i18n/i18n.h"
#include "i18n/language_manager.h"
#include "i18n/widget_i18n.h"

#include <QComboBox>
#include <QPushButton>

using namespace dalinira::i18n;

int main(int argc, char **argv) {
  // Use a dedicated organization and application name to isolate test QSettings
  QApplication app(argc, argv);
  app.setOrganizationName(QStringLiteral("DaliNiraTestOrg"));
  app.setApplicationName(QStringLiteral("LanguageManagerTest"));
  QTemporaryDir settingsRoot;
  assert(settingsRoot.isValid());
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsRoot.path());

  // Reset any previous settings
  {
    QSettings settings;
    settings.clear();
    settings.sync();
  }

  std::cout << "[TEST] Starting LanguageManager test suite..." << std::endl;

  auto &lm = LanguageManager::instance();
  lm.initialize();

  // -------------------------------------------------------------
  // Test Case 1: Configuration & metadata parsing
  // -------------------------------------------------------------
  std::cout << "[TEST 1] Verifying supported languages configuration..." << std::endl;
  const auto supported = lm.supportedLanguages();
  assert(supported.size() >= 4);
  assert(lm.isLanguageSupported(QStringLiteral("en")));
  assert(lm.isLanguageSupported(QStringLiteral("tr")));
  assert(lm.isLanguageSupported(QStringLiteral("ar")));
  assert(lm.isLanguageSupported(QStringLiteral("fr")));

  const auto enInfo = lm.languageInfo(QStringLiteral("en"));
  assert(enInfo.code == QLatin1String("en"));
  assert(enInfo.direction == QLatin1String("ltr"));
  assert(!enInfo.isRtl());

  const auto trInfo = lm.languageInfo(QStringLiteral("tr"));
  assert(trInfo.code == QLatin1String("tr"));
  assert(trInfo.nativeName == QStringLiteral("Türkçe"));
  assert(!trInfo.isRtl());

  const auto arInfo = lm.languageInfo(QStringLiteral("ar"));
  assert(arInfo.code == QLatin1String("ar"));
  assert(arInfo.nativeName == QStringLiteral("العربية"));
  assert(arInfo.direction == QLatin1String("rtl")); // text metadata only
  assert(arInfo.isRtl());
  assert(lm.languageInfo(QStringLiteral("fr")).nativeName == QStringLiteral("Français"));

  // -------------------------------------------------------------
  // Test Case 2: System locale detection mapping
  // -------------------------------------------------------------
  std::cout << "[TEST 2] Verifying system locale mappings..." << std::endl;
  // Test Turkish locale mapping
  QLocale::setDefault(QLocale(QLocale::Turkish, QLocale::Turkey));
  assert(lm.systemLocaleLanguageCode() == QLatin1String("tr"));

  // Test Arabic locale mapping
  QLocale::setDefault(QLocale(QLocale::Arabic, QLocale::SaudiArabia));
  assert(lm.systemLocaleLanguageCode() == QLatin1String("ar"));

  // Test English locale mapping
  QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
  assert(lm.systemLocaleLanguageCode() == QLatin1String("en"));

  // French is a supported application language.
  QLocale::setDefault(QLocale(QLocale::German, QLocale::Germany));
  assert(lm.systemLocaleLanguageCode() == QLatin1String("en"));
  QLocale::setDefault(QLocale(QLocale::French, QLocale::France));
  assert(lm.systemLocaleLanguageCode() == QLatin1String("fr"));
  QLocale::setDefault(QLocale(QLocale::Russian, QLocale::Russia));
  assert(lm.systemLocaleLanguageCode() == QLatin1String("en"));

  // -------------------------------------------------------------
  // Test Case 3: Preference = "system" behavior
  // -------------------------------------------------------------
  std::cout << "[TEST 3] Testing preference = 'system' with various locales..." << std::endl;
  lm.setLanguagePreference(QStringLiteral("system"));
  assert(lm.languagePreference() == QLatin1String("system"));

  // System is Turkish -> active language must be tr
  QLocale::setDefault(QLocale(QLocale::Turkish, QLocale::Turkey));
  lm.reloadTranslations();
  assert(lm.activeLanguageCode() == QLatin1String("tr"));
  assert(!lm.isRtl());

  // System is Arabic -> active language must be ar
  QLocale::setDefault(QLocale(QLocale::Arabic, QLocale::Egypt));
  lm.reloadTranslations();
  assert(lm.activeLanguageCode() == QLatin1String("ar"));
  assert(!lm.isRtl());
  assert(app.layoutDirection() == Qt::LeftToRight);

  // System is English -> active language must be en
  QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedKingdom));
  lm.reloadTranslations();
  assert(lm.activeLanguageCode() == QLatin1String("en"));
  assert(!lm.isRtl());
  assert(app.layoutDirection() == Qt::LeftToRight);

  // System is Unsupported (Japanese) -> active language must fallback to en
  QLocale::setDefault(QLocale(QLocale::Japanese, QLocale::Japan));
  lm.reloadTranslations();
  assert(lm.activeLanguageCode() == QLatin1String("en"));
  assert(!lm.isRtl());
  assert(app.layoutDirection() == Qt::LeftToRight);

  // -------------------------------------------------------------
  // Test Case 4: Explicit preference overrides system locale
  // -------------------------------------------------------------
  std::cout << "[TEST 4] Explicit preference overrides system locale..." << std::endl;
  // System is Arabic, but user explicitly prefers Turkish
  QLocale::setDefault(QLocale(QLocale::Arabic, QLocale::SaudiArabia));
  lm.setLanguagePreference(QStringLiteral("tr"));
  assert(lm.activeLanguageCode() == QLatin1String("tr"));
  assert(!lm.isRtl());
  assert(app.layoutDirection() == Qt::LeftToRight);

  // System is Turkish, but user explicitly prefers Arabic
  QLocale::setDefault(QLocale(QLocale::Turkish, QLocale::Turkey));
  lm.setLanguagePreference(QStringLiteral("ar"));
  assert(lm.activeLanguageCode() == QLatin1String("ar"));
  assert(!lm.isRtl());
  assert(app.layoutDirection() == Qt::LeftToRight);

  // User explicitly prefers English
  lm.setLanguagePreference(QStringLiteral("en"));
  assert(lm.activeLanguageCode() == QLatin1String("en"));
  assert(!lm.isRtl());
  assert(app.layoutDirection() == Qt::LeftToRight);

  // -------------------------------------------------------------
  // Test Case 5: Translations lookup & quality
  // -------------------------------------------------------------
  std::cout << "[TEST 5] Translation lookups in en, tr, and ar..." << std::endl;
  lm.setLanguagePreference(QStringLiteral("en"));
  assert(I18n::text(QStringLiteral("menu.new_tab")) == QStringLiteral("New tab"));
  assert(I18n::text(QStringLiteral("settings.title")) == QStringLiteral("Settings"));
  assert(I18n::text(QStringLiteral("newtab.downloads")) == QStringLiteral("Downloads"));

  lm.setLanguagePreference(QStringLiteral("tr"));
  assert(I18n::text(QStringLiteral("menu.new_tab")) == QStringLiteral("Yeni sekme"));
  assert(I18n::text(QStringLiteral("settings.title")) == QStringLiteral("Ayarlar"));
  assert(I18n::text(QStringLiteral("newtab.downloads")) == QStringLiteral("İndirmeler"));
  assert(I18n::text(QStringLiteral("common.save")) == QStringLiteral("Kaydet"));
  assert(I18n::text(QStringLiteral("common.cancel")) == QStringLiteral("İptal"));

  lm.setLanguagePreference(QStringLiteral("ar"));
  assert(I18n::text(QStringLiteral("menu.new_tab")) == QStringLiteral("علامة تبويب جديدة"));
  assert(I18n::text(QStringLiteral("settings.title")) == QStringLiteral("الإعدادات"));
  assert(I18n::text(QStringLiteral("newtab.downloads")) == QStringLiteral("التنزيلات"));
  assert(I18n::text(QStringLiteral("common.save")) == QStringLiteral("حفظ"));
  assert(I18n::text(QStringLiteral("common.cancel")) == QStringLiteral("إلغاء"));

  lm.setLanguagePreference(QStringLiteral("fr"));
  assert(I18n::text(QStringLiteral("menu.new_tab")) == QStringLiteral("Nouvel onglet"));
  assert(I18n::text(QStringLiteral("settings.title")) == QStringLiteral("Paramètres"));
  assert(I18n::text(QStringLiteral("newtab.downloads")) == QStringLiteral("Téléchargements"));
  assert(I18n::text(QStringLiteral("tab.context.sleep_menu")) == QStringLiteral("Veille / Mémoire"));
  assert(I18n::text(QStringLiteral("tab.hover.state_sleeping")).contains(QStringLiteral("veille")));
  assert(I18n::text(QStringLiteral("translate.translating")) == QStringLiteral("Traduction en cours…"));

  // Every supported locale is complete: no locale silently falls back to a
  // different catalog for browser chrome.
  const auto expectedKeys = lm.currentCatalog().keys();
  for (const QString &code : {QStringLiteral("tr"), QStringLiteral("en"), QStringLiteral("ar"), QStringLiteral("fr")}) {
    lm.setLanguagePreference(code);
    const auto catalog = lm.currentCatalog();
    assert(catalog.size() == expectedKeys.size());
    for (const QString &key : expectedKeys) assert(catalog.contains(key) && !catalog.value(key).isEmpty());
    assert(app.layoutDirection() == Qt::LeftToRight);
  }

  // -------------------------------------------------------------
  // Test Case 6: Fallback resolution chain (Missing in lang -> English -> Key)
  // -------------------------------------------------------------
  std::cout << "[TEST 6] Fallback resolution..." << std::endl;
  // Key missing in all catalogs returns the key itself or provided fallback
  const QString unknownKey = QStringLiteral("synthetic.nonexistent.key");
  assert(I18n::text(unknownKey) == unknownKey);

  // -------------------------------------------------------------
  // Test Case 7: Parameter replacement
  // -------------------------------------------------------------
  std::cout << "[TEST 7] Parameter replacement..." << std::endl;
  lm.setLanguagePreference(QStringLiteral("en"));
  QVariantMap replacements;
  replacements.insert(QStringLiteral("host"), QStringLiteral("example.com"));
  assert(I18n::text(QStringLiteral("toolbar.search_engine")).arg(QStringLiteral("Google"))
         == QStringLiteral("Search engine: Google"));

  // -------------------------------------------------------------
  // Test Case 8: Runtime signal notification
  // -------------------------------------------------------------
  std::cout << "[TEST 8] Runtime signal notification..." << std::endl;
  QString receivedCode;
  int signalCount = 0;
  QObject::connect(&lm, &LanguageManager::languageChanged, &app, [&](const QString &code) {
    receivedCode = code;
    signalCount++;
  });

  lm.setLanguagePreference(QStringLiteral("tr"));
  assert(signalCount == 1);
  assert(receivedCode == QLatin1String("tr"));

  lm.setLanguagePreference(QStringLiteral("ar"));
  assert(signalCount == 2);
  assert(receivedCode == QLatin1String("ar"));

  // -------------------------------------------------------------
  // Test Case 9: QSettings persistence & reload parity
  // -------------------------------------------------------------
  std::cout << "[TEST 9] QSettings persistence..." << std::endl;
  lm.setLanguagePreference(QStringLiteral("ar"));
  {
    QSettings settings;
    assert(settings.value(QStringLiteral("i18n/language")).toString() == QLatin1String("ar"));
  }

  // Simulate application restart: new manager reading from QSettings
  lm.setLanguagePreference(QStringLiteral("tr"));
  {
    QSettings settings;
    assert(settings.value(QStringLiteral("i18n/language")).toString() == QLatin1String("tr"));
  }
  lm.initialize();
  assert(lm.activeLanguageCode() == QLatin1String("tr"));

  // -------------------------------------------------------------
  // Test Case 10: Arabic text with stable LTR browser chrome
  // -------------------------------------------------------------
  std::cout << "[TEST 10] Arabic text with LTR browser chrome..." << std::endl;
  lm.setLanguagePreference(QStringLiteral("ar"));
  assert(app.layoutDirection() == Qt::LeftToRight);

  QLineEdit omnibox;
  omnibox.setLayoutDirection(Qt::LeftToRight);
  omnibox.setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  omnibox.setText(QStringLiteral("https://dalinira.test/browse"));

  assert(omnibox.layoutDirection() == Qt::LeftToRight);

  QLabel arabicText(I18n::text(QStringLiteral("settings.title")));
  arabicText.setLayoutDirection(Qt::LeftToRight);
  assert(arabicText.text() == QStringLiteral("الإعدادات"));
  assert(arabicText.fontMetrics().horizontalAdvance(arabicText.text()) > 0);
  assert(arabicText.layoutDirection() == Qt::LeftToRight);

  // -------------------------------------------------------------
  // Test Case 11: Malformed JSON tolerance (No crash)
  // -------------------------------------------------------------
  std::cout << "[TEST 11] Malformed JSON tolerance..." << std::endl;
  QTemporaryDir tempDir;
  assert(tempDir.isValid());
  const QString brokenJsonPath = tempDir.filePath(QStringLiteral("broken_languages.json"));
  {
    QFile brokenFile(brokenJsonPath);
    assert(brokenFile.open(QIODevice::WriteOnly));
    brokenFile.write("{ this is definitely not valid json : [ }");
    brokenFile.close();
  }

  // Must not crash and should safely return false while preserving safe defaults
  const bool loaded = lm.loadLanguagesConfig(brokenJsonPath);
  assert(!loaded);
  assert(lm.isLanguageSupported(QStringLiteral("en")));
  assert(lm.isLanguageSupported(QStringLiteral("tr")));

  // Non-existent file tolerance
  const bool loadedNonExistent = lm.loadLanguagesConfig(QStringLiteral("/path/does/not/exist.json"));
  assert(!loadedNonExistent);

  // -------------------------------------------------------------
  // Test Case 12: formatSystemLanguageLabel
  // -------------------------------------------------------------
  std::cout << "[TEST 12] formatSystemLanguageLabel..." << std::endl;
  QLocale::setDefault(QLocale(QLocale::Turkish, QLocale::Turkey));
  lm.setLanguagePreference(QStringLiteral("tr"));
  const QString sysLabelTr = lm.formatSystemLanguageLabel();
  assert(sysLabelTr.contains(QStringLiteral("Türkçe")));

  lm.setLanguagePreference(QStringLiteral("en"));
  const QString sysLabelEn = lm.formatSystemLanguageLabel();
  assert(sysLabelEn.contains(QStringLiteral("Türkçe")));

  // -------------------------------------------------------------
  // Test Case 13: Cross-language helpers used by live switching
  // -------------------------------------------------------------
  std::cout << "[TEST 13] Cross-language lookup helpers..." << std::endl;
  lm.setLanguagePreference(QStringLiteral("fr"));
  assert(lm.translateIn(QStringLiteral("tr"), QStringLiteral("settings.title")) == QStringLiteral("Ayarlar"));
  assert(lm.translateIn(QStringLiteral("ar"), QStringLiteral("settings.title")) == QStringLiteral("الإعدادات"));
  assert(lm.translationsOf(QStringLiteral("settings.title")).size() == 4);
  // Reverse lookup finds the key from a value in any language.
  assert(lm.keyForText(QStringLiteral("Rechercher dans les paramètres")) == QLatin1String("settings.search_placeholder"));
  assert(lm.keyForText(QStringLiteral("البحث في الإعدادات")) == QLatin1String("settings.search_placeholder"));
  assert(lm.keyForText(QStringLiteral("Ayarlarda ara")) == QLatin1String("settings.search_placeholder"));
  assert(lm.keyForText(QStringLiteral("user bookmark title")).isEmpty());
  assert(I18n::isTranslationOf(QStringLiteral("newtab.title"), QStringLiteral("Yeni Sekme")));
  assert(I18n::isTranslationOf(QStringLiteral("newtab.title"), QStringLiteral("New tab")));
  assert(!I18n::isTranslationOf(QStringLiteral("newtab.title"), QStringLiteral("GitHub")));
  assert(I18n::retranslated(QStringLiteral("Ayarlar")) == QStringLiteral("Paramètres"));
  assert(I18n::retranslated(QStringLiteral("GitHub")) == QStringLiteral("GitHub"));

  // Internal-page template tokens: HTML-escaped, JS-literal, trusted markup, language.
  const QString html = I18n::localizeHtml(QStringLiteral(
      "<html lang=\"{{i18n-lang}}\" dir=\"ltr\"><p>{{i18n:settings.title}}</p>"
      "<script>t={{i18n-js:newtab.remove_site}}</script>{{i18n-html:blockpage.adult_body_html}}</html>"));
  assert(html.contains(QStringLiteral("lang=\"fr\" dir=\"ltr\"")));
  assert(html.contains(QStringLiteral("<p>Paramètres</p>")));
  assert(html.contains(QStringLiteral("t=\"Retirer %1 de la liste\"")));
  assert(html.contains(QStringLiteral("<br><br>")));
  assert(!html.contains(QStringLiteral("{{i18n")));

  // The environment override only replaces the *system* language.
  qputenv("DALINIRA_UI_LANGUAGE", "ar");
  lm.setLanguagePreference(QStringLiteral("system"));
  assert(lm.activeLanguageCode() == QLatin1String("ar"));
  lm.setLanguagePreference(QStringLiteral("en"));
  assert(lm.activeLanguageCode() == QLatin1String("en"));
  qunsetenv("DALINIRA_UI_LANGUAGE");

  // -------------------------------------------------------------
  // Test Case 14: Live widget retranslation keeps LTR for Arabic
  // -------------------------------------------------------------
  std::cout << "[TEST 14] Live widget retranslation, Arabic stays LTR..." << std::endl;
  lm.setLanguagePreference(QStringLiteral("tr"));
  QWidget root;
  auto *title = new QLabel(I18n::text(QStringLiteral("settings.title")), &root);
  auto *button = new QPushButton(I18n::text(QStringLiteral("common.save")), &root);
  button->setToolTip(I18n::text(QStringLiteral("common.cancel")));
  auto *combo = new QComboBox(&root);
  combo->addItem(I18n::text(QStringLiteral("common.on")), QStringLiteral("on"));
  auto *userData = new QLabel(QStringLiteral("My bookmark"), &root);
  auto *skipped = new QLabel(I18n::text(QStringLiteral("settings.title")), &root);
  skipped->setProperty(kSkipRetranslationProperty, true);
  retranslateWidgetTree(&root);
  for (const QString &code : {QStringLiteral("ar"), QStringLiteral("fr"), QStringLiteral("en"), QStringLiteral("tr")}) {
    lm.setLanguagePreference(code);
    retranslateWidgetTree(&root);
    assert(title->text() == lm.translateIn(code, QStringLiteral("settings.title")));
    assert(button->text() == lm.translateIn(code, QStringLiteral("common.save")));
    assert(button->toolTip() == lm.translateIn(code, QStringLiteral("common.cancel")));
    assert(combo->itemText(0) == lm.translateIn(code, QStringLiteral("common.on")));
    assert(combo->itemData(0).toString() == QLatin1String("on"));  // data untouched
    assert(userData->text() == QStringLiteral("My bookmark"));      // user data untouched
    assert(skipped->text() == QStringLiteral("Ayarlar"));
    // Arabic text, but the application and widget structure never mirror.
    assert(app.layoutDirection() == Qt::LeftToRight);
    assert(QGuiApplication::layoutDirection() == Qt::LeftToRight);
    assert(!root.isRightToLeft() && !button->isRightToLeft());
    assert(!lm.isRtl() && lm.layoutDirection() == Qt::LeftToRight);
  }
  lm.setLanguagePreference(QStringLiteral("ar"));
  QWidget createdWhileArabic;
  assert(createdWhileArabic.layoutDirection() == Qt::LeftToRight);

  std::cout << "[TEST] All LanguageManager tests PASSED successfully!" << std::endl;
  return 0;
}
