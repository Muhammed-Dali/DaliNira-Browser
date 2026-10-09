#include "dalinira_blocker_filter_updates.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSslError>
#include <QStringDecoder>
#include <QTimer>
#include <new>

#include "dalinira_blocker_engine.h"

namespace {
constexpr qint64 kMaximumManifestBytes = 256 * 1024;
constexpr qint64 kMaximumStateBytes = 1024 * 1024;
constexpr qint64 kMinimumCopyBytes = 64 * 1024;
constexpr qint64 kMaximumCopyBytes = 32 * 1024 * 1024;
constexpr qint64 kDefaultIntervalSeconds = 4 * 24 * 3600;
constexpr qint64 kMinimumIntervalSeconds = 24 * 3600;
constexpr qint64 kMaximumIntervalSeconds = 7 * 24 * 3600;
constexpr qint64 kFirstRetrySeconds = 15 * 60;
constexpr qint64 kMaximumRetrySeconds = 12 * 3600;
constexpr int kLockTimeoutMs = 5000;
constexpr QFileDevice::Permissions kOwnerOnly = QFileDevice::ReadOwner | QFileDevice::WriteOwner;

const QRegularExpression &listIdPattern() {
  static const QRegularExpression pattern(QStringLiteral("^[a-z0-9][a-z0-9_-]{0,79}$"));
  return pattern;
}

const QRegularExpression &sha256Pattern() {
  static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{64}$"));
  return pattern;
}

QString timeToString(const QDateTime &time) {
  return time.isValid() ? time.toUTC().toString(Qt::ISODateWithMs) : QString();
}

QDateTime timeFromString(const QString &text) {
  QDateTime time = QDateTime::fromString(text, Qt::ISODateWithMs);
  return time.isValid() ? time.toUTC() : QDateTime();
}

QJsonObject copyToJson(const FilterListCopy &copy) {
  if (!copy.isValid()) return {};
  return QJsonObject{{QStringLiteral("sha256"), copy.sha256},
                     {QStringLiteral("bytes"), copy.bytes},
                     {QStringLiteral("networkRules"), copy.networkRules},
                     {QStringLiteral("activatedAt"), timeToString(copy.activatedAt)},
                     {QStringLiteral("expiresSeconds"), copy.expiresSeconds}};
}

FilterListCopy copyFromJson(const QJsonObject &object) {
  FilterListCopy copy;
  const QString sha = object.value(QStringLiteral("sha256")).toString();
  const qint64 bytes = object.value(QStringLiteral("bytes")).toVariant().toLongLong();
  if (!sha256Pattern().match(sha).hasMatch() || bytes <= 0 || bytes > kMaximumCopyBytes) return copy;
  copy.sha256 = sha;
  copy.bytes = bytes;
  copy.networkRules = qMax(0, object.value(QStringLiteral("networkRules")).toInt());
  copy.activatedAt = timeFromString(object.value(QStringLiteral("activatedAt")).toString());
  copy.expiresSeconds = qMax<qint64>(0, object.value(QStringLiteral("expiresSeconds")).toVariant().toLongLong());
  return copy;
}

QString sha256Hex(const QByteArray &bytes) {
  return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

// "! Expires: 4 days (update frequency)" in the list header.
qint64 expiresSecondsFromHeader(QStringView text) {
  static const QRegularExpression expires(QStringLiteral("^!\\s*Expires\\s*:\\s*(\\d{1,4})\\s*([a-z]*)"),
                                          QRegularExpression::CaseInsensitiveOption);
  qsizetype start = 0;
  for (int line = 0; line < 64 && start < text.size(); ++line) {
    qsizetype end = text.indexOf(QLatin1Char('\n'), start);
    if (end < 0) end = text.size();
    const QRegularExpressionMatch match = expires.match(text.mid(start, end - start).trimmed().toString());
    if (match.hasMatch()) {
      const qint64 amount = match.captured(1).toLongLong();
      const bool hours = match.captured(2).startsWith(QLatin1Char('h'), Qt::CaseInsensitive);
      return amount * (hours ? 3600 : 24 * 3600);
    }
    start = end + 1;
  }
  return 0;
}
}  // namespace

QString filterUpdateFailureName(FilterUpdateFailure failure) {
  switch (failure) {
    case FilterUpdateFailure::None: return {};
    case FilterUpdateFailure::Network: return QStringLiteral("network");
    case FilterUpdateFailure::Timeout: return QStringLiteral("timeout");
    case FilterUpdateFailure::Tls: return QStringLiteral("tls");
    case FilterUpdateFailure::Http: return QStringLiteral("http");
    case FilterUpdateFailure::Redirect: return QStringLiteral("redirect");
    case FilterUpdateFailure::Oversized: return QStringLiteral("oversized");
    case FilterUpdateFailure::Empty: return QStringLiteral("empty");
    case FilterUpdateFailure::Html: return QStringLiteral("html");
    case FilterUpdateFailure::Invalid: return QStringLiteral("invalid");
    case FilterUpdateFailure::TooFewRules: return QStringLiteral("too-few-rules");
    case FilterUpdateFailure::Shrunk: return QStringLiteral("shrunk");
    case FilterUpdateFailure::Storage: return QStringLiteral("storage");
    case FilterUpdateFailure::Cancelled: return QStringLiteral("cancelled");
  }
  return {};
}

FilterUpdateFailure filterUpdateFailureFromName(const QString &name) {
  for (int value = static_cast<int>(FilterUpdateFailure::Network);
       value <= static_cast<int>(FilterUpdateFailure::Cancelled); ++value) {
    const auto failure = static_cast<FilterUpdateFailure>(value);
    if (filterUpdateFailureName(failure) == name) return failure;
  }
  return FilterUpdateFailure::None;
}

// ---------------- Store ----------------

DaliNiraBlockerFilterStore::DaliNiraBlockerFilterStore(const QString &directory, const QString &sourcesManifestPath,
                                                       const QSet<QString> &eligibleListIds)
    : directory_(QDir(directory).absolutePath()) {
  loadSources(sourcesManifestPath, eligibleListIds);
  if (!sources_.isEmpty() && QFileInfo(directory_).isDir()) recover();
}

void DaliNiraBlockerFilterStore::loadSources(const QString &manifestPath, const QSet<QString> &eligibleListIds) {
  QFile file(manifestPath);
  if (!file.open(QIODevice::ReadOnly) || file.size() > kMaximumManifestBytes) return;
  const QJsonObject entries =
      QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("sources")).toObject();
  for (auto it = entries.constBegin(); it != entries.constEnd(); ++it) {
    // Lists whose provenance still needs review are never downloaded, even
    // when named here: the catalog decides which ids are eligible.
    if (!listIdPattern().match(it.key()).hasMatch() || !eligibleListIds.contains(it.key())) continue;
    const QJsonObject entry = it.value().toObject();
    // The one format compileTextFilterList() reads.
    if (entry.value(QStringLiteral("format")).toString() != QLatin1String("adblock-plus-text")) continue;
    FilterUpdateSource source;
    source.listId = it.key();
    source.url = QUrl(entry.value(QStringLiteral("url")).toString(), QUrl::StrictMode);
    // Loopback http passes here only so test fixtures can be described; the
    // fetcher refuses it unless a test enabled it explicitly.
    if (!DaliNiraBlockerFilterFetcher::isAllowedUrl(source.url, true)) continue;
    source.minimumNetworkRules = qBound(1, entry.value(QStringLiteral("minimumNetworkRules")).toInt(100), 1000000);
    const qint64 maximumBytes = entry.value(QStringLiteral("maximumBytes")).toVariant().toLongLong();
    if (maximumBytes > 0) source.maximumBytes = qBound(kMinimumCopyBytes, maximumBytes, kMaximumCopyBytes);
    sources_.insert(source.listId, source);
  }
}

