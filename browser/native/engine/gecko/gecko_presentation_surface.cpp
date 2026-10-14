#include "engine/gecko/gecko_presentation_surface.h"

#include <QApplication>
#include <QEvent>
#include <QLoggingCategory>
#include <QMouseEvent>
#include <QWidget>

#include "engine/gecko/gecko_engine_view.h"
#include "engine/gecko/gecko_session.h"
#include "engine/gecko/gecko_x11_embedder.h"

Q_LOGGING_CATEGORY(lcGeckoSurface, "dalinira.gecko.surface")

namespace dalinira::engine {

namespace {
constexpr int kFindIntervalMs = 100;
constexpr int kFindAttempts = 150;  // the host's window, after its session is ready
}  // namespace

GeckoPresentationSurface::GeckoPresentationSurface(GeckoSession *session, QObject *parent)
    : QObject(parent), session_(session), embedder_(new GeckoX11Embedder(this)) {
  QString why;
  setStatus(embedder_->isUsable(&why) ? QStringLiteral("Starting Gecko…")
                                      : QStringLiteral("Gecko pages cannot be shown here: %1.").arg(why));
  findTimer_.setInterval(kFindIntervalMs);
  connect(&findTimer_, &QTimer::timeout, this, &GeckoPresentationSurface::tryEmbed);
  connect(session, &GeckoSession::ready, this, [this] {
    findAttempts_ = 0;
    tryEmbed();
  });
  connect(session, &GeckoSession::disconnected, this, [this](const QString &why) {
    findTimer_.stop();
    setStatus(QStringLiteral("Gecko stopped (%1). Restart DaliNira to use Gecko again.").arg(why));
  });
  connect(embedder_, &GeckoX11Embedder::embeddedGone, this, [this](const QString &why) {
    if (container_) container_->deleteLater();
    container_ = nullptr;
    setStatus(QStringLiteral("The Gecko window is gone (%1).").arg(why));
  });
  qApp->installEventFilter(this);
}

GeckoPresentationSurface::~GeckoPresentationSurface() {
  if (qApp) qApp->removeEventFilter(this);
  embedder_->forget();
  delete container_.data();
}

void GeckoPresentationSurface::setStatus(const QString &status) {
  if (status_ == status) return;
  status_ = status;
  emit statusChanged(status_);
}

bool GeckoPresentationSurface::present(GeckoEngineView *view) {
  if (!view || !session_) return false;
  QWidget *window = view->window();
  if (!owner_) {
    if (!session_->claimPresentation(window)) return false;
    owner_ = window;
  }
  if (window != owner_) return false;
  current_ = view;
  if (!container_) {
    tryEmbed();
    if (!container_) return false;
  }
  if (container_->parentWidget() != view) container_->setParent(view);
  container_->setGeometry(view->rect());
  container_->show();
  container_->raise();
  return true;
}

QJsonObject GeckoPresentationSurface::inspect() const {
  return container_ && owner_ ? embedder_->inspect(owner_, container_) : QJsonObject{};
}

bool GeckoPresentationSurface::isPresenting(const GeckoEngineView *view) const {
  return view && container_ && current_ == view && container_->parentWidget() == view;
}

void GeckoPresentationSurface::viewResized(GeckoEngineView *view) {
  if (isPresenting(view)) container_->setGeometry(view->rect());
}

void GeckoPresentationSurface::focusPage(GeckoEngineView *view) {
  if (isPresenting(view)) embedder_->focusEmbedded();
}

void GeckoPresentationSurface::release(GeckoEngineView *view) {
  if (current_ == view) current_ = nullptr;
  if (container_ && container_->parentWidget() == view) park();
}

// A hidden child of the window: the surface stays in its window, unmapped.
QWidget *GeckoPresentationSurface::parking() {
  if (!parking_ && owner_) {
    parking_ = new QWidget(owner_);
    parking_->setObjectName(QStringLiteral("gecko-surface-parking"));
    parking_->setGeometry(0, 0, 1, 1);
    parking_->hide();
  }
  return parking_;
}

void GeckoPresentationSurface::park() {
  if (container_) container_->setParent(parking());
}

void GeckoPresentationSurface::tryEmbed() {
  QString why;
  if (container_ || !owner_ || !session_ || session_->state() != GeckoSession::State::Ready || !embedder_->isUsable(&why)) {
    findTimer_.stop();
    return;
  }
  const auto window = embedder_->findHostWindow(session_->hostPid(), QStringLiteral("DNG-HOST ") + session_->windowTag(), &why);
  if (!window) {
    if (++findAttempts_ > kFindAttempts) {
      findTimer_.stop();
      setStatus(QStringLiteral("The Gecko window was not found: %1.").arg(why));
    } else if (!findTimer_.isActive()) {
      findTimer_.start();
    }
    return;
  }
  findTimer_.stop();
  container_ = embedder_->embed(*window, parking());
  if (!container_) {
    setStatus(QStringLiteral("The Gecko window could not be embedded."));
    return;
  }
  qCInfo(lcGeckoSurface) << "embedded the Gecko host window" << window->window;
  setStatus({});
  if (current_) present(current_);
}

bool GeckoPresentationSurface::eventFilter(QObject *watched, QEvent *event) {
  if (event->type() == QEvent::Close && watched == owner_ && container_) {
    // The window is about to be destroyed (BrowserWindow accepts every close):
    // take the host's window out of it first.
    container_->hide();
    container_->setParent(nullptr);
    current_ = nullptr;
  } else if (event->type() == QEvent::MouseButtonPress && container_ && owner_ && embedder_->embeddedWindow()) {
    // A click on DaliNira's own widgets gives the keyboard back to Qt; the
    // embedder moves it to the page on a click there.
    auto *widget = qobject_cast<QWidget *>(watched);
    if (widget && widget->window() == owner_ && widget != container_ &&
        embedder_->inputFocus() == embedder_->embeddedWindow()) {
      embedder_->focusQt(owner_);
    }
  }
  return QObject::eventFilter(watched, event);
}

}  // namespace dalinira::engine
