#include "general_download_manager.h"
#include "i18n/i18n.h"

#include "security_utils.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkCookieJar>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimer>

#include <algorithm>

#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cstdio>
#endif

namespace {

Q_LOGGING_CATEGORY(lcDownloadTelemetry, "dalinira.download.telemetry", QtWarningMsg)

constexpr qint64 kParallelThreshold = 8 * 1024 * 1024;
constexpr int kMaxPartRetries = 3;
constexpr int kBaseRetryDelayMs = 100;
constexpr qint64 kMinPartSplitSize = 256 * 1024;
constexpr qint64 kMinRemainingToScale = 512 * 1024;
constexpr double kScaleUpGainThreshold = 0.15;     // +15% gain → scale up
constexpr double kScaleDownLossThreshold = 0.30;   // -30% sustained loss → scale down candidate
constexpr int kScaleDownRequiredSamples = 3;        // consecutive bad evaluations needed before scale-down
// Post-scale-UP warmup dwell (protects against premature scale-down during TCP slow-start / TLS)
constexpr qint64 kPostScaleUpWarmupDwellMs = 1500;
constexpr qint64 kPostScaleUpWarmupDwellBytes = 512 * 1024;
// Post-scale-DOWN dwell (cascade prevention)
constexpr qint64 kPostScaleDownDwellMs = 2000;
constexpr qint64 kPostScaleDownDwellBytes = 1 * 1024 * 1024;
constexpr qint64 kMinEvaluationIntervalMs = 200;   // minimum interval between scale evaluations
constexpr qint64 kMinBytesEvaluated = 128 * 1024;   // minimum bytes transferred before evaluation
constexpr double kEwmaAlpha = 0.2;                 // slower EWMA for jitter resistance
constexpr int kMaxAdaptiveConnections = 8;
constexpr qint64 kScaleDownCooldownMs = 4000;      // don't scale down again within 4s of last scale-down
constexpr qint64 kMinWorkStealSplitSize = 256 * 1024;

QByteArray headerValue(QNetworkReply *reply, const QByteArray &name) {
  return reply ? reply->rawHeader(name).trimmed() : QByteArray{};
}

bool parseContentRange(const QByteArray &header, qint64 *start, qint64 *end, qint64 *total) {
  static const QRegularExpression expression(QStringLiteral(R"(^bytes\s+(\d+)-(\d+)/(\d+)$)"),
                                              QRegularExpression::CaseInsensitiveOption);
  const auto match = expression.match(QString::fromLatin1(header).trimmed());
  if (!match.hasMatch()) return false;
  if (start) *start = match.captured(1).toLongLong();
  if (end) *end = match.captured(2).toLongLong();
  if (total) *total = match.captured(3).toLongLong();
  return true;
}

qint64 contentRangeTotal(const QByteArray &header) {
  qint64 total = -1;
  return parseContentRange(header, nullptr, nullptr, &total) ? total : -1;
}

bool terminal(GeneralDownloadState state) {
  return state == GeneralDownloadState::Completed || state == GeneralDownloadState::Failed
      || state == GeneralDownloadState::Cancelled;
}

bool atomicReplaceFile(const QString &source, const QString &target) {
#if defined(Q_OS_WIN)
  return MoveFileExW(reinterpret_cast<LPCWSTR>(source.utf16()),
                     reinterpret_cast<LPCWSTR>(target.utf16()),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
  const QByteArray encodedSource = QFile::encodeName(source);
  const QByteArray encodedTarget = QFile::encodeName(target);
  return ::rename(encodedSource.constData(), encodedTarget.constData()) == 0;
#endif
}

struct TaskTelemetry {
  qint64 startTimeMs = 0;
  qint64 sessionStartMs = 0;
  qint64 elapsedActiveMs = 0;
  qint64 peakThroughput = 0;
  int maxConnectionsUsed = 1;
  QList<int> connectionPath;
  int scaleUps = 0;
  int scaleDowns = 0;
  int workSteals = 0;
  int retries = 0;
  int throttleEvents = 0;
  bool schedulerLimitedLogged = false;
};

QHash<QUuid, TaskTelemetry> s_savedTelemetry;

}  // namespace

enum class TaskPhase {
  PendingProbe,
  Probing,
  Transferring
};

struct GeneralDownloadManager::Part {
  qint64 start = 0;
  qint64 end = -1;
  qint64 initialSize = 0;
  qint64 written = 0;
  int retryCount = 0;
  QPointer<QNetworkReply> reply;
  QPointer<QTimer> retryTimer;
  bool finished = false;
  double bytesPerSecond = 0.0;
  qint64 lastMeasureBytes = 0;
  qint64 lastMeasureMs = 0;
};

struct GeneralDownloadManager::Task {
  QUuid id;
  GeneralDownloadRequest request;
  TaskPhase phase = TaskPhase::PendingProbe;
  QVector<Part> parts;
  QPointer<QNetworkReply> probe;
  QFile *targetFile = nullptr;
  bool rangeSupported = false;
  bool fallingBack = false;
  qint64 lastMeasuredBytes = 0;
  qint64 lastMeasureMs = 0;
  qint64 lastEmitMs = 0;
  qint64 lastEmittedBytes = 0;

  bool adaptiveEnabled = false;
  int targetConnections = 1;

  // Aggregate EWMA for the current stage (resets on topology change)
  double smoothedThroughput = 0.0;

  // Comparison baseline: the stable throughput of the *previous* stage.
  // Set when we confirm stability, used only for scale-up/down comparisons.
  // Reset to 0 after scale-down to prevent cascade.
  double comparisonBaseline = 0.0;

  // Tracks the bytes transferred when the last topology change happened.
  // Used for the post-scale dwell-byte requirement.
  qint64 stageStartBytes = 0;

  // Whether the last scale change was a scale-down (determines which dwell thresholds to use)
  bool lastScaleWasDown = false;

  qint64 lastScaleChangeMs = 0;    // When the last topology change occurred
  qint64 lastEvaluationMs = 0;
  qint64 lastScaleDownMs = 0;      // When the last scale-down occurred

  int consecutiveErrors = 0;
  int consecutiveLossSamples = 0;  // Consecutive evaluations showing sustained loss
  bool scaleUpHalted = false;
  bool stageStableLogged = false;

