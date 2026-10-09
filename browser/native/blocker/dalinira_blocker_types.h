#pragma once

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>

enum class DaliNiraBlockerMode {
  Basic = 0,     // Hafif filtreleme, site uyumluluğu yüksek
  Ideal = 1,     // Dengeli filtreleme (Varsayılan)
  Aggressive = 2 // Güçlü / Kapsamlı filtreleme, YouTube sinyalleri, görünür temizlik
};

using BlockerMode = DaliNiraBlockerMode;
using AdBlockMode = DaliNiraBlockerMode;

enum class DaliNiraBlockerResourceType {
  MainFrame,
  SubFrame,
  Stylesheet,
  Script,
  Image,
  Font,
  Object,
  Xhr,
  Ping,
  CspReport,
  Media,
  WebSocket,
  Favicon,
  Other
};

using BlockerResourceType = DaliNiraBlockerResourceType;
using AdBlockResourceType = DaliNiraBlockerResourceType;

enum class DaliNiraBlockerAction {
  Allow,
  Block,
  Redirect,
  ModifyHeaders
};

using BlockerAction = DaliNiraBlockerAction;
using AdBlockAction = DaliNiraBlockerAction;

struct SitePolicy {
  bool adBlocking = true;
  bool trackerProtection = true;
  bool whitelisted = false;
  qint64 temporaryDisabledUntil = 0;
  int perSiteMode = -1; // -1: global, 0: Basic (Standart), 1: Ideal (Standart), 2: Aggressive
  bool blockScripts = false;
  bool blockFingerprinting = false;
  bool upgradeHttps = false;
  bool forgetOnClose = false;
  QString cookiePolicy = QStringLiteral("third_party"); // "third_party", "allow_all", "block_all"
};

// The decision a rule takes part in. Network rules decide requests;
// popup rules ($popup, and generated popup data) decide new windows and tabs
// a page opens; generic-hide rules ($generichide exceptions, and generated
// "unhide" hostnames) decide per document whether generic cosmetic filters
// apply; urlskip rules ($urlskip, and generated urlskip data) take a top-level
// navigation straight past a bounce-tracking redirector. All use the same
// matching.
enum class FilterRuleScope : quint8 { Network = 0, Popup = 1, GenericHide = 2, UrlSkip = 3 };

struct FilterRule {
  qint64 id = 0;
  int priority = 1;
  QString actionType; // "allow", "block", "redirect", "modifyHeaders", "upgradeScheme"
  QString urlFilter;
  QString regexFilter;
  bool isCaseSensitive = false;
  QSet<QString> requestDomains;
  QSet<QString> excludedRequestDomains;
  QSet<QString> initiatorDomains;
  QSet<QString> excludedInitiatorDomains;
  QSet<DaliNiraBlockerResourceType> resourceTypes;
  QSet<DaliNiraBlockerResourceType> excludedResourceTypes;
  QSet<QString> requestMethods;
  QSet<QString> excludedRequestMethods;
  QString domainType; // "thirdParty", "firstParty", ""
  QString rulesetId;
  QString redirectUrl;
  QJsonObject redirectTransform;
  QString regexSubstitution;
  bool unsupportedHeaderCondition = false;
  // Packaged resource a "redirect" rule substitutes (redirectUrl holds its
  // data: URL); empty for built-in stubs and URL transforms.
  QString redirectResource;
  // "modifierException" rules decide nothing themselves: they cancel the
  // resource redirects ("redirect") or parameter removal ("removeparam") of
  // the request they match, for one resource / parameter or, when
  // exceptionValue is empty, all of them.
  QString exceptionScope;
  QString exceptionValue;
  FilterRuleScope scope = FilterRuleScope::Network;
  // "urlskip" rules: uBO's steps that extract the destination from the
  // navigation URL ("?url", "-base64", "/re(gex)/", ...).
  QStringList urlSkipSteps;
};

using BlockerRule = FilterRule;

struct RequestDecision {
  RequestDecision() = default;
  RequestDecision(DaliNiraBlockerAction action, QString reason, qint64 ruleId, QString rulesetId,
                  QString redirectUrl, QString redirectResource = {})
      : action(action), reason(std::move(reason)), ruleId(ruleId), rulesetId(std::move(rulesetId)),
        redirectUrl(std::move(redirectUrl)), redirectResource(std::move(redirectResource)) {}

