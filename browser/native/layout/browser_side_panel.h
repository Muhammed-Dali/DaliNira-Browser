#pragma once

#include <QWidget>
#include <QColor>
#include <QIcon>
#include <QPixmap>
#include <QPointer>
#include <QStringList>
#include <QVector>

class QHBoxLayout;
class QLabel;
class QListWidget;
class QToolButton;

namespace dalinira::layout { class SidePanelTabList; }

namespace dalinira::layout {

// What the New Tab page paints behind its content (its computed CSS): the
// docked panel shows it through a tinted glass while New Tab is active, as if
// the page's background continued under the panel.
struct NewTabBackdrop {
  bool valid = false;
  QColor baseColor;
  QString cssImage;   // computed background-image: none, linear-gradient(...), url(...)
  QString imagePath;  // local file or resource for a url() image
  QColor overlay;     // the page's dim layer (body::before)
  bool operator==(const NewTabBackdrop &other) const {
    return valid == other.valid && baseColor == other.baseColor && cssImage == other.cssImage &&
           imagePath == other.imagePath && overlay == other.overlay;
  }
};

struct SidePanelTabInfo {
  uint64_t id = 0;
  QString title;
  QIcon icon;
  bool loading = false;
  bool sleeping = false;  // released or frozen by tab sleep (both engines)
};

class BrowserSidePanel : public QWidget {
  Q_OBJECT
 public:
  explicit BrowserSidePanel(QWidget *parent = nullptr);
  ~BrowserSidePanel() override = default;

  void setTabs(const QStringList &tabTitles, int activeIndex);
  void setTabs(const QVector<SidePanelTabInfo> &tabs, int activeIndex);
  void setActiveTab(int activeIndex);
  int tabCount() const;
  QString tabTitle(int index) const;
  void requestTabMove(int from, int to);
  void requestTabDetach(int index, const QPoint &globalPosition);
  void requestTabTransfer(BrowserSidePanel *source, int sourceIndex, int targetIndex);
  void retranslateUi();
  void refreshAppearance();

  // Side-panel layouts move the window's navigation controls (back, forward,
  // reload, add-site, main menu) into the panel's top bar, as Zen's sidebar
  // top buttons; they stay the window's own widgets. Empty lists hand them
  // back (the caller re-inserts them in its toolbar).
  void setChromeControls(const QList<QWidget *> &leading, const QList<QWidget *> &trailing);
  // The pin toggle: pinned = always open, unpinned = auto-hide/hover reveal.
  void setPinned(bool pinned);
  bool isPinned() const { return pinned_; }
  void setPanelOnRight(bool right);
  // The New Tab background to show through (invalid: the panel's own style),
  // and the page area it continues from.
  void setNewTabBackdrop(const NewTabBackdrop &backdrop);
  const NewTabBackdrop &newTabBackdrop() const { return backdrop_; }
  void setBackdropPageArea(QWidget *pageArea) { pageArea_ = pageArea; }

 protected:
  bool event(QEvent *event) override;
  void resizeEvent(QResizeEvent *event) override;
  void paintEvent(QPaintEvent *event) override;

 signals:
  void tabSelected(int index);
  void tabCloseRequested(int index);
  // Right-click on a tab: the window shows its shared tab context menu.
  void tabContextMenuRequested(int index, QPoint globalPosition);
  // A tab is being dragged inside or out of the panel (auto-hide holds).
  void tabDragActiveChanged(bool active);
  void tabMoved(int from, int to);
  void tabDetachRequested(int index, QPoint globalPosition);
  void tabTransferRequested(BrowserSidePanel *source, int sourceIndex, int targetIndex);
  void newTabRequested();
  void bookmarksRequested();
  void historyRequested();
  void settingsRequested();
  // The pin toggle was clicked (the window flips the shared preference).
  void pinToggleRequested();

 private:
  void setupUi();
  void updateStyles();

  void updatePinButton();
  void updateCardShape();

  QWidget *topBar_ = nullptr;
  QHBoxLayout *topLayout_ = nullptr;
  QToolButton *pinButton_ = nullptr;
  QList<QWidget *> chromeControls_;
  bool pinned_ = false;
  NewTabBackdrop backdrop_;
  QPointer<QWidget> pageArea_;
  QPixmap backdropCache_;
  QString backdropCacheKey_;
  bool onRight_ = false;
  QLabel *tabsHeader_ = nullptr;
  QToolButton *newTabButton_ = nullptr;
  SidePanelTabList *tabList_ = nullptr;

  QToolButton *bookmarksButton_ = nullptr;
  QToolButton *historyButton_ = nullptr;
  QToolButton *settingsButton_ = nullptr;

  int activeIndex_ = -1;
  bool updating_ = false;
};

} // namespace dalinira::layout
