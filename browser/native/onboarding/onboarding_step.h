#pragma once

#include <QWidget>
#include <QString>

namespace dalinira::onboarding {

class OnboardingStep : public QWidget {
  Q_OBJECT

 public:
  explicit OnboardingStep(QWidget *parent = nullptr);
  ~OnboardingStep() override = default;

  virtual QString stepId() const = 0;
  virtual QString stepTitle() const = 0;
  virtual QString stepSubtitle() const { return QString(); }

  virtual bool canGoBack() const { return false; }
  virtual bool canGoNext() const { return true; }
  virtual bool canSkip() const { return false; }

  virtual QString nextButtonText() const;
  virtual QString backButtonText() const;
  virtual QString skipButtonText() const;

  virtual void onStepEntering() {}
  virtual void onStepLeaving() {}

 signals:
  void stepCompleted();
  void stepBackRequested();
  void skipRequested();
  void validationChanged(bool canProceed);
};

}  // namespace dalinira::onboarding
