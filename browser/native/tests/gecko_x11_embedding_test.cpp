// GUI tier (scripts/safe-gui-test.sh only): the experimental Gecko engine on
// the real X11 display (XWayland on Wayland sessions), with the real Gecko
// host on the development runtime, in DaliNira's BrowserWindow. Local pages
// only, from a server in this process; no input is injected anywhere; every
// file goes to a temporary directory.
//
// Checks: the host's window is embedded inside the browser window (X11 tree,
// X server owner, not managed by the window manager, size of the page area),
// follows the selected tab, is unmapped while a DaliNira page (Settings) is
// selected, cross-site pages run in separate sandboxed content processes, no
// host process opens an NVIDIA device, closing the window keeps the host's
// window alive until the host is shut down, and nothing is left afterwards.
//
// Skipped (77) without a usable runtime or off xcb.

#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QUrlQuery>

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
#include "engine/gecko/gecko_presentation_surface.h"
#include "engine/gecko/gecko_session.h"
#include "engine/gecko/gecko_x11_embedder.h"
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

bool waitUntil(const std::function<bool()> &condition, int timeoutMs = 30000) {
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

QString statusField(qint64 pid, const char *field) {
  QFile status(QStringLiteral("/proc/%1/status").arg(pid));
  if (!status.open(QIODevice::ReadOnly)) return {};
  for (const QByteArray &line : status.readAll().split('\n')) {
    if (line.startsWith(QByteArray(field) + ':')) return QString::fromLatin1(line.mid(qstrlen(field) + 1)).trimmed();
  }
  return {};
}

QStringList nvidiaDevices(const QList<qint64> &pids) {
  QStringList devices;
  for (const qint64 pid : pids) {
    QDirIterator fds(QStringLiteral("/proc/%1/fd").arg(pid), QDir::System | QDir::NoDotAndDotDot);
    while (fds.hasNext()) {
      const QString target = QFile::symLinkTarget(fds.next());
      if (target.startsWith(QLatin1String("/dev/nvidia"))) devices.append(QStringLiteral("%1:%2").arg(pid).arg(target));
    }
  }
  return devices;
}

class PageServer : public QTcpServer {
 public:
  PageServer() {
    connect(this, &QTcpServer::newConnection, this, [this] {
      while (QTcpSocket *socket = nextPendingConnection()) {
        connect(socket, &QTcpSocket::readyRead, socket, [socket] {
          const QUrl url(QString::fromLatin1(socket->readAll().split(' ').value(1)));
          const QString name = QUrlQuery(url).queryItemValue(QStringLiteral("name")).toHtmlEscaped();
          const QByteArray body = QStringLiteral("<!doctype html><meta charset=utf-8><title>%1</title>"
                                                 "<body style='background:#2b6;font:40px sans-serif'><h1>%1</h1>")
                                      .arg(name).toUtf8();
          socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\nContent-Length: " +
                        QByteArray::number(body.size()) + "\r\n\r\n" + body);
          socket->disconnectFromHost();
        });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
      }
    });
    CHECK(listen(QHostAddress::LocalHost));
  }
  QUrl url(const QString &host, const QString &name) const {
    return QUrl(QStringLiteral("http://%1:%2/page?name=%3").arg(host).arg(serverPort()).arg(name));
  }
};

GeckoEngineView *geckoView(BrowserWindow *window, int index) {
  return qobject_cast<GeckoEngineView *>(window->tabInfo(index).engineView.data());
}

}  // namespace

