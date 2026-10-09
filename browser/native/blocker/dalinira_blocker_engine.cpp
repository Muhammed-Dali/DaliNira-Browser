#include <libpsl.h>
#include <QHostAddress>
#include "dalinira_blocker_engine.h"
#include "dalinira_blocker_list_manager.h"
#include "dalinira_blocker_redirect_resources.h"

#include <algorithm>
#include <QDateTime>
#include <QMutexLocker>
#include <QStringDecoder>
#include <QUrlQuery>
#include "i18n/i18n.h"

namespace {
const QStringList kCommonTlds = {
    QStringLiteral("com.tr"), QStringLiteral("org.tr"), QStringLiteral("net.tr"), QStringLiteral("edu.tr"),
    QStringLiteral("gov.tr"), QStringLiteral("co.uk"), QStringLiteral("org.uk"), QStringLiteral("com.au"),
    QStringLiteral("co.jp"), QStringLiteral("co.kr")
};

bool isGoogleAuthBypass(const QUrl &url, const QString &initiatorHost) {
  auto isGoogleAuthHostOrPath = [](const QString &host, const QString &path) -> bool {
    if (host.isEmpty()) return false;
    if (host == QLatin1String("gstatic.com") || host.endsWith(QLatin1String(".gstatic.com")) ||
        host == QLatin1String("googleusercontent.com") || host.endsWith(QLatin1String(".googleusercontent.com")) ||
        host == QLatin1String("googleapis.com") || host.endsWith(QLatin1String(".googleapis.com"))) {
      return true;
    }
    if (host == QLatin1String("accounts.google.com") || host == QLatin1String("myaccount.google.com") ||
        host == QLatin1String("oauth2.googleapis.com") || host == QLatin1String("accounts.youtube.com")) {
      return true;
    }
    if ((host == QLatin1String("google.com") || host == QLatin1String("www.google.com")) &&
        (path.contains(QLatin1String("/signin")) || path.contains(QLatin1String("/servicelogin")) || path.contains(QLatin1String("/accountchooser")))) {
      return true;
    }
    return false;
  };

  const QString host = url.host().toLower();
  const QString path = url.path().toLower();
  if (isGoogleAuthHostOrPath(host, path)) return true;

  if (!initiatorHost.isEmpty() && isGoogleAuthHostOrPath(initiatorHost.toLower(), QString())) {
    return true;
  }
  return false;
}

struct PlatformCoreDomainEntry {
  QStringList page;
  QStringList assets;
};

// Source of Truth: DaliNira-WebMedia/main.js lines 4366-4469
const PlatformCoreDomainEntry kPlatformCoreDomains[] = {
    {{QStringLiteral("deezer.com")},
     {QStringLiteral("deezer.com"), QStringLiteral("dzcdn.net")}},
    {{QStringLiteral("soundcloud.com")},
     {QStringLiteral("soundcloud.com"), QStringLiteral("sndcdn.com")}},
    {{QStringLiteral("facebook.com"), QStringLiteral("instagram.com")},
     {QStringLiteral("facebook.com"), QStringLiteral("fbcdn.com"), QStringLiteral("fbcdn.net"), QStringLiteral("fbsbx.com"), QStringLiteral("facebook.net"), QStringLiteral("instagram.com"), QStringLiteral("cdninstagram.com")}},
    {{QStringLiteral("tiktok.com")},
     {QStringLiteral("tiktok.com"), QStringLiteral("tiktokcdn.com"), QStringLiteral("tiktokcdn-us.com"), QStringLiteral("tiktokv.com"), QStringLiteral("ttwstatic.com"), QStringLiteral("byteoversea.com"), QStringLiteral("ibyteimg.com"), QStringLiteral("byteimg.com"), QStringLiteral("ibytedtos.com"), QStringLiteral("muscdn.com")}},
    {{QStringLiteral("x.com"), QStringLiteral("twitter.com")},
     {QStringLiteral("x.com"), QStringLiteral("twitter.com"), QStringLiteral("twimg.com")}},
    {{QStringLiteral("reddit.com")},
     {QStringLiteral("reddit.com"), QStringLiteral("redditstatic.com"), QStringLiteral("redditmedia.com"), QStringLiteral("redd.it")}},
    {{QStringLiteral("twitch.tv")},
     {QStringLiteral("twitch.tv"), QStringLiteral("jtvnw.net"), QStringLiteral("ttvnw.net"), QStringLiteral("twitchcdn.net"), QStringLiteral("ext-twitch.tv")}},
    {{QStringLiteral("telegram.org"), QStringLiteral("t.me")},
     {QStringLiteral("telegram.org"), QStringLiteral("t.me")}},
    {{QStringLiteral("whatsapp.com")},
     {QStringLiteral("whatsapp.com"), QStringLiteral("whatsapp.net")}},
    {{QStringLiteral("mixcloud.com")},
     {QStringLiteral("mixcloud.com"), QStringLiteral("mxcdn.net")}},
    {{QStringLiteral("spotify.com")},
     {QStringLiteral("spotify.com"), QStringLiteral("scdn.co"), QStringLiteral("spotifycdn.com"), QStringLiteral("spotifycdn.net"), QStringLiteral("akamaized.net")}},
    {{QStringLiteral("amazon.com.tr"), QStringLiteral("amazon.com"), QStringLiteral("amazon.de"), QStringLiteral("amazon.co.uk"), QStringLiteral("amazon.fr"), QStringLiteral("amazon.it"), QStringLiteral("amazon.es")},
     {QStringLiteral("amazon.com.tr"), QStringLiteral("amazon.com"), QStringLiteral("amazon.de"), QStringLiteral("amazon.co.uk"), QStringLiteral("amazon.fr"), QStringLiteral("amazon.it"), QStringLiteral("amazon.es"), QStringLiteral("media-amazon.com"), QStringLiteral("ssl-images-amazon.com"), QStringLiteral("a2z.com")}},
    {{QStringLiteral("duckduckgo.com"), QStringLiteral("google.com"), QStringLiteral("bing.com"), QStringLiteral("brave.com"), QStringLiteral("github.com")},
     {QStringLiteral("duckduckgo.com"), QStringLiteral("google.com"), QStringLiteral("gstatic.com"), QStringLiteral("bing.com"), QStringLiteral("brave.com"), QStringLiteral("github.com"), QStringLiteral("githubassets.com")}}
};

bool hostMatchesAny(const QString &host, const QStringList &domains) {
  for (const QString &d : domains) {
    if (host == d || host.endsWith(QLatin1Char('.') + d)) return true;
  }
  return false;
}

bool cosmeticDomainApplies(const QString &host, const QString &domainExpression) {
  if (domainExpression.trimmed().isEmpty()) return true;
  bool hasPositive = false;
  bool positiveMatch = false;
  const QStringList domains = domainExpression.split(QLatin1Char(','), Qt::SkipEmptyParts);
  for (QString domain : domains) {
    domain = domain.trimmed().toLower();
    const bool excluded = domain.startsWith(QLatin1Char('~'));
    if (excluded) domain.remove(0, 1);
    const bool matches = host == domain || host.endsWith(QLatin1Char('.') + domain);
    if (excluded && matches) return false;
    if (!excluded) {
      hasPositive = true;
      if (matches) positiveMatch = true;
    }
  }
  return !hasPositive || positiveMatch;
}

bool isPlatformCoreAssetBypass(const QUrl &url, DaliNiraBlockerResourceType resourceType, const QString &initiatorHost) {
  if (initiatorHost.isEmpty() || resourceType == DaliNiraBlockerResourceType::MainFrame) return false;
  const QString targetHost = url.host().toLower();
  const QString initHost = initiatorHost.toLower();

  for (const auto &entry : kPlatformCoreDomains) {
    if (hostMatchesAny(initHost, entry.page) && hostMatchesAny(targetHost, entry.assets)) {
      return true;
    }
  }
  return false;
}

// Redirect / parameter-removal exceptions matched so far for one request.
struct ModifierExceptions {
  bool redirectAll = false;
  QSet<QString> redirectResources;
  bool removeparamAll = false;
  QSet<QString> removeparamNames;
};

bool isWebUrl(const QUrl &url) {
  const QString scheme = url.scheme().toLower();
  return url.isValid() && !url.host().isEmpty() &&
         (scheme == QLatin1String("http") || scheme == QLatin1String("https"));
}

// Removes query parameters in place. Only whole parameters are removed, by
// exact (percent-decoded) name or by `pattern` on the decoded "name=value";
// the parameters that stay, and every other part of the URL, keep exactly
// their original encoding.
bool removeQueryParameters(QUrl &url, bool all, const QSet<QString> &names, const QRegularExpression *pattern,
                           const QSet<QString> &kept) {
  if (!url.hasQuery()) return false;
  QStringList remaining;
  bool changed = false;
  const QStringList pairs = url.query(QUrl::FullyEncoded).split(QLatin1Char('&'));
  for (const QString &pair : pairs) {
    if (pair.isEmpty()) {
      remaining.append(pair);
      continue;
    }
    const int equals = pair.indexOf(QLatin1Char('='));
    const QString name = QUrl::fromPercentEncoding((equals < 0 ? pair : pair.left(equals)).toUtf8());
    bool remove = all || names.contains(name) ||
                  (pattern && pattern->match(QUrl::fromPercentEncoding(pair.toUtf8())).hasMatch());
    if (remove && kept.contains(name)) remove = false;
    if (remove) changed = true;
    else remaining.append(pair);
  }
  if (!changed) return false;
  while (!remaining.isEmpty() && remaining.constLast().isEmpty()) remaining.removeLast();
  url.setQuery(remaining.isEmpty() ? QString() : remaining.join(QLatin1Char('&')), QUrl::TolerantMode);
  return true;
}

// True for the transforms $removeparam compiles to (and the generated
// queryTransform/query rules), as opposed to host/path/scheme rewrites.
bool isParameterRemoval(const QJsonObject &transform) {
  for (auto it = transform.constBegin(); it != transform.constEnd(); ++it) {
    if (it.key() == QLatin1String("query")) {
      if (!it.value().toString().remove(QLatin1Char('?')).isEmpty()) return false;
    } else if (it.key() == QLatin1String("queryTransform")) {
      if (it.value().toObject().contains(QStringLiteral("addOrReplaceParams"))) return false;
    } else {
      return false;
    }
  }
  return !transform.isEmpty();
}

// The URL a transform rule sends the request to; empty when the rule does not
// apply. `removeParamPattern` is the rule's compiled removeparam regex.
QString transformedRedirectUrl(const FilterRule &rule, const QUrl &sourceUrl, const ModifierExceptions &exceptions,
                               const QRegularExpression *removeParamPattern) {
  if (!rule.redirectTransform.isEmpty()) {
    QUrl target = sourceUrl;
    const QJsonObject transform = rule.redirectTransform;
    if (transform.contains(QStringLiteral("scheme"))) target.setScheme(transform.value(QStringLiteral("scheme")).toString().remove(QLatin1Char(':')));
    if (transform.contains(QStringLiteral("host"))) target.setHost(transform.value(QStringLiteral("host")).toString());
    if (transform.contains(QStringLiteral("port"))) {
      const QJsonValue portValue = transform.value(QStringLiteral("port"));
      bool ok = false;
      const int port = portValue.isDouble() ? portValue.toInt(-1) : portValue.toString().toInt(&ok);
      target.setPort(portValue.isDouble() || ok ? port : -1);
    }
    if (transform.contains(QStringLiteral("path"))) {
      QString path = transform.value(QStringLiteral("path")).toString();
      if (!path.startsWith(QLatin1Char('/'))) path.prepend(QLatin1Char('/'));
      target.setPath(path);
    }
    // DNR: an empty "query" clears the query, which is removing every parameter.
    bool removeAll = false;
    if (transform.contains(QStringLiteral("query"))) {
      const QString query = transform.value(QStringLiteral("query")).toString().remove(QLatin1Char('?'));
      if (query.isEmpty()) removeAll = true;
      else target.setQuery(query);
    }
    const QJsonObject queryTransform = transform.value(QStringLiteral("queryTransform")).toObject();
    QSet<QString> removeNames;
    for (const QJsonValue &value : queryTransform.value(QStringLiteral("removeParams")).toArray())
      removeNames.insert(value.toString());
    if ((removeAll || !removeNames.isEmpty() || removeParamPattern) && !exceptions.removeparamAll)
      removeQueryParameters(target, removeAll, removeNames, removeParamPattern, exceptions.removeparamNames);
    const QJsonArray addOrReplace = queryTransform.value(QStringLiteral("addOrReplaceParams")).toArray();
    if (!addOrReplace.isEmpty()) {
      QUrlQuery query(target);
      for (const QJsonValue &value : addOrReplace) {
        const QJsonObject entry = value.toObject();
        const QString key = entry.value(QStringLiteral("key")).toString();
        if (key.isEmpty()) continue;
        if (entry.value(QStringLiteral("replaceOnly")).toBool() && !query.hasQueryItem(key)) continue;
        query.removeAllQueryItems(key);
        query.addQueryItem(key, entry.value(QStringLiteral("value")).toVariant().toString());
      }
      target.setQuery(query);
    }
    if (transform.contains(QStringLiteral("fragment"))) target.setFragment(transform.value(QStringLiteral("fragment")).toString().remove(QLatin1Char('#')));
    if (transform.contains(QStringLiteral("username"))) target.setUserName(transform.value(QStringLiteral("username")).toString());
    if (transform.contains(QStringLiteral("password"))) target.setPassword(transform.value(QStringLiteral("password")).toString());
    return target.toString(QUrl::FullyEncoded);
  }
  if (!rule.regexSubstitution.isEmpty() && !rule.regexFilter.isEmpty()) {
    const QRegularExpression expression(QStringLiteral("(*LIMIT_MATCH=100000)(*LIMIT_DEPTH=1000)") + rule.regexFilter,
        rule.isCaseSensitive ? QRegularExpression::NoPatternOption : QRegularExpression::CaseInsensitiveOption);
    if (expression.isValid()) {
      QString replacement = rule.regexSubstitution;
      QString result = sourceUrl.toString();
      result.replace(expression, replacement);
      if (result != sourceUrl.toString()) return result;
    }
  }
  return QString();
}
} // namespace

