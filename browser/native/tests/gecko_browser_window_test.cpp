// The experimental Gecko engine in DaliNira's real BrowserWindow, off screen:
// GeckoBackend starts gecko_fake_host (the host side of the Gecko IPC, see
// gecko_adapter_test.cpp) from a test runtime, headless. No display, no GPU,
// no real Gecko; every file goes to a temporary directory.
//
// Checks: tabs bind host tabs (the host's first tab included), DaliNira's New
// Tab page, tab switching selects the host tab, navigation waits for the
// asynchronous host tab, a page-opened tab, closing a tab and the guarded
// close of a page with unsaved changes (cancelled, then confirmed), crash and
// recovery, one window, the keyboard (browser shortcuts typed into a page
// reach the window's actions exactly once and only for the visible active tab,
// every other key reaches the page, rebinding takes effect, the address bar
// takes the keyboard), the Gecko session file, and the exit shutdown: no host
// process left, the run directory removed, the profile kept.

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QLineEdit>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <csignal>
#include <functional>
#include <iostream>
#include <sys/stat.h>

#include "browser_window.h"
#include "core/browser_profile_service.h"
#include "core/keyboard_shortcut_service.h"
#include "desktop_tabs/tab_manager.h"
#include "desktop_tabs/tab_performance_manager.h"
#include "engine/engine_backend.h"
#include "engine/engine_storage_manager.h"
#include "engine/gecko/gecko_backend.h"
#include "engine/gecko/gecko_engine_view.h"
#include "engine/gecko/gecko_session.h"
#include "engine/gecko/gecko_ipc_server.h"
#include "engine/gecko/gecko_shortcut_router.h"
#include "i18n/i18n.h"
#include "session/session_store.h"

#define CHECK(condition)                                                                    \
  do {                                                                                      \
    if (!(condition)) {                                                                     \
      std::cout << "[FAIL] " << __FILE__ << ":" << __LINE__ << ": " #condition << std::endl; \
      std::_Exit(1);                                                                        \
    }                                                                                       \
  } while (false)

