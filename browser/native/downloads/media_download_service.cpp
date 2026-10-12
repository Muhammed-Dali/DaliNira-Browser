#include "media_download_service.h"
#include "core/application_identity.h"

#include "adult_content_protection.h"
#include "security_utils.h"
#include "yt_dlp_update_manager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeDatabase>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>
#include <QUrlQuery>

#include <algorithm>
#include <utility>
#include "i18n/i18n.h"

namespace {

constexpr qsizetype kMaximumMetadataBytes = 32 * 1024 * 1024;
constexpr int kMaximumPersistedJobs = 200;
constexpr int kMaximumProtectionUrls = 512;
const QString kAdultProtectionAnalysisMessage =
    I18n::text(QStringLiteral("media.adult_blocked_analysis"));
const QString kAdultProtectionDownloadMessage =
    I18n::text(QStringLiteral("media.adult_blocked_download"));

bool isProtectedUrlBlocked(const QUrl &url) {
  if (!url.isValid()) return false;
  const QString scheme = url.scheme().toLower();
  if (scheme != QLatin1String("http") && scheme != QLatin1String("https")) return false;

  // Adult protection is a domain policy. Never classify the serialized URL:
  // paths, queries, fragments, media ids, and metadata text are not domains.
  const QString host = dalinira::core::AdultContentProtectionService::normalizeHost(url.host());
  return !host.isEmpty()
      && dalinira::core::AdultContentProtectionService::instance().isBlockedHost(host);
}

bool containsBlockedAdultUrl(const QUrl &contextUrl, const QVector<QUrl> &urls) {
  if (isProtectedUrlBlocked(contextUrl)) return true;
  for (const QUrl &url : urls) {
    if (isProtectedUrlBlocked(url)) return true;
  }
  return false;
}

void appendProtectionUrl(const QJsonValue &value, QVector<QUrl> *urls, QSet<QString> *seen,
                         bool *complete) {
  if (!urls || !seen || !complete || !value.isString()) return;
  const QUrl url(value.toString());
  const QString scheme = url.scheme().toLower();
  const QString host = dalinira::core::AdultContentProtectionService::normalizeHost(url.host());
  if (!url.isValid() || (scheme != QLatin1String("http") && scheme != QLatin1String("https"))
      || host.isEmpty()) {
    return;
  }

  // The policy classifies domains, not complete URLs. yt-dlp can emit hundreds
  // of signed media and caption URLs for the same host; retaining each query
  // variant can exhaust the safety bound and must not turn a normal site into
  // an adult-content match.
  if (seen->contains(host)) return;
  QUrl hostUrl;
  hostUrl.setScheme(scheme);
  hostUrl.setHost(host);
  if (urls->size() >= kMaximumProtectionUrls) {
    *complete = false;
    // Continue preserving a genuine blocked hostname even after the bounded
    // collection is full. Incompleteness alone is never an adult-domain match.
    if (isProtectedUrlBlocked(hostUrl) && !urls->isEmpty()) urls->last() = hostUrl;
    return;
  }
  seen->insert(host);
  urls->append(hostUrl);
}

void collectProtectionUrls(const QJsonValue &value, QVector<QUrl> *urls, QSet<QString> *seen,
                           bool *complete, int depth = 0) {
  if (!urls || !seen || !complete) return;
  if (depth > 16) {
    *complete = false;
    return;
  }
  if (value.isArray()) {
    for (const QJsonValue &item : value.toArray())
      collectProtectionUrls(item, urls, seen, complete, depth + 1);
    return;
  }
  if (!value.isObject()) return;
  const QJsonObject object = value.toObject();
  static const QSet<QString> urlKeys{
      QStringLiteral("url"), QStringLiteral("original_url"), QStringLiteral("webpage_url"),
      QStringLiteral("webpage_url_basename"), QStringLiteral("manifest_url"),
      QStringLiteral("fragment_base_url"), QStringLiteral("thumbnail")};
  for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
    if (urlKeys.contains(it.key())) appendProtectionUrl(it.value(), urls, seen, complete);
    if (it.value().isArray() || it.value().isObject())
      collectProtectionUrls(it.value(), urls, seen, complete, depth + 1);
  }
}

QString executableName(const QString &base) {
#if defined(Q_OS_WIN)
  return base + QStringLiteral(".exe");
#else
  return base;
#endif
}

QStringList defaultBundledCandidates(const QString &name) {
  const QString appDir = dalinira::application_identity::applicationDirectory();
  return {QDir(appDir).filePath(name), QDir(appDir).filePath(QStringLiteral("bin/") + name)};
}

QStringList defaultFfmpegCandidates(const QString &name) {
  QStringList candidates = defaultBundledCandidates(name);
  const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
  candidates << QDir(dataDir).filePath(QStringLiteral("downloader-bin/") + name);
  return candidates;
}

QString sizeLabel(qint64 bytes) {
  if (bytes <= 0) return I18n::text(QStringLiteral("media.size_unknown"));
  if (bytes >= 1024 * 1024 * 1024LL) return I18n::text(QStringLiteral("units.gigabytes")).arg(bytes / 1024.0 / 1024.0 / 1024.0, 0, 'f', 2);
  return I18n::text(QStringLiteral("units.megabytes")).arg(bytes / 1024.0 / 1024.0, 0, 'f', 1);
}

QString stateText(MediaDownloadState state) {
  switch (state) {
    case MediaDownloadState::Queued: return I18n::text(QStringLiteral("downloads.state_queued"));
    case MediaDownloadState::Downloading: return I18n::text(QStringLiteral("downloads.state_downloading"));
    case MediaDownloadState::Processing: return I18n::text(QStringLiteral("downloads.state_processing"));
    case MediaDownloadState::Completed: return I18n::text(QStringLiteral("downloads.state_completed"));
    case MediaDownloadState::Failed: return I18n::text(QStringLiteral("common.failed"));
    case MediaDownloadState::Cancelled: return I18n::text(QStringLiteral("downloads.state_cancelled"));
  }
  return {};
}

QProcessEnvironment trustedProcessEnvironment(const QString &ytDlpPath, const QString &ffmpegPath) {
  const QProcessEnvironment system = QProcessEnvironment::systemEnvironment();
  QProcessEnvironment environment;
  for (const QString &name : {QStringLiteral("LANG"), QStringLiteral("LC_ALL"), QStringLiteral("LC_CTYPE"),
                              QStringLiteral("TMPDIR"), QStringLiteral("TEMP"), QStringLiteral("TMP"),
                              QStringLiteral("SSL_CERT_FILE"), QStringLiteral("SSL_CERT_DIR"),
                              QStringLiteral("SystemRoot"), QStringLiteral("WINDIR"),
                              QStringLiteral("COMSPEC"), QStringLiteral("USERPROFILE")}) {
    if (system.contains(name)) environment.insert(name, system.value(name));
  }
  QStringList paths;
  if (!ytDlpPath.isEmpty()) paths << QFileInfo(ytDlpPath).absolutePath();
  if (!ffmpegPath.isEmpty()) paths << QFileInfo(ffmpegPath).absolutePath();
#if defined(Q_OS_WIN)
  const QString systemRoot = qEnvironmentVariable("SystemRoot");
  if (!systemRoot.isEmpty()) paths << QDir(systemRoot).filePath(QStringLiteral("System32"));
#else
  paths << QStringLiteral("/usr/bin") << QStringLiteral("/usr/local/bin") << QStringLiteral("/bin")
        << QStringLiteral("/usr/sbin") << QStringLiteral("/usr/local/sbin") << QStringLiteral("/sbin");
#endif
  paths.removeAll(QString{});
  paths.removeDuplicates();
  environment.insert(QStringLiteral("PATH"), paths.join(QDir::listSeparator()));
  return environment;
}

bool isTerminal(MediaDownloadState state) {
  return state == MediaDownloadState::Completed || state == MediaDownloadState::Failed
      || state == MediaDownloadState::Cancelled;
}

