#include "tab_drag_controller.h"
#include "tab_strip_widget.h"

#include <QApplication>
#include <QDropEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLayout>
#include <QMimeData>
#include <QMouseEvent>
#include <QScreen>
#include <QSet>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace dalinira::desktop_tabs {

static bool isDiagnosticsEnabled() {
  static const bool enabled = qEnvironmentVariableIntValue(
                                  "DALINIRA_DESKTOP_TAB_DIAGNOSTICS") == 1 ||
                              qEnvironmentVariableIntValue(
                                  "DALINIRA_TAB_DIAGNOSTICS") == 1;
  return enabled;
}

static void logDetachGeometry(
    const TabLayoutModel &layoutModel,
    TabStripWidget *strip,
    const QPoint &globalPos,
    const QPoint &referenceGlobalPos,
    bool detachEligible,
    DragState stateBefore,
    DragState stateAfter) {
  if (!isDiagnosticsEnabled() || !strip) return;
  const QPoint stripLocalPos = strip->mapFromGlobal(globalPos);
  const QRect stripGlobalRect(strip->mapToGlobal(QPoint(0, 0)), strip->size());
  const QRect expandedDetachRect = layoutModel.computeDetachBoundary(strip->size())
                                       .translated(stripGlobalRect.topLeft());
  int verticalDistance = 0;
  if (globalPos.y() < stripGlobalRect.top()) {
    verticalDistance = stripGlobalRect.top() - globalPos.y();
  } else if (globalPos.y() > stripGlobalRect.bottom()) {
    verticalDistance = globalPos.y() - stripGlobalRect.bottom();
  }
  const int horizontalDistance = std::abs(globalPos.x() - referenceGlobalPos.x());
  std::fprintf(
      stderr,
      "[DRAG] globalPos=(%d,%d) stripLocalPos=(%d,%d) "
      "stripGlobalRect=(%d,%d %dx%d) expandedDetachRect=(%d,%d %dx%d) "
      "verticalDistance=%d horizontalDistance=%d detachEligible=%d "
      "stateBefore=%s stateAfter=%s\n",
      globalPos.x(), globalPos.y(), stripLocalPos.x(), stripLocalPos.y(),
      stripGlobalRect.x(), stripGlobalRect.y(), stripGlobalRect.width(),
      stripGlobalRect.height(), expandedDetachRect.x(), expandedDetachRect.y(),
      expandedDetachRect.width(), expandedDetachRect.height(), verticalDistance,
      horizontalDistance, detachEligible ? 1 : 0,
      dragStateToString(stateBefore), dragStateToString(stateAfter));
}

TabDragController &TabDragController::instance() {
  static TabDragController s_instance;
  return s_instance;
}

TabDragController::TabDragController(
    std::unique_ptr<TabDragPlatformBackend> platformBackend,
    QObject *parent)
    : QObject(parent),
      detachedWindowController_(platformBackend ? std::move(platformBackend) : nullptr),
      platformBackend_(TabDragPlatformBackend::create()) {}

TabDragController::~TabDragController() {
  handleCancel();
}

void TabDragController::ensureApplicationEventFilter() {
  if (!applicationFilterInstalled_ && qApp) {
    qApp->installEventFilter(this);
    applicationFilterInstalled_ = true;
  }
}

void TabDragController::beginPointerCapture() {
  auto *strip = session_.currentStrip();
  if (!strip || pointerCaptureOwner_) return;
  pointerCaptureOwner_ = strip;
  const bool captured = platformBackend_->beginPointerCapture(strip);
  if (isDiagnosticsEnabled()) {
    std::fprintf(stderr,
                 "[CAPTURE_ACQUIRE] sessionId=%llu source=nullptr target=%p controller=%p browserWindow=%p captured=%d\n",
                 static_cast<unsigned long long>(session_.sessionId()),
                 static_cast<void *>(strip),
                 static_cast<void *>(this),
                 static_cast<void *>(session_.currentWindow()),
                 captured ? 1 : 0);
  }
}

