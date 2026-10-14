// Windowing / input compatibility of both engines on X11, XWayland and native
// Wayland, in the real BrowserWindow.
//
// --logic (default tier, no window): engine/display_platform's backend plan for
//   every session / user-choice combination.
// --gui (GUI tier, scripts/safe-gui-test.sh): one child process per engine,
//   run one after another, as DaliNira runs one engine per process. Each child
//   applies the same display plan as main.cpp and checks the backend it got
//   against the Qt platform the test runs under (QT_QPA_PLATFORM=xcb: X11 or
//   XWayland for both engines; QT_QPA_PLATFORM=wayland: Chromium on native
//   Wayland, WebKit on XWayland). Local pages only. Cases: startup, open /
//   close tab, switching tabs, omnibox <-> page focus and keyboard input,
//   resize, maximize / restore, fullscreen enter / exit, context menu popup,
//   a second monitor when there is one, and no fatal X error (a GDK
//   XSetInputFocus BadMatch exits the process) on stdout/stderr.

#include <memory>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QProcess>
#include <QScreen>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QWebEnginePage>
#include <QWebEngineView>
#include <QWindow>
#include <functional>
#include <iostream>

#include "browser_window.h"
#include "core/application_identity.h"
#include "core/browser_profile_service.h"
#include "desktop_tabs/tab_manager.h"
#include "desktop_tabs/tab_performance_manager.h"
#include "engine/chromium/chromium_backend.h"
#include "engine/display_platform.h"
#include "engine/engine_backend.h"
#include "engine/engine_storage_manager.h"
#include "engine/engine_view.h"
#include "newtab/new_tab_scheme.h"
#include "session/session_store.h"
#if defined(DALINIRA_BUILD_WEBKIT)
#include "engine/webkit/webkit_backend.h"
#include <X11/Xlib.h>
#if defined(DALINIRA_TEST_XTEST)
#include <X11/extensions/XTest.h>
#endif
#undef KeyPress
#undef KeyRelease
#undef FocusIn
#undef FocusOut
#undef None
#undef Bool
#undef Status
#endif

#define CHECK(condition)                                                                    \
  do {                                                                                      \
    if (!(condition)) {                                                                     \
      std::cout << "[FAIL] " << __FILE__ << ":" << __LINE__ << ": " #condition << std::endl; \
      std::_Exit(1);                                                                        \
    }                                                                                       \
  } while (0)

