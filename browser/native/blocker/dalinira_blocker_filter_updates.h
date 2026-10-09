#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QUrl>
#include <optional>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;
class DaliNiraBlockerRedirectResources;

// Remote filter-list updates. One store per profile directory serves the
// shared DaliNiraBlockerListManager, so Chromium and WebKit always compile the
// same validated copy; neither engine downloads or keeps lists of its own.
//
// Only lists named in the bundled filter-update-sources.json are downloaded.
// Everything else, and every list until its first validated download, uses
// the ruleset bundled with the application.

// Why an update attempt ended without new data. Persisted and logged by name.
enum class FilterUpdateFailure {
  None,
  Network,
  Timeout,
  Tls,
  Http,
  Redirect,
  Oversized,
  Empty,
  Html,
  Invalid,
  TooFewRules,
  Shrunk,
  Storage,
  Cancelled
};

QString filterUpdateFailureName(FilterUpdateFailure failure);
FilterUpdateFailure filterUpdateFailureFromName(const QString &name);

struct FilterUpdateSource {
  QString listId;
  QUrl url;
  int minimumNetworkRules = 100;
  qint64 maximumBytes = 12 * 1024 * 1024;
};

// One validated, stored copy of a list. Copies are content addressed: the
// file name is the SHA-256 of the bytes, which is re-checked before use.
struct FilterListCopy {
  QString sha256;
  qint64 bytes = 0;
  int networkRules = 0;
  QDateTime activatedAt;
  qint64 expiresSeconds = 0;  // the list's "! Expires:" header; 0 when absent
  bool isValid() const { return sha256.size() == 64; }
};

struct FilterListUpdateState {
  QString sourceUrl;  // the URL etag/lastModified were received from
  FilterListCopy current;
  FilterListCopy previous;  // last known good before current
  QString etag;
  QString lastModified;
  QDateTime lastCheck;
  QDateTime lastSuccess;  // last check that installed or confirmed valid data
  QString lastResult;     // "updated", "not-modified" or "failed"
  FilterUpdateFailure lastFailure = FilterUpdateFailure::None;
  int consecutiveFailures = 0;
};

// Crash-safe storage of downloaded lists and their update metadata.
//
//   <dir>/state.json          the only activation record, replaced atomically
//   <dir>/state.lock          serializes writers (also across processes)
//   <dir>/<listId>/<sha>.txt  validated copies, written before state.json
//
// A copy becomes active only when state.json names it, so an interrupted
// update leaves the previous record in force; startup verifies the named
// copies (current, then previous, then the bundled ruleset) and removes the
// store's own unreferenced or temporary files. Thread-safe.
class DaliNiraBlockerFilterStore final {
 public:
  // Tests stop a commit at this point to simulate the browser being killed.
  enum class CrashPoint { None, AfterCopyWritten };

  DaliNiraBlockerFilterStore(const QString &directory, const QString &sourcesManifestPath,
                             const QSet<QString> &eligibleListIds);

  QString directory() const { return directory_; }
  QList<FilterUpdateSource> sources() const { return sources_.values(); }
  std::optional<FilterUpdateSource> source(const QString &listId) const;
  FilterListUpdateState state(const QString &listId) const;

  // The bytes of the list's active copy: current, else previous, each only
  // when its hash still matches. Empty means the bundled ruleset applies.
  QByteArray readActiveCopy(const QString &listId, bool *fromPrevious = nullptr) const;
  // "remote", "remote-previous" or "bundled", without reading the copies.
  QString activeSource(const QString &listId) const;

  // Stores `bytes` (already validated) and makes it the current copy; the
  // old current becomes previous. False leaves the active copy unchanged.
  bool commitValidatedCopy(const QString &listId, const QByteArray &bytes, const FilterListCopy &copy,
                           const QString &sourceUrl, const QString &etag, const QString &lastModified);
  bool recordNotModified(const QString &listId);
  bool recordFailure(const QString &listId, FilterUpdateFailure failure);

