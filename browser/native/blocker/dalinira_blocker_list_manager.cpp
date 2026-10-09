#include "dalinira_blocker_list_manager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QRegularExpression>
#include "dalinira_blocker_engine.h"
#include "i18n/i18n.h"

DaliNiraBlockerListManager::DaliNiraBlockerListManager(const QString &dataDir, QObject *parent)
    : QObject(parent), dataDir_(dataDir) {
  initRulesetCatalog();
  const QString baseDir = rulesetDir();
  redirectResources_ = std::make_unique<DaliNiraBlockerRedirectResources>(
      baseDir.isEmpty() ? QString() : QDir(baseDir).absoluteFilePath(QStringLiteral("../web_accessible_resources")));
  if (!dataDir_.isEmpty() && !baseDir.isEmpty()) {
    QSet<QString> eligible;
    for (const FilterListInfo &info : std::as_const(lists_))
      if (!info.attributionReviewRequired) eligible.insert(info.id);
    filterStore_ = std::make_unique<DaliNiraBlockerFilterStore>(
        dataDir_ + QStringLiteral("/adblock-filter-updates"),
        baseDir + QStringLiteral("/filter-update-sources.json"), eligible);
  }
}

DaliNiraBlockerListManager::~DaliNiraBlockerListManager() = default;

QString DaliNiraBlockerListManager::rulesetDir() const {
  const QStringList candidates = {
      dataDir_ + QStringLiteral("/adblock/rulesets"),
      dataDir_ + QStringLiteral("/rulesets"),
      QCoreApplication::applicationDirPath() + QStringLiteral("/resources/adblock/rulesets"),
      QCoreApplication::applicationDirPath() + QStringLiteral("/../resources/adblock/rulesets"),
      QCoreApplication::applicationDirPath() + QStringLiteral("/../share/dalinira-browser/resources/adblock/rulesets"),
      QCoreApplication::applicationDirPath() + QStringLiteral("/../../share/dalinira-browser/resources/adblock/rulesets"),
      QStringLiteral("/usr/share/dalinira-browser/resources/adblock/rulesets"),
      QStringLiteral("/usr/local/share/dalinira-browser/resources/adblock/rulesets"),
      QCoreApplication::applicationDirPath() + QStringLiteral("/../browser/resources/adblock/rulesets"),
      QDir::currentPath() + QStringLiteral("/browser/resources/adblock/rulesets"),
      QDir::currentPath() + QStringLiteral("/build/resources/adblock/rulesets")
  };
  for (const QString &path : candidates) {
    if (QDir(path).exists()) return path;
  }
  return QString();
}

QString DaliNiraBlockerListManager::findRulesetFilePath(const QString &fileName) const {
  const QString baseDir = rulesetDir();
  if (baseDir.isEmpty()) return QString();

  const QStringList subPaths = {
      baseDir + QStringLiteral("/main/") + fileName,
      baseDir + QStringLiteral("/") + fileName
  };
  for (const QString &p : subPaths) {
    if (QFileInfo::exists(p)) return p;
  }
  return QString();
}

void DaliNiraBlockerListManager::initRulesetCatalog() {
  QMutexLocker locker(&mutex_);
  lists_.clear();

  const QString baseDir = rulesetDir();
  const QString detailsPath = baseDir + QStringLiteral("/ruleset-details.json");
  QJsonObject attributionRulesets;
  QString defaultFormatOrigin;

  QFile attributionFile(baseDir + QStringLiteral("/ruleset-attributions.json"));
  if (attributionFile.open(QIODevice::ReadOnly)) {
    const QJsonDocument attributionDoc =
        QJsonDocument::fromJson(attributionFile.read(4 * 1024 * 1024 + 1));
    if (attributionDoc.isObject()) {
      const QJsonObject attributionRoot = attributionDoc.object();
      attributionRulesets = attributionRoot.value(QStringLiteral("rulesets")).toObject();
      defaultFormatOrigin = attributionRoot.value(QStringLiteral("generatedFormat"))
                                .toObject().value(QStringLiteral("name")).toString();
    }
  }

  if (QFileInfo::exists(detailsPath)) {
    const QFileInfo detailsInfo(detailsPath);
    const QDateTime packageTimestamp = detailsInfo.lastModified();
    QFile file(detailsPath);
    if (file.open(QIODevice::ReadOnly)) {
      const QJsonDocument doc = QJsonDocument::fromJson(file.read(32 * 1024 * 1024 + 1));
      if (doc.isArray()) {
        for (const auto &val : doc.array()) {
          if (!val.isObject()) continue;
          const QJsonObject obj = val.toObject();
          FilterListInfo info;
          info.id = obj.value(QStringLiteral("id")).toString();
          info.name = obj.value(QStringLiteral("name")).toString(info.id);
          info.group = obj.value(QStringLiteral("group")).toString(QStringLiteral("default"));
          info.enabled = obj.value(QStringLiteral("enabled")).toBool(true);
          info.downloadUrl = obj.value(QStringLiteral("homeURL")).toString();
          const QJsonObject attribution = attributionRulesets.value(info.id).toObject();
          info.provider = attribution.value(QStringLiteral("provider")).toString();
          info.projectUrl = attribution.value(QStringLiteral("projectUrl")).toString(info.downloadUrl);
          info.source = attribution.value(QStringLiteral("source")).toString();
          info.sourceUrl = attribution.value(QStringLiteral("sourceUrl")).toString(info.projectUrl);
          info.license = attribution.value(QStringLiteral("license")).toString();
          info.licenseUrl = attribution.value(QStringLiteral("licenseUrl")).toString();
          info.formatOrigin = attribution.value(QStringLiteral("formatOrigin")).toString(defaultFormatOrigin);
          info.attributionReviewRequired =
              attribution.value(QStringLiteral("reviewRequired")).toBool(false);
          info.version = packageTimestamp.isValid()
              ? QStringLiteral("bundled-%1").arg(packageTimestamp.date().toString(QStringLiteral("yyyyMMdd")))
              : QStringLiteral("bundled");
          info.localFilePath = info.id + QStringLiteral(".json");
          info.lastUpdated = packageTimestamp;

          const QJsonObject rulesObj = obj.value(QStringLiteral("rules")).toObject();
          info.ruleCount = rulesObj.value(QStringLiteral("total")).toInt(1000);

          lists_.append(info);
        }
      }
    }
  }

  if (lists_.isEmpty()) {
    // Fallback standard catalog
    auto addFallback = [this](const QString &id, const QString &name, const QString &group,
                              const QString &description, bool enabled, int ruleCount) {
      FilterListInfo info;
      info.id = id;
      info.name = name;
      info.group = group;
      info.description = description;
      info.enabled = enabled;
      info.ruleCount = ruleCount;
      info.lastUpdated = QDateTime::currentDateTime();
      info.localFilePath = id + QStringLiteral(".json");
      info.version = QStringLiteral("bundled");
      lists_.append(info);
    };
    addFallback(QStringLiteral("default-0"), I18n::text(QStringLiteral("blocker.list_default_name")),
                QStringLiteral("default"), I18n::text(QStringLiteral("blocker.list_default_desc")), true, 24500);
    addFallback(QStringLiteral("easyprivacy"), I18n::text(QStringLiteral("blocker.list_easyprivacy_name")),
                QStringLiteral("privacy"), I18n::text(QStringLiteral("blocker.list_easyprivacy_desc")), true, 18200);
    addFallback(QStringLiteral("tur-0"), I18n::text(QStringLiteral("blocker.list_turkish_name")),
                QStringLiteral("regional"), I18n::text(QStringLiteral("blocker.list_turkish_desc")), true, 4100);
    addFallback(QStringLiteral("annoyances-cookies"), I18n::text(QStringLiteral("blocker.list_cookie_notices_name")),
                QStringLiteral("annoyances"), I18n::text(QStringLiteral("blocker.list_cookie_notices_desc")), false, 9500);
    addFallback(QStringLiteral("annoyances-social"), I18n::text(QStringLiteral("blocker.list_social_name")),
                QStringLiteral("social"), I18n::text(QStringLiteral("blocker.list_social_desc")), false, 3200);
  }
}

QList<FilterListInfo> DaliNiraBlockerListManager::availableLists() const {
  QList<FilterListInfo> lists;
  {
    QMutexLocker locker(&mutex_);
    lists = lists_;
  }
  if (!filterStore_) return lists;
  for (FilterListInfo &info : lists) {
    const auto source = filterStore_->source(info.id);
    if (!source) continue;
    const FilterListUpdateState state = filterStore_->state(info.id);
    info.updateUrl = source->url.toString();
    info.activeSource = filterStore_->activeSource(info.id);
    info.lastUpdateCheck = state.lastCheck.toLocalTime();
    info.lastUpdateResult = state.lastResult;
    info.lastUpdateFailure = filterUpdateFailureName(state.lastFailure);
    const FilterListCopy &copy = info.activeSource == QLatin1String("remote") ? state.current : state.previous;
    if (info.activeSource != QLatin1String("bundled") && copy.isValid()) {
      info.lastUpdated = copy.activatedAt.toLocalTime();
      info.version = QStringLiteral("remote-") + info.lastUpdated.toString(QStringLiteral("yyyyMMdd"));
      info.ruleCount = copy.networkRules;
    }
  }
  return lists;
}

