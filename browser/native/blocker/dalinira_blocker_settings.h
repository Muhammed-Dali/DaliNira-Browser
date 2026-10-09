#pragma once

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QSettings>
#include <QRecursiveMutex>
#include <optional>
#include <QStringList>

#include "dalinira_blocker_types.h"

namespace DaliNiraBlockerDefaults {
inline constexpr DaliNiraBlockerMode Mode = DaliNiraBlockerMode::Ideal;
inline constexpr bool ProtectionEnabled = true;
inline constexpr bool AutoReload = true;
inline constexpr bool ShowCount = true;
inline constexpr bool StrictBlock = true;
inline constexpr bool HttpsByDefault = true;
inline constexpr bool FingerprintProtection = true;
inline constexpr bool PopupBlock = true;
inline constexpr bool DeveloperMode = false;
inline constexpr bool AutoUpdateRulesets = false;
inline constexpr bool RulesetSelectionConfigured = false;
}

namespace AdBlockDefaults = DaliNiraBlockerDefaults;

class DaliNiraBlockerSettings final : public QObject {
  Q_OBJECT
 public:
  explicit DaliNiraBlockerSettings(const QString &iniPath, QObject *parent = nullptr);
  ~DaliNiraBlockerSettings() override = default;

  DaliNiraBlockerMode mode() const;
  void setMode(DaliNiraBlockerMode mode);

  bool protectionEnabled() const;
  void setProtectionEnabled(bool enabled);

  bool autoReloadOnModeChange() const;
  void setAutoReloadOnModeChange(bool enable);

  bool showBlockedCountOnToolbar() const;
  void setShowBlockedCountOnToolbar(bool enable);

  bool strictBlock() const;
  void setStrictBlock(bool enable);

  // Top-level http:// navigations go to https:// first (falling back to
  // http:// for a site whose HTTPS fails), as Brave's HTTPS by default.
  bool httpsByDefault() const;
  void setHttpsByDefault(bool enable);

  // Standard fingerprinting protection on every site: canvas, audio, WebGL,
  // WebGPU, locale/language, hardware, font, display/media-query and screen
  // surfaces are reduced or varied per site and session.
  // (A site's blockFingerprinting policy is the strict form.)
  bool fingerprintProtection() const;
  void setFingerprintProtection(bool enable);

  bool popupBlock() const;
  void setPopupBlock(bool enable);

  bool developerMode() const;
  void setDeveloperMode(bool enable);

  bool autoUpdateRulesets() const;
  void setAutoUpdateRulesets(bool enable);

  QStringList customFilters() const;
  void setCustomFilters(const QStringList &filters);

  static QString normalizeSiteHost(const QString &host);
  static std::optional<SitePolicy> findSitePolicy(const QString &host, const QHash<QString, SitePolicy> &policies);
  QHash<QString, SitePolicy> sitePolicies() const;
  SitePolicy sitePolicy(const QString &host) const;
  void setSitePolicy(const QString &host, const SitePolicy &policy);
  void removeSitePolicy(const QString &host);

  QStringList enabledRulesetIds() const;
  bool rulesetSelectionConfigured() const;
  void setEnabledRulesetIds(const QStringList &ids);

  // Backup, Restore & Reset
  QJsonObject exportBackupJson() const;
  bool importBackupJson(const QJsonObject &json);
  void resetToDefaults();

 signals:
  void settingsChanged();
  // Emitted exactly once for each logical change that requires recompiling
  // the network/cosmetic plan.  UI-only and site-policy changes deliberately
  // do not rebuild the engine.
  void filteringPlanChanged();
  void modeChanged(DaliNiraBlockerMode mode);
  void protectionEnabledChanged(bool enabled);
  void toolbarCountVisibilityChanged(bool visible);
  void popupBlockChanged(bool enabled);
  void customFiltersChanged();
  void sitePoliciesChanged();
  void rulesetsChanged();

 private:
  void load();
  void save();
  QString sanitizeHost(const QString &host) const;

  mutable QRecursiveMutex mutex_;
  mutable QSettings settings_;
  DaliNiraBlockerMode mode_ = DaliNiraBlockerDefaults::Mode;
  bool protectionEnabled_ = DaliNiraBlockerDefaults::ProtectionEnabled;
  bool autoReload_ = DaliNiraBlockerDefaults::AutoReload;
  bool showCount_ = DaliNiraBlockerDefaults::ShowCount;
  bool strictBlock_ = DaliNiraBlockerDefaults::StrictBlock;
  bool httpsByDefault_ = DaliNiraBlockerDefaults::HttpsByDefault;
  bool fingerprintProtection_ = DaliNiraBlockerDefaults::FingerprintProtection;
  bool popupBlock_ = DaliNiraBlockerDefaults::PopupBlock;
  bool developerMode_ = DaliNiraBlockerDefaults::DeveloperMode;
  bool autoUpdateRulesets_ = DaliNiraBlockerDefaults::AutoUpdateRulesets;
  QStringList customFilters_;
  QHash<QString, SitePolicy> sitePolicies_;
  QStringList enabledRulesetIds_;
  bool rulesetSelectionConfigured_ = DaliNiraBlockerDefaults::RulesetSelectionConfigured;
};

using AdBlockSettings = DaliNiraBlockerSettings;
