#pragma once

#include <algorithm>

#include <QFlags>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <QUuid>
#include <QUrl>
#include <cstdint>
#include <memory>

#include "engine/tab_lifecycle_state.h"
#include "system_memory_pressure_monitor.h"
#include "tab_lifecycle_adapter.h"
#include "tab_manager.h"

namespace dalinira {

/**
 * @brief Categorized reasons why a tab is protected from aggressive lifecycle actions.
 */
enum class ProtectedReason : uint32_t {
  None              = 0,
  Visible           = 1 << 0,  ///< Supported & Detected: Tab is active/visible in its window
  RecentlyAudible   = 1 << 1,  ///< Supported & Detected: Tab is producing or recently produced audio
  RecommendedActive = 1 << 2,  ///< Supported & Detected: the engine recommends keeping the page Active
  MediaPlayback     = 1 << 3,  ///< Supported & Detected: Active media/audio playback detected
  WebRTC            = 1 << 4,  ///< Supported in Model (NOT YET DETECTED): Active WebRTC session
  Camera            = 1 << 5,  ///< Supported in Model (NOT YET DETECTED): Active camera capture
  Microphone        = 1 << 6,  ///< Supported in Model (NOT YET DETECTED): Active microphone capture
  ScreenCapture     = 1 << 7,  ///< Supported in Model (NOT YET DETECTED): Active screen capture
  BrowserDownload   = 1 << 8,  ///< Supported & Detected: a download started by this tab is in progress
  UserPinned        = 1 << 9,  ///< Supported & Detected: Tab is pinned/non-closable by user
  UserAllowlisted   = 1 << 10, ///< Supported & Detected (Phase 2C-1): Domain is user-allowlisted
  FormOrEditState   = 1 << 11, ///< Supported & Detected (Phase 2C-1): Unsaved form/input/edit state latched
  InternalScheme    = 1 << 12, ///< Supported & Detected: Non-http/https URL or internal/New Tab page
  PageDialog        = 1 << 13, ///< Supported & Detected: permission prompt or modal page dialog is open
};
Q_DECLARE_FLAGS(ProtectedReasons, ProtectedReason)
Q_DECLARE_OPERATORS_FOR_FLAGS(ProtectedReasons)

/**
 * @brief Policy requirements for discard eligibility.
 */
enum class DiscardPolicyRequirement {
  IdleAndMemoryPressure,     ///< Requires idle delay AND moderate/critical memory pressure
  IdleOnly,                  ///< Requires idle delay only (Aggressive: full sleep at the timeout)
  LongIdleOrMemoryPressure,  ///< Balanced: full sleep only after long inactivity, or under memory pressure
};

/**
 * @brief Tab sleeping mode (one policy for both engines).
 *
 * Two stages. Light sleep (Frozen) restricts a background page in place: no
 * rendering, timers stopped (Chromium) or throttled (WebKit), and the page,
 * its form input, scroll position and history stay in memory, so waking needs
 * no reload. Full sleep
 * (Discarded) releases the page's renderer / web process; waking restores it
 * from saved session state.
 */
enum class PerformancePolicyMode {
  Balanced,           ///< Default: light sleep at the timeout; full sleep only after long inactivity or under memory pressure
  MemorySaver,        ///< Aggressive: shorter timeout, full sleep at the timeout; at once under memory pressure
  MaximumPerformance, ///< Legacy (not offered in Settings): like Balanced with a longer timeout
  Off                 ///< No automatic light or full sleep; manual sleep still works
};

/**
 * @brief Performance and activity metadata tracked for each tab.
 * Uses monotonic time to remain immune to wall-clock / NTP shifts.
 */
struct TabPerformanceMetadata {
  TabManager::TabId tabId;
  TabManager::TabKind tabKind = TabManager::TabKind::Web;
  int64_t creationMonotonicMs = 0;
  int64_t lastActivationMonotonicMs = 0;
  int64_t lastVisibleMonotonicMs = 0;
  int64_t lastAudibleChangeMonotonicMs = 0;
  int64_t lastFreezeMonotonicMs = 0;
  int64_t lastResumeMonotonicMs = 0;
  int64_t lastDiscardMonotonicMs = 0;
  int64_t lastRestoreMonotonicMs = 0;
  uint32_t freezeCount = 0;
  uint32_t discardCount = 0;
  uint32_t restoreCount = 0;
  bool visible = false;
  bool recentlyAudible = false;
  bool frozenByDaliNira = false;
  bool discardedByDaliNira = false;
  bool sleptManually = false;
  bool formDirty = false;
  bool allowlisted = false;
  quint64 activationSerial = 0;
  TabLifecycleState lifecycleState = TabLifecycleState::Active;
  TabLifecycleState recommendedState = TabLifecycleState::Active;
  ProtectedReasons protectedReasons = ProtectedReason::None;

