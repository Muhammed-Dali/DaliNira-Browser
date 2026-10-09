#pragma once

// Shared harness for the WebKit GUI tests (browser-webkit-backend,
// browser-webkit-media). Rules it enforces, so a test run stays light enough
// for a developer's desktop (docs/TESTING.md, "GUI and WebKit tests"):
//
//   * one top-level window for the whole run, reused by every case;
//   * at most one WebKitEngineView at a time: a case attaches its view, and
//     destroyView() checks that view's WebKitWebProcess lifecycle (below)
//     before the next case may create another;
//   * no busy waiting: waits sleep in the event loop and re-check their
//     condition at most every 50 ms;
//   * the first web process termination, check failure or overlong wait ends
//     the run immediately, with exit code 1 and no core dump (a WebKit core
//     dump costs systemd-coredump ~5.5 GB of memory on this class of machine).

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariant>
#include <QWidget>

#include <signal.h>
#include <sys/resource.h>
#include <unistd.h>

#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>

#include "engine/engine_view.h"
#include "engine/webkit/webkit_engine_view.h"

#define WK_CHECK(condition)                                                                          \
  do {                                                                                              \
    if (!(condition)) ::dalinira::test::WebKitGuiHarness::fail(__FILE__, __LINE__, #condition);    \
  } while (0)

namespace dalinira::test {

class WebKitGuiHarness {
 public:
  // Call before QApplication. Core dumps stay off for this process and every
  // WebKit process it starts (RLIMIT_CORE is inherited); crashes are still
  // logged by the kernel and systemd-coredump, only no core is processed.
  static void disableCoreDumps() {
    if (qEnvironmentVariableIntValue("DALINIRA_GUI_KEEP_CORES") == 1) return;
    const rlimit none{0, 0};
    setrlimit(RLIMIT_CORE, &none);
  }

  [[noreturn]] static void fail(const char *file, int line, const char *what) {
    std::cout << "[FAIL] " << file << ":" << line << ": " << what << std::endl;
    std::cout.flush();
    // _Exit: no abort (no core dump) and no static destructors that could
    // block on engine teardown. The runner cleans up the process tree.
    std::_Exit(1);
  }

  explicit WebKitGuiHarness(QApplication &app) : app_(app) {
    heartbeat_.setInterval(15);
    heartbeat_.start();
    auto *layout = new QVBoxLayout(&shell_);
    layout->setContentsMargins(0, 0, 0, 0);
    stack_ = new QStackedWidget(&shell_);
    otherTab_ = new QWidget(stack_);
    stack_->addWidget(otherTab_);
    layout->addWidget(stack_);
    shell_.setWindowTitle(QStringLiteral("DaliNira WebKit test"));
    shell_.resize(800, 600);
  }

  QApplication &app() { return app_; }
  QWidget &shell() { return shell_; }
  QStackedWidget *stack() { return stack_; }
  // Stands in for another tab (a Qt page) when a case hides its view.
  QWidget *otherTab() { return otherTab_; }

  void showShell() {
    if (shell_.isVisible()) return;
    shell_.show();
    WK_CHECK(waitUntil([&] { return shell_.isVisible(); }, 3000));
  }

  // Sleeps in the event loop until condition() holds; false on timeout.
  // Events are handled as they arrive (the heartbeat wakes the loop every
  // 15 ms); the condition itself, which may cost a JavaScript round trip or
  // /proc reads, is evaluated at most every kPollMs.
  bool waitUntil(const std::function<bool()> &condition, int timeoutMs) {
    QElapsedTimer timer;
    timer.start();
    while (!condition()) {
      if (timer.elapsed() >= timeoutMs) return false;
      const qint64 next = qMin<qint64>(timer.elapsed() + kPollMs, timeoutMs);
      while (timer.elapsed() < next) app_.processEvents(QEventLoop::AllEvents | QEventLoop::WaitForMoreEvents);
    }
    return true;
  }

  void pump(int ms) { waitUntil([] { return false; }, ms); }

  // Takes ownership of the run's single view: puts it in the shared window,
  // selects it, and treats any web process termination as a failure.
  template <typename View>
  View *attach(View *view, const char *label) {
    WK_CHECK(view != nullptr);
    WK_CHECK(current_ == nullptr && "only one WebKit view may exist at a time");
    current_ = view;
    currentLabel_ = label;
    QObject::connect(view, &engine::EngineView::webProcessTerminated, [label](int reason) {
      std::cout << "[FAIL] " << label << ": WebKit web process terminated (reason " << reason << ")" << std::endl;
      std::cout.flush();
      std::_Exit(1);
    });
    stack_->addWidget(view);
    showShell();
    select(view);
    ++viewsCreated_;
    std::cout << "  [harness] view " << viewsCreated_ << " (" << label << ") attached; live views 1, web processes "
              << webProcessPids().size() << std::endl;
    return view;
  }

  void select(engine::EngineView *view) {
    stack_->setCurrentWidget(view);
    if (auto *wk = qobject_cast<engine::WebKitEngineView *>(view)) wk->setEngineActive(true);
  }

  void hide(engine::EngineView *view) {
    if (auto *wk = qobject_cast<engine::WebKitEngineView *>(view)) wk->setEngineActive(false);
    stack_->setCurrentWidget(otherTab_);
  }

  // Destroys the current view and checks its web process lifecycle, so the
  // next case starts with no web process of this run left:
  //
  //  * A page without service workers: its web process exits promptly.
  //  * A page that registered a service worker (YouTube does): WebKit keeps
  //    the process while it hosts the worker. After the origin's last page
  //    closes, SWServer waits defaultTerminationDelay (10 s) before ending an
  //    idle worker, may re-arm that delay once while the worker connection is
  //    still in use, and only then may the process shut down
  //    (WebProcessProxy::canTerminateAuxiliaryProcess, webkitgtk-2.52.6).
  //    The retained process must stay idle (no page or media activity), must
  //    not grow, and must exit within that window. It is not a leak.
  //
  // whileRetained, if given, runs once with those pids after the teardown
  // settle time while WebKit still keeps the process (e.g. to check that
  // media output stopped). Returns the pids the view's page ran in.
  QList<int> destroyView(engine::EngineView *view,
                         const std::function<void(const QList<int> &)> &whileRetained = {}) {
    WK_CHECK(view == current_);
    const bool serviceWorker = hasServiceWorkerRegistration(view);
    const QList<int> pids = webProcessPids();
    QElapsedTimer sinceClose;
    delete view;
    sinceClose.start();
    current_ = nullptr;

    if (waitUntil([&] { return !anyAlive(pids); }, serviceWorker ? kServiceWorkerSettleMs : kProcessExitTimeoutMs)) {
      std::cout << "  [harness] " << currentLabel_ << " destroyed; its web process exited after "
                << sinceClose.elapsed() << " ms" << std::endl;
      return pids;
    }
    if (!serviceWorker) {
      std::cout << "  [harness] web process still running " << sinceClose.elapsed() << " ms after close:";
      for (int pid : pids) std::cout << " " << pid;
      std::cout << std::endl;
      WK_CHECK(false && "a page without service workers must release its web process promptly");
    }

    // Retained for the page's service worker: sample it until WebKit ends it.
    if (whileRetained) whileRetained(pids);
    waitForRetainedExit(pids, sinceClose, currentLabel_);
    return pids;
  }

  // Deletes the current view without waiting for its processes, as the
  // browser does at exit.
  void closeWithoutWaiting(engine::EngineView *view) {
    WK_CHECK(view == current_);
    delete view;
    current_ = nullptr;
  }

  // True when the page has a service worker registration (checked before the
  // page goes away; decides which lifecycle destroyView() expects).
  bool hasServiceWorkerRegistration(engine::EngineView *view) {
    auto *wk = qobject_cast<engine::WebKitEngineView *>(view);
    if (!wk || !wk->rawWebView()) return false;  // discarded: no page, no process
    js(view, QStringLiteral("window.__daliniraSw=undefined;(navigator.serviceWorker?navigator.serviceWorker.getRegistrations()"
                            ":Promise.resolve([])).then(r=>window.__daliniraSw=r.length,()=>window.__daliniraSw=0);0"));
    QVariant count;
    WK_CHECK(waitUntil([&] { return (count = js(view, QStringLiteral("window.__daliniraSw"))).isValid(); }, 3000));
    return count.toInt() > 0;
  }

  QVariant js(engine::EngineView *view, const QString &script, int timeoutMs = 3000) {
    auto state = std::make_shared<std::pair<bool, QVariant>>(false, QVariant());
    view->runJavaScript(script, [state](const QVariant &value) {
      state->second = value;
      state->first = true;
    });
    WK_CHECK(waitUntil([&] { return state->first; }, timeoutMs) && "JavaScript evaluation timed out");
    return state->second;
  }

  bool loadAndWait(engine::EngineView *view, const QUrl &url, int timeoutMs = 10000) {
    bool finished = false;
    bool ok = false;
    const auto connection = QObject::connect(view, &engine::EngineView::loadFinished, [&](bool success) {
      finished = true;
      ok = success;
    });
    view->load(url);
    const bool done = waitUntil([&] { return finished; }, timeoutMs);
    QObject::disconnect(connection);
    return done && ok;
  }

  // WebKit web processes started by this test (descendants of this process).
  static QList<int> webProcessPids() { return descendants("WebKitWebProces"); }
  // Every WebKit helper process (web, network, GPU) started by this test.
  static QList<int> webKitProcessPids() { return descendants("WebKit"); }

  // After the backend releases its web context, every WebKit helper must
  // go. A web process holds a strong reference to its process pool
  // (WebProcessProxy::m_processPool), so a process WebKit keeps for a
  // service worker keeps the pool, and with it the network process, until
  // the worker delay ends; then all of them exit. They must stay idle
  // meanwhile. (At browser exit nothing lingers: the helpers exit as soon
  // as the UI process's connections close; safe-gui-test.sh checks that.)
  void waitForAllWebKitProcessesToExit(const char *what = "shutdown") {
    const QList<int> pids = webKitProcessPids();
    QElapsedTimer timer;
    timer.start();
    if (waitUntil([&] { return !anyAlive(pids); }, kServiceWorkerSettleMs)) {
      std::cout << "  [harness] " << what << ": " << pids.size() << " WebKit process(es) exited after "
                << timer.elapsed() << " ms" << std::endl;
      return;
    }
    waitForRetainedExit(pids, timer, what);
  }

  // Samples processes WebKit still keeps (idle worker process, its pool's
  // network process) until they exit. After a heavy page (YouTube) the
  // process first releases the page's memory in short bursts; playback or
  // scripting would instead cost tens to hundreds of percent. So: average CPU
  // below kRetainedMaxAverageCpu, it must reach idle, it must not grow, and
  // it must exit within the service worker window.
  void waitForRetainedExit(const QList<int> &pids, const QElapsedTimer &since, const char *what) {
    const Usage settled = usage(pids);
    Usage last = settled;
    double peakCpu = 0;
    double lowestCpu = 100.0 * pids.size();
    qint64 peakPssKb = settled.pssKb;
    bool exited = false;
    while (since.elapsed() < kServiceWorkerExitMs) {
      if ((exited = waitUntil([&] { return !anyAlive(pids); }, 1000))) break;
      const Usage now = usage(pids);
      const double cpu = cpuPercent(last, now);
      peakCpu = qMax(peakCpu, cpu);
      lowestCpu = qMin(lowestCpu, cpu);
      peakPssKb = qMax(peakPssKb, now.pssKb);
      last = now;
    }
    const double averageCpu = cpuPercent(settled, last);
    std::cout << "  [harness] " << what << ": WebKit kept " << pids.size() << " process(es) for a service worker: CPU avg "
              << averageCpu << "% peak " << peakCpu << "%, PSS " << settled.pssKb / 1024 << " -> " << last.pssKb / 1024
              << " MB (peak " << peakPssKb / 1024 << " MB); " << (exited ? "exited" : "STILL RUNNING") << " after "
              << since.elapsed() << " ms" << std::endl;
    WK_CHECK(averageCpu < kRetainedMaxAverageCpu && "a retained WebKit process must not run a page or media");
    WK_CHECK(((exited && since.elapsed() < kServiceWorkerSettleMs + 1500) || lowestCpu < kIdleCpuPercent) &&
             "a retained WebKit process must become idle");
    WK_CHECK(peakPssKb - settled.pssKb < kIdleGrowthKb && "a retained WebKit process must not grow");
    WK_CHECK(exited && "processes WebKit keeps for a service worker must exit when it ends the worker");
  }

  // True while any of the pids is still a live (non-zombie) process.
  static bool anyAlive(const QList<int> &pids) {
    for (int pid : pids) {
      if (kill(pid, 0) != 0) continue;
      QFile stat(QStringLiteral("/proc/%1/stat").arg(pid));
      if (!stat.open(QIODevice::ReadOnly)) continue;
      const QByteArray text = stat.readAll();
      const int close = text.lastIndexOf(')');
      if (close > 0 && text.mid(close + 2, 1) != "Z") return true;
    }
    return false;
  }

  struct Usage {
    qint64 cpuTicks = 0;
    qint64 pssKb = 0;
    qint64 atMs = 0;
  };
  // CPU time and PSS of the given live processes.
  static Usage usage(const QList<int> &pids) {
    Usage result;
    result.atMs = QDateTime::currentMSecsSinceEpoch();
    for (int pid : pids) {
      QFile stat(QStringLiteral("/proc/%1/stat").arg(pid));
      if (stat.open(QIODevice::ReadOnly)) {
        const QByteArray text = stat.readAll();
        const QList<QByteArray> fields = text.mid(text.lastIndexOf(')') + 2).split(' ');
        if (fields.size() > 12) result.cpuTicks += fields.at(11).toLongLong() + fields.at(12).toLongLong();
      }
      QFile rollup(QStringLiteral("/proc/%1/smaps_rollup").arg(pid));
      if (rollup.open(QIODevice::ReadOnly)) {
        for (const QByteArray &line : rollup.readAll().split('\n')) {
          if (line.startsWith("Pss:")) result.pssKb += line.simplified().split(' ').value(1).toLongLong();
        }
      }
    }
    return result;
  }
  static double cpuPercent(const Usage &from, const Usage &to) {
    const qint64 ms = to.atMs - from.atMs;
    if (ms <= 0) return 0;
    return 100.0 * (to.cpuTicks - from.cpuTicks) * 1000.0 / sysconf(_SC_CLK_TCK) / ms;
  }

  int viewsCreated() const { return viewsCreated_; }

 private:
  static constexpr int kPollMs = 50;
  // A page without service workers: its web process exits promptly.
  static constexpr int kProcessExitTimeoutMs = 10000;
  // Service worker pages: time given to teardown before the retained process
  // is sampled, and the exit bound: 2 x SWServer::defaultTerminationDelay
  // (10 s) plus 5 s for the process shutdown handshake.
  static constexpr int kServiceWorkerSettleMs = 1500;
  static constexpr int kServiceWorkerExitMs = 25000;
  static constexpr double kIdleCpuPercent = 1.0;
  static constexpr double kRetainedMaxAverageCpu = 10.0;
  static constexpr qint64 kIdleGrowthKb = 8 * 1024;

  static QList<int> descendants(const QByteArray &commPrefix) {
    QHash<int, int> parent;
    QHash<int, QByteArray> comm;
    for (const QString &entry : QDir(QStringLiteral("/proc")).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
      bool numeric = false;
      const int pid = entry.toInt(&numeric);
      if (!numeric) continue;
      QFile stat(QStringLiteral("/proc/%1/stat").arg(pid));
      if (!stat.open(QIODevice::ReadOnly)) continue;
      const QByteArray text = stat.readAll();
      const int open = text.indexOf('(');
      const int close = text.lastIndexOf(')');
      if (open < 0 || close < open) continue;
      const QList<QByteArray> fields = text.mid(close + 2).split(' ');
      if (fields.size() < 2 || fields.at(0) == "Z") continue;  // zombies have exited
      comm.insert(pid, text.mid(open + 1, close - open - 1));
      parent.insert(pid, fields.at(1).toInt());
    }
    const int self = static_cast<int>(getpid());
    QList<int> result;
    for (auto it = comm.cbegin(); it != comm.cend(); ++it) {
      if (!it.value().startsWith(commPrefix)) continue;
      for (int p = parent.value(it.key()), depth = 0; p > 1 && depth < 16; p = parent.value(p), ++depth) {
        if (p == self) {
          result.append(it.key());
          break;
        }
      }
    }
    return result;
  }

  QApplication &app_;
  QTimer heartbeat_;
  QWidget shell_;
  QStackedWidget *stack_ = nullptr;
  QWidget *otherTab_ = nullptr;
  engine::EngineView *current_ = nullptr;
  const char *currentLabel_ = "";
  int viewsCreated_ = 0;
};

// Opt-in tiers (scripts/safe-gui-test.sh --manual / --live).
inline bool manualGuiTestsEnabled() { return qEnvironmentVariableIntValue("DALINIRA_GUI_MANUAL") == 1; }
inline bool liveSiteTestsEnabled() { return qEnvironmentVariableIntValue("DALINIRA_LIVE_TESTS") == 1; }

}  // namespace dalinira::test
