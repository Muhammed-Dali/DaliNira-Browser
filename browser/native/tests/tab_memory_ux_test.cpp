// Tab sleeping UX and tab memory display through the real BrowserWindow, on
// both engines, with the same checks: the context menu's Sleep/Wake actions
// follow the lifecycle, Wake restores the same tab (as activation does), and
// the hover card shows the shared resource model (desktop_tabs/
// tab_resource_model.h) without ever presenting a shared content process's
// memory as one tab's.
//
// GUI tier (scripts/safe-gui-test.sh): one BrowserWindow at a time, at most
// two tabs, local pages. DALINIRA_LIVE_TESTS=1 (--live) uses GitHub and
// YouTube instead of the local pages.
//
// The Chromium phase runs with --renderer-process-limit=1 (set by ctest), so
// its two tabs share one renderer, as Chromium does once it reaches its
// process limit. DaliNira's WebKit tabs each get their own web process.

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <unistd.h>
#include <QElapsedTimer>
#include <QFile>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QHash>
#include <QMenu>
#include <QSettings>
#include <QScreen>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <functional>
#include <iostream>
#include <signal.h>
#include <tuple>

#include "browser_window.h"
#include "core/browser_profile_service.h"
#include "desktop_tabs/tab_hover_card.h"
#include "desktop_tabs/tab_manager.h"
#include "desktop_tabs/tab_performance_manager.h"
#include "desktop_tabs/tab_strip_widget.h"
#include "engine/engine_backend.h"
#include "engine/engine_storage_manager.h"
#if defined(DALINIRA_BUILD_WEBKIT)
#include "engine/webkit/webkit_backend.h"
#endif

#define CHECK(condition)                                                             \
  do {                                                                               \
    if (!(condition)) {                                                              \
      std::cout << "[FAIL] " << __FILE__ << ":" << __LINE__ << ": " #condition << std::endl; \
      std::_Exit(1);                                                                 \
    }                                                                                \
  } while (0)

using dalinira::TabActivity;
using dalinira::TabMemoryAttribution;
using dalinira::TabResourceSnapshot;

namespace {

bool waitUntil(const std::function<bool()> &condition, int timeoutMs) {
  QTimer heartbeat;
  heartbeat.start(20);
  QElapsedTimer timer;
  timer.start();
  while (!condition()) {
    if (timer.elapsed() >= timeoutMs) return false;
    const qint64 next = timer.elapsed() + 50;
    while (timer.elapsed() < next) QApplication::processEvents(QEventLoop::AllEvents | QEventLoop::WaitForMoreEvents);
  }
  return true;
}

class LocalSite {
 public:
  LocalSite() {
    CHECK(server_.listen(QHostAddress::LocalHost));
    QObject::connect(&server_, &QTcpServer::newConnection, [this] {
      while (QTcpSocket *socket = server_.nextPendingConnection()) {
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket] {
          const QByteArray path = socket->readAll().split(' ').value(1);
          QByteArray body;
          if (path.startsWith("/form")) {
            // A half-filled form on a long page. Timer ticks (each doing ~3 ms
            // of work) and animation frames are logged with timestamps, so the
            // test can count them per phase; loadId changes on every reload.
            body = "<!doctype html><title>Form page</title><form id=f>"
                   "<input id=name><input id=email type=email><input id=pass type=password>"
                   "<textarea id=bio></textarea><input id=agree type=checkbox>"
                   "<select id=city><option>Ankara</option><option>Izmir</option><option>Istanbul</option></select>"
                   "<input type=radio name=plan id=planA value=a checked><input type=radio name=plan id=planB value=b>"
                   "</form><div style='height:6000px'>long</div><script>"
                   "window.loadId=Math.random().toString(36).slice(2);window.tickLog=[];window.rafLog=[];window.lifecycle=[];"
                   "setInterval(()=>{const t=Date.now();tickLog.push(t);if(tickLog.length>3000)tickLog.shift();"
                   "const e=t+3;while(Date.now()<e){}},100);"
                   "(function f(){rafLog.push(Date.now());if(rafLog.length>6000)rafLog.shift();requestAnimationFrame(f)})();"
                   "document.addEventListener('freeze',()=>lifecycle.push('freeze'));"
                   "document.addEventListener('resume',()=>lifecycle.push('resume'));</script>";
          } else if (path.startsWith("/heavy")) {
            // Roughly the weight of a large page: ~80 MB of script data.
            body = "<!doctype html><title>Heavy page</title><h1>heavy</h1><script>window.blob=[];"
                   "for(let i=0;i<40;i++)window.blob.push(new Array(250000).fill(i+0.5));</script>";
          } else if (path.startsWith("/light")) {
            body = "<!doctype html><title>Light page</title><p>light</p>";
          } else {
            body = "<!doctype html><title>Other page</title><p>other</p>";
          }
          socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\nContent-Length: " +
                        QByteArray::number(body.size()) + "\r\n\r\n" + body);
          socket->disconnectFromHost();
        });
        QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
      }
    });
  }
  QUrl url(const QString &path) const { return QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(server_.serverPort()).arg(path)); }

 private:
  QTcpServer server_;
};

