// Filter-engine capability of the native blocker (Step 2 hardening):
// $removeparam, $redirect / $rewrite / redirect exceptions, $badfilter,
// $method, and the response-phase rule families neither engine can enforce.
//
// Rules from the three sources (generated rulesets, downloaded text lists,
// custom filters) are checked against each other, and the final behavior is
// checked through the real engine adapters: the Chromium request interceptor
// on an offscreen off-the-record page, and the WebKit network filter's socket
// spoken to exactly as the web process extension does. Everything is served
// by a loopback fixture; no Internet service, no real profile.

#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <atomic>

#include "blocker_http_fixture.h"
#include "dalinira_blocker_engine.h"
#include "dalinira_blocker_filter_updates.h"
#include "dalinira_blocker_list_manager.h"
#include "dalinira_blocker_redirect_resources.h"
#include "dalinira_blocker_service.h"
#include "engine/webkit/webkit_network_filter.h"
#include "i18n/i18n.h"

namespace {

const QString kSourceResources = QStringLiteral(DALINIRA_SOURCE_DIR "/browser/resources/adblock/web_accessible_resources");
const QString kRemote = QStringLiteral("fixture-remote");
const QString kGenerated = QStringLiteral("fixture-generated");

void pass(const char *name) { std::cout << "  PASS " << name << "\n"; }

void writeJson(const QString &path, const QJsonValue &value) {
  QFile file(path);
  CHECK(file.open(QIODevice::WriteOnly));
  file.write(value.isArray() ? QJsonDocument(value.toArray()).toJson() : QJsonDocument(value.toObject()).toJson());
}

QJsonObject dnr(int id, int priority, const QJsonObject &action, const QJsonObject &condition) {
  return QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("priority"), priority},
                     {QStringLiteral("action"), action}, {QStringLiteral("condition"), condition}};
}

// A profile whose catalog is two fixture lists: one updatable from the
// fixture server, one bundled-only holding generated (DNR) equivalents of
// the text syntax under test. The real packaged redirect resources are used.
class Profile {
 public:
  explicit Profile(const HttpFixture &server) {
    CHECK(dir_.isValid());
    const QString base = rulesetDir();
    CHECK(QDir().mkpath(base + QStringLiteral("/main")));
    const QString resources = dir_.path() + QStringLiteral("/adblock/web_accessible_resources");
    CHECK(QDir().mkpath(resources));
    for (const QString &name : QDir(kSourceResources).entryList(QDir::Files))
      CHECK(QFile::copy(kSourceResources + QLatin1Char('/') + name, resources + QLatin1Char('/') + name));

    QJsonArray details;
    QJsonObject attributions;
    for (const QString &id : {kRemote, kGenerated}) {
      details.append(QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("name"), id},
                                 {QStringLiteral("group"), QStringLiteral("default")}, {QStringLiteral("enabled"), true}});
      attributions.insert(id, QJsonObject{{QStringLiteral("provider"), QStringLiteral("Fixture")},
                                          {QStringLiteral("source"), id},
                                          {QStringLiteral("sourceUrl"), server.url(QStringLiteral("/remote.txt"))},
                                          {QStringLiteral("license"), QStringLiteral("Fixture")},
                                          {QStringLiteral("reviewRequired"), false}});
    }
    writeJson(base + QStringLiteral("/ruleset-details.json"), details);
    writeJson(base + QStringLiteral("/ruleset-attributions.json"),
              QJsonObject{{QStringLiteral("rulesets"), attributions}});
    writeJson(base + QStringLiteral("/main/fixture-remote.json"),
              QJsonArray{dnr(1, 10, {{QStringLiteral("type"), QStringLiteral("block")}},
                             {{QStringLiteral("requestDomains"), QJsonArray{QStringLiteral("bundled-remote.example")}}})});
    const QJsonObject block{{QStringLiteral("type"), QStringLiteral("block")}};
    writeJson(base + QStringLiteral("/main/fixture-generated.json"), QJsonArray{
        // uBO Lite's form of "||gen.example^$removeparam=gtrk".
        dnr(1, 1, {{QStringLiteral("type"), QStringLiteral("redirect")},
                   {QStringLiteral("redirect"), QJsonObject{{QStringLiteral("transform"), QJsonObject{
                       {QStringLiteral("queryTransform"),
                        QJsonObject{{QStringLiteral("removeParams"), QJsonArray{QStringLiteral("gtrk")}}}}}}}}},
            {{QStringLiteral("requestDomains"), QJsonArray{QStringLiteral("gen.example")}}}),
        // "||gen.example/ads.js$script,redirect=noop.js".
        dnr(2, 11, {{QStringLiteral("type"), QStringLiteral("redirect")},
                    {QStringLiteral("redirect"),
                     QJsonObject{{QStringLiteral("extensionPath"), QStringLiteral("/web_accessible_resources/noop.js")}}}},
            {{QStringLiteral("urlFilter"), QStringLiteral("||gen.example/ads.js")},
             {QStringLiteral("resourceTypes"), QJsonArray{QStringLiteral("script")}}}),
        // "$removeparam" (clear the query) conditioned by domain only.
        dnr(3, 1, {{QStringLiteral("type"), QStringLiteral("redirect")},
                   {QStringLiteral("redirect"), QJsonObject{{QStringLiteral("transform"),
                                                            QJsonObject{{QStringLiteral("query"), QString()}}}}}},
            {{QStringLiteral("requestDomains"), QJsonArray{QStringLiteral("fonts.gen.example")}},
             {QStringLiteral("resourceTypes"), QJsonArray{QStringLiteral("stylesheet")}}}),
        // Response phase: a CSP append and a response-header condition.
        dnr(4, 1, {{QStringLiteral("type"), QStringLiteral("modifyHeaders")},
                   {QStringLiteral("responseHeaders"),
                    QJsonArray{QJsonObject{{QStringLiteral("header"), QStringLiteral("content-security-policy")},
                                           {QStringLiteral("operation"), QStringLiteral("append")},
                                           {QStringLiteral("value"), QStringLiteral("script-src 'none'")}}}}},
            {{QStringLiteral("urlFilter"), QStringLiteral("||csp.gen.example^")},
             {QStringLiteral("resourceTypes"), QJsonArray{QStringLiteral("main_frame")}}}),
        dnr(5, 10, block,
            {{QStringLiteral("urlFilter"), QStringLiteral("||hdr.gen.example^")},
             {QStringLiteral("responseHeaders"), QJsonArray{QJsonObject{{QStringLiteral("header"), QStringLiteral("x-ad")}}}}}),
        dnr(6, 10, block, {{QStringLiteral("requestDomains"), QJsonArray{QStringLiteral("bundled-block.example")}}})});
    writeJson(base + QStringLiteral("/filter-update-sources.json"),
              QJsonObject{{QStringLiteral("sources"),
                           QJsonObject{{kRemote, QJsonObject{{QStringLiteral("url"), server.url(QStringLiteral("/remote.txt"))},
                                                             {QStringLiteral("format"), QStringLiteral("adblock-plus-text")},
                                                             {QStringLiteral("minimumNetworkRules"), 3}}}}}});
  }
  QString path() const { return dir_.path(); }
  QString rulesetDir() const { return dir_.path() + QStringLiteral("/adblock/rulesets"); }

 private:
  QTemporaryDir dir_;
};

DaliNiraBlockerFilterFetcher::Policy testPolicy() {
  DaliNiraBlockerFilterFetcher::Policy policy;
  policy.transferTimeoutMs = 3000;
  policy.totalTimeoutMs = 9000;
  policy.allowLoopbackHttpForTesting = true;
  return policy;
}

void prepare(DaliNiraBlockerService &service, const QStringList &customLines = {}) {
  service.setFilterFetchPolicyForTesting(testPolicy());
  service.settings()->setAutoReloadOnModeChange(false);
  service.settings()->setEnabledRulesetIds({kRemote, kGenerated});
  service.settings()->setCustomFilters(customLines);
}

NetworkRequestVerdict decide(DaliNiraBlockerService &service, const QString &url,
                             DaliNiraBlockerResourceType type = DaliNiraBlockerResourceType::Script,
                             const QString &firstParty = QStringLiteral("https://news.example/"),
                             const QString &method = QStringLiteral("get")) {
  return service.decideNetworkRequest(QUrl(url), type, QUrl(firstParty), 0x51, method);
}

