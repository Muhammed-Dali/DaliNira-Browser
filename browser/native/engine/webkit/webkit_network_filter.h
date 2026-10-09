#pragma once

#include <QByteArray>
#include <QHash>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QUrl>
#include <atomic>
#include <functional>
#include <memory>

class QThread;
class QTemporaryDir;

namespace dalinira::engine {

// Browser-process side of DaliNira's WebKit web process extension
// (extension/dalinira_web_extension.cpp). Each web process connects to a
// private UNIX socket and asks, synchronously, what to do with every http(s)
// request its pages send. Requests are answered on a dedicated thread, so page
// loads never wait for the GUI event loop.
//
// The decision itself is injected per page (WebKitEngineView registers its
// page id with the evaluator of its window's blocker), which keeps this
// adapter free of blocker policy: both engines share DaliNiraBlockerService.
class WebKitNetworkFilter final : public QObject {
  Q_OBJECT
 public:
  struct Request {
    quint64 pageId = 0;
    QString method;
    QString destination;  // Fetch Metadata destination (Sec-Fetch-Dest).
    bool redirected = false;
    QUrl url;
    QUrl firstPartyUrl;
  };
  enum class Action { Allow, Block, Redirect };
  struct Verdict {
    Action action = Action::Allow;
    QUrl redirectUrl;
  };
  // Called on the filter thread; must be thread-safe.
  using Evaluator = std::function<Verdict(const Request &)>;

  explicit WebKitNetworkFilter(QObject *parent = nullptr);
  ~WebKitNetworkFilter() override;

  bool start();
  bool isRunning() const { return running_; }
  QString socketPath() const { return socketPath_; }
  QByteArray token() const { return token_; }
  // Where the extension module is: next to the executable, as installed.
  static QString extensionDirectory();

  void setRoute(quint64 pageId, Evaluator evaluator);
  void removeRoute(quint64 pageId);

  // Evaluates one request as the socket server does, including the repeated
  // request suppression below. Exposed for tests.
  Verdict evaluate(const Request &request);
  quint64 answeredRequests() const { return answered_.load(); }
  int connectedProcesses() const { return connections_.load(); }

  // The web process currently hosting a page (0 when unknown), as registered
  // by the extension; the pid comes from the socket, not from the message.
  qint64 processIdForPage(quint64 pageId) const;

 private:
  class Server;
  friend class Server;

  bool running_ = false;
  QString socketPath_;
  QByteArray token_;
  std::unique_ptr<QTemporaryDir> socketDir_;
  QThread *thread_ = nullptr;
  Server *server_ = nullptr;

  QMutex routesMutex_;
  QHash<quint64, std::shared_ptr<const Evaluator>> routes_;
  // WebKit's preload scanner and its parser each request a script the
  // scanner saw; when the first is blocked, the second is the same resource
  // again. Answer it without re-evaluating so it is blocked but not counted
  // twice (Chromium reuses the failed preload and sees one request). Scoped
  // to one document: a new top-level load of the page clears it. The method
  // is part of the key: a blocked POST says nothing about a GET ($method).
  struct RecentBlock {
    quint64 pageId = 0;
    QString document;
    QString url;
    QString method;
    qint64 atMs = 0;
  };
  QMutex recentMutex_;
  QList<RecentBlock> recentBlocks_;

  void notePageProcess(quint64 pageId, qint64 pid);
  void forgetPageProcess(quint64 pageId, qint64 pid);
  void forgetProcess(qint64 pid);
  mutable QMutex processesMutex_;
  QHash<quint64, qint64> pageProcesses_;

  std::atomic<quint64> answered_{0};
  std::atomic<int> connections_{0};
};

} // namespace dalinira::engine
