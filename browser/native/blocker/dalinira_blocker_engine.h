#pragma once

#include <QHash>
#include <QList>
#include <QMutex>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>
#include <array>

#include "dalinira_blocker_types.h"

class DaliNiraBlockerRedirectResources;

// What text filters (custom lines and downloaded lists) compile against: the
// packaged redirect resources, and the canonical keys of every $badfilter
// line of the active text sources, which disable the matching text rule in
// any of them. Both may be null.
struct DaliNiraBlockerTextCompileContext {
  const DaliNiraBlockerRedirectResources *resources = nullptr;
  const QSet<QString> *badfilters = nullptr;
};

struct CosmeticRule {
  QString domain; // Empty for generic rules
  QString selector;
  bool isException = false;
};

struct ProceduralCosmeticRule {
  QString domain;
  QString signature;
  QJsonObject rule;
  bool isException = false;
};

// Rules of one non-network scope (FilterRuleScope), sorted and indexed like
// the network rules.
struct IndexedRuleSet {
  QList<FilterRule> rules;
  std::array<QVector<int>, 14> byResource;
  QHash<QString, QVector<int>> byDomain;
  QHash<QString, QVector<int>> byToken;
  QVector<int> universal;
};

struct CompiledBlockerPlan {
  QList<FilterRule> rules;
  IndexedRuleSet popupRules;
  IndexedRuleSet genericHideRules;
  IndexedRuleSet urlSkipRules;
  QList<FilterRule> customRules;
  QList<CosmeticRule> cosmeticRules;
  QList<ProceduralCosmeticRule> proceduralRules;
  QString listCosmeticCss;
  std::array<QVector<int>, 14> ruleIndicesByResource;
  QHash<QString, QVector<int>> ruleIndicesByDomain;
  QHash<QString, QVector<int>> ruleIndicesByToken;
  QVector<int> universalRuleIndices;
};

using CompiledAdBlockPlan = CompiledBlockerPlan;

class DaliNiraBlockerEngine final {
 public:
  DaliNiraBlockerEngine();
  ~DaliNiraBlockerEngine() = default;

  void clearRules();
  void loadRules(const QList<FilterRule> &rules);
  void setListCosmeticCss(const QString &css);
  using TextCompileContext = DaliNiraBlockerTextCompileContext;

  void addCustomFilterLines(const QStringList &lines, const TextCompileContext &context = {});
  static CompiledBlockerPlan compilePlan(QList<FilterRule> rules, const QString &listCosmeticCss,
                                         const QStringList &customLines, const TextCompileContext &context = {});
  void applyCompiledPlan(CompiledBlockerPlan plan);

  RequestDecision evaluate(const QUrl &url, DaliNiraBlockerResourceType resourceType,
                           const QString &initiatorHost, DaliNiraBlockerMode mode,
                           const SitePolicy &policy,
                           const QString &requestMethod = QStringLiteral("get")) const;

  // New windows and tabs a page opens (window.open, target=_blank): a
  // matching $popup rule blocks, an @@...$popup exception allows, site
  // policy applies as for requests. `openerHost` is the opening document's.
  RequestDecision evaluatePopup(const QUrl &targetUrl, const QString &openerHost, const SitePolicy &policy) const;
  // A $generichide exception (or generated "unhide" hostname) applies to the
  // document: generic cosmetic filters are not used on it. Site-specific
  // cosmetics, procedural rules, scriptlets and network rules are unaffected.
  bool genericCosmeticsDisabled(const QUrl &documentUrl) const;
  // Debouncing: for a top-level navigation to a bounce-tracking redirector,
  // the destination its urlskip rule extracts (followed through at most three
  // redirectors in a row), or an empty URL. Matching rules are tried in
  // priority order until one yields a destination. When the navigation is
  // `blocked`, only a rule with the "-blocked" step skips it.
  // `rulesetId` receives the first matching rule's ruleset.
  QUrl urlSkipTarget(const QUrl &url, bool blocked, QString *rulesetId = nullptr) const;
  // uBO's urlskip steps applied to `url`: the http(s) destination, or empty
  // when a step does not apply or fails.
  static QString applyUrlSkipSteps(const QString &url, const QStringList &steps, bool blocked);

