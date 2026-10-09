#include "tab_performance_manager.h"

#include <QPointer>
#include <QSettings>
#include <QTimer>
#include <QWidget>
#include <algorithm>
#include <chrono>
#include <limits>
#include <optional>

namespace dalinira {

TabPerformanceManager::TabPerformanceManager(TabManager *tabManager, QObject *parent)
    : QObject(parent),
      tabManager_(tabManager),
      memoryMonitor_(std::make_unique<SystemMemoryPressureMonitor>(nullptr)) {
  deadlineTimer_ = new QTimer(this);
  deadlineTimer_->setSingleShot(true);
  connect(deadlineTimer_, &QTimer::timeout, this, &TabPerformanceManager::onDeadlineTimeout);

  if (memoryMonitor_) {
    connect(memoryMonitor_.get(), &SystemMemoryPressureMonitor::pressureLevelChanged,
            this, [this](MemoryPressureLevel) {
              onDeadlineTimeout();
            });
  }

  if (tabManager_) {
    connect(tabManager_, &TabManager::tabRegistered, this, &TabPerformanceManager::onTabRegistered);
    connect(tabManager_, &TabManager::tabActivated, this, &TabPerformanceManager::onTabActivated);
    connect(tabManager_, &TabManager::tabTransferred, this, &TabPerformanceManager::onTabTransferred);
    connect(tabManager_, &TabManager::tabRemoved, this, &TabPerformanceManager::onTabRemoved);
    connect(tabManager_, &TabManager::tabUrlChanged, this, &TabPerformanceManager::onTabUrlChanged);
  }

  loadPersistedSettings();
}

TabPerformanceManager::~TabPerformanceManager() {
  isTearingDown_ = true;
  if (deadlineTimer_) {
    deadlineTimer_->stop();
  }
}

void TabPerformanceManager::loadPersistedSettings() {
  QSettings settings;
  const QString modeStr = settings.value(QStringLiteral("performance/policyMode"), QStringLiteral("balanced")).toString().toLower();
  if (modeStr == QLatin1String("off")) {
    setPolicyMode(PerformancePolicyMode::Off);
  } else if (modeStr == QLatin1String("memory_saver")) {
    setPolicyMode(PerformancePolicyMode::MemorySaver);
  } else if (modeStr == QLatin1String("maximum_performance")) {
    setPolicyMode(PerformancePolicyMode::MaximumPerformance);
  } else {
    setPolicyMode(PerformancePolicyMode::Balanced);
  }

  const bool discardEnabled = settings.value(QStringLiteral("performance/discardEnabled"), true).toBool();
  setDiscardEnabled(discardEnabled);

  const QStringList allowlist = settings.value(QStringLiteral("performance/siteAllowlist")).toStringList();
  if (!allowlist.isEmpty()) {
    setSiteAllowlist(allowlist);
  }
  sleepAfterMs_ = std::max<int64_t>(0, settings.value(QStringLiteral("performance/sleepAfterSeconds"), 0).toLongLong() * 1000);
  sleepMediaTabs_ = settings.value(QStringLiteral("performance/sleepMediaTabs"), false).toBool();
}

int64_t TabPerformanceManager::modeDefaultSleepDelayMs(PerformancePolicyMode mode) {
  switch (mode) {
    case PerformancePolicyMode::MemorySaver: return 30 * 1000;
    case PerformancePolicyMode::MaximumPerformance: return 2 * kDefaultBackgroundDiscardDelayMs;
    case PerformancePolicyMode::Balanced:
    case PerformancePolicyMode::Off: break;
  }
  return kDefaultBackgroundDiscardDelayMs;
}

void TabPerformanceManager::setSleepAfterMs(int64_t delayMs) {
  sleepAfterMs_ = std::max<int64_t>(0, delayMs);
  QSettings().setValue(QStringLiteral("performance/sleepAfterSeconds"), static_cast<qlonglong>(sleepAfterMs_ / 1000));
  emit sleepSettingsChanged();
  scheduleNextDeadlineCheck();
}

int64_t TabPerformanceManager::effectiveSleepDelayMs() const {
  // The system is running out of memory: sleep eligible background tabs now.
  const auto pressure = memoryMonitor_ ? memoryMonitor_->currentPressureLevel() : MemoryPressureLevel::Normal;
  if (pressure == MemoryPressureLevel::Critical ||
      (policyMode_ == PerformancePolicyMode::MemorySaver && pressure == MemoryPressureLevel::Moderate)) {
    return 0;
  }
  return sleepAfterMs_ > 0 ? sleepAfterMs_ : backgroundDiscardDelayMs_;
}

void TabPerformanceManager::setSleepMediaTabs(bool allowed) {
  if (sleepMediaTabs_ == allowed) return;
  sleepMediaTabs_ = allowed;
  QSettings().setValue(QStringLiteral("performance/sleepMediaTabs"), allowed);
  emit sleepSettingsChanged();
  scheduleNextDeadlineCheck();
}

int64_t TabPerformanceManager::currentMonotonicMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

int64_t TabPerformanceManager::backgroundFreezeDelayMs() const {
  return backgroundFreezeDelayOverrideMs_ >= 0 ? backgroundFreezeDelayOverrideMs_ : effectiveSleepDelayMs();
}

void TabPerformanceManager::setBackgroundFreezeDelayMs(int64_t delayMs) {
  backgroundFreezeDelayOverrideMs_ = std::max<int64_t>(0, delayMs);
  scheduleNextDeadlineCheck();
}

int64_t TabPerformanceManager::fullSleepDelayMs() const {
  const int64_t timeout = effectiveSleepDelayMs();
  if (discardRequirement_ != DiscardPolicyRequirement::LongIdleOrMemoryPressure) return timeout;
  // Balanced: memory pressure brings full sleep forward to the timeout
  // (MaximumPerformance only for critical pressure); otherwise only long
  // inactivity escalates a light-sleeping tab.
  const auto pressure = memoryMonitor_ ? memoryMonitor_->currentPressureLevel() : MemoryPressureLevel::Normal;
  const bool pressured = policyMode_ == PerformancePolicyMode::MaximumPerformance
                             ? pressure == MemoryPressureLevel::Critical
                             : pressure != MemoryPressureLevel::Normal;
  if (pressured) return timeout;
  if (balancedFullSleepAfterMs_ > 0) return std::max(balancedFullSleepAfterMs_, timeout);
  return std::max<int64_t>(kBalancedFullSleepMinMs, kBalancedFullSleepFactor * timeout);
}

void TabPerformanceManager::setBalancedFullSleepAfterMs(int64_t delayMs) {
  balancedFullSleepAfterMs_ = std::max<int64_t>(0, delayMs);
  scheduleNextDeadlineCheck();
}

int64_t TabPerformanceManager::backgroundDiscardDelayMs() const {
  return backgroundDiscardDelayMs_;
}

void TabPerformanceManager::setBackgroundDiscardDelayMs(int64_t delayMs) {
  backgroundDiscardDelayMs_ = std::max<int64_t>(0, delayMs);
  scheduleNextDeadlineCheck();
}

DiscardPolicyRequirement TabPerformanceManager::discardPolicyRequirement() const {
  return discardRequirement_;
}

void TabPerformanceManager::setDiscardPolicyRequirement(DiscardPolicyRequirement req) {
  discardRequirement_ = req;
  scheduleNextDeadlineCheck();
}

PerformancePolicyMode TabPerformanceManager::policyMode() const {
  return policyMode_;
}

void TabPerformanceManager::setPolicyMode(PerformancePolicyMode mode) {
  policyMode_ = mode;
  backgroundFreezeDelayOverrideMs_ = -1;  // light sleep follows the timeout
  QSettings settings;
  switch (mode) {
    case PerformancePolicyMode::Balanced:
      backgroundDiscardDelayMs_ = kDefaultBackgroundDiscardDelayMs;
      discardRequirement_ = DiscardPolicyRequirement::LongIdleOrMemoryPressure;
      settings.setValue(QStringLiteral("performance/policyMode"), QStringLiteral("balanced"));
      break;
    case PerformancePolicyMode::MemorySaver:
      backgroundDiscardDelayMs_ = 30 * 1000;
      discardRequirement_ = DiscardPolicyRequirement::IdleOnly;
      settings.setValue(QStringLiteral("performance/policyMode"), QStringLiteral("memory_saver"));
      break;
    case PerformancePolicyMode::MaximumPerformance:
      backgroundDiscardDelayMs_ = 2 * kDefaultBackgroundDiscardDelayMs;
      discardRequirement_ = DiscardPolicyRequirement::LongIdleOrMemoryPressure;
      settings.setValue(QStringLiteral("performance/policyMode"), QStringLiteral("maximum_performance"));
      break;
    case PerformancePolicyMode::Off:
      backgroundDiscardDelayMs_ = kDefaultBackgroundDiscardDelayMs;
      discardRequirement_ = DiscardPolicyRequirement::IdleOnly;
      settings.setValue(QStringLiteral("performance/policyMode"), QStringLiteral("off"));
      break;
  }
  emit policyModeChanged(mode);
  scheduleNextDeadlineCheck();
}

bool TabPerformanceManager::isDiscardEnabled() const {
  return discardEnabled_;
}

void TabPerformanceManager::setDiscardEnabled(bool enabled) {
  if (discardEnabled_ != enabled) {
    discardEnabled_ = enabled;
    QSettings().setValue(QStringLiteral("performance/discardEnabled"), enabled);
    emit discardEnabledChanged(enabled);
    scheduleNextDeadlineCheck();
  }
}

SystemMemoryPressureMonitor *TabPerformanceManager::memoryPressureMonitor() const {
  return memoryMonitor_.get();
}

bool TabPerformanceManager::hasMetadata(TabManager::TabId id) const {
  return metadataMap_.contains(id);
}

TabPerformanceMetadata TabPerformanceManager::metadata(TabManager::TabId id) const {
  auto it = metadataMap_.constFind(id);
  if (it != metadataMap_.cend()) {
    return it.value();
  }

  // If queried for an internal tab or untracked tab, return a safe internal representation
  TabPerformanceMetadata fallback;
  fallback.tabId = id;
  if (tabManager_) {
    const TabManager::TabRecord *record = tabManager_->record(id);
    if (record) {
      fallback.tabKind = record->kind;
      fallback.visible = record->active;
      fallback.activationSerial = record->activationSerial;
    }
  }
  if (fallback.tabKind == TabManager::TabKind::Internal) {
    fallback.protectedReasons = ProtectedReason::Visible;
  }
  return fallback;
}

bool TabPerformanceManager::isTabProtected(TabManager::TabId id) const {
  const TabManager::TabRecord *record = tabManager_ ? tabManager_->record(id) : nullptr;
  if (record && record->kind == TabManager::TabKind::Internal) {
    return true; // Native internal tabs are inherently protected from web lifecycle actions
  }

  auto it = metadataMap_.constFind(id);
  if (it == metadataMap_.cend()) {
    return false;
  }
  return it->protectedReasons != ProtectedReason::None;
}

ProtectedReasons TabPerformanceManager::protectedReasons(TabManager::TabId id) const {
  const TabManager::TabRecord *record = tabManager_ ? tabManager_->record(id) : nullptr;
  if (record && record->kind == TabManager::TabKind::Internal) {
    return ProtectedReason::Visible;
  }

  auto it = metadataMap_.constFind(id);
  if (it == metadataMap_.cend()) {
    return ProtectedReason::None;
  }
  return it->protectedReasons;
}

QList<TabManager::TabId> TabPerformanceManager::trackedWebTabs() const {
  return metadataMap_.keys();
}

bool TabPerformanceManager::isAggressiveStatePermitted(
    TabManager::TabId id, TabLifecycleState targetState) const {
  auto it = metadataMap_.constFind(id);
  if (it == metadataMap_.cend() || it->tabKind != TabManager::TabKind::Web) {
    return false;
  }

  ProtectedReasons blocking = protectedReasons(id);
  // Playing tabs may sleep when the user allows it; freezing one would only
  // stop its sound, so that stays forbidden.
  if (sleepMediaTabs_ && targetState == TabLifecycleState::Discarded) {
    blocking &= ~ProtectedReasons(ProtectedReason::RecentlyAudible | ProtectedReason::MediaPlayback);
  }
  // Unsaved input survives light sleep; it only rules out full sleep.
  if (targetState == TabLifecycleState::Frozen) {
    blocking &= ~ProtectedReasons(ProtectedReason::FormOrEditState);
  }
  if (blocking != ProtectedReason::None && targetState != TabLifecycleState::Active) {
    return false;
  }

  const auto recState = it->recommendedState;
  switch (recState) {
    case TabLifecycleState::Active:
      return targetState == TabLifecycleState::Active;
    case TabLifecycleState::Frozen:
      return targetState == TabLifecycleState::Active ||
             targetState == TabLifecycleState::Frozen;
    case TabLifecycleState::Discarded:
      return true;
    default:
      return targetState == TabLifecycleState::Active;
  }
}

bool TabPerformanceManager::canFreeze(TabManager::TabId id) const {
  if (isTearingDown_ || !automaticLifecycleEnabled()) return false;

  auto it = metadataMap_.constFind(id);
  if (it == metadataMap_.cend() || it->tabKind != TabManager::TabKind::Web) {
    return false;
  }

  // 1. Must not be visible/active
  if (it->visible) {
    return false;
  }

  // 2. Must not be producing audio (freezing would only stop the sound)
  if (it->recentlyAudible) {
    return false;
  }

  // 3. No protected reason, except unsaved input: light sleep keeps the page,
  //    and with it the input, in memory.
  if (lightSleepBlockingReasons(id) != ProtectedReason::None) {
    return false;
  }

  // 4. Must have been in the background for the light sleep delay
  const int64_t now = currentMonotonicMs();
  if (it->elapsedSinceLastUseMs(now) < backgroundFreezeDelayMs()) {
    return false;
  }

  // 5. Must satisfy Qt recommendedState boundary (Active is forbidden; Frozen/Discarded permitted)
  if (!isAggressiveStatePermitted(id, TabLifecycleState::Frozen)) {
    return false;
  }

  // 6. Page/view must still be valid
  if (!tabManager_) return false;
  const TabManager::TabRecord *record = tabManager_->record(id);
  if (!isLifecycleEligible(record) || record->lifecycle->isLoading()) {
    return false;
  }

  // 7. Must currently be in Active lifecycle state (not already Frozen or Discarded)
  if (it->lifecycleState != TabLifecycleState::Active) {
    return false;
  }

  return true;
}

bool TabPerformanceManager::freezeTab(TabManager::TabId id) {
  if (!canFreeze(id)) {
    return false;
  }

  const TabManager::TabRecord *record = tabManager_->record(id);
  if (!record || !record->lifecycle) {
    return false;
  }

  auto it = metadataMap_.find(id);
  if (it == metadataMap_.end()) {
    return false;
  }

  const int64_t now = currentMonotonicMs();

  // Apply Frozen lifecycle to the underlying Qt WebEnginePage
  record->lifecycle->setLifecycleState(TabLifecycleState::Frozen);

  it->lifecycleState = TabLifecycleState::Frozen;
  it->frozenByDaliNira = true;
  it->lastFreezeMonotonicMs = now;
  it->freezeCount++;

  emit tabFrozen(id);
  emit tabLifecycleStateChanged(id, TabLifecycleState::Frozen);
  emit tabMetadataChanged(id, it.value());

  scheduleNextDeadlineCheck();
  return true;
}

bool TabPerformanceManager::canDiscard(TabManager::TabId id) const {
  if (isTearingDown_ || !discardEnabled_ || !automaticLifecycleEnabled()) return false;

  auto it = metadataMap_.constFind(id);
  if (it == metadataMap_.cend() || it->tabKind != TabManager::TabKind::Web) {
    return false;
  }

  // 1. Must not be visible/active in any window
  if (it->visible) {
    return false;
  }

  // 2 + 3. No protected reason (media only when the user did not allow
  // sleeping playing tabs, dirty forms, allowlist, downloads, dialogs, ...)
  ProtectedReasons blocking = protectedReasons(id);
  if (sleepMediaTabs_) {
    blocking &= ~ProtectedReasons(ProtectedReason::RecentlyAudible | ProtectedReason::MediaPlayback);
  }
  if (blocking != ProtectedReason::None || (it->recentlyAudible && !sleepMediaTabs_)) {
    return false;
  }

  // 4. Form dirty latch check
  if (isTabFormDirty(id)) {
    return false;
  }

  // 5. Must satisfy Qt recommendedState boundary (Discarded must be permitted by Qt)
  if (!isAggressiveStatePermitted(id, TabLifecycleState::Discarded)) {
    return false;
  }

  // 6. Page/view must still be valid
  if (!tabManager_) return false;
  const TabManager::TabRecord *record = tabManager_->record(id);
  if (!isLifecycleEligible(record) || record->lifecycle->isLoading()) {
    return false;
  }

  // 7. Site must not be in user allowlist
  if (record->url.isValid() && isUrlAllowlisted(record->url)) {
    return false;
  }

  // 8. Must not already be Discarded
  if (it->lifecycleState == TabLifecycleState::Discarded) {
    return false;
  }

  // 9. Idle threshold: the timeout, or Balanced's long-inactivity threshold
  //    unless memory pressure brings it forward (fullSleepDelayMs).
  const int64_t now = currentMonotonicMs();
  const int64_t elapsed = it->elapsedSinceLastUseMs(now);
  if (elapsed < fullSleepDelayMs()) {
    return false;
  }

  if (discardRequirement_ == DiscardPolicyRequirement::IdleAndMemoryPressure) {
    const auto pressure = memoryMonitor_ ? memoryMonitor_->currentPressureLevel() : MemoryPressureLevel::Normal;
    if (policyMode_ == PerformancePolicyMode::MaximumPerformance) {
      if (pressure != MemoryPressureLevel::Critical) {
        return false;
      }
    } else {
      if (pressure == MemoryPressureLevel::Normal) {
        return false;
      }
    }
  }

  return true;
}

bool TabPerformanceManager::discardTab(TabManager::TabId id) {
  if (!canDiscard(id)) {
    return false;
  }
  return applySleep(id, false);
}

bool TabPerformanceManager::applySleep(TabManager::TabId id, bool manual) {
  const TabManager::TabRecord *record = tabManager_ ? tabManager_->record(id) : nullptr;
  if (!record || !record->lifecycle) {
    return false;
  }

  auto it = metadataMap_.find(id);
  if (it == metadataMap_.end()) {
    return false;
  }

  // The engine releases the page's renderer / web process resources. Only a
  // state the engine actually reached is recorded.
  // Last chance to observe the page's resources (TabResourceModel keeps the
  // figure, so the hover card can show what sleeping released).
  emit tabAboutToSleep(id);
  if (!record->lifecycle->setLifecycleState(TabLifecycleState::Discarded)) {
    return false;
  }

  const int64_t now = currentMonotonicMs();
  it->lifecycleState = TabLifecycleState::Discarded;
  it->discardedByDaliNira = true;
  it->sleptManually = manual;
  it->frozenByDaliNira = false;
  it->lastDiscardMonotonicMs = now;
  it->discardCount++;

  emit tabDiscarded(id);
  emit tabLifecycleStateChanged(id, TabLifecycleState::Discarded);
  emit tabMetadataChanged(id, it.value());

  scheduleNextDeadlineCheck();
  return true;
}

bool TabPerformanceManager::isHardProtected(TabManager::TabId id) const {
  const ProtectedReasons reasons = protectedReasons(id);
  const ProtectedReasons hard = ProtectedReason::Visible | ProtectedReason::InternalScheme |
                                ProtectedReason::BrowserDownload | ProtectedReason::PageDialog |
                                ProtectedReason::WebRTC | ProtectedReason::Camera |
                                ProtectedReason::Microphone | ProtectedReason::ScreenCapture;
  return (reasons & hard) != ProtectedReason::None;
}

bool TabPerformanceManager::canSleepNow(TabManager::TabId id) const {
  if (isTearingDown_) return false;
  auto it = metadataMap_.constFind(id);
  if (it == metadataMap_.cend() || it->tabKind != TabManager::TabKind::Web) return false;
  if (it->visible || it->lifecycleState == TabLifecycleState::Discarded) return false;
  const TabManager::TabRecord *record = tabManager_ ? tabManager_->record(id) : nullptr;
  if (!isLifecycleEligible(record) || record->lifecycle->isLoading()) return false;
  return !isHardProtected(id);
}

bool TabPerformanceManager::sleepTab(TabManager::TabId id) {
  return canSleepNow(id) && applySleep(id, true);
}

bool TabPerformanceManager::canWakeNow(TabManager::TabId id) const {
  return !isTearingDown_ && (isTabDiscarded(id) || isTabFrozen(id));
}

ProtectedReasons TabPerformanceManager::lightSleepBlockingReasons(TabManager::TabId id) const {
  return protectedReasons(id) & ~ProtectedReasons(ProtectedReason::FormOrEditState);
}

void TabPerformanceManager::resetUnsavedInputCheck(TabManager::TabId id) {
  unsavedInputChecks_.remove(id);
  if (explicitFormDirty_.value(id, false)) setTabFormDirty(id, false);
}

void TabPerformanceManager::requestUnsavedInputCheck(TabManager::TabId id) {
  const TabManager::TabRecord *record = tabManager_ ? tabManager_->record(id) : nullptr;
  if (!record || !record->lifecycle) {
    unsavedInputChecks_.insert(id, UnsavedInputCheck::Checked);  // nothing to ask
    return;
  }
  unsavedInputChecks_.insert(id, UnsavedInputCheck::Pending);
  const quint64 serial = ++unsavedInputSerial_;
  unsavedInputRequestSerial_.insert(id, serial);
  QPointer<TabPerformanceManager> self(this);
  const auto finish = [self, id, serial](std::optional<bool> dirty) {
    if (!self || self->isTearingDown_ || !self->metadataMap_.contains(id)) return;
    // Only the answer to the current request, while it is still pending (not
    // shown, answered or timed out meanwhile).
    if (self->unsavedInputRequestSerial_.value(id) != serial ||
        self->unsavedInputChecks_.value(id) != UnsavedInputCheck::Pending)
      return;
    self->unsavedInputChecks_.insert(id, UnsavedInputCheck::Checked);
    // No answer (engine could not run the check): nothing detected, and
    // nothing is invented either.
    self->setTabFormDirty(id, dirty.value_or(false));
    self->scheduleNextDeadlineCheck();
  };
  record->lifecycle->checkUnsavedInput(finish);
  QTimer::singleShot(kUnsavedInputCheckTimeoutMs, this, [finish] { finish(std::nullopt); });
}

bool TabPerformanceManager::wakeTab(TabManager::TabId id) {
  return canWakeNow(id) && resumeTab(id);
}

int TabPerformanceManager::sleepOtherTabs(QObject *ownerWindow, TabManager::TabId keep) {
  if (!tabManager_) return 0;
  const ProtectedReasons keepAwake = ProtectedReason::UserPinned | ProtectedReason::UserAllowlisted |
                                     ProtectedReason::FormOrEditState |
                                     (sleepMediaTabs_ ? ProtectedReason::None
                                                      : ProtectedReasons(ProtectedReason::RecentlyAudible |
                                                                         ProtectedReason::MediaPlayback));
  int slept = 0;
  const QList<TabManager::TabId> ids = metadataMap_.keys();
  for (const TabManager::TabId &id : ids) {
    if (id == keep) continue;
    const TabManager::TabRecord *record = tabManager_->record(id);
    if (!record || record->ownerWindow != ownerWindow) continue;
    if ((protectedReasons(id) & keepAwake) != ProtectedReason::None) continue;
    if (sleepTab(id)) ++slept;
  }
  return slept;
}

QList<TabManager::TabId> TabPerformanceManager::sleepingTabs() const {
  QList<TabManager::TabId> result;
  for (auto it = metadataMap_.cbegin(); it != metadataMap_.cend(); ++it) {
    if (it->lifecycleState == TabLifecycleState::Discarded) result.append(it.key());
  }
  return result;
}

void TabPerformanceManager::setPermissionPromptActive(TabManager::TabId id, bool active) {
  explicitPermissionPrompt_[id] = active;
  auto it = metadataMap_.find(id);
  if (it == metadataMap_.end()) return;
  const TabManager::TabRecord *record = tabManager_ ? tabManager_->record(id) : nullptr;
  recomputeProtectedReasons(it.value(), record);
  scheduleNextDeadlineCheck();
}

void TabPerformanceManager::setPageDialogActive(TabManager::TabId id, bool active) {
  explicitPageDialog_[id] = active;
  auto it = metadataMap_.find(id);
  if (it == metadataMap_.end()) return;
  const TabManager::TabRecord *record = tabManager_ ? tabManager_->record(id) : nullptr;
  recomputeProtectedReasons(it.value(), record);
  scheduleNextDeadlineCheck();
}

void TabPerformanceManager::onPageDialogStateChanged(bool open) {
  Q_UNUSED(open);
  const TabManager::TabId id = idForSender();
  auto it = metadataMap_.find(id);
  if (id.isNull() || it == metadataMap_.end()) return;
  const TabManager::TabRecord *record = tabManager_ ? tabManager_->record(id) : nullptr;
  recomputeProtectedReasons(it.value(), record);
  scheduleNextDeadlineCheck();
}

TabManager::TabId TabPerformanceManager::idForSender() const {
  auto *page = qobject_cast<TabLifecycleAdapter *>(sender());
  return page ? pageToIdMap_.value(page) : TabManager::TabId();
}

bool TabPerformanceManager::resumeTab(TabManager::TabId id) {
  auto it = metadataMap_.find(id);
  if (it == metadataMap_.end() || it->tabKind != TabManager::TabKind::Web) {
    return false;
  }

  if (it->lifecycleState == TabLifecycleState::Active) {
    return false; // Already active
  }

  if (!tabManager_) return false;
  const TabManager::TabRecord *record = tabManager_->record(id);
  if (!record || !record->lifecycle) {
    return false;
  }

  const int64_t now = currentMonotonicMs();
  const bool wasDiscarded = (it->lifecycleState == TabLifecycleState::Discarded);

  // Restore Active lifecycle on the underlying Qt WebEnginePage
  // (Qt automatically triggers native reload if page was Discarded)
  record->lifecycle->setLifecycleState(TabLifecycleState::Active);

  it->lifecycleState = TabLifecycleState::Active;
  it->frozenByDaliNira = false;
  it->discardedByDaliNira = false;
  it->lastResumeMonotonicMs = now;

  if (wasDiscarded) {
    it->lastRestoreMonotonicMs = now;
    it->restoreCount++;
    emit tabRestored(id);
  } else {
    emit tabResumed(id);
  }

  emit tabLifecycleStateChanged(id, TabLifecycleState::Active);
  emit tabMetadataChanged(id, it.value());

  scheduleNextDeadlineCheck();
  return true;
}

bool TabPerformanceManager::isTabFrozen(TabManager::TabId id) const {
  auto it = metadataMap_.constFind(id);
  if (it == metadataMap_.cend()) return false;
  return it->lifecycleState == TabLifecycleState::Frozen;
}

bool TabPerformanceManager::isTabDiscarded(TabManager::TabId id) const {
  auto it = metadataMap_.constFind(id);
  if (it == metadataMap_.cend()) return false;
  return it->lifecycleState == TabLifecycleState::Discarded;
}

QString TabPerformanceManager::normalizeSitePattern(const QString &pattern) {
  QString normalized = pattern.trimmed().toLower();
  if (normalized.startsWith(QLatin1String("http://"))) {
    normalized = normalized.mid(7);
  } else if (normalized.startsWith(QLatin1String("https://"))) {
    normalized = normalized.mid(8);
  }
  const int slashIdx = normalized.indexOf(QLatin1Char('/'));
  if (slashIdx != -1) {
    normalized = normalized.left(slashIdx);
  }
  const int colonIdx = normalized.indexOf(QLatin1Char(':'));
  if (colonIdx != -1) {
    normalized = normalized.left(colonIdx);
  }
  if (normalized.startsWith(QLatin1String("*."))) {
    normalized = normalized.mid(2);
  }
  if (normalized.startsWith(QLatin1String("www."))) {
    normalized = normalized.mid(4);
  }
  return QUrl::fromAce(normalized.toUtf8());
}

bool TabPerformanceManager::matchesAllowlistPattern(const QString &host, const QString &pattern) {
  const QString normalizedHost = normalizeSitePattern(host);
  const QString normalizedPattern = normalizeSitePattern(pattern);

  if (normalizedHost.isEmpty() || normalizedPattern.isEmpty()) {
    return false;
  }

  if (normalizedHost == normalizedPattern) {
    return true;
  }

  const QString suffix = QLatin1Char('.') + normalizedPattern;
  return normalizedHost.endsWith(suffix);
}

void TabPerformanceManager::setSiteAllowlist(const QStringList &patterns) {
  QStringList cleanList;
  for (const QString &pat : patterns) {
    const QString norm = normalizeSitePattern(pat);
    if (!norm.isEmpty() && !cleanList.contains(norm)) {
      cleanList.append(norm);
    }
  }
  siteAllowlist_ = cleanList;
  QSettings().setValue(QStringLiteral("performance/siteAllowlist"), siteAllowlist_);

  if (tabManager_) {
    for (auto it = metadataMap_.begin(); it != metadataMap_.end(); ++it) {
      const TabManager::TabRecord *record = tabManager_->record(it.key());
      recomputeProtectedReasons(it.value(), record);
    }
  }

  emit siteAllowlistChanged(siteAllowlist_);
  scheduleNextDeadlineCheck();
}

QStringList TabPerformanceManager::siteAllowlist() const {
  return siteAllowlist_;
}

bool TabPerformanceManager::isUrlAllowlisted(const QUrl &url) const {
  if (siteAllowlist_.isEmpty() || !url.isValid()) {
    return false;
  }
  const QString host = url.host().toLower();
  if (host.isEmpty()) {
    return false;
  }
  for (const QString &pattern : siteAllowlist_) {
    if (matchesAllowlistPattern(host, pattern)) {
      return true;
    }
  }
  return false;
}

void TabPerformanceManager::setTabFormDirty(TabManager::TabId id, bool dirty) {
  auto it = metadataMap_.find(id);
  if (it == metadataMap_.end()) return;

  const bool current = explicitFormDirty_.value(id, false);
  if (current == dirty) return;

  explicitFormDirty_[id] = dirty;
  const TabManager::TabRecord *record = tabManager_ ? tabManager_->record(id) : nullptr;
  recomputeProtectedReasons(it.value(), record);

  emit tabFormDirtyChanged(id, dirty);
}

bool TabPerformanceManager::isTabFormDirty(TabManager::TabId id) const {
  return explicitFormDirty_.value(id, false);
}

void TabPerformanceManager::setTabVisible(TabManager::TabId id, bool visible) {
  auto it = metadataMap_.find(id);
  if (it == metadataMap_.end()) return;

  const int64_t now = currentMonotonicMs();
  if (it->visible != visible) {
    it->visible = visible;
    it->lastVisibleMonotonicMs = now;
    // On screen the user may type or submit: check again before the next sleep.
    if (visible) resetUnsavedInputCheck(id);
    const TabManager::TabRecord *record = tabManager_ ? tabManager_->record(id) : nullptr;
    recomputeProtectedReasons(it.value(), record);

    if (visible && (isTabFrozen(id) || isTabDiscarded(id))) {
      resumeTab(id);
    }
    scheduleNextDeadlineCheck();
  }
}

void TabPerformanceManager::setMediaPlaybackActive(TabManager::TabId id, bool active) {
  explicitMediaPlayback_[id] = active;
  auto it = metadataMap_.find(id);
  if (it != metadataMap_.end()) {
    const TabManager::TabRecord *record = tabManager_ ? tabManager_->record(id) : nullptr;
    recomputeProtectedReasons(it.value(), record);

    if (active && (isTabFrozen(id) || isTabDiscarded(id))) {
      resumeTab(id);
    }
    scheduleNextDeadlineCheck();
  }
}

void TabPerformanceManager::setBrowserDownloadActive(TabManager::TabId id, bool active) {
  explicitBrowserDownload_[id] = active;
  auto it = metadataMap_.find(id);
  if (it != metadataMap_.end()) {
    const TabManager::TabRecord *record = tabManager_ ? tabManager_->record(id) : nullptr;
    recomputeProtectedReasons(it.value(), record);

    if (active && (isTabFrozen(id) || isTabDiscarded(id))) {
      resumeTab(id);
    }
    scheduleNextDeadlineCheck();
  }
}

void TabPerformanceManager::setTabPinned(TabManager::TabId id, bool pinned) {
  explicitPinned_[id] = pinned;
  auto it = metadataMap_.find(id);
  if (it != metadataMap_.end()) {
    const TabManager::TabRecord *record = tabManager_ ? tabManager_->record(id) : nullptr;
    recomputeProtectedReasons(it.value(), record);

    if (pinned && (isTabFrozen(id) || isTabDiscarded(id))) {
      resumeTab(id);
    }
    scheduleNextDeadlineCheck();
  }
}

void TabPerformanceManager::onTabRegistered(TabManager::TabId id, TabManager::TabKind kind) {
  if (kind != TabManager::TabKind::Web || !tabManager_) return;

  const TabManager::TabRecord *record = tabManager_->record(id);
  if (!record || !record->lifecycle) return;

  const int64_t now = currentMonotonicMs();
  TabPerformanceMetadata meta;
  meta.tabId = id;
  meta.tabKind = TabManager::TabKind::Web;
  meta.creationMonotonicMs = now;
  meta.lastActivationMonotonicMs = now;
  meta.lastVisibleMonotonicMs = now;
  meta.lastAudibleChangeMonotonicMs = now;
  meta.visible = record->active;
  meta.recentlyAudible = record->lifecycle->isAudible();
  meta.recommendedState = record->lifecycle->recommendedState();
  meta.lifecycleState = record->lifecycle->lifecycleState();
  meta.activationSerial = record->activationSerial;

  metadataMap_.insert(id, meta);
  pageToIdMap_.insert(record->lifecycle.data(), id);

  wirePageSignals(id, record->lifecycle.data());
  recomputeProtectedReasons(metadataMap_[id], record);

  scheduleNextDeadlineCheck();
}

void TabPerformanceManager::onTabActivated(TabManager::TabId id) {
  if (!tabManager_) return;

  const TabManager::TabRecord *targetRecord = tabManager_->record(id);
  if (!targetRecord) return;

  // A sleeping tab wakes through the same path as the manual Wake action; a
  // frozen one resumes. Both happen BEFORE foreground usage begins.
  if (isTabDiscarded(id)) {
    wakeTab(id);
  } else if (isTabFrozen(id)) {
    resumeTab(id);
  }

  const int64_t now = currentMonotonicMs();
  const QObject *ownerWindow = targetRecord->ownerWindow.data();

  // If another web tab in the same window was visible, update its visibility to false
  for (auto it = metadataMap_.begin(); it != metadataMap_.end(); ++it) {
    if (it.key() != id) {
      const TabManager::TabRecord *otherRecord = tabManager_->record(it.key());
      if (otherRecord && otherRecord->ownerWindow == ownerWindow && it->visible) {
        it->visible = false;
        it->lastVisibleMonotonicMs = now;
        recomputeProtectedReasons(it.value(), otherRecord);
      }
    }
  }

  // Update target tab
  resetUnsavedInputCheck(id);
  auto it = metadataMap_.find(id);
  if (it != metadataMap_.end()) {
    it->visible = true;
    it->lastActivationMonotonicMs = now;
    it->lastVisibleMonotonicMs = now;
    it->activationSerial = targetRecord->activationSerial;
    recomputeProtectedReasons(it.value(), targetRecord);
  }

  scheduleNextDeadlineCheck();
}

void TabPerformanceManager::onTabTransferred(TabManager::TabId id, QObject *newOwner, bool detached) {
  Q_UNUSED(detached);
  if (!tabManager_) return;

  const TabManager::TabRecord *targetRecord = tabManager_->record(id);
  if (!targetRecord) return;

  if (targetRecord->active && (isTabFrozen(id) || isTabDiscarded(id))) {
    resumeTab(id);
  }

  const int64_t now = currentMonotonicMs();

  // Update visibility of any other tabs in newOwner
  for (auto it = metadataMap_.begin(); it != metadataMap_.end(); ++it) {
    if (it.key() != id) {
      const TabManager::TabRecord *otherRecord = tabManager_->record(it.key());
      if (otherRecord && otherRecord->ownerWindow == newOwner && it->visible) {
        it->visible = false;
        it->lastVisibleMonotonicMs = now;
        recomputeProtectedReasons(it.value(), otherRecord);
      }
    }
  }

  auto it = metadataMap_.find(id);
  if (it != metadataMap_.end()) {
    it->visible = targetRecord->active;
    it->activationSerial = targetRecord->activationSerial;
    if (targetRecord->active) {
      it->lastActivationMonotonicMs = now;
      it->lastVisibleMonotonicMs = now;
    }
    recomputeProtectedReasons(it.value(), targetRecord);
  }

  scheduleNextDeadlineCheck();
}

void TabPerformanceManager::onTabRemoved(TabManager::TabId id) {
  metadataMap_.remove(id);
  explicitMediaPlayback_.remove(id);
  explicitBrowserDownload_.remove(id);
  explicitPinned_.remove(id);
  explicitFormDirty_.remove(id);
  explicitPermissionPrompt_.remove(id);
  explicitPageDialog_.remove(id);
  unsavedInputChecks_.remove(id);
  unsavedInputRequestSerial_.remove(id);

  for (auto it = pageToIdMap_.begin(); it != pageToIdMap_.end();) {
    if (it.value() == id) {
      it = pageToIdMap_.erase(it);
    } else {
      ++it;
    }
  }

  scheduleNextDeadlineCheck();
}

void TabPerformanceManager::onTabUrlChanged(TabManager::TabId id, const QUrl &url) {
  Q_UNUSED(url);
  auto it = metadataMap_.find(id);
  if (it == metadataMap_.end()) return;
  resetUnsavedInputCheck(id);  // a new document: its input is checked afresh

  const TabManager::TabRecord *record = tabManager_ ? tabManager_->record(id) : nullptr;
  recomputeProtectedReasons(it.value(), record);
  scheduleNextDeadlineCheck();
}

void TabPerformanceManager::onPageUrlChanged(const QUrl &url) {
  Q_UNUSED(url);
  auto *page = qobject_cast<TabLifecycleAdapter *>(sender());
  if (!page) return;

  const TabManager::TabId id = pageToIdMap_.value(page);
  if (id.isNull()) return;

  auto it = metadataMap_.find(id);
  if (it == metadataMap_.end()) return;

  const TabManager::TabRecord *record = tabManager_ ? tabManager_->record(id) : nullptr;
  recomputeProtectedReasons(it.value(), record);
  scheduleNextDeadlineCheck();
}

void TabPerformanceManager::wirePageSignals(TabManager::TabId id, TabLifecycleAdapter *page) {
  Q_UNUSED(id);
  if (!page) return;

  connect(page, &TabLifecycleAdapter::audibleChanged,
          this, &TabPerformanceManager::onPageRecentlyAudibleChanged, Qt::UniqueConnection);
  connect(page, &TabLifecycleAdapter::recommendedStateChanged,
          this, &TabPerformanceManager::onPageRecommendedStateChanged, Qt::UniqueConnection);
  connect(page, &TabLifecycleAdapter::lifecycleStateChanged,
          this, &TabPerformanceManager::onPageLifecycleStateChanged, Qt::UniqueConnection);
  connect(page, &TabLifecycleAdapter::dialogStateChanged,
          this, &TabPerformanceManager::onPageDialogStateChanged, Qt::UniqueConnection);
  connect(page, &TabLifecycleAdapter::loadStarted,
          this, &TabPerformanceManager::onPageLoadStarted, Qt::UniqueConnection);
  connect(page, &TabLifecycleAdapter::loadFinished,
          this, &TabPerformanceManager::onPageLoadFinished, Qt::UniqueConnection);
  connect(page, &TabLifecycleAdapter::urlChanged,
          this, &TabPerformanceManager::onPageUrlChanged, Qt::UniqueConnection);
}

void TabPerformanceManager::onPageRecentlyAudibleChanged(bool audible) {
  auto *page = qobject_cast<TabLifecycleAdapter *>(sender());
  if (!page) return;

  const TabManager::TabId id = pageToIdMap_.value(page);
  if (id.isNull()) return;

  auto it = metadataMap_.find(id);
  if (it == metadataMap_.end()) return;

  it->recentlyAudible = audible;
  it->lastAudibleChangeMonotonicMs = currentMonotonicMs();
  const TabManager::TabRecord *record = tabManager_ ? tabManager_->record(id) : nullptr;
  recomputeProtectedReasons(it.value(), record);
  emit tabRecentlyAudibleChanged(id, audible);

  if (audible && (isTabFrozen(id) || isTabDiscarded(id))) {
    resumeTab(id);
  }

  scheduleNextDeadlineCheck();
}

void TabPerformanceManager::onPageRecommendedStateChanged(TabLifecycleState state) {
  auto *page = qobject_cast<TabLifecycleAdapter *>(sender());
  if (!page) return;

  const TabManager::TabId id = pageToIdMap_.value(page);
  if (id.isNull()) return;

  auto it = metadataMap_.find(id);
  if (it == metadataMap_.end()) return;

  it->recommendedState = state;
  const TabManager::TabRecord *record = tabManager_ ? tabManager_->record(id) : nullptr;
  recomputeProtectedReasons(it.value(), record);
  emit tabRecommendedStateChanged(id, state);

  if (state == TabLifecycleState::Active && (isTabFrozen(id) || isTabDiscarded(id))) {
    resumeTab(id);
  }

  scheduleNextDeadlineCheck();
}

void TabPerformanceManager::onPageLifecycleStateChanged(TabLifecycleState state) {
  auto *page = qobject_cast<TabLifecycleAdapter *>(sender());
  if (!page) return;

  const TabManager::TabId id = pageToIdMap_.value(page);
  if (id.isNull()) return;

  auto it = metadataMap_.find(id);
  if (it == metadataMap_.end()) return;

  it->lifecycleState = state;
  if (state != TabLifecycleState::Frozen) {
    it->frozenByDaliNira = false;
  }
  if (state != TabLifecycleState::Discarded) {
    it->discardedByDaliNira = false;
  }
  const TabManager::TabRecord *record = tabManager_ ? tabManager_->record(id) : nullptr;
  recomputeProtectedReasons(it.value(), record);
  emit tabLifecycleStateChanged(id, state);

  scheduleNextDeadlineCheck();
}

void TabPerformanceManager::onPageLoadStarted() {
  auto *page = qobject_cast<TabLifecycleAdapter *>(sender());
  if (!page) return;

  const TabManager::TabId id = pageToIdMap_.value(page);
  if (id.isNull()) return;

  // Trusted native lifecycle reset: on new document navigation/reload, form dirty state is reset
  if (explicitFormDirty_.value(id, false)) {
    setTabFormDirty(id, false);
  }
}

void TabPerformanceManager::onPageLoadFinished(bool ok) {
  Q_UNUSED(ok);
  scheduleNextDeadlineCheck();
}

void TabPerformanceManager::recomputeProtectedReasons(
    TabPerformanceMetadata &meta, const TabManager::TabRecord *record) {
  ProtectedReasons reasons = ProtectedReason::None;

  if (!record || !isLifecycleEligible(record)) {
    reasons |= ProtectedReason::InternalScheme;
  }
  if (meta.visible) {
    reasons |= ProtectedReason::Visible;
  }
  if (meta.recentlyAudible) {
    reasons |= ProtectedReason::RecentlyAudible;
  }
  if (meta.recommendedState == TabLifecycleState::Active) {
    reasons |= ProtectedReason::RecommendedActive;
  }
  if (explicitMediaPlayback_.value(meta.tabId, false)) {
    reasons |= ProtectedReason::MediaPlayback;
  }
  if (explicitBrowserDownload_.value(meta.tabId, false)) {
    reasons |= ProtectedReason::BrowserDownload;
  }
  if (explicitPinned_.value(meta.tabId, false) || (record && !record->capabilities.closable)) {
    reasons |= ProtectedReason::UserPinned;
  }
  if (explicitFormDirty_.value(meta.tabId, false)) {
    reasons |= ProtectedReason::FormOrEditState;
  }
  if (explicitPermissionPrompt_.value(meta.tabId, false) || explicitPageDialog_.value(meta.tabId, false) ||
      (record && record->lifecycle && record->lifecycle->hasOpenDialog())) {
    reasons |= ProtectedReason::PageDialog;
  }
  if (record && record->url.isValid() && isUrlAllowlisted(record->url)) {
    reasons |= ProtectedReason::UserAllowlisted;
    meta.allowlisted = true;
  } else {
    meta.allowlisted = false;
  }

  meta.formDirty = explicitFormDirty_.value(meta.tabId, false);

  const ProtectedReasons oldReasons = meta.protectedReasons;
  meta.protectedReasons = reasons;

  if (oldReasons != reasons) {
    emit tabProtectionChanged(meta.tabId, reasons != ProtectedReason::None, reasons);
    emit tabMetadataChanged(meta.tabId, meta);
  }
}

bool TabPerformanceManager::isSupportedWebScheme(const QUrl &url) {
  if (!url.isValid() || url.isEmpty()) {
    return false;
  }
  const QString scheme = url.scheme().toLower();
  if (scheme != QLatin1String("http") && scheme != QLatin1String("https")) {
    return false;
  }
  const QString host = url.host().toLower();
  if (host == QLatin1String("newtab") || host == QLatin1String("dalinira-browser.local") ||
      (url.isLocalFile() && url.path().contains(QStringLiteral("/assets/new-tab/"))) ||
      url.toString().startsWith(QLatin1String("about:")) ||
      url.toString().startsWith(QLatin1String("dalinira:")) ||
      url.toString().startsWith(QLatin1String("chrome:")) ||
      url.toString().startsWith(QLatin1String("qrc:"))) {
    return false;
  }
  return true;
}

bool TabPerformanceManager::isTabLifecycleEligible(TabManager::TabId id) const {
  if (!tabManager_) return false;
  return isLifecycleEligible(tabManager_->record(id));
}

bool TabPerformanceManager::isLifecycleEligible(const TabManager::TabRecord *record) const {
  if (!record || record->kind != TabManager::TabKind::Web || !record->lifecycle) {
    return false;
  }
  return isSupportedWebScheme(record->url);
}

int TabPerformanceManager::scheduledDeadlineDelayMs() const {
  return deadlineTimer_ && deadlineTimer_->isActive() ? deadlineTimer_->remainingTime() : -1;
}

void TabPerformanceManager::scheduleNextDeadlineCheck() {
  if (isTearingDown_ || !deadlineTimer_) return;
  if (!automaticLifecycleEnabled()) {
    deadlineTimer_->stop();
    return;
  }
  const int64_t lightSleepDelayMs = backgroundFreezeDelayMs();
  const int64_t fullDelayMs = fullSleepDelayMs();
  const auto pressure = memoryMonitor_ ? memoryMonitor_->currentPressureLevel() : MemoryPressureLevel::Normal;

  const int64_t now = currentMonotonicMs();
  int64_t minRemainingMs = -1;
  const auto consider = [&minRemainingMs](int64_t delayMs) {
    if (delayMs > 0) minRemainingMs = minRemainingMs < 0 ? delayMs : std::min(minRemainingMs, delayMs);
  };

  for (auto it = metadataMap_.cbegin(); it != metadataMap_.cend(); ++it) {
    const TabManager::TabId id = it.key();
    const TabPerformanceMetadata &meta = it.value();
    if (meta.tabKind != TabManager::TabKind::Web || meta.visible) continue;

    const TabManager::TabRecord *record = tabManager_ ? tabManager_->record(id) : nullptr;
    const bool loading = record && record->lifecycle && record->lifecycle->isLoading();
    const UnsavedInputCheck check = unsavedInputChecks_.value(id, UnsavedInputCheck::NotChecked);
    const int64_t elapsed = meta.elapsedSinceLastUseMs(now);
    // A due stage waits for a pending unsaved-input answer (which
    // reschedules); a loading page is retried at a bounded interval instead
    // of spinning the event loop; otherwise it is dispatched at once, to
    // check the page or to act.
    const auto stage = [&](int64_t remainingMs, bool ready) {
      if (remainingMs > 0) consider(remainingMs);
      else if (check == UnsavedInputCheck::Pending) return;
      else if (loading) consider(kMinimumDeadlineRetryMs);
      else if (check == UnsavedInputCheck::NotChecked || ready) consider(kReadyDeadlineDispatchMs);
    };

    // 1. Light sleep (Frozen)
    if (meta.lifecycleState == TabLifecycleState::Active && !meta.recentlyAudible &&
        lightSleepBlockingReasons(id) == ProtectedReason::None &&
        isAggressiveStatePermitted(id, TabLifecycleState::Frozen)) {
      stage(lightSleepDelayMs - elapsed, canFreeze(id));
    }

    // 2. Full sleep (Discarded)
    if (discardEnabled_ && meta.lifecycleState != TabLifecycleState::Discarded &&
        isAggressiveStatePermitted(id, TabLifecycleState::Discarded) &&
        (discardRequirement_ != DiscardPolicyRequirement::IdleAndMemoryPressure ||
         pressure != MemoryPressureLevel::Normal)) {
      stage(fullDelayMs - elapsed, canDiscard(id));
    }
  }

  if (minRemainingMs > 0) {
    deadlineTimer_->start(static_cast<int>(std::min<int64_t>(
        minRemainingMs, std::numeric_limits<int>::max())));
  } else {
    deadlineTimer_->stop();
  }
}

void TabPerformanceManager::onDeadlineTimeout() {
  if (isTearingDown_) return;
  ++deadlineCheckCount_;

  const int64_t now = currentMonotonicMs();
  const int64_t lightSleepDelayMs = backgroundFreezeDelayMs();
  const int64_t fullDelayMs = fullSleepDelayMs();
  const QList<TabManager::TabId> ids = metadataMap_.keys();

  // The automatic policy checks a tab for unsaved input before either stage
  // (the answer arrives asynchronously and reschedules); canFreeze() and
  // canDiscard() then see the result as FormOrEditState.
  // Returns true while the tab must wait (check requested or pending).
  const auto checkFirst = [this](TabManager::TabId id) {
    switch (unsavedInputChecks_.value(id, UnsavedInputCheck::NotChecked)) {
      case UnsavedInputCheck::Checked: return false;
      case UnsavedInputCheck::Pending: return true;
      case UnsavedInputCheck::NotChecked: break;
    }
    const TabManager::TabRecord *record = tabManager_ ? tabManager_->record(id) : nullptr;
    if (record && record->lifecycle && record->lifecycle->isLoading()) return true;  // later
    requestUnsavedInputCheck(id);
    // A synchronous answer (an engine without script) lets the tab proceed now.
    return unsavedInputChecks_.value(id) != UnsavedInputCheck::Checked;
  };

  // Phase 1: light sleep for background tabs past the timeout.
  for (const TabManager::TabId &id : ids) {
    auto it = metadataMap_.constFind(id);
    if (it == metadataMap_.cend()) continue;
    if (it->tabKind != TabManager::TabKind::Web || it->visible ||
        it->lifecycleState != TabLifecycleState::Active ||
        it->elapsedSinceLastUseMs(now) < lightSleepDelayMs || it->recentlyAudible ||
        lightSleepBlockingReasons(id) != ProtectedReason::None ||
        !isAggressiveStatePermitted(id, TabLifecycleState::Frozen))
      continue;
    if (checkFirst(id)) continue;
    if (canFreeze(id)) freezeTab(id);
  }

  // Phase 2: full sleep (ordered & rate limited).
  if (discardEnabled_) {
    QList<TabManager::TabId> discardCandidates;
    for (const TabManager::TabId &id : ids) {
      auto it = metadataMap_.constFind(id);
      if (it == metadataMap_.cend() || it->tabKind != TabManager::TabKind::Web || it->visible ||
          it->lifecycleState == TabLifecycleState::Discarded || it->elapsedSinceLastUseMs(now) < fullDelayMs ||
          !isAggressiveStatePermitted(id, TabLifecycleState::Discarded))
        continue;
      if (checkFirst(id)) continue;
      if (canDiscard(id)) discardCandidates.append(id);
    }

    if (!discardCandidates.isEmpty()) {
      // Discard Candidate Prioritization:
      // 1. Already Frozen tabs first
      // 2. Oldest inactive / least recently active
      std::sort(discardCandidates.begin(), discardCandidates.end(),
                [this](const TabManager::TabId &a, const TabManager::TabId &b) {
                  const auto metaA = metadataMap_.value(a);
                  const auto metaB = metadataMap_.value(b);
                  const bool aFrozen = (metaA.lifecycleState == TabLifecycleState::Frozen);
                  const bool bFrozen = (metaB.lifecycleState == TabLifecycleState::Frozen);
                  if (aFrozen != bFrozen) {
                    return aFrozen; // Frozen tabs have higher discard priority
                  }
                  return metaA.lastActivationMonotonicMs < metaB.lastActivationMonotonicMs; // Oldest first
                });

      int discardedInThisBurst = 0;
      for (const TabManager::TabId &id : discardCandidates) {
        if (discardedInThisBurst >= kMaxDiscardPerEvaluation) {
          break; // Enforce burst rate limit
        }
        if (discardTab(id)) {
          discardedInThisBurst++;
        }
      }
    }
  }

  scheduleNextDeadlineCheck();
}

} // namespace dalinira