void TabDragController::handoffPointerCapture(QWidget *targetWidget) {
  if (!targetWidget || pointerCaptureOwner_ == targetWidget) return;
  QWidget *const oldOwner = pointerCaptureOwner_.data();
  if (oldOwner) {
    platformBackend_->endPointerCapture(oldOwner);
  }
  pointerCaptureOwner_ = targetWidget;
  const bool captured = platformBackend_->beginPointerCapture(targetWidget);
  if (isDiagnosticsEnabled()) {
    std::fprintf(stderr,
                 "[CAPTURE_TRANSFER] sessionId=%llu source=%p target=%p controller=%p browserWindow=%p captured=%d\n",
                 static_cast<unsigned long long>(session_.sessionId()),
                 static_cast<void *>(oldOwner),
                 static_cast<void *>(targetWidget),
                 static_cast<void *>(this),
                 static_cast<void *>(session_.currentWindow()),
                 captured ? 1 : 0);
  }
}

void TabDragController::endPointerCapture() {
  if (!pointerCaptureOwner_) return;
  QWidget *const oldOwner = pointerCaptureOwner_.data();
  if (oldOwner) {
    oldOwner->unsetCursor();
  }
  platformBackend_->endPointerCapture(oldOwner);
  pointerCaptureOwner_ = nullptr;
  if (isDiagnosticsEnabled()) {
    std::fprintf(stderr,
                 "[CAPTURE_RELEASE] sessionId=%llu source=%p target=nullptr controller=%p browserWindow=%p\n",
                 static_cast<unsigned long long>(session_.sessionId()),
                 static_cast<void *>(oldOwner),
                 static_cast<void *>(this),
                 static_cast<void *>(session_.currentWindow()));
  }
}

bool TabDragController::eventFilter(QObject *watched, QEvent *event) {
  Q_UNUSED(watched);
  if (!session_.isActive()) return QObject::eventFilter(watched, event);

  if (event->type() == QEvent::MouseMove) {
    auto *mouse = static_cast<QMouseEvent *>(event);
    handleMouseMove(mouse->globalPosition().toPoint());
    return true;
  }
  if (event->type() == QEvent::MouseButtonRelease) {
    auto *mouse = static_cast<QMouseEvent *>(event);
    if (mouse->button() == Qt::LeftButton) {
      handleMouseRelease(mouse->globalPosition().toPoint());
      return true;
    }
  }
  if (event->type() == QEvent::KeyPress) {
    auto *key = static_cast<QKeyEvent *>(event);
    if (key->key() == Qt::Key_Escape) {
      handleCancel();
      return true;
    }
  }
  return QObject::eventFilter(watched, event);
}

void TabDragController::setDetachedWindowFactory(DetachedTabWindowController::WindowFactory factory) {
  detachedWindowController_.setWindowFactory(std::move(factory));
}

void TabDragController::setDetachTransferDelegate(
    DetachedTabWindowController::DetachTransferDelegate delegate) {
  detachedWindowController_.setDetachTransferDelegate(std::move(delegate));
}

void TabDragController::handleMousePress(
    QWidget *window,
    TabStripWidget *strip,
    int tabIndex,
    const QPoint &globalPos,
    const QPoint &offsetInTab,
    const QPoint &offsetInWindow) {
  if (!window || !strip || tabIndex < 0 || tabIndex >= strip->count()) {
    return;
  }
  // One physical press owns one drag session. Live attach/re-detach must not
  // synthesize a second Pressed state.
  if (session_.isActive()) return;

  ensureApplicationEventFilter();

  TabModelItem item;
  item.tabId = strip->tabId(tabIndex);
  item.title = strip->tabText(tabIndex);
  item.icon = strip->tabIcon(tabIndex);
  item.isPinned = strip->isTabPinned(tabIndex);
  item.isAudible = strip->isTabAudible(tabIndex);
  item.isLoading = strip->isTabLoading(tabIndex);

  const bool isLastTab = (strip->count() == 1);
  session_.startSession(window, strip, tabIndex, item, globalPos, offsetInTab, offsetInWindow, isLastTab);

  initialSessionId_ = session_.sessionId();
  initialTabId_ = item.tabId;

  if (isDiagnosticsEnabled()) {
    std::fprintf(stderr, "[PRESS] sessionId=%llu tab=%d title='%s' isLastTab=%d global=(%d,%d)\n",
                 static_cast<unsigned long long>(session_.sessionId()),
                 tabIndex, item.title.toUtf8().constData(), isLastTab ? 1 : 0, globalPos.x(), globalPos.y());
  }

  emit stateChanged(session_.state());
}

