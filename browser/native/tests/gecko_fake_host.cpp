// A stand-in for the DaliNira Gecko Host (browser/resources/gecko-host) for
// the adapter tests: it is launched exactly like Gecko's `firefox` from a
// test runtime directory and speaks the host side of IPC protocol 3 as
// ipc.js and tabs.js do. It runs no web content. One child process stands in
// for a content process; if it dies, every tab "crashes" as with a real
// content process crash.
//
// At start it writes <profile>/fake-host-report.json: its arguments,
// environment, core-file limit, parent-death signal, open file descriptors
// and the profile's user.js, so the tests can check what the launcher gave it.
//
// Test URLs (load_url) select behaviours: ".../dirty" pages have a
// beforeunload handler; ".../silent" is never answered; ".../stale" is
// answered twice, first for a request id nobody sent; ".../wrong-command"
// is answered as another command; ".../break-sequence" sends an event out of
// sequence; ".../exit" ends the host; ".../open-popup" opens a tab of its
// own (source "content", with its opener) at http://fake.test/popup.

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRandomGenerator>
#include <QtEndian>

#include <csignal>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <unistd.h>

namespace {

QString randomHex(int bytes) {
  QByteArray raw(bytes, Qt::Uninitialized);
  for (char &c : raw) c = static_cast<char>(QRandomGenerator::global()->generate());
  return QString::fromLatin1(raw.toHex());
}

struct Tab {
  QString id;
  QString url = QStringLiteral("about:blank");
  QString documentId;
  QStringList back;
  QStringList forward;
  bool dirty = false;
  bool crashed = false;
};

class FakeHost : public QObject {
 public:
  bool start() {
    const QString tokenFile = qEnvironmentVariable("DNG_IPC_TOKEN_FILE");
    QFile token(tokenFile);
    if (!token.open(QIODevice::ReadOnly)) return false;
    token_ = QString::fromLatin1(token.readAll().trimmed());
    token.close();
    QFile::remove(tokenFile);  // as the real host: the token is single-use
    spawnContent();
    connect(&socket_, &QLocalSocket::readyRead, this, [this] { read(); });
    connect(&socket_, &QLocalSocket::disconnected, qApp, [] { QCoreApplication::exit(0); });
    socket_.connectToServer(qEnvironmentVariable("DNG_IPC_SOCKET"));
    if (!socket_.waitForConnected(5000)) return false;
    sessionId_ = randomHex(16);
    writeFrame({{"type", "hello"}, {"protocol", 1}, {"supportedProtocols", QJsonArray{1, 2, 3}},
                {"sessionId", sessionId_}, {"token", token_}, {"pid", QCoreApplication::applicationPid()}});
    return true;
  }

 private:
  void spawnContent() {
    content_ = new QProcess(this);
    content_->setProgram(QCoreApplication::applicationFilePath());
    content_->setArguments({QStringLiteral("--fake-content")});
    content_->setChildProcessModifier([] { ::prctl(PR_SET_PDEATHSIG, SIGKILL); });
    QProcess *process = content_;
    connect(process, &QProcess::finished, this, [this, process] {
      if (process != content_) return;
      // The content process died: every tab it hosted lost its document.
      for (Tab &tab : tabs_) {
        tab.crashed = true;
        send({{"type", "content-crashed"}, {"tabId", tab.id}, {"isTopFrame", true}});
        send({{"type", "session-state-lost"}, {"tabId", tab.id}, {"stateLost", true}, {"scope", "document"},
              {"reason", "content-process-crashed"}});
      }
      spawnContent();
    });
    content_->start();
    content_->waitForStarted(5000);
  }

  qint64 contentPid() const { return content_ ? content_->processId() : 0; }

  void writeFrame(const QJsonObject &message) {
    const QByteArray payload = QJsonDocument(message).toJson(QJsonDocument::Compact);
    QByteArray frame(4, Qt::Uninitialized);
    qToBigEndian<quint32>(static_cast<quint32>(payload.size()), frame.data());
    socket_.write(frame + payload);
    socket_.flush();
  }

  void send(QJsonObject message, qint64 sequence = 0) {
    message.insert(QStringLiteral("sessionId"), sessionId_);
    message.insert(QStringLiteral("sequence"), sequence ? sequence : ++sequence_);
    writeFrame(message);
  }

