#include "engine/gecko/gecko_backend.h"

#include <QApplication>
#include <QEvent>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QLoggingCategory>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>

#include <sys/stat.h>
#include <unistd.h>

#include "i18n/i18n.h"
#include "engine/engine_storage_manager.h"
#include "engine/gecko/gecko_engine_view.h"
#include "engine/gecko/gecko_presentation_surface.h"
#include "engine/gecko/gecko_session.h"
#include "engine/gecko/gecko_shortcut_router.h"

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
  if (qApp) qApp->installEventFilter(this);
  surface_ = new GeckoPresentationSurface(session_, this);
  shortcutRouter_ = new GeckoShortcutRouter(this);
  connect(session_, &GeckoSession::viewCreated, this, &GeckoBackend::onViewCreated);
  connect(session_, &GeckoSession::viewClosed, this, [this](GeckoSessionView *view) {
    spareTabs_.removeAll(view->tabId());
    contentTabs_.removeAll(view->tabId());
  });
}

GeckoBackend::~GeckoBackend() {
  if (qApp) qApp->removeEventFilter(this);
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
  session.testInput = config_.testInput;
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

void GeckoBackend::expectCreatedTab(GeckoRequest *request, GeckoEngineView *view) {
  const QString requestId = request->id();
  expectedCreations_.insert(requestId, view);
  // Forget it once the request is over (the creation event came, or never will).
  connect(request, &GeckoRequest::finished, this, [this, requestId] {
    expectedCreations_.remove(requestId);
  }, Qt::SingleShotConnection);
}

void GeckoBackend::onViewCreated(GeckoSessionView *view, const QString &source) {
  const QJsonObject created = view->snapshot();
  const QString requestId = created.value(QStringLiteral("requestId")).toString();
  if (!requestId.isEmpty() && expectedCreations_.contains(requestId)) {
    const QPointer<GeckoEngineView> owner = expectedCreations_.take(requestId);
    if (owner && !owner->view_) {
      owner->bind(view);
    } else if (!owner) {
      qCWarning(lcGeckoBackend) << "retaining host tab whose widget disappeared" << view->tabId();
    }
    return;
  }
  if (!requestId.isEmpty()) {
    // A late creation after timeout/cancellation is not a reusable empty tab.
    qCWarning(lcGeckoBackend) << "retaining late host creation" << view->tabId();
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
  QMessageBox box(QMessageBox::Warning, I18n::text(QStringLiteral("navigation.leave_page_title")),
                  I18n::text(QStringLiteral("navigation.leave_page_message"))
                      .arg(title.isEmpty() ? I18n::text(QStringLiteral("navigation.this_page")) : title.left(120)),
                  QMessageBox::Discard | QMessageBox::Cancel, parent);
  box.button(QMessageBox::Discard)->setText(I18n::text(QStringLiteral("navigation.discard_changes")));
  box.button(QMessageBox::Cancel)->setText(I18n::text(QStringLiteral("common.cancel")));
  box.setDefaultButton(QMessageBox::Cancel);
  return box.exec() == QMessageBox::Discard;
}

void GeckoBackend::requestShutdown(QWidget *parent, std::function<void(bool)> done) {
  if (session_->state() == GeckoSession::State::Stopping || !session_->hostIsRunning()) { done(true); return; }
  if (shutdownPending_ || session_->state() != GeckoSession::State::Ready) { done(false); return; }
  shutdownPending_ = true;
  const quint64 generation = session_->generation();
  const QJsonObject documents = session_->documentSnapshot();
  QPointer<GeckoBackend> self(this);
  QPointer<QWidget> owner(parent);
  auto finish = [self, done](bool allowed) {
    if (!self) return;
    self->shutdownPending_ = false;
    done(allowed);
  };
  GeckoRequest *request = session_->shutdown();
  connect(request, &GeckoRequest::finished, this,
          [self, owner, generation, documents, finish](const QString &status, const QJsonObject &result) {
    if (!self) return;
    if (self->session_->generation() != generation) { finish(false); return; }
    if (status == QLatin1String("completed") || !self->session_->hostIsRunning()) { finish(true); return; }
    if (status != QLatin1String("rejected") ||
        result.value(QStringLiteral("code")).toString() != QLatin1String("beforeunload-confirmation-required") ||
        result.value(QStringLiteral("documents")).toObject() != documents ||
        self->session_->documentSnapshot() != documents) { finish(false); return; }
    const bool confirmed = self->confirmDiscard(owner, QStringLiteral("Open Gecko pages"));
    if (!self) return;
    if (!confirmed || self->session_->generation() != generation ||
        self->session_->state() != GeckoSession::State::Ready || self->session_->documentSnapshot() != documents) {
      finish(false); return;
    }
    GeckoRequest *discard = self->session_->shutdown(true, documents);
    connect(discard, &GeckoRequest::finished, self.data(),
            [finish](const QString &status, const QJsonObject &) { finish(status == QLatin1String("completed")); });
  });
}

bool GeckoBackend::eventFilter(QObject *watched, QEvent *event) {
  if (watched == qApp && event->type() == QEvent::Quit && session_->hostIsRunning() &&
      session_->state() != GeckoSession::State::Stopping) {
    // Covers menu/shortcut quit as well as window-manager close. Refusal keeps
    // the event loop and all windows alive; no destructor is used as consent.
    requestShutdown(QApplication::activeWindow(), [](bool allowed) {
      if (allowed && qApp) QTimer::singleShot(0, qApp, &QCoreApplication::quit);
    });
    return true;
  }
  return EngineBackend::eventFilter(watched, event);
}

bool GeckoBackend::shutdownHost(int timeoutMs) {
  if (!session_) return true;
  if (session_->hostIsRunning() && session_->state() != GeckoSession::State::Stopping) {
    struct Decision { bool accepted = false; bool finished = false; };
    auto decision = std::make_shared<Decision>();
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QPointer<QEventLoop> waiting(&loop);
    requestShutdown(QApplication::activeWindow(), [decision, waiting](bool allowed) {
      decision->accepted = allowed;
      decision->finished = true;
      if (waiting) waiting->quit();
    });
    timeout.start(timeoutMs);
    if (!decision->finished) loop.exec(QEventLoop::ExcludeUserInputEvents);
    if (!decision->finished) { qCWarning(lcGeckoBackend) << "shutdown still pending"; return false; }
    if (!decision->accepted) return false;
  }
  const QJsonObject cleanup = session_->stopHost(timeoutMs);
  qCInfo(lcGeckoBackend) << "Gecko host stopped" << cleanup;
  return cleanup.value(QStringLiteral("leftoversKilled")).toArray().isEmpty() &&
         cleanup.value(QStringLiteral("how")).toString() != QLatin1String("refused-live-session");
}

}  // namespace dalinira::engine
