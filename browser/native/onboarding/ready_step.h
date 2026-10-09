#pragma once

#include "onboarding_step.h"
#include "core/default_browser_integration.h"

class QLabel;
class QPushButton;
class QWidget;

namespace dalinira::onboarding {

class ISearchPrivacyPreferences;

class ReadyStep final : public OnboardingStep {
  Q_OBJECT
 public:
  ReadyStep(ISearchPrivacyPreferences *searchPrivacyPreferences,
            dalinira::core::IDefaultBrowserIntegration *defaultBrowserIntegration,
            QWidget *parent = nullptr);

  QString stepId() const override { return QStringLiteral("ready"); }
  QString stepTitle() const override;
  QString stepSubtitle() const override;
  bool canGoBack() const override { return true; }
  bool canGoNext() const override { return false; }
  bool canSkip() const override { return false; }

  void onStepEntering() override;
  void retranslateUi();

  QString engineSummary() const { return engineSummary_; }
  QString layoutThemeSummary() const { return layoutThemeSummary_; }
  QString searchSummary() const { return searchSummary_; }
  QString privacySummary() const { return privacySummary_; }

  void chooseMakeDefault();
  void chooseNotNow();

 signals:
  void defaultBrowserFinished(dalinira::core::DefaultBrowserResult result);

 private:
  void refreshSummary();
  void finishWithResult(dalinira::core::DefaultBrowserResult result);

  ISearchPrivacyPreferences *searchPrivacyPreferences_ = nullptr;
  dalinira::core::IDefaultBrowserIntegration *defaultBrowserIntegration_ = nullptr;
  QWidget *summaryGrid_ = nullptr;

  QLabel *titleLabel_ = nullptr;
  QLabel *subtitleLabel_ = nullptr;

  QLabel *engineHeading_ = nullptr;
  QLabel *engineValue_ = nullptr;
  QLabel *engineIconLabel_ = nullptr;

  QLabel *layoutThemeHeading_ = nullptr;
  QLabel *layoutThemeValue_ = nullptr;
  QLabel *layoutThemeIconLabel_ = nullptr;

  QLabel *searchHeading_ = nullptr;
  QLabel *searchValue_ = nullptr;
  QLabel *searchIconLabel_ = nullptr;

  QLabel *privacyHeading_ = nullptr;
  QLabel *privacyValue_ = nullptr;
  QLabel *privacyIconLabel_ = nullptr;

  QLabel *statusLabel_ = nullptr;
  QPushButton *makeDefaultButton_ = nullptr;
  QPushButton *notNowButton_ = nullptr;
  QString engineSummary_;
  QString layoutThemeSummary_;
  QString searchSummary_;
  QString privacySummary_;
  bool finishing_ = false;
};

}  // namespace dalinira::onboarding
