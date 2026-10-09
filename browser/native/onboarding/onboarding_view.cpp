#include "onboarding_view.h"
#include "aurora_wave_background.h"
#include "onboarding_manager.h"
#include "onboarding_step.h"
#include "welcome_step.h"
#include "import_step.h"
#include "engine_step.h"
#include "layout_step.h"
#include "search_privacy_step.h"
#include "ready_step.h"
#include "i18n/i18n.h"
#include "i18n/language_manager.h"

#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QParallelAnimationGroup>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace dalinira::onboarding {

OnboardingView::OnboardingView(IBrowserDataReceiver *dataReceiver,
                               ISearchPrivacyPreferences *searchPrivacyPreferences,
                               QWidget *parent,
                               dalinira::core::IDefaultBrowserIntegration *defaultBrowserIntegration)
    : QWidget(parent) {
  setupUi();

  // Register default steps:
  // Step 0: Welcome
  addStep(new WelcomeStep(this));
  // Step 1: Import Data
  addStep(new ImportStep(dataReceiver, this));
  // Step 2: Choose Your Engine
  addStep(new EngineStep(this));
  // Step 3: Choose Layout & Panels
  addStep(new LayoutStep(this));
  // Step 4: Search & Privacy (uses the live profile/blocker services)
  addStep(new SearchPrivacyStep(searchPrivacyPreferences, this));
  // Step 5: Review choices and optionally ask the operating system to make
  // DaliNira the default browser.
  addStep(new ReadyStep(searchPrivacyPreferences, defaultBrowserIntegration, this));

  // Check saved state for safe resume
  const int savedIndex = OnboardingManager::instance().currentStepIndex();
  if (savedIndex > 0 && savedIndex < steps_.size()) {
    goToStep(savedIndex, TransitionDirection::Immediate);
  } else {
    goToStep(0, TransitionDirection::Immediate);
  }

  connect(&dalinira::i18n::LanguageManager::instance(), &dalinira::i18n::LanguageManager::languageChanged,
          this, [this] {
    for (auto *step : steps_) {
      if (auto *ws = qobject_cast<WelcomeStep *>(step)) ws->retranslateUi();
      else if (auto *is = qobject_cast<ImportStep *>(step)) is->retranslateUi();
      else if (auto *es = qobject_cast<EngineStep *>(step)) es->retranslateUi();
      else if (auto *ls = qobject_cast<LayoutStep *>(step)) ls->retranslateUi();
      else if (auto *rs = qobject_cast<ReadyStep *>(step)) rs->retranslateUi();
    }
    updateNavigationState();
  });
}