QString uniqueFilePath(const QString &directory, const QString &suggestedName) {
  const QString safeName = BrowserSecurity::sanitizeDownloadFileName(suggestedName, QStringLiteral("playlist-links.txt"));
  const QFileInfo nameInfo(safeName);
  const QString suffix = nameInfo.completeSuffix();
  const QString stem = suffix.isEmpty() ? safeName : safeName.left(safeName.size() - suffix.size() - 1);
  for (int counter = 0; counter < 10000; ++counter) {
    const QString numbered = counter == 0 ? safeName
        : stem + QStringLiteral(" (%1)").arg(counter) + (suffix.isEmpty() ? QString{} : QLatin1Char('.') + suffix);
    const QString candidate = QDir(directory).absoluteFilePath(numbered);
    if (!QFileInfo::exists(candidate)) return candidate;
  }
  return {};
}

qint64 jsonInteger(const QJsonObject &object, const QString &name) {
  return static_cast<qint64>(object.value(name).toDouble());
}

}  // namespace

MediaDownloadService::MediaDownloadService(const QString &defaultDownloadDirectory, QObject *parent,
                                           QStringList ytDlpCandidates, QStringList ffmpegCandidates,
                                           QString historyPath, YtDlpUpdateManager *updateManager)
    : QObject(parent), defaultDownloadDirectory_(defaultDownloadDirectory), historyPath_(std::move(historyPath)) {
  const QString ytName = executableName(QStringLiteral("yt-dlp"));
  const QString ffmpegName = executableName(QStringLiteral("ffmpeg"));
  const bool explicitYtDlpCandidates = !ytDlpCandidates.isEmpty();
  if (ytDlpCandidates.isEmpty()) ytDlpCandidates = defaultBundledCandidates(ytName);
  if (ffmpegCandidates.isEmpty()) ffmpegCandidates = defaultFfmpegCandidates(ffmpegName);
  updateManager_ = updateManager ? updateManager : new YtDlpUpdateManager({}, this);
  ytDlpPath_ = BrowserSecurity::resolveTrustedExecutable(ytName, ytDlpCandidates, false);
  if (ytDlpPath_.isEmpty() && explicitYtDlpCandidates) {
    const QString legacyName = executableName(QStringLiteral("ytdlp"));
    ytDlpPath_ = BrowserSecurity::resolveTrustedExecutable(legacyName, ytDlpCandidates, false);
  }
  if (ytDlpPath_.isEmpty() && !explicitYtDlpCandidates && updateManager_)
    ytDlpPath_ = updateManager_->verifiedManagedExecutable();
  if (ytDlpPath_.isEmpty() && !explicitYtDlpCandidates)
    ytDlpPath_ = BrowserSecurity::resolveTrustedExecutable(ytName);
  ffmpegPath_ = BrowserSecurity::resolveTrustedExecutable(ffmpegName, ffmpegCandidates);
  if (historyPath_.isEmpty()) {
    historyPath_ = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
                       .filePath(QStringLiteral("media-download-history.json"));
  }
  analysisTimeout_ = new QTimer(this);
  analysisTimeout_->setSingleShot(true);
  connect(analysisTimeout_, &QTimer::timeout, this, [this] {
    if (!analysisProcess_) return;
    analysisProcess_->kill();
    analysisStderr_ = QByteArrayLiteral("analysis-timeout");
  });
  jsRuntimeTimeout_ = new QTimer(this);
  jsRuntimeTimeout_->setSingleShot(true);
  connect(jsRuntimeTimeout_, &QTimer::timeout, this, [this] {
    if (jsRuntimeProcess_) jsRuntimeProcess_->kill();
  });
  connect(updateManager_, &YtDlpUpdateManager::statusChanged, this,
          [this](const QString &message, int percent) { emit enginePreparationStatus(message, percent); });
  connect(updateManager_, &YtDlpUpdateManager::ready, this,
          [this](const QString &path, bool) {
    if (!path.isEmpty()) ytDlpPath_ = path;
    if (!pendingAnalysisUrl_.isEmpty()) {
      const QUrl url = pendingAnalysisUrl_;
      pendingAnalysisUrl_ = QUrl{};
      prepareRuntimeThenAnalyze(url);
    }
  });
  connect(updateManager_, &YtDlpUpdateManager::failed, this,
          [this](const QString &message, bool existingUsable) {
    if (!pendingAnalysisUrl_.isEmpty()) {
      pendingAnalysisUrl_ = QUrl{};
      emit analysisFailed(existingUsable
          ? I18n::text(QStringLiteral("media.ytdlp_update_failed_current")) : message);
    } else if (existingUsable) {
      emit enginePreparationStatus(I18n::text(QStringLiteral("media.update_failed_using_current")), 0);
    }
  });
  loadHistory();
  // Status texts are cached in the UI language; re-express them after a live
  // language switch so views built later never show the previous language.
  connect(&dalinira::i18n::LanguageManager::instance(), &dalinira::i18n::LanguageManager::languageChanged,
          this, [this] {
            for (auto &job : jobs_) job.statusText = I18n::retranslated(job.statusText);
            emit jobsChanged();
          });
}

MediaDownloadService::~MediaDownloadService() {
  for (QProcess *process : {analysisProcess_, downloadProcess_, jsRuntimeProcess_}) {
    if (!process || process->state() == QProcess::NotRunning) continue;
    process->terminate();
    if (!process->waitForFinished(1200)) {
      process->kill();
      process->waitForFinished(500);
    }
  }
}

bool MediaDownloadService::isSupportedMediaUrl(const QUrl &url, QString *reason) {
  const auto reject = [reason](const QString &message) { if (reason) *reason = message; return false; };
  if (!url.isValid() || url.isEmpty()) return reject(I18n::text(QStringLiteral("media.enter_valid_link")));
  if (url.scheme().compare(QStringLiteral("http"), Qt::CaseInsensitive) != 0
      && url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0) {
    return reject(I18n::text(QStringLiteral("media.only_http_https")));
  }
  if (!url.userName().isEmpty() || !url.password().isEmpty()) {
    return reject(I18n::text(QStringLiteral("media.credentials_rejected")));
  }
  if (url.host().isEmpty()) return reject(I18n::text(QStringLiteral("media.no_valid_host")));
  if (reason) reason->clear();
  return true;
}

