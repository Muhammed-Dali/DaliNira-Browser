#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QLocalServer>
#include <QObject>
#include <QPointer>
#include <QString>

class QLocalSocket;
class QTimer;

namespace dalinira::engine {

// DaliNira's side of the Gecko host IPC (protocol 3; the host side is
// browser/resources/gecko-host/chrome/content/ipc.js and tabs.js).
//
// Transport: a private directory (0700, created here, owned by this user)
// holding the Unix-domain socket and a one-shot token file (0600) that the
// host reads and deletes. Exactly one session: the peer must be the launched
// host process (SO_PEERCRED pid and uid), its first frame a "hello" carrying
// the token (compared in constant time) within the hello timeout.
// Frames: big-endian uint32 length (1..64 KiB) + one UTF-8 JSON object with a
// string "type". Every host frame after the welcome carries the session id
// and the next sequence number; every event type and field is validated.
// Anything malformed, oversized, out of order or from another session ends
// the session. Nothing received is ever evaluated as code, and no code is
// sent: commands are a fixed set of typed JSON messages.
class GeckoIpcServer : public QObject {
  Q_OBJECT

 public:
  static constexpr int kProtocol = 3;
  static constexpr int kMaxFrame = 64 * 1024;
  // sockaddr_un limit Gecko's portable socket code accepts.
  static constexpr int kMaxSocketPath = 103;

  explicit GeckoIpcServer(QObject *parent = nullptr);
  ~GeckoIpcServer() override;

  // Creates `directory` (which must not exist yet) and listens there.
  bool listen(const QString &directory, QString *error);
  QString directory() const { return directory_; }
  QString socketPath() const { return socketPath_; }
  QString tokenFilePath() const { return tokenFile_; }
  QString windowTag() const { return windowTag_; }
  bool tokenFileConsumed() const;

  // Only a connection from this process is accepted.
  void setExpectedPeerPid(qint64 pid) { expectedPid_ = pid; }
  void setHelloTimeout(int milliseconds) { helloTimeoutMs_ = milliseconds; }
  QString sessionId() const { return sessionId_; }
  bool supportsInternalDocuments() const { return internalDocuments_; }
  int protocol() const { return authenticated_ ? kProtocol : 0; }
  bool isAuthenticated() const { return authenticated_; }

  // A browser shortcut as DaliNira and the host name it: "Ctrl+T",
  // "Ctrl+Shift+Tab", "Alt+ArrowLeft", "F5", "Ctrl++". Modifiers in that
  // order, Ctrl or Alt (not both: AltGr) unless the key is F1-F24, one named
  // key or one printable ASCII character (upper case); no Shift with a symbol
  // (the symbol is the key). Text editing and window-manager keys are never
  // shortcuts. browser/resources/gecko-host/chrome/content/keys.js agrees.
  static bool isValidShortcut(const QString &shortcut);

  // Sends one command to the authenticated host; false if that is not possible.
  bool send(const QJsonObject &message);
  // Handles every frame the host already wrote (e.g. before it exited).
  void drain();
  void shutdown();

 signals:
  void authenticated(qint64 peerPid);
  void messageReceived(const QJsonObject &message);
  void sessionClosed(const QString &reason);
  void connectionRejected(const QString &reason);

 private:
  void onNewConnection();
  void onReadyRead();
  void rejectConnection(QLocalSocket *socket, const QString &reason);
  void endSession(const QString &reason);
  bool handleFrame(const QByteArray &payload);
  bool sendFrame(const QJsonObject &message);

  QLocalServer server_;
  QPointer<QLocalSocket> session_;
  QTimer *helloTimer_ = nullptr;
  QByteArray buffer_;
  QByteArray token_;
  QString windowTag_;
  QString directory_;
  QString socketPath_;
  QString tokenFile_;
  QString sessionId_;
  qint64 expectedPid_ = -1;
  qint64 peerPid_ = -1;
  qint64 eventSequence_ = 0;
  qint64 commandSequence_ = 0;
  int helloTimeoutMs_ = 10000;
  bool internalDocuments_ = false;
  bool authenticated_ = false;
};

}  // namespace dalinira::engine
