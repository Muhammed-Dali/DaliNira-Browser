#pragma once

#include <QDateTime>
#include <QObject>
#include <QSettings>
#include <QString>
#include <memory>

namespace dalinira::onboarding {

class OnboardingManager final : public QObject {
  Q_OBJECT

 public:
  static constexpr auto kSettingsCompletedKey = "onboarding/completed";
  static constexpr auto kSettingsCurrentStepKey = "onboarding/current_step";
  static constexpr auto kSettingsCurrentStepIdKey = "onboarding/current_step_id";
  static constexpr auto kSettingsVersionKey = "onboarding/version";
  static constexpr auto kSettingsCompletedAtKey = "onboarding/completed_at";
  static constexpr auto kSettingsImportChoiceKey = "onboarding/import_choice";
  static constexpr auto kSettingsImportSourceKey = "onboarding/import_source";
  static constexpr auto kSettingsImportBookmarksKey = "onboarding/import_bookmarks";
  static constexpr auto kSettingsImportHistoryKey = "onboarding/import_history";
  static constexpr auto kSettingsImportPasswordsKey = "onboarding/import_passwords";
  static constexpr auto kSettingsImportHtmlPathKey = "onboarding/import_html_path";
  static constexpr auto kSettingsEngineKey = "browser/engine";
  static constexpr int kCurrentOnboardingVersion = 1;

  enum class ImportChoice {
    Fresh,
    Import
  };

  static OnboardingManager &instance();

  explicit OnboardingManager(QSettings *customSettings = nullptr, QObject *parent = nullptr);
  ~OnboardingManager() override = default;

  // Custom settings injection for tests
  void setCustomSettings(QSettings *settings);

  // Status queries
  bool isOnboardingCompleted() const;
  void setOnboardingCompleted(bool completed);

  int currentStepIndex() const;
  void setCurrentStepIndex(int index);

  QString currentStepId() const;
  void setCurrentStepId(const QString &stepId);

  int onboardingVersion() const;
  void setOnboardingVersion(int version);

  QDateTime completedAt() const;

  // Import choices & options (Step 2)
  ImportChoice importChoice() const;
  void setImportChoice(ImportChoice choice);

  QString importSource() const;
  void setImportSource(const QString &source);

  bool importBookmarksEnabled() const;
  void setImportBookmarksEnabled(bool enabled);

  bool importHistoryEnabled() const;
  void setImportHistoryEnabled(bool enabled);

  bool importPasswordsEnabled() const;
  void setImportPasswordsEnabled(bool enabled);

  QString importHtmlPath() const;
  void setImportHtmlPath(const QString &path);

  // Engine choice (Step 3) - backed by DaliNira's existing preference system
  QString selectedEngine() const;
  void setSelectedEngine(const QString &engine);

  // Layout choice (Step 4) - backed by DaliNira's existing preference system
  static constexpr auto kSettingsLayoutKey = "browser/layout";
  static constexpr auto kSettingsThemeKey = "appearance/theme";
  QString selectedLayout() const;
  void setSelectedLayout(const QString &layout);
  QString selectedTheme() const;
  void setSelectedTheme(const QString &theme);

  // Motion and accessibility
  static bool animationsEnabled();

  // Startup decision
  bool shouldShowOnboarding() const;
  static bool shouldShow();

  // Workflow actions
  void completeOnboarding();
  void skipOnboarding();
  void reopenOnboarding();
  void reset();

 signals:
  void onboardingCompleted();
  void onboardingSkipped();
  void onboardingReopened();
  void currentStepChanged(int stepIndex, const QString &stepId);
  void importChoiceChanged(ImportChoice choice);
  void engineSelectionChanged(const QString &engine);
  void layoutSelectionChanged(const QString &layout);
  void themeSelectionChanged(const QString &theme);
  void stateReset();

 private:
  QSettings *settings() const;

  QSettings *customSettings_ = nullptr;
  mutable std::unique_ptr<QSettings> ownedSettings_;
};

}  // namespace dalinira::onboarding