bool MediaDownloadService::parseAnalysisJson(const QByteArray &json, const QUrl &url,
                                             MediaAnalysisResult *result, QString *error) {
  if (!result) return false;
  QJsonParseError parseError;
  const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
  if (!document.isObject()) {
    if (error) *error = I18n::text(QStringLiteral("media.info_unreadable"));
    return false;
  }
  const QJsonObject root = document.object();
  MediaAnalysisResult parsed;
  parsed.url = url;
  parsed.adultProtectionContextUrl = url;
  QSet<QString> protectionUrls;
  appendProtectionUrl(QJsonValue(url.toString(QUrl::FullyEncoded)), &parsed.adultProtectionUrls,
                      &protectionUrls, &parsed.adultProtectionUrlsComplete);
  collectProtectionUrls(root, &parsed.adultProtectionUrls, &protectionUrls,
                        &parsed.adultProtectionUrlsComplete);
  parsed.id = root.value(QStringLiteral("id")).toString();
  parsed.title = root.value(QStringLiteral("title")).toString().trimmed().left(240);
  if (parsed.title.isEmpty()) parsed.title = root.value(QStringLiteral("fulltitle")).toString().trimmed().left(240);
  parsed.source = root.value(QStringLiteral("extractor_key")).toString();
  if (parsed.source.isEmpty()) parsed.source = root.value(QStringLiteral("extractor")).toString();
  if (parsed.source.isEmpty()) parsed.source = url.host();
  parsed.thumbnailUrl = root.value(QStringLiteral("thumbnail")).toString();
  parsed.durationSeconds = jsonInteger(root, QStringLiteral("duration"));

  QSet<QString> seenVideo;
  QSet<QString> seenAudio;
  for (const QJsonValue &value : root.value(QStringLiteral("formats")).toArray()) {
    const QJsonObject format = value.toObject();
    MediaFormatOption option;
    option.id = format.value(QStringLiteral("format_id")).toString().trimmed();
    if (option.id.isEmpty()) continue;
    option.extension = format.value(QStringLiteral("ext")).toString().toLower();
    option.videoCodec = format.value(QStringLiteral("vcodec")).toString(QStringLiteral("none"));
    option.audioCodec = format.value(QStringLiteral("acodec")).toString(QStringLiteral("none"));
    option.hasVideo = option.videoCodec != QLatin1String("none") && !option.videoCodec.isEmpty();
    option.hasAudio = option.audioCodec != QLatin1String("none") && !option.audioCodec.isEmpty();
    option.height = format.value(QStringLiteral("height")).toInt();
    option.fps = qRound(format.value(QStringLiteral("fps")).toDouble());
    option.audioBitrate = qRound(format.value(QStringLiteral("abr")).toDouble());
    option.estimatedBytes = jsonInteger(format, QStringLiteral("filesize"));
    if (!option.estimatedBytes) option.estimatedBytes = jsonInteger(format, QStringLiteral("filesize_approx"));
    if (option.hasVideo && !seenVideo.contains(option.id)) {
      seenVideo.insert(option.id);
      const QString quality = option.height > 0
          ? QStringLiteral("%1p%2").arg(option.height).arg(option.fps > 30 ? QStringLiteral(" %1fps").arg(option.fps) : QString{})
          : I18n::text(QStringLiteral("media.kind_video"));
      option.label = QStringLiteral("%1 · %2 · %3%4").arg(quality, option.extension,
          option.videoCodec.section(QLatin1Char('.'), 0, 0), option.hasAudio ? I18n::text(QStringLiteral("media.includes_audio_suffix")) : QString{})
          + QStringLiteral(" · ") + sizeLabel(option.estimatedBytes);
      parsed.videoFormats.append(option);
    }
    if (option.hasAudio && !option.hasVideo && !seenAudio.contains(option.id)) {
      seenAudio.insert(option.id);
      option.label = QStringLiteral("%1 kbps · %2 · %3 · %4")
          .arg(option.audioBitrate > 0 ? QString::number(option.audioBitrate) : QStringLiteral("?"),
               option.extension, option.audioCodec.section(QLatin1Char('.'), 0, 0), sizeLabel(option.estimatedBytes));
      parsed.audioFormats.append(option);
    }
  }
  std::sort(parsed.videoFormats.begin(), parsed.videoFormats.end(), [](const auto &a, const auto &b) {
    if (a.height != b.height) return a.height > b.height;
    return a.fps > b.fps;
  });
  std::sort(parsed.audioFormats.begin(), parsed.audioFormats.end(), [](const auto &a, const auto &b) {
    return a.audioBitrate > b.audioBitrate;
  });
  if (parsed.title.isEmpty() || (parsed.videoFormats.isEmpty() && parsed.audioFormats.isEmpty())) {
    if (error) *error = I18n::text(QStringLiteral("media.no_supported_media"));
    return false;
  }
  *result = parsed;
  if (error) error->clear();
  return true;
}

QStringList MediaDownloadService::buildDownloadArguments(const MediaDownloadRequest &request,
                                                         const QString &ffmpegPath,
                                                         const QString &jsRuntimeArgument) {
  QStringList args{QStringLiteral("--newline"), QStringLiteral("--no-colors"), QStringLiteral("--no-config"),
                   QStringLiteral("--no-update"),
                   QStringLiteral("--no-mtime"), QStringLiteral("--no-overwrites"),
                   QStringLiteral("--windows-filenames"), QStringLiteral("--trim-filenames"), QStringLiteral("200"),
                   QStringLiteral("--progress-delta"), QStringLiteral("0.05"),
                   QStringLiteral("--progress-template"),
                   QStringLiteral("download:DALINIRA_PROGRESS:%(progress._percent_str)s|%(progress.downloaded_bytes)s|%(progress.total_bytes)s|%(progress.total_bytes_estimate)s|%(progress.speed)s|%(progress.eta)s"),
                   QStringLiteral("--progress-template"), QStringLiteral("postprocess:DALINIRA_POST:%(progress.status)s"),
                   QStringLiteral("--print"), QStringLiteral("after_video:DALINIRA_FILE:%(filepath)s"),
                   QStringLiteral("--print"), QStringLiteral("after_move:DALINIRA_FILE:%(filepath)s"),
                   // yt-dlp's --print implicitly enables --quiet. Explicitly
                   // undo it so download progress continues to reach the UI.
                   QStringLiteral("--no-quiet")};
  if (request.playlist) {
    args << QStringLiteral("--yes-playlist") << QStringLiteral("--playlist-start")
         << QString::number(std::max(1, request.playlistStart));
    if (request.playlistEnd >= std::max(1, request.playlistStart))
      args << QStringLiteral("--playlist-end") << QString::number(request.playlistEnd);
  } else {
    args << QStringLiteral("--no-playlist");
  }
  if (!ffmpegPath.isEmpty()) args << QStringLiteral("--ffmpeg-location") << ffmpegPath;
  if (!jsRuntimeArgument.isEmpty()) {
    args << QStringLiteral("--no-js-runtimes") << QStringLiteral("--js-runtimes") << jsRuntimeArgument;
  }
  if (request.kind == MediaDownloadKind::PlaylistThumbnails) {
    args << QStringLiteral("--skip-download") << QStringLiteral("--write-thumbnail");
  } else if (request.kind == MediaDownloadKind::PlaylistLinks) {
    args << QStringLiteral("--skip-download") << QStringLiteral("--flat-playlist")
         << QStringLiteral("--print-to-file") << QStringLiteral("webpage_url")
         << request.auxiliaryOutputPath;
  } else if (request.kind == MediaDownloadKind::Video) {
    QString selector = request.playlist ? QString{} : request.formatId.trimmed();
    if (request.playlist) {
      const QString height = request.formatHeight > 0
          ? QStringLiteral("[height<=%1]").arg(request.formatHeight) : QString{};
      selector = QStringLiteral("bestvideo%1+bestaudio/best%1/best").arg(height);
    }
    if (selector.isEmpty()) selector = QStringLiteral("bestvideo+bestaudio/best");
    else if (!request.formatHasAudio) selector += QStringLiteral("+bestaudio/best");
    args << QStringLiteral("-f") << selector;
    const QString ext = request.formatExtension.toLower();
    args << QStringLiteral("--merge-output-format") << (ext == QLatin1String("webm") ? QStringLiteral("webm") : QStringLiteral("mp4"));
  } else {
    args << QStringLiteral("-f") << (request.playlist || request.formatId.trimmed().isEmpty()
        ? QStringLiteral("bestaudio/best") : request.formatId.trimmed());
    if (request.kind == MediaDownloadKind::AudioConvert) {
      static const QSet<QString> allowed{QStringLiteral("mp3"), QStringLiteral("m4a"), QStringLiteral("opus"),
                                         QStringLiteral("wav"), QStringLiteral("flac"), QStringLiteral("alac")};
      const QString output = allowed.contains(request.audioFormat.toLower()) ? request.audioFormat.toLower() : QStringLiteral("mp3");
      args << QStringLiteral("-x") << QStringLiteral("--audio-format") << output
           << QStringLiteral("--audio-quality") << QStringLiteral("0")
           << QStringLiteral("--embed-metadata") << QStringLiteral("--embed-thumbnail")
           << QStringLiteral("--convert-thumbnails") << QStringLiteral("jpg");
    }
  }
  const int sectionStart = std::max(0, request.sectionStartSeconds);
  const int sectionEnd = std::max(0, request.sectionEndSeconds);
  if (request.kind != MediaDownloadKind::PlaylistThumbnails
      && request.kind != MediaDownloadKind::PlaylistLinks
      && (sectionStart > 0 || sectionEnd > sectionStart)) {
    args << QStringLiteral("--download-sections")
         << QStringLiteral("*%1-%2").arg(sectionStart).arg(sectionEnd > sectionStart
                ? QString::number(sectionEnd) : QStringLiteral("inf"));
  }
  if (request.subtitles) {
    args << QStringLiteral("--write-subs") << QStringLiteral("--write-auto-subs")
         << QStringLiteral("--sub-langs") << QStringLiteral("all");
  }
  const QString outputTemplate = request.playlist
      ? QStringLiteral("%(playlist_title).100B/%(playlist_index)03d - %(title).140B [%(id)s].%(ext)s")
      : QStringLiteral("%(title).160B [%(id)s].%(ext)s");
  args << QStringLiteral("-P") << request.targetDirectory
       << QStringLiteral("-o") << outputTemplate
       << request.url.toString(QUrl::FullyEncoded);
  return args;
}

