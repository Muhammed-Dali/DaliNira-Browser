#pragma once

#include "onboarding_step.h"

#include <QFrame>
#include <QHash>
#include <QString>
#include <functional>

class QLabel;
class QRadioButton;

namespace dalinira::onboarding {

// Implemented by BrowserWindow using the browser's existing profile and
// blocker services. The onboarding page owns no duplicate preference keys.
class ISearchPrivacyPreferences {
 public:
  virtual ~ISearchPrivacyPreferences() = default;
  virtual QString searchEngine() const = 0;
  virtual void setSearchEngine(const QString &engine) = 0;
  virtual bool blockerEnabled() const = 0;
  virtual void setBlockerEnabled(bool enabled) = 0;
};

class SearchPrivacyCard final : public QFrame {
  Q_OBJECT
 public:
  explicit SearchPrivacyCard(QWidget *parent = nullptr);
  std::function<void()> clicked;

 protected:
  void mousePressEvent(QMouseEvent *event) override;
};

class SearchPrivacyStep final : public OnboardingStep {
  Q_OBJECT
 public:
  explicit SearchPrivacyStep(ISearchPrivacyPreferences *preferences,
                             QWidget *parent = nullptr);

  QString stepId() const override { return QStringLiteral("search_privacy"); }
  QString stepTitle() const override;
  QString stepSubtitle() const override;
  bool canGoBack() const override { return true; }
  bool canGoNext() const override { return true; }
  bool canSkip() const override { return false; }
  QString nextButtonText() const override;

  void onStepEntering() override;
  void onStepLeaving() override;

  QString selectedSearchEngine() const { return selectedSearchEngine_; }
  bool blockerEnabled() const { return blockerEnabled_; }
  void selectSearchEngine(const QString &engine, bool animated = true);
  void setBlockerEnabled(bool enabled, bool animated = true);

 private:
  SearchPrivacyCard *createSearchCard(const QString &engine, QWidget *parent);
  SearchPrivacyCard *createBlockerCard(bool enabled, QWidget *parent);
  void updateSelection(bool animated);
  void pulse(QWidget *widget, bool animated);

  ISearchPrivacyPreferences *preferences_ = nullptr;
  QString selectedSearchEngine_{QStringLiteral("DuckDuckGo")};
  bool blockerEnabled_ = true;
  QHash<QString, SearchPrivacyCard *> searchCards_;
  QHash<QString, QRadioButton *> searchRadios_;
  SearchPrivacyCard *blockerOnCard_ = nullptr;
  SearchPrivacyCard *blockerOffCard_ = nullptr;
  QRadioButton *blockerOnRadio_ = nullptr;
  QRadioButton *blockerOffRadio_ = nullptr;
};

}  // namespace dalinira::onboarding