bool live() { return qEnvironmentVariableIntValue("DALINIRA_LIVE_TESTS") == 1; }

struct LocalSiteUrls {
  QUrl form;
  QUrl light;
};

// WebKit web processes started by this test (descendants of this process).
QList<qint64> webProcesses() {
  QHash<qint64, qint64> parent;
  QHash<qint64, QByteArray> comm;
  for (const QString &entry : QDir(QStringLiteral("/proc")).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
    QFile stat(QStringLiteral("/proc/%1/stat").arg(entry));
    if (!stat.open(QIODevice::ReadOnly)) continue;
    const QByteArray text = stat.readAll();
    const int open = text.indexOf('(');
    const int close = text.lastIndexOf(')');
    const QList<QByteArray> fields = text.mid(close + 2).split(' ');
    if (open < 0 || close < open || fields.size() < 2 || fields.at(0) == "Z") continue;
    comm.insert(entry.toLongLong(), text.mid(open + 1, close - open - 1));
    parent.insert(entry.toLongLong(), fields.at(1).toLongLong());
  }
  QList<qint64> result;
  for (auto it = comm.cbegin(); it != comm.cend(); ++it) {
    if (!it.value().startsWith("WebKitWebProces")) continue;
    for (qint64 p = parent.value(it.key()), depth = 0; p > 1 && depth < 16; p = parent.value(p), ++depth)
      if (p == QCoreApplication::applicationPid()) { result.append(it.key()); break; }
  }
  return result;
}

// Which WebKit processes host a tab's page, and what the others cost.
void printWebProcessInventory(const char *engine, BrowserWindow &window) {
  QHash<qint64, QString> tabOf;
  for (int i = 0; i < window.tabCount(); ++i) {
    const qint64 pid = window.tabResourceSnapshot(i).processId;
    if (pid > 0) tabOf.insert(pid, window.tabInfo(i).title.left(30));
  }
  for (qint64 pid : webProcesses()) {
    const dalinira::ProcessUsage usage = dalinira::TabResourceModel::readProcessUsage(pid);
    std::cout << "  [" << engine << "] web process " << pid << ": "
              << (tabOf.contains(pid) ? "page of tab \"" + tabOf.value(pid).toStdString() + "\"" : std::string("no tab page"))
              << ", PSS " << dalinira::formatMemorySize(usage.pssBytes).toStdString() << std::endl;
  }
}

struct PageSpec {
  QUrl url;
  QString titleContains;
};

bool pageReady(BrowserWindow &window, int index, const QString &titleContains, int timeoutMs) {
  return waitUntil([&] {
    const BrowserTabInfo &tab = window.tabInfo(index);
    return tab.title.contains(titleContains, Qt::CaseInsensitive) && !tab.isLoading;
  }, timeoutMs);
}

// The resource snapshot once the tab's content process is known (a page that
// just loaded or woke may report it a moment later).
TabResourceSnapshot measured(BrowserWindow &window, int index) {
  TabResourceSnapshot snapshot;
  waitUntil([&] {
    snapshot = window.tabResourceSnapshot(index);
    return snapshot.memory != TabMemoryAttribution::Unavailable;
  }, 10000);
  return snapshot;
}

void print(const char *engine, const char *what, const TabResourceSnapshot &s) {
  const dalinira::TabResourceText text = dalinira::describeTabResources(s);
  std::cout << "  [" << engine << "] " << what << ": " << text.activity.toStdString() << " | " << text.memory.toStdString();
  if (!text.sharedMemory.isEmpty()) std::cout << " | " << text.sharedMemory.toStdString();
  if (!text.cpu.isEmpty()) std::cout << " | " << text.cpu.toStdString();
  std::cout << "  (" << text.diagnostics.toStdString() << ")" << std::endl;
}

// The sleep submenu as the tab context menu builds it: (object name, text, enabled).
QList<std::tuple<QString, QString, bool>> sleepMenu(BrowserWindow &window, int index, QMenu &menu) {
  menu.clear();
  window.populateTabSleepMenu(&menu, index);
  QList<std::tuple<QString, QString, bool>> actions;
  for (QAction *action : menu.actions())
    if (!action->isSeparator()) actions.append({action->objectName(), action->text(), action->isEnabled()});
  return actions;
}
bool enabled(QMenu &menu, const char *name) {
  for (QAction *action : menu.actions())
    if (action->objectName() == QLatin1String(name)) return action->isEnabled();
  CHECK(false && "menu action missing");
  return false;
}
void trigger(QMenu &menu, const char *name) {
  for (QAction *action : menu.actions())
    if (action->objectName() == QLatin1String(name)) return action->trigger();
  CHECK(false && "menu action missing");
}

