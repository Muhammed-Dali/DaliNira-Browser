#include "engine/gecko/gecko_engine_view.h"

#include <QLoggingCategory>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QTimer>

#include "engine/gecko/gecko_backend.h"
#include "engine/gecko/gecko_presentation_surface.h"
#include "engine/gecko/gecko_session.h"

Q_LOGGING_CATEGORY(lcGeckoView, "dalinira.gecko.view")

namespace dalinira::engine {

namespace {

bool isWebUrl(const QUrl &url) {
  const QString scheme = url.scheme().toLower();
  return url.isValid() && (scheme == QLatin1String("http") || scheme == QLatin1String("https"));
}

}  // namespace

GeckoEngineView::GeckoEngineView(GeckoBackend *backend, QWidget *parent) : EngineView(parent), backend_(backend) {
  setFocusPolicy(Qt::StrongFocus);
  setAttribute(Qt::WA_OpaquePaintEvent);
  setStatus(QStringLiteral("Starting Gecko…"));
  if (backend_) {
    backend_->registerView(this);
    connect(backend_->surface(), &GeckoPresentationSurface::statusChanged, this, [this] { update(); });
  }
  // After the browser's first load(), so its URL goes with the creation.
  QTimer::singleShot(0, this, &GeckoEngineView::createHostTab);
}

GeckoEngineView::~GeckoEngineView() {
  if (!backend_) return;
  backend_->surface()->release(this);
  backend_->unregisterView(this);
  // The tab is closing (or its window): the host closes the page without
  // asking again, as Chromium and WebKit do for a closing window.
  if (view_ && !closing_ && view_->lifecycleState() != GeckoSessionView::State::Closed &&
      view_->lifecycleState() != GeckoSessionView::State::Lost && backend_->session()->state() == GeckoSession::State::Ready) {
    view_->close(true, view_->documentId());
  }
}

QString GeckoEngineView::hostTabId() const { return view_ ? view_->tabId() : QString(); }

bool GeckoEngineView::isCrashed() const {
  return view_ && view_->lifecycleState() == GeckoSessionView::State::Crashed;
}

void GeckoEngineView::setStatus(const QString &status) {
  if (status_ == status) return;
  status_ = status;
  update();
}

void GeckoEngineView::createHostTab() {
  if (view_ || creating_ || !backend_) return;
  GeckoSession *session = backend_->session();
  if (session->state() != GeckoSession::State::Ready) {
    if (session->state() == GeckoSession::State::Starting) {
      connect(session, &GeckoSession::ready, this, &GeckoEngineView::createHostTab, Qt::SingleShotConnection);
    } else {
      hostTabFailed(backend_->lastError().isEmpty() ? QStringLiteral("Gecko is not running") : backend_->lastError());
    }
    return;
  }
  // A host tab nobody shows yet (the host's first tab, a page-opened tab).
  if (GeckoSessionView *spare = backend_->takeHostTab(this)) {
    const QUrl initial = initialUrl_;
    initialUrl_.clear();
    bind(spare);
    if (isWebUrl(initial)) load(initial);
    return;
  }
  creating_ = true;
  GeckoRequest *request = session->createView(initialUrl_, engineActive_);
  initialUrl_.clear();
  backend_->expectCreatedTab(request->id(), this);
  connect(request, &GeckoRequest::finished, this, [this](const QString &status, const QJsonObject &result) {
    creating_ = false;
    if (view_) return;  // bound already, from the creation event
    if (status == QLatin1String("completed") && backend_) {
      if (GeckoSessionView *created = backend_->session()->view(result.value(QStringLiteral("tabId")).toString())) {
        bind(created);
        return;
      }
    }
    hostTabFailed(result.value(QStringLiteral("code")).toString(status));
  });
}

void GeckoEngineView::hostTabFailed(const QString &reason) {
  qCWarning(lcGeckoView) << "no Gecko tab:" << reason;
  queue_.clear();
  setStatus(QStringLiteral("The Gecko tab could not be opened (%1).").arg(reason));
  emit loadFinished(false);
}

void GeckoEngineView::bind(GeckoSessionView *view) {
  view_ = view;
  setStatus({});
  connect(view, &GeckoSessionView::urlChanged, this, [this](const QUrl &url) {
    if (isWebUrl(url)) emit urlChanged(url);
  });
  connect(view, &GeckoSessionView::titleChanged, this, &EngineView::titleChanged);
  connect(view, &GeckoSessionView::loadStarted, this, &EngineView::loadStarted);
  connect(view, &GeckoSessionView::loadFinished, this, &EngineView::loadFinished);
  connect(view, &GeckoSessionView::loadProgress, this, &EngineView::loadProgress);
  connect(view, &GeckoSessionView::audibleChanged, this, &EngineView::audibleChanged);
  connect(view, &GeckoSessionView::webProcessTerminated, this, &EngineView::webProcessTerminated);
  connect(view, &GeckoSessionView::windowCloseRequested, this, &EngineView::windowCloseRequested);
  connect(view, &GeckoSessionView::lifecycleStateChanged, this, [this](GeckoSessionView::State state) {
    if (state == GeckoSessionView::State::Lost) setStatus(QStringLiteral("Gecko stopped. Restart DaliNira to use Gecko again."));
    else if (state == GeckoSessionView::State::Crashed) setStatus(QStringLiteral("This page crashed. Reload to recover it."));
    else if (state != GeckoSessionView::State::Closed) setStatus({});
  });
  if (isWebUrl(view->url())) emit urlChanged(view->url());
  if (!view->title().isEmpty()) emit titleChanged(view->title());
  if (engineActive_) present();
  flush();
}

void GeckoEngineView::run(Command command) {
  queue_.append(std::move(command));
  if (view_) flush();
}

void GeckoEngineView::flush() {
  while (view_ && !queue_.isEmpty()) queue_.takeFirst()(view_);
}

QUrl GeckoEngineView::url() const {
  if (view_ && isWebUrl(view_->url())) return view_->url();
  return requestedUrl_;
}

QString GeckoEngineView::title() const { return view_ ? view_->title() : QString(); }
bool GeckoEngineView::isLoading() const { return view_ && view_->isLoading(); }
bool GeckoEngineView::canGoBack() const { return view_ && view_->canGoBack(); }
bool GeckoEngineView::canGoForward() const { return view_ && view_->canGoForward(); }
bool GeckoEngineView::isAudible() const { return view_ && view_->isAudible(); }
qint64 GeckoEngineView::contentProcessId() const { return view_ ? view_->contentProcessId() : 0; }

void GeckoEngineView::load(const QUrl &url) {
  requestedUrl_ = url;
  if (!isWebUrl(url)) {
    // DaliNira's own pages (New Tab) are an empty Gecko tab for now; other
    // schemes are not opened by the Gecko host at all.
    if (url.scheme() != QLatin1String("dalinira")) {
      qCWarning(lcGeckoView) << "Gecko opens http and https pages only:" << url.scheme();
      emit loadFinished(false);
    }
    return;
  }
  if (!view_ && !creating_ && queue_.isEmpty() && initialUrl_.isEmpty()) {
    initialUrl_ = url;  // goes with the host tab's creation; later ones wait
    return;
  }
  run([this, url](GeckoSessionView *view) {
    GeckoRequest *request = view->load(url);
    connect(request, &GeckoRequest::finished, this, [this](const QString &status, const QJsonObject &result) {
      if (status != QLatin1String("completed")) {
        // e.g. "beforeunload-navigation-blocked": the page keeps its document.
        qCWarning(lcGeckoView) << "navigation refused:" << result.value(QStringLiteral("code")).toString(status);
        emit loadFinished(false);
      }
    });
    return request;
  });
}

void GeckoEngineView::reload() {
  run([](GeckoSessionView *view) {
    // Reload after a content process crash is the explicit recovery.
    return view->lifecycleState() == GeckoSessionView::State::Crashed ? view->recoverDocument() : view->reload();
  });
}

void GeckoEngineView::stop() {
  if (view_) view_->stop();
}

void GeckoEngineView::back() {
  run([](GeckoSessionView *view) { return view->back(); });
}

void GeckoEngineView::forward() {
  run([](GeckoSessionView *view) { return view->forward(); });
}

void GeckoEngineView::setEngineActive(bool active) {
  engineActive_ = active;
  if (active) present();
  update();
}

void GeckoEngineView::present() {
  if (!backend_) return;
  backend_->surface()->present(this);
  // Qt's focus may have reached this view before it showed the page (a tab
  // switch hides the old view first).
  if (hasFocus()) backend_->surface()->focusPage(this);
  if (view_ && view_->lifecycleState() != GeckoSessionView::State::Active) view_->setEngineActive(true);
}

void GeckoEngineView::requestClose(std::function<void(bool allowed)> done) {
  if (!view_ || closing_ || view_->lifecycleState() == GeckoSessionView::State::Closed ||
      view_->lifecycleState() == GeckoSessionView::State::Lost) {
    done(true);
    return;
  }
  closing_ = true;
  QPointer<GeckoEngineView> self(this);
  GeckoRequest *request = view_->close();
  connect(request, &GeckoRequest::finished, this, [self, done](const QString &status, const QJsonObject &result) {
    if (!self) return;
    if (result.value(QStringLiteral("code")).toString() != QLatin1String("beforeunload-confirmation-required")) {
      // Closed, or nothing left to close (session gone, tab already closed).
      self->closing_ = status == QLatin1String("completed");
      done(true);
      return;
    }
    // The page has unsaved changes: the user decides, bound to this document.
    const QString document = self->view_ ? self->view_->documentId() : QString();
    if (!self->backend_ || !self->view_ || !self->backend_->confirmDiscard(self, self->title())) {
      self->closing_ = false;
      done(false);
      return;
    }
    GeckoRequest *discard = self->view_->close(true, document);
    connect(discard, &GeckoRequest::finished, self.data(), [self, done](const QString &status, const QJsonObject &) {
      if (!self) return;
      self->closing_ = status == QLatin1String("completed");
      // A different document by now ("stale-document"): keep the tab.
      done(status == QLatin1String("completed"));
    });
  });
}

void GeckoEngineView::paintEvent(QPaintEvent *event) {
  QPainter painter(this);
  painter.fillRect(event->rect(), QColor(18, 24, 32));
  if (backend_ && backend_->surface()->isPresenting(this)) return;
  QString text = status_;
  if (text.isEmpty() && backend_) {
    QWidget *owner = backend_->surface()->ownerWindow();
    text = owner && owner != window() ? QStringLiteral("Gecko (experimental) shows pages in one window only.")
                                      : backend_->surface()->status();
  }
  if (text.isEmpty()) return;
  painter.setPen(QColor(200, 210, 222));
  painter.drawText(rect().adjusted(24, 24, -24, -24), Qt::AlignCenter | Qt::TextWordWrap, text);
}

void GeckoEngineView::resizeEvent(QResizeEvent *event) {
  EngineView::resizeEvent(event);
  if (backend_) backend_->surface()->viewResized(this);
}

void GeckoEngineView::focusInEvent(QFocusEvent *event) {
  EngineView::focusInEvent(event);
  if (backend_) backend_->surface()->focusPage(this);
}

void GeckoEngineView::hideEvent(QHideEvent *event) {
  // The page's window is unmapped with this view; while the page still holds
  // the keyboard, the window's top level takes it (otherwise X11 already gave
  // it to the nearest viewable window of DaliNira's).
  if (backend_) backend_->surface()->viewHidden(this);
  EngineView::hideEvent(event);
}

}  // namespace dalinira::engine