DaliNiraBlockerEngine::DaliNiraBlockerEngine() = default;

namespace {
int dnrActionRank(const QString &action) {
  if (action == QLatin1String("allow") || action == QLatin1String("allowAllRequests")) return 4;
  if (action == QLatin1String("block")) return 3;
  if (action == QLatin1String("redirect")) return 2;
  return 1;
}

void sortDnrRules(QList<FilterRule> &rules) {
  std::stable_sort(rules.begin(), rules.end(), [](const FilterRule &a, const FilterRule &b) {
    if (a.priority != b.priority) return a.priority > b.priority;
    const int rankA = dnrActionRank(a.actionType);
    const int rankB = dnrActionRank(b.actionType);
    if (rankA != rankB) return rankA > rankB;
    return a.id > b.id;
  });
}

std::array<QVector<int>, 14> buildResourceIndices(const QList<FilterRule> &rules) {
  std::array<QVector<int>, 14> result;
  for (int index = 0; index < rules.size(); ++index) {
    const FilterRule &rule = rules.at(index);
    for (int typeIndex = 0; typeIndex < static_cast<int>(result.size()); ++typeIndex) {
      const auto type = static_cast<DaliNiraBlockerResourceType>(typeIndex);
      if ((!rule.resourceTypes.isEmpty() && !rule.resourceTypes.contains(type)) ||
          rule.excludedResourceTypes.contains(type)) continue;
      result[static_cast<size_t>(typeIndex)].append(index);
    }
  }
  return result;
}

QString anchoredDomain(const QString &filter) {
  if (!filter.startsWith(QStringLiteral("||"))) return {};
  const QString rest = filter.mid(2);
  int end = 0;
  while (end < rest.size()) {
    const QChar c = rest.at(end);
    if (c == QLatin1Char('^') || c == QLatin1Char('/') ||
        c == QLatin1Char('*') || c == QLatin1Char('|')) break;
    ++end;
  }
  QString domain = rest.left(end).trimmed().toLower();
  while (domain.startsWith(QLatin1Char('.'))) domain.remove(0, 1);
  while (domain.endsWith(QLatin1Char('.'))) domain.chop(1);
  return domain;
}

QString requiredUrlToken(const QString &filter) {
  QString longest;
  QString current;
  for (const QChar c : filter) {
    if (c.isLetterOrNumber()) current.append(c.toLower());
    else {
      if (current.size() > longest.size()) longest = current;
      current.clear();
    }
  }
  if (current.size() > longest.size()) longest = current;
  return longest.size() >= 3 ? longest.left(3) : QString();
}

void buildCandidateIndices(const QList<FilterRule> &rules,
                           QHash<QString, QVector<int>> &byDomain,
                           QHash<QString, QVector<int>> &byToken,
                           QVector<int> &universal) {
  byDomain.clear();
  byToken.clear();
  universal.clear();
  for (int index = 0; index < rules.size(); ++index) {
    const FilterRule &rule = rules.at(index);
    if (!rule.requestDomains.isEmpty()) {
      for (const QString &domain : rule.requestDomains) byDomain[domain.toLower()].append(index);
      continue;
    }
    const QString domain = anchoredDomain(rule.urlFilter);
    if (!domain.isEmpty()) {
      byDomain[domain].append(index);
      continue;
    }
    const QString token = requiredUrlToken(rule.urlFilter);
    if (!token.isEmpty()) {
      byToken[token].append(index);
      continue;
    }
    universal.append(index);
  }
}

const QStringList &proceduralOperatorNames() {
  static const QStringList names = {
      QStringLiteral("matches-css-before"), QStringLiteral("matches-css-after"),
      QStringLiteral("min-text-length"), QStringLiteral("matches-attr"),
      QStringLiteral("matches-prop"), QStringLiteral("matches-path"),
      QStringLiteral("matches-media"), QStringLiteral("remove-attr"),
      QStringLiteral("remove-class"), QStringLiteral("has-text"),
      QStringLiteral("watch-attr"), QStringLiteral("matches-css"),
      QStringLiteral("if-not"), QStringLiteral("upward"),
      QStringLiteral("xpath"), QStringLiteral("shadow"),
      QStringLiteral("others"), QStringLiteral("remove"),
      QStringLiteral("style"), QStringLiteral("spath"),
      QStringLiteral("has"), QStringLiteral("not"), QStringLiteral("if")};
  return names;
}

int firstProceduralOperator(const QString &expression) {
  int first = -1;
  for (const QString &name : proceduralOperatorNames()) {
    const int position = expression.indexOf(QLatin1Char(':') + name + QLatin1Char('('));
    if (position >= 0 && (first < 0 || position < first)) first = position;
  }
  return first;
}

bool readProceduralCall(const QString &expression, int position, QString *name,
                        QString *argument, int *nextPosition) {
  if (position < 0 || position >= expression.size() || expression.at(position) != QLatin1Char(':')) return false;
  const int open = expression.indexOf(QLatin1Char('('), position + 1);
  if (open < 0) return false;
  const QString candidate = expression.mid(position + 1, open - position - 1);
  if (!proceduralOperatorNames().contains(candidate)) return false;
  int depth = 1;
  QChar quote;
  bool escaped = false;
  for (int index = open + 1; index < expression.size(); ++index) {
    const QChar ch = expression.at(index);
    if (escaped) { escaped = false; continue; }
    if (ch == QLatin1Char('\\')) { escaped = true; continue; }
    if (!quote.isNull()) {
      if (ch == quote) quote = QChar();
      continue;
    }
    if (ch == QLatin1Char('\'') || ch == QLatin1Char('"')) { quote = ch; continue; }
    if (ch == QLatin1Char('(')) ++depth;
    else if (ch == QLatin1Char(')') && --depth == 0) {
      *name = candidate;
      *argument = expression.mid(open + 1, index - open - 1).trimmed();
      *nextPosition = index + 1;
      return true;
    }
  }
  return false;
}

QString compileProceduralExpression(const QString &expression, QJsonObject *compiled) {
  const int first = firstProceduralOperator(expression);
  if (first < 0) return I18n::text(QStringLiteral("blocker.err_procedural_no_op"));
  const QString selector = expression.left(first).trimmed();
  if (selector.isEmpty()) return I18n::text(QStringLiteral("blocker.err_procedural_base_empty"));

  QJsonArray tasks;
  QJsonArray action{QStringLiteral("style"), QStringLiteral("display:none!important;")};
  bool hasAction = false;
  int position = first;
  while (position < expression.size()) {
    while (position < expression.size() && expression.at(position).isSpace()) ++position;
    if (position >= expression.size()) break;
    QString name;
    QString argument;
    int next = position;
    if (!readProceduralCall(expression, position, &name, &argument, &next))
      return I18n::text(QStringLiteral("blocker.err_procedural_invalid_op"));

    const bool actionOperator = name == QLatin1String("remove") || name == QLatin1String("style") ||
                                name == QLatin1String("remove-attr") || name == QLatin1String("remove-class");
    if (actionOperator) {
      if (hasAction) return I18n::text(QStringLiteral("blocker.err_procedural_one_action"));
      if (name == QLatin1String("remove")) action = QJsonArray{QStringLiteral("remove")};
      else action = QJsonArray{name, argument};
      hasAction = true;
    } else {
      if (hasAction) return I18n::text(QStringLiteral("blocker.err_procedural_action_last"));
      QJsonValue value(argument);
      if (name == QLatin1String("has") || name == QLatin1String("not") ||
          name == QLatin1String("if") || name == QLatin1String("if-not")) {
        value = QJsonObject{{QStringLiteral("selector"), argument}, {QStringLiteral("tasks"), QJsonArray{}}};
      } else if (name == QLatin1String("matches-css") || name == QLatin1String("matches-css-before") ||
                 name == QLatin1String("matches-css-after") || name == QLatin1String("matches-attr") ||
                 name == QLatin1String("matches-prop")) {
        const int separator = argument.indexOf(QLatin1Char('='));
        const int colon = argument.indexOf(QLatin1Char(':'));
        const int split = separator >= 0 ? separator : colon;
        if (split <= 0) return I18n::text(QStringLiteral("blocker.err_op_needs_pair")).arg(name);
        const QString key = (name == QLatin1String("matches-css") || name.startsWith(QStringLiteral("matches-css-")))
            ? QStringLiteral("name") : QStringLiteral("attr");
        value = QJsonObject{{key, argument.left(split).trimmed()},
                            {QStringLiteral("value"), argument.mid(split + 1).trimmed()}};
      } else if (name == QLatin1String("upward") || name == QLatin1String("min-text-length")) {
        bool ok = false;
        const int number = argument.toInt(&ok);
        if (ok) value = number;
      }
      tasks.append(QJsonArray{name, value});
    }
    position = next;
  }

  *compiled = QJsonObject{{QStringLiteral("selector"), selector},
                          {QStringLiteral("tasks"), tasks},
                          {QStringLiteral("action"), action}};
  return QString();
}

// ---------------- Network filter syntax ----------------
//
// One parser for custom filters and downloaded lists. It produces the same
// FilterRule model generated (DNR) rulesets load into, so every rule is
// decided by the one evaluator whatever its source:
//   $redirect=X / $rewrite=abp-resource:X  -> "redirect" + packaged resource
//   $removeparam[=name|=/regex/]           -> "redirect" + queryTransform
//   @@...$redirect[-rule] / @@...$removeparam -> "modifierException"
//   $method=                               -> requestMethods
//   $badfilter                             -> disables the same rule (key)
//   $urlskip=<steps>                       -> "urlskip" (debouncing, top-level navigations)

enum class NetworkLineError {
  None,
  PatternEmpty,
  RegexUnclosed,
  RegexInvalid,
  UnsupportedOption,
  InvalidOptionValue,
  UnknownRedirectResource,
  ResponseHeaderOption
};

// Text-rule priorities, the ones uBO Lite gives the generated rules: an
// exception beats an ordinary block, $important beats an exception, and a
// resource redirect ranks just above the block it replaces.
constexpr int kListRemoveParamPriority = 1;
constexpr int kListBlockPriority = 10;
constexpr int kListRedirectPriority = 11;
constexpr int kListAllowPriority = 30;
constexpr int kListImportantPriority = 40;
constexpr int kListImportantRedirectPriority = 41;
// Custom rules outrank every list rule, as the custom block/allow pass does.
constexpr int kCustomRemoveParamPriority = 2;
constexpr int kCustomRedirectPriority = 901;
// Modifier exceptions decide nothing; they only have to be seen first.
constexpr int kModifierExceptionPriority = 2000;

constexpr int kMaxRemoveParamPatternLength = 512;

// "/.../", optionally followed by "$options", is a regular expression; any
// other line starting with "/" is a path pattern ("/banner/ads.js$script").
// Returns the index of the closing slash, or -1.
int regexClosingSlash(const QString &body) {
  if (!body.startsWith(QLatin1Char('/')) || body.size() < 3) return -1;
  if (body.endsWith(QLatin1Char('/'))) return body.size() - 1;
  const int dollar = body.lastIndexOf(QLatin1Char('$'));
  return dollar > 2 && body.at(dollar - 1) == QLatin1Char('/') ? dollar - 1 : -1;
}

// Splits a network line (without "@@") into its pattern and option list.
void splitNetworkLine(const QString &body, QString *pattern, QStringList *options, bool *regex) {
  const int regexEnd = regexClosingSlash(body);
  const int optionIndex = regexEnd > 0 ? (regexEnd + 1 < body.size() ? regexEnd + 1 : -1)
                                       : body.lastIndexOf(QLatin1Char('$'));
  *options = optionIndex >= 0 ? body.mid(optionIndex + 1).split(QLatin1Char(','), Qt::SkipEmptyParts)
                              : QStringList{};
  const QString rest = optionIndex >= 0 ? body.left(optionIndex) : body;
  *regex = regexEnd > 0;
  *pattern = *regex ? rest.mid(1, rest.size() - 2) : rest;
}

const QHash<QString, DaliNiraBlockerResourceType> &resourceTypeOptions() {
  static const QHash<QString, DaliNiraBlockerResourceType> options = {
      {QStringLiteral("document"), DaliNiraBlockerResourceType::MainFrame},
      {QStringLiteral("subdocument"), DaliNiraBlockerResourceType::SubFrame},
      {QStringLiteral("stylesheet"), DaliNiraBlockerResourceType::Stylesheet},
      {QStringLiteral("script"), DaliNiraBlockerResourceType::Script},
      {QStringLiteral("image"), DaliNiraBlockerResourceType::Image},
      {QStringLiteral("font"), DaliNiraBlockerResourceType::Font},
      {QStringLiteral("object"), DaliNiraBlockerResourceType::Object},
      {QStringLiteral("media"), DaliNiraBlockerResourceType::Media},
      {QStringLiteral("xmlhttprequest"), DaliNiraBlockerResourceType::Xhr},
      {QStringLiteral("xhr"), DaliNiraBlockerResourceType::Xhr},
      {QStringLiteral("ping"), DaliNiraBlockerResourceType::Ping},
      {QStringLiteral("websocket"), DaliNiraBlockerResourceType::WebSocket},
      {QStringLiteral("other"), DaliNiraBlockerResourceType::Other}};
  return options;
}

// What a network line asks for, before it becomes a FilterRule.
struct NetworkLine {
  bool exception = false;
  QString pattern;
  bool regex = false;
  QSet<DaliNiraBlockerResourceType> types;
  QSet<DaliNiraBlockerResourceType> excludedTypes;
  QString domainType;
  bool matchCase = false;
  bool important = false;
  QSet<QString> domains;
  QSet<QString> excludedDomains;
  QSet<QString> methods;
  QSet<QString> excludedMethods;
  enum class Modifier { None, Redirect, RemoveParam } modifier = Modifier::None;
  bool rewrite = false;
  QString redirectResource;  // canonical packaged name; empty on an exception means all
  bool removeAll = false;
  QString removeName;
  QString removePattern;
  bool removePatternCaseInsensitive = false;
  bool badfilter = false;
  bool popup = false;        // $popup: (also) a rule for new windows/tabs
  bool generichide = false;  // @@...$generichide: a document exception
  QStringList urlSkip;       // $urlskip=<steps>: a top-level navigation skips to its destination
};

// uBO's urlskip step vocabulary: "?name" (query parameter), "&N" (Nth
// parameter name), "#" (fragment), "/regex/" (first capture group),
// "+https", "-base64", "-safebase64", "-uricomponent" and "-blocked".
bool validUrlSkipStep(const QString &step) {
  if (step.size() > 512) return false;
  if (step.size() > 1 && step.startsWith(QLatin1Char('?'))) return true;
  if (step.size() > 1 && step.startsWith(QLatin1Char('&'))) {
    bool ok = false;
    return step.mid(1).toInt(&ok) > 0 && ok;
  }
  if (step.size() > 2 && step.startsWith(QLatin1Char('/')) && step.endsWith(QLatin1Char('/')))
    return QRegularExpression(step.mid(1, step.size() - 2)).isValid();
  static const QSet<QString> kWords = {QStringLiteral("#"), QStringLiteral("+https"), QStringLiteral("-base64"),
                                       QStringLiteral("-safebase64"), QStringLiteral("-uricomponent"),
                                       QStringLiteral("-blocked")};
  return kWords.contains(step);
}

NetworkLineError parseNetworkLine(const QString &rawLine, const DaliNiraBlockerRedirectResources *resources,
                                  NetworkLine *out, QString *detail) {
  NetworkLine line;
  line.exception = rawLine.startsWith(QStringLiteral("@@"));
  const QString body = line.exception ? rawLine.mid(2) : rawLine;
  if (body.isEmpty()) return NetworkLineError::PatternEmpty;
  QStringList options;
  splitNetworkLine(body, &line.pattern, &options, &line.regex);
  if (line.regex) {
    const QRegularExpression expression(line.pattern);
    if (!expression.isValid()) {
      *detail = expression.errorString();
      return NetworkLineError::RegexInvalid;
    }
  } else if (!line.pattern.trimmed().isEmpty() && line.pattern.count(QLatin1Char('/')) == line.pattern.size()) {
    // "/" or "//" alone is an unfinished expression that would match every URL.
    return NetworkLineError::RegexUnclosed;
  }

  static const QSet<QString> kMethods = {QStringLiteral("connect"), QStringLiteral("delete"), QStringLiteral("get"),
                                         QStringLiteral("head"),    QStringLiteral("options"), QStringLiteral("patch"),
                                         QStringLiteral("post"),    QStringLiteral("put")};
  static const QRegularExpression kParameterName(QStringLiteral("^[^\\s&=#?]{1,256}$"));
  bool sawRedirect = false;
  for (const QString &rawOption : std::as_const(options)) {
    const QString option = rawOption.trimmed();
    const int equals = option.indexOf(QLatin1Char('='));
    QString name = (equals < 0 ? option : option.left(equals)).trimmed().toLower();
    const QString value = equals < 0 ? QString() : option.mid(equals + 1).trimmed();
    const bool negated = name.startsWith(QLatin1Char('~'));
    if (negated) name.remove(0, 1);
    auto invalid = [&]() {
      *detail = option.toLower();
      return NetworkLineError::InvalidOptionValue;
    };

    const auto type = resourceTypeOptions().constFind(name);
    if (type != resourceTypeOptions().constEnd() && equals < 0) {
      (negated ? line.excludedTypes : line.types).insert(type.value());
    } else if (name == QLatin1String("third-party") && equals < 0) {
      line.domainType = negated ? QStringLiteral("firstParty") : QStringLiteral("thirdParty");
    } else if (name == QLatin1String("match-case") && equals < 0) {
      line.matchCase = !negated;
    } else if (name == QLatin1String("important") && equals < 0) {
      line.important = true;
    } else if (name == QLatin1String("badfilter") && equals < 0 && !negated) {
      line.badfilter = true;
    } else if (name == QLatin1String("popup") && equals < 0 && !negated) {
      line.popup = true;
    } else if ((name == QLatin1String("generichide") || name == QLatin1String("ghide")) && equals < 0 && !negated) {
      line.generichide = true;
    } else if (name == QLatin1String("urlskip") && !negated) {
      // Space-separated steps, applied in order: "$urlskip=?url -base64".
      const QStringList steps = value.split(QLatin1Char(' '), Qt::SkipEmptyParts);
      if (line.exception || steps.isEmpty() || steps.size() > 16 ||
          !std::all_of(steps.cbegin(), steps.cend(), validUrlSkipStep))
        return invalid();
      line.urlSkip = steps;
    } else if (name == QLatin1String("domain") && !negated) {
      const QStringList domains = option.mid(equals + 1).split(QLatin1Char('|'), Qt::SkipEmptyParts);
      if (domains.isEmpty()) return invalid();
      for (QString domain : domains) {
        domain = domain.trimmed().toLower();
        const bool excluded = domain.startsWith(QLatin1Char('~'));
        if (excluded) domain.remove(0, 1);
        if (domain.isEmpty()) return invalid();
        (excluded ? line.excludedDomains : line.domains).insert(domain);
      }
    } else if (name == QLatin1String("method") && !negated) {
      const QStringList methods = value.toLower().split(QLatin1Char('|'), Qt::SkipEmptyParts);
      if (methods.isEmpty()) return invalid();
      for (QString method : methods) {
        const bool excluded = method.startsWith(QLatin1Char('~'));
        if (excluded) method.remove(0, 1);
        if (!kMethods.contains(method)) return invalid();
        (excluded ? line.excludedMethods : line.methods).insert(method);
      }
    } else if ((name == QLatin1String("redirect") || name == QLatin1String("redirect-rule") ||
                name == QLatin1String("rewrite")) && !negated) {
      if (sawRedirect || line.modifier != NetworkLine::Modifier::None) return invalid();
      sawRedirect = true;
      // $redirect-rule redirects only what another filter blocks; only its
      // exception form (cancel redirection) is in the lists DaliNira uses.
      if (name == QLatin1String("redirect-rule") && !line.exception) {
        *detail = name;
        return NetworkLineError::UnsupportedOption;
      }
      if (name == QLatin1String("rewrite") && !value.startsWith(QStringLiteral("abp-resource:"))) return invalid();
      line.rewrite = name == QLatin1String("rewrite");
      line.modifier = NetworkLine::Modifier::Redirect;
      if (value.isEmpty()) {
        if (!line.exception) return invalid();  // an exception without a name cancels every redirect
      } else {
        line.redirectResource = resources ? resources->canonicalName(value) : QString();
        if (line.redirectResource.isEmpty()) {
          *detail = value;
          return NetworkLineError::UnknownRedirectResource;
        }
      }
    } else if (name == QLatin1String("removeparam") && !negated) {
      if (line.modifier != NetworkLine::Modifier::None) return invalid();
      line.modifier = NetworkLine::Modifier::RemoveParam;
      if (value.isEmpty()) {
        line.removeAll = true;
      } else if (value.startsWith(QLatin1Char('/'))) {
        // /regex/ or /regex/i, tested against each "name=value" pair.
        const bool caseInsensitive = value.endsWith(QStringLiteral("/i"));
        const QString expression = value.mid(1, value.size() - (caseInsensitive ? 3 : 2));
        if (line.exception || value.size() < 3 || !(value.endsWith(QLatin1Char('/')) || caseInsensitive) ||
            expression.isEmpty() || expression.size() > kMaxRemoveParamPatternLength ||
            !QRegularExpression(expression).isValid())
          return invalid();
        line.removePattern = expression;
        line.removePatternCaseInsensitive = caseInsensitive;
      } else {
        // An exact name ("~name", keep-only, is not supported).
        if (!kParameterName.match(value).hasMatch() || value.startsWith(QLatin1Char('~'))) return invalid();
        line.removeName = value;
      }
    } else if (name == QLatin1String("csp") || name == QLatin1String("header") ||
               name == QLatin1String("permissions")) {
      // Neither engine lets the blocker read or change response headers.
      *detail = name;
      return NetworkLineError::ResponseHeaderOption;
    } else {
      *detail = option.toLower();
      return NetworkLineError::UnsupportedOption;
    }
  }
  // $generichide only exists as an exception, and like $popup it is not
  // combined with a request modifier.
  if (line.generichide && (!line.exception || line.popup || line.modifier != NetworkLine::Modifier::None)) {
    *detail = QStringLiteral("generichide");
    return NetworkLineError::InvalidOptionValue;
  }
  if (line.popup && line.modifier != NetworkLine::Modifier::None) {
    *detail = QStringLiteral("popup");
    return NetworkLineError::InvalidOptionValue;
  }
  if (!line.urlSkip.isEmpty() && (line.popup || line.generichide || line.modifier != NetworkLine::Modifier::None)) {
    *detail = QStringLiteral("urlskip");
    return NetworkLineError::InvalidOptionValue;
  }
  // A rule without a pattern applies to every URL: allowed for parameter
  // removal ("$removeparam=gclid") and when domain= scopes it to sites
  // ("$third-party,xmlhttprequest,domain=example.org"), never bare ("$script").
  if (!line.regex && line.pattern.trimmed().isEmpty() && line.domains.isEmpty() &&
      (line.modifier != NetworkLine::Modifier::RemoveParam || line.exception))
    return NetworkLineError::PatternEmpty;
  *out = line;
  return NetworkLineError::None;
}

// Fills `rule` (everything but id, priority and ruleset) from a parsed line.
// False when the rule cannot be built (its redirect resource is missing).
bool buildRule(const NetworkLine &line, const DaliNiraBlockerRedirectResources *resources, FilterRule &rule) {
  if (line.regex) rule.regexFilter = line.pattern;
  else rule.urlFilter = line.pattern;
  rule.isCaseSensitive = line.matchCase;
  rule.resourceTypes = line.types;
  rule.excludedResourceTypes = line.excludedTypes;
  rule.domainType = line.domainType;
  rule.initiatorDomains = line.domains;
  rule.excludedInitiatorDomains = line.excludedDomains;
  rule.requestMethods = line.methods;
  rule.excludedRequestMethods = line.excludedMethods;
  if (!line.urlSkip.isEmpty()) {
    rule.actionType = QStringLiteral("urlskip");
    rule.scope = FilterRuleScope::UrlSkip;
    rule.urlSkipSteps = line.urlSkip;
    return true;
  }
  if (line.modifier == NetworkLine::Modifier::Redirect) {
    if (line.exception) {
      rule.actionType = QStringLiteral("modifierException");
      rule.exceptionScope = QStringLiteral("redirect");
      rule.exceptionValue = line.redirectResource;
      return true;
    }
    rule.actionType = QStringLiteral("redirect");
    rule.redirectResource = line.redirectResource;
    rule.redirectUrl = resources ? resources->dataUrl(line.redirectResource) : QString();
    return !rule.redirectUrl.isEmpty();
  }
  if (line.modifier == NetworkLine::Modifier::RemoveParam) {
    if (line.exception) {
      rule.actionType = QStringLiteral("modifierException");
      rule.exceptionScope = QStringLiteral("removeparam");
      rule.exceptionValue = line.removeName;
      return true;
    }
    rule.actionType = QStringLiteral("redirect");
    // The generated rulesets' representation: DNR query / queryTransform.
    if (line.removeAll) {
      rule.redirectTransform = QJsonObject{{QStringLiteral("query"), QString()}};
    } else if (!line.removeName.isEmpty()) {
      rule.redirectTransform = QJsonObject{{QStringLiteral("queryTransform"),
                                            QJsonObject{{QStringLiteral("removeParams"), QJsonArray{line.removeName}}}}};
    } else {
      rule.redirectTransform = QJsonObject{{QStringLiteral("queryTransform"),
                                            QJsonObject{{QStringLiteral("removeParamPattern"), line.removePattern},
                                                        {QStringLiteral("removeParamPatternCaseInsensitive"),
                                                         line.removePatternCaseInsensitive}}}};
    }
    return true;
  }
  rule.actionType = line.exception ? QStringLiteral("allow") : QStringLiteral("block");
  if (line.generichide) rule.scope = FilterRuleScope::GenericHide;
  return true;
}

// A $popup line decides new windows; with resource types as well
// ("$popup,document") it is also that network rule. Returns the rules the
// line stands for (one or two).
QList<FilterRule> withPopupScope(const NetworkLine &line, const FilterRule &rule) {
  if (!line.popup) return {rule};
  FilterRule popupRule = rule;
  popupRule.scope = FilterRuleScope::Popup;
  popupRule.resourceTypes.clear();
  popupRule.excludedResourceTypes.clear();
  if (line.types.isEmpty()) return {popupRule};
  return {rule, popupRule};
}

void indexRuleSet(IndexedRuleSet &set) {
  sortDnrRules(set.rules);
  set.byResource = buildResourceIndices(set.rules);
  buildCandidateIndices(set.rules, set.byDomain, set.byToken, set.universal);
}

// Moves popup, generic-hide and urlskip rules out of `rules` into their
// indexed sets.
void partitionRuleScopes(QList<FilterRule> &rules, IndexedRuleSet &popupRules, IndexedRuleSet &genericHideRules,
                         IndexedRuleSet &urlSkipRules) {
  QList<FilterRule> network;
  network.reserve(rules.size());
  for (FilterRule &rule : rules) {
    if (rule.scope == FilterRuleScope::Popup) popupRules.rules.append(std::move(rule));
    else if (rule.scope == FilterRuleScope::GenericHide) genericHideRules.rules.append(std::move(rule));
    else if (rule.scope == FilterRuleScope::UrlSkip) urlSkipRules.rules.append(std::move(rule));
    else network.append(std::move(rule));
  }
  rules = std::move(network);
}

int listPriority(const FilterRule &rule, bool important) {
  if (rule.actionType == QLatin1String("modifierException")) return kModifierExceptionPriority;
  if (rule.actionType == QLatin1String("allow")) return kListAllowPriority;
  if (rule.actionType == QLatin1String("redirect")) {
    if (!rule.redirectTransform.isEmpty()) return kListRemoveParamPriority;
    return important ? kListImportantRedirectPriority : kListRedirectPriority;
  }
  return important ? kListImportantPriority : kListBlockPriority;
}

// Canonical form of a network line for $badfilter matching: the same rule
// written with its options in another order or letter case (and "xhr" for
// "xmlhttprequest") gets the same key; any other difference does not.
QString canonicalRuleKey(const QString &rawLine) {
  const bool exception = rawLine.startsWith(QStringLiteral("@@"));
  QString pattern;
  QStringList options;
  bool regex = false;
  splitNetworkLine(exception ? rawLine.mid(2) : rawLine, &pattern, &options, &regex);
  QStringList canonical;
  bool matchCase = false;
  for (const QString &raw : std::as_const(options)) {
    QString option = raw.trimmed();
    const int equals = option.indexOf(QLatin1Char('='));
    QString name = (equals < 0 ? option : option.left(equals)).trimmed().toLower();
    QString value = equals < 0 ? QString() : option.mid(equals + 1).trimmed();
    if (name == QLatin1String("badfilter")) continue;
    if (name == QLatin1String("match-case")) matchCase = true;
    if (name == QLatin1String("xhr")) name = QStringLiteral("xmlhttprequest");
    if (name == QLatin1String("~xhr")) name = QStringLiteral("~xmlhttprequest");
    if (name == QLatin1String("domain") || name == QLatin1String("method")) {
      QStringList values = value.toLower().split(QLatin1Char('|'), Qt::SkipEmptyParts);
      for (QString &entry : values) entry = entry.trimmed();
      values.sort();
      value = values.join(QLatin1Char('|'));
    }
    canonical.append(equals < 0 ? name : name + QLatin1Char('=') + value);
  }
  canonical.sort();
  canonical.removeDuplicates();
  if (!matchCase && !regex) pattern = pattern.toLower();
  if (regex) pattern = QLatin1Char('/') + pattern + QLatin1Char('/');
  return (exception ? QStringLiteral("@@") : QString()) + pattern +
         (canonical.isEmpty() ? QString() : QLatin1Char('$') + canonical.join(QLatin1Char(',')));
}

void compileCustomLines(const QStringList &lines, QList<FilterRule> &customRules,
                        QList<CosmeticRule> &cosmeticRules,
                        QList<ProceduralCosmeticRule> &proceduralRules,
                        QList<FilterRule> &modifierRules,
                        const DaliNiraBlockerEngine::TextCompileContext &context) {
  qint64 idCounter = 900000;
  for (const QString &raw : lines) {
    const QString line = raw.trimmed();
    if (line.isEmpty() || line.startsWith(QLatin1Char('!'))) continue;
    if (!DaliNiraBlockerEngine::validateCustomFilterLine(line, context.resources).isEmpty()) continue;
    int exceptionIndex = line.indexOf(QStringLiteral("#@?#"));
    int cosmeticIndex = line.indexOf(QStringLiteral("#?#"));
    int delimiterLength = exceptionIndex >= 0 ? 4 : 3;
    if (exceptionIndex < 0 && cosmeticIndex < 0) {
      exceptionIndex = line.indexOf(QStringLiteral("#@#"));
      cosmeticIndex = line.indexOf(QStringLiteral("##"));
      delimiterLength = exceptionIndex >= 0 ? 3 : 2;
    }
    if (exceptionIndex >= 0 || cosmeticIndex >= 0) {
      const bool exception = exceptionIndex >= 0;
      const int index = exception ? exceptionIndex : cosmeticIndex;
      const QString expression = line.mid(index + delimiterLength).trimmed();
      QJsonObject procedural;
      if (firstProceduralOperator(expression) >= 0 && compileProceduralExpression(expression, &procedural).isEmpty()) {
        proceduralRules.append(ProceduralCosmeticRule{line.left(index).trimmed().toLower(), expression,
                                                       procedural, exception});
        continue;
      }
      CosmeticRule rule;
      rule.domain = line.left(index).trimmed().toLower();
      rule.selector = expression;
      rule.isException = exception;
      if (!rule.selector.isEmpty()) cosmeticRules.append(rule);
      continue;
    }
    NetworkLine parsed;
    QString detail;
    if (parseNetworkLine(line, context.resources, &parsed, &detail) != NetworkLineError::None) continue;
    if (parsed.badfilter) continue;  // a key in context.badfilters, not a rule
    if (context.badfilters && !context.badfilters->isEmpty() && context.badfilters->contains(canonicalRuleKey(line)))
      continue;
    FilterRule built;
    built.rulesetId = QStringLiteral("custom-user");
    if (!buildRule(parsed, context.resources, built)) continue;
    for (FilterRule rule : withPopupScope(parsed, built)) {
      rule.id = ++idCounter;
      const bool blockOrAllow = rule.actionType == QLatin1String("allow") || rule.actionType == QLatin1String("block");
      if (blockOrAllow)
        rule.priority = (rule.actionType == QLatin1String("allow") ? 1000 : 900) + (parsed.important ? 100 : 0);
      if (blockOrAllow && rule.scope == FilterRuleScope::Network) {
        customRules.append(rule);
        continue;
      }
      // Redirects, parameter removal, their exceptions, and popup,
      // generic-hide and urlskip rules are decided by the indexed rule sets,
      // like the same rules from lists.
      if (rule.actionType == QLatin1String("modifierException")) rule.priority = kModifierExceptionPriority;
      else if (rule.actionType == QLatin1String("urlskip")) rule.priority = kCustomRedirectPriority;
      else if (rule.actionType == QLatin1String("redirect"))
        rule.priority = !rule.redirectTransform.isEmpty() ? kCustomRemoveParamPriority
                                                          : kCustomRedirectPriority + (parsed.important ? 100 : 0);
      modifierRules.append(rule);
    }
  }
}

// Element hiding, scriptlet and HTML filtering syntaxes (uBO, ABP, AdGuard).
bool isNonNetworkFilterLine(const QString &line) {
  static const char *const kMarkers[] = {"##", "#@#", "#?#", "#@?#", "#$#", "#@$#", "#%#", "#@%#", "$$", "$@$"};
  for (const char *marker : kMarkers)
    if (line.contains(QLatin1String(marker))) return true;
  return false;
}

// A `||host^` block/allow line with nothing but party/type options, which the
// engine matches exactly like the same host in requestDomains.
bool isPlainHostRule(const FilterRule &rule) {
  static const QRegularExpression kHostPattern(QStringLiteral("^\\|\\|[a-z0-9_.-]+\\^?$"));
  return (rule.actionType == QLatin1String("block") || rule.actionType == QLatin1String("allow")) &&
         rule.regexFilter.isEmpty() && !rule.isCaseSensitive && rule.initiatorDomains.isEmpty() &&
         rule.excludedInitiatorDomains.isEmpty() && rule.requestMethods.isEmpty() &&
         rule.excludedRequestMethods.isEmpty() && kHostPattern.match(rule.urlFilter).hasMatch();
}

QString resourceTypeSignature(const QSet<DaliNiraBlockerResourceType> &types) {
  QList<int> values;
  for (const auto type : types) values.append(static_cast<int>(type));
  std::sort(values.begin(), values.end());
  QString signature;
  for (const int value : values) signature += QString::number(value) + QLatin1Char(',');
  return signature;
}

QString skipReason(NetworkLineError error, const QString &detail) {
  switch (error) {
    case NetworkLineError::UnsupportedOption:
    case NetworkLineError::ResponseHeaderOption:
      return detail.section(QLatin1Char('='), 0, 0);
    case NetworkLineError::UnknownRedirectResource:
      return QStringLiteral("redirect-unknown-resource:") + detail.left(64);
    case NetworkLineError::InvalidOptionValue:
      return QStringLiteral("invalid-value:") + detail.section(QLatin1Char('='), 0, 0).left(32);
    case NetworkLineError::RegexInvalid:
      return QStringLiteral("invalid-regex");
    case NetworkLineError::PatternEmpty:
    case NetworkLineError::RegexUnclosed:
      return QStringLiteral("invalid-pattern");
    case NetworkLineError::None:
      break;
  }
  return {};
}
}  // namespace

