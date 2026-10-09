#ifndef DALINIRA_DESKTOP_TABS_TAB_STRIP_WIDGET_H_
#define DALINIRA_DESKTOP_TABS_TAB_STRIP_WIDGET_H_

#include <QIcon>
#include <QElapsedTimer>
#include <QHash>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>
#include <QTimer>
#include <QVector>
#include <QWidget>
#include <optional>

#include "tab_drag_types.h"
#include "tab_layout_model.h"
#include "tab_strip_animator.h"
#include "tab_group_model.h"

namespace dalinira::desktop_tabs {

class TabStripWidget : public QWidget {
  Q_OBJECT
 public:
  explicit TabStripWidget(QWidget *parent = nullptr);
  ~TabStripWidget() override;

  int count() const { return items_.size(); }
  int currentIndex() const { return activeIndex_; }
  void setCurrentIndex(int index);

  int addTab(uint64_t tabId, const QString &title, const QIcon &icon = QIcon(), bool pinned = false);
  int addTab(const QString &title);
  int addTab(const QIcon &icon, const QString &title);

  void insertTab(int index, uint64_t tabId, const QString &title, const QIcon &icon = QIcon(), bool pinned = false);
  int insertTab(int index, const QString &title);
  int insertTab(int index, const QIcon &icon, const QString &title);

  void removeTab(int index);
  void moveTab(int fromIndex, int toIndex);

  void setTabTitle(int index, const QString &title);
  void setTabText(int index, const QString &title) { setTabTitle(index, title); }
  void setTabIcon(int index, const QIcon &icon);
  void setTabPinned(int index, bool pinned);
  void setTabAudible(int index, bool audible);
  // A sleeping tab (memory saver) is drawn with a faded favicon, a moon badge
  // and a muted title; activating it wakes the page.
  void setTabSleeping(int index, bool sleeping);
  bool isTabSleeping(int index) const;
  void setTabLoading(int index, bool loading);
  void setTabData(int index, const QVariant &data);

  QString tabText(int index) const;
  QIcon tabIcon(int index) const;
  bool isTabPinned(int index) const;
  bool isTabAudible(int index) const;
  bool isTabLoading(int index) const;
  uint64_t tabId(int index) const;
  QVariant tabData(int index) const;

  int tabAt(const QPoint &pos) const;
  void cancelHover();

  TabStyle tabStyle() const { return tabStyle_; }
  void setTabStyle(TabStyle style);
  void loadSettings();

  QRect tabRect(int index) const;
  QRect detachBoundary() const;
  QRect attachBoundary() const;
  int contentWidth() const;
  int visualTabsRight() const;
  const TabLayoutModel &layoutModel() const { return layoutModel_; }

  // Drag controller interface
  void controllerBeginDrag(
      int index,
      const QPoint &globalCursor,
      const QPoint &pressOffsetInTab,
      std::optional<int> initialTabX = std::nullopt);
  void controllerUpdateDrag(const QPoint &globalCursor, int insertionSlot);
  void controllerFinishDrag(int finalIndex, bool cancelled);
  void setTabVisualSuppressed(int index, bool suppressed);

  // Tab Groups
  void setGroupModel(TabGroupModel *model);
  TabGroupModel *groupModel() const { return groupModel_; }
  QRect groupChipRect(const QUuid &groupId) const;

  QSize sizeHint() const override;
  QSize minimumSizeHint() const override;

 signals:
  void currentChanged(int index);
  void tabCloseRequested(int index);
  void newTabRequested();
  void tabMoved(int fromIndex, int toIndex);
  void tabHovered(int index, QPoint globalPos, QRect globalTabRect);
  void tabHoverLeave();
  void dragInitiated(int index, QPoint globalPos, QPoint offsetInTab, QSize tabSize);
  void tabContextMenuRequested(int index, QPoint globalPos);
  void groupChipClicked(const QUuid &groupId, const QPoint &globalPosBelowChip);

 protected:
  void paintEvent(QPaintEvent *event) override;
  void enterEvent(QEnterEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;
  void mouseDoubleClickEvent(QMouseEvent *event) override;
  void wheelEvent(QWheelEvent *event) override;
  void leaveEvent(QEvent *event) override;
  void resizeEvent(QResizeEvent *event) override;
  void keyPressEvent(QKeyEvent *event) override;
  void contextMenuEvent(QContextMenuEvent *event) override;

 private slots:
  void onAnimationTick();
  void onHoverAnimationTick();
  void onHoverTimeout();

 private:
  QVector<TabModelItem> items_;
  int activeIndex_ = -1;
  int hoveredIndex_ = -1;
  int pressedIndex_ = -1;
  int hoveredCloseIndex_ = -1;
  bool hoveredNewTabButton_ = false;

  int draggedIndex_ = -1;
  int insertionSlot_ = -1;
  int suppressedVisualTab_ = -1;
  QPoint pressGlobalPos_;
  QPoint pressLocalPos_;
  std::optional<QPoint> pendingWindowMovePress_;
  std::optional<QPoint> manualWindowMoveOffset_;
  QPoint dragOffsetInTab_;
  std::optional<QPoint> draggedTopLeft_;

  TabStyle tabStyle_ = TabStyle::DaliNiraConnected;
  TabLayoutModel layoutModel_;
  TabStripAnimator animator_;

  QTimer hoverTimer_;
  QTimer hoverAnimationTimer_;
  QElapsedTimer hoverAnimationClock_;
  QHash<uint64_t, qreal> hoverOpacities_;
  bool cardActive_ = false;

  struct ClosingGhostTab {
    quint64 transitionId = 0;
    QString title;
    QIcon icon;
    bool isPinned = false;
    uint64_t tabId = 0;
  };
  QHash<quint64, ClosingGhostTab> closingGhosts_;

  TabGroupModel *groupModel_ = nullptr;
  mutable QMap<QUuid, QRect> groupChipRects_;
  void updateGroupChipRects(const QVector<TabGeometry> &geoms) const;

  void recomputeLayout(bool animate = true, int durationMs = 150);
  QVector<TabGeometry> currentGeometries() const;
  void setHoveredIndex(int index);
  qreal hoverOpacity(uint64_t tabId) const;
};

}  // namespace dalinira::desktop_tabs

#endif  // DALINIRA_DESKTOP_TABS_TAB_STRIP_WIDGET_H_