std::optional<FilterUpdateSource> DaliNiraBlockerFilterStore::source(const QString &listId) const {
  const auto found = sources_.constFind(listId);
  if (found == sources_.constEnd()) return std::nullopt;
  return found.value();
}

FilterListUpdateState DaliNiraBlockerFilterStore::state(const QString &listId) const {
  QMutexLocker locker(&mutex_);
  return states_.value(listId);
}

QString DaliNiraBlockerFilterStore::copyPath(const QString &listId, const QString &sha256) const {
  return directory_ + QLatin1Char('/') + listId + QLatin1Char('/') + sha256 + QStringLiteral(".txt");
}

bool DaliNiraBlockerFilterStore::copyIntact(const QString &listId, const FilterListCopy &copy) const {
  if (!copy.isValid()) return false;
  QFile file(copyPath(listId, copy.sha256));
  if (!file.open(QIODevice::ReadOnly) || file.size() != copy.bytes) return false;
  return sha256Hex(file.readAll()) == copy.sha256;
}

QByteArray DaliNiraBlockerFilterStore::readActiveCopy(const QString &listId, bool *fromPrevious) const {
  if (fromPrevious) *fromPrevious = false;
  if (!sources_.contains(listId)) return {};
  // Held while reading so a concurrent commit cannot remove the file.
  QMutexLocker locker(&mutex_);
  const FilterListUpdateState entry = states_.value(listId);
  for (const FilterListCopy *copy : {&entry.current, &entry.previous}) {
    if (!copy->isValid() || damagedCopies_.contains(copy->sha256)) continue;
    QFile file(copyPath(listId, copy->sha256));
    if (file.open(QIODevice::ReadOnly) && file.size() == copy->bytes) {
      QByteArray bytes = file.readAll();
      if (sha256Hex(bytes) == copy->sha256) {
        if (fromPrevious) *fromPrevious = copy == &entry.previous;
        return bytes;
      }
    }
    damagedCopies_.insert(copy->sha256);
  }
  return {};
}

