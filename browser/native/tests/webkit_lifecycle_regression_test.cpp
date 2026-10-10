// Short WebKit lifecycle regression check through the real BrowserWindow:
// tabs opening and closing, a background (hidden) tab, tab sleep and wake, a
// second window opened as the user's New Window command does (both windows
// browse, then it closes), the window closing and a clean engine shutdown,
// with no WebKit process left behind and no web process lost on the way.
//
// It covers the lifecycle that WebKitGTK 2.54.1 crashed in (a hidden page's
// compositor torn down while its release-buffers timer ran; fixed by
// packaging/webkit-runtime/patches/0001-*). One window lifetime, local
// pages, a few seconds. The patch's statistical validation (0 crashes in 500
// window lifetimes) is research evidence in docs/research/webkit-exit-crash/
// and is not repeated here.
//
// GUI tier (scripts/safe-gui-test.sh): at most two windows and three tabs.
// A watchdog thread ends the process within seconds if the main thread
// starts spinning (an event storm must never reach the window manager).

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QPointer>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <functional>
#include <iostream>
#include <memory>
#include <signal.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <unistd.h>

#include "browser_window.h"
#include "core/browser_profile_service.h"
#include "desktop_tabs/tab_manager.h"
#include "desktop_tabs/tab_performance_manager.h"
#include "engine/engine_backend.h"
#include "engine/engine_storage_manager.h"
#include "engine/engine_view.h"
#include "engine/webkit/webkit_backend.h"

#define CHECK(condition)                                                                    \
  do {                                                                                      \
    if (!(condition)) {                                                                     \
      std::cout << "[FAIL] " << __FILE__ << ":" << __LINE__ << ": " #condition << std::endl; \
      std::_Exit(1);                                                                        \
    }                                                                                       \
  } while (0)

namespace {

bool waitUntil(const std::function<bool()> &condition, int timeoutMs) {
  QTimer heartbeat;
  heartbeat.start(20);
  QElapsedTimer timer;
  timer.start();
  while (!condition()) {
    if (timer.elapsed() >= timeoutMs) return false;
    const qint64 next = timer.elapsed() + 50;
    while (timer.elapsed() < next) {
      QApplication::processEvents(QEventLoop::AllEvents | QEventLoop::WaitForMoreEvents);
      // A closed tab's view goes through deleteLater(), which runs only on
      // the way back to an event loop; this test has none (no exec()).
      QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
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
          // An animated page keeps its compositor busy, as real pages do.
          const QByteArray name = path.mid(1, 1).toUpper() + path.mid(2);
          const QByteArray body = "<!doctype html><title>" + name + " page</title><style>"
                                  "@keyframes s{to{transform:rotate(360deg)}}"
                                  "div{width:80px;height:80px;background:#36c;animation:s 1s linear infinite}"
                                  "</style><div></div><p>" + name + "</p>";
          socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\nContent-Length: " +
                        QByteArray::number(body.size()) + "\r\n\r\n" + body);
          socket->disconnectFromHost();
        });
        QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
      }
    });
  }
  QUrl url(const QString &path) const {
    return QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(server_.serverPort()).arg(path));
  }

 private:
  QTcpServer server_;
};

// WebKit helper processes (web, network, GPU) started by this process.
QList<qint64> webKitProcesses(const char *prefix = "WebKit") {
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
    if (!it.value().startsWith(prefix)) continue;
    for (qint64 p = parent.value(it.key()), depth = 0; p > 1 && depth < 16; p = parent.value(p), ++depth)
      if (p == QCoreApplication::applicationPid()) { result.append(it.key()); break; }
  }
  return result;
}

QList<qint64> webProcesses() { return webKitProcesses("WebKitWebProces"); }

bool exited(qint64 pid) { return kill(static_cast<pid_t>(pid), 0) != 0; }

bool pageReady(BrowserWindow &window, int index, const QString &title) {
  return waitUntil([&] {
    const BrowserTabInfo &tab = window.tabInfo(index);
    return tab.title.contains(title) && !tab.isLoading && tab.engineView;
  }, 15000);
}

// The web process that hosts a tab, once the tab reports it.
qint64 processOf(BrowserWindow &window, int index) {
  qint64 pid = 0;
  waitUntil([&] { return (pid = window.tabResourceSnapshot(index).processId) > 0; }, 10000);
  return pid;
}