QSet<QString> DaliNiraBlockerEngine::badfilterKeys(QStringView text) {
  QSet<QString> keys;
  qsizetype start = 0;
  while (start < text.size()) {
    qsizetype end = text.indexOf(QLatin1Char('\n'), start);
    if (end < 0) end = text.size();
    const QStringView view = text.mid(start, end - start).trimmed();
    start = end + 1;
    if (view.isEmpty() || view.startsWith(QLatin1Char('!')) || !view.contains(QLatin1String("badfilter"), Qt::CaseInsensitive))
      continue;
    const QString line = view.toString();
    if (isNonNetworkFilterLine(line) || line.size() > 8192) continue;
    QString pattern;
    QStringList options;
    bool regex = false;
    splitNetworkLine(line.startsWith(QStringLiteral("@@")) ? line.mid(2) : line, &pattern, &options, &regex);
    const bool badfilter = std::any_of(options.cbegin(), options.cend(), [](const QString &option) {
      return option.trimmed().compare(QLatin1String("badfilter"), Qt::CaseInsensitive) == 0;
    });
    // The key only has to equal the rule's; a line naming no pattern disables nothing.
    if (badfilter && !pattern.trimmed().isEmpty()) keys.insert(canonicalRuleKey(line));
  }
  return keys;
}