// The URL a removeparam/transform redirect leads to, or empty.
QString rewritten(DaliNiraBlockerService &service, const QString &url,
                  DaliNiraBlockerResourceType type = DaliNiraBlockerResourceType::Image,
                  const QString &firstParty = QStringLiteral("https://news.example/")) {
  const NetworkRequestVerdict verdict = decide(service, url, type, firstParty);
  if (verdict.kind != NetworkVerdictKind::Redirect) return {};
  return verdict.redirectUrl.toString(QUrl::FullyEncoded);
}

bool runUpdate(DaliNiraBlockerService &service) {
  bool finished = false;
  bool success = false;
  const auto connection = QObject::connect(&service, &DaliNiraBlockerService::filterUpdateFinished,
                                           [&](bool ok, const QString &) { finished = true; success = ok; });
  service.updateFiltersAsync();
  CHECK(waitUntil([&] { return finished; }, 20000));
  QObject::disconnect(connection);
  return success;
}

// ---------------- Cases ----------------

void testParserAndValidator() {
  const DaliNiraBlockerRedirectResources resources(kSourceResources);
  auto error = [&resources](const QString &line) { return DaliNiraBlockerEngine::validateCustomFilterLine(line, &resources); };
  // Accepted syntax.
  for (const char *line : {"||a.example^$removeparam=trk", "||a.example^$removeparam", "$removeparam=gclsrc",
                           "||a.example^$removeparam=/^(trk|sid)_[a-z]+=/i", "@@||a.example^$removeparam=trk",
                           "@@||a.example^$removeparam", "||a.example/x.js$script,redirect=noopjs",
                           "||a.example/x.js$script,redirect=noop.js,important", "@@||a.example^$redirect-rule",
                           "@@||a.example^$redirect=noop.txt", "||a.example/v.mp4$rewrite=abp-resource:blank-mp4",
                           "||a.example^$badfilter", "||a.example^$method=post", "||a.example^$method=~get|~head",
                           "||a.example^$popup", "@@||a.example^$popup", "@@||a.example^$generichide",
                           "/banner/ads.js$script"})
    CHECK(error(QString::fromLatin1(line)).isEmpty());
  // Redirect targets are only packaged resources, by name.
  for (const char *target : {"nothere.js", "../../etc/passwd", "/etc/passwd", "http://evil.example/x.js",
                             "javascript:alert(1)", "file:///etc/passwd", "noop.js/../noop.js", "data:text/plain,x",
                             "not-a-packaged-gtm.js"}) {
    const QString line = QStringLiteral("||a.example^$script,redirect=") + QString::fromLatin1(target);
    // (A comma ends the option: "data:text/plain,x" is rejected as "data:text/plain".)
    CHECK(error(line).startsWith(
        I18n::text(QStringLiteral("blocker.err_redirect_resource_unknown")).section(QStringLiteral("%1"), 0, 0)));
  }
  // Without the registry nothing can be a redirect target.
  CHECK(!DaliNiraBlockerEngine::validateCustomFilterLine(QStringLiteral("||a.example^$redirect=noopjs")).isEmpty());
  // Malformed values.
  for (const char *line : {"||a.example^$removeparam=/unclosed", "||a.example^$removeparam=a&b",
                           "||a.example^$removeparam=~keep", "@@||a.example^$removeparam=/re/",
                           "||a.example^$removeparam=/(/", "||a.example^$method=fetch", "||a.example^$method=",
                           "||a.example^$redirect", "||a.example^$redirect=noopjs,removeparam=x",
                           "||a.example^$rewrite=noop.js"})
    CHECK(error(QString::fromLatin1(line)).startsWith(
        I18n::text(QStringLiteral("blocker.err_option_value_invalid")).section(QStringLiteral("%1"), 0, 0)));
  CHECK(!error(QStringLiteral("||a.example^$removeparam=") + QString(600, QLatin1Char('x'))).isEmpty());
  // Response-phase options are named for what they need.
  for (const char *option : {"csp", "header", "permissions"})
    CHECK(error(QStringLiteral("||a.example^$%1=x").arg(QString::fromLatin1(option))) ==
          I18n::text(QStringLiteral("blocker.err_option_needs_response_headers")).arg(QString::fromLatin1(option)));
  CHECK(error(QStringLiteral("||a.example^$redirect-rule=noopjs")) ==
        I18n::text(QStringLiteral("blocker.err_unsupported_option")).arg(QStringLiteral("redirect-rule")));
  // $popup became a supported Step 3 request option. Keep this assertion's
  // original purpose with a genuinely unsupported network capability instead.
  CHECK(error(QStringLiteral("||a.example^$cname")) ==
        I18n::text(QStringLiteral("blocker.err_unsupported_option")).arg(QStringLiteral("cname")));
  CHECK(error(QStringLiteral("||a.example^$~redirect=noopjs")) ==
        I18n::text(QStringLiteral("blocker.err_unsupported_option")).arg(QStringLiteral("~redirect=noopjs")));
  CHECK(error(QStringLiteral("$script")) == I18n::text(QStringLiteral("blocker.err_network_pattern_empty")));
  CHECK(error(QStringLiteral("$script,domain=~a.example")) == I18n::text(QStringLiteral("blocker.err_network_pattern_empty")));
  CHECK(error(QStringLiteral("$third-party,xmlhttprequest,domain=site.example")).isEmpty());
  {
    // EasyList/EasyPrivacy's site-scoped rules without a pattern.
    const auto scoped = DaliNiraBlockerEngine::compileTextFilterList(
        QStringLiteral("$third-party,xmlhttprequest,domain=site.example\n"), QStringLiteral("x"), {&resources, nullptr});
    DaliNiraBlockerEngine engine;
    engine.applyCompiledPlan(DaliNiraBlockerEngine::compilePlan(scoped.rules, QString(), {}));
    auto action = [&engine](const QString &url, DaliNiraBlockerResourceType type, const QString &site) {
      return engine.evaluate(QUrl(url), type, site, DaliNiraBlockerMode::Ideal, SitePolicy{}).action;
    };
    CHECK(action(QStringLiteral("https://api.other.example/x"), DaliNiraBlockerResourceType::Xhr, QStringLiteral("site.example")) ==
          DaliNiraBlockerAction::Block);
    CHECK(action(QStringLiteral("https://cdn.site.example/x"), DaliNiraBlockerResourceType::Xhr, QStringLiteral("site.example")) ==
          DaliNiraBlockerAction::Allow);
    CHECK(action(QStringLiteral("https://api.other.example/x"), DaliNiraBlockerResourceType::Script, QStringLiteral("site.example")) ==
          DaliNiraBlockerAction::Allow);
    CHECK(action(QStringLiteral("https://api.other.example/x"), DaliNiraBlockerResourceType::Xhr, QStringLiteral("elsewhere.example")) ==
          DaliNiraBlockerAction::Allow);
  }
  CHECK(error(QStringLiteral("/")) == I18n::text(QStringLiteral("blocker.err_regex_unclosed")));

  // Aliases resolve to the packaged file, and to the same shared data: URL.
  CHECK(resources.canonicalName(QStringLiteral("noopjs")) == QLatin1String("noop.js"));
  CHECK(resources.canonicalName(QStringLiteral("abp-resource:blank-mp4")) == QLatin1String("noop-1s.mp4"));
  CHECK(resources.canonicalName(QStringLiteral("../noop.js")).isEmpty());
  CHECK(resources.dataUrl(QStringLiteral("noop.js")).startsWith(QStringLiteral("data:")));
  CHECK(resources.dataUrl(QStringLiteral("noop.js")).constData() == resources.dataUrl(QStringLiteral("noop.js")).constData());
  CHECK(resources.dataUrl(QStringLiteral("../noop.js")).isEmpty());

  // Skip reasons are specific, never a generic "skipped".
  const auto compiled = DaliNiraBlockerEngine::compileTextFilterList(
      QStringLiteral("||a.example^$csp=script-src 'none'\n||b.example^$popup\n||c.example^$redirect=missing.js\n"
                     "||d.example^$method=teleport\n||e.example^$redirect-rule=noopjs\n||f.example^$generichide\n"
                     "||g.example^$removeparam=trk\n||h.example^$script,redirect=noopjs\n||i.example^$cname\n"
                     "@@||j.example^$generichide\n"),
      QStringLiteral("x"), {&resources, nullptr});
  CHECK(compiled.skippedByReason.value(QStringLiteral("csp")) == 1);
  CHECK(compiled.skippedByReason.value(QStringLiteral("cname")) == 1);
  CHECK(compiled.skippedByReason.value(QStringLiteral("invalid-value:generichide")) == 1);
  CHECK(compiled.skippedByReason.value(QStringLiteral("redirect-rule")) == 1);
  CHECK(compiled.skippedByReason.value(QStringLiteral("redirect-unknown-resource:missing.js")) == 1);
  CHECK(compiled.skippedByReason.value(QStringLiteral("invalid-value:method")) == 1);
  CHECK(compiled.popupRules == 1 && compiled.genericHideRules == 1);
  CHECK(compiled.removeparamRules == 1 && compiled.redirectRules == 1 && compiled.networkRules == 2);
  pass("parser/validator: accepted syntax, packaged-only redirect targets, malformed values, explicit skip reasons");
}

