#include "engine/webkit/webkit_network_filter.h"
#include "core/application_identity.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QLocalServer>
#include <QLocalSocket>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>

#include <sys/socket.h>
#include <unistd.h>

namespace dalinira::engine {

namespace {

constexpr int kMaxLineBytes = 160 * 1024;
constexpr int kMaxUrlBytes = 64 * 1024;
constexpr qint64 kRepeatWindowMs = 2000;
constexpr int kMaxRecentBlocks = 64;

QByteArray randomToken() {
  QByteArray bytes(24, Qt::Uninitialized);
  QRandomGenerator::system()->generate(reinterpret_cast<quint32 *>(bytes.data()),
                                       reinterpret_cast<quint32 *>(bytes.data() + bytes.size()));
  return bytes.toHex();
}

}  // namespace

// Lives on the filter thread and owns the listening socket and connections.
class WebKitNetworkFilter::Server final : public QObject {
 public:
  explicit Server(WebKitNetworkFilter *filter) : filter_(filter) {}

  bool listen(const QString &path) {
    server_ = new QLocalServer(this);
    server_->setSocketOptions(QLocalServer::UserAccessOption);
    if (!server_->listen(path)) {
      qWarning() << "WebKitNetworkFilter: cannot listen on" << path << server_->errorString();
      return false;
    }
    connect(server_, &QLocalServer::newConnection, this, [this] {
      while (QLocalSocket *socket = server_->nextPendingConnection()) accept(socket);
    });
    return true;
  }

  void stop() {
    if (server_) server_->close();
    const auto sockets = findChildren<QLocalSocket *>();
    for (QLocalSocket *socket : sockets) socket->abort();
  }

 private:
  struct Connection {
    QByteArray buffer;
    bool authenticated = false;
    qint64 pid = 0;  // The web process, from the kernel (SO_PEERCRED).
  };

  void accept(QLocalSocket *socket) {
    socket->setParent(this);
    auto connection = std::make_shared<Connection>();
    ++filter_->connections_;
    connect(socket, &QLocalSocket::disconnected, this, [this, socket, connection] {
      --filter_->connections_;
      if (connection->pid > 0) filter_->forgetProcess(connection->pid);
      socket->deleteLater();
    });
    connect(socket, &QLocalSocket::readyRead, this, [this, socket, connection] {
      // Never read an unbounded peer-controlled allocation before enforcing
      // the protocol limit. One extra byte distinguishes a maximum-sized
      // newline-terminated line from an oversized unterminated one.
      while (socket->bytesAvailable() > 0) {
        const qint64 room = kMaxLineBytes + 1 - connection->buffer.size();
        if (room <= 0) {
          socket->abort();
          return;
        }
        connection->buffer.append(socket->read(qMin(room, socket->bytesAvailable())));
        qsizetype newline;
        while ((newline = connection->buffer.indexOf('\n')) >= 0) {
          if (newline > kMaxLineBytes) {
            socket->abort();
            return;
          }
          const QByteArray line = connection->buffer.left(newline);
          connection->buffer.remove(0, newline + 1);
          if (!handleLine(socket, *connection, line)) {
            socket->abort();
            return;
          }
        }
        if (connection->buffer.size() > kMaxLineBytes) {
          socket->abort();
          return;
        }
      }
    });
  }