namespace {

using dalinira::engine::DisplayEnvironment;
using dalinira::engine::DisplayPlan;
using dalinira::engine::EngineType;
using dalinira::engine::planDisplay;

// ---------------------------------------------------------------------------
// Logic tier
// ---------------------------------------------------------------------------

DisplayEnvironment env(const char *session, const char *wayland, const char *x11, const char *qpa = "",
                       const char *gdk = "") {
  return {session, wayland, x11, qpa, gdk};
}

int runLogic() {
  // X11 session: nothing to override; GDK follows Qt for Chromium's dialogs.
  DisplayPlan plan = planDisplay(EngineType::Chromium, env("x11", "", ":0"));
  CHECK(plan.engineSupported && plan.qpaPlatform.isEmpty() && plan.gdkBackend.isEmpty() && plan.note.isEmpty());
  plan = planDisplay(EngineType::WebKit, env("x11", "", ":0"));
  CHECK(plan.engineSupported && plan.qpaPlatform == "xcb" && plan.gdkBackend == "x11" && plan.note.isEmpty());

  // Wayland session with XWayland: Chromium prefers XWayland unless the user
  // chose a platform; an explicit native Wayland choice is honoured.
  plan = planDisplay(EngineType::Chromium, env("wayland", "wayland-0", ":0"));
  CHECK(plan.engineSupported && plan.qpaPlatform == "xcb" && plan.gdkBackend == "x11");
  plan = planDisplay(EngineType::Chromium, env("wayland", "wayland-0", ":0", "wayland"));
  CHECK(plan.engineSupported && plan.qpaPlatform.isEmpty() && plan.gdkBackend == "wayland");
  plan = planDisplay(EngineType::Chromium, env("wayland", "wayland-0", ":0", "wayland;xcb", "wayland"));
  CHECK(plan.qpaPlatform.isEmpty() && plan.gdkBackend.isEmpty());
  // Wayland without XWayland: Qt picks native Wayland by itself.
  plan = planDisplay(EngineType::Chromium, env("wayland", "wayland-0", ""));
  CHECK(plan.engineSupported && plan.qpaPlatform.isEmpty() && plan.gdkBackend == "wayland");
  // XDG_SESSION_TYPE missing (e.g. a nested session): WAYLAND_DISPLAY decides.
  plan = planDisplay(EngineType::Chromium, env("", "wayland-1", ":1"));
  CHECK(plan.qpaPlatform == "xcb");

  // WebKit is always XEmbed: XWayland on Wayland, even when the user asked
  // for native Wayland, and unavailable without any X server.
  plan = planDisplay(EngineType::WebKit, env("wayland", "wayland-0", ":0"));
  CHECK(plan.engineSupported && plan.qpaPlatform == "xcb" && plan.gdkBackend == "x11" && plan.note.isEmpty());
  plan = planDisplay(EngineType::WebKit, env("wayland", "wayland-0", ":0", "wayland", "wayland"));
  CHECK(plan.engineSupported && plan.qpaPlatform == "xcb" && plan.gdkBackend == "x11" && !plan.note.isEmpty());
  plan = planDisplay(EngineType::WebKit, env("wayland", "wayland-0", ":0", "xcb", "x11,wayland"));
  CHECK(plan.qpaPlatform.isEmpty() && plan.gdkBackend.isEmpty());
  plan = planDisplay(EngineType::WebKit, env("wayland", "wayland-0", ""));
  CHECK(!plan.engineSupported && !plan.note.isEmpty());
  // Headless tooling keeps its platform.
  plan = planDisplay(EngineType::WebKit, env("wayland", "wayland-0", "", "offscreen"));
  CHECK(plan.engineSupported && plan.qpaPlatform.isEmpty());
  // Gecko: its host's X11 window is embedded: Qt on xcb (XWayland), never
  // native Wayland; no X server at all means Chromium.
  plan = planDisplay(EngineType::Gecko, env("x11", "", ":0"));
  CHECK(plan.engineSupported && plan.qpaPlatform == "xcb" && plan.gdkBackend == "x11");
  plan = planDisplay(EngineType::Gecko, env("wayland", "wayland-0", ":0", "wayland"));
  CHECK(plan.engineSupported && plan.qpaPlatform == "xcb" && !plan.note.isEmpty());
  plan = planDisplay(EngineType::Gecko, env("wayland", "wayland-0", ""));
  CHECK(!plan.engineSupported && !plan.note.isEmpty());
  plan = planDisplay(EngineType::Gecko, env("wayland", "wayland-0", "", "offscreen"));
  CHECK(plan.engineSupported && plan.qpaPlatform.isEmpty());
  std::cout << "[PASS] display platform plan" << std::endl;
  return 0;
}

// ---------------------------------------------------------------------------
// GUI tier: one engine phase
// ---------------------------------------------------------------------------

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

void settle(int ms) {
  waitUntil([] { return false; }, ms);
}

// A tab's page widget: the EngineView of a WebKit tab, the QWebEngineView of
// a Chromium tab.
QWidget *pageWidget(BrowserWindow &window, int index) {
  const BrowserTabInfo &info = window.tabInfo(index);
  if (info.engineView) return info.engineView.data();
  return info.view.data();
}

QWidget *currentPage(BrowserWindow &window) {
  if (auto *engineView = window.currentEngineView()) return engineView;
  return window.currentView();
}

bool isActivePage(QWidget *page) {
  if (auto *engineView = qobject_cast<dalinira::engine::EngineView *>(page)) return engineView->isEngineActive();
  return page->isVisible();
}

QVariant evaluate(QWidget *view, const QString &script) {
  // A result can arrive after the timeout below; the callback owns its state.
  const auto state = std::make_shared<std::pair<bool, QVariant>>(false, QVariant());
  const auto callback = [state](const QVariant &result) {
    state->second = result;
    state->first = true;
  };
  if (auto *engineView = qobject_cast<dalinira::engine::EngineView *>(view)) engineView->runJavaScript(script, callback);
  else if (auto *webView = qobject_cast<QWebEngineView *>(view)) webView->page()->runJavaScript(script, callback);
  else return {};
  if (!waitUntil([&] { return state->first; }, 10000)) return {};
  return state->second;
}

bool pageReady(BrowserWindow &window, int index, const QString &title) {
  return waitUntil([&] {
    return index < window.tabCount() && window.tabInfo(index).title == title && !window.tabInfo(index).isLoading;
  }, 20000);
}

bool focusInside(QWidget *widget) {
  QWidget *focus = QApplication::focusWidget();
  return focus && (focus == widget || widget->isAncestorOf(focus));
}

// The page's CSS viewport follows the view (resize, maximize, fullscreen).
bool viewportFollows(QWidget *view) {
  return waitUntil([&] {
    const int inner = evaluate(view, QStringLiteral("window.innerWidth")).toInt();
    return inner > 0 && qAbs(inner - view->width()) <= 2;
  }, 8000);
}

#if defined(DALINIRA_BUILD_WEBKIT)
// Where the X server's keyboard focus is, relative to a WebKit tab's host.
class XFocusProbe {
 public:
  XFocusProbe() : display_(XOpenDisplay(nullptr)) {}
  ~XFocusProbe() {
    if (display_) XCloseDisplay(display_);
  }
  bool valid() const { return display_; }
  unsigned long focusWindow() const {
    Window focused = 0;
    int revert = 0;
    if (display_) XGetInputFocus(display_, &focused, &revert);
    return focused;
  }
  bool insideWindow(WId host) const {
    if (!display_ || !host) return false;
    Window focused = 0;
    int revert = 0;
    XGetInputFocus(display_, &focused, &revert);
    const Window rootWindow = DefaultRootWindow(display_);
    for (Window w = focused; w > 1 && w != rootWindow;) {  // 0: None, 1: PointerRoot
      if (w == static_cast<Window>(host)) return true;
      Window root = 0, parent = 0, *children = nullptr;
      unsigned int count = 0;
      if (!XQueryTree(display_, w, &root, &parent, &children, &count)) return false;
      if (children) XFree(children);
      w = parent;
    }
    return false;
  }
#if defined(DALINIRA_TEST_XTEST)
  // Real key events through the X server, typed only while the X keyboard
  // focus is verified to be inside `target`, so nothing reaches other windows.
  bool type(const char *text, WId target) {
    int event = 0, error = 0, major = 0, minor = 0;
    if (!display_ || !XTestQueryExtension(display_, &event, &error, &major, &minor)) return false;
    for (const char *c = text; *c; ++c) {
      if (!insideWindow(target)) return false;
      const char keyName[2] = {*c, 0};
      const KeyCode code = XKeysymToKeycode(display_, XStringToKeysym(keyName));
      if (!code) return false;
      XTestFakeKeyEvent(display_, code, True, CurrentTime);
      XTestFakeKeyEvent(display_, code, False, CurrentTime);
      XSync(display_, False);
      settle(40);
    }
    return true;
  }
#endif

