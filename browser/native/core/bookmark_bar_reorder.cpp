#include "bookmark_bar_reorder.h"

#include <QApplication>
#include <QDrag>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFrame>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QTimer>
#include <QToolButton>
#include <QWidget>

BookmarkBarReorder::BookmarkBarReorder(QWidget *bar, std::function<void(const QUrl &, int)> moveRequested,
                                       QObject *parent)
    : QObject(parent), bar_(bar), moveRequested_(std::move(moveRequested)) {
  bar->setAcceptDrops(true);
  bar->installEventFilter(this);
  indicator_ = new QFrame(bar);
  indicator_->setObjectName(QStringLiteral("bookmarkDropIndicator"));
  indicator_->setStyleSheet(QStringLiteral("background:#8ab4f8;border:none;border-radius:1px;"));
  indicator_->setAttribute(Qt::WA_TransparentForMouseEvents);
  indicator_->hide();
}

void BookmarkBarReorder::clearItems() {
  items_.clear();
  pressedHandle_ = nullptr;
  hideIndicator();
}

void BookmarkBarReorder::addItem(QWidget *item, QWidget *handle, const QUrl &url) {
  items_.append({item, handle, url});
  handle->installEventFilter(this);
}

int BookmarkBarReorder::insertionSlot(const QPoint &barPos) const {
  int slot = 0;
  for (int i = 0; i < items_.size(); ++i) {
    QWidget *widget = items_[i].widget;
    if (!widget || !widget->isVisible() || !bar_) continue;
    const QRect rect(widget->mapTo(bar_, QPoint(0, 0)), widget->size());
    if (barPos.x() >= rect.center().x()) slot = i + 1;
  }
  return slot;
}

int BookmarkBarReorder::targetIndex(int from, int slot) {
  return slot > from ? slot - 1 : slot;
}

bool BookmarkBarReorder::isIndicatorVisible() const {
  return indicator_ && indicator_->isVisible();
}

int BookmarkBarReorder::indexOfHandle(QObject *handle) const {
  for (int i = 0; i < items_.size(); ++i)
    if (items_[i].handle == handle) return i;
  return -1;
}

int BookmarkBarReorder::indexOfUrl(const QUrl &url) const {
  for (int i = 0; i < items_.size(); ++i)
    if (items_[i].url == url) return i;
  return -1;
}

void BookmarkBarReorder::showIndicator(int slot) {
  if (!bar_ || items_.isEmpty()) return;
  int x = 0;
  QWidget *anchor = nullptr;
  if (slot < items_.size()) {
    anchor = items_[slot].widget;
    if (anchor) x = anchor->mapTo(bar_, QPoint(0, 0)).x() - 2;
  } else {
    anchor = items_.last().widget;
    if (anchor) x = anchor->mapTo(bar_, QPoint(anchor->width(), 0)).x() + 1;
  }
  if (!anchor) return;
  const int top = anchor->mapTo(bar_, QPoint(0, 0)).y() + 3;
  indicator_->setGeometry(x, top, 2, std::max(8, anchor->height() - 6));
  indicator_->show();
  indicator_->raise();
}

void BookmarkBarReorder::hideIndicator() {
  if (indicator_) indicator_->hide();
}

void BookmarkBarReorder::startDrag(int index) {
  const Item item = items_.value(index);
  if (!item.widget || !item.handle) return;
  auto *mime = new QMimeData;
  mime->setData(QString::fromLatin1(kMimeType), item.url.toString(QUrl::FullyEncoded).toUtf8());
  auto *drag = new QDrag(item.handle);
  drag->setMimeData(mime);
  QPixmap preview = item.widget->grab();
  if (!preview.isNull()) {
    QPixmap faded(preview.size());
    faded.setDevicePixelRatio(preview.devicePixelRatio());
    faded.fill(Qt::transparent);
    QPainter painter(&faded);
    painter.setOpacity(0.85);
    painter.drawPixmap(0, 0, preview);
    painter.end();
    drag->setPixmap(faded);
    drag->setHotSpot(item.handle->mapTo(item.widget, pressPos_));
  }
  const QPointer<QWidget> handle = item.handle;
  drag->exec(Qt::MoveAction);
  // The release went to the drag; leave the button unpressed and unclicked.
  if (auto *button = qobject_cast<QToolButton *>(handle.data())) button->setDown(false);
  pressedHandle_ = nullptr;
  hideIndicator();
}

bool BookmarkBarReorder::eventFilter(QObject *watched, QEvent *event) {
  if (watched == bar_) {
    switch (event->type()) {
      case QEvent::DragEnter:
      case QEvent::DragMove: {
        auto *drag = static_cast<QDragMoveEvent *>(event);
        if (!drag->mimeData()->hasFormat(QString::fromLatin1(kMimeType))) return false;
        drag->setDropAction(Qt::MoveAction);
        drag->accept();
        showIndicator(insertionSlot(drag->position().toPoint()));
        return true;
      }
      case QEvent::DragLeave:
        hideIndicator();
        return false;
      case QEvent::Drop: {
        auto *drop = static_cast<QDropEvent *>(event);
        hideIndicator();
        if (!drop->mimeData()->hasFormat(QString::fromLatin1(kMimeType))) return false;
        const QUrl url(QString::fromUtf8(drop->mimeData()->data(QString::fromLatin1(kMimeType))),
                       QUrl::StrictMode);
        const int from = indexOfUrl(url);
        drop->setDropAction(Qt::MoveAction);
        drop->accept();
        if (from < 0) return true;
        const int to = targetIndex(from, insertionSlot(drop->position().toPoint()));
        if (to != from && moveRequested_) {
          auto callback = moveRequested_;
          QTimer::singleShot(0, this, [callback, url, to] { callback(url, to); });
        }
        return true;
      }
      default:
        return false;
    }
  }

  const int index = indexOfHandle(watched);
  if (index < 0) return false;
  if (event->type() == QEvent::MouseButtonPress) {
    auto *mouse = static_cast<QMouseEvent *>(event);
    if (mouse->button() == Qt::LeftButton) {
      pressedHandle_ = watched;
      pressPos_ = mouse->position().toPoint();
    }
  } else if (event->type() == QEvent::MouseButtonRelease) {
    pressedHandle_ = nullptr;
  } else if (event->type() == QEvent::MouseMove && pressedHandle_ == watched) {
    auto *mouse = static_cast<QMouseEvent *>(event);
    if ((mouse->buttons() & Qt::LeftButton) && items_.size() > 1 &&
        (mouse->position().toPoint() - pressPos_).manhattanLength() >= QApplication::startDragDistance()) {
      startDrag(index);
      return true;
    }
  }
  return false;
}