void TabDragController::handleMouseMove(const QPoint &globalPos) {
  if (!session_.isActive()) return;

  const DragState currentState = session_.state();

  switch (currentState) {
    case DragState::Pressed: {
      const QPoint delta = globalPos - session_.pressGlobalPos();
      auto *strip = session_.currentStrip();
      if (!strip) return;
      const auto &metrics = strip->layoutModel().metrics();
      const bool beyondNoise = delta.manhattanLength() >= metrics.minimumStartDragDistance;
      const bool horizontalDrag = std::abs(delta.x()) >= metrics.horizontalStartDragThreshold;
      const bool verticalDrag = std::abs(delta.y()) >= metrics.horizontalStartDragThreshold;
      // Start the attached phase before detach eligibility. Previously a
      // vertical-only gesture stayed Pressed until it was already outside the
      // strip, so one event produced both start and detach.
      if (beyondNoise && (horizontalDrag || verticalDrag)) {
        session_.setMovedPastThreshold(true);
        startInStripDrag();
        updateInStripDrag(globalPos);
      }
      break;
    }

    case DragState::DraggingInStrip:
    case DragState::DraggingInTargetStrip: {
      updateInStripDrag(globalPos);
      break;
    }

    case DragState::DraggingDetachedWindow:
    case DragState::SearchingAttachTarget: {
      updateDetachedDrag(globalPos);
      break;
    }

    default:
      break;
  }
}

void TabDragController::handleMouseRelease(const QPoint &globalPos) {
  if (!session_.isActive()) return;

  Q_ASSERT(session_.sessionId() == initialSessionId_);
  if (isDiagnosticsEnabled()) {
    std::fprintf(stderr, "[RELEASE] sessionId=%llu pos=(%d,%d)\n",
                 static_cast<unsigned long long>(session_.sessionId()),
                 globalPos.x(), globalPos.y());
  }

  handleMouseMove(globalPos);
  completeDrag(globalPos);
}

void TabDragController::handleCancel() {
  if (!session_.isActive()) return;

  cancelDrag();
}

void TabDragController::startInStripDrag() {
  session_.setState(DragState::DraggingInStrip);
  if (isDiagnosticsEnabled()) {
    std::fprintf(stderr, "[BEGIN_DRAG] sessionId=%llu global=(%d,%d)\n",
                 static_cast<unsigned long long>(session_.sessionId()),
                 session_.pressGlobalPos().x(), session_.pressGlobalPos().y());
  }
  if (auto *strip = session_.currentStrip()) {
    strip->controllerBeginDrag(session_.currentTabIndex(), session_.pressGlobalPos(), session_.pressOffsetInTab());
  }
  // Apply closed-hand drag cursor the moment real dragging begins.
  // This is Chrome-parity: the cursor stays ClosedHandCursor for the entire
  // drag session (in-strip, detached, and re-attached). cleanUpCursorState()
  // calls restoreOverrideCursor() in every termination path, so lifecycle is
  // symmetric. We only push when the override stack is empty to avoid double
  // stacking if the caller re-enters.
  if (!QGuiApplication::overrideCursor()) {
    QGuiApplication::setOverrideCursor(QCursor(Qt::ClosedHandCursor));
  }
  beginPointerCapture();
  emit dragStarted();
  emit stateChanged(session_.state());
}

void TabDragController::updateInStripDrag(const QPoint &globalPos) {
  auto *strip = session_.currentStrip();
  auto *window = session_.currentWindow();
  if (!strip || !window) return;

  const QPoint localPos = strip->mapFromGlobal(globalPos);
  updateInStripDragAt(localPos, globalPos);
}

