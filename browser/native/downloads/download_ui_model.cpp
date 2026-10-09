#include "download_ui_model.h"

#include "core/browser_profile_service.h"
#include "general_download_manager.h"
#include "media_download_service.h"

#include <QFileInfo>
#include <QSet>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include "i18n/i18n.h"

namespace {
QString keyFor(char prefix, const QUuid &id) {
  return QString(QChar::fromLatin1(prefix)) + QLatin1Char(':') + id.toString(QUuid::WithoutBraces);
}

QUuid idFromKey(const QString &key, QChar prefix) {
  if (key.size() < 3 || key.at(0) != prefix || key.at(1) != QLatin1Char(':')) return {};
  return QUuid(key.mid(2));
}

bool active(DownloadUiState state) {
  return state == DownloadUiState::Queued || state == DownloadUiState::Downloading
      || state == DownloadUiState::Paused || state == DownloadUiState::Processing;
}

}  // namespace

DownloadUiModel::DownloadUiModel(BrowserProfileService *profile,
                                 MediaDownloadService *media, QObject *parent)
    : QObject(parent), profile_(profile), general_(profile ? profile->downloadManager() : nullptr),
      media_(media) {
  coalesceTimer_ = new QTimer(this);
  coalesceTimer_->setSingleShot(true);
  coalesceTimer_->setInterval(120);
  connect(coalesceTimer_, &QTimer::timeout, this, &DownloadUiModel::refresh);
  if (general_) {
    connect(general_, &GeneralDownloadManager::jobsChanged, this, &DownloadUiModel::scheduleRefresh);
    connect(general_, &GeneralDownloadManager::jobEnqueued, this, [this](const QUuid &) { refresh(); });
  }
  if (media_) {
    connect(media_, &MediaDownloadService::jobsChanged, this, &DownloadUiModel::scheduleRefresh);
    connect(media_, &MediaDownloadService::jobEnqueued, this, [this](const QUuid &) { refresh(); });
  }
  if (profile_) connect(profile_, &BrowserProfileService::downloadsChanged, this, &DownloadUiModel::scheduleRefresh);
  refresh();
}

QVector<DownloadUiItem> DownloadUiModel::items() const { return items_; }

int DownloadUiModel::activeCount() const { return activeCountForItems(items_); }
double DownloadUiModel::aggregateProgress() const { return aggregateProgressForItems(items_); }

bool DownloadUiModel::hasPaused() const {
  return std::any_of(items_.cbegin(), items_.cend(), [](const auto &i) { return i.state == DownloadUiState::Paused; });
}
bool DownloadUiModel::hasErrors() const {
  return std::any_of(items_.cbegin(), items_.cend(), [](const auto &i) { return i.state == DownloadUiState::Failed; });
}
bool DownloadUiModel::hasRecentCompletion() const {
  const QDateTime cutoff = QDateTime::currentDateTimeUtc().addSecs(-4);
  return std::any_of(items_.cbegin(), items_.cend(), [&cutoff](const auto &i) {
    return i.state == DownloadUiState::Completed && i.createdAt >= cutoff;
  });
}

bool DownloadUiModel::pause(const QString &key) {
  const QUuid id = idFromKey(key, QLatin1Char('g'));
  return general_ && !id.isNull() && general_->pause(id);
}
bool DownloadUiModel::resume(const QString &key) {
  const QUuid id = idFromKey(key, QLatin1Char('g'));
  return general_ && !id.isNull() && general_->resume(id);
}
bool DownloadUiModel::cancel(const QString &key) {
  QUuid id = idFromKey(key, QLatin1Char('g'));
  if (general_ && !id.isNull()) return general_->cancel(id);
  id = idFromKey(key, QLatin1Char('m'));
  return media_ && !id.isNull() && media_->cancel(id);
}
bool DownloadUiModel::retry(const QString &key) {
  QUuid id = idFromKey(key, QLatin1Char('g'));
  if (general_ && !id.isNull()) return !general_->retry(id).isNull();
  id = idFromKey(key, QLatin1Char('m'));
  return media_ && !id.isNull() && !media_->retry(id).isNull();
}
bool DownloadUiModel::remove(const QString &key) {
  QUuid id = idFromKey(key, QLatin1Char('g'));
  if (general_ && !id.isNull()) return general_->remove(id);
  id = idFromKey(key, QLatin1Char('m'));
  return media_ && !id.isNull() && media_->remove(id);
}

void DownloadUiModel::scheduleRefresh() {
  if (!coalesceTimer_->isActive()) coalesceTimer_->start();
}

QString DownloadUiModel::stateSignature(const DownloadUiItem &item) const {
  return QString::number(static_cast<int>(item.state));
}