 private:
  Display *display_;
};
#endif

void typeIntoQtFocus(const QString &text) {
  for (const QChar ch : text) {
    QWidget *target = QApplication::focusWidget();
    CHECK(target);
    const int key = Qt::Key_A + (ch.toUpper().unicode() - 'A');
    QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier, QString(ch));
    QKeyEvent release(QEvent::KeyRelease, key, Qt::NoModifier, QString(ch));
    QApplication::sendEvent(target, &press);
    QApplication::sendEvent(target, &release);
    settle(20);
  }
}

// Shows the page context menu the engines route to BrowserWindow and checks
// that the popup maps on this platform, then dismisses it.
bool contextMenuMaps(BrowserWindow &window, QWidget *view) {
  bool mapped = false;
  QTimer poll;
  QElapsedTimer elapsed;
  elapsed.start();
  QObject::connect(&poll, &QTimer::timeout, [&] {
    auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
    if (menu && menu->isVisible() && menu->windowHandle() && menu->windowHandle()->isExposed()) {
      mapped = !menu->actions().isEmpty();
      menu->close();
    } else if (elapsed.elapsed() > 5000 && menu) {
      menu->close();
    }
  });
  poll.start(50);
  window.showPageContextMenu(view->mapToGlobal(view->rect().center()));
  return mapped;
}

