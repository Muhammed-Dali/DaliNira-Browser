#include "cosmetic_runtime.h"
#include "privacy_shield_runtime.h"
#include "dalinira_blocker_service.h"

#include <QDate>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMutexLocker>
#include <QRandomGenerator>
#include <QSettings>
#include <QUrlQuery>
#include <QtConcurrent>
#include <algorithm>
#include <chrono>
#include "i18n/i18n.h"

NetworkRequestVerdict DaliNiraBlockerNetworkGate::decide(const QUrl &requestUrl,
                                                         DaliNiraBlockerResourceType resourceType,
                                                         const QUrl &firstPartyUrl, quint64 tabId,
                                                         const QString &requestMethod) {
  QReadLocker locker(&lock_);
  if (!service_) return {};
  return service_->decideNetworkRequest(requestUrl, resourceType, firstPartyUrl, tabId, requestMethod);
}

void DaliNiraBlockerNetworkGate::close() {
  QWriteLocker locker(&lock_);
  service_ = nullptr;
}

DaliNiraBlockerService::DaliNiraBlockerService(const QString &dataDir, QObject *parent)
    : QObject(parent), dataDir_(dataDir), networkGate_(std::make_shared<DaliNiraBlockerNetworkGate>(this)) {
  settings_ = new DaliNiraBlockerSettings(dataDir + QStringLiteral("/adblock-settings.ini"), this);
  QFile::setPermissions(dataDir + QStringLiteral("/adblock-settings.ini"), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  filterEngine_ = new DaliNiraBlockerEngine();
  listManager_ = new DaliNiraBlockerListManager(dataDir, this);
  interceptor_ = new DaliNiraBlockerRequestInterceptor(this, this);
  {
    QByteArray token(16, Qt::Uninitialized);
    QRandomGenerator::system()->generate(reinterpret_cast<quint32 *>(token.data()),
                                         reinterpret_cast<quint32 *>(token.data() + token.size()));
    resourceSchemeToken_ = QString::fromLatin1(token.toHex());
  }
  resourceSchemeHandler_ =
      new DaliNiraBlockerResourceSchemeHandler(listManager_->redirectResources(), resourceSchemeToken_, this);

  statsPersistTimer_.setSingleShot(true);
  statsPersistTimer_.setInterval(5000);
  connect(&statsPersistTimer_, &QTimer::timeout, this, &DaliNiraBlockerService::persistStats);
  loadPersistentStats();

  auto onRulesModified = [this]() {
    reloadRules();
    if (settings_->autoReloadOnModeChange()) {
      emit autoReloadRequested();
    }
  };

  connect(settings_, &DaliNiraBlockerSettings::filteringPlanChanged, this, onRulesModified);
  // These are the only persisted settings that alter the compiled plan.  Do
  // not rebuild the entire ruleset for toolbar/developer/strict toggles, and
  // do not also listen to the umbrella settingsChanged signal: that caused
  // every mode/custom-list edit to parse and sort the rules twice on the GUI
  // thread.
  connect(settings_, &DaliNiraBlockerSettings::sitePoliciesChanged, this, [this]() {
    if (settings_->autoReloadOnModeChange()) emit autoReloadRequested();
  });
  reloadRules();

  automaticUpdateTimer_.setSingleShot(true);
  connect(&automaticUpdateTimer_, &QTimer::timeout, this, [this]() { startFilterUpdate(true); });
  connect(settings_, &DaliNiraBlockerSettings::settingsChanged, this,
          &DaliNiraBlockerService::scheduleAutomaticFilterUpdate);
  scheduleAutomaticFilterUpdate();
}

DaliNiraBlockerService::~DaliNiraBlockerService() {
  // Off-thread engine adapters must not start or finish a decision while the
  // members they use are destroyed.
  networkGate_->close();
  automaticUpdateTimer_.stop();
  if (filterFetcher_) filterFetcher_->cancel();
  // The workers capture listManager_ and its filter store.  Finish them
  // before QObject child destruction so closing the browser during a refresh
  // cannot dereference a deleted manager. This wait is only on shutdown,
  // never in the UI workflow.
  if (remoteWorkWatcher_ && !remoteWorkWatcher_->isFinished())
    remoteWorkWatcher_->waitForFinished();
  if (filterUpdateWatcher_ && !filterUpdateWatcher_->isFinished())
    filterUpdateWatcher_->waitForFinished();
  persistStats();
}

DaliNiraBlockerSettings *DaliNiraBlockerService::settings() const { return settings_; }
DaliNiraBlockerEngine *DaliNiraBlockerService::filterEngine() const { return filterEngine_; }
DaliNiraBlockerListManager *DaliNiraBlockerService::listManager() const { return listManager_; }
DaliNiraBlockerRequestInterceptor *DaliNiraBlockerService::requestInterceptor() const { return interceptor_; }
SitePolicy DaliNiraBlockerService::sitePolicy(const QString &host) const {
  return settings_ ? settings_->sitePolicy(host) : SitePolicy{};
}

QStringList DaliNiraBlockerService::activeRulesetIds() const {
  return listManager_->resolveRulesetIds(settings_->mode(), settings_->enabledRulesetIds(),
                                         settings_->rulesetSelectionConfigured());
}

void DaliNiraBlockerService::reloadRules() {
  ++planGeneration_;
  const DaliNiraBlockerMode mode = settings_->mode();
  const QStringList enabledIds = activeRulesetIds();
  const QStringList customLines = settings_->customFilters();

  const bool selectionConfigured = true;
  QSet<QString> badfilters;
  QList<FilterRule> rules = listManager_->loadRulesForModeAndSelection(
      mode, enabledIds, selectionConfigured, settings_->strictBlock(), customLines, &badfilters);
  const QString listCosmeticCss = listManager_->loadCosmeticCssForSelection(enabledIds, selectionConfigured);

  filterEngine_->applyCompiledPlan(DaliNiraBlockerEngine::compilePlan(
      std::move(rules), listCosmeticCss, customLines, {listManager_->redirectResources(), &badfilters}));
  if (qEnvironmentVariableIntValue("DALINIRA_FEATURE_DIAGNOSTICS") == 1) {
    qInfo().noquote() << "[BLOCKER] filters loaded";
  }
}

void DaliNiraBlockerService::updateFiltersAsync() { startFilterUpdate(false); }

QStringList DaliNiraBlockerService::remoteUpdateTargets(bool onlyDue) const {
  const DaliNiraBlockerFilterStore *store = listManager_->filterStore();
  if (!store) return {};
  // Disabled lists are not downloaded; they keep whatever copy they have.
  const QStringList active = activeRulesetIds();
  const QDateTime now = QDateTime::currentDateTimeUtc();
  QStringList targets;
  for (const FilterUpdateSource &source : store->sources()) {
    if (!active.contains(source.listId)) continue;
    if (onlyDue && store->nextCheckDue(source.listId) > now) continue;
    targets.append(source.listId);
  }
  targets.sort();
  return targets;
}

void DaliNiraBlockerService::startFilterUpdate(bool automatic) {
  if (filterUpdateInProgress_) return;
  const QStringList targets = remoteUpdateTargets(automatic);
  if (automatic && targets.isEmpty()) {
    scheduleAutomaticFilterUpdate();
    return;
  }
  automaticUpdateTimer_.stop();
  filterUpdateInProgress_ = true;
  automaticRun_ = automatic;
  remoteQueue_ = targets;
  remoteTotal_ = static_cast<int>(targets.size());
  remoteDone_ = remoteUpdated_ = remoteUpToDate_ = remoteFailed_ = 0;
  emit filterUpdateStarted();
  emit filterUpdateProgress(0, remoteTotal_ + 3, QStringLiteral("prepare"));
  fetchNextRemoteList();
}

void DaliNiraBlockerService::fetchNextRemoteList() {
  DaliNiraBlockerFilterStore *store = listManager_->filterStore();
  if (remoteQueue_.isEmpty() || !store) {
    // A background check that found nothing new does not recompile.
    if (automaticRun_ && remoteUpdated_ == 0) finishFilterUpdate(remoteFailed_ == 0, remoteUpdateSummary());
    else rebuildAfterFilterUpdate();
    return;
  }
  remoteCurrentId_ = remoteQueue_.takeFirst();
  const std::optional<FilterUpdateSource> source = store->source(remoteCurrentId_);
  const FilterListUpdateState state = store->state(remoteCurrentId_);
  // Conditional headers describe the current copy; only send them while that
  // copy is intact and came from this URL, so a 304 never leaves us without data.
  QString etag;
  QString lastModified;
  if (state.sourceUrl == source->url.toString() && store->activeSource(remoteCurrentId_) == QLatin1String("remote")) {
    etag = state.etag;
    lastModified = state.lastModified;
  }
  emit filterUpdateProgress(remoteDone_, remoteTotal_ + 3, QStringLiteral("download"));
  if (!filterFetcher_) {
    filterFetcher_ = new DaliNiraBlockerFilterFetcher(this);
    connect(filterFetcher_, &DaliNiraBlockerFilterFetcher::finished, this,
            [this](const FilterFetchResult &result) { storeFetchResult(remoteCurrentId_, result); });
  }
  filterFetcher_->setPolicy(filterFetchPolicy_);
  filterFetcher_->fetch(source->url, source->maximumBytes, etag, lastModified);
}

void DaliNiraBlockerService::storeFetchResult(const QString &listId, const FilterFetchResult &result) {
  DaliNiraBlockerFilterStore *store = listManager_->filterStore();
  const FilterUpdateSource source = *store->source(listId);
  const FilterListCopy current = store->state(listId).current;
  // Validation compiles the whole list, and storage writes it: off the GUI thread.
  const DaliNiraBlockerRedirectResources *resources = listManager_->redirectResources();
  auto future = QtConcurrent::run([store, resources, source, current, result]() -> int {
    FilterUpdateFailure failure = result.failure;
    int networkRules = 0;
    RemoteListOutcome outcome = RemoteListOutcome::Failed;
    if (result.kind == FilterFetchResult::Kind::NotModified) {
      store->recordNotModified(source.listId);
      outcome = RemoteListOutcome::UpToDate;
    } else if (result.kind == FilterFetchResult::Kind::Downloaded) {
      const FilterListValidation validation =
          validateFilterListDownload(result.body, result.contentType, source, current.networkRules, resources);
      failure = validation.failure;
      networkRules = validation.copy.networkRules;
      if (failure == FilterUpdateFailure::None &&
          !store->commitValidatedCopy(source.listId, result.body, validation.copy, source.url.toString(),
                                      result.etag, result.lastModified))
        failure = FilterUpdateFailure::Storage;
      if (failure == FilterUpdateFailure::None)
        outcome = validation.copy.sha256 == current.sha256 ? RemoteListOutcome::UpToDate : RemoteListOutcome::Updated;
    }
    if (outcome == RemoteListOutcome::Failed) {
      if (failure == FilterUpdateFailure::Cancelled) return static_cast<int>(RemoteListOutcome::Skipped);
      store->recordFailure(source.listId, failure);
    }
    if (qEnvironmentVariableIntValue("DALINIRA_FEATURE_DIAGNOSTICS") == 1) {
      static const char *const kOutcomeNames[] = {"updated", "up-to-date", "failed", "skipped"};
      qInfo().noquote() << "[BLOCKER] filter list" << source.listId << "host=" + source.url.host()
                        << "http=" + QString::number(result.httpStatus)
                        << "result=" + QString::fromLatin1(kOutcomeNames[static_cast<int>(outcome)])
                        << (failure == FilterUpdateFailure::None ? QString()
                                                                 : "failure=" + filterUpdateFailureName(failure))
                        << (networkRules > 0 ? "rules=" + QString::number(networkRules) : QString());
    }
    return static_cast<int>(outcome);
  });
  auto *watcher = new QFutureWatcher<int>(this);
  remoteWorkWatcher_ = watcher;
  connect(watcher, &QFutureWatcher<int>::finished, this, [this, watcher]() {
    const auto outcome = static_cast<RemoteListOutcome>(watcher->result());
    watcher->deleteLater();
    if (remoteWorkWatcher_ == watcher) remoteWorkWatcher_ = nullptr;
    if (outcome == RemoteListOutcome::Updated) ++remoteUpdated_;
    else if (outcome == RemoteListOutcome::UpToDate) ++remoteUpToDate_;
    else if (outcome == RemoteListOutcome::Failed) ++remoteFailed_;
    ++remoteDone_;
    fetchNextRemoteList();
  });
  watcher->setFuture(future);
}

void DaliNiraBlockerService::rebuildAfterFilterUpdate() {
  // Captured here, after the downloads: settings changed while they ran
  // already rebuilt the plan and are read afresh below.
  const quint64 generation = ++planGeneration_;
  const int offset = remoteTotal_;
  const DaliNiraBlockerMode mode = settings_->mode();
  const QStringList enabledIds = activeRulesetIds();
  constexpr bool selectionConfigured = true;
  const bool strictBlock = settings_->strictBlock();
  const QStringList customLines = settings_->customFilters();
  listManager_->invalidateCaches();

  auto progress = [this, generation, offset](int completed, const QString &stage) {
    QMetaObject::invokeMethod(this, [this, generation, offset, completed, stage]() {
      if (filterUpdateInProgress_ && generation == planGeneration_)
        emit filterUpdateProgress(offset + completed, offset + 3, stage);
    }, Qt::QueuedConnection);
  };
  auto future = QtConcurrent::run([this, mode, enabledIds, strictBlock, customLines, progress]() {
    const QString baseDir = listManager_->rulesetDir();
    if (baseDir.isEmpty() || !QFileInfo::exists(baseDir + QStringLiteral("/ruleset-details.json"))) {
      return FilterUpdateResult{false, I18n::text(QStringLiteral("blocker.bundled_pack_missing")), {}};
    }
    auto validJsonArray = [](const QString &path) {
      QFile file(path);
      if (!file.open(QIODevice::ReadOnly)) return false;
      QJsonParseError error;
      const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
      return error.error == QJsonParseError::NoError && document.isArray();
    };
    for (const QString &id : enabledIds) {
      const QString mainPath = baseDir + QStringLiteral("/main/") + id + QStringLiteral(".json");
      if (!validJsonArray(mainPath)) {
        return FilterUpdateResult{false,
            I18n::text(QStringLiteral("blocker.filter_pack_unverified")).arg(id), {}};
      }
      const QString regexPath = baseDir + QStringLiteral("/regex/") + id + QStringLiteral(".json");
      if (QFileInfo::exists(regexPath) && !validJsonArray(regexPath)) {
        return FilterUpdateResult{false,
            I18n::text(QStringLiteral("blocker.regex_pack_unverified")).arg(id), {}};
      }
    }
    QSet<QString> badfilters;
    QList<FilterRule> rules = listManager_->loadRulesForModeAndSelection(
        mode, enabledIds, selectionConfigured, strictBlock, customLines, &badfilters);
    progress(1, QStringLiteral("parse"));
    const QString css = listManager_->loadCosmeticCssForSelection(enabledIds, selectionConfigured);
    progress(2, QStringLiteral("cosmetic"));
    return FilterUpdateResult{true, I18n::text(QStringLiteral("blocker.local_filters_verified")),
        DaliNiraBlockerEngine::compilePlan(std::move(rules), css, customLines,
                                           {listManager_->redirectResources(), &badfilters})};
  });

  auto *watcher = new QFutureWatcher<FilterUpdateResult>(this);
  filterUpdateWatcher_ = watcher;
  connect(watcher, &QFutureWatcher<FilterUpdateResult>::finished, this, [this, watcher, generation, offset]() {
    const bool current = generation == planGeneration_;
    FilterUpdateResult result = watcher->result();
    // One swap under the engine lock: requests see the old plan or the new
    // one, never a mix, whichever engine asks.
    if (current && result.success) filterEngine_->applyCompiledPlan(std::move(result.plan));
    watcher->deleteLater();
    if (filterUpdateWatcher_ == watcher) filterUpdateWatcher_ = nullptr;
    if (!current) {
      finishFilterUpdate(false, I18n::text(QStringLiteral("blocker.settings_changed_stale")));
      return;
    }
    if (!result.success) {
      finishFilterUpdate(false, result.message);
      return;
    }
    emit filterUpdateProgress(offset + 3, offset + 3, QStringLiteral("swap"));
    const bool manual = !automaticRun_;
    finishFilterUpdate(remoteFailed_ == 0, remoteTotal_ == 0 ? result.message : remoteUpdateSummary());
    // Background updates never reload the user's pages.
    if (manual && settings_->autoReloadOnModeChange()) emit autoReloadRequested();
  });
  watcher->setFuture(future);
}

QString DaliNiraBlockerService::remoteUpdateSummary() const {
  QString message = I18n::text(QStringLiteral("blocker.filters_update_summary"))
                        .arg(remoteUpdated_).arg(remoteUpToDate_).arg(remoteFailed_);
  if (remoteFailed_ > 0) message += QLatin1Char(' ') + I18n::text(QStringLiteral("blocker.filters_update_kept_previous"));
  return message;
}

void DaliNiraBlockerService::finishFilterUpdate(bool success, const QString &message) {
  filterUpdateInProgress_ = false;
  automaticRun_ = false;
  remoteQueue_.clear();
  emit filterUpdateFinished(success, message);
  scheduleAutomaticFilterUpdate();
}

void DaliNiraBlockerService::scheduleAutomaticFilterUpdate() {
  if (!settings_->autoUpdateRulesets()) {
    automaticUpdateTimer_.stop();
    return;
  }
  if (filterUpdateInProgress_) return;  // rescheduled when it finishes
  const DaliNiraBlockerFilterStore *store = listManager_->filterStore();
  const QStringList targets = remoteUpdateTargets(false);
  if (!store || targets.isEmpty()) {
    automaticUpdateTimer_.stop();
    return;
  }
  QDateTime earliest;
  for (const QString &id : targets) {
    const QDateTime due = store->nextCheckDue(id);
    if (!earliest.isValid() || due < earliest) earliest = due;
  }
  constexpr qint64 kMaximumTimerMs = 24LL * 3600 * 1000;  // re-evaluated when it fires
  const qint64 delay = qBound<qint64>(automaticUpdateFloorMs_,
                                      QDateTime::currentDateTimeUtc().msecsTo(earliest), kMaximumTimerMs);
  // Frequent settings changes must not keep postponing a pending check.
  if (automaticUpdateTimer_.isActive() && automaticUpdateTimer_.remainingTime() <= delay) return;
  automaticUpdateTimer_.start(static_cast<int>(delay));
}

QDateTime DaliNiraBlockerService::nextAutomaticFilterUpdate() const {
  if (!automaticUpdateTimer_.isActive()) return {};
  return QDateTime::currentDateTime().addMSecs(automaticUpdateTimer_.remainingTime());
}

void DaliNiraBlockerService::setFilterFetchPolicyForTesting(const DaliNiraBlockerFilterFetcher::Policy &policy) {
  filterFetchPolicy_ = policy;
}

void DaliNiraBlockerService::setAutomaticUpdateDelayFloorForTesting(int milliseconds) {
  automaticUpdateFloorMs_ = qMax(0, milliseconds);
  automaticUpdateTimer_.stop();
  scheduleAutomaticFilterUpdate();
}

bool DaliNiraBlockerService::isUpdatingFilters() const { return filterUpdateInProgress_; }

// ---------------- Tab Context Registry ----------------

void DaliNiraBlockerService::registerTab(quint64 tabId, const QUrl &url) {
  if (tabId == 0) return;
  QMutexLocker locker(&tabRegistryMutex_);
  TabRequestContext ctx;
  ctx.tabId = tabId;
  ctx.currentUrl = url;
  ctx.currentHost = url.host().toLower();
  emit siteOpened(DaliNiraBlockerSettings::normalizeSiteHost(ctx.currentHost));
  ctx.lastActiveTimestamp = QDateTime::currentMSecsSinceEpoch();
  tabRegistry_[tabId] = ctx;
  if (activeTabId_ == 0) activeTabId_ = tabId;
}

void DaliNiraBlockerService::unregisterTab(quint64 tabId) {
  if (tabId == 0) return;
  QString closedHost;
  QString temporaryHost;
  {
    QMutexLocker locker(&tabRegistryMutex_);
    const TabRequestContext context = tabRegistry_.value(tabId);
    closedHost = DaliNiraBlockerSettings::normalizeSiteHost(context.currentHost);
    temporaryHost = context.temporaryStorageHost;
    if (!temporaryHost.isEmpty() &&
        QDateTime::currentMSecsSinceEpoch() - context.temporaryStorageStartedAt <= 10000)
      pendingTemporarySiteExpiry_.insert(temporaryHost);
    tabRegistry_.remove(tabId);
    for (const auto &context : std::as_const(tabRegistry_)) {
      if (DaliNiraBlockerSettings::normalizeSiteHost(context.currentHost) == closedHost) { closedHost.clear(); break; }
    }
    if (activeTabId_ == tabId) {
      activeTabId_ = tabRegistry_.isEmpty() ? 0 : tabRegistry_.keys().first();
    }
  }
  clearTabStats(tabId);
  if (!closedHost.isEmpty()) emit siteClosed(closedHost);
  if (!temporaryHost.isEmpty()) {
    QMutexLocker locker(&tabRegistryMutex_);
    const bool stillOpen = std::any_of(tabRegistry_.cbegin(), tabRegistry_.cend(), [&temporaryHost](const auto &context) {
      return DaliNiraBlockerEngine::siteForHost(context.currentHost) == temporaryHost;
    });
    if (!stillOpen && pendingTemporarySiteExpiry_.remove(temporaryHost)) {
      locker.unlock();
      emit temporarySiteDataExpired(temporaryHost);
    }
  }
}

void DaliNiraBlockerService::updateTabUrl(quint64 tabId, const QUrl &url) {
  if (tabId == 0) return;
  QString expiredHost;
  QMutexLocker locker(&tabRegistryMutex_);
  if (!tabRegistry_.contains(tabId)) {
    TabRequestContext ctx;
    ctx.tabId = tabId;
    ctx.currentUrl = url;
    ctx.currentHost = url.host().toLower();
    ctx.lastActiveTimestamp = QDateTime::currentMSecsSinceEpoch();
    tabRegistry_[tabId] = ctx;
    return;
  }
  TabRequestContext &ctx = tabRegistry_[tabId];
  const QString newHost = url.host().toLower();
  emit siteOpened(DaliNiraBlockerSettings::normalizeSiteHost(newHost));
  const QString scheme = url.scheme().toLower();
  const bool wentInternal = (scheme == QLatin1String("dalinira") ||
                             scheme == QLatin1String("about") ||
                             scheme == QLatin1String("data") ||
                             scheme == QLatin1String("file"));
  const QString oldNorm = DaliNiraBlockerSettings::normalizeSiteHost(ctx.currentHost);
  const QString newNorm = DaliNiraBlockerSettings::normalizeSiteHost(newHost);
  const QString oldSite = DaliNiraBlockerEngine::siteForHost(ctx.currentHost);
  const QString newSite = DaliNiraBlockerEngine::siteForHost(newHost);
  const bool hostChanged = (!oldNorm.isEmpty() && !newNorm.isEmpty() && oldNorm != newNorm);
  const qint64 now = QDateTime::currentMSecsSinceEpoch();

  if ((oldSite != newSite || wentInternal) && ctx.temporaryStorageHost == oldSite) {
    if (now - ctx.temporaryStorageStartedAt <= 10000)
      pendingTemporarySiteExpiry_.insert(oldSite);
    ctx.temporaryStorageHost.clear();
    ctx.temporaryStorageStartedAt = 0;
  }

  ctx.currentUrl = url;
  ctx.currentHost = newHost;
  ctx.lastActiveTimestamp = now;

  if (!oldSite.isEmpty() && oldSite != newSite && isTemporaryBounceCandidate(url, oldSite)) {
    ctx.temporaryStorageHost = newSite;
    ctx.temporaryStorageStartedAt = now;
  }

  if (!oldSite.isEmpty() && pendingTemporarySiteExpiry_.contains(oldSite)) {
    const bool stillOpen = std::any_of(tabRegistry_.cbegin(), tabRegistry_.cend(), [&oldSite](const auto &context) {
      return DaliNiraBlockerEngine::siteForHost(context.currentHost) == oldSite;
    });
    if (!stillOpen) {
      pendingTemporarySiteExpiry_.remove(oldSite);
      expiredHost = oldSite;
    }
  }

  if (hostChanged || wentInternal) {
    locker.unlock();
    clearTabStats(tabId);
    if (!expiredHost.isEmpty()) emit temporarySiteDataExpired(expiredHost);
  }
}

bool DaliNiraBlockerService::isTemporaryBounceCandidate(const QUrl &url, const QString &sourceSite) const {
  const QString site = DaliNiraBlockerEngine::siteForHost(url.host());
  if (site.isEmpty() || sourceSite.isEmpty() || site == sourceSite || !settings_->protectionEnabled()) return false;
  const SitePolicy sourcePolicy = settings_->sitePolicy(sourceSite);
  const SitePolicy targetPolicy = settings_->sitePolicy(site);
  if (!sourcePolicy.trackerProtection || sourcePolicy.whitelisted || targetPolicy.whitelisted ||
      sourcePolicy.temporaryDisabledUntil > QDateTime::currentMSecsSinceEpoch()) return false;

  // Authentication and checkout redirects deliberately carry short-lived
  // first-party state. Never make them ephemeral merely because a broad list
  // also classifies one of their resources as tracking.
  const QString compatibilityKey = (url.host() + url.path()).toLower();
  static const QStringList compatibilityTokens = {
      QStringLiteral("account"), QStringLiteral("auth"), QStringLiteral("login"), QStringLiteral("oauth"),
      QStringLiteral("signin"), QStringLiteral("sso"), QStringLiteral("checkout"), QStringLiteral("payment"),
      QStringLiteral("billing"), QStringLiteral("paypal"), QStringLiteral("3ds"), QStringLiteral("securepay")};
  for (const QString &token : compatibilityTokens)
    if (compatibilityKey.contains(token)) return false;

  const DaliNiraBlockerMode mode = sourcePolicy.perSiteMode >= 0
      ? static_cast<DaliNiraBlockerMode>(sourcePolicy.perSiteMode) : settings_->mode();
  // Ask the already-compiled filter plan how this exact URL is treated as a
  // third-party resource. A first-party navigation itself is intentionally
  // allowed by many privacy lists; the third-party verdict is the tracker
  // classification signal, not a second independent hostname list.
  const RequestDecision classified = filterEngine_->evaluate(
      url, DaliNiraBlockerResourceType::Other, sourceSite, mode, sourcePolicy, QStringLiteral("get"));
  return classified.action == DaliNiraBlockerAction::Block;
}

QString DaliNiraBlockerService::temporaryStorageCleanupScript(quint64 tabId, const QUrl &url) const {
  const QString site = DaliNiraBlockerEngine::siteForHost(url.host());
  {
    QMutexLocker locker(&tabRegistryMutex_);
    const auto it = tabRegistry_.constFind(tabId);
    if (it == tabRegistry_.cend() || it->temporaryStorageHost != site) return {};
  }
  return QStringLiteral(R"JS((()=>{
    if(window.__daliniraTemporaryBounceStorage)return;
    Object.defineProperty(window,'__daliniraTemporaryBounceStorage',{value:true});
    const started=performance.now();
    addEventListener('pagehide',()=>{
      if(performance.now()-started>10000)return;
      try{localStorage.clear();sessionStorage.clear()}catch(_){}
      try{indexedDB.databases().then(list=>list.slice(0,256).forEach(db=>indexedDB.deleteDatabase(db.name))).catch(()=>{})}catch(_){}
      try{caches.keys().then(keys=>keys.slice(0,256).forEach(key=>caches.delete(key))).catch(()=>{})}catch(_){}
      try{navigator.serviceWorker.getRegistrations().then(list=>list.slice(0,256).forEach(reg=>reg.unregister())).catch(()=>{})}catch(_){}
    },{once:true});
  })())JS");
}