void DownloadUiModel::refresh() {
  QVector<DownloadUiItem> next;
  QSet<QString> representedPaths;
  if (general_) {
    for (const auto &job : general_->jobs()) {
      DownloadUiItem item;
      item.key = keyFor('g', job.id);
      item.title = job.fileName;
      item.subtitle = !job.mimeType.isEmpty() ? job.mimeType : job.url.host();
      item.localPath = job.targetPath;
      item.source = DownloadUiSource::General;
      switch (job.state) {
        case GeneralDownloadState::Queued: item.state = DownloadUiState::Queued; break;
        case GeneralDownloadState::Probing:
        case GeneralDownloadState::Downloading: item.state = DownloadUiState::Downloading; break;
        case GeneralDownloadState::Paused: item.state = DownloadUiState::Paused; break;
        case GeneralDownloadState::Completed: item.state = DownloadUiState::Completed; break;
        case GeneralDownloadState::Failed: item.state = DownloadUiState::Failed; break;
        case GeneralDownloadState::Cancelled: item.state = DownloadUiState::Cancelled; break;
      }
      item.downloadedBytes = job.downloadedBytes;
      item.totalBytes = job.totalBytes;
      item.bytesPerSecond = job.bytesPerSecond;
      item.etaSeconds = job.etaSeconds;
      item.percent = job.totalBytes > 0 ? 100.0 * job.downloadedBytes / job.totalBytes : 0.0;
      item.createdAt = job.createdAt;
      representedPaths.insert(QFileInfo(job.targetPath).absoluteFilePath());
      next.append(item);
    }
  }
  if (media_) {
    for (const auto &job : media_->jobs()) {
      DownloadUiItem item;
      item.key = keyFor('m', job.id);
      item.title = job.title;
      item.subtitle = job.url.host();
      item.localPath = job.outputPath;
      item.source = DownloadUiSource::Media;
      switch (job.state) {
        case MediaDownloadState::Queued: item.state = DownloadUiState::Queued; break;
        case MediaDownloadState::Downloading: item.state = DownloadUiState::Downloading; break;
        case MediaDownloadState::Processing: item.state = DownloadUiState::Processing; break;
        case MediaDownloadState::Completed: item.state = DownloadUiState::Completed; break;
        case MediaDownloadState::Failed: item.state = DownloadUiState::Failed; break;
        case MediaDownloadState::Cancelled: item.state = DownloadUiState::Cancelled; break;
      }
      item.downloadedBytes = job.downloadedBytes;
      item.totalBytes = job.totalBytes;
      item.bytesPerSecond = job.bytesPerSecond;
      item.etaSeconds = job.etaSeconds;
      item.percent = job.percent;
      item.createdAt = job.createdAt;
      item.statusText = job.statusText;
      item.converting = (job.state == MediaDownloadState::Processing);
      if (!job.outputPath.isEmpty()) representedPaths.insert(QFileInfo(job.outputPath).absoluteFilePath());
      next.append(item);
    }
  }
  if (profile_) {
    for (const auto &entry : profile_->nativeDownloads()) {
      const QString path = QFileInfo(entry.path).absoluteFilePath();
      if (representedPaths.contains(path)) continue;
      DownloadUiItem item;
      item.key = QStringLiteral("n:%1").arg(QString::number(qHash(path)));
      item.title = entry.fileName;
      item.subtitle = I18n::text(QStringLiteral("downloads.browser_download"));
      item.localPath = path;
      item.source = DownloadUiSource::NativeFallback;
      // The native fallback records a display string; match it in every UI
      // language so a live language switch never misclassifies a download.
      item.state = I18n::isTranslationOf(QStringLiteral("downloads.state_completed"), entry.state)
          ? DownloadUiState::Completed
          : I18n::isTranslationOf(QStringLiteral("downloads.state_cancelled"), entry.state)
          ? DownloadUiState::Cancelled
          : I18n::isTranslationOf(QStringLiteral("downloads.state_interrupted"), entry.state)
          ? DownloadUiState::Failed
          : DownloadUiState::Downloading;
      next.append(item);
    }
  }
  std::stable_sort(next.begin(), next.end(), [](const auto &a, const auto &b) {
    return a.createdAt > b.createdAt;
  });

  QHash<QString, QString> nextStates;
  for (const auto &item : next) {
    const QString signature = stateSignature(item);
    nextStates.insert(item.key, signature);
    if (initialized_ && !knownStates_.contains(item.key) && active(item.state)) emit downloadStarted(item.key);
    if (initialized_ && knownStates_.contains(item.key)
        && knownStates_.value(item.key) != signature && item.state == DownloadUiState::Completed)
      emit downloadCompleted(item.key);
    if (initialized_ && knownStates_.contains(item.key)
        && knownStates_.value(item.key) != signature && item.state == DownloadUiState::Failed)
      emit downloadFailed(item.key);
  }
  items_ = std::move(next);
  knownStates_ = std::move(nextStates);
  initialized_ = true;
  emit changed();
}
