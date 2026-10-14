// The real DaliNira Gecko Host (browser/resources/gecko-host on the
// configured development Gecko runtime), run headless: no display connection
// and no GPU access (MOZ_HEADLESS, MOZ_AVOID_OPENGL_ALTOGETHER). Local pages
// only, from a server in this process.
//
// Checks: startup and authentication, cross-site documents in separate
// sandboxed content processes (Fission, seccomp, no_new_privs), no GPU device
// opened by any host process, history, beforeunload guards, a content
// process crash and explicit recovery, guarded shutdown, no process left, and
// no file of the Gecko runtime changed.
//
// Runs only with DALINIRA_GECKO_HOST_TESTS=1 and a usable runtime; otherwise
// it reports why and exits 77 (skipped).

#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QHostAddress>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QUrlQuery>

#include <csignal>
#include <functional>
#include <iostream>
#include <sys/stat.h>

#include "engine/gecko/gecko_runtime.h"
#include "engine/gecko/gecko_session.h"

#define CHECK(condition)                                                                    \
  do {                                                                                      \
    if (!(condition)) {                                                                     \
      std::cout << "[FAIL] " << __FILE__ << ":" << __LINE__ << ": " #condition << std::endl; \
      std::_Exit(1);                                                                        \
    }                                                                                       \
  } while (false)

namespace {

using namespace dalinira::engine;
using View = GeckoSessionView;

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

QJsonObject result(GeckoRequest *request, const QString &status = QStringLiteral("completed")) {
  CHECK(request && waitUntil([&] { return request->isFinished(); }));
  if (request->status() != status)
    std::cout << request->status().toStdString() << " "
              << QJsonDocument(request->result()).toJson(QJsonDocument::Compact).toStdString() << std::endl;
  CHECK(request->status() == status);
  return request->result();
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

// Devices of a GPU any of the processes has open.
QStringList gpuDevices(const QList<qint64> &pids) {
  QStringList devices;
  for (const qint64 pid : pids) {
    QDirIterator fds(QStringLiteral("/proc/%1/fd").arg(pid), QDir::System | QDir::NoDotAndDotDot);
    while (fds.hasNext()) {
      const QString target = QFile::symLinkTarget(fds.next());
      if (target.startsWith(QLatin1String("/dev/nvidia")) || target.startsWith(QLatin1String("/dev/dri/")))
        devices.append(QStringLiteral("%1:%2").arg(pid).arg(target));
    }
  }
  return devices;
}

// Path, size, mtime and ctime of every entry of the runtime.
QStringList metadata(const QString &directory) {
  QStringList entries;
  QDirIterator it(directory, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                  QDirIterator::Subdirectories);
  while (it.hasNext()) {
    const QString path = it.next();
    struct stat st {};
    if (::lstat(QFile::encodeName(path).constData(), &st) != 0) continue;
    entries.append(QStringLiteral("%1 %2 %3.%4 %5.%6")
                       .arg(path)
                       .arg(st.st_size)
                       .arg(st.st_mtim.tv_sec)
                       .arg(st.st_mtim.tv_nsec)
                       .arg(st.st_ctim.tv_sec)
                       .arg(st.st_ctim.tv_nsec));
  }
  entries.sort();
  return entries;
}

// Local test pages: /page?name=N, /dirty?name=N with a beforeunload handler,
// and /keys: a focused text field; the title lists the keys the page received
// and the field's text ("keys:a,b,C-c|ab").
class PageServer : public QTcpServer {
 public:
  PageServer() {
    connect(this, &QTcpServer::newConnection, this, [this] {
      while (QTcpSocket *socket = nextPendingConnection()) {
        connect(socket, &QTcpSocket::readyRead, socket, [socket] {
          const QByteArray request = socket->readAll();
          const QUrl url(QString::fromLatin1(request.split(' ').value(1)));
          const QString name = QUrlQuery(url).queryItemValue(QStringLiteral("name")).toHtmlEscaped();
          const bool dirty = url.path() == QLatin1String("/dirty");
          const QByteArray body = url.path() == QLatin1String("/keys") ? QByteArray(
              "<!doctype html><meta charset=utf-8><title>keys:|</title><input id=f>"
              "<script>const seen=[];const show=()=>{document.title='keys:'+seen.join(',')+'|'+f.value};"
              "addEventListener('keydown',e=>{if(['Control','Shift','Alt'].includes(e.key))return;"
              "seen.push((e.ctrlKey?'C-':'')+(e.altKey?'A-':'')+e.key);show()},true);"
              "f.addEventListener('input',show);f.focus()</script>") : QStringLiteral("<!doctype html><meta charset=utf-8><title>%1</title><h1>%1</h1>%2")
                                      .arg(name, dirty ? QStringLiteral("<script>addEventListener('beforeunload',e=>{"
                                                                        "e.preventDefault();e.returnValue=''})</script>")
                                                       : QString())
                                      .toUtf8();
          socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\nContent-Length: " +
                        QByteArray::number(body.size()) + "\r\n\r\n" + body);
          socket->disconnectFromHost();
        });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
      }
    });
    CHECK(listen(QHostAddress::LocalHost));
  }
  QUrl url(const QString &host, const QString &path, const QString &name) const {
    return QUrl(QStringLiteral("http://%1:%2%3?name=%4").arg(host).arg(serverPort()).arg(path, name));
  }
};

void loaded(View *view, const QString &name) {
  CHECK(waitUntil([&] { return view->title() == name && !view->isLoading() && view->contentProcessId() > 0; }));
}

}  // namespace

