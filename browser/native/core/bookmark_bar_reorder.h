#pragma once

#include <QList>
#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QUrl>
#include <functional>

class QFrame;
class QWidget;

// Drag-and-drop ordering of the shortcut (Quick Access) items on the bookmark
// bar. Items are registered in display order on every render; a drop reports
// the new position through `moveRequested`, deferred to the next event-loop
// turn because the resulting re-render deletes the dragged item.
class BookmarkBarReorder : public QObject {
  Q_OBJECT

 public:
  static constexpr const char *kMimeType = "application/x-dalinira-quick-access";

  BookmarkBarReorder(QWidget *bar, std::function<void(const QUrl &url, int toIndex)> moveRequested,
                     QObject *parent = nullptr);

  void clearItems();
  // `item` is the whole shortcut (button + remove), `handle` the part that is
  // pressed to drag it.
  void addItem(QWidget *item, QWidget *handle, const QUrl &url);

  // Insertion slot (0..count) for a drop at `barPos`, before removing the source.
  int insertionSlot(const QPoint &barPos) const;
  // Final list index for moving item `from` to insertion slot `slot`.
  static int targetIndex(int from, int slot);
  bool isIndicatorVisible() const;

 protected:
  bool eventFilter(QObject *watched, QEvent *event) override;

 private:
  struct Item {
    QPointer<QWidget> widget;
    QPointer<QWidget> handle;
    QUrl url;
  };
  int indexOfHandle(QObject *handle) const;
  int indexOfUrl(const QUrl &url) const;
  void startDrag(int index);
  void showIndicator(int slot);
  void hideIndicator();

  QPointer<QWidget> bar_;
  std::function<void(const QUrl &, int)> moveRequested_;
  QList<Item> items_;
  QFrame *indicator_ = nullptr;
  QPointer<QObject> pressedHandle_;
  QPoint pressPos_;
};