bool MediaDownloadService::ytDlpAvailable() const { return !ytDlpPath_.isEmpty(); }
bool MediaDownloadService::ffmpegAvailable() const { return !ffmpegPath_.isEmpty(); }
QString MediaDownloadService::ytDlpPath() const { return ytDlpPath_; }
QString MediaDownloadService::ffmpegPath() const { return ffmpegPath_; }
QString MediaDownloadService::javaScriptRuntimeArgument() const { return jsRuntimeArgument_; }
QString MediaDownloadService::defaultDownloadDirectory() const { return defaultDownloadDirectory_; }
QVector<MediaDownloadJob> MediaDownloadService::jobs() const { return jobs_; }
bool MediaDownloadService::analysisRunning() const {
  return analysisProcess_ != nullptr || jsRuntimeProcess_ != nullptr
      || !pendingAnalysisUrl_.isEmpty() || !runtimePendingAnalysisUrl_.isEmpty();
}

void MediaDownloadService::setDefaultDownloadDirectory(const QString &directory) {
  const QFileInfo info(directory);
  if (info.isDir() && info.isAbsolute()) defaultDownloadDirectory_ = info.absoluteFilePath();
}

bool MediaDownloadService::analyze(const QUrl &url, bool adultContentProtectionEnabled) {
  QString reason;
  if (!isSupportedMediaUrl(url, &reason)) { emit analysisFailed(reason); return false; }
  if (analysisRunning()) return false;
  analysisAdultContentProtectionEnabled_ = adultContentProtectionEnabled;
  if (adultContentProtectionEnabled && isProtectedUrlBlocked(url)) {
    emit analysisFailed(kAdultProtectionAnalysisMessage);
    return false;
  }
  if (!ytDlpAvailable()) {
    if (!updateManager_) { emit analysisFailed(I18n::text(QStringLiteral("media.no_working_ytdlp"))); return false; }
    pendingAnalysisUrl_ = url;
    emit enginePreparationStatus(I18n::text(QStringLiteral("media.engine_preparing")), 0);
    updateManager_->ensureInstalled();
    return true;
  }
  if (!updateCheckTriggered_ && updateManager_
      && ytDlpPath_ == updateManager_->verifiedManagedExecutable()) {
    updateCheckTriggered_ = true;
    QTimer::singleShot(0, updateManager_, &YtDlpUpdateManager::checkForUpdate);
  }
  prepareRuntimeThenAnalyze(url);
  return true;
}

void MediaDownloadService::prepareRuntimeThenAnalyze(const QUrl &url) {
  if (jsRuntimeResolved_) { startAnalysisProcess(url); return; }
  runtimePendingAnalysisUrl_ = url;
  if (jsRuntimeCandidates_.isEmpty()) {
    for (const QString &name : {QStringLiteral("deno"), QStringLiteral("node")}) {
      const QString executable = BrowserSecurity::resolveTrustedExecutable(
          executableName(name), defaultBundledCandidates(executableName(name)));
      if (!executable.isEmpty()) jsRuntimeCandidates_.append({name, executable});
    }
  }
  jsRuntimeCandidateIndex_ = 0;
  tryNextJavaScriptRuntime();
}

void MediaDownloadService::tryNextJavaScriptRuntime() {
  if (jsRuntimeCandidateIndex_ >= jsRuntimeCandidates_.size()) {
    jsRuntimeResolved_ = true;
    jsRuntimeArgument_.clear();
    const QUrl url = runtimePendingAnalysisUrl_;
    runtimePendingAnalysisUrl_ = QUrl{};
    startAnalysisProcess(url);
    return;
  }
  const auto candidate = jsRuntimeCandidates_.at(jsRuntimeCandidateIndex_++);
  jsRuntimeOutput_.clear();
  emit enginePreparationStatus(I18n::text(QStringLiteral("media.verifying_js_runtime")), 0);
  jsRuntimeProcess_ = new QProcess(this);
  QProcess *const launched = jsRuntimeProcess_;
  launched->setProgram(candidate.second);
  launched->setArguments({QStringLiteral("--version")});
  launched->setProcessEnvironment(trustedProcessEnvironment(candidate.second, {}));
  launched->setProperty("dalinira-runtime-name", candidate.first);
  connect(launched, &QProcess::readyReadStandardOutput, this, [this, launched] {
    if (jsRuntimeProcess_ != launched) return;
    jsRuntimeOutput_ += launched->readAllStandardOutput();
    if (jsRuntimeOutput_.size() > 2048) launched->kill();
  });
  connect(launched, &QProcess::readyReadStandardError, this, [this, launched] {
    if (jsRuntimeProcess_ != launched) return;
    jsRuntimeOutput_ += launched->readAllStandardError();
    if (jsRuntimeOutput_.size() > 2048) launched->kill();
  });
  connect(launched, &QProcess::errorOccurred, this, [this, launched](QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart && jsRuntimeProcess_ == launched)
      finishJavaScriptRuntimeProbe(launched, true);
  });
  connect(launched, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
          [this, launched](int, QProcess::ExitStatus) {
    if (jsRuntimeProcess_ == launched) finishJavaScriptRuntimeProbe(launched);
  });
  launched->start();
  jsRuntimeTimeout_->start(2500);
}

void MediaDownloadService::finishJavaScriptRuntimeProbe(QProcess *process, bool failedToStart) {
  if (!process || jsRuntimeProcess_ != process) return;
  jsRuntimeTimeout_->stop();
  jsRuntimeOutput_ += process->readAllStandardOutput() + process->readAllStandardError();
  const QString name = process->property("dalinira-runtime-name").toString();
  const QString path = process->program();
  const bool exitedCleanly = !failedToStart && process->exitStatus() == QProcess::NormalExit
      && process->exitCode() == 0 && jsRuntimeOutput_.size() <= 2048;
  const QString output = QString::fromUtf8(jsRuntimeOutput_).trimmed();
  bool supported = false;
  if (exitedCleanly && name == QLatin1String("deno")) {
    const QRegularExpressionMatch match = QRegularExpression(QStringLiteral(R"(\bdeno\s+(\d+)\.(\d+))")).match(output);
    supported = match.hasMatch() && (match.captured(1).toInt() > 2
        || (match.captured(1).toInt() == 2 && match.captured(2).toInt() >= 3));
  } else if (exitedCleanly && name == QLatin1String("node")) {
    const QRegularExpressionMatch match = QRegularExpression(QStringLiteral(R"(v?(\d+)\.)")).match(output);
    supported = match.hasMatch() && match.captured(1).toInt() >= 22;
  }
  process->deleteLater();
  jsRuntimeProcess_ = nullptr;
  if (!supported) { QTimer::singleShot(0, this, &MediaDownloadService::tryNextJavaScriptRuntime); return; }
  jsRuntimeResolved_ = true;
  jsRuntimeArgument_ = name + QLatin1Char(':') + path;
  const QUrl url = runtimePendingAnalysisUrl_;
  runtimePendingAnalysisUrl_ = QUrl{};
  startAnalysisProcess(url);
}