int main(int argc, char *argv[]) {
  EngineBackend::setSelectedEngineType(EngineType::Gecko);
  QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
  QApplication app(argc, argv);
  {
    GeckoX11Embedder probe;
    QString why;
    if (!probe.isUsable(&why)) {
      std::cout << "SKIP: " << why.toStdString() << std::endl;
      return 77;
    }
  }
  QStringList problems;
  if (!GeckoRuntime::validate(GeckoRuntime::developmentPaths(), &problems).isValid()) {
    std::cout << "SKIP (BLOCKED): Gecko runtime not usable: " << problems.join(QStringLiteral("; ")).toStdString() << std::endl;
    return 77;
  }
  QTemporaryDir root;
  CHECK(root.isValid());
  const QString base = QFileInfo(root.path()).canonicalFilePath();
  QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, base + QStringLiteral("/settings"));
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, base + QStringLiteral("/settings"));
  app.setOrganizationName(QStringLiteral("DaliNiraTest"));
  app.setApplicationName(QStringLiteral("GeckoX11EmbeddingTest"));
  const QString dataDir = base + QStringLiteral("/data");
  CHECK(QDir().mkpath(dataDir));
  CHECK(QDir().mkpath(base + QStringLiteral("/ipc")) && ::chmod(QFile::encodeName(base + QStringLiteral("/ipc")).constData(), 0700) == 0);
  PageServer pages;
  std::cout << "=== GECKO X11 EMBEDDING (real host, real display) ===" << std::endl;

  BrowserProfileService profileService(dataDir, nullptr, &app);
  EngineStorageManager storage(dataDir);
  GeckoBackend::Config config;
  config.ipcRoot = base + QStringLiteral("/ipc");
  auto ownedBackend = std::make_unique<GeckoBackend>(config, &app);
  GeckoBackend *gecko = ownedBackend.get();
  gecko->setDiscardConfirmation([](QWidget *, const QString &) { return true; });
  CHECK(gecko->initialize(storage));
  EngineBackend::registerBackend(EngineType::Gecko, std::move(ownedBackend));
  GeckoSession *session = gecko->session();
  CHECK(waitUntil([&] { return session->state() == GeckoSession::State::Ready; }, 60000));

  TabManager tabManager(&app);
  tabManager.performanceManager()->setPolicyMode(dalinira::PerformancePolicyMode::Off);
  SessionStore sessionStore(dataDir, QStringLiteral("gecko"));
  BrowserServices services;
  services.profileService = &profileService;
  services.tabManager = &tabManager;
  services.sessionStore = &sessionStore;
  auto *window = new BrowserWindow(services);
  window->resize(1100, 720);
  window->show();

  window->addNewTab(pages.url(QStringLiteral("127.0.0.1"), QStringLiteral("Alpha")));
  GeckoEngineView *alpha = geckoView(window, 0);
  CHECK(alpha && waitUntil([&] { return alpha->title() == QStringLiteral("Alpha") && alpha->contentProcessId() > 0; }));
  CHECK(waitUntil([&] { return gecko->surface()->isPresenting(alpha); }));
  QJsonObject where = gecko->surface()->inspect();
  std::cout << QJsonDocument(where).toJson(QJsonDocument::Compact).toStdString() << std::endl;
  CHECK(where.value("insideQtTopLevel").toBool() && where.value("viewable").toBool() &&
        !where.value("managedByWindowManager").toBool());
  const QJsonObject geometry = where.value("geometry").toObject();
  CHECK(geometry.value("width").toInt() == alpha->width() && geometry.value("height").toInt() == alpha->height());
  std::cout << "PASS the host window is embedded in the browser window" << std::endl;

  window->addNewTab(pages.url(QStringLiteral("localhost"), QStringLiteral("Beta")));
  GeckoEngineView *beta = geckoView(window, 1);
  CHECK(beta && waitUntil([&] { return beta->title() == QStringLiteral("Beta") && beta->contentProcessId() > 0; }));
  CHECK(waitUntil([&] { return gecko->surface()->isPresenting(beta) && session->activeTabId() == beta->hostTabId(); }));
  CHECK(!gecko->surface()->isPresenting(alpha) && gecko->surface()->inspect().value("insideQtTopLevel").toBool());
  CHECK(alpha->contentProcessId() != beta->contentProcessId());
  for (const qint64 pid : {alpha->contentProcessId(), beta->contentProcessId()}) {
    CHECK(statusField(pid, "Seccomp") == QStringLiteral("2") && statusField(pid, "NoNewPrivs") == QStringLiteral("1"));
  }
  window->switchTab(0);
  CHECK(waitUntil([&] { return gecko->surface()->isPresenting(alpha) && session->activeTabId() == alpha->hostTabId(); }));
  std::cout << "PASS the surface follows the selected tab; sandboxed cross-site processes" << std::endl;

  // A DaliNira page: the Gecko window is not shown over it.
  window->showSettings();
  CHECK(waitUntil([&] { return !gecko->surface()->inspect().value("viewable").toBool(); }));
  window->switchTab(0);
  CHECK(waitUntil([&] { return gecko->surface()->inspect().value("viewable").toBool(); }));
  std::cout << "PASS hidden under DaliNira pages" << std::endl;

  QList<qint64> tree{session->hostPid()};
  for (const auto &process : session->hostProcesses()) tree.append(process.pid);
  const QStringList nvidia = nvidiaDevices(tree);
  if (!nvidia.isEmpty()) std::cout << "NVIDIA devices open: " << nvidia.join(QLatin1Char(' ')).toStdString() << std::endl;
  CHECK(nvidia.isEmpty());

  // Closing the window keeps the host's window; the host then shuts down.
  window->close();
  CHECK(waitUntil([&] { return !window->isVisible(); }));
  QThread::msleep(200);
  QCoreApplication::processEvents();
  CHECK(session->state() == GeckoSession::State::Ready && alive(session->hostPid()));
  CHECK(gecko->shutdownHost(10000));
  for (const auto &process : session->hostProcesses()) tree.append(process.pid);
  for (const qint64 pid : tree) CHECK(waitUntil([&] { return !alive(pid); }, 15000));
  delete window;
  EngineBackend::shutdown();
  std::cout << "PASS window close, then host shutdown: no process left" << std::endl;
  std::cout << "=== ALL GECKO X11 EMBEDDING TESTS PASSED ===" << std::endl;
  return 0;
}
