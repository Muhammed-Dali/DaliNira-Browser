#pragma once

#include <QComboBox>
#include <QFrame>
#include <QIcon>
#include <QPainter>
#include <QToolButton>

class DaliNiraBlockerService;
class QCheckBox;
class QLabel;
class QPushButton;

class DaliNiraBlockerQuickPopup final : public QFrame {
  Q_OBJECT
 public:
  explicit DaliNiraBlockerQuickPopup(DaliNiraBlockerService *service, QWidget *parent = nullptr);
  void updateForHost(const QString &host, quint64 blockedCount);

 signals:
  void openSettingsRequested();
  void openRulesetsRequested();
  void openLoggerRequested();
  void reloadRequested();
  // "Block an element" on the current page (the element picker).
  void pickElementRequested();

 protected:
  void showEvent(QShowEvent *event) override;

 private:
  void syncUi();

  DaliNiraBlockerService *service_ = nullptr;
  QString currentHost_;
  quint64 blockedCount_ = 0;

  QLabel *hostLabel_ = nullptr;
  QLabel *statusSubtitleLabel_ = nullptr;
  QLabel *countLabel_ = nullptr;
  QLabel *countTextLabel_ = nullptr;
  QCheckBox *masterCheck_ = nullptr;
  QCheckBox *siteProtectionCheck_ = nullptr;
  QCheckBox *adsCheck_ = nullptr;
  QCheckBox *trackersCheck_ = nullptr;
  QLabel *noticeLabel_ = nullptr;
  QPushButton *pickElementBtn_ = nullptr;

  // Advanced collapsible options
  QWidget *advancedContainer_ = nullptr;
  QLabel *advancedChevron_ = nullptr;
  QLabel *blockedBadge_ = nullptr;
  QComboBox *adsModeCombo_ = nullptr;
  QCheckBox *httpsCheck_ = nullptr;
  QCheckBox *scriptsCheck_ = nullptr;
  QCheckBox *fingerprintCheck_ = nullptr;
  QComboBox *cookieCombo_ = nullptr;
  QCheckBox *forgetOnCloseCheck_ = nullptr;
};

using AdBlockQuickPopup = DaliNiraBlockerQuickPopup;

class DaliNiraBlockerShieldButton final : public QToolButton {
  Q_OBJECT
 public:
  explicit DaliNiraBlockerShieldButton(DaliNiraBlockerService *service, QWidget *parent = nullptr);
  ~DaliNiraBlockerShieldButton() override = default;

  void setBlockedCount(quint64 count);
  quint64 blockedCount() const { return blockedCount_; }

  void setActiveHost(const QString &host);
  QString activeHost() const { return currentHost_; }

  void setInternalPage(bool internal);
  bool isInternalPage() const { return isInternalPage_; }

 signals:
  void openSettingsRequested();
  void openRulesetsRequested();
  void openLoggerRequested();
  void reloadRequested();
  void pickElementRequested();

 protected:
  void paintEvent(QPaintEvent *event) override;

 private:
  void showQuickPopup();

  DaliNiraBlockerService *service_ = nullptr;
  DaliNiraBlockerQuickPopup *popup_ = nullptr;
  quint64 blockedCount_ = 0;
  QString currentHost_;
  bool showBadge_ = true;
  bool isInternalPage_ = false;
};

using AdBlockShieldButton = DaliNiraBlockerShieldButton;
using BlockerShieldButton = DaliNiraBlockerShieldButton;
