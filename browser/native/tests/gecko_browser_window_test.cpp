// The experimental Gecko engine in DaliNira's real BrowserWindow, off screen:
// GeckoBackend starts gecko_fake_host (the host side of the Gecko IPC, see
// gecko_adapter_test.cpp) from a test runtime, headless. No display, no GPU,
// no real Gecko; every file goes to a temporary directory.
//
// Checks: tabs bind host tabs (the host's first tab included), DaliNira's New
// Tab page, tab switching selects the host tab, navigation waits for the
// asynchronous host tab, a page-opened tab, closing a tab and the guarded
// close of a page with unsaved changes (cancelled, then confirmed), crash and
// recovery, the Gecko session file, one window, and the exit shutdown: no
// host process left, the run directory removed, the profile kept.

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>

#include <csignal>
#include <functional>
#include <iostream>
#include <sys/stat.h>

#include "browser_window.h"
#include "core/browser_profile_service.h"
#include "desktop_tabs/tab_manager.h"
#include "desktop_tabs/tab_performance_manager.h"
#include "engine/engine_backend.h"
#include "engine/engine_storage_manager.h"
#include "engine/gecko/gecko_backend.h"
#include "engine/gecko/gecko_engine_view.h"
#include "engine/gecko/gecko_session.h"
#include "i18n/i18n.h"
#include "session/session_store.h"

#define CHECK(condition)                                                                    \
  do {                                                                                      \
    if (!(condition)) {                                                                     \
      std::cout << "[FAIL] " << __FILE__ << ":" << __LINE__ << ": " #condition << std::endl; \
      std::_Exit(1);                                                                        \
    }                                                                                       \
  } while (false)

namespace {

using namespace dalinira::engine;

bool waitUntil(const std::function<bool()> &condition, int timeoutMs = 10000) {
  QElapsedTimer timer;
  timer.start();
  while (!condition()) {
    if (timer.elapsed() > timeoutMs) return false;
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QThread::msleep(5);
  }
  return true;
}

bool alive(qint64 pid) {
  QFile stat(QStringLiteral("/proc/%1/stat").arg(pid));
  if (!stat.open(QIODevice::ReadOnly)) return false;
  const QByteArray text = stat.readAll();
  return text.mid(text.lastIndexOf(')') + 2, 1) != "Z";
}

mode_t modeOf(const QString &path) {
  struct stat st {};
  return ::lstat(QFile::encodeName(path).constData(), &st) == 0 ? st.st_mode & 07777 : 0;
}

QString makeFakeRuntime(const QString &dir) {
  constexpr QFile::Permissions file = QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther;
  constexpr QFile::Permissions exec = file | QFile::ExeOwner | QFile::ExeGroup | QFile::ExeOther;
  const auto write = [](const QString &path, const QByteArray &content, QFile::Permissions permissions) {
    QFile out(path);
    CHECK(out.open(QIODevice::WriteOnly) && out.write(content) == content.size() && out.setPermissions(permissions));
  };
  CHECK(QDir().mkpath(dir + QStringLiteral("/modules")));
  CHECK(QFile::copy(QStringLiteral(DALINIRA_GECKO_FAKE_HOST), dir + QStringLiteral("/firefox")));
  CHECK(QFile(dir + QStringLiteral("/firefox")).setPermissions(exec));
  write(dir + QStringLiteral("/libxul.so"), {}, file);
  write(dir + QStringLiteral("/plugin-container"), {}, exec);
  write(dir + QStringLiteral("/platform.ini"), "[Build]\nMilestone=160.0a1\n", file);
  write(dir + QStringLiteral("/modules/AppConstants.sys.mjs"), "  NIGHTLY_BUILD: true,\n", file);
  return dir;
}

GeckoEngineView *geckoView(BrowserWindow *window, int index) {
  return index >= 0 && index < window->tabCount()
             ? qobject_cast<GeckoEngineView *>(window->tabInfo(index).engineView.data())
             : nullptr;
}

int indexOfHostTab(BrowserWindow *window, const QString &hostTabId) {
  for (int i = 0; i < window->tabCount(); ++i) {
    if (auto *view = geckoView(window, i); view && view->hostTabId() == hostTabId) return i;
  }
  return -1;
}

}  // namespace

