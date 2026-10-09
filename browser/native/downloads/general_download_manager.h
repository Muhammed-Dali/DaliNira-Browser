#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QList>
#include <QNetworkCookie>
#include <QNetworkReply>
#include <QObject>
#include <QUrl>
#include <QUuid>
#include <QVector>
#include "engine/engine_type.h"

class QNetworkAccessManager;
class QNetworkRequest;
class QTimer;

enum class GeneralDownloadState {
  Queued,
  Probing,
  Downloading,
  Paused,
  Completed,
  Failed,
  Cancelled
};

struct GeneralDownloadRequest {
  QUrl url;
  QString suggestedFileName;
  QString targetDirectory;
  QString mimeType;
  QByteArray userAgent;
  QByteArray referrer;
  QList<QNetworkCookie> cookies;
  qint64 expectedBytes = -1;
  int connectionCount = 8;
  bool allowParallel = true;
  bool allowWorkStealing = true;
  bool overwriteExisting = false;
  dalinira::engine::EngineType sourceEngine = dalinira::engine::EngineType::Chromium;
  uint64_t sourceTabId = 0;
  QUrl pageUrl;
  QUrl originalUrl;
  bool isPrivate = false;
  quint64 navigationGeneration = 0;
};

struct GeneralDownloadJob {
  QUuid id;
  QUrl url;
  QString fileName;
  QString targetPath;
  QString mimeType;
  GeneralDownloadState state = GeneralDownloadState::Queued;
  QString statusText;
  QString errorText;
  qint64 totalBytes = -1;
  qint64 downloadedBytes = 0;
  qint64 bytesPerSecond = 0;
  qint64 etaSeconds = -1;
  int connections = 1;
  bool rangeSupported = false;
  QByteArray etag;
  QByteArray lastModified;
  QDateTime createdAt;
  QDateTime updatedAt;
  dalinira::engine::EngineType sourceEngine = dalinira::engine::EngineType::Chromium;
};

class GeneralDownloadManager final : public QObject {
  Q_OBJECT
 public:
  explicit GeneralDownloadManager(const QString &historyPath,
                                  QNetworkAccessManager *network = nullptr,
                                  QObject *parent = nullptr);
  ~GeneralDownloadManager() override;

  QUuid enqueue(const GeneralDownloadRequest &request);
  QUuid retry(const QUuid &id);
  bool pause(const QUuid &id);
  bool resume(const QUuid &id);
  bool cancel(const QUuid &id);
  bool remove(const QUuid &id);

  QVector<GeneralDownloadJob> jobs() const;
  GeneralDownloadJob job(const QUuid &id) const;
  GeneralDownloadRequest request(const QUuid &id) const;
  bool hasActiveJobs() const;
  QString tempDownloadPath(const QUuid &id) const;
  QString statePath(const QUuid &id) const;

  int maxActiveConnections() const;
  void setMaxActiveConnections(int limit);
  int activeConnectionCount() const;
  int partCount(const QUuid &id) const;

  static bool canHandleUrl(const QUrl &url);
  static int normalizedConnectionCount(int requested);
  static QString stateText(GeneralDownloadState state);
  static bool isTransientNetworkError(QNetworkReply::NetworkError error, int httpStatusCode = 0);

signals:
  void jobsChanged();
  void jobEnqueued(const QUuid &id);

 private:
  struct Part;
  struct Task;

  int indexOf(const QUuid &id) const;
  void begin(const QUuid &id);
  void scheduleQueue();
  void startHeadProbe(Task *task);
  void startRangeProbe(Task *task);
  void finishProbe(Task *task, bool rangeSupported, qint64 totalBytes,
                   const QByteArray &etag, const QByteArray &lastModified);
  void prepareTransfer(Task *task);
  void prepareParallel(Task *task);
  void prepareSingle(Task *task, bool resumeExisting = true);
  void startPart(Task *task, int partIndex, qint64 start, qint64 end, bool useRange);
  void schedulePartRetry(Task *task, int partIndex);
  void consumePart(Task *task, int partIndex);
  void finishPart(Task *task, int partIndex);
  void fallbackToSingle(Task *task);
  bool finalizeDownload(Task *task, QString *error);
  void finish(Task *task, GeneralDownloadState state, const QString &error = {});
  void evaluateAdaptiveScaling(Task *task);
  void scaleConnections(Task *task, int newTarget);
  bool tryWorkSteal(Task *task);
  void abortReplies(Task *task);
  void cleanupParts(const QUuid &id);
  void saveState(Task *task) const;
  bool loadState(const QUuid &id, QVector<Part> *parts, qint64 *totalBytes = nullptr,
                 QByteArray *etag = nullptr, QByteArray *lastModified = nullptr,
                 int *targetConnections = nullptr) const;
  QString uniqueTargetPath(const QString &directory, const QString &fileName) const;
  void applyRequestHeaders(QNetworkRequest *request, const GeneralDownloadRequest &source) const;
  void updateProgress(Task *task);
  void persist() const;
  void load();

  QString historyPath_;
  QNetworkAccessManager *network_ = nullptr;
  QVector<GeneralDownloadJob> jobs_;
  QHash<QUuid, GeneralDownloadRequest> requests_;
  QHash<QUuid, Task *> tasks_;
  QList<QUuid> queueOrder_;
  int maxActiveConnections_ = 12;
  int rrIndex_ = 0;
  QTimer *speedTimer_ = nullptr;
  QHash<QUuid, int> completedPartCounts_;
};