int main(int argc, char *argv[]) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app(argc, argv);
  if (qEnvironmentVariable("DALINIRA_GECKO_HOST_TESTS") != QLatin1String("1")) {
    std::cout << "SKIP: set DALINIRA_GECKO_HOST_TESTS=1 to run the real Gecko host" << std::endl;
    return 77;
  }
  const GeckoRuntimePaths paths = GeckoRuntime::developmentPaths();
  QStringList problems;
  const GeckoValidatedRuntime runtime = GeckoRuntime::validate(paths, &problems);
  if (!runtime.isValid()) {
    std::cout << "BLOCKED: the Gecko runtime '" << paths.greDirectory.toStdString() << "' is not usable:\n  "
              << problems.join(QStringLiteral("\n  ")).toStdString() << std::endl;
    return 77;
  }
  QTemporaryDir root;
  CHECK(root.isValid());
  const QString base = QFileInfo(root.path()).canonicalFilePath();
  for (const char *dir : {"/runtime", "/ipc"}) {
    CHECK(QDir().mkpath(base + QString::fromLatin1(dir)));
    CHECK(::chmod(QFile::encodeName(base + QString::fromLatin1(dir)).constData(), 0700) == 0);
  }
  const QStringList runtimeBefore = metadata(runtime.greDirectory);
  PageServer pages;
  std::cout << "=== GECKO HOST (headless) on Gecko " << runtime.milestone.toStdString() << " ===" << std::endl;

  GeckoSession session;
  GeckoSession::Config config;
  config.runtime = paths;
  config.runtimeRoot = base + QStringLiteral("/runtime");
  config.ipcRoot = base + QStringLiteral("/ipc");
  config.display = GeckoHostProcess::Display::Headless;
  config.testInput = true;
  QString error;
  if (!session.start(config, &error)) std::cout << error.toStdString() << std::endl;
  CHECK(session.hostPid() > 0);
  CHECK(waitUntil([&] { return session.state() == GeckoSession::State::Ready; }, 60000));
  CHECK(session.protocol() == 3 && session.views().size() == 1);
  std::cout << "PASS startup and authentication" << std::endl;

  View *alpha = session.view(session.activeTabId());
  result(alpha->load(pages.url(QStringLiteral("127.0.0.1"), QStringLiteral("/page"), QStringLiteral("Alpha"))));
  loaded(alpha, QStringLiteral("Alpha"));
  View *beta = session.view(result(session.createView(pages.url(QStringLiteral("localhost"), QStringLiteral("/page"),
                                                                QStringLiteral("Beta")), false))
                                .value("tabId").toString());
  CHECK(beta);
  loaded(beta, QStringLiteral("Beta"));
  CHECK(alpha->contentProcessId() != beta->contentProcessId());
  for (const qint64 pid : {alpha->contentProcessId(), beta->contentProcessId()}) {
    CHECK(statusField(pid, "Seccomp") == QStringLiteral("2") && statusField(pid, "NoNewPrivs") == QStringLiteral("1"));
  }
  std::cout << "PASS cross-site documents in separate sandboxed content processes" << std::endl;

  result(beta->load(pages.url(QStringLiteral("localhost"), QStringLiteral("/page"), QStringLiteral("Second"))));
  loaded(beta, QStringLiteral("Second"));
  result(beta->back());
  loaded(beta, QStringLiteral("Beta"));
  CHECK(waitUntil([&] { return beta->canGoForward(); }));
  std::cout << "PASS history" << std::endl;

  // Keyboard: typed through Gecko's own input path into the focused page.
  // The keys of the shortcut set never reach the page and are reported;
  // every other key (text, Ctrl+C) is the page's.
  QStringList pressed;
  QObject::connect(&session, &GeckoSession::shortcutPressed, [&](const QString &tab, const QString &name, bool) {
    pressed << (tab == alpha->tabId() ? name : QStringLiteral("other tab"));
  });
  result(session.setShortcuts({QStringLiteral("Ctrl+T"), QStringLiteral("Ctrl+Shift+Tab"), QStringLiteral("Alt+ArrowLeft"),
                               QStringLiteral("F5"), QStringLiteral("Ctrl++"), QStringLiteral("Ctrl+L")}));
  result(alpha->load(pages.url(QStringLiteral("127.0.0.1"), QStringLiteral("/keys"), QString())));
  loaded(alpha, QStringLiteral("keys:|"));
  const auto type = [&](const QString &key, const QString &code, const QStringList &modifiers) {
    result(session.submit(QStringLiteral("test_key"), {{QStringLiteral("tabId"), alpha->tabId()}, {QStringLiteral("key"), key},
                                                     {QStringLiteral("code"), code},
                                                     {QStringLiteral("modifiers"), QJsonArray::fromStringList(modifiers)}}));
  };
  const QStringList ctrl{QStringLiteral("Control")};
  type(QStringLiteral("a"), QStringLiteral("KeyA"), {});
  type(QStringLiteral("t"), QStringLiteral("KeyT"), ctrl);
  type(QStringLiteral("Tab"), QStringLiteral("Tab"), {QStringLiteral("Control"), QStringLiteral("Shift")});
  type(QStringLiteral("ArrowLeft"), QStringLiteral("ArrowLeft"), {QStringLiteral("Alt")});
  type(QStringLiteral("F5"), QStringLiteral("F5"), {});
  type(QStringLiteral("+"), QStringLiteral("Equal"), {QStringLiteral("Control"), QStringLiteral("Shift")});
  // Another layout: Cyrillic "е" on the T key is Ctrl+T as well.
  type(QString(QChar(0x0435)), QStringLiteral("KeyT"), ctrl);
  type(QStringLiteral("c"), QStringLiteral("KeyC"), ctrl);
  type(QStringLiteral("b"), QStringLiteral("KeyB"), {});
  CHECK(waitUntil([&] { return alpha->title() == QStringLiteral("keys:a,C-c,b|ab"); }));
  if (pressed != QStringList({"Ctrl+T", "Ctrl+Shift+Tab", "Alt+ArrowLeft", "F5", "Ctrl++", "Ctrl+T"}))
    std::cout << "pressed: " << pressed.join(QLatin1Char(' ')).toStdString() << std::endl;
  CHECK(pressed == QStringList({"Ctrl+T", "Ctrl+Shift+Tab", "Alt+ArrowLeft", "F5", "Ctrl++", "Ctrl+T"}));
  // Without a set, every key is the page's.
  result(session.setShortcuts({}));
  type(QStringLiteral("t"), QStringLiteral("KeyT"), ctrl);
  CHECK(waitUntil([&] { return alpha->title() == QStringLiteral("keys:a,C-c,b,C-t|ab"); }));
  CHECK(pressed.size() == 6);
  std::cout << "PASS browser shortcuts kept from the page and reported; typing and Ctrl+C reach the page" << std::endl;

  result(alpha->load(pages.url(QStringLiteral("127.0.0.1"), QStringLiteral("/dirty"), QStringLiteral("Dirty"))));
  loaded(alpha, QStringLiteral("Dirty"));
  CHECK(waitUntil([&] { return alpha->hasBeforeUnload(); }));
  const QString dirty = alpha->documentId();
  CHECK(result(alpha->close(), "rejected").value("code").toString() == QStringLiteral("beforeunload-confirmation-required"));
  CHECK(result(alpha->load(pages.url(QStringLiteral("127.0.0.1"), QStringLiteral("/page"), QStringLiteral("X"))), "rejected")
            .value("code").toString() == QStringLiteral("beforeunload-navigation-blocked"));
  CHECK(alpha->documentId() == dirty);
  std::cout << "PASS beforeunload guards" << std::endl;

  const qint64 victim = beta->contentProcessId();
  const QString crashedDocument = beta->documentId();
  CHECK(::kill(static_cast<pid_t>(victim), SIGKILL) == 0);
  CHECK(waitUntil([&] { return beta->lifecycleState() == View::State::Crashed; }));
  CHECK(result(beta->reload(), "rejected").value("code").toString() == QStringLiteral("recovery-required"));
  result(beta->recoverDocument());
  CHECK(waitUntil([&] { return beta->lifecycleState() != View::State::Crashed && beta->contentProcessId() > 0 &&
                               beta->contentProcessId() != victim; }));
  // The recovered document, as the host reports it (shutdown confirmations
  // are bound to the documents last observed).
  loaded(beta, QStringLiteral("Beta"));
  CHECK(waitUntil([&] { return !beta->documentId().isEmpty() && beta->documentId() != crashedDocument; }));
  std::cout << "PASS content crash and explicit recovery" << std::endl;

  QList<qint64> tree{session.hostPid()};
  for (const auto &process : session.hostProcesses()) tree.append(process.pid);
  const QStringList devices = gpuDevices(tree);
  if (!devices.isEmpty()) std::cout << "GPU devices open: " << devices.join(QLatin1Char(' ')).toStdString() << std::endl;
  CHECK(devices.isEmpty());
  std::cout << "PASS no GPU device opened by " << tree.size() << " host processes" << std::endl;

  CHECK(result(session.shutdown(), "rejected").value("code").toString() == QStringLiteral("beforeunload-confirmation-required"));
  result(session.shutdown(true, session.documentSnapshot()));
  const QJsonObject cleanup = session.stopHost(10000);
  CHECK(cleanup.value("leftoversKilled").toArray().isEmpty());
  for (const auto &process : session.hostProcesses()) tree.append(process.pid);
  for (const qint64 pid : tree) CHECK(waitUntil([&] { return !alive(pid); }, 15000));
  std::cout << "PASS guarded shutdown; no host process left" << std::endl;

  CHECK(metadata(runtime.greDirectory) == runtimeBefore);
  std::cout << "PASS the Gecko runtime is unchanged (" << runtimeBefore.size() << " entries)" << std::endl;
  std::cout << "=== ALL GECKO HOST TESTS PASSED ===" << std::endl;
  return 0;
}