  // When the list should next be checked automatically (the epoch when due).
  QDateTime nextCheckDue(const QString &listId) const;

  void setCrashPointForTesting(CrashPoint point) { crashPoint_ = point; }

 private:
  using StateMap = QHash<QString, FilterListUpdateState>;
  void loadSources(const QString &manifestPath, const QSet<QString> &eligibleListIds);
  StateMap readStateFile() const;
  bool writeStateFile(const StateMap &states) const;
  QString copyPath(const QString &listId, const QString &sha256) const;
  bool copyIntact(const QString &listId, const FilterListCopy &copy) const;
  void recover();
  void removeStaleFiles(const StateMap &states) const;
  template <typename Change>
  bool update(const QString &listId, Change change);

  QString directory_;
  QHash<QString, FilterUpdateSource> sources_;
  mutable QMutex mutex_;
  StateMap states_;
  mutable QSet<QString> damagedCopies_;  // hashes found not to match on read
  CrashPoint crashPoint_ = CrashPoint::None;
};

struct FilterListValidation {
  FilterUpdateFailure failure = FilterUpdateFailure::None;
  FilterListCopy copy;
  int cosmeticLines = 0;
  int unsupportedLines = 0;
};

// Checks a download before it may be stored: not empty, not an HTML page,
// valid UTF-8 text, and compiled by the blocker's own filter syntax into at
// least the source's minimum number of network rules (and not less than a
// quarter of the previous copy, which catches truncated responses).
// `resources` lets $redirect lines count as the rules they compile to.
FilterListValidation validateFilterListDownload(const QByteArray &bytes, const QString &contentType,
                                                const FilterUpdateSource &source, int previousNetworkRules,
                                                const DaliNiraBlockerRedirectResources *resources = nullptr);

struct FilterFetchResult {
  enum class Kind { Downloaded, NotModified, Failed };
  Kind kind = Kind::Failed;
  FilterUpdateFailure failure = FilterUpdateFailure::None;
  int httpStatus = 0;
  QByteArray body;
  QString contentType;
  QString etag;
  QString lastModified;
};

// Downloads one list at a time over the application's Qt network stack:
// certificate verification is never relaxed, redirects are followed only to
// HTTPS URLs, and cookies are neither sent nor stored.
class DaliNiraBlockerFilterFetcher final : public QObject {
  Q_OBJECT
 public:
  struct Policy {
    int transferTimeoutMs = 30000;  // no progress for this long
    int totalTimeoutMs = 120000;
    int maximumRedirects = 5;
    // Tests serve lists from a local plain-HTTP fixture; the browser never sets this.
    bool allowLoopbackHttpForTesting = false;
  };

  explicit DaliNiraBlockerFilterFetcher(QObject *parent = nullptr);
  ~DaliNiraBlockerFilterFetcher() override;

  void setPolicy(const Policy &policy) { policy_ = policy; }
  const Policy &policy() const { return policy_; }
  static bool isAllowedUrl(const QUrl &url, bool allowLoopbackHttp);

  bool isBusy() const { return !reply_.isNull(); }
  // `finished` follows exactly once, asynchronously, unless cancelled first.
  void fetch(const QUrl &url, qint64 maximumBytes, const QString &etag, const QString &lastModified);
  void cancel();

 signals:
  void finished(const FilterFetchResult &result);

 private:
  void request(const QUrl &url, int redirectCount);
  void handleFinished(QNetworkReply *reply, const QUrl &url, int redirectCount);
  void complete(FilterFetchResult result);

  Policy policy_;
  QNetworkAccessManager *network_ = nullptr;
  QPointer<QNetworkReply> reply_;
  QTimer *deadline_ = nullptr;
  qint64 maximumBytes_ = 0;
  QString etag_;
  QString lastModified_;
  QByteArray body_;
  FilterUpdateFailure abortReason_ = FilterUpdateFailure::None;
  bool tlsError_ = false;
};
