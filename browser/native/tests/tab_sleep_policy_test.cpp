// Shared tab sleeping policy (TabPerformanceManager) through the engine-neutral
// lifecycle adapter. Engine mechanics (Chromium discard, WebKit web process
// teardown) are covered by the engine tests; this test pins the one policy
// both engines follow: modes, timeout, protections, exceptions, manual
// actions and waking on activation.

#include <QApplication>
#include <QElapsedTimer>
#include <QSettings>
#include <QTemporaryDir>
#include <QWidget>
#include <cassert>
#include <iostream>

#include "desktop_tabs/tab_manager.h"
#include "desktop_tabs/tab_lifecycle_adapter.h"
#include "desktop_tabs/tab_performance_manager.h"
#include "desktop_tabs/tab_resource_model.h"
#include "engine/engine_view.h"

#include <QWebEngineView>
#include <unistd.h>

using dalinira::PerformancePolicyMode;
using dalinira::ProtectedReason;
using dalinira::TabLifecycleState;
using dalinira::TabPerformanceManager;

namespace {

// Stands in for an engine: accepts Frozen/Discarded only off screen, like
// both real engines, and remembers what it was asked.
class FakeEngineView final : public dalinira::engine::EngineView {
 public:
  explicit FakeEngineView(const QUrl &url) : url_(url) {}
  QUrl url() const override { return url_; }
  QString title() const override { return QStringLiteral("Fake page"); }
  bool isLoading() const override { return loading; }
  void setEngineActive(bool active) override { active_ = active; }
  bool isEngineActive() const override { return active_; }
  TabLifecycleState lifecycleState() const override { return state_; }
  bool setLifecycleState(TabLifecycleState state) override {
    if (state != TabLifecycleState::Active && active_) return false;
    if (state == state_) return true;
    if (state_ == TabLifecycleState::Discarded && state == TabLifecycleState::Active) ++restores;
    if (state == TabLifecycleState::Discarded) ++discards;
    state_ = state;
    ++transitions;
    emit lifecycleStateChanged(state_);
    return true;
  }
  bool isAudible() const override { return audible; }
  bool hasOpenDialog() const override { return dialog; }
  void setAudible(bool value) {
    audible = value;
    emit audibleChanged(value);
  }

  // Answers the shared unsaved-input check as a page with (or without)
  // typed form values would.
  void runJavaScript(const QString &script, std::function<void(const QVariant &)> callback) override {
    if (script == QString::fromUtf8(dalinira::unsavedInputScript())) ++unsavedInputChecks;
    if (callback) callback(QVariant(unsavedInput));
  }

  // The content process a real engine reports; none while discarded.
  qint64 contentProcessId() const override { return state_ == TabLifecycleState::Discarded ? 0 : processId; }

  bool loading = false;
  bool audible = false;
  bool dialog = false;
  int transitions = 0;
  int restores = 0;  // Discarded -> Active: a reload in a real engine
  int discards = 0;
  int unsavedInputChecks = 0;
  bool unsavedInput = false;
  qint64 processId = 0;

 private:
  QUrl url_;
  bool active_ = false;
  TabLifecycleState state_ = TabLifecycleState::Active;
};

void pump(int ms) {
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < ms) QApplication::processEvents(QEventLoop::AllEvents, 10);
}

struct Tabs {
  QWidget window;
  TabManager manager;
  TabPerformanceManager *perf = manager.performanceManager();
  QList<FakeEngineView *> views;
  QList<TabManager::TabId> ids;

  Tabs() {
    // Each scenario starts from the defaults (settings persist between them).
    perf->setSleepMediaTabs(false);
    perf->setSiteAllowlist({});
    perf->setSleepAfterMs(0);
    perf->setDiscardEnabled(true);
    perf->memoryPressureMonitor()->setSimulatedPressureLevel(dalinira::MemoryPressureLevel::Normal);
  }

  TabManager::TabId add(const QString &url) {
    auto *view = new FakeEngineView(QUrl(url));
    view->setParent(&window);
    const auto id = manager.registerEngineTab(view, &window, false, QStringLiteral("Tab"), QUrl(url));
    views.append(view);
    ids.append(id);
    return id;
  }
  void activate(int index) {
    for (int i = 0; i < views.size(); ++i) views[i]->setEngineActive(i == index);
    manager.activate(ids[index]);
  }
  TabLifecycleState state(int index) const { return perf->metadata(ids[index]).lifecycleState; }
};

}  // namespace