  TaskTelemetry telemetry;
};


GeneralDownloadManager::GeneralDownloadManager(const QString &historyPath,
                                               QNetworkAccessManager *network,
                                               QObject *parent)
    : QObject(parent), historyPath_(QFileInfo(historyPath).absoluteFilePath()), network_(network) {
  if (!network_) {
    network_ = new QNetworkAccessManager(this);
    network_->setCookieJar(new QNetworkCookieJar(network_));
  } else if (!network_->cookieJar()) {
    network_->setCookieJar(new QNetworkCookieJar(network_));
  }
  load();
  speedTimer_ = new QTimer(this);
  speedTimer_->setInterval(1000);
  connect(speedTimer_, &QTimer::timeout, this, [this] {
    for (Task *task : std::as_const(tasks_)) updateProgress(task);
  });
  speedTimer_->start();
  // Status texts are cached in the UI language; re-express them after a live
  // language switch so views built later never show the previous language.
  connect(&dalinira::i18n::LanguageManager::instance(), &dalinira::i18n::LanguageManager::languageChanged,
          this, [this] {
            for (auto &job : jobs_) job.statusText = I18n::retranslated(job.statusText);
            emit jobsChanged();
          });
}

GeneralDownloadManager::~GeneralDownloadManager() {
  const auto activeTasks = tasks_.values();
  for (Task *task : activeTasks) {
    abortReplies(task);
    delete task;
  }
  tasks_.clear();
  for (const GeneralDownloadJob &job : std::as_const(jobs_)) s_savedTelemetry.remove(job.id);
  persist();
}

bool GeneralDownloadManager::canHandleUrl(const QUrl &url) {
  if (!url.isValid() || !url.userInfo().isEmpty()) return false;
  return url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https");
}

int GeneralDownloadManager::normalizedConnectionCount(int requested) {
  if (requested >= 8) return 8;
  if (requested >= 4) return 4;
  if (requested >= 2) return 2;
  return 1;
}

QString GeneralDownloadManager::stateText(GeneralDownloadState state) {
  switch (state) {
    case GeneralDownloadState::Queued: return I18n::text(QStringLiteral("downloads.state_queued"));
    case GeneralDownloadState::Probing: return I18n::text(QStringLiteral("downloads.probing_server"));
    case GeneralDownloadState::Downloading: return I18n::text(QStringLiteral("downloads.state_downloading"));
    case GeneralDownloadState::Paused: return I18n::text(QStringLiteral("downloads.state_paused"));
    case GeneralDownloadState::Completed: return I18n::text(QStringLiteral("downloads.state_completed"));
    case GeneralDownloadState::Failed: return I18n::text(QStringLiteral("common.failed"));
    case GeneralDownloadState::Cancelled: return I18n::text(QStringLiteral("downloads.state_cancelled"));
  }
  return {};
}

int GeneralDownloadManager::maxActiveConnections() const {
  return maxActiveConnections_;
}

void GeneralDownloadManager::setMaxActiveConnections(int limit) {
  maxActiveConnections_ = std::max(1, limit);
  scheduleQueue();
}

int GeneralDownloadManager::activeConnectionCount() const {
  int count = 0;
  for (const Task *task : tasks_) {
    if (!task) continue;
    if (task->probe) ++count;
    for (const Part &part : task->parts) {
      if (part.reply) ++count;
    }
  }
  return count;
}

int GeneralDownloadManager::partCount(const QUuid &id) const {
  const Task *task = tasks_.value(id, nullptr);
  if (task) {
    return task->parts.size();
  }
  if (completedPartCounts_.contains(id)) {
    return completedPartCounts_.value(id);
  }
  QVector<Part> parts;
  if (loadState(id, &parts)) {
    return parts.size();
  }
  return 0;
}

bool GeneralDownloadManager::isTransientNetworkError(QNetworkReply::NetworkError error, int httpStatusCode) {
  if (httpStatusCode == 400 || httpStatusCode == 401 || httpStatusCode == 403 ||
      httpStatusCode == 404 || httpStatusCode == 405 || httpStatusCode == 410 ||
      httpStatusCode == 416) {
    return false;
  }
  if (httpStatusCode == 429 || httpStatusCode == 500 || httpStatusCode == 502 ||
      httpStatusCode == 503 || httpStatusCode == 504) {
    return true;
  }
  switch (error) {
    case QNetworkReply::NoError:
      return false;
    case QNetworkReply::ConnectionRefusedError:
    case QNetworkReply::RemoteHostClosedError:
    case QNetworkReply::HostNotFoundError:
    case QNetworkReply::TimeoutError:
    case QNetworkReply::TemporaryNetworkFailureError:
    case QNetworkReply::NetworkSessionFailedError:
    case QNetworkReply::BackgroundRequestNotAllowedError:
    case QNetworkReply::ProxyConnectionRefusedError:
    case QNetworkReply::ProxyConnectionClosedError:
    case QNetworkReply::ProxyTimeoutError:
    case QNetworkReply::InternalServerError:
    case QNetworkReply::ServiceUnavailableError:
    case QNetworkReply::UnknownNetworkError:
    case QNetworkReply::UnknownServerError:
      return true;
    default:
      return false;
  }
}

QUuid GeneralDownloadManager::enqueue(const GeneralDownloadRequest &candidate) {
  if (!canHandleUrl(candidate.url)) return {};
  const QFileInfo directoryInfo(candidate.targetDirectory);
  if (!directoryInfo.isDir() || !directoryInfo.isWritable()) return {};
  GeneralDownloadRequest request = candidate;
  request.suggestedFileName = BrowserSecurity::sanitizeDownloadFileName(
      request.suggestedFileName, QStringLiteral("download"));
  request.targetDirectory = directoryInfo.absoluteFilePath();
  request.connectionCount = normalizedConnectionCount(request.connectionCount);
  if (request.expectedBytes > 0 && request.expectedBytes < kParallelThreshold)
    request.allowParallel = false;

  GeneralDownloadJob job;
  job.id = QUuid::createUuid();
  job.url = request.url;
  job.fileName = request.suggestedFileName;
  job.sourceEngine = request.sourceEngine;
  const QString exactTarget = QDir(request.targetDirectory).absoluteFilePath(job.fileName);
  const bool exactTargetActive = std::any_of(jobs_.cbegin(), jobs_.cend(), [&exactTarget](const auto &existing) {
    return !terminal(existing.state) && existing.targetPath == exactTarget;
  });
  job.targetPath = request.overwriteExisting && !exactTargetActive
      ? exactTarget : uniqueTargetPath(request.targetDirectory, job.fileName);
  job.fileName = QFileInfo(job.targetPath).fileName();
  job.mimeType = request.mimeType;
  job.state = GeneralDownloadState::Queued;
  job.statusText = stateText(job.state);
  job.totalBytes = request.expectedBytes;
  job.createdAt = QDateTime::currentDateTimeUtc();
  job.updatedAt = job.createdAt;
  jobs_.prepend(job);
  requests_.insert(job.id, request);
  persist();
  emit jobEnqueued(job.id);
  emit jobsChanged();
  QTimer::singleShot(0, this, [this, id = job.id] { begin(id); });
  return job.id;
}

QUuid GeneralDownloadManager::retry(const QUuid &id) {
  const int index = indexOf(id);
  if (index < 0 || !terminal(jobs_.at(index).state)) return {};
  GeneralDownloadRequest request = requests_.value(id);
  if (!canHandleUrl(request.url)) return {};
  request.suggestedFileName = jobs_.at(index).fileName;
  request.targetDirectory = QFileInfo(jobs_.at(index).targetPath).absolutePath();
  if (jobs_.at(index).state == GeneralDownloadState::Completed) request.overwriteExisting = false;
  return enqueue(request);
}

bool GeneralDownloadManager::pause(const QUuid &id) {
  const int index = indexOf(id);
  if (index < 0 || (jobs_[index].state != GeneralDownloadState::Downloading
                    && jobs_[index].state != GeneralDownloadState::Probing
                    && jobs_[index].state != GeneralDownloadState::Queued)) return false;
  queueOrder_.removeAll(id);
  Task *task = tasks_.take(id);
  if (task) {
    task->telemetry.elapsedActiveMs += (QDateTime::currentMSecsSinceEpoch() - task->telemetry.sessionStartMs);
    s_savedTelemetry.insert(id, task->telemetry);
    saveState(task);
    abortReplies(task);
    delete task;
  }
  jobs_[index].state = GeneralDownloadState::Paused;
  jobs_[index].statusText = stateText(GeneralDownloadState::Paused);
  jobs_[index].bytesPerSecond = 0;
  jobs_[index].etaSeconds = -1;
  jobs_[index].updatedAt = QDateTime::currentDateTimeUtc();
  persist();
  emit jobsChanged();
  scheduleQueue();
  return true;
}

bool GeneralDownloadManager::resume(const QUuid &id) {
  const int index = indexOf(id);
  if (index < 0 || jobs_[index].state != GeneralDownloadState::Paused || tasks_.contains(id)) return false;
  jobs_[index].state = GeneralDownloadState::Queued;
  jobs_[index].statusText = I18n::text(QStringLiteral("downloads.resuming"));
  emit jobsChanged();
  QTimer::singleShot(0, this, [this, id] { begin(id); });
  return true;
}

bool GeneralDownloadManager::cancel(const QUuid &id) {
  const int index = indexOf(id);
  if (index < 0 || terminal(jobs_[index].state)) return false;
  queueOrder_.removeAll(id);
  Task *task = tasks_.take(id);
  if (task) {
    abortReplies(task);
    delete task;
  }
  s_savedTelemetry.remove(id);
  cleanupParts(id);
  jobs_[index].state = GeneralDownloadState::Cancelled;
  jobs_[index].statusText = stateText(GeneralDownloadState::Cancelled);
  jobs_[index].bytesPerSecond = 0;
  jobs_[index].etaSeconds = -1;
  jobs_[index].updatedAt = QDateTime::currentDateTimeUtc();
  persist();
  emit jobsChanged();
  scheduleQueue();
  return true;
}

bool GeneralDownloadManager::remove(const QUuid &id) {
  const int index = indexOf(id);
  if (index < 0 || !terminal(jobs_[index].state)) return false;
  queueOrder_.removeAll(id);
  cleanupParts(id);
  jobs_.removeAt(index);
  requests_.remove(id);
  completedPartCounts_.remove(id);
  persist();
  emit jobsChanged();
  scheduleQueue();
  return true;
}

QVector<GeneralDownloadJob> GeneralDownloadManager::jobs() const { return jobs_; }

GeneralDownloadJob GeneralDownloadManager::job(const QUuid &id) const {
  const int index = indexOf(id);
  return index >= 0 ? jobs_.at(index) : GeneralDownloadJob{};
}

GeneralDownloadRequest GeneralDownloadManager::request(const QUuid &id) const {
  return requests_.value(id);
}

bool GeneralDownloadManager::hasActiveJobs() const {
  for (const GeneralDownloadJob &job : jobs_) {
    if (job.state == GeneralDownloadState::Downloading
        || job.state == GeneralDownloadState::Probing
        || job.state == GeneralDownloadState::Queued
        || job.state == GeneralDownloadState::Paused) {
      return true;
    }
  }
  return false;
}

int GeneralDownloadManager::indexOf(const QUuid &id) const {
  for (int index = 0; index < jobs_.size(); ++index)
    if (jobs_.at(index).id == id) return index;
  return -1;
}

void GeneralDownloadManager::begin(const QUuid &id) {
  const int index = indexOf(id);
  if (index < 0 || tasks_.contains(id)) return;
  auto *task = new Task;
  task->id = id;
  task->phase = TaskPhase::PendingProbe;
  task->request = requests_.value(id);
  task->lastMeasureMs = QDateTime::currentMSecsSinceEpoch();
  task->lastMeasuredBytes = jobs_[index].downloadedBytes;
  if (s_savedTelemetry.contains(id)) {
    task->telemetry = s_savedTelemetry.take(id);
    task->telemetry.sessionStartMs = task->lastMeasureMs;
  } else {
    task->telemetry.startTimeMs = task->lastMeasureMs;
    task->telemetry.sessionStartMs = task->lastMeasureMs;
    task->telemetry.connectionPath.append(1);
  }
  tasks_.insert(id, task);
  if (!queueOrder_.contains(id)) {
    queueOrder_.append(id);
  }
  jobs_[index].state = GeneralDownloadState::Queued;
  jobs_[index].statusText = I18n::text(QStringLiteral("downloads.waiting_connection"));
  jobs_[index].errorText.clear();
  jobs_[index].updatedAt = QDateTime::currentDateTimeUtc();
  emit jobsChanged();
  scheduleQueue();
}

void GeneralDownloadManager::scheduleQueue() {
  if (queueOrder_.isEmpty()) return;

  bool anyStarted = true;
  while (anyStarted && activeConnectionCount() < maxActiveConnections_) {
    anyStarted = false;
    const int taskCount = queueOrder_.size();
    if (taskCount == 0) break;
    for (int step = 0; step < taskCount; ++step) {
      if (activeConnectionCount() >= maxActiveConnections_) break;
      const int idx = (rrIndex_ + step) % taskCount;
      const QUuid id = queueOrder_.at(idx);
      Task *task = tasks_.value(id, nullptr);
      if (!task) continue;

      if (task->phase == TaskPhase::PendingProbe) {
        task->phase = TaskPhase::Probing;
        const int jobIndex = indexOf(id);
        if (jobIndex >= 0) {
          jobs_[jobIndex].state = GeneralDownloadState::Probing;
          jobs_[jobIndex].statusText = stateText(GeneralDownloadState::Probing);
        }
        startHeadProbe(task);
        anyStarted = true;
        rrIndex_ = (idx + 1) % taskCount;
        break;
      } else if (task->phase == TaskPhase::Transferring) {
        int activeForTask = 0;
        for (const Part &p : task->parts) {
          if (p.reply) ++activeForTask;
        }
        if (activeForTask >= task->targetConnections) {
          continue;
        }

        int candidatePart = -1;
        for (int p = 0; p < task->parts.size(); ++p) {
          Part &part = task->parts[p];
          if (!part.finished && !part.reply && !part.retryTimer) {
            candidatePart = p;
            break;
          }
        }
        if (candidatePart < 0 && task->request.allowWorkStealing) {
          if (tryWorkSteal(task)) {
            for (int p = 0; p < task->parts.size(); ++p) {
              Part &part = task->parts[p];
              if (!part.finished && !part.reply && !part.retryTimer) {
                candidatePart = p;
                break;
              }
            }
          }
        }
        if (candidatePart >= 0) {
          Part &part = task->parts[candidatePart];
          const bool useRange = task->rangeSupported && (task->parts.size() > 1 || part.written > 0 || task->adaptiveEnabled);
          startPart(task, candidatePart, part.start + part.written, part.end, useRange);
          anyStarted = true;
          rrIndex_ = (idx + 1) % taskCount;
          break;
        }
      }
    }
  }

  for (const QUuid &id : std::as_const(queueOrder_)) {
    Task *task = tasks_.value(id, nullptr);
    if (!task || task->phase != TaskPhase::Transferring) continue;
    int activeForTask = 0;
    for (const Part &p : task->parts) {
      if (p.reply) ++activeForTask;
    }
    const bool isLimited = (activeForTask < task->targetConnections && activeConnectionCount() >= maxActiveConnections_);
    if (isLimited && !task->telemetry.schedulerLimitedLogged) {
      task->telemetry.schedulerLimitedLogged = true;
      const int jobIdx = indexOf(task->id);
      qCInfo(lcDownloadTelemetry).noquote() << QStringLiteral("[DownloadTelemetry] SCHEDULER LIMITED: file=%1 requested=%2 active=%3 globalActive=%4 globalLimit=%5")
          .arg(jobIdx >= 0 ? jobs_[jobIdx].fileName : QString())
          .arg(task->targetConnections)
          .arg(activeForTask)
          .arg(activeConnectionCount())
          .arg(maxActiveConnections_);
    } else if (!isLimited) {
      task->telemetry.schedulerLimitedLogged = false;
    }
  }

  bool changed = false;
  for (const QUuid &id : std::as_const(queueOrder_)) {
    Task *task = tasks_.value(id, nullptr);
    const int jobIndex = indexOf(id);
    if (!task || jobIndex < 0) continue;
    GeneralDownloadJob &job = jobs_[jobIndex];
    if (terminal(job.state) || job.state == GeneralDownloadState::Paused) continue;

    GeneralDownloadState newState = job.state;
    QString newStatus = job.statusText;

    if (task->phase == TaskPhase::PendingProbe) {
      newState = GeneralDownloadState::Queued;
      newStatus = I18n::text(QStringLiteral("downloads.waiting_connection"));
    } else if (task->phase == TaskPhase::Probing) {
      newState = GeneralDownloadState::Probing;
      newStatus = stateText(GeneralDownloadState::Probing);
    } else if (task->phase == TaskPhase::Transferring) {
      int activeReplies = 0;
      int retryingParts = 0;
      for (const Part &p : task->parts) {
        if (p.reply) ++activeReplies;
        if (p.retryTimer) ++retryingParts;
      }
      if (activeReplies > 0) {
        newState = GeneralDownloadState::Downloading;
        if (task->adaptiveEnabled) {
          job.connections = task->targetConnections;
          if (task->targetConnections == 1) {
            newStatus = I18n::text(QStringLiteral("downloads.one_connection_measuring"));
          } else {
            newStatus = I18n::text(QStringLiteral("downloads.connections_auto")).arg(task->targetConnections);
          }
        } else {
          job.connections = 1;
          newStatus = I18n::text(QStringLiteral("downloads.state_downloading"));
        }
      } else if (retryingParts == 0) {
        newState = GeneralDownloadState::Queued;
        newStatus = I18n::text(QStringLiteral("downloads.waiting_connection"));
      }
    }

    if (job.state != newState || job.statusText != newStatus) {
      job.state = newState;
      job.statusText = newStatus;
      job.updatedAt = QDateTime::currentDateTimeUtc();
      changed = true;
    }
  }

  if (changed) {
    emit jobsChanged();
  }
}

void GeneralDownloadManager::startHeadProbe(Task *task) {
  QNetworkRequest request(task->request.url);
  applyRequestHeaders(&request, task->request);
  QNetworkReply *reply = network_->head(request);
  task->probe = reply;
  connect(reply, &QNetworkReply::finished, this, [this, id = task->id, reply] {
    Task *current = tasks_.value(id, nullptr);
    if (!current || current->probe != reply) { reply->deleteLater(); scheduleQueue(); return; }
    current->probe = nullptr;
    const qint64 total = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
    const QByteArray ranges = headerValue(reply, QByteArrayLiteral("Accept-Ranges")).toLower();
    const QByteArray etag = headerValue(reply, QByteArrayLiteral("ETag"));
    const QByteArray modified = headerValue(reply, QByteArrayLiteral("Last-Modified"));
    const int jobIndex = indexOf(id);
    const bool resuming = jobIndex >= 0 && jobs_.at(jobIndex).downloadedBytes > 0;
    const bool accelerationCandidate = current->request.allowParallel
        && current->request.connectionCount > 1 && total >= kParallelThreshold;
    const bool canTryRange = reply->error() == QNetworkReply::NoError && total > 0
        && (ranges.contains("bytes") || resuming) && (accelerationCandidate || resuming);
    reply->deleteLater();
    if (canTryRange) startRangeProbe(current);
    else finishProbe(current, false, total > 0 ? total : current->request.expectedBytes, etag, modified);
  });
}

void GeneralDownloadManager::startRangeProbe(Task *task) {
  QNetworkRequest request(task->request.url);
  applyRequestHeaders(&request, task->request);
  request.setRawHeader(QByteArrayLiteral("Range"), QByteArrayLiteral("bytes=0-0"));
  QNetworkReply *reply = network_->get(request);
  task->probe = reply;
  connect(reply, &QNetworkReply::readyRead, reply, [reply] {
    if (reply->bytesAvailable() > 4096) reply->abort();
  });
  connect(reply, &QNetworkReply::finished, this, [this, id = task->id, reply] {
    Task *current = tasks_.value(id, nullptr);
    if (!current || current->probe != reply) { reply->deleteLater(); scheduleQueue(); return; }
    current->probe = nullptr;
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const qint64 total = contentRangeTotal(headerValue(reply, QByteArrayLiteral("Content-Range")));
    const QByteArray etag = headerValue(reply, QByteArrayLiteral("ETag"));
    const QByteArray modified = headerValue(reply, QByteArrayLiteral("Last-Modified"));
    const bool supported = status == 206 && total > 0;
    reply->deleteLater();
    finishProbe(current, supported, total > 0 ? total : current->request.expectedBytes, etag, modified);
  });
}

void GeneralDownloadManager::finishProbe(Task *task, bool rangeSupported, qint64 totalBytes,
                                         const QByteArray &etag, const QByteArray &lastModified) {
  const int index = indexOf(task->id);
  if (index < 0 || tasks_.value(task->id) != task) return;
  GeneralDownloadJob &job = jobs_[index];
  const bool changed = (job.totalBytes > 0 && totalBytes > 0 && job.totalBytes != totalBytes)
      || (!job.etag.isEmpty() && job.etag != etag)
      || (!job.lastModified.isEmpty() && job.lastModified != lastModified);
  if (changed) {
    cleanupParts(task->id);
    job.downloadedBytes = 0;
    job.statusText = I18n::text(QStringLiteral("downloads.server_file_changed"));
  }
  if (totalBytes > 0) job.totalBytes = totalBytes;
  job.etag = etag;
  job.lastModified = lastModified;
  job.rangeSupported = rangeSupported;
  task->rangeSupported = rangeSupported;
  qCInfo(lcDownloadTelemetry).noquote() << QStringLiteral("[DownloadTelemetry] RANGE PROBE: file=%1 totalBytes=%2 rangeSupported=%3 etagPresent=%4 lastModifiedPresent=%5")
      .arg(job.fileName)
      .arg(totalBytes)
      .arg(rangeSupported ? QStringLiteral("true") : QStringLiteral("false"))
      .arg(!etag.isEmpty() ? QStringLiteral("true") : QStringLiteral("false"))
      .arg(!lastModified.isEmpty() ? QStringLiteral("true") : QStringLiteral("false"));
  prepareTransfer(task);
  if (tasks_.value(task->id) != task) return;
  scheduleQueue();
}

void GeneralDownloadManager::prepareTransfer(Task *task) {
  const QUuid id = task->id;
  const int index = indexOf(id);
  if (index < 0) return;
  GeneralDownloadJob &job = jobs_[index];
  if (task->request.allowParallel && task->rangeSupported && job.totalBytes >= kParallelThreshold
      && task->request.connectionCount > 1) {
    task->adaptiveEnabled = true;
    prepareParallel(task);
  } else {
    task->adaptiveEnabled = false;
    job.connections = 1;
    prepareSingle(task, true);
  }
}

void GeneralDownloadManager::prepareParallel(Task *task) {
  const QUuid id = task->id;
  const int index = indexOf(id);
  if (index < 0) return;
  const qint64 total = jobs_[index].totalBytes;
  const QString tempPath = tempDownloadPath(id);

  if (!task->targetFile) {
    task->targetFile = new QFile(tempPath);
    if (!task->targetFile->open(QIODevice::ReadWrite)) {
      delete task->targetFile;
      task->targetFile = nullptr;
      finish(task, GeneralDownloadState::Failed, I18n::text(QStringLiteral("downloads.temp_open_failed")));
      return;
    }
  }

  if (total > 0 && task->targetFile->size() != total) {
    if (!task->targetFile->resize(total)) {
      task->targetFile->close();
      delete task->targetFile;
      task->targetFile = nullptr;
      finish(task, GeneralDownloadState::Failed, I18n::text(QStringLiteral("downloads.allocate_failed")));
      return;
    }
  }

  QVector<Part> savedParts;
  qint64 savedTotal = -1;
  QByteArray savedEtag;
  QByteArray savedModified;
  int savedTargetConnections = 1;
  const bool stateLoaded = loadState(id, &savedParts, &savedTotal, &savedEtag,
                                     &savedModified, &savedTargetConnections);
  bool validTopology = stateLoaded && !savedParts.isEmpty()
      && (savedTotal <= 0 || savedTotal == total);
  qint64 expectedStart = 0;
  if (validTopology) {
    for (const Part &part : std::as_const(savedParts)) {
      const qint64 length = part.end >= part.start ? part.end - part.start + 1 : -1;
      if (part.start != expectedStart || length <= 0 || part.end >= total
          || part.written < 0 || part.written > length) {
        validTopology = false;
        break;
      }
      expectedStart = part.end + 1;
    }
    validTopology = validTopology && expectedStart == total;
  }
  const bool validatorsMatch = (savedEtag.isEmpty() || jobs_[index].etag.isEmpty()
          || savedEtag == jobs_[index].etag)
      && (savedModified.isEmpty() || jobs_[index].lastModified.isEmpty()
          || savedModified == jobs_[index].lastModified);
  const bool hasState = validTopology && validatorsMatch;
  if (stateLoaded && !hasState) {
    task->targetFile->close();
    delete task->targetFile;
    task->targetFile = nullptr;
    cleanupParts(id);
    task->targetFile = new QFile(tempPath);
    if (!task->targetFile->open(QIODevice::ReadWrite)
        || !task->targetFile->resize(total)) {
      if (task->targetFile->isOpen()) task->targetFile->close();
      delete task->targetFile;
      task->targetFile = nullptr;
      finish(task, GeneralDownloadState::Failed, I18n::text(QStringLiteral("downloads.temp_reprepare_failed")));
      return;
    }
  }

  if (hasState) {
    task->parts = savedParts;
    for (Part &p : task->parts) {
      p.retryCount = 0;
      p.retryTimer = nullptr;
      p.reply = nullptr;
      p.bytesPerSecond = 0.0;
      p.lastMeasureBytes = p.written;
      p.lastMeasureMs = 0;
    }
    // A resumed transfer deliberately re-measures from one active connection;
    // persisted segments remain intact and the normal 1→2→4→8 ramp can restart.
    task->targetConnections = 1;
    task->smoothedThroughput = 0.0;
    task->comparisonBaseline = 0.0;
    task->stageStartBytes = 0;
    for (const Part &part : std::as_const(task->parts)) task->stageStartBytes += part.written;
    task->lastScaleChangeMs = QDateTime::currentMSecsSinceEpoch();
    task->lastScaleDownMs = 0;
    task->consecutiveErrors = 0;
    task->consecutiveLossSamples = 0;
    task->scaleUpHalted = false;
  } else {
    task->targetConnections = 1;
    task->telemetry.connectionPath.append(1);
    qCInfo(lcDownloadTelemetry).noquote() << QStringLiteral("[DownloadTelemetry] ADAPTIVE START: file=%1 connections=1")
        .arg(jobs_[index].fileName);
    task->parts.resize(1);
    task->parts[0].start = 0;
    task->parts[0].end = total > 0 ? total - 1 : -1;
    task->parts[0].written = 0;
    task->parts[0].finished = false;
    task->parts[0].reply = nullptr;
    task->parts[0].retryTimer = nullptr;
    task->parts[0].bytesPerSecond = 0.0;
    task->parts[0].lastMeasureBytes = 0;
    task->parts[0].lastMeasureMs = 0;

    task->smoothedThroughput = 0.0;
    task->comparisonBaseline = 0.0;
    task->stageStartBytes = 0;
    task->lastScaleChangeMs = QDateTime::currentMSecsSinceEpoch();
    task->lastScaleDownMs = 0;
    task->consecutiveErrors = 0;
    task->consecutiveLossSamples = 0;
    task->scaleUpHalted = false;
  }

  jobs_[index].connections = task->targetConnections;

  bool allComplete = true;
  for (int part = 0; part < task->parts.size(); ++part) {
    Part &p = task->parts[part];
    const qint64 partLength = p.end >= p.start ? (p.end - p.start + 1) : -1;
    if (partLength > 0 && p.written >= partLength) {
      p.finished = true;
    } else {
      p.finished = false;
      allComplete = false;
    }
  }

  task->phase = TaskPhase::Transferring;
  updateProgress(task);
  if (tasks_.value(id) != task) return;

  if (allComplete) {
    QString error;
    if (finalizeDownload(task, &error)) finish(task, GeneralDownloadState::Completed);
    else finish(task, GeneralDownloadState::Failed, error);
  }
}

void GeneralDownloadManager::prepareSingle(Task *task, bool resumeExisting) {
  const QUuid id = task->id;
  const int index = indexOf(id);
  if (index < 0) return;
  const qint64 total = jobs_[index].totalBytes;
  const QString tempPath = tempDownloadPath(id);

  task->parts.clear();
  task->parts.resize(1);

  if (!task->targetFile) {
    task->targetFile = new QFile(tempPath);
    const QIODevice::OpenMode mode = (resumeExisting && task->rangeSupported)
        ? QIODevice::ReadWrite
        : (QIODevice::ReadWrite | QIODevice::Truncate);

    if (!task->targetFile->open(mode)) {
      delete task->targetFile;
      task->targetFile = nullptr;
      finish(task, GeneralDownloadState::Failed, I18n::text(QStringLiteral("downloads.temp_open_failed")));
      return;
    }
  }

  qint64 existing = 0;
  if (resumeExisting && task->rangeSupported) {
    QVector<Part> savedParts;
    if (loadState(id, &savedParts) && !savedParts.isEmpty()) {
      existing = savedParts[0].written;
    } else {
      existing = task->targetFile->size();
      if (total > 0 && existing > total) existing = 0;
    }
  } else {
    task->targetFile->resize(0);
  }

  if (total > 0 && task->targetFile->size() < total && task->rangeSupported) {
    task->targetFile->resize(total);
  }

  task->parts[0].start = 0;
  task->parts[0].end = total > 0 ? total - 1 : -1;
  task->parts[0].written = existing;
  task->parts[0].finished = (total > 0 && existing >= total);
  task->parts[0].reply = nullptr;
  task->parts[0].retryTimer = nullptr;

  task->phase = TaskPhase::Transferring;

  if (task->parts[0].finished) {
    QString error;
    if (finalizeDownload(task, &error)) finish(task, GeneralDownloadState::Completed);
    else finish(task, GeneralDownloadState::Failed, error);
  }
}

void GeneralDownloadManager::startPart(Task *task, int partIndex, qint64 start, qint64 end,
                                       bool useRange) {
  if (!task || partIndex < 0 || partIndex >= task->parts.size()) return;
  Part &part = task->parts[partIndex];
  if (part.reply) return;
  QNetworkRequest request(task->request.url);
  applyRequestHeaders(&request, task->request);
  if (useRange) {
    QByteArray value = QByteArrayLiteral("bytes=") + QByteArray::number(start) + QByteArrayLiteral("-");
    if (end >= 0) value += QByteArray::number(end);
    request.setRawHeader(QByteArrayLiteral("Range"), value);
    const int index = indexOf(task->id);
    if (index >= 0 && !jobs_[index].etag.isEmpty())
      request.setRawHeader(QByteArrayLiteral("If-Range"), jobs_[index].etag);
    else if (index >= 0 && !jobs_[index].lastModified.isEmpty())
      request.setRawHeader(QByteArrayLiteral("If-Range"), jobs_[index].lastModified);
  }
  QNetworkReply *reply = network_->get(request);
  part.reply = reply;
  connect(reply, &QNetworkReply::metaDataChanged, this,
          [this, id = task->id, partIndex, useRange, start, end] {
    Task *current = tasks_.value(id, nullptr);
    if (!current || partIndex >= current->parts.size()) return;
    QNetworkReply *partReply = current->parts[partIndex].reply;
    if (!partReply) return;
    const int status = partReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (useRange) {
      qint64 returnedStart = -1;
      qint64 returnedEnd = -1;
      qint64 returnedTotal = -1;
      const bool validRange = status == 206
          && parseContentRange(headerValue(partReply, QByteArrayLiteral("Content-Range")),
                               &returnedStart, &returnedEnd, &returnedTotal)
          && returnedStart == start && (end < 0 || returnedEnd == end);
      const int currentJobIndex = indexOf(id);
      const bool validTotal = currentJobIndex < 0 || jobs_[currentJobIndex].totalBytes <= 0
          || jobs_[currentJobIndex].totalBytes == returnedTotal;
      if (!validRange || !validTotal) { fallbackToSingle(current); return; }
    }
    const int jobIndex = indexOf(id);
    if (jobIndex >= 0 && !useRange) {
      const qint64 length = partReply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
      if (length > 0) jobs_[jobIndex].totalBytes = length;
    }
  });
  connect(reply, &QNetworkReply::readyRead, this,
          [this, id = task->id, partIndex] {
    if (Task *current = tasks_.value(id, nullptr)) consumePart(current, partIndex);
  });
  connect(reply, &QNetworkReply::finished, this,
          [this, id = task->id, partIndex, reply] {
    Task *current = tasks_.value(id, nullptr);
    if (!current || partIndex >= current->parts.size()
        || current->parts[partIndex].reply != reply) { reply->deleteLater(); scheduleQueue(); return; }
    finishPart(current, partIndex);
  });
}

void GeneralDownloadManager::consumePart(Task *task, int partIndex) {
  if (!task || partIndex < 0 || partIndex >= task->parts.size()) return;
  Part &part = task->parts[partIndex];
  if (!part.reply || !task->targetFile || !task->targetFile->isOpen()) return;
  const QByteArray data = part.reply->readAll();
  if (data.isEmpty()) return;
  const qint64 offset = part.start + part.written;
  if (!task->targetFile->seek(offset)) {
    finish(task, GeneralDownloadState::Failed, I18n::text(QStringLiteral("downloads.seek_failed")));
    return;
  }
  if (task->targetFile->write(data) != data.size()) {
    finish(task, GeneralDownloadState::Failed, I18n::text(QStringLiteral("downloads.write_failed")));
    return;
  }
  part.written += data.size();

  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  if (part.lastMeasureMs == 0) {
    part.lastMeasureMs = now;
    part.lastMeasureBytes = part.written;
  } else {
    const qint64 elapsed = now - part.lastMeasureMs;
    if (elapsed >= 100) {
      const qint64 bytesDelta = part.written - part.lastMeasureBytes;
      const double instantRate = (bytesDelta * 1000.0) / elapsed;
      if (part.bytesPerSecond <= 0.0) {
        part.bytesPerSecond = instantRate;
      } else {
        part.bytesPerSecond = (1.0 - kEwmaAlpha) * part.bytesPerSecond + kEwmaAlpha * instantRate;
      }
      part.lastMeasureBytes = part.written;
      part.lastMeasureMs = now;
    }
  }

  updateProgress(task);
}

void GeneralDownloadManager::schedulePartRetry(Task *task, int partIndex) {
  if (!task || partIndex < 0 || partIndex >= task->parts.size()) return;
  Part &part = task->parts[partIndex];
  if (part.retryTimer) {
    part.retryTimer->stop();
    part.retryTimer->deleteLater();
    part.retryTimer = nullptr;
  }
  const int delayMs = kBaseRetryDelayMs * (1 << part.retryCount);
  part.retryCount++;
  task->telemetry.retries++;

  const int index = indexOf(task->id);
  if (index >= 0) {
    jobs_[index].statusText = I18n::text(QStringLiteral("downloads.retrying_connection"))
        .arg(partIndex + 1).arg(part.retryCount).arg(kMaxPartRetries);
    emit jobsChanged();
  }

  saveState(task);

  auto *timer = new QTimer(this);
  timer->setSingleShot(true);
  part.retryTimer = timer;
  connect(timer, &QTimer::timeout, this, [this, id = task->id, partIndex, timer] {
    timer->deleteLater();
    Task *current = tasks_.value(id, nullptr);
    if (!current || partIndex >= current->parts.size()) return;
    if (current->parts[partIndex].retryTimer == timer) {
      current->parts[partIndex].retryTimer = nullptr;
    }
    scheduleQueue();
  });
  timer->start(delayMs);
  scheduleQueue();
}

void GeneralDownloadManager::finishPart(Task *task, int partIndex) {
  if (!task || partIndex < 0 || partIndex >= task->parts.size()) return;
  const QUuid id = task->id;
  Part &part = task->parts[partIndex];
  QNetworkReply *reply = part.reply;
  consumePart(task, partIndex);
  if (tasks_.value(id) != task) { if (reply) reply->deleteLater(); scheduleQueue(); return; }
  const QNetworkReply::NetworkError networkError = reply ? reply->error() : QNetworkReply::UnknownNetworkError;
  const int statusCode = reply ? reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() : 0;
  const QString errorText = reply ? reply->errorString() : I18n::text(QStringLiteral("downloads.no_network_reply"));
  part.reply = nullptr;
  if (reply) reply->deleteLater();

  const int jobIndex = indexOf(id);
  const qint64 expected = part.end >= part.start ? part.end - part.start + 1 : -1;
  const qint64 totalBytes = jobIndex >= 0 ? jobs_[jobIndex].totalBytes : -1;
  const qint64 expectedTotal = totalBytes > 0 ? totalBytes : task->request.expectedBytes;

  // Premature EOF kontrolleri:
  bool prematureEof = false;
  if (expected > 0 && part.written != expected) {
    prematureEof = true;
  } else if (expected <= 0) {
    if (part.written <= 0) {
      prematureEof = true;
    } else if (expectedTotal > 0 && part.written != expectedTotal) {
      prematureEof = true;
    } else if (statusCode != 200 && statusCode != 206) {
      prematureEof = true;
    }
  }

  const bool hasError = (networkError != QNetworkReply::NoError) || prematureEof;

  if (hasError) {
    if (task->adaptiveEnabled) {
      if (statusCode == 429) {
        // 429 is a definitive throttling signal — immediate scale-down is appropriate.
        task->consecutiveErrors = 0;
        task->consecutiveLossSamples = 0;
        task->telemetry.throttleEvents++;
        if (task->targetConnections > 1) {
          const int newTarget = std::max(1, task->targetConnections / 2);
          qCInfo(lcDownloadTelemetry).noquote() << QStringLiteral("[DownloadTelemetry] SCALE DOWN %1 -> %2: file=%3 reason=429_throttle")
              .arg(task->targetConnections)
              .arg(newTarget)
              .arg(jobIndex >= 0 ? jobs_[jobIndex].fileName : QString());
          task->scaleUpHalted = true;
          task->lastScaleDownMs = QDateTime::currentMSecsSinceEpoch();
          task->telemetry.scaleDowns++;
          scaleConnections(task, newTarget);
        }
      } else if (statusCode == 503 || networkError == QNetworkReply::RemoteHostClosedError) {
        task->consecutiveErrors++;
        if (task->consecutiveErrors >= 2 && task->targetConnections > 1) {
          const int newTarget = std::max(1, task->targetConnections / 2);
          qCInfo(lcDownloadTelemetry).noquote() << QStringLiteral("[DownloadTelemetry] SCALE DOWN %1 -> %2: file=%3 reason=repeated_errors")
              .arg(task->targetConnections)
              .arg(newTarget)
              .arg(jobIndex >= 0 ? jobs_[jobIndex].fileName : QString());
          task->scaleUpHalted = true;
          task->lastScaleDownMs = QDateTime::currentMSecsSinceEpoch();
          task->consecutiveErrors = 0;
          task->consecutiveLossSamples = 0;
          task->telemetry.scaleDowns++;
          scaleConnections(task, newTarget);
        }
      }
    }

    const bool transient = isTransientNetworkError(networkError, statusCode) || prematureEof;
    if (transient && part.retryCount < kMaxPartRetries && task->rangeSupported) {
      schedulePartRetry(task, partIndex);
      return;
    }
    const QString finalError = prematureEof
        ? I18n::text(QStringLiteral("downloads.range_incomplete"))
        : errorText;
    finish(task, GeneralDownloadState::Failed, finalError);
    return;
  }

  part.finished = true;
  task->consecutiveErrors = 0;
  for (const Part &candidate : std::as_const(task->parts)) {
    if (!candidate.finished) {
      scheduleQueue();
      return;
    }
  }
  QString error;
  if (finalizeDownload(task, &error)) finish(task, GeneralDownloadState::Completed);
  else finish(task, GeneralDownloadState::Failed, error);
}

void GeneralDownloadManager::fallbackToSingle(Task *task) {
  if (!task || task->fallingBack || tasks_.value(task->id) != task) return;
  const QUuid id = task->id;
  task->fallingBack = true;
  task->adaptiveEnabled = false;
  task->targetConnections = 1;
  abortReplies(task);
  cleanupParts(id);
  task->parts.clear();
  task->rangeSupported = false;
  const int index = indexOf(id);
  if (index >= 0) {
    jobs_[index].rangeSupported = false;
    jobs_[index].connections = 1;
    jobs_[index].downloadedBytes = 0;
    jobs_[index].statusText = I18n::text(QStringLiteral("downloads.continuing_single"));
    emit jobsChanged();
  }
  if (tasks_.value(id) != task) return;
  task->fallingBack = false;
  prepareSingle(task, false);
  if (tasks_.value(id) != task) return;
  scheduleQueue();
}

bool GeneralDownloadManager::finalizeDownload(Task *task, QString *error) {
  const int index = indexOf(task->id);
  if (index < 0) return false;
  if (task->targetFile) {
    task->targetFile->flush();
    task->targetFile->close();
    delete task->targetFile;
    task->targetFile = nullptr;
  }
  const QString tempPath = tempDownloadPath(task->id);
  const QString targetPath = jobs_[index].targetPath;
  if (!QFile::exists(tempPath)) {
    if (error) *error = I18n::text(QStringLiteral("downloads.temp_not_found"));
    return false;
  }
  const qint64 total = jobs_[index].totalBytes;
  if (total > 0 && QFileInfo(tempPath).size() != total) {
    if (error) *error = I18n::text(QStringLiteral("downloads.temp_size_mismatch"));
    return false;
  }
  if (!atomicReplaceFile(tempPath, targetPath)) {
    if (error) *error = I18n::text(QStringLiteral("downloads.move_failed"));
    return false;
  }
  QFile::remove(statePath(task->id));
  return true;
}

void GeneralDownloadManager::finish(Task *task, GeneralDownloadState state, const QString &error) {
  if (!task || tasks_.value(task->id) != task) return;
  const QUuid id = task->id;
  const int index = indexOf(id);
  completedPartCounts_.insert(id, task->parts.size());
  tasks_.remove(id);
  queueOrder_.removeAll(id);
  abortReplies(task);
  s_savedTelemetry.remove(id);
  if (state != GeneralDownloadState::Paused) cleanupParts(id);
  if (index >= 0) {
    GeneralDownloadJob &job = jobs_[index];
    job.state = state;
    job.statusText = stateText(state);
    job.errorText = error;
    job.bytesPerSecond = 0;
    job.etaSeconds = -1;
    if (state == GeneralDownloadState::Completed) {
      job.downloadedBytes = job.totalBytes > 0 ? job.totalBytes : QFileInfo(job.targetPath).size();
      job.totalBytes = job.downloadedBytes;
    }
    job.updatedAt = QDateTime::currentDateTimeUtc();
    if (state != GeneralDownloadState::Paused && task->adaptiveEnabled) {
      const qint64 durationMs = std::max<qint64>(1, task->telemetry.elapsedActiveMs + (QDateTime::currentMSecsSinceEpoch() - task->telemetry.sessionStartMs));
      const qint64 totalDownloaded = job.downloadedBytes;
      const qint64 avgSpeed = (totalDownloaded * 1000) / durationMs;

      QStringList pathList;
      for (int c : task->telemetry.connectionPath) {
        pathList.append(QString::number(c));
      }
      const QString connPath = pathList.isEmpty() ? QStringLiteral("1") : pathList.join(QStringLiteral("->"));

      qCInfo(lcDownloadTelemetry).noquote() << QStringLiteral(
          "[DownloadTelemetry] ADAPTIVE DOWNLOAD SUMMARY\n"
          "file=%1\n"
          "size=%2\n"
          "duration=%3 ms\n"
          "averageSpeed=%4 B/s\n"
          "peakSpeed=%5 B/s\n"
          "connectionPath=%6\n"
          "maxConnectionsUsed=%7\n"
          "scaleUps=%8\n"
          "scaleDowns=%9\n"
          "workSteals=%10\n"
          "retries=%11\n"
          "throttleEvents=%12\n"
          "finalStatus=%13")
          .arg(job.fileName)
          .arg(totalDownloaded)
          .arg(durationMs)
          .arg(avgSpeed)
          .arg(task->telemetry.peakThroughput)
          .arg(connPath)
          .arg(task->telemetry.maxConnectionsUsed)
          .arg(task->telemetry.scaleUps)
          .arg(task->telemetry.scaleDowns)
          .arg(task->telemetry.workSteals)
          .arg(task->telemetry.retries)
          .arg(task->telemetry.throttleEvents)
          .arg(stateText(state));
    }
  }
  delete task;
  persist();
  emit jobsChanged();
  scheduleQueue();
}

void GeneralDownloadManager::evaluateAdaptiveScaling(Task *task) {
  if (!task || !task->adaptiveEnabled || !task->rangeSupported) return;
  if (task->phase != TaskPhase::Transferring) return;

  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  if (task->lastScaleChangeMs == 0) {
    task->lastScaleChangeMs = now;
    task->lastEvaluationMs = now;
    return;
  }

  qint64 totalWritten = 0;
  int activeConns = 0;
  int unfinishedParts = 0;
  for (const Part &p : task->parts) {
    totalWritten += p.written;
    if (p.reply) ++activeConns;
    if (!p.finished) ++unfinishedParts;
  }

  const qint64 elapsedSinceScale = now - task->lastScaleChangeMs;
  const qint64 bytesSinceScale = totalWritten - task->stageStartBytes;

  // Rate-limit scale evaluations so consecutiveLossSamples and EWMA represent distinct windows
  if (now - task->lastEvaluationMs < kMinEvaluationIntervalMs) return;

  // Require meaningful transferred data in this stage
  if (bytesSinceScale < kMinBytesEvaluated) return;

  // Require smoothed throughput to be meaningful
  if (task->smoothedThroughput <= 0.0) return;

  // If new connections have not become active yet, give them more time
  if (activeConns < task->targetConnections && activeConns < unfinishedParts) return;

  const int jobIndex = indexOf(task->id);
  if (jobIndex < 0) return;
  const qint64 totalBytes = jobs_[jobIndex].totalBytes;
  const qint64 remainingTotal = totalBytes > 0 ? (totalBytes - totalWritten) : -1;

  // Don't scale near the end of the file
  if (totalBytes > 0 && remainingTotal < kMinRemainingToScale) return;

  task->lastEvaluationMs = now;

  // --- POST-SCALE WARMUP DWELL (Protects against premature scale-down) ---
  // When connections just scaled up (or down), TCP slow-start / TLS handshake
  // can cause transient throughput drops. Do not consider scale-down during warmup!
  const qint64 warmupDwellMs = task->lastScaleWasDown ? kPostScaleDownDwellMs : kPostScaleUpWarmupDwellMs;
  const qint64 warmupDwellBytes = task->lastScaleWasDown ? kPostScaleDownDwellBytes : kPostScaleUpWarmupDwellBytes;
  const bool inScaleDownWarmup = (elapsedSinceScale < warmupDwellMs) || (bytesSinceScale < warmupDwellBytes);

  if (!inScaleDownWarmup && !task->stageStableLogged) {
    task->stageStableLogged = true;
    qCInfo(lcDownloadTelemetry).noquote() << QStringLiteral(
        "[DownloadTelemetry] STAGE STABLE: file=%1 connections=%2 throughput=%3 B/s")
        .arg(jobs_[jobIndex].fileName)
        .arg(task->targetConnections)
        .arg(static_cast<qint64>(task->smoothedThroughput));
  }

  // --- COMPUTE GAIN vs COMPARISON BASELINE ---
  double gainPct = 0.0;
  double gain = 0.0;
  if (task->comparisonBaseline > 0.0) {
    gain = (task->smoothedThroughput - task->comparisonBaseline) / task->comparisonBaseline;
    gainPct = gain * 100.0;
  }

  // --- 1. SCALE DOWN: Only on SUSTAINED loss (3 consecutive samples), NEVER during warmup ---
  if (!inScaleDownWarmup && task->targetConnections > 1 && task->comparisonBaseline > 0.0) {
    const double loss = (task->comparisonBaseline - task->smoothedThroughput) / task->comparisonBaseline;
    if (loss >= kScaleDownLossThreshold) {
      task->consecutiveLossSamples++;
      const qint64 cooldownOk = (now - task->lastScaleDownMs >= kScaleDownCooldownMs);
      qCInfo(lcDownloadTelemetry).noquote() << QStringLiteral(
          "[DownloadTelemetry] SCALE DOWN CANDIDATE: file=%1 connections=%2 "
          "loss=%3% consecutiveSamples=%4/%5")
          .arg(jobs_[jobIndex].fileName)
          .arg(task->targetConnections)
          .arg(QString::number(loss * 100.0, 'f', 1))
          .arg(task->consecutiveLossSamples)
          .arg(kScaleDownRequiredSamples);

      if (task->consecutiveLossSamples >= kScaleDownRequiredSamples && cooldownOk) {
        const int newTarget = std::max(1, task->targetConnections / 2);
        qCInfo(lcDownloadTelemetry).noquote() << QStringLiteral(
            "[DownloadTelemetry] SCALE DOWN %1 -> %2: file=%3 reason=sustained_throughput_loss "
            "loss=%4% baseline=%5 B/s current=%6 B/s samples=%7")
            .arg(task->targetConnections)
            .arg(newTarget)
            .arg(jobs_[jobIndex].fileName)
            .arg(QString::number(loss * 100.0, 'f', 1))
            .arg(static_cast<qint64>(task->comparisonBaseline))
            .arg(static_cast<qint64>(task->smoothedThroughput))
            .arg(task->consecutiveLossSamples);
        task->scaleUpHalted = true;
        task->lastScaleDownMs = now;
        task->consecutiveLossSamples = 0;
        task->telemetry.scaleDowns++;
        scaleConnections(task, newTarget);
        return;
      }
      return;
    } else {
      task->consecutiveLossSamples = 0;
    }
  } else {
    task->consecutiveLossSamples = 0;
  }

  // --- SCALE UP CHECKS ---
  if (task->targetConnections >= kMaxAdaptiveConnections) return;

  bool canSplit = false;
  for (const Part &p : task->parts) {
    if (p.finished) continue;
    const qint64 rem = p.end - (p.start + p.written) + 1;
    if (rem >= 2 * kMinPartSplitSize) { canSplit = true; break; }
  }
  if (!canSplit) return;

  // --- 2. Initial probe scale up: 1 -> 2 ---
  if (task->targetConnections == 1) {
    constexpr qint64 kProbeMinMs = 400;
    constexpr qint64 kProbeMinBytes = 384 * 1024;
    if (elapsedSinceScale < kProbeMinMs || bytesSinceScale < kProbeMinBytes) return;

    task->comparisonBaseline = task->smoothedThroughput;
    qCInfo(lcDownloadTelemetry).noquote() << QStringLiteral(
        "[DownloadTelemetry] ADAPTIVE EVALUATION: file=%1 currentActive=%2 targetConnections=%3 "
        "ewmaThroughput=%4 B/s baselineThroughput=%5 B/s gain=%6% remainingBytes=%7")
        .arg(jobs_[jobIndex].fileName)
        .arg(activeConns)
        .arg(task->targetConnections)
        .arg(static_cast<qint64>(task->smoothedThroughput))
        .arg(static_cast<qint64>(task->comparisonBaseline))
        .arg(QString::number(gainPct, 'f', 1))
        .arg(remainingTotal);
    qCInfo(lcDownloadTelemetry).noquote() << QStringLiteral(
        "[DownloadTelemetry] SCALE UP 1 -> 2: file=%1 reason=initial-probe baseline=%2 B/s current=%3 B/s")
        .arg(jobs_[jobIndex].fileName)
        .arg(static_cast<qint64>(task->comparisonBaseline))
        .arg(static_cast<qint64>(task->smoothedThroughput));
    task->telemetry.scaleUps++;
    scaleConnections(task, 2);
    return;
  }

  // --- 3. Multi-connection evaluation: 2->4 or 4->8 ---
  if (task->comparisonBaseline > 0.0) {
    if (gain >= kScaleUpGainThreshold) {
      task->scaleUpHalted = false;
      task->consecutiveLossSamples = 0;
      const int nextTarget = std::min(kMaxAdaptiveConnections, task->targetConnections * 2);
      qCInfo(lcDownloadTelemetry).noquote() << QStringLiteral(
          "[DownloadTelemetry] ADAPTIVE EVALUATION: file=%1 currentActive=%2 targetConnections=%3 "
          "ewmaThroughput=%4 B/s baselineThroughput=%5 B/s gain=%6% remainingBytes=%7")
          .arg(jobs_[jobIndex].fileName)
          .arg(activeConns)
          .arg(task->targetConnections)
          .arg(static_cast<qint64>(task->smoothedThroughput))
          .arg(static_cast<qint64>(task->comparisonBaseline))
          .arg(QString::number(gainPct, 'f', 1))
          .arg(remainingTotal);
      qCInfo(lcDownloadTelemetry).noquote() << QStringLiteral(
          "[DownloadTelemetry] SCALE UP %1 -> %2: file=%3 reason=gain gain=%4% baseline=%5 B/s current=%6 B/s")
          .arg(task->targetConnections)
          .arg(nextTarget)
          .arg(jobs_[jobIndex].fileName)
          .arg(QString::number(gain * 100.0, 'f', 1))
          .arg(static_cast<qint64>(task->comparisonBaseline))
          .arg(static_cast<qint64>(task->smoothedThroughput));
      task->comparisonBaseline = task->smoothedThroughput;
      task->telemetry.scaleUps++;
      scaleConnections(task, nextTarget);
      return;
    } else {
      if (task->scaleUpHalted) return;
      // Don't halt scale-up during early warmup window — give connections time to demonstrate speed
      if (elapsedSinceScale < warmupDwellMs) return;
      task->scaleUpHalted = true;
      qCInfo(lcDownloadTelemetry).noquote() << QStringLiteral(
          "[DownloadTelemetry] ADAPTIVE EVALUATION: file=%1 currentActive=%2 targetConnections=%3 "
          "ewmaThroughput=%4 B/s baselineThroughput=%5 B/s gain=%6% remainingBytes=%7")
          .arg(jobs_[jobIndex].fileName)
          .arg(activeConns)
          .arg(task->targetConnections)
          .arg(static_cast<qint64>(task->smoothedThroughput))
          .arg(static_cast<qint64>(task->comparisonBaseline))
          .arg(QString::number(gainPct, 'f', 1))
          .arg(remainingTotal);
      qCInfo(lcDownloadTelemetry).noquote() << QStringLiteral(
          "[DownloadTelemetry] SCALE UP HALTED: file=%1 reason=insufficient_gain gain=%2% baseline=%3 B/s current=%4 B/s")
          .arg(jobs_[jobIndex].fileName)
          .arg(QString::number(gain * 100.0, 'f', 1))
          .arg(static_cast<qint64>(task->comparisonBaseline))
          .arg(static_cast<qint64>(task->smoothedThroughput));
      return;
    }
  }
}

void GeneralDownloadManager::scaleConnections(Task *task, int newTarget) {
  if (!task || newTarget == task->targetConnections) return;
  const int oldTarget = task->targetConnections;
  task->targetConnections = newTarget;
  task->scaleUpHalted = false;
  task->stageStableLogged = false;
  task->consecutiveLossSamples = 0;
  if (task->telemetry.connectionPath.isEmpty() || task->telemetry.connectionPath.last() != newTarget) {
    task->telemetry.connectionPath.append(newTarget);
  }
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  task->lastScaleChangeMs = now;
  task->lastEvaluationMs = now;
  // Reset the stage-isolated EWMA so old topology's throughput decay
  // doesn't contaminate the new stage's measurements.
  task->smoothedThroughput = 0.0;
  // Record the byte position at which this stage began (for dwell-byte tracking).
  qint64 totalWritten = 0;
  for (const Part &p : task->parts) totalWritten += p.written;
  task->stageStartBytes = totalWritten;
  // If this was a scale-DOWN: reset comparisonBaseline to 0 so the new
  // (lower) stage must build its own stable measurement before becoming
  // eligible for further scale-down (cascade prevention).
  if (newTarget < oldTarget) {
    task->comparisonBaseline = 0.0;
    task->lastScaleWasDown = true;
    qCInfo(lcDownloadTelemetry).noquote() << QStringLiteral(
        "[DownloadTelemetry] STAGE WARMUP: file=%1 connections=%2 reason=post-scale-down")
        .arg(indexOf(task->id) >= 0 ? jobs_[indexOf(task->id)].fileName : QString())
        .arg(newTarget);
  } else {
    // Scale-UP: keep comparisonBaseline from the caller (already set before calling)
    task->lastScaleWasDown = false;
    qCInfo(lcDownloadTelemetry).noquote() << QStringLiteral(
        "[DownloadTelemetry] STAGE WARMUP: file=%1 connections=%2 reason=post-scale-up")
        .arg(indexOf(task->id) >= 0 ? jobs_[indexOf(task->id)].fileName : QString())
        .arg(newTarget);
  }

  const int jobIndex = indexOf(task->id);
  if (jobIndex >= 0) {
    jobs_[jobIndex].connections = newTarget;
    if (newTarget == 1) {
      jobs_[jobIndex].statusText = I18n::text(QStringLiteral("downloads.one_connection_measuring"));
    } else {
      jobs_[jobIndex].statusText = I18n::text(QStringLiteral("downloads.connections_auto")).arg(newTarget);
    }
    emit jobsChanged();
  }

  if (newTarget > oldTarget) {
    int unfinishedCount = 0;
    for (const Part &p : task->parts) {
      if (!p.finished) ++unfinishedCount;
    }
    while (unfinishedCount < newTarget) {
      int bestPart = -1;
      qint64 maxRemaining = 0;
      for (int i = 0; i < task->parts.size(); ++i) {
        const Part &p = task->parts[i];
        if (p.finished) continue;
        const qint64 rem = p.end - (p.start + p.written) + 1;
        if (rem > maxRemaining) {
          maxRemaining = rem;
          bestPart = i;
        }
      }

      if (bestPart < 0 || maxRemaining < 2 * kMinPartSplitSize) {
        break;
      }

      consumePart(task, bestPart);
      Part &parent = task->parts[bestPart];
      const qint64 uncompletedStart = parent.start + parent.written;
      const qint64 uncompletedLen = parent.end - uncompletedStart + 1;
      if (uncompletedLen < 2 * kMinPartSplitSize) {
        break;
      }

      const qint64 half = uncompletedLen / 2;
      const qint64 splitPoint = uncompletedStart + half - 1;
      const qint64 oldEnd = parent.end;

      if (parent.reply) {
        QPointer<QNetworkReply> oldReply = parent.reply;
        parent.reply = nullptr;
        oldReply->disconnect(this);
        oldReply->abort();
        oldReply->deleteLater();
      }
      if (parent.retryTimer) {
        parent.retryTimer->stop();
        parent.retryTimer->deleteLater();
        parent.retryTimer = nullptr;
      }

      parent.end = splitPoint;

      Part child;
      child.start = splitPoint + 1;
      child.end = oldEnd;
      child.written = 0;
      child.finished = false;
      child.retryCount = 0;
      child.reply = nullptr;
      child.retryTimer = nullptr;
      child.bytesPerSecond = 0.0;
      child.lastMeasureBytes = 0;
      child.lastMeasureMs = 0;

      task->parts.append(child);
      ++unfinishedCount;
    }
  } else if (newTarget < oldTarget) {
    int activeCount = 0;
    for (const Part &p : task->parts) {
      if (p.reply) ++activeCount;
    }
    for (int i = task->parts.size() - 1; i >= 0 && activeCount > newTarget; --i) {
      Part &p = task->parts[i];
      if (p.reply && !p.finished) {
        consumePart(task, i);
        QPointer<QNetworkReply> oldReply = p.reply;
        p.reply = nullptr;
        oldReply->disconnect(this);
        oldReply->abort();
        oldReply->deleteLater();
        --activeCount;
      }
    }
  }

  saveState(task);
  scheduleQueue();
}

bool GeneralDownloadManager::tryWorkSteal(Task *task) {
  if (!task || !task->rangeSupported || !task->request.allowWorkStealing) {
    return false;
  }
  if (!task->targetFile || !task->targetFile->isOpen()) {
    return false;
  }
  const int jobIndex = indexOf(task->id);
  if (jobIndex < 0 || terminal(jobs_[jobIndex].state) || jobs_[jobIndex].state == GeneralDownloadState::Paused) {
    return false;
  }
  if (task->phase != TaskPhase::Transferring) {
    return false;
  }

  // 1. targetConnections constraint: do not steal if task is already at or above targetConnections
  int activeForTask = 0;
  for (const Part &p : task->parts) {
    if (p.reply) ++activeForTask;
  }
  if (activeForTask >= task->targetConnections) {
    return false;
  }

  // 2. Global scheduler slot constraint
  if (activeConnectionCount() >= maxActiveConnections_) {
    return false;
  }

  // 3. File remaining constraint: do not steal if file is almost finished
  qint64 totalRemaining = 0;
  for (const Part &p : task->parts) {
    if (p.finished) continue;
    const qint64 rem = p.end >= p.start ? (p.end - (p.start + p.written) + 1) : 0;
    if (rem > 0) totalRemaining += rem;
  }
  if (totalRemaining < kMinRemainingToScale) {
    return false;
  }

  // 4. Select best donor (highest remaining bytes and/or longest ETA)
  int bestDonor = -1;
  double bestScore = -1.0;
  qint64 bestRemaining = 0;

  for (int i = 0; i < task->parts.size(); ++i) {
    const Part &p = task->parts[i];
    if (p.finished) continue;
    if (p.retryTimer) continue;  // Never steal from a part in backoff retry

    const qint64 rem = p.end - (p.start + p.written) + 1;
    if (rem < 2 * kMinWorkStealSplitSize) {
      continue;
    }

    double etaSec = 0.0;
    if (p.bytesPerSecond > 1024.0) {
      etaSec = static_cast<double>(rem) / p.bytesPerSecond;
    } else {
      etaSec = static_cast<double>(rem) / 65536.0;
    }
    const double score = static_cast<double>(rem) * (1.0 + std::min(60.0, etaSec));
    if (score > bestScore) {
      bestScore = score;
      bestDonor = i;
      bestRemaining = rem;
    }
  }

  if (bestDonor < 0 || bestRemaining < 2 * kMinWorkStealSplitSize) {
    return false;
  }

  // 5. Consume buffered data from donor first to preserve disk/byte integrity
  consumePart(task, bestDonor);
  Part &parent = task->parts[bestDonor];
  const qint64 uncompletedStart = parent.start + parent.written;
  const qint64 uncompletedLen = parent.end - uncompletedStart + 1;
  if (uncompletedLen < 2 * kMinWorkStealSplitSize) {
    return false;
  }

  const qint64 half = uncompletedLen / 2;
  const qint64 splitPoint = uncompletedStart + half - 1;
  const qint64 oldEnd = parent.end;

  // 6. Abort donor's old reply safely so two replies never download the same range
  if (parent.reply) {
    QPointer<QNetworkReply> oldReply = parent.reply;
    parent.reply = nullptr;
    oldReply->disconnect(this);
    oldReply->abort();
    oldReply->deleteLater();
  }
  if (parent.retryTimer) {
    parent.retryTimer->stop();
    parent.retryTimer->deleteLater();
    parent.retryTimer = nullptr;
  }

  parent.end = splitPoint;
  parent.bytesPerSecond = 0.0;
  parent.lastMeasureBytes = parent.written;
  parent.lastMeasureMs = QDateTime::currentMSecsSinceEpoch();

  // 7. Create stolen child segment
  Part child;
  child.start = splitPoint + 1;
  child.end = oldEnd;
  child.initialSize = 0;
  child.written = 0;
  child.finished = false;
  child.retryCount = 0;
  child.reply = nullptr;
  child.retryTimer = nullptr;
  child.bytesPerSecond = 0.0;
  child.lastMeasureBytes = 0;
  child.lastMeasureMs = 0;

  task->parts.append(child);
  saveState(task);
  task->telemetry.workSteals++;
  int activeConns = 0;
  for (const Part &p : task->parts) {
    if (p.reply) ++activeConns;
  }
  qCInfo(lcDownloadTelemetry).noquote() << QStringLiteral(
      "[DownloadTelemetry] WORK STEAL: file=%1 donorRemaining=%2 newRange=[%3-%4] activeConnections=%5")
      .arg(jobIndex >= 0 ? jobs_[jobIndex].fileName : QString())
      .arg(uncompletedLen)
      .arg(child.start)
      .arg(child.end)
      .arg(activeConns);
  return true;
}

void GeneralDownloadManager::abortReplies(Task *task) {
  if (!task) return;
  if (task->probe) {
    QPointer<QNetworkReply> reply = task->probe;
    task->probe = nullptr;
    reply->disconnect(this);
    reply->abort();
    if (reply) reply->deleteLater();
  }
  for (Part &part : task->parts) {
    if (part.retryTimer) {
      part.retryTimer->stop();
      part.retryTimer->deleteLater();
      part.retryTimer = nullptr;
    }
    if (part.reply) {
      QPointer<QNetworkReply> reply = part.reply;
      part.reply = nullptr;
      reply->disconnect(this);
      reply->abort();
      if (reply) reply->deleteLater();
    }
  }
  if (task->targetFile) {
    task->targetFile->flush();
    task->targetFile->close();
    delete task->targetFile;
    task->targetFile = nullptr;
  }
}

void GeneralDownloadManager::cleanupParts(const QUuid &id) {
  QFile::remove(tempDownloadPath(id));
  QFile::remove(statePath(id));
  const int jobIndex = indexOf(id);
  if (jobIndex >= 0) {
    for (int part = 0; part < 8; ++part) {
      QFile::remove(jobs_.at(jobIndex).targetPath + QStringLiteral(".dalinira-%1.part%2")
          .arg(id.toString(QUuid::WithoutBraces)).arg(part));
    }
  }
}

void GeneralDownloadManager::saveState(Task *task) const {
  if (!task) return;
  const int index = indexOf(task->id);
  if (index < 0) return;
  QJsonObject root;
  root.insert(QStringLiteral("totalBytes"), static_cast<double>(jobs_[index].totalBytes));
  root.insert(QStringLiteral("etag"), QString::fromLatin1(jobs_[index].etag.toBase64()));
  root.insert(QStringLiteral("lastModified"), QString::fromLatin1(jobs_[index].lastModified.toBase64()));
  root.insert(QStringLiteral("targetConnections"), task->targetConnections);
  QJsonArray partsArray;
  for (const Part &part : task->parts) {
    QJsonObject partObj;
    partObj.insert(QStringLiteral("start"), static_cast<double>(part.start));
    partObj.insert(QStringLiteral("end"), static_cast<double>(part.end));
    partObj.insert(QStringLiteral("written"), static_cast<double>(part.written));
    partObj.insert(QStringLiteral("finished"), part.finished);
    partObj.insert(QStringLiteral("retryCount"), part.retryCount);
    partsArray.append(partObj);
  }
  root.insert(QStringLiteral("parts"), partsArray);
  QSaveFile file(statePath(task->id));
  if (file.open(QIODevice::WriteOnly)) {
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    file.commit();
  }
}

bool GeneralDownloadManager::loadState(const QUuid &id, QVector<Part> *parts, qint64 *totalBytes,
                                       QByteArray *etag, QByteArray *lastModified,
                                       int *targetConnections) const {
  QFile file(statePath(id));
  if (!file.open(QIODevice::ReadOnly)) return false;
  const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
  if (!doc.isObject()) return false;
  const QJsonObject root = doc.object();
  if (totalBytes) *totalBytes = static_cast<qint64>(root.value(QStringLiteral("totalBytes")).toDouble(-1));
  if (etag) *etag = QByteArray::fromBase64(root.value(QStringLiteral("etag")).toString().toLatin1());
  if (lastModified) *lastModified = QByteArray::fromBase64(root.value(QStringLiteral("lastModified")).toString().toLatin1());
  if (targetConnections) *targetConnections = root.value(QStringLiteral("targetConnections")).toInt(1);
  if (parts) {
    parts->clear();
    const QJsonArray partsArray = root.value(QStringLiteral("parts")).toArray();
    for (const QJsonValue &v : partsArray) {
      const QJsonObject p = v.toObject();
      Part part;
      part.start = static_cast<qint64>(p.value(QStringLiteral("start")).toDouble(0));
      part.end = static_cast<qint64>(p.value(QStringLiteral("end")).toDouble(-1));
      part.written = static_cast<qint64>(p.value(QStringLiteral("written")).toDouble(0));
      part.finished = p.value(QStringLiteral("finished")).toBool(false);
      part.retryCount = p.value(QStringLiteral("retryCount")).toInt(0);
      parts->append(part);
    }
  }
  return true;
}

QString GeneralDownloadManager::tempDownloadPath(const QUuid &id) const {
  const int jobIndex = indexOf(id);
  if (jobIndex < 0) return {};
  return jobs_.at(jobIndex).targetPath + QStringLiteral(".dalinira-%1.download")
      .arg(id.toString(QUuid::WithoutBraces));
}

QString GeneralDownloadManager::statePath(const QUuid &id) const {
  const int jobIndex = indexOf(id);
  if (jobIndex < 0) return {};
  return jobs_.at(jobIndex).targetPath + QStringLiteral(".dalinira-%1.state")
      .arg(id.toString(QUuid::WithoutBraces));
}

QString GeneralDownloadManager::uniqueTargetPath(const QString &directory, const QString &fileName) const {
  const QString safe = BrowserSecurity::sanitizeDownloadFileName(fileName, QStringLiteral("download"));
  const QFileInfo source(safe);
  const QString stem = source.completeBaseName().isEmpty() ? QStringLiteral("download") : source.completeBaseName();
  const QString suffix = source.completeSuffix();
  QString candidate = QDir(directory).absoluteFilePath(safe);
  const auto unavailable = [this](const QString &path) {
    if (QFileInfo::exists(path)) return true;
    for (const GeneralDownloadJob &job : jobs_) if (job.targetPath == path) return true;
    return false;
  };
  for (int counter = 1; unavailable(candidate) && counter < 10000; ++counter) {
    const QString numbered = suffix.isEmpty() ? QStringLiteral("%1 (%2)").arg(stem).arg(counter)
                                              : QStringLiteral("%1 (%2).%3").arg(stem).arg(counter).arg(suffix);
    candidate = QDir(directory).absoluteFilePath(numbered);
  }
  return candidate;
}

void GeneralDownloadManager::applyRequestHeaders(QNetworkRequest *request,
                                                 const GeneralDownloadRequest &source) const {
  request->setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                        QNetworkRequest::NoLessSafeRedirectPolicy);
  if (!source.userAgent.isEmpty()) request->setRawHeader(QByteArrayLiteral("User-Agent"), source.userAgent);
  if (!source.referrer.isEmpty()) request->setRawHeader(QByteArrayLiteral("Referer"), source.referrer);
  // Directly attach the source context's scoped cookies to this request.
  // This guarantees strict engine and request isolation without polluting
  // the shared network manager's cookie jar across distinct downloads.
  if (!source.cookies.isEmpty()) {
    QByteArray cookieHeader;
    for (const QNetworkCookie &cookie : source.cookies) {
      if (!cookieHeader.isEmpty()) cookieHeader.append("; ");
      cookieHeader.append(cookie.toRawForm(QNetworkCookie::NameAndValueOnly));
    }
    if (!cookieHeader.isEmpty()) {
      request->setRawHeader(QByteArrayLiteral("Cookie"), cookieHeader);
    }
  }
}

