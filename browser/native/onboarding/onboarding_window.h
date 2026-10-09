#pragma once

#include <QWidget>

class QCloseEvent;
class QResizeEvent;
class QToolButton;

namespace dalinira::core {
class IDefaultBrowserIntegration;
}

namespace dalinira::onboarding {

class OnboardingView;
class IBrowserDataReceiver;
class ISearchPrivacyPreferences;

class OnboardingWindow final : public QWidget {
  Q_OBJECT

 public:
  explicit OnboardingWindow(IBrowserDataReceiver *dataReceiver = nullptr,
                            ISearchPrivacyPreferences *searchPrivacyPreferences = nullptr,
                            dalinira::core::IDefaultBrowserIntegration *defaultBrowser = nullptr,
                            QWidget *parent = nullptr);
  ~OnboardingWindow() override = default;

  OnboardingView *view() const { return onboardingView_; }

 signals:
  void onboardingFinished();
  void onboardingDismissed();

 protected:
  void closeEvent(QCloseEvent *event) override;
  void resizeEvent(QResizeEvent *event) override;

 private:
  void setupUi();

  IBrowserDataReceiver *dataReceiver_ = nullptr;
  ISearchPrivacyPreferences *searchPrivacyPreferences_ = nullptr;
  dalinira::core::IDefaultBrowserIntegration *defaultBrowser_ = nullptr;

  OnboardingView *onboardingView_ = nullptr;
  QToolButton *closeButton_ = nullptr;
  bool completedOrSkipped_ = false;
};

}  // namespace dalinira::onboarding
