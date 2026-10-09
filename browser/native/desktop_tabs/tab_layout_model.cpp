#include "tab_layout_model.h"

#include <algorithm>

namespace dalinira::desktop_tabs {

TabLayoutModel::TabLayoutModel(LayoutMetrics metrics)
    : metrics_(metrics) {}

void TabLayoutModel::setMetrics(const LayoutMetrics &metrics) {
  metrics_ = metrics;
}

int TabLayoutModel::computeUnpinnedTabWidth(int availableWidth, int pinnedCount, int unpinnedCount) const {
  if (unpinnedCount <= 0) return metrics_.preferredTabWidth;

  const int reservedForPinned = pinnedCount * metrics_.pinnedTabWidth;
  const int reservedForNewTab = metrics_.newTabButtonWidth +
                                metrics_.newTabButtonGap + 4;
  const int usableWidth = std::max(0, availableWidth - reservedForPinned - reservedForNewTab);

  if (unpinnedCount == 1) {
    return std::clamp(usableWidth, metrics_.minTabWidth, metrics_.preferredTabWidth);
  }

  // Calculate width per unpinned tab
  const int computedWidth = usableWidth / unpinnedCount;
  return std::clamp(computedWidth, metrics_.minTabWidth, metrics_.preferredTabWidth);
}

int TabLayoutModel::computePreferredTotalWidth(const QVector<TabModelItem> &items) const {
  int total = 0;
  for (const auto &item : items) {
    total += item.isPinned ? metrics_.pinnedTabWidth : metrics_.preferredTabWidth;
  }
  total += metrics_.newTabButtonWidth + metrics_.newTabButtonGap + 4;
  return total;
}

QVector<TabGeometry> TabLayoutModel::computeLayout(
    int availableWidth,
    const QVector<TabModelItem> &items,
    int activeIndex,
    int draggedIndex,
    int insertionSlot,
    std::optional<QPoint> draggedTabTopLeft,
    const QFont &font) const {
  QVector<TabGeometry> result;
  const int count = items.size();
  if (count <= 0) return result;

  result.resize(count);

  int pinnedCount = 0;
  for (const auto &item : items) {
    if (item.isPinned) ++pinnedCount;
  }
  const int unpinnedCount = count - pinnedCount;
  const int unpinnedWidth = computeUnpinnedTabWidth(availableWidth, pinnedCount, unpinnedCount);

  // If dragging in strip, calculate logical slots for all tabs
  // Sibling tabs shift around insertionSlot
  int currentX = 0;
  const int tabHeight = metrics_.tabHeight;

  // First pass: compute ideal logical slot rects
  QVector<QRect> slotRects(count);
  for (int slot = 0; slot < count; ++slot) {
    // Determine which item is nominally in this slot
    int itemWidth = metrics_.preferredTabWidth;
    // Pinned slots come first
    if (slot < pinnedCount) {
      itemWidth = metrics_.pinnedTabWidth;
    } else {
      itemWidth = unpinnedWidth;
    }

    slotRects[slot] = QRect(currentX, 0, itemWidth, tabHeight);
    currentX += itemWidth;
  }

  QFontMetrics fm(font);

  // Second pass: assign logicalRect and visualRect to each item
  const bool isDragging = (draggedIndex >= 0 && draggedIndex < count);
  const int targetSlot = (isDragging && insertionSlot >= 0 && insertionSlot < count)
                             ? insertionSlot
                             : draggedIndex;

  for (int i = 0; i < count; ++i) {
    auto &geom = result[i];
    geom.tabIndex = i;
    geom.tabId = items[i].tabId;
    geom.title = items[i].title;
    geom.icon = items[i].icon;
    geom.isPinned = items[i].isPinned;
    geom.isAudible = items[i].isAudible;
    geom.isLoading = items[i].isLoading;
    geom.isSleeping = items[i].isSleeping;
    geom.isActive = (i == activeIndex);
    geom.isDragged = (i == draggedIndex);

    // Calculate ideal slot for item i
    int targetSlotForI = i;
    if (isDragging) {
      if (i == draggedIndex) {
        targetSlotForI = targetSlot;
      } else if (draggedIndex < targetSlot) {
        // Dragged moving right: items in (draggedIndex, targetSlot] shift left by 1
        if (i > draggedIndex && i <= targetSlot) {
          targetSlotForI = i - 1;
        }
      } else if (draggedIndex > targetSlot) {
        // Dragged moving left: items in [targetSlot, draggedIndex) shift right by 1
        if (i >= targetSlot && i < draggedIndex) {
          targetSlotForI = i + 1;
        }
      }
    }

    const int safeSlot = std::clamp(targetSlotForI, 0, count - 1);
    geom.logicalRect = slotRects[safeSlot];

    if (geom.isDragged && draggedTabTopLeft.has_value()) {
      geom.visualRect = QRect(draggedTabTopLeft.value(), QSize(slotRects[safeSlot].width(), tabHeight));
    } else {
      geom.visualRect = geom.logicalRect;
    }

    computeSubRects(geom, fm);
  }

  return result;
}

void TabLayoutModel::computeSubRects(TabGeometry &geom, const QFontMetrics &fm) const {
  Q_UNUSED(fm);
  const QRect r = geom.visualRect;
  if (r.isEmpty()) return;

  const int iconSize = metrics_.faviconSize;
  const int btnSize = metrics_.closeButtonSize;
  const int tabHeight = r.height();

  if (geom.isPinned) {
    // Pinned tab: centered favicon, no close button, no text
    const int iconX = r.left() + (r.width() - iconSize) / 2;
    const int iconY = r.top() + (tabHeight - iconSize) / 2;
    geom.faviconRect = QRect(iconX, iconY, iconSize, iconSize);
    geom.closeButtonRect = QRect();
    geom.textRect = QRect();
    geom.audioIconRect = geom.isAudible ? QRect(r.right() - 14, r.top() + 4, 12, 12) : QRect();
    return;
  }

  // Normal tab
  int leftOffset = geom.isActive ? metrics_.activeContentLeftPadding
                                 : metrics_.inactiveContentLeftPadding;
  const int iconY = r.top() + (tabHeight - iconSize) / 2;

  // Favicon
  geom.faviconRect = QRect(r.left() + leftOffset, iconY, iconSize, iconSize);
  leftOffset += iconSize + metrics_.faviconTitleSpacing;

  // Close button
  const int btnX = r.right() - btnSize - metrics_.closeButtonTrailingPadding;
  const int btnY = r.top() + (tabHeight - btnSize) / 2;
  geom.closeButtonRect = QRect(btnX, btnY, btnSize, btnSize);

  // Audio indicator (if audible, reserved before close button)
  int rightReserved = btnSize + metrics_.closeButtonTrailingPadding + 4;
  if (geom.isAudible) {
    const int audioSize = 14;
    const int audioX = btnX - audioSize - 4;
    const int audioY = r.top() + (tabHeight - audioSize) / 2;
    geom.audioIconRect = QRect(audioX, audioY, audioSize, audioSize);
    rightReserved += audioSize + 4;
  } else {
    geom.audioIconRect = QRect();
  }

  // Title text
  const int textWidth = std::max(0, r.width() - leftOffset - rightReserved);
  if (textWidth > 5) {
    geom.textRect = QRect(r.left() + leftOffset, r.top(), textWidth, tabHeight);
  } else {
    geom.textRect = QRect();
  }
}

int TabLayoutModel::hitTestTabIndex(const QVector<TabGeometry> &geometries, const QPoint &point) const {
  // First check dragged tab if active
  for (const auto &g : geometries) {
    if (g.isDragged && g.visualRect.contains(point)) {
      return g.tabIndex;
    }
  }

  // Next check active tab (rendered on top)
  for (const auto &g : geometries) {
    if (g.isActive && !g.isDragged && g.visualRect.contains(point)) {
      return g.tabIndex;
    }
  }

  // Check from right to left
  for (int i = geometries.size() - 1; i >= 0; --i) {
    const auto &g = geometries[i];
    if (!g.isActive && !g.isDragged && g.visualRect.contains(point)) {
      return g.tabIndex;
    }
  }

  return -1;
}

bool TabLayoutModel::hitTestCloseButton(const TabGeometry &geometry, const QPoint &point) const {
  if (geometry.isPinned || geometry.closeButtonRect.isEmpty()) return false;
  const int margin = metrics_.closeButtonHitMargin;
  return geometry.closeButtonRect.adjusted(-margin, -margin, margin, margin)
      .contains(point);
}

int TabLayoutModel::computeInsertionSlot(
    int availableWidth,
    const QVector<TabModelItem> &items,
    int draggedIndex,
    const QPoint &draggedLocalPoint) const {
  const int count = items.size();
  if (count <= 1) return 0;

  int pinnedCount = 0;
  for (const auto &item : items) {
    if (item.isPinned) ++pinnedCount;
  }
  const int unpinnedCount = count - pinnedCount;
  const int unpinnedWidth = computeUnpinnedTabWidth(availableWidth, pinnedCount, unpinnedCount);

  // If the dragged tab is pinned, restrict insertion to pinned range [0, pinnedCount-1]
  const bool isPinned = (draggedIndex >= 0 && draggedIndex < count) ? items[draggedIndex].isPinned : false;
  const int minSlot = isPinned ? 0 : pinnedCount;
  const int maxSlot = isPinned ? std::max(0, pinnedCount - 1) : (count - 1);

  // Compute center X of each slot
  QVector<int> slotCenters(count);
  int currentX = 0;
  for (int s = 0; s < count; ++s) {
    const int itemWidth = (s < pinnedCount) ? metrics_.pinnedTabWidth : unpinnedWidth;
    slotCenters[s] = currentX + itemWidth / 2;
    currentX += itemWidth;
  }

  const int cursorX = draggedLocalPoint.x();

  // Find slot using midpoints between adjacent slot centers as transition boundaries
  int targetSlot = count - 1;
  for (int s = 0; s < count - 1; ++s) {
    const int transitionBoundary = (slotCenters[s] + slotCenters[s + 1]) / 2;
    if (cursorX < transitionBoundary) {
      targetSlot = s;
      break;
    }
  }

  return std::clamp(targetSlot, minSlot, maxSlot);
}

int TabLayoutModel::computeExternalInsertionIndex(
    int availableWidth,
    const QVector<TabModelItem> &items,
    bool draggedTabPinned,
    const QPoint &draggedLocalPoint) const {
  const int count = items.size();
  if (count == 0) return 0;

  int pinnedCount = 0;
  for (const auto &item : items) {
    if (item.isPinned) ++pinnedCount;
  }
  const int unpinnedCount = count - pinnedCount;
  const int unpinnedWidth = computeUnpinnedTabWidth(
      availableWidth, pinnedCount, unpinnedCount);
  const int minIndex = draggedTabPinned ? 0 : pinnedCount;
  const int maxIndex = draggedTabPinned ? pinnedCount : count;

  int currentX = 0;
  for (int slot = 0; slot < count; ++slot) {
    const int width = slot < pinnedCount ? metrics_.pinnedTabWidth
                                         : unpinnedWidth;
    if (draggedLocalPoint.x() < currentX + width / 2) {
      return std::clamp(slot, minIndex, maxIndex);
    }
    currentX += width;
  }
  return maxIndex;
}

void TabLayoutModel::applyVisualRect(
    TabGeometry &geometry, const QRect &visualRect, const QFont &font) const {
  geometry.visualRect = visualRect;
  geometry.closeButtonRect = {};
  geometry.faviconRect = {};
  geometry.audioIconRect = {};
  geometry.textRect = {};
  computeSubRects(geometry, QFontMetrics(font));
}

QRect TabLayoutModel::computeDetachBoundary(const QSize &stripSize) const {
  return QRect(0, 0, stripSize.width(), stripSize.height()).adjusted(
      -metrics_.horizontalMagnetism,
      -metrics_.verticalDetachThreshold,
      metrics_.horizontalMagnetism,
      metrics_.verticalDetachThreshold);
}

QRect TabLayoutModel::computeAttachBoundary(const QSize &stripSize) const {
  return QRect(0, 0, stripSize.width(), stripSize.height()).adjusted(
      -metrics_.attachMagnetism,
      -metrics_.attachMagnetism,
      metrics_.attachMagnetism,
      metrics_.attachMagnetism);
}

bool TabLayoutModel::isOutsideDetachBoundary(const QPoint &localPoint, const QSize &stripSize) const {
  const QRect boundary = computeDetachBoundary(stripSize);
  // Horizontal travel is reordering, never detaching. Only crossing the
  // expanded top/bottom edge transitions the session into detach.
  return localPoint.y() < boundary.top() || localPoint.y() > boundary.bottom();
}

QRect TabLayoutModel::computeNewTabButtonRect(int visualTabsRight, const QSize &stripSize) const {
  const int btnW = metrics_.newTabButtonWidth;
  const int btnH = metrics_.newTabButtonWidth;
  const int x = visualTabsRight + 1 + metrics_.newTabButtonGap;
  const int y = std::max(0, (stripSize.height() - btnH) / 2);
  return QRect(x, y, btnW, btnH);
}

}  // namespace dalinira::desktop_tabs
