#pragma once

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QMap>
#include <QMutex>
#include <QReadWriteLock>
#include <memory>
#include <atomic>
#include <QObject>
#include <QPair>
#include <QRandomGenerator>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QWebEngineScript>
#include <QFutureWatcher>

#include "dalinira_blocker_engine.h"
#include "dalinira_blocker_filter_updates.h"
#include "dalinira_blocker_list_manager.h"
#include "dalinira_blocker_request_interceptor.h"
#include "dalinira_blocker_settings.h"
#include "dalinira_blocker_types.h"

struct TabRequestContext {
  quint64 tabId = 0;
  QUrl currentUrl;
  QString currentHost;
  qint64 lastActiveTimestamp = 0;
  // A cross-site first-party that the active filter lists identify as a
  // tracker. Its storage is temporary only when it forwards the tab again
  // within the bounded bounce window.
  QString temporaryStorageHost;
  qint64 temporaryStorageStartedAt = 0;
};

struct FilterUpdateResult {
  bool success = false;
  QString message;
  CompiledBlockerPlan plan;
};

class DaliNiraBlockerService;

// Thread-safe handle to a service's network decisions for engine adapters
// that answer requests off the GUI thread (the WebKit network filter). The
// service closes it first thing on destruction, waiting for any decision in
// progress, so a handle never reaches a destroyed service.
class DaliNiraBlockerNetworkGate final {
 public:
  explicit DaliNiraBlockerNetworkGate(DaliNiraBlockerService *service) : service_(service) {}
  // Allow (and nothing counted) once the service is gone.
  NetworkRequestVerdict decide(const QUrl &requestUrl, DaliNiraBlockerResourceType resourceType,
                               const QUrl &firstPartyUrl, quint64 tabId, const QString &requestMethod);
  void close();

 private:
  QReadWriteLock lock_;
  DaliNiraBlockerService *service_ = nullptr;
};

class DaliNiraBlockerService final : public QObject {
  Q_OBJECT
 public:
  explicit DaliNiraBlockerService(const QString &dataDir, QObject *parent = nullptr);
  ~DaliNiraBlockerService() override;

  DaliNiraBlockerSettings *settings() const;
  DaliNiraBlockerEngine *filterEngine() const;
  DaliNiraBlockerListManager *listManager() const;
  DaliNiraBlockerRequestInterceptor *requestInterceptor() const;
  // Chromium's source of packaged redirect substitutes; install it on every
  // QWebEngineProfile that uses requestInterceptor(), under
  // DaliNiraBlockerRequestInterceptor::resourceScheme().
  DaliNiraBlockerResourceSchemeHandler *resourceSchemeHandler() const { return resourceSchemeHandler_; }
  QString resourceSchemeToken() const { return resourceSchemeToken_; }
  SitePolicy sitePolicy(const QString &host) const;

  // Tab Context Registry (Thread-safe tab resolution from IO interceptor)
  void registerTab(quint64 tabId, const QUrl &url = QUrl());
  void unregisterTab(quint64 tabId);
  bool shouldForgetClosedHost(const QString &host) const;
  // Document-start cleanup installed only while a tab is visiting a
  // conservative bounce candidate. It clears script-visible first-party
  // state on a quick pagehide; native cookie/site-data cleanup is requested
  // separately once the redirect chain is confirmed.
  QString temporaryStorageCleanupScript(quint64 tabId, const QUrl &url) const;
  void updateTabUrl(quint64 tabId, const QUrl &url);
  void setActiveTabId(quint64 tabId);
  quint64 activeTabId() const;
  quint64 resolveTabId(const QUrl &firstPartyUrl, const QUrl &requestUrl) const;

  // Interceptor entry point (Thread-safe)
  RequestDecision evaluateRequest(const QUrl &requestUrl, int resourceTypeInt,
                                  const QUrl &firstPartyUrl, quint64 tabId,
                                  const QString &requestMethod = QStringLiteral("get"));
  RequestDecision evaluateRequest(const QUrl &requestUrl, DaliNiraBlockerResourceType resourceType,
                                  const QUrl &firstPartyUrl, quint64 tabId,
                                  const QString &requestMethod);

  // The single network decision both engines apply (thread-safe): strips
  // tracking parameters, evaluates the rules and site policy (counting real
  // blocks for `tabId`, or the resolved tab when 0) and returns what to do.
  NetworkRequestVerdict decideNetworkRequest(const QUrl &requestUrl, DaliNiraBlockerResourceType resourceType,
                                             const QUrl &firstPartyUrl, quint64 tabId,
                                             const QString &requestMethod);
  // HTTPS by default: the http:// URL to retry when the load of `failedUrl`
  // (an https:// URL this service upgraded in the last minute, or its http://
  // original) failed; the host then stays on http:// for this session. An
  // empty URL when the failure was not an upgrade's.
  QUrl httpsUpgradeFallback(const QUrl &failedUrl);
  // The upgraded load of `url` succeeded: it is no longer pending.
  void noteHttpsLoadSucceeded(const QUrl &url);

