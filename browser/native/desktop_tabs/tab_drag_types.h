#ifndef DALINIRA_DESKTOP_TABS_TAB_DRAG_TYPES_H_
#define DALINIRA_DESKTOP_TABS_TAB_DRAG_TYPES_H_

#include <QIcon>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>
#include <cstdint>

namespace dalinira::desktop_tabs {

enum class DragState {
  Idle,
  Pressed,
  DraggingInStrip,
  Detaching,
  CreatingDetachedWindow,
  DraggingDetachedWindow,
  SearchingAttachTarget,
  AttachingToTarget,
  DraggingInTargetStrip,
  Redetaching,
  Settling,
  Cancelled
};

inline const char *dragStateToString(DragState state) {
  switch (state) {
    case DragState::Idle: return "Idle";
    case DragState::Pressed: return "Pressed";
    case DragState::DraggingInStrip: return "DraggingInStrip";
    case DragState::Detaching: return "Detaching";
    case DragState::CreatingDetachedWindow: return "CreatingDetachedWindow";
    case DragState::DraggingDetachedWindow: return "DraggingDetachedWindow";
    case DragState::SearchingAttachTarget: return "SearchingAttachTarget";
    case DragState::AttachingToTarget: return "AttachingToTarget";
    case DragState::DraggingInTargetStrip: return "DraggingInTargetStrip";
    case DragState::Redetaching: return "Redetaching";
    case DragState::Settling: return "Settling";
    case DragState::Cancelled: return "Cancelled";
  }
  return "Unknown";
}

enum class EndDragReason {
  Completed,
  Cancelled,
  TargetClosed,
  SourceClosed,
  Interrupted
};

enum class TabStyle {
  ChromeCurved,
  DaliNiraSignature,
  FloatingPill,
  DaliNiraConnected
};

struct TabGeometry {
  int tabIndex = -1;
  uint64_t tabId = 0;
  QRect logicalRect;
  QRect visualRect;
  QRect closeButtonRect;
  QRect faviconRect;
  QRect audioIconRect;
  QRect textRect;
  bool isPinned = false;
  bool isActive = false;
  bool isDragged = false;
  bool isHovered = false;
  qreal hoverOpacity = 0.0;
  bool isClosing = false;
  qreal visualOpacity = 1.0;
  bool isAudible = false;
  bool isLoading = false;
  bool isSleeping = false;
  QString title;
  QIcon icon;
};

struct LayoutMetrics {
  int minTabWidth = 110;
  int preferredTabWidth = 220;
  int pinnedTabWidth = 42;
  int tabHeight = 34;
  int tabOverlap = 10;
  int closeButtonSize = 18;
  int faviconSize = 16;
  int activeContentLeftPadding = 14;
  int inactiveContentLeftPadding = 12;
  int faviconTitleSpacing = 7;
  int closeButtonTrailingPadding = 6;
  int closeButtonHitMargin = 2;
  int horizontalMagnetism = 40;
  int verticalMagnetism = 35;
  int attachMagnetism = 12;
  int minimumStartDragDistance = 4;
  int horizontalStartDragThreshold = 8;
  int verticalDetachThreshold = 35;
  int newTabButtonWidth = 28;
  int newTabButtonGap = 4;
};

}  // namespace dalinira::desktop_tabs

#endif  // DALINIRA_DESKTOP_TABS_TAB_DRAG_TYPES_H_