DaliNiraBlockerEngine::TextFilterListCompilation DaliNiraBlockerEngine::compileTextFilterList(
    QStringView text, const QString &rulesetId, const TextCompileContext &context) {
  // Bounds the linear requestDomains scan in evaluate().
  constexpr int kMaxDomainsPerRule = 1000;
  static const QRegularExpression kControlCharacters(QStringLiteral("[\\x00-\\x08\\x0B\\x0C\\x0E-\\x1F]"));

  TextFilterListCompilation result;
  const bool checkBadfilters = context.badfilters && !context.badfilters->isEmpty();
  qint64 idCounter = 0;
  int conditionalDepth = 0;
  QHash<QString, int> hostGroups;  // signature -> index of the open group rule in result.rules
  auto skip = [&result](const QString &reason) {
    ++result.unsupportedLines;
    ++result.skippedByReason[reason];
  };
  qsizetype start = 0;
  while (start < text.size()) {
    qsizetype end = text.indexOf(QLatin1Char('\n'), start);
    if (end < 0) end = text.size();
    const QString line = text.mid(start, end - start).trimmed().toString();
    start = end + 1;
    if (line.isEmpty()) continue;
    if (line.startsWith(QLatin1Char('!'))) {
      // Preprocessor blocks target other environments; skip them entirely
      // rather than guess their condition.
      if (line.startsWith(QStringLiteral("!#if"))) ++conditionalDepth;
      else if (line.startsWith(QStringLiteral("!#endif")) && conditionalDepth > 0) --conditionalDepth;
      ++result.commentLines;
      continue;
    }
    if (conditionalDepth > 0) {
      skip(QStringLiteral("!#if-block"));
      continue;
    }
    if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))) {
      ++result.commentLines;  // "[Adblock Plus 2.0]" header
      continue;
    }
    if (isNonNetworkFilterLine(line)) {
      ++result.cosmeticLines;
      static const QStringList kCosmeticMarkers = {QStringLiteral("#@?#"), QStringLiteral("#?#"),
                                                    QStringLiteral("#@#"),  QStringLiteral("##"),
                                                    QStringLiteral("#@$#"), QStringLiteral("#$#"),
                                                    QStringLiteral("#@%#"), QStringLiteral("#%#"),
                                                    QStringLiteral("$@$"),  QStringLiteral("$$")};
      int marker = -1;
      for (const QString &candidate : kCosmeticMarkers) {
        const int found = line.indexOf(candidate);
        if (found >= 0 && (marker < 0 || found < marker)) marker = found;
      }
      ++(marker == 0 ? result.genericCosmeticLines : result.siteCosmeticLines);
      if (line.contains(QStringLiteral("#?#")) || line.contains(QStringLiteral("#@?#")) ||
          line.contains(QStringLiteral(":has(")) || line.contains(QStringLiteral(":has-text(")))
        ++result.proceduralCosmeticLines;
      if (line.contains(QStringLiteral("+js("))) ++result.scriptletCosmeticLines;
      continue;
    }
    if (line.size() > 8192 || line.contains(kControlCharacters)) {
      skip(QStringLiteral("invalid-pattern"));
      continue;
    }
    NetworkLine parsed;
    QString detail;
    const NetworkLineError error = parseNetworkLine(line, context.resources, &parsed, &detail);
    if (error != NetworkLineError::None) {
      skip(skipReason(error, detail));
      continue;
    }
    if (parsed.badfilter) {
      ++result.badfilterLines;
      continue;
    }
    if (checkBadfilters && context.badfilters->contains(canonicalRuleKey(line))) {
      ++result.disabledByBadfilter;
      continue;
    }
    FilterRule built;
    built.rulesetId = rulesetId;
    if (!buildRule(parsed, context.resources, built)) {
      skip(QStringLiteral("redirect-missing-resource:") + parsed.redirectResource);
      continue;
    }
    // Popup and generic-hide lines are compiled and enforced by their own
    // indexed realms. Do not inflate the network-rule metric with them. A
    // typed popup line (for example $popup,document) also emits a genuine
    // network rule and therefore counts once in both categories.
    const bool hasNetworkScope = built.scope != FilterRuleScope::GenericHide &&
                                 (!parsed.popup || !parsed.types.isEmpty());
    if (hasNetworkScope) {
      if (built.actionType == QLatin1String("block")) ++result.blockRules;
      else if (built.actionType == QLatin1String("allow")) ++result.allowRules;
      if (!parsed.types.isEmpty() || !parsed.excludedTypes.isEmpty()) ++result.typedNetworkRules;
      if (!parsed.domainType.isEmpty()) ++result.partyNetworkRules;
      if (!parsed.domains.isEmpty() || !parsed.excludedDomains.isEmpty()) ++result.domainNetworkRules;
      if (!parsed.methods.isEmpty() || !parsed.excludedMethods.isEmpty()) ++result.methodNetworkRules;
      if (parsed.regex) ++result.regexNetworkRules;
      if (parsed.important) ++result.importantNetworkRules;
    }
    if (built.scope == FilterRuleScope::GenericHide) {
      ++result.genericHideRules;
    } else if (built.scope == FilterRuleScope::UrlSkip) {
      ++result.urlSkipRules;
    } else if (parsed.popup) {
      ++(parsed.exception ? result.popupExceptions : result.popupRules);
      if (!parsed.types.isEmpty()) ++result.networkRules;
    } else {
      ++result.networkRules;
      if (built.actionType == QLatin1String("modifierException")) ++result.modifierExceptions;
      else if (built.actionType == QLatin1String("redirect")) {
        ++(built.redirectTransform.isEmpty() ? result.redirectRules : result.removeparamRules);
        if (parsed.rewrite) ++result.rewriteRules;
      }
    }

    for (FilterRule rule : withPopupScope(parsed, built)) {
      rule.priority = listPriority(rule, parsed.important);
      if (isPlainHostRule(rule)) {
        QString host = rule.urlFilter.mid(2);
        if (host.endsWith(QLatin1Char('^'))) host.chop(1);
        const QString signature = QString::number(static_cast<int>(rule.scope)) + QLatin1Char('|') + rule.actionType +
                                  QLatin1Char('|') + QString::number(rule.priority) + QLatin1Char('|') + rule.domainType +
                                  QLatin1Char('|') + resourceTypeSignature(rule.resourceTypes) + QLatin1Char('|') +
                                  resourceTypeSignature(rule.excludedResourceTypes);
        auto group = hostGroups.find(signature);
        if (group == hostGroups.end() || result.rules.at(group.value()).requestDomains.size() >= kMaxDomainsPerRule) {
          rule.id = ++idCounter;
          rule.urlFilter.clear();
          rule.requestDomains.insert(host);
          result.rules.append(rule);
          hostGroups.insert(signature, result.rules.size() - 1);
        } else {
          result.rules[group.value()].requestDomains.insert(host);
        }
        continue;
      }
      rule.id = ++idCounter;
      result.rules.append(rule);
    }
  }
  return result;
}