void MediaDownloadService::startAnalysisProcess(const QUrl &url) {
  if (analysisAdultContentProtectionEnabled_ && isProtectedUrlBlocked(url)) {
    emit analysisFailed(kAdultProtectionAnalysisMessage);
    return;
  }
  analysisUrl_ = url;
  analysisStdout_.clear();
  analysisStderr_.clear();
  analysisWasCancelled_ = false;
  analysisProcess_ = new QProcess(this);
  QProcess *const launchedAnalysis = analysisProcess_;
  analysisProcess_->setProgram(ytDlpPath_);
  analysisProcess_->setProcessEnvironment(trustedProcessEnvironment(ytDlpPath_, ffmpegPath_));
  QStringList analysisArguments{QStringLiteral("--dump-single-json"), QStringLiteral("--no-playlist"),
      QStringLiteral("--no-warnings"), QStringLiteral("--no-config"), QStringLiteral("--no-update")};
  if (!jsRuntimeArgument_.isEmpty())
    analysisArguments << QStringLiteral("--no-js-runtimes") << QStringLiteral("--js-runtimes") << jsRuntimeArgument_;
  analysisArguments << url.toString(QUrl::FullyEncoded);
  analysisProcess_->setArguments(analysisArguments);
  connect(analysisProcess_, &QProcess::readyReadStandardOutput, this, [this] {
    analysisStdout_ += analysisProcess_->readAllStandardOutput();
    if (analysisStdout_.size() > kMaximumMetadataBytes) analysisProcess_->kill();
  });
  connect(analysisProcess_, &QProcess::readyReadStandardError, this, [this] {
    analysisStderr_ += analysisProcess_->readAllStandardError();
    if (analysisStderr_.size() > 256 * 1024) analysisStderr_ = analysisStderr_.right(256 * 1024);
  });
  connect(analysisProcess_, &QProcess::errorOccurred, this, [this, launchedAnalysis](QProcess::ProcessError error) {
    if (analysisStderr_.isEmpty()) analysisStderr_ = QByteArrayLiteral("process-start-failed");
    if (error != QProcess::FailedToStart || analysisProcess_ != launchedAnalysis) return;
    analysisTimeout_->stop();
    analysisProcess_->deleteLater();
    analysisProcess_ = nullptr;
    emit analysisFailed(I18n::text(QStringLiteral("media.ytdlp_start_failed")));
  });
  connect(analysisProcess_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
          [this, launchedAnalysis](int exitCode, QProcess::ExitStatus exitStatus) {
    if (analysisProcess_ != launchedAnalysis) return;
    analysisTimeout_->stop();
    QProcess *finished = analysisProcess_;
    analysisProcess_ = nullptr;
    if (finished) {
      analysisStdout_ += finished->readAllStandardOutput();
      analysisStderr_ += finished->readAllStandardError();
      finished->deleteLater();
    }
    if (analysisWasCancelled_) { emit analysisCancelled(); return; }
    if (exitStatus != QProcess::NormalExit || exitCode != 0 || analysisStdout_.size() > kMaximumMetadataBytes) {
      emit analysisFailed(categorizedError(analysisStderr_, exitCode));
      return;
    }
    MediaAnalysisResult result;
    QString error;
    if (!parseAnalysisJson(analysisStdout_, analysisUrl_, &result, &error)) { emit analysisFailed(error); return; }
    result.adultContentProtectionEnabled = analysisAdultContentProtectionEnabled_;
    result.adultProtectionContextUrl = analysisUrl_;
    if (analysisAdultContentProtectionEnabled_
        && containsBlockedAdultUrl(result.adultProtectionContextUrl, result.adultProtectionUrls)) {
      emit analysisFailed(kAdultProtectionAnalysisMessage);
      return;
    }
    emit analysisReady(result);
  });
  emit analysisStarted(url);
  analysisProcess_->start();
  analysisTimeout_->start(60000);
}

void MediaDownloadService::cancelAnalysis() {
  if (!pendingAnalysisUrl_.isEmpty()) {
    pendingAnalysisUrl_ = QUrl{};
    if (updateManager_) updateManager_->cancel();
    emit analysisCancelled();
    return;
  }
  if (!runtimePendingAnalysisUrl_.isEmpty()) {
    runtimePendingAnalysisUrl_ = QUrl{};
    jsRuntimeTimeout_->stop();
    if (jsRuntimeProcess_) {
      QProcess *process = jsRuntimeProcess_;
      jsRuntimeProcess_ = nullptr;
      process->disconnect(this);
      process->kill();
      process->deleteLater();
    }
    emit analysisCancelled();
    return;
  }
  if (!analysisProcess_) return;
  analysisWasCancelled_ = true;
  analysisProcess_->terminate();
  QTimer::singleShot(1200, analysisProcess_, [process = QPointer<QProcess>(analysisProcess_)] {
    if (process && process->state() != QProcess::NotRunning) process->kill();
  });
}

QUuid MediaDownloadService::enqueue(const MediaDownloadRequest &candidate, QString *error) {
  MediaDownloadRequest request = candidate;
  QString reason;
  if (error) error->clear();
  if (request.adultContentProtectionEnabled
      && containsBlockedAdultUrl(request.adultProtectionContextUrl.isEmpty()
                                     ? request.url : request.adultProtectionContextUrl,
                                 request.adultProtectionUrls)) {
    if (error) *error = kAdultProtectionDownloadMessage;
    return {};
  }
  if (!ytDlpAvailable() || !isSupportedMediaUrl(request.url, &reason)) {
    if (error) *error = reason;
    return {};
  }
  const QFileInfo directory(request.targetDirectory.isEmpty() ? defaultDownloadDirectory_ : request.targetDirectory);
  if (!directory.isDir() || !directory.isAbsolute() || !directory.isWritable()) return {};
  request.targetDirectory = directory.absoluteFilePath();
  if (request.kind == MediaDownloadKind::PlaylistThumbnails || request.kind == MediaDownloadKind::PlaylistLinks)
    request.playlist = true;
  if (request.kind == MediaDownloadKind::PlaylistLinks) {
    request.auxiliaryOutputPath = uniqueFilePath(request.targetDirectory,
        request.title + QStringLiteral(" links.txt"));
    if (request.auxiliaryOutputPath.isEmpty()) return {};
  }
  const bool needsFfmpeg = request.kind == MediaDownloadKind::AudioConvert
      || (request.kind == MediaDownloadKind::Video && !request.formatHasAudio)
      || request.sectionStartSeconds > 0 || request.sectionEndSeconds > 0;
  if (needsFfmpeg && !ffmpegAvailable()) return {};
  for (const MediaDownloadJob &existing : std::as_const(jobs_)) {
    if (!isTerminal(existing.state) && existing.url == request.url && existing.kind == request.kind
        && existing.playlist == request.playlist && existing.targetDirectory == request.targetDirectory) {
      return existing.id;
    }
  }
  MediaDownloadJob job;
  job.id = QUuid::createUuid();
  job.url = request.url;
  job.title = request.title.trimmed().left(240);
  if (job.title.isEmpty()) job.title = I18n::text(QStringLiteral("media.default_job_title"));
  job.targetDirectory = request.targetDirectory;
  job.source = request.source.trimmed().left(120);
  job.thumbnailUrl = request.thumbnailUrl.left(2048);
  job.kind = request.kind;
  job.playlist = request.playlist;
  job.totalBytes = std::max<qint64>(0, request.estimatedBytes);
  if (request.kind == MediaDownloadKind::PlaylistLinks) job.outputPath = request.auxiliaryOutputPath;
  job.state = MediaDownloadState::Queued;
  job.statusText = stateText(job.state);
  job.createdAt = QDateTime::currentDateTimeUtc();
  jobs_.prepend(job);
  requests_.insert(job.id, request);
  queue_.enqueue(job.id);
  emit jobEnqueued(job.id);
  emit jobsChanged();
  startNextDownload();
  return job.id;
}

bool MediaDownloadService::cancel(const QUuid &id) {
  const int index = jobIndex(id);
  if (index < 0 || isTerminal(jobs_[index].state)) return false;
  if (id == currentJobId_ && downloadProcess_) {
    currentCancelRequested_ = true;
    downloadProcess_->terminate();
    QTimer::singleShot(1600, downloadProcess_, [process = QPointer<QProcess>(downloadProcess_)] {
      if (process && process->state() != QProcess::NotRunning) process->kill();
    });
    return true;
  }
  if (queue_.removeOne(id)) {
    jobs_[index].state = MediaDownloadState::Cancelled;
    jobs_[index].statusText = stateText(jobs_[index].state);
    persistHistory();
    emit jobsChanged();
    return true;
  }
  return false;
}

QUuid MediaDownloadService::retry(const QUuid &id) {
  const auto request = requests_.constFind(id);
  if (request == requests_.cend()) return {};
  const int index = jobIndex(id);
  if (index < 0 || !isTerminal(jobs_[index].state)) return {};
  return enqueue(*request);
}

QUuid MediaDownloadService::retry(const QUuid &id, bool adultContentProtectionEnabled,
                                  QString *error) {
  const auto stored = requests_.constFind(id);
  if (stored == requests_.cend()) return {};
  const int index = jobIndex(id);
  if (index < 0 || !isTerminal(jobs_[index].state)) return {};
  MediaDownloadRequest request = *stored;
  request.adultContentProtectionEnabled = adultContentProtectionEnabled;
  return enqueue(request, error);
}