QString DaliNiraBlockerFilterStore::activeSource(const QString &listId) const {
  if (!sources_.contains(listId)) return QStringLiteral("bundled");
  QMutexLocker locker(&mutex_);
  const FilterListUpdateState entry = states_.value(listId);
  if (entry.current.isValid() && !damagedCopies_.contains(entry.current.sha256)) return QStringLiteral("remote");
  if (entry.previous.isValid() && !damagedCopies_.contains(entry.previous.sha256))
    return QStringLiteral("remote-previous");
  return QStringLiteral("bundled");
}

DaliNiraBlockerFilterStore::StateMap DaliNiraBlockerFilterStore::readStateFile() const {
  StateMap states;
  QFile file(directory_ + QStringLiteral("/state.json"));
  if (!file.open(QIODevice::ReadOnly) || file.size() > kMaximumStateBytes) return states;
  const QJsonObject lists =
      QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("lists")).toObject();
  for (auto it = lists.constBegin(); it != lists.constEnd(); ++it) {
    if (!listIdPattern().match(it.key()).hasMatch()) continue;
    const QJsonObject object = it.value().toObject();
    FilterListUpdateState entry;
    entry.sourceUrl = object.value(QStringLiteral("sourceUrl")).toString();
    entry.current = copyFromJson(object.value(QStringLiteral("current")).toObject());
    entry.previous = copyFromJson(object.value(QStringLiteral("previous")).toObject());
    entry.etag = object.value(QStringLiteral("etag")).toString().left(1024);
    entry.lastModified = object.value(QStringLiteral("lastModified")).toString().left(128);
    entry.lastCheck = timeFromString(object.value(QStringLiteral("lastCheck")).toString());
    entry.lastSuccess = timeFromString(object.value(QStringLiteral("lastSuccess")).toString());
    entry.lastResult = object.value(QStringLiteral("lastResult")).toString();
    entry.lastFailure = filterUpdateFailureFromName(object.value(QStringLiteral("lastFailure")).toString());
    entry.consecutiveFailures = qBound(0, object.value(QStringLiteral("consecutiveFailures")).toInt(), 1000);
    states.insert(it.key(), entry);
  }
  return states;
}