CompiledBlockerPlan DaliNiraBlockerEngine::compilePlan(QList<FilterRule> rules, const QString &listCosmeticCss,
                                                       const QStringList &customLines,
                                                       const TextCompileContext &context) {
  CompiledBlockerPlan plan;
  plan.rules = std::move(rules);
  compileCustomLines(customLines, plan.customRules, plan.cosmeticRules, plan.proceduralRules, plan.rules, context);
  partitionRuleScopes(plan.rules, plan.popupRules, plan.genericHideRules, plan.urlSkipRules);
  indexRuleSet(plan.popupRules);
  indexRuleSet(plan.genericHideRules);
  indexRuleSet(plan.urlSkipRules);
  sortDnrRules(plan.rules);
  plan.ruleIndicesByResource = buildResourceIndices(plan.rules);
  buildCandidateIndices(plan.rules, plan.ruleIndicesByDomain,
                        plan.ruleIndicesByToken, plan.universalRuleIndices);
  plan.listCosmeticCss = listCosmeticCss;
  return plan;
}

void DaliNiraBlockerEngine::applyCompiledPlan(CompiledBlockerPlan plan) {
  QMutexLocker locker(&mutex_);
  rules_.swap(plan.rules);
  ruleIndicesByResource_.swap(plan.ruleIndicesByResource);
  ruleIndicesByDomain_.swap(plan.ruleIndicesByDomain);
  ruleIndicesByToken_.swap(plan.ruleIndicesByToken);
  universalRuleIndices_.swap(plan.universalRuleIndices);
  customRules_.swap(plan.customRules);
  cosmeticRules_.swap(plan.cosmeticRules);
  proceduralRules_.swap(plan.proceduralRules);
  listCosmeticCss_.swap(plan.listCosmeticCss);
  std::swap(popupRules_, plan.popupRules);
  std::swap(genericHideRules_, plan.genericHideRules);
  std::swap(urlSkipRules_, plan.urlSkipRules);
  regexCache_.clear();
}

void DaliNiraBlockerEngine::clearRules() {
  QMutexLocker locker(&mutex_);
  rules_.clear();
  for (auto &indices : ruleIndicesByResource_) indices.clear();
  ruleIndicesByDomain_.clear();
  ruleIndicesByToken_.clear();
  universalRuleIndices_.clear();
  customRules_.clear();
  cosmeticRules_.clear();
  proceduralRules_.clear();
  listCosmeticCss_.clear();
  popupRules_ = {};
  genericHideRules_ = {};
  urlSkipRules_ = {};
  regexCache_.clear();
}

void DaliNiraBlockerEngine::setListCosmeticCss(const QString &css) {
  QMutexLocker locker(&mutex_);
  listCosmeticCss_ = css;
}

void DaliNiraBlockerEngine::loadRules(const QList<FilterRule> &rules) {
  QMutexLocker locker(&mutex_);
  rules_ = rules;
  sortDnrRules(rules_);
  ruleIndicesByResource_ = buildResourceIndices(rules_);
  buildCandidateIndices(rules_, ruleIndicesByDomain_, ruleIndicesByToken_, universalRuleIndices_);
}

void DaliNiraBlockerEngine::addCustomFilterLines(const QStringList &lines, const TextCompileContext &context) {
  QMutexLocker locker(&mutex_);
  customRules_.clear();
  cosmeticRules_.clear();
  proceduralRules_.clear();
  // Custom redirects, parameter removal, popup, generic-hide and urlskip rules
  // live in the indexed rule sets.
  auto isCustom = [](const FilterRule &rule) { return rule.rulesetId == QLatin1String("custom-user"); };
  rules_.removeIf(isCustom);
  popupRules_.rules.removeIf(isCustom);
  genericHideRules_.rules.removeIf(isCustom);
  urlSkipRules_.rules.removeIf(isCustom);
  QList<FilterRule> compiled;
  compileCustomLines(lines, customRules_, cosmeticRules_, proceduralRules_, compiled, context);
  partitionRuleScopes(compiled, popupRules_, genericHideRules_, urlSkipRules_);
  rules_.append(compiled);
  indexRuleSet(popupRules_);
  indexRuleSet(genericHideRules_);
  indexRuleSet(urlSkipRules_);
  sortDnrRules(rules_);
  ruleIndicesByResource_ = buildResourceIndices(rules_);
  buildCandidateIndices(rules_, ruleIndicesByDomain_, ruleIndicesByToken_, universalRuleIndices_);
  regexCache_.clear();
}

int DaliNiraBlockerEngine::ruleCount() const {
  QMutexLocker locker(&mutex_);
  return rules_.size() + customRules_.size();
}

int DaliNiraBlockerEngine::popupRuleCount() const {
  QMutexLocker locker(&mutex_);
  return popupRules_.rules.size();
}

int DaliNiraBlockerEngine::genericHideRuleCount() const {
  QMutexLocker locker(&mutex_);
  return genericHideRules_.rules.size();
}

int DaliNiraBlockerEngine::urlSkipRuleCount() const {
  QMutexLocker locker(&mutex_);
  return urlSkipRules_.rules.size();
}

int DaliNiraBlockerEngine::customRuleCount() const {
  QMutexLocker locker(&mutex_);
  return customRules_.size() + cosmeticRules_.size() + proceduralRules_.size();
}

QString DaliNiraBlockerEngine::validateCustomFilterLine(const QString &rawLine,
                                                        const DaliNiraBlockerRedirectResources *resources) {
  QString line = rawLine.trimmed();
  if (line.isEmpty() || line.startsWith(QLatin1Char('!'))) return QString();
  if (line.size() > 8192 || line.contains(QRegularExpression(QStringLiteral("[\\x00-\\x08\\x0B\\x0C\\x0E-\\x1F]"))))
    return I18n::text(QStringLiteral("blocker.err_control_chars"));

  int exceptionIndex = line.indexOf(QStringLiteral("#@?#"));
  int cosmeticIndex = line.indexOf(QStringLiteral("#?#"));
  int delimiterLength = exceptionIndex >= 0 ? 4 : 3;
  if (exceptionIndex < 0 && cosmeticIndex < 0) {
    exceptionIndex = line.indexOf(QStringLiteral("#@#"));
    cosmeticIndex = line.indexOf(QStringLiteral("##"));
    delimiterLength = exceptionIndex >= 0 ? 3 : 2;
  }
  if (exceptionIndex >= 0 || cosmeticIndex >= 0) {
    const bool exception = exceptionIndex >= 0;
    const int index = exception ? exceptionIndex : cosmeticIndex;
    const QString expression = line.mid(index + delimiterLength).trimmed();
    if (expression.contains(QLatin1Char('{')) || expression.contains(QLatin1Char('}')) ||
        expression.contains(QStringLiteral("url("), Qt::CaseInsensitive) || expression.contains(QStringLiteral("@import"), Qt::CaseInsensitive))
      return I18n::text(QStringLiteral("blocker.err_cosmetic_unsafe"));
    if (expression.isEmpty())
      return I18n::text(QStringLiteral("blocker.err_cosmetic_empty"));
    if (firstProceduralOperator(expression) >= 0) {
      QJsonObject compiled;
      return compileProceduralExpression(expression, &compiled);
    }
    if (delimiterLength > 3 || (delimiterLength == 3 && !exception))
      return I18n::text(QStringLiteral("blocker.err_procedural_filter_no_op"));
    return QString();
  }

  QString detail;
  NetworkLine parsed;
  switch (parseNetworkLine(line, resources, &parsed, &detail)) {
    case NetworkLineError::None: return QString();
    case NetworkLineError::PatternEmpty: return I18n::text(QStringLiteral("blocker.err_network_pattern_empty"));
    case NetworkLineError::RegexUnclosed: return I18n::text(QStringLiteral("blocker.err_regex_unclosed"));
    case NetworkLineError::RegexInvalid: return I18n::text(QStringLiteral("blocker.err_regex_invalid")).arg(detail);
    case NetworkLineError::UnsupportedOption:
      return I18n::text(QStringLiteral("blocker.err_unsupported_option")).arg(detail);
    case NetworkLineError::InvalidOptionValue:
      return I18n::text(QStringLiteral("blocker.err_option_value_invalid")).arg(detail);
    case NetworkLineError::UnknownRedirectResource:
      return I18n::text(QStringLiteral("blocker.err_redirect_resource_unknown")).arg(detail);
    case NetworkLineError::ResponseHeaderOption:
      return I18n::text(QStringLiteral("blocker.err_option_needs_response_headers")).arg(detail);
  }
  return QString();
}

