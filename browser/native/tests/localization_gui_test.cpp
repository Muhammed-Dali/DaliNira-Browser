// Chromium localization GUI validation, run once through
// scripts/safe-gui-test.sh (one window, local pages only, hard timeout).
//
// Cycles tr -> en -> ar -> fr on a single open BrowserWindow (live switch, no
// tab recreation) and checks: main toolbar, the New Tab document, Settings
// (General/Privacy/Performance/Languages), Audio Effects, a tab context menu,
// tab hover-card text and the translation popup. Arabic must keep the browser
// chrome left-to-right. A screenshot per language/page is written to
// gui-test-logs/ for visual review.

#include "audio/audio_effects_page.h"
#include "blocker/dalinira_blocker_page.h"
#include "blocker/dalinira_blocker_service.h"
#include "blocker/dalinira_blocker_settings.h"
#include "browser_profile_service.h"
#include "browser_window.h"
#include "desktop_tabs/tab_hover_card.h"
#include "desktop_tabs/tab_strip_widget.h"
#include "engine/engine_backend.h"
#include "engine/engine_storage_manager.h"
#if defined(DALINIRA_BUILD_WEBKIT)
#include "engine/webkit/webkit_backend.h"
#endif
#include "i18n/i18n.h"
#include "i18n/language_manager.h"
#include "i18n/widget_i18n.h"
#include "settings_page.h"
#include "translate/translate_bubble_popup.h"

#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QCheckBox>
#include <QDir>
#include <QElapsedTimer>
#include <QGroupBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QSettings>
#include <QScrollArea>
#include <QScrollBar>
#include <QPlainTextEdit>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QToolButton>
#include <QWebEnginePage>
#include <QWebEngineView>

#include <sys/resource.h>

#include <functional>
#include <iostream>
#include <memory>

using dalinira::i18n::LanguageManager;

namespace {

int failures = 0;

void check(bool condition, const QString &message) {
  if (condition) return;
  ++failures;
  std::cout << "FAIL: " << message.toStdString() << std::endl;
}

bool waitUntil(const std::function<bool()> &condition, int timeoutMs) {
  QElapsedTimer timer;
  timer.start();
  while (!condition()) {
    if (timer.elapsed() > timeoutMs) return false;
    QApplication::processEvents(QEventLoop::AllEvents, 50);
  }
  return true;
}

QVariant runInTab(BrowserWindow &window, int index, const QString &script) {
  auto state = std::make_shared<std::pair<bool, QVariant>>(false, QVariant());
  const BrowserTabInfo &tab = window.tabInfo(index);
  const auto done = [state](const QVariant &value) { state->first = true; state->second = value; };
  if (tab.engineView) tab.engineView->runJavaScript(script, done);
  else if (tab.view) tab.view->page()->runJavaScript(script, done);
  waitUntil([state] { return state->first; }, 5000);
  return state->second;
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
    if (auto *combo = qobject_cast<QComboBox *>(widget))
      for (int i = 0; i < combo->count(); ++i) texts << combo->itemText(i);
    if (auto *list = qobject_cast<QListWidget *>(widget))
      for (int i = 0; i < list->count(); ++i) texts << list->item(i)->text();
  }
  return texts;
}

bool shows(QWidget *root, const QString &expected) {
  const QString escaped = expected.toHtmlEscaped();
  for (const QString &text : visibleTexts(root))
    if (text == expected || text.contains(escaped)) return true;
  return false;
}

int tabIndexFor(BrowserWindow &window, const QString &internalId) {
  for (int i = 0; i < window.tabCount(); ++i)
    if (window.tabInfo(i).internalId == internalId) return i;
  return -1;
}

void snapshot(QWidget *widget, const QString &name) {
  QDir().mkpath(QStringLiteral("gui-test-logs"));
  widget->grab().save(QStringLiteral("gui-test-logs/localization-%1.png").arg(name));
}

struct LocalBlockerSite {
  QTcpServer server;
  QStringList paths;