void OnboardingView::setupUi() {
  setObjectName(QStringLiteral("onboarding-view"));
  setStyleSheet(QStringLiteral(
      "#onboarding-view {"
      "  background: transparent;"
      "}"));

  auroraBackground_ = new AuroraWaveBackground(this);
  auroraBackground_->lower();

  auto *rootLayout = new QVBoxLayout(this);
  rootLayout->setContentsMargins(0, 0, 0, 0);
  rootLayout->setSpacing(0);

  stepStack_ = new QStackedWidget(this);
  stepStack_->setObjectName(QStringLiteral("onboarding-step-stack"));
  stepStack_->setStyleSheet(QStringLiteral(
      "#onboarding-step-stack {"
      "  background: transparent;"
      "}"));
  rootLayout->addWidget(stepStack_, 1);

  // Footer navigation bar (for multi-step flows)
  footerBar_ = new QWidget(this);
  footerBar_->setObjectName(QStringLiteral("onboarding-footer-bar"));
  footerBar_->setFixedHeight(64);
  footerBar_->setStyleSheet(QStringLiteral(
      "#onboarding-footer-bar {"
      "  background: rgba(11, 17, 28, 0.88);"
      "  border-top: 1px solid rgba(255, 255, 255, 0.10);"
      "  padding: 0 32px;"
      "}"));

  auto *footerLayout = new QHBoxLayout(footerBar_);
  footerLayout->setContentsMargins(32, 0, 32, 0);

  backBtn_ = new QPushButton(dalinira::i18n::I18n::text(QStringLiteral("onboarding.back")), footerBar_);
  backBtn_->setObjectName(QStringLiteral("onboarding-nav-back-btn"));
  backBtn_->setCursor(Qt::PointingHandCursor);
  backBtn_->setStyleSheet(QStringLiteral(
      "#onboarding-nav-back-btn {"
      "  background: rgba(255, 255, 255, 0.08);"
      "  color: #f1f5f9;"
      "  font-size: 13px;"
      "  font-weight: 600;"
      "  border: 1px solid rgba(255, 255, 255, 0.15);"
      "  border-radius: 8px;"
      "  padding: 8px 18px;"
      "}"
      "#onboarding-nav-back-btn:hover {"
      "  background: rgba(255, 255, 255, 0.14);"
      "}"));
  connect(backBtn_, &QPushButton::clicked, this, &OnboardingView::goToPreviousStep);
  footerLayout->addWidget(backBtn_);

  footerLayout->addStretch();

  stepIndicator_ = new QLabel(footerBar_);
  stepIndicator_->setObjectName(QStringLiteral("onboarding-step-indicator"));
  stepIndicator_->setStyleSheet(QStringLiteral(
      "#onboarding-step-indicator {"
      "  color: #cbd5e1;"
      "  font-size: 13px;"
      "  font-weight: 500;"
      "  background: transparent;"
      "}"));
  footerLayout->addWidget(stepIndicator_);

  footerLayout->addStretch();

  skipBtn_ = new QPushButton(dalinira::i18n::I18n::text(QStringLiteral("onboarding.skip")), footerBar_);
  skipBtn_->setObjectName(QStringLiteral("onboarding-nav-skip-btn"));
  skipBtn_->setCursor(Qt::PointingHandCursor);
  skipBtn_->setStyleSheet(QStringLiteral(
      "#onboarding-nav-skip-btn {"
      "  background: transparent;"
      "  color: #cbd5e1;"
      "  font-size: 13px;"
      "  font-weight: 500;"
      "  border: none;"
      "  padding: 8px 16px;"
      "}"
      "#onboarding-nav-skip-btn:hover {"
      "  color: #ffffff;"
      "  text-decoration: underline;"
      "}"));
  connect(skipBtn_, &QPushButton::clicked, this, &OnboardingView::skip);
  footerLayout->addWidget(skipBtn_);

  nextBtn_ = new QPushButton(dalinira::i18n::I18n::text(QStringLiteral("onboarding.next")), footerBar_);
  nextBtn_->setObjectName(QStringLiteral("onboarding-nav-next-btn"));
  nextBtn_->setCursor(Qt::PointingHandCursor);
  nextBtn_->setStyleSheet(QStringLiteral(
      "#onboarding-nav-next-btn {"
      "  background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #2563eb, stop:1 #06b6d4);"
      "  color: #ffffff;"
      "  font-size: 13px;"
      "  font-weight: 700;"
      "  border: none;"
      "  border-radius: 8px;"
      "  padding: 8px 24px;"
      "}"
      "#onboarding-nav-next-btn:hover {"
      "  background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #3b82f6, stop:1 #22d3ee);"
      "}"));
  connect(nextBtn_, &QPushButton::clicked, this, &OnboardingView::goToNextStep);
  footerLayout->addWidget(nextBtn_);

  rootLayout->addWidget(footerBar_);
}

void OnboardingView::resizeEvent(QResizeEvent *event) {
  QWidget::resizeEvent(event);
  if (auroraBackground_) {
    auroraBackground_->setGeometry(rect());
  }
}

void OnboardingView::addStep(OnboardingStep *step) {
  if (!step) return;

  steps_.append(step);
  stepStack_->addWidget(step);

  connect(step, &OnboardingStep::stepCompleted, this, &OnboardingView::goToNextStep);
  connect(step, &OnboardingStep::stepBackRequested, this, &OnboardingView::goToPreviousStep);
  connect(step, &OnboardingStep::skipRequested, this, &OnboardingView::skip);
  connect(step, &OnboardingStep::validationChanged, this, [this](bool canProceed) {
    if (nextBtn_) nextBtn_->setEnabled(canProceed);
  });

  updateNavigationState();
}