bool DaliNiraBlockerFilterStore::writeStateFile(const StateMap &states) const {
  QJsonObject lists;
  for (auto it = states.constBegin(); it != states.constEnd(); ++it) {
    const FilterListUpdateState &entry = it.value();
    lists.insert(it.key(), QJsonObject{
        {QStringLiteral("sourceUrl"), entry.sourceUrl},
        {QStringLiteral("current"), copyToJson(entry.current)},
        {QStringLiteral("previous"), copyToJson(entry.previous)},
        {QStringLiteral("etag"), entry.etag},
        {QStringLiteral("lastModified"), entry.lastModified},
        {QStringLiteral("lastCheck"), timeToString(entry.lastCheck)},
        {QStringLiteral("lastSuccess"), timeToString(entry.lastSuccess)},
        {QStringLiteral("lastResult"), entry.lastResult},
        {QStringLiteral("lastFailure"), filterUpdateFailureName(entry.lastFailure)},
        {QStringLiteral("consecutiveFailures"), entry.consecutiveFailures}});
  }
  const QString path = directory_ + QStringLiteral("/state.json");
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly)) return false;
  const QByteArray json =
      QJsonDocument(QJsonObject{{QStringLiteral("schemaVersion"), 1}, {QStringLiteral("lists"), lists}})
          .toJson(QJsonDocument::Indented);
  if (file.write(json) != json.size() || !file.commit()) return false;
  QFile::setPermissions(path, kOwnerOnly);
  return true;
}

// Only the store's own names are ever removed: unreferenced copies, and the
// temporary files an interrupted QSaveFile can leave. Links are not followed.
void DaliNiraBlockerFilterStore::removeStaleFiles(const StateMap &states) const {
  static const QRegularExpression copyName(QStringLiteral("^[0-9a-f]{64}\\.txt$"));
  static const QRegularExpression copyTemporary(QStringLiteral("^[0-9a-f]{64}\\.txt\\.[A-Za-z0-9]{6}$"));
  static const QRegularExpression stateTemporary(QStringLiteral("^state\\.json\\.[A-Za-z0-9]{6}$"));
  constexpr QDir::Filters kPlainFiles = QDir::Files | QDir::Hidden | QDir::NoSymLinks;
  for (auto it = sources_.constBegin(); it != sources_.constEnd(); ++it) {
    const QDir dir(directory_ + QLatin1Char('/') + it.key());
    if (!dir.exists()) continue;
    const FilterListUpdateState entry = states.value(it.key());
    for (const QFileInfo &info : dir.entryInfoList(kPlainFiles)) {
      const QString name = info.fileName();
      const bool referenced = (entry.current.isValid() && name == entry.current.sha256 + QStringLiteral(".txt")) ||
                              (entry.previous.isValid() && name == entry.previous.sha256 + QStringLiteral(".txt"));
      if ((copyName.match(name).hasMatch() && !referenced) || copyTemporary.match(name).hasMatch())
        QFile::remove(info.absoluteFilePath());
    }
  }
  for (const QFileInfo &info : QDir(directory_).entryInfoList(kPlainFiles)) {
    if (stateTemporary.match(info.fileName()).hasMatch()) QFile::remove(info.absoluteFilePath());
  }
}

