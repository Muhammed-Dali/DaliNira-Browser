#pragma once

#include <QDeadlineTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QUrl>

#include "engine/engine_capabilities.h"
#include "engine/gecko/gecko_host_process.h"
#include "engine/gecko/gecko_runtime.h"

class QWidget;

namespace dalinira::engine {

class GeckoIpcServer;
class GeckoSession;

// One correlated, asynchronous command. It finishes exactly once:
// "completed" (the host accepted it; for a navigation that is not the end of
// the load), "rejected", "cancelled", "timed-out" or "disconnected".
// "mayHaveApplied" in the result of the last three says whether the host may
// already have acted on it: nothing is ever retried or rolled back.
class GeckoRequest : public QObject {
  Q_OBJECT
 public:
  QString id() const { return id_; }
  quint64 generation() const { return generation_; }
  QString status() const { return status_; }
  bool isFinished() const { return finished_; }
  QJsonObject result() const { return result_; }
  void cancel();

 signals:
  void finished(const QString &status, const QJsonObject &result);

 private:
  friend class GeckoSession;
  explicit GeckoRequest(GeckoSession *owner);
  void complete(const QString &status, const QJsonObject &result = {});

  QPointer<GeckoSession> owner_;
  QString id_;
  QString status_ = QStringLiteral("queued");
  quint64 generation_ = 0;
  bool finished_ = false;
  QJsonObject result_;
  QDeadlineTimer deadline_;
};

// One tab of the host, by its stable host tab id. A logical view, not a
// widget: every view of a session shares the session's single native
// presentation surface (GeckoX11Embedder). The session owns it; once Closed
// or Lost it refuses every operation and may be retired with deleteLater().
class GeckoSessionView : public QObject {
  Q_OBJECT
 public:
  enum class State { Inactive, Active, Crashed, Closed, Lost };
  Q_ENUM(State)

  QString tabId() const { return id_; }
  quint64 generation() const { return generation_; }
  State lifecycleState() const { return lifecycle_; }
  QUrl url() const { return QUrl(state_.value(QStringLiteral("url")).toString()); }
  QString title() const { return state_.value(QStringLiteral("title")).toString(); }
  bool isLoading() const { return state_.value(QStringLiteral("loading")).toBool(); }
  bool canGoBack() const { return state_.value(QStringLiteral("canGoBack")).toBool(); }
  bool canGoForward() const { return state_.value(QStringLiteral("canGoForward")).toBool(); }
  bool isAudible() const { return state_.value(QStringLiteral("mediaAudible")).toBool(); }
  bool hasBeforeUnload() const { return state_.value(QStringLiteral("hasBeforeUnload")).toBool(); }
  QString documentId() const { return state_.value(QStringLiteral("documentId")).toString(); }
  qint64 contentProcessId() const;
  bool isEngineActive() const { return lifecycle_ == State::Active; }
  QJsonObject snapshot() const { return state_; }

  GeckoRequest *load(const QUrl &url);
  GeckoRequest *reload();
  GeckoRequest *stop();
  GeckoRequest *back();
  GeckoRequest *forward();
  // Selects this view. Deactivating a view on its own is not supported (one
  // surface): select another view instead.
  GeckoRequest *setEngineActive(bool active);
  // Without `discard`, a document with a beforeunload handler is not closed
  // ("beforeunload-confirmation-required"). Discard is bound to the document
  // the user confirmed for: a different document is not closed.
  GeckoRequest *close(bool discard = false, const QString &confirmedDocumentId = {});
  // After a content process crash: a fresh document at the same URL under the
  // same tab id; input, history and opener are lost.
  GeckoRequest *recoverDocument();

 signals:
  void urlChanged(const QUrl &url);
  void titleChanged(const QString &title);
  void loadStarted();
  void loadFinished(bool success);
  void loadProgress(int progress);
  void audibleChanged(bool audible);
  void webProcessTerminated(int reason);
  void lifecycleStateChanged(dalinira::engine::GeckoSessionView::State state);
  void stateLost(const QString &reason);
  void windowCloseRequested();

 private:
  friend class GeckoSession;
  GeckoSessionView(GeckoSession *session, const QString &id, quint64 generation);
  void update(const QJsonObject &state);
  void transition(State state);
  GeckoRequest *command(const QString &type, QJsonObject fields = {});

  QPointer<GeckoSession> session_;
  QString id_;
  quint64 generation_ = 0;
  State lifecycle_ = State::Inactive;
  QJsonObject state_;
};

// One Gecko host process and its authenticated IPC session (protocol 3), on
// the GUI thread. start() reports whether the launch was accepted; ready()
// follows once the host authenticated and tagged its window.
//
// Generations: every start() is a new generation with a new session id and
// request id prefix; replies, events and views of an earlier generation are
// ignored. Losing the host or the connection invalidates every view and
// pending request (their metadata stays in lostSessionSnapshot(), nothing is
// restored); a new start() begins an empty session with new tab ids.
class GeckoSession : public QObject {
  Q_OBJECT
 public:
  enum class State { Stopped, Starting, Ready, Stopping, Disconnected };
  Q_ENUM(State)