void DaliNiraBlockerService::setActiveTabId(quint64 tabId) {
  QMutexLocker locker(&tabRegistryMutex_);
  activeTabId_ = tabId;
  if (tabRegistry_.contains(tabId)) {
    tabRegistry_[tabId].lastActiveTimestamp = QDateTime::currentMSecsSinceEpoch();
  }
}

quint64 DaliNiraBlockerService::activeTabId() const {
  QMutexLocker locker(&tabRegistryMutex_);
  return activeTabId_;
}

quint64 DaliNiraBlockerService::resolveTabId(const QUrl &firstPartyUrl, const QUrl &requestUrl) const {
  QMutexLocker locker(&tabRegistryMutex_);
  if (tabRegistry_.isEmpty()) return 0;
  if (tabRegistry_.size() == 1) return tabRegistry_.keys().first();

  const QString firstPartyHost = firstPartyUrl.host().toLower();
  const QString requestHost = requestUrl.host().toLower();

  // 1. Exact match on full firstPartyUrl
  if (firstPartyUrl.isValid() && !firstPartyUrl.isEmpty()) {
    for (auto it = tabRegistry_.constBegin(); it != tabRegistry_.constEnd(); ++it) {
      if (it.value().currentUrl == firstPartyUrl) {
        return it.key();
      }
    }
  }

  // 2. Match on firstParty host with most recent activity (normalized and subdomain matching)
  if (!firstPartyHost.isEmpty()) {
    const QString firstPartyNorm = DaliNiraBlockerSettings::normalizeSiteHost(firstPartyHost);
    quint64 bestTab = 0;
    qint64 latestTs = -1;
    for (auto it = tabRegistry_.constBegin(); it != tabRegistry_.constEnd(); ++it) {
      const QString tabNorm = DaliNiraBlockerSettings::normalizeSiteHost(it.value().currentHost);
      if (it.value().currentHost == firstPartyHost || tabNorm == firstPartyNorm ||
          (!firstPartyNorm.isEmpty() && !tabNorm.isEmpty() &&
           (firstPartyNorm.endsWith(QLatin1String(".") + tabNorm) ||
            tabNorm.endsWith(QLatin1String(".") + firstPartyNorm)))) {
        if (it.value().lastActiveTimestamp > latestTs) {
          latestTs = it.value().lastActiveTimestamp;
          bestTab = it.key();
        }
      }
    }
    if (bestTab != 0) return bestTab;
  }

  // 3. Match on requestHost for main frame navigations or direct requests
  if (!requestHost.isEmpty()) {
    const QString requestNorm = DaliNiraBlockerSettings::normalizeSiteHost(requestHost);
    for (auto it = tabRegistry_.constBegin(); it != tabRegistry_.constEnd(); ++it) {
      const QString tabNorm = DaliNiraBlockerSettings::normalizeSiteHost(it.value().currentHost);
      if (it.value().currentHost == requestHost ||
          (!requestNorm.isEmpty() && tabNorm == requestNorm)) {
        return it.key();
      }
    }
  }

  // 4. Fallback to active tab
  return activeTabId_ != 0 ? activeTabId_ : tabRegistry_.keys().first();
}