bool MediaDownloadService::remove(const QUuid &id) {
  const int index = jobIndex(id);
  if (index < 0 || !isTerminal(jobs_[index].state)) return false;
  jobs_.removeAt(index);
  requests_.remove(id);
  persistHistory();
  emit jobsChanged();
  return true;
}

int MediaDownloadService::jobIndex(const QUuid &id) const {
  for (int index = 0; index < jobs_.size(); ++index) if (jobs_.at(index).id == id) return index;
  return -1;
}

void MediaDownloadService::startNextDownload() {
  if (downloadProcess_ || queue_.isEmpty()) return;
  currentJobId_ = queue_.dequeue();
  const int index = jobIndex(currentJobId_);
  const auto request = requests_.constFind(currentJobId_);
  if (index < 0 || request == requests_.cend()) { currentJobId_ = {}; startNextDownload(); return; }
  if (request->adultContentProtectionEnabled
      && containsBlockedAdultUrl(request->adultProtectionContextUrl.isEmpty()
                                     ? request->url : request->adultProtectionContextUrl,
                                 request->adultProtectionUrls)) {
    finishCurrent(MediaDownloadState::Failed, kAdultProtectionDownloadMessage);
    return;
  }
  jobs_[index].state = MediaDownloadState::Downloading;
  jobs_[index].statusText = stateText(jobs_[index].state);
  currentCancelRequested_ = false;
  downloadStdoutBuffer_.clear();
  downloadStderrBuffer_.clear();
  downloadErrorTail_.clear();
  downloadProcess_ = new QProcess(this);
  QProcess *const launchedDownload = downloadProcess_;
  downloadProcess_->setWorkingDirectory(request->targetDirectory);
  downloadProcess_->setProgram(ytDlpPath_);
  downloadProcess_->setProcessEnvironment(trustedProcessEnvironment(ytDlpPath_, ffmpegPath_));
  downloadProcess_->setArguments(buildDownloadArguments(*request, ffmpegPath_, jsRuntimeArgument_));
  connect(downloadProcess_, &QProcess::readyReadStandardOutput, this, [this] {
    processDownloadOutput(&downloadStdoutBuffer_, downloadProcess_->readAllStandardOutput());
  });
  connect(downloadProcess_, &QProcess::readyReadStandardError, this, [this] {
    const QByteArray chunk = downloadProcess_->readAllStandardError();
    downloadErrorTail_ = (downloadErrorTail_ + chunk).right(256 * 1024);
    processDownloadOutput(&downloadStderrBuffer_, chunk);
  });
  connect(downloadProcess_, &QProcess::errorOccurred, this, [this, launchedDownload](QProcess::ProcessError error) {
    if (downloadErrorTail_.isEmpty()) downloadErrorTail_ = QByteArrayLiteral("process-start-failed");
    if (error == QProcess::FailedToStart && downloadProcess_ == launchedDownload) {
      finishCurrent(MediaDownloadState::Failed, I18n::text(QStringLiteral("media.ytdlp_start_failed")));
    }
  });
  connect(downloadProcess_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
          [this, launchedDownload](int exitCode, QProcess::ExitStatus status) {
    if (downloadProcess_ != launchedDownload) return;
    if (downloadProcess_) {
      processDownloadOutput(&downloadStdoutBuffer_, downloadProcess_->readAllStandardOutput() + QByteArrayLiteral("\n"));
      const QByteArray error = downloadProcess_->readAllStandardError();
      downloadErrorTail_ = (downloadErrorTail_ + error).right(256 * 1024);
      processDownloadOutput(&downloadStderrBuffer_, error + QByteArrayLiteral("\n"));
    }
    if (currentCancelRequested_) finishCurrent(MediaDownloadState::Cancelled);
    else if (status == QProcess::NormalExit && exitCode == 0) finishCurrent(MediaDownloadState::Completed);
    else finishCurrent(MediaDownloadState::Failed, categorizedError(downloadErrorTail_, exitCode));
  });
  emit jobsChanged();
  downloadProcess_->start();
}

void MediaDownloadService::processDownloadOutput(QByteArray *buffer, const QByteArray &chunk) {
  if (!buffer) return;
  *buffer += chunk;
  while (true) {
    const qsizetype nl = buffer->indexOf('\n');
    const qsizetype cr = buffer->indexOf('\r');
    qsizetype splitPos = -1;
    if (nl >= 0 && cr >= 0) splitPos = std::min(nl, cr);
    else if (nl >= 0) splitPos = nl;
    else if (cr >= 0) splitPos = cr;
    else break;

    const QByteArray line = buffer->left(splitPos);
    buffer->remove(0, splitPos + 1);
    const QString text = QString::fromUtf8(line).trimmed();
    if (!text.isEmpty()) {
      processDownloadLine(text);
    }
  }
  if (buffer->size() > 64 * 1024) *buffer = buffer->right(64 * 1024);
}

void MediaDownloadService::processDownloadLine(const QString &line) {
  const int index = jobIndex(currentJobId_);
  if (index < 0 || line.isEmpty()) return;
  if (line.startsWith(QStringLiteral("DALINIRA_PROGRESS:"))) {
    const QStringList fields = line.mid(16).split(QLatin1Char('|'));
    if (fields.size() >= 5) {
      QString cleanPercent;
      for (const QChar c : fields.value(0)) {
        if (c.isDigit() || c == QLatin1Char('.')) cleanPercent.append(c);
      }
      bool ok = false;
      const double parsedPercent = cleanPercent.toDouble(&ok);
      const qint64 downloaded = fields.value(1).toLongLong();

      qint64 total = 0;
      qint64 speed = 0;
      int eta = -1;

      if (fields.size() >= 6) {
        total = fields.value(2).toLongLong();
        if (total <= 0) total = fields.value(3).toLongLong();
        speed = static_cast<qint64>(fields.value(4).toDouble());
        eta = fields.value(5).toInt();
      } else {
        total = fields.value(2).toLongLong();
        speed = static_cast<qint64>(fields.value(3).toDouble());
        eta = fields.value(4).toInt();
      }

      double finalPercent = 0.0;
      if (total > 0 && downloaded > 0) {
        finalPercent = std::clamp(100.0 * static_cast<double>(downloaded) / static_cast<double>(total), 0.0, 100.0);
      } else if (ok && parsedPercent > 0.0) {
        finalPercent = std::clamp(parsedPercent, 0.0, 100.0);
      }

      jobs_[index].percent = finalPercent;
      jobs_[index].downloadedBytes = downloaded;
      if (total > 0) jobs_[index].totalBytes = total;
      jobs_[index].bytesPerSecond = speed;
      jobs_[index].etaSeconds = eta;
      jobs_[index].state = MediaDownloadState::Downloading;
      jobs_[index].statusText = stateText(jobs_[index].state);
      emit jobsChanged();
    }
    return;
  }
  if (line.startsWith(QStringLiteral("DALINIRA_POST:"))) {
    const QString status = line.mid(12).trimmed();
    if (status != QLatin1String("finished")) {
      jobs_[index].state = MediaDownloadState::Processing;
      const MediaDownloadRequest request = requests_.value(currentJobId_);
      if (request.kind == MediaDownloadKind::AudioConvert) {
        jobs_[index].statusText = I18n::text(QStringLiteral("downloads.converting"));
      } else if (request.kind == MediaDownloadKind::Video && !request.formatHasAudio) {
        jobs_[index].statusText = I18n::text(QStringLiteral("media.merging"));
      } else {
        jobs_[index].statusText = stateText(jobs_[index].state);
      }
      emit jobsChanged();
    }
    return;
  }
  if (line.startsWith(QStringLiteral("DALINIRA_FILE:"))) {
    QString reported = line.mid(12).trimmed();
    if (reported.startsWith(QLatin1Char('"')) && reported.endsWith(QLatin1Char('"')) && reported.size() >= 2) {
      reported = reported.mid(1, reported.size() - 2).trimmed();
    }
    const QFileInfo output(reported);
    const QString absolute = output.isAbsolute() ? output.absoluteFilePath()
        : QDir(jobs_[index].targetDirectory).absoluteFilePath(reported);
    const QString cleanAbsolute = QDir::cleanPath(absolute);
    const QString cleanTarget = QDir::cleanPath(jobs_[index].targetDirectory);

    if (!cleanAbsolute.endsWith(QLatin1String(".part"), Qt::CaseInsensitive)
        && !cleanAbsolute.endsWith(QLatin1String(".ytdl"), Qt::CaseInsensitive)
        && !cleanAbsolute.endsWith(QLatin1String(".temp"), Qt::CaseInsensitive)) {
      const QDir root(cleanTarget);
      const QString relative = root.relativeFilePath(cleanAbsolute);
      bool safe = (!QDir::isAbsolutePath(relative) && relative != QLatin1String("..")
                   && !relative.startsWith(QStringLiteral("../"))
                   && !relative.startsWith(QStringLiteral("..\\")));
      if (!safe) {
        const QString canonRoot = QFileInfo(cleanTarget).canonicalFilePath();
        const QString canonFile = QFileInfo(cleanAbsolute).canonicalFilePath();
        if (!canonRoot.isEmpty() && !canonFile.isEmpty()) {
          const QString canonRel = QDir(canonRoot).relativeFilePath(canonFile);
          if (!QDir::isAbsolutePath(canonRel) && canonRel != QLatin1String("..")
              && !canonRel.startsWith(QStringLiteral("../"))
              && !canonRel.startsWith(QStringLiteral("..\\"))) {
            safe = true;
          }
        }
      }
      if (safe) {
        jobs_[index].outputPath = cleanAbsolute;
      }
    }
    return;
  }
  if (line.startsWith(QStringLiteral("[Merger] Merging formats into \"")) && line.endsWith(QLatin1Char('"'))) {
    const QString candidate = line.mid(31, line.size() - 32).trimmed();
    if (!candidate.isEmpty() && !candidate.endsWith(QLatin1String(".part"), Qt::CaseInsensitive)) {
      const QString absolute = QFileInfo(candidate).isAbsolute() ? candidate
          : QDir(jobs_[index].targetDirectory).filePath(candidate);
      const QString clean = QDir::cleanPath(absolute);
      if (clean.startsWith(QDir::cleanPath(jobs_[index].targetDirectory))) {
        jobs_[index].outputPath = clean;
      }
    }
    return;
  }
  if (line.startsWith(QStringLiteral("[ExtractAudio] Destination: "))
      || line.startsWith(QStringLiteral("[download] Destination: "))) {
    const qsizetype idxColon = line.indexOf(QLatin1String("Destination: "));
    if (idxColon >= 0) {
      const QString candidate = line.mid(idxColon + 13).trimmed();
      if (!candidate.isEmpty() && !candidate.endsWith(QLatin1String(".part"), Qt::CaseInsensitive)
          && !candidate.endsWith(QLatin1String(".ytdl"), Qt::CaseInsensitive)) {
        const QString absolute = QFileInfo(candidate).isAbsolute() ? candidate
            : QDir(jobs_[index].targetDirectory).filePath(candidate);
        const QString clean = QDir::cleanPath(absolute);
        if (clean.startsWith(QDir::cleanPath(jobs_[index].targetDirectory))) {
          jobs_[index].outputPath = clean;
        }
      }
    }
    return;
  }
}

