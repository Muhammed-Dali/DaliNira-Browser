#ifndef DALINIRA_DESKTOP_TABS_TAB_LAYOUT_MODEL_H_
#define DALINIRA_DESKTOP_TABS_TAB_LAYOUT_MODEL_H_

#include <QFont>
#include <QFontMetrics>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QVariant>
#include <QVector>
#include <optional>

#include "tab_drag_types.h"

namespace dalinira::desktop_tabs {

struct TabModelItem {
  uint64_t tabId = 0;
  QString title;
  QIcon icon;
  bool isPinned = false;
  bool isAudible = false;
  bool isLoading = false;
  bool isSleeping = false;
  QVariant userData;
};

class TabLayoutModel {
 public:
  explicit TabLayoutModel(LayoutMetrics metrics = {});

  const LayoutMetrics &metrics() const { return metrics_; }
  void setMetrics(const LayoutMetrics &metrics);

  // Calculates the ideal (logical) bounds for every tab given the total strip width,
  // active items, dragged tab index and insertion slot.
  QVector<TabGeometry> computeLayout(
      int availableWidth,
      const QVector<TabModelItem> &items,
      int activeIndex,
      int draggedIndex = -1,
      int insertionSlot = -1,
      std::optional<QPoint> draggedTabTopLeft = std::nullopt,
      const QFont &font = QFont()) const;

  // Calculates unconstrained content width (sum of preferred tab widths)
  int computePreferredTotalWidth(const QVector<TabModelItem> &items) const;

  // Calculates ideal tab width for unpinned tabs
  int computeUnpinnedTabWidth(int availableWidth, int pinnedCount, int unpinnedCount) const;

  // Hit-test to find tab index at point
  int hitTestTabIndex(const QVector<TabGeometry> &geometries, const QPoint &point) const;

  // Hit-test close button of a tab
  bool hitTestCloseButton(const TabGeometry &geometry, const QPoint &point) const;

  // Calculates the insertion slot index (0..count-1) for a dragged tab at local coordinate
  int computeInsertionSlot(
      int availableWidth,
      const QVector<TabModelItem> &items,
      int draggedIndex,
      const QPoint &draggedLocalPoint) const;

  // Calculates an insertion index (0..count) for a tab arriving from another
  // strip. Unlike computeInsertionSlot(), this can return the append position.
  int computeExternalInsertionIndex(
      int availableWidth,
      const QVector<TabModelItem> &items,
      bool draggedTabPinned,
      const QPoint &draggedLocalPoint) const;

  // Recomputes icon/text/close hit bounds after animation changes visualRect.
  void applyVisualRect(TabGeometry &geometry, const QRect &visualRect,
                       const QFont &font = QFont()) const;

  // Returns the expanded strip bounding rect for detach threshold testing
  QRect computeDetachBoundary(const QSize &stripSize) const;

  // Returns the deliberately narrow strip-entry area used while a detached
  // tab is looking for a new owner. This must not reuse the wider detach
  // boundary or the tab can attach before it has actually reached the strip.
  QRect computeAttachBoundary(const QSize &stripSize) const;

  // Returns true if the pointer has broken through the magnetic detach boundary
  bool isOutsideDetachBoundary(const QPoint &localPoint, const QSize &stripSize) const;

  // Returns the bounding rect for the New Tab (+) button placed after the tabs
  QRect computeNewTabButtonRect(int visualTabsRight, const QSize &stripSize) const;

 private:
  LayoutMetrics metrics_;

  void computeSubRects(TabGeometry &geom, const QFontMetrics &fm) const;
};

}  // namespace dalinira::desktop_tabs

#endif  // DALINIRA_DESKTOP_TABS_TAB_LAYOUT_MODEL_H_
