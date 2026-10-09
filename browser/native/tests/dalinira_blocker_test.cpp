#include <QApplication>
#include <QCheckBox>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QFrame>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMouseEvent>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <algorithm>
#include <cassert>
#include <iostream>

#include <QWebEngineUrlRequestInfo>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineScriptCollection>
#include "dalinira_blocker_engine.h"
#include "dalinira_blocker_element_picker.h"
#include "dalinira_blocker_list_manager.h"
#include "dalinira_blocker_page.h"
#include "dalinira_blocker_service.h"
#include "dalinira_blocker_settings.h"
#include "dalinira_blocker_shield_button.h"
#include "dalinira_blocker_types.h"
#include "i18n/i18n.h"
#include "i18n/language_manager.h"
#include "new_tab_html.h"
#include "blocker_http_fixture.h"

static int runAdvancedBounceStorageTest() {
  std::cout << "[Advanced bounce + ephemeral storage] Running focused policy tests...\n";
  QTemporaryDir dir;
  assert(dir.isValid());
  DaliNiraBlockerService service(dir.path());
  service.settings()->setEnabledRulesetIds({});
  service.settings()->setCustomFilters({
      QStringLiteral("||tracker-bounce.test^$third-party"),
      QStringLiteral("||login-tracker.test^$third-party"),
      QStringLiteral("||payment-tracker.test^$third-party")});
  service.reloadRules();

  QStringList expired;
  QObject::connect(&service, &DaliNiraBlockerService::temporarySiteDataExpired,
                   [&expired](const QString &host) { expired.append(host); });

  const QUrl source(QStringLiteral("https://shop.test/start"));
  const QUrl bounce(QStringLiteral("https://tracker-bounce.test/redirect?id=7"));
  const QUrl destination(QStringLiteral("https://publisher.test/article"));
  service.registerTab(0x801, source);
  service.updateTabUrl(0x801, bounce);
  const QString cleanup = service.temporaryStorageCleanupScript(0x801, bounce);
  assert(cleanup.contains(QStringLiteral("localStorage.clear")));
  assert(cleanup.contains(QStringLiteral("indexedDB.databases")));
  assert(cleanup.contains(QStringLiteral("serviceWorker.getRegistrations")));
  service.updateTabUrl(0x801, destination);
  assert(expired == QStringList{QStringLiteral("tracker-bounce.test")});

  // Broad privacy rules must not make authentication or payment hand-offs
  // ephemeral: these flows deliberately depend on temporary first-party state.
  service.registerTab(0x802, source);
  const QUrl login(QStringLiteral("https://login-tracker.test/oauth/callback?state=kept"));
  service.updateTabUrl(0x802, login);
  assert(service.temporaryStorageCleanupScript(0x802, login).isEmpty());
  const QUrl payment(QStringLiteral("https://payment-tracker.test/3ds/complete"));
  service.updateTabUrl(0x802, payment);
  assert(service.temporaryStorageCleanupScript(0x802, payment).isEmpty());

  // Cleanup waits until the last top-level owner leaves the temporary site.
  service.registerTab(0x803, source);
  service.registerTab(0x804, source);
  service.updateTabUrl(0x803, bounce);
  service.updateTabUrl(0x804, bounce);
  service.updateTabUrl(0x803, destination);
  assert(expired.size() == 1);
  service.unregisterTab(0x804);
  assert(expired.size() == 2 && expired.last() == QLatin1String("tracker-bounce.test"));

  // Ordinary cross-site navigation and explicit site exemptions stay normal.
  service.registerTab(0x805, source);
  const QUrl ordinary(QStringLiteral("https://cdn.test/content"));
  service.updateTabUrl(0x805, ordinary);
  assert(service.temporaryStorageCleanupScript(0x805, ordinary).isEmpty());
  SitePolicy allowed;
  allowed.whitelisted = true;
  service.settings()->setSitePolicy(QStringLiteral("tracker-bounce.test"), allowed);
  service.registerTab(0x806, source);
  service.updateTabUrl(0x806, bounce);
  assert(service.temporaryStorageCleanupScript(0x806, bounce).isEmpty());

  std::cout << "  ✓ tracker-listed rapid intermediaries expire after their last top-level owner; login/payment and ordinary sites remain persistent.\n";
  return 0;
}

static int runChromiumBounceStorageRuntimeTest() {
  std::cout << "[Advanced bounce storage] Running focused Chromium runtime test...\n";
  QTemporaryDir dir;
  assert(dir.isValid());
  DaliNiraBlockerService service(dir.path());
  service.settings()->setEnabledRulesetIds({});
  service.settings()->setCustomFilters({QStringLiteral("||tracker-bounce.test^$third-party")});
  service.reloadRules();
  const quint64 tab = 0x8c1;
  service.registerTab(tab, QUrl(QStringLiteral("https://source.test/")));
  const QUrl classified(QStringLiteral("https://tracker-bounce.test/r"));
  service.updateTabUrl(tab, classified);
  const QString cleanupSource = service.temporaryStorageCleanupScript(tab, classified);
  assert(!cleanupSource.isEmpty());

  HttpFixture bounce;
  HttpFixture destination(QHostAddress(QStringLiteral("127.0.0.2")));
  bounce.setRoute("/bounce", {200,
      "<!doctype html><title>bounce</title><script>"
      "localStorage.setItem('bounce','1');sessionStorage.setItem('bounce','1');window.seeded=true;"
      "</script>", "text/html", {}});
  destination.setRoute("/landing", {200, "<!doctype html><title>landing</title>", "text/html", {}});

  QWebEngineProfile profile;
  QWebEnginePage page(&profile);
  QWebEngineScript cleanup;
  cleanup.setName(QStringLiteral("dalinira-temporary-bounce-runtime-test"));
  cleanup.setInjectionPoint(QWebEngineScript::DocumentCreation);
  cleanup.setWorldId(QWebEngineScript::ApplicationWorld);
  cleanup.setRunsOnSubFrames(false);
  cleanup.setSourceCode(cleanupSource);
  page.scripts().insert(cleanup);
  auto load = [&](const QUrl &url) {
    bool finished = false;
    bool ok = false;
    const auto connection = QObject::connect(&page, &QWebEnginePage::loadFinished, [&](bool success) {
      finished = true;
      ok = success;
    });
    page.load(url);
    assert(waitUntil([&] { return finished; }, 10000) && ok);
    QObject::disconnect(connection);
  };
  auto js = [&](const QString &source) {
    bool done = false;
    QVariant value;
    page.runJavaScript(source, QWebEngineScript::ApplicationWorld, [&](const QVariant &result) {
      value = result;
      done = true;
    });
    assert(waitUntil([&] { return done; }, 3000));
    return value;
  };

  const QUrl bounceUrl(bounce.url(QStringLiteral("/bounce")));
  load(bounceUrl);
  // The fixture's marker is in the page world; the cleanup intentionally
  // runs in ApplicationWorld. DOM storage is shared between those worlds.
  assert(js(QStringLiteral("localStorage.getItem('bounce')==='1'")).toBool());
  load(QUrl(destination.url(QStringLiteral("/landing"))));
  QEventLoop settle;
  QTimer::singleShot(200, &settle, &QEventLoop::quit);
  settle.exec();
  bounce.setRoute("/bounce", {200, "<!doctype html><title>verify</title>", "text/html", {}});
  load(bounceUrl);
  assert(js(QStringLiteral("localStorage.getItem('bounce')===null")).toBool());
  std::cout << "  ✓ Chromium clears temporary bounce local/session storage before the rapid intermediate is discarded.\n";
  return 0;
}

