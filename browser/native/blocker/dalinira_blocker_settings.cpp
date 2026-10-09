#include <QMutexLocker>
#include <libpsl.h>
#include <QHostAddress>
#include "dalinira_blocker_settings.h"

#include <QJsonArray>
#include <QUrl>

namespace {
constexpr char kFormatName[] = "dalinira-blocker-settings";
constexpr int kFormatVersion = 1;
}

DaliNiraBlockerSettings::DaliNiraBlockerSettings(const QString &iniPath, QObject *parent)
    : QObject(parent), settings_(iniPath, QSettings::IniFormat) {
  load();
}

DaliNiraBlockerMode DaliNiraBlockerSettings::mode() const {
  QMutexLocker locker(&mutex_); return mode_; }

void DaliNiraBlockerSettings::setMode(DaliNiraBlockerMode mode) {
  QMutexLocker locker(&mutex_);
  if (mode_ == mode && (settings_.contains(QStringLiteral("blocker/mode")) || settings_.contains(QStringLiteral("adblock/mode")))) return;
  mode_ = mode;
  settings_.setValue(QStringLiteral("blocker/mode"), modeToString(mode_));
  settings_.setValue(QStringLiteral("adblock/mode"), modeToString(mode_));
  settings_.sync();
  emit modeChanged(mode_);
  emit filteringPlanChanged();
  emit settingsChanged();
}

bool DaliNiraBlockerSettings::protectionEnabled() const {
  QMutexLocker locker(&mutex_); return protectionEnabled_; }

void DaliNiraBlockerSettings::setProtectionEnabled(bool enabled) {
  QMutexLocker locker(&mutex_);
  if (protectionEnabled_ == enabled &&
      (settings_.contains(QStringLiteral("blocker/protectionEnabled")) || settings_.contains(QStringLiteral("adblock/protectionEnabled")))) return;
  protectionEnabled_ = enabled;
  settings_.setValue(QStringLiteral("blocker/protectionEnabled"), protectionEnabled_);
  settings_.setValue(QStringLiteral("adblock/protectionEnabled"), protectionEnabled_);
  settings_.sync();
  emit protectionEnabledChanged(protectionEnabled_);
  emit settingsChanged();
}

bool DaliNiraBlockerSettings::autoReloadOnModeChange() const {
  QMutexLocker locker(&mutex_); return autoReload_; }

void DaliNiraBlockerSettings::setAutoReloadOnModeChange(bool enable) {
  QMutexLocker locker(&mutex_);
  if (autoReload_ == enable && (settings_.contains(QStringLiteral("blocker/autoReload")) || settings_.contains(QStringLiteral("adblock/autoReload")))) return;
  autoReload_ = enable;
  settings_.setValue(QStringLiteral("blocker/autoReload"), autoReload_);
  settings_.setValue(QStringLiteral("adblock/autoReload"), autoReload_);
  settings_.sync();
  emit settingsChanged();
}

bool DaliNiraBlockerSettings::showBlockedCountOnToolbar() const {
  QMutexLocker locker(&mutex_); return showCount_; }

void DaliNiraBlockerSettings::setShowBlockedCountOnToolbar(bool enable) {
  QMutexLocker locker(&mutex_);
  if (showCount_ == enable && (settings_.contains(QStringLiteral("blocker/showBlockedCount")) || settings_.contains(QStringLiteral("adblock/showBlockedCount")))) return;
  showCount_ = enable;
  settings_.setValue(QStringLiteral("blocker/showBlockedCount"), showCount_);
  settings_.setValue(QStringLiteral("adblock/showBlockedCount"), showCount_);
  settings_.sync();
  emit toolbarCountVisibilityChanged(showCount_);
  emit settingsChanged();
}

bool DaliNiraBlockerSettings::strictBlock() const {
  QMutexLocker locker(&mutex_); return strictBlock_; }

