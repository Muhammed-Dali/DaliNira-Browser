// The optional Gecko adapter (browser/native/engine/gecko) without Gecko:
//  - runtime validation, on test runtimes broken in one way each;
//  - the host launcher's environment and arguments;
//  - the IPC server's transport security, against a client in this process;
//  - the X11 embedder off xcb (it must refuse, not crash);
//  - complete sessions against gecko_fake_host, which the real launcher
//    starts from a test runtime exactly as it starts Gecko's `firefox`:
//    authentication, requests, cancellation, deadlines, stale and invalid
//    replies, beforeunload guards, content and host crashes, sequence
//    violations, restarts, guarded shutdown and process-tree cleanup;
//  - a host never outliving its parent (SIGKILL of the parent).
// Every file goes to temporary directories of this test. The real Gecko host
// is tested by browser-gecko-host (gecko_host_test.cpp).

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <QWidget>
#include <QtEndian>

#include <csignal>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

#include "engine/gecko/gecko_host_process.h"
#include "engine/gecko/gecko_ipc_server.h"
#include "engine/gecko/gecko_runtime.h"
#include "engine/gecko/gecko_session.h"
#include "engine/gecko/gecko_x11_embedder.h"

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
  static void receive(GeckoSession &session, quint64 generation, const QJsonObject &message) {
    session.receive(generation, message);
  }
  static GeckoIpcServer *ipc(GeckoSession &session) { return session.ipc_; }
};
}  // namespace dalinira::engine

namespace {

using namespace dalinira::engine;
using Access = GeckoSessionTestAccess;
using View = GeckoSessionView;

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

QJsonObject result(GeckoRequest *request, const QString &status = QStringLiteral("completed")) {
  CHECK(request);
  CHECK(waitUntil([&] { return request->isFinished(); }));
  if (request->status() != status) {
    std::cout << "request " << request->id().toStdString() << ": " << request->status().toStdString() << " "
              << QJsonDocument(request->result()).toJson(QJsonDocument::Compact).toStdString() << std::endl;
  }
  CHECK(request->status() == status);
  return request->result();
}

bool alive(qint64 pid) {
  QFile stat(QStringLiteral("/proc/%1/stat").arg(pid));
  if (!stat.open(QIODevice::ReadOnly)) return false;
  const QByteArray text = stat.readAll();
  return text.mid(text.lastIndexOf(')') + 2, 1) != "Z";
}

bool writeFile(const QString &path, const QByteArray &content, QFile::Permissions permissions) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(content) == content.size() && file.setPermissions(permissions);
}

constexpr QFile::Permissions kFileMode = QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther;
constexpr QFile::Permissions kExecMode = kFileMode | QFile::ExeOwner | QFile::ExeGroup | QFile::ExeOther;

// A runtime directory around gecko_fake_host, laid out like Gecko's dist/bin.
QString makeFakeRuntime(const QString &dir) {
  CHECK(QDir().mkpath(dir + QStringLiteral("/modules")));
  QFile::remove(dir + QStringLiteral("/firefox"));
  CHECK(QFile::copy(QStringLiteral(DALINIRA_GECKO_FAKE_HOST), dir + QStringLiteral("/firefox")));
  CHECK(QFile(dir + QStringLiteral("/firefox")).setPermissions(kExecMode));
  CHECK(writeFile(dir + QStringLiteral("/libxul.so"), {}, kFileMode));
  CHECK(writeFile(dir + QStringLiteral("/plugin-container"), {}, kExecMode));
  CHECK(writeFile(dir + QStringLiteral("/platform.ini"), "[Build]\nMilestone=160.0a1\n", kFileMode));
  CHECK(writeFile(dir + QStringLiteral("/modules/AppConstants.sys.mjs"), "  NIGHTLY_BUILD: true,\n", kFileMode));
  return dir;
}

QString hostApplication() { return QStringLiteral(DALINIRA_GECKO_HOST_APP_DIR); }

QString makePrivateDirectory(const QString &path) {
  CHECK(QDir().mkpath(path));
  CHECK(::chmod(QFile::encodeName(path).constData(), 0700) == 0);
  return path;
}

// ---------------------------------------------------------------------------