void testRemoveParam(HttpFixture &server) {
  server.reset();
  Profile profile(server);
  DaliNiraBlockerService service(profile.path());
  prepare(service, {QStringLiteral("||shop.example^$removeparam=trk"),
                    QStringLiteral("||shop.example^$removeparam=/^sid_[a-z]+=/i"),
                    QStringLiteral("||scoped.example^$removeparam=trk,domain=news.example"),
                    QStringLiteral("||typed.example^$image,removeparam=trk"),
                    QStringLiteral("||wipe.example^$removeparam"),
                    QStringLiteral("$removeparam=gtrk2"),
                    QStringLiteral("||gen2.example^$removeparam=gtrk"),
                    QStringLiteral("||kept.example^$removeparam=trk"),
                    QStringLiteral("@@||kept.example/keep/$removeparam=trk"),
                    QStringLiteral("@@||kept.example/all/$removeparam"),
                    QStringLiteral("@@||kept.example/other/$removeparam=other"),
                    QStringLiteral("||blocked.example^$removeparam=trk"),
                    QStringLiteral("||blocked.example/b/"),
                    QStringLiteral("@@||allowed.example^"),
                    QStringLiteral("||allowed.example^$removeparam=trk")});
  // Simple, repeated, similar names, positions.
  CHECK(rewritten(service, QStringLiteral("https://shop.example/p?id=1&trk=abc&x=2")) ==
        QLatin1String("https://shop.example/p?id=1&x=2"));
  CHECK(rewritten(service, QStringLiteral("https://shop.example/p?trk=1&a=2&trk=3")) ==
        QLatin1String("https://shop.example/p?a=2"));
  CHECK(rewritten(service, QStringLiteral("https://shop.example/p?trk=1&trkx=2&xtrk=3&TRK=4")) ==
        QLatin1String("https://shop.example/p?trkx=2&xtrk=3&TRK=4"));
  // Encoding of everything that stays is kept byte for byte; names compare decoded.
  CHECK(rewritten(service, QStringLiteral("https://shop.example/p?q=a%20b%2Fc&tr%6B=%E2%9C%93&e=%26&u=%C3%A9")) ==
        QLatin1String("https://shop.example/p?q=a%20b%2Fc&e=%26&u=%C3%A9"));
  // Removing the last parameter drops the "?"; the fragment is kept.
  CHECK(rewritten(service, QStringLiteral("https://shop.example/p?trk=1#frag")) ==
        QLatin1String("https://shop.example/p#frag"));
  // Regex form on the decoded name=value, case-insensitive with /i.
  CHECK(rewritten(service, QStringLiteral("https://shop.example/p?SID_abc=1&sid=2&sid_=3")) ==
        QLatin1String("https://shop.example/p?sid=2&sid_=3"));
  // Nothing to remove: no redirect (and never a redirect to the same URL).
  CHECK(decide(service, QStringLiteral("https://shop.example/p?id=1")).kind == NetworkVerdictKind::Allow);
  CHECK(decide(service, QStringLiteral("https://shop.example/trk=1/p")).kind == NetworkVerdictKind::Allow);
  // Domain and type restrictions.
  CHECK(!rewritten(service, QStringLiteral("https://scoped.example/p?trk=1")).isEmpty());
  CHECK(rewritten(service, QStringLiteral("https://scoped.example/p?trk=1"), DaliNiraBlockerResourceType::Image,
                  QStringLiteral("https://other.example/")).isEmpty());
  CHECK(!rewritten(service, QStringLiteral("https://typed.example/p?trk=1"), DaliNiraBlockerResourceType::Image).isEmpty());
  CHECK(rewritten(service, QStringLiteral("https://typed.example/p?trk=1"), DaliNiraBlockerResourceType::Script).isEmpty());
  // Remove all; generic (no pattern).
  CHECK(rewritten(service, QStringLiteral("https://wipe.example/p?a=1&b=2")) == QLatin1String("https://wipe.example/p"));
  CHECK(decide(service, QStringLiteral("https://wipe.example/p")).kind == NetworkVerdictKind::Allow);
  CHECK(rewritten(service, QStringLiteral("https://any.example/x?gtrk2=1&k=v")) == QLatin1String("https://any.example/x?k=v"));
  // Exceptions: by name, all, and an unrelated name does not cancel.
  CHECK(decide(service, QStringLiteral("https://kept.example/keep/a?trk=1")).kind == NetworkVerdictKind::Allow);
  CHECK(decide(service, QStringLiteral("https://kept.example/all/a?trk=1")).kind == NetworkVerdictKind::Allow);
  CHECK(!rewritten(service, QStringLiteral("https://kept.example/other/a?trk=1")).isEmpty());
  CHECK(!rewritten(service, QStringLiteral("https://kept.example/else/a?trk=1")).isEmpty());
  // Precedence: a block wins over parameter removal, an exception too.
  CHECK(decide(service, QStringLiteral("https://blocked.example/b/x?trk=1")).kind == NetworkVerdictKind::Block);
  CHECK(!rewritten(service, QStringLiteral("https://blocked.example/c/x?trk=1")).isEmpty());
  CHECK(decide(service, QStringLiteral("https://allowed.example/x?trk=1")).kind == NetworkVerdictKind::Allow);
  // Generated (DNR queryTransform) and text forms decide the same.
  CHECK(rewritten(service, QStringLiteral("https://gen.example/p?a=1&gtrk=2&b=%2F")) ==
        QLatin1String("https://gen.example/p?a=1&b=%2F"));
  CHECK(rewritten(service, QStringLiteral("https://gen2.example/p?a=1&gtrk=2&b=%2F")) ==
        QLatin1String("https://gen2.example/p?a=1&b=%2F"));
  // Generated query-clear rule conditioned only by domain: once the query is
  // gone the stylesheet loads; it is never redirected to itself.
  CHECK(rewritten(service, QStringLiteral("https://fonts.gen.example/k.css?v=1"), DaliNiraBlockerResourceType::Stylesheet) ==
        QLatin1String("https://fonts.gen.example/k.css"));
  CHECK(decide(service, QStringLiteral("https://fonts.gen.example/k.css"), DaliNiraBlockerResourceType::Stylesheet).kind ==
        NetworkVerdictKind::Allow);
  // Following the redirects converges (each hop removes something).
  QString url = QStringLiteral("https://shop.example/p?trk=1&SID_a=2&keep=3&trk=4");
  int hops = 0;
  for (QString next; !(next = rewritten(service, url)).isEmpty(); url = next) CHECK(++hops < 5);
  CHECK(url == QLatin1String("https://shop.example/p?keep=3"));
  // Site controls still apply.
  SitePolicy allowlisted;
  allowlisted.whitelisted = true;
  service.settings()->setSitePolicy(QStringLiteral("news.example"), allowlisted);
  CHECK(decide(service, QStringLiteral("https://shop.example/p?trk=1")).kind == NetworkVerdictKind::Allow);
  service.settings()->setSitePolicy(QStringLiteral("news.example"), SitePolicy{});
  pass("removeparam: exact/repeated/similar names, encoding kept, fragment, regex, all, generic, scope, exceptions, precedence, generated==text, no self-redirect");
}