  int64_t elapsedSinceCreationMs(int64_t nowMonotonicMs) const {
    return nowMonotonicMs >= creationMonotonicMs ? nowMonotonicMs - creationMonotonicMs : 0;
  }
  int64_t elapsedSinceLastActiveMs(int64_t nowMonotonicMs) const {
    return nowMonotonicMs >= lastActivationMonotonicMs ? nowMonotonicMs - lastActivationMonotonicMs : 0;
  }
  int64_t elapsedSinceLastVisibleMs(int64_t nowMonotonicMs) const {
    return nowMonotonicMs >= lastVisibleMonotonicMs ? nowMonotonicMs - lastVisibleMonotonicMs : 0;
  }
  // Inactivity for the sleep policy: since the tab was last on screen or
  // woken (a manual Wake of a background tab restarts the clock).
  int64_t elapsedSinceLastUseMs(int64_t nowMonotonicMs) const {
    const int64_t lastUse = std::max(lastVisibleMonotonicMs, lastResumeMonotonicMs);
    return nowMonotonicMs >= lastUse ? nowMonotonicMs - lastUse : 0;
  }
};

/**
 * @brief Central event-driven performance and lifecycle manager.
 */
class TabPerformanceManager : public QObject {
  Q_OBJECT

 public:
  // One central product policy: an eligible background page is frozen first,
  // then discarded after this interval. Qt's recommendedState remains the
  // final safety boundary for either transition.
  static constexpr int64_t kDefaultBackgroundDiscardDelayMs = 60 * 1000;
  // Balanced escalates light sleep to full sleep after this much inactivity:
  // four times the timeout, and never before 30 minutes.
  static constexpr int64_t kBalancedFullSleepMinMs = 30 * 60 * 1000;
  static constexpr int kBalancedFullSleepFactor = 4;
  // How long the unsaved-input check before light sleep may take.
  static constexpr int kUnsavedInputCheckTimeoutMs = 2000;
  static constexpr int kMaxDiscardPerEvaluation = 2;                           // Rate limiting burst max
  static constexpr int kReadyDeadlineDispatchMs = 1;
  static constexpr int kMinimumDeadlineRetryMs = 1000;

  explicit TabPerformanceManager(TabManager *tabManager, QObject *parent = nullptr);
  ~TabPerformanceManager() override;

  /// Monotonic timestamp in milliseconds since steady_clock epoch.
  static int64_t currentMonotonicMs();

  // Delays and policy configuration
  // When a background tab enters light sleep: the inactivity timeout, unless
  // overridden (setBackgroundFreezeDelayMs; setPolicyMode clears it).
  int64_t backgroundFreezeDelayMs() const;
  void setBackgroundFreezeDelayMs(int64_t delayMs);
  // When automatic full sleep may happen: the timeout (Aggressive, or under
  // memory pressure) or the long-inactivity threshold (Balanced).
  int64_t fullSleepDelayMs() const;
  // Balanced's long-inactivity threshold; 0 restores the default.
  void setBalancedFullSleepAfterMs(int64_t delayMs);

  int64_t backgroundDiscardDelayMs() const;
  void setBackgroundDiscardDelayMs(int64_t delayMs);

  DiscardPolicyRequirement discardPolicyRequirement() const;
  void setDiscardPolicyRequirement(DiscardPolicyRequirement req);

  PerformancePolicyMode policyMode() const;
  void setPolicyMode(PerformancePolicyMode mode);

  bool isDiscardEnabled() const;
  void setDiscardEnabled(bool enabled);

  // User-chosen inactivity timeout before a background tab sleeps; 0 means
  // the mode's default (see effectiveSleepDelayMs()).
  int64_t sleepAfterMs() const { return sleepAfterMs_; }
  void setSleepAfterMs(int64_t delayMs);
  int64_t effectiveSleepDelayMs() const;
  static int64_t modeDefaultSleepDelayMs(PerformancePolicyMode mode);
  // Tabs playing audio/video are protected unless the user allows sleeping them.
  bool sleepMediaTabs() const { return sleepMediaTabs_; }
  void setSleepMediaTabs(bool allowed);
  bool automaticLifecycleEnabled() const { return policyMode_ != PerformancePolicyMode::Off; }