void checkRuntimeValidation(const QString &base) {
  const QString gre = makeFakeRuntime(base + QStringLiteral("/gre"));
  QStringList problems;
  const auto valid = GeckoRuntime::validate({gre, hostApplication()}, &problems);
  if (!problems.isEmpty()) std::cout << problems.join(QLatin1Char('\n')).toStdString() << std::endl;
  CHECK(valid.isValid() && problems.isEmpty());
  CHECK(valid.executable == QFileInfo(gre).canonicalFilePath() + QStringLiteral("/firefox"));
  CHECK(valid.milestone == QStringLiteral("160.0a1"));
  CHECK(valid.updatePolicyFile == QFileInfo(hostApplication()).canonicalFilePath() + QStringLiteral("/distribution/policies.json"));

  const auto expectProblem = [&](const char *what, const GeckoRuntimePaths &paths, const char *fragment) {
    QStringList found;
    const bool usable = GeckoRuntime::validate(paths, &found).isValid();
    const bool mentioned = found.join(QLatin1Char('\n')).contains(QString::fromLatin1(fragment));
    if (usable || !mentioned) std::cout << what << ": " << found.join(QLatin1Char('|')).toStdString() << std::endl;
    CHECK(!usable && mentioned);
  };
  expectProblem("relative", {QStringLiteral("gre"), hostApplication()}, "not an absolute path");
  expectProblem("missing", {base + QStringLiteral("/nowhere"), hostApplication()}, "does not exist");

  const QString broken = makeFakeRuntime(base + QStringLiteral("/broken"));
  QFile::remove(broken + QStringLiteral("/libxul.so"));
  expectProblem("no libxul", {broken, hostApplication()}, "libxul.so is missing");
  makeFakeRuntime(broken);
  CHECK(writeFile(broken + QStringLiteral("/modules/AppConstants.sys.mjs"), "  NIGHTLY_BUILD: false,\n", kFileMode));
  expectProblem("not nightly", {broken, hostApplication()}, "not a Nightly development build");
  makeFakeRuntime(broken);
  CHECK(QDir().mkpath(broken + QStringLiteral("/distribution")));
  CHECK(writeFile(broken + QStringLiteral("/distribution/policies.json"), "{}", kFileMode));
  expectProblem("runtime policy", {broken, hostApplication()}, "take precedence");
  CHECK(QDir(broken + QStringLiteral("/distribution")).removeRecursively());
  CHECK(writeFile(broken + QStringLiteral("/platform.ini"), "[Build]\nMilestone=161.0a1\n", kFileMode));
  expectProblem("version", {broken, hostApplication()}, "not the version the host application supports");
  makeFakeRuntime(broken);
  CHECK(::chmod(QFile::encodeName(broken).constData(), 0775) == 0);
  expectProblem("group-writable runtime", {broken, hostApplication()}, "can be changed by other users");
  CHECK(::chmod(QFile::encodeName(broken).constData(), 0755) == 0);
  CHECK(QFile(broken + QStringLiteral("/firefox")).setPermissions(kExecMode | QFile::WriteGroup));
  expectProblem("group-writable executable", {broken, hostApplication()}, "only its owner can write");
  // An executable that leads outside the runtime.
  QFile::remove(broken + QStringLiteral("/firefox"));
  CHECK(QFile::link(gre + QStringLiteral("/firefox"), broken + QStringLiteral("/firefox")));
  expectProblem("executable outside", {broken, hostApplication()}, "leads outside");

  // The host application: exactly DaliNira's update policy and name.
  const QString app = base + QStringLiteral("/app");
  CHECK(QDir().mkpath(app));
  for (const QString &entry : {QStringLiteral("application.ini"), QStringLiteral("chrome.manifest"),
                               QStringLiteral("chrome/content/host.js"), QStringLiteral("distribution/policies.json")}) {
    CHECK(QDir().mkpath(QFileInfo(app + QLatin1Char('/') + entry).path()));
    QFile::remove(app + QLatin1Char('/') + entry);
    CHECK(QFile::copy(hostApplication() + QLatin1Char('/') + entry, app + QLatin1Char('/') + entry));
    CHECK(QFile(app + QLatin1Char('/') + entry).setPermissions(kFileMode));
  }
  CHECK(GeckoRuntime::validate({gre, app}, nullptr).isValid());
  CHECK(writeFile(app + QStringLiteral("/distribution/policies.json"),
                  R"({"policies":{"DisableAppUpdate":true,"Extensions":{}}})", kFileMode));
  expectProblem("extra policy", {gre, app}, "must disable application updates and nothing else");
  CHECK(QFile::remove(app + QStringLiteral("/distribution/policies.json")));
  CHECK(QFile::copy(hostApplication() + QStringLiteral("/distribution/policies.json"),
                    app + QStringLiteral("/distribution/policies.json")));
  QFile ini(app + QStringLiteral("/application.ini"));
  CHECK(ini.setPermissions(kFileMode) && ini.open(QIODevice::ReadOnly));
  QByteArray text = ini.readAll();
  ini.close();
  CHECK(writeFile(app + QStringLiteral("/application.ini"), text.replace("Name=DaliNiraGeckoHost", "Name=Other"), kFileMode));
  expectProblem("other application", {gre, app}, "is not the DaliNira Gecko Host");

  // The configured development runtime, as it is on this machine.
  const GeckoRuntimePaths development = GeckoRuntime::developmentPaths();
  QStringList developmentProblems;
  GeckoRuntime::validate(development, &developmentProblems);
  std::cout << "INFO development runtime '" << development.greDirectory.toStdString() << "': "
            << (developmentProblems.isEmpty() ? std::string("usable")
                                              : developmentProblems.join(QStringLiteral("; ")).toStdString())
            << std::endl;
  std::cout << "PASS runtime validation" << std::endl;
}