void testRedirect(HttpFixture &server) {
  server.reset();
  Profile profile(server);
  DaliNiraBlockerService service(profile.path());
  prepare(service, {QStringLiteral("||ads.example/lib.js$script,redirect=noopjs"),
                    QStringLiteral("||ads.example/lib2.js$script,redirect=noop.js"),
                    QStringLiteral("||ads.example/xhr$xmlhttprequest,redirect=noopjson"),
                    QStringLiteral("||ads.example/scoped.js$script,redirect=noopjs,domain=news.example"),
                    QStringLiteral("||media.example/v.mp4$rewrite=abp-resource:blank-mp4"),
                    QStringLiteral("||ex.example/a.js$script,redirect=noopjs"),
                    QStringLiteral("@@||ex.example^$redirect-rule"),
                    QStringLiteral("||ex2.example/a.js$script,redirect=noopjs"),
                    QStringLiteral("@@||ex2.example^$redirect=noop.txt"),
                    QStringLiteral("@@||gen.example^$redirect"),
                    QStringLiteral("||prec.example/a.js$script,redirect=noopjs"),
                    QStringLiteral("||prec.example/b.js$script"),
                    QStringLiteral("@@||google-analytics.com/analytics.js$redirect-rule,script,domain=mond.how")});
  const DaliNiraBlockerRedirectResources *resources = service.listManager()->redirectResources();
  const QString noopJs = resources->dataUrl(QStringLiteral("noop.js"));
  CHECK(noopJs.startsWith(QStringLiteral("data:")));
  auto verdictOf = [&](const QString &url, DaliNiraBlockerResourceType type = DaliNiraBlockerResourceType::Script,
                       const QString &site = QStringLiteral("https://news.example/")) {
    return decide(service, url, type, site);
  };
  NetworkRequestVerdict v = verdictOf(QStringLiteral("https://ads.example/lib.js"));
  CHECK(v.kind == NetworkVerdictKind::Redirect && v.redirectUrl.toString() == noopJs);
  CHECK(v.decision.reason == QLatin1String("dnr-redirect-resource"));
  CHECK(verdictOf(QStringLiteral("https://ads.example/lib2.js")).redirectUrl.toString() == noopJs);
  CHECK(verdictOf(QStringLiteral("https://ads.example/xhr"), DaliNiraBlockerResourceType::Xhr).redirectUrl.toString() ==
        resources->dataUrl(QStringLiteral("noop.json")));
  // Resource type and domain restrictions.
  CHECK(verdictOf(QStringLiteral("https://ads.example/lib.js"), DaliNiraBlockerResourceType::Image).kind ==
        NetworkVerdictKind::Allow);
  CHECK(verdictOf(QStringLiteral("https://ads.example/scoped.js")).kind == NetworkVerdictKind::Redirect);
  CHECK(verdictOf(QStringLiteral("https://ads.example/scoped.js"), DaliNiraBlockerResourceType::Script,
                  QStringLiteral("https://other.example/")).kind == NetworkVerdictKind::Allow);
  CHECK(verdictOf(QStringLiteral("https://media.example/v.mp4"), DaliNiraBlockerResourceType::Media).redirectUrl.toString() ==
        resources->dataUrl(QStringLiteral("noop-1s.mp4")));
  // Redirect exceptions keep the block and drop the substitution; one for
  // another resource does not apply.
  v = verdictOf(QStringLiteral("https://ex.example/a.js"));
  CHECK(v.kind == NetworkVerdictKind::Block && v.decision.reason == QLatin1String("redirect-exception-block"));
  CHECK(verdictOf(QStringLiteral("https://ex2.example/a.js")).kind == NetworkVerdictKind::Redirect);
  // A text exception cancels a generated (extensionPath) redirect as well.
  v = verdictOf(QStringLiteral("https://gen.example/ads.js"));
  CHECK(v.kind == NetworkVerdictKind::Block && v.decision.reason == QLatin1String("redirect-exception-block"));
  // ... and the built-in analytics stub, only on the site EasyPrivacy names.
  CHECK(verdictOf(QStringLiteral("https://www.google-analytics.com/analytics.js"), DaliNiraBlockerResourceType::Script,
                  QStringLiteral("https://mond.how/")).kind == NetworkVerdictKind::Block);
  CHECK(verdictOf(QStringLiteral("https://www.google-analytics.com/analytics.js")).kind == NetworkVerdictKind::Redirect);
  CHECK(verdictOf(QStringLiteral("https://prec.example/a.js")).kind == NetworkVerdictKind::Redirect);
  CHECK(verdictOf(QStringLiteral("https://prec.example/b.js")).kind == NetworkVerdictKind::Block);
  // List precedence (uBO Lite priorities): a redirect replaces the block it
  // outranks, an exception beats a redirect, an $important redirect beats the
  // exception. (Custom @@ rules keep winning over every custom rule.)
  {
    const auto list = DaliNiraBlockerEngine::compileTextFilterList(
        QStringLiteral("||imp.example/a.js$script,important,redirect=noopjs\n@@||imp.example^\n"
                       "||imp2.example/a.js$script,redirect=noopjs\n@@||imp2.example^\n"
                       "||prec2.example/a.js$script\n||prec2.example/a.js$script,redirect=noopjs\n"),
        QStringLiteral("list"), {resources, nullptr});
    DaliNiraBlockerEngine engine;
    engine.applyCompiledPlan(DaliNiraBlockerEngine::compilePlan(list.rules, QString(), {}));
    auto action = [&engine](const QString &url) {
      return engine.evaluate(QUrl(url), DaliNiraBlockerResourceType::Script, QStringLiteral("news.example"),
                             DaliNiraBlockerMode::Ideal, SitePolicy{}).action;
    };
    CHECK(action(QStringLiteral("https://imp.example/a.js")) == DaliNiraBlockerAction::Redirect);
    CHECK(action(QStringLiteral("https://imp2.example/a.js")) == DaliNiraBlockerAction::Allow);
    CHECK(action(QStringLiteral("https://prec2.example/a.js")) == DaliNiraBlockerAction::Redirect);
  }
  // The substitute is a data: URL, which is never filtered again (no loops).
  CHECK(decide(service, noopJs).kind == NetworkVerdictKind::Allow);
  // Site controls.
  SitePolicy noAds;
  noAds.adBlocking = false;
  service.settings()->setSitePolicy(QStringLiteral("news.example"), noAds);
  CHECK(verdictOf(QStringLiteral("https://ads.example/lib.js")).kind == NetworkVerdictKind::Allow);
  service.settings()->setSitePolicy(QStringLiteral("news.example"), SitePolicy{});

  // A packaged file that disappears is skipped (the rule never points at a
  // missing resource), not turned into a block.
  QTemporaryDir partial;
  CHECK(QFile::copy(kSourceResources + QStringLiteral("/noop.js"), partial.path() + QStringLiteral("/noop.js")));
  const DaliNiraBlockerRedirectResources shrinking(partial.path());
  CHECK(QFile::remove(partial.path() + QStringLiteral("/noop.js")));
  const auto compiled = DaliNiraBlockerEngine::compileTextFilterList(
      QStringLiteral("||a.example/x.js$script,redirect=noop.js\n"), QStringLiteral("x"), {&shrinking, nullptr});
  CHECK(compiled.rules.isEmpty() && compiled.skippedByReason.value(QStringLiteral("redirect-missing-resource:noop.js")) == 1);
  pass("redirect: aliases, types, domains, $rewrite, exceptions (text and generated), precedence, site controls, missing resource");
}