QStringList DaliNiraBlockerListManager::resolveRulesetIds(DaliNiraBlockerMode mode, const QStringList &enabledIds,
                                                         bool selectionConfigured) const {
  if (selectionConfigured) return enabledIds;
  QList<FilterListInfo> lists;
  {
    QMutexLocker locker(&mutex_);
    lists = lists_;
  }
  const QString baseDir = rulesetDir();
  QStringList resolved;
  for (const auto &info : lists) {
    const bool defaultId = info.enabled &&
        QFileInfo::exists(baseDir + QStringLiteral("/main/") + info.id + QStringLiteral(".json"));
    bool include = false;
    if (mode == DaliNiraBlockerMode::Basic)
      include = defaultId && info.group == QLatin1String("default");
    else if (mode == DaliNiraBlockerMode::Ideal)
      include = defaultId || info.id == QLatin1String("tur-0");
    else
      include = defaultId || info.id == QLatin1String("tur-0") ||
                info.id == QLatin1String("ublock-experimental");
    if (include) resolved.append(info.id);
  }
  return resolved;
}

QList<FilterRule> DaliNiraBlockerListManager::parseRulesetFile(const QString &filePath, const QString &rulesetId,
                                                             int *responsePhaseSkipped) {
  QList<FilterRule> out;
  QFile file(filePath);
  if (!file.open(QIODevice::ReadOnly) || file.size() > 32 * 1024 * 1024) return out;
  const QJsonDocument doc = QJsonDocument::fromJson(file.read(32 * 1024 * 1024 + 1));
  if (!doc.isArray()) return out;

  const QJsonArray arr = doc.array();
  if (arr.size() > 200000) return out;
  qint64 idCounter = 1;
  for (const auto &val : arr) {
    if (!val.isObject()) continue;
    const QJsonObject obj = val.toObject();
    const QJsonObject action = obj.value(QStringLiteral("action")).toObject();
    const QJsonObject condition = obj.value(QStringLiteral("condition")).toObject();

    FilterRule rule;
    rule.id = obj.value(QStringLiteral("id")).toVariant().toLongLong();
    if (rule.id <= 0) rule.id = ++idCounter;
    rule.priority = obj.value(QStringLiteral("priority")).toInt(1);
    rule.actionType = action.value(QStringLiteral("type")).toString(QStringLiteral("block"));
    rule.urlFilter = condition.value(QStringLiteral("urlFilter")).toString();
    rule.regexFilter = condition.value(QStringLiteral("regexFilter")).toString();
    rule.isCaseSensitive = condition.value(QStringLiteral("isUrlFilterCaseSensitive")).toBool(false);
    if (rule.urlFilter.size() > 8192 || rule.regexFilter.size() > 2048 ||
        (!rule.regexFilter.isEmpty() && !QRegularExpression(rule.regexFilter).isValid())) continue;
    rule.rulesetId = rulesetId;
    rule.domainType = condition.value(QStringLiteral("domainType")).toString();

    if (action.value(QStringLiteral("redirect")).isObject()) {
      const QJsonObject redirect = action.value(QStringLiteral("redirect")).toObject();
      rule.redirectUrl = redirect.value(QStringLiteral("url")).toString();
      rule.redirectTransform = redirect.value(QStringLiteral("transform")).toObject();
      rule.regexSubstitution = redirect.value(QStringLiteral("regexSubstitution")).toString();
      // uBO-Lite DNR files refer to web-accessible resources through
      // extensionPath. They resolve through the same packaged-resource
      // registry text $redirect rules use (one shared data: URL each), so
      // redirect rules are not silently converted into blocks or no-ops.
      if (rule.redirectUrl.isEmpty() && redirectResources_) {
        const QString name = QFileInfo(redirect.value(QStringLiteral("extensionPath")).toString()).fileName();
        rule.redirectResource = redirectResources_->canonicalName(name);
        rule.redirectUrl = redirectResources_->dataUrl(rule.redirectResource);
      }
    }

    // Resource Types
    const QJsonArray resTypes = condition.value(QStringLiteral("resourceTypes")).toArray();
    for (const auto &t : resTypes) rule.resourceTypes.insert(resourceTypeFromString(t.toString()));

    const QJsonArray excResTypes = condition.value(QStringLiteral("excludedResourceTypes")).toArray();
    for (const auto &t : excResTypes) rule.excludedResourceTypes.insert(resourceTypeFromString(t.toString()));

    const QJsonArray requestMethods = condition.value(QStringLiteral("requestMethods")).toArray();
    for (const auto &method : requestMethods) rule.requestMethods.insert(method.toString().toLower());
    const QJsonArray excludedRequestMethods = condition.value(QStringLiteral("excludedRequestMethods")).toArray();
    for (const auto &method : excludedRequestMethods) rule.excludedRequestMethods.insert(method.toString().toLower());
    // QWebEngineUrlRequestInterceptor runs before response headers exist. Such
    // conditions must fail open; evaluating them as unconditional rules causes
    // false blocks (including media/player requests).
    rule.unsupportedHeaderCondition = !condition.value(QStringLiteral("requestHeaders")).toArray().isEmpty() ||
                                      !condition.value(QStringLiteral("responseHeaders")).toArray().isEmpty() ||
                                      !condition.value(QStringLiteral("excludedResponseHeaders")).toArray().isEmpty();
    // Neither engine lets the blocker see or change a response: Chromium's
    // QWebEngineUrlRequestInterceptor and WebKit's send-request run before
    // it. Rules that need it (response-header conditions, modifyHeaders:
    // CSP / Permissions-Policy appends, Set-Cookie / Location / Refresh
    // removal) could never decide anything, so they are left out.
    if (rule.unsupportedHeaderCondition || rule.actionType == QLatin1String("modifyHeaders")) {
      if (responsePhaseSkipped) ++*responsePhaseSkipped;
      continue;
    }

    // Domains
    const QJsonArray reqDomains = condition.value(QStringLiteral("requestDomains")).toArray();
    for (const auto &d : reqDomains) rule.requestDomains.insert(d.toString().toLower());

    const QJsonArray excReqDomains = condition.value(QStringLiteral("excludedRequestDomains")).toArray();
    for (const auto &d : excReqDomains) rule.excludedRequestDomains.insert(d.toString().toLower());

    const QJsonArray initDomains = condition.value(QStringLiteral("initiatorDomains")).toArray();
    for (const auto &d : initDomains) rule.initiatorDomains.insert(d.toString().toLower());

    const QJsonArray excInitDomains = condition.value(QStringLiteral("excludedInitiatorDomains")).toArray();
    for (const auto &d : excInitDomains) rule.excludedInitiatorDomains.insert(d.toString().toLower());

    out.append(rule);
  }
  return out;
}

namespace {
// Hostname lists become requestDomains rules, at most 1000 hosts each (the
// same bound text rules are merged under).
void appendHostnameRules(QList<FilterRule> &out, const QJsonArray &hostnames, const QString &action, int priority,
                         FilterRuleScope scope, const QString &rulesetId, qint64 *idCounter) {
  static const QRegularExpression kHostname(QStringLiteral("^[a-z0-9_-][a-z0-9_.-]{0,252}$"));
  FilterRule current;
  for (const QJsonValue &value : hostnames) {
    const QString host = value.toString().toLower();
    if (!kHostname.match(host).hasMatch()) continue;
    if (current.requestDomains.size() >= 1000) {
      out.append(current);
      current = FilterRule{};
    }
    if (current.requestDomains.isEmpty()) {
      current.id = ++*idCounter;
      current.actionType = action;
      current.priority = priority;
      current.scope = scope;
      current.rulesetId = rulesetId;
    }
    current.requestDomains.insert(host);
  }
  if (!current.requestDomains.isEmpty()) out.append(current);
}

// uBO Lite's generated form of a list's $popup filters
// (scripting/popup/<id>.js: "const details = {block|allow: {hostnames,
// regexes}}"), as popup-scope rules with the text rules' priorities.
QList<FilterRule> generatedPopupRules(const QString &path, const QString &rulesetId) {
  QList<FilterRule> out;
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly) || file.size() > 4 * 1024 * 1024) return out;
  const QString source = QString::fromUtf8(file.readAll());
  static const QString kMarker = QStringLiteral("const details = ");
  const int begin = source.indexOf(kMarker);
  const int end = begin < 0 ? -1 : source.indexOf(QStringLiteral(";\n"), begin);
  if (begin < 0 || end < 0) return out;
  const QJsonObject details =
      QJsonDocument::fromJson(source.mid(begin + kMarker.size(), end - begin - kMarker.size()).toUtf8()).object();
  qint64 idCounter = 0;
  for (const QString &kind : {QStringLiteral("block"), QStringLiteral("allow")}) {
    const QJsonObject entry = details.value(kind).toObject();
    const bool allow = kind == QLatin1String("allow");
    const QString action = allow ? QStringLiteral("allow") : QStringLiteral("block");
    const int priority = allow ? 30 : 10;
    appendHostnameRules(out, entry.value(QStringLiteral("hostnames")).toArray(), action, priority,
                        FilterRuleScope::Popup, rulesetId, &idCounter);
    // Regexes come as [literal prefilter, JSON [{re, f, to?, xto?}]] pairs.
    const QJsonArray regexes = entry.value(QStringLiteral("regexes")).toArray();
    for (int i = 0; i + 1 < regexes.size(); i += 2) {
      const QJsonArray group = QJsonDocument::fromJson(regexes.at(i + 1).toString().toUtf8()).array();
      for (const QJsonValue &value : group) {
        const QJsonObject object = value.toObject();
        FilterRule rule;
        rule.regexFilter = object.value(QStringLiteral("re")).toString();
        if (rule.regexFilter.isEmpty() || rule.regexFilter.size() > 2048 ||
            !QRegularExpression(rule.regexFilter).isValid())
          continue;
        rule.isCaseSensitive = !object.value(QStringLiteral("f")).toString().contains(QLatin1Char('i'));
        for (const QJsonValue &domain : object.value(QStringLiteral("to")).toArray())
          rule.requestDomains.insert(domain.toString().toLower());
        for (const QJsonValue &domain : object.value(QStringLiteral("xto")).toArray())
          rule.excludedRequestDomains.insert(domain.toString().toLower());
        rule.id = ++idCounter;
        rule.actionType = action;
        rule.priority = priority;
        rule.scope = FilterRuleScope::Popup;
        rule.rulesetId = rulesetId;
        out.append(rule);
      }
    }
  }
  return out;
}

