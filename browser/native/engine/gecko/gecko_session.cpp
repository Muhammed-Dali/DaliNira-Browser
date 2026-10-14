#include "engine/gecko/gecko_session.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QStandardPaths>
#include <QTimer>
#include <QUuid>
#include <QWidget>

#include <algorithm>
#include <sys/stat.h>
#include <unistd.h>

#include "engine/gecko/gecko_ipc_server.h"

Q_LOGGING_CATEGORY(lcGeckoSession, "dalinira.gecko.session")

namespace dalinira::engine {

namespace {

// An existing directory of this user, reached without symlinks, that nobody
// else can write.
bool privateRoot(const QString &path, QString *error) {
  const QString clean = QDir::cleanPath(path);
  struct stat st {};
  if (path.isEmpty() || QDir::isRelativePath(path) || QFileInfo(clean).canonicalFilePath() != clean ||
      ::lstat(QFile::encodeName(clean).constData(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != ::getuid() ||
      (st.st_mode & 0022) != 0) {
    *error = QStringLiteral("%1 is not an existing directory of this user that only it can write, without symlinks")
                 .arg(path);
    return false;
  }
  return true;
}

QString randomId(int length) { return QUuid::createUuid().toString(QUuid::Id128).left(length); }

}  // namespace

GeckoRequest::GeckoRequest(GeckoSession *owner) : QObject(owner), owner_(owner) {}

void GeckoRequest::cancel() {
  if (owner_) owner_->cancel(id_);
}

void GeckoRequest::complete(const QString &status, const QJsonObject &result) {
  if (finished_) return;
  finished_ = true;
  status_ = status;
  result_ = result;
  emit finished(status, result);
}

GeckoSessionView::GeckoSessionView(GeckoSession *session, const QString &id, quint64 generation)
    : QObject(session), session_(session), id_(id), generation_(generation) {}

qint64 GeckoSessionView::contentProcessId() const {
  return lifecycle_ == State::Crashed || lifecycle_ == State::Lost || lifecycle_ == State::Closed
             ? 0
             : state_.value(QStringLiteral("contentPid")).toInteger();
}

void GeckoSessionView::transition(State state) {
  if (lifecycle_ == state) return;
  lifecycle_ = state;
  emit lifecycleStateChanged(state);
}

void GeckoSessionView::update(const QJsonObject &state) {
  const QJsonObject old = state_;
  state_ = state;
  if (old.value(QStringLiteral("url")) != state.value(QStringLiteral("url"))) emit urlChanged(url());
  if (old.value(QStringLiteral("title")) != state.value(QStringLiteral("title"))) emit titleChanged(title());
  if (old.value(QStringLiteral("mediaAudible")) != state.value(QStringLiteral("mediaAudible"))) emit audibleChanged(isAudible());
  if (!old.value(QStringLiteral("loading")).toBool() && isLoading()) emit loadStarted();
}

GeckoRequest *GeckoSessionView::command(const QString &type, QJsonObject fields) {
  if (!session_) return nullptr;
  fields.insert(QStringLiteral("tabId"), id_);
  if (generation_ != session_->generation() || lifecycle_ == State::Lost || lifecycle_ == State::Closed)
    return session_->submit(QStringLiteral("invalid_view"), fields);
  return session_->submit(type, fields);
}

GeckoRequest *GeckoSessionView::load(const QUrl &url) {
  return command(QStringLiteral("load_url"), {{QStringLiteral("url"), url.toString()}});
}
GeckoRequest *GeckoSessionView::reload() { return command(QStringLiteral("reload")); }
GeckoRequest *GeckoSessionView::stop() { return command(QStringLiteral("stop")); }
GeckoRequest *GeckoSessionView::back() { return command(QStringLiteral("back")); }
GeckoRequest *GeckoSessionView::forward() { return command(QStringLiteral("forward")); }

GeckoRequest *GeckoSessionView::setEngineActive(bool active) {
  if (!session_) return nullptr;
  return active ? command(QStringLiteral("activate_tab"))
                : session_->submit(QStringLiteral("unsupported_deactivate"), {{QStringLiteral("tabId"), id_}});
}

GeckoRequest *GeckoSessionView::close(bool discard, const QString &confirmedDocumentId) {
  QJsonObject fields{{QStringLiteral("documentId"), discard ? confirmedDocumentId : (confirmedDocumentId.isEmpty() ? documentId() : confirmedDocumentId)}};
  if (discard) {
    fields.insert(QStringLiteral("discard"), true);
  }
  return command(QStringLiteral("close_tab"), fields);
}

GeckoRequest *GeckoSessionView::recoverDocument() { return command(QStringLiteral("recover_tab")); }

GeckoSession::GeckoSession(QObject *parent) : QObject(parent) {}

GeckoSession::~GeckoSession() {
  if (state_ == State::Ready && !stopping_) {
    qCWarning(lcGeckoSession) << "destroyed with a live session; its documents are lost";
    loseSession(generation_, QStringLiteral("adapter-destroyed"));
  }
  stopHost(0);
}

bool GeckoSession::supportsCapability(EngineCapability capability) const {
  switch (capability) {
    case EngineCapability::Navigation:
    case EngineCapability::History:
    case EngineCapability::Popups:
      return true;
    default:
      return false;
  }
}

bool GeckoSession::claimPresentation(QWidget *owner) {
  if (!owner || (presentationOwner_ && presentationOwner_ != owner)) return false;
  presentationOwner_ = owner;
  return true;
}

void GeckoSession::releasePresentation(QWidget *owner) {
  if (presentationOwner_ == owner) presentationOwner_ = nullptr;
}

qint64 GeckoSession::hostPid() const { return host_ ? host_->pid() : 0; }
bool GeckoSession::hostIsRunning() const { return host_ && host_->isRunning(); }
QString GeckoSession::windowTag() const { return ipc_ ? ipc_->windowTag() : QString(); }
int GeckoSession::protocol() const { return ipc_ ? ipc_->protocol() : 0; }

QList<GeckoHostProcess::ProcessId> GeckoSession::hostProcesses() const {
  return host_ ? host_->descendants() : QList<GeckoHostProcess::ProcessId>{};
}

QList<GeckoSessionView *> GeckoSession::views() const {
  QList<GeckoSessionView *> result;
  for (const auto &id : order_) {
    if (views_.value(id)) result.append(views_.value(id));
  }
  return result;
}

GeckoSessionView *GeckoSession::view(const QString &id) const { return views_.value(id); }

GeckoSessionView *GeckoSession::adopt(const QString &id) {
  if (views_.value(id)) return views_.value(id);
  auto *created = new GeckoSessionView(this, id, generation_);
  views_.insert(id, created);
  order_.append(id);
  return created;
}

bool GeckoSession::start(const Config &config, QString *error) {
  if (host_ && host_->isRunning()) {
    *error = QStringLiteral("host-already-running");
    return false;
  }
  QStringList problems;
  const GeckoValidatedRuntime runtime = GeckoRuntime::validate(config.runtime, &problems);
  if (!runtime.isValid()) {
    *error = problems.join(QStringLiteral("; "));
    return false;
  }
  const QString ipcRoot = config.ipcRoot.isEmpty()
                              ? QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation)
                              : config.ipcRoot;
  if (!privateRoot(config.runtimeRoot, error) || !privateRoot(ipcRoot, error)) return false;

  if (host_ || ipc_) stopHost(0);
  runtimeRoot_ = QDir::cleanPath(config.runtimeRoot);
  stopping_ = false;
  ++generation_;
  requestNumber_ = 0;
  requestPrefix_ = QStringLiteral("g%1_%2_").arg(generation_).arg(randomId(12));
  state_ = State::Starting;
  views_.clear();
  order_.clear();
  activeId_.clear();
  // Unique across launches and sessions, even when a pid is reused.
  runDir_ = QStringLiteral("%1/s%2-%3-%4")
                .arg(QDir::cleanPath(config.runtimeRoot))
                .arg(generation_)
                .arg(::getpid(), 0, 16)
                .arg(randomId(8));
  if (::mkdir(QFile::encodeName(runDir_).constData(), 0700) != 0) {
    *error = QStringLiteral("cannot create the run directory %1").arg(runDir_);
    runDir_.clear();
    state_ = State::Stopped;
    return false;
  }

  ipc_ = new GeckoIpcServer(this);
  ipc_->setHelloTimeout(config.helloTimeoutMs);
  const quint64 generation = generation_;
  connect(ipc_, &GeckoIpcServer::messageReceived, this,
          [this, generation](const QJsonObject &message) { receive(generation, message); });
  connect(ipc_, &GeckoIpcServer::sessionClosed, this,
          [this, generation](const QString &why) { loseSession(generation, why); });
  if (!ipc_->listen(QDir::cleanPath(ipcRoot) + QStringLiteral("/dalinira-gecko-") + randomId(16), error)) {
    ipc_->shutdown();
    removeRunDirectory();
    state_ = State::Stopped;
    return false;
  }
  host_ = new GeckoHostProcess(this);
  connect(host_, &GeckoHostProcess::exited, this, [this, generation](int code, QProcess::ExitStatus status) {
    if (generation != generation_) return;
    // Its last replies and events may still be unread: a shutdown ack, a
    // crash report.
    if (ipc_) ipc_->drain();
    loseSession(generation, QStringLiteral("host-exited"));
    if (ipc_) ipc_->shutdown();
    emit hostExited(code, status);
  });
  GeckoHostProcess::Config launch;
  launch.runtime = runtime;
  launch.runDirectory = runDir_;
  launch.socketPath = ipc_->socketPath();
  launch.tokenFile = ipc_->tokenFilePath();
  launch.display = config.display;
  launch.profileDirectory = config.profileDirectory;
  launch.logFile = config.logFile;
  launch.testInput = config.testInput;
  if (!host_->start(launch, error)) {
    ipc_->shutdown();
    removeRunDirectory();
    state_ = State::Stopped;
    return false;
  }
  // Connections are handled from the event loop, so none is accepted before
  // the expected pid is known.
  ipc_->setExpectedPeerPid(host_->pid());
  qCInfo(lcGeckoSession) << "session" << generation_ << "started host" << hostPid() << "in" << runDir_;
  return true;
}

GeckoRequest *GeckoSession::submit(const QString &type, QJsonObject fields, int timeoutMs) {
  auto *request = new GeckoRequest(this);
  request->deadline_ = QDeadlineTimer(qMax(0, timeoutMs));
  request->generation_ = generation_;
  request->id_ = requestPrefix_ + QString::number(++requestNumber_);
  const QString id = request->id_;
  fields.insert(QStringLiteral("type"), type);
  fields.insert(QStringLiteral("protocol"), GeckoIpcServer::kProtocol);
  fields.insert(QStringLiteral("requestId"), id);
  pending_.insert(id, {request, type, fields.value(QStringLiteral("tabId")).toString(), fields});
  connect(request, &GeckoRequest::finished, this, [this, id](const QString &status, const QJsonObject &result) {
    qCDebug(lcGeckoSession) << "request" << id << status << result;
    emit requestFinished(id, status, result);
  });
  QTimer::singleShot(0, this, [this, id] {
    if (!pending_.contains(id)) return;  // cancelled while queued: never sent
    const Pending pending = pending_.value(id);
    if (!pending.request) {
      pending_.remove(id);
      return;
    }
    if (pending.request->deadline_.hasExpired()) {
      finish(id, QStringLiteral("timed-out"), {{"code", "deadline-exceeded"}, {"mayHaveApplied", false}});
      return;
    }
    if (pending.request->generation() != generation_ || state_ != State::Ready || !ipc_ || !ipc_->isAuthenticated()) {
      finish(id, QStringLiteral("rejected"), {{"code", "session-not-ready"}});
      return;
    }
    if (pending_.size() > 256 || pending.type == QLatin1String("invalid_view") ||
        pending.type == QLatin1String("unsupported_deactivate") || pending.type == QLatin1String("invalid_shortcuts")) {
      finish(id, QStringLiteral("rejected"),
             {{"code", pending.type == QLatin1String("unsupported_deactivate") ? "unsupported-independent-surface"
                       : pending.type == QLatin1String("invalid_shortcuts")   ? "invalid-shortcuts"
                                                                              : "invalid-view-or-request-limit"}});
      return;
    }
    if (!ipc_->send(pending.message)) {
      finish(id, QStringLiteral("disconnected"), {{"code", "send-failed"}});
      return;
    }
    pending.request->status_ = QStringLiteral("sent");
    emit requestSent(id);
  });
  QTimer::singleShot(qMax(1, timeoutMs), this, [this, id] {
    if (!pending_.contains(id)) return;
    const auto request = pending_.value(id).request;
    finish(id, QStringLiteral("timed-out"),
           {{"code", "deadline-exceeded"}, {"mayHaveApplied", request && request->status() == QLatin1String("sent")}});
  });
  return request;
}

GeckoRequest *GeckoSession::createView(const QUrl &url, bool activate) {
  QJsonObject fields{{QStringLiteral("activate"), activate}};
  if (!url.isEmpty()) fields.insert(QStringLiteral("url"), url.toString());
  return submit(QStringLiteral("create_tab"), fields);
}

GeckoRequest *GeckoSession::listViews() { return submit(QStringLiteral("list_tabs")); }

GeckoRequest *GeckoSession::setShortcuts(const QStringList &shortcuts) {
  const bool valid = shortcuts.size() <= 256 && std::all_of(shortcuts.cbegin(), shortcuts.cend(), [](const QString &shortcut) {
    return GeckoIpcServer::isValidShortcut(shortcut);
  });
  if (!valid) return submit(QStringLiteral("invalid_shortcuts"));
  return submit(QStringLiteral("set_shortcuts"), {{QStringLiteral("shortcuts"), QJsonArray::fromStringList(shortcuts)}});
}

GeckoRequest *GeckoSession::shutdown(bool discard, const QJsonObject &confirmedDocuments) {
  QJsonObject fields{{QStringLiteral("documents"), discard ? confirmedDocuments : documentSnapshot()}};
  if (discard) fields.insert(QStringLiteral("discard"), true);
  return submit(QStringLiteral("shutdown"), fields);
}

QJsonObject GeckoSession::documentSnapshot() const {
  QJsonObject result;
  for (auto *item : views()) result.insert(item->tabId(), item->documentId());
  return result;
}

void GeckoSession::finish(const QString &id, const QString &status, const QJsonObject &result) {
  const Pending pending = pending_.take(id);
  if (pending.request) pending.request->complete(status, result);
}

bool GeckoSession::cancel(const QString &id) {
  if (!pending_.contains(id) || !pending_.value(id).request) return false;
  const bool sent = pending_.value(id).request->status() == QLatin1String("sent");
  finish(id, QStringLiteral("cancelled"), {{"code", "cancelled"}, {"mayHaveApplied", sent}});
  // A command still queued at the host is dropped there; one already
  // applied is not rolled back.
  if (sent && ipc_ && ipc_->isAuthenticated()) {
    ipc_->send({{"type", "cancel_request"},
                {"protocol", GeckoIpcServer::kProtocol},
                {"requestId", requestPrefix_ + QString::number(++requestNumber_)},
                {"targetRequestId", id}});
  }
  return true;
}

void GeckoSession::receive(quint64 generation, const QJsonObject &message) {
  if (generation != generation_ || state_ == State::Disconnected || state_ == State::Stopped) {
    qCDebug(lcGeckoSession) << "ignored an event of generation" << generation << message.value(QStringLiteral("type"));
    return;
  }
  const QString type = message.value(QStringLiteral("type")).toString();
  const QString id = message.value(QStringLiteral("tabId")).toString();
  if (type == QLatin1String("tab-created")) {
    auto *item = adopt(id);
    item->update(message);
    emit viewCreated(item, message.value(QStringLiteral("source")).toString());
  } else if (type == QLatin1String("active-tab-changed")) {
    activeId_ = id;
    for (auto *item : views()) {
      if (item->lifecycleState() != GeckoSessionView::State::Crashed)
        item->transition(item->tabId() == id ? GeckoSessionView::State::Active : GeckoSessionView::State::Inactive);
    }
    emit activeViewChanged(view(id));
  } else if (type == QLatin1String("tab-closed")) {
    auto *closed = view(id);
    views_.remove(id);
    order_.removeAll(id);
    if (closed) {
      closed->transition(GeckoSessionView::State::Closed);
      emit viewClosed(closed);
    }
    for (const auto &requestId : pending_.keys()) {
      if (pending_.value(requestId).tabId == id && pending_.value(requestId).type != QLatin1String("close_tab"))
        finish(requestId, QStringLiteral("rejected"), {{"code", "view-closed"}});
    }
  } else if (type == QLatin1String("state")) {
    if (auto *item = view(id)) item->update(message);
  } else if (type == QLatin1String("progress")) {
    if (auto *item = view(id)) {
      emit item->loadProgress(qRound(message.value(QStringLiteral("value")).toDouble() * 100));
      if (message.value(QStringLiteral("phase")).toString() == QLatin1String("stop"))
        emit item->loadFinished(message.value(QStringLiteral("ok")).toBool());
    }
  } else if (type == QLatin1String("content-crashed")) {
    if (auto *item = view(id)) {
      item->transition(GeckoSessionView::State::Crashed);
      emit item->webProcessTerminated(1);
    }
  } else if (type == QLatin1String("session-state-lost")) {
    if (auto *item = view(id)) emit item->stateLost(message.value(QStringLiteral("reason")).toString());
  } else if (type == QLatin1String("recovered")) {
    if (auto *item = view(id)) {
      QJsonObject recovered = item->snapshot();
      recovered.insert(QStringLiteral("contentPid"), message.value(QStringLiteral("contentPid")));
      recovered.insert(QStringLiteral("url"), message.value(QStringLiteral("url")));
      recovered.insert(QStringLiteral("crashed"), false);
      recovered.insert(QStringLiteral("loading"), false);
      item->update(recovered);
      item->transition(activeId_ == id ? GeckoSessionView::State::Active : GeckoSessionView::State::Inactive);
    }
  } else if (type == QLatin1String("view-close-requested")) {
    if (auto *item = view(id)) emit item->windowCloseRequested();
  } else if (type == QLatin1String("shortcut")) {
    if (view(id))
      emit shortcutPressed(id, message.value(QStringLiteral("shortcut")).toString(), message.value(QStringLiteral("repeat")).toBool());
  } else if (type == QLatin1String("window-tagged")) {
    state_ = State::Ready;
    emit ready();
  }

  if (type == QLatin1String("ack") || type == QLatin1String("error") ||
      (type == QLatin1String("tabs-list") && message.contains(QStringLiteral("requestId")))) {
    const QString requestId = message.value(QStringLiteral("requestId")).toString();
    if (!pending_.contains(requestId)) {
      // Cancelled, timed out or of an earlier session: dropped before any UI.
      qCDebug(lcGeckoSession) << "dropped a stale reply" << requestId << type;
      return;
    }
    const Pending expected = pending_.value(requestId);
    const QString command = message.value(QStringLiteral("command")).toString();
    if ((type == QLatin1String("tabs-list") && expected.type != QLatin1String("list_tabs")) ||
        (type != QLatin1String("tabs-list") && command != expected.type) ||
        (!expected.tabId.isEmpty() && message.value(QStringLiteral("tabId")).toString() != expected.tabId) ||
        (expected.type == QLatin1String("close_tab") &&
         (type == QLatin1String("ack") || message.value(QStringLiteral("code")) == QLatin1String("beforeunload-confirmation-required")) &&
         (expected.message.value(QStringLiteral("documentId")).toString().isEmpty() ||
          message.value(QStringLiteral("documentId")) != expected.message.value(QStringLiteral("documentId")))) ||
        (expected.type == QLatin1String("shutdown") &&
         (type == QLatin1String("ack") || message.value(QStringLiteral("code")) == QLatin1String("beforeunload-confirmation-required")) &&
         message.value(QStringLiteral("documents")) != expected.message.value(QStringLiteral("documents")))) {
      finish(requestId, QStringLiteral("rejected"), {{"code", "invalid-reply"}});
      return;
    }
    if (expected.request && expected.request->deadline_.hasExpired()) {
      finish(requestId, QStringLiteral("timed-out"), {{"code", "deadline-exceeded"}, {"mayHaveApplied", true}});
      return;
    }
    if (type == QLatin1String("ack") && expected.type == QLatin1String("shutdown")) {
      stopping_ = true;
      state_ = State::Stopping;
    }
    // The observed state first: a callback may open a modal loop, and a newer
    // operation inside it must not be overwritten by this older reply.
    emit eventReceived(message);
    finish(requestId, type == QLatin1String("error") ? QStringLiteral("rejected") : QStringLiteral("completed"), message);
    return;
  }
  emit eventReceived(message);
}

void GeckoSession::loseSession(quint64 generation, const QString &reason) {
  if (generation != generation_ || state_ == State::Disconnected || state_ == State::Stopped) return;
  state_ = State::Disconnected;
  lostViews_ = {};
  for (auto *item : views()) {
    QJsonObject snapshot = item->snapshot();
    snapshot.insert(QStringLiteral("tabId"), item->tabId());
    lostViews_.append(snapshot);
    item->transition(GeckoSessionView::State::Lost);
    if (!stopping_) emit item->stateLost(reason);
  }
  for (const auto &id : pending_.keys()) {
    const auto request = pending_.value(id).request;
    finish(id, QStringLiteral("disconnected"),
           {{"code", "session-lost"},
            {"stateLost", !stopping_},
            {"mayHaveApplied", request && request->status() == QLatin1String("sent")}});
  }
  if (!stopping_) {
    qCWarning(lcGeckoSession) << "session" << generation_ << "lost:" << reason;
    emit sessionStateLost(lostViews_, reason);
    emit disconnected(reason);
  }
}

QJsonObject GeckoSession::stopHost(int graceMs) {
  if (state_ == State::Ready && !stopping_) return {{"how", "refused-live-session"}, {"requiresShutdown", true}};
  stopping_ = true;
  QJsonObject result;
  if (host_) result = host_->stop(graceMs);
  if (ipc_) ipc_->shutdown();
  loseSession(generation_, QStringLiteral("stopped"));
  state_ = State::Stopped;
  if (ipc_) {
    ipc_->deleteLater();
    ipc_ = nullptr;
  }
  if (host_) {
    host_->deleteLater();
    host_ = nullptr;
  }
  removeRunDirectory();
  return result;
}

void GeckoSession::removeRunDirectory() {
  if (runDir_.isEmpty()) return;
  const QString directory = runDir_;
  runDir_.clear();
  // Only the directory start() made: directly in the runtime root, a real
  // directory of this user (QDir::removeRecursively does not follow links).
  struct stat st {};
  if (QFileInfo(directory).path() != runtimeRoot_ ||
      ::lstat(QFile::encodeName(directory).constData(), &st) != 0 || !S_ISDIR(st.st_mode) ||
      st.st_uid != ::getuid()) {
    qCWarning(lcGeckoSession) << "left the run directory alone:" << directory;
    return;
  }
  if (!QDir(directory).removeRecursively()) qCWarning(lcGeckoSession) << "could not remove" << directory;
}

}  // namespace dalinira::engine