namespace dalinira::engine {
class GeckoSessionTestAccess {
 public:
  static QJsonObject pending(GeckoSession *session, const QString &id) { return session->pending_.value(id).message; }
  static void receive(GeckoSession *session, quint64 generation, const QJsonObject &message) { session->receive(generation, message); }
  static void disconnect(GeckoSession *session) { session->ipc_->shutdown(); }
};
}

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
  QTemporaryDir root(QDir::tempPath() + QStringLiteral("/gc-XXXXXX"));
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
  config.testInput = true;  // "test_key": keys typed into the fake host's page
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

  // The host's names of DaliNira's shortcuts; every default binding has one.
  using Router = GeckoShortcutRouter;
  const auto keys = [](QKeyCombination combination) { return QKeySequence(combination); };
  CHECK(Router::hostShortcut(keys(Qt::CTRL | Qt::Key_T)) == QStringLiteral("Ctrl+T"));
  CHECK(Router::hostShortcut(keys(Qt::CTRL | Qt::SHIFT | Qt::Key_Tab)) == QStringLiteral("Ctrl+Shift+Tab"));
  CHECK(Router::hostShortcut(keys(Qt::CTRL | Qt::Key_Backtab)) == QStringLiteral("Ctrl+Shift+Tab"));
  CHECK(Router::hostShortcut(keys(Qt::ALT | Qt::Key_Left)) == QStringLiteral("Alt+ArrowLeft"));
  CHECK(Router::hostShortcut(keys(Qt::CTRL | Qt::Key_PageDown)) == QStringLiteral("Ctrl+PageDown"));
  CHECK(Router::hostShortcut(keys(Qt::CTRL | Qt::Key_Plus)) == QStringLiteral("Ctrl++"));
  CHECK(Router::hostShortcut(keys(Qt::CTRL | Qt::SHIFT | Qt::Key_Equal)) == QStringLiteral("Ctrl+="));
  CHECK(Router::hostShortcut(keys(Qt::SHIFT | Qt::Key_F3)) == QStringLiteral("Shift+F3"));
  CHECK(Router::hostShortcut(keys(QKeyCombination(Qt::Key_F5))) == QStringLiteral("F5"));
  for (const QKeySequence &none : {keys(Qt::CTRL | Qt::Key_C), keys(Qt::META | Qt::Key_T), keys(Qt::CTRL | Qt::ALT | Qt::Key_Q),
                                   keys(Qt::CTRL | Qt::Key_Odiaeresis), keys(QKeyCombination(Qt::Key_T)), QKeySequence()})
    CHECK(Router::hostShortcut(none).isEmpty());
  Router *router = gecko->shortcutRouter();
  CHECK(waitUntil([&] { return !router->hostShortcuts().isEmpty(); }));
  auto &shortcutService = dalinira::KeyboardShortcutService::instance();
  for (const auto &definition : dalinira::KeyboardShortcutService::registry()) {
    for (const QKeySequence &sequence : shortcutService.shortcuts(definition.id))
      CHECK(router->hostShortcuts().contains(Router::hostShortcut(sequence)));
  }
  std::cout << "PASS every browser shortcut has a host name and is given to the host" << std::endl;

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

  // The keyboard. "test_key" types into the host's active tab as a keyboard
  // would; a browser shortcut runs the window's action once, any other key
  // reaches the page (whose title lists what it received).
  const auto finished = [](GeckoRequest *request) {
    CHECK(waitUntil([&] { return request->isFinished(); }));
    return request->status();
  };
  const auto type = [&](const QString &key, const QStringList &modifiers) {
    CHECK(finished(session->submit(QStringLiteral("test_key"),
                                   {{QStringLiteral("tabId"), session->activeTabId()}, {QStringLiteral("key"), key},
                                    {QStringLiteral("code"), QString()},
                                    {QStringLiteral("modifiers"), QJsonArray::fromStringList(modifiers)}})) ==
          QStringLiteral("completed"));
  };
  const auto settle = [] {
    // Room for a second (wrong) action to run.
    for (int i = 0; i < 20; ++i) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
      QThread::msleep(5);
    }
  };
  const auto current = [&] { return qobject_cast<GeckoEngineView *>(window->currentEngineView()); };
  const auto hostActive = [&](GeckoEngineView *view) {
    return waitUntil([&] { return view && current() == view && session->activeTabId() == view->hostTabId(); });
  };
  const QStringList ctrl{QStringLiteral("Control")};
  window->activateWindow();
  window->switchTab(indexOfHostTab(window, a->hostTabId()));
  CHECK(hostActive(a));

  // Ctrl+T: one new tab, in this window.
  int tabs = window->tabCount();
  type(QStringLiteral("t"), ctrl);
  CHECK(waitUntil([&] { return window->tabCount() == tabs + 1; }));
  settle();
  CHECK(window->tabCount() == tabs + 1);
  GeckoEngineView *opened = current();
  CHECK(opened && opened != a && waitUntil([&] { return !opened->hostTabId().isEmpty(); }) && hostActive(opened));
  // Ctrl+W closes it (and only it).
  type(QStringLiteral("w"), ctrl);
  CHECK(waitUntil([&] { return window->tabCount() == tabs; }));
  settle();
  CHECK(window->tabCount() == tabs && indexOfHostTab(window, a->hostTabId()) >= 0);
  std::cout << "PASS Ctrl+T and Ctrl+W from the page: one action each" << std::endl;

  // Ctrl+Tab and Ctrl+Shift+Tab go to the next and previous tab.
  window->switchTab(indexOfHostTab(window, a->hostTabId()));
  CHECK(hostActive(a));
  const int aIndex = indexOfHostTab(window, a->hostTabId());
  GeckoEngineView *next = geckoView(window, (aIndex + 1) % window->tabCount());
  type(QStringLiteral("Tab"), ctrl);
  CHECK(hostActive(next));
  type(QStringLiteral("Tab"), {QStringLiteral("Control"), QStringLiteral("Shift")});
  CHECK(hostActive(a));
  std::cout << "PASS Ctrl+Tab and Ctrl+Shift+Tab from the page" << std::endl;

  // Ctrl+R reloads the page; Alt+Left goes back.
  int reloads = 0;
  QObject::connect(session, &GeckoSession::requestFinished, &app, [&](const QString &, const QString &, const QJsonObject &reply) {
    if (reply.value(QStringLiteral("command")).toString() == QLatin1String("reload")) ++reloads;
  });
  // (The fake host's one content process crashed every tab above: recover this one.)
  if (a->isCrashed()) a->reload();
  CHECK(waitUntil([&] { return !a->isCrashed(); }));
  type(QStringLiteral("r"), ctrl);
  CHECK(waitUntil([&] { return reloads == 1; }));
  const QUrl beforeA2 = a->url();
  a->load(QUrl(QStringLiteral("http://fake.test/a2")));
  CHECK(waitUntil([&] { return a->url() == QUrl(QStringLiteral("http://fake.test/a2")) && a->canGoBack(); }));
  type(QStringLiteral("ArrowLeft"), {QStringLiteral("Alt")});
  CHECK(waitUntil([&] { return a->url() == beforeA2; }));
  settle();
  CHECK(reloads == 1);
  std::cout << "PASS Ctrl+R and Alt+Left from the page" << std::endl;

  // Other keys are the page's: text and the reserved editing keys.
  type(QStringLiteral("c"), ctrl);
  type(QStringLiteral("x"), {});
  CHECK(waitUntil([&] { return a->title() == QStringLiteral("keys:Ctrl+C,X"); }));
  CHECK(window->tabCount() == tabs);
  // Ctrl+L: the address bar takes the keyboard; the page gets it back.
  auto *omnibox = window->findChild<QLineEdit *>(QStringLiteral("omnibox"));
  CHECK(omnibox);
  a->setFocus();
  CHECK(waitUntil([&] { return a->hasFocus(); }));
  type(QStringLiteral("l"), ctrl);
  CHECK(waitUntil([&] { return omnibox->hasFocus(); }));
  a->setFocus();
  CHECK(waitUntil([&] { return a->hasFocus(); }));
  std::cout << "PASS page keys stay the page's; Ctrl+L focuses the address bar" << std::endl;

  // Reports for a tab that is not the visible active one are dropped (a key
  // pressed just before a tab switch); so are reports while the window is hidden.
  GeckoEngineView *background = next;
  CHECK(background != a && session->activeTabId() == a->hostTabId());
  int sentBackground = 0;
  QObject::connect(session, &GeckoSession::shortcutPressed, &app, [&] { ++sentBackground; });
  background->load(QUrl(QStringLiteral("http://fake.test/shortcut-from-background")));
  CHECK(waitUntil([&] { return sentBackground == 1; }));
  settle();
  CHECK(window->tabCount() == tabs);
  window->hide();
  type(QStringLiteral("t"), ctrl);
  CHECK(waitUntil([&] { return sentBackground == 2; }));
  settle();
  CHECK(window->tabCount() == tabs);
  window->show();
  window->activateWindow();
  std::cout << "PASS shortcuts of a background tab or hidden window are dropped" << std::endl;

  // Rebinding takes effect at once: the old key goes to the page, the new one works.
  int published = 0;
  QObject::connect(router, &GeckoShortcutRouter::shortcutsPublished, &app, [&] { ++published; });
  CHECK(shortcutService.setShortcut(QStringLiteral("tabs.new"), keys(Qt::CTRL | Qt::SHIFT | Qt::Key_Y)).ok());
  CHECK(waitUntil([&] { return published == 1; }));
  CHECK(!router->hostShortcuts().contains(QStringLiteral("Ctrl+T")) && router->hostShortcuts().contains(QStringLiteral("Ctrl+Shift+Y")));
  type(QStringLiteral("t"), ctrl);
  CHECK(waitUntil([&] { return a->title() == QStringLiteral("keys:Ctrl+C,X,Ctrl+T"); }));
  CHECK(window->tabCount() == tabs);
  type(QStringLiteral("Y"), {QStringLiteral("Control"), QStringLiteral("Shift")});
  CHECK(waitUntil([&] { return window->tabCount() == tabs + 1; }));
  settle();
  CHECK(window->tabCount() == tabs + 1);
  GeckoEngineView *rebound = current();
  CHECK(rebound && waitUntil([&] { return !rebound->hostTabId().isEmpty(); }) && hostActive(rebound));
  type(QStringLiteral("w"), ctrl);
  CHECK(waitUntil([&] { return window->tabCount() == tabs; }));
  shortcutService.resetShortcut(QStringLiteral("tabs.new"));
  CHECK(waitUntil([&] { return published == 2 && router->hostShortcuts().contains(QStringLiteral("Ctrl+T")); }));
  std::cout << "PASS rebinding takes effect in the page at once" << std::endl;

  // M3-B4.1: failed close attempts never remove a live host document.
  for (const QString &fault : {QStringLiteral("close-error"), QStringLiteral("close-ipc-failure"),
                              QStringLiteral("close-timeout"), QStringLiteral("close-cancel"),
                              QStringLiteral("close-ack-without-close"), QStringLiteral("close-unload-refused"),
                              QStringLiteral("close-missing-tab"), QStringLiteral("close-missing-document"),
                              QStringLiteral("close-wrong-command"), QStringLiteral("close-wrong-document")}) {
    window->addNewTab(QUrl(QStringLiteral("http://fake.test/dirty-%1").arg(fault)));
    auto *v = current();
    CHECK(v && waitUntil([&] { return !v->hostTabId().isEmpty() && session->view(v->hostTabId())->hasBeforeUnload(); }));
    const QString id = v->hostTabId();
    const QString document = session->view(id)->documentId();
    int results = 0;
    auto sent = QObject::connect(session, &GeckoSession::requestSent, &app, [&](const QString &requestId) {
      const auto msg = GeckoSessionTestAccess::pending(session, requestId);
      if (fault == QLatin1String("close-cancel") && msg.value("type") == "close_tab" && msg.value("tabId") == id)
        session->cancel(requestId);
    });
    v->requestClose([&](bool allowed) { CHECK(!allowed); ++results; });
    // A second close while unresolved is denied, without another host command.
    if (fault == QLatin1String("close-timeout")) {
      bool duplicate = true;
      v->requestClose([&](bool allowed) { duplicate = allowed; });
      CHECK(!duplicate);
    }
    CHECK(waitUntil([&] { return results == 1; }));
    QObject::disconnect(sent);
    CHECK(session->view(id) && session->view(id)->documentId() == document && indexOfHostTab(window, id) >= 0);
    // No stuck pending flag: retry requires the real beforeunload decision.
    const int previousAsked = asked;
    discard = false;
    window->closeTab(indexOfHostTab(window, id));
    CHECK(waitUntil([&] { return asked == previousAsked + 1; }));
    CHECK(indexOfHostTab(window, id) >= 0 && session->view(id));
    discard = true;
    window->closeTab(indexOfHostTab(window, id));
    CHECK(waitUntil([&] { return !session->view(id) && indexOfHostTab(window, id) < 0; }));
    std::cout << "PASS fail-closed " << fault.toStdString() << "; retry remains guarded" << std::endl;
  }

  // Confirmation binds to the requesting document, not a later one.
  window->addNewTab(QUrl(QStringLiteral("http://fake.test/dirty-stale-confirmation")));
  auto *staleView = current();
  CHECK(staleView && waitUntil([&] { return !staleView->hostTabId().isEmpty() && session->view(staleView->hostTabId())->hasBeforeUnload(); }));
  const QString staleId = staleView->hostTabId();
  const auto original = session->view(staleId)->snapshot();
  const quint64 generation = session->generation();
  gecko->setDiscardConfirmation([&](QWidget *, const QString &) {
    auto changed = original;
    changed.insert("documentId", "replacement-document");
    GeckoSessionTestAccess::receive(session, generation, changed);
    return true;
  });
  bool staleDone = false;
  staleView->requestClose([&](bool allowed) { CHECK(!allowed); staleDone = true; });
  CHECK(waitUntil([&] { return staleDone; }));
  CHECK(session->view(staleId));
  GeckoSessionTestAccess::receive(session, generation, original);
  // Destroying a widget in a nested confirmation does not dereference it or
  // discard the host document. Its tab remains session-owned (not spare/reused).
  auto *detached = qobject_cast<GeckoEngineView *>(gecko->createView(window));
  detached->load(QUrl(QStringLiteral("http://fake.test/dirty-destroy")));
  CHECK(waitUntil([&] { return !detached->hostTabId().isEmpty() && session->view(detached->hostTabId())->hasBeforeUnload(); }));
  const QString detachedId = detached->hostTabId();
  QPointer<GeckoEngineView> detachedGuard(detached);
  bool detachedCallback = false;
  gecko->setDiscardConfirmation([&](QWidget *widget, const QString &) { delete widget; return true; });
  detached->requestClose([&](bool) { detachedCallback = true; });
  CHECK(waitUntil([&] { return detachedGuard.isNull(); }, 100));
  settle();
  CHECK(!detachedCallback && session->view(detachedId));
  gecko->setDiscardConfirmation([&](QWidget *, const QString &) { ++asked; return discard; });
  discard = true;
  window->closeTab(indexOfHostTab(window, staleId));
  CHECK(waitUntil([&] { return !session->view(staleId); }));
  std::cout << "PASS document-bound confirmation and destroyed-widget retention" << std::endl;

  // Destruction before asynchronous creation neither force-closes nor reuses
  // the resulting host document.
  QPointer<EngineView> unbound(gecko->createView(window));
  unbound->load(QUrl(QStringLiteral("http://fake.test/dirty-orphan")));
  auto disposeOnSend = QObject::connect(session, &GeckoSession::requestSent, &app, [&](const QString &id) {
    const auto msg = GeckoSessionTestAccess::pending(session, id);
    if (msg.value("type") == "create_tab" && msg.value("url") == "http://fake.test/dirty-orphan") delete unbound.data();
  });
  CHECK(waitUntil([&] { return unbound.isNull(); }));
  QObject::disconnect(disposeOnSend);
  CHECK(waitUntil([&] {
    for (auto *v : session->views()) if (v->url() == QUrl("http://fake.test/dirty-orphan")) return v->hasBeforeUnload();
    return false;
  }));
  CHECK(session->state() == GeckoSession::State::Ready);
  std::cout << "PASS asynchronous widget disposal retains the newly created document" << std::endl;

  auto *cancelledCreation = qobject_cast<GeckoEngineView *>(gecko->createView(window));
  cancelledCreation->load(QUrl("http://fake.test/dirty-late-creation"));
  auto cancelCreation = QObject::connect(session, &GeckoSession::requestSent, &app, [&](const QString &id) {
    if (GeckoSessionTestAccess::pending(session, id).value("url") == "http://fake.test/dirty-late-creation") session->cancel(id);
  });
  QString lateId;
  CHECK(waitUntil([&] {
    for (auto *v : session->views()) if (v->url() == QUrl("http://fake.test/dirty-late-creation")) { lateId = v->tabId(); return true; }
    return false;
  }));
  QObject::disconnect(cancelCreation);
  CHECK(cancelledCreation->hostTabId().isEmpty());
  delete cancelledCreation;
  auto *fresh = qobject_cast<GeckoEngineView *>(gecko->createView(window));
  CHECK(waitUntil([&] { return !fresh->hostTabId().isEmpty(); }));
  CHECK(fresh->hostTabId() != lateId && session->view(lateId)->hasBeforeUnload());
  delete fresh;
  CHECK(session->view(lateId));
  std::cout << "PASS cancelled late creation retained and not recycled into a different widget" << std::endl;

  // A stale/forged shutdown acknowledgement cannot authorize cleanup.
  GeckoSessionTestAccess::receive(session, generation,
      {{"type", "ack"}, {"command", "shutdown"}, {"requestId", "nobody-sent-this"}});
  CHECK(session->state() == GeckoSession::State::Ready);
  discard = false;
  bool shutdownAnswered = false;
  gecko->requestShutdown(window, [&](bool allowed) { CHECK(!allowed); shutdownAnswered = true; });
  CHECK(waitUntil([&] { return shutdownAnswered; }));
  CHECK(session->hostPid() > 0 && session->view(detachedId));
  window->close();
  settle();
  CHECK(window->isVisible() && session->state() == GeckoSession::State::Ready);
  QEvent quit(QEvent::Quit);
  QCoreApplication::sendEvent(&app, &quit);
  settle();
  CHECK(session->state() == GeckoSession::State::Ready && window->isVisible());
  const int asksBeforeQuit = asked;
  bool guardedLoopStayedAlive = false;
  QTimer::singleShot(0, &app, &QCoreApplication::quit);
  QTimer::singleShot(300, &app, [&] { guardedLoopStayedAlive = true; app.exit(7); });
  CHECK(app.exec() == 7 && guardedLoopStayedAlive && asked > asksBeforeQuit);
  CHECK(window->isVisible() && session->state() == GeckoSession::State::Ready);
  discard = true;
  std::cout << "PASS shutdown/window/application quit denial keeps the live session" << std::endl;

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
  // No host, no shortcuts: nothing of the old set is acted on.
  CHECK(router->hostShortcuts().isEmpty());
  delete window;
  // Real transport loss and host death: deny while the live parent remains;
  // permit disposal only after it has definitively exited. A new launch cannot
  // make old widget/request identities target its tabs.
  {
    auto extraConfig = config;
    extraConfig.dataDirectory = base + QStringLiteral("/extra-gecko");
    GeckoBackend extra(extraConfig, &app);
    extra.setDiscardConfirmation([](QWidget *, const QString &) { return false; });
    CHECK(extra.initialize(storage));
    auto *extraSession = extra.session();
    CHECK(waitUntil([&] { return extraSession->state() == GeckoSession::State::Ready; }));
    auto *v = qobject_cast<GeckoEngineView *>(extra.createView(nullptr));
    v->load(QUrl("http://fake.test/dirty-close-timeout"));
    CHECK(waitUntil([&] { return !v->hostTabId().isEmpty() && extraSession->view(v->hostTabId())->hasBeforeUnload(); }));
    bool answered = false;
    auto disconnectOnClose = QObject::connect(extraSession, &GeckoSession::requestSent, &app, [&](const QString &id) {
      if (GeckoSessionTestAccess::pending(extraSession, id).value("type") == "close_tab")
        GeckoSessionTestAccess::disconnect(extraSession);
    });
    v->requestClose([&](bool allowed) { CHECK(!allowed); answered = true; });
    CHECK(waitUntil([&] { return answered; }));
    QObject::disconnect(disconnectOnClose);
    CHECK(waitUntil([&] { return !extraSession->hostIsRunning(); }));
    bool ceased = false;
    v->requestClose([&](bool allowed) { ceased = allowed; });
    CHECK(ceased && extra.shutdownHost(1000));
    const quint64 oldGeneration = extraSession->generation();
    CHECK(extra.initialize(storage));
    CHECK(waitUntil([&] { return extraSession->state() == GeckoSession::State::Ready; }));
    GeckoSessionTestAccess::receive(extraSession, oldGeneration,
        {{"type", "ack"}, {"command", "shutdown"}, {"requestId", "stale-generation"}});
    CHECK(extraSession->state() == GeckoSession::State::Ready);
    bool oldAllowed = true;
    bool oldAnswered = false;
    v->requestClose([&](bool allowed) { oldAllowed = allowed; oldAnswered = true; });
    CHECK(waitUntil([&] { return oldAnswered; }) && !oldAllowed);
    delete v;
    auto *live = qobject_cast<GeckoEngineView *>(extra.createView(nullptr));
    live->load(QUrl("http://fake.test/dirty-host-crash"));
    CHECK(waitUntil([&] { return !live->hostTabId().isEmpty() && extraSession->view(live->hostTabId())->hasBeforeUnload(); }));
    const qint64 ownedHost = extraSession->hostPid();
    CHECK(ownedHost > 0 && ::kill(static_cast<pid_t>(ownedHost), SIGKILL) == 0);
    CHECK(waitUntil([&] { return !extraSession->hostIsRunning(); }));
    bool deadAllowed = false;
    live->requestClose([&](bool allowed) { deadAllowed = allowed; });
    CHECK(deadAllowed && extra.shutdownHost(1000));
    delete live;
    std::cout << "PASS IPC failure, definitive host death, and stale session generation" << std::endl;
  }
  EngineBackend::shutdown();
  std::cout << "PASS exit: no host process, run directory removed, profile kept" << std::endl;
  std::cout << "=== ALL GECKO BROWSERWINDOW TESTS PASSED ===" << std::endl;
  return 0;
}