// uBO Lite's generated form of a list's $generichide exceptions: hostnames
// on which its generic cosmetic filters are not used (generic-details.json).
QList<FilterRule> generatedGenericHideRules(const QJsonArray &genericDetails, const QString &rulesetId) {
  QList<FilterRule> out;
  qint64 idCounter = 0;
  for (const QJsonValue &value : genericDetails) {
    const QJsonArray pair = value.toArray();
    if (pair.at(0).toString() != rulesetId) continue;
    appendHostnameRules(out, pair.at(1).toObject().value(QStringLiteral("unhide")).toArray(), QStringLiteral("allow"),
                        30, FilterRuleScope::GenericHide, rulesetId, &idCounter);
  }
  return out;
}

// uBO Lite's generated form of a list's $urlskip filters (urlskip/<id>.json):
// [{re, steps, hostnames?, c?}] for top-level navigations: "hostnames" are the
// filter's request domains, "c" makes the expression case-sensitive.
QList<FilterRule> generatedUrlSkipRules(const QString &path, const QString &rulesetId) {
  QList<FilterRule> out;
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly) || file.size() > 4 * 1024 * 1024) return out;
  qint64 idCounter = 0;
  for (const QJsonValue &value : QJsonDocument::fromJson(file.readAll()).array()) {
    const QJsonObject object = value.toObject();
    FilterRule rule;
    rule.regexFilter = object.value(QStringLiteral("re")).toString();
    for (const QJsonValue &step : object.value(QStringLiteral("steps")).toArray())
      rule.urlSkipSteps.append(step.toString());
    if (rule.regexFilter.isEmpty() || rule.regexFilter.size() > 2048 || rule.urlSkipSteps.isEmpty() ||
        !QRegularExpression(rule.regexFilter).isValid())
      continue;
    for (const QJsonValue &host : object.value(QStringLiteral("hostnames")).toArray())
      rule.requestDomains.insert(host.toString().toLower());
    rule.isCaseSensitive = object.value(QStringLiteral("c")).toBool();
    rule.id = ++idCounter;
    rule.actionType = QStringLiteral("urlskip");
    rule.priority = 10;
    rule.scope = FilterRuleScope::UrlSkip;
    rule.rulesetId = rulesetId;
    out.append(rule);
  }
  return out;
}

QJsonArray generatedArray(const QString &source, const QString &name) {
  const QString marker = QStringLiteral("const %1").arg(name);
  const int declaration = source.indexOf(marker);
  if (declaration < 0) return {};
  const int begin = source.indexOf(QLatin1Char('['), declaration + marker.size());
  if (begin < 0) return {};
  const int end = source.indexOf(QStringLiteral("];"), begin);
  if (end < 0) return {};
  const QJsonDocument document = QJsonDocument::fromJson(source.mid(begin, end - begin + 1).toUtf8());
  return document.isArray() ? document.array() : QJsonArray{};
}

bool generatedScriptletAppliesToHost(const QString &source, const QString &rawHost) {
  const QString host = rawHost.trimmed().toLower();
  if (host.isEmpty()) return false;
  const QJsonArray indexedHosts = generatedArray(source, QStringLiteral("$scriptletHostnames$"));
  if (indexedHosts.isEmpty()) return false;

  QSet<QString> candidates{host, QStringLiteral("*")};
  const QStringList parts = host.split(QLatin1Char('.'), Qt::SkipEmptyParts);
  for (int i = 0; i < parts.size(); ++i) candidates.insert(parts.mid(i).join(QLatin1Char('.')));
  if (parts.size() > 1) {
    const int n = parts.size() - 1;
    for (int i = 0; i < n; ++i)
      for (int j = n; j > i; --j)
        candidates.insert(parts.mid(i, j - i).join(QLatin1Char('.')) + QStringLiteral(".*"));
  }
  for (const QJsonValue &value : indexedHosts) {
    if (candidates.contains(value.toString())) return true;
  }

  const QJsonArray regexEntries = generatedArray(source, QStringLiteral("$scriptletFromRegexes$"));
  for (int i = 0; i + 2 < regexEntries.size(); i += 3) {
    if (!host.contains(regexEntries.at(i).toString())) continue;
    const QRegularExpression expression(QStringLiteral("(*LIMIT_MATCH=100000)(*LIMIT_DEPTH=1000)") + regexEntries.at(i + 1).toString());
    if (expression.isValid() && expression.match(host).hasMatch()) return true;
  }
  return false;
}

const QString kNoopJs = QStringLiteral("data:application/javascript;base64,InVzZSBzdHJpY3QiOwp2b2lkIDA7Cg==");
const QString kNoopJson = QStringLiteral("data:application/json;base64,e30=");
// uBOL's safe resource substitution for doubleclick.net/instream/ad_status.js.
// It preserves the tiny player-facing API without loading the remote ad script
// and does not emit any impression, completion, playback, or click telemetry.
const QString kDoubleClickInstreamStatusJs =
    QStringLiteral("data:application/javascript;base64,d2luZG93Lmdvb2dsZV9hZF9zdGF0dXMgPSAxOwo=");
// Stands in for a blocked Adobe Launch (Experience Platform Tags) loader: a
// _satellite object whose track/getVar/cookie/logger calls do nothing, so a
// page's own scripts that call into it keep running. Sends nothing.
const QString kAdobeLaunchStubJs = QStringLiteral(
    "data:application/javascript;base64,"
    "KGZ1bmN0aW9uKCl7J3VzZSBzdHJpY3QnO2lmKHdpbmRvdy5fc2F0ZWxsaXRlKXJldHVybjt2YXIgbm9vcD1mdW5jdGlvbigpe30scz17fTsKWyd0"
    "cmFjaycsJ3NldFZhcicsJ3BhZ2VCb3R0b20nLCdub3RpZnknLCdzZXREZWJ1ZycsJ3NldENvb2tpZScsJ3JlYWRDb29raWUnLCdyZW1vdmVDb29r"
    "aWUnXS5mb3JFYWNoKGZ1bmN0aW9uKG4pe3Nbbl09bm9vcDt9KTsKcy5nZXRWYXI9ZnVuY3Rpb24oKXt9O3MuZ2V0VmlzaXRvcklkPWZ1bmN0aW9u"
    "KCl7cmV0dXJuIG51bGw7fTsKcy5jb29raWU9e2dldDpub29wLHNldDpub29wLHJlbW92ZTpub29wfTtzLmxvZ2dlcj17bG9nOm5vb3AsaW5mbzpu"
    "b29wLHdhcm46bm9vcCxlcnJvcjpub29wLGRlYnVnOm5vb3B9OwpzLmJ1aWxkSW5mbz17fTtzLmVudmlyb25tZW50PXt9O3MucHJvcGVydHk9e25h"
    "bWU6Jyd9O3MuY29tcGFueT17b3JnSWQ6Jyd9Owp3aW5kb3cuX3NhdGVsbGl0ZT1zO30pKCk7Cg==");

const QSet<QString> kYouTubeInitiators = {
    QStringLiteral("youtube.com"),
    QStringLiteral("www.youtube.com"),
    QStringLiteral("m.youtube.com"),
    QStringLiteral("music.youtube.com")
};

