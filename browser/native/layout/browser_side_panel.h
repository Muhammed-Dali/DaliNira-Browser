#pragma once

#include <QWidget>
#include <QIcon>
#include <QStringList>
#include <QVector>

class QLabel;
class QListWidget;
class QToolButton;

namespace dalinira::layout { class SidePanelTabList; }

namespace dalinira::layout {

struct SidePanelTabInfo {
  uint64_t id = 0;
  QString title;
  QIcon icon;
  bool loading = false;
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

 signals:
  void tabSelected(int index);
  void tabCloseRequested(int index);
  void tabMoved(int from, int to);
  void tabDetachRequested(int index, QPoint globalPosition);
  void tabTransferRequested(BrowserSidePanel *source, int sourceIndex, int targetIndex);
  void newTabRequested();
  void bookmarksRequested();
  void historyRequested();
  void downloadsRequested();
  void passwordsRequested();
  void settingsRequested();
  void closePanelRequested();

 private:
  void setupUi();
  void updateStyles();

  QLabel *headerTitle_ = nullptr;
  QToolButton *closeButton_ = nullptr;
  QLabel *tabsHeader_ = nullptr;
  QToolButton *newTabButton_ = nullptr;
  SidePanelTabList *tabList_ = nullptr;

  QToolButton *bookmarksButton_ = nullptr;
  QToolButton *historyButton_ = nullptr;
  QToolButton *downloadsButton_ = nullptr;
  QToolButton *passwordsButton_ = nullptr;
  QToolButton *settingsButton_ = nullptr;

  int activeIndex_ = -1;
  bool updating_ = false;
};

} // namespace dalinira::layout