void DaliNiraBlockerSettings::setStrictBlock(bool enable) {
  QMutexLocker locker(&mutex_);
  if (strictBlock_ == enable && (settings_.contains(QStringLiteral("blocker/strictBlock")) || settings_.contains(QStringLiteral("adblock/strictBlock")))) return;
  strictBlock_ = enable;
  settings_.setValue(QStringLiteral("blocker/strictBlock"), strictBlock_);
  settings_.setValue(QStringLiteral("adblock/strictBlock"), strictBlock_);
  settings_.sync();
  emit filteringPlanChanged();
  emit settingsChanged();
}

bool DaliNiraBlockerSettings::httpsByDefault() const {
  QMutexLocker locker(&mutex_); return httpsByDefault_; }

void DaliNiraBlockerSettings::setHttpsByDefault(bool enable) {
  QMutexLocker locker(&mutex_);
  if (httpsByDefault_ == enable && settings_.contains(QStringLiteral("blocker/httpsByDefault"))) return;
  httpsByDefault_ = enable;
  settings_.setValue(QStringLiteral("blocker/httpsByDefault"), httpsByDefault_);
  settings_.sync();
  emit settingsChanged();
}

bool DaliNiraBlockerSettings::fingerprintProtection() const {
  QMutexLocker locker(&mutex_); return fingerprintProtection_; }

void DaliNiraBlockerSettings::setFingerprintProtection(bool enable) {
  QMutexLocker locker(&mutex_);
  if (fingerprintProtection_ == enable && settings_.contains(QStringLiteral("blocker/fingerprintProtection"))) return;
  fingerprintProtection_ = enable;
  settings_.setValue(QStringLiteral("blocker/fingerprintProtection"), fingerprintProtection_);
  settings_.sync();
  emit settingsChanged();
}

bool DaliNiraBlockerSettings::popupBlock() const {
  QMutexLocker locker(&mutex_); return popupBlock_; }

void DaliNiraBlockerSettings::setPopupBlock(bool enable) {
  QMutexLocker locker(&mutex_);
  if (popupBlock_ == enable && (settings_.contains(QStringLiteral("blocker/popupBlock")) || settings_.contains(QStringLiteral("adblock/popupBlock")))) return;
  popupBlock_ = enable;
  settings_.setValue(QStringLiteral("blocker/popupBlock"), popupBlock_);
  settings_.setValue(QStringLiteral("adblock/popupBlock"), popupBlock_);
  settings_.sync();
  emit popupBlockChanged(popupBlock_);
  emit settingsChanged();
}

bool DaliNiraBlockerSettings::developerMode() const {
  QMutexLocker locker(&mutex_); return developerMode_; }

void DaliNiraBlockerSettings::setDeveloperMode(bool enable) {
  QMutexLocker locker(&mutex_);
  if (developerMode_ == enable && (settings_.contains(QStringLiteral("blocker/developerMode")) || settings_.contains(QStringLiteral("adblock/developerMode")))) return;
  developerMode_ = enable;
  settings_.setValue(QStringLiteral("blocker/developerMode"), developerMode_);
  settings_.setValue(QStringLiteral("adblock/developerMode"), developerMode_);
  settings_.sync();
  emit settingsChanged();
}

bool DaliNiraBlockerSettings::autoUpdateRulesets() const {
  QMutexLocker locker(&mutex_); return autoUpdateRulesets_; }

void DaliNiraBlockerSettings::setAutoUpdateRulesets(bool enable) {
  QMutexLocker locker(&mutex_);
  if (autoUpdateRulesets_ == enable && (settings_.contains(QStringLiteral("blocker/autoUpdateRulesets")) || settings_.contains(QStringLiteral("adblock/autoUpdateRulesets")))) return;
  autoUpdateRulesets_ = enable;
  settings_.setValue(QStringLiteral("blocker/autoUpdateRulesets"), autoUpdateRulesets_);
  settings_.setValue(QStringLiteral("adblock/autoUpdateRulesets"), autoUpdateRulesets_);
  settings_.sync();
  emit settingsChanged();
}

QStringList DaliNiraBlockerSettings::customFilters() const {
  QMutexLocker locker(&mutex_); return customFilters_; }

