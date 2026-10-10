#include "browser_layout_controller.h"

#include <QApplication>
#include <QCursor>
#include <QEvent>
#include <QMouseEvent>
#include <QPropertyAnimation>
#include <QSettings>

namespace dalinira::layout {

namespace {
const QString kAutoHideKey = QStringLiteral("browser/sidePanelAutoHide");

// A menu (context menu, the panel's own popups) or a pressed button means the
// user is still working with the panel: it must not slide away under them.
bool userBusyElsewhere() {
  return QApplication::activePopupWidget() != nullptr || QApplication::mouseButtons() != Qt::NoButton;
}
}  // namespace

QString sidePanelStateName(SidePanelState state) {
  switch (state) {
    case SidePanelState::None: return QStringLiteral("none");
    case SidePanelState::Docked: return QStringLiteral("docked");
    case SidePanelState::Collapsed: return QStringLiteral("collapsed");
    case SidePanelState::Revealed: return QStringLiteral("revealed");
  }
  return QStringLiteral("none");
}

bool sidePanelAutoHideEnabled(BrowserLayoutMode mode) {
  const QSettings settings;
  if (settings.contains(kAutoHideKey)) return settings.value(kAutoHideKey).toBool();
  return mode == BrowserLayoutMode::FloatingSidebar;
}

namespace {
int storedAutoHideSetting() {
  const QSettings settings;
  return settings.contains(kAutoHideKey) ? (settings.value(kAutoHideKey).toBool() ? 1 : 0) : -1;
}
}  // namespace

void setSidePanelAutoHideEnabled(bool enabled) {
  QSettings settings;
  settings.setValue(kAutoHideKey, enabled);
  settings.sync();
}

BrowserLayoutController::BrowserLayoutController(QWidget *window,
                                                 QWidget *bodyContainer,
                                                 QBoxLayout *bodyLayout,
                                                 QWidget *sidePanel,
                                                 QWidget *pageStack,
                                                 QWidget *topBar,
                                                 QWidget *tabStrip,
                                                 QWidget *navBar,
                                                 QObject *parent)
    : QObject(parent),
      window_(window),
      bodyContainer_(bodyContainer),
      bodyLayout_(bodyLayout),
      sidePanel_(sidePanel),
      pageStack_(pageStack),
      topBar_(topBar),
      tabStrip_(tabStrip),
      navBar_(navBar),
      mode_(BrowserLayoutMode::Classic),
      autoHideSetting_(storedAutoHideSetting()) {
  if (bodyContainer_) bodyContainer_->setMouseTracking(true);
  // Revealing never takes keyboard focus from the page or the address bar.
  if (sidePanel_) sidePanel_->setAttribute(Qt::WA_ShowWithoutActivating);
  // One application filter sees the body's and the panel's events once each.
  if (qApp) qApp->installEventFilter(this);

  hideTimer_ = new QTimer(this);
  hideTimer_->setObjectName(QStringLiteral("sidePanelHideTimer"));
  hideTimer_->setSingleShot(true);
  hideTimer_->setInterval(kHideDelayMs);
  connect(hideTimer_, &QTimer::timeout, this, [this] {
    if (!pointerRevealed_ || state_ != SidePanelState::Revealed) return;
    if (userBusyElsewhere()) {
      hideTimer_->start();
      return;
    }
    if (!pointerOverPanel(QCursor::pos())) hidePointerRevealed();
  });

}

bool BrowserLayoutController::isSidePanelVisible() const {
  return sidePanel_ && !sidePanel_->isHidden();
}

bool BrowserLayoutController::isSidePanelOnLeft() const {
  return mode_ == BrowserLayoutMode::VerticalTabsLeft && isSidePanelVisible();
}

bool BrowserLayoutController::isSidePanelOnRight() const {
  return mode_ == BrowserLayoutMode::VerticalTabsRight && isSidePanelVisible();
}

bool BrowserLayoutController::isHorizontalTabsVisible() const {
  return tabStrip_ && !tabStrip_->isHidden();
}

bool BrowserLayoutController::isVerticalTabsEnabled() const {
  return mode_ == BrowserLayoutMode::VerticalTabsLeft ||
         mode_ == BrowserLayoutMode::VerticalTabsRight;
}

bool BrowserLayoutController::isFloatingPanelEnabled() const {
  return mode_ == BrowserLayoutMode::FloatingSidebar;
}

bool BrowserLayoutController::isBottomTabsEnabled() const { return false; }

bool BrowserLayoutController::isAutoHideActive() const {
  if (mode_ == BrowserLayoutMode::Classic) return false;
  return autoHideSetting_ < 0 ? isFloatingPanelEnabled() : autoHideSetting_ == 1;
}

bool BrowserLayoutController::preservesPageWidget(QWidget *page) const {
  return page && pageStack_ && (page == pageStack_ || page->parentWidget() == pageStack_ ||
                               pageStack_->isAncestorOf(page));
}

void BrowserLayoutController::setLayoutMode(BrowserLayoutMode mode, bool animated) {
  mode_ = mode;
  animationsEnabled_ = animated;
  stopSlide();
  hideTimer_->stop();
  pointerRevealed_ = false;
  // Start from a clean slate: the panel's place follows from the new mode.
  undock();
  if (sidePanel_) sidePanel_->hide();
  state_ = SidePanelState::None;
  switch (mode) {
    case BrowserLayoutMode::Classic:
      applyClassicLayout(animated);
      break;
    case BrowserLayoutMode::VerticalTabsLeft:
      applyLeftPanelLayout(animated);
      break;
    case BrowserLayoutMode::VerticalTabsRight:
      applyRightPanelLayout(animated);
      break;
    case BrowserLayoutMode::FloatingSidebar:
      applyFloatingPanelLayout(animated);
      break;
  }
  // A layout switch places the panel at once; only reveals and hides slide.
  animationsEnabled_ = false;
  applyState(targetState());
  animationsEnabled_ = animated;
  emit pinnedChanged(isPinned());
  emit layoutModeChanged(mode_);
}

void BrowserLayoutController::applyClassicLayout(bool /*animated*/) {
  if (topBar_) topBar_->show();
  if (tabStrip_) tabStrip_->show();
}

void BrowserLayoutController::applyLeftPanelLayout(bool /*animated*/) {
  if (topBar_) topBar_->show();
  if (tabStrip_) tabStrip_->hide();
}

void BrowserLayoutController::applyRightPanelLayout(bool animated) {
  applyLeftPanelLayout(animated);
}

void BrowserLayoutController::applyVerticalTabsLayout(bool animated) {
  applyLeftPanelLayout(animated);
}

void BrowserLayoutController::applyFloatingPanelLayout(bool animated) {
  applyLeftPanelLayout(animated);
}

void BrowserLayoutController::setAutoHideEnabled(bool enabled) {
  const bool wasPinned = isPinned();
  autoHideSetting_ = enabled ? 1 : 0;
  if (isPinned() == wasPinned) return;
  pointerRevealed_ = false;
  hideTimer_->stop();
  evaluate();
  emit pinnedChanged(isPinned());
}

void BrowserLayoutController::setNewTabActive(bool newTab, bool loading) {
  // New Tab keeps the panel open; a navigation away from it releases the
  // hold once it has finished loading, so the page does not resize mid-load.
  bool hold = newTabHold_;
  if (newTab) hold = true;
  else if (!loading) hold = false;
  if (hold == newTabHold_) return;
  newTabHold_ = hold;
  evaluate();
}

void BrowserLayoutController::setFullScreen(bool fullScreen) {
  if (fullScreen_ == fullScreen) return;
  fullScreen_ = fullScreen;
  pointerRevealed_ = false;
  hideTimer_->stop();
  evaluate();
}

void BrowserLayoutController::setTabDragActive(bool dragging) {
  if (dragging_ == dragging) return;
  dragging_ = dragging;
  if (!dragging && state_ == SidePanelState::Revealed && isAutoHideActive()) {
    // The drag kept it open; now the pointer decides again.
    pointerRevealed_ = true;
    if (!pointerOverPanel(QCursor::pos())) hideTimer_->start();
  }
  evaluate();
}

SidePanelState BrowserLayoutController::targetState() const {
  if (mode_ == BrowserLayoutMode::Classic || !sidePanel_ || !bodyLayout_) return SidePanelState::None;
  // Fullscreen content (page element or browser fullscreen) is never covered.
  if (fullScreen_) return SidePanelState::Collapsed;
  // A drag that started in the visible panel keeps it where it is.
  if (dragging_ && (state_ == SidePanelState::Docked || state_ == SidePanelState::Revealed)) return state_;
  // Pinned, or the New Tab page: part of the window, beside the page (also
  // in the floating layout, whose card style is for the overlay reveal).
  if (!isAutoHideActive() || newTabHold_) return SidePanelState::Docked;
  if (pointerRevealed_) return SidePanelState::Revealed;
  return SidePanelState::Collapsed;
}

void BrowserLayoutController::evaluate() {
  SidePanelState next = targetState();
  // Leaving New Tab (or "kept open") while the pointer is on the docked panel,
  // e.g. after clicking a website tab in it: it stays usable over the page
  // until the pointer leaves, instead of vanishing under the pointer.
  if (next == SidePanelState::Collapsed && state_ == SidePanelState::Docked && !fullScreen_ &&
      pointerOverPanel(QCursor::pos())) {
    pointerRevealed_ = true;
    next = SidePanelState::Revealed;
  }
  if (next != state_) applyState(next);
}

void BrowserLayoutController::applyState(SidePanelState next) {
  if (!sidePanel_ || !bodyContainer_) return;
  const SidePanelState previous = state_;
  state_ = next;
  if (next != SidePanelState::Revealed) hideTimer_->stop();
  // A WebKit page is a native window, and with it the page stack: an alien
  // panel would be painted under the page. A native panel stacks above it.
  if (next != SidePanelState::None && pageStack_ && pageStack_->internalWinId() && !sidePanel_->internalWinId())
    sidePanel_->winId();
  // The floating layout's overlay is an inset, rounded card.
  sidePanel_->setProperty("floatingCard",
                          isFloatingPanelEnabled() && (next == SidePanelState::Revealed ||
                                                       (next == SidePanelState::Collapsed && sidePanel_->isVisible())));
  switch (next) {
    case SidePanelState::None:
      stopSlide();
      undock();
      sidePanel_->hide();
      break;
    case SidePanelState::Docked:
      // From an overlay at the same place: docking is seamless.
      stopSlide();
      dock();
      break;
    case SidePanelState::Collapsed: {
      const bool wasShown = sidePanel_->isVisible();
      if (docked_) {
        undock();  // the page takes the full width once, here
        sidePanel_->setGeometry(overlayRect(true));
        sidePanel_->raise();
      }
      if (wasShown) slideTo(overlayRect(false), true);
      else sidePanel_->hide();
      break;
    }
    case SidePanelState::Revealed:
      if (docked_) {
        undock();
        sidePanel_->setGeometry(overlayRect(true));
        sidePanel_->show();
        sidePanel_->raise();
        stopSlide();
        break;
      }
      if (!sidePanel_->isVisible() || previous == SidePanelState::Collapsed) {
        if (!sidePanel_->isVisible()) sidePanel_->setGeometry(overlayRect(false));
        sidePanel_->show();
      }
      sidePanel_->raise();
      slideTo(overlayRect(true), false);
      break;
  }
  emit sidePanelStateChanged(next);
}

void BrowserLayoutController::dock() {
  if (!bodyLayout_ || !sidePanel_) return;
  if (!docked_) {
    bodyLayout_->removeWidget(sidePanel_);
    if (sidePanel_->parentWidget() != bodyContainer_) sidePanel_->setParent(bodyContainer_);
    if (panelOnLeft()) bodyLayout_->insertWidget(0, sidePanel_);
    else bodyLayout_->addWidget(sidePanel_);
    docked_ = true;
  }
  sidePanel_->show();
}

void BrowserLayoutController::undock() {
  if (!docked_ || !bodyLayout_ || !sidePanel_) return;
  if (sidePanel_->width() >= sidePanel_->minimumWidth()) dockedWidth_ = sidePanel_->width();
  bodyLayout_->removeWidget(sidePanel_);
  docked_ = false;
}

QRect BrowserLayoutController::overlayRect(bool shown) const {
  if (!bodyContainer_ || !sidePanel_) return {};
  const QRect body = bodyContainer_->rect();
  if (isFloatingPanelEnabled()) {
    // The floating layout: a card inset from the left edge.
    const int margin = 8;
    const int width = qBound(sidePanel_->minimumWidth(), 224, sidePanel_->maximumWidth());
    const QRect visible(margin, margin, width, qMax(1, body.height() - margin * 2));
    return shown ? visible : visible.translated(-(width + margin + 2), 0);
  }
  // A docked layout's panel over the page, flush with its own edge.
  const int width = qBound(sidePanel_->minimumWidth(), dockedWidth_, sidePanel_->maximumWidth());
  const int height = qMax(1, body.height());
  if (panelOnLeft()) return QRect(shown ? 0 : -width - 2, 0, width, height);
  return QRect(shown ? body.width() - width : body.width() + 2, 0, width, height);
}

void BrowserLayoutController::slideTo(const QRect &target, bool hideAtEnd) {
  stopSlide();
  if (!animationsEnabled_ || !sidePanel_->isVisible()) {
    sidePanel_->setGeometry(target);
    if (hideAtEnd) sidePanel_->hide();
    return;
  }
  // A short geometry slide of the panel alone: the page is never relaid out
  // or repainted by it (no WebKit/Chromium specific path).
  auto *animation = new QPropertyAnimation(sidePanel_, "geometry", this);
  animation->setDuration(kSlideMs);
  animation->setEasingCurve(QEasingCurve::OutCubic);
  animation->setStartValue(sidePanel_->geometry());
  animation->setEndValue(target);
  if (hideAtEnd) {
    connect(animation, &QPropertyAnimation::finished, this, [this] {
      if (state_ == SidePanelState::Collapsed && sidePanel_) sidePanel_->hide();
    });
  }
  slide_ = animation;
  animation->start(QAbstractAnimation::DeleteWhenStopped);
}

void BrowserLayoutController::stopSlide() {
  if (slide_) slide_->stop();  // DeleteWhenStopped
  slide_.clear();
}

void BrowserLayoutController::hidePointerRevealed() {
  hideTimer_->stop();
  pointerRevealed_ = false;
  evaluate();
}

bool BrowserLayoutController::pointerOverPanel(const QPoint &globalPos) const {
  if (!sidePanel_ || !sidePanel_->isVisible() || !bodyContainer_) return false;
  if (state_ != SidePanelState::Revealed)
    return QRect(sidePanel_->mapToGlobal(QPoint(0, 0)), sidePanel_->size()).contains(globalPos);
  // Revealed: where the panel is going, not where its slide currently is, and
  // the reveal band itself (the pointer resting on the window edge).
  const QRect shown = overlayRect(true);
  if (QRect(bodyContainer_->mapToGlobal(shown.topLeft()), shown.size()).contains(globalPos)) return true;
  const QPoint local = bodyContainer_->mapFromGlobal(globalPos);
  const int edgeDistance = panelOnLeft() ? local.x() : bodyContainer_->width() - 1 - local.x();
  return local.y() >= 0 && local.y() < bodyContainer_->height() && edgeDistance >= 0 && edgeDistance < kRevealEdgePx;
}

bool BrowserLayoutController::eventFilter(QObject *watched, QEvent *event) {
  if (state_ == SidePanelState::Collapsed || state_ == SidePanelState::Revealed) {
    switch (event->type()) {
      case QEvent::Resize:
        if (watched == bodyContainer_ && state_ == SidePanelState::Revealed && !slide_)
          sidePanel_->setGeometry(overlayRect(true));
        break;
      case QEvent::Leave:
        // Native panels report leaving even where the page hides moves.
        if (watched == sidePanel_ && pointerRevealed_ && state_ == SidePanelState::Revealed &&
            !pointerOverPanel(QCursor::pos()))
          hideTimer_->start();
        break;
      case QEvent::Enter:
        if (watched == sidePanel_) hideTimer_->stop();
        break;
      case QEvent::MouseMove: {
        auto *widget = qobject_cast<QWidget *>(watched);
        if (!widget || !bodyContainer_ || (widget != bodyContainer_ && !bodyContainer_->isAncestorOf(widget)))
          break;
        updatePanelForPointer(static_cast<QMouseEvent *>(event)->globalPosition().toPoint());
        break;
      }
      default:
        break;
    }
  }
  return QObject::eventFilter(watched, event);
}

void BrowserLayoutController::updatePanelForPointer(const QPoint &globalPos) {
  if (!isAutoHideActive() || fullScreen_ || !bodyContainer_ || !sidePanel_) return;
  if (state_ != SidePanelState::Collapsed && state_ != SidePanelState::Revealed) return;
  if (userBusyElsewhere()) return;
  const QRect body = bodyContainer_->rect();
  const QPoint local = bodyContainer_->mapFromGlobal(globalPos);
  if (state_ == SidePanelState::Collapsed) {
    // A narrow band along the panel's own edge of the body (the page area),
    // so ordinary page use never reveals it.
    const int edgeDistance = panelOnLeft() ? local.x() : body.width() - 1 - local.x();
    if (local.y() >= 0 && local.y() < body.height() && edgeDistance >= 0 && edgeDistance < kRevealEdgePx) {
      pointerRevealed_ = true;
      evaluate();
    }
    return;
  }
  if (pointerOverPanel(globalPos)) {
    hideTimer_->stop();
    return;
  }
  if (!pointerRevealed_) return;  // kept open or dragging: the pointer does not hide it
  const QRect panel = overlayRect(true);  // where it is shown, also mid-slide
  const int beyond = panelOnLeft() ? local.x() - panel.right() : panel.left() - local.x();
  if (body.contains(local) && beyond > kLeaveSlackPx) {
    hidePointerRevealed();  // clearly gone: no delay
    return;
  }
  if (!hideTimer_->isActive()) hideTimer_->start();
}

} // namespace dalinira::layout