// A lost web process fails the test at once: that is the crash this guards.
void watchForCrash(BrowserWindow &window, int index) {
  dalinira::engine::EngineView *view = window.tabInfo(index).engineView;
  CHECK(view);
  QObject::connect(view, &dalinira::engine::EngineView::webProcessTerminated, [index](int reason) {
    std::cout << "[FAIL] tab " << index << ": web process terminated (reason " << reason << ")" << std::endl;
    std::_Exit(1);
  });
}

void step(const char *text) { std::cout << "[STEP] " << text << std::endl; }

// Ends the process at once, from its own thread, when the test runs away:
// CPU >= 60% of a core for 5 s in a row, more than 900 MB resident, or more
// than 60 s in total. A healthy run takes a few seconds at modest CPU.
void startWatchdog() {
  std::thread([] {
    const long ticks = sysconf(_SC_CLK_TCK);
    const long page = sysconf(_SC_PAGESIZE);
    const auto cpuTicks = [] {
      FILE *f = std::fopen("/proc/self/stat", "r");
      if (!f) return 0L;
      char buffer[1024] = {};
      const size_t n = std::fread(buffer, 1, sizeof(buffer) - 1, f);
      std::fclose(f);
      buffer[n] = 0;
      const char *p = std::strrchr(buffer, ')');
      long utime = 0, stime = 0;
      // Fields after the command name: state is field 3; utime/stime are 14/15.
      if (p) std::sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %ld %ld", &utime, &stime);
      return utime + stime;
    };
    const auto residentBytes = [page] {
      long size = 0, resident = 0;
      if (FILE *f = std::fopen("/proc/self/statm", "r")) {
        if (std::fscanf(f, "%ld %ld", &size, &resident) != 2) resident = 0;
        std::fclose(f);
      }
      return resident * page;
    };
    long previous = cpuTicks();
    int hotSeconds = 0;
    for (int second = 1;; ++second) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      const long now = cpuTicks();
      const long percent = (now - previous) * 100 / ticks;
      previous = now;
      hotSeconds = percent >= 60 ? hotSeconds + 1 : 0;
      const long rssMb = residentBytes() / (1024 * 1024);
      const char *why = hotSeconds >= 5 ? "main loop spinning (CPU >= 60% for 5 s)"
                        : rssMb > 900   ? "resident memory above 900 MB"
                        : second > 60   ? "over 60 s"
                                        : nullptr;
      if (why) {
        std::printf("[FAIL] watchdog: %s; cpu=%ld%% rss=%ld MB at %d s\n", why, percent, rssMb, second);
        std::fflush(stdout);
        std::_Exit(1);
      }
    }
  }).detach();
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
  app.setApplicationName(QStringLiteral("WebKitLifecycleRegressionTest"));
  std::cout << "=== WEBKIT LIFECYCLE REGRESSION (tabs, background, sleep, two windows, close, shutdown) ===" << std::endl;
  startWatchdog();
  LocalSite site;

  dalinira::engine::EngineStorageManager storage(root.path() + QStringLiteral("/engines"));
  auto webkit = std::make_unique<dalinira::engine::WebKitBackend>();
  CHECK(webkit->initialize(storage));
  CHECK(webkit->enableNetworkFiltering());  // as main.cpp does
  dalinira::engine::EngineBackend::registerBackend(dalinira::engine::EngineType::WebKit, std::move(webkit));
  dalinira::engine::EngineBackend::setSelectedEngineType(dalinira::engine::EngineType::WebKit);
  {
    BrowserProfileService profile(root.path() + QStringLiteral("/profile"), nullptr, nullptr, false, nullptr);
    TabManager tabManager;
    tabManager.performanceManager()->setPolicyMode(dalinira::PerformancePolicyMode::Off);  // explicit sleep only
    BrowserServices services;
    services.profileService = &profile;
    services.tabManager = &tabManager;
    {
      BrowserWindow window(services, true);
      window.resize(1000, 700);
      window.show();

      step("open three tabs: two pages and a New Tab; the earlier ones go to the background");
      window.addNewTab(site.url(QStringLiteral("/alpha")));
      CHECK(pageReady(window, 0, QStringLiteral("Alpha page")));
      watchForCrash(window, 0);
      window.addNewTab(site.url(QStringLiteral("/beta")));
      CHECK(pageReady(window, 1, QStringLiteral("Beta page")));
      watchForCrash(window, 1);
      window.addNewTab();
      CHECK(waitUntil([&] { return window.tabCount() == 3 && window.tabInfo(2).engineView && !window.tabInfo(2).isLoading; }, 15000));
      watchForCrash(window, 2);
      const qint64 alpha = processOf(window, 0);
      const qint64 beta = processOf(window, 1);
      const qint64 newTab = processOf(window, 2);
      CHECK(alpha > 0 && beta > 0 && newTab > 0);
      CHECK(alpha != beta && beta != newTab && alpha != newTab && "each WebKit tab has its own web process");
      CHECK(webProcesses().size() == 3);

      step("close the New Tab in front: its web process exits");
      window.closeTab(2);
      CHECK(window.tabCount() == 2);
      CHECK(waitUntil([&] { return exited(newTab); }, 10000));

      step("sleep the hidden background tab: its web process exits; wake it");
      window.switchTab(1);
      CHECK(window.sleepTabAt(0));
      CHECK(waitUntil([&] { return window.isTabSleeping(0); }, 5000));
      CHECK(waitUntil([&] { return exited(alpha); }, 10000));
      CHECK(window.wakeTabAt(0));
      CHECK(waitUntil([&] { return !window.isTabSleeping(0); }, 5000));
      CHECK(pageReady(window, 0, QStringLiteral("Alpha page")));
      watchForCrash(window, 0);
      const qint64 alphaAwake = processOf(window, 0);
      CHECK(alphaAwake > 0 && alphaAwake != alpha);

      step("bring the woken tab to the front, then close the now hidden other tab");
      window.switchTab(0);
      CHECK(waitUntil([&] { return window.tabInfo(1).engineView && !window.tabInfo(1).engineView->isVisible(); }, 5000));
      window.closeTab(1);
      CHECK(waitUntil([&] { return exited(beta); }, 10000));
      CHECK(webProcesses() == QList<qint64>{alphaAwake});

      step("open a second window as New Window does; both windows browse; close the second");
      QPointer<BrowserWindow> second = window.openNewWindow(site.url(QStringLiteral("/gamma")));
      CHECK(second);
      CHECK(pageReady(*second, 0, QStringLiteral("Gamma page")));
      watchForCrash(*second, 0);
      const qint64 gamma = processOf(*second, 0);
      CHECK(gamma > 0 && gamma != alphaAwake);
      // The first window, now behind the second, keeps browsing.
      window.navigateCurrentTab(site.url(QStringLiteral("/delta")));
      CHECK(pageReady(window, 0, QStringLiteral("Delta page")));
      CHECK(window.tabCount() == 1 && second->tabCount() == 1);
      second->close();
      // The closed window goes away with its tabs, and so does its web process.
      const bool secondReleased = waitUntil([&] { return second.isNull() && exited(gamma); }, 10000);
      if (!secondReleased) {
        std::cout << "  closed second window: " << (second.isNull() ? "deleted" : "still alive") << ", web process "
                  << gamma << (exited(gamma) ? " exited" : " still running") << std::endl;
      }
      CHECK(secondReleased && "closing a window ends its WebKit tabs' web processes");
      CHECK(webProcesses().size() == 1);

      step("close the window");
      window.close();
    }
    CHECK(waitUntil([] { return webProcesses().isEmpty(); }, 10000));
  }

  step("shut the engine down: no WebKit process is left");
  dalinira::engine::EngineBackend::setSelectedEngineType(dalinira::engine::EngineType::Chromium);
  dalinira::engine::EngineBackend::shutdown();
  CHECK(dalinira::engine::WebKitBackend::waitForRetainedWebProcesses(10000));
  const bool clean = waitUntil([] { return webKitProcesses().isEmpty(); }, 10000);
  if (!clean) {
    for (qint64 pid : webKitProcesses()) std::cout << "  left behind: pid " << pid << std::endl;
  }
  CHECK(clean);
  std::cout << "=== WEBKIT LIFECYCLE REGRESSION PASSED ===" << std::endl;
  return 0;
}
