#include "onboarding_manager.h"

#include "engine/engine_backend.h"

#include <QCoreApplication>
#include <QProcessEnvironment>

namespace dalinira::onboarding {

OnboardingManager &OnboardingManager::instance() {
  static OnboardingManager s_instance;
  return s_instance;
}

OnboardingManager::OnboardingManager(QSettings *customSettings, QObject *parent)
    : QObject(parent), customSettings_(customSettings) {}

void OnboardingManager::setCustomSettings(QSettings *settings) {
  customSettings_ = settings;
}

QSettings *OnboardingManager::settings() const {
  if (customSettings_) {
    return customSettings_;
  }
  if (!ownedSettings_) {
    ownedSettings_ = std::make_unique<QSettings>();
  }
  return ownedSettings_.get();
}

bool OnboardingManager::isOnboardingCompleted() const {
  return settings()->value(QString::fromLatin1(kSettingsCompletedKey), false).toBool();
}

void OnboardingManager::setOnboardingCompleted(bool completed) {
  settings()->setValue(QString::fromLatin1(kSettingsCompletedKey), completed);
  settings()->sync();
}

int OnboardingManager::currentStepIndex() const {
  return settings()->value(QString::fromLatin1(kSettingsCurrentStepKey), 0).toInt();
}

void OnboardingManager::setCurrentStepIndex(int index) {
  settings()->setValue(QString::fromLatin1(kSettingsCurrentStepKey), index);
  settings()->sync();
}

QString OnboardingManager::currentStepId() const {
  return settings()->value(QString::fromLatin1(kSettingsCurrentStepIdKey), QStringLiteral("welcome")).toString();
}

void OnboardingManager::setCurrentStepId(const QString &stepId) {
  settings()->setValue(QString::fromLatin1(kSettingsCurrentStepIdKey), stepId);
  settings()->sync();
}

int OnboardingManager::onboardingVersion() const {
  return settings()->value(QString::fromLatin1(kSettingsVersionKey), kCurrentOnboardingVersion).toInt();
}

void OnboardingManager::setOnboardingVersion(int version) {
  settings()->setValue(QString::fromLatin1(kSettingsVersionKey), version);
  settings()->sync();
}

QDateTime OnboardingManager::completedAt() const {
  return settings()->value(QString::fromLatin1(kSettingsCompletedAtKey)).toDateTime();
}

OnboardingManager::ImportChoice OnboardingManager::importChoice() const {
  const QString val = settings()->value(QString::fromLatin1(kSettingsImportChoiceKey), QStringLiteral("fresh")).toString();
  return (val == QLatin1String("import")) ? ImportChoice::Import : ImportChoice::Fresh;
}

void OnboardingManager::setImportChoice(ImportChoice choice) {
  const QString str = (choice == ImportChoice::Import) ? QStringLiteral("import") : QStringLiteral("fresh");
  settings()->setValue(QString::fromLatin1(kSettingsImportChoiceKey), str);
  settings()->sync();
  emit importChoiceChanged(choice);
}

QString OnboardingManager::importSource() const {
  return settings()->value(QString::fromLatin1(kSettingsImportSourceKey)).toString();
}

void OnboardingManager::setImportSource(const QString &source) {
  settings()->setValue(QString::fromLatin1(kSettingsImportSourceKey), source);
  settings()->sync();
}

bool OnboardingManager::importBookmarksEnabled() const {
  return settings()->value(QString::fromLatin1(kSettingsImportBookmarksKey), true).toBool();
}

void OnboardingManager::setImportBookmarksEnabled(bool enabled) {
  settings()->setValue(QString::fromLatin1(kSettingsImportBookmarksKey), enabled);
  settings()->sync();
}

bool OnboardingManager::importHistoryEnabled() const {
  return settings()->value(QString::fromLatin1(kSettingsImportHistoryKey), true).toBool();
}

void OnboardingManager::setImportHistoryEnabled(bool enabled) {
  settings()->setValue(QString::fromLatin1(kSettingsImportHistoryKey), enabled);
  settings()->sync();
}

bool OnboardingManager::importPasswordsEnabled() const {
  return settings()->value(QString::fromLatin1(kSettingsImportPasswordsKey), false).toBool();
}

void OnboardingManager::setImportPasswordsEnabled(bool enabled) {
  settings()->setValue(QString::fromLatin1(kSettingsImportPasswordsKey), enabled);
  settings()->sync();
}

QString OnboardingManager::importHtmlPath() const {
  return settings()->value(QString::fromLatin1(kSettingsImportHtmlPathKey)).toString();
}

void OnboardingManager::setImportHtmlPath(const QString &path) {
  settings()->setValue(QString::fromLatin1(kSettingsImportHtmlPathKey), path);
  settings()->sync();
}

QString OnboardingManager::selectedEngine() const {
  // Nothing chosen yet: the engine this session runs (an --engine start
  // included), so finishing setup without a choice does not switch engines.
  const QString running = dalinira::engine::EngineBackend::selectedEngineType() == dalinira::engine::EngineType::WebKit
      ? QStringLiteral("webkit") : QStringLiteral("chromium");
  return settings()->value(QString::fromLatin1(kSettingsEngineKey), running).toString().trimmed().toLower();
}

void OnboardingManager::setSelectedEngine(const QString &engine) {
  const QString val = engine.trimmed().toLower();
  settings()->setValue(QString::fromLatin1(kSettingsEngineKey), val);
  settings()->sync();
  emit engineSelectionChanged(val);
}

QString OnboardingManager::selectedLayout() const {
  const QString val = settings()->value(QString::fromLatin1(kSettingsLayoutKey), QStringLiteral("classic")).toString().trimmed().toLower();
  return val.isEmpty() ? QStringLiteral("classic") : val;
}

void OnboardingManager::setSelectedLayout(const QString &layout) {
  const QString val = layout.trimmed().toLower();
  settings()->setValue(QString::fromLatin1(kSettingsLayoutKey), val);
  settings()->sync();
  emit layoutSelectionChanged(val);
}

QString OnboardingManager::selectedTheme() const {
  const QString value = settings()->value(QString::fromLatin1(kSettingsThemeKey),
                                          QStringLiteral("system")).toString().trimmed().toLower();
  return value.isEmpty() ? QStringLiteral("system") : value;
}

void OnboardingManager::setSelectedTheme(const QString &theme) {
  QString value = theme.trimmed().toLower();
  if (value != QLatin1String("light") && value != QLatin1String("dark"))
    value = QStringLiteral("system");
  settings()->setValue(QString::fromLatin1(kSettingsThemeKey), value);
  settings()->sync();
  emit themeSelectionChanged(value);
}

bool OnboardingManager::animationsEnabled() {
  if (qEnvironmentVariableIntValue("DALINIRA_DISABLE_ANIMATIONS") == 1) {
    return false;
  }
  if (qEnvironmentVariableIntValue("DALINIRA_REDUCED_MOTION") == 1) {
    return false;
  }
  const QSettings settings;
  if (settings.value(QStringLiteral("ui/reduceMotion"), false).toBool()) {
    return false;
  }
  if (!settings.value(QStringLiteral("browser/animationsEnabled"), true).toBool()) {
    return false;
  }
  if (!settings.value(QStringLiteral("ui/animationsEnabled"), true).toBool()) {
    return false;
  }
  return true;
}

bool OnboardingManager::shouldShowOnboarding() const {
  return !isOnboardingCompleted();
}

bool OnboardingManager::shouldShow() {
  return instance().shouldShowOnboarding();
}

void OnboardingManager::completeOnboarding() {
  setOnboardingCompleted(true);
  setOnboardingVersion(kCurrentOnboardingVersion);
  settings()->setValue(QString::fromLatin1(kSettingsCompletedAtKey), QDateTime::currentDateTimeUtc());
  settings()->sync();
  emit onboardingCompleted();
}

void OnboardingManager::skipOnboarding() {
  completeOnboarding();
  emit onboardingSkipped();
}

void OnboardingManager::reopenOnboarding() {
  settings()->setValue(QString::fromLatin1(kSettingsCompletedKey), false);
  settings()->setValue(QString::fromLatin1(kSettingsCurrentStepKey), 0);
  settings()->setValue(QString::fromLatin1(kSettingsCurrentStepIdKey), QStringLiteral("welcome"));
  settings()->remove(QString::fromLatin1(kSettingsCompletedAtKey));
  settings()->sync();
  emit onboardingReopened();
}

void OnboardingManager::reset() {
  settings()->remove(QString::fromLatin1(kSettingsCompletedKey));
  settings()->remove(QString::fromLatin1(kSettingsCurrentStepKey));
  settings()->remove(QString::fromLatin1(kSettingsCurrentStepIdKey));
  settings()->remove(QString::fromLatin1(kSettingsCompletedAtKey));
  settings()->remove(QString::fromLatin1(kSettingsImportChoiceKey));
  settings()->remove(QString::fromLatin1(kSettingsImportSourceKey));
  settings()->remove(QString::fromLatin1(kSettingsImportBookmarksKey));
  settings()->remove(QString::fromLatin1(kSettingsImportHistoryKey));
  settings()->remove(QString::fromLatin1(kSettingsImportPasswordsKey));
  settings()->remove(QString::fromLatin1(kSettingsImportHtmlPathKey));
  settings()->setValue(QString::fromLatin1(kSettingsEngineKey), QStringLiteral("chromium"));
  settings()->setValue(QString::fromLatin1(kSettingsLayoutKey), QStringLiteral("classic"));
  settings()->setValue(QString::fromLatin1(kSettingsThemeKey), QStringLiteral("system"));
  settings()->sync();
  emit stateReset();
}

}  // namespace dalinira::onboarding