bool DaliNiraBlockerEngine::domainMatches(const QString &host, const QString &ruleDomain) const {
  if (host.isEmpty() || ruleDomain.isEmpty()) return false;
  return host == ruleDomain || host.endsWith(QLatin1Char('.') + ruleDomain);
}

QString DaliNiraBlockerEngine::getSiteDomain(const QString &host) const { return siteForHost(host); }

QString DaliNiraBlockerEngine::siteForHost(const QString &host) {
  QString clean = host.trimmed().toLower();
  if (clean.endsWith(QLatin1Char('.'))) clean.chop(1);
  if (!QHostAddress(clean).isNull()) return clean;
  const QByteArray ace = QUrl::toAce(clean);
  const char *registrable = psl_registrable_domain(psl_builtin(), ace.constData());
  return registrable ? QString::fromUtf8(registrable) : clean;
}

bool DaliNiraBlockerEngine::isSameSite(const QString &hostA, const QString &hostB) const {
  if (hostA.isEmpty() || hostB.isEmpty()) return false;
  return getSiteDomain(hostA) == getSiteDomain(hostB);
}

bool DaliNiraBlockerEngine::urlFilterMatches(const QString &rawFilter, const QString &url, const QString &host,
                                           bool caseSensitive) const {
  QString filter = rawFilter.trimmed();
  if (!caseSensitive) filter = filter.toLower();
  const QString cacheKey = QStringLiteral("url:%1:%2")
                               .arg(caseSensitive ? QLatin1Char('s') : QLatin1Char('i'))
                               .arg(filter);
  if (filter.isEmpty()) return true;

  if (filter.startsWith(QStringLiteral("||"))) {
    const QString rest = filter.mid(2);
    int markerIndex = -1;
    for (int i = 0; i < rest.size(); ++i) {
      const QChar c = rest.at(i);
      if (c == QLatin1Char('^') || c == QLatin1Char('/') ||
          c == QLatin1Char('*') || c == QLatin1Char('|')) {
        markerIndex = i;
        break;
      }
    }
    QString domain = (markerIndex == -1 ? rest : rest.left(markerIndex)).toLower();
    while (domain.startsWith(QLatin1Char('.'))) domain.remove(0, 1);
    while (domain.endsWith(QLatin1Char('.'))) domain.chop(1);
    if (!domain.isEmpty() && !domainMatches(host, domain)) return false;
    const QString suffix = markerIndex == -1 ? QString() : rest.mid(markerIndex);
    if (suffix.isEmpty() || suffix == QLatin1String("^") || suffix == QLatin1String("|")) return true;
    QString nextFilter = suffix;
    if (nextFilter.startsWith(QLatin1Char('^'))) nextFilter.remove(0, 1);
    return urlFilterMatches(nextFilter, url, host, caseSensitive);
  }

  bool anchoredStart = false;
  bool anchoredEnd = false;
  if (filter.startsWith(QLatin1Char('|'))) {
    anchoredStart = true;
    filter.remove(0, 1);
  }
  if (filter.endsWith(QLatin1Char('|'))) {
    anchoredEnd = true;
    filter.chop(1);
  }

  if (!filter.contains(QLatin1Char('*')) && !filter.contains(QLatin1Char('^'))) {
    if (anchoredStart && anchoredEnd) return url == filter;
    if (anchoredStart) return url.startsWith(filter);
    if (anchoredEnd) return url.endsWith(filter);
    return url.contains(filter);
  }

  if (regexCache_.contains(cacheKey)) {
    return regexCache_.value(cacheKey).match(url).hasMatch();
  }

  QString escaped;
  for (int i = 0; i < filter.length(); ++i) {
    const QChar c = filter.at(i);
    if (c == QLatin1Char('*')) escaped.append(QStringLiteral(".*"));
    else if (c == QLatin1Char('^')) escaped.append(QStringLiteral("(?:[^A-Za-z0-9_.%-]|$)"));
    else escaped.append(QRegularExpression::escape(QString(c)));
  }
  QRegularExpression regex((anchoredStart ? QStringLiteral("^") : QString()) + escaped + (anchoredEnd ? QStringLiteral("$") : QString()),
                           caseSensitive ? QRegularExpression::NoPatternOption : QRegularExpression::CaseInsensitiveOption);
  if (regexCache_.size() < 2000) {
    regexCache_.insert(cacheKey, regex);
  }
  return regex.match(url).hasMatch();
}

