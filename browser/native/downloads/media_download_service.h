#pragma once

#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QPair>
#include <QQueue>
#include <QStringList>
#include <QUrl>
#include <QUuid>
#include <QVector>

class QProcess;
class QTimer;
class YtDlpUpdateManager;

enum class MediaDownloadState {
  Queued,
  Downloading,
  Processing,
  Completed,
  Failed,
  Cancelled
};

enum class MediaDownloadKind {
  Video,
  AudioOriginal,
  AudioConvert,
  PlaylistThumbnails,
  PlaylistLinks
};

struct MediaFormatOption {
  QString id;
  QString label;
  QString extension;
  QString videoCodec;
  QString audioCodec;
  int height = 0;
  int fps = 0;
  int audioBitrate = 0;
  qint64 estimatedBytes = 0;
  bool hasVideo = false;
  bool hasAudio = false;
};

struct MediaAnalysisResult {
  QUrl url;
  QUrl adultProtectionContextUrl;
  QVector<QUrl> adultProtectionUrls;
  bool adultProtectionUrlsComplete = true;
  bool adultContentProtectionEnabled = false;
  QString id;
  QString title;
  QString source;
  QString thumbnailUrl;
  qint64 durationSeconds = 0;
  QVector<MediaFormatOption> videoFormats;
  QVector<MediaFormatOption> audioFormats;
};

struct MediaDownloadRequest {
  QUrl url;
  QUrl adultProtectionContextUrl;
  QVector<QUrl> adultProtectionUrls;
  bool adultProtectionUrlsComplete = true;
  bool adultContentProtectionEnabled = false;
  QString title;
  QString source;
  QString thumbnailUrl;
  QString targetDirectory;
  MediaDownloadKind kind = MediaDownloadKind::Video;
  QString formatId;
  QString formatExtension;
  int formatHeight = 0;
  bool formatHasAudio = false;
  qint64 estimatedBytes = 0;
  QString audioFormat = QStringLiteral("mp3");
  QString auxiliaryOutputPath;
  bool subtitles = false;
  int sectionStartSeconds = 0;
  int sectionEndSeconds = 0;
  bool playlist = false;
  int playlistStart = 1;
  int playlistEnd = 0;
};

struct MediaDownloadJob {
  QUuid id;
  QUrl url;
  QString title;
  QString targetDirectory;
  QString outputPath;
  QString mimeType;
  QString source;
  QString thumbnailUrl;
  MediaDownloadKind kind = MediaDownloadKind::Video;
  bool playlist = false;
  MediaDownloadState state = MediaDownloadState::Queued;
  double percent = 0.0;
  qint64 downloadedBytes = 0;
  qint64 totalBytes = 0;
  qint64 bytesPerSecond = 0;
  int etaSeconds = -1;
  QString statusText;
  QString errorText;
  QDateTime createdAt;
};

class MediaDownloadService final : public QObject {
  Q_OBJECT
 public:
  explicit MediaDownloadService(const QString &defaultDownloadDirectory,
                                QObject *parent = nullptr,
                                QStringList ytDlpCandidates = {},
                                QStringList ffmpegCandidates = {},
                                QString historyPath = {},
                                YtDlpUpdateManager *updateManager = nullptr);
  ~MediaDownloadService() override;

  static bool isSupportedMediaUrl(const QUrl &url, QString *reason = nullptr);
  static bool parseAnalysisJson(const QByteArray &json, const QUrl &url,
                                MediaAnalysisResult *result, QString *error = nullptr);
  static QStringList buildDownloadArguments(const MediaDownloadRequest &request,
                                            const QString &ffmpegPath = {},
                                            const QString &jsRuntimeArgument = {});

  bool ytDlpAvailable() const;
  bool ffmpegAvailable() const;
  QString ytDlpPath() const;
  QString ffmpegPath() const;
  QString javaScriptRuntimeArgument() const;
  QString defaultDownloadDirectory() const;
  void setDefaultDownloadDirectory(const QString &directory);
  QVector<MediaDownloadJob> jobs() const;
  bool analysisRunning() const;

  bool analyze(const QUrl &url, bool adultContentProtectionEnabled = false);
  void cancelAnalysis();
  QUuid enqueue(const MediaDownloadRequest &request, QString *error = nullptr);
  bool cancel(const QUuid &id);
  QUuid retry(const QUuid &id);
  QUuid retry(const QUuid &id, bool adultContentProtectionEnabled, QString *error = nullptr);
  bool remove(const QUuid &id);

 signals:
  void analysisStarted(const QUrl &url);
  void analysisReady(const MediaAnalysisResult &result);
  void analysisFailed(const QString &message);
  void analysisCancelled();
  void enginePreparationStatus(const QString &message, int percent);
  void jobsChanged();
  void jobEnqueued(const QUuid &id);

 private:
  int jobIndex(const QUuid &id) const;
  void prepareRuntimeThenAnalyze(const QUrl &url);
  void tryNextJavaScriptRuntime();
  void finishJavaScriptRuntimeProbe(QProcess *process, bool failedToStart = false);
  void startAnalysisProcess(const QUrl &url);
  void startNextDownload();
  void processDownloadOutput(QByteArray *buffer, const QByteArray &chunk);
  void processDownloadLine(const QString &line);
  void finishCurrent(MediaDownloadState state, const QString &error = {});
  QString resolveCompletedFilePath(const MediaDownloadJob &job) const;
  static QString extractMediaId(const QUrl &url);
  void persistHistory() const;
  void loadHistory();
  static QString categorizedError(const QByteArray &stderrOutput, int exitCode);

  QString ytDlpPath_;
  QString ffmpegPath_;
  QString defaultDownloadDirectory_;
  QString historyPath_;
  YtDlpUpdateManager *updateManager_ = nullptr;
  QUrl pendingAnalysisUrl_;
  QUrl runtimePendingAnalysisUrl_;
  bool updateCheckTriggered_ = false;
  bool jsRuntimeResolved_ = false;
  QVector<QPair<QString, QString>> jsRuntimeCandidates_;
  int jsRuntimeCandidateIndex_ = 0;
  QString jsRuntimeArgument_;
  QProcess *jsRuntimeProcess_ = nullptr;
  QTimer *jsRuntimeTimeout_ = nullptr;
  QByteArray jsRuntimeOutput_;
  QProcess *analysisProcess_ = nullptr;
  QTimer *analysisTimeout_ = nullptr;
  QByteArray analysisStdout_;
  QByteArray analysisStderr_;
  QUrl analysisUrl_;
  bool analysisAdultContentProtectionEnabled_ = false;
  bool analysisWasCancelled_ = false;
  QProcess *downloadProcess_ = nullptr;
  QByteArray downloadStdoutBuffer_;
  QByteArray downloadStderrBuffer_;
  QByteArray downloadErrorTail_;
  QUuid currentJobId_;
  bool currentCancelRequested_ = false;
  QVector<MediaDownloadJob> jobs_;
  QQueue<QUuid> queue_;
  QHash<QUuid, MediaDownloadRequest> requests_;
};

Q_DECLARE_METATYPE(MediaAnalysisResult)
