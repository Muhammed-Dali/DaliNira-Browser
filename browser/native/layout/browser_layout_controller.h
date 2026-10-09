#pragma once

#include <QObject>
#include <QWidget>
#include <QBoxLayout>
#include <QPointer>
#include <QTimer>
#include "browser_layout.h"

namespace dalinira::layout {

class BrowserLayoutController : public QObject {
  Q_OBJECT
 public:
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
  // Floating sidebar: shows the panel when the pointer reaches the body's
  // left edge and hides it once the pointer has moved away from it.
  void updateFloatingPanelForPointer(const QPoint &globalPos);

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

 private:
  QWidget *window_ = nullptr;
  QWidget *bodyContainer_ = nullptr;
  QBoxLayout *bodyLayout_ = nullptr;
  QWidget *sidePanel_ = nullptr;
  QWidget *pageStack_ = nullptr;
  QWidget *topBar_ = nullptr;
  QWidget *tabStrip_ = nullptr;
  QWidget *navBar_ = nullptr;
  BrowserLayoutMode mode_{BrowserLayoutMode::Classic};
  bool floatingPanelShown_ = false;
  bool animationsEnabled_ = true;
  // Pointer moves over a native page (WebKit's XEmbed host) never reach Qt,
  // so in floating mode the pointer is also polled.
  QTimer *pointerPoll_ = nullptr;
  void placeDockedPanel(bool left);
  void setFloatingPanelVisible(bool visible, bool animated);
};

} // namespace dalinira::layout