void MediaDownloadService::finishCurrent(MediaDownloadState state, const QString &error) {
  const int index = jobIndex(currentJobId_);
  const MediaDownloadRequest request = requests_.value(currentJobId_);
  if (index >= 0) {
    jobs_[index].state = state;
    jobs_[index].statusText = stateText(state);
    jobs_[index].errorText = error;
    if (state == MediaDownloadState::Completed) {
      jobs_[index].percent = 100.0;
      if (jobs_[index].outputPath.isEmpty() || !QFileInfo::exists(jobs_[index].outputPath)) {
        const QString resolved = resolveCompletedFilePath(jobs_[index]);
        if (!resolved.isEmpty()) {
          jobs_[index].outputPath = resolved;
        }
      }
      const QFileInfo output(jobs_[index].outputPath);
      if (output.isFile()) {
        QMimeDatabase database;
        QMimeType mime = database.mimeTypeForFile(output, QMimeDatabase::MatchContent);
        if (!mime.isValid() || mime.name() == QLatin1String("application/octet-stream")) {
          mime = database.mimeTypeForFile(output, QMimeDatabase::MatchExtension);
        }
        jobs_[index].mimeType = mime.name().toLower();
      }
    }
  }
  if (request.kind == MediaDownloadKind::PlaylistLinks && !request.auxiliaryOutputPath.isEmpty()) {
    if (state == MediaDownloadState::Completed) {
      QFile::setPermissions(request.auxiliaryOutputPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    } else {
      QFile::remove(request.auxiliaryOutputPath);
    }
  }
  if (downloadProcess_) downloadProcess_->deleteLater();
  downloadProcess_ = nullptr;
  currentJobId_ = {};
  currentCancelRequested_ = false;
  persistHistory();
  emit jobsChanged();
  QTimer::singleShot(0, this, &MediaDownloadService::startNextDownload);
}

QString MediaDownloadService::categorizedError(const QByteArray &stderrOutput, int exitCode) {
  const QString text = QString::fromUtf8(stderrOutput).toLower();
  if (text.contains(QStringLiteral("analysis-timeout"))) return I18n::text(QStringLiteral("media.analysis_timeout"));
  if (text.contains(QStringLiteral("unsupported url"))) return I18n::text(QStringLiteral("media.no_supported_media"));
  if (text.contains(QStringLiteral("private video")) || text.contains(QStringLiteral("login")) || text.contains(QStringLiteral("sign in")))
    return I18n::text(QStringLiteral("media.private_or_login"));
  if (text.contains(QStringLiteral("no space left"))) return I18n::text(QStringLiteral("media.no_disk_space"));
  if (text.contains(QStringLiteral("permission denied"))) return I18n::text(QStringLiteral("media.no_write_permission"));
  if (text.contains(QStringLiteral("requested format"))) return I18n::text(QStringLiteral("media.format_no_longer_available"));
  if (text.contains(QStringLiteral("network")) || text.contains(QStringLiteral("timed out")) || text.contains(QStringLiteral("unable to download")))
    return I18n::text(QStringLiteral("media.network_error"));
  if (text.contains(QStringLiteral("process-start-failed"))) return I18n::text(QStringLiteral("media.ytdlp_start_failed"));
  return I18n::text(QStringLiteral("media.process_failed_code")).arg(exitCode);
}

QString MediaDownloadService::extractMediaId(const QUrl &url) {
  if (!url.isValid()) return {};
  const QUrlQuery query(url);
  if (query.hasQueryItem(QStringLiteral("v"))) {
    const QString val = query.queryItemValue(QStringLiteral("v")).trimmed();
    if (!val.isEmpty()) return val;
  }
  const QString path = url.path();
  const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
  if (!parts.isEmpty()) {
    return parts.last().trimmed();
  }
  return {};
}

QString MediaDownloadService::resolveCompletedFilePath(const MediaDownloadJob &job) const {
  if (job.targetDirectory.isEmpty()) return {};
  const QDir dir(job.targetDirectory);
  if (!dir.exists()) return {};

  const QString mediaId = extractMediaId(job.url);
  if (!mediaId.isEmpty() && mediaId.size() >= 4) {
    const QString pattern = QStringLiteral("*[%1].*").arg(mediaId);
    const QFileInfoList matches = dir.entryInfoList({pattern}, QDir::Files, QDir::Time);
    for (const QFileInfo &info : matches) {
      const QString name = info.fileName();
      if (!name.endsWith(QLatin1String(".part"), Qt::CaseInsensitive)
          && !name.endsWith(QLatin1String(".ytdl"), Qt::CaseInsensitive)
          && !name.endsWith(QLatin1String(".temp"), Qt::CaseInsensitive)) {
        return info.absoluteFilePath();
      }
    }
  }

  const QString safeTitle = BrowserSecurity::sanitizeDownloadFileName(job.title);
  if (!safeTitle.isEmpty() && safeTitle != QLatin1String("download")) {
    const QString pattern = QStringLiteral("%1*").arg(safeTitle.left(25));
    const QFileInfoList matches = dir.entryInfoList({pattern}, QDir::Files, QDir::Time);
    for (const QFileInfo &info : matches) {
      const QString name = info.fileName();
      if (!name.endsWith(QLatin1String(".part"), Qt::CaseInsensitive)
          && !name.endsWith(QLatin1String(".ytdl"), Qt::CaseInsensitive)
          && !name.endsWith(QLatin1String(".temp"), Qt::CaseInsensitive)) {
        return info.absoluteFilePath();
      }
    }
  }

  return {};
}

void MediaDownloadService::persistHistory() const {
  if (historyPath_.isEmpty()) return;
  QJsonArray entries;
  for (const MediaDownloadJob &job : jobs_) {
    if (!isTerminal(job.state)) continue;
    const MediaDownloadRequest request = requests_.value(job.id);
    QJsonArray protectionUrls;
    for (const QUrl &url : request.adultProtectionUrls) {
      const QUrl sanitized = BrowserSecurity::sanitizeUrlForPersistence(url);
      if (sanitized.isValid()) protectionUrls.append(sanitized.toString(QUrl::FullyEncoded));
    }
    entries.append(QJsonObject{{QStringLiteral("id"), job.id.toString(QUuid::WithoutBraces)},
        {QStringLiteral("url"), BrowserSecurity::sanitizeUrlForPersistence(job.url).toString(QUrl::FullyEncoded)},
        {QStringLiteral("adultProtectionContextUrl"), BrowserSecurity::sanitizeUrlForPersistence(
            request.adultProtectionContextUrl).toString(QUrl::FullyEncoded)},
        {QStringLiteral("adultProtectionUrls"), protectionUrls},
        {QStringLiteral("adultProtectionUrlsComplete"), request.adultProtectionUrlsComplete},
        {QStringLiteral("title"), job.title}, {QStringLiteral("targetDirectory"), job.targetDirectory},
        {QStringLiteral("outputPath"), job.outputPath}, {QStringLiteral("mimeType"), job.mimeType},
        {QStringLiteral("source"), job.source},
        {QStringLiteral("thumbnailUrl"), BrowserSecurity::sanitizeUrlForPersistence(
            QUrl(job.thumbnailUrl)).toString(QUrl::FullyEncoded)},
        {QStringLiteral("kind"), static_cast<int>(job.kind)},
        {QStringLiteral("playlist"), job.playlist}, {QStringLiteral("formatId"), request.formatId},
        {QStringLiteral("formatExtension"), request.formatExtension}, {QStringLiteral("formatHeight"), request.formatHeight},
        {QStringLiteral("formatHasAudio"), request.formatHasAudio},
        {QStringLiteral("estimatedBytes"), request.estimatedBytes},
        {QStringLiteral("audioFormat"), request.audioFormat},
        {QStringLiteral("auxiliaryOutputPath"), request.auxiliaryOutputPath},
        {QStringLiteral("subtitles"), request.subtitles}, {QStringLiteral("playlistStart"), request.playlistStart},
        {QStringLiteral("sectionStartSeconds"), request.sectionStartSeconds},
        {QStringLiteral("sectionEndSeconds"), request.sectionEndSeconds},
        {QStringLiteral("playlistEnd"), request.playlistEnd},
        {QStringLiteral("state"), static_cast<int>(job.state)}, {QStringLiteral("error"), job.errorText},
        {QStringLiteral("createdAt"), job.createdAt.toString(Qt::ISODate)}});
    if (entries.size() >= kMaximumPersistedJobs) break;
  }
  QDir().mkpath(QFileInfo(historyPath_).absolutePath());
  QSaveFile file(historyPath_);
  const QByteArray bytes = QJsonDocument(QJsonObject{{QStringLiteral("version"), 2},
      {QStringLiteral("jobs"), entries}}).toJson(QJsonDocument::Compact);
  if (file.open(QIODevice::WriteOnly)
      && file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
      && file.write(bytes) == bytes.size() && file.commit()) {
    QFile::setPermissions(historyPath_, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  }
}

void MediaDownloadService::loadHistory() {
  QFile file(historyPath_);
  if (!file.open(QIODevice::ReadOnly)) return;
  const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
  for (const QJsonValue &value : document.object().value(QStringLiteral("jobs")).toArray()) {
    if (jobs_.size() >= kMaximumPersistedJobs) break;
    const QJsonObject object = value.toObject();
    const QUrl url = BrowserSecurity::sanitizeUrlForPersistence(QUrl(object.value(QStringLiteral("url")).toString()));
    if (!isSupportedMediaUrl(url)) continue;
    MediaDownloadJob job;
    job.id = QUuid(object.value(QStringLiteral("id")).toString());
    if (job.id.isNull()) job.id = QUuid::createUuid();
    job.url = url;
    job.title = object.value(QStringLiteral("title")).toString().left(240);
    job.targetDirectory = object.value(QStringLiteral("targetDirectory")).toString();
    job.outputPath = object.value(QStringLiteral("outputPath")).toString();
    job.mimeType = object.value(QStringLiteral("mimeType")).toString().left(160).toLower();
    job.source = object.value(QStringLiteral("source")).toString().left(120);
    job.thumbnailUrl = BrowserSecurity::sanitizeUrlForPersistence(
        QUrl(object.value(QStringLiteral("thumbnailUrl")).toString())).toString(QUrl::FullyEncoded).left(2048);
    const int storedKind = object.value(QStringLiteral("kind")).toInt(-1);
    if (storedKind < static_cast<int>(MediaDownloadKind::Video)
        || storedKind > static_cast<int>(MediaDownloadKind::PlaylistLinks)) continue;
    job.kind = static_cast<MediaDownloadKind>(storedKind);
    job.playlist = object.value(QStringLiteral("playlist")).toBool();
    const int storedState = object.value(QStringLiteral("state")).toInt(-1);
    if (storedState < static_cast<int>(MediaDownloadState::Completed)
        || storedState > static_cast<int>(MediaDownloadState::Cancelled)) continue;
    job.state = static_cast<MediaDownloadState>(storedState);
    if (!isTerminal(job.state)) continue;
    job.statusText = stateText(job.state);
    job.errorText = object.value(QStringLiteral("error")).toString();
    job.createdAt = QDateTime::fromString(object.value(QStringLiteral("createdAt")).toString(), Qt::ISODate);
    jobs_.append(job);
    MediaDownloadRequest request;
    request.url = job.url;
    request.adultProtectionContextUrl = BrowserSecurity::sanitizeUrlForPersistence(
        QUrl(object.value(QStringLiteral("adultProtectionContextUrl")).toString()));
    request.adultProtectionUrlsComplete = object.value(
        QStringLiteral("adultProtectionUrlsComplete")).toBool(true);
    for (const QJsonValue &storedUrl : object.value(QStringLiteral("adultProtectionUrls")).toArray()) {
      if (request.adultProtectionUrls.size() >= kMaximumProtectionUrls) {
        request.adultProtectionUrlsComplete = false;
        break;
      }
      const QUrl sanitized = BrowserSecurity::sanitizeUrlForPersistence(QUrl(storedUrl.toString()));
      if (isSupportedMediaUrl(sanitized)) request.adultProtectionUrls.append(sanitized);
    }
    request.title = job.title;
    request.source = job.source;
    request.thumbnailUrl = job.thumbnailUrl;
    request.targetDirectory = job.targetDirectory;
    request.kind = job.kind;
    request.playlist = job.playlist;
    request.formatId = object.value(QStringLiteral("formatId")).toString();
    request.formatExtension = object.value(QStringLiteral("formatExtension")).toString();
    request.formatHeight = object.value(QStringLiteral("formatHeight")).toInt();
    request.formatHasAudio = object.value(QStringLiteral("formatHasAudio")).toBool();
    request.estimatedBytes = object.value(QStringLiteral("estimatedBytes")).toInteger();
    request.audioFormat = object.value(QStringLiteral("audioFormat")).toString(QStringLiteral("mp3"));
    request.auxiliaryOutputPath = object.value(QStringLiteral("auxiliaryOutputPath")).toString();
    request.subtitles = object.value(QStringLiteral("subtitles")).toBool();
    request.sectionStartSeconds = std::max(0, object.value(QStringLiteral("sectionStartSeconds")).toInt());
    request.sectionEndSeconds = std::max(0, object.value(QStringLiteral("sectionEndSeconds")).toInt());
    request.playlistStart = std::max(1, object.value(QStringLiteral("playlistStart")).toInt(1));
    request.playlistEnd = object.value(QStringLiteral("playlistEnd")).toInt();
    requests_.insert(job.id, request);
  }
}