RequestDecision DaliNiraBlockerEngine::evaluate(const QUrl &url, DaliNiraBlockerResourceType resourceType,
                                              const QString &initiatorHost, DaliNiraBlockerMode mode,
                                              const SitePolicy &policy,
                                              const QString &requestMethod) const {
  if (!url.isValid()) return RequestDecision{DaliNiraBlockerAction::Allow, QStringLiteral("invalid-url"), 0, QString(), QString()};
  const QString scheme = url.scheme().toLower();
  if (scheme != QLatin1String("http") && scheme != QLatin1String("https")) {
    return RequestDecision{DaliNiraBlockerAction::Allow, QStringLiteral("internal-scheme"), 0, QString(), QString()};
  }

  // Full site protection and temporary bypasses disable every filtering layer.
  // Ad and tracker toggles remain independent below at matched-rule time.
  if (policy.whitelisted || policy.temporaryDisabledUntil > QDateTime::currentMSecsSinceEpoch()) {
    return RequestDecision{DaliNiraBlockerAction::Allow, QStringLiteral("site-whitelisted"), 0, QString(), QString()};
  }
  if (!policy.adBlocking && !policy.trackerProtection) {
    return RequestDecision{DaliNiraBlockerAction::Allow, QStringLiteral("site-filtering-disabled"), 0, QString(), QString()};
  }

  // Google Auth bypass
  if (isGoogleAuthBypass(url, initiatorHost)) {
    return RequestDecision{DaliNiraBlockerAction::Allow, QStringLiteral("google-auth-bypass"), 0, QString(), QString()};
  }

  // Platform Core Asset bypass (Source of Truth: DaliNira-WebMedia isPlatformCoreAssetBypassRequest)
  if (isPlatformCoreAssetBypass(url, resourceType, initiatorHost)) {
    return RequestDecision{DaliNiraBlockerAction::Allow, QStringLiteral("platform-core-asset-bypass"), 0, QString(), QString()};
  }

  // Search engines subresource bypass (Source of Truth: DaliNira-WebMedia lines 5197-5201)
  if (resourceType != DaliNiraBlockerResourceType::MainFrame && !initiatorHost.isEmpty()) {
    const QString targetHost = url.host().toLower();
    const QString initHost = initiatorHost.toLower();
    if (((initHost == QLatin1String("google.com") || initHost.endsWith(QLatin1String(".google.com"))) || (initHost == QLatin1String("duckduckgo.com") || initHost.endsWith(QLatin1String(".duckduckgo.com"))) ||
         (initHost == QLatin1String("bing.com") || initHost.endsWith(QLatin1String(".bing.com"))) || (initHost == QLatin1String("brave.com") || initHost.endsWith(QLatin1String(".brave.com")))) &&
        ((targetHost == QLatin1String("google.com") || targetHost.endsWith(QLatin1String(".google.com"))) || (targetHost == QLatin1String("duckduckgo.com") || targetHost.endsWith(QLatin1String(".duckduckgo.com"))) ||
         (targetHost == QLatin1String("bing.com") || targetHost.endsWith(QLatin1String(".bing.com"))) || (targetHost == QLatin1String("brave.com") || targetHost.endsWith(QLatin1String(".brave.com"))))) {
      return RequestDecision{DaliNiraBlockerAction::Allow, QStringLiteral("search-engine-subresource-allow"), 0, QString(), QString()};
    }
  }

  // Basic Mode Policy: Do not block main frame navigation in basic mode
  if (mode == DaliNiraBlockerMode::Basic && resourceType == DaliNiraBlockerResourceType::MainFrame) {
    return RequestDecision{DaliNiraBlockerAction::Allow, QStringLiteral("basic-mode-mainframe-pass"), 0, QString(), QString()};
  }

  QMutexLocker locker(&mutex_);
  const QString urlString = url.toString();
  const QString urlLower = urlString.toLower();
  const QString host = url.host().toLower();
  const QString method = requestMethod.trimmed().toLower();

  // 1. Evaluate Custom User Rules (Allow rules first to override block rules)
  auto customMatches = [&](const FilterRule &rule) {
    if (!rule.resourceTypes.isEmpty() && !rule.resourceTypes.contains(resourceType)) return false;
    if (rule.excludedResourceTypes.contains(resourceType)) return false;
    if (!rule.requestMethods.isEmpty() && !rule.requestMethods.contains(method)) return false;
    if (rule.excludedRequestMethods.contains(method)) return false;
    if (!rule.domainType.isEmpty() && !initiatorHost.isEmpty()) {
      const bool same = isSameSite(host, initiatorHost);
      if (rule.domainType == QLatin1String("thirdParty") && same) return false;
      if (rule.domainType == QLatin1String("firstParty") && !same) return false;
    }
    if (!rule.initiatorDomains.isEmpty()) {
      bool included = false;
      for (const QString &domain : rule.initiatorDomains) {
        if (domainMatches(initiatorHost, domain)) { included = true; break; }
      }
      if (!included) return false;
    }
    for (const QString &domain : rule.excludedInitiatorDomains)
      if (domainMatches(initiatorHost, domain)) return false;
    const QString &candidateUrl = rule.isCaseSensitive ? urlString : urlLower;
    if (!rule.urlFilter.isEmpty() &&
        !urlFilterMatches(rule.urlFilter, candidateUrl, host, rule.isCaseSensitive)) return false;
    if (!rule.regexFilter.isEmpty()) {
      const QRegularExpression expression(QStringLiteral("(*LIMIT_MATCH=100000)(*LIMIT_DEPTH=1000)") + rule.regexFilter,
          rule.isCaseSensitive ? QRegularExpression::NoPatternOption : QRegularExpression::CaseInsensitiveOption);
      if (!expression.match(candidateUrl).hasMatch()) return false;
    }
    return true;
  };
  for (const auto &rule : customRules_) {
    if (rule.actionType == QLatin1String("allow") && customMatches(rule)) {
      return RequestDecision{DaliNiraBlockerAction::Allow, QStringLiteral("user-allow"), rule.id, rule.rulesetId, QString()};
    }
  }
  for (const auto &rule : customRules_) {
    if (rule.actionType == QLatin1String("block") && customMatches(rule)) {
      if (!policy.adBlocking) return RequestDecision{DaliNiraBlockerAction::Allow, QStringLiteral("ad-blocking-off"), 0, QString(), QString()};
      return RequestDecision{DaliNiraBlockerAction::Block, QStringLiteral("user-block"), rule.id, rule.rulesetId, QString()};
    }
  }

  // 2. Evaluate Engine Sorted DNR Rules (Highest rank / priority rule matches first)
  const QVector<quint8> hostAndUrlCandidates = candidateFlags(
      rules_.size(), ruleIndicesByDomain_, ruleIndicesByToken_, universalRuleIndices_, host, urlLower);
  const MatchContext context{urlString, urlLower, host, initiatorHost, method, resourceType};

  // YouTube Playback Fail-Safe: Never block genuine user video streams on googlevideo.com
  // even if a generic/downstream rule misclassifies them. Only genuine ad streams
  // (ctier=l, adformat, source=web_video_ads) or explicit ad endpoints should be blocked.
  auto blockDecision = [&](const FilterRule &rule, const QString &reason) {
    if (host.endsWith(QLatin1String("googlevideo.com")) &&
        (resourceType == DaliNiraBlockerResourceType::Media || resourceType == DaliNiraBlockerResourceType::Xhr)) {
      static const QRegularExpression kAdStreamParamRx(
          QStringLiteral(R"((?:[?&]ctier=l(?:&|$)|[?&]adformat=|[?&]source=web_video_ads(?:&|$)))"),
          QRegularExpression::CaseInsensitiveOption);
      if (!kAdStreamParamRx.match(urlString).hasMatch()) {
        return RequestDecision{DaliNiraBlockerAction::Allow, QStringLiteral("youtube-playback-failsafe"), 1000011, QStringLiteral("dalinira-youtube-core"), QString()};
      }
    }
    return RequestDecision{DaliNiraBlockerAction::Block, reason, rule.id, rule.rulesetId, QString()};
  };
  ModifierExceptions exceptions;
  const auto &candidateIndices = ruleIndicesByResource_.at(static_cast<size_t>(resourceType));
  auto isTrackerRule = [](const FilterRule &rule) {
    return rule.rulesetId.contains(QStringLiteral("privacy"), Qt::CaseInsensitive) ||
           rule.rulesetId.contains(QStringLiteral("tracker"), Qt::CaseInsensitive);
  };
  // A $removeparam rule's compiled pattern; false when it cannot compile.
  auto removeParamPatternFor = [this](const FilterRule &rule, const QRegularExpression **pattern) {
    *pattern = nullptr;
    const QJsonObject queryTransform = rule.redirectTransform.value(QStringLiteral("queryTransform")).toObject();
    const QString patternSource = queryTransform.value(QStringLiteral("removeParamPattern")).toString();
    if (patternSource.isEmpty()) return true;
    const bool caseInsensitive = queryTransform.value(QStringLiteral("removeParamPatternCaseInsensitive")).toBool();
    const QString key = QStringLiteral("removeparam:%1:%2").arg(caseInsensitive ? QLatin1Char('i') : QLatin1Char('s'))
                            .arg(patternSource);
    auto cached = regexCache_.find(key);
    if (cached == regexCache_.end()) {
      cached = regexCache_.insert(key, QRegularExpression(
          QStringLiteral("(*LIMIT_MATCH=100000)(*LIMIT_DEPTH=1000)") + patternSource,
          caseInsensitive ? QRegularExpression::CaseInsensitiveOption : QRegularExpression::NoPatternOption));
    }
    if (!cached->isValid()) return false;
    *pattern = &cached.value();
    return true;
  };
  for (qsizetype position = 0; position < candidateIndices.size(); ++position) {
    const int ruleIndex = candidateIndices.at(position);
    if (!hostAndUrlCandidates.at(ruleIndex)) continue;
    const FilterRule &rule = rules_.at(ruleIndex);
    const bool trackerRule = isTrackerRule(rule);
    if (!ruleMatches(rule, context)) continue;

    // Rule matches! Top priority decision
    if (rule.actionType == QLatin1String("modifierException")) {
      // Sorted ahead of every redirect: record what it cancels and go on.
      const bool all = rule.exceptionValue.isEmpty();
      if (rule.exceptionScope == QLatin1String("redirect")) {
        if (all) exceptions.redirectAll = true;
        else exceptions.redirectResources.insert(rule.exceptionValue);
      } else if (rule.exceptionScope == QLatin1String("removeparam")) {
        if (all) exceptions.removeparamAll = true;
        else exceptions.removeparamNames.insert(rule.exceptionValue);
      }
      continue;
    }
    if (rule.actionType == QLatin1String("allow") || rule.actionType == QLatin1String("allowAllRequests")) {
      return RequestDecision{DaliNiraBlockerAction::Allow, QStringLiteral("dnr-allow"), rule.id, rule.rulesetId, QString()};
    }
    if ((trackerRule && !policy.trackerProtection) || (!trackerRule && !policy.adBlocking)) continue;
    if (rule.actionType == QLatin1String("block")) return blockDecision(rule, QStringLiteral("dnr-block"));
    if (rule.actionType == QLatin1String("redirect")) {
      if (!rule.redirectUrl.isEmpty()) {
        // A packaged resource (data:) or built-in stub replaces a request
        // the rule would otherwise block. A redirect exception keeps the
        // block and only drops the substitution.
        const bool packaged = rule.redirectUrl.startsWith(QStringLiteral("data:"));
        if (!packaged && !isWebUrl(QUrl(rule.redirectUrl))) continue;
        if (exceptions.redirectAll ||
            (!rule.redirectResource.isEmpty() && exceptions.redirectResources.contains(rule.redirectResource)))
          return blockDecision(rule, QStringLiteral("redirect-exception-block"));
        return RequestDecision{DaliNiraBlockerAction::Redirect,
                               packaged ? QStringLiteral("dnr-redirect-resource") : QStringLiteral("dnr-redirect"),
                               rule.id, rule.rulesetId, rule.redirectUrl, rule.redirectResource};
      }
      const QRegularExpression *removeParamPattern = nullptr;
      if (!removeParamPatternFor(rule, &removeParamPattern)) continue;
      QString redirectUrl = transformedRedirectUrl(rule, url, exceptions, removeParamPattern);
      // Fail open when the rule does not apply: nothing to change (which also
      // keeps a transform from redirecting a request to itself), or a target
      // that is not a web URL.
      const QUrl target(redirectUrl);
      if (redirectUrl.isEmpty() || !isWebUrl(target) ||
          target.toString(QUrl::FullyEncoded) == url.toString(QUrl::FullyEncoded))
        continue;
      if (!isParameterRemoval(rule.redirectTransform))
        return RequestDecision{DaliNiraBlockerAction::Redirect, QStringLiteral("dnr-transform"), rule.id, rule.rulesetId,
                               redirectUrl};
      // Parameter removal happens in one pass: every other $removeparam rule
      // that matches the request removes its parameters from the same
      // target, so a URL with many tracking parameters is cleaned by a single
      // redirect (the rest of the sorted rules can only be other removals;
      // modifier exceptions were all seen first).
      QUrl cleaned = target;
      for (qsizetype next = position + 1; next < candidateIndices.size(); ++next) {
        const int otherIndex = candidateIndices.at(next);
        if (!hostAndUrlCandidates.at(otherIndex)) continue;
        const FilterRule &other = rules_.at(otherIndex);
        if (other.actionType != QLatin1String("redirect") || !other.redirectUrl.isEmpty() ||
            !isParameterRemoval(other.redirectTransform) || !ruleMatches(other, context))
          continue;
        if ((isTrackerRule(other) && !policy.trackerProtection) || (!isTrackerRule(other) && !policy.adBlocking)) continue;
        const QRegularExpression *otherPattern = nullptr;
        if (!removeParamPatternFor(other, &otherPattern)) continue;
        const QString further = transformedRedirectUrl(other, cleaned, exceptions, otherPattern);
        if (!further.isEmpty() && isWebUrl(QUrl(further))) cleaned = QUrl(further);
      }
      redirectUrl = cleaned.toString(QUrl::FullyEncoded);
      return RequestDecision{DaliNiraBlockerAction::Redirect, QStringLiteral("dnr-remove-param"), rule.id,
                             rule.rulesetId, redirectUrl};
    }
    if (rule.actionType == QLatin1String("upgradeScheme") && url.scheme().compare(QStringLiteral("http"), Qt::CaseInsensitive) == 0) {
      QUrl upgraded = url;
      upgraded.setScheme(QStringLiteral("https"));
      return RequestDecision{DaliNiraBlockerAction::Redirect, QStringLiteral("dnr-upgrade-scheme"), rule.id, rule.rulesetId, upgraded.toString()};
    }
  }

  return RequestDecision{DaliNiraBlockerAction::Allow, QStringLiteral("no-rule-matched"), 0, QString(), QString()};
}

QVector<quint8> DaliNiraBlockerEngine::candidateFlags(int ruleCount, const QHash<QString, QVector<int>> &byDomain,
                                                       const QHash<QString, QVector<int>> &byToken,
                                                       const QVector<int> &universal, const QString &host,
                                                       const QString &urlLower) const {
  QVector<quint8> flags(ruleCount, 0);
  for (const int index : universal) flags[index] = 1;
  QString domainVariant = host;
  while (!domainVariant.isEmpty()) {
    const auto found = byDomain.constFind(domainVariant);
    if (found != byDomain.constEnd())
      for (const int index : found.value()) flags[index] = 1;
    const int dot = domainVariant.indexOf(QLatin1Char('.'));
    if (dot < 0) break;
    domainVariant.remove(0, dot + 1);
  }
  QSet<QString> urlTokens;
  for (int i = 0; i + 2 < urlLower.size(); ++i) urlTokens.insert(urlLower.mid(i, 3));
  for (const QString &token : urlTokens) {
    const auto found = byToken.constFind(token);
    if (found != byToken.constEnd())
      for (const int index : found.value()) flags[index] = 1;
  }
  return flags;
}

bool DaliNiraBlockerEngine::ruleMatches(const FilterRule &rule, const MatchContext &context) const {
  const QString &host = context.host;
  const QString &initiatorHost = context.initiatorHost;
  const QString &urlForRule = rule.isCaseSensitive ? context.urlString : context.urlLower;
  if (rule.unsupportedHeaderCondition) return false;
  if (!rule.resourceTypes.isEmpty() && !rule.resourceTypes.contains(context.resourceType)) return false;
  if (rule.excludedResourceTypes.contains(context.resourceType)) return false;
  if (!rule.requestMethods.isEmpty() && !rule.requestMethods.contains(context.method)) return false;
  if (rule.excludedRequestMethods.contains(context.method)) return false;

  // First party / third party.
  if (!rule.domainType.isEmpty() && !initiatorHost.isEmpty()) {
    const bool same = isSameSite(host, initiatorHost);
    if (rule.domainType == QLatin1String("thirdParty") && same) return false;
    if (rule.domainType == QLatin1String("firstParty") && !same) return false;
  }

  // Initiator domains.
  if (!rule.initiatorDomains.isEmpty()) {
    bool matchedInit = false;
    for (const QString &d : rule.initiatorDomains) {
      if (domainMatches(initiatorHost, d)) { matchedInit = true; break; }
    }
    if (!matchedInit) return false;
  }
  for (const QString &d : rule.excludedInitiatorDomains)
    if (domainMatches(initiatorHost, d)) return false;

  // Request domains.
  if (!rule.requestDomains.isEmpty()) {
    bool matched = false;
    for (const QString &d : rule.requestDomains) {
      if (domainMatches(host, d)) { matched = true; break; }
    }
    if (!matched) return false;
  }
  for (const QString &d : rule.excludedRequestDomains)
    if (domainMatches(host, d)) return false;

  if (!rule.urlFilter.isEmpty() && !urlFilterMatches(rule.urlFilter, urlForRule, host, rule.isCaseSensitive)) return false;

  if (!rule.regexFilter.isEmpty()) {
    const QString regexKey = QStringLiteral("regex:%1:%2")
                                 .arg(rule.isCaseSensitive ? QLatin1Char('s') : QLatin1Char('i'))
                                 .arg(rule.regexFilter);
    if (!regexCache_.contains(regexKey)) {
      regexCache_.insert(regexKey, QRegularExpression(QStringLiteral("(*LIMIT_MATCH=100000)(*LIMIT_DEPTH=1000)") + rule.regexFilter,
          rule.isCaseSensitive ? QRegularExpression::NoPatternOption : QRegularExpression::CaseInsensitiveOption));
    }
    if (!regexCache_.value(regexKey).match(urlForRule).hasMatch()) return false;
  }
  return true;
}

// Rules without resource types sit in every type index; documents (the popup
// target, the page) are MainFrame.
const FilterRule *DaliNiraBlockerEngine::firstMatch(const IndexedRuleSet &set, const QUrl &url,
                                                    const QString &initiatorHost) const {
  if (set.rules.isEmpty()) return nullptr;
  const QString urlString = url.toString();
  const QString urlLower = urlString.toLower();
  const QString host = url.host().toLower();
  const QString method = QStringLiteral("get");
  const MatchContext context{urlString, urlLower, host, initiatorHost, method, DaliNiraBlockerResourceType::MainFrame};
  const QVector<quint8> flags = candidateFlags(set.rules.size(), set.byDomain, set.byToken, set.universal, host, urlLower);
  for (const int index : set.byResource.at(static_cast<size_t>(DaliNiraBlockerResourceType::MainFrame))) {
    if (!flags.at(index)) continue;
    const FilterRule &rule = set.rules.at(index);
    if (ruleMatches(rule, context)) return &rule;
  }
  return nullptr;
}

