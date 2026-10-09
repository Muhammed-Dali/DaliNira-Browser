#include "desktop_tabs/tab_drag_controller.h"
#include "desktop_tabs/tab_drag_session.h"
#include "desktop_tabs/tab_drag_types.h"
#include "desktop_tabs/tab_layout_model.h"
#include "desktop_tabs/tab_strip_widget.h"
#include "desktop_tabs/tab_window_registry.h"
#include "desktop_tabs/window_drag_geometry.h"

#include <QApplication>
#include <QPointer>
#include <QVector>
#include <QWidget>
#include <algorithm>
#include <cassert>
#include <iostream>

using namespace dalinira::desktop_tabs;

static int indexOfTabId(TabStripWidget *strip, uint64_t tabId) {
  if (!strip) return -1;
  for (int index = 0; index < strip->count(); ++index) {
    if (strip->tabId(index) == tabId) return index;
  }
  return -1;
}

int main(int argc, char *argv[]) {
  if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM") &&
      qEnvironmentVariableIsSet("DISPLAY") &&
      qgetenv("XDG_SESSION_TYPE") == "wayland") {
    qputenv("QT_QPA_PLATFORM", "xcb");
  }
  QApplication app(argc, argv);

  std::cout << "[TEST] Running dalinira::desktop_tabs::TabDragController unit test...\n";

  const QRect restored = restoredWindowGeometryForDrag(
      QRect(0, 24, 1920, 1056), QSize(1200, 800), QPoint(960, 40), QPoint(1020, 110));
  assert(restored.size() == QSize(1200, 800));
  assert(restored.topLeft() == QPoint(420, 94));
  assert(QPoint(1020, 110) - restored.topLeft() == QPoint(600, 16));

  auto &controller = TabDragController::instance();
  assert(!controller.isActive());
  assert(controller.state() == DragState::Idle);

  // Create test widgets
  QWidget window1;
  window1.setGeometry(100, 100, 800, 600);
  window1.show();

  auto *strip1 = new TabStripWidget(&window1);
  strip1->setGeometry(0, 0, 800, 40);
  strip1->insertTab(0, QStringLiteral("Tab 1"));
  strip1->insertTab(1, QStringLiteral("Tab 2"));
  strip1->insertTab(2, QStringLiteral("Tab 3"));

  TabWindowRegistry::instance().registerWindow(&window1, strip1);
  assert(!TabWindowRegistry::instance().registeredWindows().isEmpty());

  // Test drag initiation
  bool moveDelegateCalled = false;
  int fromSlot = -1, toSlot = -1;
  controller.setTabMoveDelegate([&](QWidget *, int from, int to) {
    moveDelegateCalled = true;
    fromSlot = from;
    toSlot = to;
  });

  const QPoint pressPos(50, 20);
  const QPoint pressGlobal = strip1->mapToGlobal(pressPos);
  controller.handleMousePress(&window1, strip1, 0, pressGlobal, QPoint(10, 10), QPoint(10, 10));

  assert(controller.isActive());
  assert(controller.session().sourceTabIndex() == 0);
  assert(controller.session().sourceWindow() == &window1);

  // In-strip drag move
  const QPoint moveGlobal = strip1->mapToGlobal(QPoint(300, 20));
  controller.handleMouseMove(moveGlobal);

  assert(controller.session().state() == DragState::DraggingInStrip);

  // Mouse release within strip
  controller.handleMouseRelease(moveGlobal);

  assert(!controller.isActive());
  assert(controller.state() == DragState::Idle);
  assert(moveDelegateCalled);
  assert(fromSlot == 0);
  assert(toSlot >= 1);

  // Test Cancel
  controller.handleMousePress(&window1, strip1, 1, pressGlobal, QPoint(10, 10), QPoint(10, 10));
  assert(controller.isActive());
  controller.handleCancel();
  assert(!controller.isActive());
  assert(controller.state() == DragState::Idle);

  TabWindowRegistry::instance().unregisterWindow(&window1);

  // Regression A-F: exercise one complete physical-gesture state machine
  // against real widget geometry. The previous tests jumped directly between
  // controller methods without transferring the tab model, so an attach that
  // immediately re-detached could still report green.
  TabWindowRegistry::instance().clear();
  QWidget sourceWindow;
  sourceWindow.setGeometry(100, 100, 900, 600);
  sourceWindow.setProperty("daliniraTabProfile", 42);
  sourceWindow.setProperty("daliniraTabWindowType", QStringLiteral("regular"));
  auto *sourceStrip = new TabStripWidget(&sourceWindow);
  sourceStrip->setGeometry(0, 0, 900, 34);
  sourceStrip->addTab(101, QStringLiteral("Left"));
  sourceStrip->addTab(102, QStringLiteral("Middle"));
  sourceStrip->addTab(103, QStringLiteral("Right"));
  sourceWindow.show();
  QApplication::processEvents();
  TabWindowRegistry::instance().registerWindow(&sourceWindow, sourceStrip);

  QVector<QWidget *> shells;
  int detachedWindowCount = 0;
  controller.setDetachedWindowFactory(
      [&](QWidget *, uint64_t) -> QWidget * {
        auto *shell = new QWidget;
        shell->setGeometry(0, 0, 900, 600);
        shell->setProperty("daliniraTabProfile", 42);
        shell->setProperty("daliniraTabWindowType", QStringLiteral("regular"));
        shell->setProperty("daliniraDragCaptureShell", true);
        auto *strip = new TabStripWidget(shell);
        strip->setGeometry(0, 0, 900, 34);
        TabWindowRegistry::instance().registerWindow(shell, strip);
        shells.push_back(shell);
        ++detachedWindowCount;
        return shell;
      });

  const auto transfer = [](QWidget *fromWindow, QWidget *toWindow,
                           uint64_t tabId, int targetIndex) {
    auto *fromStrip = fromWindow ? fromWindow->findChild<TabStripWidget *>() : nullptr;
    auto *toStrip = toWindow ? toWindow->findChild<TabStripWidget *>() : nullptr;
    const int fromIndex = indexOfTabId(fromStrip, tabId);
    if (!fromStrip || !toStrip || fromIndex < 0) return false;
    const QString title = fromStrip->tabText(fromIndex);
    const QIcon icon = fromStrip->tabIcon(fromIndex);
    const bool pinned = fromStrip->isTabPinned(fromIndex);
    const bool audible = fromStrip->isTabAudible(fromIndex);
    const bool loading = fromStrip->isTabLoading(fromIndex);
    const QVariant data = fromStrip->tabData(fromIndex);
    fromStrip->removeTab(fromIndex);
    const int inserted = std::clamp(targetIndex, 0, toStrip->count());
    toStrip->insertTab(inserted, tabId, title, icon, pinned);
    toStrip->setTabAudible(inserted, audible);
    toStrip->setTabLoading(inserted, loading);
    toStrip->setTabData(inserted, data);
    return true;
  };
  controller.setDetachTransferDelegate(
      [&](QWidget *destination, QWidget *origin, uint64_t tabId, int index) {
        return transfer(origin, destination, tabId, index);
      });
  controller.setTabTransferDelegate(transfer);
  controller.setTabMoveDelegate(
      [](QWidget *window, int from, int to) {
        if (auto *strip = window ? window->findChild<TabStripWidget *>() : nullptr)
          strip->moveTab(from, to);
      });

  const QRect middleRect = sourceStrip->tabRect(1);
  const QPoint middleOffset(middleRect.width() / 2, middleRect.height() / 2);
  const QPoint middlePress = sourceStrip->mapToGlobal(middleRect.topLeft() + middleOffset);
  controller.handleMousePress(&sourceWindow, sourceStrip, 1, middlePress,
                              middleOffset, sourceWindow.mapFromGlobal(middlePress));

  // A: horizontal movement with a tiny vertical wobble remains attached.
  const QPoint horizontalPoint = middlePress + QPoint(100, 2);
  controller.handleMouseMove(horizontalPoint);
  assert(controller.state() == DragState::DraggingInStrip);
  assert(detachedWindowCount == 0);
  assert(sourceStrip->count() == 3);

  // B: crossing the named vertical boundary creates exactly one shell and
  // keeps the same held tab pixel beneath the cursor.
  const int detachY = sourceStrip->height()
      + sourceStrip->layoutModel().metrics().verticalDetachThreshold + 2;
  const QPoint detachPoint = sourceStrip->mapToGlobal(
      QPoint(sourceStrip->mapFromGlobal(horizontalPoint).x(), detachY));
  controller.handleMouseMove(detachPoint);
  assert(detachedWindowCount == 1);
  assert(controller.state() == DragState::DraggingDetachedWindow);
  assert(controller.session().currentWindow() != &sourceWindow);
  assert(sourceStrip->count() == 2);
  assert((controller.session().currentWindow()->pos()
          + controller.session().heldPointInWindow() - detachPoint)
             .manhattanLength() <= 2);

  // C/E/F: enter through the attach halo. The event that attaches is consumed
  // and cannot also redetach; all coordinates describe the same point.
  const int haloY = -sourceStrip->layoutModel().metrics().attachMagnetism + 1;
  const QPoint attachPoint = sourceStrip->mapToGlobal(QPoint(520, haloY));
  assert(sourceStrip->mapFromGlobal(attachPoint) == QPoint(520, haloY));
  controller.handleMouseMove(attachPoint);
  assert(controller.state() == DragState::DraggingInTargetStrip);
  assert(controller.session().currentWindow() == &sourceWindow);
  assert(controller.session().currentStrip() == sourceStrip);
  assert(controller.session().attachGeneration() == 1);
  assert(controller.session().requiresTargetStripEntry());
  assert(detachedWindowCount == 1);
  assert(sourceStrip->count() == 3);

  // Even a move beyond the detach boundary cannot bounce the tab back out
  // before the pointer has entered the actual target row.
  controller.handleMouseMove(sourceStrip->mapToGlobal(QPoint(522, -50)));
  assert(controller.state() == DragState::DraggingInTargetStrip);
  assert(detachedWindowCount == 1);

  const QPoint insideTarget = sourceStrip->mapToGlobal(QPoint(530, 17));
  controller.handleMouseMove(insideTarget);
  assert(!controller.session().requiresTargetStripEntry());
  for (int dx : {2, 5, 9, 16}) {
    controller.handleMouseMove(insideTarget + QPoint(dx, (dx % 3) - 1));
    assert(controller.state() == DragState::DraggingInTargetStrip);
    assert(detachedWindowCount == 1);
  }

  // D: after genuine strip entry, an intentional vertical pull re-detaches
  // exactly once without requiring a second press.
  const QPoint redetachPoint = sourceStrip->mapToGlobal(
      QPoint(560, sourceStrip->height()
                      + sourceStrip->layoutModel().metrics().verticalDetachThreshold + 2));
  controller.handleMouseMove(redetachPoint);
  assert(detachedWindowCount == 2);
  assert(controller.state() == DragState::DraggingDetachedWindow);
  assert(sourceStrip->count() == 2);

  controller.handleCancel();
  assert(!controller.isActive());
  assert(sourceStrip->count() == 3);
  assert(indexOfTabId(sourceStrip, 102) == 1);

  controller.setDetachedWindowFactory({});
  controller.setDetachTransferDelegate({});
  controller.setTabTransferDelegate({});
  controller.setTabMoveDelegate({});
  TabWindowRegistry::instance().clear();
  for (QWidget *shell : shells) delete shell;

  std::cout << "[TEST] All TabDragController tests passed!\n";
  return 0;
}