// The EasyPrivacy $redirect / $redirect-rule / $rewrite lines of the
// 2026-10-01 copy, commit 5133bbcd877909ee11fdcef829aecb958a6effe0
// (EasyList project, GPL-3.0-or-later OR CC-BY-SA-3.0), plus one EasyList
// $rewrite line from 2026-10-01 commit 210ff738ba1aeb7e8ac60516ee79a052e0d6e1cd.
const char *const kEasyPrivacyRedirectLines[] = {
    "||pcoptimizedsettings.com/wp-content/plugins/koko-analytics/$important,redirect=noop.js,script",
    "||pcoptimizedsettings.com/wp-content/uploads/breeze/google/gtag.js$important,redirect=noop.js,script",
    "||d21y75miwcfqoq.cloudfront.net/436f3341$image,redirect=nooptext",
    "||static.chartbeat.com/js/chartbeat.js$redirect=chartbeat.js,script",
    "||static.chartbeat.com/js/chartbeat_mab.js$redirect=chartbeat.js,script",
    "||askdriver.com^$redirect=noopjs,script",
    "||backtension.com^$redirect=noopjs,script",
    "||curbneon.com^$redirect=noopjs,script",
    "||dogbedscentral.com^$redirect=noopjs,script",
    "||endhairfall.com^$redirect=noopjson,xmlhttprequest",
    "||fishingtoolsbox.com^$redirect=noopjs,script",
    "||fjordsand.com^$redirect=noopjs,script",
    "||fontserif.com^$redirect=noopjs,script",
    "||funtoyplanet.com^$redirect=noopjs,script",
    "||glasscoyote.com^$redirect=noopjs,script",
    "||goddesslamps.com^$redirect=noopjs,script",
    "||kindhush.com^$redirect=noopjs,script",
    "||oceancloudhosts.com^$redirect=noopjson,xmlhttprequest",
    "||randisphotos.com^$redirect=noopjs,script",
    "||targetfax.com^$redirect=noopjs,script",
    "||ubiquitoussea.com^$redirect=noopjs,script",
    "||unloadyourself.com^$redirect=noopjs,script",
    "||googletagmanager.com/gtag/js$important,redirect=googletagmanager_gtm.js,script,xmlhttprequest,domain=redstoneonline.jp",
    "||sstats.bitdefender.com^$important,redirect=noop.txt,xmlhttprequest",
    "||r10s.jp/com/img/home/t.gif?$image,important,redirect=1x1.gif,domain=24.rakuten.co.jp",
    "@@||google-analytics.com/analytics.js$redirect-rule,script,domain=mond.how",
    "||curos.ca/res/*/resources/scripts/lib/fingerprint.js$important,redirect=fingerprint2.js,script",
    "||metrics.streaks.jp^$redirect=noop.txt,xmlhttprequest",
    "||ipcheck.blogsys.jp^$redirect=noop.js,script",
    "||void.nicopr.fr/rec$method=POST",
    "||d1z2jf7jlzjs58.cloudfront.net^$rewrite=abp-resource:blank-js,domain=voici.fr",
    "||innovid.com/media/encoded/*.mp4$rewrite=abp-resource:blank-mp4,domain=ktla.com",
};

void testRealListRedirectLines() {
  const DaliNiraBlockerRedirectResources resources(kSourceResources);
  QString text;
  for (const char *line : kEasyPrivacyRedirectLines) text += QString::fromLatin1(line) + QLatin1Char('\n');
  const auto compiled = DaliNiraBlockerEngine::compileTextFilterList(text, QStringLiteral("easyprivacy"), {&resources, nullptr});
  // All 28 $redirect lines now name provenance-verified packaged resources.
  CHECK(compiled.redirectRules == 28 + 2);  // + the two $rewrite lines
  CHECK(compiled.modifierExceptions == 1);
  CHECK(compiled.unsupportedLines == 0);
  CHECK(compiled.networkRules == 28 + 2 + 1 + 1);  // redirects, rewrites, the exception, $method

  DaliNiraBlockerEngine engine;
  engine.applyCompiledPlan(DaliNiraBlockerEngine::compilePlan(compiled.rules, QString(), {}, {&resources, nullptr}));
  auto decideOn = [&engine](const QString &url, DaliNiraBlockerResourceType type, const QString &site,
                            const QString &method = QStringLiteral("get")) {
    return engine.evaluate(QUrl(url), type, site, DaliNiraBlockerMode::Ideal, SitePolicy{}, method);
  };
  const auto script = DaliNiraBlockerResourceType::Script;
  CHECK(decideOn(QStringLiteral("https://static.chartbeat.com/js/chartbeat.js"), script, QStringLiteral("site.example")).redirectUrl ==
        resources.dataUrl(QStringLiteral("chartbeat.js")));
  CHECK(decideOn(QStringLiteral("https://cdn.askdriver.com/t.js"), script, QStringLiteral("site.example")).redirectUrl ==
        resources.dataUrl(QStringLiteral("noop.js")));
  CHECK(decideOn(QStringLiteral("https://endhairfall.com/api"), DaliNiraBlockerResourceType::Xhr, QStringLiteral("site.example")).redirectUrl ==
        resources.dataUrl(QStringLiteral("noop.json")));
  CHECK(decideOn(QStringLiteral("https://d21y75miwcfqoq.cloudfront.net/436f3341/p.gif"), DaliNiraBlockerResourceType::Image,
                 QStringLiteral("site.example")).redirectUrl == resources.dataUrl(QStringLiteral("noop.txt")));
  CHECK(decideOn(QStringLiteral("https://r10s.jp/com/img/home/t.gif?x=1"), DaliNiraBlockerResourceType::Image,
                 QStringLiteral("24.rakuten.co.jp")).redirectUrl == resources.dataUrl(QStringLiteral("1x1.gif")));
  CHECK(decideOn(QStringLiteral("https://r10s.jp/com/img/home/t.gif?x=1"), DaliNiraBlockerResourceType::Image,
                 QStringLiteral("www.rakuten.co.jp")).action == DaliNiraBlockerAction::Allow);
  CHECK(decideOn(QStringLiteral("https://curos.ca/res/v2/resources/scripts/lib/fingerprint.js"), script, QStringLiteral("curos.ca")).redirectUrl ==
        resources.dataUrl(QStringLiteral("fingerprint2.js")));
  CHECK(decideOn(QStringLiteral("https://innovid.com/media/encoded/a.mp4"), DaliNiraBlockerResourceType::Media,
                 QStringLiteral("ktla.com")).redirectUrl == resources.dataUrl(QStringLiteral("noop-1s.mp4")));
  CHECK(decideOn(QStringLiteral("https://void.nicopr.fr/rec"), DaliNiraBlockerResourceType::Xhr, QStringLiteral("x.example"),
                 QStringLiteral("post")).action == DaliNiraBlockerAction::Block);
  CHECK(decideOn(QStringLiteral("https://void.nicopr.fr/rec"), DaliNiraBlockerResourceType::Xhr, QStringLiteral("x.example")).action ==
        DaliNiraBlockerAction::Allow);
  CHECK(decideOn(QStringLiteral("https://www.googletagmanager.com/gtag/js?id=1"), script,
                 QStringLiteral("redstoneonline.jp")).redirectUrl ==
        resources.dataUrl(QStringLiteral("googletagmanager_gtm.js")));
  pass("real EasyPrivacy/EasyList lines: 28/28 $redirect + 2 $rewrite enforced, $redirect-rule exception, $method");
}