void checkLaunchEnvironment() {
  QProcessEnvironment system;
  for (const auto &[name, value] : std::initializer_list<std::pair<const char *, const char *>>{
           {"PATH", "/usr/bin"}, {"LANG", "tr_TR.UTF-8"}, {"LC_TIME", "C"}, {"DISPLAY", ":9"},
           {"XAUTHORITY", "/run/user/1000/xauth"}, {"WAYLAND_DISPLAY", "wayland-0"},
           {"DBUS_SESSION_BUS_ADDRESS", "unix:path=/run/user/1000/bus"}, {"HOME", "/home/user"},
           {"LD_PRELOAD", "/tmp/evil.so"}, {"LD_LIBRARY_PATH", "/tmp/lib"}, {"GTK_MODULES", "evil"},
           {"GIO_EXTRA_MODULES", "/tmp/gio"}, {"QT_QPA_PLATFORM", "xcb"}, {"MOZ_FORCE_DISABLE_E10S", "1"},
           {"MOZ_HEADLESS", "1"}, {"GDK_BACKEND", "wayland"}, {"XDG_CONFIG_HOME", "/home/user/.config"}}) {
    system.insert(QString::fromLatin1(name), QString::fromLatin1(value));
  }
  GeckoHostProcess::Config config;
  config.runtime.applicationIni = QStringLiteral("/app/application.ini");
  config.socketPath = QStringLiteral("/run/ipc/host.sock");
  config.tokenFile = QStringLiteral("/run/ipc/token");
  QString error;
  QProcessEnvironment x11 = GeckoHostProcess::environmentFor(system, config, QStringLiteral("/run/s1/home"), &error);
  for (const char *dropped : {"LD_PRELOAD", "LD_LIBRARY_PATH", "GTK_MODULES", "GIO_EXTRA_MODULES", "QT_QPA_PLATFORM",
                              "MOZ_FORCE_DISABLE_E10S", "MOZ_HEADLESS", "WAYLAND_DISPLAY"}) {
    CHECK(!x11.contains(QString::fromLatin1(dropped)));
  }
  for (const auto &[name, value] : std::initializer_list<std::pair<const char *, const char *>>{
           {"PATH", "/usr/bin"}, {"LANG", "tr_TR.UTF-8"}, {"LC_TIME", "C"}, {"DISPLAY", ":9"},
           {"XAUTHORITY", "/run/user/1000/xauth"}, {"DBUS_SESSION_BUS_ADDRESS", "unix:path=/run/user/1000/bus"},
           {"HOME", "/run/s1/home"}, {"XDG_CONFIG_HOME", "/run/s1/home/.config"},
           {"XDG_CACHE_HOME", "/run/s1/home/.cache"}, {"XDG_DATA_HOME", "/run/s1/home/.local/share"},
           {"XDG_STATE_HOME", "/run/s1/home/.local/state"}, {"TMPDIR", "/run/s1/tmp"}, {"GDK_BACKEND", "x11"},
           {"EGL_PLATFORM", "x11"}, {"MOZ_CRASHREPORTER_DISABLE", "1"}, {"MOZ_DISABLE_UPDATE_PROCESSING", "1"},
           {"MOZ_GDB_SLEEP", "0"}, {"DNG_IPC_SOCKET", "/run/ipc/host.sock"}, {"DNG_IPC_TOKEN_FILE", "/run/ipc/token"}}) {
    CHECK(x11.value(QString::fromLatin1(name)) == QString::fromLatin1(value));
  }
  CHECK(!x11.contains(QStringLiteral("MOZ_AVOID_OPENGL_ALTOGETHER")));

  config.display = GeckoHostProcess::Display::Headless;
  const QProcessEnvironment headless = GeckoHostProcess::environmentFor(system, config, QStringLiteral("/run/s1/home"), &error);
  CHECK(!headless.contains(QStringLiteral("DISPLAY")) && !headless.contains(QStringLiteral("XAUTHORITY")));
  CHECK(!headless.contains(QStringLiteral("WAYLAND_DISPLAY")) && !headless.contains(QStringLiteral("GDK_BACKEND")));
  CHECK(headless.value(QStringLiteral("MOZ_HEADLESS")) == QStringLiteral("1"));
  CHECK(headless.value(QStringLiteral("MOZ_AVOID_OPENGL_ALTOGETHER")) == QStringLiteral("1"));

  config.display = GeckoHostProcess::Display::X11;
  system.remove(QStringLiteral("DISPLAY"));
  error.clear();
  CHECK(GeckoHostProcess::environmentFor(system, config, QStringLiteral("/run/s1/home"), &error).isEmpty() &&
        error.contains(QStringLiteral("X11 display")));
  CHECK(GeckoHostProcess::argumentsFor(config, QStringLiteral("/run/s1/profile")) ==
        QStringList({QStringLiteral("-app"), QStringLiteral("/app/application.ini"), QStringLiteral("--new-instance"),
                     QStringLiteral("-profile"), QStringLiteral("/run/s1/profile")}));
  std::cout << "PASS launch environment" << std::endl;
}

// A client of a GeckoIpcServer in this process (its pid is the peer pid).
struct IpcClient {
  QLocalSocket socket;
  QByteArray buffer;
  explicit IpcClient(const QString &path) {
    socket.connectToServer(path);
    CHECK(socket.waitForConnected(2000));
  }
  void raw(const QByteArray &bytes) {
    socket.write(bytes);
    socket.flush();
  }
  void send(const QJsonObject &message) {
    const QByteArray payload = QJsonDocument(message).toJson(QJsonDocument::Compact);
    QByteArray frame(4, Qt::Uninitialized);
    qToBigEndian<quint32>(static_cast<quint32>(payload.size()), frame.data());
    raw(frame + payload);
  }
  QJsonObject next() {
    QJsonObject message;
    CHECK(waitUntil([&] {
      buffer += socket.readAll();
      if (buffer.size() < 4) return false;
      const quint32 length = qFromBigEndian<quint32>(buffer.constData());
      if (buffer.size() < 4 + qsizetype(length)) return false;
      message = QJsonDocument::fromJson(buffer.mid(4, length)).object();
      buffer.remove(0, 4 + length);
      return true;
    }, 3000));
    return message;
  }
};

