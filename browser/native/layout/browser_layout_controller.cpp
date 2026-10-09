#include "browser_layout_controller.h"

#include <QCursor>
#include <QEvent>
#include <QMouseEvent>
#include <QPropertyAnimation>
#include <QApplication>

namespace dalinira::layout {

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
      mode_(BrowserLayoutMode::Classic) {
  if (bodyContainer_) {
    bodyContainer_->setMouseTracking(true);
    bodyContainer_->installEventFilter(this);
  }
  if (qApp) qApp->installEventFilter(this);
  pointerPoll_ = new QTimer(this);
  pointerPoll_->setObjectName(QStringLiteral("floatingPanelPointerPoll"));
  pointerPoll_->setInterval(100);
  connect(pointerPoll_, &QTimer::timeout, this, [this] {
    // Only a native page area hides pointer moves from Qt.
    if (!window_ || !window_->isVisible() || window_->isMinimized() || !pageStack_ ||
        !pageStack_->internalWinId())
      return;
    updateFloatingPanelForPointer(QCursor::pos());
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

bool BrowserLayoutController::preservesPageWidget(QWidget *page) const {
  return page && pageStack_ && (page == pageStack_ || page->parentWidget() == pageStack_ ||
                               pageStack_->isAncestorOf(page));
}

void BrowserLayoutController::setLayoutMode(BrowserLayoutMode mode, bool animated) {
  mode_ = mode;
  animationsEnabled_ = animated;
  if (mode == BrowserLayoutMode::FloatingSidebar) pointerPoll_->start();
  else pointerPoll_->stop();
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
  emit layoutModeChanged(mode_);
}

void BrowserLayoutController::applyClassicLayout(bool /*animated*/) {
  if (bodyLayout_ && sidePanel_) {
    bodyLayout_->removeWidget(sidePanel_);
    sidePanel_->hide();
  }
  if (topBar_) {
    topBar_->show();
  }
  if (tabStrip_) tabStrip_->show();
}

void BrowserLayoutController::applyLeftPanelLayout(bool /*animated*/) {
  placeDockedPanel(true);
}

void BrowserLayoutController::applyRightPanelLayout(bool /*animated*/) {
  placeDockedPanel(false);
}

void BrowserLayoutController::applyVerticalTabsLayout(bool animated) {
  applyLeftPanelLayout(animated);
}

void BrowserLayoutController::placeDockedPanel(bool left) {
  if (!bodyLayout_ || !sidePanel_) return;
  bodyLayout_->removeWidget(sidePanel_);
  sidePanel_->setParent(bodyContainer_);
  if (left) bodyLayout_->insertWidget(0, sidePanel_);
  else bodyLayout_->addWidget(sidePanel_);
  sidePanel_->show();
  if (topBar_) topBar_->show();
  if (tabStrip_) tabStrip_->hide();
  floatingPanelShown_ = false;
}

void BrowserLayoutController::applyFloatingPanelLayout(bool animated) {
  if (!bodyContainer_ || !sidePanel_ || !bodyLayout_) return;
  bodyLayout_->removeWidget(sidePanel_);
  sidePanel_->setParent(bodyContainer_);
  sidePanel_->raise();
  if (topBar_) topBar_->show();
  if (tabStrip_) tabStrip_->hide();
  setFloatingPanelVisible(false, animated);
}

void BrowserLayoutController::setFloatingPanelVisible(bool visible, bool animated) {
  if (mode_ != BrowserLayoutMode::FloatingSidebar || !sidePanel_ || !bodyContainer_) return;
  floatingPanelShown_ = visible;
  const int margin = 8;
  const int width = qBound(sidePanel_->minimumWidth(), 224, sidePanel_->maximumWidth());
  const QRect shown(margin, margin, width,
                    qMax(1, bodyContainer_->height() - margin * 2));
  const QRect hidden(-width - 2, margin, width, shown.height());
  // A WebKit page is a native window, and with it the page stack: an alien
  // panel would be painted under the page. A native panel stacks above it.
  if (pageStack_ && pageStack_->internalWinId() && !sidePanel_->internalWinId()) sidePanel_->winId();
  sidePanel_->show();
  sidePanel_->raise();
  if (!animated) {
    sidePanel_->setGeometry(visible ? shown : hidden);
    return;
  }
  auto *animation = new QPropertyAnimation(sidePanel_, "geometry", this);
  animation->setDuration(180);
  animation->setEasingCurve(QEasingCurve::OutCubic);
  animation->setStartValue(sidePanel_->geometry().isValid() ? sidePanel_->geometry() : hidden);
  animation->setEndValue(visible ? shown : hidden);
  animation->start(QAbstractAnimation::DeleteWhenStopped);
}

bool BrowserLayoutController::eventFilter(QObject *watched, QEvent *event) {
  if (mode_ == BrowserLayoutMode::FloatingSidebar) {
    if (event->type() == QEvent::Resize) {
      if (watched == bodyContainer_) setFloatingPanelVisible(floatingPanelShown_, false);
    } else if (event->type() == QEvent::MouseMove) {
      const auto *mouse = static_cast<QMouseEvent *>(event);
      auto *widget = qobject_cast<QWidget *>(watched);
      if (!widget || !bodyContainer_ ||
          (widget != bodyContainer_ && !bodyContainer_->isAncestorOf(widget)))
        return QObject::eventFilter(watched, event);
      updateFloatingPanelForPointer(mouse->globalPosition().toPoint());
    }
  }
  return QObject::eventFilter(watched, event);
}

void BrowserLayoutController::updateFloatingPanelForPointer(const QPoint &globalPos) {
  if (mode_ != BrowserLayoutMode::FloatingSidebar || !bodyContainer_ || !sidePanel_) return;
  const QPoint local = bodyContainer_->mapFromGlobal(globalPos);
  if (!bodyContainer_->rect().contains(local)) return;
  if (!floatingPanelShown_ && local.x() <= 10) setFloatingPanelVisible(true, animationsEnabled_);
  else if (floatingPanelShown_ && local.x() > sidePanel_->width() + 32)
    setFloatingPanelVisible(false, animationsEnabled_);
}

} // namespace dalinira::layout