  void loadPersistedSettings();

  SystemMemoryPressureMonitor *memoryPressureMonitor() const;

  // Metadata and protection
  bool hasMetadata(TabManager::TabId id) const;
  TabPerformanceMetadata metadata(TabManager::TabId id) const;
  bool isTabProtected(TabManager::TabId id) const;
  ProtectedReasons protectedReasons(TabManager::TabId id) const;

  /// Returns list of all tracked web tab IDs.
  QList<TabManager::TabId> trackedWebTabs() const;

  /// Safety gate query for lifecycle policies:
  /// Verifies that targetState is not more aggressive than Qt's recommendedState.
  bool isAggressiveStatePermitted(TabManager::TabId id, TabLifecycleState targetState) const;

  // Phase 2B & 2C: Freeze, Discard & Restore
  bool canFreeze(TabManager::TabId id) const;
  bool freezeTab(TabManager::TabId id);
  bool canDiscard(TabManager::TabId id) const;
  bool discardTab(TabManager::TabId id);
  bool resumeTab(TabManager::TabId id); // Restores either Frozen or Discarded tab to Active

  // User actions (tab context menu). They skip the inactivity timeout and the
  // mode, but never sleep the tab on screen, an internal page, or a tab with a
  // download, permission prompt or page dialog in progress.
  bool canSleepNow(TabManager::TabId id) const;
  bool sleepTab(TabManager::TabId id);
  // Waking a sleeping tab: the one path for the "Wake this tab" action and for
  // activating the tab (onTabActivated). It restores the same tab in place
  // (resumeTab) and does nothing for a tab that is already awake.
  // Light-sleeping (Frozen) and fully sleeping (Discarded) tabs can be woken.
  bool canWakeNow(TabManager::TabId id) const;
  bool wakeTab(TabManager::TabId id);
  // Sleeps every other eligible tab of the window; pinned tabs and tabs the
  // automatic policy protects (media unless allowed, never-sleep sites,
  // unsaved forms) are left awake. Returns how many tabs went to sleep.
  int sleepOtherTabs(QObject *ownerWindow, TabManager::TabId keep);
  QList<TabManager::TabId> sleepingTabs() const;

  bool isTabFrozen(TabManager::TabId id) const;
  bool isTabDiscarded(TabManager::TabId id) const;
  bool isTabLifecycleEligible(TabManager::TabId id) const;
  static bool isSupportedWebScheme(const QUrl &url);
  quint64 deadlineCheckCount() const { return deadlineCheckCount_; }
  int scheduledDeadlineDelayMs() const;

  // Site allowlist model
  static QString normalizeSitePattern(const QString &pattern);
  static bool matchesAllowlistPattern(const QString &host, const QString &pattern);
  void setSiteAllowlist(const QStringList &patterns);
  QStringList siteAllowlist() const;
  bool isUrlAllowlisted(const QUrl &url) const;

  // Form dirty state (spoof-safe latching)
  void setTabFormDirty(TabManager::TabId id, bool dirty);
  bool isTabFormDirty(TabManager::TabId id) const;

  // External explicit signal hooks (for testing or controller integration)
  void setTabVisible(TabManager::TabId id, bool visible);
  void setMediaPlaybackActive(TabManager::TabId id, bool active);
  void setBrowserDownloadActive(TabManager::TabId id, bool active);
  void setTabPinned(TabManager::TabId id, bool pinned);
  void setPermissionPromptActive(TabManager::TabId id, bool active);
  void setPageDialogActive(TabManager::TabId id, bool active);