int main(int argc, char *argv[]) {
  QApplication app(argc, argv);
  QTemporaryDir settingsRoot;
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsRoot.path());
  app.setOrganizationName(QStringLiteral("DaliNiraTest"));
  app.setApplicationName(QStringLiteral("TabSleepPolicyTest"));
  std::cout << "=== SHARED TAB SLEEPING POLICY ===" << std::endl;

  {
    std::cout << "[1] Engine tabs are tracked through the lifecycle adapter..." << std::endl;
    Tabs tabs;
    tabs.add(QStringLiteral("https://a.example/"));
    tabs.add(QStringLiteral("https://b.example/"));
    tabs.activate(0);
    QString reason;
    assert(tabs.manager.validate(&reason));
    assert(tabs.perf->trackedWebTabs().size() == 2);
    assert(tabs.manager.record(tabs.ids[1])->lifecycle);
    std::cout << "[PASS]" << std::endl;
  }

  {
    std::cout << "[2] Balanced: light sleep at the timeout, full sleep only after long inactivity..." << std::endl;
    Tabs tabs;
    tabs.perf->setPolicyMode(PerformancePolicyMode::Balanced);
    tabs.perf->setSleepAfterMs(300);
    tabs.perf->setBalancedFullSleepAfterMs(1500);
    assert(tabs.perf->backgroundFreezeDelayMs() == 300 && tabs.perf->fullSleepDelayMs() == 1500);
    tabs.add(QStringLiteral("https://a.example/"));
    tabs.add(QStringLiteral("https://b.example/"));
    tabs.activate(0);
    tabs.activate(1);  // tab 0 goes to the background
    pump(100);
    assert(tabs.state(0) == TabLifecycleState::Active && "nothing happens before the timeout");
    pump(450);
    assert(tabs.state(0) == TabLifecycleState::Frozen && tabs.views[0]->lifecycleState() == TabLifecycleState::Frozen);
    assert(tabs.views[0]->unsavedInputChecks == 1 && "checked for unsaved input before light sleep");
    assert(tabs.views[0]->discards == 0 && tabs.perf->sleepingTabs().isEmpty());
    assert(tabs.state(1) == TabLifecycleState::Active && "the active tab never sleeps automatically");
    pump(1300);
    assert(tabs.state(0) == TabLifecycleState::Discarded && "long inactivity escalates to full sleep");
    assert(tabs.views[0]->lifecycleState() == TabLifecycleState::Discarded && "the engine really slept it");
    assert(tabs.perf->sleepingTabs() == QList<TabManager::TabId>{tabs.ids[0]});
    // Waking on activation.
    tabs.activate(0);
    assert(tabs.state(0) == TabLifecycleState::Active && tabs.views[0]->restores == 1);
    assert(tabs.views[0]->lifecycleState() == TabLifecycleState::Active);
    std::cout << "[PASS]" << std::endl;
  }

  {
    std::cout << "[3] Off: nothing is frozen or slept automatically, manual sleep still works..." << std::endl;
    Tabs tabs;
    tabs.perf->setPolicyMode(PerformancePolicyMode::Off);
    tabs.perf->setSleepAfterMs(100);
    tabs.add(QStringLiteral("https://a.example/"));
    tabs.add(QStringLiteral("https://b.example/"));
    tabs.activate(0);
    tabs.activate(1);
    pump(400);
    assert(tabs.state(0) == TabLifecycleState::Active && tabs.views[0]->transitions == 0);
    assert(tabs.perf->sleepTab(tabs.ids[0]));
    assert(tabs.state(0) == TabLifecycleState::Discarded);
    assert(tabs.perf->metadata(tabs.ids[0]).sleptManually);
    std::cout << "[PASS]" << std::endl;
  }

  {
    std::cout << "[4] Manual sleep never takes the active tab, internal pages or protected tabs..." << std::endl;
    Tabs tabs;
    tabs.perf->setPolicyMode(PerformancePolicyMode::Off);
    tabs.add(QStringLiteral("https://active.example/"));
    tabs.add(QStringLiteral("dalinira://newtab/"));
    tabs.add(QStringLiteral("https://dialog.example/"));
    tabs.add(QStringLiteral("https://permission.example/"));
    tabs.add(QStringLiteral("https://download.example/"));
    tabs.add(QStringLiteral("https://loading.example/"));
    tabs.activate(0);
    assert(!tabs.perf->sleepTab(tabs.ids[0]) && "active tab");
    assert(!tabs.perf->sleepTab(tabs.ids[1]) && "internal New Tab page");
    tabs.views[2]->dialog = true;
    tabs.perf->setPageDialogActive(tabs.ids[2], true);
    assert(!tabs.perf->sleepTab(tabs.ids[2]) && "open page dialog");
    tabs.perf->setPermissionPromptActive(tabs.ids[3], true);
    assert(!tabs.perf->sleepTab(tabs.ids[3]) && "permission prompt");
    tabs.perf->setBrowserDownloadActive(tabs.ids[4], true);
    assert(!tabs.perf->sleepTab(tabs.ids[4]) && "page save in progress");
    tabs.views[5]->loading = true;
    assert(!tabs.perf->sleepTab(tabs.ids[5]) && "loading page");
    tabs.perf->setPermissionPromptActive(tabs.ids[3], false);
    assert(tabs.perf->sleepTab(tabs.ids[3]) && "sleepable once the prompt is answered");
    std::cout << "[PASS]" << std::endl;
  }

  {
    std::cout << "[5] Media tabs are protected unless the user allows sleeping them..." << std::endl;
    Tabs tabs;
    tabs.perf->setPolicyMode(PerformancePolicyMode::MemorySaver);
    tabs.perf->setSleepAfterMs(200);
    tabs.perf->setSleepMediaTabs(false);
    tabs.add(QStringLiteral("https://active.example/"));
    tabs.add(QStringLiteral("https://music.example/"));
    tabs.views[1]->setAudible(true);
    tabs.activate(1);
    tabs.activate(0);
    pump(600);
    assert(tabs.state(1) == TabLifecycleState::Active && "a playing tab keeps playing");
    assert(tabs.perf->protectedReasons(tabs.ids[1]).testFlag(ProtectedReason::RecentlyAudible));
    tabs.perf->setSleepMediaTabs(true);
    pump(600);
    assert(tabs.state(1) == TabLifecycleState::Discarded && "allowed by the user");
    std::cout << "[PASS]" << std::endl;
  }

  {
    std::cout << "[6] Never-sleep sites and pinned tabs stay awake..." << std::endl;
    Tabs tabs;
    tabs.perf->setPolicyMode(PerformancePolicyMode::MemorySaver);
    tabs.perf->setSleepAfterMs(200);
    tabs.perf->setSiteAllowlist({QStringLiteral("music.example")});
    tabs.add(QStringLiteral("https://active.example/"));
    tabs.add(QStringLiteral("https://www.music.example/player"));
    tabs.add(QStringLiteral("https://pinned.example/"));
    tabs.add(QStringLiteral("https://plain.example/"));
    tabs.perf->setTabPinned(tabs.ids[2], true);
    tabs.activate(1);
    tabs.activate(2);
    tabs.activate(3);
    tabs.activate(0);
    pump(700);
    assert(tabs.state(1) == TabLifecycleState::Active && "never-sleep site (subdomains included): neither stage");
    assert(tabs.state(2) == TabLifecycleState::Active && "pinned tabs never sleep automatically");
    assert(tabs.state(3) == TabLifecycleState::Discarded);
    assert(tabs.perf->sleepTab(tabs.ids[2]) && "a pinned tab can still be slept manually");
    tabs.perf->setSiteAllowlist({});
    std::cout << "[PASS]" << std::endl;
  }

  {
    std::cout << "[7] Sleep all other tabs keeps the current tab and protected tabs awake..." << std::endl;
    Tabs tabs;
    tabs.perf->setPolicyMode(PerformancePolicyMode::Balanced);
    tabs.perf->setSleepAfterMs(60 * 60 * 1000);
    tabs.add(QStringLiteral("https://current.example/"));
    tabs.add(QStringLiteral("https://one.example/"));
    tabs.add(QStringLiteral("https://two.example/"));
    tabs.add(QStringLiteral("https://pinned.example/"));
    tabs.add(QStringLiteral("https://radio.example/"));
    tabs.perf->setTabPinned(tabs.ids[3], true);
    tabs.views[4]->setAudible(true);
    tabs.activate(0);
    const int slept = tabs.perf->sleepOtherTabs(&tabs.window, tabs.ids[0]);
    assert(slept == 2);
    assert(tabs.state(0) == TabLifecycleState::Active);
    assert(tabs.state(1) == TabLifecycleState::Discarded && tabs.state(2) == TabLifecycleState::Discarded);
    assert(tabs.state(3) != TabLifecycleState::Discarded && tabs.state(4) != TabLifecycleState::Discarded);
    std::cout << "[PASS]" << std::endl;
  }

  {
    std::cout << "[8] Memory pressure: Aggressive sleeps background tabs at once..." << std::endl;
    Tabs tabs;
    tabs.perf->setPolicyMode(PerformancePolicyMode::MemorySaver);
    tabs.perf->setSleepAfterMs(60 * 60 * 1000);
    tabs.add(QStringLiteral("https://a.example/"));
    tabs.add(QStringLiteral("https://b.example/"));
    tabs.activate(1);
    tabs.activate(0);
    pump(200);
    assert(tabs.state(1) != TabLifecycleState::Discarded);
    tabs.perf->memoryPressureMonitor()->setSimulatedPressureLevel(dalinira::MemoryPressureLevel::Moderate);
    pump(300);
    assert(tabs.state(1) == TabLifecycleState::Discarded);
    assert(tabs.state(0) == TabLifecycleState::Active);
    std::cout << "[PASS]" << std::endl;
  }

  {
    std::cout << "[9] Wake: Sleep and Wake follow the lifecycle; waking restores the same tab once..." << std::endl;
    Tabs tabs;
    tabs.perf->setPolicyMode(PerformancePolicyMode::Off);  // only explicit actions
    tabs.add(QStringLiteral("https://a.example/"));
    tabs.add(QStringLiteral("https://b.example/"));
    tabs.activate(1);
    tabs.activate(0);
    // Awake background tab: Sleep available, Wake not.
    assert(tabs.perf->canSleepNow(tabs.ids[1]) && !tabs.perf->canWakeNow(tabs.ids[1]));
    // The tab on screen: neither.
    assert(!tabs.perf->canSleepNow(tabs.ids[0]) && !tabs.perf->canWakeNow(tabs.ids[0]));
    assert(!tabs.perf->wakeTab(tabs.ids[0]) && "an awake tab is not woken again");

    assert(tabs.perf->sleepTab(tabs.ids[1]));
    assert(tabs.state(1) == TabLifecycleState::Discarded);
    // Sleeping tab: Wake available, Sleep not.
    assert(!tabs.perf->canSleepNow(tabs.ids[1]) && tabs.perf->canWakeNow(tabs.ids[1]));

    const int records = tabs.manager.recordCount();
    const int transitions = tabs.views[1]->transitions;
    assert(tabs.perf->wakeTab(tabs.ids[1]));  // at once, not after the timeout
    assert(tabs.state(1) == TabLifecycleState::Active);
    assert(tabs.views[1]->lifecycleState() == TabLifecycleState::Active);
    assert(tabs.views[1]->transitions == transitions + 1 && "one restore, through the adapter");
    assert(tabs.manager.recordCount() == records && tabs.manager.record(tabs.ids[1]) && "the same tab, no new one");
    assert(tabs.perf->canSleepNow(tabs.ids[1]) && !tabs.perf->canWakeNow(tabs.ids[1]));
    assert(!tabs.perf->wakeTab(tabs.ids[1]) && tabs.views[1]->transitions == transitions + 1 &&
           "waking an awake tab does nothing");

    // Activating a sleeping tab wakes it through the same path.
    assert(tabs.perf->sleepTab(tabs.ids[1]));
    const int beforeActivation = tabs.views[1]->transitions;
    tabs.activate(1);
    assert(tabs.state(1) == TabLifecycleState::Active && tabs.views[1]->transitions == beforeActivation + 1);
    assert(tabs.manager.recordCount() == records);
    std::cout << "[PASS]" << std::endl;
  }

  {
    std::cout << "[10] Both engines' tabs feed the same resource model inputs..." << std::endl;
    Tabs tabs;
    tabs.perf->setPolicyMode(PerformancePolicyMode::Off);
    tabs.add(QStringLiteral("https://a.example/"));  // EngineView adapter (the WebKit path)
    auto *chromium = new QWebEngineView(&tabs.window);  // QWebEnginePage adapter (the Chromium path)
    const auto chromiumId = tabs.manager.registerTab(chromium, &tabs.window, false, QStringLiteral("Chromium tab"));
    tabs.views[0]->processId = 4242;
    tabs.activate(0);
    const QVector<dalinira::TabResourceInput> inputs = dalinira::TabResourceModel::inputs(tabs.manager);
    assert(inputs.size() == 2);
    for (const dalinira::TabResourceInput &input : inputs) {
      if (input.id == tabs.ids[0]) {
        assert(input.processId == 4242 && input.visible && input.lifecycle == TabLifecycleState::Active);
      } else {
        assert(input.id == chromiumId && !input.visible && input.lifecycle == TabLifecycleState::Active);
        assert(input.processId == chromium->page()->renderProcessPid());
      }
    }
    std::cout << "[PASS]" << std::endl;
  }

  {
    std::cout << "[11] Sleeping records the tab's memory first, then shows it released..." << std::endl;
    Tabs tabs;
    tabs.perf->setPolicyMode(PerformancePolicyMode::Off);
    tabs.add(QStringLiteral("https://a.example/"));
    tabs.add(QStringLiteral("https://b.example/"));
    tabs.add(QStringLiteral("https://c.example/"));
    tabs.views[1]->processId = getpid();  // real processes for the real /proc reader
    tabs.views[2]->processId = getppid();
    tabs.activate(2);
    tabs.activate(1);
    tabs.activate(0);
    const dalinira::TabResourceSnapshot awake = tabs.manager.resourceSnapshot(tabs.ids[1]);
    assert(awake.activity == dalinira::TabActivity::Background);
    assert(awake.memory == dalinira::TabMemoryAttribution::Dedicated && awake.tabMemoryBytes > 0);
    // Tab 2 is never looked at before it sleeps: the memory saver's pre-sleep
    // signal alone lets the model record its figure.
    assert(tabs.perf->sleepTab(tabs.ids[2]));
    const dalinira::TabResourceSnapshot asleep = tabs.manager.resourceSnapshot(tabs.ids[2]);
    assert(asleep.activity == dalinira::TabActivity::Sleeping);
    assert(asleep.memory == dalinira::TabMemoryAttribution::Released && asleep.tabMemoryBytes == 0);
    assert(asleep.memoryBeforeSleepBytes > 0);
    assert(tabs.perf->wakeTab(tabs.ids[2]));
    const dalinira::TabResourceSnapshot woke = tabs.manager.resourceSnapshot(tabs.ids[2]);
    assert(woke.activity == dalinira::TabActivity::Background && woke.memory == dalinira::TabMemoryAttribution::Dedicated);
    std::cout << "[PASS]" << std::endl;
  }

  {
    std::cout << "[12] Balanced: memory pressure escalates a light-sleeping tab to full sleep..." << std::endl;
    Tabs tabs;
    tabs.perf->setPolicyMode(PerformancePolicyMode::Balanced);
    tabs.perf->setSleepAfterMs(300);  // long inactivity threshold stays at its 30-minute default
    tabs.add(QStringLiteral("https://a.example/"));
    tabs.add(QStringLiteral("https://b.example/"));
    tabs.activate(1);
    tabs.activate(0);
    pump(600);
    assert(tabs.state(1) == TabLifecycleState::Frozen);
    assert(tabs.perf->fullSleepDelayMs() == 30 * 60 * 1000);
    tabs.perf->memoryPressureMonitor()->setSimulatedPressureLevel(dalinira::MemoryPressureLevel::Moderate);
    assert(tabs.perf->fullSleepDelayMs() == 300);
    pump(300);
    assert(tabs.state(1) == TabLifecycleState::Discarded && tabs.state(0) == TabLifecycleState::Active);
    tabs.perf->memoryPressureMonitor()->setSimulatedPressureLevel(dalinira::MemoryPressureLevel::Normal);
    std::cout << "[PASS]" << std::endl;
  }

  {
    std::cout << "[13] Unsaved input: light sleep keeps it; no automatic full sleep; manual sleep still works..." << std::endl;
    Tabs tabs;
    tabs.perf->setPolicyMode(PerformancePolicyMode::Balanced);
    tabs.perf->setSleepAfterMs(300);
    tabs.perf->setBalancedFullSleepAfterMs(700);
    tabs.add(QStringLiteral("https://form.example/"));
    tabs.add(QStringLiteral("https://b.example/"));
    tabs.views[0]->unsavedInput = true;  // a half-filled registration form
    tabs.activate(0);
    tabs.activate(1);
    pump(550);
    assert(tabs.state(0) == TabLifecycleState::Frozen && "light sleep preserves the typed input");
    assert(tabs.perf->protectedReasons(tabs.ids[0]).testFlag(ProtectedReason::FormOrEditState));
    pump(900);
    assert(tabs.state(0) == TabLifecycleState::Frozen && "long inactivity does not discard unsaved input");
    tabs.perf->memoryPressureMonitor()->setSimulatedPressureLevel(dalinira::MemoryPressureLevel::Critical);
    pump(300);
    assert(tabs.state(0) == TabLifecycleState::Frozen && "nor does memory pressure");
    tabs.perf->memoryPressureMonitor()->setSimulatedPressureLevel(dalinira::MemoryPressureLevel::Normal);
    tabs.perf->setPolicyMode(PerformancePolicyMode::MemorySaver);
    tabs.perf->setSleepAfterMs(300);
    pump(300);
    assert(tabs.state(0) == TabLifecycleState::Frozen && "nor does Aggressive");
    // Waking a light-sleeping tab: no reload, and it is checked again next time.
    tabs.activate(0);
    assert(tabs.state(0) == TabLifecycleState::Active && tabs.views[0]->restores == 0 && tabs.views[0]->discards == 0);
    assert(!tabs.perf->protectedReasons(tabs.ids[0]).testFlag(ProtectedReason::FormOrEditState));
    // The user submitted the form; next time in the background it may fully sleep.
    tabs.views[0]->unsavedInput = false;
    tabs.activate(1);
    pump(700);
    assert(tabs.state(0) == TabLifecycleState::Discarded && tabs.views[0]->unsavedInputChecks == 2);
    // Manual sleep is the user's explicit choice: full sleep even with unsaved input.
    tabs.activate(0);
    tabs.views[0]->unsavedInput = true;
    tabs.perf->setPolicyMode(PerformancePolicyMode::Off);
    tabs.activate(1);
    assert(tabs.perf->sleepTab(tabs.ids[0]) && tabs.state(0) == TabLifecycleState::Discarded);
    std::cout << "[PASS]" << std::endl;
  }

  {
    std::cout << "[14] Aggressive: full sleep at the (shorter) timeout..." << std::endl;
    Tabs tabs;
    tabs.perf->setPolicyMode(PerformancePolicyMode::MemorySaver);
    assert(tabs.perf->effectiveSleepDelayMs() == 30 * 1000 && "Aggressive's default timeout is shorter");
    tabs.perf->setSleepAfterMs(300);
    assert(tabs.perf->fullSleepDelayMs() == 300);
    tabs.add(QStringLiteral("https://a.example/"));
    tabs.add(QStringLiteral("https://b.example/"));
    tabs.activate(1);
    tabs.activate(0);
    pump(150);
    assert(tabs.state(1) == TabLifecycleState::Active);
    pump(450);
    assert(tabs.state(1) == TabLifecycleState::Discarded && tabs.views[1]->discards == 1);
    std::cout << "[PASS]" << std::endl;
  }

  {
    std::cout << "[15] Wake from light sleep: in place, no reload; menu state follows both sleep stages..." << std::endl;
    Tabs tabs;
    tabs.perf->setPolicyMode(PerformancePolicyMode::Balanced);
    tabs.perf->setSleepAfterMs(200);
    tabs.add(QStringLiteral("https://a.example/"));
    tabs.add(QStringLiteral("https://b.example/"));
    tabs.activate(0);
    tabs.activate(1);
    pump(450);
    assert(tabs.state(0) == TabLifecycleState::Frozen);
    // Light sleep: Wake available, and full Sleep too (the user may free the memory).
    assert(tabs.perf->canWakeNow(tabs.ids[0]) && tabs.perf->canSleepNow(tabs.ids[0]));
    const int records = tabs.manager.recordCount();
    assert(tabs.perf->wakeTab(tabs.ids[0]));
    assert(tabs.state(0) == TabLifecycleState::Active && tabs.views[0]->restores == 0 && "no reload");
    assert(tabs.manager.recordCount() == records);
    assert(!tabs.perf->canWakeNow(tabs.ids[0]) && !tabs.perf->wakeTab(tabs.ids[0]));
    // Manual Sleep from light sleep goes straight to full sleep.
    pump(450);
    assert(tabs.state(0) == TabLifecycleState::Frozen);
    assert(tabs.perf->sleepTab(tabs.ids[0]) && tabs.state(0) == TabLifecycleState::Discarded);
    assert(tabs.perf->metadata(tabs.ids[0]).sleptManually);
    std::cout << "[PASS]" << std::endl;
  }

  std::cout << "=== ALL SHARED TAB SLEEPING POLICY TESTS PASSED ===" << std::endl;
  return 0;
}
