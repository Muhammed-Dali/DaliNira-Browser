// Localization regression test (non-GUI, offscreen).
//
// Covers the shared catalog used by both engines, every Settings category in
// Turkish/English/Arabic/French with live language switching (no stale
// strings after a switch), the Audio Effects page, the shared New Tab /
// private tab / block pages, language names, and the Arabic LTR guarantee.

#include "audio/audio_effects_page.h"
#include "audio/web_audio_effects_controller.h"
#include "browser_profile_service.h"
#include "i18n/i18n.h"
#include "i18n/language_manager.h"
#include "i18n/widget_i18n.h"
#include "newtab/new_tab_html.h"
#include "settings_page.h"
#include "tab_manager.h"
#include "translate/language_detector.h"

#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QGroupBox>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QRegularExpression>
#include <QSettings>
#include <QTemporaryDir>
#include <QTreeWidget>

#include <cstdlib>
#include <iostream>

using dalinira::i18n::LanguageManager;

namespace {

int failures = 0;

void check(bool condition, const QString &message) {
  if (condition) return;
  ++failures;
  std::cerr << "FAIL: " << message.toStdString() << std::endl;
}

QStringList visibleTexts(QWidget *root) {
  QStringList texts;
  QList<QWidget *> widgets{root};
  widgets.append(root->findChildren<QWidget *>());
  for (QWidget *widget : std::as_const(widgets)) {
    texts << widget->toolTip() << widget->accessibleName();
    if (auto *label = qobject_cast<QLabel *>(widget)) texts << label->text();
    if (auto *button = qobject_cast<QAbstractButton *>(widget)) texts << button->text();
    if (auto *group = qobject_cast<QGroupBox *>(widget)) texts << group->title();
    if (auto *edit = qobject_cast<QLineEdit *>(widget)) texts << edit->placeholderText();
    if (auto *combo = qobject_cast<QComboBox *>(widget)) {
      for (int i = 0; i < combo->count(); ++i) texts << combo->itemText(i);
    }
    if (auto *list = qobject_cast<QListWidget *>(widget)) {
      for (int i = 0; i < list->count(); ++i) texts << list->item(i)->text();
    }
    if (auto *tree = qobject_cast<QTreeWidget *>(widget)) {
      for (int c = 0; tree->headerItem() && c < tree->headerItem()->columnCount(); ++c)
        texts << tree->headerItem()->text(c);
    }
  }
  texts.removeAll(QString());
  return texts;
}

bool showsText(QWidget *root, const QString &expected) {
  const QString escaped = expected.toHtmlEscaped();
  for (const QString &text : visibleTexts(root)) {
    if (text == expected || text.contains(escaped)) return true;
  }
  return false;
}

QStringList placeholders(const QString &text) {
  static const QRegularExpression re(QStringLiteral(R"(%\d{1,2}(?!\d)|%s|\{[A-Za-z_][A-Za-z0-9_]*\})"));
  QStringList found;
  auto it = re.globalMatch(text);
  while (it.hasNext()) found << it.next().captured(0);
  found.sort();
  return found;
}

// Strings of the Turkish catalog that must not remain on screen after
// switching to `language` (values identical in both languages are fine).
QSet<QString> staleTurkish(const QString &language) {
  auto &lm = LanguageManager::instance();
  QSet<QString> current;
  QSet<QString> stale;
  const QStringList keys = lm.currentCatalog().keys();
  for (const QString &key : keys) current.insert(lm.translateIn(language, key));
  for (const QString &key : keys) {
    const QString turkish = lm.translateIn(QStringLiteral("tr"), key);
    if (turkish.size() > 3 && !current.contains(turkish)) stale.insert(turkish);
  }
  return stale;
}

}  // namespace