void GeneralDownloadManager::updateProgress(Task *task) {
  if (!task || tasks_.value(task->id) != task) return;
  const int index = indexOf(task->id);
  if (index < 0) return;
  qint64 downloaded = 0;
  for (const Part &part : std::as_const(task->parts)) downloaded += part.written;
  GeneralDownloadJob &job = jobs_[index];
  job.downloadedBytes = downloaded;
  const qint64 now = QDateTime::currentMSecsSinceEpoch();

  if (task->lastMeasureMs == 0) {
    if (downloaded > 0) {
      task->lastMeasureMs = now;
      task->lastMeasuredBytes = downloaded;
    }
  } else {
    const qint64 elapsed = now - task->lastMeasureMs;
    if (elapsed >= 100) {
      const qint64 delta = std::max<qint64>(0, downloaded - task->lastMeasuredBytes);
      const double instantRate = (delta * 1000.0) / elapsed;
      if (task->smoothedThroughput <= 0.0) {
        task->smoothedThroughput = instantRate;
      } else {
        task->smoothedThroughput = (1.0 - kEwmaAlpha) * task->smoothedThroughput + kEwmaAlpha * instantRate;
      }
      job.bytesPerSecond = static_cast<qint64>(task->smoothedThroughput);
      task->telemetry.peakThroughput = std::max<qint64>(task->telemetry.peakThroughput,
                                                        static_cast<qint64>(task->smoothedThroughput));
      job.etaSeconds = job.bytesPerSecond > 0 && job.totalBytes > downloaded
          ? (job.totalBytes - downloaded) / job.bytesPerSecond : -1;
      task->lastMeasuredBytes = downloaded;
      task->lastMeasureMs = now;

      if (task->adaptiveEnabled) {
        evaluateAdaptiveScaling(task);
      }
    }
  }

  int activeReplies = 0;
  for (const Part &p : task->parts) { if (p.reply) ++activeReplies; }
  task->telemetry.maxConnectionsUsed = std::max(task->telemetry.maxConnectionsUsed, activeReplies);

  job.updatedAt = QDateTime::currentDateTimeUtc();
  const bool firstData = downloaded > 0 && task->lastEmittedBytes == 0;
  const bool complete = job.totalBytes > 0 && downloaded >= job.totalBytes;
  if (firstData || complete || task->lastEmitMs == 0 || now - task->lastEmitMs >= 200) {
    task->lastEmitMs = now;
    task->lastEmittedBytes = downloaded;
    emit jobsChanged();
  }
}