  // `includeGeneric` false leaves out the generic (not site-scoped) cosmetics.
  QString cosmeticCssForHost(const QString &host, bool includeGeneric = true) const;
  QString applyCosmeticExceptions(const QString &host, const QString &css) const;
  QJsonArray customProceduralRulesForHost(const QString &host, bool includeGeneric = true) const;
  int ruleCount() const;
  int customRuleCount() const;
  int popupRuleCount() const;
  int genericHideRuleCount() const;
  int urlSkipRuleCount() const;
  // Empty means valid; otherwise this is suitable for showing beside the
  // editor. $redirect names are checked against `resources` when given.
  static QString validateCustomFilterLine(const QString &line,
                                          const DaliNiraBlockerRedirectResources *resources = nullptr);

  // A downloaded ABP/uBO-syntax list compiled into network rules by the same
  // line compiler custom filters use, into the same rule model generated
  // rulesets load into ($redirect: packaged resource redirects;
  // $removeparam: query transforms). Lines it cannot represent (unsupported
  // options, `!#if` blocks) are skipped, which leaves requests allowed.
  // Element hiding and scriptlet lines are counted but not compiled: a list's
  // cosmetic data stays the bundled generated assets.
  struct TextFilterListCompilation {
    QList<FilterRule> rules;  // plain `||host^` lines are merged into requestDomains
    int networkRules = 0;     // network lines compiled (before merging)
    int cosmeticLines = 0;
    int genericCosmeticLines = 0;
    int siteCosmeticLines = 0;
    int proceduralCosmeticLines = 0;  // subtype of cosmeticLines; not compiled from downloaded text
    int scriptletCosmeticLines = 0;   // subtype of cosmeticLines; not compiled from downloaded text
    int unsupportedLines = 0;
    int commentLines = 0;
    int redirectRules = 0;
    int rewriteRules = 0;     // subtype of redirectRules
    int removeparamRules = 0;
    int modifierExceptions = 0;
    int blockRules = 0;
    int allowRules = 0;
    int typedNetworkRules = 0;
    int partyNetworkRules = 0;
    int domainNetworkRules = 0;
    int methodNetworkRules = 0;
    int regexNetworkRules = 0;
    int importantNetworkRules = 0;
    int badfilterLines = 0;
    int disabledByBadfilter = 0;
    int popupRules = 0;       // $popup lines (blocking)
    int popupExceptions = 0;  // @@...$popup lines
    int genericHideRules = 0; // @@...$generichide lines
    int urlSkipRules = 0;     // $urlskip lines
    QHash<QString, int> skippedByReason;  // option name or parse problem -> lines
  };
  static TextFilterListCompilation compileTextFilterList(QStringView text, const QString &rulesetId,
                                                         const TextCompileContext &context = {});
  // Canonical keys of the rules the $badfilter lines of `text` disable.
  static QSet<QString> badfilterKeys(QStringView text);
  // The site (registrable domain, eTLD+1) of a host; an IP address is its
  // own site.
  static QString siteForHost(const QString &host);

 private:
  struct MatchContext {
    const QString &urlString;
    const QString &urlLower;
    const QString &host;
    const QString &initiatorHost;
    const QString &method;
    DaliNiraBlockerResourceType resourceType;
  };
  // Every condition of `rule` (types, methods, party, domains, pattern)
  // holds for the request. The one matcher of every rule scope.
  bool ruleMatches(const FilterRule &rule, const MatchContext &context) const;
  QVector<quint8> candidateFlags(int ruleCount, const QHash<QString, QVector<int>> &byDomain,
                                 const QHash<QString, QVector<int>> &byToken, const QVector<int> &universal,
                                 const QString &host, const QString &urlLower) const;
  const FilterRule *firstMatch(const IndexedRuleSet &set, const QUrl &url, const QString &initiatorHost) const;
  bool domainMatches(const QString &host, const QString &ruleDomain) const;
  bool isSameSite(const QString &hostA, const QString &hostB) const;
  QString getSiteDomain(const QString &host) const;
  bool urlFilterMatches(const QString &urlFilter, const QString &url, const QString &host,
                        bool caseSensitive = false) const;

  mutable QMutex mutex_;
  QList<FilterRule> rules_;
  std::array<QVector<int>, 14> ruleIndicesByResource_;
  QHash<QString, QVector<int>> ruleIndicesByDomain_;
  QHash<QString, QVector<int>> ruleIndicesByToken_;
  QVector<int> universalRuleIndices_;
  QList<FilterRule> customRules_;
  QList<CosmeticRule> cosmeticRules_;
  QList<ProceduralCosmeticRule> proceduralRules_;
  QString listCosmeticCss_;
  IndexedRuleSet popupRules_;
  IndexedRuleSet genericHideRules_;
  IndexedRuleSet urlSkipRules_;
  mutable QHash<QString, QRegularExpression> regexCache_;
};

using AdBlockFilterEngine = DaliNiraBlockerEngine;