const char *kPage =
    "<!doctype html><title>%1</title>"
    "<style>body{margin:0;background:#123}input{font-size:20px}</style>"
    "<input id=t value=''><p>%1</p>";

int runPhase(const QString &engine, const QString &expectedBackend, const QString &base, int argc, char *argv[]) {
  const bool webkit = engine == QLatin1String("webkit");
  const EngineType engineType = webkit ? EngineType::WebKit : EngineType::Chromium;
  const DisplayPlan plan = planDisplay(engineType, DisplayEnvironment::fromProcess());
  if (!plan.engineSupported) {
    std::cout << "[SKIP] " << plan.note.toStdString() << std::endl;
    return 77;
  }
  dalinira::engine::applyDisplayPlan(plan);
  dalinira::application_identity::apply();
  dalinira::engine::EngineBackend::setSelectedEngineType(engineType);
  if (webkit) QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
  else registerDaliNiraUrlSchemes();
  QApplication app(argc, argv);
  dalinira::application_identity::apply();
  const QString tag = QStringLiteral("[%1/%2]").arg(engine, dalinira::engine::activeDisplayBackend());
  const auto log = [&](const QString &text) { std::cout << tag.toStdString() << ' ' << text.toStdString() << std::endl; };
  log(QStringLiteral("platform %1, plan note: %2").arg(QGuiApplication::platformName(),
                                                       plan.note.isEmpty() ? QStringLiteral("-") : plan.note));
  CHECK(dalinira::engine::activeDisplayBackend() == expectedBackend);

  const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  QDir().mkpath(dataDir);
  BrowserProfileService profileService(dataDir, nullptr, &app);
  dalinira::engine::EngineStorageManager engineStorage(dataDir);
  if (!webkit) {
    auto backend = std::make_unique<dalinira::engine::ChromiumBackend>(profileService.profile(), &app);
    CHECK(backend->initialize(engineStorage));
    dalinira::engine::EngineBackend::registerBackend(EngineType::Chromium, std::move(backend));
  }
#if defined(DALINIRA_BUILD_WEBKIT)
  else {
    auto backend = std::make_unique<dalinira::engine::WebKitBackend>(&app);
    CHECK(backend->initialize(engineStorage));
    dalinira::engine::EngineBackend::registerBackend(EngineType::WebKit, std::move(backend));
  }
  XFocusProbe xfocus;
  if (webkit) CHECK(xfocus.valid());
#else
  else {
    std::cout << "[SKIP] built without WebKit" << std::endl;
    return 77;
  }
#endif

  TabManager tabManager(&app);
  tabManager.performanceManager()->setPolicyMode(dalinira::PerformancePolicyMode::Off);
  SessionStore sessionStore(dataDir, engine);
  BrowserServices services;
  services.profile = webkit ? nullptr : profileService.profile();
  services.profileService = &profileService;
  services.tabManager = &tabManager;
  services.sessionStore = &sessionStore;

  // Startup and window activation.
  auto *window = new BrowserWindow(services);
  window->resize(1000, 700);
  window->show();
  window->ensureInitialTab();
  CHECK(waitUntil([&] { return window->windowHandle() && window->windowHandle()->isExposed(); }, 10000));
  window->activateWindow();
  const bool active = waitUntil([&] { return window->isActiveWindow(); }, 5000);
  log(active ? QStringLiteral("window active") : QStringLiteral("[NOTE] compositor did not activate the window"));

  // Open tabs, open + close one more.
  window->navigateCurrentTab(QUrl(base + QStringLiteral("/a")));
  CHECK(pageReady(*window, 0, QStringLiteral("Page A")));
  window->addNewTab(QUrl(base + QStringLiteral("/b")));
  CHECK(pageReady(*window, 1, QStringLiteral("Page B")));
  window->addNewTab(QUrl(base + QStringLiteral("/c")));
  CHECK(pageReady(*window, 2, QStringLiteral("Page C")));
  window->closeTab(2);
  CHECK(waitUntil([&] { return window->tabCount() == 2; }, 5000));
  log(QStringLiteral("PASS startup, open/close tab"));

  auto *omnibox = window->findChild<QLineEdit *>(QStringLiteral("omnibox"));
  CHECK(omnibox);
  CHECK(omnibox->width() >= window->width() / 3);
  CHECK(omnibox->width() <= 1100);
  auto *bookmarkButton = window->findChild<QWidget *>(QStringLiteral("bookmark-button"));
  CHECK(bookmarkButton);
  CHECK(bookmarkButton->parentWidget() == omnibox->parentWidget());
  CHECK(omnibox->x() - bookmarkButton->geometry().right() <= 16);

  // Tab switching, with focus moving between omnibox and page each time.
  for (int round = 0; round < 6; ++round) {
    const int index = round % 2 == 0 ? 0 : 1;
    window->switchTab(index);
    QWidget *view = currentPage(*window);
    CHECK(view && view == pageWidget(*window, index));
    CHECK(waitUntil([&] { return view->isVisible() && isActivePage(view); }, 5000));
    // WebKit reports the page visibility to the web process asynchronously.
    CHECK(waitUntil([&] {
      return evaluate(view, QStringLiteral("document.visibilityState")).toString() == QLatin1String("visible");
    }, 5000));
    omnibox->setFocus(Qt::ShortcutFocusReason);
    CHECK(waitUntil([&] { return QApplication::focusWidget() == omnibox; }, 3000));
    view->setFocus(Qt::MouseFocusReason);
    CHECK(waitUntil([&] { return focusInside(view); }, 3000));
#if defined(DALINIRA_BUILD_WEBKIT)
    if (webkit) CHECK(waitUntil([&] { return xfocus.insideWindow(view->internalWinId()); }, 3000));
#endif
  }
  CHECK(!isActivePage(pageWidget(*window, 0)));  // tab 1 is current
  log(QStringLiteral("PASS tab switching with focus transfer"));

  QWidget *view = currentPage(*window);
  CHECK(view);

  // Omnibox first: keys reach the omnibox and never the page.
  CHECK(evaluate(view, QStringLiteral("document.getElementById('t').focus(); document.activeElement.id")).toString() ==
        QLatin1String("t"));
  omnibox->setFocus(Qt::ShortcutFocusReason);
  omnibox->clear();
  CHECK(waitUntil([&] { return QApplication::focusWidget() == omnibox; }, 3000));
#if defined(DALINIRA_BUILD_WEBKIT)
  if (webkit) {
    // The X keyboard focus left the GTK surface.
    CHECK(waitUntil([&] { return !xfocus.insideWindow(view->internalWinId()); }, 3000));
    CHECK(waitUntil([&] { return !evaluate(view, QStringLiteral("document.hasFocus()")).toBool(); }, 3000));
  }
#endif
  typeIntoQtFocus(QStringLiteral("omni"));
  CHECK(omnibox->text() == QLatin1String("omni"));
  QString omniboxText = omnibox->text();
#if defined(DALINIRA_BUILD_WEBKIT) && defined(DALINIRA_TEST_XTEST)
  if (webkit && active && xfocus.insideWindow(window->internalWinId())) {
    // Real X key events while Qt owns the focus go to the omnibox, not WebKit.
    CHECK(xfocus.type("x", window->internalWinId()));
    omniboxText += QLatin1Char('x');
    if (!waitUntil([&] { return omnibox->text() == omniboxText; }, 3000)) {
      QWidget *popup = QApplication::activePopupWidget();
      std::cout << "[DIAG] omnibox='" << omnibox->text().toStdString() << "' focusWidget="
                << (QApplication::focusWidget() ? QApplication::focusWidget()->metaObject()->className() : "null")
                << " popup=" << (popup ? popup->metaObject()->className() : "null")
                << " xfocus=0x" << std::hex << xfocus.focusWindow() << " top=0x" << window->internalWinId()
                << " host=0x" << view->internalWinId() << std::dec
                << " page='" << evaluate(view, QStringLiteral("document.getElementById('t').value")).toString().toStdString()
                << "'" << std::endl;
      CHECK(false && "real X key while the omnibox has focus");
    }
    CHECK(evaluate(view, QStringLiteral("document.getElementById('t').value")).toString().isEmpty());
  }
#endif

  // Then the page: focus follows and keys reach the page's text field.
  view->setFocus(Qt::MouseFocusReason);
  CHECK(waitUntil([&] { return focusInside(view); }, 3000));
  CHECK(evaluate(view, QStringLiteral("document.getElementById('t').focus(); document.activeElement.id")).toString() ==
        QLatin1String("t"));
  if (!webkit) {
    typeIntoQtFocus(QStringLiteral("dali"));
    CHECK(omnibox->text() == omniboxText);
    CHECK(waitUntil([&] {
      return evaluate(view, QStringLiteral("document.getElementById('t').value")).toString() == QLatin1String("dali");
    }, 3000));
    log(QStringLiteral("PASS omnibox then page focus, keyboard input"));
  }
#if defined(DALINIRA_BUILD_WEBKIT)
  else {
    CHECK(waitUntil([&] { return xfocus.insideWindow(view->internalWinId()); }, 3000));
    CHECK(waitUntil([&] { return evaluate(view, QStringLiteral("document.hasFocus()")).toBool(); }, 3000));
#if defined(DALINIRA_TEST_XTEST)
    if (active) {
      CHECK(xfocus.type("dali", view->internalWinId()));
      CHECK(waitUntil([&] {
        return evaluate(view, QStringLiteral("document.getElementById('t').value")).toString() == QLatin1String("dali");
      }, 3000));
      CHECK(omnibox->text() == omniboxText);
      log(QStringLiteral("PASS omnibox then page focus, real X keyboard input"));
    } else {
      log(QStringLiteral("PASS omnibox then page focus ([NOTE] window inactive: real key input skipped)"));
    }
#else
    log(QStringLiteral("PASS omnibox then page focus (built without XTest: real key input skipped)"));
#endif
  }
#endif

  // Resize.
  const QSize before = view->size();
  window->resize(820, 600);
  CHECK(waitUntil([&] { return view->size() != before; }, 5000));
  CHECK(viewportFollows(view));
  log(QStringLiteral("PASS resize"));

  // Maximize / restore.
  const QSize normalSize = window->size();
  window->showMaximized();
  CHECK(waitUntil([&] { return window->isMaximized(); }, 5000));
  waitUntil([&] { return window->size() != normalSize; }, 3000);
  CHECK(viewportFollows(view));
  window->showNormal();
  CHECK(waitUntil([&] { return !window->isMaximized(); }, 5000));
  CHECK(viewportFollows(view));
  log(QStringLiteral("PASS maximize/restore"));

  // Fullscreen enter / exit (the shared page fullscreen path).
  window->setPageFullScreen(true);
  CHECK(waitUntil([&] { return window->isFullScreen(); }, 5000));
  CHECK(waitUntil([&] { return view->width() >= window->screen()->geometry().width() - 2; }, 5000));
  CHECK(viewportFollows(view));
  window->setPageFullScreen(false);
  CHECK(waitUntil([&] { return !window->isFullScreen(); }, 5000));
  CHECK(viewportFollows(view));
  view->setFocus(Qt::MouseFocusReason);
  CHECK(waitUntil([&] { return focusInside(view); }, 3000));
  log(QStringLiteral("PASS fullscreen enter/exit"));

  // Context menu popup.
  CHECK(contextMenuMaps(*window, view));
  CHECK(waitUntil([&] { return !QApplication::activePopupWidget(); }, 3000));
  log(QStringLiteral("PASS context menu popup"));

  // Moving between monitors; clients cannot position themselves on Wayland.
  const QList<QScreen *> screens = QGuiApplication::screens();
  if (screens.size() > 1 && expectedBackend != QLatin1String("wayland")) {
    QScreen *other = screens.first() == window->screen() ? screens.at(1) : screens.first();
    window->move(other->availableGeometry().topLeft() + QPoint(40, 40));
    CHECK(waitUntil([&] { return window->screen() == other; }, 5000));
    CHECK(viewportFollows(view));
    log(QStringLiteral("PASS moved to monitor %1").arg(other->name()));
  } else {
    log(QStringLiteral("[SKIP] monitor move (%1 screen(s))").arg(screens.size()));
  }

  // Close a tab: focus moves to the remaining page.
  window->closeTab(1);
  CHECK(waitUntil([&] { return window->tabCount() == 1; }, 5000));
  CHECK(currentPage(*window) && waitUntil([&] { return currentPage(*window)->isVisible(); }, 3000));
  CHECK(evaluate(currentPage(*window), QStringLiteral("document.title")).toString() == QLatin1String("Page A"));
  log(QStringLiteral("PASS tab close"));

  window->close();
  delete window;
  dalinira::engine::EngineBackend::shutdown();
  log(QStringLiteral("PASS"));
  return 0;
}

