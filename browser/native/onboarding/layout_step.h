#pragma once

#include "onboarding_step.h"
#include "layout/browser_layout.h"

#include <QFrame>
#include <functional>

class QLabel;
class QRadioButton;

namespace dalinira::onboarding {

class LayoutPreviewWidget;

class ClickableLayoutCard : public QFrame {
  Q_OBJECT
 public:
  explicit ClickableLayoutCard(QWidget *parent = nullptr);
  std::function<void()> onClicked;

 protected:
  void mousePressEvent(QMouseEvent *event) override;
};

class LayoutStep final : public OnboardingStep {
  Q_OBJECT

 public:
  explicit LayoutStep(QWidget *parent = nullptr);
  ~LayoutStep() override = default;

  QString stepId() const override { return QStringLiteral("layout"); }
  QString stepTitle() const override;
  QString stepSubtitle() const override;

  bool canGoNext() const override { return true; }
  bool canGoBack() const override { return true; }
  bool canSkip() const override { return false; }

  QString nextButtonText() const override;
  QString backButtonText() const override;

  void onStepEntering() override;
  void onStepLeaving() override;
  void retranslateUi();

  dalinira::layout::BrowserLayoutMode selectedLayoutMode() const { return selectedMode_; }
  void setSelectedLayoutMode(dalinira::layout::BrowserLayoutMode mode);
  dalinira::layout::AppearanceTheme selectedTheme() const { return selectedTheme_; }
  void setSelectedTheme(dalinira::layout::AppearanceTheme theme);

 private:
  void setupUi();
  void updateSelectionVisuals(bool animated);

  LayoutPreviewWidget *previewWidget_ = nullptr;

  ClickableLayoutCard *classicCard_ = nullptr;
  ClickableLayoutCard *leftCard_ = nullptr;
  ClickableLayoutCard *rightCard_ = nullptr;
  ClickableLayoutCard *floatingCard_ = nullptr;

  QRadioButton *classicRadio_ = nullptr;
  QRadioButton *leftRadio_ = nullptr;
  QRadioButton *rightRadio_ = nullptr;
  QRadioButton *floatingRadio_ = nullptr;
  QRadioButton *systemThemeRadio_ = nullptr;
  QRadioButton *lightThemeRadio_ = nullptr;
  QRadioButton *darkThemeRadio_ = nullptr;

  QLabel *classicTitleLabel_ = nullptr;
  QLabel *classicBadgeLabel_ = nullptr;
  QLabel *classicDescLabel_ = nullptr;

  QLabel *leftTitleLabel_ = nullptr;
  QLabel *leftBadgeLabel_ = nullptr;
  QLabel *leftDescLabel_ = nullptr;

  QLabel *rightTitleLabel_ = nullptr;
  QLabel *rightBadgeLabel_ = nullptr;
  QLabel *rightDescLabel_ = nullptr;
  QLabel *floatingTitleLabel_ = nullptr;
  QLabel *floatingBadgeLabel_ = nullptr;
  QLabel *floatingDescLabel_ = nullptr;

  QLabel *themeLabel_ = nullptr;
  QLabel *systemThemeLabel_ = nullptr;
  QLabel *lightThemeLabel_ = nullptr;
  QLabel *darkThemeLabel_ = nullptr;

  dalinira::layout::BrowserLayoutMode selectedMode_{dalinira::layout::BrowserLayoutMode::Classic};
  dalinira::layout::AppearanceTheme selectedTheme_{dalinira::layout::AppearanceTheme::System};
};

} // namespace dalinira::onboarding