// The hover card's visible lines for a tab, through the real hover path.
QStringList hoverLines(BrowserWindow &window, int index) {
  const QRect rect = window.tabStrip()->tabRect(index).translated(window.tabStrip()->mapToGlobal(QPoint(0, 0)));
  CHECK(QMetaObject::invokeMethod(&window, "onTabHovered", Q_ARG(int, index), Q_ARG(QPoint, rect.center()), Q_ARG(QRect, rect)));
  auto *card = window.findChild<TabHoverCard *>();
  CHECK(card && card->isVisible());
  QApplication::processEvents();
  QScreen *screen = QGuiApplication::screenAt(rect.center());
  CHECK(screen);
  QRect bounds = screen->availableGeometry();
  const QRect windowBounds = window.frameGeometry();
  const QRect intersection = bounds.intersected(windowBounds);
  if (intersection.width() >= card->width() + 16 && intersection.height() >= card->height() + 16)
    bounds = intersection;
  CHECK(bounds.contains(card->geometry()) && "hover card must remain inside the usable window/screen area");
  for (QLabel *label : card->findChildren<QLabel *>()) {
    if (!label->isVisible() || label->text().isEmpty()) continue;
    const QRect labelRect(label->mapTo(card, QPoint(0, 0)), label->size());
    CHECK(card->rect().contains(labelRect) && "hover-card label lies outside the card");
    if (!label->wordWrap()) continue;
    const int required = QFontMetrics(label->font())
                             .boundingRect(QRect(0, 0, label->width(), 10000),
                                           Qt::TextWordWrap | Qt::AlignLeft, label->text())
                             .height();
    CHECK(label->height() >= required && "hover-card label is clipped");
  }
  QStringList lines;
  for (const char *name : {"hover-lifecycle", "hover-memory", "hover-shared-memory",
                           "hover-memory-before-sleep", "hover-cpu"}) {
    auto *label = card->findChild<QLabel *>(QLatin1String(name));
    if (label && label->parentWidget()->isVisible()) lines.append(label->text());
  }
  QMetaObject::invokeMethod(&window, "onTabHoverLeave");
  return lines;
}
void checkUserFacing(const QStringList &lines) {
  static const QStringList prefixes{QStringLiteral("Durum:"), QStringLiteral("Bellek kullanımı:"),
                                    QStringLiteral("Paylaşılan işlem belleği:"), QStringLiteral("Uyumadan önce:"),
                                    QStringLiteral("İşlemci kullanımı:")};
  for (const QString &line : lines) {
    bool known = false;
    for (const QString &prefix : prefixes) known = known || line.startsWith(prefix);
    CHECK(known && "only the shared wording appears");
    CHECK(!line.contains(QLatin1String("PSS")) && !line.contains(QLatin1String("Renderer")) &&
          !line.contains(QLatin1String("GPU")));
  }
}

struct EngineResult {
  QList<std::tuple<QString, QString, bool>> awakeMenu;
  QList<std::tuple<QString, QString, bool>> sleepingMenu;
};