QList<FilterRule> getBuiltinCoreRules() {
  QList<FilterRule> list;

  // 1. ADBLOCK_NOOP_DNR_RULES (priority: 65)
  {
    FilterRule r;
    r.id = 1100003; r.priority = 65; r.actionType = QStringLiteral("redirect");
    r.urlFilter = QStringLiteral("||www.googletagservices.com/tag/js/gpt.js");
    r.resourceTypes = {DaliNiraBlockerResourceType::Script};
    r.redirectUrl = kNoopJs;
    r.rulesetId = QStringLiteral("dalinira-noop");
    r.redirectResource = QStringLiteral("noop.js");
    list.append(r);
  }
  {
    FilterRule r;
    r.id = 1100004; r.priority = 65; r.actionType = QStringLiteral("redirect");
    r.urlFilter = QStringLiteral("||www.google-analytics.com/analytics.js");
    r.resourceTypes = {DaliNiraBlockerResourceType::Script};
    r.redirectUrl = kNoopJs;
    r.rulesetId = QStringLiteral("dalinira-noop");
    r.redirectResource = QStringLiteral("noop.js");
    list.append(r);
  }
  {
    FilterRule r;
    r.id = 1100005; r.priority = 65; r.actionType = QStringLiteral("redirect");
    r.urlFilter = QStringLiteral("||www.google-analytics.com/gtag/js");
    r.resourceTypes = {DaliNiraBlockerResourceType::Script};
    r.redirectUrl = kNoopJs;
    r.rulesetId = QStringLiteral("dalinira-noop");
    r.redirectResource = QStringLiteral("noop.js");
    list.append(r);
  }
  {
    FilterRule r;
    r.id = 1100006; r.priority = 65; r.actionType = QStringLiteral("redirect");
    r.urlFilter = QStringLiteral("/pagead/ppub_config");
    r.resourceTypes = {DaliNiraBlockerResourceType::Xhr};
    r.redirectUrl = kNoopJson;
    r.rulesetId = QStringLiteral("dalinira-noop");
    r.redirectResource = QStringLiteral("noop.json");
    list.append(r);
  }

  // 2. ADBLOCK_YOUTUBE_DNR_RULES (Source of Truth: DaliNira-WebMedia/main.js lines 3740-3890)
  // 2a. YouTube Core Allow Rules (priority: 90)
  {
    FilterRule r;
    r.id = 1000000; r.priority = 90; r.actionType = QStringLiteral("allow");
    r.initiatorDomains = kYouTubeInitiators;
    r.requestDomains = {
        QStringLiteral("i.ytimg.com"),
        QStringLiteral("yt3.ggpht.com"),
        QStringLiteral("ggpht.com"),
        QStringLiteral("youtube.com"),
        QStringLiteral("www.youtube.com"),
        QStringLiteral("m.youtube.com"),
        QStringLiteral("music.youtube.com"),
        QStringLiteral("youtubei.googleapis.com")
    };
    r.resourceTypes = {
        DaliNiraBlockerResourceType::Script,
        DaliNiraBlockerResourceType::Xhr,
        DaliNiraBlockerResourceType::Image,
        DaliNiraBlockerResourceType::Stylesheet,
        DaliNiraBlockerResourceType::Font
    };
    r.rulesetId = QStringLiteral("dalinira-youtube-core");
    list.append(r);
  }
  {
    FilterRule r;
    r.id = 1000011; r.priority = 90; r.actionType = QStringLiteral("allow");
    r.initiatorDomains = kYouTubeInitiators;
    r.requestDomains = {QStringLiteral("googlevideo.com")};
    r.resourceTypes = {DaliNiraBlockerResourceType::Media, DaliNiraBlockerResourceType::Xhr};
    r.rulesetId = QStringLiteral("dalinira-youtube-core");
    list.append(r);
  }

  // 2b. YouTube Targeted Ad Blocks & Redirects (priority: 110)
  {
    FilterRule r;
    r.id = 1000006; r.priority = 120; r.actionType = QStringLiteral("redirect");
    r.initiatorDomains = kYouTubeInitiators;
    r.urlFilter = QStringLiteral("||static.doubleclick.net/instream/ad_status.js|");
    r.resourceTypes = {DaliNiraBlockerResourceType::Script, DaliNiraBlockerResourceType::Xhr};
    r.redirectUrl = kDoubleClickInstreamStatusJs;
    r.rulesetId = QStringLiteral("dalinira-youtube-safe-resource");
    r.redirectResource = QStringLiteral("doubleclick_instream_ad_status.js");
    list.append(r);
  }
  {
    FilterRule r;
    r.id = 1000001; r.priority = 110; r.actionType = QStringLiteral("block");
    r.initiatorDomains = kYouTubeInitiators;
    r.requestDomains = {
        QStringLiteral("ad.doubleclick.net"),
        QStringLiteral("googleads.g.doubleclick.net"),
        QStringLiteral("pagead2.googlesyndication.com"),
        QStringLiteral("tpc.googlesyndication.com"),
        QStringLiteral("static.doubleclick.net")
    };
    r.resourceTypes = {
        DaliNiraBlockerResourceType::Script,
        DaliNiraBlockerResourceType::Xhr,
        DaliNiraBlockerResourceType::SubFrame,
        DaliNiraBlockerResourceType::Image,
        DaliNiraBlockerResourceType::Media
    };
    r.rulesetId = QStringLiteral("dalinira-youtube");
    list.append(r);
  }
  {
    FilterRule r;
    r.id = 1000002; r.priority = 110; r.actionType = QStringLiteral("block");
    r.initiatorDomains = kYouTubeInitiators;
    r.urlFilter = QStringLiteral("||youtube.com/pagead/");
    r.resourceTypes = {
        DaliNiraBlockerResourceType::Script,
        DaliNiraBlockerResourceType::Xhr,
        DaliNiraBlockerResourceType::SubFrame,
        DaliNiraBlockerResourceType::Image
    };
    r.rulesetId = QStringLiteral("dalinira-youtube");
    list.append(r);
  }
  {
    FilterRule r;
    r.id = 1000003; r.priority = 110; r.actionType = QStringLiteral("block");
    r.initiatorDomains = kYouTubeInitiators;
    r.urlFilter = QStringLiteral("/api/stats/ads");
    r.resourceTypes = {DaliNiraBlockerResourceType::Xhr, DaliNiraBlockerResourceType::Ping};
    r.rulesetId = QStringLiteral("dalinira-youtube");
    list.append(r);
  }
  {
    FilterRule r;
    r.id = 1000004; r.priority = 110; r.actionType = QStringLiteral("block");
    r.initiatorDomains = kYouTubeInitiators;
    r.urlFilter = QStringLiteral("||youtube.com/youtubei/v1/log_event");
    r.resourceTypes = {DaliNiraBlockerResourceType::Xhr, DaliNiraBlockerResourceType::Ping};
    r.rulesetId = QStringLiteral("dalinira-youtube");
    list.append(r);
  }
  {
    FilterRule r;
    r.id = 1000005; r.priority = 110; r.actionType = QStringLiteral("block");
    r.requestDomains = {QStringLiteral("ads.youtube.com")};
    r.rulesetId = QStringLiteral("dalinira-youtube");
    list.append(r);
  }
  {
    FilterRule r;
    r.id = 1000009; r.priority = 110; r.actionType = QStringLiteral("block");
    r.initiatorDomains = kYouTubeInitiators;
    r.regexFilter = QStringLiteral(R"([?&]ctier=l(?:&|$))");
    r.requestDomains = {QStringLiteral("googlevideo.com")};
    r.resourceTypes = {DaliNiraBlockerResourceType::Xhr, DaliNiraBlockerResourceType::Media};
    r.rulesetId = QStringLiteral("dalinira-youtube");
    list.append(r);
  }
  {
    FilterRule r;
    r.id = 1000010; r.priority = 110; r.actionType = QStringLiteral("block");
    r.initiatorDomains = kYouTubeInitiators;
    r.regexFilter = QStringLiteral(R"([?&]adformat=)");
    r.requestDomains = {QStringLiteral("googlevideo.com")};
    r.resourceTypes = {DaliNiraBlockerResourceType::Xhr, DaliNiraBlockerResourceType::Media};
    r.rulesetId = QStringLiteral("dalinira-youtube");
    list.append(r);
  }
  {
    FilterRule r;
    r.id = 1000012; r.priority = 110; r.actionType = QStringLiteral("block");
    r.initiatorDomains = kYouTubeInitiators;
    r.regexFilter = QStringLiteral(R"([?&]source=web_video_ads(?:&|$))");
    r.requestDomains = {QStringLiteral("googlevideo.com")};
    r.resourceTypes = {DaliNiraBlockerResourceType::Xhr, DaliNiraBlockerResourceType::Media};
    r.rulesetId = QStringLiteral("dalinira-youtube");
    list.append(r);
  }

  // 3. ADBLOCK_PLATFORM_CORE_DNR_RULES (Source of Truth: DaliNira-WebMedia/main.js lines 3891-4050, priority: 95)
  const QSet<DaliNiraBlockerResourceType> standardPlatformTypes = {
      DaliNiraBlockerResourceType::Stylesheet,
      DaliNiraBlockerResourceType::Script,
      DaliNiraBlockerResourceType::Font,
      DaliNiraBlockerResourceType::Image,
      DaliNiraBlockerResourceType::Media,
      DaliNiraBlockerResourceType::Xhr
  };

  auto addPlatformCore = [&list, &standardPlatformTypes](qint64 id, const QSet<QString> &initiators, const QSet<QString> &requests) {
    FilterRule r;
    r.id = id;
    r.priority = 95;
    r.actionType = QStringLiteral("allow");
    r.initiatorDomains = initiators;
    r.requestDomains = requests;
    r.resourceTypes = standardPlatformTypes;
    r.rulesetId = QStringLiteral("dalinira-platform-core");
    list.append(r);
  };

  // Deezer
  addPlatformCore(1010000, {QStringLiteral("deezer.com"), QStringLiteral("www.deezer.com")},
                  {QStringLiteral("deezer.com"), QStringLiteral("www.deezer.com"), QStringLiteral("dzcdn.net")});

  // SoundCloud
  addPlatformCore(1010001, {QStringLiteral("soundcloud.com"), QStringLiteral("www.soundcloud.com")},
                  {QStringLiteral("soundcloud.com"), QStringLiteral("www.soundcloud.com"), QStringLiteral("sndcdn.com")});

  // Facebook & Instagram
  addPlatformCore(1010002, {
      QStringLiteral("facebook.com"), QStringLiteral("www.facebook.com"), QStringLiteral("m.facebook.com"),
      QStringLiteral("instagram.com"), QStringLiteral("www.instagram.com")
  }, {
      QStringLiteral("facebook.com"), QStringLiteral("www.facebook.com"), QStringLiteral("m.facebook.com"),
      QStringLiteral("web.facebook.com"), QStringLiteral("graph.facebook.com"), QStringLiteral("static.xx.fbcdn.net"),
      QStringLiteral("fbcdn.net"), QStringLiteral("fbcdn.com"), QStringLiteral("fbsbx.com"),
      QStringLiteral("facebook.net"), QStringLiteral("instagram.com"), QStringLiteral("www.instagram.com"),
      QStringLiteral("cdninstagram.com")
  });

  // TikTok
  addPlatformCore(1010003, {QStringLiteral("tiktok.com"), QStringLiteral("www.tiktok.com"), QStringLiteral("m.tiktok.com")}, {
      QStringLiteral("tiktok.com"), QStringLiteral("www.tiktok.com"), QStringLiteral("m.tiktok.com"),
      QStringLiteral("tiktokcdn.com"), QStringLiteral("tiktokcdn-us.com"), QStringLiteral("tiktokv.com"),
      QStringLiteral("ttwstatic.com"), QStringLiteral("byteoversea.com"), QStringLiteral("ibyteimg.com"),
      QStringLiteral("byteimg.com"), QStringLiteral("ibytedtos.com"), QStringLiteral("muscdn.com")
  });

  // X / Twitter
  addPlatformCore(1010004, {QStringLiteral("x.com"), QStringLiteral("www.x.com"), QStringLiteral("twitter.com"), QStringLiteral("www.twitter.com")}, {
      QStringLiteral("x.com"), QStringLiteral("www.x.com"), QStringLiteral("twitter.com"), QStringLiteral("www.twitter.com"), QStringLiteral("twimg.com")
  });

  // Reddit
  addPlatformCore(1010005, {QStringLiteral("reddit.com"), QStringLiteral("www.reddit.com"), QStringLiteral("old.reddit.com")}, {
      QStringLiteral("reddit.com"), QStringLiteral("www.reddit.com"), QStringLiteral("old.reddit.com"),
      QStringLiteral("redditstatic.com"), QStringLiteral("redditmedia.com"), QStringLiteral("redd.it")
  });

  // Twitch
  addPlatformCore(1010006, {QStringLiteral("twitch.tv"), QStringLiteral("www.twitch.tv")}, {
      QStringLiteral("twitch.tv"), QStringLiteral("www.twitch.tv"), QStringLiteral("jtvnw.net"),
      QStringLiteral("ttvnw.net"), QStringLiteral("twitchcdn.net"), QStringLiteral("ext-twitch.tv")
  });

  // Telegram
  addPlatformCore(1010007, {QStringLiteral("telegram.org"), QStringLiteral("www.telegram.org"), QStringLiteral("web.telegram.org"), QStringLiteral("t.me"), QStringLiteral("www.t.me")}, {
      QStringLiteral("telegram.org"), QStringLiteral("www.telegram.org"), QStringLiteral("web.telegram.org"), QStringLiteral("t.me"), QStringLiteral("www.t.me")
  });

  // WhatsApp
  addPlatformCore(1010008, {QStringLiteral("whatsapp.com"), QStringLiteral("www.whatsapp.com"), QStringLiteral("web.whatsapp.com")}, {
      QStringLiteral("whatsapp.com"), QStringLiteral("www.whatsapp.com"), QStringLiteral("web.whatsapp.com"), QStringLiteral("whatsapp.net")
  });

  // Mixcloud
  addPlatformCore(1010009, {QStringLiteral("mixcloud.com"), QStringLiteral("www.mixcloud.com")}, {
      QStringLiteral("mixcloud.com"), QStringLiteral("www.mixcloud.com"), QStringLiteral("mxcdn.net")
  });

  // Spotify
  addPlatformCore(1010010, {
      QStringLiteral("spotify.com"), QStringLiteral("www.spotify.com"), QStringLiteral("open.spotify.com"), QStringLiteral("accounts.spotify.com")
  }, {
      QStringLiteral("spotify.com"), QStringLiteral("www.spotify.com"), QStringLiteral("open.spotify.com"),
      QStringLiteral("accounts.spotify.com"), QStringLiteral("scdn.co"), QStringLiteral("spotifycdn.com"),
      QStringLiteral("spotifycdn.net"), QStringLiteral("akamaized.net")
  });

  // 4. Trackers the default lists let through. List priorities apply: a
  //    list's site-specific exception (30) still wins for a site that needs
  //    the script; the "privacy" ruleset id counts these as trackers and
  //    puts them under the tracker-protection toggle.
  // 4a. The Adobe Launch loader (assets.adobedtm.com/.../launch-*.js), once
  //     EasyPrivacy's generic exceptions for it are dropped (see
  //     isSupersededListException), is replaced by a _satellite stub instead
  //     of EasyPrivacy's plain ||adobedtm.com^$third-party block (priority 11
  //     ranks it above that block, as a list $redirect would). Its exclusions
  //     are that block's.
  for (const auto &[id, filter] : {std::pair{qint64(1300000), QStringLiteral("||adobedtm.com/launch-")},
                                   std::pair{qint64(1300001), QStringLiteral("||adobedtm.com^*/launch-")}}) {
    FilterRule r;
    r.id = id; r.priority = 11; r.actionType = QStringLiteral("redirect");
    r.urlFilter = filter;
    r.domainType = QStringLiteral("thirdParty");
    r.excludedInitiatorDomains = {QStringLiteral("adobe.com"), QStringLiteral("costco.com")};
    r.resourceTypes = {DaliNiraBlockerResourceType::Script};
    r.redirectUrl = kAdobeLaunchStubJs;
    r.rulesetId = QStringLiteral("dalinira-privacy");
    list.append(r);
  }
  // 4b. The Optimizely Web snippet (cdn.optimizely.com/js/<project>.js).
  //     EasyPrivacy already blocks its newer path (||optimizely.com/public/)
  //     and its event endpoints; this is the same snippet at its older path.
  {
    FilterRule r;
    r.id = 1300002; r.priority = 10; r.actionType = QStringLiteral("block");
    r.urlFilter = QStringLiteral("||cdn.optimizely.com/js/");
    r.domainType = QStringLiteral("thirdParty");
    r.resourceTypes = {DaliNiraBlockerResourceType::Script};
    r.rulesetId = QStringLiteral("dalinira-privacy");
    list.append(r);
  }

  // 5. Debouncing of the link-shim redirectors uBO lists only in its
  //    (optional) overlay annoyances list; the default uBlock filters list
  //    brings the rest (bundled urlskip data). The same patterns and steps.
  for (const auto &[id, pattern] : {
           std::pair{qint64(1300100), QStringLiteral(R"(^[^:]+://(?:[^/]+\.)?l\.facebook\.com/l\.php\?u=)")},
           std::pair{qint64(1300101), QStringLiteral(R"(^[^:]+://(?:[^/]+\.)?l\.instagram\.com/\?u=)")}}) {
    FilterRule r;
    r.id = id; r.priority = 10; r.actionType = QStringLiteral("urlskip");
    r.regexFilter = pattern;
    r.scope = FilterRuleScope::UrlSkip;
    r.urlSkipSteps = {QStringLiteral("?u")};
    r.rulesetId = QStringLiteral("dalinira-debounce");
    list.append(r);
  }

  return list;
}