void TabDragController::updateInStripDragAt(
    const QPoint &localPos, const QPoint &globalPos) {
  auto *strip = session_.currentStrip();
  auto *window = session_.currentWindow();
  if (!strip || !window) return;

  const TabLayoutModel &layoutModel = strip->layoutModel();
  const DragState stateBefore = session_.state();
  bool detachEligible = layoutModel.isOutsideDetachBoundary(localPos, strip->size());

  // The attach halo may accept the tab a few pixels outside the painted row.
  // That edge position must not immediately become a new tear-off. First
  // require entry into the target strip's real vertical extent.
  if (session_.state() == DragState::DraggingInTargetStrip &&
      session_.requiresTargetStripEntry()) {
    const bool enteredRealStrip = localPos.y() >= 0 && localPos.y() < strip->height();
    if (enteredRealStrip) {
      session_.setRequiresTargetStripEntry(false);
      if (isDiagnosticsEnabled()) {
        std::fprintf(stderr,
                     "[DRAG] attach hysteresis cleared generation=%d localY=%d\n",
                     session_.attachGeneration(), localPos.y());
      }
    } else {
      detachEligible = false;
    }
  }

  // Check if cursor broke through vertical detach boundary
  if (detachEligible) {
    if (session_.state() == DragState::DraggingInTargetStrip) {
      performRedetach(globalPos);
    } else {
      performDetach(globalPos);
    }
    logDetachGeometry(layoutModel, strip, globalPos,
                      session_.detachReferenceGlobalPos(), true,
                      stateBefore, session_.state());
    return;
  }

  logDetachGeometry(layoutModel, strip, globalPos,
                    session_.detachReferenceGlobalPos(), false,
                    stateBefore, session_.state());

  // Calculate insertion slot
  QVector<TabModelItem> items;
  for (int i = 0; i < strip->count(); ++i) {
    items.append(TabModelItem{strip->tabId(i), strip->tabText(i), strip->tabIcon(i), strip->isTabPinned(i), strip->isTabAudible(i), strip->isTabLoading(i), strip->isTabSleeping(i), strip->tabData(i)});
  }

  const int currentIndex = session_.currentTabIndex();
  const QRect draggedRect = strip->tabRect(currentIndex);
  const int centerAdjustment = draggedRect.width() / 2 - session_.pressOffsetInTab().x();
  const QPoint draggedCenter(localPos.x() + centerAdjustment, localPos.y());
  const int newSlot = layoutModel.computeInsertionSlot(
      strip->width(), items, currentIndex, draggedCenter);
  if (newSlot != session_.currentInsertionSlot()) {
    session_.setCurrentInsertionSlot(newSlot);
    if (isDiagnosticsEnabled()) {
      if (session_.state() == DragState::DraggingInTargetStrip) {
        std::fprintf(stderr, "[TARGET_DRAG_MOVE] sessionId=%llu pos=(%d,%d) slot=%d\n",
                     static_cast<unsigned long long>(session_.sessionId()),
                     globalPos.x(), globalPos.y(), newSlot);
      } else {
        std::fprintf(stderr, "[DRAG] DraggingInStrip insertionSlot=%d\n", newSlot);
      }
      std::fprintf(stderr, "[ANIM] siblingTargetsUpdated insertionSlot=%d\n", newSlot);
    }
  }

  strip->controllerUpdateDrag(globalPos, newSlot);
}