RequestDecision DaliNiraBlockerEngine::evaluatePopup(const QUrl &targetUrl, const QString &openerHost,
                                                     const SitePolicy &policy) const {
  const QString scheme = targetUrl.scheme().toLower();
  if (!targetUrl.isValid() || (scheme != QLatin1String("http") && scheme != QLatin1String("https")))
    return RequestDecision{DaliNiraBlockerAction::Allow, QStringLiteral("internal-scheme"), 0, QString(), QString()};
  if (policy.whitelisted || policy.temporaryDisabledUntil > QDateTime::currentMSecsSinceEpoch() || !policy.adBlocking)
    return RequestDecision{DaliNiraBlockerAction::Allow, QStringLiteral("site-whitelisted"), 0, QString(), QString()};
  QMutexLocker locker(&mutex_);
  // Sorted: an exception (priority 30) is met before an ordinary $popup
  // block (10), an $important block (40) before the exception.
  if (const FilterRule *rule = firstMatch(popupRules_, targetUrl, openerHost.toLower())) {
    if (rule->actionType == QLatin1String("allow"))
      return RequestDecision{DaliNiraBlockerAction::Allow, QStringLiteral("popup-allow"), rule->id, rule->rulesetId, QString()};
    return RequestDecision{DaliNiraBlockerAction::Block, QStringLiteral("popup-block"), rule->id, rule->rulesetId, QString()};
  }
  return RequestDecision{DaliNiraBlockerAction::Allow, QStringLiteral("no-rule-matched"), 0, QString(), QString()};
}

bool DaliNiraBlockerEngine::genericCosmeticsDisabled(const QUrl &documentUrl) const {
  const QString scheme = documentUrl.scheme().toLower();
  if (!documentUrl.isValid() || (scheme != QLatin1String("http") && scheme != QLatin1String("https"))) return false;
  QMutexLocker locker(&mutex_);
  // domain= of a generichide exception names the document's own site.
  return firstMatch(genericHideRules_, documentUrl, documentUrl.host().toLower()) != nullptr;
}

namespace {
// URLSearchParams' application/x-www-form-urlencoded decoding of one
// name or value: "+" is a space, then UTF-8 percent-decoding.
QString formDecode(QString component) {
  component.replace(QLatin1Char('+'), QLatin1Char(' '));
  return QString::fromUtf8(QByteArray::fromPercentEncoding(component.toUtf8()));
}

// decodeURIComponent(): fails on a malformed escape or invalid UTF-8.
bool decodeUriComponent(const QString &input, QString *out) {
  const QByteArray bytes = input.toUtf8();
  for (qsizetype i = 0; i < bytes.size(); ++i) {
    if (bytes.at(i) != '%') continue;
    if (i + 2 >= bytes.size() || !isxdigit(static_cast<unsigned char>(bytes.at(i + 1))) ||
        !isxdigit(static_cast<unsigned char>(bytes.at(i + 2))))
      return false;
  }
  const QByteArray decoded = QByteArray::fromPercentEncoding(bytes);
  QStringDecoder decoder(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
  *out = decoder(decoded);
  return !decoder.hasError();
}

// atob(): whitespace ignored, padding optional; the bytes as Latin-1.
bool decodeBase64(QString input, bool urlSafe, QString *out) {
  input.remove(QRegularExpression(QStringLiteral("[\\t\\n\\f\\r ]")));
  if (urlSafe) {
    input.replace(QLatin1Char('-'), QLatin1Char('+'));
    input.replace(QLatin1Char('_'), QLatin1Char('/'));
  }
  if (input.size() % 4 == 0 && input.endsWith(QLatin1Char('='))) input.chop(input.endsWith(QStringLiteral("==")) ? 2 : 1);
  static const QRegularExpression kAlphabet(QStringLiteral("^[A-Za-z0-9+/]*$"));
  if (input.size() % 4 == 1 || !kAlphabet.match(input).hasMatch()) return false;
  const auto result = QByteArray::fromBase64Encoding(input.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
  if (!result) return false;
  *out = QString::fromLatin1(*result);
  return true;
}

// The query of a URL string as URLSearchParams sees it: (name, value) pairs.
bool searchParams(const QString &url, QList<std::pair<QString, QString>> *params) {
  const QUrl parsed(url, QUrl::StrictMode);
  if (!parsed.isValid() || parsed.scheme().isEmpty()) return false;
  params->clear();
  for (const QString &pair : parsed.query(QUrl::FullyEncoded).split(QLatin1Char('&'), Qt::SkipEmptyParts)) {
    const qsizetype equals = pair.indexOf(QLatin1Char('='));
    params->append({formDecode(equals < 0 ? pair : pair.left(equals)),
                    equals < 0 ? QString() : formDecode(pair.mid(equals + 1))});
  }
  return true;
}
}  // namespace

// The semantics of uBO's urlSkip() (src/js/urlskip.js), step for step.
QString DaliNiraBlockerEngine::applyUrlSkipSteps(const QString &url, const QStringList &steps, bool blocked) {
  if (url.size() > 8192 || steps.isEmpty()) return {};
  bool redirectBlocked = false;
  QString out = url;
  QList<std::pair<QString, QString>> params;
  for (const QString &step : steps) {
    const QString in = out;
    if (step == QLatin1String("#")) {
      const qsizetype hash = in.indexOf(QLatin1Char('#'));
      out = hash < 0 ? QString() : in.mid(hash + 1);
    } else if (step.startsWith(QLatin1Char('&')) && step.size() > 1) {
      bool ok = false;
      const int index = step.mid(1).toInt(&ok) - 1;
      if (!ok || index < 0 || !searchParams(in, &params) || index >= params.size()) return {};
      if (!decodeUriComponent(params.at(index).first, &out)) return {};
    } else if (step == QLatin1String("+https")) {
      static const QRegularExpression kScheme(QStringLiteral("^https?://"));
      static const QRegularExpression kOtherScheme(QStringLiteral("^[\\w-]://"));
      QString rest = in;
      rest.remove(kScheme);
      if (kOtherScheme.match(rest).hasMatch()) return {};
      out = QStringLiteral("https://") + rest;
    } else if (step == QLatin1String("-base64") || step == QLatin1String("-safebase64")) {
      if (!decodeBase64(in, step == QLatin1String("-safebase64"), &out)) return {};
    } else if (step == QLatin1String("-uricomponent")) {
      if (!decodeUriComponent(in, &out)) return {};
    } else if (step == QLatin1String("-blocked")) {
      redirectBlocked = true;
    } else if (step.size() > 2 && step.startsWith(QLatin1Char('/')) && step.endsWith(QLatin1Char('/'))) {
      const QRegularExpression expression(QStringLiteral("(*LIMIT_MATCH=100000)(*LIMIT_DEPTH=1000)") +
                                          step.mid(1, step.size() - 2));
      const QRegularExpressionMatch match = expression.match(in);
      if (!expression.isValid() || !match.hasMatch() || expression.captureCount() < 1 ||
          match.capturedStart(1) < 0)
        return {};
      out = match.captured(1);
    } else if (step.startsWith(QLatin1Char('?')) && step.size() > 1) {
      if (!searchParams(in, &params)) return {};
      const QString name = step.mid(1);
      const auto found = std::find_if(params.cbegin(), params.cend(), [&](const auto &p) { return p.first == name; });
      if (found == params.cend()) return {};
      out = found->second;
      out.replace(QLatin1Char(' '), QStringLiteral("%20"));
    } else {
      return {};  // an unknown step applies nothing
    }
  }
  const QUrl target(out, QUrl::TolerantMode);
  const QString scheme = target.scheme().toLower();
  if (!target.isValid() || target.host().isEmpty() ||
      (scheme != QLatin1String("http") && scheme != QLatin1String("https")))
    return {};
  if (blocked && !redirectBlocked) return {};
  return out;
}

QUrl DaliNiraBlockerEngine::urlSkipTarget(const QUrl &url, bool blocked, QString *rulesetId) const {
  QMutexLocker locker(&mutex_);
  QUrl current = url;
  for (int hop = 0; hop < 3; ++hop) {
    const QString scheme = current.scheme().toLower();
    if (!current.isValid() || (scheme != QLatin1String("http") && scheme != QLatin1String("https"))) break;
    // Patterns are written against the URL as the browser sends it (fully
    // encoded); domain= of a urlskip rule names the navigation's own site.
    const QString urlString = current.toString(QUrl::FullyEncoded);
    const QString urlLower = urlString.toLower();
    const QString host = current.host().toLower();
    const QString method = QStringLiteral("get");
    const MatchContext context{urlString, urlLower, host, host, method, DaliNiraBlockerResourceType::MainFrame};
    const QVector<quint8> flags = candidateFlags(urlSkipRules_.rules.size(), urlSkipRules_.byDomain,
                                                 urlSkipRules_.byToken, urlSkipRules_.universal, host, urlLower);
    const FilterRule *rule = nullptr;
    QString target;
    for (const int index : urlSkipRules_.byResource.at(static_cast<size_t>(DaliNiraBlockerResourceType::MainFrame))) {
      if (!flags.at(index) || !ruleMatches(urlSkipRules_.rules.at(index), context)) continue;
      target = applyUrlSkipSteps(urlString, urlSkipRules_.rules.at(index).urlSkipSteps, hop == 0 && blocked);
      if (!target.isEmpty()) {
        rule = &urlSkipRules_.rules.at(index);
        break;
      }
    }
    if (!rule) break;
    const QUrl next(target, QUrl::TolerantMode);
    if (next == current) break;
    if (hop == 0 && rulesetId) *rulesetId = rule->rulesetId;
    current = next;
  }
  return current == url ? QUrl() : current;
}

QString DaliNiraBlockerEngine::cosmeticCssForHost(const QString &host, bool includeGeneric) const {
  QMutexLocker locker(&mutex_);
  QSet<QString> exceptions;

  const QString lowerHost = host.trimmed().toLower();
  QString combined;
  // The lists' generic (not site-scoped) cosmetic CSS.
  if (includeGeneric && !listCosmeticCss_.isEmpty()) {
    combined += listCosmeticCss_ + QStringLiteral("\n");
  }

  for (const auto &cr : cosmeticRules_) {
    if (cr.isException) {
      if (cosmeticDomainApplies(lowerHost, cr.domain)) {
        exceptions.insert(cr.selector);
      }
    }
  }

  // One rule per selector: a selector the engine's CSS parser rejects then
  // drops only itself, not every custom rule of the page.
  QSet<QString> seen;
  for (const auto &cr : cosmeticRules_) {
    if (cr.isException || (!includeGeneric && cr.domain.trimmed().isEmpty())) continue;
    if (cosmeticDomainApplies(lowerHost, cr.domain) && !exceptions.contains(cr.selector) && !seen.contains(cr.selector)) {
      seen.insert(cr.selector);
      combined += cr.selector + QStringLiteral(" { display: none !important; }\n");
    }
  }

  return combined.trimmed();
}

QJsonArray DaliNiraBlockerEngine::customProceduralRulesForHost(const QString &host, bool includeGeneric) const {
  QMutexLocker locker(&mutex_);
  const QString lowerHost = host.trimmed().toLower();
  QSet<QString> exceptions;
  for (const ProceduralCosmeticRule &rule : proceduralRules_) {
    if (rule.isException && cosmeticDomainApplies(lowerHost, rule.domain))
      exceptions.insert(rule.signature);
  }
  QJsonArray result;
  QSet<QString> seen;
  for (const ProceduralCosmeticRule &rule : proceduralRules_) {
    if (rule.isException || !cosmeticDomainApplies(lowerHost, rule.domain) ||
        (!includeGeneric && rule.domain.trimmed().isEmpty()) ||
        exceptions.contains(rule.signature) || seen.contains(rule.signature)) continue;
    result.append(rule.rule);
    seen.insert(rule.signature);
  }
  return result;
}

QString DaliNiraBlockerEngine::applyCosmeticExceptions(const QString &host, const QString &css) const {
  QMutexLocker locker(&mutex_);
  QSet<QString> exceptions;
  for (const auto &rule : cosmeticRules_)
    if (rule.isException && cosmeticDomainApplies(host.toLower(), rule.domain)) exceptions.insert(rule.selector);
  if (exceptions.isEmpty()) return css;
  QString result;
  int position = 0;
  while (position < css.size()) {
    const int open = css.indexOf(QLatin1Char('{'), position);
    const int close = css.indexOf(QLatin1Char('}'), open + 1);
    if (open < 0 || close < 0) { result += css.mid(position); break; }
    const QString group = css.mid(position, open - position);
    QStringList selectors;
    int start = 0, depth = 0; QChar quote;
    for (int i = 0; i <= group.size(); ++i) {
      const QChar c = i < group.size() ? group.at(i) : QLatin1Char(',');
      if (!quote.isNull()) { if(c == quote && (i == 0 || group.at(i-1) != QLatin1Char('\\'))) quote = QChar(); continue; }
      if (c == QLatin1Char('\"') || c == QLatin1Char('\'')) { quote = c; continue; }
      if (c == QLatin1Char('(') || c == QLatin1Char('[')) ++depth;
      if (c == QLatin1Char(')') || c == QLatin1Char(']')) --depth;
      if (c == QLatin1Char(',') && depth == 0) {
        const QString selector = group.mid(start, i-start).trimmed();
        if (!selector.isEmpty() && !exceptions.contains(selector)) selectors.append(selector);
        start = i+1;
      }
    }
    if (!selectors.isEmpty()) result += selectors.join(QStringLiteral(",\n")) + css.mid(open,close-open+1) + QLatin1Char('\n');
    position = close+1;
  }
  return result;
}