EngineResult runScenario(const char *engine, BrowserServices services, const PageSpec &first, const PageSpec &second,
                         bool expectSharedProcess) {
  std::cout << "[" << engine << "] tab sleeping UX and memory display..." << std::endl;
  EngineResult result;
  TabManager tabManager;
  services.tabManager = &tabManager;
  tabManager.performanceManager()->setPolicyMode(dalinira::PerformancePolicyMode::Off);  // explicit actions only
  BrowserWindow window(services, true);
  window.resize(1000, 700);
  window.show();
  const int pageTimeout = live() ? 45000 : 15000;

  // 1. One tab: alone in its content process, so its memory is the process's.
  window.addNewTab(first.url);
  CHECK(pageReady(window, 0, first.titleContains, pageTimeout));
  const TabResourceSnapshot alone = measured(window, 0);
  print(engine, "one tab", alone);
  CHECK(alone.activity == TabActivity::Active);
  CHECK(alone.memory == TabMemoryAttribution::Dedicated && alone.tabMemoryExact);
  CHECK(alone.tabMemoryBytes == alone.processMemoryBytes && alone.tabMemoryBytes > 5 * 1024 * 1024);
  checkUserFacing(hoverLines(window, 0));

  // 2. Two tabs.
  window.addNewTab(second.url);
  CHECK(pageReady(window, 1, second.titleContains, pageTimeout));
  const TabResourceSnapshot firstOfTwo = measured(window, 0);
  const TabResourceSnapshot secondOfTwo = measured(window, 1);
  print(engine, "tab 1 of 2", firstOfTwo);
  print(engine, "tab 2 of 2", secondOfTwo);
  CHECK(firstOfTwo.activity == TabActivity::Background && secondOfTwo.activity == TabActivity::Active);
  if (expectSharedProcess) {
    CHECK(firstOfTwo.processId == secondOfTwo.processId);
    CHECK(firstOfTwo.memory == TabMemoryAttribution::Shared && firstOfTwo.tabsInProcess == 2);
    CHECK(firstOfTwo.tabMemoryBytes < 0 && !firstOfTwo.tabMemoryExact && "the shared total is not this tab's memory");
    const QStringList lines = hoverLines(window, 0);
    checkUserFacing(lines);
    CHECK(lines.filter(QStringLiteral("Paylaşılan işlem belleği:")).size() == 1);
  } else {
    printWebProcessInventory(engine, window);
    CHECK(firstOfTwo.processId != secondOfTwo.processId);
    CHECK(firstOfTwo.memory == TabMemoryAttribution::Dedicated && secondOfTwo.memory == TabMemoryAttribution::Dedicated);
    // Each tab shows its own process, not the sum of every web process.
    CHECK(firstOfTwo.tabMemoryBytes == firstOfTwo.processMemoryBytes);
    checkUserFacing(hoverLines(window, 0));
  }

  QMenu menu;
  // Why the memory saver does not offer Sleep right now (diagnostics).
  const auto whyNoSleep = [&](int index) {
    auto *perf = tabManager.performanceManager();
    const auto meta = perf->metadata(window.tabInfo(index).uuid);
    const auto *record = tabManager.record(window.tabInfo(index).uuid);
    std::cout << "  sleep not offered: visible=" << meta.visible << " state=" << int(meta.lifecycleState)
              << " protected=0x" << std::hex << meta.protectedReasons.toInt() << std::dec
              << " loading=" << (record && record->lifecycle && record->lifecycle->isLoading())
              << " url=" << (record ? record->url.toString().toStdString() : std::string()) << std::endl;
  };
  // "Bu sekmeyi uyut" from the menu, once it is offered (a woken page reloads first).
  const auto sleepFromMenu = [&](int index) {
    const bool offered = waitUntil([&] { return sleepMenu(window, index, menu), enabled(menu, "tab-sleep"); }, 15000);
    if (!offered) whyNoSleep(index);
    CHECK(offered);
    trigger(menu, "tab-sleep");
    CHECK(waitUntil([&] { return window.isTabSleeping(index) && window.tabStrip()->isTabSleeping(index); }, 5000));
  };

  // 3. Menu while awake: Sleep available, Wake not.
  sleepMenu(window, 0, menu);
  CHECK(!enabled(menu, "tab-wake") && "an awake tab offers no Wake");
  // Sleep is offered once the background page may sleep (a live page may
  // still be loading, and a loading tab never sleeps).
  const bool sleepOffered = waitUntil([&] { return sleepMenu(window, 0, menu), enabled(menu, "tab-sleep"); }, 15000);
  if (!sleepOffered) whyNoSleep(0);
  CHECK(sleepOffered);
  result.awakeMenu = sleepMenu(window, 0, menu);
  sleepMenu(window, 1, menu);
  CHECK(!enabled(menu, "tab-sleep") && !enabled(menu, "tab-wake") && "the tab on screen can neither sleep nor wake");

  // 4. Sleep from the menu: Wake becomes available, memory is released.
  const qint64 processBeforeSleep = firstOfTwo.processId;
  sleepFromMenu(0);
  result.sleepingMenu = sleepMenu(window, 0, menu);
  CHECK(!enabled(menu, "tab-sleep") && enabled(menu, "tab-wake"));
  const TabResourceSnapshot asleep = window.tabResourceSnapshot(0);
  print(engine, "tab 1 asleep", asleep);
  CHECK(asleep.activity == TabActivity::Sleeping && asleep.memory == TabMemoryAttribution::Released &&
        asleep.tabMemoryBytes == 0);
  CHECK((asleep.memoryBeforeSleepBytes > 0) == !expectSharedProcess && "a figure before sleep exists when it was exact");
  checkUserFacing(hoverLines(window, 0));
  if (!expectSharedProcess) {
    // Its own web process is gone: sleeping really released it.
    CHECK(waitUntil([&] { return kill(static_cast<pid_t>(processBeforeSleep), 0) != 0; }, 10000));
  } else {
    // The shared renderer lives on for the other tab, which now has it alone.
    const TabResourceSnapshot remaining = measured(window, 1);
    print(engine, "tab 2 while tab 1 sleeps", remaining);
    CHECK(remaining.memory == TabMemoryAttribution::Dedicated && remaining.tabsInProcess == 1);
  }
  const int tabsBefore = window.tabCount();
  const QUrl urlBefore = window.tabInfo(0).url;
  const QString titleBefore = window.tabInfo(0).title;

  // 5. Wake from the menu: at once, the same tab, measured again.
  trigger(menu, "tab-wake");
  CHECK(waitUntil([&] { return !window.isTabSleeping(0) && !window.tabStrip()->isTabSleeping(0); }, 5000));
  CHECK(window.tabCount() == tabsBefore && "waking does not create a tab");
  CHECK(window.tabInfo(0).url == urlBefore);
  CHECK(pageReady(window, 0, first.titleContains, pageTimeout));
  CHECK(window.tabInfo(0).title == titleBefore);
  sleepMenu(window, 0, menu);
  CHECK(!enabled(menu, "tab-wake") && "an awake tab offers no Wake");
  // Sleep is offered again once the woken page may sleep (it reloads first).
  const bool sleepOfferedAgain = waitUntil([&] { return sleepMenu(window, 0, menu), enabled(menu, "tab-sleep"); }, 15000);
  if (!sleepOfferedAgain) whyNoSleep(0);
  CHECK(sleepOfferedAgain);
  const TabResourceSnapshot woke = measured(window, 0);
  print(engine, "tab 1 woken", woke);
  CHECK(woke.activity == TabActivity::Background);
  CHECK(woke.memory == (expectSharedProcess ? TabMemoryAttribution::Shared : TabMemoryAttribution::Dedicated));

  // 6. Sleep again, then wake by clicking (activating) the tab.
  sleepFromMenu(0);
  window.switchTab(0);
  CHECK(waitUntil([&] { return !window.isTabSleeping(0) && !window.tabStrip()->isTabSleeping(0); }, 5000));
  CHECK(window.tabCount() == tabsBefore);
  CHECK(pageReady(window, 0, first.titleContains, pageTimeout));
  const TabResourceSnapshot activated = measured(window, 0);
  print(engine, "tab 1 activated", activated);
  CHECK(activated.activity == TabActivity::Active);

  // 7. Close one of the two tabs: the other keeps (or regains) its process alone.
  window.closeTab(1);
  CHECK(waitUntil([&] { return window.tabCount() == 1; }, 5000));
  TabResourceSnapshot last;
  CHECK(waitUntil([&] {
    last = window.tabResourceSnapshot(0);
    return last.memory == TabMemoryAttribution::Dedicated && last.tabsInProcess == 1;
  }, 10000));
  print(engine, "remaining tab", last);
  checkUserFacing(hoverLines(window, 0));

  window.close();
  std::cout << "[PASS] " << engine << ": Sleep/Wake menu follows the lifecycle; memory is attributed honestly." << std::endl;
  return result;
}