  void sendState(const Tab &tab) {
    send({{"type", "state"}, {"tabId", tab.id}, {"url", tab.url}, {"title", tab.url.section('/', -1)},
          {"loading", false}, {"canGoBack", !tab.back.isEmpty()}, {"canGoForward", !tab.forward.isEmpty()},
          {"documentURI", tab.url}, {"contentPid", tab.crashed ? QJsonValue() : QJsonValue(contentPid())},
          {"remoteType", "webIsolated=http://fake.test"}, {"mediaPlaying", false}, {"mediaAudible", false},
          {"progress", 1}, {"documentId", tab.documentId}, {"hasBeforeUnload", tab.dirty}, {"crashed", tab.crashed}});
  }

  void navigate(Tab &tab, const QString &url) {
    tab.url = url;
    tab.documentId = randomHex(8);
    tab.dirty = url.contains(QLatin1String("/dirty"));
    send({{"type", "progress"}, {"tabId", tab.id}, {"phase", "start"}, {"value", 0}});
    sendState(tab);
    send({{"type", "progress"}, {"tabId", tab.id}, {"phase", "stop"}, {"value", 1}, {"status", "0x0"}, {"ok", true}});
  }

  Tab &createTab(const QString &url, const QString &source, const QString &requestId, const QString &opener = {}) {
    Tab tab;
    tab.id = QStringLiteral("tab-") + randomHex(16);
    tab.documentId = randomHex(8);
    order_.append(tab.id);
    tabs_.insert(tab.id, tab);
    QJsonObject created{{"type", "tab-created"}, {"tabId", tab.id}, {"url", tab.url}, {"title", ""},
                        {"loading", false}, {"active", false}, {"source", source}};
    if (!requestId.isEmpty()) created.insert(QStringLiteral("requestId"), requestId);
    if (!opener.isEmpty()) created.insert(QStringLiteral("openerTabId"), opener);
    send(created);
    Tab &stored = tabs_[tab.id];
    if (!url.isEmpty()) navigate(stored, url);
    else sendState(stored);
    return stored;
  }

  void activate(const QString &id) {
    active_ = id;
    send({{"type", "active-tab-changed"}, {"tabId", id}});
    sendState(tabs_[id]);
  }

  void ack(const QJsonObject &command, const QString &tabId = {}) {
    QJsonObject reply{{"type", "ack"}, {"command", command.value("type")}, {"requestId", command.value("requestId")}};
    if (!tabId.isEmpty()) reply.insert(QStringLiteral("tabId"), tabId);
    send(reply);
  }

  void error(const QJsonObject &command, const QString &code) {
    QJsonObject reply{{"type", "error"}, {"command", command.value("type")}, {"requestId", command.value("requestId")},
                      {"code", code}};
    if (command.contains(QStringLiteral("tabId"))) reply.insert(QStringLiteral("tabId"), command.value("tabId"));
    send(reply);
  }

  void read() {
    buffer_ += socket_.readAll();
    while (buffer_.size() >= 4) {
      const quint32 length = qFromBigEndian<quint32>(buffer_.constData());
      if (buffer_.size() < 4 + qsizetype(length)) return;
      const QJsonObject message = QJsonDocument::fromJson(buffer_.mid(4, length)).object();
      buffer_.remove(0, 4 + length);
      handle(message);
    }
  }