  DaliNiraBlockerAction action = DaliNiraBlockerAction::Allow;
  QString reason;
  qint64 ruleId = 0;
  QString rulesetId;
  QString redirectUrl;
  // Packaged resource a redirect substitutes (redirectUrl is its data: URL).
  QString redirectResource;
};

using BlockerDecision = RequestDecision;

// Final outcome of a network request after tracking-parameter stripping and
// rule evaluation. Both engines apply exactly this: Chromium through its
// QWebEngineUrlRequestInterceptor, WebKit through its web process extension.
enum class NetworkVerdictKind { Allow, Block, Redirect };

struct NetworkRequestVerdict {
  NetworkVerdictKind kind = NetworkVerdictKind::Allow;
  QUrl redirectUrl;
  RequestDecision decision;  // decision.redirectResource names a packaged substitute
};

enum class DaliNiraBlockType {
  NetworkAd,
  Tracker,
  Cosmetic,
  Scriptlet
};

struct TabBlockerStats {
  quint64 blockedRequests = 0;
  quint64 allowedRequests = 0;
  quint64 redirectedRequests = 0;
  quint64 blockedAds = 0;
  quint64 blockedTrackers = 0;
  quint64 blockedCosmetics = 0;
  quint64 blockedScriptlets = 0;

  quint64 totalBlocked() const {
    return blockedRequests + blockedCosmetics + blockedScriptlets;
  }
};

using TabAdBlockStats = TabBlockerStats;
using TabDaliNiraBlockerStats = TabBlockerStats;

struct NetworkLogEntry {
  QDateTime timestamp;
  quint64 tabId = 0;
  QString siteHost; // Compatibility alias for requestHost.
  QString requestHost;
  QString initiatorHost;
  QString topLevelSite;
  QString requestMethod;
  QString resourceTypeStr;
  QString requestUrl;
  DaliNiraBlockerAction action = DaliNiraBlockerAction::Allow;
  QString reason;
  QString rulesetId;
  qint64 ruleId = 0;
};

inline QString modeToString(DaliNiraBlockerMode mode) {
  switch (mode) {
    case DaliNiraBlockerMode::Basic: return QStringLiteral("basic");
    case DaliNiraBlockerMode::Aggressive: return QStringLiteral("aggressive");
    case DaliNiraBlockerMode::Ideal:
    default: return QStringLiteral("ideal");
  }
}

inline DaliNiraBlockerMode modeFromString(const QString &str) {
  const QString lower = str.trimmed().toLower();
  if (lower == QLatin1String("basic")) return DaliNiraBlockerMode::Basic;
  if (lower == QLatin1String("aggressive") || lower == QLatin1String("kapsamli")) return DaliNiraBlockerMode::Aggressive;
  return DaliNiraBlockerMode::Ideal;
}

inline DaliNiraBlockerResourceType resourceTypeFromWebEngine(int type) {
  switch (type) {
    case 0: return DaliNiraBlockerResourceType::MainFrame;
    case 1: return DaliNiraBlockerResourceType::SubFrame;
    case 2: return DaliNiraBlockerResourceType::Stylesheet;
    case 3: return DaliNiraBlockerResourceType::Script;
    case 4: return DaliNiraBlockerResourceType::Image;
    case 5: return DaliNiraBlockerResourceType::Font;
    case 6: return DaliNiraBlockerResourceType::SubFrame;
    case 7: return DaliNiraBlockerResourceType::Object;
    case 8: return DaliNiraBlockerResourceType::Media;
    case 9: return DaliNiraBlockerResourceType::Other;
    case 10: return DaliNiraBlockerResourceType::Other;
    case 11: return DaliNiraBlockerResourceType::Other;
    case 12: return DaliNiraBlockerResourceType::Favicon;
    case 13: return DaliNiraBlockerResourceType::Xhr;
    case 14: return DaliNiraBlockerResourceType::Ping;
    case 15: return DaliNiraBlockerResourceType::Other;
    case 16: return DaliNiraBlockerResourceType::CspReport;
    case 17: return DaliNiraBlockerResourceType::Other;
    case 18: return DaliNiraBlockerResourceType::Other;
    case 19: return DaliNiraBlockerResourceType::MainFrame; // navigation preload
    case 20: return DaliNiraBlockerResourceType::SubFrame;  // navigation preload
    case 254: return DaliNiraBlockerResourceType::WebSocket;
    default: return DaliNiraBlockerResourceType::Other;
  }
}

