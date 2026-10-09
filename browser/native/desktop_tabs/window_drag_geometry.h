#pragma once

#include <QPoint>
#include <QRect>
#include <QSize>
#include <QtGlobal>

namespace dalinira::desktop_tabs {

// Keep the point grabbed in a maximized title bar beneath the cursor when
// restoring to a floating window before starting the native move.
inline QRect restoredWindowGeometryForDrag(const QRect &maximized, const QSize &restored,
                                           const QPoint &pressed, const QPoint &cursor) {
  const int heldX = qBound(0, qRound(qreal(pressed.x() - maximized.left()) * restored.width() /
                                 qMax(1, maximized.width())), restored.width() - 1);
  const int heldY = qBound(0, pressed.y() - maximized.top(), restored.height() - 1);
  return QRect(cursor - QPoint(heldX, heldY), restored);
}

}  // namespace dalinira::desktop_tabs