// EasyPrivacy's two generic exceptions for the Adobe Launch loader
// (@@||adobedtm.com/launch-$script,xhr and @@||adobedtm.com^*/launch-$script,xhr)
// let the tag manager, and the analytics it loads, run on every site.
// Without them EasyPrivacy's own ||adobedtm.com^$third-party block applies
// (and DaliNira's stub, section 4a). The lists' site-specific (domain=)
// exceptions for sites that need the loader are not affected.
bool isSupersededListException(const FilterRule &rule) {
  static const QSet<QString> kFilters = {QStringLiteral("||adobedtm.com/launch-"),
                                         QStringLiteral("||adobedtm.com^*/launch-")};
  return rule.actionType == QLatin1String("allow") && rule.scope == FilterRuleScope::Network &&
         rule.rulesetId != QLatin1String("custom-user") && rule.initiatorDomains.isEmpty() &&
         rule.requestDomains.isEmpty() && kFilters.contains(rule.urlFilter.toLower());
}
} // namespace

QList<FilterRule> DaliNiraBlockerListManager::loadRulesForModeAndSelection(DaliNiraBlockerMode mode, const QStringList &enabledIds,
                                                                           bool selectionConfigured,
                                                                           bool strictBlock,
                                                                           const QStringList &customLines,
                                                                           QSet<QString> *badfilterKeys) {
  QList<FilterRule> rules;
  QList<FilterListInfo> lists;
  {
    QMutexLocker locker(&mutex_);
    lists = lists_;
  }

  // Always include built-in core rules (YouTube core allows & ad blocks, platform core allows, noop redirects)
  rules.append(getBuiltinCoreRules());

  // One resolver feeds every network/cosmetic/procedural/scriptlet realm.
  const QStringList resolvedIds = resolveRulesetIds(mode, enabledIds, selectionConfigured);
  const QSet<QString> activeIds(resolvedIds.begin(), resolvedIds.end());

  const QString baseDir = rulesetDir();
  // Text sources first: a $badfilter line in any of them (or in the custom
  // filters) disables the same rule in all of them. Generated rulesets have
  // no text form; uBO Lite applied their lists' $badfilter when generating.
  QHash<QString, QByteArray> downloaded;
  QSet<QString> badfilters = DaliNiraBlockerEngine::badfilterKeys(customLines.join(QLatin1Char('\n')));
  for (const auto &info : lists) {
    if (!activeIds.contains(info.id) || !filterStore_) continue;
    const QByteArray copy = filterStore_->readActiveCopy(info.id);
    if (copy.isEmpty()) continue;
    if (copy.contains("badfilter")) badfilters.unite(DaliNiraBlockerEngine::badfilterKeys(QString::fromUtf8(copy)));
    downloaded.insert(info.id, copy);
  }
  const DaliNiraBlockerEngine::TextCompileContext textContext{redirectResources_.get(), &badfilters};
  int responsePhaseSkipped = 0;
  QJsonArray genericDetails;

  for (const auto &info : lists) {
    if (!activeIds.contains(info.id)) continue;

    // A validated download replaces the list's bundled network rules; the
    // bundled cosmetic, scriptlet and strictblock data stay in use.
    const auto copy = downloaded.constFind(info.id);
    if (copy != downloaded.constEnd()) {
      rules.append(DaliNiraBlockerEngine::compileTextFilterList(QString::fromUtf8(copy.value()), info.id,
                                                                textContext).rules);
    } else {
      const QString mainPath = baseDir + QStringLiteral("/main/") + info.id + QStringLiteral(".json");
      if (QFileInfo::exists(mainPath)) rules.append(parseRulesetFile(mainPath, info.id, &responsePhaseSkipped));

      const QString regexPath = baseDir + QStringLiteral("/regex/") + info.id + QStringLiteral(".json");
      if (QFileInfo::exists(regexPath)) rules.append(parseRulesetFile(regexPath, info.id, &responsePhaseSkipped));

      // The same list's $popup, $urlskip and $generichide filters, in their
      // generated form (a downloaded copy carries them as text instead).
      rules.append(generatedPopupRules(baseDir + QStringLiteral("/scripting/popup/") + info.id + QStringLiteral(".js"), info.id));
      rules.append(generatedUrlSkipRules(baseDir + QStringLiteral("/urlskip/") + info.id + QStringLiteral(".json"), info.id));
      if (genericDetails.isEmpty()) {
        QFile detailsFile(baseDir + QStringLiteral("/generic-details.json"));
        if (detailsFile.open(QIODevice::ReadOnly) && detailsFile.size() <= 8 * 1024 * 1024)
          genericDetails = QJsonDocument::fromJson(detailsFile.readAll()).array();
        if (genericDetails.isEmpty()) genericDetails.append(QJsonValue());  // read once
      }
      rules.append(generatedGenericHideRules(genericDetails, info.id));
    }

    // Legacy strictblock realms are always active for malware lists, and are
    // additionally enabled by strict protection or the aggressive preset.
    const bool includeStrict = strictBlock || mode == DaliNiraBlockerMode::Aggressive ||
                               info.group == QLatin1String("malware");
    const QString strictPath = baseDir + QStringLiteral("/strictblock/") + info.id + QStringLiteral(".json");
    if (includeStrict && QFileInfo::exists(strictPath)) {
      QList<FilterRule> strictRules = parseRulesetFile(strictPath, QStringLiteral("strictblock-") + info.id);
      for (FilterRule &rule : strictRules) {
        rule.id = 1200000 + rule.id;
        rule.priority = qMax(80, rule.priority);
        rule.actionType = QStringLiteral("block");
        rule.redirectUrl.clear();
      }
      rules.append(strictRules);
    }
  }
  rules.removeIf(isSupersededListException);
  responsePhaseRulesSkipped_ = responsePhaseSkipped;
  if (badfilterKeys) *badfilterKeys = badfilters;
  return rules;
}