 signals:
  void tabMetadataChanged(TabManager::TabId id, const dalinira::TabPerformanceMetadata &metadata);
  void tabProtectionChanged(TabManager::TabId id, bool isProtected, dalinira::ProtectedReasons reasons);
  void tabRecentlyAudibleChanged(TabManager::TabId id, bool audible);
  void tabRecommendedStateChanged(TabManager::TabId id, dalinira::TabLifecycleState state);
  void tabLifecycleStateChanged(TabManager::TabId id, dalinira::TabLifecycleState state);
  void tabFrozen(TabManager::TabId id);
  void tabResumed(TabManager::TabId id);
  void tabAboutToSleep(TabManager::TabId id);
  void tabDiscarded(TabManager::TabId id);
  void tabRestored(TabManager::TabId id);
  void tabFormDirtyChanged(TabManager::TabId id, bool dirty);
  void siteAllowlistChanged(const QStringList &patterns);
  void policyModeChanged(dalinira::PerformancePolicyMode mode);
  void discardEnabledChanged(bool enabled);
  void sleepSettingsChanged();

 public slots:
  void onTabRegistered(TabManager::TabId id, TabManager::TabKind kind);
  void onTabActivated(TabManager::TabId id);
  void onTabTransferred(TabManager::TabId id, QObject *newOwner, bool detached);
  void onTabRemoved(TabManager::TabId id);
  void onTabUrlChanged(TabManager::TabId id, const QUrl &url);

 private slots:
  void onPageRecentlyAudibleChanged(bool audible);
  void onPageRecommendedStateChanged(dalinira::TabLifecycleState state);
  void onPageLifecycleStateChanged(dalinira::TabLifecycleState state);
  void onPageDialogStateChanged(bool open);
  void onPageLoadStarted();
  void onPageLoadFinished(bool ok);
  void onPageUrlChanged(const QUrl &url);
  void onDeadlineTimeout();

 private:
  void wirePageSignals(TabManager::TabId id, TabLifecycleAdapter *page);
  TabManager::TabId idForSender() const;
  bool isHardProtected(TabManager::TabId id) const;
  // Reasons that keep a tab out of light sleep: everything except unsaved
  // input, which light sleep preserves (and media only while it plays).
  ProtectedReasons lightSleepBlockingReasons(TabManager::TabId id) const;

  // Unsaved input (typed form values) is checked in the page just before light
  // sleep, through the engine adapter; the answer holds until the tab is shown
  // or navigates again (a hidden page cannot be typed into).
  enum class UnsavedInputCheck { NotChecked, Pending, Checked };
  void requestUnsavedInputCheck(TabManager::TabId id);
  void resetUnsavedInputCheck(TabManager::TabId id);
  QHash<TabManager::TabId, UnsavedInputCheck> unsavedInputChecks_;
  QHash<TabManager::TabId, quint64> unsavedInputRequestSerial_;
  quint64 unsavedInputSerial_ = 0;
  bool applySleep(TabManager::TabId id, bool manual);
  void recomputeProtectedReasons(TabPerformanceMetadata &meta, const TabManager::TabRecord *record);
  bool isLifecycleEligible(const TabManager::TabRecord *record) const;
  void scheduleNextDeadlineCheck();

  TabManager *tabManager_ = nullptr;
  std::unique_ptr<SystemMemoryPressureMonitor> memoryMonitor_;
  int64_t backgroundFreezeDelayOverrideMs_ = -1;  // -1: follow the inactivity timeout
  int64_t balancedFullSleepAfterMs_ = 0;           // 0: kBalancedFullSleepMinMs / factor rule
  int64_t backgroundDiscardDelayMs_ = kDefaultBackgroundDiscardDelayMs;
  DiscardPolicyRequirement discardRequirement_ = DiscardPolicyRequirement::LongIdleOrMemoryPressure;
  PerformancePolicyMode policyMode_ = PerformancePolicyMode::Balanced;
  bool discardEnabled_ = true;
  int64_t sleepAfterMs_ = 0;
  bool sleepMediaTabs_ = false;

  QTimer *deadlineTimer_ = nullptr;
  bool isTearingDown_ = false;
  quint64 deadlineCheckCount_ = 0;

  QStringList siteAllowlist_;

  QHash<TabManager::TabId, TabPerformanceMetadata> metadataMap_;
  QHash<const TabLifecycleAdapter *, TabManager::TabId> pageToIdMap_;
  QHash<TabManager::TabId, bool> explicitMediaPlayback_;
  QHash<TabManager::TabId, bool> explicitBrowserDownload_;
  QHash<TabManager::TabId, bool> explicitPinned_;
  QHash<TabManager::TabId, bool> explicitFormDirty_;
  QHash<TabManager::TabId, bool> explicitPermissionPrompt_;
  QHash<TabManager::TabId, bool> explicitPageDialog_;
};

} // namespace dalinira