  // Removes every click-tracking query parameter (fbclid, gclid, utm_*, and
  // Brave's query-filter list) in one pass; all other parameters keep their
  // exact encoding.
  static bool stripTrackingParameters(QUrl &url);
  void setTrackingParameterStrippingEnabled(bool enabled) { stripTrackingEnabled_.store(enabled); }
  std::shared_ptr<DaliNiraBlockerNetworkGate> networkGate() const { return networkGate_; }

  // Tab stats management
  void reportBlockedEvent(quint64 tabId, DaliNiraBlockType type, quint64 count = 1,
                          const QString &ruleOrUrl = QString());
  void reportCosmeticBlock(quint64 tabId, quint64 count = 1,
                           const QString &ruleOrSelector = QStringLiteral("cosmetic")) {
    reportBlockedEvent(tabId, DaliNiraBlockType::Cosmetic, count, ruleOrSelector);
  }
  TabBlockerStats statsForTab(quint64 tabId) const;
  void clearTabStats(quint64 tabId);
  void resetAllStats();

  // Session & Global stats
  quint64 sessionBlockedCount() const;
  quint64 totalBlockedCount() const;
  quint64 totalTrackersBlockedCount() const;
  quint64 whitelistAllowedCount() const;
  quint64 estimatedBytesSaved() const;
  quint64 todayBlockedCount() const;
  quint64 weekBlockedCount() const;
  quint64 monthBlockedCount() const;
  quint64 totalAllowedCount() const;
  quint64 totalRedirectedCount() const;

  QList<QPair<QString, quint64>> topBlockedHosts(int limit = 10) const;
  QList<QPair<QString, quint64>> topMatchedRulesets(int limit = 10) const;

  // Ring buffer Network Logs
  QList<NetworkLogEntry> recentLogs(int maxCount = 100) const;

  // Real Diagnostic Performance & Memory Metrics
  double averageEvaluationTimeMs() const;
  double lastEvaluationTimeMs() const;
  double maxEvaluationTimeMs() const;
  quint64 evaluationCount() const;
  quint64 estimatedMemoryBytes() const;

  // Strict Blocking Temporary Bypass
  void allowTemporaryStrictBypass(const QString &host, int durationMinutes = 15);
  bool isStrictBypassActive(const QString &host) const;

  // Cosmetic stylesheet & Early Injection Script
  // `pageUrl` is the document the scripts are for (the host's root when
  // omitted): a $generichide exception for it leaves out generic cosmetics.
  QString cosmeticCssForHost(const QString &host, const QUrl &pageUrl = QUrl()) const;
  QWebEngineScript createCosmeticScriptForHost(const QString &host, const QUrl &pageUrl = QUrl()) const;
  QList<QWebEngineScript> createScriptingScriptsForHost(const QString &host, const QUrl &pageUrl = QUrl()) const;
  bool genericCosmeticsDisabled(const QString &host, const QUrl &pageUrl = QUrl()) const;

  // A page asks to open a new window or tab (window.open, target=_blank):
  // decided by the lists' and custom $popup rules for the opener's site,
  // logged and counted like a blocked request. Both engines call this before
  // any window is created.
  RequestDecision decidePopup(const QUrl &targetUrl, const QUrl &openerUrl, quint64 tabId);
  // Engine-neutral sources of the cosmetic + procedural document-start scripts,
  // in injection order, for engines that cannot consume QWebEngineScript (WebKit).
  // Empty when protection, the site's ad blocking, or a temporary pause disables them.
  QStringList cosmeticDocumentScriptsForHost(const QString &host, const QUrl &pageUrl = QUrl()) const;
  // The page-world privacy shield both engines run in every frame at
  // document start (privacy_shield_runtime.h): Global Privacy Control, and
  // fingerprinting protection seeded per site with this session's key.
  // Changes with the settings (exempt sites, fingerprintProtection).
  QString privacyShieldScript() const;

  // Re-build engine ruleset when settings or lists change
  void reloadRules();
  // Downloads the active lists that have a configured upstream source (one at
  // a time), stores the copies that pass validation, then verifies and
  // recompiles every active list and swaps the engine plan. Lists that fail
  // keep their previous copy. A call while an update runs is ignored.
  void updateFiltersAsync();
  bool isUpdatingFilters() const;
  // When autoUpdateRulesets is on, due lists are checked in the background
  // (never earlier than two minutes after start, then on each list's
  // interval, with backoff after failures). Invalid when nothing is scheduled.
  QDateTime nextAutomaticFilterUpdate() const;
  void setFilterFetchPolicyForTesting(const DaliNiraBlockerFilterFetcher::Policy &policy);
  void setAutomaticUpdateDelayFloorForTesting(int milliseconds);

 signals:
  void siteClosed(const QString &host);
  void siteOpened(const QString &host);
  // The last top-level owner left a confirmed short-lived tracker bounce.
  // Engine adapters use the narrowest origin-scoped deletion they expose.
  void temporarySiteDataExpired(const QString &host);
  void tabStatsChanged(quint64 tabId, const TabBlockerStats &stats);
  void globalStatsChanged(quint64 sessionBlocked, quint64 totalBlocked);
  void requestLogged(const NetworkLogEntry &entry);
  void autoReloadRequested();
  void filterUpdateStarted();
  void filterUpdateProgress(int completed, int total, const QString &stage);
  void filterUpdateFinished(bool success, const QString &message);

