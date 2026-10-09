#ifndef DALINIRA_DESKTOP_TABS_FIND_BAR_WIDGET_H_
#define DALINIRA_DESKTOP_TABS_FIND_BAR_WIDGET_H_

#include <QWidget>
#include <QLineEdit>
#include <QLabel>
#include <QToolButton>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QSize>
#include <QFrame>

namespace dalinira::desktop_tabs {

class FindBarWidget : public QWidget {
  Q_OBJECT

 public:
  explicit FindBarWidget(QWidget *parent = nullptr);
  ~FindBarWidget() override = default;

  void setFindText(const QString &text);
  QString findText() const;
  bool isCaseSensitive() const;
  void setMatchCount(int current, int total);
  void clearMatchCount();
  void focusAndSelectAll();
  void refreshAppearance();
  QSize sizeHint() const override;

 signals:
  void findRequested(const QString &text, bool forward, bool caseSensitive);
  void clearFindRequested();
  void closeRequested();

 protected:
  bool eventFilter(QObject *watched, QEvent *event) override;
  void keyPressEvent(QKeyEvent *event) override;
  void changeEvent(QEvent *event) override;
  void resizeEvent(QResizeEvent *event) override;
  void showEvent(QShowEvent *event) override;

 private slots:
  void onTextChanged(const QString &text);
  void onNextClicked();
  void onPrevClicked();
  void onCaseToggled();

 private:
  void setupUi();
  void applyStyle(bool force = true);
  void retranslateUi();
  void updateNavigationState();
  void updateNativeMask();

  QLineEdit *searchEdit_ = nullptr;
  QLabel *matchCountLabel_ = nullptr;
  QToolButton *prevBtn_ = nullptr;
  QToolButton *nextBtn_ = nullptr;
  QToolButton *caseBtn_ = nullptr;
  QFrame *divider_ = nullptr;
  bool styledDark_ = false;
  bool styled_ = false;
  bool applyingStyle_ = false;
  QToolButton *closeBtn_ = nullptr;
};

}  // namespace dalinira::desktop_tabs

#endif  // DALINIRA_DESKTOP_TABS_FIND_BAR_WIDGET_H_
