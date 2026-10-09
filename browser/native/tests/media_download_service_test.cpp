#include "media_download_service.h"
#include "adult_content_protection.h"
#include "security_utils.h"

#include "i18n/language_manager.h"

#include <QCoreApplication>
#include <QSettings>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimer>

#include <cassert>
#include <functional>
#include <iostream>

namespace {

QString createExecutable(const QString &path, const QByteArray &contents) {
  QFile file(path);
  assert(file.open(QIODevice::WriteOnly));
  assert(file.write(contents) == contents.size());
  file.close();
  assert(QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
  return QFileInfo(path).canonicalFilePath();
}

bool waitFor(std::function<bool()> predicate, int timeoutMs = 5000) {
  QEventLoop loop;
  QTimer poll;
  poll.setInterval(20);
  QTimer timeout;
  timeout.setSingleShot(true);
  QObject::connect(&poll, &QTimer::timeout, &loop, [&] { if (predicate()) loop.quit(); });
  QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
  poll.start();
  timeout.start(timeoutMs);
  loop.exec();
  return predicate();
}

}  // namespace

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  QString reason;
  assert(MediaDownloadService::isSupportedMediaUrl(QUrl(QStringLiteral("https://example.com/watch?v=1")), &reason));
  assert(!MediaDownloadService::isSupportedMediaUrl(QUrl(QStringLiteral("dalinira://settings")), &reason));
  assert(!MediaDownloadService::isSupportedMediaUrl(QUrl(QStringLiteral("file:///tmp/video.mp4")), &reason));
  assert(!MediaDownloadService::isSupportedMediaUrl(QUrl(QStringLiteral("https://user:pass@example.com/video")), &reason));
  assert(BrowserSecurity::sanitizeDownloadFileName(QStringLiteral("../../CON")) == QStringLiteral("download"));
  assert(!BrowserSecurity::sanitizeDownloadFileName(QStringLiteral("../../movie.mp4")).contains(QLatin1Char('/')));