class LocalSite {
 public:
  LocalSite() {
    CHECK(server_.listen(QHostAddress::LocalHost));
    QObject::connect(&server_, &QTcpServer::newConnection, [this] {
      while (QTcpSocket *socket = server_.nextPendingConnection()) {
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket] {
          const QByteArray path = socket->readAll().split(' ').value(1);
          static const QHash<QByteArray, QByteArray> titles{{"/a", "Page A"}, {"/b", "Page B"}, {"/c", "Page C"}};
          const QByteArray title = titles.value(path);
          const QByteArray status = title.isEmpty() ? "404 Not Found" : "200 OK";
          const QByteArray body = QString::fromLatin1(kPage).arg(QString::fromLatin1(title)).toUtf8();
          socket->write("HTTP/1.1 " + status + "\r\nContent-Type: text/html\r\nConnection: close\r\nContent-Length: " +
                        QByteArray::number(body.size()) + "\r\n\r\n" + body);
          socket->disconnectFromHost();
        });
        QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
      }
    });
  }
  QString base() const { return QStringLiteral("http://127.0.0.1:%1").arg(server_.serverPort()); }

 private:
  QTcpServer server_;
};

int runGui(int argc, char *argv[]) {
  QCoreApplication app(argc, argv);
  const DisplayEnvironment display = DisplayEnvironment::fromProcess();
  const QByteArray qpa = display.qpaPlatform.split(';').first().trimmed();
  const bool waylandSession = display.session() == dalinira::engine::DisplayServer::Wayland;
  if (qpa != "xcb" && qpa != "wayland") {
    std::cout << "[SKIP] QT_QPA_PLATFORM must be xcb or wayland" << std::endl;
    return 77;
  }
  if (qpa == "wayland" && display.waylandDisplay.isEmpty()) {
    std::cout << "[SKIP] not a Wayland session" << std::endl;
    return 77;
  }
  const QString xBackend = waylandSession ? QStringLiteral("xwayland") : QStringLiteral("x11");
  QTemporaryDir home;
  CHECK(home.isValid());
  LocalSite site;
  std::cout << "=== WINDOW PLATFORM COMPATIBILITY (QT_QPA_PLATFORM=" << qpa.constData() << ") ===" << std::endl;
  // ctest hides a passing test's output; keep the per-case results.
  const QString logDir = QCoreApplication::applicationDirPath() + QStringLiteral("/gui-test-logs");
  QDir().mkpath(logDir);
  QFile log(logDir + QStringLiteral("/window-platform-%1.log").arg(QString::fromLatin1(qpa)));
  CHECK(log.open(QIODevice::WriteOnly | QIODevice::Truncate));
  QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
  environment.insert(QStringLiteral("XDG_CONFIG_HOME"), home.filePath(QStringLiteral("config")));
  environment.insert(QStringLiteral("XDG_DATA_HOME"), home.filePath(QStringLiteral("data")));
  environment.insert(QStringLiteral("XDG_CACHE_HOME"), home.filePath(QStringLiteral("cache")));

  struct Phase {
    QString engine;
    QString backend;
  };
  QList<Phase> phases{{QStringLiteral("chromium"), qpa == "wayland" ? QStringLiteral("wayland") : xBackend}};
#if defined(DALINIRA_BUILD_WEBKIT)
  phases.append({QStringLiteral("webkit"), xBackend});  // WebKit is always XEmbed (XWayland on Wayland)
#endif
  for (const Phase &phase : phases) {
    QProcess child;
    child.setProcessEnvironment(environment);
    child.setProcessChannelMode(QProcess::MergedChannels);
    QByteArray output;
    QObject::connect(&child, &QProcess::readyRead, [&] {
      const QByteArray chunk = child.readAll();
      output += chunk;
      log.write(chunk);
      log.flush();
      std::cout << chunk.constData() << std::flush;
    });
    QEventLoop loop;  // keeps the local site serving while the phase runs
    QObject::connect(&child, &QProcess::finished, &loop, &QEventLoop::quit);
    QObject::connect(&child, &QProcess::errorOccurred, &loop, &QEventLoop::quit);
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    child.start(QCoreApplication::applicationFilePath(),
                {QStringLiteral("--phase"), phase.engine, phase.backend, site.base()});
    timeout.start(120000);
    loop.exec();
    if (child.state() != QProcess::NotRunning) {
      std::cout << "[FAIL] " << phase.engine.toStdString() << " timed out" << std::endl;
      child.kill();
      child.waitForFinished(5000);
      return 1;
    }
    output += child.readAll();
    // GDK prints this and exits on an untrapped X error (e.g. XSetInputFocus BadMatch).
    if (output.contains("X Window System error") || output.contains("BadMatch")) {
      std::cout << "[FAIL] " << phase.engine.toStdString() << ": X error in the log" << std::endl;
      return 1;
    }
    if (child.exitStatus() != QProcess::NormalExit || child.exitCode() != 0) {
      std::cout << "[FAIL] " << phase.engine.toStdString() << " exit " << child.exitCode() << std::endl;
      return child.exitCode() == 77 ? 77 : 1;
    }
  }
  std::cout << "=== WINDOW PLATFORM COMPATIBILITY PASSED ===" << std::endl;
  return 0;
}

}  // namespace

int main(int argc, char *argv[]) {
  for (int i = 1; i < argc; ++i) {
    const QByteArray arg(argv[i]);
    if (arg == "--gui") return runGui(argc, argv);
    if (arg == "--phase" && i + 3 < argc)
      return runPhase(QString::fromLocal8Bit(argv[i + 1]), QString::fromLocal8Bit(argv[i + 2]),
                      QString::fromLocal8Bit(argv[i + 3]), argc, argv);
  }
  return runLogic();
}