// ---------------------------------------------------------------------------
// Two-stage sleep: automatic light sleep (Frozen) keeps the page, manual Sleep
// discards it; what survives each, measured on the real engine.
// ---------------------------------------------------------------------------
QVariant tabJs(BrowserWindow &window, int index, const QString &script) {
  auto state = std::make_shared<std::pair<bool, QVariant>>(false, QVariant());
  const auto done = [state](const QVariant &value) { state->second = value; state->first = true; };
  const BrowserTabInfo &tab = window.tabInfo(index);
  if (tab.engineView) tab.engineView->runJavaScript(script, done);
  else if (tab.view && tab.view->page()) tab.view->page()->runJavaScript(script, done);
  CHECK(waitUntil([&] { return state->first; }, 5000) && "script in the tab did not answer");
  return state->second;
}

const QString kFillForm = QStringLiteral(
    "name.value='Ada Lovelace';email.value='ada@example.com';pass.value='gizli-123';bio.value='satir 1\\nsatir 2';"
    "agree.checked=true;city.selectedIndex=2;planB.checked=true;scrollTo(0,1500);true");
const QString kReadForm = QStringLiteral(
    "JSON.stringify({name:name.value,email:email.value,pass:pass.value,bio:bio.value,agree:agree.checked,"
    "city:city.selectedIndex,plan:planB.checked?'b':'a',scrollY:Math.round(scrollY),loadId:loadId})");