void testBadfilter(HttpFixture &server) {
  server.reset();
  Profile profile(server);
  server.setRoute("/remote.txt", FixtureRoute{200, "[Adblock Plus 2.0]\n"
                                                   "||remote-a.example^$script\n"
                                                   "||remote-b.example^$script,third-party\n"
                                                   "||remote-c.example^$script\n"
                                                   "||remote-d.example^$domain=b.example|a.example\n"
                                                   "@@||remote-e.example/ok.js\n"
                                                   "||remote-e.example^\n"
                                                   "||remote-f.example^\n"
                                                   "||remote-a.example^$script,badfilter\n",
                                              "text/plain", "\"v1\""});
  DaliNiraBlockerService service(profile.path());
  prepare(service);
  CHECK(runUpdate(service));
  auto blocked = [&](const QString &url, const QString &site = QStringLiteral("https://a.example/")) {
    return decide(service, url, DaliNiraBlockerResourceType::Script, site).kind == NetworkVerdictKind::Block;
  };
  // Same list: the matching rule is disabled, the line itself blocks nothing.
  CHECK(!blocked(QStringLiteral("https://remote-a.example/x.js")));
  CHECK(blocked(QStringLiteral("https://remote-b.example/x.js")));
  CHECK(blocked(QStringLiteral("https://remote-c.example/x.js")));
  // Custom badfilters: options in another order / case, domain order.
  service.settings()->setCustomFilters({QStringLiteral("||remote-b.example^$THIRD-PARTY,badfilter,script"),
                                        QStringLiteral("||remote-d.example^$domain=a.example|b.example,badfilter")});
  CHECK(!blocked(QStringLiteral("https://remote-b.example/x.js")));
  CHECK(!blocked(QStringLiteral("https://remote-d.example/x.js")));
  // Near misses disable nothing.
  service.settings()->setCustomFilters({QStringLiteral("||remote-c.example^$image,badfilter"),
                                        QStringLiteral("||remote-c.example^$badfilter"),
                                        QStringLiteral("||remote-c.examples^$script,badfilter"),
                                        QStringLiteral("|remote-c.example^$script,badfilter"),
                                        QStringLiteral("||remote-c.example^$script,domain=x.example,badfilter")});
  CHECK(blocked(QStringLiteral("https://remote-c.example/x.js")));
  CHECK(blocked(QStringLiteral("https://remote-b.example/x.js")));
  // An exception can be disabled too.
  CHECK(!blocked(QStringLiteral("https://remote-e.example/ok.js")));
  service.settings()->setCustomFilters({QStringLiteral("@@||remote-e.example/ok.js$badfilter")});
  CHECK(blocked(QStringLiteral("https://remote-e.example/ok.js")));
  // Custom rule on / custom badfilter on / off.
  service.settings()->setCustomFilters({QStringLiteral("||custom-z.example^")});
  CHECK(blocked(QStringLiteral("https://custom-z.example/x.js")));
  service.settings()->setCustomFilters({QStringLiteral("||custom-z.example^"), QStringLiteral("||custom-z.example^$badfilter")});
  CHECK(!blocked(QStringLiteral("https://custom-z.example/x.js")));
  service.settings()->setCustomFilters({QStringLiteral("||custom-z.example^")});
  CHECK(blocked(QStringLiteral("https://custom-z.example/x.js")));
  // Generated rulesets have no text form: a $badfilter does not reach them
  // (an @@ exception does).
  service.settings()->setCustomFilters({QStringLiteral("||bundled-block.example^$badfilter")});
  CHECK(blocked(QStringLiteral("https://bundled-block.example/x.js")));
  service.settings()->setCustomFilters({});

  // Remote recompilation: the next version drops the $badfilter line.
  server.setRoute("/remote.txt", FixtureRoute{200, "[Adblock Plus 2.0]\n"
                                                   "||remote-a.example^$script\n"
                                                   "||remote-b.example^$script,third-party\n"
                                                   "||remote-c.example^$script\n"
                                                   "||remote-f.example^\n",
                                              "text/plain", "\"v2\""});
  CHECK(runUpdate(service));
  CHECK(blocked(QStringLiteral("https://remote-a.example/x.js")));
  // ... and it survives a restart.
  {
    DaliNiraBlockerService restarted(profile.path());
    restarted.setFilterFetchPolicyForTesting(testPolicy());
    CHECK(decide(restarted, QStringLiteral("https://remote-a.example/x.js"), DaliNiraBlockerResourceType::Script,
                 QStringLiteral("https://a.example/")).kind == NetworkVerdictKind::Block);
  }
  // Keys: order, case, aliases; different rules differ.
  const QSet<QString> keys = DaliNiraBlockerEngine::badfilterKeys(
      QStringLiteral("||k.example^$XHR,Third-Party,badfilter\n!||c.example^$badfilter\n##.x$badfilter\n"));
  CHECK(keys.size() == 1);
  CHECK(DaliNiraBlockerEngine::badfilterKeys(QStringLiteral("||k.example^$third-party,xmlhttprequest,badfilter")) == keys);
  CHECK(DaliNiraBlockerEngine::badfilterKeys(QStringLiteral("||k.example^$third-party,badfilter")) != keys);
  pass("badfilter: same-list, custom (option order/case, domain order), near misses, exceptions, on/off/on, remote recompilation, restart");
}

void testMethodAndResponsePhase(HttpFixture &server) {
  server.reset();
  Profile profile(server);
  DaliNiraBlockerService service(profile.path());
  prepare(service, {QStringLiteral("||post.example^$method=post"), QStringLiteral("||notget.example^$method=~get")});
  const auto xhr = DaliNiraBlockerResourceType::Xhr;
  const QString site = QStringLiteral("https://news.example/");
  CHECK(decide(service, QStringLiteral("https://post.example/c"), xhr, site, QStringLiteral("post")).kind == NetworkVerdictKind::Block);
  CHECK(decide(service, QStringLiteral("https://post.example/c"), xhr, site, QStringLiteral("get")).kind == NetworkVerdictKind::Allow);
  CHECK(decide(service, QStringLiteral("https://notget.example/c"), xhr, site, QStringLiteral("put")).kind == NetworkVerdictKind::Block);
  CHECK(decide(service, QStringLiteral("https://notget.example/c"), xhr, site, QStringLiteral("get")).kind == NetworkVerdictKind::Allow);

  // Response-phase rules (CSP/modifyHeaders, response-header conditions)
  // are left out: they are never applied, and never mistaken for blocks.
  CHECK(service.listManager()->responsePhaseRulesSkipped() == 2);
  CHECK(decide(service, QStringLiteral("https://hdr.gen.example/a.js")).kind == NetworkVerdictKind::Allow);
  CHECK(decide(service, QStringLiteral("https://csp.gen.example/"), DaliNiraBlockerResourceType::MainFrame).kind ==
        NetworkVerdictKind::Allow);
  service.settings()->setCustomFilters({QStringLiteral("||csp.example^$csp=script-src 'none'")});
  CHECK(decide(service, QStringLiteral("https://csp.example/"), DaliNiraBlockerResourceType::MainFrame).kind ==
        NetworkVerdictKind::Allow);
  // The real bundled rulesets.
  QTemporaryDir realProfile;
  DaliNiraBlockerService real(realProfile.path());
  std::cout << "      bundled response-phase rules left out (default selection): "
            << real.listManager()->responsePhaseRulesSkipped() << "\n";
  CHECK(real.listManager()->responsePhaseRulesSkipped() > 0);
  pass("method: =post and =~get; response-phase generated rules and $csp text rules are not enforced and never block");
}

void testLoggerAndConcurrency(HttpFixture &server) {
  server.reset();
  Profile profile(server);
  DaliNiraBlockerService service(profile.path());
  prepare(service, {QStringLiteral("||log.example^$removeparam=trk"), QStringLiteral("||log.example/r.js$script,redirect=noopjs"),
                    QStringLiteral("||log.example/x.js$script,redirect=noopjs"), QStringLiteral("@@||log.example/x.js$redirect-rule"),
                    QStringLiteral("||log.example/b.js"), QStringLiteral("@@||log.example/b.js")});
  auto logged = [&](const QString &url, DaliNiraBlockerResourceType type) {
    decide(service, url, type);
    return service.recentLogs(1).value(0);
  };
  NetworkLogEntry entry = logged(QStringLiteral("https://log.example/p?trk=secret-token&a=1"), DaliNiraBlockerResourceType::Image);
  CHECK(entry.action == DaliNiraBlockerAction::Redirect && entry.reason == QLatin1String("dnr-remove-param"));
  CHECK(!entry.requestUrl.contains(QStringLiteral("secret-token")));
  entry = logged(QStringLiteral("https://log.example/r.js"), DaliNiraBlockerResourceType::Script);
  CHECK(entry.action == DaliNiraBlockerAction::Redirect && entry.reason == QLatin1String("dnr-redirect-resource"));
  entry = logged(QStringLiteral("https://log.example/x.js"), DaliNiraBlockerResourceType::Script);
  CHECK(entry.action == DaliNiraBlockerAction::Block && entry.reason == QLatin1String("redirect-exception-block"));
  entry = logged(QStringLiteral("https://log.example/b.js"), DaliNiraBlockerResourceType::Script);
  CHECK(entry.action == DaliNiraBlockerAction::Allow && entry.reason == QLatin1String("user-allow"));

  // Engine adapters decide on their own threads while plans are swapped.
  const std::shared_ptr<DaliNiraBlockerNetworkGate> gate = service.networkGate();
  std::atomic<bool> stop{false};
  std::atomic<int> samples{0};
  std::atomic<int> invalid{0};
  QThread *reader = QThread::create([&] {
    const QUrl site(QStringLiteral("https://news.example/"));
    while (!stop.load()) {
      const auto a = gate->decide(QUrl(QStringLiteral("https://log.example/p?trk=1")), DaliNiraBlockerResourceType::Image, site,
                                  0x77, QStringLiteral("get"));
      const auto b = gate->decide(QUrl(QStringLiteral("https://log.example/r.js")), DaliNiraBlockerResourceType::Script, site,
                                  0x77, QStringLiteral("get"));
      if (a.kind == NetworkVerdictKind::Redirect && a.redirectUrl != QUrl(QStringLiteral("https://log.example/p"))) ++invalid;
      if (b.kind == NetworkVerdictKind::Redirect && !b.redirectUrl.toString().startsWith(QStringLiteral("data:"))) ++invalid;
      ++samples;
    }
  });
  reader->start();
  for (int i = 0; i < 6; ++i) {
    service.settings()->setCustomFilters(i % 2 ? QStringList{} : QStringList{QStringLiteral("||log.example^$removeparam=trk"),
                                                                            QStringLiteral("||log.example/r.js$script,redirect=noopjs")});
    waitUntil([] { return false; }, 20);
  }
  stop = true;
  reader->wait();
  delete reader;
  CHECK(samples.load() > 10 && invalid.load() == 0);
  pass("logger: parameters removed / replaced / redirect exception / exception labels, no query; concurrent swaps consistent");
}