// Startup: every named copy is verified. A damaged current copy is replaced
// by the previous one (or by the bundled ruleset when that is damaged too),
// and conditional request headers are dropped so the next check downloads
// the list in full.
void DaliNiraBlockerFilterStore::recover() {
  QLockFile lock(directory_ + QStringLiteral("/state.lock"));
  lock.setStaleLockTime(30000);
  StateMap states = readStateFile();
  if (!lock.tryLock(kLockTimeoutMs)) {
    // Another process is writing; use what is on disk and let reads verify.
    QMutexLocker locker(&mutex_);
    states_ = states;
    return;
  }
  states = readStateFile();
  bool changed = false;
  for (auto it = states.begin(); it != states.end(); ++it) {
    if (!sources_.contains(it.key())) continue;
    FilterListUpdateState &entry = it.value();
    const bool currentIntact = copyIntact(it.key(), entry.current);
    const bool previousIntact = copyIntact(it.key(), entry.previous);
    if (entry.current.isValid() && !currentIntact) {
      entry.current = previousIntact ? entry.previous : FilterListCopy{};
      entry.previous = {};
      entry.etag.clear();
      entry.lastModified.clear();
      changed = true;
    } else if (entry.previous.isValid() && !previousIntact) {
      entry.previous = {};
      changed = true;
    }
  }
  if (changed) writeStateFile(states);
  QMutexLocker locker(&mutex_);
  states_ = states;
  removeStaleFiles(states);
}

template <typename Change>
bool DaliNiraBlockerFilterStore::update(const QString &listId, Change change) {
  if (!sources_.contains(listId)) return false;
  if (!QDir().mkpath(directory_)) return false;
  QFile::setPermissions(directory_, kOwnerOnly | QFileDevice::ExeOwner);
  QLockFile lock(directory_ + QStringLiteral("/state.lock"));
  lock.setStaleLockTime(30000);
  if (!lock.tryLock(kLockTimeoutMs)) return false;
  // The file, not this process's snapshot, is the record another browser
  // process may have changed.
  StateMap states = readStateFile();
  if (!change(states[listId])) return false;
  if (!writeStateFile(states)) return false;
  QMutexLocker locker(&mutex_);
  states_ = states;
  removeStaleFiles(states);
  return true;
}

bool DaliNiraBlockerFilterStore::commitValidatedCopy(const QString &listId, const QByteArray &bytes,
                                                     const FilterListCopy &copy, const QString &sourceUrl,
                                                     const QString &etag, const QString &lastModified) {
  if (!copy.isValid() || bytes.size() != copy.bytes || sha256Hex(bytes) != copy.sha256) return false;
  const bool committed = update(listId, [&](FilterListUpdateState &entry) {
    const QString listDirectory = directory_ + QLatin1Char('/') + listId;
    if (!QDir().mkpath(listDirectory)) return false;
    QFile::setPermissions(listDirectory, kOwnerOnly | QFileDevice::ExeOwner);
    // 1. Stage: the copy is written completely (temporary file + rename)
    //    under a name nothing refers to yet.
    if (!copyIntact(listId, copy)) {
      const QString path = copyPath(listId, copy.sha256);
      QSaveFile file(path);
      if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) return false;
      QFile::setPermissions(path, kOwnerOnly);
    }
    if (crashPoint_ == CrashPoint::AfterCopyWritten) return false;
    // 2. Activate: the caller writes state.json, atomically, naming it.
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const bool unchanged = entry.current.sha256 == copy.sha256;
    if (!unchanged) {
      if (copyIntact(listId, entry.current)) entry.previous = entry.current;
      entry.current = copy;
      entry.current.activatedAt = now;
    } else {
      entry.current.expiresSeconds = copy.expiresSeconds;
    }
    entry.sourceUrl = sourceUrl;
    entry.etag = etag.left(1024);
    entry.lastModified = lastModified.left(128);
    entry.lastCheck = now;
    entry.lastSuccess = now;
    entry.lastResult = unchanged ? QStringLiteral("not-modified") : QStringLiteral("updated");
    entry.lastFailure = FilterUpdateFailure::None;
    entry.consecutiveFailures = 0;
    return true;
  });
  if (committed) {
    QMutexLocker locker(&mutex_);
    damagedCopies_.remove(copy.sha256);
  }
  return committed;
}