int main(int argc, char *argv[]) {
  // Qt WebEngine consumes these while QApplication is being constructed.
  // Keep this test self-contained when it is launched directly instead of
  // through CTest. This changes only the test process; production startup
  // continues to use its normal sandbox policy.
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
  if (qEnvironmentVariableIsEmpty("QTWEBENGINE_CHROMIUM_FLAGS"))
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", "--no-sandbox --disable-gpu");
  qputenv("QTWEBENGINE_DISABLE_SANDBOX", "1");

  // Use QApplication because we test widgets and event filters
  QApplication app(argc, argv);
  const bool chromiumPickerOnly =
      argc == 2 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--chromium-picker-only");
  const bool bounceStorageOnly =
      argc == 2 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--bounce-storage-only");
  if (bounceStorageOnly) return runAdvancedBounceStorageTest();
  const bool chromiumBounceStorageOnly =
      argc == 2 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--chromium-bounce-storage-only");
  if (chromiumBounceStorageOnly) return runChromiumBounceStorageRuntimeTest();
  std::cout << "[DaliNira Blocker Release Audit Test] Running comprehensive audit verification test suite...\n";

  QTemporaryDir tempDir;
  assert(tempDir.isValid());
  const QString dataDir = tempDir.path();

  if (!chromiumPickerOnly) {
    // -------------------------------------------------------------
    // TEST 1: Mode Card Interaction & EventFilter
    // -------------------------------------------------------------
    {
    std::cout << "  - [1/17] Testing Mode Card Click Event Filter...\n";
    DaliNiraBlockerService service(dataDir);
    DaliNiraBlockerPage page(&service);

    const auto catalog = service.listManager()->availableLists();
    assert(catalog.size() == 56);
    for (const auto &list : catalog) {
      assert(!list.id.isEmpty() && !list.name.isEmpty());
      assert(!list.provider.isEmpty() && !list.projectUrl.isEmpty());
      assert(!list.source.isEmpty() && !list.sourceUrl.isEmpty());
      assert(!list.license.isEmpty());
      assert(list.formatOrigin == QStringLiteral("uBO Lite generated ruleset"));
      assert(QFileInfo::exists(service.listManager()->rulesetDir() +
                               QStringLiteral("/main/") + list.id + QStringLiteral(".json")));
    }
    const auto catalogEntry = [&catalog](const QString &id) -> FilterListInfo {
      const auto it = std::find_if(catalog.cbegin(), catalog.cend(), [&id](const FilterListInfo &list) {
        return list.id == id;
      });
      assert(it != catalog.cend());
      return *it;
    };
    assert(catalogEntry(QStringLiteral("ublock-filters")).name ==
           QStringLiteral("uBlock filters – Ads, trackers, and more"));
    assert(catalogEntry(QStringLiteral("ublock-badware")).name ==
           QStringLiteral("uBlock filters – Badware risks"));
    assert(catalogEntry(QStringLiteral("adguard-mobile")).name ==
           QStringLiteral("AdGuard/uBO – Mobile Ads"));
    assert(catalogEntry(QStringLiteral("easyprivacy")).provider ==
           QStringLiteral("EasyList project"));

    auto &languageManager = dalinira::i18n::LanguageManager::instance();
    for (const QString &language : {QStringLiteral("tr"), QStringLiteral("en"),
                                    QStringLiteral("ar"), QStringLiteral("fr")}) {
      for (const QString &key : {QStringLiteral("blocker.third_party_filter_lists"),
                                 QStringLiteral("blocker.third_party_notice"),
                                 QStringLiteral("blocker.list_provider"),
                                 QStringLiteral("blocker.list_project_source"),
                                 QStringLiteral("blocker.list_compiled_format")}) {
        const QString translated = languageManager.translateIn(language, key);
        assert(!translated.isEmpty() && translated != key);
      }
    }

    page.setActiveTab(DaliNiraBlockerPage::Tab::Rulesets);
    QApplication::processEvents();
    const auto *engineNotice =
        page.findChild<QLabel *>(QStringLiteral("ruleset-native-engine-notice"));
    const auto *officialName =
        page.findChild<QLabel *>(QStringLiteral("ruleset-name-ublock-filters"));
    const auto *provider =
        page.findChild<QLabel *>(QStringLiteral("ruleset-provider-ublock-filters"));
    const auto *source =
        page.findChild<QLabel *>(QStringLiteral("ruleset-source-ublock-filters"));
    const auto *format =
        page.findChild<QLabel *>(QStringLiteral("ruleset-format-ublock-filters"));
    assert(engineNotice != nullptr && !engineNotice->text().isEmpty());
    assert(officialName != nullptr && officialName->text() ==
           QStringLiteral("uBlock filters – Ads, trackers, and more"));
    assert(provider != nullptr && provider->text().contains(QStringLiteral("uAssets")));
    assert(source != nullptr && source->openExternalLinks() &&
           source->text().contains(QStringLiteral("github.com/uBlockOrigin/uAssets")));
    assert(format != nullptr && format->text().contains(QStringLiteral("uBO Lite")));

    // Initial mode is Ideal
    assert(service.settings()->mode() == DaliNiraBlockerMode::Ideal);

    // Find basic card and simulate click
    QWidget *basicCard = page.findChild<QWidget *>(QStringLiteral("mode-card"));
    assert(basicCard != nullptr);

    QMouseEvent pressEv(QEvent::MouseButtonPress, QPointF(10, 10), QPointF(10, 10), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent releaseEv(QEvent::MouseButtonRelease, QPointF(10, 10), QPointF(10, 10), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);

    // Direct mode setting verification
    service.settings()->setMode(DaliNiraBlockerMode::Basic);
    assert(service.settings()->mode() == DaliNiraBlockerMode::Basic);

    service.settings()->setMode(DaliNiraBlockerMode::Aggressive);
    assert(service.settings()->mode() == DaliNiraBlockerMode::Aggressive);

    service.settings()->setMode(DaliNiraBlockerMode::Ideal);
    assert(service.settings()->mode() == DaliNiraBlockerMode::Ideal);

    DaliNiraBlockerQuickPopup popup(&service);
    popup.updateForHost(QStringLiteral("youtube.com"), 5);
    popup.show();
    QApplication::processEvents();
    const auto *popupCount = popup.findChild<QLabel *>(QStringLiteral("adblock-popup-blocked-count"));
    assert(popupCount != nullptr && popupCount->text() == QStringLiteral("5"));
    auto *masterToggle = popup.findChild<QCheckBox *>(QStringLiteral("adblock-master-toggle"));
    assert(masterToggle != nullptr && masterToggle->isChecked());
    masterToggle->setChecked(false);
    assert(service.settings()->protectionEnabled());
    assert(service.settings()->sitePolicy(QStringLiteral("youtube.com")).whitelisted);
    const auto disabledDecision = service.evaluateRequest(
        QUrl(QStringLiteral("https://ad.doubleclick.net/pagead/ads")),
        static_cast<int>(QWebEngineUrlRequestInfo::ResourceTypeScript),
        QUrl(QStringLiteral("https://www.youtube.com/")), 1);
    assert(disabledDecision.action == DaliNiraBlockerAction::Allow);
    assert(disabledDecision.reason == QStringLiteral("site-whitelisted"));
    assert(service.createScriptingScriptsForHost(QStringLiteral("youtube.com")).isEmpty());
    masterToggle->setChecked(true);
    assert(!service.settings()->sitePolicy(QStringLiteral("youtube.com")).whitelisted);

    popup.updateForHost(QStringLiteral("youtube.com"), 5);
    auto *siteProtectionToggle =
        popup.findChild<QCheckBox *>(QStringLiteral("adblock-site-protection-toggle"));
    assert(siteProtectionToggle != nullptr && siteProtectionToggle->isChecked());
    service.settings()->setAutoReloadOnModeChange(false);
    bool siteReloadRequested = false;
    QObject::connect(&popup, &DaliNiraBlockerQuickPopup::reloadRequested,
                     [&siteReloadRequested] { siteReloadRequested = true; });
    siteProtectionToggle->setChecked(false);
    assert(siteReloadRequested);
    assert(service.settings()->sitePolicy(QStringLiteral("youtube.com")).whitelisted);
    const auto whitelistedDecision = service.evaluateRequest(
        QUrl(QStringLiteral("https://ad.doubleclick.net/pagead/ads")),
        static_cast<int>(QWebEngineUrlRequestInfo::ResourceTypeScript),
        QUrl(QStringLiteral("https://www.youtube.com/")), 1);
    assert(whitelistedDecision.action == DaliNiraBlockerAction::Allow);
    assert(whitelistedDecision.reason == QStringLiteral("site-whitelisted"));
    assert(!popup.findChild<QCheckBox *>(QStringLiteral("adblock-site-protection-toggle"))->isChecked());

    siteProtectionToggle->setChecked(true);
    assert(!service.settings()->sitePolicy(QStringLiteral("youtube.com")).whitelisted);
    popup.hide();

    service.evaluateRequest(
        QUrl(QStringLiteral("https://ad.doubleclick.net/pagead/ads?ui-row-test=1")),
        static_cast<int>(QWebEngineUrlRequestInfo::ResourceTypeScript),
        QUrl(QStringLiteral("https://www.youtube.com/")), 1);
    page.setActiveTab(DaliNiraBlockerPage::Tab::Statistics);
    page.refreshAll();
    const int firstMetricRowCount =
        page.findChildren<QFrame *>(QStringLiteral("ranked-metric-row")).size();
    page.refreshAll();
    const int secondMetricRowCount =
        page.findChildren<QFrame *>(QStringLiteral("ranked-metric-row")).size();
    assert(firstMetricRowCount > 0 && secondMetricRowCount == firstMetricRowCount);
    const QString snapshotPath = qEnvironmentVariable("DALINIRA_ADBLOCK_UI_SNAPSHOT");
    if (!snapshotPath.isEmpty()) {
      page.resize(1500, 850);
      page.show();
      QApplication::processEvents();
      assert(page.grab().save(snapshotPath));
      page.hide();
    }
    std::cout << "    ✓ Mode card switching and persistence verified.\n";
  }

  // -------------------------------------------------------------
  // TEST 2: Tab ID Resolution & Counter Isolation
  // -------------------------------------------------------------
  {
    std::cout << "  - [2/17] Testing Tab Context Registry & Tab ID Isolation...\n";
    DaliNiraBlockerService service(dataDir);
    service.settings()->setCustomFilters({QStringLiteral("||ad-server.com^")});
    service.reloadRules();

    const quint64 tabA = 1001;
    const quint64 tabB = 1002;

    service.registerTab(tabA, QUrl(QStringLiteral("https://news.com/tech/article1")));
    service.registerTab(tabB, QUrl(QStringLiteral("https://portal.org/home")));

    // Verify Tab Resolution
    assert(service.resolveTabId(QUrl(QStringLiteral("https://news.com/tech/article1")), QUrl(QStringLiteral("https://ad-server.com/banner.js"))) == tabA);
    assert(service.resolveTabId(QUrl(QStringLiteral("https://portal.org/home")), QUrl(QStringLiteral("https://portal.org/logo.png"))) == tabB);

    // Request from Tab A -> Ad Blocked
    auto decA = service.evaluateRequest(QUrl(QStringLiteral("https://ad-server.com/banner.js")),
                                        static_cast<int>(QWebEngineUrlRequestInfo::ResourceTypeScript),
                                        QUrl(QStringLiteral("https://news.com/tech/article1")), 0);
    assert(decA.action == DaliNiraBlockerAction::Block);

    // Request from Tab B -> Normal Request Allowed
    auto decB = service.evaluateRequest(QUrl(QStringLiteral("https://portal.org/logo.png")),
                                        static_cast<int>(QWebEngineUrlRequestInfo::ResourceTypeImage),
                                        QUrl(QStringLiteral("https://portal.org/home")), 0);
    assert(decB.action == DaliNiraBlockerAction::Allow);

    // Verify Tab Isolation
    TabBlockerStats statsA = service.statsForTab(tabA);
    TabBlockerStats statsB = service.statsForTab(tabB);
    assert(statsA.blockedRequests == 1);
    assert(statsB.blockedRequests == 0);
    assert(statsA.allowedRequests == 0);
    assert(statsB.allowedRequests == 1);
    assert(service.sessionBlockedCount() == 1);
    const QList<NetworkLogEntry> trace = service.recentLogs(2);
    assert(trace.size() == 2);
    assert(trace.first().requestHost == QStringLiteral("portal.org"));
    assert(trace.first().initiatorHost == QStringLiteral("portal.org"));
    assert(trace.first().topLevelSite == QStringLiteral("portal.org"));
    assert(trace.first().requestMethod == QStringLiteral("GET"));

    // Unregister tab cleans its stats
    service.unregisterTab(tabA);
    assert(service.statsForTab(tabA).blockedRequests == 0);
    std::cout << "    ✓ Multi-tab request mapping & counter isolation verified.\n";
  }

  // -------------------------------------------------------------
  // TEST 3: Diagnostic Timing & Calculated Memory Metrics
  // -------------------------------------------------------------
  {
    std::cout << "  - [3/17] Testing Performance Timing & Memory Calculation...\n";
    DaliNiraBlockerService service(dataDir);
    service.settings()->setCustomFilters({QStringLiteral("||tracker.net^")});
    QElapsedTimer reloadTimer;
    reloadTimer.start();
    service.reloadRules();
    const qint64 reloadMs = reloadTimer.elapsed();

    assert(service.evaluationCount() == 0);
    assert(service.averageEvaluationTimeMs() == 0.0);

    // Evaluate 5 requests
    for (int i = 0; i < 5; ++i) {
      service.evaluateRequest(QUrl(QStringLiteral("https://tracker.net/track")),
                              static_cast<int>(QWebEngineUrlRequestInfo::ResourceTypeScript),
                              QUrl(QStringLiteral("https://example.com")), 1);
    }

    // Include worst-case no-match requests; a benchmark made only of an early
    // custom-rule hit hides the cost of scanning the compiled DNR plan.
    for (int i = 0; i < 50; ++i) {
      service.evaluateRequest(QUrl(QStringLiteral("https://clean-origin.test/assets/app-%1.js").arg(i)),
                              static_cast<int>(QWebEngineUrlRequestInfo::ResourceTypeScript),
                              QUrl(QStringLiteral("https://clean-origin.test")), 1);
    }

    assert(service.evaluationCount() == 55);
    assert(service.averageEvaluationTimeMs() >= 0.0);
    assert(service.lastEvaluationTimeMs() >= 0.0);

    auto percentileMicros = [](QVector<qint64> samples, double percentile) {
      std::sort(samples.begin(), samples.end());
      const qsizetype index = qBound<qsizetype>(0, static_cast<qsizetype>(percentile * (samples.size() - 1)),
                                               samples.size() - 1);
      return samples.at(index) / 1000.0;
    };
    QVector<qint64> requestSamples;
    requestSamples.reserve(1000);
    for (int i = 0; i < 1000; ++i) {
      QElapsedTimer sample;
      sample.start();
      service.evaluateRequest(QUrl(QStringLiteral("https://benchmark-clean.test/assets/%1.js").arg(i)),
                              static_cast<int>(QWebEngineUrlRequestInfo::ResourceTypeScript),
                              QUrl(QStringLiteral("https://benchmark-origin.test/")), 2);
      requestSamples.append(sample.nsecsElapsed());
    }
    QVector<qint64> popupSamples;
    popupSamples.reserve(1000);
    for (int i = 0; i < 1000; ++i) {
      QElapsedTimer sample;
      sample.start();
      service.decidePopup(QUrl(QStringLiteral("https://benchmark-popup.test/%1").arg(i)),
                          QUrl(QStringLiteral("https://benchmark-origin.test/")), 2);
      popupSamples.append(sample.nsecsElapsed());
    }

    const quint64 mem = service.estimatedMemoryBytes();
    assert(mem > 0);
    std::cout << "    ✓ Average evaluation time: " << service.averageEvaluationTimeMs() << " ms (Real Timer)\n";
    std::cout << "    ✓ reloadRules time: " << reloadMs << " ms\n";
    std::cout << "    ✓ Request evaluation p95/p99: " << percentileMicros(requestSamples, 0.95) << "/"
              << percentileMicros(requestSamples, 0.99) << " us; popup p95/p99: "
              << percentileMicros(popupSamples, 0.95) << "/" << percentileMicros(popupSamples, 0.99) << " us\n";
    std::cout << "    ✓ Estimated memory size: " << (mem / 1024.0) << " KB (Calculated Structure Metric)\n";
  }

  // -------------------------------------------------------------
  // TEST 4: Early Cosmetic Script Injection (DocumentCreation)
  // -------------------------------------------------------------
  {
    std::cout << "  - [4/17] Testing Early Cosmetic Script Injection...\n";
    DaliNiraBlockerService service(dataDir);
    service.settings()->setCustomFilters({
        QStringLiteral("site-a.com##.ad-banner"),
        QStringLiteral("site-b.com##.sponsor-box")
    });
    service.reloadRules();

    QElapsedTimer cosmeticTimer;
    cosmeticTimer.start();
    QWebEngineScript scriptA = service.createCosmeticScriptForHost(QStringLiteral("site-a.com"));
    const qint64 cosmeticMicros = cosmeticTimer.nsecsElapsed() / 1000;
    assert(scriptA.name() == QStringLiteral("dalinira-adblock-cosmetic"));
    assert(scriptA.injectionPoint() == QWebEngineScript::DocumentCreation);
    assert(scriptA.worldId() == QWebEngineScript::ApplicationWorld);
    assert(scriptA.sourceCode().contains(QStringLiteral(".ad-banner")));
    assert(!scriptA.sourceCode().contains(QStringLiteral(".sponsor-box")));

    QWebEngineScript scriptB = service.createCosmeticScriptForHost(QStringLiteral("site-b.com"));
    assert(scriptB.sourceCode().contains(QStringLiteral(".sponsor-box")));
    assert(!scriptB.sourceCode().contains(QStringLiteral(".ad-banner")));

    QWebEngineScript scriptYT = service.createCosmeticScriptForHost(QStringLiteral("youtube.com"));
    assert(scriptYT.name() == QStringLiteral("dalinira-adblock-cosmetic"));
    assert(scriptYT.injectionPoint() == QWebEngineScript::DocumentCreation);
    assert(scriptYT.worldId() == QWebEngineScript::ApplicationWorld);
    assert(scriptYT.sourceCode().contains(QStringLiteral("ytd-ad-slot-renderer")));
    // Player-internal ad containers are intentionally not hard-hidden. Live
    // A/B testing proved these broad selectors trigger YouTube's anti-adblock
    // machine even when every network request is allowed.
    assert(!scriptYT.sourceCode().contains(QStringLiteral(".ytp-ad-module")));
    assert(!scriptYT.sourceCode().contains(QStringLiteral(".video-ads")));
    assert(!scriptYT.sourceCode().contains(QStringLiteral("ytInitialPlayerResponse")));
    assert(!scriptYT.sourceCode().contains(QStringLiteral("pruneAdsFromObject")));

    // An unrelated host must not receive the large host-mapped scriptlet
    // bundles. Generic cosmetic assets may still use ApplicationWorld.
    const auto unrelatedScripts = service.createScriptingScriptsForHost(QStringLiteral("example.com"));
    for (const auto &runtimeScript : unrelatedScripts) {
      assert(runtimeScript.name() != QStringLiteral("dalinira-adblock-scriptlets-main"));
    }

    const auto runtimeScripts = service.createScriptingScriptsForHost(QStringLiteral("www.youtube.com"));
    bool hasMain = false;
    bool hasIsolated = false;
    for (const auto &runtimeScript : runtimeScripts) {
      if (runtimeScript.name() == QStringLiteral("dalinira-adblock-scriptlets-main")) {
        hasMain = runtimeScript.worldId() == QWebEngineScript::MainWorld &&
                  runtimeScript.injectionPoint() == QWebEngineScript::DocumentCreation &&
                  !runtimeScript.sourceCode().isEmpty();
      }
      if (runtimeScript.name() == QStringLiteral("dalinira-adblock-scriptlets-isolated")) {
        hasIsolated = runtimeScript.worldId() == QWebEngineScript::ApplicationWorld &&
                      runtimeScript.injectionPoint() == QWebEngineScript::DocumentCreation &&
                      !runtimeScript.sourceCode().isEmpty();
      }
    }
    assert(hasMain);
    const auto mainIt = std::find_if(runtimeScripts.begin(), runtimeScripts.end(), [](const QWebEngineScript &script) {
      return script.name() == QLatin1String("dalinira-adblock-scriptlets-main");
    });
    assert(mainIt != runtimeScripts.end());
    assert(mainIt->sourceCode().contains(QStringLiteral("all_web_enable_network_machine")));

    const auto isolatedIt = std::find_if(runtimeScripts.begin(), runtimeScripts.end(), [](const QWebEngineScript &script) {
      return script.name() == QLatin1String("dalinira-adblock-scriptlets-isolated");
    });
    // The generated YouTube isolated bundle currently contains a destructive
    // reload/seek scriptlet, so the list manager may reject that whole mapped
    // asset. Before Step 3, popup metadata was accidentally appended as an
    // "isolated" script and made this assertion pass without testing a
    // scriptlet at all. If a safe YouTube bundle is present it must retain the
    // fail-safe; isolated-world scriptlets are verified independently below.
    if (isolatedIt != runtimeScripts.end()) {
      assert(hasIsolated);
      assert(!isolatedIt->sourceCode().contains(QStringLiteral("serverContract")));
      assert(!isolatedIt->sourceCode().contains(QStringLiteral("loadVideoById")));
    }
    std::cout << "    ✓ Cosmetic script build: " << cosmeticMicros << " us\n";

    const auto isolatedHostScripts = service.createScriptingScriptsForHost(QStringLiteral("msn.com"));
    const auto genuineIsolatedIt = std::find_if(
        isolatedHostScripts.begin(), isolatedHostScripts.end(), [](const QWebEngineScript &script) {
          return script.name() == QLatin1String("dalinira-adblock-scriptlets-isolated");
        });
    assert(genuineIsolatedIt != isolatedHostScripts.end());
    assert(genuineIsolatedIt->worldId() == QWebEngineScript::ApplicationWorld);
    assert(genuineIsolatedIt->injectionPoint() == QWebEngineScript::DocumentCreation);
    assert(!genuineIsolatedIt->sourceCode().isEmpty());

    // A YouTube SPA route change keeps the same document and host. Rebuilding
    // the plan for that host must be deterministic, and the returned plan must
    // contain at most one script for each installed name. BrowserWindow uses
    // those names to replace (rather than append) scripts on full navigation.
    const auto spaScripts = service.createScriptingScriptsForHost(QStringLiteral("www.youtube.com"));
    assert(spaScripts.size() == runtimeScripts.size());
    QSet<QString> installedNames;
    for (qsizetype i = 0; i < runtimeScripts.size(); ++i) {
      const QWebEngineScript &initialScript = runtimeScripts.at(i);
      const QWebEngineScript &spaScript = spaScripts.at(i);
      assert(!installedNames.contains(initialScript.name()));
      installedNames.insert(initialScript.name());
      assert(spaScript.name() == initialScript.name());
      assert(spaScript.sourceCode() == initialScript.sourceCode());
      assert(spaScript.worldId() == initialScript.worldId());
      assert(spaScript.injectionPoint() == initialScript.injectionPoint());
    }

    const QString specificCss = service.listManager()->loadSpecificCosmeticCssForHost(
        QStringLiteral("www.youtube.com"), {});
    assert(!specificCss.isEmpty());
    assert(specificCss.contains(QStringLiteral("display: none !important")));
    const QJsonArray proceduralRules = service.listManager()->loadProceduralRulesForHost(
        QStringLiteral("www.youtube.com"), {});
    assert(!proceduralRules.isEmpty());
    bool hasProcedural = false;
    for (const auto &runtimeScript : service.createScriptingScriptsForHost(QStringLiteral("www.youtube.com"))) {
      if (runtimeScript.name() == QStringLiteral("dalinira-adblock-procedural")) {
        hasProcedural = true;
      }
    }
    assert(!hasProcedural);

    std::cout << "    ✓ DocumentCreation early injection, clean YouTube anti-ad & sponsored-card protections verified.\n";
  }

  // -------------------------------------------------------------
  // TEST 4b: Allowlist applies to every filtering layer
  // -------------------------------------------------------------
  {
    std::cout << "  - [5/17] Testing Whitelist Cosmetic/Scriptlet Parity...\n";
    DaliNiraBlockerService service(dataDir);
    SitePolicy policy;
    policy.whitelisted = true;
    service.settings()->setSitePolicy(QStringLiteral("youtube.com"), policy);
    const auto script = service.createCosmeticScriptForHost(QStringLiteral("www.youtube.com"));
    assert(script.sourceCode().isEmpty());
    assert(service.createScriptingScriptsForHost(QStringLiteral("www.youtube.com")).isEmpty());
    const auto decision = service.evaluateRequest(
        QUrl(QStringLiteral("https://ad.doubleclick.net/pagead/ads")),
        static_cast<int>(QWebEngineUrlRequestInfo::ResourceTypeScript),
        QUrl(QStringLiteral("https://www.youtube.com/")), 11);
    assert(decision.action == DaliNiraBlockerAction::Allow);
    assert(decision.reason == QStringLiteral("site-whitelisted"));
    service.settings()->removeSitePolicy(QStringLiteral("youtube.com"));
    std::cout << "    ✓ Whitelist disables network, cosmetic, and scriptlet layers together.\n";
  }

  // -------------------------------------------------------------
  // TEST 4c: DNR case sensitivity must not be silently discarded
  // -------------------------------------------------------------
  {
    std::cout << "  - [6/17] Testing DNR Case-Sensitive URL Filters...\n";
    DaliNiraBlockerEngine engine;
    FilterRule rule;
    rule.id = 321;
    rule.actionType = QStringLiteral("block");
    rule.urlFilter = QStringLiteral("/CaseSensitiveAsset.js");
    rule.isCaseSensitive = true;
    engine.loadRules({rule});
    assert(engine.evaluate(QUrl(QStringLiteral("https://example.test/CaseSensitiveAsset.js")),
                           DaliNiraBlockerResourceType::Script, QStringLiteral("example.test"),
                           DaliNiraBlockerMode::Ideal, SitePolicy{}).action == DaliNiraBlockerAction::Block);
    assert(engine.evaluate(QUrl(QStringLiteral("https://example.test/casesensitiveasset.js")),
                           DaliNiraBlockerResourceType::Script, QStringLiteral("example.test"),
                           DaliNiraBlockerMode::Ideal, SitePolicy{}).action == DaliNiraBlockerAction::Allow);
    std::cout << "    ✓ Case-sensitive DNR condition honored.\n";
  }

  // -------------------------------------------------------------
  // TEST 5: Strict-Block Security Warning & Temporary Bypass
  // -------------------------------------------------------------
  {
    std::cout << "  - [7/17] Testing Strict-Block Warning Page & Bypass Lifecycle...\n";
    DaliNiraBlockerService service(dataDir);

    // Add malware strict-block rule
    FilterRule strictRule;
    strictRule.id = 999;
    strictRule.rulesetId = QStringLiteral("malware-strict");
    strictRule.actionType = QStringLiteral("block");
    strictRule.urlFilter = QStringLiteral("||dangerous-phishing.com^");
    service.filterEngine()->loadRules({strictRule});

    // 0. Default strictBlock is true -> Redirected to strictblock warning
    assert(service.settings()->strictBlock());
    auto dec0 = service.evaluateRequest(QUrl(QStringLiteral("https://dangerous-phishing.com/login")),
                                        static_cast<int>(QWebEngineUrlRequestInfo::ResourceTypeMainFrame),
                                        QUrl(), 100);
    assert(dec0.action == DaliNiraBlockerAction::Redirect);
    assert(dec0.redirectUrl.contains(QStringLiteral("strictblock=1")));
    assert(dec0.redirectUrl.contains(QStringLiteral("dangerous-phishing.com")));

    // 1. Disable strictBlock -> Regular Block without interstitial
    service.settings()->setStrictBlock(false);
    service.filterEngine()->loadRules({strictRule});
    assert(!service.settings()->strictBlock());
    auto dec1 = service.evaluateRequest(QUrl(QStringLiteral("https://dangerous-phishing.com/login")),
                                        static_cast<int>(QWebEngineUrlRequestInfo::ResourceTypeMainFrame),
                                        QUrl(), 100);
    assert(dec1.action == DaliNiraBlockerAction::Block);

    // Re-enable strictBlock for bypass lifecycle
    service.settings()->setStrictBlock(true);
    service.filterEngine()->loadRules({strictRule});

    // 2. Render Warning HTML
    QString warningHtml = strictBlockWarningHtml(QStringLiteral("dangerous-phishing.com"), QStringLiteral("https://dangerous-phishing.com/login"));
    assert(warningHtml.contains(I18n::text(QStringLiteral("blockpage.strict_heading")).toHtmlEscaped()));
    assert(warningHtml.contains(QStringLiteral("dangerous-phishing.com")));
    assert(warningHtml.contains(I18n::text(QStringLiteral("blockpage.proceed_15_min")).toHtmlEscaped()));
    const QString injection = QStringLiteral("https://dangerous-phishing.com/');window.daliniraOwned=true;//");
    const QString hardenedWarning = strictBlockWarningHtml(QStringLiteral("dangerous-phishing.com"), injection);
    assert(!hardenedWarning.isEmpty());
    assert(hardenedWarning.contains(QStringLiteral("const bypassTarget = \"https://dangerous-phishing.com/")));
    assert(!hardenedWarning.contains(QStringLiteral("const bypassTarget = '")));
    assert(!hardenedWarning.contains(QStringLiteral("encodeURIComponent('https://dangerous-phishing.com/');")));
    assert(strictBlockWarningHtml(QStringLiteral("dangerous-phishing.com"), QStringLiteral("file:///etc/passwd")).isEmpty());
    assert(strictBlockWarningHtml(QStringLiteral("dangerous-phishing.com"), QStringLiteral("https://different.example/")).isEmpty());
    const QUrl bypass(QStringLiteral("dalinira://bypass-strictblock?domain=dangerous-phishing.com&target=https%3A%2F%2Fdangerous-phishing.com%2Flogin"));
    assert(isAuthorizedStrictBlockBypass(bypass, QUrl(QStringLiteral("dalinira://newtab"))));
    assert(!isAuthorizedStrictBlockBypass(bypass, QUrl(QStringLiteral("https://attacker.example"))));

    // 3. User proceeds -> Temporary Bypass activated
    assert(!service.isStrictBypassActive(QStringLiteral("dangerous-phishing.com")));
    service.allowTemporaryStrictBypass(QStringLiteral("dangerous-phishing.com"), 15);
    assert(service.isStrictBypassActive(QStringLiteral("dangerous-phishing.com")));

    // 4. Re-evaluate same request -> bypass permits navigation, no loop.
    auto dec2 = service.evaluateRequest(QUrl(QStringLiteral("https://dangerous-phishing.com/login")),
                                        static_cast<int>(QWebEngineUrlRequestInfo::ResourceTypeMainFrame),
                                        QUrl(), 100);
    assert(dec2.action == DaliNiraBlockerAction::Allow);
    assert(dec2.reason == QStringLiteral("strict-temporary-bypass"));
    assert(dec2.redirectUrl.isEmpty());
    std::cout << "    ✓ Strict-block warning generation & temporary bypass lifecycle verified.\n";
  }
  {
    std::cout << "  - [8/17] Testing YouTube Core Assets vs Ad Stream Blocking in All Modes...\n";
    DaliNiraBlockerService service(dataDir);

    const QUrl ytInitiator(QStringLiteral("https://www.youtube.com/"));
    const QList<DaliNiraBlockerMode> testModes = {DaliNiraBlockerMode::Basic, DaliNiraBlockerMode::Ideal, DaliNiraBlockerMode::Aggressive};

    for (DaliNiraBlockerMode mode : testModes) {
      service.settings()->setMode(mode);
      service.reloadRules();

      // 1. YouTube Thumbnails MUST be allowed
      auto decThumb = service.evaluateRequest(QUrl(QStringLiteral("https://i.ytimg.com/vi/abc1234/hqdefault.jpg")),
                                              static_cast<int>(QWebEngineUrlRequestInfo::ResourceTypeImage),
                                              ytInitiator, 10);
      assert(decThumb.action == DaliNiraBlockerAction::Allow);

      // 2. YouTube Avatars MUST be allowed
      auto decAvatar = service.evaluateRequest(QUrl(QStringLiteral("https://yt3.ggpht.com/a/default-user.jpg")),
                                               static_cast<int>(QWebEngineUrlRequestInfo::ResourceTypeImage),
                                               ytInitiator, 10);
      assert(decAvatar.action == DaliNiraBlockerAction::Allow);

      // 3. YouTube Player API & scripts MUST be allowed
      auto decPlayer = service.evaluateRequest(QUrl(QStringLiteral("https://youtubei.googleapis.com/youtubei/v1/player")),
                                               static_cast<int>(QWebEngineUrlRequestInfo::ResourceTypeXhr),
                                               ytInitiator, 10);
      assert(decPlayer.action == DaliNiraBlockerAction::Allow);

      // 4. Normal video playback media streams MUST be allowed (both GET and POST, current and future epoch timestamps)
      auto decMedia = service.evaluateRequest(QUrl(QStringLiteral("https://rr3---sn-4g5edn6e.googlevideo.com/videoplayback?expire=1724000000&id=abcdef")),
                                              static_cast<int>(QWebEngineUrlRequestInfo::ResourceTypeMedia),
                                              ytInitiator, 10);
      assert(decMedia.action == DaliNiraBlockerAction::Allow);

      auto decGetMedia = service.evaluateRequest(
          QUrl(QStringLiteral("https://rr3---sn-4g5edn6e.googlevideo.com/videoplayback?expire=1787358320&id=normal")),
          static_cast<int>(QWebEngineUrlRequestInfo::ResourceTypeXhr), ytInitiator, 10,
          QStringLiteral("get"));
      assert(decGetMedia.action == DaliNiraBlockerAction::Allow);

      auto decPostMedia = service.evaluateRequest(
          QUrl(QStringLiteral("https://rr3---sn-4g5edn6e.googlevideo.com/videoplayback?expire=1787358320&id=normal")),
          static_cast<int>(QWebEngineUrlRequestInfo::ResourceTypeXhr), ytInitiator, 10,
          QStringLiteral("post"));
      assert(decPostMedia.action == DaliNiraBlockerAction::Allow);

      // 5. YouTube Video AD streams with ctier=l or adformat or source=web_video_ads MUST be strictly blocked
      auto decCtierAd = service.evaluateRequest(
          QUrl(QStringLiteral("https://rr3---sn-4g5edn6e.googlevideo.com/videoplayback?expire=1787358320&ctier=l&sparams=ctier,expire")),
          static_cast<int>(QWebEngineUrlRequestInfo::ResourceTypeXhr),
          ytInitiator, 10);
      assert(decCtierAd.action == DaliNiraBlockerAction::Block);

      auto decAdMedia = service.evaluateRequest(QUrl(QStringLiteral("https://rr3---sn-4g5edn6e.googlevideo.com/videoplayback?expire=1724000000&adformat=1_2_3")),
                                                static_cast<int>(QWebEngineUrlRequestInfo::ResourceTypeMedia),
                                                ytInitiator, 10);
      assert(decAdMedia.action == DaliNiraBlockerAction::Block);

      auto decWebVideoAds = service.evaluateRequest(QUrl(QStringLiteral("https://rr3---sn-4g5edn6e.googlevideo.com/videoplayback?expire=1724000000&source=web_video_ads")),
                                                    static_cast<int>(QWebEngineUrlRequestInfo::ResourceTypeMedia),
                                                    ytInitiator, 10);
      assert(decWebVideoAds.action == DaliNiraBlockerAction::Block);

      // 5b. Fail-safe regression test: Video playback with ctier=l substring inside token/signature MUST NOT be blocked
      auto decSubstrPlayback = service.evaluateRequest(
          QUrl(QStringLiteral("https://rr3---sn-4g5edn6e.googlevideo.com/videoplayback?expire=1787358320&id=normal-ctier=l_test&source=youtube")),
          static_cast<int>(QWebEngineUrlRequestInfo::ResourceTypeMedia),
          ytInitiator, 10);
      assert(decSubstrPlayback.action == DaliNiraBlockerAction::Allow);

      // 6. YouTube doubleclick ads MUST be strictly blocked
      auto decDclk = service.evaluateRequest(QUrl(QStringLiteral("https://ad.doubleclick.net/pagead/ads?client=ca-pub-123")),
                                             static_cast<int>(QWebEngineUrlRequestInfo::ResourceTypeScript),
                                             ytInitiator, 10);
      assert(decDclk.action == DaliNiraBlockerAction::Block);

      // uBOL substitutes this tiny player-facing status resource instead of
      // abruptly blocking the script. It does not generate ad telemetry.
      auto decAdStatus = service.evaluateRequest(
          QUrl(QStringLiteral("https://static.doubleclick.net/instream/ad_status.js")),
          static_cast<int>(QWebEngineUrlRequestInfo::ResourceTypeScript), ytInitiator, 10);
      assert(decAdStatus.action == DaliNiraBlockerAction::Redirect);
      assert(decAdStatus.ruleId == 1000006);
      assert(decAdStatus.redirectUrl.startsWith(QStringLiteral("data:application/javascript;base64,")));

      // 7. YouTube pagead requests MUST be strictly blocked
      auto decPageAd = service.evaluateRequest(QUrl(QStringLiteral("https://www.youtube.com/pagead/parallel")),
                                               static_cast<int>(QWebEngineUrlRequestInfo::ResourceTypeXhr),
                                               ytInitiator, 10);
      assert(decPageAd.action == DaliNiraBlockerAction::Block);
    }
    service.settings()->setMode(DaliNiraBlockerMode::Ideal);
    service.settings()->setStrictBlock(true);
    const auto strictNormalMedia = service.evaluateRequest(
        QUrl(QStringLiteral("https://rr3---sn-4g5edn6e.googlevideo.com/videoplayback?expire=1787358405&id=normal")),
        static_cast<int>(QWebEngineUrlRequestInfo::ResourceTypeXhr), ytInitiator, 10,
        QStringLiteral("post"));
    assert(strictNormalMedia.action == DaliNiraBlockerAction::Allow);
    std::cout << "    ✓ YouTube thumbnail, avatar, player and video stream preserved while ad streams blocked across all 3 modes.\n";
  }

  // -------------------------------------------------------------
  // TEST 7: Settings Persistence and Default Handling Across Restarts
  // -------------------------------------------------------------
  {
    std::cout << "  - [9/17] Testing Settings Restart Persistence and Default 'Ideal' Mode...\n";
    QTemporaryDir restartDir;
    assert(restartDir.isValid());
    const QString iniPath = restartDir.path() + QStringLiteral("/adblock-settings.ini");

    // Fresh install: without ini entry -> must default to Ideal, AutoReload=true, StrictBlock=true, PopupBlock=true
    {
      DaliNiraBlockerSettings freshSettings(iniPath);
      assert(freshSettings.mode() == DaliNiraBlockerMode::Ideal);
      assert(freshSettings.autoReloadOnModeChange() == true);
      assert(freshSettings.showBlockedCountOnToolbar() == true);
      assert(freshSettings.strictBlock() == true);
      assert(freshSettings.popupBlock() == true);
      assert(freshSettings.developerMode() == false);
      assert(freshSettings.protectionEnabled());
    }

    // User explicitly sets Basic, strictBlock=false, popupBlock=false -> must persist
    {
      DaliNiraBlockerSettings settingsA(iniPath);
      settingsA.setMode(DaliNiraBlockerMode::Basic);
      settingsA.setStrictBlock(false);
      settingsA.setPopupBlock(false);
      settingsA.setAutoReloadOnModeChange(false);
      settingsA.setProtectionEnabled(false);
      assert(settingsA.mode() == DaliNiraBlockerMode::Basic);
      assert(settingsA.strictBlock() == false);
      assert(settingsA.popupBlock() == false);
      assert(settingsA.autoReloadOnModeChange() == false);
      assert(!settingsA.protectionEnabled());
    }

    // Restart app -> must still be Basic, strictBlock=false, popupBlock=false
    {
      DaliNiraBlockerSettings settingsB(iniPath);
      assert(settingsB.mode() == DaliNiraBlockerMode::Basic);
      assert(settingsB.strictBlock() == false);
      assert(settingsB.popupBlock() == false);
      assert(settingsB.autoReloadOnModeChange() == false);
      assert(!settingsB.protectionEnabled());

      // User resets to defaults
      settingsB.resetToDefaults();
      assert(settingsB.mode() == DaliNiraBlockerMode::Ideal);
      assert(settingsB.strictBlock() == true);
      assert(settingsB.popupBlock() == true);
      assert(settingsB.autoReloadOnModeChange() == true);
      assert(settingsB.protectionEnabled());
    }

    // Restart app again -> must be Ideal, strictBlock=true, popupBlock=true
    {
      DaliNiraBlockerSettings settingsC(iniPath);
      assert(settingsC.mode() == DaliNiraBlockerMode::Ideal);
      assert(settingsC.strictBlock() == true);
      assert(settingsC.popupBlock() == true);
      assert(settingsC.protectionEnabled());

      settingsC.setMode(DaliNiraBlockerMode::Aggressive);
      settingsC.setStrictBlock(true);
      settingsC.setPopupBlock(false);
      settingsC.setDeveloperMode(true);
      settingsC.setProtectionEnabled(false);
      settingsC.setCustomFilters({QStringLiteral("! kept comment"), QStringLiteral("||backup.test^")});
      SitePolicy backupPolicy;
      backupPolicy.whitelisted = true;
      settingsC.setSitePolicy(QStringLiteral("example.com"), backupPolicy);
      const QJsonObject backup = settingsC.exportBackupJson();
      assert(!backup.contains(QStringLiteral("statistics")));
      assert(backup.value(QStringLiteral("popupBlock")).toBool() == false);

      QJsonObject futureBackup = backup;
      futureBackup[QStringLiteral("version")] = 999;
      assert(!settingsC.importBackupJson(futureBackup));
      settingsC.resetToDefaults();
      assert(settingsC.importBackupJson(backup));
      assert(settingsC.mode() == DaliNiraBlockerMode::Aggressive);
      assert(settingsC.strictBlock());
      assert(settingsC.popupBlock() == false);
      assert(settingsC.developerMode());
      assert(!settingsC.protectionEnabled());
      assert(settingsC.customFilters().contains(QStringLiteral("! kept comment")));
      assert(settingsC.sitePolicy(QStringLiteral("deep.example.com")).whitelisted);
    }
    std::cout << "    ✓ Defaults, restart persistence, transactional backup/restore, and schema rejection verified.\n";
  }

  // -------------------------------------------------------------
  // TEST 8: Explicit empty ruleset selection must not fall back to defaults
  // -------------------------------------------------------------
  {
    std::cout << "  - [10/17] Testing Explicit Empty Ruleset Selection...\n";
    DaliNiraBlockerService service(dataDir);
    service.settings()->setEnabledRulesetIds({});
    assert(service.settings()->rulesetSelectionConfigured());
    service.reloadRules();
    const auto decision = service.evaluateRequest(
        QUrl(QStringLiteral("https://doubleclick.net/pagead/ads")),
        static_cast<int>(QWebEngineUrlRequestInfo::ResourceTypeScript),
        QUrl(QStringLiteral("https://example.com/")), 77);
    // Built-in targeted core rules remain active, but list-origin filtering
    // must be empty; this URL is deliberately not covered by core rules.
    assert(decision.action == DaliNiraBlockerAction::Allow);
    const auto scripts = service.createScriptingScriptsForHost(QStringLiteral("example.com"));
    assert(std::none_of(scripts.cbegin(), scripts.cend(), [](const QWebEngineScript &script) {
      return script.name().startsWith(QStringLiteral("dalinira-adblock-scriptlets")) ||
             script.name() == QStringLiteral("dalinira-adblock-procedural");
    }));
    std::cout << "    ✓ Explicit empty selection persists without hidden default scripts/rules.\n";
  }

  // -------------------------------------------------------------
  // TEST 9: DNR redirect resources and upgradeScheme action semantics
  // -------------------------------------------------------------
  {
    std::cout << "  - [11/17] Testing DNR Redirect Resources and Upgrade Scheme...\n";
    DaliNiraBlockerService service(dataDir);
    service.settings()->setEnabledRulesetIds({QStringLiteral("ublock-filters")});
    service.reloadRules();
    const auto redirected = service.evaluateRequest(
        QUrl(QStringLiteral("https://intellipopup.com/ads.js")),
        static_cast<int>(QWebEngineUrlRequestInfo::ResourceTypeScript),
        QUrl(QStringLiteral("https://example.com/")), 88);
    assert(redirected.action == DaliNiraBlockerAction::Redirect);
    assert(redirected.redirectUrl.startsWith(QStringLiteral("data:")));
    assert(redirected.redirectUrl.contains(QStringLiteral(";base64,")));

    DaliNiraBlockerEngine engine;
    FilterRule upgrade;
    upgrade.id = 444;
    upgrade.actionType = QStringLiteral("upgradeScheme");
    upgrade.urlFilter = QStringLiteral("||upgrade.example^");
    engine.loadRules({upgrade});
    const auto upgraded = engine.evaluate(QUrl(QStringLiteral("http://upgrade.example/path")),
                                          DaliNiraBlockerResourceType::Script, QStringLiteral("origin.example"),
                                          DaliNiraBlockerMode::Ideal, SitePolicy{});
    assert(upgraded.action == DaliNiraBlockerAction::Redirect);
    assert(upgraded.redirectUrl == QStringLiteral("https://upgrade.example/path"));

    FilterRule transform;
    transform.id = 445;
    transform.actionType = QStringLiteral("redirect");
    transform.urlFilter = QStringLiteral("||transform.example^");
    transform.redirectTransform = QJsonObject{
        {QStringLiteral("scheme"), QStringLiteral("https")},
        {QStringLiteral("host"), QStringLiteral("clean.example")},
        {QStringLiteral("queryTransform"), QJsonObject{
            {QStringLiteral("removeParams"), QJsonArray{QStringLiteral("track")}},
            {QStringLiteral("addOrReplaceParams"), QJsonArray{QJsonObject{
                {QStringLiteral("key"), QStringLiteral("safe")},
                {QStringLiteral("value"), QStringLiteral("1")}}}}}}};
    engine.loadRules({transform});
    const auto transformed = engine.evaluate(QUrl(QStringLiteral("http://transform.example/a?track=x&keep=y")),
                                              DaliNiraBlockerResourceType::Script, QStringLiteral("origin.example"),
                                              DaliNiraBlockerMode::Ideal, SitePolicy{});
    assert(transformed.action == DaliNiraBlockerAction::Redirect);
    assert(transformed.redirectUrl == QStringLiteral("https://clean.example/a?keep=y&safe=1"));
    std::cout << "    ✓ extensionPath resources and upgradeScheme are executed, not ignored.\n";
  }

  // -------------------------------------------------------------
  // TEST 10: Legacy mode registry + main/regex/strict realms
  // -------------------------------------------------------------
  {
    std::cout << "  - [12/17] Testing Legacy Ruleset Registry and Realm Selection...\n";
    DaliNiraBlockerListManager manager(dataDir);
    const QStringList basic = manager.resolveRulesetIds(DaliNiraBlockerMode::Basic, {}, false);
    const QStringList ideal = manager.resolveRulesetIds(DaliNiraBlockerMode::Ideal, {}, false);
    const QStringList aggressive = manager.resolveRulesetIds(DaliNiraBlockerMode::Aggressive, {}, false);
    assert(basic.contains(QStringLiteral("ublock-filters")));
    assert(!basic.contains(QStringLiteral("ublock-badware")));
    assert(ideal.contains(QStringLiteral("ublock-badware")));
    assert(ideal.contains(QStringLiteral("urlhaus-full")));
    assert(ideal.contains(QStringLiteral("tur-0")));
    assert(!ideal.contains(QStringLiteral("ublock-experimental")));
    assert(aggressive.contains(QStringLiteral("ublock-experimental")));

    const QList<FilterRule> normal = manager.loadRulesForModeAndSelection(
        DaliNiraBlockerMode::Ideal, {QStringLiteral("ublock-filters")}, true, false);
    assert(std::any_of(normal.begin(), normal.end(), [](const FilterRule &rule) {
      return rule.rulesetId == QLatin1String("ublock-filters") && !rule.regexFilter.isEmpty();
    }));
    assert(std::none_of(normal.begin(), normal.end(), [](const FilterRule &rule) {
      return rule.rulesetId.startsWith(QStringLiteral("strictblock-"));
    }));
    const QList<FilterRule> strict = manager.loadRulesForModeAndSelection(
        DaliNiraBlockerMode::Ideal, {QStringLiteral("ublock-filters")}, true, true);
    assert(std::any_of(strict.begin(), strict.end(), [](const FilterRule &rule) {
      return rule.rulesetId == QLatin1String("strictblock-ublock-filters") && rule.priority >= 80;
    }));
    std::cout << "    ✓ Basic/Ideal/Aggressive and main+regex+strictblock realms match the legacy registry.\n";
  }

  // -------------------------------------------------------------
  // TEST 11: Request method/resource mapping + custom syntax
  // -------------------------------------------------------------
  {
    std::cout << "  - [13/17] Testing DNR Request Methods and Custom Filter Options...\n";
    assert(resourceTypeFromWebEngine(19) == DaliNiraBlockerResourceType::MainFrame);
    assert(resourceTypeFromWebEngine(20) == DaliNiraBlockerResourceType::SubFrame);
    assert(resourceTypeFromWebEngine(254) == DaliNiraBlockerResourceType::WebSocket);

    DaliNiraBlockerEngine engine;
    FilterRule getOnly;
    getOnly.id = 991;
    getOnly.actionType = QStringLiteral("block");
    getOnly.urlFilter = QStringLiteral("||method.example^");
    getOnly.requestMethods = {QStringLiteral("get")};
    engine.loadRules({getOnly});
    assert(engine.evaluate(QUrl(QStringLiteral("https://method.example/a")), DaliNiraBlockerResourceType::Xhr,
                           QStringLiteral("origin.test"), DaliNiraBlockerMode::Ideal, SitePolicy{},
                           QStringLiteral("get")).action == DaliNiraBlockerAction::Block);
    assert(engine.evaluate(QUrl(QStringLiteral("https://method.example/a")), DaliNiraBlockerResourceType::Xhr,
                           QStringLiteral("origin.test"), DaliNiraBlockerMode::Ideal, SitePolicy{},
                           QStringLiteral("post")).action == DaliNiraBlockerAction::Allow);

    FilterRule genericPriorityBlock;
    genericPriorityBlock.id = 990;
    genericPriorityBlock.priority = 10;
    genericPriorityBlock.actionType = QStringLiteral("block");
    genericPriorityBlock.urlFilter = QStringLiteral("||priority.example^");
    genericPriorityBlock.rulesetId = QStringLiteral("easylist");
    FilterRule specificPriorityAllow;
    specificPriorityAllow.id = 989;
    specificPriorityAllow.priority = 30;
    specificPriorityAllow.actionType = QStringLiteral("allow");
    specificPriorityAllow.urlFilter = QStringLiteral("||priority.example^");
    specificPriorityAllow.initiatorDomains = {QStringLiteral("player.test")};
    specificPriorityAllow.rulesetId = QStringLiteral("ublock-filters");
    engine.loadRules({genericPriorityBlock, specificPriorityAllow});
    assert(engine.evaluate(QUrl(QStringLiteral("https://priority.example/control")),
                           DaliNiraBlockerResourceType::Xhr, QStringLiteral("player.test"),
                           DaliNiraBlockerMode::Ideal, SitePolicy{}).action == DaliNiraBlockerAction::Allow);
    assert(engine.evaluate(QUrl(QStringLiteral("https://priority.example/ad")),
                           DaliNiraBlockerResourceType::Xhr, QStringLiteral("other.test"),
                           DaliNiraBlockerMode::Ideal, SitePolicy{}).action == DaliNiraBlockerAction::Block);

    FilterRule adRule;
    adRule.id = 992;
    adRule.actionType = QStringLiteral("block");
    adRule.urlFilter = QStringLiteral("||ads.example^");
    adRule.rulesetId = QStringLiteral("easylist");
    FilterRule trackerRule;
    trackerRule.id = 993;
    trackerRule.actionType = QStringLiteral("block");
    trackerRule.urlFilter = QStringLiteral("||metrics.example^");
    trackerRule.rulesetId = QStringLiteral("easyprivacy");
    engine.loadRules({adRule, trackerRule});
    SitePolicy trackerOnly;
    trackerOnly.adBlocking = false;
    trackerOnly.trackerProtection = true;
    assert(engine.evaluate(QUrl(QStringLiteral("https://ads.example/a")), DaliNiraBlockerResourceType::Xhr,
                           QStringLiteral("origin.test"), DaliNiraBlockerMode::Ideal, trackerOnly).action ==
           DaliNiraBlockerAction::Allow);
    assert(engine.evaluate(QUrl(QStringLiteral("https://metrics.example/a")), DaliNiraBlockerResourceType::Xhr,
                           QStringLiteral("origin.test"), DaliNiraBlockerMode::Ideal, trackerOnly).action ==
           DaliNiraBlockerAction::Block);
    SitePolicy adsOnly;
    adsOnly.adBlocking = true;
    adsOnly.trackerProtection = false;
    assert(engine.evaluate(QUrl(QStringLiteral("https://ads.example/a")), DaliNiraBlockerResourceType::Xhr,
                           QStringLiteral("origin.test"), DaliNiraBlockerMode::Ideal, adsOnly).action ==
           DaliNiraBlockerAction::Block);
    assert(engine.evaluate(QUrl(QStringLiteral("https://metrics.example/a")), DaliNiraBlockerResourceType::Xhr,
                           QStringLiteral("origin.test"), DaliNiraBlockerMode::Ideal, adsOnly).action ==
           DaliNiraBlockerAction::Allow);

    const QString custom = QStringLiteral("||custom.example^$script,third-party,domain=allowed.test|~excluded.allowed.test");
    assert(DaliNiraBlockerEngine::validateCustomFilterLine(custom).isEmpty());
    assert(!DaliNiraBlockerEngine::validateCustomFilterLine(QStringLiteral("||x^$unsupported-option")).isEmpty());
    engine.applyCompiledPlan(DaliNiraBlockerEngine::compilePlan({}, {}, {custom}));
    assert(engine.evaluate(QUrl(QStringLiteral("https://custom.example/a.js")), DaliNiraBlockerResourceType::Script,
                           QStringLiteral("allowed.test"), DaliNiraBlockerMode::Ideal, SitePolicy{}).action == DaliNiraBlockerAction::Block);
    assert(engine.evaluate(QUrl(QStringLiteral("https://custom.example/a.js")), DaliNiraBlockerResourceType::Image,
                           QStringLiteral("allowed.test"), DaliNiraBlockerMode::Ideal, SitePolicy{}).action == DaliNiraBlockerAction::Allow);
    assert(engine.evaluate(QUrl(QStringLiteral("https://custom.example/a.js")), DaliNiraBlockerResourceType::Script,
                           QStringLiteral("excluded.allowed.test"), DaliNiraBlockerMode::Ideal, SitePolicy{}).action == DaliNiraBlockerAction::Allow);

    std::cout << "    ✓ HTTP method, navigation preload/WebSocket mapping, and custom options are enforced.\n";
  }

  // -------------------------------------------------------------
  // TEST 12: Cosmetic host lists/exceptions
  // -------------------------------------------------------------
  {
    std::cout << "  - [14/17] Testing Cosmetic Multi-host Scoping and Exceptions...\n";
    DaliNiraBlockerEngine engine;
    engine.addCustomFilterLines({
        QStringLiteral("example.com,sub.test##.sponsored"),
        QStringLiteral("deep.example.com#@#.sponsored"),
        QStringLiteral("example.com,~safe.example.com##.ad-only"),
        QStringLiteral("example.com#?#.promo:has-text(Sponsored):remove()"),
        QStringLiteral("deep.example.com#@?#.promo:has-text(Sponsored):remove()")
    });
    const QString parentCss = engine.cosmeticCssForHost(QStringLiteral("www.example.com"));
    assert(parentCss.contains(QStringLiteral(".sponsored")));
    assert(parentCss.contains(QStringLiteral(".ad-only")));
    const QString deepCss = engine.cosmeticCssForHost(QStringLiteral("deep.example.com"));
    assert(!deepCss.contains(QStringLiteral(".sponsored")));
    const QString safeCss = engine.cosmeticCssForHost(QStringLiteral("safe.example.com"));
    assert(!safeCss.contains(QStringLiteral(".ad-only")));
    const QString unrelatedCss = engine.cosmeticCssForHost(QStringLiteral("unrelated-example.com"));
    assert(!unrelatedCss.contains(QStringLiteral(".sponsored")));
    assert(DaliNiraBlockerEngine::validateCustomFilterLine(
        QStringLiteral("example.com#?#.promo:has-text(Sponsored):remove()")).isEmpty());
    assert(!DaliNiraBlockerEngine::validateCustomFilterLine(
        QStringLiteral("example.com#?#.promo:unknown-task(x)")).isEmpty());
    const QJsonArray parentProcedural = engine.customProceduralRulesForHost(QStringLiteral("www.example.com"));
    assert(parentProcedural.size() == 1);
    assert(parentProcedural.first().toObject().value(QStringLiteral("selector")).toString() == QLatin1String(".promo"));
    assert(parentProcedural.first().toObject().value(QStringLiteral("action")).toArray().first().toString() == QLatin1String("remove"));
    assert(engine.customProceduralRulesForHost(QStringLiteral("deep.example.com")).isEmpty());
    assert(engine.customProceduralRulesForHost(QStringLiteral("unrelated-example.com")).isEmpty());

    DaliNiraBlockerService proceduralService(dataDir);
    proceduralService.settings()->setCustomFilters({
        QStringLiteral("custom-procedural.test##article:has-text(/Sponsored/i):remove()")});
    bool injectedCustomProcedural = false;
    for (const QWebEngineScript &script : proceduralService.createScriptingScriptsForHost(
             QStringLiteral("custom-procedural.test"))) {
      if (script.name() == QLatin1String("dalinira-adblock-procedural")) {
        injectedCustomProcedural = script.sourceCode().contains(QStringLiteral("article")) &&
                                   script.sourceCode().contains(QStringLiteral("Sponsored"));
      }
    }
    assert(injectedCustomProcedural);
    std::cout << "    ✓ Parent/subdomain matching, unrelated hosts, and cosmetic exceptions are isolated.\n";
  }

  // -------------------------------------------------------------
  // TEST 13: Redirect counters are not reported as network blocks
  // -------------------------------------------------------------
  {
    std::cout << "  - [15/17] Testing Block/Allow/Redirect Counter Separation...\n";
    DaliNiraBlockerService service(dataDir);
    service.resetAllStats();
    FilterRule redirect;
    redirect.id = 992;
    redirect.actionType = QStringLiteral("redirect");
    redirect.urlFilter = QStringLiteral("||redirect-counter.example^");
    redirect.redirectUrl = QStringLiteral("data:application/json;base64,e30=");
    FilterRule block;
    block.id = 993;
    block.actionType = QStringLiteral("block");
    block.urlFilter = QStringLiteral("||persistent-block.example^");
    FilterRule whitelistProbe;
    whitelistProbe.id = 994;
    whitelistProbe.actionType = QStringLiteral("block");
    whitelistProbe.urlFilter = QStringLiteral("||whitelist-probe.example^");
    service.filterEngine()->loadRules({redirect, block, whitelistProbe});
    const auto decision = service.evaluateRequest(
        QUrl(QStringLiteral("https://redirect-counter.example/a")),
        static_cast<int>(QWebEngineUrlRequestInfo::ResourceTypeXhr),
        QUrl(QStringLiteral("https://origin.test")), 77);
    assert(decision.action == DaliNiraBlockerAction::Redirect);
    assert(service.statsForTab(77).blockedRequests == 0);
    assert(service.statsForTab(77).redirectedRequests == 1);
    assert(service.totalRedirectedCount() == 1);
    assert(service.evaluateRequest(
        QUrl(QStringLiteral("https://persistent-block.example/ad.js")),
        static_cast<int>(QWebEngineUrlRequestInfo::ResourceTypeScript),
        QUrl(QStringLiteral("https://origin.test")), 77).action == DaliNiraBlockerAction::Block);
    SitePolicy whitelisted;
    whitelisted.whitelisted = true;
    service.settings()->setSitePolicy(QStringLiteral("allowed.test"), whitelisted);
    assert(service.evaluateRequest(
        QUrl(QStringLiteral("https://whitelist-probe.example/ad.js")),
        static_cast<int>(QWebEngineUrlRequestInfo::ResourceTypeScript),
        QUrl(QStringLiteral("https://allowed.test")), 77).action == DaliNiraBlockerAction::Allow);
    assert(service.totalBlockedCount() == 1);
    assert(service.whitelistAllowedCount() == 1);
    assert(service.todayBlockedCount() == 1);
  }
  {
    DaliNiraBlockerService restarted(dataDir);
    assert(restarted.sessionBlockedCount() == 0);
    assert(restarted.totalBlockedCount() == 1);
    assert(restarted.totalRedirectedCount() == 1);
    assert(restarted.whitelistAllowedCount() == 1);
    assert(restarted.todayBlockedCount() == 1);
    restarted.resetAllStats();
    std::cout << "    ✓ Block/redirect counters are separate and aggregate statistics survive restart/reset.\n";
  }

  // -------------------------------------------------------------
  // TEST 14: Async update state and failed-update rollback
  // -------------------------------------------------------------
  {
    std::cout << "  - [16/17] Testing Async Filter Update and Atomic Failure Rollback...\n";
    DaliNiraBlockerService service(dataDir);
    service.settings()->setEnabledRulesetIds({QStringLiteral("ublock-filters")});
    bool firstFinished = false;
    bool firstSuccess = false;
    int lastProgress = -1;
    QEventLoop successLoop;
    const auto progressConnection = QObject::connect(
        &service, &DaliNiraBlockerService::filterUpdateProgress, &successLoop,
        [&](int completed, int, const QString &) { lastProgress = qMax(lastProgress, completed); });
    const auto successConnection = QObject::connect(
        &service, &DaliNiraBlockerService::filterUpdateFinished, &successLoop,
        [&](bool ok, const QString &) { firstFinished = true; firstSuccess = ok; successLoop.quit(); });
    service.updateFiltersAsync();
    assert(service.isUpdatingFilters());
    QTimer::singleShot(10000, &successLoop, &QEventLoop::quit);
    successLoop.exec();
    QObject::disconnect(progressConnection);
    QObject::disconnect(successConnection);
    assert(firstFinished && firstSuccess && lastProgress == 3);
    const int healthyRuleCount = service.filterEngine()->ruleCount();

    const QString brokenBase = dataDir + QStringLiteral("/adblock/rulesets");
    assert(QDir().mkpath(brokenBase + QStringLiteral("/main")));
    QFile details(brokenBase + QStringLiteral("/ruleset-details.json"));
    assert(details.open(QIODevice::WriteOnly));
    details.write("[]");
    details.close();
    QFile broken(brokenBase + QStringLiteral("/main/ublock-filters.json"));
    assert(broken.open(QIODevice::WriteOnly));
    broken.write("{not-valid-json");
    broken.close();

    bool finished = false;
    bool success = true;
    QEventLoop loop;
    QObject::connect(&service, &DaliNiraBlockerService::filterUpdateFinished, &loop,
                     [&](bool ok, const QString &) { finished = true; success = ok; loop.quit(); });
    service.updateFiltersAsync();
    assert(service.isUpdatingFilters());
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    loop.exec();
    assert(finished);
    assert(!success);
    assert(!service.isUpdatingFilters());
    assert(service.filterEngine()->ruleCount() == healthyRuleCount);
    std::cout << "    ✓ Busy state is immediate; invalid packages fail without replacing the healthy engine.\n";
  }

  // -------------------------------------------------------------
  // TEST 15: Generated specific-cosmetic index ordering/exceptions
  // -------------------------------------------------------------
  {
    std::cout << "  - [17/17] Testing Generated Specific-cosmetic Host Index...\n";
    QTemporaryDir generatedDir;
    assert(generatedDir.isValid());
    const QString base = generatedDir.path() + QStringLiteral("/adblock/rulesets");
    assert(QDir().mkpath(base + QStringLiteral("/main")));
    assert(QDir().mkpath(base + QStringLiteral("/scripting/specific")));
    QFile details(base + QStringLiteral("/ruleset-details.json"));
    assert(details.open(QIODevice::WriteOnly));
    details.write("[{\"id\":\"synthetic\",\"name\":\"Synthetic\",\"group\":\"default\",\"enabled\":true}]");
    details.close();
    QFile mainRules(base + QStringLiteral("/main/synthetic.json"));
    assert(mainRules.open(QIODevice::WriteOnly));
    mainRules.write("[]");
    mainRules.close();
    const QJsonObject generated{
        {QStringLiteral("selectors"), QJsonArray{QStringLiteral(".root-ad"), QStringLiteral(".sub-ad"), QStringLiteral(".deep-ad")}},
        {QStringLiteral("selectorLists"), QJsonArray{QStringLiteral("0"), QStringLiteral("1"), QStringLiteral("2,-1")}},
        {QStringLiteral("selectorListRefs"), QJsonArray{0, 1, 2}},
        // Required generated ordering: hostname length, then lexical.
        {QStringLiteral("hostnames"), QJsonArray{QStringLiteral("example.com"), QStringLiteral("sub.example.com"), QStringLiteral("deep.sub.example.com")}},
        {QStringLiteral("hasEntities"), false},
        {QStringLiteral("regexes"), QJsonArray{}}};
    QFile specific(base + QStringLiteral("/scripting/specific/synthetic.json"));
    assert(specific.open(QIODevice::WriteOnly));
    specific.write(QJsonDocument(generated).toJson(QJsonDocument::Compact));
    specific.close();

    DaliNiraBlockerListManager manager(generatedDir.path());
    const QString root = manager.loadSpecificCosmeticCssForHost(
        QStringLiteral("example.com"), {QStringLiteral("synthetic")}, true);
    const QString sub = manager.loadSpecificCosmeticCssForHost(
        QStringLiteral("sub.example.com"), {QStringLiteral("synthetic")}, true);
    const QString deep = manager.loadSpecificCosmeticCssForHost(
        QStringLiteral("deep.sub.example.com"), {QStringLiteral("synthetic")}, true);
    const QString unrelated = manager.loadSpecificCosmeticCssForHost(
        QStringLiteral("unrelated-example.com"), {QStringLiteral("synthetic")}, true);
    assert(root.contains(QStringLiteral(".root-ad")));
    assert(sub.contains(QStringLiteral(".root-ad")) && sub.contains(QStringLiteral(".sub-ad")));
    assert(!deep.contains(QStringLiteral(".root-ad")) && deep.contains(QStringLiteral(".sub-ad")) &&
           deep.contains(QStringLiteral(".deep-ad")));
    assert(unrelated.isEmpty());
    std::cout << "    ✓ Length+lexical lookup, subdomain accumulation, exception, and unrelated-host isolation pass.\n";
  }

  // -------------------------------------------------------------
  // TEST 18: Settings & Allowlist Migration Test
  // -------------------------------------------------------------
  {
    std::cout << "  - [18/18] Testing Legacy adblock/* to blocker/* Settings & Allowlist Migration...\n";
    QTemporaryDir migDir;
    assert(migDir.isValid());
    const QString iniPath = migDir.path() + QStringLiteral("/legacy-browser-settings.ini");

    // Populate with legacy adblock keys
    {
      QSettings legacy(iniPath, QSettings::IniFormat);
      legacy.setValue(QStringLiteral("adblock/mode"), QStringLiteral("aggressive"));
      legacy.setValue(QStringLiteral("adblock/strictBlock"), true);
      legacy.setValue(QStringLiteral("adblock/popupBlock"), true);
      legacy.setValue(QStringLiteral("adblock/showBlockedCount"), true);
      legacy.setValue(QStringLiteral("adblock/autoReload"), true);
      legacy.beginGroup(QStringLiteral("adblock/sites"));
      QJsonObject siteObj;
      siteObj[QStringLiteral("adBlocking")] = false;
      siteObj[QStringLiteral("trackerProtection")] = true;
      siteObj[QStringLiteral("whitelisted")] = true;
      legacy.setValue(QStringLiteral("whitelisted-site.com"), siteObj);
      legacy.endGroup();
      legacy.sync();
    }

    // Load with new DaliNiraBlockerSettings
    DaliNiraBlockerSettings settings(iniPath);
    assert(settings.mode() == DaliNiraBlockerMode::Aggressive);
    assert(settings.strictBlock() == true);
    assert(settings.popupBlock() == true);
    assert(settings.showBlockedCountOnToolbar() == true);
    assert(settings.autoReloadOnModeChange() == true);

    const SitePolicy pol = settings.sitePolicy(QStringLiteral("whitelisted-site.com"));
    assert(pol.whitelisted == true);
    assert(pol.adBlocking == false);

    // Modify a value and save
    settings.setMode(DaliNiraBlockerMode::Ideal);

    // Reopen with another instance to ensure persistence under blocker/* and adblock/*
    DaliNiraBlockerSettings reloaded(iniPath);
    assert(reloaded.mode() == DaliNiraBlockerMode::Ideal);
    assert(reloaded.sitePolicy(QStringLiteral("whitelisted-site.com")).whitelisted == true);
    std::cout << "    ✓ Legacy adblock/* settings and site allowlists successfully migrated, preserved, and synced.\n";
  }

  // -------------------------------------------------------------
  // WebKit cosmetic parity: same scripts, same policy, real hit accounting
  // -------------------------------------------------------------
  {
    std::cout << "  - [19] Testing engine-neutral cosmetic document scripts (WebKit parity)...\n";
    DaliNiraBlockerService service(dataDir);
    service.settings()->setProtectionEnabled(true);
    service.settings()->setCustomFilters({
        QStringLiteral("##.generic-ad-slot"),
        QStringLiteral("site-a.com##.site-a-banner"),
        QStringLiteral("site-a.com##article:has-text(/Sponsored/i):remove()")});
    service.reloadRules();

    const QStringList scripts = service.cosmeticDocumentScriptsForHost(QStringLiteral("site-a.com"));
    assert(scripts.size() == 2 && "cosmetic runtime then procedural rules");
    assert(scripts[0] == service.createCosmeticScriptForHost(QStringLiteral("site-a.com")).sourceCode());
    assert(scripts[0].contains(QStringLiteral(".generic-ad-slot")) && scripts[0].contains(QStringLiteral(".site-a-banner")));
    assert(scripts[1].contains(QStringLiteral("Sponsored")));
    assert(scripts[0].contains(QStringLiteral("daliniraBlocker")) && "runtime reports hits through the WebKit handler");
    assert(!service.cosmeticDocumentScriptsForHost(QStringLiteral("site-b.com")).join(QString()).contains(QStringLiteral(".site-a-banner")));

    // The removed WebKit path used only the engine's generic CSS. Site-specific
    // list CSS (e.g. search ads) exists only in the service output.
    const QString engineOnly = service.filterEngine()->cosmeticCssForHost(QStringLiteral("www.youtube.com"));
    const QString full = service.cosmeticCssForHost(QStringLiteral("www.youtube.com"));
    assert(full.contains(QStringLiteral("ytd-ad-slot-renderer")) && !engineOnly.contains(QStringLiteral("ytd-ad-slot-renderer")));
    assert(service.cosmeticDocumentScriptsForHost(QStringLiteral("www.youtube.com")).first().contains(QStringLiteral("ytd-ad-slot-renderer")));

    // Every site-level switch removes the scripts for that host only.
    SitePolicy noAds;
    noAds.adBlocking = false;
    service.settings()->setSitePolicy(QStringLiteral("site-a.com"), noAds);
    assert(service.cosmeticDocumentScriptsForHost(QStringLiteral("site-a.com")).isEmpty());
    SitePolicy allowlisted;
    allowlisted.whitelisted = true;
    service.settings()->setSitePolicy(QStringLiteral("site-a.com"), allowlisted);
    assert(service.cosmeticDocumentScriptsForHost(QStringLiteral("site-a.com")).isEmpty());
    SitePolicy paused;
    paused.temporaryDisabledUntil = QDateTime::currentMSecsSinceEpoch() + 60000;
    service.settings()->setSitePolicy(QStringLiteral("site-a.com"), paused);
    assert(service.cosmeticDocumentScriptsForHost(QStringLiteral("site-a.com")).isEmpty());
    assert(!service.cosmeticDocumentScriptsForHost(QStringLiteral("site-b.com")).isEmpty());
    service.settings()->setSitePolicy(QStringLiteral("site-a.com"), SitePolicy{});
    assert(service.cosmeticDocumentScriptsForHost(QStringLiteral("site-a.com")).size() == 2);
    service.settings()->setProtectionEnabled(false);
    assert(service.cosmeticDocumentScriptsForHost(QStringLiteral("site-a.com")).isEmpty());
    service.settings()->setProtectionEnabled(true);

    // Reported cosmetic hits (what WebKit's message handler forwards) update
    // per-tab and global statistics, and are dropped where the site is paused.
    const quint64 tabId = 0xC05;
    service.registerTab(tabId, QUrl(QStringLiteral("https://site-a.com/")));
    int statsSignals = 0;
    quint64 signalledTotal = 0;
    QObject::connect(&service, &DaliNiraBlockerService::tabStatsChanged,
                     [&](quint64 id, const TabBlockerStats &stats) {
      if (id == tabId) { ++statsSignals; signalledTotal = stats.totalBlocked(); }
    });
    const quint64 totalBefore = service.totalBlockedCount();
    service.reportBlockedEvent(tabId, DaliNiraBlockType::Cosmetic, 3, QStringLiteral("cosmetic"));
    service.reportBlockedEvent(tabId, DaliNiraBlockType::Cosmetic, 1, QStringLiteral("procedural"));
    QCoreApplication::processEvents();
    assert(service.statsForTab(tabId).blockedCosmetics == 4);
    assert(service.statsForTab(tabId).totalBlocked() == 4);
    assert(service.totalBlockedCount() == totalBefore + 4);
    assert(statsSignals == 2 && signalledTotal == 4);
    service.settings()->setSitePolicy(QStringLiteral("site-a.com"), paused);
    service.reportBlockedEvent(tabId, DaliNiraBlockType::Cosmetic, 5, QStringLiteral("cosmetic"));
    assert(service.statsForTab(tabId).blockedCosmetics == 4 && "no counts while the site is paused");
    service.settings()->setSitePolicy(QStringLiteral("site-a.com"), SitePolicy{});
    service.unregisterTab(tabId);
    std::cout << "    ✓ WebKit receives Chromium's full cosmetic scripts, obeys site policy, and counts real hits.\n";
  }

  // -------------------------------------------------------------
  // Network decisions: one shared verdict for Chromium and WebKit
  // -------------------------------------------------------------
  {
    std::cout << "  - [20] Testing shared network decisions (Chromium interceptor vs WebKit extension)...\n";
    // A clean profile: earlier sections install stub rulesets in theirs.
    QTemporaryDir networkDir;
    assert(networkDir.isValid());
    DaliNiraBlockerService service(networkDir.path());
    service.settings()->setProtectionEnabled(true);
    service.settings()->setMode(DaliNiraBlockerMode::Ideal);
    service.reloadRules();
    std::cout << "      bundled network rules loaded: " << service.filterEngine()->ruleCount() << "\n";
    assert(service.filterEngine()->ruleCount() > 10000);

    // Chromium describes a request with QWebEngineUrlRequestInfo's resource
    // type; WebKit's extension with the request's Sec-Fetch-Dest header.
    struct Case {
      const char *url;
      const char *firstParty;
      int webEngineType;
      const char *fetchDest;
      const char *method;
      int expect;  // 0 allow, 1 block, 2 redirect, -1 not asserted (reported only)
    };
    const Case cases[] = {
        // Ad and analytics scripts are replaced by uBO's neutered data: stubs
        // (a redirect), so pages that call into them do not break.
        {"https://pagead2.googlesyndication.com/pagead/js/adsbygoogle.js", "https://news.example.org/", 3, "script", "get", 2},
        {"https://securepubads.g.doubleclick.net/tag/js/gpt.js", "https://news.example.org/", 3, "script", "get", 2},
        {"https://www.google-analytics.com/analytics.js", "https://news.example.org/", 3, "script", "get", 2},
        {"https://www.googletagmanager.com/gtag/js?id=G-TEST", "https://news.example.org/", 3, "script", "get", 2},
        // Tracking pixels and beacons (EasyPrivacy).
        {"https://www.facebook.com/tr?id=123&ev=PageView&noscript=1", "https://news.example.org/", 4, "image", "get", -1},
        {"https://bat.bing.com/action/0?ti=1&Ver=2", "https://news.example.org/", 4, "image", "get", -1},
        {"https://static.hotjar.com/c/hotjar-1.js?sv=6", "https://news.example.org/", 3, "script", "get", -1},
        {"https://stats.g.doubleclick.net/g/collect?v=2", "https://news.example.org/", 13, "empty", "post", -1},
        // YouTube ad endpoints (DaliNira's YouTube rules).
        {"https://googleads.g.doubleclick.net/pagead/id", "https://www.youtube.com/watch?v=abc", 13, "empty", "get", 1},
        {"https://www.youtube.com/api/stats/ads?ver=2&ns=yt", "https://www.youtube.com/watch?v=abc", 13, "empty", "get", 1},
        {"https://www.youtube.com/pagead/adview?ai=x", "https://www.youtube.com/watch?v=abc", 4, "image", "get", 1},
        // Trackers the lists let through, covered by DaliNira's built-in
        // rules: the Adobe Launch loader gets a _satellite stub (a redirect),
        // the Optimizely snippet is blocked. A list's site-specific exception
        // (uBlock filters: rogers.com) still loads the loader there.
        {"https://assets.adobedtm.com/launch-EN0123abcd.min.js", "https://news.example.org/", 3, "script", "get", 2},
        {"https://assets.adobedtm.com/0a1b2c/3d4e/launch-5f6a.min.js", "https://news.example.org/", 3, "script", "get", 2},
        {"https://cdn.optimizely.com/js/1234567890.js", "https://news.example.org/", 3, "script", "get", 1},
        {"https://assets.adobedtm.com/launch-EN0123abcd.min.js", "https://www.rogers.com/", 3, "script", "get", 0},
        // Debouncing: a top-level navigation to a bounce redirector goes
        // straight to its destination (uBlock filters' bundled $urlskip data).
        {"https://www.google.com/url?q=https%3A%2F%2Fexample.org%2Fpage&sa=D", "https://www.google.com/url?q=https%3A%2F%2Fexample.org%2Fpage&sa=D", 0, "document", "get", 2},
        // Tracking parameters are stripped (a redirect to the clean URL).
        {"https://shop.example.org/item?id=7&utm_source=mail&fbclid=abc", "https://shop.example.org/", 0, "document", "get", 2},
        // Legitimate YouTube playback, Google, GitHub and login traffic.
        {"https://rr3---sn-4g5e6nsz.googlevideo.com/videoplayback?expire=1&itag=248&mime=video%2Fwebm&range=0-99999", "https://www.youtube.com/watch?v=abc", 13, "empty", "post", 0},
        {"https://rr3---sn-4g5e6nsz.googlevideo.com/videoplayback?expire=1&itag=251&mime=audio%2Fwebm", "https://www.youtube.com/watch?v=abc", 8, "video", "get", 0},
        {"https://www.youtube.com/youtubei/v1/player?prettyPrint=false", "https://www.youtube.com/watch?v=abc", 13, "empty", "post", 0},
        {"https://www.youtube.com/s/player/0123abcd/player_ias.vflset/en_US/base.js", "https://www.youtube.com/watch?v=abc", 3, "script", "get", 0},
        {"https://i.ytimg.com/vi/aqz-KE-bpKQ/hqdefault.jpg", "https://www.youtube.com/", 4, "image", "get", 0},
        {"https://www.youtube.com/watch?v=aqz-KE-bpKQ", "https://www.youtube.com/watch?v=aqz-KE-bpKQ", 0, "document", "get", 0},
        {"https://www.google.com/search?q=dalinira", "https://www.google.com/search?q=dalinira", 0, "document", "get", 0},
        {"https://github.com/login", "https://github.com/login", 0, "document", "get", 0},
        {"https://github.githubassets.com/assets/app-4f2a.js", "https://github.com/", 3, "script", "get", 0},
        {"https://accounts.google.com/v3/signin/identifier?flowName=GlifWebSignIn", "https://accounts.google.com/", 0, "document", "get", 0},
        {"https://objects.githubusercontent.com/github-production-release-asset/1/release.tar.gz", "https://github.com/", 0, "document", "get", 0},
    };
    const quint64 chromiumTab = 0xC401;
    const quint64 webkitTab = 0xC402;
    service.registerTab(chromiumTab, QUrl(QStringLiteral("https://news.example.org/")));
    service.registerTab(webkitTab, QUrl(QStringLiteral("https://news.example.org/")));
    int blocked = 0;
    QStringList mismatches;
    for (const Case &c : cases) {
      const QUrl url(QString::fromLatin1(c.url));
      const QUrl firstParty(QString::fromLatin1(c.firstParty));
      const NetworkRequestVerdict chromium = service.decideNetworkRequest(
          url, resourceTypeFromWebEngine(c.webEngineType), firstParty, chromiumTab, QString::fromLatin1(c.method));
      const NetworkRequestVerdict webkit = service.decideNetworkRequest(
          url, resourceTypeFromFetchDestination(QString::fromLatin1(c.fetchDest)), firstParty, webkitTab,
          QString::fromLatin1(c.method));
      std::cout << "      " << (chromium.kind == NetworkVerdictKind::Block ? "BLOCK   " :
                                chromium.kind == NetworkVerdictKind::Redirect ? "REDIRECT" : "allow   ")
                << " " << c.url << " (" << chromium.decision.rulesetId.toStdString() << ")"
                << (chromium.kind == NetworkVerdictKind::Redirect ? " -> " + chromium.redirectUrl.toString().left(90).toStdString() : std::string())
                << "\n";
      assert(resourceTypeFromWebEngine(c.webEngineType) ==
             resourceTypeFromFetchDestination(QString::fromLatin1(c.fetchDest)));
      assert(chromium.kind == webkit.kind && "both engines must reach the same verdict");
      assert(chromium.redirectUrl == webkit.redirectUrl);
      assert(chromium.decision.rulesetId == webkit.decision.rulesetId);
      if (c.expect >= 0 && static_cast<int>(chromium.kind) != c.expect) mismatches.append(QString::fromLatin1(c.url));
      if (chromium.kind == NetworkVerdictKind::Block) ++blocked;
    }
    for (const QString &m : mismatches) std::cout << "      UNEXPECTED verdict: " << m.toStdString() << "\n";
    assert(mismatches.isEmpty());
    {
      // What the redirects above are (a tab of its own keeps the counts below).
      const quint64 detailTab = 0xC403;
      const NetworkRequestVerdict launch = service.decideNetworkRequest(
          QUrl(QStringLiteral("https://assets.adobedtm.com/launch-EN0123abcd.min.js")),
          DaliNiraBlockerResourceType::Script, QUrl(QStringLiteral("https://news.example.org/")), detailTab,
          QStringLiteral("get"));
      assert(launch.decision.rulesetId == QLatin1String("dalinira-privacy"));
      assert(launch.redirectUrl.scheme() == QLatin1String("data"));
      const QUrl bounce(QStringLiteral("https://www.google.com/url?q=https%3A%2F%2Fexample.org%2Fpage&sa=D"));
      const NetworkRequestVerdict skipped = service.decideNetworkRequest(
          bounce, DaliNiraBlockerResourceType::MainFrame, bounce, detailTab, QStringLiteral("get"));
      assert(skipped.redirectUrl == QUrl(QStringLiteral("https://example.org/page")));
      assert(skipped.decision.rulesetId == QLatin1String("ublock-filters"));
      assert(service.filterEngine()->urlSkipRuleCount() > 200);
    }
    assert(blocked >= 5);
    // Counters are real and identical: only actual blocks were counted.
    const TabBlockerStats chromiumStats = service.statsForTab(chromiumTab);
    const TabBlockerStats webkitStats = service.statsForTab(webkitTab);
    std::cout << "      counted per engine: blocked=" << webkitStats.blockedRequests << " ads=" << webkitStats.blockedAds
              << " trackers=" << webkitStats.blockedTrackers << " redirected=" << webkitStats.redirectedRequests << "\n";
    assert(chromiumStats.blockedRequests == static_cast<quint64>(blocked));
    assert(webkitStats.blockedRequests == chromiumStats.blockedRequests);
    assert(webkitStats.blockedAds == chromiumStats.blockedAds && webkitStats.blockedTrackers == chromiumStats.blockedTrackers);
    assert(webkitStats.redirectedRequests == chromiumStats.redirectedRequests);
    assert(webkitStats.blockedTrackers > 0 && webkitStats.blockedAds > 0);

    // The stripped URL keeps its real parameters.
    const NetworkRequestVerdict stripped = service.decideNetworkRequest(
        QUrl(QStringLiteral("https://shop.example.org/item?id=7&utm_source=mail&fbclid=abc")),
        DaliNiraBlockerResourceType::MainFrame, QUrl(QStringLiteral("https://shop.example.org/")), webkitTab,
        QStringLiteral("get"));
    assert(stripped.redirectUrl == QUrl(QStringLiteral("https://shop.example.org/item?id=7")));

    // Compatibility: signed URLs and non-navigation requests are never
    // implicitly rewritten. Both engine adapters share this decision.
    const QUrl tracked(QStringLiteral("https://shop.example.org/check?utm_source=mail&payload=a%26b%3Dc&x=1+2&x=3"));
    for (const auto type : {DaliNiraBlockerResourceType::Script, DaliNiraBlockerResourceType::Xhr}) {
      assert(service.decideNetworkRequest(tracked, type, tracked, webkitTab, QStringLiteral("get")).kind == NetworkVerdictKind::Allow);
    }
    assert(service.decideNetworkRequest(tracked, DaliNiraBlockerResourceType::MainFrame, tracked, webkitTab,
                                        QStringLiteral("post")).kind == NetworkVerdictKind::Allow);
    for (const QString &key : {QStringLiteral("signature"), QStringLiteral("state"), QStringLiteral("token"), QStringLiteral("X-Amz-Signature")}) {
      QUrl signedUrl(tracked.toString(QUrl::FullyEncoded) + QLatin1Char('&') + key + QStringLiteral("=abc"));
      assert(service.decideNetworkRequest(signedUrl, DaliNiraBlockerResourceType::MainFrame, signedUrl, webkitTab,
                                          QStringLiteral("get")).kind == NetworkVerdictKind::Allow);
    }
    const auto cleaned = service.decideNetworkRequest(tracked, DaliNiraBlockerResourceType::MainFrame, tracked,
                                                      webkitTab, QStringLiteral("get"));
    assert(cleaned.redirectUrl.query(QUrl::FullyEncoded) == QStringLiteral("payload=a%26b%3Dc&x=1+2&x=3"));
    service.settings()->setProtectionEnabled(false);
    assert(service.decideNetworkRequest(tracked, DaliNiraBlockerResourceType::MainFrame, tracked, webkitTab,
                                        QStringLiteral("get")).kind == NetworkVerdictKind::Allow);
    service.settings()->setProtectionEnabled(true);
    SitePolicy compatibilityBypass;
    compatibilityBypass.whitelisted = true;
    service.settings()->setSitePolicy(QStringLiteral("shop.example.org"), compatibilityBypass);
    assert(service.decideNetworkRequest(tracked, DaliNiraBlockerResourceType::MainFrame, tracked, webkitTab,
                                        QStringLiteral("get")).kind == NetworkVerdictKind::Allow);
    service.settings()->removeSitePolicy(QStringLiteral("shop.example.org"));

    // Site switches apply to both engines: an allowlisted or paused site, or a
    // site with ad blocking off, loads its ads and nothing is counted.
    const QUrl ad(QStringLiteral("https://googleads.g.doubleclick.net/pagead/id"));
    const QUrl site(QStringLiteral("https://www.youtube.com/"));
    SitePolicy allowlisted;
    allowlisted.whitelisted = true;
    service.settings()->setSitePolicy(QStringLiteral("youtube.com"), allowlisted);
    assert(service.decideNetworkRequest(ad, DaliNiraBlockerResourceType::Script, site, webkitTab, QStringLiteral("get")).kind ==
           NetworkVerdictKind::Allow);
    SitePolicy paused;
    paused.temporaryDisabledUntil = QDateTime::currentMSecsSinceEpoch() + 60000;
    service.settings()->setSitePolicy(QStringLiteral("youtube.com"), paused);
    assert(service.decideNetworkRequest(ad, DaliNiraBlockerResourceType::Script, site, webkitTab, QStringLiteral("get")).kind ==
           NetworkVerdictKind::Allow);
    SitePolicy noAds;
    noAds.adBlocking = false;
    service.settings()->setSitePolicy(QStringLiteral("youtube.com"), noAds);
    assert(service.decideNetworkRequest(ad, DaliNiraBlockerResourceType::Script, site, webkitTab, QStringLiteral("get")).kind ==
           NetworkVerdictKind::Allow);
    SitePolicy httpsOnly;
    httpsOnly.upgradeHttps = true;
    service.settings()->setSitePolicy(QStringLiteral("news.example.org"), httpsOnly);
    const NetworkRequestVerdict upgraded = service.decideNetworkRequest(
        QUrl(QStringLiteral("http://news.example.org/page")), DaliNiraBlockerResourceType::MainFrame,
        QUrl(QStringLiteral("http://news.example.org/page")), webkitTab, QStringLiteral("get"));
    assert(upgraded.kind == NetworkVerdictKind::Redirect &&
           upgraded.redirectUrl == QUrl(QStringLiteral("https://news.example.org/page")));
    service.settings()->setSitePolicy(QStringLiteral("news.example.org"), SitePolicy{});
    service.settings()->setSitePolicy(QStringLiteral("youtube.com"), SitePolicy{});
    assert(service.statsForTab(webkitTab).blockedRequests == webkitStats.blockedRequests);
    service.settings()->setProtectionEnabled(false);
    assert(service.decideNetworkRequest(ad, DaliNiraBlockerResourceType::Script, site, webkitTab, QStringLiteral("get")).kind ==
           NetworkVerdictKind::Allow);
    service.settings()->setProtectionEnabled(true);
    assert(service.decideNetworkRequest(ad, DaliNiraBlockerResourceType::Script, site, webkitTab, QStringLiteral("get")).kind ==
           NetworkVerdictKind::Block);

    // Non-web schemes are never filtered.
    assert(service.decideNetworkRequest(QUrl(QStringLiteral("dalinira://newtab/")), DaliNiraBlockerResourceType::MainFrame,
                                        QUrl(), webkitTab, QStringLiteral("get")).kind == NetworkVerdictKind::Allow);

    // The off-thread gate reaches the service while it lives and answers
    // "allow" (counting nothing) once it has been closed.
    const auto gate = service.networkGate();
    assert(gate->decide(ad, DaliNiraBlockerResourceType::Script, site, webkitTab, QStringLiteral("get")).kind ==
           NetworkVerdictKind::Block);
    service.unregisterTab(chromiumTab);
    service.unregisterTab(webkitTab);
    std::shared_ptr<DaliNiraBlockerNetworkGate> survivingGate;
    {
      DaliNiraBlockerService shortLived(networkDir.path());
      survivingGate = shortLived.networkGate();
    }
    assert(survivingGate->decide(ad, DaliNiraBlockerResourceType::Script, site, webkitTab, QStringLiteral("get")).kind ==
           NetworkVerdictKind::Allow);
    std::cout << "    ✓ Chromium and WebKit reach identical verdicts and counts; streams, Google, GitHub and logins load.\n";
  }

  // -------------------------------------------------------------
  // Step 3: shared $popup and $generichide semantics
  // -------------------------------------------------------------
  {
    std::cout << "  - [21] Testing shared popup and generic-hide decisions...\n";
    QTemporaryDir step3Dir;
    assert(step3Dir.isValid());
    DaliNiraBlockerService service(step3Dir.path());
    service.settings()->setEnabledRulesetIds({});
    service.settings()->setCustomFilters({
        QStringLiteral("||ads.popup.test^$popup"),
        QStringLiteral("@@||login.ads.popup.test^$popup"),
        QStringLiteral("||scoped.popup.test^$popup,domain=opener.test"),
        QStringLiteral("@@||generic-off.test^$generichide"),
        QStringLiteral("##.generic-ad"),
        QStringLiteral("generic-off.test##.site-ad"),
        QStringLiteral("##article:has-text(/sponsor/i):remove()"),
        QStringLiteral("generic-off.test##section:has-text(/promo/i):remove()"),
        QStringLiteral("||network-still-blocked.test^")});
    service.reloadRules();

    auto popup = [&service](const QString &target, const QString &opener = QStringLiteral("https://opener.test/")) {
      return service.decidePopup(QUrl(target), QUrl(opener), 0x503);
    };
    assert(popup(QStringLiteral("https://ads.popup.test/offer")).action == DaliNiraBlockerAction::Block);
    assert(popup(QStringLiteral("https://login.ads.popup.test/oauth")).action == DaliNiraBlockerAction::Allow);
    assert(popup(QStringLiteral("https://scoped.popup.test/x")).action == DaliNiraBlockerAction::Block);
    assert(popup(QStringLiteral("https://scoped.popup.test/x"), QStringLiteral("https://elsewhere.test/" )).action ==
           DaliNiraBlockerAction::Allow);
    assert(popup(QStringLiteral("https://accounts.example/oauth")).action == DaliNiraBlockerAction::Allow);
    assert(service.decidePopup(QUrl(QStringLiteral("https://ads.popup.test/x")),
                               QUrl(QStringLiteral("dalinira://newtab/")), 0x503).action ==
           DaliNiraBlockerAction::Allow);
    // Popup-only rules never become ordinary request blocks, and the user's
    // broad popup preference is independent of list-rule decisions.
    assert(service.evaluateRequest(QUrl(QStringLiteral("https://ads.popup.test/x")),
                                   static_cast<int>(QWebEngineUrlRequestInfo::ResourceTypeMainFrame),
                                   QUrl(QStringLiteral("https://opener.test/")), 0x503).action ==
           DaliNiraBlockerAction::Allow);
    service.settings()->setPopupBlock(false);
    assert(popup(QStringLiteral("https://ads.popup.test/x")).action == DaliNiraBlockerAction::Block);

    const QString css = service.cosmeticCssForHost(QStringLiteral("generic-off.test"));
    assert(!css.contains(QStringLiteral(".generic-ad")));
    assert(css.contains(QStringLiteral(".site-ad")));
    const QJsonArray procedural = service.filterEngine()->customProceduralRulesForHost(
        QStringLiteral("generic-off.test"),
        !service.genericCosmeticsDisabled(QStringLiteral("generic-off.test"),
                                           QUrl(QStringLiteral("https://generic-off.test/"))));
    assert(procedural.size() == 1);
    assert(QJsonDocument(procedural).toJson().contains("promo"));
    assert(service.evaluateRequest(QUrl(QStringLiteral("https://network-still-blocked.test/a.js")),
                                   static_cast<int>(QWebEngineUrlRequestInfo::ResourceTypeScript),
                                   QUrl(QStringLiteral("https://generic-off.test/")), 0x504).action ==
           DaliNiraBlockerAction::Block);
    std::cout << "    ✓ Popup exceptions/domain scope and generic-vs-specific cosmetics stay separated.\n";
  }

  // -------------------------------------------------------------
  // Tracking parameters: removed in one pass, the same on both engines
  // -------------------------------------------------------------
  {
    std::cout << "  - [20b] Testing tracking-parameter removal in one pass...\n";
    QTemporaryDir paramDir;
    assert(paramDir.isValid());
    DaliNiraBlockerService service(paramDir.path());  // the default lists
    // Parameters only these rules remove (not on the built-in list).
    service.settings()->setCustomFilters({QStringLiteral("$removeparam=aaa_id"), QStringLiteral("$removeparam=bbb_id"),
                                          QStringLiteral("$removeparam=/^ccc_[a-z]+=/"),
                                          QStringLiteral("@@||kept.example^$removeparam=bbb_id")});
    service.reloadRules();
    const quint64 tab = 0x9a1;
    // One decision, as WebKit applies it; Chromium re-enters the decision with
    // the redirect target, which must then be final (no second round trip).
    auto clean = [&](const QString &url, DaliNiraBlockerResourceType type = DaliNiraBlockerResourceType::MainFrame) {
      const QUrl source(url);
      const QUrl firstParty = type == DaliNiraBlockerResourceType::MainFrame ? source
                                                                            : QUrl(QStringLiteral("https://news.example/"));
      const NetworkRequestVerdict viaChromium =
          service.decideNetworkRequest(source, type, firstParty, tab, QStringLiteral("get"));
      const NetworkRequestVerdict viaWebKit = service.decideNetworkRequest(
          source,
          type == DaliNiraBlockerResourceType::MainFrame ? resourceTypeFromFetchDestination(QStringLiteral("document"))
                                                         : resourceTypeFromFetchDestination(QStringLiteral("image")),
          firstParty, tab, QStringLiteral("get"));
      assert(viaChromium.kind == viaWebKit.kind && viaChromium.redirectUrl == viaWebKit.redirectUrl);
      if (viaChromium.kind != NetworkVerdictKind::Redirect) return source.toString(QUrl::FullyEncoded);
      const QUrl target = viaChromium.redirectUrl;
      const QUrl targetFirstParty = type == DaliNiraBlockerResourceType::MainFrame ? target : firstParty;
      assert(service.decideNetworkRequest(target, type, targetFirstParty, tab, QStringLiteral("get")).kind ==
             NetworkVerdictKind::Allow && "one pass: the cleaned URL needs no further redirect");
      return target.toString(QUrl::FullyEncoded);
    };
    const QStringList tracking = {
        QStringLiteral("__hsfp"), QStringLiteral("__hssc"), QStringLiteral("__hstc"), QStringLiteral("__s"),
        QStringLiteral("_bhlid"), QStringLiteral("_branch_match_id"), QStringLiteral("_branch_referrer"),
        QStringLiteral("_gl"), QStringLiteral("_hsenc"), QStringLiteral("_kx"), QStringLiteral("_openstat"),
        QStringLiteral("at_recipient_id"), QStringLiteral("at_recipient_list"), QStringLiteral("bbeml"),
        QStringLiteral("bsft_clkid"), QStringLiteral("bsft_uid"), QStringLiteral("dclid"), QStringLiteral("et_rid"),
        QStringLiteral("fb_action_ids"), QStringLiteral("fb_comment_id"), QStringLiteral("fbclid"),
        QStringLiteral("gbraid"), QStringLiteral("gclid"), QStringLiteral("guce_referrer"),
        QStringLiteral("guce_referrer_sig"), QStringLiteral("hsCtaTracking"), QStringLiteral("igshid"),
        QStringLiteral("irclickid"), QStringLiteral("mc_cid"), QStringLiteral("mc_eid"), QStringLiteral("mkt_tok"),
        QStringLiteral("ml_subscriber"), QStringLiteral("ml_subscriber_hash"), QStringLiteral("msclkid"),
        QStringLiteral("mtm_cid"), QStringLiteral("oft_c"), QStringLiteral("oft_ck"), QStringLiteral("oft_d"),
        QStringLiteral("oft_id"), QStringLiteral("oft_ids"), QStringLiteral("oft_k"), QStringLiteral("oft_lk"),
        QStringLiteral("oft_sk"), QStringLiteral("oly_anon_id"), QStringLiteral("oly_enc_id"), QStringLiteral("pk_cid"),
        QStringLiteral("rb_clickid"), QStringLiteral("s_cid"), QStringLiteral("sc_customer"), QStringLiteral("sc_eh"),
        QStringLiteral("sc_uid"), QStringLiteral("srsltid"), QStringLiteral("ss_email_id"), QStringLiteral("twclid"),
        QStringLiteral("unicorn_click_id"), QStringLiteral("vero_conv"), QStringLiteral("vero_id"),
        QStringLiteral("vgo_ee"), QStringLiteral("wbraid"), QStringLiteral("wickedid"), QStringLiteral("yclid"),
        QStringLiteral("ymclid"), QStringLiteral("ysclid"), QStringLiteral("utm_source"), QStringLiteral("utm_campaign"),
        QStringLiteral("aaa_id")};
    assert(tracking.size() == 66);
    // Legitimate parameters stay, in their original order and encoding.
    const QString legit = QStringLiteral("q=red%20shoes&page=2&id=7&ref=home&x=1+2&next=%2Fcart%3Fa%3Db");
    // Single parameter, at the start, middle and end of the query.
    for (const QString &name : tracking) {
      const QString value = name + QStringLiteral("=TRK1");
      assert(clean(QStringLiteral("https://shop.example/item?") + value + QLatin1Char('&') + legit) ==
             QStringLiteral("https://shop.example/item?") + legit);
      assert(clean(QStringLiteral("https://shop.example/item?q=red%20shoes&") + value +
                   QStringLiteral("&page=2&id=7&ref=home&x=1+2&next=%2Fcart%3Fa%3Db")) ==
             QStringLiteral("https://shop.example/item?") + legit);
      assert(clean(QStringLiteral("https://shop.example/item?") + legit + QLatin1Char('&') + value) ==
             QStringLiteral("https://shop.example/item?") + legit);
    }
    // All 66 at once, interleaved with legitimate parameters: one redirect.
    QStringList mixed;
    const QStringList legitItems = legit.split(QLatin1Char('&'));
    for (int i = 0; i < tracking.size(); ++i) {
      mixed.append(tracking.at(i) + QStringLiteral("=v") + QString::number(i));
      if (i % 11 == 0 && i / 11 < legitItems.size()) mixed.append(legitItems.at(i / 11));
    }
    assert(clean(QStringLiteral("https://shop.example/item?") + mixed.join(QLatin1Char('&')) + QStringLiteral("#frag")) ==
           QStringLiteral("https://shop.example/item?") + legit + QStringLiteral("#frag"));
    // Only tracking parameters: the query goes away entirely.
    assert(clean(QStringLiteral("https://shop.example/item?fbclid=1&gclid=2&igshid=3")) ==
           QLatin1String("https://shop.example/item"));
    // The lists' and custom $removeparam rules also all apply in one pass,
    // on subresources too; a $removeparam exception still keeps its name.
    assert(clean(QStringLiteral("https://cdn.example/photos/cat.jpg?aaa_id=1&k=2&bbb_id=3&ccc_foo=4"),
                 DaliNiraBlockerResourceType::Image) == QLatin1String("https://cdn.example/photos/cat.jpg?k=2"));
    assert(clean(QStringLiteral("https://kept.example/photos/cat.jpg?aaa_id=1&k=2&bbb_id=3&ccc_foo=4"),
                 DaliNiraBlockerResourceType::Image) == QLatin1String("https://kept.example/photos/cat.jpg?k=2&bbb_id=3"));
    // Compatibility: Marketo unsubscribe links keep their token, signed
    // URLs keep their whole query, legitimate-only URLs are not touched.
    assert(clean(QStringLiteral("https://mkt.example/unsubscribe?mkt_tok=abc&fbclid=1")) ==
           QLatin1String("https://mkt.example/unsubscribe?mkt_tok=abc"));
    assert(clean(QStringLiteral("https://mkt.example/offer?mkt_tok=abc")) == QLatin1String("https://mkt.example/offer"));
    assert(clean(QStringLiteral("https://files.example/a.zip?utm_source=1&X-Amz-Signature=abc")) ==
           QLatin1String("https://files.example/a.zip?utm_source=1&X-Amz-Signature=abc"));
    assert(clean(QStringLiteral("https://shop.example/item?") + legit) == QStringLiteral("https://shop.example/item?") + legit);
    // A debounced destination arrives already cleaned, in the same verdict.
    service.settings()->setCustomFilters({QStringLiteral("$removeparam=aaa_id"),
                                          QStringLiteral("||bounce.example^$urlskip=?to")});
    const QString destination = QString::fromLatin1(
        QUrl::toPercentEncoding(QStringLiteral("https://dest.example/a?fbclid=1&aaa_id=2&k=3&gclid=4")));
    assert(clean(QStringLiteral("https://bounce.example/r?to=") + destination) == QLatin1String("https://dest.example/a?k=3"));
    std::cout << "    ✓ 66 single-parameter and mixed multi-parameter URLs are cleaned in one identical redirect.\n";
  }

  // -------------------------------------------------------------
  // HTTPS by default, GPC and fingerprinting protection
  // -------------------------------------------------------------
  {
    std::cout << "  - [20c] Testing HTTPS by default and the privacy shield...\n";
    QTemporaryDir httpsDir;
    assert(httpsDir.isValid());
    DaliNiraBlockerService service(httpsDir.path());
    service.settings()->setEnabledRulesetIds({});
    service.settings()->setCustomFilters({QStringLiteral("||blocked-site.example^")});
    service.reloadRules();
    assert(service.settings()->httpsByDefault() && service.settings()->fingerprintProtection());
    const quint64 tab = 0x4e1;
    auto navigate = [&](const QString &url, DaliNiraBlockerResourceType type = DaliNiraBlockerResourceType::MainFrame,
                        const QString &method = QStringLiteral("get")) {
      // One decision per navigation (a second one for the same host would be
      // a redirect back to http://). Both engines describe a top-level
      // document as MainFrame, so they share it.
      const QUrl target(url);
      return service.decideNetworkRequest(target, type, target, tab, method);
    };
    assert(resourceTypeFromFetchDestination(QStringLiteral("document")) == DaliNiraBlockerResourceType::MainFrame);
    // A top-level http:// navigation to a public host goes to https://.
    NetworkRequestVerdict v = navigate(QStringLiteral("http://news.example/a?b=1#c"));
    assert(v.kind == NetworkVerdictKind::Redirect && v.redirectUrl == QUrl(QStringLiteral("https://news.example/a?b=1#c")));
    assert(v.decision.reason == QLatin1String("https-upgrade-by-default"));
    service.noteHttpsLoadSucceeded(v.redirectUrl);
    // Not: subresources, POST, other ports, local and IP hosts, blocked sites.
    assert(navigate(QStringLiteral("http://cdn.example/x.js"), DaliNiraBlockerResourceType::Script).kind == NetworkVerdictKind::Allow);
    assert(navigate(QStringLiteral("http://form.example/send"), DaliNiraBlockerResourceType::MainFrame, QStringLiteral("post")).kind ==
           NetworkVerdictKind::Allow);
    assert(navigate(QStringLiteral("http://port.example:8080/")).kind == NetworkVerdictKind::Allow);
    for (const char *local : {"http://localhost/", "http://192.168.1.1/", "http://printer.local/", "http://dev.test/"})
      assert(navigate(QString::fromLatin1(local)).kind == NetworkVerdictKind::Allow);
    assert(navigate(QStringLiteral("http://blocked-site.example/")).kind == NetworkVerdictKind::Block);
    // A failed HTTPS load falls back to http:// once; the host then stays on http://.
    v = navigate(QStringLiteral("http://legacy.example/page"));
    assert(v.kind == NetworkVerdictKind::Redirect);
    assert(service.httpsUpgradeFallback(v.redirectUrl) == QUrl(QStringLiteral("http://legacy.example/page")));
    assert(navigate(QStringLiteral("http://legacy.example/page")).kind == NetworkVerdictKind::Allow);
    assert(!service.httpsUpgradeFallback(QUrl(QStringLiteral("https://unrelated.example/"))).isValid());
    // A site that redirects the upgraded navigation straight back to http://
    // falls back at once instead of looping.
    assert(navigate(QStringLiteral("http://bounce-back.example/")).kind == NetworkVerdictKind::Redirect);
    assert(navigate(QStringLiteral("http://bounce-back.example/")).kind == NetworkVerdictKind::Allow);
    assert(navigate(QStringLiteral("http://bounce-back.example/")).kind == NetworkVerdictKind::Allow);
    // Site switches and the setting.
    SitePolicy allowlisted;
    allowlisted.whitelisted = true;
    service.settings()->setSitePolicy(QStringLiteral("old.example"), allowlisted);
    assert(navigate(QStringLiteral("http://old.example/")).kind == NetworkVerdictKind::Allow);
    service.settings()->setHttpsByDefault(false);
    assert(navigate(QStringLiteral("http://news2.example/")).kind == NetworkVerdictKind::Allow);
    service.settings()->setHttpsByDefault(true);

    // The privacy shield: GPC always; fingerprinting protection with the
    // session's key, exempting allowlisted sites, off with the switch.
    const QString shield = service.privacyShieldScript();
    assert(shield.contains(QStringLiteral("globalPrivacyControl")) && shield.contains(QStringLiteral("FARBLE=true")));
    for (const char *surface : {"Navigator.prototype,'language'", "queryLocalFonts", "Navigator.prototype,'gpu'",
                                "getShaderPrecisionFormat", "MediaQueryList.prototype", "maxTouchPoints",
                                "getHighEntropyValues", "powerEfficient:false"})
      assert(shield.contains(QString::fromLatin1(surface)));
    assert(!shield.contains(QStringLiteral("__DN_")) && shield.contains(QStringLiteral("\"old.example\"")));
    DaliNiraBlockerService otherSession(httpsDir.path());
    static const QRegularExpression kKey(QStringLiteral("KEY='([0-9a-f]+)'"));
    const QString key = kKey.match(shield).captured(1);
    assert(key.size() >= 16 && kKey.match(otherSession.privacyShieldScript()).captured(1) != key &&
           "each session (and profile) seeds its own values");
    service.settings()->setFingerprintProtection(false);
    assert(service.privacyShieldScript().contains(QStringLiteral("FARBLE=false")));
    service.settings()->setFingerprintProtection(true);
    std::cout << "    ✓ HTTPS by default upgrades navigations with one-time fallback; the shield follows site policy.\n";
  }

  // -------------------------------------------------------------
  // Debouncing: $urlskip steps and the shared navigation decision
  // -------------------------------------------------------------
  {
    std::cout << "  - [21b] Testing debouncing ($urlskip) steps and decisions...\n";
    // uBO's urlskip step semantics.
    auto skip = [](const char *url, const QStringList &steps, bool blocked = false) {
      return DaliNiraBlockerEngine::applyUrlSkipSteps(QString::fromLatin1(url), steps, blocked);
    };
    assert(skip("https://www.google.com/url?q=https%3A%2F%2Fexample.org%2Fa%3Fb%3D1&sa=D", {QStringLiteral("?q")}) ==
           QLatin1String("https://example.org/a?b=1"));
    assert(skip("https://r.test/?u=https://dest.test/a+b", {QStringLiteral("?u")}) ==
           QLatin1String("https://dest.test/a%20b"));
    assert(skip("https://r.test/?t=aHR0cHM6Ly9kZXN0LnRlc3Qv", {QStringLiteral("?t"), QStringLiteral("-base64")}) ==
           QLatin1String("https://dest.test/"));
    assert(skip("https://r.test/?t=aHR0cHM6Ly9kZXN0LnRlc3QvP3E9Pj4-", {QStringLiteral("?t"), QStringLiteral("-safebase64")}) ==
           QLatin1String("https://dest.test/?q=>>>"));
    assert(skip("https://r.test/out/https%3A%2F%2Fdest.test%2F",
                {QStringLiteral("/\\/out\\/(.+)$/"), QStringLiteral("-uricomponent")}) == QLatin1String("https://dest.test/"));
    assert(skip("https://r.test/go?to=dest.test/x", {QStringLiteral("?to"), QStringLiteral("+https")}) ==
           QLatin1String("https://dest.test/x"));
    assert(skip("https://r.test/?https%3A%2F%2Fdest.test%2F", {QStringLiteral("&1")}) == QLatin1String("https://dest.test/"));
    assert(skip("https://r.test/#https://dest.test/", {QStringLiteral("#")}) == QLatin1String("https://dest.test/"));
    // Nothing for: a missing parameter, a malformed escape, a non-web
    // destination, an unknown step, a blocked navigation without "-blocked".
    assert(skip("https://r.test/?x=1", {QStringLiteral("?u")}).isEmpty());
    for (const char *bad : {"https://r.test/out/%E0%A4%A", "https://r.test/out/%FF", "https://r.test/out/%E0%A4"})
      assert(skip(bad, {QStringLiteral("/\\/out\\/(.+)$/"), QStringLiteral("-uricomponent")}).isEmpty());
    assert(skip("https://r.test/?u=javascript:alert(1)", {QStringLiteral("?u")}).isEmpty());
    assert(skip("https://r.test/?u=/relative", {QStringLiteral("?u")}).isEmpty());
    assert(skip("https://r.test/?u=https://dest.test/", {QStringLiteral("?u"), QStringLiteral("-rot13")}).isEmpty());
    assert(skip("https://r.test/?u=https://dest.test/", {QStringLiteral("?u")}, true).isEmpty());
    assert(skip("https://r.test/?u=https://dest.test/", {QStringLiteral("-blocked"), QStringLiteral("?u")}, true) ==
           QLatin1String("https://dest.test/"));

    // $urlskip as filter text (custom filters, downloaded lists).
    for (const char *line : {"||r.test^$urlskip=?u", "||r.test^$urlskip=?t -base64", "||r.test^$urlskip=-blocked ?u",
                             "/out/$urlskip=/\\/out\\/(.+)/ -uricomponent"})
      assert(DaliNiraBlockerEngine::validateCustomFilterLine(QString::fromLatin1(line)).isEmpty());
    for (const char *line : {"||r.test^$urlskip=", "||r.test^$urlskip=bogus", "@@||r.test^$urlskip=?u",
                             "||r.test^$urlskip=?u,popup", "||r.test^$urlskip=?u,removeparam=x"})
      assert(!DaliNiraBlockerEngine::validateCustomFilterLine(QString::fromLatin1(line)).isEmpty());
    const auto compiled = DaliNiraBlockerEngine::compileTextFilterList(
        QStringLiteral("||r.test^$urlskip=?u\n||r.test^$urlskip=?u,badfilter\n||s.test^$urlskip=?u"), QStringLiteral("t"),
        {nullptr, nullptr});
    assert(compiled.urlSkipRules == 2 && compiled.networkRules == 0);

    QTemporaryDir debounceDir;
    assert(debounceDir.isValid());
    DaliNiraBlockerService service(debounceDir.path());
    service.settings()->setEnabledRulesetIds({});
    service.settings()->setCustomFilters({
        QStringLiteral("||bounce.test^$urlskip=?to"),
        QStringLiteral("||chain-a.test^$urlskip=?next"),
        QStringLiteral("||chain-b.test^$urlskip=?next"),
        QStringLiteral("||blocked-bounce.test^"),
        QStringLiteral("||blocked-bounce.test^$urlskip=?to"),
        QStringLiteral("||blocked-skip.test^"),
        QStringLiteral("||blocked-skip.test^$urlskip=-blocked ?to"),
        QStringLiteral("||blocked-dest.test^")});
    service.reloadRules();
    const quint64 tab = 0x5b1;
    // Chromium's interceptor and WebKit's extension describe the navigation
    // differently; both reach the same verdict.
    auto navigate = [&](const QString &url, DaliNiraBlockerResourceType type = DaliNiraBlockerResourceType::MainFrame,
                        const QString &method = QStringLiteral("get")) {
      const QUrl target(url);
      const NetworkRequestVerdict viaChromium = service.decideNetworkRequest(target, type, target, tab, method);
      const NetworkRequestVerdict viaWebKit = service.decideNetworkRequest(
          target, type == DaliNiraBlockerResourceType::MainFrame ? resourceTypeFromFetchDestination(QStringLiteral("document"))
                                                                 : type,
          target, tab, method);
      assert(viaChromium.kind == viaWebKit.kind && viaChromium.redirectUrl == viaWebKit.redirectUrl);
      return viaChromium;
    };
    const QString dest = QStringLiteral("https%3A%2F%2Fdest.test%2Fa%3Fb%3D1");
    NetworkRequestVerdict v = navigate(QStringLiteral("https://bounce.test/r?to=") + dest);
    assert(v.kind == NetworkVerdictKind::Redirect && v.redirectUrl == QUrl(QStringLiteral("https://dest.test/a?b=1")));
    assert(v.decision.reason == QLatin1String("urlskip-debounce") && v.decision.rulesetId == QLatin1String("custom-user"));
    // DaliNira's built-in entries: Facebook's and Instagram's link shims.
    v = navigate(QStringLiteral("https://l.facebook.com/l.php?u=") + dest + QStringLiteral("&h=AT0x"));
    assert(v.redirectUrl == QUrl(QStringLiteral("https://dest.test/a?b=1")) &&
           v.decision.rulesetId == QLatin1String("dalinira-debounce"));
    // A chain of redirectors is followed to its end.
    v = navigate(QStringLiteral("https://chain-a.test/?next=") +
                 QString::fromLatin1(QUrl::toPercentEncoding(QStringLiteral("https://chain-b.test/?next=") + dest)));
    assert(v.redirectUrl == QUrl(QStringLiteral("https://dest.test/a?b=1")));
    // Only top-level GET navigations.
    assert(navigate(QStringLiteral("https://bounce.test/r?to=") + dest, DaliNiraBlockerResourceType::SubFrame).kind ==
           NetworkVerdictKind::Allow);
    assert(navigate(QStringLiteral("https://bounce.test/r?to=") + dest, DaliNiraBlockerResourceType::MainFrame,
                    QStringLiteral("post")).kind == NetworkVerdictKind::Allow);
    // A blocked redirector stays blocked unless its rule says "-blocked".
    assert(navigate(QStringLiteral("https://blocked-bounce.test/r?to=") + dest).kind == NetworkVerdictKind::Block);
    v = navigate(QStringLiteral("https://blocked-skip.test/r?to=") + dest);
    assert(v.kind == NetworkVerdictKind::Redirect && v.redirectUrl.host() == QLatin1String("dest.test"));
    // No skipping to a destination the filters block, or to a non-web URL.
    assert(navigate(QStringLiteral("https://bounce.test/r?to=https%3A%2F%2Fblocked-dest.test%2F")).kind ==
           NetworkVerdictKind::Allow);
    assert(navigate(QStringLiteral("https://bounce.test/r?to=javascript%3Aalert(1)")).kind == NetworkVerdictKind::Allow);
    // Site switches: an allowlisted redirector, tracker protection off, and
    // protection off all keep the redirector.
    SitePolicy allowlisted;
    allowlisted.whitelisted = true;
    service.settings()->setSitePolicy(QStringLiteral("bounce.test"), allowlisted);
    assert(navigate(QStringLiteral("https://bounce.test/r?to=") + dest).kind == NetworkVerdictKind::Allow);
    SitePolicy noTrackers;
    noTrackers.trackerProtection = false;
    service.settings()->setSitePolicy(QStringLiteral("bounce.test"), noTrackers);
    assert(navigate(QStringLiteral("https://bounce.test/r?to=") + dest).kind == NetworkVerdictKind::Allow);
    service.settings()->removeSitePolicy(QStringLiteral("bounce.test"));
    service.settings()->setProtectionEnabled(false);
    assert(navigate(QStringLiteral("https://bounce.test/r?to=") + dest).kind == NetworkVerdictKind::Allow);
    service.settings()->setProtectionEnabled(true);
    assert(navigate(QStringLiteral("https://bounce.test/r?to=") + dest).kind == NetworkVerdictKind::Redirect);
    // Debounces are counted as redirected requests (two per navigate()).
    assert(service.statsForTab(tab).redirectedRequests >= 10);
    std::cout << "    ✓ urlskip steps follow uBO; both engines skip redirectors on top-level GET navigations only.\n";
  }

  }

  // -------------------------------------------------------------
  // Step 3: real Chromium picker runtime + shared persistence
  // -------------------------------------------------------------
  {
    std::cout << "  - [22] Testing element picker runtime, persistence, and dynamic CSS...\n";
    QTemporaryDir pickerDir;
    assert(pickerDir.isValid());
    DaliNiraBlockerService service(pickerDir.path());
    service.settings()->setEnabledRulesetIds({});
    service.settings()->setAutoReloadOnModeChange(false);

    QString error;
    assert(DaliNiraElementPickerSession::filterFor(
               QUrl(QStringLiteral("https://picker.test/page")), QStringLiteral("div.ad-slot"), &error) ==
           QStringLiteral("picker.test##div.ad-slot"));
    assert(DaliNiraElementPickerSession::filterFor(
               QUrl(QStringLiteral("https://picker.test/")), QStringLiteral(".x{color:red}"), &error).isEmpty());
    assert(DaliNiraElementPickerSession::filterFor(
               QUrl(QStringLiteral("file:///tmp/x")), QStringLiteral(".x"), &error).isEmpty());

    QWebEngineProfile profile;
    QWebEnginePage page(&profile);
    bool loaded = false;
    QObject::connect(&page, &QWebEnginePage::loadFinished, [&](bool ok) { loaded = ok; });
    page.setHtml(QStringLiteral(R"HTML(
      <!doctype html><button id="action">action</button>
      <div class="ad-slot">ad</div>
      <script>window.actionCount=0;document.querySelector('#action').addEventListener('click',()=>actionCount++);</script>
    )HTML"), QUrl(QStringLiteral("https://picker.test/page")));
    QElapsedTimer loadTimer;
    loadTimer.start();
    while (!loaded && loadTimer.elapsed() < 10000) {
      QEventLoop loop; QTimer::singleShot(20, &loop, &QEventLoop::quit); loop.exec();
    }
    assert(loaded);

    auto isolated = [&page](const QString &script, std::function<void(const QVariant &)> callback) {
      if (callback) page.runJavaScript(script, QWebEngineScript::ApplicationWorld, std::move(callback));
      else page.runJavaScript(script, QWebEngineScript::ApplicationWorld);
    };
    bool saved = false;
    QString savedFilter;
    DaliNiraElementPickerSession session(&service, QUrl(QStringLiteral("https://picker.test/page")), isolated);
    QObject::connect(&session, &DaliNiraElementPickerSession::filterSaved,
                     [&](const QString &filter) { saved = true; savedFilter = filter; });
    session.start();

    auto jsValue = [&page](const QString &script, QWebEngineScript::ScriptWorldId world) {
      QVariant result;
      bool done = false;
      page.runJavaScript(script, world, [&](const QVariant &value) { result = value; done = true; });
      QElapsedTimer timer; timer.start();
      while (!done && timer.elapsed() < 5000) {
        QEventLoop loop; QTimer::singleShot(10, &loop, &QEventLoop::quit); loop.exec();
      }
      assert(done);
      return result;
    };
    QElapsedTimer pickerTimer;
    pickerTimer.start();
    while (!jsValue(QStringLiteral("!!window.__daliniraPicker"), QWebEngineScript::ApplicationWorld).toBool() &&
           pickerTimer.elapsed() < 5000) {}
    const qint64 pickerActivationMs = pickerTimer.elapsed();
    assert(jsValue(QStringLiteral("window.__daliniraPicker.select(document.querySelector('dalinira-element-picker'))"),
                   QWebEngineScript::ApplicationWorld).toBool() == false);
    // Page actions are swallowed while picking.
    jsValue(QStringLiteral("document.querySelector('#action').click();actionCount"), QWebEngineScript::MainWorld);
    assert(jsValue(QStringLiteral("actionCount"), QWebEngineScript::MainWorld).toInt() == 0);
    assert(jsValue(QStringLiteral("window.__daliniraPicker.select(document.querySelector('.ad-slot'))"),
                   QWebEngineScript::ApplicationWorld).toBool());
    const QString state = jsValue(QStringLiteral("window.__daliniraPicker.state()"),
                                  QWebEngineScript::ApplicationWorld).toString();
    assert(state.contains(QStringLiteral("ad-slot")) && state.contains(QStringLiteral("previewActive\":true")));
    assert(jsValue(QStringLiteral("window.__daliniraPicker.confirm()"),
                   QWebEngineScript::ApplicationWorld).toBool());
    QElapsedTimer saveTimer; saveTimer.start();
    while (!saved && saveTimer.elapsed() < 5000) {
      QEventLoop loop; QTimer::singleShot(20, &loop, &QEventLoop::quit); loop.exec();
    }
    assert(saved && savedFilter == QStringLiteral("picker.test##div.ad-slot"));
    assert(service.settings()->customFilters().count(savedFilter) == 1);
    assert(jsValue(QStringLiteral("getComputedStyle(document.querySelector('.ad-slot')).display"),
                   QWebEngineScript::MainWorld).toString() == QStringLiteral("none"));
    jsValue(QStringLiteral("const x=document.createElement('div');x.className='ad-slot';x.id='dynamic-ad';document.body.append(x);"),
            QWebEngineScript::MainWorld);
    assert(jsValue(QStringLiteral("getComputedStyle(document.querySelector('#dynamic-ad')).display"),
                   QWebEngineScript::MainWorld).toString() == QStringLiteral("none"));
    session.cancel();

    DaliNiraBlockerService restarted(pickerDir.path());
    restarted.settings()->setEnabledRulesetIds({});
    restarted.reloadRules();
    assert(restarted.settings()->customFilters().contains(savedFilter));
    assert(restarted.cosmeticCssForHost(QStringLiteral("picker.test")).contains(QStringLiteral("div.ad-slot")));
    QStringList removed = restarted.settings()->customFilters();
    removed.removeAll(savedFilter);
    restarted.settings()->setCustomFilters(removed);
    assert(!restarted.cosmeticCssForHost(QStringLiteral("picker.test")).contains(QStringLiteral("div.ad-slot")));
    std::cout << "    ✓ Picker selects safely, suppresses page actions, applies dynamically, persists, and removes cleanly"
              << " (activation " << pickerActivationMs << " ms).\n";
  }

  std::cout << "\n============================================================\n";
  if (chromiumPickerOnly)
    std::cout << "  CHROMIUM PICKER RUNTIME PASSED!\n";
  else
    std::cout << "  ALL 22 DALINIRA BLOCKER RELEASE AUDIT ITEMS PASSED!\n";
  std::cout << "============================================================\n";
  return 0;
}
