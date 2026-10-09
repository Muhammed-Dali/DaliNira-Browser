#pragma once

#include <QDateTime>
#include <QHash>
#include <QListWidget>
#include <QPainter>
#include <QStackedWidget>
#include <QTimer>
#include <QUrl>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>

class DaliNiraBlockerService;
class QCheckBox;
class QLabel;
class QLineEdit;
class QTextEdit;
class QPlainTextEdit;
class QComboBox;
class QPushButton;
class QTableWidget;
class QProgressBar;

// Custom animated/painted circular gauge widget matching the blocker window canvas
class ModeKnobWidget final : public QWidget {
  Q_OBJECT
 public:
  ModeKnobWidget(int level, int value, const QString &label, QWidget *parent = nullptr);
  void setValue(int val);
  QSize sizeHint() const override { return QSize(120, 130); }
  QSize minimumSizeHint() const override { return QSize(100, 110); }

 protected:
  void paintEvent(QPaintEvent *event) override;

 private:
  int level_ = 2;
  int value_ = 65;
  QString label_;
};

class DaliNiraBlockerPage final : public QWidget {
  Q_OBJECT
 public:
  enum class Tab {
    Settings,
    Rulesets,
    CustomFilters,
    Sites,
    Statistics,
    Logger,
    Develop,
    About
  };

  explicit DaliNiraBlockerPage(DaliNiraBlockerService *service, QWidget *parent = nullptr);
  ~DaliNiraBlockerPage() override = default;

  void setActiveTab(Tab tab);
  void setActiveHost(const QString &host);
  void refreshAll();

 signals:
  void openUrlRequested(const QUrl &url);

 protected:
  void showEvent(QShowEvent *event) override;
  void hideEvent(QHideEvent *event) override;
  bool eventFilter(QObject *watched, QEvent *event) override;

 private:
  void createSettingsTab();
  void createRulesetsTab();
  void createCustomFiltersTab();
  void createSitesTab();
  void createStatisticsTab();
  void createLoggerTab();
  void createDevelopTab();
  void createAboutTab();

  void updateModeUi();
  void refreshRulesetTab();
  void refreshCustomFiltersTab();
  void refreshSitesTab();
  void refreshStatisticsTab();
  void refreshLoggerTab();
  void refreshDevelopTab();

  DaliNiraBlockerService *service_ = nullptr;
  QString activeHost_;
  QTimer refreshTimer_;

  QListWidget *sidebar_ = nullptr;
  QStackedWidget *stack_ = nullptr;
  QHash<Tab, int> tabIndices_;

  // Settings Tab elements
  QLabel *modePill_ = nullptr;
  QLabel *modeDesc_ = nullptr;
  QWidget *basicCard_ = nullptr;
  QWidget *idealCard_ = nullptr;
  QWidget *aggressiveCard_ = nullptr;
  QCheckBox *autoReloadCheck_ = nullptr;
  QCheckBox *showCountCheck_ = nullptr;
  QCheckBox *strictBlockCheck_ = nullptr;
  QCheckBox *popupBlockCheck_ = nullptr;
  QCheckBox *developerModeCheck_ = nullptr;

  // Rulesets Tab elements
  QLabel *rulesetSessionCount_ = nullptr;
  QLabel *rulesetTotalCount_ = nullptr;
  QLabel *rulesetCount_ = nullptr;
  QLabel *rulesetDomainRuleCount_ = nullptr;
  QLabel *rulesetDnrRuleCount_ = nullptr;
  QVBoxLayout *rulesetBreakdownLayout_ = nullptr;
  QVBoxLayout *rulesetCatalogLayout_ = nullptr;
  QLabel *rulesetUpdateStatus_ = nullptr;
  QCheckBox *autoUpdateListsCheck_ = nullptr;
  QProgressBar *rulesetUpdateProgress_ = nullptr;

  // Custom Filters Tab elements
  QPlainTextEdit *userFilterEditor_ = nullptr;
  QLabel *userFilterValidation_ = nullptr;
  QVBoxLayout *userFilterListLayout_ = nullptr;

  // Sites Tab elements
  QLineEdit *siteHostInput_ = nullptr;
  QCheckBox *siteWhitelistCheck_ = nullptr;
  QCheckBox *siteAdsCheck_ = nullptr;
  QCheckBox *siteTrackersCheck_ = nullptr;
  QLabel *siteActiveStatus_ = nullptr;
  QVBoxLayout *sitePolicyListLayout_ = nullptr;

  // Statistics Tab elements
  QLabel *statsToday_ = nullptr;
  QLabel *statsWeek_ = nullptr;
  QLabel *statsMonth_ = nullptr;
  QLabel *statsTotal_ = nullptr;
  QLabel *statsTrackers_ = nullptr;
  QLabel *statsSaved_ = nullptr;
  QLabel *statsSpeed_ = nullptr;
  QLabel *statsWhitelistSites_ = nullptr;
  QLabel *statsWhitelistAllowed_ = nullptr;
  QVBoxLayout *topSitesLayout_ = nullptr;
  QVBoxLayout *topListsLayout_ = nullptr;

  // Logger Tab elements
  QLineEdit *loggerSearch_ = nullptr;
  QComboBox *loggerActionCombo_ = nullptr;
  QTableWidget *logTable_ = nullptr;
  QLabel *diagLoaded_ = nullptr;
  QLabel *diagActive_ = nullptr;
  QLabel *diagBlocked_ = nullptr;
  QLabel *diagAllowed_ = nullptr;
  QLabel *diagTime_ = nullptr;
  QLabel *diagMemory_ = nullptr;

  // Develop Tab elements
  QComboBox *developViewCombo_ = nullptr;
  QPlainTextEdit *developEditor_ = nullptr;

  // About Tab elements
  QLabel *aboutActiveHostLabel_ = nullptr;
};

using AdBlockPage = DaliNiraBlockerPage;
using BlockerPage = DaliNiraBlockerPage;