// ---------------- Strict Bypass ----------------

void DaliNiraBlockerService::allowTemporaryStrictBypass(const QString &host, int durationMinutes) {
  QString clean = host.trimmed().toLower();
  if (clean.startsWith(QStringLiteral("www."))) clean.remove(0, 4);
  if (clean.isEmpty()) return;
  QMutexLocker locker(&bypassMutex_);
  strictBypassMap_[clean] = QDateTime::currentMSecsSinceEpoch() + (durationMinutes * 60 * 1000);
}

bool DaliNiraBlockerService::isStrictBypassActive(const QString &host) const {
  QString clean = host.trimmed().toLower();
  if (clean.startsWith(QStringLiteral("www."))) clean.remove(0, 4);
  if (clean.isEmpty()) return false;
  QMutexLocker locker(&bypassMutex_);
  const qint64 exp = strictBypassMap_.value(clean, 0);
  return exp > QDateTime::currentMSecsSinceEpoch();
}

// ---------------- Request Evaluation ----------------

bool DaliNiraBlockerService::stripTrackingParameters(QUrl &url) {
  if (!url.hasQuery()) return false;
  // Keep untouched query components byte-for-byte: decoding/re-encoding can
  // invalidate signatures and change the meaning of encoded delimiters.
  const QStringList items = url.query(QUrl::FullyEncoded).split(QLatin1Char('&'));
  QStringList kept;
  bool changed = false;
  // Brave's query-filter list (brave-core query_filter.cc), plus mc_cid and
  // _hsmi; names are compared lower-cased.
  static const QSet<QString> trackingKeys = {
      QStringLiteral("__hsfp"), QStringLiteral("__hssc"), QStringLiteral("__hstc"), QStringLiteral("__s"),
      QStringLiteral("_bhlid"), QStringLiteral("_branch_match_id"), QStringLiteral("_branch_referrer"),
      QStringLiteral("_gl"), QStringLiteral("_hsenc"), QStringLiteral("_hsmi"), QStringLiteral("_kx"),
      QStringLiteral("_openstat"), QStringLiteral("at_recipient_id"), QStringLiteral("at_recipient_list"),
      QStringLiteral("bbeml"), QStringLiteral("bsft_clkid"), QStringLiteral("bsft_uid"), QStringLiteral("dclid"),
      QStringLiteral("et_rid"), QStringLiteral("fb_action_ids"), QStringLiteral("fb_comment_id"),
      QStringLiteral("fbclid"), QStringLiteral("gbraid"), QStringLiteral("gclid"), QStringLiteral("guce_referrer"),
      QStringLiteral("guce_referrer_sig"), QStringLiteral("hsctatracking"), QStringLiteral("igshid"),
      QStringLiteral("irclickid"), QStringLiteral("mc_cid"), QStringLiteral("mc_eid"), QStringLiteral("mkt_tok"),
      QStringLiteral("ml_subscriber"), QStringLiteral("ml_subscriber_hash"), QStringLiteral("msclkid"),
      QStringLiteral("mtm_cid"), QStringLiteral("oft_c"), QStringLiteral("oft_ck"), QStringLiteral("oft_d"),
      QStringLiteral("oft_id"), QStringLiteral("oft_ids"), QStringLiteral("oft_k"), QStringLiteral("oft_lk"),
      QStringLiteral("oft_sk"), QStringLiteral("oly_anon_id"), QStringLiteral("oly_enc_id"),
      QStringLiteral("pk_cid"), QStringLiteral("rb_clickid"), QStringLiteral("s_cid"),
      QStringLiteral("sc_customer"), QStringLiteral("sc_eh"), QStringLiteral("sc_uid"), QStringLiteral("srsltid"),
      QStringLiteral("ss_email_id"), QStringLiteral("twclid"), QStringLiteral("unicorn_click_id"),
      QStringLiteral("vero_conv"), QStringLiteral("vero_id"), QStringLiteral("vgo_ee"), QStringLiteral("wbraid"),
      QStringLiteral("wickedid"), QStringLiteral("yclid"), QStringLiteral("ymclid"), QStringLiteral("ysclid")};
  // Marketo unsubscribe links need their token (Brave keeps it there too).
  const bool unsubscribe = url.path().contains(QLatin1String("unsubscribe"), Qt::CaseInsensitive);
  for (const QString &item : items) {
    const QString key = QUrl::fromPercentEncoding(item.section(QLatin1Char('='), 0, 0).toUtf8()).toLower();
    // Signed/authentication navigations must retain their complete query.
    if (key == QLatin1String("sig") || key == QLatin1String("signature") ||
        key == QLatin1String("token") || key == QLatin1String("state") ||
        key.startsWith(QLatin1String("x-amz-")) || key.startsWith(QLatin1String("x-goog-"))) return false;
    const bool tracking = (trackingKeys.contains(key) && !(unsubscribe && key == QLatin1String("mkt_tok"))) ||
                          key.startsWith(QLatin1String("utm_"));
    if (tracking) changed = true;
    else kept.append(item);
  }
  if (changed) url.setQuery(kept.isEmpty() ? QString() : kept.join(QLatin1Char('&')), QUrl::StrictMode);
  return changed;
}

