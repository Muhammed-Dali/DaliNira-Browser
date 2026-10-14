#include "engine/gecko/gecko_ipc_server.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QLoggingCategory>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSet>
#include <QStringDecoder>
#include <QTimer>
#include <QtEndian>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

Q_LOGGING_CATEGORY(lcGeckoIpc, "dalinira.gecko.ipc")

namespace dalinira::engine {

namespace {

QByteArray randomHex(int bytes) {
  QByteArray raw(bytes, Qt::Uninitialized);
  QRandomGenerator::system()->generate(reinterpret_cast<quint32 *>(raw.data()),
                                       reinterpret_cast<quint32 *>(raw.data() + bytes));
  return raw.toHex();
}

bool constantTimeEquals(const QByteArray &a, const QByteArray &b) {
  if (a.size() != b.size()) return false;
  unsigned char diff = 0;
  for (qsizetype i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
  return diff == 0;
}

// Messages the host may send after authentication.
const QSet<QString> &hostMessageTypes() {
  static const QSet<QString> types = {
      QStringLiteral("window-tagged"), QStringLiteral("state"), QStringLiteral("progress"),
      QStringLiteral("load-error"), QStringLiteral("geometry"), QStringLiteral("focus"),
      QStringLiteral("ack"), QStringLiteral("error"), QStringLiteral("content-crashed"),
      QStringLiteral("recovered"), QStringLiteral("tab-created"), QStringLiteral("tab-closed"),
      QStringLiteral("active-tab-changed"), QStringLiteral("tabs-list"),
      QStringLiteral("session-state-lost"), QStringLiteral("view-close-requested"),
  };
  return types;
}

}  // namespace

GeckoIpcServer::GeckoIpcServer(QObject *parent) : QObject(parent) {
  server_.setSocketOptions(QLocalServer::UserAccessOption);
  server_.setMaxPendingConnections(2);
  connect(&server_, &QLocalServer::newConnection, this, &GeckoIpcServer::onNewConnection);
}

GeckoIpcServer::~GeckoIpcServer() { shutdown(); }

bool GeckoIpcServer::listen(const QString &directory, QString *error) {
  const QByteArray dir = QFile::encodeName(directory);
  // mkdir fails for an existing path (or a symlink planted there).
  if (::mkdir(dir.constData(), 0700) != 0) {
    *error = QStringLiteral("cannot create the private IPC directory %1").arg(directory);
    return false;
  }
  directory_ = directory;
  struct stat st {};
  if (::lstat(dir.constData(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != ::getuid() ||
      (st.st_mode & 077) != 0) {
    *error = QStringLiteral("the IPC directory is not a private directory of this user");
    return false;
  }
  socketPath_ = directory + QStringLiteral("/host.sock");
  if (QFile::encodeName(socketPath_).size() > kMaxSocketPath) {
    *error = QStringLiteral("the IPC socket path is too long for Gecko: %1").arg(socketPath_);
    return false;
  }

  token_ = randomHex(32);
  windowTag_ = QString::fromLatin1(randomHex(16));
  tokenFile_ = directory + QStringLiteral("/token");
  const int fd = ::open(QFile::encodeName(tokenFile_).constData(),
                        O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) {
    *error = QStringLiteral("cannot create the IPC token file");
    return false;
  }
  const QByteArray line = token_ + '\n';
  const bool written = ::write(fd, line.constData(), line.size()) == line.size();
  ::close(fd);
  if (!written) {
    *error = QStringLiteral("cannot write the IPC token file");
    return false;
  }
  if (!server_.listen(socketPath_)) {
    *error = server_.errorString();
    return false;
  }
  qCDebug(lcGeckoIpc) << "listening on" << socketPath_;
  return true;
}

bool GeckoIpcServer::tokenFileConsumed() const { return !tokenFile_.isEmpty() && !QFile::exists(tokenFile_); }

void GeckoIpcServer::rejectConnection(QLocalSocket *socket, const QString &reason) {
  qCWarning(lcGeckoIpc) << "rejected connection:" << reason;
  socket->abort();
  socket->deleteLater();
  emit connectionRejected(reason);
}

void GeckoIpcServer::onNewConnection() {
  while (QLocalSocket *socket = server_.nextPendingConnection()) {
    struct ucred cred {};
    socklen_t len = sizeof(cred);
    if (::getsockopt(static_cast<int>(socket->socketDescriptor()), SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) {
      rejectConnection(socket, QStringLiteral("no peer credentials"));
      continue;
    }
    if (cred.uid != ::getuid()) {
      rejectConnection(socket, QStringLiteral("peer uid %1 is not this user").arg(cred.uid));
      continue;
    }
    if (expectedPid_ <= 0 || cred.pid != expectedPid_) {
      rejectConnection(socket, QStringLiteral("peer pid %1 is not the launched host %2").arg(cred.pid).arg(expectedPid_));
      continue;
    }
    if (session_) {
      rejectConnection(socket, QStringLiteral("a session already exists"));
      continue;
    }
    session_ = socket;
    peerPid_ = cred.pid;
    buffer_.clear();
    connect(socket, &QLocalSocket::readyRead, this, &GeckoIpcServer::onReadyRead);
    connect(socket, &QLocalSocket::disconnected, this, [this] { endSession(QStringLiteral("host disconnected")); });
    helloTimer_ = new QTimer(this);
    helloTimer_->setSingleShot(true);
    connect(helloTimer_, &QTimer::timeout, this, [this] {
      if (!authenticated_) endSession(QStringLiteral("no hello in time"));
    });
    helloTimer_->start(helloTimeoutMs_);
  }
}

void GeckoIpcServer::onReadyRead() {
  if (!session_) return;
  buffer_ += session_->readAll();
  while (buffer_.size() >= 4) {
    const quint32 length = qFromBigEndian<quint32>(buffer_.constData());
    if (length == 0 || length > static_cast<quint32>(kMaxFrame)) {
      endSession(QStringLiteral("invalid frame length %1").arg(length));
      return;
    }
    if (buffer_.size() < 4 + static_cast<qsizetype>(length)) return;
    const QByteArray payload = buffer_.mid(4, length);
    buffer_.remove(0, 4 + length);
    if (!handleFrame(payload)) return;
  }
}

bool GeckoIpcServer::handleFrame(const QByteArray &payload) {
  // The decoder runs only when its result becomes a QString.
  QStringDecoder utf8(QStringDecoder::Utf8);
  const QString decoded = utf8(payload);
  if (utf8.hasError()) {
    endSession(QStringLiteral("invalid UTF-8"));
    return false;
  }
  QJsonParseError parseError;
  const QJsonDocument doc = QJsonDocument::fromJson(payload, &parseError);
  if (parseError.error != QJsonParseError::NoError || !doc.isObject() ||
      !doc.object().value(QStringLiteral("type")).isString()) {
    endSession(QStringLiteral("malformed frame"));
    return false;
  }
  const QJsonObject msg = doc.object();
  const QString type = msg.value(QStringLiteral("type")).toString();

  if (!authenticated_) {
    const QByteArray token = msg.value(QStringLiteral("token")).toString().toLatin1();
    const bool ok = type == QLatin1String("hello") && msg.value(QStringLiteral("protocol")).isDouble() &&
                    msg.value(QStringLiteral("protocol")).toDouble() == 1 &&
                    msg.value(QStringLiteral("pid")).toInteger(-1) == peerPid_ && constantTimeEquals(token, token_);
    if (!ok) {
      endSession(QStringLiteral("authentication failed"));
      return false;
    }
    // DaliNira needs protocol 3's sessions, sequences and lifecycle guards.
    if (!msg.value(QStringLiteral("supportedProtocols")).toArray().contains(kProtocol)) {
      endSession(QStringLiteral("the host does not support protocol 3"));
      return false;
    }
    sessionId_ = msg.value(QStringLiteral("sessionId")).toString();
    static const QRegularExpression sessionPattern(QStringLiteral("^[0-9a-f]{32}$"));
    if (!sessionPattern.match(sessionId_).hasMatch()) {
      endSession(QStringLiteral("invalid session id"));
      return false;
    }
    authenticated_ = true;
    if (helloTimer_) helloTimer_->stop();
    // The token is single-use: a reconnect needs a new launch.
    token_ = randomHex(32);
    qCDebug(lcGeckoIpc) << "authenticated host" << peerPid_ << "token consumed:" << tokenFileConsumed();
    sendFrame({{"type", "welcome"}, {"protocol", kProtocol}, {"windowTag", windowTag_}, {"sessionId", sessionId_}});
    emit authenticated(peerPid_);
    return true;
  }

  const auto sequence = msg.value(QStringLiteral("sequence")).toInteger(-1);
  if (msg.value(QStringLiteral("sessionId")).toString() != sessionId_ || sequence != eventSequence_ + 1) {
    endSession(QStringLiteral("stale or reordered session event"));
    return false;
  }
  eventSequence_ = sequence;
  if (!hostMessageTypes().contains(type)) {
    qCDebug(lcGeckoIpc) << "ignored unknown host message" << type.left(64);
    return true;
  }

  static const QRegularExpression tabIdPattern(QStringLiteral("^tab-[0-9a-f]{32}$"));
  const auto validTabId = [](const QJsonValue &value) {
    return value.isString() && tabIdPattern.match(value.toString()).hasMatch();
  };
  static const QSet<QString> tabEvents = {
      QStringLiteral("state"), QStringLiteral("progress"), QStringLiteral("load-error"),
      QStringLiteral("content-crashed"), QStringLiteral("recovered"), QStringLiteral("tab-created"),
      QStringLiteral("tab-closed"), QStringLiteral("active-tab-changed"), QStringLiteral("view-close-requested")};
  bool valid = !tabEvents.contains(type) || validTabId(msg.value(QStringLiteral("tabId")));
  const auto stringOrNull = [&](const char *key) {
    const auto value = msg.value(QString::fromLatin1(key));
    return value.isString() || value.isNull() || value.isUndefined();
  };
  const auto boolean = [&](const char *key) { return msg.value(QString::fromLatin1(key)).isBool(); };
  if (type == QLatin1String("tabs-list")) {
    const auto tabs = msg.value(QStringLiteral("tabs")).toArray();
    valid = msg.value(QStringLiteral("tabs")).isArray() && validTabId(msg.value(QStringLiteral("activeTabId"))) &&
            !tabs.isEmpty() && tabs.size() <= 32;
    QSet<QString> ids;
    for (const auto &tab : tabs) {
      const auto id = tab.toObject().value(QStringLiteral("tabId"));
      valid = valid && tab.isObject() && validTabId(id) && !ids.contains(id.toString());
      ids.insert(id.toString());
    }
    valid = valid && ids.contains(msg.value(QStringLiteral("activeTabId")).toString());
  } else if (type == QLatin1String("state")) {
    valid = valid && msg.value(QStringLiteral("title")).isString() && stringOrNull("url") && stringOrNull("documentURI") &&
            stringOrNull("remoteType") && boolean("canGoBack") && boolean("canGoForward") &&
            (boolean("loading") || msg.value(QStringLiteral("loading")).isNull()) &&
            (msg.value(QStringLiteral("contentPid")).isNull() || msg.value(QStringLiteral("contentPid")).isDouble()) &&
            boolean("mediaPlaying") && boolean("mediaAudible") && msg.value(QStringLiteral("documentId")).isString() &&
            boolean("hasBeforeUnload") && boolean("crashed");
  } else if (type == QLatin1String("progress")) {
    const auto phase = msg.value(QStringLiteral("phase")).toString();
    const auto value = msg.value(QStringLiteral("value"));
    valid = valid &&
            (phase == QLatin1String("start") || phase == QLatin1String("change") || phase == QLatin1String("stop")) &&
            value.isDouble() && value.toDouble() >= 0 && value.toDouble() <= 1;
    if (phase == QLatin1String("stop")) valid = valid && boolean("ok") && msg.value(QStringLiteral("status")).isString();
  } else if (type == QLatin1String("load-error")) {
    valid = valid && msg.value(QStringLiteral("error")).isString() && stringOrNull("url");
  } else if (type == QLatin1String("content-crashed")) {
    valid = valid && boolean("isTopFrame");
  } else if (type == QLatin1String("recovered")) {
    valid = valid && msg.value(QStringLiteral("url")).isString() && msg.value(QStringLiteral("contentPid")).isDouble();
  } else if (type == QLatin1String("session-state-lost")) {
    valid = validTabId(msg.value(QStringLiteral("tabId"))) && boolean("stateLost") &&
            msg.value(QStringLiteral("scope")).toString() == QLatin1String("document") &&
            msg.value(QStringLiteral("reason")).isString();
  } else if (type == QLatin1String("tab-created")) {
    valid = valid && msg.value(QStringLiteral("title")).isString() && msg.value(QStringLiteral("url")).isString() &&
            boolean("loading") && boolean("active") && msg.value(QStringLiteral("source")).isString();
  } else if (type == QLatin1String("window-tagged")) {
    valid = msg.value(QStringLiteral("tag")).toString() == windowTag_;
  } else if (type == QLatin1String("focus")) {
    valid = boolean("active") && msg.value(QStringLiteral("reason")).isString();
  } else if (type == QLatin1String("geometry")) {
    for (const char *key : {"innerWidth", "innerHeight", "devicePixelRatio", "browserWidth", "browserHeight"})
      valid = valid && msg.value(QString::fromLatin1(key)).isDouble();
  } else if (type == QLatin1String("ack") || type == QLatin1String("error")) {
    valid = msg.value(QStringLiteral("command")).isString();
    if (msg.contains(QStringLiteral("tabId"))) valid = valid && validTabId(msg.value(QStringLiteral("tabId")));
    if (type == QLatin1String("error")) valid = valid && msg.value(QStringLiteral("code")).isString();
  }
  if (!valid) {
    endSession(QStringLiteral("invalid %1 event").arg(type));
    return false;
  }
  emit messageReceived(msg);
  return true;
}

void GeckoIpcServer::drain() {
  if (!session_) return;
  session_->waitForReadyRead(0);
  onReadyRead();
}

bool GeckoIpcServer::send(const QJsonObject &message) {
  if (!authenticated_) return false;
  QJsonObject envelope = message;
  envelope.insert(QStringLiteral("sessionId"), sessionId_);
  envelope.insert(QStringLiteral("commandSequence"), ++commandSequence_);
  return sendFrame(envelope);
}

bool GeckoIpcServer::sendFrame(const QJsonObject &message) {
  if (!session_ || session_->state() != QLocalSocket::ConnectedState) return false;
  const QByteArray payload = QJsonDocument(message).toJson(QJsonDocument::Compact);
  if (payload.isEmpty() || payload.size() > kMaxFrame) return false;
  QByteArray frame(4, Qt::Uninitialized);
  qToBigEndian<quint32>(static_cast<quint32>(payload.size()), frame.data());
  frame += payload;
  if (session_->write(frame) != frame.size()) return false;
  // Callers may block the event loop next (shutdown); push the bytes now.
  session_->flush();
  return true;
}

void GeckoIpcServer::endSession(const QString &reason) {
  if (!session_) return;
  QLocalSocket *socket = session_;
  session_ = nullptr;
  const bool wasAuthenticated = authenticated_;
  authenticated_ = false;
  socket->disconnect(this);
  socket->abort();
  socket->deleteLater();
  qCDebug(lcGeckoIpc) << "session closed:" << reason << "authenticated:" << wasAuthenticated;
  emit sessionClosed(reason);
}

void GeckoIpcServer::shutdown() {
  endSession(QStringLiteral("embedder shutdown"));
  if (server_.isListening()) server_.close();
  // Only what listen() created.
  if (!tokenFile_.isEmpty()) QFile::remove(tokenFile_);
  if (!socketPath_.isEmpty()) QFile::remove(socketPath_);
  if (!directory_.isEmpty()) QDir().rmdir(directory_);
}

}  // namespace dalinira::engine