void TabDragController::performDetach(const QPoint &globalPos) {
  Q_ASSERT(session_.sessionId() == initialSessionId_);
  const bool redetaching = session_.state() == DragState::Redetaching;
  auto *originWindow = session_.currentWindow();
  auto *originStrip = session_.currentStrip();
  const int originIndex = session_.currentTabIndex();
  const DragState fallbackState = redetaching
      ? DragState::DraggingInTargetStrip : DragState::DraggingInStrip;

  const bool movingLastTab = originStrip && originStrip->count() == 1;
  session_.setMovingLastTab(movingLastTab);
  session_.setRequiresTargetStripEntry(false);
  if (isDiagnosticsEnabled()) {
    std::fprintf(stderr, "[DETACH] sessionId=%llu global=(%d,%d) isLastTab=%d\n",
                 static_cast<unsigned long long>(session_.sessionId()),
                 globalPos.x(), globalPos.y(),
                 movingLastTab ? 1 : 0);
  }

  if (!redetaching) {
    session_.setState(DragState::Detaching);
    emit stateChanged(session_.state());
  }

  if (movingLastTab) {
    // The source window IS the dragged window — no new shell, no transfer.
    // In Chromium, dragging the only tab moves the window itself. If maximized,
    // restore it to normal floating geometry anchored at the grabbed tab.
    if (originWindow && (originWindow->isMaximized() || originWindow->isFullScreen() ||
                         (originWindow->windowState() & (Qt::WindowMaximized | Qt::WindowFullScreen)))) {
      QScreen *screen = QGuiApplication::screenAt(globalPos);
      if (!screen) screen = originWindow->screen();
      const QRect avail = screen ? screen->availableGeometry() : QRect(0, 0, 1920, 1080);
      QSize restoredSize;
      const QVariant customRestored = originWindow->property("daliniraRestoredSize");
      if (customRestored.isValid() && customRestored.toSize().isValid() &&
          customRestored.toSize().width() >= 600 && customRestored.toSize().height() >= 400) {
        restoredSize = customRestored.toSize();
      } else {
        const QRect norm = originWindow->normalGeometry();
        if (norm.isValid() && norm.width() >= 600 && norm.height() >= 400 &&
            norm.width() < avail.width() && norm.height() < avail.height()) {
          restoredSize = norm.size();
        } else {
          const int w = std::clamp(static_cast<int>(avail.width() * 0.72), 800, 1200);
          const int h = std::clamp(static_cast<int>(avail.height() * 0.72), 560, 800);
          restoredSize = QSize(w, h);
        }
      }
      const QSize maxSz = originWindow->size();
      restoredSize.setWidth(std::clamp(std::max(maxSz.width() / 2, restoredSize.width()), 600, avail.width() - 40));
      restoredSize.setHeight(std::clamp(std::max(maxSz.height() / 2, restoredSize.height()), 400, avail.height() - 40));

      originWindow->setWindowState(Qt::WindowNoState);
      originWindow->resize(restoredSize);
      originWindow->ensurePolished();
      if (originWindow->layout()) originWindow->layout()->activate();

      if (originStrip) {
        originStrip->ensurePolished();
        const QRect tab0Rect = originStrip->tabRect(0);
        const QPoint tab0TopLeft = originStrip->mapTo(originWindow, tab0Rect.topLeft());
        const QPoint newHeld = tab0TopLeft + session_.pressOffsetInTab();
        session_.setHeldPointInWindow(newHeld);
        const QPoint newTopLeft = globalPos - newHeld;
        originWindow->move(newTopLeft);
      }
    }

    session_.setDetached(true);
    session_.setState(DragState::DraggingDetachedWindow);
    emit stateChanged(session_.state());

    updateDetachedDrag(globalPos);
    return;
  }

  // Create new detached window (shell + content transfer)
  session_.setState(DragState::CreatingDetachedWindow);
  emit stateChanged(session_.state());

  QWidget *detachedWindow = detachedWindowController_.createDetachedWindow(
      originWindow, session_.tabItem().tabId,
      globalPos, session_.heldPointInWindow(), session_.pressOffsetInTab());

  if (detachedWindow) {
    session_.setHeldPointInWindow(detachedWindowController_.heldPointInWindow());
    session_.setCurrentWindow(detachedWindow);
    auto *destStrip = detachedWindow->findChild<TabStripWidget *>();
    if (destStrip) {
      destStrip->controllerBeginDrag(
          0, globalPos, session_.pressOffsetInTab(),
          detachedWindowController_.initialTabX());
      session_.setCurrentStrip(destStrip);
    } else {
      session_.setCurrentStrip(nullptr);
    }
    session_.setCurrentTabIndex(0);
    session_.setDetached(true);
    session_.setState(DragState::DraggingDetachedWindow);
    emit stateChanged(session_.state());

    if (originStrip) {
      originStrip->controllerFinishDrag(originIndex, true);
    }

    handoffPointerCapture(detachedWindow);
    Q_ASSERT(session_.sessionId() == initialSessionId_);
  } else {
    // Transfer failed — stay in strip
    if (isDiagnosticsEnabled()) {
      std::fprintf(stderr, "[DRAG] Detach failed (factory/transfer returned null) — staying in strip\n");
    }
    session_.setMovingLastTab(false);
    session_.setState(fallbackState);
    emit stateChanged(session_.state());
    cleanUpCursorState();
  }
}