NetworkRequestVerdict DaliNiraBlockerService::decideNetworkRequest(const QUrl &requestUrl,
                                                                   DaliNiraBlockerResourceType resourceType,
                                                                   const QUrl &firstPartyUrl, quint64 tabId,
                                                                   const QString &requestMethod) {
  NetworkRequestVerdict verdict;
  const QString scheme = requestUrl.scheme().toLower();
  if (!requestUrl.isValid() || (scheme != QLatin1String("http") && scheme != QLatin1String("https")))
    return verdict;

  QUrl url = requestUrl;
  const bool stripped = linkCleanupApplies(url, firstPartyUrl, resourceType, requestMethod) &&
                        stripTrackingParameters(url);
  // The rules see the URL without the built-in list's parameters; their own
  // $removeparam rules then all apply in this same evaluation.
  verdict.decision = evaluateRequest(url, resourceType, firstPartyUrl, tabId, requestMethod);
  if (verdict.decision.action == DaliNiraBlockerAction::Block) {
    verdict.kind = NetworkVerdictKind::Block;
  } else if (verdict.decision.action == DaliNiraBlockerAction::Redirect && !verdict.decision.redirectUrl.isEmpty()) {
    verdict.kind = NetworkVerdictKind::Redirect;
    verdict.redirectUrl = QUrl(verdict.decision.redirectUrl);
    // A debounced destination is cleaned here too: Chromium would otherwise
    // clean it on a second round trip and WebKit (which rewrites the request
    // in place) not at all.
    if (verdict.decision.reason == QLatin1String("urlskip-debounce"))
      verdict.redirectUrl = cleanedNavigationUrl(verdict.redirectUrl, requestMethod);
  } else if (stripped) {
    verdict.kind = NetworkVerdictKind::Redirect;
    verdict.redirectUrl = url;
  }
  return verdict;
}

QUrl DaliNiraBlockerService::httpsUpgradeFallback(const QUrl &failedUrl) {
  const QString host = failedUrl.host().toLower();
  if (host.isEmpty()) return {};
  QMutexLocker locker(&httpsMutex_);
  const auto pending = httpsUpgrades_.constFind(host);
  if (pending == httpsUpgrades_.constEnd() ||
      QDateTime::currentMSecsSinceEpoch() - pending->second > 60000)
    return {};
  const QUrl original = pending->first;
  httpsUpgrades_.remove(host);
  httpsFallbackHosts_.insert(host);
  return original;
}

void DaliNiraBlockerService::noteHttpsLoadSucceeded(const QUrl &url) {
  if (url.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) != 0) return;
  QMutexLocker locker(&httpsMutex_);
  httpsUpgrades_.remove(url.host().toLower());
}

bool DaliNiraBlockerService::linkCleanupApplies(const QUrl &url, const QUrl &firstPartyUrl,
                                                DaliNiraBlockerResourceType resourceType,
                                                const QString &requestMethod) const {
  const QString policyHost = firstPartyUrl.host().isEmpty() ? url.host() : firstPartyUrl.host();
  const SitePolicy policy = settings_->sitePolicy(policyHost);
  return stripTrackingEnabled_.load() && settings_->protectionEnabled() && policy.trackerProtection &&
         !policy.whitelisted && policy.temporaryDisabledUntil <= QDateTime::currentMSecsSinceEpoch() &&
         !settings_->sitePolicy(url.host()).whitelisted && resourceType == DaliNiraBlockerResourceType::MainFrame &&
         requestMethod.trimmed().compare(QLatin1String("get"), Qt::CaseInsensitive) == 0;
}

QUrl DaliNiraBlockerService::cleanedNavigationUrl(const QUrl &url, const QString &requestMethod) const {
  QUrl cleaned = url;
  if (!linkCleanupApplies(cleaned, cleaned, DaliNiraBlockerResourceType::MainFrame, requestMethod)) return cleaned;
  stripTrackingParameters(cleaned);
  const SitePolicy policy = settings_->sitePolicy(cleaned.host());
  const DaliNiraBlockerMode mode = (policy.perSiteMode >= 0) ? static_cast<DaliNiraBlockerMode>(policy.perSiteMode)
                                                             : settings_->mode();
  const RequestDecision rules = filterEngine_->evaluate(cleaned, DaliNiraBlockerResourceType::MainFrame,
                                                        cleaned.host().toLower(), mode, policy, requestMethod);
  if (rules.action == DaliNiraBlockerAction::Redirect && rules.reason == QLatin1String("dnr-remove-param"))
    cleaned = QUrl(rules.redirectUrl);
  return cleaned;
}

RequestDecision DaliNiraBlockerService::evaluateRequest(const QUrl &requestUrl, int resourceTypeInt,
                                                const QUrl &firstPartyUrl, quint64 tabId,
                                                const QString &requestMethod) {
  return evaluateRequest(requestUrl, resourceTypeFromWebEngine(resourceTypeInt), firstPartyUrl, tabId,
                         requestMethod);
}

