#pragma once

#include <QAbstractItemView>
#include <QMouseEvent>
#include <QPainter>
#include <QPersistentModelIndex>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QStyle>
#include <QApplication>
#include <functional>

// Own the remove hit area so QCompleter never activates a clicked X.
class SuggestionRemoveDelegate final : public QStyledItemDelegate {
 public:
  static constexpr int RemovableRole = Qt::UserRole + 3;
  using Remove = std::function<void(const QModelIndex &)>;
  static bool usesHistoryIcon(const QString &type) {
    return type == QLatin1String("history") || type == QLatin1String("search-history") ||
           type == QLatin1String("frequent");
  }
  SuggestionRemoveDelegate(QAbstractItemView *view, Remove remove)
      : QStyledItemDelegate(view), view_(view), remove_(std::move(remove)) {
    view->setMouseTracking(true);
    view->viewport()->installEventFilter(this);
  }
  static QRect removeRect(const QRect &row) {
    return QRect(row.right() - 27, row.top(), 28, row.height());
  }
  void paint(QPainter *painter, const QStyleOptionViewItem &option,
             const QModelIndex &index) const override {
    auto row = option;
    initStyleOption(&row, index);
    row.showDecorationSelected = true;
    const bool removable = index.data(RemovableRole).toBool();
    const QStyle *style = row.widget ? row.widget->style() : QApplication::style();
    if (removable) {
      // Reserve label space, keeping the full row rectangle for its panel.
      const QRect textRect = style->subElementRect(QStyle::SE_ItemViewItemText, &row, row.widget);
      row.text = row.fontMetrics.elidedText(row.text, row.textElideMode,
          qMax(0, removeRect(row.rect).left() - textRect.left() - 6));
    }
    style->drawControl(QStyle::CE_ItemViewItem, &row, painter, row.widget);
    if (!removable) return;
    painter->save();
    painter->setPen(QPen(row.palette.color(row.state & QStyle::State_Selected
        ? QPalette::HighlightedText : QPalette::Text), 1.5));
    const QPoint center = removeRect(option.rect).center();
    painter->drawLine(center + QPoint(-4, -4), center + QPoint(4, 4));
    painter->drawLine(center + QPoint(-4, 4), center + QPoint(4, -4));
    painter->restore();
  }
 protected:
  bool eventFilter(QObject *object, QEvent *event) override {
    if (object == view_->viewport() && event->type() == QEvent::Leave) {
      view_->viewport()->unsetCursor();
      clearMouseHover();
    }
    if (object == view_->viewport() && event->type() == QEvent::MouseMove) {
      const auto *mouse = static_cast<QMouseEvent *>(event);
      const QModelIndex index = view_->indexAt(mouse->position().toPoint());
      const bool hit = index.isValid() && index.data(RemovableRole).toBool() &&
          removeRect(view_->visualRect(index)).contains(mouse->position().toPoint());
      if (hit) view_->viewport()->setCursor(Qt::PointingHandCursor);
      else view_->viewport()->unsetCursor();
      if (mouse->buttons() == Qt::NoButton || removeGesture_) {
        // Hover selects visually without QCompleter's highlighted signal
        // replacing the omnibox text. Keyboard selection remains unchanged.
        const QSignalBlocker blocker(view_->selectionModel());
        if (index.isValid()) {
          view_->setCurrentIndex(index);
          mouseHovered_ = true;
        } else {
          clearMouseHover();
        }
        view_->viewport()->update();
        return true;
      }
    }
    if (object == view_->viewport() &&
        (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease ||
         event->type() == QEvent::MouseButtonDblClick)) {
      const auto *mouse = static_cast<QMouseEvent *>(event);
      if (mouse->button() == Qt::LeftButton) {
        const QModelIndex index = view_->indexAt(mouse->position().toPoint());
        const bool hit = index.isValid() && index.data(RemovableRole).toBool() &&
            removeRect(view_->visualRect(index)).contains(mouse->position().toPoint());
        if (event->type() == QEvent::MouseButtonRelease && removeGesture_) {
          const bool remove = hit && index == pressed_;
          removeGesture_ = false;
          pressed_ = QPersistentModelIndex();
          if (remove) {
            // Clearing/repopulating suggestions must not preview another row.
            const QSignalBlocker blocker(view_->selectionModel());
            remove_(index);
            view_->viewport()->update();
          }
          return true;
        }
        if (hit) {
          if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonDblClick) {
            pressed_ = index;
            removeGesture_ = true;
          }
          return true;
        }
      }
    }
    return QStyledItemDelegate::eventFilter(object, event);
  }
 private:
  void clearMouseHover() {
    if (!mouseHovered_) return;
    mouseHovered_ = false;
    const QSignalBlocker blocker(view_->selectionModel());
    view_->selectionModel()->clearSelection();
    view_->setCurrentIndex(QModelIndex());
    view_->viewport()->update();
  }
  QAbstractItemView *view_;
  Remove remove_;
  QPersistentModelIndex pressed_;
  bool removeGesture_ = false;
  bool mouseHovered_ = false;
};