// Maps a Fetch Metadata destination (the Sec-Fetch-Dest request header that
// WebKit sets before its web process extension sees a request) to the same
// resource types resourceTypeFromWebEngine() produces for Chromium. Workers
// map to Other, as QWebEngineUrlRequestInfo's worker types do.
inline DaliNiraBlockerResourceType resourceTypeFromFetchDestination(const QString &destination) {
  const QString d = destination.trimmed().toLower();
  if (d == QLatin1String("document")) return DaliNiraBlockerResourceType::MainFrame;
  if (d == QLatin1String("iframe") || d == QLatin1String("frame")) return DaliNiraBlockerResourceType::SubFrame;
  if (d == QLatin1String("style")) return DaliNiraBlockerResourceType::Stylesheet;
  if (d == QLatin1String("script")) return DaliNiraBlockerResourceType::Script;
  if (d == QLatin1String("image")) return DaliNiraBlockerResourceType::Image;
  if (d == QLatin1String("font")) return DaliNiraBlockerResourceType::Font;
  if (d == QLatin1String("object") || d == QLatin1String("embed")) return DaliNiraBlockerResourceType::Object;
  if (d == QLatin1String("audio") || d == QLatin1String("video") || d == QLatin1String("track"))
    return DaliNiraBlockerResourceType::Media;
  if (d == QLatin1String("empty")) return DaliNiraBlockerResourceType::Xhr;  // fetch(), XHR, sendBeacon()
  if (d == QLatin1String("report")) return DaliNiraBlockerResourceType::CspReport;
  return DaliNiraBlockerResourceType::Other;
}

inline QString resourceTypeToString(DaliNiraBlockerResourceType type) {
  switch (type) {
    case DaliNiraBlockerResourceType::MainFrame: return QStringLiteral("main_frame");
    case DaliNiraBlockerResourceType::SubFrame: return QStringLiteral("sub_frame");
    case DaliNiraBlockerResourceType::Stylesheet: return QStringLiteral("stylesheet");
    case DaliNiraBlockerResourceType::Script: return QStringLiteral("script");
    case DaliNiraBlockerResourceType::Image: return QStringLiteral("image");
    case DaliNiraBlockerResourceType::Font: return QStringLiteral("font");
    case DaliNiraBlockerResourceType::Object: return QStringLiteral("object");
    case DaliNiraBlockerResourceType::Xhr: return QStringLiteral("xmlhttprequest");
    case DaliNiraBlockerResourceType::Ping: return QStringLiteral("ping");
    case DaliNiraBlockerResourceType::CspReport: return QStringLiteral("csp_report");
    case DaliNiraBlockerResourceType::Media: return QStringLiteral("media");
    case DaliNiraBlockerResourceType::WebSocket: return QStringLiteral("websocket");
    case DaliNiraBlockerResourceType::Favicon: return QStringLiteral("favicon");
    case DaliNiraBlockerResourceType::Other:
    default: return QStringLiteral("other");
  }
}

inline DaliNiraBlockerResourceType resourceTypeFromString(const QString &str) {
  const QString s = str.trimmed().toLower();
  if (s == QLatin1String("main_frame") || s == QLatin1String("mainframe")) return DaliNiraBlockerResourceType::MainFrame;
  if (s == QLatin1String("sub_frame") || s == QLatin1String("subframe") || s == QLatin1String("subdocument") || s == QLatin1String("frame")) return DaliNiraBlockerResourceType::SubFrame;
  if (s == QLatin1String("stylesheet")) return DaliNiraBlockerResourceType::Stylesheet;
  if (s == QLatin1String("script")) return DaliNiraBlockerResourceType::Script;
  if (s == QLatin1String("image")) return DaliNiraBlockerResourceType::Image;
  if (s == QLatin1String("font")) return DaliNiraBlockerResourceType::Font;
  if (s == QLatin1String("object")) return DaliNiraBlockerResourceType::Object;
  if (s == QLatin1String("xmlhttprequest") || s == QLatin1String("xhr") || s == QLatin1String("fetch")) return DaliNiraBlockerResourceType::Xhr;
  if (s == QLatin1String("ping")) return DaliNiraBlockerResourceType::Ping;
  if (s == QLatin1String("csp_report")) return DaliNiraBlockerResourceType::CspReport;
  if (s == QLatin1String("media")) return DaliNiraBlockerResourceType::Media;
  if (s == QLatin1String("websocket")) return DaliNiraBlockerResourceType::WebSocket;
  if (s == QLatin1String("favicon")) return DaliNiraBlockerResourceType::Favicon;
  return DaliNiraBlockerResourceType::Other;
}