 private:
  QStringList activeRulesetIds() const;
  bool isTemporaryBounceCandidate(const QUrl &url, const QString &sourceSite) const;
  // Automatic link cleanup applies to a top-level GET navigation outside any
  // site bypass (never to POST bodies or subresources).
  bool linkCleanupApplies(const QUrl &url, const QUrl &firstPartyUrl, DaliNiraBlockerResourceType resourceType,
                          const QString &requestMethod) const;
  // A navigation URL with its tracking parameters removed: the built-in list
  // and every matching $removeparam rule, in one pass.
  QUrl cleanedNavigationUrl(const QUrl &url, const QString &requestMethod) const;
  void logEntry(const NetworkLogEntry &entry);
  void recordHostBlock(const QString &host);
  void recordRulesetMatch(const QString &rulesetId);
  void loadPersistentStats();
  void scheduleStatsPersistence();
  void persistStats();
  enum class RemoteListOutcome { Updated = 0, UpToDate, Failed, Skipped };
  QStringList remoteUpdateTargets(bool onlyDue) const;
  void startFilterUpdate(bool automatic);
  void fetchNextRemoteList();
  void storeFetchResult(const QString &listId, const FilterFetchResult &result);
  void rebuildAfterFilterUpdate();
  void finishFilterUpdate(bool success, const QString &message);
  QString remoteUpdateSummary() const;
  void scheduleAutomaticFilterUpdate();

  QString dataDir_;
  DaliNiraBlockerSettings *settings_ = nullptr;
  DaliNiraBlockerEngine *filterEngine_ = nullptr;
  DaliNiraBlockerListManager *listManager_ = nullptr;
  DaliNiraBlockerRequestInterceptor *interceptor_ = nullptr;
  DaliNiraBlockerResourceSchemeHandler *resourceSchemeHandler_ = nullptr;
  QString resourceSchemeToken_;

  mutable QMutex tabRegistryMutex_;
  QHash<quint64, TabRequestContext> tabRegistry_;
  quint64 activeTabId_ = 0;

  mutable QMutex statsMutex_;
  QHash<quint64, TabBlockerStats> tabStats_;
  quint64 sessionBlocked_ = 0;
  quint64 totalBlocked_ = 0;
  quint64 totalAllowed_ = 0;
  quint64 totalRedirected_ = 0;
  quint64 totalTrackersBlocked_ = 0;
  quint64 whitelistAllowed_ = 0;
  QHash<QString, quint64> blockedByHost_;
  QHash<QString, quint64> matchedByRuleset_;
  QMap<QString, quint64> dailyBlocked_;
  QTimer statsPersistTimer_;
  bool statsDirty_ = false;

  // Diagnostics timing metrics
  mutable QMutex timingMutex_;
  quint64 evaluationCount_ = 0;
  double totalEvaluationTimeMs_ = 0.0;
  double lastEvaluationTimeMs_ = 0.0;
  double maxEvaluationTimeMs_ = 0.0;

  // Strict block bypass map (host -> expiry msecs)
  mutable QMutex bypassMutex_;
  QHash<QString, qint64> strictBypassMap_;

  static constexpr int kMaxRingBufferLogs = 1000;
  mutable QMutex logMutex_;
  QList<NetworkLogEntry> logsRingBuffer_;
  QFutureWatcher<FilterUpdateResult> *filterUpdateWatcher_ = nullptr;
  quint64 planGeneration_ = 0;
  bool filterUpdateInProgress_ = false;
  // Remote phase of an update: one download, then one validation/storage
  // task on the thread pool, per list.
  DaliNiraBlockerFilterFetcher *filterFetcher_ = nullptr;
  DaliNiraBlockerFilterFetcher::Policy filterFetchPolicy_;
  QFutureWatcher<int> *remoteWorkWatcher_ = nullptr;
  QStringList remoteQueue_;
  QString remoteCurrentId_;
  int remoteTotal_ = 0;
  int remoteDone_ = 0;
  int remoteUpdated_ = 0;
  int remoteUpToDate_ = 0;
  int remoteFailed_ = 0;
  bool automaticRun_ = false;
  QTimer automaticUpdateTimer_;
  int automaticUpdateFloorMs_ = 120000;
  std::shared_ptr<DaliNiraBlockerNetworkGate> networkGate_;
  std::atomic<bool> stripTrackingEnabled_{true};
  // Seeds fingerprinting protection; new for every browser session and profile.
  const QString sessionKey_ = QString::number(QRandomGenerator::system()->generate64(), 16) +
                              QString::number(QRandomGenerator::system()->generate64(), 16);
  // HTTPS by default: pending upgrades (host -> original URL, time) and
  // hosts that fell back to http:// this session.
  QMutex httpsMutex_;
  QHash<QString, std::pair<QUrl, qint64>> httpsUpgrades_;
  QSet<QString> httpsFallbackHosts_;
  QSet<QString> pendingTemporarySiteExpiry_;
};

using AdBlockService = DaliNiraBlockerService;
