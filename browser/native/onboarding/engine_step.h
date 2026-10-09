#pragma once

#include "onboarding_step.h"

#include <QFrame>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>

namespace dalinira::onboarding {

class EngineStep final : public OnboardingStep {
  Q_OBJECT

 public:
  explicit EngineStep(QWidget *parent = nullptr);
  ~EngineStep() override = default;

  QString stepId() const override { return QStringLiteral("engine"); }
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

  QString selectedEngine() const { return selectedEngine_; }
  void selectEngine(const QString &engine, bool animate = true);

  void retranslateUi();

 private:
  void setupUi();
  void updateCardStyles();
  void updateExplanation(bool animate = true);

  QString selectedEngine_ = QStringLiteral("chromium");

  QLabel *titleLabel_ = nullptr;
  QLabel *subtitleLabel_ = nullptr;

  QFrame *chromiumCard_ = nullptr;
  QFrame *webkitCard_ = nullptr;

  QRadioButton *chromiumRadio_ = nullptr;
  QRadioButton *webkitRadio_ = nullptr;

  QLabel *chromiumTitle_ = nullptr;
  QLabel *chromiumBadge_ = nullptr;
  QLabel *webkitTitle_ = nullptr;
  QLabel *webkitBadge_ = nullptr;

  QFrame *explanationBanner_ = nullptr;
  QLabel *explanationTitle_ = nullptr;
  QLabel *explanationDesc_ = nullptr;
  QLabel *noteLabel_ = nullptr;

  QGraphicsOpacityEffect *bannerOpacityEffect_ = nullptr;
};

}  // namespace dalinira::onboarding
