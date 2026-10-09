// Remote filter-list updates of the native blocker (Step 1 hardening).
//
// Everything is served by an in-process loopback HTTP fixture: no public
// Internet service is contacted. Each case uses its own temporary profile
// with a small fixture ruleset catalog, so no real profile, credentials or
// filter data are read or written.
//
// Engine parity is exercised through the real adapters: the Chromium
// QWebEngineUrlRequestInterceptor on an off-the-record offscreen profile, and
// the WebKit network filter's socket server spoken to exactly as the web
// process extension does.

#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <atomic>
#include <cstdlib>
#include <functional>
#include <iostream>

#include "dalinira_blocker_engine.h"
#include "dalinira_blocker_filter_updates.h"
#include "dalinira_blocker_list_manager.h"
#include "dalinira_blocker_service.h"
#include "engine/webkit/webkit_network_filter.h"
#include "i18n/i18n.h"

#define CHECK(condition)                                                                    \
  do {                                                                                      \
    if (!(condition)) {                                                                     \
      std::cerr << "CHECK FAILED " << __FILE__ << ":" << __LINE__ << ": " #condition "\n"; \
      std::exit(1);                                                                         \
    }                                                                                       \
  } while (false)

namespace {

bool waitUntil(const std::function<bool()> &condition, int timeoutMs) {
  QElapsedTimer timer;
  timer.start();
  while (!condition()) {
    if (timer.elapsed() > timeoutMs) return false;
    QEventLoop loop;
    QTimer::singleShot(10, &loop, &QEventLoop::quit);
    loop.exec();
  }
  return true;
}

// ---------------- Loopback HTTP fixture ----------------

struct Route {
  int status = 200;
  QByteArray body;
  QByteArray contentType = "text/plain; charset=utf-8";
  QByteArray etag;
  QByteArray lastModified;
  QByteArray location;
  bool hang = false;               // accept and never answer
  bool omitContentLength = false;  // body ends when the connection closes
  qint64 declaredLength = -1;      // a Content-Length larger than the body: truncation
};

struct RecordedRequest {
  QByteArray path;
  QByteArray ifNoneMatch;
  QByteArray ifModifiedSince;
  QByteArray cookie;
};

class FixtureServer final : public QObject {
 public:
  FixtureServer() {
    CHECK(server_.listen(QHostAddress::LocalHost, 0));
    connect(&server_, &QTcpServer::newConnection, this, [this] {
      while (QTcpSocket *socket = server_.nextPendingConnection()) {
        socket->setParent(this);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] { handle(socket); });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
      }
    });
  }
  quint16 port() const { return server_.serverPort(); }
  QString url(const QString &path) const {
    return QStringLiteral("http://127.0.0.1:%1%2").arg(port()).arg(path);
  }
  void setRoute(const QByteArray &path, const Route &route) { routes_[path] = route; }
  void reset() {
    routes_.clear();
    requests.clear();
  }
  int count(const QByteArray &path) const {
    int total = 0;
    for (const RecordedRequest &request : requests) total += request.path == path ? 1 : 0;
    return total;
  }
  const RecordedRequest *last(const QByteArray &path) const {
    for (auto it = requests.crbegin(); it != requests.crend(); ++it)
      if (it->path == path) return &*it;
    return nullptr;
  }
  QList<RecordedRequest> requests;

 private:
  void handle(QTcpSocket *socket) {
    QByteArray &buffer = buffers_[socket];
    buffer += socket->readAll();
    const qsizetype end = buffer.indexOf("\r\n\r\n");
    if (end < 0) return;
    const QList<QByteArray> lines = buffer.left(end).split('\n');
    buffer.clear();
    RecordedRequest request;
    const QList<QByteArray> requestLine = lines.value(0).trimmed().split(' ');
    request.path = requestLine.value(1);
    for (const QByteArray &raw : lines.mid(1)) {
      const QByteArray line = raw.trimmed();
      const qsizetype colon = line.indexOf(':');
      if (colon <= 0) continue;
      const QByteArray name = line.left(colon).trimmed().toLower();
      const QByteArray value = line.mid(colon + 1).trimmed();
      if (name == "if-none-match") request.ifNoneMatch = value;
      else if (name == "if-modified-since") request.ifModifiedSince = value;
      else if (name == "cookie") request.cookie = value;
    }
    requests.append(request);
    const Route route = routes_.value(request.path, Route{404, "missing", "text/plain", {}, {}, {}, false, false, -1});
    if (route.hang) return;
    int status = route.status;
    QByteArray body = route.body;
    const bool conditionalHit = status == 200 &&
        ((!route.etag.isEmpty() && request.ifNoneMatch == route.etag) ||
         (!route.lastModified.isEmpty() && request.ifModifiedSince == route.lastModified));
    if (conditionalHit) {
      status = 304;
      body.clear();
    }
    QByteArray response = "HTTP/1.1 " + QByteArray::number(status) + " Fixture\r\n";
    if (!body.isEmpty()) response += "Content-Type: " + route.contentType + "\r\n";
    if (!route.etag.isEmpty()) response += "ETag: " + route.etag + "\r\n";
    if (!route.lastModified.isEmpty()) response += "Last-Modified: " + route.lastModified + "\r\n";
    if (!route.location.isEmpty()) response += "Location: " + route.location + "\r\n";
    if (!route.omitContentLength)
      response += "Content-Length: " +
                  QByteArray::number(route.declaredLength >= 0 ? route.declaredLength : body.size()) + "\r\n";
    // Every request must reach the fixture: nothing may be answered from a cache.
    response += "Cache-Control: no-store\r\nConnection: close\r\n\r\n";
    socket->write(response + body);
    socket->disconnectFromHost();
  }

  QTcpServer server_;
  QHash<QByteArray, Route> routes_;
  QHash<QTcpSocket *, QByteArray> buffers_;
};

// ---------------- Fixture profile ----------------

const QString kAds = QStringLiteral("fixture-ads");
const QString kPrivacy = QStringLiteral("fixture-privacy");
const QString kBundledOnly = QStringLiteral("fixture-bundled-only");
const QString kReview = QStringLiteral("fixture-review");

QByteArray listText(const QStringList &rules, const QString &expires = QStringLiteral("2 days")) {
  QString text = QStringLiteral("[Adblock Plus 2.0]\n! Title: Fixture\n! Expires: %1\n").arg(expires);
  for (const QString &rule : rules) text += rule + QLatin1Char('\n');
  return text.toUtf8();
}

// Version 1 of the ads list, with syntax the compiler must skip.
QByteArray adsV1() {
  return listText({QStringLiteral("||remote-ads-v1.example^"),
                   QStringLiteral("||remote-ads-v1b.example^$third-party"),
                   QStringLiteral("/dalinira-remote-new-ad.js"),
                   QStringLiteral("||remote-allowed.example^"),
                   QStringLiteral("@@||remote-allowed.example^"),
                   QStringLiteral("example.com##.ad-banner"),
                   QStringLiteral("||unsupported.example^$csp=script-src 'none'"),
                   QStringLiteral("!#if env_firefox"),
                   QStringLiteral("||conditional.example^"),
                   QStringLiteral("!#endif")});
}

QByteArray adsV2() {
  return listText({QStringLiteral("||remote-ads-v2.example^"), QStringLiteral("||remote-ads-v2b.example^"),
                   QStringLiteral("/dalinira-remote-new-ad.js"), QStringLiteral("||remote-ads-v2c.example^")});
}

QByteArray privacyV1() {
  return listText({QStringLiteral("||remote-tracker.example^"), QStringLiteral("||remote-tracker2.example^"),
                   QStringLiteral("/remote-pixel.gif$image"), QStringLiteral("||remote-tracker3.example^")});
}

void writeJson(const QString &path, const QJsonDocument &document) {
  QFile file(path);
  CHECK(file.open(QIODevice::WriteOnly));
  file.write(document.toJson());
}

