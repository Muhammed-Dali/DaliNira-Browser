#include "engine/gecko/gecko_engine_view.h"

#include <QLoggingCategory>
#include <QPainter>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QTimer>
#include <memory>

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
  if (view_ && view_->snapshot().value("internalPage") == QLatin1String("newtab"))
    view_->retireNewTab(view_->snapshot().value("internalInstance").toString());
  backend_->surface()->release(this);
  backend_->unregisterView(this);
  // Widget disposal is not permission to destroy a live host document.
  // Detached tabs stay owned by the session until an explicit close/shutdown.
  if (view_ && view_->lifecycleState() != GeckoSessionView::State::Closed &&
      view_->lifecycleState() != GeckoSessionView::State::Lost)
    qCWarning(lcGeckoView) << "view disposed; retaining host document" << view_->tabId();

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
  backend_->expectCreatedTab(request, this);
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
  connect(backend_->session(), &GeckoSession::eventReceived, this, [this](const QJsonObject &event) {
    if(!view_ || event.value("tabId").toString()!=view_->tabId())return;
    if(event.value("type")==QLatin1String("state")){
      publishNewTab();
      if(view_->snapshot().value("internalPage")==QLatin1String("newtab") && newTabState_.value("revision").toInteger()>view_->snapshot().value("internalRevision").toInteger())
        view_->updateNewTabState(view_->snapshot().value("internalInstance").toString(),newTabState_);
    }
    if(event.value("type")==QLatin1String("internal-action") &&
       event.value("documentId").toString()==view_->documentId() &&
       event.value("instance")==view_->snapshot().value("internalInstance") && engineActive_ && activationConfirmed_)
      emit internalAction(event);
  });
  setStatus({});
  connect(view, &GeckoSessionView::urlChanged, this, [this](const QUrl &url) {
    if (isWebUrl(url) || (url == QUrl(QStringLiteral("dalinira://newtab/")) && view_->snapshot().value("internalPage") == QLatin1String("newtab"))) emit urlChanged(url);
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
  publishNewTab();
}

void GeckoEngineView::run(Command command) {
  queue_.append(std::move(command));
  if (view_) flush();
}

void GeckoEngineView::flush() {
  while (view_ && !queue_.isEmpty()) queue_.takeFirst()(view_);
}

QUrl GeckoEngineView::url() const {
  if (view_ && (isWebUrl(view_->url()) || view_->snapshot().value("internalPage") == QLatin1String("newtab"))) return view_->url();
  // A pending named load is not evidence of an internal document.
  return requestedUrl_.scheme() == QLatin1String("dalinira") ? QUrl{} : requestedUrl_;
}

QString GeckoEngineView::title() const { return view_ ? view_->title() : QString(); }
bool GeckoEngineView::isLoading() const { return view_ && view_->isLoading(); }
bool GeckoEngineView::canGoBack() const { return view_ && view_->canGoBack(); }
bool GeckoEngineView::canGoForward() const { return view_ && view_->canGoForward(); }
bool GeckoEngineView::isAudible() const { return view_ && view_->isAudible(); }
qint64 GeckoEngineView::contentProcessId() const { return view_ ? view_->contentProcessId() : 0; }

void GeckoEngineView::load(const QUrl &url) {
  ++presentationEpoch_;  // Pending grants cannot outlive a navigation intent.
  requestedUrl_ = url;
  if (url.scheme() == QLatin1String("dalinira") && url.host() == QLatin1String("newtab") && (url.path().isEmpty() || url.path() == QLatin1String("/")) && url.query().isEmpty() && url.fragment().isEmpty() && url.userName().isEmpty() && url.password().isEmpty() && url.port(-1)==-1) { emit newTabRequested(); return; }
  pendingNewTab_.clear();
  if (!isWebUrl(url)) {
    // Only the exact New Tab route above has an internal-page implementation.
    // Other schemes are never forwarded to the Gecko host.
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

void GeckoEngineView::prepareNewTab(const QByteArray &markup,const QJsonObject &state){pendingNewTab_=markup;newTabState_=state;publishNewTab();}
void GeckoEngineView::publishNewTab(){
  if(pendingNewTab_.isEmpty()||newTabLoading_||!view_||view_->documentId().isEmpty()||!backend_)return;
  if(!backend_->session()->supportsInternalDocuments()){setStatus(QStringLiteral("This host does not support DaliNira New Tab."));return;}
  QString error;const QString bundle=backend_->session()->publishNewTab(pendingNewTab_,&error);
  if(bundle.isEmpty()){setStatus(error);pendingNewTab_.clear();emit loadFinished(false);return;}
  pendingNewTab_.clear();newTabLoading_=true;
  auto *request=view_->loadNewTab(bundle,newTabState_);
  connect(request,&GeckoRequest::finished,this,[this](const QString &status,const QJsonObject &result){newTabLoading_=false;if(status!=QLatin1String("completed")){setStatus(QStringLiteral("New Tab failed: ")+result.value("code").toString());emit loadFinished(false);}publishNewTab();});
}
void GeckoEngineView::updateNewTabState(const QJsonObject &state){
 newTabState_=state;
 if(view_&&view_->snapshot().value("internalPage")==QLatin1String("newtab"))view_->updateNewTabState(view_->snapshot().value("internalInstance").toString(),state);
}
void GeckoEngineView::commitNewTabAction(const QJsonObject &event,const QJsonObject &effect,std::function<void()> committed){
 if(!view_||!backend_||!engineActive_||!activationConfirmed_||!isVisible()||event.value("documentId").toString()!=view_->documentId()||event.value("instance")!=view_->snapshot().value("internalInstance"))return;
 const quint64 generation=backend_->session()->generation(), presentation=presentationEpoch_;
 auto *request=view_->commitNewTabAction(event,effect);
 connect(request,&GeckoRequest::finished,this,[this,generation,presentation,event,effect,committed](const QString &status,const QJsonObject &result){
  if(status!=QLatin1String("completed")||!backend_||backend_->session()->generation()!=generation||presentationEpoch_!=presentation||!view_||result.value("documentId")!=event.value("documentId")||result.value("instance")!=event.value("instance")||result.value("actionId")!=event.value("actionId")||result.value("revision")!=event.value("revision"))return;
  if(effect.value("kind")==QLatin1String("preference")&&engineActive_&&isVisible()&&view_->documentId()==event.value("documentId").toString()&&view_->snapshot().value("internalInstance")==event.value("instance")&&activationConfirmed_&&committed)committed();
 });
}
void GeckoEngineView::reload() {
  ++presentationEpoch_;  // Pending grants cannot outlive a navigation intent.
  run([](GeckoSessionView *view) {
    // Reload after a content process crash is the explicit recovery.
    return view->lifecycleState() == GeckoSessionView::State::Crashed ? view->recoverDocument() : view->reload();
  });
}

void GeckoEngineView::stop() {
  ++presentationEpoch_;  // Pending grants cannot outlive a navigation intent.
  if (view_) view_->stop();
}

void GeckoEngineView::back() {
  ++presentationEpoch_;  // Pending grants cannot outlive a navigation intent.
  run([](GeckoSessionView *view) { return view->back(); });
}

void GeckoEngineView::forward() {
  ++presentationEpoch_;  // Pending grants cannot outlive a navigation intent.
  run([](GeckoSessionView *view) { return view->forward(); });
}

void GeckoEngineView::setEngineActive(bool active) {
  if (engineActive_ != active) { ++presentationEpoch_; activationConfirmed_ = false; }
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
  if (view_ && (!activationConfirmed_ || view_->lifecycleState() != GeckoSessionView::State::Active) && !activationRequest_) {
    const quint64 epoch = presentationEpoch_;
    auto *request = view_->setEngineActive(true);
    activationRequest_ = request;
    connect(request, &GeckoRequest::finished, this, [this, epoch, request](const QString &status, const QJsonObject &) {
      if (activationRequest_ == request) activationRequest_.clear();
      if (epoch == presentationEpoch_ && engineActive_ && status == QLatin1String("completed")) activationConfirmed_ = true;
      if (epoch != presentationEpoch_ && engineActive_) present();
    });
  }
}

void GeckoEngineView::requestClose(std::function<void(bool allowed)> done) {
  const auto gone = [this] {
    return (view_ && view_->lifecycleState() == GeckoSessionView::State::Closed) ||
           (backend_ && !backend_->session()->hostIsRunning() &&
            (backend_->session()->state() == GeckoSession::State::Disconnected ||
             backend_->session()->state() == GeckoSession::State::Stopped));
  };
  if (gone()) { done(true); return; }
  // No observed document yet, or another close is pending: no permission.
  if (!view_ || closing_ || !backend_ || view_->documentId().isEmpty()) { done(false); return; }
  closing_ = true;
  QPointer<GeckoEngineView> self(this);
  QPointer<GeckoSessionView> target(view_);
  const quint64 generation = target->generation();
  const QString document = target->documentId();
  auto answered = std::make_shared<bool>(false);
  auto finish = [self, target, generation, answered, done](bool allowed) {
    if (!self || *answered) return;
    *answered = true;
    self->closing_ = false;
    done(allowed && self->view_ == target && target && target->generation() == generation &&
         self->backend_ && self->backend_->session()->generation() == generation &&
         (target->lifecycleState() == GeckoSessionView::State::Closed ||
          (target->lifecycleState() == GeckoSessionView::State::Lost && !self->backend_->session()->hostIsRunning())));
  };
  auto awaitClosure = [self, target, finish] {
    if (!self || !target) return;
    if (target->lifecycleState() == GeckoSessionView::State::Closed) { finish(true); return; }
    connect(target, &GeckoSessionView::lifecycleStateChanged, self.data(),
            [finish](GeckoSessionView::State state) {
      if (state == GeckoSessionView::State::Closed) finish(true);
      else if (state == GeckoSessionView::State::Lost) finish(false);
    });
    QTimer::singleShot(5000, self.data(), [finish] { finish(false); });
  };
  // The plain close names the document Qt observed. When the host refuses it
  // as stale because the tab moved on to a document Qt had not observed yet
  // (the internal New Tab replacing about:blank right after Ctrl+T), it is sent
  // once more, for the document the host named and only after Qt observed it:
  // the same guarded close, with the host's beforeunload check, now bound to
  // that document. Any other document, no observation in time, or a second
  // refusal denies the close.
  using Attempt = std::function<void(const QString &document, bool retried)>;
  auto attempt = std::make_shared<Attempt>();
  std::weak_ptr<Attempt> again = attempt;
  *attempt = [self, target, generation, finish, awaitClosure, again](const QString &document, bool retried) {
    if (!self || !target) { finish(false); return; }
    GeckoRequest *request = target->close(false, document);
    if (!request) { finish(false); return; }
    connect(request, &GeckoRequest::finished, self.data(),
            [self, target, generation, document, retried, finish, awaitClosure, keep = again.lock()](
                const QString &status, const QJsonObject &result) {
      if (!self) return;
      if (!target || self->view_ != target || target->generation() != generation) { finish(false); return; }
      const bool ceased = target->lifecycleState() == GeckoSessionView::State::Closed ||
          (self->backend_ && !self->backend_->session()->hostIsRunning() &&
           target->lifecycleState() == GeckoSessionView::State::Lost);
      if (ceased) { finish(true); return; }
      if (status == QLatin1String("completed")) { awaitClosure(); return; }
      const QString code = result.value(QStringLiteral("code")).toString();
      const QString current = result.value(QStringLiteral("documentId")).toString();
      if (status == QLatin1String("rejected") && code == QLatin1String("stale-document") && !retried && keep &&
          !current.isEmpty() && current != document && target->lifecycleState() != GeckoSessionView::State::Lost &&
          (target->documentId() == document || target->documentId() == current)) {
        auto resolved = std::make_shared<bool>(false);
        auto watch = std::make_shared<QMetaObject::Connection>();
        auto settle = [self, target, generation, document, current, finish, keep, resolved, watch] {
          if (*resolved) return;
          const bool same = self && target && self->view_ == target && target->generation() == generation &&
                            target->lifecycleState() != GeckoSessionView::State::Lost;
          if (same && target->documentId() == document) return;  // not observed yet
          *resolved = true;
          QObject::disconnect(*watch);
          if (same && target->documentId() == current) (*keep)(current, true);
          else finish(false);
        };
        *watch = connect(target, &GeckoSessionView::documentChanged, self.data(), [settle](const QString &) { settle(); });
        QTimer::singleShot(2000, self.data(), [resolved, watch, finish] {
          if (*resolved) return;
          *resolved = true;
          QObject::disconnect(*watch);
          finish(false);
        });
        settle();
        return;
      }
      if (status != QLatin1String("rejected") || code != QLatin1String("beforeunload-confirmation-required") ||
          current != document || target->documentId() != document || !self->backend_) { finish(false); return; }
      // Confirmation may enter a nested loop, navigate, or delete this widget.
      const bool confirmed = self->backend_->confirmDiscard(self, self->title());
      if (!self) return;
      if (!confirmed || !target || self->view_ != target || target->generation() != generation ||
          target->documentId() != document || target->lifecycleState() == GeckoSessionView::State::Lost) {
        finish(false); return;
      }
      GeckoRequest *discard = target->close(true, document);
      if (!discard) { finish(false); return; }
      connect(discard, &GeckoRequest::finished, self.data(),
              [finish, awaitClosure](const QString &status, const QJsonObject &) {
        if (status == QLatin1String("completed")) awaitClosure();
        else finish(false);
      });
    });
  };
  (*attempt)(document, false);
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
