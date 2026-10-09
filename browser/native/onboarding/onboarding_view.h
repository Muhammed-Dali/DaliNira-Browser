#pragma once

#include <QWidget>
#include <QList>
#include <memory>

class QStackedWidget;
class QPushButton;
class QLabel;
class QHBoxLayout;
class QPropertyAnimation;
class QParallelAnimationGroup;

namespace dalinira::onboarding {

class OnboardingStep;
class OnboardingManager;
class IBrowserDataReceiver;
class ISearchPrivacyPreferences;
class AuroraWaveBackground;

}  // namespace dalinira::onboarding

namespace dalinira::core {
class IDefaultBrowserIntegration;
}

namespace dalinira::onboarding {

enum class TransitionDirection {
  Forward,
  Backward,
  Immediate
};

class OnboardingView final : public QWidget {
  Q_OBJECT

 public:
  explicit OnboardingView(IBrowserDataReceiver *dataReceiver = nullptr,
                          ISearchPrivacyPreferences *searchPrivacyPreferences = nullptr,
                          QWidget *parent = nullptr,
                          dalinira::core::IDefaultBrowserIntegration *defaultBrowserIntegration = nullptr);
  ~OnboardingView() override = default;

  void addStep(OnboardingStep *step);
  int stepCount() const;
  int currentStepIndex() const { return currentStepIndex_; }
  OnboardingStep *currentStep() const;
  OnboardingStep *stepAt(int index) const;

  void goToStep(int index, TransitionDirection direction = TransitionDirection::Forward);
  void goToNextStep();
  void goToPreviousStep();

  void complete();
  void skip();

 signals:
  void finished();
  void stepTransitionStarted(int fromIndex, int toIndex);
  void stepTransitionFinished(int stepIndex);

 protected:
  void resizeEvent(QResizeEvent *event) override;

 private:
  void setupUi();
  void updateNavigationState();
  void animateTransition(QWidget *oldWidget, QWidget *newWidget, TransitionDirection direction);

  AuroraWaveBackground *auroraBackground_ = nullptr;
  QStackedWidget *stepStack_ = nullptr;
  QWidget *footerBar_ = nullptr;
  QPushButton *backBtn_ = nullptr;
  QPushButton *skipBtn_ = nullptr;
  QPushButton *nextBtn_ = nullptr;
  QLabel *stepIndicator_ = nullptr;

  QList<OnboardingStep *> steps_;
  int currentStepIndex_ = 0;
  bool isTransitioning_ = false;
};

}  // namespace dalinira::onboarding