void testRemoteUpdateWithNewSyntax(HttpFixture &server) {
  server.reset();
  Profile profile(server);
  server.setRoute("/remote.txt", FixtureRoute{200, "[Adblock Plus 2.0]\n! Expires: 2 days\n"
                                                   "||remote-redirect.example/lib.js$script,redirect=noopjs\n"
                                                   "||remote-param.example^$removeparam=rtrk\n"
                                                   "||remote-method.example^$method=post\n"
                                                   "||remote-csp.example^$csp=script-src 'none'\n"
                                                   "||remote-block.example^\n",
                                              "text/plain", "\"s1\""});
  {
    DaliNiraBlockerService service(profile.path());
    prepare(service);
    CHECK(runUpdate(service));
    const FilterListUpdateState state = service.listManager()->filterStore()->state(kRemote);
    CHECK(state.lastResult == QLatin1String("updated") && state.current.networkRules == 4);
    CHECK(decide(service, QStringLiteral("https://remote-redirect.example/lib.js")).kind == NetworkVerdictKind::Redirect);
    CHECK(rewritten(service, QStringLiteral("https://remote-param.example/p?rtrk=1&k=2")) ==
          QLatin1String("https://remote-param.example/p?k=2"));
    CHECK(decide(service, QStringLiteral("https://remote-method.example/c"), DaliNiraBlockerResourceType::Xhr,
                 QStringLiteral("https://news.example/"), QStringLiteral("post")).kind == NetworkVerdictKind::Block);
    CHECK(runUpdate(service));  // a 304 keeps it
    CHECK(service.listManager()->filterStore()->state(kRemote).lastResult == QLatin1String("not-modified"));
  }
  DaliNiraBlockerService restarted(profile.path());
  restarted.setFilterFetchPolicyForTesting(testPolicy());
  CHECK(decide(restarted, QStringLiteral("https://remote-redirect.example/lib.js")).kind == NetworkVerdictKind::Redirect);
  CHECK(server.count("/remote.txt") == 2);
  pass("remote lists: new syntax validates, counts, activates, 304-persists and survives restart");
}

// ---------------- Engine parity ----------------

void testEngineParity(HttpFixture &server) {
  server.reset();
  Profile profile(server);
  FixtureRoute page;
  page.contentType = "text/html";
  page.body = "<!doctype html><html><body>"
              "<img id=p src=\"/pixel.gif?id=1&ptrk=abc&keep=%2F\">"
              "<script src=\"/lib/cb.js\"></script>"
              "<script src=\"/lib/bf.js\"></script>"
              "<script>window.fetchDone=0;"
              "fetch('/m',{method:'POST',body:'x'}).catch(()=>{}).finally(()=>{window.fetchDone|=1});"
              "fetch('/m').catch(()=>{}).finally(()=>{window.fetchDone|=2});"
              "fetch('/api/stub').then(r=>r.text()).then(t=>{window.stubJson=t},()=>{window.stubJson='error'})"
              ".finally(()=>{window.fetchDone|=4});</script>"
              "</body></html>";
  server.setRoute("/page.html", page);
  FixtureRoute ok;
  ok.contentType = "application/javascript";
  ok.body = "window.loadedFromServer=(window.loadedFromServer||0)+1;";
  for (const QByteArray &path : {QByteArray("/lib/cb.js"), QByteArray("/lib/bf.js"), QByteArray("/m"), QByteArray("/pixel.gif")})
    server.setRoute(path, ok);

  DaliNiraBlockerService service(profile.path());
  const QStringList rules = {QStringLiteral("||127.0.0.1^$removeparam=ptrk"),
                             QStringLiteral("/lib/cb.js$script,redirect=chartbeat.js"),
                             QStringLiteral("/lib/bf.js$script"),
                             QStringLiteral("/m$method=post"),
                             QStringLiteral("/api/stub$xmlhttprequest,redirect=noopjson")};
  prepare(service, rules);
  const QString chartbeat = service.listManager()->redirectResources()->dataUrl(QStringLiteral("chartbeat.js"));

  // Chromium: the service's QWebEngineUrlRequestInterceptor on a real page.
  // Set up as BrowserProfileService sets up every profile.
  QWebEngineProfile chromiumProfile;
  chromiumProfile.setUrlRequestInterceptor(service.requestInterceptor());
  CHECK(DaliNiraBlockerRequestInterceptor::resourceSchemeRegistered());
  chromiumProfile.installUrlSchemeHandler(DaliNiraBlockerRequestInterceptor::resourceScheme(),
                                          service.resourceSchemeHandler());
  QWebEnginePage chromiumPage(&chromiumProfile);
  auto evalJs = [&](const QString &script) {
    QVariant result;
    bool done = false;
    chromiumPage.runJavaScript(script, [&](const QVariant &value) { result = value; done = true; });
    CHECK(waitUntil([&] { return done; }, 10000));
    return result;
  };
  auto loadPage = [&]() {
    bool loaded = false;
    const auto connection = QObject::connect(&chromiumPage, &QWebEnginePage::loadFinished, [&](bool) { loaded = true; });
    server.requests.clear();
    chromiumPage.load(QUrl(server.url(QStringLiteral("/page.html"))));
    CHECK(waitUntil([&] { return loaded; }, 30000));
    CHECK(waitUntil([&] { return evalJs(QStringLiteral("window.fetchDone")).toInt() == 7; }, 15000));
    QObject::disconnect(connection);
  };

  // WebKit: the filter's socket server with the evaluator BrowserWindow installs.
  dalinira::engine::WebKitNetworkFilter webkitFilter;
  CHECK(webkitFilter.start());
  const std::shared_ptr<DaliNiraBlockerNetworkGate> gate = service.networkGate();
  constexpr quint64 kPageId = 9;
  webkitFilter.setRoute(kPageId, [gate](const dalinira::engine::WebKitNetworkFilter::Request &request) {
    using Filter = dalinira::engine::WebKitNetworkFilter;
    const NetworkRequestVerdict result = gate->decide(request.url, resourceTypeFromFetchDestination(request.destination),
                                                      request.firstPartyUrl, 0x9191, request.method.toLower());
    switch (result.kind) {
      case NetworkVerdictKind::Block: return Filter::Verdict{Filter::Action::Block, {}};
      case NetworkVerdictKind::Redirect: return Filter::Verdict{Filter::Action::Redirect, result.redirectUrl};
      case NetworkVerdictKind::Allow: break;
    }
    return Filter::Verdict{};
  });
  QLocalSocket socket;
  socket.connectToServer(webkitFilter.socketPath());
  CHECK(socket.waitForConnected(3000));
  socket.write("HELLO\t" + webkitFilter.token() + "\nP\t" + QByteArray::number(kPageId) + "\n");
  int seq = 0;
  const QByteArray document = QUrl(server.url(QStringLiteral("/page.html"))).toEncoded();
  auto webkitAsk = [&](const QString &target, const char *destination, const char *method = "GET") {
    const QByteArray number = QByteArray::number(++seq);
    socket.write("Q\t" + number + "\t" + QByteArray::number(kPageId) + "\t" + method + "\t" + destination + "\t0\t" +
                 QUrl(server.url(target)).toEncoded() + "\t" + document + "\n");
    socket.flush();
    QByteArray line;
    QElapsedTimer timer;
    timer.start();
    while (!line.endsWith('\n') && timer.elapsed() < 5000)
      if (socket.bytesAvailable() || socket.waitForReadyRead(100)) line += socket.readLine();
    CHECK(line.startsWith(number + "\t"));
    return line.mid(number.size() + 1).trimmed();  // "A", "B" or "R\t<url>"
  };
  // A new top-level document, as the extension reports on (re)load.
  auto webkitNavigate = [&]() { CHECK(webkitAsk(QStringLiteral("/page.html"), "document") == "A"); };

  // 1. removeparam, resource redirect, block, method.
  loadPage();
  CHECK(server.count("/pixel.gif") == 1);
  bool pixelStripped = false;
  for (const FixtureRequest &request : std::as_const(server.requests))
    if (request.path == "/pixel.gif") pixelStripped = request.target == "/pixel.gif?id=1&keep=%2F";
  CHECK(pixelStripped);
  CHECK(server.count("/lib/cb.js") == 0);
  CHECK(evalJs(QStringLiteral("typeof window.pSUPERFLY")).toString() == QLatin1String("object"));  // the stub ran
  CHECK(server.count("/lib/bf.js") == 0);
  CHECK(server.count("/m", "GET") == 1 && server.count("/m", "POST") == 0);
  CHECK(server.count("/api/stub") == 0);
  CHECK(evalJs(QStringLiteral("window.stubJson")).toString() == QLatin1String("{}"));  // fetch() got noop.json

  webkitNavigate();
  CHECK(webkitAsk(QStringLiteral("/pixel.gif?id=1&ptrk=abc&keep=%2F"), "image") ==
        "R\t" + QUrl(server.url(QStringLiteral("/pixel.gif?id=1&keep=%2F"))).toEncoded());
  CHECK(webkitAsk(QStringLiteral("/lib/cb.js"), "script") == "R\t" + QUrl(chartbeat).toEncoded());
  CHECK(webkitAsk(QStringLiteral("/lib/bf.js"), "script") == "B");
  CHECK(webkitAsk(QStringLiteral("/m"), "empty", "POST") == "B");
  CHECK(webkitAsk(QStringLiteral("/m"), "empty", "GET") == "A");
  CHECK(webkitAsk(QStringLiteral("/api/stub"), "empty") ==
        "R\t" + QUrl(service.listManager()->redirectResources()->dataUrl(QStringLiteral("noop.json"))).toEncoded());

  // 2. badfilter lifts the block; a redirect exception keeps the block but
  //    drops the substitute.
  service.settings()->setCustomFilters(rules + QStringList{QStringLiteral("/lib/bf.js$script,badfilter"),
                                                           QStringLiteral("@@/lib/cb.js$redirect-rule")});
  loadPage();
  CHECK(server.count("/lib/bf.js") == 1);
  CHECK(server.count("/lib/cb.js") == 0);
  CHECK(evalJs(QStringLiteral("typeof window.pSUPERFLY")).toString() == QLatin1String("undefined"));
  webkitNavigate();
  CHECK(webkitAsk(QStringLiteral("/lib/bf.js"), "script") == "A");
  CHECK(webkitAsk(QStringLiteral("/lib/cb.js"), "script") == "B");

  // 3. Without the badfilter the block is back in both.
  service.settings()->setCustomFilters(rules);
  loadPage();
  CHECK(server.count("/lib/bf.js") == 0);
  webkitNavigate();
  CHECK(webkitAsk(QStringLiteral("/lib/bf.js"), "script") == "B");

  // 4. Debouncing ($urlskip): a top-level navigation to a redirector goes
  //    straight to its destination on another site (127.0.0.2), and the
  //    redirector is never contacted.
  HttpFixture destination(QHostAddress(QStringLiteral("127.0.0.2")));
  FixtureRoute landing;
  landing.contentType = "text/html";
  landing.body = "<!doctype html><title>landing</title>landed";
  destination.setRoute("/landing", landing);
  service.settings()->setCustomFilters(rules + QStringList{QStringLiteral("/bounce?$urlskip=?to")});
  const QString landingUrl = destination.url(QStringLiteral("/landing"));
  const QString bouncePath =
      QStringLiteral("/bounce?to=") + QString::fromLatin1(QUrl::toPercentEncoding(landingUrl));
  {
    bool loaded = false;
    const auto connection = QObject::connect(&chromiumPage, &QWebEnginePage::loadFinished, [&](bool) { loaded = true; });
    server.requests.clear();
    chromiumPage.load(QUrl(server.url(bouncePath)));
    CHECK(waitUntil([&] { return loaded; }, 30000));
    QObject::disconnect(connection);
  }
  CHECK(chromiumPage.url() == QUrl(landingUrl));
  CHECK(server.count("/bounce") == 0 && destination.count("/landing") == 1);
  CHECK(webkitAsk(bouncePath, "document") == "R\t" + QUrl(landingUrl).toEncoded());
  CHECK(webkitAsk(bouncePath, "iframe") == "A");  // top-level navigations only
  socket.disconnectFromServer();
  pass("parity: removeparam, resource redirect (stub executed), redirect exception, badfilter on/off, $method, "
       "$urlskip debouncing via Chromium page and WebKit socket");
}

}  // namespace