  LocalBlockerSite() {
    check(server.listen(QHostAddress::LocalHost), QStringLiteral("local blocker fixture listens"));
    QObject::connect(&server, &QTcpServer::newConnection, [&] {
      while (QTcpSocket *socket = server.nextPendingConnection()) {
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
          const QByteArray path = socket->readAll().split(' ').value(1);
          paths.append(QString::fromLatin1(path));
          QByteArray type = "text/javascript";
          QByteArray body;
          if (path == "/") {
            type = "text/html; charset=utf-8";
            body = "<!doctype html><html><head><title>Blocker Phase 3</title>"
                   "<script src='/ads/phase3.js'></script><script src='/ok.js'></script>"
                   "</head><body>blocker fixture</body></html>";
          } else if (path == "/ads/phase3.js") {
            body = "window.phase3AdLoaded=1;";
          } else if (path == "/ok.js") {
            body = "window.phase3OkLoaded=1;";
          }
          socket->write("HTTP/1.1 200 OK\r\nContent-Type: " + type +
                        "\r\nConnection: close\r\nContent-Length: " +
                        QByteArray::number(body.size()) + "\r\n\r\n" + body);
          socket->disconnectFromHost();
        });
      }
    });
  }

  QUrl pageUrl() const {
    return QUrl(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort()));
  }
};

}  // namespace

int main(int argc, char **argv) {
  if (qEnvironmentVariableIntValue("DALINIRA_GUI_KEEP_CORES") != 1) {
    const rlimit none{0, 0};
    setrlimit(RLIMIT_CORE, &none);
  }
  bool useWebKit = false;
  for (int i = 1; i < argc; ++i) useWebKit = useWebKit || QByteArray(argv[i]) == "--webkit";
#if defined(DALINIRA_BUILD_WEBKIT)
  if (useWebKit) QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
#else
  if (useWebKit) return 77;
#endif
  dalinira::engine::EngineBackend::setSelectedEngineType(
      useWebKit ? dalinira::engine::EngineType::WebKit : dalinira::engine::EngineType::Chromium);
  qunsetenv("DALINIRA_UI_LANGUAGE");
  QApplication app(argc, argv);
  app.setOrganizationName(QStringLiteral("DaliNiraTest"));
  app.setApplicationName(QStringLiteral("LocalizationGuiTest"));
  QTemporaryDir tempDir;
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, tempDir.path());

  auto &lm = LanguageManager::instance();
  lm.initialize();
  dalinira::i18n::installApplicationRetranslator();
  lm.setLanguagePreference(QStringLiteral("tr"));

  BrowserProfileService profileService(tempDir.path() + QStringLiteral("/profile"), nullptr, nullptr, false);
  dalinira::engine::EngineStorageManager engineStorage(tempDir.path() + QStringLiteral("/engine"));
#if defined(DALINIRA_BUILD_WEBKIT)
  if (useWebKit) {
    auto backend = std::make_unique<dalinira::engine::WebKitBackend>(&app);
    check(backend->initialize(engineStorage), QStringLiteral("WebKit backend initializes"));
    check(backend->enableNetworkFiltering(), QStringLiteral("WebKit network filter starts"));
    dalinira::engine::EngineBackend::registerBackend(dalinira::engine::EngineType::WebKit, std::move(backend));
  }