int OnboardingView::stepCount() const {
  return steps_.size();
}

OnboardingStep *OnboardingView::currentStep() const {
  if (currentStepIndex_ >= 0 && currentStepIndex_ < steps_.size()) {
    return steps_[currentStepIndex_];
  }
  return nullptr;
}

OnboardingStep *OnboardingView::stepAt(int index) const {
  if (index >= 0 && index < steps_.size()) {
    return steps_[index];
  }
  return nullptr;
}

void OnboardingView::goToStep(int index, TransitionDirection direction) {
  if (index < 0 || index >= steps_.size()) return;
  if (index == currentStepIndex_ && stepStack_->currentIndex() == index) return;

  const int previousIndex = currentStepIndex_;
  OnboardingStep *oldStep = currentStep();
  if (oldStep) {
    oldStep->onStepLeaving();
  }

  emit stepTransitionStarted(previousIndex, index);

  currentStepIndex_ = index;
  OnboardingStep *newStep = steps_[index];
  newStep->onStepEntering();

  // Persist current step state for safe resume
  OnboardingManager::instance().setCurrentStepIndex(index);
  OnboardingManager::instance().setCurrentStepId(newStep->stepId());

  if (direction == TransitionDirection::Immediate || !OnboardingManager::animationsEnabled()) {
    stepStack_->setCurrentIndex(index);
    updateNavigationState();
    emit stepTransitionFinished(index);
  } else {
    animateTransition(oldStep, newStep, direction);
  }
}

void OnboardingView::goToNextStep() {
  if (currentStepIndex_ + 1 < steps_.size()) {
    goToStep(currentStepIndex_ + 1, TransitionDirection::Forward);
  } else {
    complete();
  }
}

void OnboardingView::goToPreviousStep() {
  if (currentStepIndex_ > 0) {
    goToStep(currentStepIndex_ - 1, TransitionDirection::Backward);
  }
}

void OnboardingView::complete() {
  OnboardingManager::instance().completeOnboarding();
  emit finished();
}

void OnboardingView::skip() {
  OnboardingManager::instance().skipOnboarding();
  emit finished();
}

void OnboardingView::updateNavigationState() {
  // If on Welcome step (index 0) or only 1 step, hide footer bar because Welcome has its own hero CTA
  if (currentStepIndex_ == 0 || steps_.size() <= 1) {
    footerBar_->hide();
    return;
  }

  footerBar_->show();
  OnboardingStep *step = currentStep();
  if (!step) return;

  backBtn_->setVisible(step->canGoBack());
  backBtn_->setText(step->backButtonText());

  skipBtn_->setVisible(step->canSkip());
  skipBtn_->setText(step->skipButtonText());

  nextBtn_->setVisible(step->canGoNext());
  nextBtn_->setText(step->nextButtonText());

  const QString counterText = QStringLiteral("%1 / %2")
      .arg(currentStepIndex_ + 1)
      .arg(steps_.size());
  stepIndicator_->setText(counterText);
}

void OnboardingView::animateTransition(QWidget *oldWidget, QWidget *newWidget, TransitionDirection direction) {
  Q_UNUSED(direction);
  if (!newWidget) {
    stepStack_->setCurrentIndex(currentStepIndex_);
    updateNavigationState();
    emit stepTransitionFinished(currentStepIndex_);
    return;
  }

  isTransitioning_ = true;
  stepStack_->setCurrentWidget(newWidget);
  updateNavigationState();

  auto *opacityEffect = new QGraphicsOpacityEffect(newWidget);
  newWidget->setGraphicsEffect(opacityEffect);

  auto *anim = new QPropertyAnimation(opacityEffect, "opacity", newWidget);
  anim->setDuration(200);
  anim->setStartValue(0.0);
  anim->setEndValue(1.0);
  anim->setEasingCurve(QEasingCurve::OutCubic);

  connect(anim, &QPropertyAnimation::finished, this, [this, newWidget, anim] {
    newWidget->setGraphicsEffect(nullptr);
    anim->deleteLater();
    isTransitioning_ = false;
    emit stepTransitionFinished(currentStepIndex_);
  });

  anim->start();
}

}  // namespace dalinira::onboarding