  bool handleLine(QLocalSocket *socket, Connection &connection, const QByteArray &line) {
    const QList<QByteArray> fields = line.split('\t');
    if (!connection.authenticated) {
      // Only web processes of this browser, which received the token from
      // WebKit's extension initialization data, may use the filter.
      connection.authenticated = fields.size() == 2 && fields[0] == "HELLO" && fields[1] == filter_->token_ &&
                                 peerProcessId(socket, &connection.pid);
      return connection.authenticated;
    }
    if (fields.size() == 2 && (fields[0] == "P" || fields[0] == "X")) {
      bool pageOk = false;
      const quint64 pageId = fields[1].toULongLong(&pageOk);
      if (!pageOk || pageId == 0) return false;
      if (connection.pid > 0) {
        if (fields[0] == "P")
          filter_->notePageProcess(pageId, connection.pid);
        else
          filter_->forgetPageProcess(pageId, connection.pid);
      }
      return true;
    }
    if (fields.size() != 8 || fields[0] != "Q") return false;
    bool seqOk = false;
    bool pageOk = false;
    const quint64 seq = fields[1].toULongLong(&seqOk);
    Request request;
    request.pageId = fields[2].toULongLong(&pageOk);
    if (!seqOk || seq == 0 || !pageOk || request.pageId == 0 || connection.pid <= 0 ||
        filter_->processIdForPage(request.pageId) != connection.pid)
      return false;
    if (fields[3].isEmpty() || fields[3].size() > 32 || fields[3].contains('\r') || fields[4].size() > 64 ||
        fields[4].contains('\r') || (fields[5] != "0" && fields[5] != "1") || fields[6].size() > kMaxUrlBytes ||
        fields[7].size() > kMaxUrlBytes)
      return false;
    request.method = QString::fromLatin1(fields[3]);
    request.destination = QString::fromLatin1(fields[4]);
    request.redirected = fields[5] == "1";
    request.url = QUrl::fromEncoded(fields[6], QUrl::StrictMode);
    request.firstPartyUrl = QUrl::fromEncoded(fields[7], QUrl::StrictMode);
    const auto isHttpUrl = [](const QUrl &url) {
      return url.isValid() && !url.host().isEmpty() &&
             (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https"));
    };
    if (!isHttpUrl(request.url)) return false;
    // A document without a web URL of its own (about:blank or a srcdoc a
    // popup's opener wrote) reports that as its first party; it is decided
    // without one instead of dropping the whole web process's connection,
    // which also serves the opener's page when they share the process.
    if (!isHttpUrl(request.firstPartyUrl)) request.firstPartyUrl = QUrl();

    const Verdict verdict = filter_->evaluate(request);
    QByteArray reply = QByteArray::number(seq) + '\t';
    switch (verdict.action) {
      case Action::Block:
        reply += 'B';
        break;
      case Action::Redirect: {
        const QByteArray target = verdict.redirectUrl.toEncoded();
        if (verdict.redirectUrl.isValid() && !target.contains('\t') && !target.contains('\n'))
          reply += "R\t" + target;
        else
          reply += 'A';
        break;
      }
      case Action::Allow:
        reply += 'A';
        break;
    }
    reply += '\n';
    socket->write(reply);
    socket->flush();
    ++filter_->answered_;
    return true;
  }

  static bool peerProcessId(QLocalSocket *socket, qint64 *pid) {
    ucred credentials{};
    socklen_t length = sizeof(credentials);
    if (getsockopt(static_cast<int>(socket->socketDescriptor()), SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0)
      return false;
    if (length != sizeof(credentials) || credentials.pid <= 0 || credentials.uid != geteuid()) return false;
    *pid = credentials.pid;
    return true;
  }

  WebKitNetworkFilter *filter_ = nullptr;
  QLocalServer *server_ = nullptr;
};

WebKitNetworkFilter::WebKitNetworkFilter(QObject *parent) : QObject(parent) {}

WebKitNetworkFilter::~WebKitNetworkFilter() {
  if (thread_) {
    if (server_) QMetaObject::invokeMethod(server_, [this] { server_->stop(); }, Qt::BlockingQueuedConnection);
    // The server is deleted on its own thread when the thread finishes.
    thread_->quit();
    thread_->wait();
  }
}

QString WebKitNetworkFilter::extensionDirectory() {
  return QDir(dalinira::application_identity::applicationDirectory()).filePath(QStringLiteral("webkit-extensions"));
}

bool WebKitNetworkFilter::start() {
  if (running_) return true;
  QString base = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
  if (base.isEmpty()) base = QDir::tempPath();
  // QTemporaryDir creates the directory with 0700 permissions.
  socketDir_ = std::make_unique<QTemporaryDir>(QDir(base).filePath(QStringLiteral("dalinira-webkit-XXXXXX")));
  if (!socketDir_->isValid()) {
    qWarning() << "WebKitNetworkFilter: cannot create socket directory in" << base;
    return false;
  }
  socketPath_ = socketDir_->filePath(QStringLiteral("filter.sock"));
  token_ = randomToken();

  thread_ = new QThread(this);
  thread_->setObjectName(QStringLiteral("DaliNiraWebKitFilter"));
  server_ = new Server(this);
  server_->moveToThread(thread_);
  connect(thread_, &QThread::finished, server_, &QObject::deleteLater);
  thread_->start();
  bool listening = false;
  const bool invoked = QMetaObject::invokeMethod(
      server_, [this, &listening] { listening = server_->listen(socketPath_); }, Qt::BlockingQueuedConnection);
  if (!invoked) qWarning() << "WebKitNetworkFilter: could not initialize socket server thread";
  running_ = listening;
  return running_;
}

void WebKitNetworkFilter::setRoute(quint64 pageId, Evaluator evaluator) {
  if (pageId == 0) return;
  QMutexLocker locker(&routesMutex_);
  if (evaluator)
    routes_.insert(pageId, std::make_shared<const Evaluator>(std::move(evaluator)));
  else
    routes_.remove(pageId);
}

qint64 WebKitNetworkFilter::processIdForPage(quint64 pageId) const {
  QMutexLocker locker(&processesMutex_);
  return pageProcesses_.value(pageId, 0);
}

void WebKitNetworkFilter::notePageProcess(quint64 pageId, qint64 pid) {
  QMutexLocker locker(&processesMutex_);
  pageProcesses_.insert(pageId, pid);
}

void WebKitNetworkFilter::forgetPageProcess(quint64 pageId, qint64 pid) {
  QMutexLocker locker(&processesMutex_);
  // After a process swap the new process may register the page before the
  // old one reports that it let go of it.
  if (pageProcesses_.value(pageId) == pid) pageProcesses_.remove(pageId);
}

void WebKitNetworkFilter::forgetProcess(qint64 pid) {
  QMutexLocker locker(&processesMutex_);
  pageProcesses_.removeIf([pid](const auto &entry) { return entry.value() == pid; });
}

void WebKitNetworkFilter::removeRoute(quint64 pageId) {
  QMutexLocker locker(&routesMutex_);
  routes_.remove(pageId);
}

WebKitNetworkFilter::Verdict WebKitNetworkFilter::evaluate(const Request &request) {
  std::shared_ptr<const Evaluator> evaluator;
  {
    QMutexLocker locker(&routesMutex_);
    evaluator = routes_.value(request.pageId);
  }
  // Pages without a blocker (none configured for their window) load unfiltered.
  if (!evaluator || !request.url.isValid()) return {};

  const QString url = request.url.toString(QUrl::FullyEncoded);
  const QString document = request.firstPartyUrl.toString(QUrl::FullyEncoded);
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  {
    QMutexLocker locker(&recentMutex_);
    if (request.destination == QLatin1String("document")) {
      // A new top-level document (navigation or reload, possibly after a site
      // switch changed the policy): its requests are decided afresh.
      recentBlocks_.removeIf([&](const RecentBlock &recent) { return recent.pageId == request.pageId; });
    } else {
      for (const RecentBlock &recent : std::as_const(recentBlocks_)) {
        if (recent.pageId == request.pageId && recent.document == document && recent.url == url &&
            recent.method == request.method && now - recent.atMs <= kRepeatWindowMs)
          return Verdict{Action::Block, {}};
      }
    }
  }
  const Verdict verdict = (*evaluator)(request);
  if (verdict.action == Action::Block) {
    QMutexLocker locker(&recentMutex_);
    recentBlocks_.append(RecentBlock{request.pageId, document, url, request.method, now});
    while (recentBlocks_.size() > kMaxRecentBlocks) recentBlocks_.removeFirst();
  }
  return verdict;
}

} // namespace dalinira::engine