struct FormState {
  QJsonObject fields;
  QString describe() const { return QString::fromUtf8(QJsonDocument(fields).toJson(QJsonDocument::Compact)); }
};
FormState readForm(BrowserWindow &window, int index) {
  return FormState{QJsonDocument::fromJson(tabJs(window, index, kReadForm).toString().toUtf8()).object()};
}
// Timer ticks and animation frames the page ran between two wall-clock times.
std::pair<double, double> activityPerSecond(BrowserWindow &window, int index, qint64 fromMs, qint64 toMs) {
  const double seconds = (toMs - fromMs) / 1000.0;
  const QString range = QStringLiteral("t=>t>=%1&&t<=%2").arg(fromMs).arg(toMs);
  const double ticks = tabJs(window, index, QStringLiteral("tickLog.filter(%1).length").arg(range)).toDouble();
  const double frames = tabJs(window, index, QStringLiteral("rafLog.filter(%1).length").arg(range)).toDouble();
  return {ticks / seconds, frames / seconds};
}
double processCpuPercent(qint64 pid, int windowMs) {
  const dalinira::ProcessUsage before = dalinira::TabResourceModel::readProcessUsage(pid);
  waitUntil([] { return false; }, windowMs);
  const dalinira::ProcessUsage after = dalinira::TabResourceModel::readProcessUsage(pid);
  return 100.0 * (after.cpuTicks - before.cpuTicks) / sysconf(_SC_CLK_TCK) / (windowMs / 1000.0);
}

struct StageReport {
  QStringList survivesFullSleep;
  QStringList lostInFullSleep;
};