class Profile {
 public:
  explicit Profile(const FixtureServer &server) {
    CHECK(dir_.isValid());
    const QString base = rulesetDir();
    CHECK(QDir().mkpath(base + QStringLiteral("/main")));
    QJsonArray details;
    QJsonObject attributions;
    const QList<QPair<QString, QString>> bundled = {{kAds, QStringLiteral("bundled-ads.example")},
                                                   {kPrivacy, QStringLiteral("bundled-tracker.example")},
                                                   {kBundledOnly, QStringLiteral("bundled-only.example")},
                                                   {kReview, QStringLiteral("bundled-review.example")}};
    for (const auto &[id, host] : bundled) {
      details.append(QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("name"), id},
                                 {QStringLiteral("group"), QStringLiteral("default")},
                                 {QStringLiteral("enabled"), true},
                                 {QStringLiteral("rules"), QJsonObject{{QStringLiteral("total"), 1}}}});
      attributions.insert(id, QJsonObject{{QStringLiteral("provider"), QStringLiteral("Fixture")},
                                          {QStringLiteral("source"), id},
                                          {QStringLiteral("sourceUrl"), server.url(QStringLiteral("/") + id)},
                                          {QStringLiteral("license"), QStringLiteral("Fixture")},
                                          {QStringLiteral("reviewRequired"), id == kReview}});
      writeJson(base + QStringLiteral("/main/") + id + QStringLiteral(".json"),
                QJsonDocument(QJsonArray{QJsonObject{
                    {QStringLiteral("id"), 1}, {QStringLiteral("priority"), 10},
                    {QStringLiteral("action"), QJsonObject{{QStringLiteral("type"), QStringLiteral("block")}}},
                    {QStringLiteral("condition"),
                     QJsonObject{{QStringLiteral("requestDomains"), QJsonArray{host}}}}}}));
    }
    writeJson(base + QStringLiteral("/ruleset-details.json"), QJsonDocument(details));
    writeJson(base + QStringLiteral("/ruleset-attributions.json"),
              QJsonDocument(QJsonObject{{QStringLiteral("rulesets"), attributions}}));
    auto source = [&server](const QString &path) {
      return QJsonObject{{QStringLiteral("url"), server.url(path)},
                         {QStringLiteral("format"), QStringLiteral("adblock-plus-text")},
                         {QStringLiteral("minimumNetworkRules"), 3},
                         {QStringLiteral("maximumBytes"), 65536}};
    };
    // fixture-review is named but must never be downloaded (reviewRequired).
    writeJson(base + QStringLiteral("/filter-update-sources.json"),
              QJsonDocument(QJsonObject{{QStringLiteral("sources"),
                                         QJsonObject{{kAds, source(QStringLiteral("/ads.txt"))},
                                                     {kPrivacy, source(QStringLiteral("/privacy.txt"))},
                                                     {kReview, source(QStringLiteral("/review.txt"))}}}}));
  }
  QString path() const { return dir_.path(); }
  QString rulesetDir() const { return dir_.path() + QStringLiteral("/adblock/rulesets"); }
  QString storeDir() const { return dir_.path() + QStringLiteral("/adblock-filter-updates"); }

 private:
  QTemporaryDir dir_;
};

DaliNiraBlockerFilterFetcher::Policy testPolicy(int transferTimeoutMs = 3000) {
  DaliNiraBlockerFilterFetcher::Policy policy;
  policy.transferTimeoutMs = transferTimeoutMs;
  policy.totalTimeoutMs = transferTimeoutMs * 3;
  policy.maximumRedirects = 3;
  policy.allowLoopbackHttpForTesting = true;
  return policy;
}

void prepare(DaliNiraBlockerService &service, const QStringList &enabled = {kAds, kPrivacy, kBundledOnly, kReview}) {
  service.setFilterFetchPolicyForTesting(testPolicy());
  service.settings()->setAutoReloadOnModeChange(false);
  service.settings()->setEnabledRulesetIds(enabled);
}

struct UpdateOutcome {
  bool finished = false;
  bool success = false;
  QString message;
};

UpdateOutcome runUpdate(DaliNiraBlockerService &service, int timeoutMs = 20000) {
  UpdateOutcome outcome;
  const auto connection = QObject::connect(&service, &DaliNiraBlockerService::filterUpdateFinished,
                                           [&outcome](bool ok, const QString &message) {
                                             outcome.finished = true;
                                             outcome.success = ok;
                                             outcome.message = message;
                                           });
  service.updateFiltersAsync();
  CHECK(waitUntil([&outcome] { return outcome.finished; }, timeoutMs));
  QObject::disconnect(connection);
  return outcome;
}

NetworkVerdictKind verdict(DaliNiraBlockerService &service, const QString &url,
                           DaliNiraBlockerResourceType type = DaliNiraBlockerResourceType::Script) {
  return service.decideNetworkRequest(QUrl(url), type, QUrl(QStringLiteral("https://news.example.org/")), 0x77,
                                      QStringLiteral("get")).kind;
}

bool blocks(DaliNiraBlockerService &service, const QString &host) {
  return verdict(service, QStringLiteral("https://%1/asset.js").arg(host)) == NetworkVerdictKind::Block;
}

FilterListInfo listInfo(DaliNiraBlockerService &service, const QString &id) {
  for (const FilterListInfo &info : service.listManager()->availableLists())
    if (info.id == id) return info;
  CHECK(false);
  return {};
}

Route listRoute(const QByteArray &body, const QByteArray &etag = {}, const QByteArray &lastModified = {}) {
  Route route;
  route.body = body;
  route.etag = etag;
  route.lastModified = lastModified;
  return route;
}

