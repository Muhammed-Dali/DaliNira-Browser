#pragma once

#include "onboarding_step.h"
#include "browser_data_importer.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>

namespace dalinira::onboarding {

class IBrowserDataReceiver;

class ModernCheckBox;

class ImportStep final : public OnboardingStep {
  Q_OBJECT

 public:
  explicit ImportStep(IBrowserDataReceiver *dataReceiver = nullptr, QWidget *parent = nullptr);
  ~ImportStep() override = default;

  QString stepId() const override { return QStringLiteral("import"); }
  QString stepTitle() const override;
  QString stepSubtitle() const override;

  bool canGoBack() const override { return true; }
  bool canGoNext() const override { return true; }
  bool canSkip() const override { return true; }

  QString nextButtonText() const override;
  QString backButtonText() const override;
  QString skipButtonText() const override;

  void onStepEntering() override;
  void onStepLeaving() override;

  // Manual import trigger (called on Next)
  int executeImport();

  void retranslateUi();

 private:
  void setupUi();
  void selectChoice(bool isImport);
  void refreshCardStyles();
  void updateSourceOptionsVisibility();
  void populateBrowsers();

  IBrowserDataReceiver *dataReceiver_ = nullptr;
  QList<DetectedBrowser> detectedBrowsers_;

  QLabel *titleLabel_ = nullptr;
  QLabel *subtitleLabel_ = nullptr;

  QLabel *importTitle_ = nullptr;
  QLabel *importDesc_ = nullptr;
  QLabel *freshTitle_ = nullptr;
  QLabel *freshDesc_ = nullptr;
  QLabel *sourceLabel_ = nullptr;

  QFrame *importCard_ = nullptr;
  QFrame *freshCard_ = nullptr;
  QRadioButton *importRadio_ = nullptr;
  QRadioButton *freshRadio_ = nullptr;

  QWidget *importOptionsContainer_ = nullptr;
  QComboBox *browserSelector_ = nullptr;
  ModernCheckBox *bookmarksCheck_ = nullptr;
  ModernCheckBox *historyCheck_ = nullptr;
  QWidget *htmlFileRow_ = nullptr;
  QLineEdit *htmlPathEdit_ = nullptr;
  QPushButton *browseBtn_ = nullptr;
  QLabel *statusLabel_ = nullptr;
};

}  // namespace dalinira::onboarding