struct IpcFixture {
  GeckoIpcServer server;
  QString closedReason;
  QString rejectedReason;
  QString token;
  explicit IpcFixture(const QString &dir) {
    QString error;
    CHECK(server.listen(dir, &error));
    server.setExpectedPeerPid(QCoreApplication::applicationPid());
    QObject::connect(&server, &GeckoIpcServer::sessionClosed, [this](const QString &why) { closedReason = why; });
    QObject::connect(&server, &GeckoIpcServer::connectionRejected, [this](const QString &why) { rejectedReason = why; });
    QFile file(server.tokenFilePath());
    CHECK(file.open(QIODevice::ReadOnly));
    token = QString::fromLatin1(file.readAll().trimmed());
  }
  QJsonObject hello(const QJsonArray &protocols = {1, 2, 3}) const {
    return {{"type", "hello"}, {"protocol", 1}, {"supportedProtocols", protocols},
            {"sessionId", QStringLiteral("0123456789abcdef0123456789abcdef")}, {"token", token},
            {"pid", QCoreApplication::applicationPid()}};
  }
  bool closed(const char *fragment) {
    return waitUntil([&] { return closedReason.contains(QString::fromLatin1(fragment)); }, 3000);
  }
};

mode_t modeOf(const QString &path) {
  struct stat st {};
  return ::lstat(QFile::encodeName(path).constData(), &st) == 0 ? st.st_mode & 07777 : 0;
}

void checkIpcServer(const QString &base) {
  int n = 0;
  const auto dir = [&] { return base + QStringLiteral("/i%1").arg(++n); };
  {
    const QString path = dir();
    IpcFixture f(path);
    CHECK(modeOf(path) == 0700 && modeOf(f.server.tokenFilePath()) == 0600 && QFile::exists(f.server.socketPath()));
    QString error;
    GeckoIpcServer again;
    CHECK(!again.listen(path, &error));  // never reuses an existing directory
    CHECK(!f.server.send({{"type", "load_url"}}));  // nothing before authentication
    IpcClient client(f.server.socketPath());
    client.send(f.hello());
    const QJsonObject welcome = client.next();
    CHECK(welcome.value("type") == "welcome" && welcome.value("protocol") == 3 &&
          welcome.value("sessionId") == "0123456789abcdef0123456789abcdef" &&
          welcome.value("windowTag").toString() == f.server.windowTag() && f.server.isAuthenticated());
    // One session only.
    IpcClient second(f.server.socketPath());
    CHECK(waitUntil([&] { return f.rejectedReason.contains(QStringLiteral("session already exists")); }));
    // Commands carry the session id and increasing sequence numbers.
    CHECK(f.server.send({{"type", "list_tabs"}}) && f.server.send({{"type", "list_tabs"}}));
    const QJsonObject first = client.next(), next = client.next();
    CHECK(first.value("sessionId") == welcome.value("sessionId") && first.value("commandSequence") == 1 &&
          next.value("commandSequence") == 2);
    // An unknown event in sequence is ignored; an event out of sequence ends the session.
    QStringList received;
    QObject::connect(&f.server, &GeckoIpcServer::messageReceived,
                     [&](const QJsonObject &m) { received.append(m.value("type").toString()); });
    client.send({{"type", "something-new"}, {"sessionId", welcome.value("sessionId")}, {"sequence", 1}});
    client.send({{"type", "focus"}, {"reason", "x"}, {"active", true}, {"sessionId", welcome.value("sessionId")}, {"sequence", 2}});
    CHECK(waitUntil([&] { return received == QStringList{QStringLiteral("focus")}; }));
    client.send({{"type", "focus"}, {"reason", "x"}, {"active", true}, {"sessionId", welcome.value("sessionId")}, {"sequence", 2}});
    CHECK(f.closed("stale or reordered"));
    f.server.shutdown();
    CHECK(!QFileInfo::exists(f.server.socketPath()) && !QFileInfo::exists(path));
  }
  const auto authenticated = [&](IpcFixture &f, IpcClient &client) {
    client.send(f.hello());
    return client.next().value("sessionId").toString();
  };
  const auto expectEnd = [&](const char *what, const std::function<void(IpcFixture &, IpcClient &, const QString &)> &act,
                             const char *reason) {
    IpcFixture f(dir());
    IpcClient client(f.server.socketPath());
    act(f, client, authenticated(f, client));
    if (!f.closed(reason)) std::cout << what << ": '" << f.closedReason.toStdString() << "'" << std::endl;
    CHECK(f.closedReason.contains(QString::fromLatin1(reason)));
  };
  expectEnd("oversized", [](IpcFixture &, IpcClient &c, const QString &) { c.raw(QByteArray::fromHex("00010001") + "x"); },
            "invalid frame length");
  expectEnd("utf8", [](IpcFixture &, IpcClient &c, const QString &) { c.raw(QByteArray::fromHex("00000002c328")); },
            "invalid UTF-8");
  expectEnd("not json", [](IpcFixture &, IpcClient &c, const QString &) { c.raw(QByteArray::fromHex("000000025b5d")); },
            "malformed frame");
  expectEnd("other session", [](IpcFixture &, IpcClient &c, const QString &) {
    c.send({{"type", "focus"}, {"reason", "x"}, {"active", true}, {"sessionId", "ffffffffffffffffffffffffffffffff"}, {"sequence", 1}});
  }, "stale or reordered");
  expectEnd("bad tab id", [](IpcFixture &, IpcClient &c, const QString &session) {
    c.send({{"type", "tab-closed"}, {"tabId", "tab-../../x"}, {"sessionId", session}, {"sequence", 1}});
  }, "invalid tab-closed event");
  expectEnd("bad window tag", [](IpcFixture &, IpcClient &c, const QString &session) {
    c.send({{"type", "window-tagged"}, {"tag", "0000"}, {"sessionId", session}, {"sequence", 1}});
  }, "invalid window-tagged event");

  const auto expectRefused = [&](const char *what, const std::function<void(IpcFixture &, IpcClient &)> &act, const char *reason) {
    IpcFixture f(dir());
    IpcClient client(f.server.socketPath());
    act(f, client);
    if (!f.closed(reason)) std::cout << what << ": '" << f.closedReason.toStdString() << "'" << std::endl;
    CHECK(f.closedReason.contains(QString::fromLatin1(reason)) && !f.server.isAuthenticated());
  };
  expectRefused("wrong token", [](IpcFixture &f, IpcClient &c) {
    QJsonObject hello = f.hello();
    hello.insert("token", QString(64, QLatin1Char('0')));
    c.send(hello);
  }, "authentication failed");
  expectRefused("wrong pid", [](IpcFixture &f, IpcClient &c) {
    QJsonObject hello = f.hello();
    hello.insert("pid", 1);
    c.send(hello);
  }, "authentication failed");
  expectRefused("old protocol", [](IpcFixture &f, IpcClient &c) { c.send(f.hello({1, 2})); }, "does not support protocol 3");
  expectRefused("no hello", [](IpcFixture &f, IpcClient &) { f.server.setHelloTimeout(1); }, "no hello in time");
  {
    IpcFixture f(dir());
    f.server.setExpectedPeerPid(1);  // another process
    IpcClient client(f.server.socketPath());
    CHECK(waitUntil([&] { return f.rejectedReason.contains(QStringLiteral("is not the launched host")); }));
  }
  {
    QString error;
    GeckoIpcServer server;
    CHECK(!server.listen(base + QLatin1Char('/') + QString(120, QLatin1Char('x')), &error) &&
          error.contains(QStringLiteral("too long")));
  }
  std::cout << "PASS ipc transport" << std::endl;
}

