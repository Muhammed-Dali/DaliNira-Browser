#include "desktop_tabs/tab_manager.h"
#include "desktop_tabs/tab_performance_manager.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>
#include <QWebEngineView>
#include <cassert>
#include <functional>

namespace {
bool processUntil(const std::function<bool()> &condition, int timeoutMs) {
  QElapsedTimer elapsed;
  elapsed.start();
  while (!condition() && elapsed.elapsed() < timeoutMs) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QThread::msleep(5);
  }
  return condition();
}
}  // namespace

int main(int argc, char *argv[]) {
  qputenv("QT_QPA_PLATFORM", QByteArray("offscreen"));
  qputenv("QTWEBENGINE_CHROMIUM_FLAGS", QByteArray("--no-sandbox --disable-gpu"));
  QApplication app(argc, argv);

  TabManager manager;
  auto *performance = manager.performanceManager();
  QObject owner;
  auto *active = new QWebEngineView;
  auto *background = new QWebEngineView;
  active->setUrl(QUrl(QStringLiteral("https://example.com/active")));
  background->setUrl(QUrl(QStringLiteral("https://example.com/background")));
  const auto activeId = manager.registerTab(active, &owner, false, QStringLiteral("Active"));
  const auto backgroundId = manager.registerTab(background, &owner, false, QStringLiteral("Background"));
  manager.activate(activeId);
  performance->setDiscardEnabled(false);
  performance->setBackgroundFreezeDelayMs(0);

  // A new document is temporarily ineligible because it is loading, while
  // its lifecycle deadline is already overdue.
  background->setHtml(QStringLiteral("<html><body>ready</body></html>"),
                      QUrl(QStringLiteral("https://example.com/background")));
  QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  assert(background->page()->isLoading());
  emit background->page()->recommendedStateChanged(QWebEnginePage::LifecycleState::Frozen);
  const int scheduledDelay = performance->scheduledDeadlineDelayMs();
  assert(scheduledDelay > 0);
  assert(scheduledDelay <= dalinira::TabPerformanceManager::kMinimumDeadlineRetryMs + 200);

  // Once the page may sleep, one pass asks it about unsaved input and one
  // more (after the answer) freezes it: never more than that, however long
  // loading took.
  const quint64 checksBefore = performance->deadlineCheckCount();
  QElapsedTimer shortWindow;
  shortWindow.start();
  while (shortWindow.elapsed() < 150) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  assert(performance->deadlineCheckCount() <= checksBefore + 2);

  // Once loading settles, the bounded retry performs the original transition.
  assert(processUntil([&] { return performance->isTabFrozen(backgroundId); }, 4000));
  assert(performance->deadlineCheckCount() <= checksBefore + 4);

  manager.remove(backgroundId);
  manager.remove(activeId);
  delete background;
  delete active;
  return 0;
}