int main(int argc, char **argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  qunsetenv("DALINIRA_UI_LANGUAGE");
  QApplication app(argc, argv);
  QTemporaryDir settingsRoot;
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsRoot.path());
  app.setOrganizationName(QStringLiteral("DaliNiraTest"));
  app.setApplicationName(QStringLiteral("LocalizationUiTest"));

  auto &lm = LanguageManager::instance();
  lm.initialize();
  dalinira::i18n::installApplicationRetranslator();
  const QStringList languages{QStringLiteral("tr"), QStringLiteral("en"), QStringLiteral("ar"),
                              QStringLiteral("fr")};

  // 1. Catalogs: exact parity, no empty values, identical placeholders.
  QSet<QString> baseline;
  QHash<QString, QHash<QString, QString>> catalogs;
  for (const QString &language : languages) {
    QHash<QString, QString> catalog;
    check(lm.loadCatalog(language, catalog), QStringLiteral("catalog %1 loads").arg(language));
    catalogs.insert(language, catalog);
    const QStringList keyList = catalog.keys();
    const QSet<QString> keys(keyList.begin(), keyList.end());
    if (language == QLatin1String("tr")) baseline = keys;
    check(keys == baseline, QStringLiteral("%1 has exactly the tr key set").arg(language));
    check(keys.size() > 1500, QStringLiteral("%1 catalog is complete").arg(language));
  }
  for (const QString &key : std::as_const(baseline)) {
    const QStringList expected = placeholders(catalogs[QStringLiteral("tr")].value(key));
    for (const QString &language : languages) {
      const QString value = catalogs[language].value(key);
      check(!value.trimmed().isEmpty(), QStringLiteral("%1/%2 not empty").arg(language, key));
      check(placeholders(value) == expected, QStringLiteral("%1/%2 placeholders").arg(language, key));
    }
  }

  // 2. Settings: every category, four languages, live switching.
  QTemporaryDir profileDir;
  BrowserProfileService profileService(profileDir.path(), nullptr, &app);
  TabManager tabManager;
  auto *perfManager = tabManager.performanceManager();
  SettingsPage::Hooks hooks;
  hooks.searchEngine = [] { return QStringLiteral("Google"); };
  hooks.setSearchEngine = [](const QString &) {};
  hooks.syncNewTabs = [] {};
  hooks.refreshBookmarks = [] {};
  hooks.performanceManager = [perfManager] { return perfManager; };
  lm.setLanguagePreference(QStringLiteral("tr"));
  SettingsPage page(&profileService, std::move(hooks));
  page.resize(1100, 800);
  page.show();

  WebAudioEffectsController audioController(nullptr, /*persistenceEnabled=*/false);
  AudioEffectsPage audioPage(&audioController);
  audioPage.resize(1100, 800);
  audioPage.show();
  QApplication::processEvents();

  using Category = SettingsPage::Category;
  struct Probe {
    Category category;
    const char *categoryKey;
    QStringList keys;
  };
  const QList<Probe> probes = {
      {Category::Startup, "settings.category.startup",
       {QStringLiteral("settings.startup.restore_tabs"), QStringLiteral("settings.startup.behavior_card")}},
      {Category::Appearance, "settings.category.appearance",
       {QStringLiteral("settings.appearance.show_frequent_sites"), QStringLiteral("settings.appearance.removed_sites"),
        QStringLiteral("settings.appearance.tab_style_card")}},
      {Category::Performance, "settings.category.performance",
       {QStringLiteral("settings.performance.tab_sleep_card"), QStringLiteral("settings.performance.sleep_media_tabs"),
        QStringLiteral("settings.performance.never_sleep_card")}},
      {Category::Content, "settings.category.content",
       {QStringLiteral("settings.content.block_cookies")}},
      {Category::Privacy, "settings.category.privacy",
       {QStringLiteral("settings.privacy.permissions_card"), QStringLiteral("settings.privacy.cookies_and_site_data"),
        QStringLiteral("settings.privacy.secure_dns_card"), QStringLiteral("settings.privacy.adult_content_protection")}},
      {Category::Blocker, "settings.category.blocker",
       {QStringLiteral("settings.blocker.card"), QStringLiteral("settings.blocker.toolbar_counter")}},
      {Category::Search, "settings.category.search",
       {QStringLiteral("settings.search.default_engine"), QStringLiteral("settings.search.suggestions")}},
      {Category::Passwords, "settings.category.passwords",
       {QStringLiteral("settings.passwords.open_manager")}},
      {Category::Languages, "settings.category.languages",
       {QStringLiteral("settings.languages.preferred"), QStringLiteral("settings.languages.spellcheck"),
        QStringLiteral("settings.languages.use_dalinira_translate")}},
      {Category::Downloads, "settings.category.downloads",
       {QStringLiteral("settings.downloads.location"), QStringLiteral("settings.downloads.ask_location")}},
      {Category::Bookmarks, "settings.category.bookmarks",
       {QStringLiteral("settings.bookmarks.bar_card"), QStringLiteral("settings.bookmarks.column_name")}},
      {Category::History, "settings.category.history",
       {QStringLiteral("settings.history.recent_card"), QStringLiteral("settings.history.search_placeholder")}},
      {Category::Accessibility, "settings.category.accessibility",
       {QStringLiteral("settings.accessibility.placeholder_title")}},
      {Category::System, "settings.category.system",
       {QStringLiteral("settings.system.engine_card"), QStringLiteral("settings.system.apply_restart")}},
      {Category::Listening, "settings.category.listening",
       {QStringLiteral("pulse.sensitivity"), QStringLiteral("pulse.stop_on_found")}},
      {Category::Reset, "settings.category.reset",
       {QStringLiteral("settings.reset.performance_button")}},
      {Category::About, "settings.category.about",
       {QStringLiteral("settings.about.developer_card"), QStringLiteral("settings.about.feedback_card"),
        QStringLiteral("settings.about.feedback_bug_report"),
        QStringLiteral("settings.about.feedback_feature_request"),
        QStringLiteral("settings.about.feedback_privacy_note")}},
  };
  const QStringList audioKeys{QStringLiteral("audio.presets"), QStringLiteral("audio.reset_module"),
                              QStringLiteral("audio.module_eq32"), QStringLiteral("audio.balance_title"),
                              QStringLiteral("audio.param_threshold")};

  // tr -> en -> ar -> fr -> tr: every step is a live switch on open pages.
  QStringList sequence = languages;
  sequence << QStringLiteral("tr");
  for (const QString &language : std::as_const(sequence)) {
    lm.setLanguagePreference(language);
    QApplication::processEvents();
    check(lm.activeLanguageCode() == language, QStringLiteral("active language %1").arg(language));

    // Arabic text, left-to-right browser chrome.
    check(QGuiApplication::layoutDirection() == Qt::LeftToRight,
          QStringLiteral("%1: application layout stays LTR").arg(language));
    check(!lm.isRtl() && lm.layoutDirection() == Qt::LeftToRight,
          QStringLiteral("%1: LanguageManager reports LTR").arg(language));
    check(!page.isRightToLeft() && !audioPage.isRightToLeft(),
          QStringLiteral("%1: pages keep LTR layout").arg(language));
    QWidget freshWidget;
    check(freshWidget.layoutDirection() == Qt::LeftToRight,
          QStringLiteral("%1: new widgets default to LTR").arg(language));

    check(showsText(&page, lm.translateIn(language, QStringLiteral("settings.title"))),
          QStringLiteral("%1: Settings title").arg(language));
    check(showsText(&page, lm.translateIn(language, QStringLiteral("settings.search_placeholder"))),
          QStringLiteral("%1: Settings search placeholder").arg(language));
    for (const Probe &probe : probes) {
      page.setCategory(probe.category);
      QApplication::processEvents();
      check(showsText(&page, lm.translateIn(language, QString::fromLatin1(probe.categoryKey))),
            QStringLiteral("%1: sidebar %2").arg(language, QString::fromLatin1(probe.categoryKey)));
      for (const QString &key : probe.keys) {
        check(showsText(&page, lm.translateIn(language, key)),
              QStringLiteral("%1: Settings control %2 = %3")
                  .arg(language, key, lm.translateIn(language, key)));
      }
    }
    for (const QString &key : audioKeys) {
      check(showsText(&audioPage, lm.translateIn(language, key)),
            QStringLiteral("%1: audio control %2").arg(language, key));
    }

    if (language != QLatin1String("tr")) {
      const QSet<QString> stale = staleTurkish(language);
      for (QWidget *root : {static_cast<QWidget *>(&page), static_cast<QWidget *>(&audioPage)}) {
        for (const QString &text : visibleTexts(root)) {
          if (text.startsWith(QLatin1Char('/'))) continue;  // filesystem paths are user data
          check(!stale.contains(text), QStringLiteral("%1: stale Turkish text after live switch: %2")
                                           .arg(language, text));
          // Composed strings are not catalog values; Turkish-only letters
          // reveal any that were not refreshed.
          static const QRegularExpression turkishOnly(QStringLiteral("[ğışİĞŞ]"));
          check(!turkishOnly.match(text).hasMatch(),
                QStringLiteral("%1: Turkish text remains after live switch: %2").arg(language, text));
        }
      }
    }

    // Language names follow the UI language.
    check(LanguageDetector::languageDisplayName(QStringLiteral("de"))
              == lm.translateIn(language, QStringLiteral("language.name.de")),
          QStringLiteral("%1: language names are localized").arg(language));

    // 3. Shared New Tab (Chromium scheme handler and WebKit both call newTabHtml).
    QJsonArray frequent{QJsonObject{{QStringLiteral("name"), QStringLiteral("Example")},
                                    {QStringLiteral("url"), QStringLiteral("https://example.com/")}}};
    const QString ntp = newTabHtml(QStringLiteral("Google"), frequent, {}, 12, 3, 4, true, true,
                                   QStringLiteral("all_time"), QStringLiteral("cap"));
    const QString langAttr = QStringLiteral("lang=\"%1\" dir=\"ltr\"").arg(language);
    check(ntp.contains(langAttr), QStringLiteral("%1: New Tab declares lang and dir=ltr").arg(language));
    check(!ntp.contains(QLatin1String("dir=\"rtl\"")), QStringLiteral("%1: New Tab never RTL").arg(language));
    check(!ntp.contains(QLatin1String("{{i18n")), QStringLiteral("%1: New Tab tokens resolved").arg(language));
    for (const char *key : {"newtab.customize_title", "newtab.frequent_sites", "newtab.cat_top_sites",
                            "newtab.show_top_sites", "settings.appearance.removed_sites", "common.restore",
                            "newtab.downloads", "newtab.blocked_items"}) {
      const QString value = lm.translateIn(language, QString::fromLatin1(key)).toHtmlEscaped();
      check(ntp.contains(value), QStringLiteral("%1: New Tab shows %2").arg(language, QString::fromLatin1(key)));
    }
    const QString placeholder =
        lm.translateIn(language, QStringLiteral("newtab.search_placeholder_google"));
    check(ntp.contains(placeholder.toHtmlEscaped()), QStringLiteral("%1: search placeholder").arg(language));
    const QString removeSite = lm.translateIn(language, QStringLiteral("newtab.remove_site"));
    check(ntp.contains(QString(removeSite).replace(QLatin1Char('\''), QStringLiteral("\\u0027")))
              || ntp.contains(removeSite),
          QStringLiteral("%1: frequent-site remove control").arg(language));
    const QString privateTab = incognitoNewTabHtml(QStringLiteral("Google"));
    check(privateTab.contains(lm.translateIn(language, QStringLiteral("incognito.heading")).toHtmlEscaped()),
          QStringLiteral("%1: private New Tab").arg(language));
    const QString blocked = adultBlockedWarningHtml();
    check(blocked.contains(lm.translateIn(language, QStringLiteral("blockpage.adult_heading")).toHtmlEscaped()),
          QStringLiteral("%1: blocked page").arg(language));
    if (language != QLatin1String("tr")) {
      for (const QString &html : {ntp, privateTab, blocked}) {
        check(!html.contains(lm.translateIn(QStringLiteral("tr"), QStringLiteral("newtab.customize_title")))
                  && !html.contains(QStringLiteral("Gizli Moddasınız"))
                  && !html.contains(QStringLiteral("Yetişkin İçerik")),
              QStringLiteral("%1: no Turkish in internal pages").arg(language));
      }
    }
  }

  if (failures) {
    std::cerr << failures << " localization check(s) failed" << std::endl;
    return EXIT_FAILURE;
  }
  std::cout << "Localization UI test passed (tr/en/ar/fr, live switching, LTR)" << std::endl;
  return EXIT_SUCCESS;
}