  QByteArray metadata = R"JSON({
    "id":"fixture-id","title":"Fixture Video","extractor_key":"Fixture","duration":125,
    "thumbnail":"https://example.com/thumb.jpg","formats":[
      {"format_id":"v1080","ext":"mp4","height":1080,"fps":60,"vcodec":"avc1.640028","acodec":"none","filesize_approx":10485760},
      {"format_id":"mux720","ext":"mp4","height":720,"fps":30,"vcodec":"avc1.4d401f","acodec":"mp4a.40.2","filesize":5242880},
      {"format_id":"a1","ext":"m4a","vcodec":"none","acodec":"mp4a.40.2","abr":128,"filesize":1048576}
    ]})JSON";
  QByteArray redirectedMetadata = R"JSON({
    "id":"redirect-fixture","title":"Redirected Fixture","extractor_key":"Fixture",
    "webpage_url":"https://adult-test.example/redirected","formats":[
      {"format_id":"redirect-video","ext":"mp4","height":720,"vcodec":"avc1","acodec":"mp4a",
       "url":"https://cdn.example/media.mp4"}
    ]})JSON";
  MediaAnalysisResult parsed;
  assert(MediaDownloadService::parseAnalysisJson(metadata, QUrl(QStringLiteral("https://example.com/watch?v=1")), &parsed, &reason));
  assert(parsed.title == QStringLiteral("Fixture Video"));
  assert(parsed.videoFormats.size() == 2 && parsed.videoFormats.front().height == 1080);
  assert(parsed.audioFormats.size() == 1 && parsed.audioFormats.front().id == QStringLiteral("a1"));

  MediaAnalysisResult emptyParsed;
  QString emptyReason;
  const QByteArray noMediaMetadata = R"JSON({"id":"nomedia","title":"Webpage with no media","formats":[]})JSON";
  assert(!MediaDownloadService::parseAnalysisJson(noMediaMetadata, QUrl(QStringLiteral("https://example.com")), &emptyParsed, &emptyReason));
  assert(emptyReason == QStringLiteral("Bu bağlantıda desteklenen video veya ses bulunamadı."));

  QTemporaryDir temporary;
  assert(temporary.isValid());
  const QString binDir = QDir(temporary.path()).filePath(QStringLiteral("bin"));
  const QString outputDir = QDir(temporary.path()).filePath(QStringLiteral("downloads"));
  assert(QDir().mkpath(binDir));
  assert(QDir().mkpath(outputDir));
  assert(QFile::setPermissions(binDir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
  const QString fixtureOutput = QDir(outputDir).filePath(QStringLiteral("Fixture Video [fixture-id].mp4"));
  const QString invocationLog = QDir(temporary.path()).filePath(QStringLiteral("yt-dlp-invocations.log"));
  qputenv("DALINIRA_MEDIA_TEST_SECRET", QByteArrayLiteral("must-not-reach-helper"));
  QByteArray script = QByteArrayLiteral("#!/bin/sh\n")
      + QByteArrayLiteral("if [ -n \"$DALINIRA_MEDIA_TEST_SECRET\" ]; then exit 91; fi\n")
      + QByteArrayLiteral("printf 'invoked\\n' >> '") + invocationLog.toUtf8() + QByteArrayLiteral("'\n")
      + QByteArrayLiteral("for arg in \"$@\"; do case \"$arg\" in *cancel-fixture*) sleep 10; exit 0;; esac; done\n")
      + QByteArrayLiteral("for arg in \"$@\"; do case \"$arg\" in *redirect-fixture*) printf '%s\\n' '")
      + redirectedMetadata.replace('\n', ' ') + QByteArrayLiteral("'; exit 0;; esac; done\n")
      + QByteArrayLiteral("for arg in \"$@\"; do if [ \"$arg\" = \"--dump-single-json\" ]; then printf '%s\\n' '")
      + metadata.replace('\n', ' ') + QByteArrayLiteral("'; exit 0; fi; done\n")
      + QByteArrayLiteral("target=''\nlink_file=''\ncapture_link_path=0\nprev=''\n")
      + QByteArrayLiteral("for arg in \"$@\"; do if [ \"$capture_link_path\" = 1 ]; then link_file=\"$arg\"; capture_link_path=0; fi; if [ \"$prev\" = \"--print-to-file\" ] && [ \"$arg\" = \"webpage_url\" ]; then capture_link_path=1; fi; if [ \"$prev\" = \"-P\" ]; then target=\"$arg\"; fi; prev=\"$arg\"; done\n")
      + QByteArrayLiteral("if [ -n \"$link_file\" ]; then printf '%s\\n' 'https://example.com/playlist-item' > \"$link_file\"; exit 0; fi\n")
      + QByteArrayLiteral("printf 'DALINIRA_PROGRESS:42.5%%|425|1000|100|6\\n'\n")
      + QByteArrayLiteral("printf 'DALINIRA_POST:started\\n'\n")
      + QByteArrayLiteral("file=\"$target/Fixture Video [fixture-id].mp4\"\n: > \"$file\"\nprintf 'DALINIRA_FILE:%s\\n' \"$file\"\n");
  const QString ytDlp = createExecutable(QDir(binDir).filePath(QStringLiteral("yt-dlp")), script);
  const QString legacyYtDlp = createExecutable(QDir(binDir).filePath(QStringLiteral("ytdlp")), script);
  const QString ffmpeg = createExecutable(QDir(binDir).filePath(QStringLiteral("ffmpeg")), QByteArrayLiteral("#!/bin/sh\nexit 0\n"));
  const QString historyPath = QDir(temporary.path()).filePath(QStringLiteral("history.json"));
  MediaDownloadService legacyNamedService(outputDir, nullptr, {legacyYtDlp}, {ffmpeg},
      QDir(temporary.path()).filePath(QStringLiteral("legacy-name-history.json")));
  assert(legacyNamedService.ytDlpPath() == legacyYtDlp);
  MediaDownloadService service(outputDir, nullptr, {ytDlp}, {ffmpeg}, historyPath);
  assert(service.ytDlpAvailable() && service.ffmpegAvailable());
  assert(QFileInfo(service.ytDlpPath()).isAbsolute());

  bool analysisReady = false;
  MediaAnalysisResult normalAnalysis;
  QObject::connect(&service, &MediaDownloadService::analysisReady, &app,
                   [&](const MediaAnalysisResult &result) {
    if (result.title == QStringLiteral("Fixture Video")) {
      analysisReady = true;
      if (normalAnalysis.url.isEmpty()) normalAnalysis = result;
    }
  });
  assert(service.analyze(QUrl(QStringLiteral("https://example.com/watch?v=1"))));
  assert(waitFor([&] { return analysisReady; }));
  const QString trustedDeno = BrowserSecurity::resolveTrustedExecutable(QStringLiteral("deno"));
  const QString trustedNode = BrowserSecurity::resolveTrustedExecutable(QStringLiteral("node"));
  const QString runtime = service.javaScriptRuntimeArgument();
  if (!runtime.isEmpty()) {
    assert(runtime == QStringLiteral("deno:") + trustedDeno
        || runtime == QStringLiteral("node:") + trustedNode);
    assert(QFileInfo(runtime.section(QLatin1Char(':'), 1)).isAbsolute());
  }

  auto &adultProtection = dalinira::core::AdultContentProtectionService::instance();
  adultProtection.clear();
  adultProtection.clearAllowlist();
  adultProtection.loadFromLines({QStringLiteral("adult-test.example")});
  const QUrl normalYoutubeUrl(QStringLiteral(
      "https://www.youtube.com/watch?v=AsRD9IhICs8&list=RDAsRD9IhICs8&start_radio=1"));

  // A current YouTube analysis can contain hundreds of distinct signed
  // subtitle URLs on the same hostname. Domain protection must deduplicate by
  // normalized hostname instead of treating the URL count as adult content.
  QJsonObject largeYoutubeMetadata = QJsonDocument::fromJson(metadata).object();
  QJsonArray captionUrls;
  for (int index = 0; index < 700; ++index) {
    captionUrls.append(QJsonObject{{QStringLiteral("url"),
        QStringLiteral("https://www.youtube.com/api/timedtext?translation=%1").arg(index)}});
  }
  largeYoutubeMetadata.insert(QStringLiteral("automatic_captions"), captionUrls);
  MediaAnalysisResult largeYoutubeAnalysis;
  assert(MediaDownloadService::parseAnalysisJson(
      QJsonDocument(largeYoutubeMetadata).toJson(QJsonDocument::Compact), normalYoutubeUrl,
      &largeYoutubeAnalysis, &reason));
  assert(largeYoutubeAnalysis.adultProtectionUrlsComplete);
  assert(largeYoutubeAnalysis.adultProtectionUrls.size() == 2);
  for (const QUrl &protectionUrl : largeYoutubeAnalysis.adultProtectionUrls) {
    assert(protectionUrl.query().isEmpty());
    assert(!adultProtection.isBlockedHost(protectionUrl.host()));
  }

  const QUrl normalUrlWithBlockedText(QStringLiteral(
      "https://normal.example/video/adult-test.example?next=adult-test.example"));
  for (const QUrl &normalUrl : {normalYoutubeUrl, normalUrlWithBlockedText}) {
    analysisReady = false;
    assert(service.analyze(normalUrl, true));
    assert(waitFor([&] { return analysisReady; }));
  }
  const QUrl blockedUrl(QStringLiteral("https://adult-test.example/watch?v=blocked"));
  const qint64 invocationsBeforeBlocked = QFileInfo(invocationLog).size();
  bool blockedAnalysisStarted = false;
  QString blockedAnalysisError;
  const QMetaObject::Connection blockedStartedConnection = QObject::connect(
      &service, &MediaDownloadService::analysisStarted, &app,
      [&](const QUrl &url) { if (url == blockedUrl) blockedAnalysisStarted = true; });
  const QMetaObject::Connection blockedErrorConnection = QObject::connect(
      &service, &MediaDownloadService::analysisFailed, &app,
      [&](const QString &message) { blockedAnalysisError = message; });
  assert(!service.analyze(blockedUrl, true));
  assert(!blockedAnalysisStarted);
  assert(blockedAnalysisError
      == QStringLiteral("Yetişkin İçerik Koruması bu bağlantının analiz edilmesini engelledi."));
  assert(QFileInfo(invocationLog).size() == invocationsBeforeBlocked);
  QObject::disconnect(blockedStartedConnection);
  QObject::disconnect(blockedErrorConnection);

  analysisReady = false;
  assert(service.analyze(blockedUrl, false));
  assert(waitFor([&] { return analysisReady; }));

  bool redirectedReady = false;
  QString redirectedError;
  const QMetaObject::Connection redirectReadyConnection = QObject::connect(
      &service, &MediaDownloadService::analysisReady, &app,
      [&](const MediaAnalysisResult &result) {
    if (result.title == QStringLiteral("Redirected Fixture")) redirectedReady = true;
  });
  const QMetaObject::Connection redirectErrorConnection = QObject::connect(
      &service, &MediaDownloadService::analysisFailed, &app,
      [&](const QString &message) { redirectedError = message; });
  assert(service.analyze(QUrl(QStringLiteral("https://example.com/redirect-fixture")), true));
  assert(waitFor([&] { return !redirectedError.isEmpty(); }));
  assert(!redirectedReady);
  assert(redirectedError
      == QStringLiteral("Yetişkin İçerik Koruması bu bağlantının analiz edilmesini engelledi."));
  QObject::disconnect(redirectReadyConnection);
  QObject::disconnect(redirectErrorConnection);

  MediaDownloadRequest request;
  request.url = QUrl(QStringLiteral("https://example.com/watch?q=kept&code=synthetic-code&text=%24%28touch%20never%29"));
  request.title = QStringLiteral("Fixture Video");
  request.source = QStringLiteral("Fixture");
  request.thumbnailUrl = QStringLiteral("https://example.com/thumb.jpg");
  request.targetDirectory = outputDir;
  request.kind = MediaDownloadKind::Video;
  request.formatId = QStringLiteral("v1080");
  request.formatExtension = QStringLiteral("mp4");
  request.formatHasAudio = false;
  request.estimatedBytes = 2048;

  MediaDownloadRequest blockedContextRequest = request;
  blockedContextRequest.url = QUrl(QStringLiteral("https://cdn.example/allowed-media.mp4"));
  blockedContextRequest.adultProtectionContextUrl = blockedUrl;
  blockedContextRequest.adultContentProtectionEnabled = true;
  QString blockedDownloadError;
  assert(service.enqueue(blockedContextRequest, &blockedDownloadError).isNull());
  assert(blockedDownloadError
      == QStringLiteral("Yetişkin İçerik Koruması bu indirmenin başlatılmasını engelledi."));

  MediaDownloadRequest blockedFinalRequest = request;
  blockedFinalRequest.url = QUrl(QStringLiteral("https://example.com/final-url-check"));
  blockedFinalRequest.adultProtectionContextUrl = blockedFinalRequest.url;
  blockedFinalRequest.adultProtectionUrls = {QUrl(QStringLiteral("https://adult-test.example/final.mp4"))};
  blockedFinalRequest.adultContentProtectionEnabled = true;
  blockedDownloadError.clear();
  assert(service.enqueue(blockedFinalRequest, &blockedDownloadError).isNull());
  assert(!blockedDownloadError.isEmpty());

  MediaDownloadRequest incompleteFinalRequest = request;
  incompleteFinalRequest.url = QUrl(QStringLiteral("https://example.com/incomplete-metadata"));
  incompleteFinalRequest.adultContentProtectionEnabled = true;
  incompleteFinalRequest.adultProtectionUrlsComplete = false;
  blockedDownloadError.clear();
  assert(!service.enqueue(incompleteFinalRequest, &blockedDownloadError).isNull());
  assert(blockedDownloadError.isEmpty());

  MediaDownloadRequest normalProtectedRequest = request;
  normalProtectedRequest.url = QUrl(QStringLiteral("https://example.com/normal-protected"));
  normalProtectedRequest.adultProtectionContextUrl = normalAnalysis.adultProtectionContextUrl;
  normalProtectedRequest.adultProtectionUrls = normalAnalysis.adultProtectionUrls;
  normalProtectedRequest.adultContentProtectionEnabled = true;
  assert(!service.enqueue(normalProtectedRequest).isNull());

  MediaDownloadRequest protectionOffRequest = blockedContextRequest;
  protectionOffRequest.url = QUrl(QStringLiteral("https://cdn.example/protection-off.mp4"));
  protectionOffRequest.adultContentProtectionEnabled = false;
  const QUuid protectionOffId = service.enqueue(protectionOffRequest);
  assert(!protectionOffId.isNull());
  const QStringList args = MediaDownloadService::buildDownloadArguments(request, ffmpeg);
  assert(args.contains(request.url.toString(QUrl::FullyEncoded)));
  assert(args.contains(QStringLiteral("--no-config")));
  assert(args.contains(QStringLiteral("--progress-delta")) && args.contains(QStringLiteral("0.05")));
  assert(args.indexOf(QStringLiteral("--no-quiet")) > args.indexOf(QStringLiteral("--print")));
  assert(args.contains(QStringLiteral("--ffmpeg-location")) && args.contains(ffmpeg));
  assert(!args.contains(QStringLiteral("sh")) && !args.contains(QStringLiteral("-c")));
  assert(args.contains(QStringLiteral("--no-playlist")) && !args.contains(QStringLiteral("--yes-playlist")));
  MediaDownloadRequest originalAudioRequest = request;
  originalAudioRequest.kind = MediaDownloadKind::AudioOriginal;
  originalAudioRequest.formatId = QStringLiteral("a1");
  const QStringList originalAudioArgs = MediaDownloadService::buildDownloadArguments(originalAudioRequest, ffmpeg);
  assert(originalAudioArgs.contains(QStringLiteral("a1")) && !originalAudioArgs.contains(QStringLiteral("-x")));
  MediaDownloadRequest convertedAudioRequest = originalAudioRequest;
  convertedAudioRequest.kind = MediaDownloadKind::AudioConvert;
  convertedAudioRequest.audioFormat = QStringLiteral("opus");
  const QStringList convertedAudioArgs = MediaDownloadService::buildDownloadArguments(convertedAudioRequest, ffmpeg);
  assert(convertedAudioArgs.contains(QStringLiteral("-x"))
      && convertedAudioArgs.contains(QStringLiteral("--audio-format"))
      && convertedAudioArgs.contains(QStringLiteral("opus")));
  const QString runtimeArgument = QStringLiteral("node:/usr/bin/node");
  const QStringList runtimeArgs = MediaDownloadService::buildDownloadArguments(request, ffmpeg, runtimeArgument);
  assert(runtimeArgs.contains(QStringLiteral("--no-js-runtimes")));
  assert(runtimeArgs.contains(QStringLiteral("--js-runtimes")) && runtimeArgs.contains(runtimeArgument));
  MediaDownloadRequest sectionRequest = request;
  sectionRequest.sectionStartSeconds = 15;
  sectionRequest.sectionEndSeconds = 75;
  const QStringList sectionArgs = MediaDownloadService::buildDownloadArguments(sectionRequest, ffmpeg);
  assert(sectionArgs.contains(QStringLiteral("--download-sections")));
  assert(sectionArgs.contains(QStringLiteral("*15-75")));
  MediaDownloadRequest playlistRequest = request;
  playlistRequest.playlist = true;
  playlistRequest.formatHeight = 1080;
  playlistRequest.playlistStart = 2;
  playlistRequest.playlistEnd = 5;
  const QStringList playlistArgs = MediaDownloadService::buildDownloadArguments(playlistRequest, ffmpeg);
  assert(playlistArgs.contains(QStringLiteral("--yes-playlist")) && !playlistArgs.contains(QStringLiteral("--no-playlist")));
  assert(playlistArgs.contains(QStringLiteral("--playlist-start")) && playlistArgs.contains(QStringLiteral("2")));
  assert(playlistArgs.contains(QStringLiteral("--playlist-end")) && playlistArgs.contains(QStringLiteral("5")));
  MediaDownloadRequest thumbnailRequest = playlistRequest;
  thumbnailRequest.kind = MediaDownloadKind::PlaylistThumbnails;
  const QStringList thumbnailArgs = MediaDownloadService::buildDownloadArguments(thumbnailRequest, ffmpeg);
  assert(thumbnailArgs.contains(QStringLiteral("--skip-download")));
  assert(thumbnailArgs.contains(QStringLiteral("--write-thumbnail")));
  MediaDownloadRequest linksRequest = playlistRequest;
  linksRequest.kind = MediaDownloadKind::PlaylistLinks;
  linksRequest.auxiliaryOutputPath = QDir(outputDir).filePath(QStringLiteral("Fixture links.txt"));
  const QStringList linksArgs = MediaDownloadService::buildDownloadArguments(linksRequest, ffmpeg);
  assert(linksArgs.contains(QStringLiteral("--flat-playlist")));
  assert(linksArgs.contains(QStringLiteral("--print-to-file")));
  assert(linksArgs.contains(linksRequest.auxiliaryOutputPath));
  bool sawDownloadProgress = false;
  bool sawMerging = false;
  bool sawConverting = false;
  QObject::connect(&service, &MediaDownloadService::jobsChanged, &app, [&] {
    for (const auto &job : service.jobs()) {
      if (job.state == MediaDownloadState::Downloading && job.percent == 42.5
          && job.downloadedBytes == 425 && job.totalBytes == 1000
          && job.bytesPerSecond == 100 && job.etaSeconds == 6) sawDownloadProgress = true;
      if (job.state == MediaDownloadState::Processing && job.statusText == QStringLiteral("Birleştiriliyor"))
        sawMerging = true;
      if (job.state == MediaDownloadState::Processing && job.statusText == QStringLiteral("Dönüştürülüyor"))
        sawConverting = true;
    }
  });
  const QUuid id = service.enqueue(request);
  assert(!id.isNull());
  bool sawInitialEstimate = false;
  for (const auto &job : service.jobs()) {
    if (job.id == id && job.totalBytes == request.estimatedBytes) {
      sawInitialEstimate = true;
      break;
    }
  }
  assert(sawInitialEstimate);
  assert(waitFor([&] {
    for (const auto &job : service.jobs()) if (job.id == id) return job.state == MediaDownloadState::Completed;
    return false;
  }));
  assert(QFileInfo::exists(fixtureOutput));
  assert(sawDownloadProgress && sawMerging);
  assert(waitFor([&] {
    for (const auto &job : service.jobs()) if (job.id == protectionOffId)
      return job.state == MediaDownloadState::Completed;
    return false;
  }));
  blockedDownloadError.clear();
  assert(service.retry(protectionOffId, true, &blockedDownloadError).isNull());
  assert(blockedDownloadError
      == QStringLiteral("Yetişkin İçerik Koruması bu indirmenin başlatılmasını engelledi."));

  convertedAudioRequest.url = QUrl(QStringLiteral("https://example.com/audio-convert"));
  convertedAudioRequest.targetDirectory = outputDir;
  const QUuid convertedAudioId = service.enqueue(convertedAudioRequest);
  assert(!convertedAudioId.isNull());
  assert(waitFor([&] {
    for (const auto &job : service.jobs()) if (job.id == convertedAudioId)
      return job.state == MediaDownloadState::Completed;
    return false;
  }));
  assert(sawConverting);

  const QUuid linksId = service.enqueue(linksRequest);
  assert(!linksId.isNull());
  assert(waitFor([&] {
    for (const auto &job : service.jobs()) if (job.id == linksId) return job.state == MediaDownloadState::Completed;
    return false;
  }));
  QString linksOutput;
  for (const auto &job : service.jobs()) if (job.id == linksId) linksOutput = job.outputPath;
  QFile linksFile(linksOutput);
  assert(linksFile.open(QIODevice::ReadOnly));
  assert(linksFile.readAll().contains("https://example.com/playlist-item"));
  const auto linksPermissions = QFileInfo(linksOutput).permissions();
  assert(!(linksPermissions & (QFileDevice::ReadGroup | QFileDevice::WriteGroup
                               | QFileDevice::ReadOther | QFileDevice::WriteOther)));

  MediaDownloadRequest cancelledRequest = request;
  cancelledRequest.url = QUrl(QStringLiteral("https://example.com/cancel-fixture"));
  const QUuid cancelledId = service.enqueue(cancelledRequest);
  const QUuid duplicateActiveId = service.enqueue(cancelledRequest);
  const QUuid queuedAfterCancelId = service.enqueue(request);
  assert(!cancelledId.isNull() && duplicateActiveId == cancelledId && !queuedAfterCancelId.isNull());
  assert(waitFor([&] {
    for (const auto &job : service.jobs()) if (job.id == cancelledId) return job.state == MediaDownloadState::Downloading;
    return false;
  }));
  assert(service.cancel(cancelledId));
  assert(waitFor([&] {
    bool cancelled = false;
    bool nextCompleted = false;
    for (const auto &job : service.jobs()) {
      if (job.id == cancelledId) cancelled = job.state == MediaDownloadState::Cancelled;
      if (job.id == queuedAfterCancelId) nextCompleted = job.state == MediaDownloadState::Completed;
    }
    return cancelled && nextCompleted;
  }));
  const QUuid retriedId = service.retry(cancelledId);
  assert(!retriedId.isNull() && retriedId != cancelledId);
  assert(waitFor([&] {
    for (const auto &job : service.jobs()) if (job.id == retriedId)
      return job.state == MediaDownloadState::Downloading;
    return false;
  }));
  assert(service.cancel(retriedId));
  assert(waitFor([&] {
    for (const auto &job : service.jobs()) if (job.id == retriedId)
      return job.state == MediaDownloadState::Cancelled;
    return false;
  }));

  const QString slowBinDir = QDir(temporary.path()).filePath(QStringLiteral("slow-bin"));
  assert(QDir().mkpath(slowBinDir));
  assert(QFile::setPermissions(slowBinDir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
  const QString slowYtDlp = createExecutable(QDir(slowBinDir).filePath(QStringLiteral("yt-dlp")),
      QByteArrayLiteral("#!/bin/sh\nsleep 10\nexit 0\n"));
  MediaDownloadService analysisCancelService(outputDir, nullptr, {slowYtDlp}, {ffmpeg},
      QDir(temporary.path()).filePath(QStringLiteral("cancel-history.json")));
  bool analysisCancelled = false;
  QObject::connect(&analysisCancelService, &MediaDownloadService::analysisCancelled, &app,
                   [&] { analysisCancelled = true; });
  assert(analysisCancelService.analyze(QUrl(QStringLiteral("https://example.com/slow-analysis"))));
  assert(analysisCancelService.analysisRunning());
  analysisCancelService.cancelAnalysis();
  assert(waitFor([&] { return analysisCancelled && !analysisCancelService.analysisRunning(); }));

  QFile history(historyPath);
  assert(history.open(QIODevice::ReadOnly));
  const QByteArray historyBytes = history.readAll();
  assert(!historyBytes.contains("synthetic-code"));
  assert(historyBytes.contains("q=kept"));
  assert(historyBytes.contains("\"source\":\"Fixture\""));
  assert(historyBytes.contains("\"thumbnailUrl\":\"https://example.com/thumb.jpg\""));
  const auto permissions = QFileInfo(historyPath).permissions();
  assert(!(permissions & (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ReadOther | QFileDevice::WriteOther)));

  MediaDownloadService restored(outputDir, nullptr, {ytDlp}, {ffmpeg}, historyPath);
  bool restoredMetadata = false;
  for (const MediaDownloadJob &job : restored.jobs()) {
    if (job.title == QStringLiteral("Fixture Video")
        && job.source == QStringLiteral("Fixture")
        && job.thumbnailUrl == QStringLiteral("https://example.com/thumb.jpg")
        && !job.outputPath.isEmpty()) {
      restoredMetadata = true;
      break;
    }
  }
  assert(restoredMetadata);

  // A restored history entry keeps no stale language: after a live switch its
  // status follows the new UI language (the Downloads page is often opened
  // only after switching, e.g. Turkish -> English must not show "Tamamlandı").
  {
    QTemporaryDir settingsRoot;
    assert(settingsRoot.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsRoot.path());
    auto &languages = dalinira::i18n::LanguageManager::instance();
    int refreshes = 0;
    QObject::connect(&restored, &MediaDownloadService::jobsChanged, [&refreshes] { ++refreshes; });
    for (const QString &code : {QStringLiteral("tr"), QStringLiteral("en"), QStringLiteral("ar"), QStringLiteral("fr")}) {
      const int before = refreshes;
      languages.setLanguagePreference(code);
      assert(languages.activeLanguageCode() == code);
      assert(refreshes > before && "views are told to redraw in the new language");
      bool sawCompleted = false;
      for (const MediaDownloadJob &job : restored.jobs()) {
        if (job.state != MediaDownloadState::Completed) continue;
        sawCompleted = true;
        assert(job.statusText == languages.translateIn(code, QStringLiteral("downloads.state_completed")));
      }
      assert(sawCompleted);
    }
  }

  const QString malformedHistoryPath = QDir(temporary.path()).filePath(QStringLiteral("malformed-history.json"));
  QJsonArray malformedJobs;
  for (int i = 0; i < 205; ++i) {
    malformedJobs.append(QJsonObject{
        {QStringLiteral("id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {QStringLiteral("url"), QStringLiteral("https://example.com/media/%1").arg(i)},
        {QStringLiteral("title"), QStringLiteral("Stored %1").arg(i)},
        {QStringLiteral("thumbnailUrl"), QStringLiteral("https://user:secret@example.com/cover.jpg")},
        {QStringLiteral("kind"), static_cast<int>(MediaDownloadKind::Video)},
        {QStringLiteral("state"), static_cast<int>(MediaDownloadState::Completed)}});
  }
  malformedJobs.prepend(QJsonObject{
      {QStringLiteral("url"), QStringLiteral("https://example.com/invalid-state")},
      {QStringLiteral("kind"), 999}, {QStringLiteral("state"), 999}});
  QFile malformedHistory(malformedHistoryPath);
  assert(malformedHistory.open(QIODevice::WriteOnly));
  assert(malformedHistory.write(QJsonDocument(QJsonObject{
      {QStringLiteral("version"), 2}, {QStringLiteral("jobs"), malformedJobs}})
      .toJson(QJsonDocument::Compact)) > 0);
  malformedHistory.close();
  MediaDownloadService boundedRestore(outputDir, nullptr, {ytDlp}, {ffmpeg}, malformedHistoryPath);
  assert(boundedRestore.jobs().size() == 200);
  for (const MediaDownloadJob &job : boundedRestore.jobs()) {
    assert(job.thumbnailUrl == QStringLiteral("https://example.com/cover.jpg"));
  }

  std::cout << "media downloader URL, metadata, arguments, queue/cancel lifecycle and persistence: ok\n";
  return 0;
}