void DaliNiraBlockerSettings::setCustomFilters(const QStringList &filters) {
  QMutexLocker locker(&mutex_);
  customFilters_ = filters;
  settings_.setValue(QStringLiteral("blocker/customFilters"), customFilters_);
  settings_.setValue(QStringLiteral("adblock/customFilters"), customFilters_);
  settings_.sync();
  emit customFiltersChanged();
  emit filteringPlanChanged();
  emit settingsChanged();
}

QHash<QString, SitePolicy> DaliNiraBlockerSettings::sitePolicies() const {
  QMutexLocker locker(&mutex_); return sitePolicies_; }

QString DaliNiraBlockerSettings::normalizeSiteHost(const QString &host) {
  const QString input = host.trimmed();
  if (input.isEmpty() || input.size() > 2048) return {};
  const QHostAddress directAddress(input);
  if (!directAddress.isNull()) return directAddress.toString();
  QUrl url(input.contains(QStringLiteral("://")) ? input : QStringLiteral("https://") + input,
           QUrl::StrictMode);
  if (!url.isValid() || (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https")) ||
      !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment() ||
      (!url.path().isEmpty() && url.path() != QLatin1String("/"))) return {};
  const QHostAddress address(url.host());
  if (!address.isNull()) return address.toString();
  QString clean = QString::fromLatin1(QUrl::toAce(url.host())).toLower();
  if (clean.endsWith(QLatin1Char('.'))) clean.chop(1);
  if (clean.startsWith(QStringLiteral("www."))) clean.remove(0, 4);
  if (clean.isEmpty() || clean.size() > 253 || clean.contains(QStringLiteral(".."))) return {};
  if (psl_is_public_suffix(psl_builtin(), clean.toUtf8().constData())) return {};
  return clean;
}

QString DaliNiraBlockerSettings::sanitizeHost(const QString &host) const {
  return normalizeSiteHost(host);
}

std::optional<SitePolicy> DaliNiraBlockerSettings::findSitePolicy(const QString &rawHost, const QHash<QString, SitePolicy> &policies) {
  QString candidate = normalizeSiteHost(rawHost);
  if (candidate.isEmpty()) return std::nullopt;
  const bool address = !QHostAddress(candidate).isNull();
  while (!candidate.isEmpty()) {
    const auto it = policies.constFind(candidate);
    if (it != policies.constEnd()) return it.value();
    if (address) break; // Never walk DNS parents of an IP literal.
    const int dot = candidate.indexOf(QLatin1Char('.'));
    if (dot < 0 || psl_is_public_suffix(psl_builtin(), candidate.mid(dot + 1).toUtf8().constData())) break;
    candidate.remove(0, dot + 1);
  }
  return std::nullopt;
}

SitePolicy DaliNiraBlockerSettings::sitePolicy(const QString &rawHost) const {
  QMutexLocker locker(&mutex_);
  return findSitePolicy(rawHost, sitePolicies_).value_or(SitePolicy{});
}

void DaliNiraBlockerSettings::setSitePolicy(const QString &rawHost, const SitePolicy &policy) {
  QMutexLocker locker(&mutex_);
  const QString clean = sanitizeHost(rawHost);
  if (clean.isEmpty()) return;
  sitePolicies_[clean] = policy;
  save();
  emit sitePoliciesChanged();
  emit settingsChanged();
}

void DaliNiraBlockerSettings::removeSitePolicy(const QString &rawHost) {
  QMutexLocker locker(&mutex_);
  const QString clean = sanitizeHost(rawHost);
  if (clean.isEmpty() || !sitePolicies_.contains(clean)) return;
  sitePolicies_.remove(clean);
  save();
  emit sitePoliciesChanged();
  emit settingsChanged();
}

QStringList DaliNiraBlockerSettings::enabledRulesetIds() const {
  QMutexLocker locker(&mutex_); return enabledRulesetIds_; }
bool DaliNiraBlockerSettings::rulesetSelectionConfigured() const {
  QMutexLocker locker(&mutex_); return rulesetSelectionConfigured_; }

void DaliNiraBlockerSettings::setEnabledRulesetIds(const QStringList &ids) {
  QMutexLocker locker(&mutex_);
  enabledRulesetIds_ = ids;
  rulesetSelectionConfigured_ = true;
  settings_.setValue(QStringLiteral("blocker/enabledRulesetIds"), enabledRulesetIds_);
  settings_.setValue(QStringLiteral("blocker/rulesetSelectionConfigured"), true);
  settings_.setValue(QStringLiteral("adblock/enabledRulesetIds"), enabledRulesetIds_);
  settings_.setValue(QStringLiteral("adblock/rulesetSelectionConfigured"), true);
  settings_.sync();
  emit rulesetsChanged();
  emit filteringPlanChanged();
  emit settingsChanged();
}

void DaliNiraBlockerSettings::load() {
  QMutexLocker locker(&mutex_);
  auto readSetting = [this](const QString &suffix, const QVariant &def) {
    if (settings_.contains(QStringLiteral("blocker/") + suffix)) {
      return settings_.value(QStringLiteral("blocker/") + suffix, def);
    }
    return settings_.value(QStringLiteral("adblock/") + suffix, def);
  };

  if (settings_.contains(QStringLiteral("blocker/mode")) || settings_.contains(QStringLiteral("adblock/mode"))) {
    mode_ = modeFromString(readSetting(QStringLiteral("mode"), QStringLiteral("ideal")).toString());
  } else {
    mode_ = DaliNiraBlockerDefaults::Mode;
  }
  protectionEnabled_ = readSetting(QStringLiteral("protectionEnabled"),
                                   DaliNiraBlockerDefaults::ProtectionEnabled).toBool();
  autoReload_ = readSetting(QStringLiteral("autoReload"), DaliNiraBlockerDefaults::AutoReload).toBool();
  showCount_ = readSetting(QStringLiteral("showBlockedCount"), DaliNiraBlockerDefaults::ShowCount).toBool();
  strictBlock_ = readSetting(QStringLiteral("strictBlock"), DaliNiraBlockerDefaults::StrictBlock).toBool();
  httpsByDefault_ = readSetting(QStringLiteral("httpsByDefault"), DaliNiraBlockerDefaults::HttpsByDefault).toBool();
  fingerprintProtection_ = readSetting(QStringLiteral("fingerprintProtection"),
                                       DaliNiraBlockerDefaults::FingerprintProtection).toBool();
  popupBlock_ = readSetting(QStringLiteral("popupBlock"), DaliNiraBlockerDefaults::PopupBlock).toBool();
  developerMode_ = readSetting(QStringLiteral("developerMode"), DaliNiraBlockerDefaults::DeveloperMode).toBool();
  autoUpdateRulesets_ = readSetting(QStringLiteral("autoUpdateRulesets"), DaliNiraBlockerDefaults::AutoUpdateRulesets).toBool();
  customFilters_ = readSetting(QStringLiteral("customFilters"), QStringList{}).toStringList();
  enabledRulesetIds_ = readSetting(QStringLiteral("enabledRulesetIds"), QStringList{}).toStringList();
  rulesetSelectionConfigured_ = readSetting(QStringLiteral("rulesetSelectionConfigured"), DaliNiraBlockerDefaults::RulesetSelectionConfigured).toBool();

  sitePolicies_.clear();
  auto readSiteGroup = [this](const QString &group) -> bool {
    settings_.beginGroup(group);
    const QStringList keys = settings_.childKeys();
    if (keys.isEmpty()) {
      settings_.endGroup();
      return false;
    }
    for (const QString &host : keys) {
      QJsonObject obj = settings_.value(host).toJsonObject();
      SitePolicy p;
      p.adBlocking = obj.value(QStringLiteral("adBlocking")).toBool(true);
      p.trackerProtection = obj.value(QStringLiteral("trackerProtection")).toBool(true);
      p.whitelisted = obj.value(QStringLiteral("whitelisted")).toBool(false);
      p.temporaryDisabledUntil = obj.value(QStringLiteral("temporaryDisabledUntil")).toVariant().toLongLong();
      p.perSiteMode = obj.value(QStringLiteral("perSiteMode")).toInt(-1);
      p.blockScripts = obj.value(QStringLiteral("blockScripts")).toBool(false);
      p.blockFingerprinting = obj.value(QStringLiteral("blockFingerprinting")).toBool(false);
      p.upgradeHttps = obj.value(QStringLiteral("upgradeHttps")).toBool(SitePolicy{}.upgradeHttps);
      p.forgetOnClose = obj.value(QStringLiteral("forgetOnClose")).toBool(false);
      p.cookiePolicy = obj.value(QStringLiteral("cookiePolicy")).toString(QStringLiteral("third_party"));
      const QString key = normalizeSiteHost(host);
      if (!key.isEmpty()) sitePolicies_[key] = p;
    }
    settings_.endGroup();
    return true;
  };

  if (!readSiteGroup(QStringLiteral("blocker/sites"))) {
    readSiteGroup(QStringLiteral("adblock/sites"));
  }
}

void DaliNiraBlockerSettings::save() {
  QMutexLocker locker(&mutex_);
  auto writeSites = [this](const QString &group) {
    settings_.remove(group);
    settings_.beginGroup(group);
    for (auto it = sitePolicies_.constBegin(); it != sitePolicies_.constEnd(); ++it) {
      QJsonObject obj;
      obj[QStringLiteral("adBlocking")] = it.value().adBlocking;
      obj[QStringLiteral("trackerProtection")] = it.value().trackerProtection;
      obj[QStringLiteral("whitelisted")] = it.value().whitelisted;
      obj[QStringLiteral("temporaryDisabledUntil")] = it.value().temporaryDisabledUntil;
      obj[QStringLiteral("perSiteMode")] = it.value().perSiteMode;
      obj[QStringLiteral("blockScripts")] = it.value().blockScripts;
      obj[QStringLiteral("blockFingerprinting")] = it.value().blockFingerprinting;
      obj[QStringLiteral("upgradeHttps")] = it.value().upgradeHttps;
      obj[QStringLiteral("forgetOnClose")] = it.value().forgetOnClose;
      obj[QStringLiteral("cookiePolicy")] = it.value().cookiePolicy;
      settings_.setValue(it.key(), obj);
    }
    settings_.endGroup();
  };

  writeSites(QStringLiteral("blocker/sites"));
  writeSites(QStringLiteral("adblock/sites"));
  settings_.sync();
}

QJsonObject DaliNiraBlockerSettings::exportBackupJson() const {
  QMutexLocker locker(&mutex_);
  QJsonObject root;
  root[QStringLiteral("format")] = QString::fromLatin1(kFormatName);
  root[QStringLiteral("version")] = kFormatVersion;
  root[QStringLiteral("mode")] = modeToString(mode_);
  root[QStringLiteral("protectionEnabled")] = protectionEnabled_;
  root[QStringLiteral("autoReload")] = autoReload_;
  root[QStringLiteral("showBlockedCount")] = showCount_;
  root[QStringLiteral("strictBlock")] = strictBlock_;
  root[QStringLiteral("httpsByDefault")] = httpsByDefault_;
  root[QStringLiteral("fingerprintProtection")] = fingerprintProtection_;
  root[QStringLiteral("popupBlock")] = popupBlock_;
  root[QStringLiteral("developerMode")] = developerMode_;
  root[QStringLiteral("autoUpdateRulesets")] = autoUpdateRulesets_;
  root[QStringLiteral("customFilters")] = QJsonArray::fromStringList(customFilters_);
  root[QStringLiteral("enabledRulesetIds")] = QJsonArray::fromStringList(enabledRulesetIds_);
  root[QStringLiteral("rulesetSelectionConfigured")] = rulesetSelectionConfigured_;

  QJsonObject sitesObj;
  for (auto it = sitePolicies_.constBegin(); it != sitePolicies_.constEnd(); ++it) {
    QJsonObject s;
    s[QStringLiteral("adBlocking")] = it.value().adBlocking;
    s[QStringLiteral("trackerProtection")] = it.value().trackerProtection;
    s[QStringLiteral("whitelisted")] = it.value().whitelisted;
    s[QStringLiteral("temporaryDisabledUntil")] = it.value().temporaryDisabledUntil;
    s[QStringLiteral("perSiteMode")] = it.value().perSiteMode;
    s[QStringLiteral("blockScripts")] = it.value().blockScripts;
    s[QStringLiteral("blockFingerprinting")] = it.value().blockFingerprinting;
    s[QStringLiteral("upgradeHttps")] = it.value().upgradeHttps;
    s[QStringLiteral("forgetOnClose")] = it.value().forgetOnClose;
    s[QStringLiteral("cookiePolicy")] = it.value().cookiePolicy;
    sitesObj[it.key()] = s;
  }
  root[QStringLiteral("sitePolicies")] = sitesObj;
  return root;
}

bool DaliNiraBlockerSettings::importBackupJson(const QJsonObject &json) {
  QMutexLocker locker(&mutex_);
  // Support "dalinira-blocker-settings", "dalinira-deliblock-settings" and legacy formats
  const QString fmt = json.value(QStringLiteral("format")).toString();
  if (!fmt.isEmpty() && fmt != QLatin1String(kFormatName) && fmt != QLatin1String("dalinira-deliblock-settings") && fmt != QLatin1String("dalinira-adblock-settings")) {
    return false;
  }
  if (fmt == QLatin1String(kFormatName) || fmt == QLatin1String("dalinira-deliblock-settings")) {
    const int version = json.value(QStringLiteral("version")).toInt(0);
    if (version < 1 || version > kFormatVersion) return false;
  }

  QJsonObject blockerObj = json.contains(QStringLiteral("blocker")) ? json.value(QStringLiteral("blocker")).toObject() :
                           (json.contains(QStringLiteral("adblock")) ? json.value(QStringLiteral("adblock")).toObject() : json);
  if (blockerObj.isEmpty() ||
      (!blockerObj.contains(QStringLiteral("mode")) &&
       !blockerObj.contains(QStringLiteral("customFilters")) &&
       !blockerObj.contains(QStringLiteral("sitePolicies")) &&
       !blockerObj.contains(QStringLiteral("enabledRulesetIds")))) return false;

  mode_ = modeFromString(blockerObj.value(QStringLiteral("mode")).toString());
  protectionEnabled_ = blockerObj.value(QStringLiteral("protectionEnabled"))
                           .toBool(DaliNiraBlockerDefaults::ProtectionEnabled);
  autoReload_ = blockerObj.value(QStringLiteral("autoReload")).toBool(
      blockerObj.value(QStringLiteral("autoRefreshOnModeChange")).toBool(DaliNiraBlockerDefaults::AutoReload));
  showCount_ = blockerObj.value(QStringLiteral("showBlockedCount")).toBool(DaliNiraBlockerDefaults::ShowCount);
  strictBlock_ = blockerObj.value(QStringLiteral("strictBlock")).toBool(DaliNiraBlockerDefaults::StrictBlock);
  httpsByDefault_ = blockerObj.value(QStringLiteral("httpsByDefault")).toBool(DaliNiraBlockerDefaults::HttpsByDefault);
  fingerprintProtection_ = blockerObj.value(QStringLiteral("fingerprintProtection"))
                               .toBool(DaliNiraBlockerDefaults::FingerprintProtection);
  popupBlock_ = blockerObj.value(QStringLiteral("popupBlock")).toBool(DaliNiraBlockerDefaults::PopupBlock);
  developerMode_ = blockerObj.value(QStringLiteral("developerMode")).toBool(DaliNiraBlockerDefaults::DeveloperMode);
  autoUpdateRulesets_ = blockerObj.value(QStringLiteral("autoUpdateRulesets")).toBool(DaliNiraBlockerDefaults::AutoUpdateRulesets);

  QStringList custom;
  if (blockerObj.value(QStringLiteral("customFilters")).isArray()) {
    for (const auto &val : blockerObj.value(QStringLiteral("customFilters")).toArray()) {
      if (val.isString()) custom.append(val.toString());
      else if (val.isObject()) custom.append(val.toObject().value(QStringLiteral("text")).toString());
    }
  }
  customFilters_ = custom;

  rulesetSelectionConfigured_ = blockerObj.value(QStringLiteral("rulesetSelectionConfigured")).toBool(
      blockerObj.contains(QStringLiteral("enabledRulesetIds")));
  QStringList rulesets;
  for (const auto &val : blockerObj.value(QStringLiteral("enabledRulesetIds")).toArray()) rulesets.append(val.toString());
  if (rulesetSelectionConfigured_) {
    enabledRulesetIds_ = rulesets;
  } else {
    enabledRulesetIds_.clear();
    settings_.remove(QStringLiteral("blocker/enabledRulesetIds"));
    settings_.remove(QStringLiteral("adblock/enabledRulesetIds"));
  }

  sitePolicies_.clear();
  QJsonObject sites = blockerObj.value(QStringLiteral("sitePolicies")).toObject();
  for (auto it = sites.constBegin(); it != sites.constEnd(); ++it) {
    QJsonObject s = it.value().toObject();
    SitePolicy p;
    p.adBlocking = s.value(QStringLiteral("adBlocking")).toBool(true);
    p.trackerProtection = s.value(QStringLiteral("trackerProtection")).toBool(true);
    p.whitelisted = s.value(QStringLiteral("whitelisted")).toBool(false);
    p.temporaryDisabledUntil = s.value(QStringLiteral("temporaryDisabledUntil")).toVariant().toLongLong();
    p.perSiteMode = s.value(QStringLiteral("perSiteMode")).toInt(-1);
    p.blockScripts = s.value(QStringLiteral("blockScripts")).toBool(false);
    p.blockFingerprinting = s.value(QStringLiteral("blockFingerprinting")).toBool(false);
    p.upgradeHttps = s.value(QStringLiteral("upgradeHttps")).toBool(SitePolicy{}.upgradeHttps);
    p.forgetOnClose = s.value(QStringLiteral("forgetOnClose")).toBool(false);
    p.cookiePolicy = s.value(QStringLiteral("cookiePolicy")).toString(QStringLiteral("third_party"));
    const QString key = normalizeSiteHost(it.key());
    if (!key.isEmpty()) sitePolicies_[key] = p;
  }
  save();
  settings_.setValue(QStringLiteral("blocker/mode"), modeToString(mode_));
  settings_.setValue(QStringLiteral("blocker/protectionEnabled"), protectionEnabled_);
  settings_.setValue(QStringLiteral("blocker/autoReload"), autoReload_);
  settings_.setValue(QStringLiteral("blocker/showBlockedCount"), showCount_);
  settings_.setValue(QStringLiteral("blocker/strictBlock"), strictBlock_);
  settings_.setValue(QStringLiteral("blocker/httpsByDefault"), httpsByDefault_);
  settings_.setValue(QStringLiteral("blocker/fingerprintProtection"), fingerprintProtection_);
  settings_.setValue(QStringLiteral("blocker/popupBlock"), popupBlock_);
  settings_.setValue(QStringLiteral("blocker/developerMode"), developerMode_);
  settings_.setValue(QStringLiteral("blocker/autoUpdateRulesets"), autoUpdateRulesets_);
  settings_.setValue(QStringLiteral("blocker/customFilters"), customFilters_);
  if (rulesetSelectionConfigured_) {
    settings_.setValue(QStringLiteral("blocker/enabledRulesetIds"), enabledRulesetIds_);
  }
  settings_.setValue(QStringLiteral("blocker/rulesetSelectionConfigured"), rulesetSelectionConfigured_);

  // Legacy mirrors
  settings_.setValue(QStringLiteral("adblock/mode"), modeToString(mode_));
  settings_.setValue(QStringLiteral("adblock/protectionEnabled"), protectionEnabled_);
  settings_.setValue(QStringLiteral("adblock/autoReload"), autoReload_);
  settings_.setValue(QStringLiteral("adblock/showBlockedCount"), showCount_);
  settings_.setValue(QStringLiteral("adblock/strictBlock"), strictBlock_);
  settings_.setValue(QStringLiteral("adblock/popupBlock"), popupBlock_);
  settings_.setValue(QStringLiteral("adblock/developerMode"), developerMode_);
  settings_.setValue(QStringLiteral("adblock/autoUpdateRulesets"), autoUpdateRulesets_);
  settings_.setValue(QStringLiteral("adblock/customFilters"), customFilters_);
  if (rulesetSelectionConfigured_) {
    settings_.setValue(QStringLiteral("adblock/enabledRulesetIds"), enabledRulesetIds_);
  }
  settings_.setValue(QStringLiteral("adblock/rulesetSelectionConfigured"), rulesetSelectionConfigured_);

  settings_.sync();
  emit modeChanged(mode_);
  emit protectionEnabledChanged(protectionEnabled_);
  emit toolbarCountVisibilityChanged(showCount_);
  emit popupBlockChanged(popupBlock_);
  emit customFiltersChanged();
  emit rulesetsChanged();
  emit sitePoliciesChanged();
  emit filteringPlanChanged();
  emit settingsChanged();
  return true;
}

void DaliNiraBlockerSettings::resetToDefaults() {
  QMutexLocker locker(&mutex_);
  mode_ = DaliNiraBlockerDefaults::Mode;
  protectionEnabled_ = DaliNiraBlockerDefaults::ProtectionEnabled;
  autoReload_ = DaliNiraBlockerDefaults::AutoReload;
  showCount_ = DaliNiraBlockerDefaults::ShowCount;
  strictBlock_ = DaliNiraBlockerDefaults::StrictBlock;
  httpsByDefault_ = DaliNiraBlockerDefaults::HttpsByDefault;
  fingerprintProtection_ = DaliNiraBlockerDefaults::FingerprintProtection;
  popupBlock_ = DaliNiraBlockerDefaults::PopupBlock;
  developerMode_ = DaliNiraBlockerDefaults::DeveloperMode;
  autoUpdateRulesets_ = DaliNiraBlockerDefaults::AutoUpdateRulesets;
  customFilters_.clear();
  sitePolicies_.clear();
  enabledRulesetIds_.clear();
  rulesetSelectionConfigured_ = DaliNiraBlockerDefaults::RulesetSelectionConfigured;

  settings_.remove(QStringLiteral("blocker"));
  settings_.remove(QStringLiteral("adblock"));

  settings_.setValue(QStringLiteral("blocker/mode"), modeToString(mode_));
  settings_.setValue(QStringLiteral("blocker/protectionEnabled"), protectionEnabled_);
  settings_.setValue(QStringLiteral("blocker/autoReload"), autoReload_);
  settings_.setValue(QStringLiteral("blocker/showBlockedCount"), showCount_);
  settings_.setValue(QStringLiteral("blocker/strictBlock"), strictBlock_);
  settings_.setValue(QStringLiteral("blocker/httpsByDefault"), httpsByDefault_);
  settings_.setValue(QStringLiteral("blocker/fingerprintProtection"), fingerprintProtection_);
  settings_.setValue(QStringLiteral("blocker/popupBlock"), popupBlock_);
  settings_.setValue(QStringLiteral("blocker/developerMode"), developerMode_);
  settings_.setValue(QStringLiteral("blocker/autoUpdateRulesets"), autoUpdateRulesets_);

  // Legacy mirrors
  settings_.setValue(QStringLiteral("adblock/mode"), modeToString(mode_));
  settings_.setValue(QStringLiteral("adblock/protectionEnabled"), protectionEnabled_);
  settings_.setValue(QStringLiteral("adblock/autoReload"), autoReload_);
  settings_.setValue(QStringLiteral("adblock/showBlockedCount"), showCount_);
  settings_.setValue(QStringLiteral("adblock/strictBlock"), strictBlock_);
  settings_.setValue(QStringLiteral("adblock/popupBlock"), popupBlock_);
  settings_.setValue(QStringLiteral("adblock/developerMode"), developerMode_);
  settings_.setValue(QStringLiteral("adblock/autoUpdateRulesets"), autoUpdateRulesets_);
  settings_.sync();

  emit modeChanged(mode_);
  emit protectionEnabledChanged(protectionEnabled_);
  emit toolbarCountVisibilityChanged(showCount_);
  emit popupBlockChanged(popupBlock_);
  emit customFiltersChanged();
  emit sitePoliciesChanged();
  emit rulesetsChanged();
  emit filteringPlanChanged();
  emit settingsChanged();
}
