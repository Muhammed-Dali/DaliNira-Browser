#pragma once

#include "onboarding_step.h"

class QLabel;
class QPushButton;
class QComboBox;
class QResizeEvent;

namespace dalinira::onboarding {

class WelcomeStep final : public OnboardingStep {
  Q_OBJECT

 public:
  explicit WelcomeStep(QWidget *parent = nullptr);
  ~WelcomeStep() override = default;

  QString stepId() const override { return QStringLiteral("welcome"); }
  QString stepTitle() const override;
  QString stepSubtitle() const override;

  bool canGoBack() const override { return false; }
  bool canGoNext() const override { return true; }
  bool canSkip() const override { return true; }

  QString nextButtonText() const override;
  QString skipButtonText() const override;

  void retranslateUi();

 protected:
  void resizeEvent(QResizeEvent *event) override;

 private:
  struct FeatureCardWidgets {
    QWidget *card = nullptr;
    QLabel *titleLabel = nullptr;
    QLabel *descLabel = nullptr;
  };

  void setupUi();
  // Word-wrapped labels do not grow a box-layout row; size the cards from
  // their text at the current width so no translation is clipped.
  void fitFeatureCards();
  QWidget *createDualEngineCard(const QString &title, const QString &description, FeatureCardWidgets &outWidgets);
  QWidget *createFeatureCard(const QString &title, const QString &description,
                             const QString &svgName, const QColor &accentColor,
                             FeatureCardWidgets &outWidgets);

  QLabel *titleLabel_ = nullptr;
  QLabel *subtitleLabel_ = nullptr;
  QLabel *langLabel_ = nullptr;
  QComboBox *langCombo_ = nullptr;
  bool updatingLangCombo_ = false;

  FeatureCardWidgets cardDualEngine_;
  FeatureCardWidgets cardPrivacy_;
  FeatureCardWidgets cardAudio_;
  FeatureCardWidgets cardPulse_;
  FeatureCardWidgets cardDownload_;

  QPushButton *getStartedBtn_ = nullptr;
  QPushButton *skipBtn_ = nullptr;
};

}  // namespace dalinira::onboarding