void TabDragController::updateDetachedDrag(const QPoint &globalPos) {
  if (isDiagnosticsEnabled()) {
    std::fprintf(stderr, "[DETACHED_DRAG_MOVE] sessionId=%llu pos=(%d,%d)\n",
                 static_cast<unsigned long long>(session_.sessionId()),
                 globalPos.x(), globalPos.y());
  }

  if (session_.isMovingLastTab()) {
    // The current owner window itself is the drag window — move it directly.
    QWidget *currentWindow = session_.currentWindow();
    if (currentWindow) {
      const QPoint targetTopLeft = globalPos - session_.heldPointInWindow();
      platformBackend_->moveWindow(currentWindow, targetTopLeft);
      platformBackend_->bringToFront(currentWindow);
    }
  } else {
    QWidget *currentWindow = session_.currentWindow();
    if (currentWindow) {
      detachedWindowController_.moveDetachedWindow(globalPos, session_.heldPointInWindow());
    }
  }

  session_.setState(DragState::SearchingAttachTarget);

  // Search for candidate target window
  const auto target = TabWindowRegistry::instance().findTargetAt(
      globalPos, session_.currentWindow());
  if (target.window && target.tabStrip) {
    if (isDiagnosticsEnabled()) {
      std::fprintf(stderr, "[TARGET_FOUND] sessionId=%llu window=%p strip=%p\n",
                   static_cast<unsigned long long>(session_.sessionId()),
                   static_cast<void *>(target.window.data()),
                   static_cast<void *>(target.tabStrip.data()));
    }
    attachToTarget(target.window.data(), target.tabStrip.data(), globalPos);
  }
}

void TabDragController::attachToTarget(QWidget *targetWindow, TabStripWidget *targetStrip, const QPoint &globalPos) {
  if (!targetWindow || !targetStrip) return;

  attachToTargetAt(targetWindow, targetStrip,
                   targetStrip->mapFromGlobal(globalPos), globalPos);
}