void DaliNiraBlockerListManager::invalidateCaches() {
  QMutexLocker locker(&mutex_);
  scriptingSourceCache_.clear();
  scriptingJsonCache_.clear();
  scriptingApplicabilityCache_.clear();
}

QJsonObject DaliNiraBlockerListManager::cachedScriptingJson(const QString &path) const {
  {
    QMutexLocker locker(&mutex_);
    if (const auto *found = scriptingJsonCache_.object(path)) return *found;
  }
  QFile file(path);
  QJsonObject object;
  if (file.open(QIODevice::ReadOnly) && file.size() <= 8 * 1024 * 1024) object = QJsonDocument::fromJson(file.read(8 * 1024 * 1024)).object();
  QMutexLocker locker(&mutex_);
  scriptingJsonCache_.insert(path, new QJsonObject(object), qMax<qint64>(1, file.size() * 3));
  return object;
}

QString DaliNiraBlockerListManager::loadCosmeticCssForSelection(const QStringList &enabledIds, bool selectionConfigured) const {
  const QString baseDir = rulesetDir();
  if (baseDir.isEmpty()) return QString();

  QString combinedCss;
  const QString genericHighDir = baseDir + QStringLiteral("/scripting/generichigh");
  if (QDir(genericHighDir).exists()) {
    QStringList targetIds = enabledIds;
    if (!selectionConfigured && targetIds.isEmpty()) {
      targetIds = {QStringLiteral("easylist"), QStringLiteral("annoyances-others"), QStringLiteral("ublock-filters")};
    }
    for (const QString &id : targetIds) {
      if (!QRegularExpression(QStringLiteral("^[a-zA-Z0-9_-]{1,80}$")).match(id).hasMatch()) continue;
      const QString cssFile = genericHighDir + QStringLiteral("/") + id + QStringLiteral(".css");
      QFile f(cssFile);
      if (f.exists() && f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        combinedCss += QString::fromUtf8(f.read(8 * 1024 * 1024)) + QStringLiteral("\n\n");
      }
    }
  }
  return combinedCss;
}