void GeneralDownloadManager::persist() const {
  QJsonArray array;
  for (const GeneralDownloadJob &job : jobs_) {
    const GeneralDownloadRequest request = requests_.value(job.id);
    QJsonObject object;
    object.insert(QStringLiteral("id"), job.id.toString(QUuid::WithoutBraces));
    object.insert(QStringLiteral("url"), BrowserSecurity::sanitizeUrlForPersistence(job.url)
        .toString(QUrl::FullyEncoded));
    object.insert(QStringLiteral("fileName"), job.fileName);
    object.insert(QStringLiteral("targetPath"), job.targetPath);
    object.insert(QStringLiteral("mimeType"), job.mimeType);
    object.insert(QStringLiteral("state"), static_cast<int>(job.state));
    object.insert(QStringLiteral("status"), job.statusText);
    object.insert(QStringLiteral("error"), job.errorText);
    object.insert(QStringLiteral("total"), static_cast<double>(job.totalBytes));
    object.insert(QStringLiteral("downloaded"), static_cast<double>(job.downloadedBytes));
    object.insert(QStringLiteral("connections"), request.connectionCount);
    object.insert(QStringLiteral("allowParallel"), request.allowParallel);
    object.insert(QStringLiteral("allowWorkStealing"), request.allowWorkStealing);
    object.insert(QStringLiteral("overwriteExisting"), request.overwriteExisting);
    object.insert(QStringLiteral("range"), job.rangeSupported);
    object.insert(QStringLiteral("etag"), QString::fromLatin1(job.etag.toBase64()));
    object.insert(QStringLiteral("modified"), QString::fromLatin1(job.lastModified.toBase64()));
    object.insert(QStringLiteral("userAgent"), QString::fromLatin1(request.userAgent.toBase64()));
    object.insert(QStringLiteral("referrer"), QString::fromLatin1(request.referrer.toBase64()));
    object.insert(QStringLiteral("created"), job.createdAt.toString(Qt::ISODateWithMs));
    object.insert(QStringLiteral("updated"), job.updatedAt.toString(Qt::ISODateWithMs));
    array.append(object);
  }
  QDir().mkpath(QFileInfo(historyPath_).absolutePath());
  QSaveFile output(historyPath_);
  if (!output.open(QIODevice::WriteOnly)) return;
  output.write(QJsonDocument(array).toJson(QJsonDocument::Compact));
  if (output.commit()) QFile::setPermissions(historyPath_, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}

void GeneralDownloadManager::load() {
  QFile input(historyPath_);
  if (!input.open(QIODevice::ReadOnly)) return;
  const QJsonDocument document = QJsonDocument::fromJson(input.readAll());
  if (!document.isArray()) return;
  for (const QJsonValue &value : document.array()) {
    const QJsonObject object = value.toObject();
    GeneralDownloadJob job;
    job.id = QUuid(object.value(QStringLiteral("id")).toString());
    job.url = QUrl(object.value(QStringLiteral("url")).toString());
    job.fileName = BrowserSecurity::sanitizeDownloadFileName(object.value(QStringLiteral("fileName")).toString());
    job.targetPath = QFileInfo(object.value(QStringLiteral("targetPath")).toString()).absoluteFilePath();
    job.mimeType = object.value(QStringLiteral("mimeType")).toString();
    job.state = static_cast<GeneralDownloadState>(object.value(QStringLiteral("state")).toInt());
    // History stores the status as it was shown, in the UI language of that
    // session; show it in the current language.
    job.statusText = I18n::retranslated(object.value(QStringLiteral("status")).toString());
    job.errorText = object.value(QStringLiteral("error")).toString();
    job.totalBytes = static_cast<qint64>(object.value(QStringLiteral("total")).toDouble(-1));
    job.downloadedBytes = static_cast<qint64>(object.value(QStringLiteral("downloaded")).toDouble(0));
    job.connections = object.value(QStringLiteral("connections")).toInt(1);
    job.rangeSupported = object.value(QStringLiteral("range")).toBool(false);
    job.etag = QByteArray::fromBase64(object.value(QStringLiteral("etag")).toString().toLatin1());
    job.lastModified = QByteArray::fromBase64(object.value(QStringLiteral("modified")).toString().toLatin1());
    job.createdAt = QDateTime::fromString(object.value(QStringLiteral("created")).toString(), Qt::ISODateWithMs);
    job.updatedAt = QDateTime::fromString(object.value(QStringLiteral("updated")).toString(), Qt::ISODateWithMs);
    if (job.id.isNull() || !canHandleUrl(job.url) || job.targetPath.isEmpty()) continue;
    if (!terminal(job.state)) {
      job.state = GeneralDownloadState::Paused;
      job.statusText = I18n::text(QStringLiteral("downloads.browser_restarted_resumable"));
      job.bytesPerSecond = 0;
      job.etaSeconds = -1;
    }
    GeneralDownloadRequest request;
    request.url = job.url;
    request.suggestedFileName = job.fileName;
    request.targetDirectory = QFileInfo(job.targetPath).absolutePath();
    request.mimeType = job.mimeType;
    request.expectedBytes = job.totalBytes;
    request.connectionCount = normalizedConnectionCount(object.value(QStringLiteral("connections")).toInt(4));
    request.allowParallel = object.value(QStringLiteral("allowParallel")).toBool(true);
    request.allowWorkStealing = object.value(QStringLiteral("allowWorkStealing")).toBool(true);
    request.overwriteExisting = object.value(QStringLiteral("overwriteExisting")).toBool(false);
    request.userAgent = QByteArray::fromBase64(object.value(QStringLiteral("userAgent")).toString().toLatin1());
    request.referrer = QByteArray::fromBase64(object.value(QStringLiteral("referrer")).toString().toLatin1());
    jobs_.append(job);
    requests_.insert(job.id, request);
  }
}
