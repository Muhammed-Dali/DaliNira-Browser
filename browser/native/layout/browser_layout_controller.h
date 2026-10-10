#pragma once

#include <QObject>
#include <QWidget>
#include <QBoxLayout>
#include <QPointer>
#include <QRect>
#include <QTimer>
#include "browser_layout.h"

class QPropertyAnimation;

namespace dalinira::layout {

// Where the side tab panel is, as one explicit state (Zen's compact mode,
// adapted to DaliNira's docked and floating layouts):
//   None      classic layout, horizontal tab strip, no panel
//   Docked    in the body layout beside the page (expanded): pinned (auto-hide
//             off), or while the New Tab page is active
//   Collapsed hidden; the page has the full body width
//   Revealed  over the page (overlay), after the pointer reached the window
//             edge; also kept while a tab is dragged or a panel menu is open
// Holds that keep it visible (New Tab, pinned, dragging, menus) are inputs to
// one evaluation, never separate visibility code paths.
enum class SidePanelState { None, Docked, Collapsed, Revealed };

QString sidePanelStateName(SidePanelState state);

// Auto-hide preference (profile setting, shared by every window and both
// engines): on = the panel hides on websites and is revealed from the window
// edge; off = pinned open. The panel's pin toggle, Settings and the shortcut
// all set it. While unset, the floating layout auto-hides and the docked
// left/right layouts stay pinned (their behavior before the setting existed).
bool sidePanelAutoHideEnabled(BrowserLayoutMode mode);
void setSidePanelAutoHideEnabled(bool enabled);

class BrowserLayoutController : public QObject {
  Q_OBJECT
 public:
  // Pointer-leave grace before the panel hides (no flicker when the pointer
  // grazes its border), and the edge band that reveals it.
  static constexpr int kHideDelayMs = 300;
  static constexpr int kRevealEdgePx = 6;
  static constexpr int kLeaveSlackPx = 32;
  static constexpr int kSlideMs = 150;

  BrowserLayoutController(QWidget *window,
                          QWidget *bodyContainer,
                          QBoxLayout *bodyLayout,
                          QWidget *sidePanel,
                          QWidget *pageStack,
                          QWidget *topBar = nullptr,
                          QWidget *tabStrip = nullptr,
                          QWidget *navBar = nullptr,
                          QObject *parent = nullptr);
  ~BrowserLayoutController() override = default;

  BrowserLayoutMode currentMode() const { return mode_; }
  void setLayoutMode(BrowserLayoutMode mode, bool animated = true);

  bool isSidePanelVisible() const;
  bool isSidePanelOnLeft() const;
  bool isSidePanelOnRight() const;
  bool isHorizontalTabsVisible() const;
  bool isVerticalTabsEnabled() const;
  bool isBottomTabsEnabled() const;
  bool isFloatingPanelEnabled() const;
  bool preservesPageWidget(QWidget *page) const;

  SidePanelState sidePanelState() const { return state_; }
  // Auto-hide is in effect: a side layout (left, right, floating) that is
  // not pinned.
  bool isAutoHideActive() const;
  bool isPinned() const { return mode_ != BrowserLayoutMode::Classic && !isAutoHideActive(); }
  bool panelOnLeft() const { return mode_ != BrowserLayoutMode::VerticalTabsRight; }

  // Inputs from the window (shared tab/page state; the engine never matters).
  void setAutoHideEnabled(bool enabled);
  // The active tab is the New Tab page; `loading`: the active tab is loading.
  // Leaving New Tab keeps the panel docked until that navigation completes.
  void setNewTabActive(bool newTab, bool loading);
  void setFullScreen(bool fullScreen);
  void setTabDragActive(bool dragging);

  // Pointer position (global): the edge band reveals the panel; leaving it
  // hides it after kHideDelayMs, or at once beyond kLeaveSlackPx. Event-driven
  // only: Qt mouse moves (application filter) and, over a native WebKit page
  // that hides them from Qt, EngineView::pagePointerMoved from GTK. No polling.
  void updatePanelForPointer(const QPoint &globalPos);
  void updateFloatingPanelForPointer(const QPoint &globalPos) { updatePanelForPointer(globalPos); }

  // Extensibility points for current and future layout styles
  virtual void applyClassicLayout(bool animated);
  virtual void applyLeftPanelLayout(bool animated);
  virtual void applyRightPanelLayout(bool animated);
  virtual void applyVerticalTabsLayout(bool animated);
  virtual void applyFloatingPanelLayout(bool animated);

 protected:
  bool eventFilter(QObject *watched, QEvent *event) override;

 signals:
  void layoutModeChanged(BrowserLayoutMode mode);
  void sidePanelStateChanged(SidePanelState state);
  // The panel's pin state (auto-hide off) for this layout changed.
  void pinnedChanged(bool pinned);

 private:
  SidePanelState targetState() const;
  void evaluate();
  void applyState(SidePanelState state);
  void dock();
  void undock();
  QRect overlayRect(bool shown) const;
  void slideTo(const QRect &target, bool hideAtEnd);
  void stopSlide();
  void hidePointerRevealed();
  bool pointerOverPanel(const QPoint &globalPos) const;

  QWidget *window_ = nullptr;
  QWidget *bodyContainer_ = nullptr;
  QBoxLayout *bodyLayout_ = nullptr;
  QWidget *sidePanel_ = nullptr;
  QWidget *pageStack_ = nullptr;
  QWidget *topBar_ = nullptr;
  QWidget *tabStrip_ = nullptr;
  QWidget *navBar_ = nullptr;
  BrowserLayoutMode mode_{BrowserLayoutMode::Classic};
  SidePanelState state_{SidePanelState::None};
  bool animationsEnabled_ = true;
  int autoHideSetting_ = -1;  // -1 unset (per-layout default), 0 off, 1 on
  bool newTabHold_ = false;
  bool fullScreen_ = false;
  bool dragging_ = false;
  bool pointerRevealed_ = false;
  bool docked_ = false;
  int dockedWidth_ = 224;
  QPointer<QPropertyAnimation> slide_;
  QTimer *hideTimer_ = nullptr;
};

} // namespace dalinira::layout