bool DaliNiraBlockerFilterStore::recordNotModified(const QString &listId) {
  return update(listId, [](FilterListUpdateState &entry) {
    const QDateTime now = QDateTime::currentDateTimeUtc();
    entry.lastCheck = now;
    entry.lastSuccess = now;
    entry.lastResult = QStringLiteral("not-modified");
    entry.lastFailure = FilterUpdateFailure::None;
    entry.consecutiveFailures = 0;
    return true;
  });
}

bool DaliNiraBlockerFilterStore::recordFailure(const QString &listId, FilterUpdateFailure failure) {
  return update(listId, [failure](FilterListUpdateState &entry) {
    entry.lastCheck = QDateTime::currentDateTimeUtc();
    entry.lastResult = QStringLiteral("failed");
    entry.lastFailure = failure;
    entry.consecutiveFailures = qMin(entry.consecutiveFailures + 1, 1000);
    return true;
  });
}

QDateTime DaliNiraBlockerFilterStore::nextCheckDue(const QString &listId) const {
  const QDateTime due = QDateTime::fromMSecsSinceEpoch(0).toUTC();
  if (!sources_.contains(listId)) return {};
  const FilterListUpdateState entry = state(listId);
  const QDateTime now = QDateTime::currentDateTimeUtc();
  // A check "in the future" means the clock was changed; do not wait for it.
  if (!entry.lastCheck.isValid() || entry.lastCheck > now.addSecs(3600)) return due;
  qint64 seconds = kDefaultIntervalSeconds;
  if (entry.lastResult == QLatin1String("failed")) {
    const int doublings = qBound(0, entry.consecutiveFailures - 1, 10);
    seconds = qMin(kFirstRetrySeconds << doublings, kMaximumRetrySeconds);
  } else if (entry.current.expiresSeconds > 0) {
    seconds = qBound(kMinimumIntervalSeconds, entry.current.expiresSeconds, kMaximumIntervalSeconds);
  }
  return entry.lastCheck.addSecs(seconds);
}

// ---------------- Validation ----------------