void checkEmbedderWithoutXcb() {
  GeckoX11Embedder embedder;
  QString why;
  CHECK(!embedder.isUsable(&why) && why.contains(QStringLiteral("xcb")));
  why.clear();
  CHECK(!embedder.findHostWindow(QCoreApplication::applicationPid(), QStringLiteral("DNG-HOST x"), &why) && !why.isEmpty());
  QWidget parent;
  CHECK(embedder.embed({}, &parent) == nullptr && embedder.inputFocus() == 0);
  embedder.focusEmbedded();
  embedder.focusQt(&parent);
  std::cout << "PASS x11 embedder refuses without xcb" << std::endl;
}

// ---------------------------------------------------------------------------

struct Paths {
  QString gre;
  QString runtimeRoot;
  QString ipcRoot;
};

GeckoSession::Config sessionConfig(const Paths &paths) {
  GeckoSession::Config config;
  config.runtime = {paths.gre, hostApplication()};
  config.runtimeRoot = paths.runtimeRoot;
  config.ipcRoot = paths.ipcRoot;
  config.display = GeckoHostProcess::Display::Headless;
  return config;
}

void startReady(GeckoSession &session, const Paths &paths) {
  QString error;
  if (!session.start(sessionConfig(paths), &error)) std::cout << error.toStdString() << std::endl;
  CHECK(session.hostPid() > 0);
  CHECK(waitUntil([&] { return session.state() == GeckoSession::State::Ready; }));
}

void loaded(View *view, const QString &fragment) {
  CHECK(view);
  CHECK(waitUntil([&] { return view->url().toString().contains(fragment) && !view->isLoading(); }));
}

QJsonObject launchReport(const GeckoSession &session) {
  QFile file(session.runDirectory() + QStringLiteral("/profile/fake-host-report.json"));
  CHECK(waitUntil([&] { return file.exists(); }));
  CHECK(file.open(QIODevice::ReadOnly));
  return QJsonDocument::fromJson(file.readAll()).object();
}

