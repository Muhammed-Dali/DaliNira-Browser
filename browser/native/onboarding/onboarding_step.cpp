#include "onboarding_step.h"
#include "i18n/i18n.h"

namespace dalinira::onboarding {

OnboardingStep::OnboardingStep(QWidget *parent) : QWidget(parent) {}

QString OnboardingStep::nextButtonText() const {
  return dalinira::i18n::I18n::text(QStringLiteral("onboarding.next"));
}

QString OnboardingStep::backButtonText() const {
  return dalinira::i18n::I18n::text(QStringLiteral("onboarding.back"));
}

QString OnboardingStep::skipButtonText() const {
  return dalinira::i18n::I18n::text(QStringLiteral("onboarding.skip"));
}

}  // namespace dalinira::onboarding