// Every file of the profile's update store, relative to the profile.
QStringList storeFiles(const Profile &profile) {
  QStringList files;
  QDirIterator it(profile.path(), QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
  while (it.hasNext()) {
    const QString path = it.next();
    if (path.contains(QStringLiteral("/adblock-filter-updates/")))
      files.append(QDir(profile.path()).relativeFilePath(path));
  }
  files.sort();
  return files;
}

void corrupt(const QString &path) {
  QFile file(path);
  CHECK(file.open(QIODevice::ReadWrite));
  file.seek(file.size() / 2);
  file.write("#corrupted#");
}

void pass(const char *name) { std::cout << "  PASS " << name << "\n"; }

// ---------------- Cases ----------------

void testCompiler() {
  const QString text = QString::fromUtf8(adsV1()) +
                       QStringLiteral("||important.example^$important\n@@||important.example^\n");
  const auto compiled = DaliNiraBlockerEngine::compileTextFilterList(text, kAds);
  // v1: 5 network rules, +2 above; the cosmetic, removeparam and #if lines are skipped.
  CHECK(compiled.networkRules == 7);
  CHECK(compiled.cosmeticLines == 1);
  CHECK(compiled.unsupportedLines == 2);  // $csp (needs response headers), and the line inside !#if
  bool mergedHosts = false;
  for (const FilterRule &rule : compiled.rules) {
    CHECK(rule.rulesetId == kAds);
    if (rule.requestDomains.contains(QStringLiteral("remote-ads-v1.example"))) {
      mergedHosts = rule.urlFilter.isEmpty() && rule.priority == 10 && rule.actionType == QLatin1String("block");
    }
    if (rule.actionType == QLatin1String("allow")) CHECK(rule.priority == 30);
    if (rule.requestDomains.contains(QStringLiteral("important.example")) && rule.actionType == QLatin1String("block"))
      CHECK(rule.priority == 40);
  }
  CHECK(mergedHosts);

  // Merged host groups stay bounded.
  QString many;
  for (int i = 0; i < 2500; ++i) many += QStringLiteral("||host%1.example^\n").arg(i);
  const auto chunked = DaliNiraBlockerEngine::compileTextFilterList(many, kAds);
  CHECK(chunked.networkRules == 2500 && chunked.rules.size() == 3);
  for (const FilterRule &rule : chunked.rules) CHECK(rule.requestDomains.size() <= 1000);

  // Same rules through the engine: exception beats block, $important beats exception.
  DaliNiraBlockerEngine engine;
  engine.applyCompiledPlan(DaliNiraBlockerEngine::compilePlan(compiled.rules, QString(), {}));
  auto decide = [&engine](const QString &url) {
    return engine.evaluate(QUrl(url), DaliNiraBlockerResourceType::Script, QStringLiteral("news.example.org"),
                           DaliNiraBlockerMode::Ideal, SitePolicy{}).action;
  };
  CHECK(decide(QStringLiteral("https://sub.remote-ads-v1.example/a.js")) == DaliNiraBlockerAction::Block);
  CHECK(decide(QStringLiteral("https://remote-allowed.example/a.js")) == DaliNiraBlockerAction::Allow);
  CHECK(decide(QStringLiteral("https://important.example/a.js")) == DaliNiraBlockerAction::Block);
  CHECK(decide(QStringLiteral("https://unsupported.example/a.js?foo=1")) == DaliNiraBlockerAction::Allow);
  CHECK(decide(QStringLiteral("https://conditional.example/a.js")) == DaliNiraBlockerAction::Allow);
  CHECK(decide(QStringLiteral("https://cdn.example.net/x/dalinira-remote-new-ad.js")) == DaliNiraBlockerAction::Block);

  // $popup is supported by Step 3. Keep this validator check aimed at a
  // genuinely unsupported network capability (CNAME uncloaking).
  CHECK(DaliNiraBlockerEngine::validateCustomFilterLine(QStringLiteral("||a.example^$cname")) ==
        I18n::text(QStringLiteral("blocker.err_unsupported_option")).arg(QStringLiteral("cname")));
  CHECK(DaliNiraBlockerEngine::validateCustomFilterLine(QStringLiteral("/")) ==
        I18n::text(QStringLiteral("blocker.err_regex_unclosed")));
  CHECK(!DaliNiraBlockerEngine::validateCustomFilterLine(QStringLiteral("/ad(/$script")).isEmpty());
  // Path patterns starting with "/" (a quarter of EasyList's network rules)
  // are patterns, not unterminated regular expressions.
  const auto paths = DaliNiraBlockerEngine::compileTextFilterList(
      QStringLiteral("/banner/ads.js$script\n/ad[0-9]+\\.js/$image\n/adserver.\n"), kAds);
  CHECK(paths.networkRules == 3 && paths.unsupportedLines == 0);
  CHECK(paths.rules.at(0).urlFilter == QLatin1String("/banner/ads.js") &&
        paths.rules.at(0).resourceTypes.contains(DaliNiraBlockerResourceType::Script));
  CHECK(paths.rules.at(1).regexFilter == QLatin1String("ad[0-9]+\\.js") &&
        paths.rules.at(1).resourceTypes.contains(DaliNiraBlockerResourceType::Image));
  CHECK(paths.rules.at(2).urlFilter == QLatin1String("/adserver."));
  CHECK(DaliNiraBlockerEngine::validateCustomFilterLine(QStringLiteral("/banner/ads.js$script")).isEmpty());
  CHECK(DaliNiraBlockerEngine::validateCustomFilterLine(QStringLiteral("||ok.example^$script,third-party")).isEmpty());
  pass("compiler: remote text uses the custom-filter line syntax, uBOL priorities, bounded host merging");
}

void testValidation() {
  FilterUpdateSource source;
  source.listId = kAds;
  source.minimumNetworkRules = 3;
  source.maximumBytes = 65536;
  auto failure = [&source](const QByteArray &bytes, const QString &type = QStringLiteral("text/plain"), int previous = 0) {
    return validateFilterListDownload(bytes, type, source, previous).failure;
  };
  CHECK(failure(adsV1()) == FilterUpdateFailure::None);
  CHECK(validateFilterListDownload(adsV1(), QStringLiteral("text/plain"), source, 0).copy.expiresSeconds == 2 * 86400);
  CHECK(failure(QByteArray()) == FilterUpdateFailure::Empty);
  CHECK(failure(QByteArray(" \n\t\n")) == FilterUpdateFailure::Empty);
  CHECK(failure(QByteArray("<!DOCTYPE html><html><body>Sign in to Wi-Fi</body></html>")) == FilterUpdateFailure::Html);
  CHECK(failure(adsV1(), QStringLiteral("text/html; charset=utf-8")) == FilterUpdateFailure::Html);
  CHECK(failure(QByteArray("\xEF\xBB\xBF  <html>")) == FilterUpdateFailure::Html);
  CHECK(failure(QByteArray("||a.example^\n\xC3\x28||b.example^\n||c.example^\n")) == FilterUpdateFailure::Invalid);
  CHECK(failure(QByteArray("||a.example^\n||b.example^\n||c.example^\n\xE2\x82")) == FilterUpdateFailure::Invalid);
  CHECK(failure(QByteArray("||a.example^\n||b.example^\0\n||c.example^\n", 40)) == FilterUpdateFailure::Invalid);
  CHECK(failure(listText({QStringLiteral("||only-one.example^")})) == FilterUpdateFailure::TooFewRules);
  CHECK(failure(listText({QStringLiteral("||a.example^$csp=script-src"), QStringLiteral("||b.example^$redirect=noop.js"),
                          QStringLiteral("||c.example^$badfilter"), QStringLiteral("##.ad")})) ==
        FilterUpdateFailure::TooFewRules);
  CHECK(failure(QByteArray(70000, 'a')) == FilterUpdateFailure::Oversized);
  CHECK(failure(adsV1(), QStringLiteral("text/plain"), 100) == FilterUpdateFailure::Shrunk);
  // Legitimate syntax without a header or comments is accepted.
  CHECK(failure(QByteArray("||a.example^\r\n||b.example^\r\n/banner/*$image\r\n")) == FilterUpdateFailure::None);
  pass("validation: empty, HTML, invalid UTF-8/binary, truncated, oversized, too few rules and shrink rejected");
}

void testUrlPolicy() {
  CHECK(DaliNiraBlockerFilterFetcher::isAllowedUrl(QUrl(QStringLiteral("https://easylist.to/easylist/easylist.txt")), false));
  CHECK(!DaliNiraBlockerFilterFetcher::isAllowedUrl(QUrl(QStringLiteral("http://easylist.to/easylist/easylist.txt")), false));
  CHECK(!DaliNiraBlockerFilterFetcher::isAllowedUrl(QUrl(QStringLiteral("http://easylist.to/easylist.txt")), true));
  CHECK(!DaliNiraBlockerFilterFetcher::isAllowedUrl(QUrl(QStringLiteral("http://127.0.0.1:9/list.txt")), false));
  CHECK(DaliNiraBlockerFilterFetcher::isAllowedUrl(QUrl(QStringLiteral("http://127.0.0.1:9/list.txt")), true));
  CHECK(!DaliNiraBlockerFilterFetcher::isAllowedUrl(QUrl(QStringLiteral("https://user:pw@easylist.to/x.txt")), false));
  CHECK(!DaliNiraBlockerFilterFetcher::isAllowedUrl(QUrl(QStringLiteral("file:///etc/passwd")), true));
  CHECK(!DaliNiraBlockerFilterFetcher::isAllowedUrl(QUrl(QStringLiteral("ftp://easylist.to/x.txt")), false));
  pass("url policy: HTTPS only (loopback http solely under the test policy), no credentials, no other schemes");
}

void testBundledManifest() {
  // The shipped source-of-truth: only lists whose attribution names the list
  // file itself, never review-required ones, all HTTPS.
  const QString base = QStringLiteral(DALINIRA_SOURCE_DIR "/browser/resources/adblock/rulesets");
  QFile manifestFile(base + QStringLiteral("/filter-update-sources.json"));
  QFile attributionFile(base + QStringLiteral("/ruleset-attributions.json"));
  CHECK(manifestFile.open(QIODevice::ReadOnly) && attributionFile.open(QIODevice::ReadOnly));
  const QJsonObject sources = QJsonDocument::fromJson(manifestFile.readAll()).object().value(QStringLiteral("sources")).toObject();
  const QJsonObject attributions =
      QJsonDocument::fromJson(attributionFile.readAll()).object().value(QStringLiteral("rulesets")).toObject();
  CHECK(!sources.isEmpty());
  for (auto it = sources.constBegin(); it != sources.constEnd(); ++it) {
    const QJsonObject attribution = attributions.value(it.key()).toObject();
    const QString url = it.value().toObject().value(QStringLiteral("url")).toString();
    CHECK(!attribution.isEmpty());
    CHECK(!attribution.value(QStringLiteral("reviewRequired")).toBool(true));
    CHECK(attribution.value(QStringLiteral("sourceUrl")).toString() == url);
    CHECK(DaliNiraBlockerFilterFetcher::isAllowedUrl(QUrl(url), false));
  }

  // A real profile with the bundled resources: the store knows exactly these
  // sources, nothing is downloaded at construction, and no automatic update
  // is scheduled while the (default off) setting is off.
  QTemporaryDir profile;
  DaliNiraBlockerService service(profile.path());
  CHECK(service.listManager()->filterStore());
  CHECK(service.listManager()->filterStore()->sources().size() == sources.size());
  CHECK(!service.settings()->autoUpdateRulesets());
  CHECK(!service.nextAutomaticFilterUpdate().isValid());
  CHECK(!QFileInfo::exists(profile.path() + QStringLiteral("/adblock-filter-updates")));
  for (const FilterListInfo &info : service.listManager()->availableLists()) {
    CHECK(info.activeSource == QLatin1String("bundled"));
    CHECK(info.updateUrl.isEmpty() == !sources.contains(info.id));
  }
  pass("bundled manifest: HTTPS list files from existing attribution, no review-required list, off by default");
}

void testBundledFallbackOffline(FixtureServer &server) {
  server.reset();
  Profile profile(server);
  DaliNiraBlockerService service(profile.path());
  prepare(service);
  CHECK(blocks(service, QStringLiteral("bundled-ads.example")));
  CHECK(blocks(service, QStringLiteral("bundled-tracker.example")));
  CHECK(!blocks(service, QStringLiteral("remote-ads-v1.example")));
  CHECK(listInfo(service, kAds).activeSource == QLatin1String("bundled"));
  CHECK(listInfo(service, kAds).updateUrl == server.url(QStringLiteral("/ads.txt")));
  CHECK(listInfo(service, kBundledOnly).updateUrl.isEmpty());
  CHECK(listInfo(service, kReview).updateUrl.isEmpty());

  // "Offline": the fixture refuses connections on this port.
  QTcpServer closed;
  CHECK(closed.listen(QHostAddress::LocalHost, 0));
  const quint16 deadPort = closed.serverPort();
  closed.close();
  QFile manifest(profile.rulesetDir() + QStringLiteral("/filter-update-sources.json"));
  CHECK(manifest.open(QIODevice::ReadOnly));
  QByteArray json = manifest.readAll();
  manifest.close();
  json.replace(QByteArray::number(server.port()), QByteArray::number(deadPort));
  CHECK(manifest.open(QIODevice::WriteOnly | QIODevice::Truncate));
  manifest.write(json);
  manifest.close();

  DaliNiraBlockerService offline(profile.path());
  prepare(offline);
  const UpdateOutcome outcome = runUpdate(offline);
  CHECK(!outcome.success);
  CHECK(outcome.message.contains(QString::number(2)));  // both lists failed
  CHECK(blocks(offline, QStringLiteral("bundled-ads.example")));
  CHECK(blocks(offline, QStringLiteral("bundled-tracker.example")));
  const FilterListInfo ads = listInfo(offline, kAds);
  CHECK(ads.activeSource == QLatin1String("bundled"));
  CHECK(ads.lastUpdateResult == QLatin1String("failed") && ads.lastUpdateFailure == QLatin1String("network"));
  CHECK(server.count("/review.txt") == 0);
  pass("1 bundled fallback: blocks from the bundled copy, and an offline update keeps it");
}

void testSuccessEtagAndRestart(FixtureServer &server) {
  server.reset();
  Profile profile(server);
  server.setRoute("/ads.txt", listRoute(adsV1(), "\"ads-v1\""));
  server.setRoute("/privacy.txt", listRoute(privacyV1(), {}, "Wed, 01 Jul 2026 10:00:00 GMT"));
  {
    DaliNiraBlockerService service(profile.path());
    prepare(service);
    CHECK(blocks(service, QStringLiteral("bundled-ads.example")));
    int started = 0;
    int progressMax = 0;
    QObject::connect(&service, &DaliNiraBlockerService::filterUpdateStarted, [&started] { ++started; });
    QObject::connect(&service, &DaliNiraBlockerService::filterUpdateProgress,
                     [&progressMax](int done, int total, const QString &) { progressMax = qMax(progressMax, done); CHECK(done <= total); });
    const UpdateOutcome outcome = runUpdate(service);
    CHECK(outcome.success);
    CHECK(started == 1 && progressMax == 2 + 3);
    CHECK(outcome.message == I18n::text(QStringLiteral("blocker.filters_update_summary")).arg(2).arg(0).arg(0));
    // The downloaded copy replaced the list's bundled network rules.
    CHECK(blocks(service, QStringLiteral("remote-ads-v1.example")));
    CHECK(!blocks(service, QStringLiteral("bundled-ads.example")));
    CHECK(blocks(service, QStringLiteral("remote-tracker.example")));
    CHECK(!blocks(service, QStringLiteral("bundled-tracker.example")));
    CHECK(blocks(service, QStringLiteral("bundled-only.example")));  // untouched list
    CHECK(!blocks(service, QStringLiteral("remote-allowed.example")));
    CHECK(!blocks(service, QStringLiteral("unsupported.example")));
    // Tracker classification and the request logger see the list's id.
    const NetworkRequestVerdict tracker = service.decideNetworkRequest(
        QUrl(QStringLiteral("https://remote-tracker.example/p.js?uid=secret")), DaliNiraBlockerResourceType::Script,
        QUrl(QStringLiteral("https://news.example.org/")), 0x78, QStringLiteral("get"));
    CHECK(tracker.decision.rulesetId == kPrivacy);
    CHECK(service.statsForTab(0x78).blockedTrackers == 1);
    const NetworkLogEntry logged = service.recentLogs(1).value(0);
    CHECK(logged.rulesetId == kPrivacy && !logged.requestUrl.contains(QStringLiteral("secret")));

    const FilterListInfo ads = listInfo(service, kAds);
    CHECK(ads.activeSource == QLatin1String("remote") && ads.lastUpdateResult == QLatin1String("updated"));
    CHECK(ads.version.startsWith(QStringLiteral("remote-")) && ads.ruleCount == 5);
    CHECK(server.last("/ads.txt")->ifNoneMatch.isEmpty() && server.last("/ads.txt")->cookie.isEmpty());
    const QString sha = service.listManager()->filterStore()->state(kAds).current.sha256;

    // 3. ETag: the next check is conditional and a 304 keeps the valid copy.
    const UpdateOutcome second = runUpdate(service);
    CHECK(second.success);
    CHECK(server.last("/ads.txt")->ifNoneMatch == "\"ads-v1\"");
    CHECK(second.message == I18n::text(QStringLiteral("blocker.filters_update_summary")).arg(0).arg(2).arg(0));
    CHECK(service.listManager()->filterStore()->state(kAds).lastResult == QLatin1String("not-modified"));
    CHECK(service.listManager()->filterStore()->state(kAds).current.sha256 == sha);
    CHECK(blocks(service, QStringLiteral("remote-ads-v1.example")));
    // 4. Last-Modified / If-Modified-Since for a server without ETag.
    CHECK(server.last("/privacy.txt")->ifModifiedSince == "Wed, 01 Jul 2026 10:00:00 GMT");
    CHECK(server.last("/privacy.txt")->ifNoneMatch.isEmpty());
    CHECK(blocks(service, QStringLiteral("remote-tracker.example")));
    pass("2 successful update downloads, validates, activates; 3 ETag/If-None-Match 304; 4 Last-Modified/If-Modified-Since 304");
  }
  // 12. Restart: the copy is active at once, before and without any network.
  const int requestsBefore = static_cast<int>(server.requests.size());
  DaliNiraBlockerService restarted(profile.path());
  prepare(restarted);
  CHECK(blocks(restarted, QStringLiteral("remote-ads-v1.example")));
  CHECK(blocks(restarted, QStringLiteral("remote-tracker.example")));
  CHECK(!blocks(restarted, QStringLiteral("bundled-ads.example")));
  CHECK(listInfo(restarted, kAds).lastUpdateResult == QLatin1String("not-modified"));
  CHECK(server.requests.size() == requestsBefore);
  // 17. One store per profile: a single state record and one copy per list,
  //     nothing engine-specific. A relaunch in the other engine is this same
  //     construction on the same profile directory.
  const QStringList files = storeFiles(profile);
  CHECK(files.size() == 3);
  CHECK(files.contains(QStringLiteral("adblock-filter-updates/state.json")));
  for (const QString &file : files) {
    CHECK(!file.contains(QStringLiteral("webkit"), Qt::CaseInsensitive));
    CHECK(!file.contains(QStringLiteral("chromium"), Qt::CaseInsensitive));
  }
  pass("12 restart persistence; 17 one shared store per profile, no per-engine copies or state");
}

void testFailuresKeepActiveList(FixtureServer &server) {
  server.reset();
  Profile profile(server);
  server.setRoute("/ads.txt", listRoute(adsV1(), "\"v1\""));
  server.setRoute("/privacy.txt", listRoute(privacyV1()));
  DaliNiraBlockerService service(profile.path());
  prepare(service);
  CHECK(runUpdate(service).success);
  const QString sha = service.listManager()->filterStore()->state(kAds).current.sha256;
  const int rulesBefore = service.filterEngine()->ruleCount();
  auto unchanged = [&] {
    CHECK(blocks(service, QStringLiteral("remote-ads-v1.example")));
    CHECK(!blocks(service, QStringLiteral("bundled-ads.example")));
    CHECK(service.filterEngine()->ruleCount() == rulesBefore);
    CHECK(service.listManager()->filterStore()->state(kAds).current.sha256 == sha);
    CHECK(listInfo(service, kAds).activeSource == QLatin1String("remote"));
  };
  auto expectFailure = [&](const Route &route, const char *failure) {
    server.setRoute("/ads.txt", route);
    const UpdateOutcome outcome = runUpdate(service);
    CHECK(!outcome.success);
    CHECK(outcome.message.endsWith(I18n::text(QStringLiteral("blocker.filters_update_kept_previous"))));
    const FilterListInfo ads = listInfo(service, kAds);
    if (ads.lastUpdateFailure != QLatin1String(failure))
      std::cerr << "expected " << failure << " got " << ads.lastUpdateFailure.toStdString() << "\n";
    CHECK(ads.lastUpdateResult == QLatin1String("failed") && ads.lastUpdateFailure == QLatin1String(failure));
    unchanged();
  };

  Route error = listRoute("internal error");
  error.status = 500;
  expectFailure(error, "http");  // 5
  Route missing = listRoute("gone");
  missing.status = 404;
  expectFailure(missing, "http");
  CHECK(service.listManager()->filterStore()->state(kAds).consecutiveFailures == 2);

  Route hang;
  hang.hang = true;
  service.setFilterFetchPolicyForTesting(testPolicy(400));
  expectFailure(hang, "timeout");  // 6
  service.setFilterFetchPolicyForTesting(testPolicy());

  Route html = listRoute("<!DOCTYPE html><html><head><title>Portal</title></head><body>Log in</body></html>");
  html.contentType = "text/html";
  expectFailure(html, "html");  // 7
  expectFailure(listRoute("<html><body>ok</body></html>"), "html");  // HTML sent as text/plain
  expectFailure(listRoute(QByteArray()), "empty");  // 8
  expectFailure(listRoute(QByteArray("||a.example^\n") + QByteArray("\xFF\xFE", 2) + QByteArray(1, '\0') +
                          QByteArray("garbage\n")),
                "invalid");
  Route truncated = listRoute(adsV2());
  truncated.declaredLength = adsV2().size() + 4096;  // connection closes early
  expectFailure(truncated, "network");
  expectFailure(listRoute(listText({QStringLiteral("||only.example^")})), "too-few-rules");

  Route oversized = listRoute(QByteArray(70000, '!'));
  expectFailure(oversized, "oversized");  // 9, announced by Content-Length
  oversized.omitContentLength = true;
  expectFailure(oversized, "oversized");  // 9, detected while streaming

  // 10. Content the compiler cannot use at all.
  expectFailure(listRoute(listText({QStringLiteral("||a.example^$csp=default-src"), QStringLiteral("||b.example^$redirect=noop.js"),
                                    QStringLiteral("||c.example^$removeparam=utm"), QStringLiteral("example.org##+js(noeval)"),
                                    QStringLiteral("||d.example^$badfilter")})),
                "too-few-rules");

  // Redirects: allowed hops are followed, unsafe or endless ones refused.
  Route toPlainHttp;
  toPlainHttp.status = 302;
  toPlainHttp.location = "http://filters.example.org/ads.txt";
  expectFailure(toPlainHttp, "redirect");
  Route loop;
  loop.status = 302;
  loop.location = "/ads.txt";
  expectFailure(loop, "redirect");
  CHECK(service.listManager()->filterStore()->state(kAds).consecutiveFailures >= 12);

  // Production policy: the loopback http fixture is refused before any request.
  DaliNiraBlockerFilterFetcher::Policy production = testPolicy();
  production.allowLoopbackHttpForTesting = false;
  service.setFilterFetchPolicyForTesting(production);
  const int adsRequests = server.count("/ads.txt");
  server.setRoute("/ads.txt", listRoute(adsV2()));
  CHECK(!runUpdate(service).success);
  CHECK(server.count("/ads.txt") == adsRequests);
  unchanged();
  service.setFilterFetchPolicyForTesting(testPolicy());

  // A valid redirect is followed, and recovery resets the failure count.
  Route moved;
  moved.status = 301;
  moved.location = server.url(QStringLiteral("/ads-v2.txt")).toLatin1();
  server.setRoute("/ads.txt", moved);
  server.setRoute("/ads-v2.txt", listRoute(adsV2(), "\"v2\""));
  CHECK(runUpdate(service).success);
  CHECK(blocks(service, QStringLiteral("remote-ads-v2.example")));
  CHECK(!blocks(service, QStringLiteral("remote-ads-v1.example")));
  CHECK(service.listManager()->filterStore()->state(kAds).consecutiveFailures == 0);
  CHECK(service.listManager()->filterStore()->state(kAds).previous.sha256 == sha);
  pass("5 HTTP errors, 6 timeout, 7 HTML, 8 empty/corrupt/truncated, 9 oversized, 10 unusable content, redirects: active list unchanged");
}

void testLastKnownGoodRecovery(FixtureServer &server) {
  server.reset();
  Profile profile(server);
  server.setRoute("/privacy.txt", listRoute(privacyV1()));
  QString currentPath;
  QString previousPath;
  {
    DaliNiraBlockerService service(profile.path());
    prepare(service);
    server.setRoute("/ads.txt", listRoute(adsV1(), "\"v1\""));
    CHECK(runUpdate(service).success);
    server.setRoute("/ads.txt", listRoute(adsV2(), "\"v2\""));
    CHECK(runUpdate(service).success);
    const FilterListUpdateState state = service.listManager()->filterStore()->state(kAds);
    currentPath = profile.storeDir() + QStringLiteral("/fixture-ads/") + state.current.sha256 + QStringLiteral(".txt");
    previousPath = profile.storeDir() + QStringLiteral("/fixture-ads/") + state.previous.sha256 + QStringLiteral(".txt");
    CHECK(QFileInfo::exists(currentPath) && QFileInfo::exists(previousPath));
    CHECK(blocks(service, QStringLiteral("remote-ads-v2.example")));

    // Damage while running: the next compile uses the previous copy.
    corrupt(currentPath);
    service.reloadRules();
    CHECK(blocks(service, QStringLiteral("remote-ads-v1.example")));
    CHECK(!blocks(service, QStringLiteral("remote-ads-v2.example")));
    CHECK(listInfo(service, kAds).activeSource == QLatin1String("remote-previous"));
  }
  {
    // Restart: previous is promoted; conditional headers are dropped so the
    // next check downloads the list in full.
    DaliNiraBlockerService restarted(profile.path());
    prepare(restarted);
    CHECK(blocks(restarted, QStringLiteral("remote-ads-v1.example")));
    const FilterListUpdateState state = restarted.listManager()->filterStore()->state(kAds);
    CHECK(state.etag.isEmpty() && !state.previous.isValid());
    CHECK(!QFileInfo::exists(currentPath));  // the damaged file was removed
    CHECK(runUpdate(restarted).success);
    CHECK(server.last("/ads.txt")->ifNoneMatch.isEmpty());
    CHECK(blocks(restarted, QStringLiteral("remote-ads-v2.example")));
    // Both copies damaged: the bundled ruleset.
    const FilterListUpdateState repaired = restarted.listManager()->filterStore()->state(kAds);
    corrupt(profile.storeDir() + QStringLiteral("/fixture-ads/") + repaired.current.sha256 + QStringLiteral(".txt"));
    corrupt(profile.storeDir() + QStringLiteral("/fixture-ads/") + repaired.previous.sha256 + QStringLiteral(".txt"));
  }
  DaliNiraBlockerService bundled(profile.path());
  prepare(bundled);
  CHECK(blocks(bundled, QStringLiteral("bundled-ads.example")));
  CHECK(!blocks(bundled, QStringLiteral("remote-ads-v1.example")));
  CHECK(!blocks(bundled, QStringLiteral("remote-ads-v2.example")));
  CHECK(listInfo(bundled, kAds).activeSource == QLatin1String("bundled"));

  // A state file that is not valid JSON is ignored (bundled), never fatal.
  QFile state(profile.storeDir() + QStringLiteral("/state.json"));
  CHECK(state.open(QIODevice::WriteOnly | QIODevice::Truncate));
  state.write("{\"lists\": {\"fixture-ads\": {\"current\": ");
  state.close();
  DaliNiraBlockerService garbled(profile.path());
  prepare(garbled);
  CHECK(blocks(garbled, QStringLiteral("bundled-ads.example")));
  pass("13 last known good: damaged current falls back to previous (running and after restart), then to bundled");
}

void testCrashRecovery(FixtureServer &server) {
  server.reset();
  Profile profile(server);
  server.setRoute("/ads.txt", listRoute(adsV1(), "\"v1\""));
  server.setRoute("/privacy.txt", listRoute(privacyV1()));
  {
    DaliNiraBlockerService service(profile.path());
    prepare(service);
    CHECK(runUpdate(service).success);
  }
  const QSet<QString> eligible{kAds, kPrivacy};
  const QString manifest = profile.rulesetDir() + QStringLiteral("/filter-update-sources.json");
  FilterUpdateSource source;
  source.listId = kAds;
  source.minimumNetworkRules = 3;
  const FilterListValidation v2 = validateFilterListDownload(adsV2(), QStringLiteral("text/plain"), source, 0);
  CHECK(v2.failure == FilterUpdateFailure::None);
  QString v1Sha;
  {
    // Killed after staging the new copy, before activation.
    DaliNiraBlockerFilterStore store(profile.storeDir(), manifest, eligible);
    v1Sha = store.state(kAds).current.sha256;
    store.setCrashPointForTesting(DaliNiraBlockerFilterStore::CrashPoint::AfterCopyWritten);
    CHECK(!store.commitValidatedCopy(kAds, adsV2(), v2.copy, QStringLiteral("u"), QStringLiteral("\"v2\""), {}));
    CHECK(QFileInfo::exists(profile.storeDir() + QStringLiteral("/fixture-ads/") + v2.copy.sha256 + QStringLiteral(".txt")));
  }
  // Leftovers of writes interrupted mid-file, and files the store does not own.
  const QString listDir = profile.storeDir() + QStringLiteral("/fixture-ads/");
  for (const QString &name : {v2.copy.sha256 + QStringLiteral(".txt.AbC123"), QStringLiteral("notes.txt")}) {
    QFile file(listDir + name);
    CHECK(file.open(QIODevice::WriteOnly));
    file.write("partial");
  }
  for (const QString &name : {QStringLiteral("state.json.XyZ789"), QStringLiteral("readme.txt")}) {
    QFile file(profile.storeDir() + QLatin1Char('/') + name);
    CHECK(file.open(QIODevice::WriteOnly));
    file.write("partial");
  }
  {
    DaliNiraBlockerService restarted(profile.path());
    prepare(restarted);
    CHECK(blocks(restarted, QStringLiteral("remote-ads-v1.example")));
    CHECK(!blocks(restarted, QStringLiteral("remote-ads-v2.example")));
    CHECK(restarted.listManager()->filterStore()->state(kAds).current.sha256 == v1Sha);
    CHECK(!QFileInfo::exists(listDir + v2.copy.sha256 + QStringLiteral(".txt")));
    CHECK(!QFileInfo::exists(listDir + v2.copy.sha256 + QStringLiteral(".txt.AbC123")));
    CHECK(!QFileInfo::exists(profile.storeDir() + QStringLiteral("/state.json.XyZ789")));
    CHECK(QFileInfo::exists(listDir + QStringLiteral("notes.txt")));
    CHECK(QFileInfo::exists(profile.storeDir() + QStringLiteral("/readme.txt")));
  }
  {
    // Killed after activation (state.json written) but before the engine
    // swapped plans: the next start uses the new copy.
    DaliNiraBlockerFilterStore store(profile.storeDir(), manifest, eligible);
    CHECK(store.commitValidatedCopy(kAds, adsV2(), v2.copy, QStringLiteral("u"), QStringLiteral("\"v2\""), {}));
  }
  DaliNiraBlockerService afterActivation(profile.path());
  prepare(afterActivation);
  CHECK(blocks(afterActivation, QStringLiteral("remote-ads-v2.example")));
  CHECK(afterActivation.listManager()->filterStore()->state(kAds).previous.sha256 == v1Sha);
  pass("11 crash safety: staged-but-unactivated copy ignored and cleaned, temp leftovers removed, foreign files kept");
}

void testAtomicSwapUnderLoad(FixtureServer &server) {
  server.reset();
  Profile profile(server);
  server.setRoute("/ads.txt", listRoute(adsV1(), "\"v1\""));
  server.setRoute("/privacy.txt", listRoute(privacyV1()));
  DaliNiraBlockerService service(profile.path());
  prepare(service);
  CHECK(runUpdate(service).success);
  server.setRoute("/ads.txt", listRoute(adsV2(), "\"v2\""));

  // An engine adapter thread (as WebKit's filter thread) asks continuously
  // while the plan is replaced. The old plan blocks v1, the new one v2: a
  // sample where neither is blocked would be a half-built plan.
  const std::shared_ptr<DaliNiraBlockerNetworkGate> gate = service.networkGate();
  std::atomic<bool> stop{false};
  std::atomic<int> samples{0};
  std::atomic<int> torn{0};
  QThread *reader = QThread::create([&] {
    const QUrl firstParty(QStringLiteral("https://news.example.org/"));
    while (!stop.load()) {
      const bool v1 = gate->decide(QUrl(QStringLiteral("https://remote-ads-v1.example/a.js")),
                                   DaliNiraBlockerResourceType::Script, firstParty, 0x99, QStringLiteral("get"))
                          .kind == NetworkVerdictKind::Block;
      const bool v2 = gate->decide(QUrl(QStringLiteral("https://remote-ads-v2.example/a.js")),
                                   DaliNiraBlockerResourceType::Script, firstParty, 0x99, QStringLiteral("get"))
                          .kind == NetworkVerdictKind::Block;
      if (!v1 && !v2) ++torn;
      ++samples;
    }
  });
  reader->start();
  CHECK(runUpdate(service).success);
  stop = true;
  reader->wait();
  delete reader;
  CHECK(samples.load() > 10);
  CHECK(torn.load() == 0);
  CHECK(blocks(service, QStringLiteral("remote-ads-v2.example")));
  pass("11 atomic activation: concurrent readers only ever see the old or the new complete plan");
}

void testConcurrencyAndIndependence(FixtureServer &server) {
  server.reset();
  Profile profile(server);
  server.setRoute("/ads.txt", listRoute(adsV1(), "\"v1\""));
  Route failing = listRoute("down");
  failing.status = 503;
  server.setRoute("/privacy.txt", failing);
  DaliNiraBlockerService service(profile.path());
  prepare(service);
  int finishedSignals = 0;
  QObject::connect(&service, &DaliNiraBlockerService::filterUpdateFinished, [&finishedSignals] { ++finishedSignals; });
  // 14. Repeated manual requests while one runs are coalesced into it.
  service.updateFiltersAsync();
  CHECK(service.isUpdatingFilters());
  service.updateFiltersAsync();
  service.updateFiltersAsync();
  CHECK(waitUntil([&service] { return !service.isUpdatingFilters(); }, 20000));
  waitUntil([] { return false; }, 200);
  CHECK(finishedSignals == 1);
  CHECK(server.count("/ads.txt") == 1 && server.count("/privacy.txt") == 1);
  // 15. The failing provider did not stop the other list.
  CHECK(blocks(service, QStringLiteral("remote-ads-v1.example")));
  CHECK(blocks(service, QStringLiteral("bundled-tracker.example")));
  CHECK(listInfo(service, kAds).lastUpdateResult == QLatin1String("updated"));
  CHECK(listInfo(service, kPrivacy).lastUpdateResult == QLatin1String("failed"));
  CHECK(listInfo(service, kPrivacy).activeSource == QLatin1String("bundled"));
  pass("14 concurrent manual updates coalesced (one request per list); 15 one failing provider does not block others");
}

void testDisabledListAndSiteControls(FixtureServer &server) {
  server.reset();
  Profile profile(server);
  server.setRoute("/ads.txt", listRoute(adsV1(), "\"v1\""));
  server.setRoute("/privacy.txt", listRoute(privacyV1()));
  {
    DaliNiraBlockerService service(profile.path());
    prepare(service, {kAds, kBundledOnly});
    CHECK(runUpdate(service).success);
    CHECK(server.count("/privacy.txt") == 0);  // disabled lists are not downloaded
    CHECK(!blocks(service, QStringLiteral("bundled-tracker.example")));
    CHECK(!blocks(service, QStringLiteral("remote-tracker.example")));
    CHECK(blocks(service, QStringLiteral("remote-ads-v1.example")));

    // 19. Site controls apply to downloaded rules as to bundled ones.
    const QUrl ad(QStringLiteral("https://remote-ads-v1.example/a.js"));
    const QUrl site(QStringLiteral("https://news.example.org/"));
    auto kind = [&service, &ad, &site] {
      return service.decideNetworkRequest(ad, DaliNiraBlockerResourceType::Script, site, 0x55, QStringLiteral("get")).kind;
    };
    SitePolicy allowlisted;
    allowlisted.whitelisted = true;
    service.settings()->setSitePolicy(QStringLiteral("news.example.org"), allowlisted);
    CHECK(kind() == NetworkVerdictKind::Allow);
    SitePolicy paused;
    paused.temporaryDisabledUntil = QDateTime::currentMSecsSinceEpoch() + 60000;
    service.settings()->setSitePolicy(QStringLiteral("news.example.org"), paused);
    CHECK(kind() == NetworkVerdictKind::Allow);
    SitePolicy noAds;
    noAds.adBlocking = false;
    service.settings()->setSitePolicy(QStringLiteral("news.example.org"), noAds);
    CHECK(kind() == NetworkVerdictKind::Allow);
    service.settings()->setSitePolicy(QStringLiteral("news.example.org"), SitePolicy{});
    CHECK(kind() == NetworkVerdictKind::Block);
    service.settings()->setProtectionEnabled(false);
    CHECK(kind() == NetworkVerdictKind::Allow);
    service.settings()->setProtectionEnabled(true);

    // 20. Custom filters still take precedence and still add cosmetics.
    service.settings()->setCustomFilters({QStringLiteral("@@||remote-ads-v1.example^"),
                                          QStringLiteral("||custom-block.example^"),
                                          QStringLiteral("news.example.org##.custom-ad")});
    CHECK(kind() == NetworkVerdictKind::Allow);
    CHECK(blocks(service, QStringLiteral("custom-block.example")));
    CHECK(service.cosmeticCssForHost(QStringLiteral("news.example.org")).contains(QStringLiteral(".custom-ad")));
    service.settings()->setCustomFilters({});
    CHECK(kind() == NetworkVerdictKind::Block);
  }
  // 18. Still disabled after the update and a restart; re-enabling it uses
  //     the bundled copy until it is downloaded.
  DaliNiraBlockerService restarted(profile.path());
  restarted.setFilterFetchPolicyForTesting(testPolicy());
  CHECK(restarted.settings()->rulesetSelectionConfigured());
  CHECK(!restarted.settings()->enabledRulesetIds().contains(kPrivacy));
  CHECK(!blocks(restarted, QStringLiteral("bundled-tracker.example")));
  CHECK(blocks(restarted, QStringLiteral("remote-ads-v1.example")));
  restarted.settings()->setEnabledRulesetIds({kAds, kPrivacy});
  CHECK(blocks(restarted, QStringLiteral("bundled-tracker.example")));
  pass("18 disabled list stays disabled and is not downloaded; 19 site controls; 20 custom filters and cosmetics");
}

void testScheduling(FixtureServer &server) {
  server.reset();
  Profile profile(server);
  server.setRoute("/ads.txt", listRoute(adsV1(), "\"v1\""));
  server.setRoute("/privacy.txt", listRoute(privacyV1()));
  {
    DaliNiraBlockerService service(profile.path());
    prepare(service, {kAds, kPrivacy});
    int reloads = 0;
    QObject::connect(&service, &DaliNiraBlockerService::autoReloadRequested, [&reloads] { ++reloads; });
    service.settings()->setAutoReloadOnModeChange(true);
    CHECK(!service.nextAutomaticFilterUpdate().isValid());  // setting off
    service.setAutomaticUpdateDelayFloorForTesting(50);
    service.settings()->setAutoUpdateRulesets(true);
    CHECK(service.nextAutomaticFilterUpdate().isValid());
    CHECK(waitUntil([&server] { return server.count("/ads.txt") == 1 && server.count("/privacy.txt") == 1; }, 10000));
    CHECK(waitUntil([&service] { return !service.isUpdatingFilters() && service.nextAutomaticFilterUpdate().isValid(); }, 10000));
    CHECK(blocks(service, QStringLiteral("remote-ads-v1.example")));
    CHECK(reloads == 0);  // background updates never reload pages
    // Next check honours "! Expires: 2 days", not the start-up floor (the
    // timer itself is re-armed at most a day ahead).
    CHECK(service.nextAutomaticFilterUpdate() > QDateTime::currentDateTime().addSecs(23 * 3600));
    const QDateTime due = service.listManager()->filterStore()->nextCheckDue(kAds);
    CHECK(due > QDateTime::currentDateTimeUtc().addSecs(47 * 3600) &&
          due < QDateTime::currentDateTimeUtc().addSecs(49 * 3600));
    waitUntil([] { return false; }, 300);
    CHECK(server.count("/ads.txt") == 1);  // no polling
  }
  {
    // A restart soon after a successful check does not check again.
    DaliNiraBlockerService restarted(profile.path());
    restarted.setFilterFetchPolicyForTesting(testPolicy());
    restarted.setAutomaticUpdateDelayFloorForTesting(50);
    CHECK(restarted.settings()->autoUpdateRulesets());
    CHECK(restarted.nextAutomaticFilterUpdate() > QDateTime::currentDateTime().addSecs(23 * 3600));
    waitUntil([] { return false; }, 300);
    CHECK(server.count("/ads.txt") == 1);
  }
  // Backoff after failures: 15 min, 30 min, 1 h, ... capped at 12 h.
  DaliNiraBlockerFilterStore store(profile.storeDir(), profile.rulesetDir() + QStringLiteral("/filter-update-sources.json"),
                                   {kAds, kPrivacy});
  const qint64 minutes[] = {15, 30, 60, 120, 240, 480, 720, 720};
  for (const qint64 expected : minutes) {
    CHECK(store.recordFailure(kAds, FilterUpdateFailure::Network));
    const FilterListUpdateState state = store.state(kAds);
    CHECK(state.lastCheck.secsTo(store.nextCheckDue(kAds)) == expected * 60);
  }
  // Only the failing list is retried early; unrelated lists keep their interval.
  CHECK(store.nextCheckDue(kPrivacy) > QDateTime::currentDateTimeUtc().addSecs(47 * 3600));
  pass("scheduling: opt-in, start-up floor, list Expires interval, no relaunch re-check, per-list backoff, no page reloads");
}

void testEngineParity(FixtureServer &server) {
  server.reset();
  Profile profile(server);
  server.setRoute("/ads.txt", listRoute(adsV1(), "\"v1\""));
  server.setRoute("/privacy.txt", listRoute(privacyV1()));
  Route page;
  page.contentType = "text/html";
  page.body = "<!doctype html><html><body><script src=\"/dalinira-remote-new-ad.js\"></script>"
              "<script src=\"/control.js\"></script></body></html>";
  server.setRoute("/page.html", page);
  Route script;
  script.contentType = "application/javascript";
  script.body = "window.loaded = (window.loaded || 0) + 1;";
  server.setRoute("/dalinira-remote-new-ad.js", script);
  server.setRoute("/control.js", script);

  DaliNiraBlockerService service(profile.path());
  prepare(service);

  // Chromium: the service's QWebEngineUrlRequestInterceptor on a real page.
  QWebEngineProfile chromiumProfile;  // off the record
  chromiumProfile.setUrlRequestInterceptor(service.requestInterceptor());
  QWebEnginePage chromiumPage(&chromiumProfile);
  auto loadPage = [&]() {
    bool loaded = false;
    const auto connection = QObject::connect(&chromiumPage, &QWebEnginePage::loadFinished, [&loaded](bool) { loaded = true; });
    const int controlBefore = server.count("/control.js");
    chromiumPage.load(QUrl(server.url(QStringLiteral("/page.html"))));
    CHECK(waitUntil([&] { return loaded && server.count("/control.js") > controlBefore; }, 30000));
    QObject::disconnect(connection);
  };

  // WebKit: the filter's socket server, spoken to as the web process
  // extension does, with the evaluator BrowserWindow installs per page.
  dalinira::engine::WebKitNetworkFilter webkitFilter;
  CHECK(webkitFilter.start());
  const std::shared_ptr<DaliNiraBlockerNetworkGate> gate = service.networkGate();
  constexpr quint64 kPageId = 7;
  constexpr quint64 kWebKitTab = 0x7717;
  webkitFilter.setRoute(kPageId, [gate](const dalinira::engine::WebKitNetworkFilter::Request &request) {
    using Filter = dalinira::engine::WebKitNetworkFilter;
    const NetworkRequestVerdict result = gate->decide(request.url, resourceTypeFromFetchDestination(request.destination),
                                                      request.firstPartyUrl, kWebKitTab, request.method.toLower());
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
  auto webkitAsk = [&](const QString &path) {
    const QByteArray url = QUrl(server.url(path)).toEncoded();
    const QByteArray document = QUrl(server.url(QStringLiteral("/page.html"))).toEncoded();
    const QByteArray number = QByteArray::number(++seq);
    socket.write("Q\t" + number + "\t" + QByteArray::number(kPageId) + "\tGET\tscript\t0\t" + url + "\t" + document + "\n");
    socket.flush();
    QByteArray line;
    QElapsedTimer timer;
    timer.start();
    while (!line.endsWith('\n') && timer.elapsed() < 5000) {
      if (socket.bytesAvailable() || socket.waitForReadyRead(100)) line += socket.readLine();
    }
    CHECK(line.startsWith(number + "\t"));
    return line.mid(number.size() + 1, 1);  // A, B or R
  };

  // Before the update neither engine blocks the new rule's script.
  loadPage();
  CHECK(server.count("/dalinira-remote-new-ad.js") == 1);
  CHECK(webkitAsk(QStringLiteral("/dalinira-remote-new-ad.js")) == "A");

  CHECK(runUpdate(service).success);
  const int downloads = server.count("/ads.txt");

  // 16. After one shared update both engines apply the new rule.
  loadPage();
  CHECK(server.count("/dalinira-remote-new-ad.js") == 1);  // Chromium never requested it
  CHECK(webkitAsk(QStringLiteral("/dalinira-remote-new-ad.js")) == "B");
  CHECK(webkitAsk(QStringLiteral("/control.js")) == "A");
  CHECK(service.statsForTab(kWebKitTab).blockedRequests == 1);
  CHECK(server.count("/ads.txt") == downloads);  // one download served both
  socket.disconnectFromServer();
  pass("16 Chromium interceptor (real page) and WebKit filter socket both apply the one shared update");
}

}  // namespace

int main(int argc, char *argv[]) {
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
  if (qEnvironmentVariableIsEmpty("QTWEBENGINE_CHROMIUM_FLAGS"))
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", "--no-sandbox --disable-gpu");
  qputenv("QTWEBENGINE_DISABLE_SANDBOX", "1");
  QApplication app(argc, argv);
  std::cout << "[DaliNira blocker remote filter updates]\n";
  FixtureServer server;

  testCompiler();
  testValidation();
  testUrlPolicy();
  testBundledManifest();
  testBundledFallbackOffline(server);
  testSuccessEtagAndRestart(server);
  testFailuresKeepActiveList(server);
  testLastKnownGoodRecovery(server);
  testCrashRecovery(server);
  testAtomicSwapUnderLoad(server);
  testConcurrencyAndIndependence(server);
  testDisabledListAndSiteControls(server);
  testScheduling(server);
  testEngineParity(server);
  std::cout << "ALL REMOTE FILTER UPDATE CHECKS PASSED\n";
  return 0;
}