RequestDecision DaliNiraBlockerService::evaluateRequest(const QUrl &requestUrl,
                                                        DaliNiraBlockerResourceType resType,
                                                        const QUrl &firstPartyUrl, quint64 tabId,
                                                        const QString &requestMethod) {
  const auto start = std::chrono::high_resolution_clock::now();

  const quint64 resolvedTab = (tabId > 0) ? tabId : resolveTabId(firstPartyUrl, requestUrl);
  const QString initiatorHost = firstPartyUrl.host().toLower();
  const QString siteHost = requestUrl.host().toLower();
  SitePolicy policy = settings_->sitePolicy(initiatorHost.isEmpty() ? siteHost : initiatorHost);
  if (!policy.whitelisted && !siteHost.isEmpty()) {
    const SitePolicy siteSelfPolicy = settings_->sitePolicy(siteHost);
    if (siteSelfPolicy.whitelisted) policy = siteSelfPolicy;
  }

  const QString firstScheme = firstPartyUrl.scheme().toLower();
  const QString reqScheme = requestUrl.scheme().toLower();
  if (firstScheme == QLatin1String("dalinira") || firstScheme == QLatin1String("about") ||
      firstScheme == QLatin1String("data") || firstScheme == QLatin1String("file") ||
      reqScheme == QLatin1String("dalinira") || reqScheme == QLatin1String("about") ||
      reqScheme == QLatin1String("data") || reqScheme == QLatin1String("file") ||
      reqScheme == QLatin1String("qrc")) {
    return RequestDecision{DaliNiraBlockerAction::Allow, QStringLiteral("internal-scheme"),
                           0, QStringLiteral("dalinira-internal"), QString()};
  }

  RequestDecision decision;
  bool isLocal = false;
  const QString reqHost = requestUrl.host().toLower();
  if (reqHost.isEmpty() || reqHost == QLatin1String("localhost") || reqHost.endsWith(QLatin1String(".local")) ||
      reqHost.endsWith(QLatin1String(".test")) || reqHost.endsWith(QLatin1String(".invalid")) ||
      reqHost.endsWith(QLatin1String(".onion"))) {
    isLocal = true;
  } else {
    const QHostAddress addr(reqHost);
    if (!addr.isNull() || reqHost.contains(QLatin1Char(':'))) {
      isLocal = true;
    }
  }

  if (!settings_->protectionEnabled()) {
    decision = RequestDecision{DaliNiraBlockerAction::Allow, QStringLiteral("global-protection-disabled"),
                               0, QStringLiteral("dalinira-global"), QString()};
  } else if (!policy.whitelisted && policy.upgradeHttps && reqScheme == QLatin1String("http") && !isLocal && !requestUrl.isLocalFile()) {
    QUrl upgraded = requestUrl;
    upgraded.setScheme(QStringLiteral("https"));
    decision = RequestDecision{DaliNiraBlockerAction::Redirect, QStringLiteral("https-upgrade-by-policy"),
                               0, QStringLiteral("dalinira-https-upgrade"), upgraded.toString()};
  } else if (!policy.whitelisted && policy.blockScripts && resType == DaliNiraBlockerResourceType::Script) {
    decision = RequestDecision{DaliNiraBlockerAction::Block, QStringLiteral("scripts-blocked-by-policy"),
                               0, QStringLiteral("dalinira-script-block"), QString()};
  } else {
    const DaliNiraBlockerMode activeMode = (policy.perSiteMode >= 0) ?
        static_cast<DaliNiraBlockerMode>(policy.perSiteMode) : settings_->mode();
    decision = filterEngine_->evaluate(
        requestUrl, resType, initiatorHost, activeMode, policy, requestMethod);
  }

  // Debouncing: a top-level GET navigation to a bounce-tracking redirector
  // (a $urlskip rule) goes straight to the destination it forwards to. Both
  // engines apply this one verdict. A destination the filters would block is
  // not skipped to: the redirector's own response then leads there, and that
  // navigation is decided like any other.
  if (resType == DaliNiraBlockerResourceType::MainFrame && settings_->protectionEnabled() &&
      policy.trackerProtection && !policy.whitelisted &&
      policy.temporaryDisabledUntil <= QDateTime::currentMSecsSinceEpoch() &&
      requestMethod.trimmed().compare(QLatin1String("get"), Qt::CaseInsensitive) == 0) {
    QString urlSkipRuleset;
    const QUrl target = filterEngine_->urlSkipTarget(requestUrl, decision.action == DaliNiraBlockerAction::Block,
                                                     &urlSkipRuleset);
    if (target.isValid()) {
      const SitePolicy targetPolicy = settings_->sitePolicy(target.host());
      const DaliNiraBlockerMode targetMode = (targetPolicy.perSiteMode >= 0) ?
          static_cast<DaliNiraBlockerMode>(targetPolicy.perSiteMode) : settings_->mode();
      const RequestDecision targetDecision = filterEngine_->evaluate(
          target, DaliNiraBlockerResourceType::MainFrame, target.host().toLower(), targetMode, targetPolicy,
          requestMethod);
      if (targetDecision.action != DaliNiraBlockerAction::Block)
        decision = RequestDecision{DaliNiraBlockerAction::Redirect, QStringLiteral("urlskip-debounce"), 0,
                                   urlSkipRuleset, target.toString(QUrl::FullyEncoded)};
    }
  }

  // HTTPS by default: a top-level GET navigation to a public http:// host on
  // the default port goes to https:// (Brave's standard behavior). A failed
  // HTTPS load falls back to http:// once (httpsUpgradeFallback); a site
  // that redirects the upgraded navigation back to http:// within seconds
  // falls back at once instead of looping.
  if (decision.action == DaliNiraBlockerAction::Allow && resType == DaliNiraBlockerResourceType::MainFrame &&
      reqScheme == QLatin1String("http") && !isLocal && requestUrl.port(80) == 80 && settings_->protectionEnabled() &&
      settings_->httpsByDefault() && !policy.whitelisted &&
      policy.temporaryDisabledUntil <= QDateTime::currentMSecsSinceEpoch() &&
      requestMethod.trimmed().compare(QLatin1String("get"), Qt::CaseInsensitive) == 0) {
    const QString host = requestUrl.host().toLower();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QMutexLocker locker(&httpsMutex_);
    const auto pending = httpsUpgrades_.constFind(host);
    if (pending != httpsUpgrades_.constEnd() && now - pending->second < 10000) {
      httpsUpgrades_.remove(host);
      httpsFallbackHosts_.insert(host);  // the site sent us back to http://
    } else if (!httpsFallbackHosts_.contains(host)) {
      QUrl upgraded = requestUrl;
      upgraded.setScheme(QStringLiteral("https"));
      upgraded.setPort(-1);
      httpsUpgrades_.insert(host, {requestUrl, now});
      decision = RequestDecision{DaliNiraBlockerAction::Redirect, QStringLiteral("https-upgrade-by-default"), 0,
                                 QStringLiteral("dalinira-https-upgrade"), upgraded.toString(QUrl::FullyEncoded)};
    }
  }

  // Strict blocking redirection logic for dangerous/strict-block matches
  if (decision.action == DaliNiraBlockerAction::Block && resType == DaliNiraBlockerResourceType::MainFrame) {
    const bool compiledStrict = decision.rulesetId.startsWith(QStringLiteral("strictblock-"));
    const bool strictMatch = compiledStrict || (settings_->strictBlock() &&
        (decision.rulesetId.contains(QStringLiteral("malware"), Qt::CaseInsensitive) ||
         decision.rulesetId.contains(QStringLiteral("strict"), Qt::CaseInsensitive)));
    if (strictMatch) {
      if (isStrictBypassActive(siteHost)) {
        // A bypass must permit the navigation. Returning Block here made the
        // warning page's "continue" action loop into a cancelled request.
        decision = RequestDecision{DaliNiraBlockerAction::Allow, QStringLiteral("strict-temporary-bypass"),
                                   decision.ruleId, decision.rulesetId, QString()};
      } else {
        decision.action = DaliNiraBlockerAction::Redirect;
        decision.redirectUrl = QStringLiteral("dalinira://newtab?strictblock=1&domain=%1&url=%2")
                                   .arg(QUrl::toPercentEncoding(siteHost), QUrl::toPercentEncoding(requestUrl.toString()));
      }
    }
  }

  // Update real evaluation timer metrics
  const auto end = std::chrono::high_resolution_clock::now();
  const double elapsedMs = std::chrono::duration<double, std::milli>(end - start).count();
  {
    QMutexLocker locker(&timingMutex_);
    evaluationCount_++;
    totalEvaluationTimeMs_ += elapsedMs;
    lastEvaluationTimeMs_ = elapsedMs;
    maxEvaluationTimeMs_ = std::max(maxEvaluationTimeMs_, elapsedMs);
  }

  // Update tab and global statistics
  if (decision.action == DaliNiraBlockerAction::Block) {
    const bool isTracker = decision.rulesetId.contains(QStringLiteral("privacy"), Qt::CaseInsensitive) ||
                           decision.rulesetId.contains(QStringLiteral("tracker"), Qt::CaseInsensitive);
    reportBlockedEvent(resolvedTab, isTracker ? DaliNiraBlockType::Tracker : DaliNiraBlockType::NetworkAd, 1,
                       decision.rulesetId.isEmpty() ? QStringLiteral("default") : decision.rulesetId);
  } else {
    bool statsChanged = false;
    bool persistNeeded = false;
    TabBlockerStats changedStats;
    quint64 changedSessionBlocked = 0;
    quint64 changedTotalBlocked = 0;
    {
      QMutexLocker locker(&statsMutex_);
      TabBlockerStats &tStats = tabStats_[resolvedTab];
      if (decision.action == DaliNiraBlockerAction::Redirect) {
        tStats.redirectedRequests++;
        totalRedirected_++;
        recordRulesetMatch(decision.rulesetId.isEmpty() ? QStringLiteral("default") : decision.rulesetId);
        statsChanged = true;
        changedStats = tStats;
        changedSessionBlocked = sessionBlocked_;
        changedTotalBlocked = totalBlocked_;
        persistNeeded = true;
      } else {
        tStats.allowedRequests++;
        totalAllowed_++;
        if (decision.reason == QLatin1String("site-whitelisted")) {
          whitelistAllowed_++;
          persistNeeded = true;
        }
      }
      if (persistNeeded) statsDirty_ = true;
    }
    if (persistNeeded) scheduleStatsPersistence();
    if (statsChanged) {
      emit tabStatsChanged(resolvedTab, changedStats);
      emit globalStatsChanged(changedSessionBlocked, changedTotalBlocked);
    }
  }

  // Log to ring buffer
  NetworkLogEntry entry;
  entry.timestamp = QDateTime::currentDateTime();
  entry.tabId = resolvedTab;
  entry.siteHost = siteHost.isEmpty() ? initiatorHost : siteHost;
  entry.requestHost = siteHost;
  entry.initiatorHost = initiatorHost;
  entry.topLevelSite = firstPartyUrl.host().toLower();
  entry.requestMethod = requestMethod.trimmed().toUpper();
  entry.resourceTypeStr = resourceTypeToString(resType);
  entry.requestUrl = requestUrl.adjusted(QUrl::RemoveUserInfo | QUrl::RemoveQuery | QUrl::RemoveFragment).toString();
  entry.action = decision.action;
  entry.reason = decision.reason;
  entry.rulesetId = decision.rulesetId;
  entry.ruleId = decision.ruleId;
  logEntry(entry);

  return decision;
}