  void handle(const QJsonObject &message) {
    const QString type = message.value(QStringLiteral("type")).toString();
    if (!welcomed_) {
      // Only the welcome of the embedder this host authenticated to.
      if (type != QLatin1String("welcome") || message.value(QStringLiteral("protocol")).toInt() != 3 ||
          message.value(QStringLiteral("sessionId")).toString() != sessionId_) {
        QCoreApplication::exit(4);
        return;
      }
      welcomed_ = true;
      const QString tag = message.value(QStringLiteral("windowTag")).toString();
      Tab &first = createTab({}, QStringLiteral("startup"), {});
      activate(first.id);
      send({{"type", "window-tagged"}, {"tag", tag}});
      return;
    }
    const qint64 commandSequence = message.value(QStringLiteral("commandSequence")).toInteger();
    if (message.value(QStringLiteral("sessionId")).toString() != sessionId_ || commandSequence <= commandSequence_ ||
        message.value(QStringLiteral("protocol")).toInt() != 3) {
      QCoreApplication::exit(5);  // the real host ends a session it cannot trust
      return;
    }
    commandSequence_ = commandSequence;
    const QString tabId = message.value(QStringLiteral("tabId")).toString();
    if (message.contains(QStringLiteral("tabId")) && !tabs_.contains(tabId)) {
      error(message, QStringLiteral("unknown-tab"));
      return;
    }
    if (type == QLatin1String("create_tab")) {
      Tab &tab = createTab(message.value(QStringLiteral("url")).toString(), QStringLiteral("embedder"),
                           message.value(QStringLiteral("requestId")).toString());
      ack(message, tab.id);
      if (message.value(QStringLiteral("activate")).toBool(true)) activate(tab.id);
    } else if (type == QLatin1String("activate_tab")) {
      ack(message, tabId);
      activate(tabId);
    } else if (type == QLatin1String("load_url")) {
      Tab &tab = tabs_[tabId];
      const QString url = message.value(QStringLiteral("url")).toString();
      if (tab.dirty) return error(message, QStringLiteral("beforeunload-navigation-blocked"));
      if (url.endsWith(QLatin1String("/silent"))) return;
      if (url.endsWith(QLatin1String("/exit"))) {
        ack(message, tabId);
        QCoreApplication::exit(0);
        return;
      }
      if (url.endsWith(QLatin1String("/break-sequence"))) {
        send({{"type", "focus"}, {"reason", "test"}, {"active", true}}, sequence_ + 5);
        return;
      }
      if (url.endsWith(QLatin1String("/wrong-command"))) {
        QJsonObject wrong = message;
        wrong.insert(QStringLiteral("type"), QStringLiteral("reload"));
        return ack(wrong, tabId);
      }
      if (url.endsWith(QLatin1String("/stale"))) {
        send({{"type", "ack"}, {"command", "load_url"}, {"requestId", "g999_stale_1"}, {"tabId", tabId}});
      }
      tab.forward.clear();
      tab.back.append(tab.url);
      ack(message, tabId);
      navigate(tab, url);
      if (url.endsWith(QLatin1String("/open-popup"))) {
        const QString popup = createTab(QStringLiteral("http://fake.test/popup"), QStringLiteral("content"), {}, tabId).id;
        activate(popup);
      }
    } else if (type == QLatin1String("back") || type == QLatin1String("forward")) {
      Tab &tab = tabs_[tabId];
      QStringList &from = type == QLatin1String("back") ? tab.back : tab.forward;
      QStringList &to = type == QLatin1String("back") ? tab.forward : tab.back;
      if (tab.dirty) return error(message, QStringLiteral("beforeunload-navigation-blocked"));
      if (from.isEmpty()) return error(message, QStringLiteral("no-history"));
      to.append(tab.url);
      ack(message, tabId);
      navigate(tab, from.takeLast());
    } else if (type == QLatin1String("reload") || type == QLatin1String("stop")) {
      if (tabs_[tabId].crashed) return error(message, QStringLiteral("recovery-required"));
      ack(message, tabId);
      sendState(tabs_[tabId]);
    } else if (type == QLatin1String("recover_tab")) {
      Tab &tab = tabs_[tabId];
      if (!tab.crashed) return error(message, QStringLiteral("not-crashed"));
      tab.crashed = false;
      tab.back.clear();
      tab.forward.clear();
      tab.documentId = randomHex(8);
      tab.dirty = false;
      ack(message, tabId);
      send({{"type", "recovered"}, {"tabId", tabId}, {"url", tab.url}, {"contentPid", contentPid()}});
      sendState(tab);
    } else if (type == QLatin1String("list_tabs")) {
      QJsonArray tabs;
      for (const QString &id : std::as_const(order_))
        tabs.append(QJsonObject{{"tabId", id}, {"url", tabs_[id].url}, {"title", ""}, {"loading", false},
                                {"active", id == active_}});
      send({{"type", "tabs-list"}, {"tabs", tabs}, {"activeTabId", active_}, {"requestId", message.value("requestId")}});
    } else if (type == QLatin1String("close_tab")) {
      Tab &tab = tabs_[tabId];
      if (message.value(QStringLiteral("discard")).toBool()) {
        if (message.value(QStringLiteral("documentId")).toString() != tab.documentId)
          return error(message, QStringLiteral("stale-document"));
      } else if (tab.dirty) {
        return error(message, QStringLiteral("beforeunload-confirmation-required"));
      }
      ack(message, tabId);
      tabs_.remove(tabId);
      order_.removeAll(tabId);
      send({{"type", "tab-closed"}, {"tabId", tabId}});
      if (order_.isEmpty()) activate(createTab({}, QStringLiteral("embedder"), {}).id);
      else if (active_ == tabId) activate(order_.last());
    } else if (type == QLatin1String("cancel_request")) {
      send({{"type", "ack"}, {"command", "cancel_request"}, {"requestId", message.value("requestId")}, {"cancelled", false}});
    } else if (type == QLatin1String("shutdown")) {
      if (message.value(QStringLiteral("discard")).toBool()) {
        const QJsonObject documents = message.value(QStringLiteral("documents")).toObject();
        for (const Tab &tab : std::as_const(tabs_)) {
          if (documents.value(tab.id).toString() != tab.documentId) return error(message, QStringLiteral("stale-document"));
        }
      } else {
        for (const Tab &tab : std::as_const(tabs_)) {
          if (tab.dirty) return error(message, QStringLiteral("beforeunload-confirmation-required"));
        }
      }
      ack(message);
      QCoreApplication::exit(0);
    } else {
      error(message, QStringLiteral("unknown-command"));
    }
  }