  struct Config {
    GeckoRuntimePaths runtime;
    // An existing directory of this user that nobody else can write; each
    // launch makes a private run directory (profile, HOME, logs) in it.
    QString runtimeRoot;
    // Parent of the private IPC directory; empty: the user's runtime
    // directory (XDG_RUNTIME_DIR). Socket paths must stay short.
    QString ipcRoot;
    GeckoHostProcess::Display display = GeckoHostProcess::Display::X11;
    int helloTimeoutMs = 10000;
    // See GeckoHostProcess::Config: a profile kept across launches (empty: a
    // fresh one per launch) and the host's log (empty: in the run directory).
    QString profileDirectory;
    QString logFile;
    bool testInput = false;  // GeckoHostProcess::Config::testInput (tests only)
  };

  explicit GeckoSession(QObject *parent = nullptr);
  ~GeckoSession() override;

  bool supportsCapability(EngineCapability capability) const;
  bool start(const Config &config, QString *error);
  State state() const { return state_; }
  quint64 generation() const { return generation_; }
  QString runDirectory() const { return runDir_; }
  qint64 hostPid() const;
  bool hostIsRunning() const;
  QString windowTag() const;
  int protocol() const;
  QList<GeckoHostProcess::ProcessId> hostProcesses() const;

  // At most one widget presents the session's surface at a time.
  bool claimPresentation(QWidget *owner);
  void releasePresentation(QWidget *owner);

  QString activeTabId() const { return activeId_; }
  QList<GeckoSessionView *> views() const;
  GeckoSessionView *view(const QString &id) const;
  GeckoRequest *createView(const QUrl &url = {}, bool activate = true);
  GeckoRequest *listViews();
  // The browser shortcuts the host keeps from its pages and reports as
  // shortcutPressed() instead (GeckoIpcServer::isValidShortcut; at most 256).
  // Every call replaces the previous set; an invalid set is rejected here.
  GeckoRequest *setShortcuts(const QStringList &shortcuts);
  GeckoRequest *submit(const QString &type, QJsonObject fields = {}, int timeoutMs = 5000);
  // Closes the whole host; with beforeunload documents open it needs
  // `discard` and documentSnapshot() of exactly the documents confirmed.
  GeckoRequest *shutdown(bool discard = false, const QJsonObject &confirmedDocuments = {});
  // Final cleanup after shutdown() was accepted (refused for a live session):
  // the host's process tree, its IPC directory and this session's own run
  // directory (never a persistent profile).
  QJsonObject stopHost(int graceMs = 8000);
  bool cancel(const QString &requestId);
  QJsonArray lostSessionSnapshot() const { return lostViews_; }
  QJsonObject documentSnapshot() const;

 signals:
  void ready();
  void eventReceived(const QJsonObject &event);
  void requestSent(const QString &id);
  void requestFinished(const QString &id, const QString &status, const QJsonObject &result);
  void viewClosed(dalinira::engine::GeckoSessionView *view);
  void viewCreated(dalinira::engine::GeckoSessionView *view, const QString &source);
  void activeViewChanged(dalinira::engine::GeckoSessionView *view);
  // A key of the set given to setShortcuts() was pressed in the tab's page;
  // the page did not receive it.
  void shortcutPressed(const QString &tabId, const QString &shortcut, bool repeat);
  void disconnected(const QString &reason);
  void sessionStateLost(const QJsonArray &views, const QString &reason);
  void hostExited(int exitCode, QProcess::ExitStatus status);

 private:
  friend class GeckoSessionView;
  friend class GeckoSessionTestAccess;
  struct Pending {
    QPointer<GeckoRequest> request;
    QString type;
    QString tabId;
    QJsonObject message;
  };
  void receive(quint64 generation, const QJsonObject &message);
  void loseSession(quint64 generation, const QString &reason);
  GeckoSessionView *adopt(const QString &id);
  void finish(const QString &id, const QString &status, const QJsonObject &result = {});
  void removeRunDirectory();

  QPointer<QWidget> presentationOwner_;
  QPointer<GeckoIpcServer> ipc_;
  QPointer<GeckoHostProcess> host_;
  QHash<QString, QPointer<GeckoSessionView>> views_;
  QList<QString> order_;
  QHash<QString, Pending> pending_;
  QJsonArray lostViews_;
  QString runDir_;
  QString runtimeRoot_;
  QString activeId_;
  QString requestPrefix_;
  quint64 generation_ = 0;
  quint64 requestNumber_ = 0;
  State state_ = State::Stopped;
  bool stopping_ = false;
};

}  // namespace dalinira::engine