StageReport runSleepStagesScenario(const char *engine, BrowserServices services, const LocalSiteUrls &urls) {
  std::cout << "[" << engine << "] two-stage sleep: light sleep keeps the page, manual sleep discards it..." << std::endl;
  TabManager tabManager;
  services.tabManager = &tabManager;
  auto *perf = tabManager.performanceManager();
  perf->setPolicyMode(dalinira::PerformancePolicyMode::Balanced);
  perf->setSleepAfterMs(4000);                     // light sleep after 4 s in the background
  perf->setBalancedFullSleepAfterMs(10 * 60 * 1000);  // no escalation until step 5
  BrowserWindow window(services, true);
  window.resize(1000, 700);
  window.show();
  const auto state = [&](int index) { return perf->metadata(window.tabInfo(index).uuid).lifecycleState; };

  // 1. Fill in the form, scroll, and measure the page while it is on screen.
  window.addNewTab(urls.form);
  CHECK(pageReady(window, 0, QStringLiteral("Form page"), 15000));
  CHECK(tabJs(window, 0, kFillForm).toBool());
  waitUntil([] { return false; }, 1500);
  const qint64 visibleFrom = QDateTime::currentMSecsSinceEpoch() - 1200;
  const qint64 visibleTo = QDateTime::currentMSecsSinceEpoch();
  const auto visible = activityPerSecond(window, 0, visibleFrom, visibleTo);
  const FormState filled = readForm(window, 0);
  std::cout << "  [" << engine << "] filled: " << filled.describe().toStdString() << std::endl;
  const qint64 pid = window.tabResourceSnapshot(0).processId;

  // 2. Background: running (throttled by the engine) until the timeout. The
  //    site is on the never-sleep list at first: no stage may touch it.
  perf->setSiteAllowlist({QStringLiteral("127.0.0.1")});
  window.addNewTab(urls.light);
  CHECK(pageReady(window, 1, QStringLiteral("Light page"), 15000));
  const qint64 backgroundFrom = QDateTime::currentMSecsSinceEpoch();
  const double backgroundCpu = processCpuPercent(pid, 2000);
  const qint64 backgroundTo = QDateTime::currentMSecsSinceEpoch();
  CHECK(state(0) == dalinira::TabLifecycleState::Active && "no sleep before the timeout");
  const TabResourceSnapshot background = window.tabResourceSnapshot(0);
  print(engine, "form tab in the background", background);
  waitUntil([] { return false; }, 4500);  // well past the 4 s timeout
  CHECK(state(0) == dalinira::TabLifecycleState::Active && "a never-sleep site gets neither light nor full sleep");
  CHECK(state(1) == dalinira::TabLifecycleState::Active && "the active tab never sleeps automatically");
  perf->setSiteAllowlist({});

  // 3. Automatic light sleep at the timeout, after the unsaved-input check.
  CHECK(waitUntil([&] { return state(0) == dalinira::TabLifecycleState::Frozen; }, 8000));
  CHECK(perf->protectedReasons(window.tabInfo(0).uuid).testFlag(dalinira::ProtectedReason::FormOrEditState) &&
        "the typed form values were detected as unsaved input");
  CHECK(!window.isTabSleeping(0) && "light sleep is not full sleep");
  const qint64 frozenFrom = QDateTime::currentMSecsSinceEpoch();
  const double frozenCpu = processCpuPercent(pid, 3000);
  const qint64 frozenTo = QDateTime::currentMSecsSinceEpoch();
  const TabResourceSnapshot frozen = window.tabResourceSnapshot(0);
  print(engine, "form tab in light sleep", frozen);
  CHECK(frozen.activity == TabActivity::LightSleep && frozen.memory != TabMemoryAttribution::Released);
  CHECK(state(0) == dalinira::TabLifecycleState::Frozen && "unsaved input is never escalated to full sleep");
  checkUserFacing(hoverLines(window, 0));

  // 4. Wake by clicking the tab: in place, no reload, everything as it was.
  const int tabs = window.tabCount();
  window.switchTab(0);
  CHECK(waitUntil([&] { return state(0) == dalinira::TabLifecycleState::Active; }, 5000));
  CHECK(window.tabCount() == tabs);
  const FormState woken = readForm(window, 0);
  std::cout << "  [" << engine << "] after light sleep: " << woken.describe().toStdString() << std::endl;
  CHECK(woken.fields.value(QStringLiteral("loadId")) == filled.fields.value(QStringLiteral("loadId")) &&
        "waking from light sleep does not reload the page");
  CHECK(woken.fields == filled.fields && "form values, checkbox, select, radio and scroll survive light sleep");
  const auto backgroundActivity = activityPerSecond(window, 0, backgroundFrom, backgroundTo);
  const auto frozenActivity = activityPerSecond(window, 0, frozenFrom + 300, frozenTo);
  const QString lifecycleEvents = tabJs(window, 0, QStringLiteral("lifecycle.join(',')")).toString();
  std::cout << "  [" << engine << "] activity per second: timers " << visible.first << " visible, "
            << backgroundActivity.first << " background, " << frozenActivity.first << " light sleep; frames "
            << visible.second << " visible, " << backgroundActivity.second << " background, " << frozenActivity.second
            << " light sleep; content process CPU " << backgroundCpu << "% background, " << frozenCpu
            << "% light sleep; page lifecycle events: " << lifecycleEvents.toStdString() << std::endl;
  CHECK(frozenActivity.second < 1.0 && "a light-sleeping page does not render");

  // 5. Manual Sleep: straight to full sleep (no light sleep first).
  window.switchTab(1);
  const auto freezesBefore = perf->metadata(window.tabInfo(0).uuid).freezeCount;
  QMenu menu;
  CHECK(waitUntil([&] { return sleepMenu(window, 0, menu), enabled(menu, "tab-sleep"); }, 15000));
  trigger(menu, "tab-sleep");
  CHECK(waitUntil([&] { return window.isTabSleeping(0); }, 5000));
  CHECK(perf->metadata(window.tabInfo(0).uuid).freezeCount == freezesBefore && "manual sleep is full sleep at once");
  const TabResourceSnapshot asleep = window.tabResourceSnapshot(0);
  print(engine, "form tab fully asleep", asleep);
  CHECK(asleep.memory == TabMemoryAttribution::Released);

  // 6. Wake from full sleep through the menu: the page is restored from saved state.
  sleepMenu(window, 0, menu);
  trigger(menu, "tab-wake");
  CHECK(waitUntil([&] { return !window.isTabSleeping(0); }, 5000));
  CHECK(window.tabCount() == tabs);
  CHECK(pageReady(window, 0, QStringLiteral("Form page"), 15000));
  waitUntil([] { return false; }, 1000);  // restoration of form state and scroll happens after load
  const FormState restored = readForm(window, 0);
  std::cout << "  [" << engine << "] after full sleep: " << restored.describe().toStdString() << std::endl;
  StageReport report;
  for (const QString &key : filled.fields.keys()) {
    if (key == QLatin1String("loadId")) continue;
    const bool kept = key == QLatin1String("scrollY")
                          ? qAbs(restored.fields.value(key).toInt() - filled.fields.value(key).toInt()) <= 5
                          : restored.fields.value(key) == filled.fields.value(key);
    (kept ? report.survivesFullSleep : report.lostInFullSleep).append(key);
  }
  std::cout << "  [" << engine << "] full sleep keeps: " << report.survivesFullSleep.join(',').toStdString()
            << "; loses: " << report.lostInFullSleep.join(',').toStdString() << std::endl;
  CHECK(restored.fields.value(QStringLiteral("loadId")) != filled.fields.value(QStringLiteral("loadId")) &&
        "full sleep released the page; waking loaded it again");

  // 7. Balanced escalation: a light-sleeping tab without unsaved input is
  //    fully slept after long inactivity.
  window.switchTab(0);
  CHECK(waitUntil([&] { return state(0) == dalinira::TabLifecycleState::Active; }, 5000));
  perf->setBalancedFullSleepAfterMs(8000);  // light sleep at 4 s, full sleep at 8 s
  CHECK(waitUntil([&] { return state(1) == dalinira::TabLifecycleState::Frozen; }, 10000));
  CHECK(waitUntil([&] { return state(1) == dalinira::TabLifecycleState::Discarded; }, 10000));
  const auto meta1 = perf->metadata(window.tabInfo(1).uuid);
  CHECK(meta1.freezeCount >= 1 && !meta1.sleptManually && "light sleep came first, then automatic full sleep");
  print(engine, "clean tab after long inactivity", window.tabResourceSnapshot(1));
  CHECK(window.tabCount() == tabs);

  window.close();
  std::cout << "[PASS] " << engine << ": light sleep keeps the page as it was; manual and long-inactivity sleep release it."
            << std::endl;
  return report;
}

}  // namespace

