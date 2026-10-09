#include "tab_throbber.h"

#include <QDateTime>
#include <QPainter>
#include <QPixmap>
#include <QWebEngineView>
#include <cmath>

TabThrobber &TabThrobber::instance() {
  static TabThrobber singleInstance;
  return singleInstance;
}
TabThrobber::TabThrobber(QObject *parent) : QObject(parent) {
  animationTimer_.setInterval(33); // ~30 FPS
  connect(&animationTimer_, &QTimer::timeout, this, &TabThrobber::onTimerTick);
}

void TabThrobber::startLoading(const QObject *view, QObject *ownerWindow, qint64 currentTimeMs) {
  if (!view) return;
  const qint64 now = (currentTimeMs > 0) ? currentTimeMs : QDateTime::currentMSecsSinceEpoch();
  ViewState &state = states_[view];
  state.view = view;
  state.ownerWindow = ownerWindow;
  state.isLoading = true;
  state.visibleOnTabBar = false; // Activation delay starts
  state.startTimeMs = now;

  connect(view, &QObject::destroyed, this, &TabThrobber::onViewDestroyed, Qt::UniqueConnection);
  updateTimerState();
}

void TabThrobber::onViewDestroyed(QObject *obj) {
  if (!obj) return;
  removeView(obj);
}

void TabThrobber::finishLoading(const QObject *view, bool success) {
  Q_UNUSED(success);
  if (!view) return;
  states_.remove(view);
  updateTimerState();
  emit throbberTick();
}

void TabThrobber::cacheFavicon(const QObject *view, const QIcon &icon) {
  if (!view || icon.isNull()) return;
  ViewState *state = findState(view);
  if (state) {
    state->cachedFavicon = icon;
  }
}

void TabThrobber::updateOwner(const QObject *view, QObject *newOwner) {
  if (!view) return;
  ViewState *state = findState(view);
  if (state) {
    state->ownerWindow = newOwner;
  }
}

void TabThrobber::removeView(const QObject *view) {
  if (!view) return;
  states_.remove(view);
  updateTimerState();
}

bool TabThrobber::isLoading(const QObject *view) const {
  const ViewState *state = findState(view);
  return state && state->isLoading;
}

bool TabThrobber::isThrobberVisible(const QObject *view) const {
  const ViewState *state = findState(view);
  return state && state->isLoading && state->visibleOnTabBar;
}

QIcon TabThrobber::cachedFavicon(const QObject *view) const {
  const ViewState *state = findState(view);
  return state ? state->cachedFavicon : QIcon();
}

TabThrobber::ViewState *TabThrobber::findState(const QObject *view) {
  auto it = states_.find(view);
  return (it != states_.end()) ? &it.value() : nullptr;
}

const TabThrobber::ViewState *TabThrobber::findState(const QObject *view) const {
  auto it = states_.constFind(view);
  return (it != states_.constEnd()) ? &it.value() : nullptr;
}

void TabThrobber::updateTimerState() {
  const bool shouldRun = !states_.isEmpty();
  if (shouldRun && !animationTimer_.isActive()) {
    frameStep_ = 0;
    animationTimer_.start();
  } else if (!shouldRun && animationTimer_.isActive()) {
    animationTimer_.stop();
    frameStep_ = 0;
  }
}

void TabThrobber::onTimerTick() {
  if (states_.isEmpty()) {
    updateTimerState();
    return;
  }

  frameStep_ += 1;
  const qint64 now = QDateTime::currentMSecsSinceEpoch();

  for (auto it = states_.begin(); it != states_.end(); ++it) {
    ViewState &state = it.value();
    if (state.isLoading && !state.visibleOnTabBar) {
      if ((now - state.startTimeMs) >= 100) { // 100 ms activation delay
        state.visibleOnTabBar = true;
      }
    }
  }

  emit throbberTick();
}

QIcon TabThrobber::renderThrobberIcon(int frameStep, const QPalette &palette, bool activeTab, qreal dpr, int baseSize) {
  const qreal scale = (dpr > 0.0) ? dpr : 1.0;
  const int size = static_cast<int>(baseSize * scale);
  QPixmap pixmap(size, size);
  pixmap.setDevicePixelRatio(scale);
  pixmap.fill(Qt::transparent);

  QPainter painter(&pixmap);
  painter.setRenderHint(QPainter::Antialiasing, true);

  // Color: accent for active tab, muted for inactive
  QColor strokeColor;
  if (activeTab) {
    strokeColor = palette.color(QPalette::Highlight);
    if (!strokeColor.isValid() || strokeColor.alpha() == 0 ||
        strokeColor == palette.color(QPalette::WindowText)) {
      strokeColor = QColor(QStringLiteral("#4d90fe")); // Chrome blue
    }
  } else {
    strokeColor = palette.color(QPalette::PlaceholderText);
    if (!strokeColor.isValid() || strokeColor.alpha() == 0 ||
        strokeColor == palette.color(QPalette::WindowText)) {
      strokeColor = QColor(QStringLiteral("#9aa0a6"));
    }
  }

  // Track (background circle) — very faint
  QColor trackColor = strokeColor;
  trackColor.setAlphaF(0.15);
  QPen trackPen(trackColor);
  trackPen.setWidthF(1.6 * scale);
  trackPen.setCapStyle(Qt::RoundCap);
  painter.setPen(trackPen);
  const qreal margin = 2.5 * scale;
  const QRectF bounds(margin, margin, size - 2 * margin, size - 2 * margin);
  painter.drawEllipse(bounds);

  // Spinner arc — fixed 270° span, rotates at constant speed
  QPen spinPen(strokeColor);
  spinPen.setWidthF(1.6 * scale);
  spinPen.setCapStyle(Qt::RoundCap);
  painter.setPen(spinPen);

  // 14° per frame → one full revolution in ~25.7 frames ≈ 0.85s at 30fps
  const int startAngleDeg = (360 - (frameStep * 14) % 360);
  const int spanAngleDeg  = -270; // fixed 270° arc, Chrome-style

  painter.drawArc(bounds, startAngleDeg * 16, spanAngleDeg * 16);
  painter.end();

  return QIcon(pixmap);
}