void checkSessionRefusals(const QString &base, const Paths &paths) {
  GeckoSession session;
  QString error;
  GeckoSession::Config config = sessionConfig(paths);
  config.runtime.greDirectory = base + QStringLiteral("/nowhere");
  CHECK(!session.start(config, &error) && error.contains(QStringLiteral("does not exist")) && session.hostPid() == 0);
  // A runtime root reached through a symlink, or writable by others.
  const QString link = base + QStringLiteral("/root-link");
  CHECK(QFile::link(paths.runtimeRoot, link));
  config = sessionConfig(paths);
  config.runtimeRoot = link;
  CHECK(!session.start(config, &error) && session.hostPid() == 0);
  const QString open = base + QStringLiteral("/open-root");
  CHECK(QDir().mkpath(open) && ::chmod(QFile::encodeName(open).constData(), 0777) == 0);
  config.runtimeRoot = open;
  CHECK(!session.start(config, &error) && session.hostPid() == 0);
  config = sessionConfig(paths);
  config.ipcRoot = open;
  CHECK(!session.start(config, &error) && session.hostPid() == 0);
  CHECK(QDir(paths.runtimeRoot).entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty());
  std::cout << "PASS session refuses unsafe paths before launching" << std::endl;
}

void checkSession(const Paths &paths) {
  qputenv("DALINIRA_TEST_NOT_FOR_GECKO", "1");
  GeckoSession session;
  startReady(session, paths);
  CHECK(session.protocol() == 3 && session.views().size() == 1);
  CHECK(Access::ipc(session)->tokenFileConsumed());
  CHECK(modeOf(session.runDirectory()) == 0700);
  CHECK(session.supportsCapability(EngineCapability::Navigation) && !session.supportsCapability(EngineCapability::JavaScript) &&
        !session.supportsCapability(EngineCapability::PrivateProfile));
  QWidget surface, other;
  CHECK(session.claimPresentation(&surface) && !session.claimPresentation(&other));

  // What the launcher gave the host.
  const QJsonObject report = launchReport(session);
  const QJsonObject env = report.value("environment").toObject();
  const QString run = session.runDirectory();
  CHECK(report.value("arguments").toArray().toVariantList() ==
        QVariantList({"-app", QFileInfo(hostApplication()).canonicalFilePath() + "/application.ini", "--new-instance",
                      "-profile", run + "/profile"}));
  CHECK(env.value("HOME") == run + "/home" && env.value("TMPDIR") == run + "/tmp" &&
        env.value("XDG_CONFIG_HOME") == run + "/home/.config" && env.value("MOZ_HEADLESS") == "1");
  CHECK(!env.contains("DALINIRA_TEST_NOT_FOR_GECKO") && !env.contains("QT_QPA_PLATFORM") &&
        !env.contains("LD_PRELOAD") && !env.contains("LD_LIBRARY_PATH"));
  CHECK(report.value("coreSoft").toInteger() == 0 && report.value("coreHard").toInteger() == 0);
  CHECK(report.value("deathSignal").toInt() == SIGTERM);
  CHECK(report.value("workingDirectory") == run);
  // Only the standard streams were inherited: /dev/null and the log. (The
  // descriptor that listed /proc/self/fd is already closed: no target.)
  QStringList inherited;
  for (const auto &target : report.value("descriptors").toArray()) {
    if (!target.toString().isEmpty() && !target.toString().startsWith(QLatin1String("/proc/")))
      inherited.append(target.toString());
  }
  CHECK(inherited == QStringList({"/dev/null", run + "/host-output.log", run + "/host-output.log"}));
  const QString userJs = report.value("userJs").toString();
  CHECK(userJs.contains("browser.policies.alternatePath") &&
        userJs.contains(QFileInfo(hostApplication()).canonicalFilePath() + "/distribution/policies.json") &&
        userJs.contains("toolkit.policies.perUserDir\", false"));
  CHECK(modeOf(run + "/profile/user.js") == 0600 && modeOf(run + "/profile") == 0700 && modeOf(run + "/home") == 0700);

  // A foreign client of the same user is refused; the session goes on.
  {
    QLocalSocket foreign;
    foreign.connectToServer(Access::ipc(session)->socketPath());
    CHECK(waitUntil([&] { return foreign.state() == QLocalSocket::UnconnectedState; }));
    CHECK(session.state() == GeckoSession::State::Ready);
  }
  CHECK(session.stopHost(0).value("how") == "refused-live-session" && session.state() == GeckoSession::State::Ready);

  // Views, activation and history.
  View *alpha = session.view(session.activeTabId());
  result(alpha->load(QUrl(QStringLiteral("http://fake.test/alpha"))));
  loaded(alpha, "alpha");
  const QString alphaDocument = alpha->documentId();
  View *beta = session.view(result(session.createView(QUrl(QStringLiteral("http://fake.test/beta")), false))
                                .value("tabId").toString());
  loaded(beta, "beta");
  CHECK(alpha != beta && session.activeTabId() == alpha->tabId() && beta->lifecycleState() == View::State::Inactive);
  for (int i = 0; i < 12; ++i) {
    beta->setEngineActive(true);
    alpha->setEngineActive(true);
  }
  CHECK(waitUntil([&] { return session.activeTabId() == alpha->tabId() && alpha->isEngineActive(); }));
  CHECK(alpha->documentId() == alphaDocument);
  CHECK(result(alpha->setEngineActive(false), "rejected").value("code") == "unsupported-independent-surface");
  result(beta->load(QUrl(QStringLiteral("http://fake.test/page2"))));
  loaded(beta, "page2");
  result(beta->back());
  loaded(beta, "beta");
  CHECK(waitUntil([&] { return beta->canGoForward(); }));

  // Cancellation: queued (never sent) and sent (may have applied, completes once).
  const int viewCount = session.views().size();
  GeckoRequest *queued = session.createView();
  int queuedFinished = 0;
  QObject::connect(queued, &GeckoRequest::finished, [&] { ++queuedFinished; });
  queued->cancel();
  CHECK(!result(queued, "cancelled").value("mayHaveApplied").toBool());
  QThread::msleep(100);
  QCoreApplication::processEvents();
  CHECK(session.views().size() == viewCount && queuedFinished == 1);
  GeckoRequest *sent = session.listViews();
  int sentFinished = 0;
  const auto cancelOnSend = QObject::connect(&session, &GeckoSession::requestSent, [&](const QString &id) {
    if (id == sent->id()) sent->cancel();
  });
  QObject::connect(sent, &GeckoRequest::finished, [&] { ++sentFinished; });
  CHECK(result(sent, "cancelled").value("mayHaveApplied").toBool());
  QObject::disconnect(cancelOnSend);
  QThread::msleep(100);
  QCoreApplication::processEvents();
  CHECK(sentFinished == 1);

  // Deadlines: before dispatch nothing was sent; after it, it may have applied.
  CHECK(!result(session.submit(QStringLiteral("list_tabs"), {}, 0), "timed-out").value("mayHaveApplied").toBool());
  GeckoRequest *silent = session.submit(QStringLiteral("load_url"),
                                        {{"tabId", beta->tabId()}, {"url", "http://fake.test/silent"}}, 300);
  CHECK(result(silent, "timed-out").value("mayHaveApplied").toBool());

  // A reply nobody waits for is dropped; a reply to another command is refused.
  result(beta->load(QUrl(QStringLiteral("http://fake.test/stale"))));
  loaded(beta, "stale");
  CHECK(result(beta->load(QUrl(QStringLiteral("http://fake.test/wrong-command"))), "rejected").value("code") == "invalid-reply");
  CHECK(session.state() == GeckoSession::State::Ready);

  // Unsaved documents: nothing is closed or navigated away without a
  // confirmation bound to the very document.
  result(alpha->load(QUrl(QStringLiteral("http://fake.test/dirty"))));
  CHECK(waitUntil([&] { return alpha->hasBeforeUnload(); }));
  const QString dirtyDocument = alpha->documentId();
  CHECK(result(alpha->close(), "rejected").value("code") == "beforeunload-confirmation-required");
  CHECK(result(alpha->load(QUrl(QStringLiteral("http://fake.test/page2"))), "rejected").value("code") ==
        "beforeunload-navigation-blocked");
  CHECK(result(alpha->close(true, QStringLiteral("other-document")), "rejected").value("code") == "stale-document");
  CHECK(alpha->documentId() == dirtyDocument && alpha->lifecycleState() != View::State::Closed);
  result(alpha->close(true, dirtyDocument));
  CHECK(waitUntil([&] { return alpha->lifecycleState() == View::State::Closed; }));
  CHECK(session.view(beta->tabId()) == beta);
  CHECK(result(alpha->reload(), "rejected").value("code") == "invalid-view-or-request-limit");

  // A content process crash: the document is lost until explicitly recovered.
  const qint64 victim = beta->contentProcessId();
  bool owned = false;
  for (const auto &process : session.hostProcesses()) owned = owned || process.pid == victim;
  CHECK(victim > 0 && owned);
  int losses = 0;
  QObject::connect(beta, &View::stateLost, [&] { ++losses; });
  const QString crashedId = beta->tabId();
  CHECK(::kill(static_cast<pid_t>(victim), SIGKILL) == 0);
  CHECK(waitUntil([&] { return beta->lifecycleState() == View::State::Crashed && losses > 0; }));
  CHECK(beta->contentProcessId() == 0 && beta->tabId() == crashedId);
  CHECK(result(beta->reload(), "rejected").value("code") == "recovery-required");
  result(beta->recoverDocument());
  CHECK(waitUntil([&] { return beta->lifecycleState() != View::State::Crashed && beta->contentProcessId() > 0; }));
  CHECK(beta->tabId() == crashedId && beta->contentProcessId() != victim);

  // Events of an earlier generation change nothing.
  const QString selected = session.activeTabId();
  Access::receive(session, session.generation() - 1, {{"type", "active-tab-changed"}, {"tabId", crashedId}});
  CHECK(session.activeTabId() == selected);

  // An event out of sequence ends the session: every view and request is lost.
  const quint64 firstGeneration = session.generation();
  int sessionLosses = 0;
  QObject::connect(&session, &GeckoSession::sessionStateLost, [&] { ++sessionLosses; });
  GeckoRequest *broken = beta->load(QUrl(QStringLiteral("http://fake.test/break-sequence")));
  CHECK(waitUntil([&] { return session.state() == GeckoSession::State::Disconnected; }));
  result(broken, "disconnected");
  CHECK(beta->lifecycleState() == View::State::Lost && sessionLosses == 1 && !session.lostSessionSnapshot().isEmpty());
  CHECK(result(beta->reload(), "rejected").value("code") == "session-not-ready");
  const qint64 oldHost = session.hostPid();
  const auto cleanup = session.stopHost(3000);
  CHECK(cleanup.value("leftoversKilled").toArray().isEmpty());
  CHECK(waitUntil([&] { return !alive(oldHost); }));

  // A restart is a new, empty session; nothing of the old one applies.
  startReady(session, paths);
  CHECK(session.generation() > firstGeneration && session.views().size() == 1 && session.activeTabId() != crashedId);
  Access::receive(session, firstGeneration, {{"type", "active-tab-changed"}, {"tabId", crashedId}});
  CHECK(session.activeTabId() != crashedId);

  // The host dies: the session is lost, its content process dies with it.
  const qint64 host = session.hostPid();
  qint64 content = 0;
  for (const auto &process : session.hostProcesses()) content = process.pid;
  CHECK(content > 0);
  bool exited = false;
  QObject::connect(&session, &GeckoSession::hostExited, [&] { exited = true; });
  CHECK(::kill(static_cast<pid_t>(host), SIGKILL) == 0);
  CHECK(waitUntil([&] { return exited && session.state() == GeckoSession::State::Disconnected; }));
  CHECK(waitUntil([&] { return !alive(content); }));
  session.stopHost(1000);

  // Guarded shutdown, then nothing of the host is left.
  startReady(session, paths);
  View *last = session.view(session.activeTabId());
  result(last->load(QUrl(QStringLiteral("http://fake.test/dirty"))));
  CHECK(waitUntil([&] { return last->hasBeforeUnload(); }));
  CHECK(result(session.shutdown(), "rejected").value("code") == "beforeunload-confirmation-required");
  CHECK(session.state() == GeckoSession::State::Ready);
  QList<qint64> tree{session.hostPid()};
  for (const auto &process : session.hostProcesses()) tree.append(process.pid);
  result(session.shutdown(true, session.documentSnapshot()));
  const auto final = session.stopHost(3000);
  CHECK(final.value("leftoversKilled").toArray().isEmpty());
  for (const qint64 pid : tree) CHECK(waitUntil([&] { return !alive(pid); }));
  session.releasePresentation(&surface);
  std::cout << "PASS session lifecycle against the fake host" << std::endl;
}

