#include <QApplication>
#include <QEventLoop>
#include <QSettings>
#include <QTimer>
#include <QTemporaryDir>
#include <QWebEnginePage>
#include <QWebEngineView>
#include <QWidget>
#include <cassert>

#include "tab_manager.h"
#include "tab_performance_manager.h"

int main(int argc, char *argv[]) {
  qputenv("QT_QPA_PLATFORM", QByteArray("offscreen"));
  QApplication app(argc, argv);

  QTemporaryDir settingsRoot;
  assert(settingsRoot.isValid());
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsRoot.path());
  app.setOrganizationName(QStringLiteral("DaliNiraTest"));
  app.setApplicationName(QStringLiteral("TabPerformanceManagerTest"));

  QSettings testSettings;
  testSettings.remove(QStringLiteral("performance/policyMode"));
  testSettings.remove(QStringLiteral("performance/discardEnabled"));
  testSettings.remove(QStringLiteral("performance/siteAllowlist"));

  using namespace dalinira;

  // Test 1: Monotonic time initialization and web tab registration
  {
    TabManager manager;
    auto *perf = manager.performanceManager();
    assert(perf != nullptr);

    QObject owner1;
    auto *view1 = new QWebEngineView();
    view1->setUrl(QUrl(QStringLiteral("https://example.com/test1")));

    const int64_t beforeMs = TabPerformanceManager::currentMonotonicMs();
    const auto id1 = manager.registerTab(view1, &owner1, false, QStringLiteral("Tab 1"));
    const int64_t afterMs = TabPerformanceManager::currentMonotonicMs();

    assert(!id1.isNull());
    assert(perf->hasMetadata(id1));
    assert(perf->trackedWebTabs().contains(id1));

    const TabPerformanceMetadata meta = perf->metadata(id1);
    assert(meta.tabId == id1);
    assert(meta.tabKind == TabManager::TabKind::Web);
    assert(meta.creationMonotonicMs >= beforeMs && meta.creationMonotonicMs <= afterMs);
    assert(meta.lastActivationMonotonicMs >= beforeMs);
    assert(meta.lastVisibleMonotonicMs >= beforeMs);
    assert(meta.lastAudibleChangeMonotonicMs >= beforeMs);
    assert(meta.elapsedSinceCreationMs(afterMs + 100) >= 100);

    delete view1;
  }

  // Test 2: Internal tabs are excluded from WebEngine tracking and inherently protected
  {
    TabManager manager;
    auto *perf = manager.performanceManager();
    QObject owner;
    auto *widget = new QWidget();

    const auto internalId = manager.registerInternalTab(widget, &owner, QStringLiteral("Settings"),
                                                        QStringLiteral("settings"), {true, true, false, false});
    assert(!internalId.isNull());
    assert(!perf->hasMetadata(internalId));
    assert(!perf->trackedWebTabs().contains(internalId));
    assert(perf->isTabProtected(internalId)); // Native tabs are inherently protected
    assert(perf->protectedReasons(internalId).testFlag(ProtectedReason::Visible));

    delete widget;
  }

  // Test 3: Tab activation updates visibility and monotonic timestamps
  {
    TabManager manager;
    auto *perf = manager.performanceManager();
    QObject owner;

    auto *view1 = new QWebEngineView();
    auto *view2 = new QWebEngineView();
    view1->setUrl(QUrl(QStringLiteral("https://example.com/tab1")));
    view2->setUrl(QUrl(QStringLiteral("https://example.com/tab2")));

    const auto id1 = manager.registerTab(view1, &owner, false, QStringLiteral("Tab 1"));
    const auto id2 = manager.registerTab(view2, &owner, false, QStringLiteral("Tab 2"));

    // Activate tab 1
    manager.activate(id1);
    const auto meta1AfterAct = perf->metadata(id1);
    assert(meta1AfterAct.visible);
    assert(perf->isTabProtected(id1));
    assert(perf->protectedReasons(id1).testFlag(ProtectedReason::Visible));

    const int64_t act1Time = meta1AfterAct.lastActivationMonotonicMs;

    // Activate tab 2: tab 1 should become hidden and update lastVisibleMonotonicMs
    manager.activate(id2);
    const auto meta1AfterSwitch = perf->metadata(id1);
    const auto meta2AfterSwitch = perf->metadata(id2);

    assert(!meta1AfterSwitch.visible);
    assert(!perf->protectedReasons(id1).testFlag(ProtectedReason::Visible));
    assert(meta1AfterSwitch.lastVisibleMonotonicMs >= act1Time);

    assert(meta2AfterSwitch.visible);
    assert(perf->protectedReasons(id2).testFlag(ProtectedReason::Visible));
    assert(meta2AfterSwitch.lastActivationMonotonicMs >= act1Time);

    delete view1;
    delete view2;
  }

  // Test 4: Recently audible signal integration
  {
    TabManager manager;
    auto *perf = manager.performanceManager();
    QObject owner;
    auto *view = new QWebEngineView();
    view->setUrl(QUrl(QStringLiteral("https://example.com/audio")));
    const auto id = manager.registerTab(view, &owner, false, QStringLiteral("Audio Tab"));

    int audibleSignalCount = 0;
    bool recordedAudible = false;
    QObject::connect(perf, &TabPerformanceManager::tabRecentlyAudibleChanged,
                     [&](TabManager::TabId tabId, bool audible) {
                       if (tabId == id) {
                         ++audibleSignalCount;
                         recordedAudible = audible;
                       }
                     });

    // Emit recentlyAudibleChanged directly on page
    emit view->page()->recentlyAudibleChanged(true);

    assert(audibleSignalCount == 1);
    assert(recordedAudible == true);

    const auto meta = perf->metadata(id);
    assert(meta.recentlyAudible == true);
    assert(perf->isTabProtected(id));
    assert(perf->protectedReasons(id).testFlag(ProtectedReason::RecentlyAudible));

    // Turn audio off
    emit view->page()->recentlyAudibleChanged(false);
    assert(perf->metadata(id).recentlyAudible == false);
    assert(!perf->protectedReasons(id).testFlag(ProtectedReason::RecentlyAudible));

    delete view;
  }

  // Test 5: Recommended state signal integration
  {
    TabManager manager;
    auto *perf = manager.performanceManager();
    QObject owner;
    auto *view = new QWebEngineView();
    view->setUrl(QUrl(QStringLiteral("https://example.com/rec")));
    const auto id = manager.registerTab(view, &owner, false, QStringLiteral("Rec State Tab"));

    int recSignalCount = 0;
    dalinira::TabLifecycleState recordedState = dalinira::TabLifecycleState::Active;
    QObject::connect(perf, &TabPerformanceManager::tabRecommendedStateChanged,
                     [&](TabManager::TabId tabId, dalinira::TabLifecycleState state) {
                       if (tabId == id) {
                         ++recSignalCount;
                         recordedState = state;
                       }
                     });

    emit view->page()->recommendedStateChanged(QWebEnginePage::LifecycleState::Active);
    assert(perf->metadata(id).recommendedState == dalinira::TabLifecycleState::Active);
    assert(perf->protectedReasons(id).testFlag(ProtectedReason::RecommendedActive));

    emit view->page()->recommendedStateChanged(QWebEnginePage::LifecycleState::Frozen);
    assert(perf->metadata(id).recommendedState == dalinira::TabLifecycleState::Frozen);
    assert(!perf->protectedReasons(id).testFlag(ProtectedReason::RecommendedActive));

    delete view;
  }

  // Test 6: isAggressiveStatePermitted safety enforcement
  {
    TabManager manager;
    auto *perf = manager.performanceManager();
    QObject owner;
    auto *view = new QWebEngineView();
    view->setUrl(QUrl(QStringLiteral("https://example.com/gate")));
    const auto id = manager.registerTab(view, &owner, false, QStringLiteral("Gate Tab"));

    // If recommendedState is Active: Frozen & Discarded are NOT permitted
    emit view->page()->recommendedStateChanged(QWebEnginePage::LifecycleState::Active);
    assert(perf->isAggressiveStatePermitted(id, dalinira::TabLifecycleState::Active) == true);
    assert(perf->isAggressiveStatePermitted(id, dalinira::TabLifecycleState::Frozen) == false);
    assert(perf->isAggressiveStatePermitted(id, dalinira::TabLifecycleState::Discarded) == false);

    // Make tab non-visible and recommendedState Frozen
    perf->setTabVisible(id, false);
    emit view->page()->recommendedStateChanged(QWebEnginePage::LifecycleState::Frozen);
    assert(perf->isAggressiveStatePermitted(id, dalinira::TabLifecycleState::Active) == true);
    assert(perf->isAggressiveStatePermitted(id, dalinira::TabLifecycleState::Frozen) == true);
    assert(perf->isAggressiveStatePermitted(id, dalinira::TabLifecycleState::Discarded) == false);

    // If recommendedState is Discarded
    emit view->page()->recommendedStateChanged(QWebEnginePage::LifecycleState::Discarded);
    assert(perf->isAggressiveStatePermitted(id, dalinira::TabLifecycleState::Active) == true);
    assert(perf->isAggressiveStatePermitted(id, dalinira::TabLifecycleState::Frozen) == true);
    assert(perf->isAggressiveStatePermitted(id, dalinira::TabLifecycleState::Discarded) == true);

    delete view;
  }

  // Test 7: Protected reason bitmask model and unsupported reason immunity
  {
    TabManager manager;
    auto *perf = manager.performanceManager();
    QObject owner;
    auto *view = new QWebEngineView();
    view->setUrl(QUrl(QStringLiteral("https://example.com/bitmask")));
    const auto id = manager.registerTab(view, &owner, false, QStringLiteral("Bitmask Tab"));

    perf->setTabVisible(id, false);
    emit view->page()->recommendedStateChanged(QWebEnginePage::LifecycleState::Frozen);

    // When all supported active signals are false, tab has no protected reasons
    assert(perf->protectedReasons(id) == ProtectedReason::None);
    assert(!perf->isTabProtected(id));

    // Unsupported reasons (WebRTC, Camera, Microphone, ScreenCapture, FormOrEditState)
    // are NOT falsely set
    assert(!perf->protectedReasons(id).testFlag(ProtectedReason::WebRTC));
    assert(!perf->protectedReasons(id).testFlag(ProtectedReason::Camera));
    assert(!perf->protectedReasons(id).testFlag(ProtectedReason::Microphone));
    assert(!perf->protectedReasons(id).testFlag(ProtectedReason::ScreenCapture));
    assert(!perf->protectedReasons(id).testFlag(ProtectedReason::FormOrEditState));
    assert(!perf->protectedReasons(id).testFlag(ProtectedReason::UserAllowlisted));

    // Pinned protection
    perf->setTabPinned(id, true);
    assert(perf->protectedReasons(id).testFlag(ProtectedReason::UserPinned));
    assert(perf->isTabProtected(id));
    perf->setTabPinned(id, false);

    // Media playback hook
    perf->setMediaPlaybackActive(id, true);
    assert(perf->protectedReasons(id).testFlag(ProtectedReason::MediaPlayback));
    assert(perf->isTabProtected(id));
    perf->setMediaPlaybackActive(id, false);

    // Browser download hook
    perf->setBrowserDownloadActive(id, true);
    assert(perf->protectedReasons(id).testFlag(ProtectedReason::BrowserDownload));
    assert(perf->isTabProtected(id));
    perf->setBrowserDownloadActive(id, false);

    delete view;
  }

  // Test 8: Tab detach/attach transfer preserves metadata and connections
  {
    TabManager manager;
    auto *perf = manager.performanceManager();
    QObject window1;
    QObject window2;

    auto *view = new QWebEngineView();
    view->setUrl(QUrl(QStringLiteral("https://example.com/moving")));
    const auto id = manager.registerTab(view, &window1, false, QStringLiteral("Moving Tab"));
    manager.activate(id);

    const auto metaBefore = perf->metadata(id);
    const int64_t creationBefore = metaBefore.creationMonotonicMs;
    assert(creationBefore > 0);

    // Transfer tab to window 2
    const bool transferred = manager.transfer(id, &window2, true);
    assert(transferred);

    assert(perf->hasMetadata(id));
    const auto metaAfter = perf->metadata(id);
    assert(metaAfter.creationMonotonicMs == creationBefore);
    assert(metaAfter.visible == true);

    // Verify page signals still route cleanly after transfer
    emit view->page()->recentlyAudibleChanged(true);
    assert(perf->metadata(id).recentlyAudible == true);
    assert(perf->isTabProtected(id));

    delete view;
  }

  // Test 9: Tab close unregisters cleanly without stale pointers
  {
    TabManager manager;
    auto *perf = manager.performanceManager();
    QObject owner;
    auto *view = new QWebEngineView();
    view->setUrl(QUrl(QStringLiteral("https://example.com/close")));
    const auto id = manager.registerTab(view, &owner, false, QStringLiteral("Close Tab"));

    assert(perf->hasMetadata(id));
    manager.remove(id);

    assert(!perf->hasMetadata(id));
    assert(!perf->trackedWebTabs().contains(id));
    assert(!perf->isTabProtected(id));

    delete view;
  }

  // Test 10: Deterministic background media playback test
  {
    TabManager manager;
    auto *perf = manager.performanceManager();
    QObject owner;

    auto *mediaView = new QWebEngineView();
    auto *otherView = new QWebEngineView();
    mediaView->setUrl(QUrl(QStringLiteral("https://music.youtube.com/watch?v=123")));
    otherView->setUrl(QUrl(QStringLiteral("https://example.com/other")));

    const auto mediaId = manager.registerTab(mediaView, &owner, false, QStringLiteral("YouTube / Media Tab"));
    const auto otherId = manager.registerTab(otherView, &owner, false, QStringLiteral("Other Tab"));

    manager.activate(mediaId);
    emit mediaView->page()->recentlyAudibleChanged(true);

    assert(perf->metadata(mediaId).visible == true);
    assert(perf->metadata(mediaId).recentlyAudible == true);
    assert(perf->isTabProtected(mediaId) == true);

    // Switch to other tab: media tab is now in background
    manager.activate(otherId);

    const auto mediaMetaBg = perf->metadata(mediaId);
    assert(mediaMetaBg.visible == false);
    assert(mediaMetaBg.recentlyAudible == true);
    // Background media tab MUST still be protected due to audible state
    assert(perf->isTabProtected(mediaId) == true);
    assert(perf->protectedReasons(mediaId).testFlag(ProtectedReason::RecentlyAudible));

    delete mediaView;
    delete otherView;
  }

  // Test 11: Phase 2B — Basic Freeze of Eligible Background Tab
  {
    TabManager manager;
    auto *perf = manager.performanceManager();
    QObject owner;

    auto *viewA = new QWebEngineView();
    auto *viewB = new QWebEngineView();
    viewA->setUrl(QUrl(QStringLiteral("https://example.com/page-a")));
    viewB->setUrl(QUrl(QStringLiteral("https://example.com/page-b")));

    const auto idA = manager.registerTab(viewA, &owner, false, QStringLiteral("Tab A"));
    const auto idB = manager.registerTab(viewB, &owner, false, QStringLiteral("Tab B"));

    // A loading page must never be discarded. Settle this fixture before
    // exercising the remaining eligibility gates.
    QEventLoop loadLoop;
    QTimer loadTimeout;
    loadTimeout.setSingleShot(true);
    QObject::connect(&loadTimeout, &QTimer::timeout, &loadLoop, &QEventLoop::quit);
    QObject::connect(viewB->page(), &QWebEnginePage::loadFinished, &loadLoop, &QEventLoop::quit);
    viewB->setHtml(QStringLiteral("<title>Tab B</title>"), QUrl(QStringLiteral("https://example.com/page-b")));
    loadTimeout.start(3000);
    loadLoop.exec();
    assert(!viewB->page()->isLoading());

    // Set delay to 0 ms for deterministic test
    perf->setBackgroundFreezeDelayMs(0);

    // Tab A is active, Tab B is background
    manager.activate(idA);
    assert(perf->metadata(idA).visible == true);
    assert(perf->metadata(idB).visible == false);

    // Permit freeze on Tab B by setting recommendedState to Frozen
    emit viewB->page()->recommendedStateChanged(QWebEnginePage::LifecycleState::Frozen);

    assert(perf->canFreeze(idB) == true);
    assert(perf->canFreeze(idA) == false); // Visible tab cannot be frozen

    bool frozenSignalEmitted = false;
    QObject::connect(perf, &TabPerformanceManager::tabFrozen, [&](TabManager::TabId tabId) {
      if (tabId == idB) frozenSignalEmitted = true;
    });

    const bool frozenSuccess = perf->freezeTab(idB);
    assert(frozenSuccess == true);
    assert(frozenSignalEmitted == true);
    assert(perf->isTabFrozen(idB) == true);
    assert(viewB->page()->lifecycleState() == QWebEnginePage::LifecycleState::Frozen);

    const auto metaB = perf->metadata(idB);
    assert(metaB.frozenByDaliNira == true);
    assert(metaB.lifecycleState == dalinira::TabLifecycleState::Frozen);
    assert(metaB.lastFreezeMonotonicMs > 0);

    delete viewA;
    delete viewB;
  }

  // Test 12: Phase 2B — Resume on Activation without Reload or Loss of State
  {
    TabManager manager;
    auto *perf = manager.performanceManager();
    QObject owner;

    auto *viewA = new QWebEngineView();
    auto *viewB = new QWebEngineView();
    const QUrl urlB(QStringLiteral("https://example.com/page-b-state"));
    viewA->setUrl(QUrl(QStringLiteral("https://example.com/page-a-state")));
    viewB->setUrl(urlB);

    const auto idA = manager.registerTab(viewA, &owner, false, QStringLiteral("Tab A"));
    const auto idB = manager.registerTab(viewB, &owner, false, QStringLiteral("Tab B"));

    perf->setBackgroundFreezeDelayMs(0);
    manager.activate(idA);
    emit viewB->page()->recommendedStateChanged(QWebEnginePage::LifecycleState::Frozen);

    perf->freezeTab(idB);
    assert(perf->isTabFrozen(idB) == true);

    bool resumedSignalEmitted = false;
    QObject::connect(perf, &TabPerformanceManager::tabResumed, [&](TabManager::TabId tabId) {
      if (tabId == idB) resumedSignalEmitted = true;
    });

    // User switches to Tab B -> activation must resume to Active automatically
    manager.activate(idB);

    assert(resumedSignalEmitted == true);
    assert(perf->isTabFrozen(idB) == false);
    assert(viewB->page()->lifecycleState() == QWebEnginePage::LifecycleState::Active);

    const auto metaB = perf->metadata(idB);
    assert(metaB.visible == true);
    assert(metaB.frozenByDaliNira == false);
    assert(metaB.lifecycleState == dalinira::TabLifecycleState::Active);
    assert(metaB.lastResumeMonotonicMs > 0);
    assert(viewB->url() == urlB);

    delete viewA;
    delete viewB;
  }

  // Test 13: Phase 2B — Audible Protection & Audio Wakes Up Frozen Tab
  {
    TabManager manager;
    auto *perf = manager.performanceManager();
    QObject owner;

    auto *viewA = new QWebEngineView();
    auto *viewMedia = new QWebEngineView();
    viewA->setUrl(QUrl(QStringLiteral("https://example.com/viewa")));
    viewMedia->setUrl(QUrl(QStringLiteral("https://example.com/media")));

    const auto idA = manager.registerTab(viewA, &owner, false, QStringLiteral("Tab A"));
    const auto idMedia = manager.registerTab(viewMedia, &owner, false, QStringLiteral("Media Tab"));

    perf->setBackgroundFreezeDelayMs(0);
    manager.activate(idA);

    // 1. If recentlyAudible is true, canFreeze must be false
    emit viewMedia->page()->recentlyAudibleChanged(true);
    emit viewMedia->page()->recommendedStateChanged(QWebEnginePage::LifecycleState::Frozen);

    assert(perf->canFreeze(idMedia) == false);
    assert(perf->freezeTab(idMedia) == false);

    // 2. If tab was frozen and starts producing audio, it resumes immediately
    emit viewMedia->page()->recentlyAudibleChanged(false);
    assert(perf->canFreeze(idMedia) == true);
    perf->freezeTab(idMedia);
    assert(perf->isTabFrozen(idMedia) == true);

    // Audio starts playing in background
    emit viewMedia->page()->recentlyAudibleChanged(true);
    assert(perf->isTabFrozen(idMedia) == false);
    assert(viewMedia->page()->lifecycleState() == QWebEnginePage::LifecycleState::Active);
    assert(perf->metadata(idMedia).recentlyAudible == true);

    delete viewA;
    delete viewMedia;
  }

  // Test 14: Phase 2B — Qt recommendedState == Active Absolute Boundary
  {
    TabManager manager;
    auto *perf = manager.performanceManager();
    QObject owner;

    auto *viewA = new QWebEngineView();
    auto *viewB = new QWebEngineView();
    viewA->setUrl(QUrl(QStringLiteral("https://example.com/viewa")));
    viewB->setUrl(QUrl(QStringLiteral("https://example.com/viewb")));

    const auto idA = manager.registerTab(viewA, &owner, false, QStringLiteral("Tab A"));
    const auto idB = manager.registerTab(viewB, &owner, false, QStringLiteral("Tab B"));

    perf->setBackgroundFreezeDelayMs(0);
    manager.activate(idA);

    // Recommended state is Active -> FREEZE FORBIDDEN
    emit viewB->page()->recommendedStateChanged(QWebEnginePage::LifecycleState::Active);
    assert(perf->canFreeze(idB) == false);
    assert(perf->freezeTab(idB) == false);

    // If recommendedState becomes Frozen -> freeze becomes possible
    emit viewB->page()->recommendedStateChanged(QWebEnginePage::LifecycleState::Frozen);
    assert(perf->canFreeze(idB) == true);
    perf->freezeTab(idB);
    assert(perf->isTabFrozen(idB) == true);

    // If Qt changes recommendedState back to Active -> auto-resumes
    emit viewB->page()->recommendedStateChanged(QWebEnginePage::LifecycleState::Active);
    assert(perf->isTabFrozen(idB) == false);
    assert(viewB->page()->lifecycleState() == QWebEnginePage::LifecycleState::Active);

    delete viewA;
    delete viewB;
  }

  // Test 15: Phase 2B — Visible Tab Protection
  {
    TabManager manager;
    auto *perf = manager.performanceManager();
    QObject owner;

    auto *view = new QWebEngineView();
    view->setUrl(QUrl(QStringLiteral("https://example.com/visible")));
    const auto id = manager.registerTab(view, &owner, false, QStringLiteral("Visible Tab"));

    perf->setBackgroundFreezeDelayMs(0);
    manager.activate(id);
    emit view->page()->recommendedStateChanged(QWebEnginePage::LifecycleState::Frozen);

    // Visible tab CANNOT be frozen under any circumstances
    assert(perf->metadata(id).visible == true);
    assert(perf->canFreeze(id) == false);
    assert(perf->freezeTab(id) == false);
    assert(view->page()->lifecycleState() == QWebEnginePage::LifecycleState::Active);

    delete view;
  }

  // Test 16: Phase 2B — Native Internal Tab Immunity
  {
    TabManager manager;
    auto *perf = manager.performanceManager();
    QObject owner;
    auto *widget = new QWidget();

    const auto internalId = manager.registerInternalTab(widget, &owner, QStringLiteral("Settings"),
                                                        QStringLiteral("settings"), {true, true, false, false});
    perf->setBackgroundFreezeDelayMs(0);

    assert(perf->canFreeze(internalId) == false);
    assert(perf->freezeTab(internalId) == false);
    assert(perf->isTabFrozen(internalId) == false);

    delete widget;
  }

  // Test 17: Phase 2B — Detach / Attach Transfer
  {
    TabManager manager;
    auto *perf = manager.performanceManager();
    QObject window1;
    QObject window2;

    auto *viewA = new QWebEngineView();
    auto *viewB = new QWebEngineView();
    viewA->setUrl(QUrl(QStringLiteral("https://example.com/viewa")));
    viewB->setUrl(QUrl(QStringLiteral("https://example.com/viewb")));

    const auto idA = manager.registerTab(viewA, &window1, false, QStringLiteral("Tab A"));
    const auto idB = manager.registerTab(viewB, &window1, false, QStringLiteral("Tab B"));

    perf->setBackgroundFreezeDelayMs(0);
    manager.activate(idA);
    emit viewB->page()->recommendedStateChanged(QWebEnginePage::LifecycleState::Frozen);

    perf->freezeTab(idB);
    assert(perf->isTabFrozen(idB) == true);

    // Transfer Tab B to Window 2 as active tab
    const bool transferred = manager.transfer(idB, &window2, true);
    assert(transferred == true);

    // Transferred active tab must resume to Active immediately
    assert(perf->isTabFrozen(idB) == false);
    assert(viewB->page()->lifecycleState() == QWebEnginePage::LifecycleState::Active);
    assert(perf->metadata(idB).visible == true);

    delete viewA;
    delete viewB;
  }

  // Test 18: Phase 2B — Tab Close Race Condition
  {
    TabManager manager;
    auto *perf = manager.performanceManager();
    QObject owner;

    auto *viewA = new QWebEngineView();
    auto *viewB = new QWebEngineView();
    viewA->setUrl(QUrl(QStringLiteral("https://example.com/viewa")));
    viewB->setUrl(QUrl(QStringLiteral("https://example.com/viewb")));

    const auto idA = manager.registerTab(viewA, &owner, false, QStringLiteral("Tab A"));
    const auto idB = manager.registerTab(viewB, &owner, false, QStringLiteral("Tab B"));

    perf->setBackgroundFreezeDelayMs(0);
    manager.activate(idA);
    emit viewB->page()->recommendedStateChanged(QWebEnginePage::LifecycleState::Frozen);

    perf->freezeTab(idB);
    assert(perf->isTabFrozen(idB) == true);

    // Close Tab B while frozen
    manager.remove(idB);
    assert(!perf->hasMetadata(idB));
    assert(!perf->isTabFrozen(idB));
    assert(perf->canFreeze(idB) == false);

    delete viewA;
    delete viewB;
  }

  // Test 19: Phase 2B — Signal Race (Pinning and Downloads Cancel Freeze)
  {
    TabManager manager;
    auto *perf = manager.performanceManager();
    QObject owner;

    auto *viewA = new QWebEngineView();
    auto *viewB = new QWebEngineView();
    viewA->setUrl(QUrl(QStringLiteral("https://example.com/viewa")));
    viewB->setUrl(QUrl(QStringLiteral("https://example.com/viewb")));

    const auto idA = manager.registerTab(viewA, &owner, false, QStringLiteral("Tab A"));
    const auto idB = manager.registerTab(viewB, &owner, false, QStringLiteral("Tab B"));

    perf->setBackgroundFreezeDelayMs(0);
    manager.activate(idA);
    emit viewB->page()->recommendedStateChanged(QWebEnginePage::LifecycleState::Frozen);

    // User pins tab B -> cannot be frozen
    perf->setTabPinned(idB, true);
    assert(perf->canFreeze(idB) == false);
    perf->setTabPinned(idB, false);
    assert(perf->canFreeze(idB) == true);

    // Browser download active -> cannot be frozen
    perf->setBrowserDownloadActive(idB, true);
    assert(perf->canFreeze(idB) == false);
    perf->setBrowserDownloadActive(idB, false);
    assert(perf->canFreeze(idB) == true);

    delete viewA;
    delete viewB;
  }

  // Test 20: Phase 2B — Idle Threshold & Grace Period Verification
  {
    TabManager manager;
    auto *perf = manager.performanceManager();
    QObject owner;

    auto *viewA = new QWebEngineView();
    auto *viewB = new QWebEngineView();
    viewA->setUrl(QUrl(QStringLiteral("https://example.com/viewa")));
    viewB->setUrl(QUrl(QStringLiteral("https://example.com/viewb")));

    const auto idA = manager.registerTab(viewA, &owner, false, QStringLiteral("Tab A"));
    const auto idB = manager.registerTab(viewB, &owner, false, QStringLiteral("Tab B"));

    // Set 5 minute delay (300,000 ms)
    perf->setBackgroundFreezeDelayMs(300000);
    manager.activate(idA);
    emit viewB->page()->recommendedStateChanged(QWebEnginePage::LifecycleState::Frozen);

    // Since Tab B was just deactivated, elapsed time < 300,000 ms -> cannot freeze yet
    assert(perf->canFreeze(idB) == false);
    assert(perf->freezeTab(idB) == false);

    delete viewA;
    delete viewB;
  }

  // Test 21: Phase 2C-1 — Allowlist Normalization, Matching & Protection
  {
    TabManager manager;
    auto *perf = manager.performanceManager();
    QObject owner;

    auto *view = new QWebEngineView();
    view->setUrl(QUrl(QStringLiteral("https://example.com/tab")));
    const auto id = manager.registerTab(view, &owner, false, QStringLiteral("Tab"));

    perf->setSiteAllowlist({QStringLiteral("https://youtube.com/"), QStringLiteral("*.github.com")});
    assert(perf->siteAllowlist().contains(QStringLiteral("youtube.com")));
    assert(perf->siteAllowlist().contains(QStringLiteral("github.com")));

    assert(perf->isUrlAllowlisted(QUrl(QStringLiteral("https://music.youtube.com/watch"))));
    assert(perf->isUrlAllowlisted(QUrl(QStringLiteral("https://gist.github.com"))));
    assert(!perf->isUrlAllowlisted(QUrl(QStringLiteral("https://evil-youtube.com"))));

    manager.updateUrl(id, QUrl(QStringLiteral("https://music.youtube.com/playlist")));
    assert(perf->isTabProtected(id));
    assert(perf->protectedReasons(id).testFlag(ProtectedReason::UserAllowlisted));
    assert(perf->metadata(id).allowlisted);

    perf->setSiteAllowlist({});
    delete view;
  }

  // Test 22: Phase 2C-1 — Form Dirty Latching & Anti-Spoof Protection
  {
    TabManager manager;
    auto *perf = manager.performanceManager();
    QObject owner;

    auto *view = new QWebEngineView();
    view->setUrl(QUrl(QStringLiteral("https://example.com/tab")));
    const auto id = manager.registerTab(view, &owner, false, QStringLiteral("Tab"));

    assert(!perf->isTabFormDirty(id));
    assert(!perf->protectedReasons(id).testFlag(ProtectedReason::FormOrEditState));

    perf->setTabFormDirty(id, true);
    assert(perf->isTabFormDirty(id));
    assert(perf->protectedReasons(id).testFlag(ProtectedReason::FormOrEditState));
    assert(perf->metadata(id).formDirty);
    assert(perf->isTabProtected(id));

    // Trusted navigation reset
    emit view->page()->loadStarted();
    assert(!perf->isTabFormDirty(id));
    assert(!perf->protectedReasons(id).testFlag(ProtectedReason::FormOrEditState));

    delete view;
  }

  // Test 23: Phase 2C-1 — Memory Pressure Monitor Simulation
  {
    TabManager manager;
    auto *perf = manager.performanceManager();
    auto *monitor = perf->memoryPressureMonitor();
    assert(monitor != nullptr);

    monitor->setSimulatedPressureLevel(MemoryPressureLevel::Moderate);
    assert(monitor->currentPressureLevel() == MemoryPressureLevel::Moderate);

    monitor->setSimulatedPressureLevel(MemoryPressureLevel::Critical);
    assert(monitor->currentPressureLevel() == MemoryPressureLevel::Critical);

    monitor->setSimulatedPressureLevel(std::nullopt);
  }

  // Test 24: Phase 2C-1 — canDiscard Eligibility Checks
  {
    TabManager manager;
    auto *perf = manager.performanceManager();
    QObject owner;

    auto *viewA = new QWebEngineView();
    auto *viewB = new QWebEngineView();
    viewA->setUrl(QUrl(QStringLiteral("https://example.com/viewa")));
    viewB->setUrl(QUrl(QStringLiteral("https://example.com/viewb")));

    const auto idA = manager.registerTab(viewA, &owner, false, QStringLiteral("Tab A"));
    const auto idB = manager.registerTab(viewB, &owner, false, QStringLiteral("Tab B"));

    QEventLoop loadLoop;
    QTimer loadTimeout;
    loadTimeout.setSingleShot(true);
    QObject::connect(&loadTimeout, &QTimer::timeout, &loadLoop, &QEventLoop::quit);
    QObject::connect(viewB->page(), &QWebEnginePage::loadFinished, &loadLoop, &QEventLoop::quit);
    viewB->setHtml(QStringLiteral("<title>Tab B</title>"), QUrl(QStringLiteral("https://example.com/viewb")));
    loadTimeout.start(3000);
    loadLoop.exec();
    assert(!viewB->page()->isLoading());

    perf->setBackgroundDiscardDelayMs(0);
    // A newly created invisible view may already be discarded by Qt. Make this
    // fixture active once, then place it in the background like a real tab.
    manager.activate(idB);
    viewB->page()->setLifecycleState(QWebEnginePage::LifecycleState::Active);
    QApplication::processEvents();
    manager.activate(idA);

    // Tab A is visible -> cannot discard
    assert(!perf->canDiscard(idA));

    // Tab B is background. If recommendedState is Active -> cannot discard
    if (perf->metadata(idB).recommendedState == dalinira::TabLifecycleState::Active) {
      assert(!perf->canDiscard(idB));
    }

    // Set recommendedState to Discarded
    emit viewB->page()->recommendedStateChanged(QWebEnginePage::LifecycleState::Discarded);

    // If memory pressure is Normal and policy requires Moderate/Critical -> cannot discard
    perf->memoryPressureMonitor()->setSimulatedPressureLevel(MemoryPressureLevel::Normal);
    perf->setDiscardPolicyRequirement(DiscardPolicyRequirement::IdleAndMemoryPressure);
    assert(!perf->canDiscard(idB));

    // If policy is IdleOnly -> eligible!
    perf->setDiscardPolicyRequirement(DiscardPolicyRequirement::IdleOnly);
    // Headless Qt can independently discard an invisible fixture. In that
    // case it is already in the desired terminal state, so only exercise
    // canDiscard's remaining gates when a renderer is still active.
    if (!perf->isTabDiscarded(idB)) {
      assert(perf->canDiscard(idB));

      // But if Tab B has dirty form -> NOT eligible
      perf->setTabFormDirty(idB, true);
      assert(!perf->canDiscard(idB));
      perf->setTabFormDirty(idB, false);
      assert(perf->canDiscard(idB));

      // Or if Tab B is allowlisted -> NOT eligible
      perf->setSiteAllowlist({QStringLiteral("allowlisted-site.com")});
      manager.updateUrl(idB, QUrl(QStringLiteral("https://allowlisted-site.com")));
      assert(!perf->canDiscard(idB));
      perf->setSiteAllowlist({});
    }

    delete viewA;
    delete viewB;
  }

  // Test 25: Phase 2C-2 — Discard & Restore Execution and Signals
  {
    TabManager manager;
    auto *perf = manager.performanceManager();
    QObject owner;

    auto *view = new QWebEngineView();
    view->setUrl(QUrl(QStringLiteral("https://example.com/tab")));
    const auto id = manager.registerTab(view, &owner, false, QStringLiteral("Tab"));

    perf->setBackgroundDiscardDelayMs(0);
    perf->setDiscardPolicyRequirement(DiscardPolicyRequirement::IdleOnly);
    emit view->page()->recommendedStateChanged(QWebEnginePage::LifecycleState::Discarded);

    bool discardedEmitted = false;
    bool restoredEmitted = false;
    QObject::connect(perf, &TabPerformanceManager::tabDiscarded, [&](TabManager::TabId tid) {
      if (tid == id) discardedEmitted = true;
    });
    QObject::connect(perf, &TabPerformanceManager::tabRestored, [&](TabManager::TabId tid) {
      if (tid == id) restoredEmitted = true;
    });

    assert(perf->canDiscard(id));
    assert(perf->discardTab(id));
    assert(perf->isTabDiscarded(id));
    assert(discardedEmitted);
    assert(perf->metadata(id).discardedByDaliNira);
    assert(perf->metadata(id).discardCount == 1);

    // Restore tab
    assert(perf->resumeTab(id));
    assert(!perf->isTabDiscarded(id));
    assert(restoredEmitted);
    assert(!perf->metadata(id).discardedByDaliNira);
    assert(perf->metadata(id).restoreCount == 1);

    delete view;
  }

  // Test 26: Phase 2C-2 — Discard Ordering (Frozen tabs prioritized over Active, oldest activation first)
  {
    TabManager manager;
    auto *perf = manager.performanceManager();
    QObject owner;

    auto *viewA = new QWebEngineView();
    auto *viewB = new QWebEngineView();
    auto *viewC = new QWebEngineView();
    viewA->setUrl(QUrl(QStringLiteral("https://example.com/viewa")));
    viewB->setUrl(QUrl(QStringLiteral("https://example.com/viewb")));
    viewC->setUrl(QUrl(QStringLiteral("https://example.com/viewc")));

    const auto idA = manager.registerTab(viewA, &owner, false, QStringLiteral("Tab A"));
    const auto idB = manager.registerTab(viewB, &owner, false, QStringLiteral("Tab B"));
    const auto idC = manager.registerTab(viewC, &owner, false, QStringLiteral("Tab C"));
    assert(!idA.isNull() && !idB.isNull() && !idC.isNull());

    perf->setBackgroundFreezeDelayMs(0);
    perf->setBackgroundDiscardDelayMs(0);
    perf->setDiscardPolicyRequirement(DiscardPolicyRequirement::IdleOnly);

    emit viewA->page()->recommendedStateChanged(QWebEnginePage::LifecycleState::Discarded);
    emit viewB->page()->recommendedStateChanged(QWebEnginePage::LifecycleState::Discarded);
    emit viewC->page()->recommendedStateChanged(QWebEnginePage::LifecycleState::Discarded);

    // Freeze Tab B only
    perf->freezeTab(idB);
    assert(perf->isTabFrozen(idB));
    assert(!perf->isTabFrozen(idA));

    // Tab B (Frozen) should have highest priority
    assert(perf->canDiscard(idB));

    delete viewA;
    delete viewB;
    delete viewC;
  }

  // Test 27: Phase 2C-2 — Discard Rate Limiting (Burst Max 2 per evaluation)
  {
    TabManager manager;
    auto *perf = manager.performanceManager();
    QObject owner;

    auto *view1 = new QWebEngineView();
    auto *view2 = new QWebEngineView();
    auto *view3 = new QWebEngineView();
    view1->setUrl(QUrl(QStringLiteral("https://example.com/view1")));
    view2->setUrl(QUrl(QStringLiteral("https://example.com/view2")));
    view3->setUrl(QUrl(QStringLiteral("https://example.com/view3")));

    const auto id1 = manager.registerTab(view1, &owner, false, QStringLiteral("Tab 1"));
    const auto id2 = manager.registerTab(view2, &owner, false, QStringLiteral("Tab 2"));
    const auto id3 = manager.registerTab(view3, &owner, false, QStringLiteral("Tab 3"));
    assert(!id1.isNull() && !id2.isNull() && !id3.isNull());

    perf->setBackgroundFreezeDelayMs(0);
    perf->setBackgroundDiscardDelayMs(0);
    perf->setDiscardPolicyRequirement(DiscardPolicyRequirement::IdleOnly);

    emit view1->page()->recommendedStateChanged(QWebEnginePage::LifecycleState::Discarded);
    emit view2->page()->recommendedStateChanged(QWebEnginePage::LifecycleState::Discarded);
    emit view3->page()->recommendedStateChanged(QWebEnginePage::LifecycleState::Discarded);

    assert(TabPerformanceManager::kMaxDiscardPerEvaluation == 2);

    delete view1;
    delete view2;
    delete view3;
  }

  // Test 28: Phase 2C-2 — Discard Kill-Switch Toggle
  {
    TabManager manager;
    auto *perf = manager.performanceManager();
    QObject owner;

    auto *view = new QWebEngineView();
    view->setUrl(QUrl(QStringLiteral("https://example.com/view")));
    const auto id = manager.registerTab(view, &owner, false, QStringLiteral("Tab"));

    perf->setBackgroundDiscardDelayMs(0);
    perf->setDiscardPolicyRequirement(DiscardPolicyRequirement::IdleOnly);
    emit view->page()->recommendedStateChanged(QWebEnginePage::LifecycleState::Discarded);

    assert(perf->isDiscardEnabled());
    assert(perf->canDiscard(id));

    // Toggle kill-switch OFF
    perf->setDiscardEnabled(false);
    assert(!perf->isDiscardEnabled());
    assert(!perf->canDiscard(id));
    assert(!perf->discardTab(id));

    // Toggle kill-switch ON
    perf->setDiscardEnabled(true);
    assert(perf->isDiscardEnabled());
    assert(perf->canDiscard(id));

    delete view;
  }

  // Test 29: Phase 2C-2 — Performance Policy Modes
  {
    TabManager manager;
    auto *perf = manager.performanceManager();

    // Default: Balanced. Light sleep at the timeout; full sleep only after
    // long inactivity (or memory pressure).
    assert(perf->policyMode() == PerformancePolicyMode::Balanced);
    assert(perf->backgroundFreezeDelayMs() == 60 * 1000);
    assert(perf->backgroundDiscardDelayMs() == 60 * 1000);
    assert(perf->discardPolicyRequirement() == DiscardPolicyRequirement::LongIdleOrMemoryPressure);
    assert(perf->fullSleepDelayMs() == TabPerformanceManager::kBalancedFullSleepMinMs);

    // Switch to MemorySaver (Aggressive): full sleep at the shorter timeout
    perf->setPolicyMode(PerformancePolicyMode::MemorySaver);
    assert(perf->policyMode() == PerformancePolicyMode::MemorySaver);
    assert(perf->backgroundFreezeDelayMs() == 30 * 1000);
    assert(perf->backgroundDiscardDelayMs() == 30 * 1000);
    assert(perf->fullSleepDelayMs() == 30 * 1000);

    // Switch to MaximumPerformance
    perf->setPolicyMode(PerformancePolicyMode::MaximumPerformance);
    assert(perf->policyMode() == PerformancePolicyMode::MaximumPerformance);
    assert(perf->backgroundFreezeDelayMs() == 2 * 60 * 1000);
    assert(perf->backgroundDiscardDelayMs() == 2 * 60 * 1000);

    // Revert to Balanced
    perf->setPolicyMode(PerformancePolicyMode::Balanced);
    assert(perf->policyMode() == PerformancePolicyMode::Balanced);
  }

  // Test 30: Phase 2C-2 — Automatic Restore on Tab Activation
  {
    TabManager manager;
    auto *perf = manager.performanceManager();
    QObject owner;

    auto *view1 = new QWebEngineView();
    auto *view2 = new QWebEngineView();
    view1->setUrl(QUrl(QStringLiteral("https://example.com/view1")));
    view2->setUrl(QUrl(QStringLiteral("https://example.com/view2")));

    const auto id1 = manager.registerTab(view1, &owner, false, QStringLiteral("Tab 1"));
    const auto id2 = manager.registerTab(view2, &owner, false, QStringLiteral("Tab 2"));
    assert(!id1.isNull() && !id2.isNull());

    perf->setBackgroundDiscardDelayMs(0);
    perf->setDiscardPolicyRequirement(DiscardPolicyRequirement::IdleOnly);
    emit view1->page()->recommendedStateChanged(QWebEnginePage::LifecycleState::Discarded);

    perf->discardTab(id1);
    assert(perf->isTabDiscarded(id1));

    // Activating Tab 1 automatically restores it to Active
    manager.activate(id1);
    assert(!perf->isTabDiscarded(id1));
    assert(perf->metadata(id1).visible);
    assert(perf->metadata(id1).lifecycleState == dalinira::TabLifecycleState::Active);

    delete view1;
    delete view2;
  }

  // Test 31: Phase 2C-2 — Discard / Close Race Safety
  {
    TabManager manager;
    auto *perf = manager.performanceManager();
    QObject owner;

    auto *view = new QWebEngineView();
    view->setUrl(QUrl(QStringLiteral("https://example.com/view")));
    const auto id = manager.registerTab(view, &owner, false, QStringLiteral("Tab"));

    perf->setBackgroundDiscardDelayMs(0);
    perf->setDiscardPolicyRequirement(DiscardPolicyRequirement::IdleOnly);
    emit view->page()->recommendedStateChanged(QWebEnginePage::LifecycleState::Discarded);

    // Close/remove tab
    manager.remove(id);
    assert(!perf->hasMetadata(id));
    assert(!perf->canDiscard(id));
    assert(!perf->discardTab(id));

    delete view;
  }

  // Test 32: Phase 2C-2 — Discard / Transfer Race Safety
  {
    TabManager manager;
    auto *perf = manager.performanceManager();
    QObject ownerA;
    QObject ownerB;

    auto *view = new QWebEngineView();
    view->setUrl(QUrl(QStringLiteral("https://example.com/view")));
    const auto id = manager.registerTab(view, &ownerA, false, QStringLiteral("Tab"));

    perf->setBackgroundDiscardDelayMs(0);
    perf->setDiscardPolicyRequirement(DiscardPolicyRequirement::IdleOnly);
    emit view->page()->recommendedStateChanged(QWebEnginePage::LifecycleState::Discarded);

    perf->discardTab(id);
    assert(perf->isTabDiscarded(id));

    // Transferring and making active in ownerB automatically restores it
    manager.transfer(id, &ownerB, false);
    assert(!perf->isTabDiscarded(id));
    assert(perf->metadata(id).visible);

    delete view;
  }

  // Test 33: Phase 2C-2 — Discard / Form Dirty Race Safety
  {
    TabManager manager;
    auto *perf = manager.performanceManager();
    QObject owner;

    auto *view = new QWebEngineView();
    view->setUrl(QUrl(QStringLiteral("https://example.com/view")));
    const auto id = manager.registerTab(view, &owner, false, QStringLiteral("Tab"));

    perf->setBackgroundDiscardDelayMs(0);
    perf->setDiscardPolicyRequirement(DiscardPolicyRequirement::IdleOnly);
    emit view->page()->recommendedStateChanged(QWebEnginePage::LifecycleState::Discarded);

    assert(perf->canDiscard(id));

    // Form becomes dirty
    perf->setTabFormDirty(id, true);
    assert(!perf->canDiscard(id));
    assert(!perf->discardTab(id));

    delete view;
  }

  // Test 34: Phase 2C-2 — Discard / Audible & RecommendedState Race Safety
  {
    TabManager manager;
    auto *perf = manager.performanceManager();
    QObject owner;

    auto *view = new QWebEngineView();
    view->setUrl(QUrl(QStringLiteral("https://example.com/view")));
    const auto id = manager.registerTab(view, &owner, false, QStringLiteral("Tab"));

    perf->setBackgroundDiscardDelayMs(0);
    perf->setDiscardPolicyRequirement(DiscardPolicyRequirement::IdleOnly);
    emit view->page()->recommendedStateChanged(QWebEnginePage::LifecycleState::Discarded);

    assert(perf->canDiscard(id));

    // Tab becomes audible -> discard immediately forbidden
    emit view->page()->recentlyAudibleChanged(true);
    assert(!perf->canDiscard(id));
    assert(!perf->discardTab(id));

    emit view->page()->recentlyAudibleChanged(false);

    // Recommended state changes back to Active -> discard immediately forbidden
    emit view->page()->recommendedStateChanged(QWebEnginePage::LifecycleState::Active);
    assert(!perf->canDiscard(id));
    assert(!perf->discardTab(id));

    delete view;
  }

  // Test 35: Phase 2D — Persistence Round-Trip for Performance Settings
  {
    QSettings settings;
    const QVariant origMode = settings.value(QStringLiteral("performance/policyMode"));
    const QVariant origDiscard = settings.value(QStringLiteral("performance/discardEnabled"));
    const QVariant origAllowlist = settings.value(QStringLiteral("performance/siteAllowlist"));

    settings.setValue(QStringLiteral("performance/policyMode"), QStringLiteral("memory_saver"));
    settings.setValue(QStringLiteral("performance/discardEnabled"), false);
    settings.setValue(QStringLiteral("performance/siteAllowlist"), QStringList{QStringLiteral("youtube.com"), QStringLiteral("github.com")});

    TabManager manager;
    auto *perf = manager.performanceManager();
    assert(perf->policyMode() == PerformancePolicyMode::MemorySaver);
    assert(!perf->isDiscardEnabled());
    assert(perf->siteAllowlist().contains(QStringLiteral("youtube.com")));
    assert(perf->siteAllowlist().contains(QStringLiteral("github.com")));

    // Change and verify persistence
    perf->setPolicyMode(PerformancePolicyMode::MaximumPerformance);
    assert(settings.value(QStringLiteral("performance/policyMode")).toString() == QStringLiteral("maximum_performance"));

    perf->setDiscardEnabled(true);
    assert(settings.value(QStringLiteral("performance/discardEnabled")).toBool() == true);

    perf->setSiteAllowlist({QStringLiteral("wikipedia.org")});
    assert(settings.value(QStringLiteral("performance/siteAllowlist")).toStringList() == QStringList{QStringLiteral("wikipedia.org")});

    // Cleanup
    if (origMode.isValid()) settings.setValue(QStringLiteral("performance/policyMode"), origMode);
    else settings.remove(QStringLiteral("performance/policyMode"));
    if (origDiscard.isValid()) settings.setValue(QStringLiteral("performance/discardEnabled"), origDiscard);
    else settings.remove(QStringLiteral("performance/discardEnabled"));
    if (origAllowlist.isValid()) settings.setValue(QStringLiteral("performance/siteAllowlist"), origAllowlist);
    else settings.remove(QStringLiteral("performance/siteAllowlist"));
  }

  // Test 36: Phase 2D — Fallback on Corrupted / Unknown PolicyMode in QSettings
  {
    QSettings settings;
    const QVariant origMode = settings.value(QStringLiteral("performance/policyMode"));

    settings.setValue(QStringLiteral("performance/policyMode"), QStringLiteral("invalid_mode_xyz"));

    TabManager manager;
    auto *perf = manager.performanceManager();
    assert(perf->policyMode() == PerformancePolicyMode::Balanced);

    if (origMode.isValid()) settings.setValue(QStringLiteral("performance/policyMode"), origMode);
    else settings.remove(QStringLiteral("performance/policyMode"));
  }

  // Test 37: Phase 2D — Site Allowlist Normalization on Load
  {
    QSettings settings;
    const QVariant origAllowlist = settings.value(QStringLiteral("performance/siteAllowlist"));

    settings.setValue(QStringLiteral("performance/siteAllowlist"), QStringList{
        QStringLiteral("https://MUSIC.YOUTUBE.COM/watch?v=abc"),
        QStringLiteral("http://example.com/"),
        QStringLiteral("https://example.com/search?q=test")  // duplicate after normalization
    });

    TabManager manager;
    auto *perf = manager.performanceManager();
    assert(perf->siteAllowlist().size() == 2);
    assert(perf->siteAllowlist().contains(QStringLiteral("music.youtube.com")));
    assert(perf->siteAllowlist().contains(QStringLiteral("example.com")));

    if (origAllowlist.isValid()) settings.setValue(QStringLiteral("performance/siteAllowlist"), origAllowlist);
    else settings.remove(QStringLiteral("performance/siteAllowlist"));
  }

  // Test 38: Phase 2D — Dynamic Policy Mode Switching Delays and Signals
  {
    TabManager manager;
    auto *perf = manager.performanceManager();

    bool signalFired = false;
    PerformancePolicyMode signalMode = PerformancePolicyMode::Balanced;
    QObject::connect(perf, &TabPerformanceManager::policyModeChanged, [&signalFired, &signalMode](PerformancePolicyMode m) {
      signalFired = true;
      signalMode = m;
    });

    perf->setPolicyMode(PerformancePolicyMode::MemorySaver);
    assert(signalFired);
    assert(signalMode == PerformancePolicyMode::MemorySaver);
    assert(perf->backgroundFreezeDelayMs() == 30 * 1000);
    assert(perf->backgroundDiscardDelayMs() == 30 * 1000);

    signalFired = false;
    perf->setPolicyMode(PerformancePolicyMode::MaximumPerformance);
    assert(signalFired);
    assert(signalMode == PerformancePolicyMode::MaximumPerformance);
    assert(perf->backgroundFreezeDelayMs() == 2 * 60 * 1000);
    assert(perf->backgroundDiscardDelayMs() == 2 * 60 * 1000);
  }

  return 0;
}