int main(int argc, char *argv[]) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  // As main.cpp: only the selected engine runs in the process.
  EngineBackend::setSelectedEngineType(EngineType::Gecko);
  QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
  QApplication app(argc, argv);
  QTemporaryDir root;
  CHECK(root.isValid());
  const QString base = QFileInfo(root.path()).canonicalFilePath();
  QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, base + QStringLiteral("/settings"));
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, base + QStringLiteral("/settings"));
  app.setOrganizationName(QStringLiteral("DaliNiraTest"));
  app.setApplicationName(QStringLiteral("GeckoBrowserWindowTest"));
  const QString dataDir = base + QStringLiteral("/data");
  CHECK(QDir().mkpath(dataDir));
  CHECK(QDir().mkpath(base + QStringLiteral("/ipc")) && ::chmod(QFile::encodeName(base + QStringLiteral("/ipc")).constData(), 0700) == 0);
  std::cout << "=== GECKO IN BROWSERWINDOW (off screen, fake host) ===" << std::endl;

  BrowserProfileService profileService(dataDir, nullptr, &app);
  EngineStorageManager storage(dataDir);
  GeckoBackend::Config config;
  config.runtime = {makeFakeRuntime(base + QStringLiteral("/gre")), QStringLiteral(DALINIRA_GECKO_HOST_APP_DIR)};
  config.ipcRoot = base + QStringLiteral("/ipc");
  config.display = GeckoHostProcess::Display::Headless;
  auto ownedBackend = std::make_unique<GeckoBackend>(config, &app);
  GeckoBackend *gecko = ownedBackend.get();
  bool discard = false;
  int asked = 0;
  gecko->setDiscardConfirmation([&](QWidget *, const QString &) {
    ++asked;
    return discard;
  });
  const bool initialized = gecko->initialize(storage);
  if (!initialized) std::cout << gecko->lastError().toStdString() << std::endl;
  CHECK(initialized);
  EngineBackend::registerBackend(EngineType::Gecko, std::move(ownedBackend));
  GeckoSession *session = gecko->session();
  CHECK(waitUntil([&] { return session->state() == GeckoSession::State::Ready; }));
  const QString geckoData = storage.engineDir(EngineType::Gecko);
  CHECK((modeOf(geckoData) & 0022) == 0 && modeOf(geckoData + QStringLiteral("/runtime")) == 0700 &&
        modeOf(geckoData + QStringLiteral("/profile")) == 0700 && modeOf(geckoData + QStringLiteral("/host-output.log")) == 0600);
  CHECK(!gecko->createEphemeralView(nullptr) && gecko->versionString() == QStringLiteral("Gecko 160.0a1"));
  std::cout << "PASS backend started; private Gecko storage" << std::endl;

  TabManager tabManager(&app);
  tabManager.performanceManager()->setPolicyMode(dalinira::PerformancePolicyMode::Off);
  SessionStore sessionStore(dataDir, QStringLiteral("gecko"));
  BrowserServices services;
  services.profileService = &profileService;
  services.tabManager = &tabManager;
  services.sessionStore = &sessionStore;
  auto *window = new BrowserWindow(services);
  window->resize(1000, 700);
  window->show();
  CHECK(window->currentEngineScope() == QStringLiteral("gecko"));

  // The first tab shows the host's own first tab, then loads.
  window->addNewTab(QUrl(QStringLiteral("http://fake.test/a")));
  GeckoEngineView *a = geckoView(window, 0);
  CHECK(a);
  CHECK(waitUntil([&] { return !a->hostTabId().isEmpty() && a->url() == QUrl(QStringLiteral("http://fake.test/a")); }));
  CHECK(session->views().size() == 1);
  // DaliNira's New Tab page: an empty Gecko tab; the address stays DaliNira's.
  window->addNewTab(QUrl(QStringLiteral("dalinira://newtab/")));
  GeckoEngineView *b = geckoView(window, 1);
  CHECK(b && waitUntil([&] { return !b->hostTabId().isEmpty(); }));
  CHECK(b->hostTabId() != a->hostTabId() && b->url() == QUrl(QStringLiteral("dalinira://newtab/")) &&
        session->views().size() == 2);
  // Selecting a tab selects its host tab.
  window->switchTab(0);
  CHECK(waitUntil([&] { return session->activeTabId() == a->hostTabId(); }));
  window->switchTab(1);
  CHECK(waitUntil([&] { return session->activeTabId() == b->hostTabId(); }));
  std::cout << "PASS tabs bind host tabs; switching selects them" << std::endl;

  // Navigation before the host tab exists waits for it, in order.
  window->addNewTab(QUrl(QStringLiteral("http://fake.test/q1")));
  GeckoEngineView *c = geckoView(window, 2);
  CHECK(c && c->hostTabId().isEmpty());
  c->load(QUrl(QStringLiteral("http://fake.test/q2")));
  c->load(QUrl(QStringLiteral("http://fake.test/q3")));
  CHECK(waitUntil([&] { return c->url() == QUrl(QStringLiteral("http://fake.test/q3")) && !c->isLoading(); }));
  CHECK(waitUntil([&] { return c->canGoBack(); }));
  CHECK(waitUntil([&] { return window->tabInfo(2).url == QUrl(QStringLiteral("http://fake.test/q3")); }));
  c->back();
  CHECK(waitUntil([&] { return c->url() == QUrl(QStringLiteral("http://fake.test/q2")); }));
  std::cout << "PASS queued navigation and history" << std::endl;

  // A page-opened tab becomes a tab of this window, bound to that host tab.
  const int beforePopup = window->tabCount();
  a->load(QUrl(QStringLiteral("http://fake.test/open-popup")));
  CHECK(waitUntil([&] { return window->tabCount() == beforePopup + 1; }));
  GeckoEngineView *popup = geckoView(window, window->tabCount() - 1);
  CHECK(popup && waitUntil([&] { return popup->url() == QUrl(QStringLiteral("http://fake.test/popup")); }));
  const QString popupHostTab = popup->hostTabId();
  CHECK(session->views().size() == 4);
  // Closing a clean tab closes its host tab.
  window->closeTab(indexOfHostTab(window, popupHostTab));
  CHECK(waitUntil([&] { return window->tabCount() == beforePopup && !session->view(popupHostTab); }));
  std::cout << "PASS page-opened tab adopted; closing a tab closes its host tab" << std::endl;

  // Unsaved changes: the user decides; cancelled keeps the tab and its page.
  c->load(QUrl(QStringLiteral("http://fake.test/dirty")));
  CHECK(waitUntil([&] { return session->view(c->hostTabId()) && session->view(c->hostTabId())->hasBeforeUnload(); }));
  const QString dirtyHostTab = c->hostTabId();
  const int tabsBefore = window->tabCount();
  window->closeTab(indexOfHostTab(window, dirtyHostTab));
  CHECK(waitUntil([&] { return asked == 1; }));
  QThread::msleep(100);
  QCoreApplication::processEvents();
  CHECK(window->tabCount() == tabsBefore && session->view(dirtyHostTab) && indexOfHostTab(window, dirtyHostTab) >= 0);
  discard = true;
  window->closeTab(indexOfHostTab(window, dirtyHostTab));
  CHECK(waitUntil([&] { return asked == 2 && window->tabCount() == tabsBefore - 1 && !session->view(dirtyHostTab); }));
  std::cout << "PASS guarded close: cancelled, then confirmed" << std::endl;

  // A content process crash: the tab says so; reload recovers the page.
  const qint64 victim = b->contentProcessId();
  CHECK(victim > 0);
  const int bIndex = indexOfHostTab(window, b->hostTabId());
  CHECK(::kill(static_cast<pid_t>(victim), SIGKILL) == 0);
  CHECK(waitUntil([&] { return b->isCrashed(); }));
  CHECK(waitUntil([&] {
    return window->tabInfo(bIndex).title == dalinira::i18n::I18n::text(QStringLiteral("tab.process_terminated"));
  }));
  b->reload();
  CHECK(waitUntil([&] { return !b->isCrashed() && b->contentProcessId() > 0 && b->contentProcessId() != victim; }));
  std::cout << "PASS crash shown; reload recovers" << std::endl;

  // One window: Gecko tabs stay in it; a new window is a new tab here.
  CHECK(!window->canMoveTabToAnotherWindow(window->tabInfo(0).id));
  const int beforeNewWindow = window->tabCount();
  CHECK(window->openNewWindow(QUrl(QStringLiteral("http://fake.test/window"))) == window);
  CHECK(window->tabCount() == beforeNewWindow + 1);
  std::cout << "PASS one window" << std::endl;

  // The Gecko workspace's session file only.
  window->saveSessionNow();
  QFile sessionFile(dataDir + QStringLiteral("/session/gecko/tabs.session.json"));
  CHECK(sessionFile.open(QIODevice::ReadOnly) && sessionFile.readAll().contains("http://fake.test/"));
  CHECK(!QFileInfo::exists(dataDir + QStringLiteral("/session/chromium")) &&
        !QFileInfo::exists(dataDir + QStringLiteral("/session/webkit")));
  std::cout << "PASS the Gecko session file only" << std::endl;

  // Exit, as main.cpp: the host first, then the window, then the backend.
  QList<qint64> tree{session->hostPid()};
  for (const auto &process : session->hostProcesses()) tree.append(process.pid);
  const QString runDirectory = session->runDirectory();
  CHECK(QFileInfo(runDirectory).isDir());
  CHECK(gecko->shutdownHost(5000));
  for (const qint64 pid : tree) CHECK(waitUntil([&] { return !alive(pid); }));
  CHECK(!QFileInfo::exists(runDirectory));
  CHECK(QDir(geckoData + QStringLiteral("/runtime")).entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty());
  CHECK(QDir(base + QStringLiteral("/ipc")).entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty());
  CHECK(QFileInfo(geckoData + QStringLiteral("/profile/user.js")).isFile());
  delete window;
  EngineBackend::shutdown();
  std::cout << "PASS exit: no host process, run directory removed, profile kept" << std::endl;
  std::cout << "=== ALL GECKO BROWSERWINDOW TESTS PASSED ===" << std::endl;
  return 0;
}