void TabDragController::attachToTargetAt(
    QWidget *targetWindow,
    TabStripWidget *targetStrip,
    const QPoint &localPos,
    const QPoint &globalPos) {
  if (!targetWindow || !targetStrip) return;

  if (isDiagnosticsEnabled()) {
    std::fprintf(stderr, "[DRAG] Attaching to target window\n");
  }

  session_.setState(DragState::AttachingToTarget);
  emit stateChanged(session_.state());

  // Transfer tab content to target window
  QVector<TabModelItem> items;
  for (int i = 0; i < targetStrip->count(); ++i) {
    items.append(TabModelItem{targetStrip->tabId(i), targetStrip->tabText(i), targetStrip->tabIcon(i), targetStrip->isTabPinned(i), targetStrip->isTabAudible(i), targetStrip->isTabLoading(i), targetStrip->isTabSleeping(i), targetStrip->tabData(i)});
  }
  const TabLayoutModel &targetLayoutModel = targetStrip->layoutModel();
  const int draggedWidth = session_.tabItem().isPinned
      ? targetLayoutModel.metrics().pinnedTabWidth
      : targetLayoutModel.metrics().preferredTabWidth;
  const QPoint draggedCenter(
      localPos.x() + draggedWidth / 2 - session_.pressOffsetInTab().x(),
      localPos.y());
  const int targetIndex = targetLayoutModel.computeExternalInsertionIndex(
      targetStrip->width(), items, session_.tabItem().isPinned, draggedCenter);

  bool transferred = false;
  QWidget *const oldWindow = session_.currentWindow();
  const bool oldWindowWasCaptureShell =
      oldWindow && oldWindow->property("daliniraDragCaptureShell").toBool();
  if (transferDelegate_) {
    transferred = transferDelegate_(oldWindow, targetWindow, session_.tabItem().tabId, targetIndex);
  }

  if (transferred) {
    // If we had created a temporary detached window, finalize it
    if (session_.isDetached() && oldWindowWasCaptureShell) {
      if (oldWindow) {
        oldWindow->unsetCursor();
      }
      detachedWindowController_.finalizeDetachedWindow();
      if (oldWindow && oldWindow != session_.sourceWindow()) {
        oldWindow->close();
      }
    }

    if (targetStrip) {
      targetStrip->unsetCursor();
    }
    if (targetWindow) {
      targetWindow->unsetCursor();
    }
    while (QGuiApplication::overrideCursor()) {
      QGuiApplication::restoreOverrideCursor();
    }

    session_.setCurrentWindow(targetWindow);
    session_.setCurrentStrip(targetStrip);
    session_.setHeldPointInWindow(targetStrip->mapTo(targetWindow, localPos));
    const int clampedIdx = std::clamp(targetIndex, 0, targetStrip->count() - 1);
    session_.setCurrentTabIndex(clampedIdx);
    session_.setCurrentInsertionSlot(clampedIdx);
    session_.setDetached(false);
    session_.setMovingLastTab(false);
    session_.markAttached(globalPos);

    // Update the session tabId to the widget pointer at the new position so
    // a subsequent re-detach can call findTabUidByDragId correctly.
    if (clampedIdx >= 0 && clampedIdx < targetStrip->count()) {
      auto updatedItem = session_.tabItem();
      updatedItem.tabId = targetStrip->tabId(clampedIdx);
      session_.setTabItem(updatedItem);
    }

    session_.setState(DragState::DraggingInTargetStrip);
    emit stateChanged(session_.state());

    if (isDiagnosticsEnabled()) {
      std::fprintf(stderr,
                   "[LIVE_ATTACH] sessionId=%llu window=%p strip=%p targetIndex=%d\n",
                   static_cast<unsigned long long>(session_.sessionId()),
                   static_cast<void *>(targetWindow),
                   static_cast<void *>(targetStrip),
                   clampedIdx);
    }

    targetStrip->controllerBeginDrag(
        session_.currentTabIndex(), globalPos, session_.pressOffsetInTab());
    handoffPointerCapture(targetStrip);
    Q_ASSERT(session_.sessionId() == initialSessionId_);
  } else {
    session_.setState(DragState::DraggingDetachedWindow);
    emit stateChanged(session_.state());
  }
}

void TabDragController::performRedetach(const QPoint &globalPos) {
  Q_ASSERT(session_.sessionId() == initialSessionId_);
  if (isDiagnosticsEnabled()) {
    std::fprintf(stderr, "[REDETACH] sessionId=%llu pos=(%d,%d)\n",
                 static_cast<unsigned long long>(session_.sessionId()),
                 globalPos.x(), globalPos.y());
  }

  session_.setState(DragState::Redetaching);
  emit stateChanged(session_.state());
  performDetach(globalPos);
  Q_ASSERT(session_.sessionId() == initialSessionId_);
}