int main(int argc, char *argv[]) {
  qputenv("QT_QPA_PLATFORM", "xcb");
  qputenv("GDK_BACKEND", "x11");
  QApplication app(argc, argv);
  QTemporaryDir root;
  CHECK(root.isValid());
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, root.path());
  app.setOrganizationName(QStringLiteral("DaliNiraTest"));
  app.setApplicationName(QStringLiteral("TabMemoryUxTest"));
  std::cout << "=== TAB SLEEP/WAKE AND MEMORY UX (BOTH ENGINES) ===" << std::endl;

  LocalSite site;
  const PageSpec first = live() ? PageSpec{QUrl(QStringLiteral("https://github.com/")), QStringLiteral("GitHub")}
                                : PageSpec{site.url(QStringLiteral("/heavy")), QStringLiteral("Heavy page")};
  const PageSpec second = live() ? PageSpec{QUrl(QStringLiteral("https://www.youtube.com/")), QStringLiteral("YouTube")}
                                 : PageSpec{site.url(QStringLiteral("/light")), QStringLiteral("Light page")};

  EngineResult chromium;
  StageReport chromiumStages;
  const LocalSiteUrls urls{site.url(QStringLiteral("/form")), site.url(QStringLiteral("/light"))};
  {
    dalinira::engine::EngineBackend::setSelectedEngineType(dalinira::engine::EngineType::Chromium);
    BrowserProfileService profile(root.path() + QStringLiteral("/chromium"), nullptr, nullptr, false, nullptr);
    BrowserServices services;
    services.profile = profile.profile();
    services.profileService = &profile;
    const bool shared = QString::fromLocal8Bit(qgetenv("QTWEBENGINE_CHROMIUM_FLAGS")).contains(QLatin1String("--renderer-process-limit=1"));
    chromium = runScenario("Chromium", services, first, second, shared);
    chromiumStages = runSleepStagesScenario("Chromium", services, urls);
  }
#if defined(DALINIRA_BUILD_WEBKIT)
  {
    dalinira::engine::EngineStorageManager storage(root.path() + QStringLiteral("/engines"));
    auto webkit = std::make_unique<dalinira::engine::WebKitBackend>();
    CHECK(webkit->initialize(storage));
    CHECK(webkit->enableNetworkFiltering());  // as main.cpp does: the extension reports page processes
    dalinira::engine::EngineBackend::registerBackend(dalinira::engine::EngineType::WebKit, std::move(webkit));
    dalinira::engine::EngineBackend::setSelectedEngineType(dalinira::engine::EngineType::WebKit);
    BrowserProfileService profile(root.path() + QStringLiteral("/webkit"), nullptr, nullptr, false, nullptr);
    BrowserServices services;
    services.profileService = &profile;
    const EngineResult webkitResult = runScenario("WebKit", services, first, second, false);
    const StageReport webkitStages = runSleepStagesScenario("WebKit", services, urls);
    std::cout << "[REPORT] full sleep keeps — Chromium: " << chromiumStages.survivesFullSleep.join(',').toStdString()
              << " | WebKit: " << webkitStages.survivesFullSleep.join(',').toStdString() << std::endl;
    // The same menu, wording and state on both engines.
    CHECK(webkitResult.awakeMenu == chromium.awakeMenu);
    CHECK(webkitResult.sleepingMenu == chromium.sleepingMenu);
    std::cout << "[PASS] Chromium and WebKit show the same Sleep/Wake menu in both states." << std::endl;
    dalinira::engine::EngineBackend::setSelectedEngineType(dalinira::engine::EngineType::Chromium);
    dalinira::engine::EngineBackend::shutdown();
  }
#endif
  std::cout << "=== ALL TAB SLEEP/WAKE AND MEMORY UX TESTS PASSED ===" << std::endl;
  return 0;
}