double DaliNiraBlockerService::averageEvaluationTimeMs() const {
  QMutexLocker locker(&timingMutex_);
  if (evaluationCount_ == 0) return 0.0;
  return totalEvaluationTimeMs_ / evaluationCount_;
}

double DaliNiraBlockerService::lastEvaluationTimeMs() const {
  QMutexLocker locker(&timingMutex_);
  return lastEvaluationTimeMs_;
}

double DaliNiraBlockerService::maxEvaluationTimeMs() const {
  QMutexLocker locker(&timingMutex_);
  return maxEvaluationTimeMs_;
}

quint64 DaliNiraBlockerService::evaluationCount() const {
  QMutexLocker locker(&timingMutex_);
  return evaluationCount_;
}

quint64 DaliNiraBlockerService::estimatedMemoryBytes() const {
  // Calculated estimated memory of adblock data structures
  quint64 bytes = 0;
  bytes += sizeof(DaliNiraBlockerService) + sizeof(DaliNiraBlockerEngine) + sizeof(DaliNiraBlockerListManager);
  bytes += filterEngine_->ruleCount() * 128ULL; // Approx bytes per FilterRule + parsed strings
  bytes += filterEngine_->customRuleCount() * 96ULL;
  bytes += logsRingBuffer_.size() * 256ULL; // NetworkLogEntry and URL strings
  bytes += tabStats_.size() * sizeof(TabBlockerStats);
  bytes += blockedByHost_.size() * 48ULL;
  bytes += matchedByRuleset_.size() * 48ULL;
  return bytes;
}

void DaliNiraBlockerService::recordHostBlock(const QString &host) {
  if (host.isEmpty()) return;
  blockedByHost_[host]++;
}

void DaliNiraBlockerService::recordRulesetMatch(const QString &rulesetId) {
  if (rulesetId.isEmpty()) return;
  matchedByRuleset_[rulesetId]++;
}

void DaliNiraBlockerService::loadPersistentStats() {
  QSettings persisted(dataDir_ + QStringLiteral("/adblock-statistics.ini"), QSettings::IniFormat);
  QMutexLocker locker(&statsMutex_);
  totalBlocked_ = persisted.value(QStringLiteral("statistics/totalBlocked"), 0).toULongLong();
  totalAllowed_ = persisted.value(QStringLiteral("statistics/totalAllowed"), 0).toULongLong();
  totalRedirected_ = persisted.value(QStringLiteral("statistics/totalRedirected"), 0).toULongLong();
  totalTrackersBlocked_ = persisted.value(QStringLiteral("statistics/trackersBlocked"), 0).toULongLong();
  whitelistAllowed_ = persisted.value(QStringLiteral("statistics/whitelistAllowed"), 0).toULongLong();
  const int size = persisted.beginReadArray(QStringLiteral("daily"));
  for (int index = 0; index < size; ++index) {
    persisted.setArrayIndex(index);
    const QString day = persisted.value(QStringLiteral("date")).toString();
    const quint64 count = persisted.value(QStringLiteral("count"), 0).toULongLong();
    if (QDate::fromString(day, Qt::ISODate).isValid() && count > 0) dailyBlocked_.insert(day, count);
  }
  persisted.endArray();
  while (dailyBlocked_.size() > 120) dailyBlocked_.erase(dailyBlocked_.begin());
}

void DaliNiraBlockerService::scheduleStatsPersistence() {
  QMetaObject::invokeMethod(this, [this]() {
    if (!statsPersistTimer_.isActive()) statsPersistTimer_.start();
  }, Qt::QueuedConnection);
}