// The host never outlives its parent: a helper process starts a session and
// is killed.
void checkOrphanPrevention(const QString &base, const Paths &paths) {
  const QString report = base + QStringLiteral("/orphan.txt");
  QProcess parent;
  parent.setProgram(QCoreApplication::applicationFilePath());
  parent.setArguments({QStringLiteral("--orphan-parent"), paths.gre, paths.runtimeRoot, paths.ipcRoot, report});
  parent.setProcessChannelMode(QProcess::ForwardedChannels);
  parent.start();
  CHECK(parent.waitForStarted(5000));
  CHECK(waitUntil([&] { return QFileInfo(report).size() > 0; }, 15000));
  QFile file(report);
  CHECK(file.open(QIODevice::ReadOnly));
  QList<qint64> tree;
  for (const QByteArray &pid : file.readAll().trimmed().split(' ')) tree.append(pid.toLongLong());
  CHECK(tree.size() >= 2);
  for (const qint64 pid : tree) CHECK(alive(pid));
  CHECK(::kill(static_cast<pid_t>(parent.processId()), SIGKILL) == 0);
  CHECK(parent.waitForFinished(5000));
  for (const qint64 pid : tree) CHECK(waitUntil([&] { return !alive(pid); }));
  std::cout << "PASS host and content die with a killed parent" << std::endl;
}