QString DaliNiraBlockerListManager::loadSpecificCosmeticCssForHost(const QString &rawHost,
                                                                  const QStringList &enabledIds,
                                                                  bool selectionConfigured) const {
  const QString host = rawHost.trimmed().toLower();
  if (host.isEmpty()) return QString();
  const QString baseDir = rulesetDir();

  QStringList ids = enabledIds;
  if (!selectionConfigured && ids.isEmpty()) ids = {QStringLiteral("ublock-filters"), QStringLiteral("easylist"),
                             QStringLiteral("easyprivacy"), QStringLiteral("tur-0")};
  QSet<QString> selectors;
  QSet<QString> exceptions;

  auto addList = [&selectors, &exceptions](const QJsonObject &data, int listIndex) {
    const QJsonArray selectorLists = data.value(QStringLiteral("selectorLists")).toArray();
    const QJsonArray sourceSelectors = data.value(QStringLiteral("selectors")).toArray();
    if (listIndex < 0 || listIndex >= selectorLists.size()) return;
    const QJsonDocument fragment = QJsonDocument::fromJson(
        (QStringLiteral("[") + selectorLists.at(listIndex).toString() + QStringLiteral("]")).toUtf8());
    if (!fragment.isArray()) return;
    for (const QJsonValue &value : fragment.array()) {
      const int selectorIndex = value.toInt();
      const int resolved = selectorIndex >= 0 ? selectorIndex : ~selectorIndex;
      if (resolved < 0 || resolved >= sourceSelectors.size()) continue;
      const QString selector = sourceSelectors.at(resolved).toString().trimmed();
      if (selector.isEmpty()) continue;
      // Procedural rules formatted as JSON (e.g. {"selector":..., "tasks":...})
      // must not be mixed into standard CSS stylesheets.
      if (selector.startsWith(QLatin1Char('{'))) continue;
      if (selectorIndex >= 0) selectors.insert(selector);
      else exceptions.insert(selector);
    }
  };

  if (!baseDir.isEmpty()) {
    for (const QString &id : ids) {
    if (!QRegularExpression(QStringLiteral("^[a-zA-Z0-9_-]{1,80}$")).match(id).hasMatch()) continue;
      const QJsonObject data = cachedScriptingJson(
          baseDir + QStringLiteral("/scripting/specific/") + id + QStringLiteral(".json"));
      if (data.isEmpty()) continue;
    const QJsonArray hostnames = data.value(QStringLiteral("hostnames")).toArray();
    const QJsonArray refs = data.value(QStringLiteral("selectorListRefs")).toArray();
    if (hostnames.size() != refs.size()) continue;

    QStringList variants;
    const QStringList parts = host.split(QLatin1Char('.'), Qt::SkipEmptyParts);
    for (int i = 0; i < parts.size(); ++i) variants.append(parts.mid(i).join(QLatin1Char('.')));
    if (data.value(QStringLiteral("hasEntities")).toBool() && parts.size() > 1) {
      const int n = parts.size() - 1;
      for (int i = 0; i < n; ++i)
        for (int j = n; j > i; --j) variants.append(parts.mid(i, j - i).join(QLatin1Char('.')) + QStringLiteral(".*"));
    }
    variants.removeDuplicates();
    auto compareIndexedHostname = [](const QString &left, const QString &right) {
      if (left.size() != right.size()) return left.size() < right.size() ? -1 : 1;
      if (left == right) return 0;
      return left < right ? -1 : 1;
    };
    for (const QString &variant : variants) {
      int low = 0;
      int high = hostnames.size();
      while (low < high) {
        const int mid = (low + high) / 2;
        if (compareIndexedHostname(hostnames.at(mid).toString(), variant) < 0) low = mid + 1;
        else high = mid;
      }
      if (low < hostnames.size() && hostnames.at(low).toString() == variant) addList(data, refs.at(low).toInt(-1));
    }
    const QJsonArray regexes = data.value(QStringLiteral("regexes")).toArray();
    for (int i = 0; i + 2 < regexes.size(); i += 3) {
      if (!host.contains(regexes.at(i).toString())) continue;
      const QRegularExpression regex(QStringLiteral("(*LIMIT_MATCH=100000)(*LIMIT_DEPTH=1000)") + regexes.at(i + 1).toString());
      if (regex.isValid() && regex.match(host).hasMatch()) addList(data, regexes.at(i + 2).toInt(-1));
    }
  }
  }

  const bool isYouTubeHost = host == QLatin1String("youtube.com") ||
                             host == QLatin1String("www.youtube.com") ||
                             host == QLatin1String("m.youtube.com") ||
                             host == QLatin1String("music.youtube.com") ||
                             host.endsWith(QLatin1String(".youtube.com")) ||
                             host == QLatin1String("youtu.be");
  if (isYouTubeHost) {
    static const QStringList kYouTubeAdSelectors = {
        QStringLiteral("ytd-rich-item-renderer:has(ytd-ad-slot-renderer)"),
        QStringLiteral("ytd-rich-item-renderer:has(ytd-in-feed-ad-layout-renderer)"),
        QStringLiteral("ytd-rich-item-renderer:has(.ytd-in-feed-ad-layout-renderer)"),
        QStringLiteral("ytd-rich-item-renderer:has(#ad-badge)"),
        QStringLiteral("ytd-rich-item-renderer:has(.badge-style-type-ad)"),
        QStringLiteral("ytd-rich-item-renderer:has([badge-style-type=\"BADGE_STYLE_TYPE_AD\"])"),
        QStringLiteral("ytd-rich-item-renderer:has(ytd-promoted-sparkles-web-renderer)"),
        QStringLiteral("ytd-rich-item-renderer:has(ytd-promoted-video-renderer)"),
        QStringLiteral("ytd-rich-item-renderer:has(ytd-display-ad-renderer)"),
        QStringLiteral("ytd-rich-section-renderer:has(ytd-ad-slot-renderer)"),
        QStringLiteral("ytd-rich-section-renderer:has(ytd-in-feed-ad-layout-renderer)"),
        QStringLiteral("ytd-rich-section-renderer:has(#masthead-ad)"),
        QStringLiteral("ytd-ad-slot-renderer"),
        QStringLiteral("ytd-in-feed-ad-layout-renderer"),
        QStringLiteral("#masthead-ad"),
        QStringLiteral("ytd-masthead-ad-v3-renderer"),
        QStringLiteral("ytd-video-masthead-ad-v3-renderer"),
        QStringLiteral("ytd-promoted-sparkles-web-renderer"),
        QStringLiteral("ytd-compact-promoted-video-renderer"),
        QStringLiteral("ytd-promoted-video-renderer"),
        QStringLiteral("ytd-display-ad-renderer"),
        QStringLiteral("ytd-search-pyv-renderer"),
        QStringLiteral("#player-ads")
    };
    for (const QString &sel : kYouTubeAdSelectors) {
      selectors.insert(sel);
    }
  }

  if (host == QLatin1String("facebook.com") || host.endsWith(QLatin1String(".facebook.com"))) {
    for (const QString &selector : {
        QStringLiteral("[role=\"article\"]:has(a[href*=\"/ads/about/\"])"),
        QStringLiteral("[role=\"article\"]:has(a[href*=\"/ads/why_am_i_seeing_this_ad/\"])"),
        QStringLiteral("[role=\"article\"]:has([data-ad-preview=\"message\"])"),
        QStringLiteral("[role=\"article\"]:has([data-ad-comet-preview])"),
        QStringLiteral("[role=\"article\"]:has(a[aria-label=\"Sponsored\"])"),
        QStringLiteral("[role=\"article\"]:has(a[aria-label=\"Sponsorlu\"])"),
        QStringLiteral("[role=\"feed\"] [role=\"article\"]:has(a[href*=\"/ads/about/\"])"),
        QStringLiteral("[role=\"feed\"] [role=\"article\"]:has(a[href*=\"/ads/why_am_i_seeing_this_ad/\"])"),
        QStringLiteral("[role=\"feed\"] [role=\"article\"]:has([data-ad-preview=\"message\"])"),
        QStringLiteral("[role=\"feed\"] [role=\"article\"]:has(a[aria-label=\"Sponsored\"])"),
        QStringLiteral("[role=\"feed\"] [role=\"article\"]:has(a[aria-label=\"Sponsorlu\"])"),
        QStringLiteral("div[data-pagelet*=\"Reel\"]:has(a[href*=\"/ads/about/\"])"),
        QStringLiteral("div[data-pagelet*=\"Reel\"]:has(a[aria-label=\"Sponsored\"])"),
        QStringLiteral("div[data-pagelet*=\"Reel\"]:has(a[aria-label=\"Sponsorlu\"])"),
        QStringLiteral("div[data-pagelet*=\"FeedUnit\"]:has(a[href*=\"/ads/about/\"])"),
        QStringLiteral("div[data-pagelet*=\"FeedUnit\"]:has(a[aria-label=\"Sponsored\"])"),
        QStringLiteral("div[data-pagelet*=\"FeedUnit\"]:has(a[aria-label=\"Sponsorlu\"])"),
        QStringLiteral("div[data-ad-preview]"),
        QStringLiteral("div[data-ad-comet-preview]"),
        QStringLiteral("div[data-pagelet*=\"sponsored\"]"),
        QStringLiteral("div[data-pagelet*=\"Sponsored\"]"),
        QStringLiteral("div[data-pagelet*=\"ad_\"]")}) selectors.insert(selector);
  }
  for (const QString &exception : exceptions) selectors.remove(exception);
  if (selectors.isEmpty()) return {};

  // Keep selectors in independent rules. Older Qt WebEngine versions reject
  // an entire comma-separated selector list when one entry (notably :has())
  // is unsupported, which also disables otherwise compatible direct rules.
  QStringList cssRules;
  cssRules.reserve(selectors.size());
  for (const QString &selector : selectors) {
    cssRules.append(selector + QStringLiteral(" { display: none !important; }"));
  }
  return cssRules.join(QLatin1Char('\n'));
}