FilterListValidation validateFilterListDownload(const QByteArray &bytes, const QString &contentType,
                                                const FilterUpdateSource &source, int previousNetworkRules,
                                                const DaliNiraBlockerRedirectResources *resources) {
  FilterListValidation result;
  auto fail = [&result](FilterUpdateFailure failure) {
    result.failure = failure;
    return result;
  };
  if (bytes.size() > source.maximumBytes) return fail(FilterUpdateFailure::Oversized);
  qsizetype first = bytes.startsWith("\xEF\xBB\xBF") ? 3 : 0;
  while (first < bytes.size() && QChar::isSpace(static_cast<uchar>(bytes.at(first)))) ++first;
  if (first >= bytes.size()) return fail(FilterUpdateFailure::Empty);
  // Captive portals and error pages answer 200 with HTML; a filter list
  // never starts with markup.
  const QString mediaType = contentType.section(QLatin1Char(';'), 0, 0).trimmed().toLower();
  if (mediaType == QLatin1String("text/html") || mediaType == QLatin1String("application/xhtml+xml") ||
      bytes.at(first) == '<')
    return fail(FilterUpdateFailure::Html);
  if (bytes.contains('\0')) return fail(FilterUpdateFailure::Invalid);
  QStringDecoder decoder(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
  const QString text = decoder(bytes);
  if (decoder.hasError()) return fail(FilterUpdateFailure::Invalid);

  DaliNiraBlockerEngine::TextFilterListCompilation compiled;
  try {
    compiled = DaliNiraBlockerEngine::compileTextFilterList(text, source.listId, {resources, nullptr});
  } catch (const std::bad_alloc &) {
    return fail(FilterUpdateFailure::Invalid);
  }
  result.cosmeticLines = compiled.cosmeticLines;
  result.unsupportedLines = compiled.unsupportedLines;
  if (compiled.networkRules < source.minimumNetworkRules) return fail(FilterUpdateFailure::TooFewRules);
  if (previousNetworkRules > 0 && static_cast<qint64>(compiled.networkRules) * 4 < previousNetworkRules)
    return fail(FilterUpdateFailure::Shrunk);

  result.copy.sha256 = sha256Hex(bytes);
  result.copy.bytes = bytes.size();
  result.copy.networkRules = compiled.networkRules;
  result.copy.expiresSeconds = expiresSecondsFromHeader(text);
  return result;
}

// ---------------- Fetcher ----------------

DaliNiraBlockerFilterFetcher::DaliNiraBlockerFilterFetcher(QObject *parent) : QObject(parent) {
  deadline_ = new QTimer(this);
  deadline_->setSingleShot(true);
  connect(deadline_, &QTimer::timeout, this, [this] {
    if (!reply_) return;
    abortReason_ = FilterUpdateFailure::Timeout;
    reply_->abort();
  });
}

DaliNiraBlockerFilterFetcher::~DaliNiraBlockerFilterFetcher() { cancel(); }

bool DaliNiraBlockerFilterFetcher::isAllowedUrl(const QUrl &url, bool allowLoopbackHttp) {
  if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty()) return false;
  const QString scheme = url.scheme().toLower();
  if (scheme == QLatin1String("https")) return true;
  if (scheme != QLatin1String("http") || !allowLoopbackHttp) return false;
  const QString host = url.host().toLower();
  return host == QLatin1String("localhost") || QHostAddress(host).isLoopback();
}

void DaliNiraBlockerFilterFetcher::fetch(const QUrl &url, qint64 maximumBytes, const QString &etag,
                                         const QString &lastModified) {
  if (reply_) return;
  maximumBytes_ = maximumBytes;
  etag_ = etag;
  lastModified_ = lastModified;
  body_.clear();
  abortReason_ = FilterUpdateFailure::None;
  tlsError_ = false;
  if (!network_) network_ = new QNetworkAccessManager(this);
  deadline_->start(policy_.totalTimeoutMs);
  request(url, 0);
}

void DaliNiraBlockerFilterFetcher::cancel() {
  deadline_->stop();
  if (!reply_) return;
  QNetworkReply *reply = reply_;
  reply_.clear();
  disconnect(reply, nullptr, this, nullptr);
  reply->abort();
  reply->deleteLater();
  body_.clear();
}

void DaliNiraBlockerFilterFetcher::request(const QUrl &url, int redirectCount) {
  if (!isAllowedUrl(url, policy_.allowLoopbackHttpForTesting)) {
    FilterFetchResult result;
    result.failure = redirectCount > 0 ? FilterUpdateFailure::Redirect : FilterUpdateFailure::Network;
    QTimer::singleShot(0, this, [this, result] { complete(result); });
    return;
  }
  QNetworkRequest networkRequest(url);
  // Redirects are followed by hand so every hop is checked (HTTPS only).
  networkRequest.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
  networkRequest.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
  networkRequest.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
  networkRequest.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
  networkRequest.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
  networkRequest.setTransferTimeout(policy_.transferTimeoutMs);
  networkRequest.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("DaliNira-Browser filter-list-updater"));
  networkRequest.setRawHeader("Accept", "text/plain, */*;q=0.1");
  if (!etag_.isEmpty()) networkRequest.setRawHeader("If-None-Match", etag_.toLatin1());
  if (!lastModified_.isEmpty()) networkRequest.setRawHeader("If-Modified-Since", lastModified_.toLatin1());

  QNetworkReply *reply = network_->get(networkRequest);
  reply_ = reply;
  // Certificate errors are never ignored, so the request fails; note why.
  connect(reply, &QNetworkReply::sslErrors, this, [this](const QList<QSslError> &) { tlsError_ = true; });
  connect(reply, &QNetworkReply::metaDataChanged, this, [this, reply] {
    const QVariant length = reply->header(QNetworkRequest::ContentLengthHeader);
    if (length.isValid() && length.toLongLong() > maximumBytes_) {
      abortReason_ = FilterUpdateFailure::Oversized;
      reply->abort();
    }
  });
  connect(reply, &QNetworkReply::readyRead, this, [this, reply] {
    if (abortReason_ != FilterUpdateFailure::None) return;
    const QByteArray chunk = reply->readAll();
    if (body_.size() + chunk.size() > maximumBytes_) {
      abortReason_ = FilterUpdateFailure::Oversized;
      body_.clear();
      reply->abort();
      return;
    }
    body_.append(chunk);
  });
  connect(reply, &QNetworkReply::finished, this,
          [this, reply, url, redirectCount] { handleFinished(reply, url, redirectCount); });
}