void DaliNiraBlockerService::persistStats() {
  quint64 blocked = 0;
  quint64 allowed = 0;
  quint64 redirected = 0;
  quint64 trackers = 0;
  quint64 whitelist = 0;
  QMap<QString, quint64> daily;
  {
    QMutexLocker locker(&statsMutex_);
    blocked = totalBlocked_;
    allowed = totalAllowed_;
    redirected = totalRedirected_;
    trackers = totalTrackersBlocked_;
    whitelist = whitelistAllowed_;
    daily = dailyBlocked_;
    statsDirty_ = false;
  }
  QSettings persisted(dataDir_ + QStringLiteral("/adblock-statistics.ini"), QSettings::IniFormat);
  persisted.setValue(QStringLiteral("statistics/totalBlocked"), blocked);
  persisted.setValue(QStringLiteral("statistics/totalAllowed"), allowed);
  persisted.setValue(QStringLiteral("statistics/totalRedirected"), redirected);
  persisted.setValue(QStringLiteral("statistics/trackersBlocked"), trackers);
  persisted.setValue(QStringLiteral("statistics/whitelistAllowed"), whitelist);
  persisted.remove(QStringLiteral("daily"));
  persisted.beginWriteArray(QStringLiteral("daily"), daily.size());
  int index = 0;
  for (auto it = daily.cbegin(); it != daily.cend(); ++it, ++index) {
    persisted.setArrayIndex(index);
    persisted.setValue(QStringLiteral("date"), it.key());
    persisted.setValue(QStringLiteral("count"), it.value());
  }
  persisted.endArray();
  persisted.sync();
  QFile::setPermissions(dataDir_ + QStringLiteral("/adblock-statistics.ini"), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  if (persisted.status() != QSettings::NoError) statsDirty_ = true;
}

void DaliNiraBlockerService::reportBlockedEvent(quint64 tabId, DaliNiraBlockType type, quint64 count,
                                             const QString &ruleOrUrl) {
  if (count == 0 || !settings_->protectionEnabled()) return;

  QString tabHost;
  {
    QMutexLocker locker(&tabRegistryMutex_);
    if (tabId != 0 && tabRegistry_.contains(tabId)) {
      tabHost = tabRegistry_.value(tabId).currentHost;
    } else if (activeTabId_ != 0 && tabRegistry_.contains(activeTabId_)) {
      tabHost = tabRegistry_.value(activeTabId_).currentHost;
      tabId = activeTabId_;
    }
  }

  const QString normHost = DaliNiraBlockerSettings::normalizeSiteHost(tabHost);
  if (!normHost.isEmpty()) {
    const SitePolicy policy = settings_->sitePolicy(normHost);
    if (policy.whitelisted) return;
    if (policy.temporaryDisabledUntil > QDateTime::currentMSecsSinceEpoch()) return;
    if ((type == DaliNiraBlockType::NetworkAd || type == DaliNiraBlockType::Cosmetic) && !policy.adBlocking) return;
    if (type == DaliNiraBlockType::Tracker && !policy.trackerProtection) return;
  }

  bool statsChanged = false;
  TabBlockerStats changedStats;
  quint64 changedSessionBlocked = 0;
  quint64 changedTotalBlocked = 0;

  {
    QMutexLocker locker(&statsMutex_);
    TabBlockerStats &tStats = tabStats_[tabId];
    const QString todayKey = QDate::currentDate().toString(Qt::ISODate);

    switch (type) {
      case DaliNiraBlockType::NetworkAd:
        tStats.blockedRequests += count;
        tStats.blockedAds += count;
        break;
      case DaliNiraBlockType::Tracker:
        tStats.blockedRequests += count;
        tStats.blockedTrackers += count;
        totalTrackersBlocked_ += count;
        break;
      case DaliNiraBlockType::Cosmetic:
        tStats.blockedCosmetics += count;
        tStats.blockedAds += count;
        break;
      case DaliNiraBlockType::Scriptlet:
        tStats.blockedScriptlets += count;
        tStats.blockedAds += count;
        break;
    }

    sessionBlocked_ += count;
    totalBlocked_ += count;
    dailyBlocked_[todayKey] += count;

    if (!normHost.isEmpty()) {
      for (quint64 i = 0; i < count; ++i) recordHostBlock(normHost);
    }
    if (!ruleOrUrl.isEmpty()) {
      for (quint64 i = 0; i < count; ++i) recordRulesetMatch(ruleOrUrl);
    }

    statsChanged = true;
    changedStats = tStats;
    changedSessionBlocked = sessionBlocked_;
    changedTotalBlocked = totalBlocked_;
    statsDirty_ = true;
  }

  scheduleStatsPersistence();

  if (statsChanged) {
    emit tabStatsChanged(tabId, changedStats);
    emit globalStatsChanged(changedSessionBlocked, changedTotalBlocked);
  }
}

TabBlockerStats DaliNiraBlockerService::statsForTab(quint64 tabId) const {
  QMutexLocker locker(&statsMutex_);
  return tabStats_.value(tabId, TabBlockerStats{});
}

void DaliNiraBlockerService::clearTabStats(quint64 tabId) {
  {
    QMutexLocker locker(&statsMutex_);
    tabStats_.remove(tabId);
  }
  emit tabStatsChanged(tabId, TabBlockerStats{});
}

void DaliNiraBlockerService::resetAllStats() {
  {
    QMutexLocker locker(&statsMutex_);
    tabStats_.clear();
    sessionBlocked_ = 0;
    totalBlocked_ = 0;
    totalAllowed_ = 0;
    totalRedirected_ = 0;
    totalTrackersBlocked_ = 0;
    whitelistAllowed_ = 0;
    blockedByHost_.clear();
    matchedByRuleset_.clear();
    dailyBlocked_.clear();
    statsDirty_ = true;
  }
  persistStats();
  {
    QMutexLocker locker(&timingMutex_);
    evaluationCount_ = 0;
    totalEvaluationTimeMs_ = 0.0;
    lastEvaluationTimeMs_ = 0.0;
    maxEvaluationTimeMs_ = 0.0;
  }
  emit globalStatsChanged(0, 0);
}

quint64 DaliNiraBlockerService::sessionBlockedCount() const {
  QMutexLocker locker(&statsMutex_);
  return sessionBlocked_;
}

quint64 DaliNiraBlockerService::totalBlockedCount() const {
  QMutexLocker locker(&statsMutex_);
  return totalBlocked_;
}

quint64 DaliNiraBlockerService::totalAllowedCount() const {
  QMutexLocker locker(&statsMutex_);
  return totalAllowed_;
}

quint64 DaliNiraBlockerService::totalRedirectedCount() const {
  QMutexLocker locker(&statsMutex_);
  return totalRedirected_;
}

quint64 DaliNiraBlockerService::totalTrackersBlockedCount() const {
  QMutexLocker locker(&statsMutex_);
  return totalTrackersBlocked_;
}

quint64 DaliNiraBlockerService::whitelistAllowedCount() const {
  QMutexLocker locker(&statsMutex_);
  return whitelistAllowed_;
}

quint64 DaliNiraBlockerService::estimatedBytesSaved() const {
  QMutexLocker locker(&statsMutex_);
  // Heuristic estimation: ~35 KB per blocked ad/tracker request
  return totalBlocked_ * 35840ULL;
}

quint64 DaliNiraBlockerService::todayBlockedCount() const {
  QMutexLocker locker(&statsMutex_);
  const QString todayKey = QDate::currentDate().toString(Qt::ISODate);
  return dailyBlocked_.value(todayKey, 0);
}

quint64 DaliNiraBlockerService::weekBlockedCount() const {
  QMutexLocker locker(&statsMutex_);
  const QDate today = QDate::currentDate();
  quint64 sum = 0;
  for (int i = 0; i < 7; ++i) {
    sum += dailyBlocked_.value(today.addDays(-i).toString(Qt::ISODate), 0);
  }
  return sum;
}

quint64 DaliNiraBlockerService::monthBlockedCount() const {
  QMutexLocker locker(&statsMutex_);
  const QDate today = QDate::currentDate();
  quint64 sum = 0;
  for (int i = 0; i < 30; ++i) {
    sum += dailyBlocked_.value(today.addDays(-i).toString(Qt::ISODate), 0);
  }
  return sum;
}

QList<QPair<QString, quint64>> DaliNiraBlockerService::topBlockedHosts(int limit) const {
  QMutexLocker locker(&statsMutex_);
  QList<QPair<QString, quint64>> list;
  for (auto it = blockedByHost_.constBegin(); it != blockedByHost_.constEnd(); ++it) {
    list.append(qMakePair(it.key(), it.value()));
  }
  std::sort(list.begin(), list.end(), [](const auto &a, const auto &b) {
    return a.second > b.second;
  });
  if (limit > 0 && list.size() > limit) list = list.mid(0, limit);
  return list;
}

QList<QPair<QString, quint64>> DaliNiraBlockerService::topMatchedRulesets(int limit) const {
  QMutexLocker locker(&statsMutex_);
  QList<QPair<QString, quint64>> list;
  for (auto it = matchedByRuleset_.constBegin(); it != matchedByRuleset_.constEnd(); ++it) {
    list.append(qMakePair(it.key(), it.value()));
  }
  std::sort(list.begin(), list.end(), [](const auto &a, const auto &b) {
    return a.second > b.second;
  });
  if (limit > 0 && list.size() > limit) list = list.mid(0, limit);
  return list;
}

bool DaliNiraBlockerService::genericCosmeticsDisabled(const QString &host, const QUrl &pageUrl) const {
  const QUrl document = pageUrl.isValid() && !pageUrl.host().isEmpty()
                            ? pageUrl
                            : QUrl(QStringLiteral("https://") + host.trimmed().toLower() + QLatin1Char('/'));
  return filterEngine_->genericCosmeticsDisabled(document);
}

RequestDecision DaliNiraBlockerService::decidePopup(const QUrl &targetUrl, const QUrl &openerUrl, quint64 tabId) {
  const QString openerScheme = openerUrl.scheme().toLower();
  // Windows the browser opens itself (New Tab, internal pages) are not popups.
  if (!settings_->protectionEnabled() ||
      (openerScheme != QLatin1String("http") && openerScheme != QLatin1String("https")))
    return RequestDecision{DaliNiraBlockerAction::Allow, QStringLiteral("not-filtered"), 0, QString(), QString()};
  const QString openerHost = openerUrl.host().toLower();
  const RequestDecision decision = filterEngine_->evaluatePopup(targetUrl, openerHost, settings_->sitePolicy(openerHost));
  if (decision.action == DaliNiraBlockerAction::Block)
    reportBlockedEvent(tabId, DaliNiraBlockType::NetworkAd, 1, decision.rulesetId);

  NetworkLogEntry entry;
  entry.timestamp = QDateTime::currentDateTime();
  entry.tabId = tabId;
  entry.siteHost = targetUrl.host().toLower();
  entry.requestHost = entry.siteHost;
  entry.initiatorHost = openerHost;
  entry.topLevelSite = openerHost;
  entry.requestMethod = QStringLiteral("GET");
  entry.resourceTypeStr = QStringLiteral("popup");
  entry.requestUrl = targetUrl.adjusted(QUrl::RemoveUserInfo | QUrl::RemoveQuery | QUrl::RemoveFragment).toString();
  entry.action = decision.action;
  entry.reason = decision.reason;
  entry.rulesetId = decision.rulesetId;
  entry.ruleId = decision.ruleId;
  logEntry(entry);
  return decision;
}

QString DaliNiraBlockerService::cosmeticCssForHost(const QString &host, const QUrl &pageUrl) const {
  if (!settings_->protectionEnabled()) return {};
  QString css = filterEngine_->cosmeticCssForHost(host, !genericCosmeticsDisabled(host, pageUrl));
  if (listManager_) {
    const QString specificCss = listManager_->loadSpecificCosmeticCssForHost(
        host, activeRulesetIds(), true);
    if (!specificCss.isEmpty()) css += (css.isEmpty() ? QString() : QStringLiteral("\n")) + specificCss;
  }
  return filterEngine_->applyCosmeticExceptions(host, css);
}

QWebEngineScript DaliNiraBlockerService::createCosmeticScriptForHost(const QString &host, const QUrl &pageUrl) const {
  QWebEngineScript script;
  script.setName(QStringLiteral("dalinira-adblock-cosmetic"));
  script.setInjectionPoint(QWebEngineScript::DocumentCreation);
  script.setWorldId(QWebEngineScript::MainWorld);
  script.setRunsOnSubFrames(false);

  if (!settings_->protectionEnabled()) return script;

  const QString lowerHost = host.trimmed().toLower();
  const SitePolicy policy = settings_->sitePolicy(lowerHost);
  // Network allowlisting in the old webview preload also suppresses cosmetic
  // and scriptlet injection. Keeping this check at script construction makes
  // the Qt layer obey the same all-layer whitelist contract.
  if (policy.whitelisted || !policy.adBlocking ||
      policy.temporaryDisabledUntil > QDateTime::currentMSecsSinceEpoch()) {
    return script;
  }
  const QString css = cosmeticCssForHost(host, pageUrl);

  // CSS automatically re-evaluates recycled nodes, including attribute/text
  // replacement; no sticky per-node hidden flag or destructive inline style.
  QString combined = css;
  const QString json = QString::fromUtf8(QJsonDocument(QJsonArray{combined}).toJson(QJsonDocument::Compact));
  script.setWorldId(QWebEngineScript::ApplicationWorld);
  const QString expectedHost = QString::fromUtf8(QJsonDocument(QJsonArray{lowerHost}).toJson(QJsonDocument::Compact));
  script.setSourceCode(QStringLiteral("(()=>{const start=()=>{const h=location.hostname.toLowerCase();const exp=%1[0];if(h===exp||h.replace(/^www\\./,'')===exp.replace(/^www\\./,'')||h.endsWith('.'+exp.replace(/^www\\./,''))){\n").arg(expectedHost) +
      QString::fromUtf8(kDaliNiraCosmeticRuntime).arg(json + QStringLiteral("[0]")) +
      QStringLiteral("\n}};if(document.readyState==='loading')document.addEventListener('DOMContentLoaded',start,{once:true});start();})();"));

  return script;
}

QString DaliNiraBlockerService::privacyShieldScript() const {
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  QJsonArray exempt;
  const QHash<QString, SitePolicy> policies = settings_->sitePolicies();
  for (auto it = policies.cbegin(); it != policies.cend(); ++it)
    if (it.value().whitelisted || it.value().temporaryDisabledUntil > now)
      exempt.append(QString::fromLatin1(QUrl::toAce(it.key().trimmed().toLower())));
  const bool farble = settings_->protectionEnabled() && settings_->fingerprintProtection();
  return QString::fromUtf8(kDaliNiraPrivacyShield)
      .replace(QStringLiteral("__DN_KEY__"), sessionKey_)
      .replace(QStringLiteral("__DN_EXEMPT__"), QString::fromUtf8(QJsonDocument(exempt).toJson(QJsonDocument::Compact)))
      .replace(QStringLiteral("__DN_FARBLE__"), farble ? QStringLiteral("true") : QStringLiteral("false"));
}

QStringList DaliNiraBlockerService::cosmeticDocumentScriptsForHost(const QString &host, const QUrl &pageUrl) const {
  QStringList sources;
  for (const QWebEngineScript &script : createScriptingScriptsForHost(host, pageUrl)) {
    if (script.name() == QLatin1String("dalinira-adblock-cosmetic") ||
        script.name() == QLatin1String("dalinira-adblock-procedural"))
      sources.append(script.sourceCode());
  }
  return sources;
}

QList<QWebEngineScript> DaliNiraBlockerService::createScriptingScriptsForHost(const QString &host, const QUrl &pageUrl) const {
  QList<QWebEngineScript> scripts;
  if (!settings_->protectionEnabled()) return scripts;
  const bool includeGeneric = !genericCosmeticsDisabled(host, pageUrl);
  const QWebEngineScript cosmetic = createCosmeticScriptForHost(host, pageUrl);
  if (!cosmetic.sourceCode().isEmpty()) scripts.append(cosmetic);

  const SitePolicy policy = settings_->sitePolicy(host);
  if (!policy.whitelisted && policy.temporaryDisabledUntil <= QDateTime::currentMSecsSinceEpoch()) {
    if (policy.blockFingerprinting) {
      QWebEngineScript privacy;
      privacy.setName(QStringLiteral("dalinira-fingerprint-protection"));
      privacy.setInjectionPoint(QWebEngineScript::DocumentCreation);
      privacy.setWorldId(QWebEngineScript::MainWorld);
      privacy.setRunsOnSubFrames(true);
      privacy.setSourceCode(QStringLiteral(R"JS((()=>{
        const deny=()=>{throw new DOMException('Canvas readback blocked by site privacy policy','SecurityError')};
        for(const [prototype,names] of [[HTMLCanvasElement.prototype,['toDataURL','toBlob']],[CanvasRenderingContext2D.prototype,['getImageData']]]){
          for(const name of names)try{Object.defineProperty(prototype,name,{value:deny,writable:false,configurable:false})}catch(_){}
        }
        for(const prototype of [window.WebGLRenderingContext?.prototype,window.WebGL2RenderingContext?.prototype]){
          if(!prototype)continue;const original=prototype.getParameter;
          try{Object.defineProperty(prototype,'getParameter',{value:function(p){if(p===37445||p===37446)return 'DaliNira';return original.call(this,p)},writable:false,configurable:false})}catch(_){}
        }
      })())JS"));
      scripts.append(privacy);
    }

  }
  if (policy.whitelisted || !policy.adBlocking ||
      policy.temporaryDisabledUntil > QDateTime::currentMSecsSinceEpoch()) {
    return scripts;
  }

  for (const auto &asset : listManager_->loadScriptingSourcesForHost(
      host, activeRulesetIds(), true, includeGeneric)) {
    QWebEngineScript script;
    const bool mainWorld = asset.first == QLatin1String("main");
    script.setName(mainWorld ? QStringLiteral("dalinira-adblock-scriptlets-main")
                             : QStringLiteral("dalinira-adblock-scriptlets-isolated"));
    script.setInjectionPoint(QWebEngineScript::DocumentCreation);
    script.setWorldId(mainWorld ? QWebEngineScript::MainWorld : QWebEngineScript::ApplicationWorld);
    script.setRunsOnSubFrames(false);
    script.setSourceCode(asset.second);
    scripts.append(script);
  }

  const QString lowerHost = host.trimmed().toLower();
  const bool isYouTubeHost = lowerHost == QLatin1String("youtube.com") ||
                             lowerHost == QLatin1String("www.youtube.com") ||
                             lowerHost == QLatin1String("m.youtube.com") ||
                             lowerHost == QLatin1String("music.youtube.com") ||
                             lowerHost.endsWith(QLatin1String(".youtube.com")) ||
                             lowerHost == QLatin1String("youtu.be");

  if (!isYouTubeHost) {
    QJsonArray proceduralRules = listManager_->loadProceduralRulesForHost(host, activeRulesetIds(), true);
    const QJsonArray customProcedural = filterEngine_->customProceduralRulesForHost(host, includeGeneric);
    for (const QJsonValue &rule : customProcedural) proceduralRules.append(rule);
    if (!proceduralRules.isEmpty()) {
      const QString json = QString::fromUtf8(QJsonDocument(proceduralRules).toJson(QJsonDocument::Compact));
      QWebEngineScript procedural;
      procedural.setName(QStringLiteral("dalinira-adblock-procedural"));
      procedural.setInjectionPoint(QWebEngineScript::DocumentCreation);
      procedural.setWorldId(QWebEngineScript::ApplicationWorld);
      procedural.setRunsOnSubFrames(false);
      // This is the procedural subset used by the legacy webview preload:
      // selector/tasks/action plus re-evaluation on SPA DOM mutation. Unknown
      // task names fail closed (do not hide content), never as broad selectors.
      procedural.setSourceCode(QStringLiteral(R"JS(
(function(){
 const runtime=window.__daliniraCosmeticRuntime;
 if(!runtime||window.__daliniraProceduralRules)return;window.__daliniraProceduralRules=1;
 const rules=%1;
 const rx=v=>{const s=String(v||'');const m=/^\/([\s\S]*)\/([a-z]*)$/i.exec(s);if(s.length>512||(m&&/[(){}]/.test(m[1])))return /$a/;try{return m?new RegExp(m[1],m[2]):new RegExp(s.replace(/[.*+?^${}()|[\]\\]/g,'\\$&'),'i')}catch(_){return /^$/}};
 const query=(root,sel)=>{try{if(!sel)return root&&root.nodeType===1?[root]:[];if(sel==='*')return [];const list=Array.from((root||document).querySelectorAll(sel)).slice(0,512);if(root?.nodeType===1&&root.matches(sel))list.unshift(root);return list}catch(_){return[]}};
 const relative=(node,sel)=>{try{if(/^[>+~]/.test(sel))return Array.from(node.parentElement.querySelectorAll(':scope '+sel));return query(node,sel)}catch(_){return[]}};
 const filter=(nodes,tasks)=>{let out=nodes.filter(Boolean);for(const task of(tasks||[])){const n=String(task[0]||''),a=task[1],next=[];for(const node of out){try{
  if(n==='has-text'){if(rx(a).test(String(node.textContent||'').slice(0,8192)))next.push(node)}
  else if(n==='matches-path'){if(rx(a).test(location.pathname+location.search))next.push(node)}
  else if(n==='matches-media'){if(matchMedia(String(a||'')).matches)next.push(node)}
  else if(n==='matches-css'||n==='matches-css-before'||n==='matches-css-after'){const pseudo=n.endsWith('before')?'::before':n.endsWith('after')?'::after':null;const st=getComputedStyle(node,pseudo);if(rx(a&&a.value).test(st.getPropertyValue(a&&a.name)))next.push(node)}
  else if(n==='matches-attr'){const nr=rx(a&&a.attr),vr=rx(a&&a.value);if(Array.from(node.attributes||[]).some(x=>nr.test(x.name)&&vr.test(x.value)))next.push(node)}
  else if(n==='matches-prop'){try{let v=node;for(const p of String(a&&a.attr||'').split('.'))v=v==null?undefined:v[p];if(rx(a&&a.value).test(String(v??'')))next.push(node)}catch(_){}}
  else if(n==='min-text-length'){if(String(node.textContent||'').slice(0,8192).length>=Number(a||0))next.push(node)}
  else if(n==='has'||n==='if'){const o=a&&typeof a==='object'?a:{selector:String(a||'*'),tasks:[]};if(filter(relative(node,o.selector||'*'),o.tasks).length)next.push(node)}
  else if(n==='not'||n==='if-not'){const o=a&&typeof a==='object'?a:{selector:String(a||''),tasks:[]};if(!filter(o.selector?relative(node,o.selector):[node],o.tasks).length)next.push(node)}
  else if(n==='upward'){let x=node;if(typeof a==='number'){for(let i=0;i<a&&x;i++)x=x.parentElement}else x=node.closest(String(a||''));if(x)next.push(x)}
  else if(n==='spath'){const s=String(a||'');if(!s)next.push(node);else if(s.startsWith(':')){if(node.matches(s))next.push(node)}else next.push(...relative(node,s))}
  else if(n==='xpath'){try{const it=document.evaluate(String(a||''),node,null,XPathResult.ORDERED_NODE_ITERATOR_TYPE,null);let x;while((x=it.iterateNext()))if(x.nodeType===1)next.push(x)}catch(_){}}
  else if(n==='shadow'){next.push(...query(node.shadowRoot,String(a||'*')))}
  else if(n==='others'){if(node.parentElement)next.push(...Array.from(node.parentElement.children).filter(x=>x!==node))}
  else if(n==='watch-attr')next.push(node)
 }catch(_){}}out=[...new Set(next)];if(!out.length)break}return out};
  const apply=(node,rule)=>{try{
   if(runtime.hiddenNodes&&runtime.hiddenNodes.has(node))return;
   if(runtime.hiddenNodes)runtime.hiddenNodes.add(node);
   if(runtime.reportBlock)runtime.reportBlock(1,'procedural');
   const a=Array.isArray(rule.action)?rule.action:['style','display:none!important;'];
   if(a[0]==='remove')node.remove();
   else if(a[0]==='remove-attr')node.removeAttribute(String(a[1]||''));
   else if(a[0]==='remove-class')node.classList.remove(...String(a[1]||'').split(/\s+/));
   else if(a[0]==='style')node.style.cssText=String(a[1]||'display:none!important;');
   else node.style.setProperty('display','none','important');
  }catch(_){}};
 let cursor=0;
 runtime.callbacks.push((roots)=>{
  const deadline=performance.now()+12;let count=0;
  while(count<rules.length){
   const r=rules[cursor];cursor=(cursor+1)%rules.length;count++;
   const candidates=new Set();
   for(const root of roots){for(const node of query(root===document?document:(root?.parentElement||root),r.selector||''))candidates.add(node);if(candidates.size>=512)break;}
   for(const n of filter(Array.from(candidates).slice(0,512),r.tasks).slice(0,512))apply(n,r);
   // Resume at the next rule on a later DOM event, never a timer loop.
   if(performance.now()>deadline)break;
  }
 });
 runtime.schedule(document);
})();
)JS").arg(json));
      scripts.append(procedural);
    }
  }
  return scripts;
}

void DaliNiraBlockerService::logEntry(const NetworkLogEntry &entry) {
  {
    QMutexLocker locker(&logMutex_);
    logsRingBuffer_.prepend(entry);
    if (logsRingBuffer_.size() > kMaxRingBufferLogs) logsRingBuffer_.removeLast();
  }
  emit requestLogged(entry);
}

QList<NetworkLogEntry> DaliNiraBlockerService::recentLogs(int maxCount) const {
  QMutexLocker locker(&logMutex_);
  if (maxCount <= 0 || maxCount >= logsRingBuffer_.size()) return logsRingBuffer_;
  return logsRingBuffer_.mid(0, maxCount);
}

bool DaliNiraBlockerService::shouldForgetClosedHost(const QString &host) const {
  const QString key = DaliNiraBlockerSettings::normalizeSiteHost(host);
  const auto policy = settings_->sitePolicy(key);
  if (key.isEmpty() || !settings_->protectionEnabled() || policy.whitelisted || !policy.forgetOnClose) return false;
  QMutexLocker locker(&tabRegistryMutex_);
  for (const auto &context : tabRegistry_)
    if (DaliNiraBlockerSettings::normalizeSiteHost(context.currentHost) == key) return false;
  return true;
}