QJsonArray DaliNiraBlockerListManager::loadProceduralRulesForHost(const QString &rawHost,
                                                                  const QStringList &enabledIds,
                                                                  bool selectionConfigured) const {
  const QString baseDir = rulesetDir();
  const QString host = rawHost.trimmed().toLower();
  if (baseDir.isEmpty() || host.isEmpty()) return {};
  QStringList ids = enabledIds;
  if (!selectionConfigured && ids.isEmpty()) ids = {QStringLiteral("ublock-filters"), QStringLiteral("easylist"),
                             QStringLiteral("easyprivacy"), QStringLiteral("tur-0")};
  QSet<QString> entries;
  QSet<QString> exceptions;
  auto addList = [&entries, &exceptions](const QJsonObject &data, int listIndex) {
    const QJsonArray lists = data.value(QStringLiteral("selectorLists")).toArray();
    const QJsonArray source = data.value(QStringLiteral("selectors")).toArray();
    if (listIndex < 0 || listIndex >= lists.size()) return;
    const QJsonDocument fragment = QJsonDocument::fromJson(
        (QStringLiteral("[") + lists.at(listIndex).toString() + QStringLiteral("]")).toUtf8());
    if (!fragment.isArray()) return;
    for (const QJsonValue &value : fragment.array()) {
      const int index = value.toInt();
      const int resolved = index >= 0 ? index : ~index;
      if (resolved < 0 || resolved >= source.size()) continue;
      const QString item = source.at(resolved).toString();
      if (index >= 0) entries.insert(item); else exceptions.insert(item);
    }
  };
  auto compareIndexedHostname = [](const QString &left, const QString &right) {
    if (left.size() != right.size()) return left.size() < right.size() ? -1 : 1;
    if (left == right) return 0;
    return left < right ? -1 : 1;
  };
  for (const QString &id : ids) {
    if (!QRegularExpression(QStringLiteral("^[a-zA-Z0-9_-]{1,80}$")).match(id).hasMatch()) continue;
    const QJsonObject data = cachedScriptingJson(
        baseDir + QStringLiteral("/scripting/procedural/") + id + QStringLiteral(".json"));
    if (data.isEmpty()) continue;
    const QJsonArray hosts = data.value(QStringLiteral("hostnames")).toArray();
    const QJsonArray refs = data.value(QStringLiteral("selectorListRefs")).toArray();
    if (hosts.size() != refs.size()) continue;
    QStringList variants;
    const QStringList parts = host.split(QLatin1Char('.'), Qt::SkipEmptyParts);
    for (int i = 0; i < parts.size(); ++i) variants.append(parts.mid(i).join(QLatin1Char('.')));
    if (data.value(QStringLiteral("hasEntities")).toBool() && parts.size() > 1) {
      const int n = parts.size() - 1;
      for (int i = 0; i < n; ++i)
        for (int j = n; j > i; --j) variants.append(parts.mid(i, j - i).join(QLatin1Char('.')) + QStringLiteral(".*"));
    }
    variants.removeDuplicates();
    for (const QString &variant : variants) {
      int low = 0, high = hosts.size();
      while (low < high) {
        const int mid = (low + high) / 2;
        if (compareIndexedHostname(hosts.at(mid).toString(), variant) < 0) low = mid + 1; else high = mid;
      }
      if (low < hosts.size() && hosts.at(low).toString() == variant) addList(data, refs.at(low).toInt(-1));
    }
    const QJsonArray regexes = data.value(QStringLiteral("regexes")).toArray();
    for (int i = 0; i + 2 < regexes.size(); i += 3) {
      if (!host.contains(regexes.at(i).toString())) continue;
      const QRegularExpression regex(QStringLiteral("(*LIMIT_MATCH=100000)(*LIMIT_DEPTH=1000)") + regexes.at(i + 1).toString());
      if (regex.isValid() && regex.match(host).hasMatch()) addList(data, regexes.at(i + 2).toInt(-1));
    }
  }
  QJsonArray rules;
  for (const QString &entry : entries) {
    if (exceptions.contains(entry)) continue;
    const QJsonDocument parsed = QJsonDocument::fromJson(entry.toUtf8());
    if (parsed.isObject()) rules.append(parsed.object());
    if (rules.size() >= 600) break;
  }
  return rules;
}

QList<QPair<QString, QString>> DaliNiraBlockerListManager::loadScriptingSourcesForHost(const QString &host,
                                                                                       const QStringList &enabledIds,
                                                                                       bool selectionConfigured,
                                                                                       bool includeGeneric) const {
  const QString baseDir = rulesetDir();
  if (baseDir.isEmpty()) return {};

  // Preflight the generated hostname/entity map so irrelevant large bundles
  // are not injected into every site. The selected program remains the source
  // of truth for argument references and exceptions.
  QStringList ids = enabledIds;
  if (!selectionConfigured && ids.isEmpty()) {
    ids = {QStringLiteral("ublock-filters"), QStringLiteral("easylist"),
           QStringLiteral("easyprivacy"), QStringLiteral("tur-0")};
  }
  const QString lowerHost = host.trimmed().toLower();
  const bool isYouTubeHost = lowerHost == QLatin1String("youtube.com") ||
                             lowerHost == QLatin1String("www.youtube.com") ||
                             lowerHost == QLatin1String("m.youtube.com") ||
                             lowerHost == QLatin1String("music.youtube.com") ||
                             lowerHost.endsWith(QLatin1String(".youtube.com")) ||
                             lowerHost == QLatin1String("youtu.be");
  if (isYouTubeHost && !ids.contains(QStringLiteral("ublock-experimental"))) {
    ids.append(QStringLiteral("ublock-experimental"));
  }

  QString mainSource;
  QString isolatedSource;
  auto readAsset = [this](const QString &path) {
    {
      QMutexLocker locker(&mutex_);
      if (const auto *found = scriptingSourceCache_.object(path)) return *found;
    }
    QFile file(path);
    QString source;
    if (file.open(QIODevice::ReadOnly | QIODevice::Text) && file.size() <= 8 * 1024 * 1024) source = QString::fromUtf8(file.read(8 * 1024 * 1024));
    QMutexLocker locker(&mutex_);
    scriptingSourceCache_.insert(path, new QString(source), qMax<qsizetype>(1, source.size() * 2));
    return source;
  };
  auto appendMappedAsset = [this, &readAsset, &host, isYouTubeHost](QString &target, const QString &path) {
    const QString source = readAsset(path);
    // YouTube fail-safe: Never inject scriptlets containing player reload/seek loops
    // (e.g. serverContract / loadVideoById / statsForNerds mutation observers) that destabilize playback.
    if (isYouTubeHost && (source.contains(QLatin1String("serverContract")) ||
                         source.contains(QLatin1String("loadVideoById")))) {
      return;
    }
    const QString cacheKey = path + QLatin1Char('\n') + host.toLower();
    bool applies = false;
    bool cached = false;
    {
      QMutexLocker locker(&mutex_);
      const auto found = scriptingApplicabilityCache_.constFind(cacheKey);
      if (found != scriptingApplicabilityCache_.constEnd()) {
        applies = found.value();
        cached = true;
      }
    }
    if (!cached) {
      applies = generatedScriptletAppliesToHost(source, host);
      QMutexLocker locker(&mutex_);
      if (scriptingApplicabilityCache_.size() >= 1024) scriptingApplicabilityCache_.clear();
      scriptingApplicabilityCache_.insert(cacheKey, applies);
    }
    if (applies) target += source + QStringLiteral("\n;\n");
  };
  auto appendGenericAsset = [&readAsset](QString &target, const QString &path) {
    const QString source = readAsset(path);
    if (!source.trimmed().isEmpty()) target += source + QStringLiteral("\n;\n");
  };
  for (const QString &id : ids) {
    if (!QRegularExpression(QStringLiteral("^[a-zA-Z0-9_-]{1,80}$")).match(id).hasMatch()) continue;
    appendMappedAsset(mainSource, baseDir + QStringLiteral("/scripting/scriptlet/main/") + id + QStringLiteral(".js"));
    appendMappedAsset(isolatedSource, baseDir + QStringLiteral("/scripting/scriptlet/isolated/") + id + QStringLiteral(".js"));
    if (includeGeneric)
      appendGenericAsset(isolatedSource, baseDir + QStringLiteral("/scripting/generic/") + id + QStringLiteral(".js"));
    // scripting/popup/<id>.js is data only (no bundled script reads it): its
    // $popup rules are loaded natively instead (generatedPopupRules).
  }

  QList<QPair<QString, QString>> result;
  if (!mainSource.trimmed().isEmpty()) result.append(qMakePair(QStringLiteral("main"), mainSource));
  if (!isolatedSource.trimmed().isEmpty()) result.append(qMakePair(QStringLiteral("isolated"), isolatedSource));
  return result;
}