  QLocalSocket socket_;
  QPointer<QProcess> content_;
  QByteArray buffer_;
  QString token_;
  QString sessionId_;
  QString active_;
  QHash<QString, Tab> tabs_;
  QStringList order_;
  qint64 sequence_ = 0;
  qint64 commandSequence_ = 0;
  bool welcomed_ = false;
};

void writeReport(const QStringList &arguments) {
  const int profileIndex = arguments.indexOf(QStringLiteral("-profile"));
  if (profileIndex < 0 || profileIndex + 1 >= arguments.size()) return;
  const QString profile = arguments.at(profileIndex + 1);
  rlimit core{};
  ::getrlimit(RLIMIT_CORE, &core);
  int deathSignal = 0;
  ::prctl(PR_GET_PDEATHSIG, &deathSignal);
  QJsonObject environment;
  for (const QString &entry : QProcessEnvironment::systemEnvironment().toStringList())
    environment.insert(entry.section('=', 0, 0), entry.section('=', 1));
  QJsonArray descriptors;
  for (const QString &fd : QDir(QStringLiteral("/proc/self/fd")).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot))
    descriptors.append(QFile::symLinkTarget(QStringLiteral("/proc/self/fd/") + fd));
  QFile userJs(profile + QStringLiteral("/user.js"));
  const QByteArray userJsText = userJs.open(QIODevice::ReadOnly) ? userJs.readAll() : QByteArray();
  const QJsonObject report{{"arguments", QJsonArray::fromStringList(arguments)},
                           {"environment", environment},
                           {"workingDirectory", QDir::currentPath()},
                           {"coreSoft", qint64(core.rlim_cur)},
                           {"coreHard", qint64(core.rlim_max)},
                           {"deathSignal", deathSignal},
                           {"descriptors", descriptors},
                           {"userJs", QString::fromUtf8(userJsText)}};
  QFile out(profile + QStringLiteral("/fake-host-report.json"));
  if (out.open(QIODevice::WriteOnly)) out.write(QJsonDocument(report).toJson());
}

}  // namespace

int main(int argc, char *argv[]) {
  if (argc == 2 && qstrcmp(argv[1], "--fake-content") == 0) {
    for (;;) ::pause();  // a content process that only dies with its host
  }
  // Before QCoreApplication opens descriptors of its own.
  QStringList arguments;
  for (int i = 1; i < argc; ++i) arguments.append(QString::fromLocal8Bit(argv[i]));
  writeReport(arguments);
  QCoreApplication app(argc, argv);
  FakeHost host;
  if (!host.start()) return 3;
  return app.exec();
}
