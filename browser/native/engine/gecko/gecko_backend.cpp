#include "engine/gecko/gecko_backend.h"

#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QLoggingCategory>
#include <QMessageBox>
#include <QTimer>

#include <sys/stat.h>
#include <unistd.h>

#include "engine/engine_storage_manager.h"
#include "engine/gecko/gecko_engine_view.h"
#include "engine/gecko/gecko_presentation_surface.h"
#include "engine/gecko/gecko_session.h"

Q_LOGGING_CATEGORY(lcGeckoBackend, "dalinira.gecko.backend")

namespace dalinira::engine {

namespace {

// Creates `path` (0700), or accepts an existing directory of this user that
// nobody else can write (`privateOnly`: nobody else can enter). Never
// changes an existing directory.
bool ownDirectory(const QString &path, bool privateOnly, QString *error) {
  struct stat st {};
  if (::lstat(QFile::encodeName(path).constData(), &st) != 0) {
    if (::mkdir(QFile::encodeName(path).constData(), 0700) == 0) return true;
    *error = QStringLiteral("cannot create %1").arg(path);
    return false;
  }
  if (!S_ISDIR(st.st_mode) || st.st_uid != ::getuid() || (st.st_mode & (privateOnly ? 0077 : 0022)) != 0) {
    *error = QStringLiteral("%1 is not a %2directory of this user").arg(path, privateOnly ? QStringLiteral("private ") : QString());
    return false;
  }
  return true;
}

}  // namespace

GeckoBackend::GeckoBackend(Config config, QObject *parent)
    : EngineBackend(parent), config_(std::move(config)), session_(new GeckoSession(this)) {
  surface_ = new GeckoPresentationSurface(session_, this);
  connect(session_, &GeckoSession::viewCreated, this, &GeckoBackend::onViewCreated);
  connect(session_, &GeckoSession::viewClosed, this, [this](GeckoSessionView *view) {
    spareTabs_.removeAll(view->tabId());
    contentTabs_.removeAll(view->tabId());
  });
}

GeckoBackend::~GeckoBackend() {
  // Views are gone by now (main.cpp deletes the windows first).
  delete surface_;
  surface_ = nullptr;
  delete session_;  // stops a host still running
  session_ = nullptr;
}

QString GeckoBackend::versionString() const {
  return milestone_.isEmpty() ? QString() : QStringLiteral("Gecko %1").arg(milestone_);
}

bool GeckoBackend::supportsCapability(EngineCapability cap) const {
  switch (cap) {
    case EngineCapability::Navigation:
    case EngineCapability::History:
    case EngineCapability::Popups:
      return true;
    default:
      return false;
  }
}

bool GeckoBackend::initialize(const EngineStorageManager &storage) {
  dataDirectory_ = config_.dataDirectory.isEmpty() ? storage.engineDir(EngineType::Gecko) : config_.dataDirectory;
  const QString runtimeRoot = dataDirectory_ + QStringLiteral("/runtime");
  const QString profileDirectory = dataDirectory_ + QStringLiteral("/profile");
  profile_ = std::make_unique<EngineProfile>(EngineType::Gecko, profileDirectory, this);
  QString error;
  // <AppData>/gecko also holds the Gecko UI's New Tab background (gecko/ui,
  // BrowserProfileService); the host's own directories are private.
  if (!ownDirectory(dataDirectory_, false, &error) || !ownDirectory(runtimeRoot, true, &error)) {
    lastError_ = error;
    qCWarning(lcGeckoBackend) << "Gecko storage:" << error;
    return false;
  }
  QStringList problems;
  milestone_ = GeckoRuntime::validate(config_.runtime, &problems).milestone;

  GeckoSession::Config session;
  session.runtime = config_.runtime;
  session.runtimeRoot = runtimeRoot;
  session.ipcRoot = config_.ipcRoot;
  session.display = config_.display;
  session.profileDirectory = profileDirectory;
  session.logFile = dataDirectory_ + QStringLiteral("/host-output.log");
  if (!session_->start(session, &error)) {
    lastError_ = error;
    qCWarning(lcGeckoBackend) << "the Gecko host did not start:" << error;
    return false;
  }
  return true;
}

EngineView *GeckoBackend::createView(QWidget *parent) {
  auto *view = new GeckoEngineView(this, parent);
  if (!adoptNext_.isEmpty()) {
    adopted_.insert(view, adoptNext_);
    adoptNext_.clear();
  }
  return view;
}

EngineView *GeckoBackend::createEphemeralView(QWidget *parent) {
  Q_UNUSED(parent);
  // A private page would write to the persistent Gecko profile.
  return nullptr;
}

void GeckoBackend::registerView(GeckoEngineView *view) { views_.append(view); }

void GeckoBackend::unregisterView(GeckoEngineView *view) {
  views_.removeAll(view);
  adopted_.remove(view);
}

GeckoEngineView *GeckoBackend::viewShowing(const QString &tabId) const {
  for (const auto &view : views_) {
    if (view && view->hostTabId() == tabId) return view;
  }
  return nullptr;
}

GeckoSessionView *GeckoBackend::takeHostTab(GeckoEngineView *view) {
  QString id = adopted_.take(view);
  if (!id.isEmpty()) {
    contentTabs_.removeAll(id);
  } else if (!spareTabs_.isEmpty()) {
    id = spareTabs_.takeFirst();
  }
  GeckoSessionView *hostTab = id.isEmpty() ? nullptr : session_->view(id);
  return hostTab && !viewShowing(id) ? hostTab : nullptr;
}

void GeckoBackend::expectCreatedTab(const QString &requestId, GeckoEngineView *view) {
  expectedCreations_.insert(requestId, view);
  // Forget it once the request is over (the creation event came, or never will).
  connect(session_, &GeckoSession::requestFinished, view, [this, requestId](const QString &id) {
    if (id == requestId) expectedCreations_.remove(requestId);
  });
}

void GeckoBackend::onViewCreated(GeckoSessionView *view, const QString &source) {
  const QJsonObject created = view->snapshot();
  const QString requestId = created.value(QStringLiteral("requestId")).toString();
  if (!requestId.isEmpty() && expectedCreations_.contains(requestId)) {
    const QPointer<GeckoEngineView> owner = expectedCreations_.take(requestId);
    if (owner && !owner->view_) {
      owner->bind(view);
    } else if (!owner) {
      view->close(true, view->documentId());  // its tab closed before it existed
    }
    return;
  }
  if (source == QLatin1String("content")) {
    // A page opened it: its tab's window shows it next to the opener.
    contentTabs_.append(view->tabId());
    if (GeckoEngineView *opener = viewShowing(created.value(QStringLiteral("openerTabId")).toString())) {
      emit opener->contentTabOpened(view->tabId());
    }
    return;
  }
  spareTabs_.append(view->tabId());
}

bool GeckoBackend::confirmDiscard(QWidget *parent, const QString &title) const {
  if (confirmation_) return confirmation_(parent, title);
  QMessageBox box(QMessageBox::Warning, QStringLiteral("Leave page?"),
                  QStringLiteral("\"%1\" has changes that may not be saved.")
                      .arg(title.isEmpty() ? QStringLiteral("This page") : title.left(120)),
                  QMessageBox::Discard | QMessageBox::Cancel, parent);
  box.setDefaultButton(QMessageBox::Cancel);
  return box.exec() == QMessageBox::Discard;
}

bool GeckoBackend::shutdownHost(int timeoutMs) {
  if (!session_) return true;
  if (session_->state() == GeckoSession::State::Ready) {
    GeckoRequest *request = session_->shutdown(true, session_->documentSnapshot());
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    connect(request, &GeckoRequest::finished, &loop, &QEventLoop::quit);
    connect(session_, &GeckoSession::hostExited, &loop, &QEventLoop::quit);
    timeout.start(timeoutMs);
    if (!request->isFinished()) loop.exec(QEventLoop::ExcludeUserInputEvents);
    if (request->status() != QLatin1String("completed"))
      qCWarning(lcGeckoBackend) << "the Gecko host did not accept its shutdown:" << request->status() << request->result();
  }
  // Accepted: waits for the host, then ends what is left of its tree.
  // Refused or no answer: the session is ended when the backend is destroyed.
  const QJsonObject cleanup = session_->stopHost(timeoutMs);
  qCInfo(lcGeckoBackend) << "Gecko host stopped" << cleanup;
  return cleanup.value(QStringLiteral("leftoversKilled")).toArray().isEmpty() &&
         cleanup.value(QStringLiteral("how")).toString() != QLatin1String("refused-live-session");
}

}  // namespace dalinira::engine