#endif
  BrowserServices services;
  services.profile = useWebKit ? nullptr : profileService.profile();
  services.profileService = &profileService;

  int result = 0;
  {
  BrowserWindow window(services, true);
  window.resize(1280, 820);
  window.show();
  const int newTabIndex = window.addNewTab(QUrl(QStringLiteral("dalinira://newtab/")));
  window.showSettings();
  window.showAudioEffects();
  window.showDaliNiraBlockerSettings(DaliNiraBlockerPage::Tab::Rulesets);
  check(waitUntil([&] { return window.findChild<SettingsPage *>() && window.findChild<AudioEffectsPage *>()
                           && window.findChild<DaliNiraBlockerPage *>(); }, 5000),
        QStringLiteral("internal pages opened"));
  auto *settings = window.findChild<SettingsPage *>();
  auto *audio = window.findChild<AudioEffectsPage *>();
  auto *blockerPage = window.findChild<DaliNiraBlockerPage *>();
  auto *blockerService = profileService.adBlockService();
  TranslateBubblePopup translatePopup(&window);

  check(blockerService != nullptr && blockerPage != nullptr, QStringLiteral("blocker service and page available"));
  const auto *ublockName = blockerPage->findChild<QLabel *>(QStringLiteral("ruleset-name-ublock-filters"));
  check(ublockName && ublockName->text() == QStringLiteral("uBlock filters – Ads, trackers, and more"),
        QStringLiteral("official uBlock filters name unchanged"));
  if (ublockName) {
    auto *toggle = ublockName->parentWidget()->findChild<QCheckBox *>();
    check(toggle != nullptr, QStringLiteral("ruleset toggle is accessible"));
    if (toggle) {
      const bool wasChecked = toggle->isChecked();
      toggle->setChecked(!wasChecked);
      QApplication::processEvents();
      check(blockerService->settings()->rulesetSelectionConfigured(),
            QStringLiteral("ruleset selection persists through settings"));
      check(blockerService->settings()->enabledRulesetIds().contains(QStringLiteral("ublock-filters")) == !wasChecked,
            QStringLiteral("ruleset toggle changes immutable ID selection"));
      toggle->setChecked(wasChecked);
    }
  }

  for (const QString &language : {QStringLiteral("tr"), QStringLiteral("en"), QStringLiteral("ar"),
                                  QStringLiteral("fr")}) {
    std::cout << "[" << language.toStdString() << "]" << std::endl;
    lm.setLanguagePreference(language);
    QApplication::processEvents();

    // Arabic text, left-to-right browser chrome.
    check(QGuiApplication::layoutDirection() == Qt::LeftToRight, language + QStringLiteral(": app LTR"));
    check(!window.isRightToLeft() && !window.tabStrip()->isRightToLeft(), language + QStringLiteral(": window LTR"));

    // Blocker provenance UI stays live across language switches. Official
    // third-party names are intentionally not translated; attribution labels are.
    window.showDaliNiraBlockerSettings(DaliNiraBlockerPage::Tab::Rulesets);
    QApplication::processEvents();
    check(shows(blockerPage, lm.translateIn(language, QStringLiteral("blocker.third_party_notice"))),
          language + QStringLiteral(": blocker native-engine notice"));
    struct AttributionSpot { const char *id; const char *provider; };
    const QList<AttributionSpot> spots{
        {"ublock-filters", "uBlock Origin project (uAssets)"},
        {"easylist", "EasyList project"},
        {"easyprivacy", "EasyList project"},
        {"adguard-mobile", "AdGuard and uBlock Origin projects"},
        {"tur-0", "AdGuard"},
    };
    for (const AttributionSpot &spot : spots) {
      const QString id = QString::fromLatin1(spot.id);
      const QString provider = QString::fromLatin1(spot.provider);
      const auto *providerLabel = blockerPage->findChild<QLabel *>(QStringLiteral("ruleset-provider-") + id);
      const auto *sourceLabel = blockerPage->findChild<QLabel *>(QStringLiteral("ruleset-source-") + id);
      const auto *formatLabel = blockerPage->findChild<QLabel *>(QStringLiteral("ruleset-format-") + id);
      check(providerLabel && providerLabel->text() ==
              lm.translateIn(language, QStringLiteral("blocker.list_provider")).arg(provider),
            language + QStringLiteral(": provider ") + id);
      check(sourceLabel && sourceLabel->openExternalLinks() && sourceLabel->text().contains(QStringLiteral("href=")),
            language + QStringLiteral(": project/source link ") + id);
      check(sourceLabel && sourceLabel->text().contains(
              lm.translateIn(language, QStringLiteral("blocker.list_project_source")).section(QStringLiteral("%1"), 0, 0).trimmed()),
            language + QStringLiteral(": localized project/source label ") + id);
      check(formatLabel && formatLabel->text() ==
              lm.translateIn(language, QStringLiteral("blocker.list_compiled_format"))
                  .arg(QStringLiteral("uBO Lite generated ruleset")),
            language + QStringLiteral(": generated format ") + id);
    }
    QScrollArea *rulesetScroll = nullptr;
    if (auto *catalog = blockerPage->findChild<QGroupBox *>(QStringLiteral("ruleset-catalog-group"))) {
      for (QScrollArea *candidate : blockerPage->findChildren<QScrollArea *>()) {
        if (candidate->widget() &&
            (candidate->widget() == catalog || candidate->widget()->isAncestorOf(catalog))) {
          rulesetScroll = candidate;
          break;
        }
      }
      check(rulesetScroll != nullptr, language + QStringLiteral(": ruleset scroll area found"));
      if (rulesetScroll) check(rulesetScroll->horizontalScrollBar()->maximum() == 0,
                               language + QStringLiteral(": no blocker horizontal overflow"));
    }
    snapshot(&window, (useWebKit ? QStringLiteral("webkit-") : QStringLiteral("chromium-")) +
                           language + QStringLiteral("-blocker-rulesets"));
    if (rulesetScroll) {
      rulesetScroll->verticalScrollBar()->setValue(
          qMin(rulesetScroll->verticalScrollBar()->maximum(), 900));
      QApplication::processEvents();
      snapshot(&window, (useWebKit ? QStringLiteral("webkit-") : QStringLiteral("chromium-")) +
                             language + QStringLiteral("-blocker-rows"));
      rulesetScroll->verticalScrollBar()->setValue(0);
    }

    // Main toolbar.
    check(shows(&window, lm.translateIn(language, QStringLiteral("toolbar.main_menu"))),
          language + QStringLiteral(": toolbar main menu tooltip"));
    check(shows(&window, lm.translateIn(language, QStringLiteral("toolbar.passwords"))),
          language + QStringLiteral(": toolbar passwords tooltip"));

    // New Tab (Chromium): reloaded in place by the language switch.
    window.switchTab(newTabIndex);
    QApplication::processEvents();
    const bool loaded = waitUntil([&] {
      return runInTab(window, newTabIndex, QStringLiteral("document.documentElement.lang")).toString() == language
          && runInTab(window, newTabIndex, QStringLiteral("document.readyState")).toString() == QLatin1String("complete");
    }, 15000);
    check(loaded, language + QStringLiteral(": New Tab reloaded in the new language"));
    const QString json = runInTab(window, newTabIndex, QStringLiteral(
        "JSON.stringify({dir:document.documentElement.dir,bodyDir:getComputedStyle(document.body).direction,"
        "placeholder:document.querySelector('#query')?.placeholder||'',"
        "customize:document.querySelector('#customization-title')?.textContent||'',"
        "restore:document.querySelector('#restore-frequent-sites')?.textContent||'',"
        "frequent:document.querySelector('#shortcuts')?.getAttribute('aria-label')||''})")).toString();
    std::cout << "  newtab " << json.toStdString() << std::endl;
    const QJsonObject ntp = QJsonDocument::fromJson(json.toUtf8()).object();
    check(ntp.value(QStringLiteral("dir")).toString() == QLatin1String("ltr"), language + QStringLiteral(": New Tab dir=ltr"));
    check(ntp.value(QStringLiteral("bodyDir")).toString() == QLatin1String("ltr"), language + QStringLiteral(": New Tab body ltr"));
    // The engine the window actually uses (profile preference, DuckDuckGo by
    // default) on both Chromium and WebKitGTK.
    const QString newTabSearchEngine = window.currentSearchEngine();
    check(ntp.value(QStringLiteral("placeholder")).toString()
              == lm.translateIn(language, QStringLiteral("newtab.search_placeholder_")
                                              + newTabSearchEngine.toLower()),
          language + QStringLiteral(": New Tab search placeholder"));
    check(ntp.value(QStringLiteral("customize")).toString() == lm.translateIn(language, QStringLiteral("newtab.customize_title")),
          language + QStringLiteral(": New Tab customization title"));
    check(ntp.value(QStringLiteral("restore")).toString() == lm.translateIn(language, QStringLiteral("common.restore")),
          language + QStringLiteral(": New Tab restore hidden sites"));
    check(ntp.value(QStringLiteral("frequent")).toString() == lm.translateIn(language, QStringLiteral("newtab.frequent_sites")),
          language + QStringLiteral(": New Tab frequent sites"));
    snapshot(&window, language + QStringLiteral("-newtab"));

    // Settings pages that were already open before the switch.
    const int settingsIndex = tabIndexFor(window, QStringLiteral("settings"));
    window.switchTab(settingsIndex);
    check(window.tabInfo(settingsIndex).title == lm.translateIn(language, QStringLiteral("menu.settings")),
          language + QStringLiteral(": Settings tab title"));
    struct Page { SettingsPage::Category category; const char *name; QStringList keys; };
    const QList<Page> pages{
        {SettingsPage::Category::Startup, "general", {QStringLiteral("settings.startup.restore_tabs")}},
        {SettingsPage::Category::Privacy, "privacy", {QStringLiteral("settings.privacy.permissions_card"),
                                                      QStringLiteral("settings.privacy.cookies_and_site_data")}},
        {SettingsPage::Category::Performance, "memory", {QStringLiteral("settings.performance.tab_sleep_card"),
                                                         QStringLiteral("settings.performance.never_sleep_card")}},
        {SettingsPage::Category::Languages, "languages", {QStringLiteral("settings.languages.preferred"),
                                                          QStringLiteral("settings.languages.spellcheck")}},
    };
    for (const Page &page : pages) {
      settings->setCategory(page.category);
      QApplication::processEvents();
      for (const QString &key : page.keys)
        check(shows(settings, lm.translateIn(language, key)), QStringLiteral("%1: Settings %2").arg(language, key));
      snapshot(&window, language + QStringLiteral("-settings-") + QString::fromLatin1(page.name));
    }

    // Audio Effects page.
    window.switchTab(tabIndexFor(window, QStringLiteral("audio-effects")));
    QApplication::processEvents();
    check(shows(audio, lm.translateIn(language, QStringLiteral("audio.presets"))), language + QStringLiteral(": audio presets"));
    check(shows(audio, lm.translateIn(language, QStringLiteral("audio.module_eq32"))), language + QStringLiteral(": audio module list"));
    snapshot(&window, language + QStringLiteral("-audio"));

    // Tab context menu, created after the switch.
    QStringList menuTexts;
    QTimer::singleShot(400, &app, [&] {
      if (auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
        for (QAction *action : menu->actions()) menuTexts << action->text();
        snapshot(menu, language + QStringLiteral("-tab-context-menu"));
        menu->close();
      }
    });
    window.onTabContextMenuRequested(newTabIndex, window.mapToGlobal(QPoint(120, 20)));
    check(menuTexts.contains(lm.translateIn(language, QStringLiteral("tab.context.new_tab_right"))),
          language + QStringLiteral(": tab context menu"));
    check(menuTexts.contains(lm.translateIn(language, QStringLiteral("tab.context.sleep_menu"))),
          language + QStringLiteral(": sleep/memory submenu"));

    // Tab hover-card text for the live New Tab.
    const dalinira::TabResourceText hover = dalinira::describeTabResources(window.tabResourceSnapshot(newTabIndex));
    check(!hover.activity.isEmpty() && lm.keyForText(hover.activity).startsWith(QLatin1String("tab.hover.")),
          language + QStringLiteral(": hover card state ") + hover.activity);
    check(lm.translationsOf(lm.keyForText(hover.activity)).contains(hover.activity)
              && hover.activity == lm.translateIn(language, lm.keyForText(hover.activity)),
          language + QStringLiteral(": hover card in UI language"));

    // Translation popup (static strings follow the UI language).
    QApplication::processEvents();
    check(shows(&translatePopup, lm.translateIn(language, QStringLiteral("translate.open_settings"))),
          language + QStringLiteral(": translation popup"));
  }

  // A deterministic local request passes through the selected real engine and
  // the shared DaliNiraBlockerService. The custom rule blocks exactly one
  // script; disabling protection lets that same script load on reload.
  LocalBlockerSite blockerSite;
  const QUrl blockedScript(blockerSite.pageUrl().resolved(QUrl(QStringLiteral("ads/phase3.js"))));
  blockerService->settings()->setCustomFilters(
      {QStringLiteral("|") + blockedScript.toString() + QStringLiteral("|$script")});
  blockerService->settings()->setProtectionEnabled(true);
  blockerService->reloadRules();
  const quint64 blockedBefore = blockerService->sessionBlockedCount();
  const int smokeIndex = window.addNewTab(blockerSite.pageUrl());
  check(waitUntil([&] {
          return runInTab(window, smokeIndex,
                          QStringLiteral("window.phase3OkLoaded===1&&window.phase3AdLoaded===undefined")).toBool();
        }, 12000),
        QStringLiteral("selected engine blocks the fixture script and loads the ordinary script"));
  check(!blockerSite.paths.contains(QStringLiteral("/ads/phase3.js")),
        QStringLiteral("blocked request never reaches fixture server"));
  check(blockerService->sessionBlockedCount() > blockedBefore,
        QStringLiteral("blocker statistics increment"));
  const auto smokeLogs = blockerService->recentLogs();
  check(std::any_of(smokeLogs.cbegin(), smokeLogs.cend(),
                    [&blockedScript](const NetworkLogEntry &entry) {
                      return entry.requestUrl == blockedScript.toString()
                          && entry.action == DaliNiraBlockerAction::Block;
                    }), QStringLiteral("blocked fixture request is recorded in the request log"));

  blockerService->settings()->setProtectionEnabled(false);
  if (window.tabInfo(smokeIndex).engineView) window.tabInfo(smokeIndex).engineView->reload();
  else if (window.tabInfo(smokeIndex).view) window.tabInfo(smokeIndex).view->reload();
  check(waitUntil([&] {
          return runInTab(window, smokeIndex, QStringLiteral("window.phase3AdLoaded===1")).toBool();
        }, 12000), QStringLiteral("disabling blocker allows the same script on reload"));
  check(blockerSite.paths.contains(QStringLiteral("/ads/phase3.js")),
        QStringLiteral("disabled blocker request reaches fixture server"));
  blockerService->settings()->setProtectionEnabled(true);
  blockerService->settings()->setCustomFilters({});

  blockerPage->refreshAll();
  blockerPage->setActiveTab(DaliNiraBlockerPage::Tab::Statistics);
  QApplication::processEvents();
  blockerPage->setActiveTab(DaliNiraBlockerPage::Tab::Logger);
  QApplication::processEvents();
  auto *logTable = blockerPage->findChild<QTableWidget *>();
  check(logTable && logTable->rowCount() > 0, QStringLiteral("request-log UI shows the smoke-test decision"));
  blockerPage->setActiveTab(DaliNiraBlockerPage::Tab::CustomFilters);
  QApplication::processEvents();
  check(blockerPage->findChild<QPlainTextEdit *>() != nullptr,
        QStringLiteral("custom filters editor remains accessible"));
  blockerPage->setActiveTab(DaliNiraBlockerPage::Tab::Sites);
  QApplication::processEvents();
  const auto siteEdits = blockerPage->findChildren<QLineEdit *>();
  check(std::any_of(siteEdits.cbegin(), siteEdits.cend(), [](QLineEdit *edit) {
                      return edit->placeholderText() == QLatin1String("example.com");
                    }), QStringLiteral("site-specific controls remain accessible"));
  blockerPage->setActiveTab(DaliNiraBlockerPage::Tab::Settings);
  QApplication::processEvents();
  QCheckBox *strictToggle = nullptr;
  QCheckBox *popupToggle = nullptr;
  for (QCheckBox *toggle : blockerPage->findChildren<QCheckBox *>()) {
    if (toggle->text() == lm.translateIn(QStringLiteral("fr"), QStringLiteral("blocker.strict_block")))
      strictToggle = toggle;
    if (toggle->text() == lm.translateIn(QStringLiteral("fr"), QStringLiteral("blocker.popup_block")))
      popupToggle = toggle;
  }
  check(strictToggle && popupToggle, QStringLiteral("strict and popup toggle widgets remain accessible"));
  if (strictToggle) strictToggle->setChecked(true);
  if (popupToggle) popupToggle->setChecked(true);
  check(blockerService->settings()->strictBlock() && blockerService->settings()->popupBlock(),
        QStringLiteral("strict and popup controls remain functional"));
  if (strictToggle) strictToggle->setChecked(false);
  if (popupToggle) popupToggle->setChecked(false);

  std::cout << (failures ? "FAILED" : "PASS") << ": "
            << (useWebKit ? "WebKit" : "Chromium") << " localization/blocker GUI validation ("
            << failures << " failures)" << std::endl;
  result = failures ? 1 : 0;
  window.close();
  QApplication::processEvents();
  }
  if (useWebKit) dalinira::engine::EngineBackend::shutdown();
  return result;
}
