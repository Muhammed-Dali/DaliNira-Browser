#ifndef DALINIRA_DESKTOP_TABS_TAB_SEARCH_POPUP_H_
#define DALINIRA_DESKTOP_TABS_TAB_SEARCH_POPUP_H_

#include <QDateTime>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QScrollArea>
#include <QToolButton>
#include <QUrl>
#include <QUuid>
#include <QColor>
#include <QVBoxLayout>
#include <QWidget>
#include <vector>
#include <optional>

class BrowserWindow;

namespace dalinira::desktop_tabs {

struct TabSearchItem {
  enum class Type { OpenTab, ClosedTab };
  Type type;
  QPointer<BrowserWindow> window;
  int tabIndex = -1;
  int closedIndex = -1;
  uint64_t tabId = 0;
  QString title;
  QUrl url;
  QIcon icon;
  QDateTime time;
  QWidget *rowWidget = nullptr;
  std::optional<QUuid> groupId;
  QString groupName;
  QColor groupColor;
};

class TabSearchPopup : public QWidget {
  Q_OBJECT
 public:
  explicit TabSearchPopup(BrowserWindow *parentWindow = nullptr);
  ~TabSearchPopup() override = default;

  void showBelow(QWidget *anchorWidget);
  // The configured "Search tabs" shortcut (KeyboardShortcutService); hidden when empty.
  void setShortcutHint(const QString &text);

 protected:
  bool eventFilter(QObject *watched, QEvent *event) override;
  void keyPressEvent(QKeyEvent *event) override;

 private slots:
  void onSearchTextChanged(const QString &text);

 private:
  void setupUi();
  void populateData();
  void filterRows(const QString &query);
  void selectRow(int index);
  void activateSelectedRow();

  QPointer<BrowserWindow> parentWindow_;
  QLineEdit *searchEdit_ = nullptr;
  QLabel *shortcutLabel_ = nullptr;
  QScrollArea *scrollArea_ = nullptr;
  QWidget *contentContainer_ = nullptr;
  QVBoxLayout *contentLayout_ = nullptr;

  QWidget *openSectionHeader_ = nullptr;
  QWidget *closedSectionHeader_ = nullptr;

  std::vector<TabSearchItem> items_;
  int selectedIndex_ = -1;
};

}  // namespace dalinira::desktop_tabs

#endif  // DALINIRA_DESKTOP_TABS_TAB_SEARCH_POPUP_H_