int orphanParent(const QStringList &args) {
  GeckoSession session;
  Paths paths{args.at(0), args.at(1), args.at(2)};
  startReady(session, paths);
  QStringList tree{QString::number(session.hostPid())};
  for (const auto &process : session.hostProcesses()) tree.append(QString::number(process.pid));
  QFile file(args.at(3));
  CHECK(file.open(QIODevice::WriteOnly));
  file.write(tree.join(QLatin1Char(' ')).toLatin1());
  file.close();
  for (;;) QThread::sleep(1);  // until killed
}

}  // namespace

int main(int argc, char *argv[]) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app(argc, argv);
  const QStringList args = app.arguments().mid(1);
  if (args.size() == 5 && args.first() == QLatin1String("--orphan-parent")) return orphanParent(args.mid(1));

  QTemporaryDir root;
  CHECK(root.isValid());
  const QString base = QFileInfo(root.path()).canonicalFilePath();
  Paths paths{makeFakeRuntime(base + QStringLiteral("/gre")), makePrivateDirectory(base + QStringLiteral("/runtime")),
              makePrivateDirectory(base + QStringLiteral("/ipc"))};
  std::cout << "=== GECKO ADAPTER ===" << std::endl;
  checkRuntimeValidation(base + QStringLiteral("/validation"));
  checkLaunchEnvironment();
  checkIpcServer(makePrivateDirectory(base + QStringLiteral("/ipc-tests")));
  checkEmbedderWithoutXcb();
  checkSessionRefusals(base, paths);
  checkSession(paths);
  checkOrphanPrevention(base, paths);
  std::cout << "=== ALL GECKO ADAPTER TESTS PASSED ===" << std::endl;
  return 0;
}