void DaliNiraBlockerFilterFetcher::handleFinished(QNetworkReply *reply, const QUrl &url, int redirectCount) {
  reply->deleteLater();
  if (reply_ != reply) return;  // cancelled
  reply_.clear();
  FilterFetchResult result;
  result.httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  if (abortReason_ != FilterUpdateFailure::None) {
    result.failure = abortReason_;
    complete(result);
    return;
  }
  if (reply->bytesAvailable() > 0) {
    const QByteArray rest = reply->readAll();
    if (body_.size() + rest.size() > maximumBytes_) {
      result.failure = FilterUpdateFailure::Oversized;
      complete(result);
      return;
    }
    body_.append(rest);
  }
  const int status = result.httpStatus;
  if (status >= 300 && status < 400 && status != 304) {
    const QUrl target = url.resolved(reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl());
    if (redirectCount >= policy_.maximumRedirects || !isAllowedUrl(target, policy_.allowLoopbackHttpForTesting)) {
      result.failure = FilterUpdateFailure::Redirect;
      complete(result);
      return;
    }
    body_.clear();
    request(target, redirectCount + 1);
    return;
  }
  if (status == 304) {
    // Only an answer to a conditional request can mean "keep what you have".
    if (etag_.isEmpty() && lastModified_.isEmpty()) result.failure = FilterUpdateFailure::Http;
    else result.kind = FilterFetchResult::Kind::NotModified;
    complete(result);
    return;
  }
  const QNetworkReply::NetworkError error = reply->error();
  if (error != QNetworkReply::NoError) {
    if (tlsError_ || error == QNetworkReply::SslHandshakeFailedError) result.failure = FilterUpdateFailure::Tls;
    else if (error == QNetworkReply::OperationCanceledError || error == QNetworkReply::TimeoutError)
      result.failure = FilterUpdateFailure::Timeout;
    // Codes below 200 are connection and proxy failures (including a body cut
    // short); from 200 on the server answered with an error.
    else if (error >= QNetworkReply::ContentAccessDenied) result.failure = FilterUpdateFailure::Http;
    else result.failure = FilterUpdateFailure::Network;
    complete(result);
    return;
  }
  if (status != 200) {
    result.failure = FilterUpdateFailure::Http;
    complete(result);
    return;
  }
  result.kind = FilterFetchResult::Kind::Downloaded;
  result.body = std::move(body_);
  result.contentType = reply->header(QNetworkRequest::ContentTypeHeader).toString();
  result.etag = QString::fromLatin1(reply->rawHeader("ETag"));
  result.lastModified = QString::fromLatin1(reply->rawHeader("Last-Modified"));
  complete(result);
}

void DaliNiraBlockerFilterFetcher::complete(FilterFetchResult result) {
  deadline_->stop();
  body_.clear();
  emit finished(result);
}