int main(int argc, char *argv[]) {
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
  if (qEnvironmentVariableIsEmpty("QTWEBENGINE_CHROMIUM_FLAGS"))
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", "--no-sandbox --disable-gpu");
  qputenv("QTWEBENGINE_DISABLE_SANDBOX", "1");
  DaliNiraBlockerRequestInterceptor::registerResourceScheme();  // as main() does
  QApplication app(argc, argv);
  if (argc == 4 && QString::fromLocal8Bit(argv[1]) == QLatin1String("--analyze-list")) {
    QFile file(QString::fromLocal8Bit(argv[2]));
    CHECK(file.open(QIODevice::ReadOnly));
    const QByteArray bytes = file.read(64 * 1024 * 1024 + 1);
    CHECK(bytes.size() <= 64 * 1024 * 1024);
    const DaliNiraBlockerRedirectResources resources(kSourceResources);
    const auto compiled = DaliNiraBlockerEngine::compileTextFilterList(
        QString::fromUtf8(bytes), QString::fromLocal8Bit(argv[3]), {&resources, nullptr});
    const auto plan = DaliNiraBlockerEngine::compilePlan(compiled.rules, QString(), {}, {&resources, nullptr});
    QJsonObject skipped;
    for (auto it = compiled.skippedByReason.cbegin(); it != compiled.skippedByReason.cend(); ++it)
      skipped.insert(it.key(), it.value());
    const QJsonObject report{
        {QStringLiteral("bytes"), bytes.size()},
        {QStringLiteral("networkRules"), compiled.networkRules},
        {QStringLiteral("engineRules"), compiled.rules.size()},
        {QStringLiteral("networkEngineRules"), plan.rules.size()},
        {QStringLiteral("popupEngineRules"), plan.popupRules.rules.size()},
        {QStringLiteral("genericHideEngineRules"), plan.genericHideRules.rules.size()},
        {QStringLiteral("cosmeticLines"), compiled.cosmeticLines},
        {QStringLiteral("genericCosmeticLines"), compiled.genericCosmeticLines},
        {QStringLiteral("siteCosmeticLines"), compiled.siteCosmeticLines},
        {QStringLiteral("proceduralCosmeticLines"), compiled.proceduralCosmeticLines},
        {QStringLiteral("scriptletCosmeticLines"), compiled.scriptletCosmeticLines},
        {QStringLiteral("unsupportedLines"), compiled.unsupportedLines},
        {QStringLiteral("popupRules"), compiled.popupRules},
        {QStringLiteral("popupExceptions"), compiled.popupExceptions},
        {QStringLiteral("genericHideRules"), compiled.genericHideRules},
        {QStringLiteral("redirectRules"), compiled.redirectRules},
        {QStringLiteral("rewriteRules"), compiled.rewriteRules},
        {QStringLiteral("removeparamRules"), compiled.removeparamRules},
        {QStringLiteral("modifierExceptions"), compiled.modifierExceptions},
        {QStringLiteral("blockRules"), compiled.blockRules},
        {QStringLiteral("allowRules"), compiled.allowRules},
        {QStringLiteral("typedNetworkRules"), compiled.typedNetworkRules},
        {QStringLiteral("partyNetworkRules"), compiled.partyNetworkRules},
        {QStringLiteral("domainNetworkRules"), compiled.domainNetworkRules},
        {QStringLiteral("methodNetworkRules"), compiled.methodNetworkRules},
        {QStringLiteral("regexNetworkRules"), compiled.regexNetworkRules},
        {QStringLiteral("importantNetworkRules"), compiled.importantNetworkRules},
        {QStringLiteral("badfilterLines"), compiled.badfilterLines},
        {QStringLiteral("disabledByBadfilter"), compiled.disabledByBadfilter},
        {QStringLiteral("skippedByReason"), skipped}};
    std::cout << QJsonDocument(report).toJson(QJsonDocument::Indented).constData();
    return 0;
  }
  std::cout << "[DaliNira blocker filter syntax]\n";
  HttpFixture server;
  testParserAndValidator();
  testRemoveParam(server);
  testRedirect(server);
  testRealListRedirectLines();
  testBadfilter(server);
  testMethodAndResponsePhase(server);
  testLoggerAndConcurrency(server);
  testRemoteUpdateWithNewSyntax(server);
  testEngineParity(server);
  std::cout << "ALL FILTER SYNTAX CHECKS PASSED\n";
  return 0;
}