void TabDragController::completeDrag(const QPoint &globalPos) {
  Q_ASSERT(session_.sessionId() == initialSessionId_);
  const DragState endState = session_.state();

  if (isDiagnosticsEnabled()) {
    std::fprintf(stderr, "[COMPLETE] sessionId=%llu pos=(%d,%d) state=%s\n",
                 static_cast<unsigned long long>(session_.sessionId()),
                 globalPos.x(), globalPos.y(), dragStateToString(endState));
  }

  if (endState == DragState::DraggingInStrip || endState == DragState::DraggingInTargetStrip) {
    auto *strip = session_.currentStrip();
    auto *window = session_.currentWindow();
    const int fromIndex = session_.currentTabIndex();
    const int toIndex = session_.currentInsertionSlot();

    if (strip) {
      session_.setState(DragState::Settling);
      emit stateChanged(session_.state());

      if (moveDelegate_ && window && fromIndex >= 0 && toIndex >= 0 && fromIndex != toIndex) {
        moveDelegate_(window, fromIndex, toIndex);
        session_.setCurrentTabIndex(toIndex);
      }
      strip->controllerFinishDrag(toIndex, false);
    }
  } else if (session_.isMovingLastTab() && session_.sourceStrip()) {
    session_.sourceStrip()->controllerFinishDrag(session_.currentTabIndex(), false);
  } else if (endState == DragState::DraggingDetachedWindow ||
             endState == DragState::SearchingAttachTarget) {
    if (session_.currentStrip()) {
      session_.currentStrip()->controllerFinishDrag(0, false);
    }
    detachedWindowController_.finalizeDetachedWindow();
  }

  cleanUpCursorState();
  endPointerCapture();
  session_.reset();
  initialSessionId_ = 0;
  initialTabId_ = 0;
  emit dragFinished(EndDragReason::Completed);
  emit stateChanged(DragState::Idle);
}

void TabDragController::cancelDrag() {
  if (isDiagnosticsEnabled()) {
    std::fprintf(stderr, "[DRAG] Cancelled drag sessionId=%llu\n",
                 static_cast<unsigned long long>(session_.sessionId()));
  }

  auto *currentStrip = session_.currentStrip();
  QWidget *const currentWindow = session_.currentWindow();
  QWidget *const sourceWindow = session_.sourceWindow();
  const int currentIndex = session_.currentTabIndex();
  if (currentStrip) {
    currentStrip->controllerFinishDrag(currentIndex, true);
  }

  bool restored = true;
  if (currentWindow && sourceWindow && currentWindow != sourceWindow) {
    restored = transferDelegate_ && transferDelegate_(
        currentWindow, sourceWindow, session_.tabItem().tabId,
        session_.sourceTabIndex());
    if (restored && currentWindow->property("daliniraDragCaptureShell").toBool()) {
      detachedWindowController_.finalizeDetachedWindow();
      currentWindow->close();
    }
  } else if (currentWindow == sourceWindow && currentStrip &&
             currentIndex >= 0 && session_.sourceTabIndex() >= 0 &&
             currentIndex != session_.sourceTabIndex() && moveDelegate_) {
    moveDelegate_(sourceWindow, currentIndex, session_.sourceTabIndex());
  }

  if (sourceWindow && (session_.sourceWasLastTab() || restored)) {
    if (session_.sourceWasLastTab()) {
      platformBackend_->moveWindow(sourceWindow, session_.sourceWindowTopLeft());
    }
    if (!sourceWindow->isVisible()) sourceWindow->show();
    platformBackend_->bringToFront(sourceWindow);
  }

  cleanUpCursorState();
  endPointerCapture();
  session_.reset();
  initialSessionId_ = 0;
  initialTabId_ = 0;
  emit dragFinished(EndDragReason::Cancelled);
  emit stateChanged(DragState::Idle);
}

void TabDragController::cleanUpCursorState() {
  while (QGuiApplication::overrideCursor()) {
    QGuiApplication::restoreOverrideCursor();
  }

  if (auto *strip = session_.currentStrip()) {
    strip->unsetCursor();
  }
  if (auto *sourceStrip = session_.sourceStrip()) {
    sourceStrip->unsetCursor();
  }
  if (auto *win = session_.currentWindow()) {
    win->unsetCursor();
  }
  if (auto *sourceWin = session_.sourceWindow()) {
    sourceWin->unsetCursor();
  }
  if (pointerCaptureOwner_) {
    pointerCaptureOwner_->unsetCursor();
  }
}

}  // namespace dalinira::desktop_tabs
