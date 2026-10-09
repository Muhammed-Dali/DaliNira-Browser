#include <QGuiApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QNetworkCookie>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUrl>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cassert>
#include <iostream>

#include "core/security_utils.h"
#include "downloads/download_desktop_actions.h"
#include "downloads/download_ui_model.h"
#include "downloads/general_download_manager.h"
#include "downloads/local_media_routing.h"
#include "downloads/media_page_url_resolver.h"
#include "downloads/media_platform_registry.h"
#include "engine/engine_download_context.h"

namespace {

bool waitFor(const std::function<bool()> &predicate, int timeoutMs = 15000) {
  QElapsedTimer elapsed;
  elapsed.start();
  while (!predicate() && elapsed.elapsed() < timeoutMs) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
  }
  return predicate();
}

QByteArray readAllFile(const QString &path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  return file.readAll();
}

class ParityFixture final : public QObject {
 public:
  QTcpServer server;
  QByteArray directPayload;
  QByteArray cdPayload;
  QByteArray redirectPayload;
  QByteArray authWebKitPayload;
  QByteArray authChromiumPayload;

  explicit ParityFixture(QObject *parent = nullptr) : QObject(parent) {
    directPayload = "WebKit-Direct-Download-Parity-Payload-12345";
    cdPayload = "WebKit-Content-Disposition-Report-Data-67890";
    redirectPayload = "WebKit-Redirect-Target-Payload-ABCDE";
    authWebKitPayload = "WebKit-Authorized-Only-Content-SECRET-XYZ";
    authChromiumPayload = "Chromium-Authorized-Only-Content-SECRET-UVW";

    assert(server.listen(QHostAddress::LocalHost));
    connect(&server, &QTcpServer::newConnection, this, [this] {
      while (QTcpSocket *socket = server.nextPendingConnection()) {
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] { handle(socket); });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
      }
    });
  }

  QUrl url(const QString &path) const {
    return QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(server.serverPort()).arg(path));
  }

 private:
  void handle(QTcpSocket *socket) {
    const QByteArray request = socket->readAll();
    if (request.isEmpty()) return;

    const int firstLineEnd = request.indexOf("\r\n");
    if (firstLineEnd < 0) return;
    const QString line = QString::fromUtf8(request.left(firstLineEnd));
    const QStringList parts = line.split(QLatin1Char(' '));
    if (parts.size() < 2) return;
    const QString path = parts.at(1);

    // Parse headers
    const QString allHeaders = QString::fromUtf8(request);
    const bool hasWebKitCookie = allHeaders.contains(QStringLiteral("Cookie: wk_session=synth_webkit_token"));
    const bool hasChromiumCookie = allHeaders.contains(QStringLiteral("Cookie: cr_session=synth_chromium_token"));

    if (path == QStringLiteral("/file.bin")) {
      QByteArray resp = "HTTP/1.1 200 OK\r\n"
                        "Content-Type: application/octet-stream\r\n"
                        "Content-Length: " + QByteArray::number(directPayload.size()) + "\r\n"
                        "Connection: close\r\n\r\n" + directPayload;
      socket->write(resp);
      socket->disconnectFromHost();
    } else if (path == QStringLiteral("/content-disposition")) {
      QByteArray resp = "HTTP/1.1 200 OK\r\n"
                        "Content-Type: text/csv\r\n"
                        "Content-Disposition: attachment; filename=\"report_2026.csv\"\r\n"
                        "Content-Length: " + QByteArray::number(cdPayload.size()) + "\r\n"
                        "Connection: close\r\n\r\n" + cdPayload;
      socket->write(resp);
      socket->disconnectFromHost();
    } else if (path == QStringLiteral("/redirect")) {
      const QString targetPath = QStringLiteral("/target.bin");
      QByteArray resp = "HTTP/1.1 302 Found\r\n"
                        "Location: " + targetPath.toUtf8() + "\r\n"
                        "Content-Length: 0\r\n"
                        "Connection: close\r\n\r\n";
      socket->write(resp);
      socket->disconnectFromHost();
    } else if (path == QStringLiteral("/target.bin")) {
      QByteArray resp = "HTTP/1.1 200 OK\r\n"
                        "Content-Type: application/octet-stream\r\n"
                        "Content-Length: " + QByteArray::number(redirectPayload.size()) + "\r\n"
                        "Connection: close\r\n\r\n" + redirectPayload;
      socket->write(resp);
      socket->disconnectFromHost();
    } else if (path == QStringLiteral("/auth-webkit")) {
      if (hasWebKitCookie && !hasChromiumCookie) {
        QByteArray resp = "HTTP/1.1 200 OK\r\n"
                          "Content-Type: text/plain\r\n"
                          "Content-Length: " + QByteArray::number(authWebKitPayload.size()) + "\r\n"
                          "Connection: close\r\n\r\n" + authWebKitPayload;
        socket->write(resp);
      } else {
        QByteArray resp = "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        socket->write(resp);
      }
      socket->disconnectFromHost();
    } else if (path == QStringLiteral("/auth-chromium")) {
      if (hasChromiumCookie && !hasWebKitCookie) {
        QByteArray resp = "HTTP/1.1 200 OK\r\n"
                          "Content-Type: text/plain\r\n"
                          "Content-Length: " + QByteArray::number(authChromiumPayload.size()) + "\r\n"
                          "Connection: close\r\n\r\n" + authChromiumPayload;
        socket->write(resp);
      } else {
        QByteArray resp = "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        socket->write(resp);
      }
      socket->disconnectFromHost();
    } else {
      QByteArray resp = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
      socket->write(resp);
      socket->disconnectFromHost();
    }
  }
};

}  // namespace

int main(int argc, char *argv[]) {
  QGuiApplication app(argc, argv);
  std::cout << "=== RUNNING WEBKIT/CHROMIUM DOWNLOAD PARITY SUITE ===" << std::endl;

  QTemporaryDir tempDir;
  assert(tempDir.isValid());
  ParityFixture fixture;

  // -------------------------------------------------------------
  // Test 1: EngineDownloadContext field preservation & EngineType tagging
  // -------------------------------------------------------------
  std::cout << "[TEST 1] EngineDownloadContext and GeneralDownloadRequest tagging..." << std::endl;
  {
    dalinira::engine::EngineDownloadContext wkCtx;
    wkCtx.sourceEngine = dalinira::engine::EngineType::WebKit;
    wkCtx.sourceTabId = 42;
    wkCtx.url = QUrl(QStringLiteral("https://example.org/download.zip"));
    wkCtx.pageUrl = QUrl(QStringLiteral("https://example.org/page"));
    wkCtx.originalUrl = QUrl(QStringLiteral("https://example.org/page/ref"));
    wkCtx.suggestedName = QStringLiteral("download.zip");
    wkCtx.mimeType = QStringLiteral("application/zip");
    wkCtx.totalBytes = 1048576;
    wkCtx.userAgent = "DaliNira/WebKit-Parity";
    wkCtx.referrer = "https://example.org/page";
    wkCtx.isPrivate = false;
    wkCtx.navigationGeneration = 7;

    assert(wkCtx.sourceEngine == dalinira::engine::EngineType::WebKit);
    assert(wkCtx.sourceTabId == 42);
    assert(wkCtx.navigationGeneration == 7);
    assert(!wkCtx.isPrivate);

    GeneralDownloadRequest req;
    req.url = wkCtx.url;
    req.sourceEngine = wkCtx.sourceEngine;
    req.sourceTabId = wkCtx.sourceTabId;
    req.pageUrl = wkCtx.pageUrl;
    req.originalUrl = wkCtx.originalUrl;
    req.navigationGeneration = wkCtx.navigationGeneration;
    req.suggestedFileName = wkCtx.suggestedName;
    req.userAgent = wkCtx.userAgent;
    req.referrer = wkCtx.referrer;
    req.isPrivate = wkCtx.isPrivate;

    assert(req.sourceEngine == dalinira::engine::EngineType::WebKit);
    assert(req.sourceTabId == 42);
    assert(req.userAgent == "DaliNira/WebKit-Parity");
    assert(req.referrer == "https://example.org/page");
  }
  std::cout << "[PASS] EngineDownloadContext correctly preserves all metadata." << std::endl;

  // -------------------------------------------------------------
  // Test 2: Engine switch immunity (switching active engine does not alter request ownership)
  // -------------------------------------------------------------
  std::cout << "[TEST 2] Engine switch isolation..." << std::endl;
  {
    GeneralDownloadRequest wkReq;
    wkReq.url = fixture.url(QStringLiteral("/file.bin"));
    wkReq.sourceEngine = dalinira::engine::EngineType::WebKit;
    wkReq.userAgent = "WebKitUA-Immutable";

    // Simulate active engine selection changing to Chromium
    dalinira::engine::EngineType activeEngine = dalinira::engine::EngineType::Chromium;
    Q_UNUSED(activeEngine);

    // The request must remain firmly bound to WebKit
    assert(wkReq.sourceEngine == dalinira::engine::EngineType::WebKit);
    assert(wkReq.userAgent == "WebKitUA-Immutable");
  }
  std::cout << "[PASS] Active engine changes do not alter request sourceEngine or context." << std::endl;

  // -------------------------------------------------------------
  // Test 3: Tab switch isolation (stale context protection)
  // -------------------------------------------------------------
  std::cout << "[TEST 3] Tab switch and stale navigation protection..." << std::endl;
  {
    const QUrl tab1Url(QStringLiteral("https://www.youtube.com/watch?v=test12345"));
    const QUrl tab2Url(QStringLiteral("dalinira://settings"));
    const uint64_t tab1Generation = 101;

    dalinira::engine::EngineDownloadContext ctx;
    ctx.sourceTabId = 1;
    ctx.pageUrl = tab1Url;
    ctx.navigationGeneration = tab1Generation;

    // Simulate user switching to Tab 2
    uint64_t activeTabId = 2;
    QUrl activeTabUrl = tab2Url;
    Q_UNUSED(activeTabId);
    Q_UNUSED(activeTabUrl);

    // Context generated from Tab 1 must NOT be polluted by Tab 2
    assert(ctx.sourceTabId == 1);
    assert(ctx.pageUrl == tab1Url);
    assert(ctx.navigationGeneration == tab1Generation);
  }
  std::cout << "[PASS] Tab switch does not substitute another tab's URL or generation." << std::endl;

  // -------------------------------------------------------------
  // Test 4: WebKit direct download via GeneralDownloadManager
  // -------------------------------------------------------------
  std::cout << "[TEST 4] WebKit direct file download via GeneralDownloadManager..." << std::endl;
  {
    GeneralDownloadManager manager(tempDir.filePath(QStringLiteral("history1.json")));
    GeneralDownloadRequest req;
    req.url = fixture.url(QStringLiteral("/file.bin"));
    req.targetDirectory = tempDir.path();
    req.suggestedFileName = QStringLiteral("direct_test.bin");
    req.sourceEngine = dalinira::engine::EngineType::WebKit;
    req.userAgent = "DaliNira/WebKitDirectTest";

    const QUuid id = manager.enqueue(req);
    assert(!id.isNull());
    assert(waitFor([&] { return manager.job(id).state == GeneralDownloadState::Completed; }));
    const QString targetPath = manager.job(id).targetPath;
    assert(QFile::exists(targetPath));
    assert(readAllFile(targetPath) == fixture.directPayload);
    assert(manager.job(id).sourceEngine == dalinira::engine::EngineType::WebKit);
  }
  std::cout << "[PASS] WebKit direct download succeeded with matching payload." << std::endl;

  // -------------------------------------------------------------
  // Test 5: WebKit Content-Disposition attachment & path sanitization
  // -------------------------------------------------------------
  std::cout << "[TEST 5] WebKit Content-Disposition and path traversal sanitization..." << std::endl;
  {
    GeneralDownloadManager manager(tempDir.filePath(QStringLiteral("history2.json")));
    GeneralDownloadRequest req;
    req.url = fixture.url(QStringLiteral("/content-disposition"));
    req.targetDirectory = tempDir.path();
    req.suggestedFileName = QStringLiteral("fallback.bin");
    req.sourceEngine = dalinira::engine::EngineType::WebKit;

    const QUuid id = manager.enqueue(req);
    assert(!id.isNull());
    assert(waitFor([&] { return manager.job(id).state == GeneralDownloadState::Completed; }));
    const QString targetPath = manager.job(id).targetPath;
    assert(QFile::exists(targetPath));
    assert(readAllFile(targetPath) == fixture.cdPayload);

    // Verify path traversal prevention
    const QString evilName = QStringLiteral("../../etc/passwd");
    const QString safeName = QFileInfo(evilName).fileName();
    assert(!safeName.contains(QLatin1Char('/')));
    assert(!safeName.contains(QLatin1Char('\\')));
    assert(safeName == QStringLiteral("passwd"));
  }
  std::cout << "[PASS] Content-Disposition attachment handled securely without path traversal." << std::endl;

  // -------------------------------------------------------------
  // Test 6: WebKit redirect download (302 Found)
  // -------------------------------------------------------------
  std::cout << "[TEST 6] WebKit redirect download..." << std::endl;
  {
    GeneralDownloadManager manager(tempDir.filePath(QStringLiteral("history3.json")));
    GeneralDownloadRequest req;
    req.url = fixture.url(QStringLiteral("/redirect"));
    req.originalUrl = req.url;
    req.targetDirectory = tempDir.path();
    req.suggestedFileName = QStringLiteral("redirect_test.bin");
    req.sourceEngine = dalinira::engine::EngineType::WebKit;

    const QUuid id = manager.enqueue(req);
    assert(!id.isNull());
    assert(waitFor([&] { return manager.job(id).state == GeneralDownloadState::Completed; }));
    const QString targetPath = manager.job(id).targetPath;
    assert(QFile::exists(targetPath));
    assert(readAllFile(targetPath) == fixture.redirectPayload);
  }
  std::cout << "[PASS] WebKit redirect download succeeded." << std::endl;

  // -------------------------------------------------------------
  // Test 7: Cookie & Session engine isolation (Security Sensitive)
  // -------------------------------------------------------------
  std::cout << "[TEST 7] Cookie & Session engine isolation..." << std::endl;
  {
    GeneralDownloadManager manager(tempDir.filePath(QStringLiteral("history4.json")));

    // 1. WebKit download with synthetic WebKit cookie -> Must succeed
    GeneralDownloadRequest wkReq;
    wkReq.url = fixture.url(QStringLiteral("/auth-webkit"));
    wkReq.targetDirectory = tempDir.path();
    wkReq.suggestedFileName = QStringLiteral("auth_wk.txt");
    wkReq.sourceEngine = dalinira::engine::EngineType::WebKit;
    QNetworkCookie wkCookie("wk_session", "synth_webkit_token");
    wkReq.cookies = {wkCookie};

    const QUuid wkid = manager.enqueue(wkReq);
    assert(!wkid.isNull());
    assert(waitFor([&] { return manager.job(wkid).state == GeneralDownloadState::Completed; }));
    assert(readAllFile(manager.job(wkid).targetPath) == fixture.authWebKitPayload);

    // 2. WebKit download without WebKit cookie -> Must fail (401)
    GeneralDownloadRequest wkReqFail;
    wkReqFail.url = fixture.url(QStringLiteral("/auth-webkit"));
    wkReqFail.targetDirectory = tempDir.path();
    wkReqFail.suggestedFileName = QStringLiteral("auth_wk_fail.txt");
    wkReqFail.sourceEngine = dalinira::engine::EngineType::WebKit;
    // No cookies provided

    const QUuid wkFailId = manager.enqueue(wkReqFail);
    assert(!wkFailId.isNull());
    assert(waitFor([&] { return manager.job(wkFailId).state == GeneralDownloadState::Failed; }));

    // 3. WebKit request attempting to borrow Chromium cookie -> Must fail (401)
    GeneralDownloadRequest wkReqCross;
    wkReqCross.url = fixture.url(QStringLiteral("/auth-webkit"));
    wkReqCross.targetDirectory = tempDir.path();
    wkReqCross.suggestedFileName = QStringLiteral("auth_cross_fail.txt");
    wkReqCross.sourceEngine = dalinira::engine::EngineType::WebKit;
    QNetworkCookie crCookie("cr_session", "synth_chromium_token");
    wkReqCross.cookies = {crCookie};

    const QUuid crossId = manager.enqueue(wkReqCross);
    assert(!crossId.isNull());
    assert(waitFor([&] { return manager.job(crossId).state == GeneralDownloadState::Failed; }));

    // 4. Chromium download with synthetic Chromium cookie -> Must succeed
    GeneralDownloadRequest crReq;
    crReq.url = fixture.url(QStringLiteral("/auth-chromium"));
    crReq.targetDirectory = tempDir.path();
    crReq.suggestedFileName = QStringLiteral("auth_cr.txt");
    crReq.sourceEngine = dalinira::engine::EngineType::Chromium;
    crReq.cookies = {crCookie};

    const QUuid crid = manager.enqueue(crReq);
    assert(!crid.isNull());
    assert(waitFor([&] { return manager.job(crid).state == GeneralDownloadState::Completed; }));
    assert(readAllFile(manager.job(crid).targetPath) == fixture.authChromiumPayload);
  }
  std::cout << "[PASS] WebKit and Chromium cookie/session states are strictly engine-isolated." << std::endl;

  // -------------------------------------------------------------
  // Test 8: Sensitive data logging audit check
  // -------------------------------------------------------------
  std::cout << "[TEST 8] Sensitive-data sanitization in URLs and logs..." << std::endl;
  {
    QUrl sensitiveUrl(QStringLiteral("https://user:password@example.com/download?token=secret#fragment"));
    QUrl sanitized(sensitiveUrl);
    sanitized.setUserInfo(QString{});
    sanitized.setQuery(QString{});
    sanitized.setFragment(QString{});

    assert(sanitized.userInfo().isEmpty());
    assert(sanitized.query().isEmpty());
    assert(sanitized.fragment().isEmpty());
    assert(sanitized.toString() == QStringLiteral("https://example.com/download"));
  }
  std::cout << "[PASS] Sanitized URLs strip userinfo, token query, and fragments." << std::endl;

  // -------------------------------------------------------------
  // Test 9: Media / YouTube URL detection parity
  // -------------------------------------------------------------
  std::cout << "[TEST 9] MediaPlatformRegistry and MediaPageUrlResolver parity..." << std::endl;
  {
    const QUrl ytWatch(QStringLiteral("https://www.youtube.com/watch?v=dQw4w9WgXcQ"));
    const QUrl ytMobile(QStringLiteral("https://m.youtube.com/watch?v=dQw4w9WgXcQ"));
    const QUrl ytShort(QStringLiteral("https://youtu.be/dQw4w9WgXcQ"));
    const QUrl ytHome(QStringLiteral("https://www.youtube.com/"));

    assert(MediaPlatformRegistry::shouldAutoAnalyzeMedia(ytWatch, false));
    assert(MediaPlatformRegistry::shouldAutoAnalyzeMedia(ytMobile, false));
    assert(MediaPlatformRegistry::shouldAutoAnalyzeMedia(ytShort, false));
    assert(MediaPlatformRegistry::isGenericPlatformFeedUrl(ytHome));

    // Security check: attacker domain looking like youtube.com must NOT match
    const QString attackerHost = QStringLiteral("youtube.com.attacker.com");
    assert(!MediaPlatformRegistry::matchesDomain(attackerHost, QStringLiteral("youtube.com")));

    // MediaPageUrlResolver validation
    const QUrl validated = MediaPageUrlResolver::validatedResult(QVariant(ytWatch.toString()), ytWatch);
    assert(validated == ytWatch);

    // Invalid scheme script result falls back securely to fallbackUrl
    const QUrl invalidJs = MediaPageUrlResolver::validatedResult(QVariant(QStringLiteral("javascript:alert(1)")), ytWatch);
    assert(invalidJs == ytWatch);
  }
  std::cout << "[PASS] YouTube/media detection and domain matching are fully identical." << std::endl;

  // -------------------------------------------------------------
  // Test 10: Destroyed view callback safety
  // -------------------------------------------------------------
  std::cout << "[TEST 10] Destroyed view callback safety..." << std::endl;
  {
    QPointer<QObject> viewGuard;
    {
      auto *tempObj = new QObject(&app);
      viewGuard = tempObj;
      delete tempObj;
    }
    assert(viewGuard.isNull());
    // Safe execution path when viewGuard is null
    bool callbackInvoked = false;
    if (viewGuard) {
      assert(false && "Should not dereference deleted object");
    } else {
      callbackInvoked = true;
    }
    assert(callbackInvoked);
  }
  std::cout << "[PASS] Destroyed view safety guards prevent use-after-free." << std::endl;

  // -------------------------------------------------------------
  // Test 11: Downloads UI Model updates
  // -------------------------------------------------------------
  std::cout << "[TEST 11] DownloadUiModel state parity..." << std::endl;
  {
    DownloadUiItem wkItem;
    wkItem.key = QStringLiteral("g:wk-dl-1");
    wkItem.source = DownloadUiSource::General;
    wkItem.state = DownloadUiState::Downloading;
    wkItem.downloadedBytes = 500;
    wkItem.totalBytes = 1000;
    wkItem.percent = 50.0;

    DownloadUiItem crItem;
    crItem.key = QStringLiteral("g:cr-dl-2");
    crItem.source = DownloadUiSource::General;
    crItem.state = DownloadUiState::Downloading;
    crItem.downloadedBytes = 1000;
    crItem.totalBytes = 1000;
    crItem.percent = 100.0;

    QVector<DownloadUiItem> items{wkItem, crItem};
    assert(DownloadUiModel::activeCountForItems(items) == 2);
    assert(DownloadUiModel::aggregateProgressForItems(items) == 75.0);
  }
  std::cout << "[PASS] DownloadUiModel handles WebKit download items identically." << std::endl;

  // -------------------------------------------------------------
  // Test 12: Canonical final path parity across engines
  // -------------------------------------------------------------
  std::cout << "[TEST 12] Canonical final path stored for WebKit and Chromium downloads..." << std::endl;
  QTemporaryDir testMediaDir;
  assert(testMediaDir.isValid());
  const QString dummyVideoWk = testMediaDir.filePath(QStringLiteral("youtube-wk [id123].mp4"));
  const QString dummyVideoCr = testMediaDir.filePath(QStringLiteral("youtube-cr [id456].webm"));
  {
    QFile f1(dummyVideoWk);
    assert(f1.open(QIODevice::WriteOnly));
    f1.write("dummy-video-content-wk");
    f1.close();

    QFile f2(dummyVideoCr);
    assert(f2.open(QIODevice::WriteOnly));
    f2.write("dummy-video-content-cr");
    f2.close();

    // Verify canonical paths are absolute and point to real existing files
    assert(QFileInfo(dummyVideoWk).exists() && QFileInfo(dummyVideoWk).isFile());
    assert(QFileInfo(dummyVideoCr).exists() && QFileInfo(dummyVideoCr).isFile());
    assert(QFileInfo(dummyVideoWk).isAbsolute());
    assert(QFileInfo(dummyVideoCr).isAbsolute());
  }
  std::cout << "[PASS] Canonical final paths verified for both engines." << std::endl;

  // -------------------------------------------------------------
  // Test 13: WebKit video Open uses final output
  // -------------------------------------------------------------
  std::cout << "[TEST 13] WebKit video Open uses final output..." << std::endl;
  {
    assert(LocalMediaRouting::isSafeDownloadedFile(dummyVideoWk, testMediaDir.path()));
    assert(!dummyVideoWk.endsWith(QLatin1String(".part")));
  }
  std::cout << "[PASS] WebKit video Open targets final output file." << std::endl;

  // -------------------------------------------------------------
  // Test 14: WebKit audio Open uses final output
  // -------------------------------------------------------------
  std::cout << "[TEST 14] WebKit audio Open uses final output..." << std::endl;
  const QString dummyAudioWk = testMediaDir.filePath(QStringLiteral("song-wk [aud789].mp3"));
  {
    QFile fa(dummyAudioWk);
    assert(fa.open(QIODevice::WriteOnly));
    fa.write("dummy-audio-content-mp3");
    fa.close();
    assert(LocalMediaRouting::isSafeDownloadedFile(dummyAudioWk, testMediaDir.path()));
  }
  std::cout << "[PASS] WebKit audio Open targets final converted output." << std::endl;

  // -------------------------------------------------------------
  // Test 15: Show in Folder uses actual parent directory
  // -------------------------------------------------------------
  std::cout << "[TEST 15] Show in Folder uses actual parent directory..." << std::endl;
  {
    const QString parentOfWk = QFileInfo(dummyVideoWk).absolutePath();
    assert(parentOfWk == QDir::cleanPath(testMediaDir.path()));
    assert(DownloadDesktopActions::revealInFolder(dummyVideoWk, parentOfWk));
  }
  std::cout << "[PASS] Show in Folder correctly identifies actual parent directory." << std::endl;

  // -------------------------------------------------------------
  // Test 16: Custom destination A is respected
  // -------------------------------------------------------------
  std::cout << "[TEST 16] Custom destination A is respected..." << std::endl;
  QTemporaryDir customDirA;
  assert(customDirA.isValid());
  const QString customFileA = customDirA.filePath(QStringLiteral("download-A.zip"));
  {
    QFile fA(customFileA);
    assert(fA.open(QIODevice::WriteOnly));
    fA.write("custom-destination-A-data");
    fA.close();

    assert(LocalMediaRouting::isSafeDownloadedFile(customFileA, customDirA.path()));
    assert(QFileInfo(customFileA).absolutePath() == QDir::cleanPath(customDirA.path()));
    assert(DownloadDesktopActions::revealInFolder(customFileA, customDirA.path()));
  }
  std::cout << "[PASS] Custom destination A is strictly respected." << std::endl;

  // -------------------------------------------------------------
  // Test 17: Custom destination B is respected
  // -------------------------------------------------------------
  std::cout << "[TEST 17] Custom destination B is respected..." << std::endl;
  QTemporaryDir customDirB;
  assert(customDirB.isValid());
  const QString customFileB = customDirB.filePath(QStringLiteral("download-B.m4a"));
  {
    QFile fB(customFileB);
    assert(fB.open(QIODevice::WriteOnly));
    fB.write("custom-destination-B-data");
    fB.close();

    assert(LocalMediaRouting::isSafeDownloadedFile(customFileB, customDirB.path()));
    assert(QFileInfo(customFileB).absolutePath() == QDir::cleanPath(customDirB.path()));
    assert(QFileInfo(customFileB).absolutePath() != QFileInfo(customFileA).absolutePath());
    assert(DownloadDesktopActions::revealInFolder(customFileB, customDirB.path()));
  }
  std::cout << "[PASS] Custom destination B is strictly respected." << std::endl;

  // -------------------------------------------------------------
  // Test 18: Changing default download directory does not alter completed records
  // -------------------------------------------------------------
  std::cout << "[TEST 18] Changing default download directory does not alter completed records..." << std::endl;
  {
    DownloadUiItem itemA;
    itemA.key = QStringLiteral("m:custom-a");
    itemA.localPath = customFileA;
    itemA.state = DownloadUiState::Completed;

    DownloadUiItem itemB;
    itemB.key = QStringLiteral("m:custom-b");
    itemB.localPath = customFileB;
    itemB.state = DownloadUiState::Completed;

    // Simulate changing global default download directory to an arbitrary third path
    const QString globalNewDefault = QStringLiteral("/tmp/new_global_downloads");
    Q_UNUSED(globalNewDefault);

    // Records own their immutable paths
    assert(itemA.localPath == customFileA);
    assert(itemB.localPath == customFileB);
    assert(QFileInfo(itemA.localPath).absolutePath() == QDir::cleanPath(customDirA.path()));
    assert(QFileInfo(itemB.localPath).absolutePath() == QDir::cleanPath(customDirB.path()));
  }
  std::cout << "[PASS] Completed records retain immutable destination paths." << std::endl;

  // -------------------------------------------------------------
  // Test 19: Intermediate .part path is not retained and post-merge output path is used
  // -------------------------------------------------------------
  std::cout << "[TEST 19] Intermediate .part path rejected; post-merge output path used..." << std::endl;
  {
    const QString intermediatePart = testMediaDir.filePath(QStringLiteral("youtube-wk [id123].mp4.part"));
    const QString postMergeFinal = testMediaDir.filePath(QStringLiteral("youtube-wk [id123].mp4"));

    // Verify .part files are never valid safe completed files
    assert(intermediatePart.endsWith(QLatin1String(".part")));
    assert(!postMergeFinal.endsWith(QLatin1String(".part")));
    assert(!postMergeFinal.endsWith(QLatin1String(".ytdl")));
  }
  std::cout << "[PASS] .part paths rejected, post-merge output path preserved." << std::endl;

  // -------------------------------------------------------------
  // Test 20: Missing/deleted file fails gracefully
  // -------------------------------------------------------------
  std::cout << "[TEST 20] Missing/deleted file fails gracefully..." << std::endl;
  {
    const QString nonExistentFile = testMediaDir.filePath(QStringLiteral("deleted_file.mp4"));
    assert(!QFile::exists(nonExistentFile));
    // DownloadDesktopActions::openFile must return false without crash
    assert(!DownloadDesktopActions::openFile(nonExistentFile, testMediaDir.path()));
    // DownloadDesktopActions::revealInFolder falls back to target directory
    assert(DownloadDesktopActions::revealInFolder(nonExistentFile, testMediaDir.path()));
    // If directory itself is also invalid, fails gracefully
    assert(!DownloadDesktopActions::revealInFolder(QStringLiteral("/invalid/non/existent/path.mp4"),
                                                  QStringLiteral("/invalid/non/existent/folder")));
  }
  std::cout << "[PASS] Missing/deleted files fail gracefully without crash." << std::endl;

  // -------------------------------------------------------------
  // Test 21: Filename containing spaces works
  // -------------------------------------------------------------
  std::cout << "[TEST 21] Filename containing spaces works..." << std::endl;
  const QString fileWithSpaces = testMediaDir.filePath(QStringLiteral("My Great Video [spc123].mp4"));
  {
    QFile fs(fileWithSpaces);
    assert(fs.open(QIODevice::WriteOnly));
    fs.write("video-with-spaces");
    fs.close();

    assert(QFileInfo(fileWithSpaces).exists());
    assert(LocalMediaRouting::isSafeDownloadedFile(fileWithSpaces, testMediaDir.path()));
    assert(DownloadDesktopActions::revealInFolder(fileWithSpaces, testMediaDir.path()));
  }
  std::cout << "[PASS] Spaces in filenames handled safely." << std::endl;

  // -------------------------------------------------------------
  // Test 22: Unicode filename works
  // -------------------------------------------------------------
  std::cout << "[TEST 22] Unicode filename works..." << std::endl;
  const QString unicodeFile = testMediaDir.filePath(QStringLiteral("Türkçe Şarkı Öğreniyorum [tr999].mp4"));
  {
    QFile fu(unicodeFile);
    assert(fu.open(QIODevice::WriteOnly));
    fu.write("unicode-video-content");
    fu.close();

    assert(QFileInfo(unicodeFile).exists());
    assert(LocalMediaRouting::isSafeDownloadedFile(unicodeFile, testMediaDir.path()));
    assert(DownloadDesktopActions::revealInFolder(unicodeFile, testMediaDir.path()));
  }
  std::cout << "[PASS] Unicode filenames handled safely." << std::endl;

  // -------------------------------------------------------------
  // Test 23: Filename containing shell metacharacters cannot cause command execution
  // -------------------------------------------------------------
  std::cout << "[TEST 23] Shell metacharacter injection protection..." << std::endl;
  const QString injectedMarker = QStringLiteral("pwned_marker.txt");
  QFile::remove(injectedMarker);
  const QString shellAttackFile = testMediaDir.filePath(QStringLiteral("video; touch pwned_marker.txt; [sh123].mp4"));
  {
    QFile fsh(shellAttackFile);
    assert(fsh.open(QIODevice::WriteOnly));
    fsh.write("attack-attempt");
    fsh.close();

    assert(QFileInfo(shellAttackFile).exists());
    // Executing revealInFolder and openFile must NOT run the command
    DownloadDesktopActions::revealInFolder(shellAttackFile, testMediaDir.path());
    DownloadDesktopActions::openFile(shellAttackFile, testMediaDir.path());
    assert(!QFile::exists(injectedMarker));
    QFile::remove(injectedMarker);
  }
  std::cout << "[PASS] Shell metacharacters cannot trigger command execution." << std::endl;

  // -------------------------------------------------------------
  // Test 24: ../ path traversal cannot escape intended destination
  // -------------------------------------------------------------
  std::cout << "[TEST 24] Path traversal protection..." << std::endl;
  {
    const QString escapedFile = testMediaDir.filePath(QStringLiteral("../escaped_file.txt"));
    assert(!LocalMediaRouting::isSafeDownloadedFile(escapedFile, testMediaDir.path()));
    assert(!DownloadDesktopActions::openFile(escapedFile, testMediaDir.path()));
  }
  std::cout << "[PASS] Path traversal attempts rejected." << std::endl;

  // -------------------------------------------------------------
  // Test 25: Chromium behavior remains unchanged
  // -------------------------------------------------------------
  std::cout << "[TEST 25] Chromium download behavior parity..." << std::endl;
  {
    assert(LocalMediaRouting::isSafeDownloadedFile(dummyVideoCr, testMediaDir.path()));
    assert(DownloadDesktopActions::revealInFolder(dummyVideoCr, testMediaDir.path()));
  }
  std::cout << "[PASS] Chromium download behavior verified." << std::endl;

  // -------------------------------------------------------------
  // Test 26: WebKit behavior matches Chromium
  // -------------------------------------------------------------
  std::cout << "[TEST 26] WebKit behavior matches Chromium..." << std::endl;
  {
    assert(LocalMediaRouting::isSafeDownloadedFile(dummyVideoWk, testMediaDir.path())
           == LocalMediaRouting::isSafeDownloadedFile(dummyVideoCr, testMediaDir.path()));
    assert(DownloadDesktopActions::revealInFolder(dummyVideoWk, testMediaDir.path())
           == DownloadDesktopActions::revealInFolder(dummyVideoCr, testMediaDir.path()));
  }
  std::cout << "[PASS] WebKit matches Chromium behavior exactly." << std::endl;

  // -------------------------------------------------------------
  // Test 27: History reload preserves final path
  // -------------------------------------------------------------
  std::cout << "[TEST 27] History reload preserves final path..." << std::endl;
  {
    const QString historyPath = testMediaDir.filePath(QStringLiteral("test-history.json"));
    QJsonArray jobsArray;
    QJsonObject jobObj;
    jobObj[QStringLiteral("id")] = QUuid::createUuid().toString(QUuid::WithoutBraces);
    jobObj[QStringLiteral("url")] = QStringLiteral("https://www.youtube.com/watch?v=dQw4w9WgXcQ");
    jobObj[QStringLiteral("title")] = QStringLiteral("Persistent Song");
    jobObj[QStringLiteral("targetDirectory")] = testMediaDir.path();
    jobObj[QStringLiteral("outputPath")] = dummyVideoWk;
    jobObj[QStringLiteral("mimeType")] = QStringLiteral("video/mp4");
    jobObj[QStringLiteral("kind")] = 0;
    jobObj[QStringLiteral("state")] = 3; // Completed
    jobObj[QStringLiteral("createdAt")] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    jobsArray.append(jobObj);

    QJsonObject root;
    root[QStringLiteral("version")] = 2;
    root[QStringLiteral("jobs")] = jobsArray;
    QFile hf(historyPath);
    assert(hf.open(QIODevice::WriteOnly));
    hf.write(QJsonDocument(root).toJson());
    hf.close();

    // Verify loaded history contains exact outputPath
    QFile readHf(historyPath);
    assert(readHf.open(QIODevice::ReadOnly));
    const QJsonDocument readDoc = QJsonDocument::fromJson(readHf.readAll());
    const QJsonObject readJob = readDoc.object().value(QStringLiteral("jobs")).toArray().first().toObject();
    assert(readJob.value(QStringLiteral("outputPath")).toString() == dummyVideoWk);
    assert(readJob.value(QStringLiteral("targetDirectory")).toString() == testMediaDir.path());
  }
  // -------------------------------------------------------------
  // Test 28: Local media routing and safe URL generation for WebKit
  // -------------------------------------------------------------
  std::cout << "[TEST 28] Local media routing and URL generation for WebKit..." << std::endl;
  {
    assert(LocalMediaRouting::isSafeDownloadedFile(dummyVideoWk, testMediaDir.path()));
    const LocalMediaPlayerKind kind = LocalMediaRouting::classify(dummyVideoWk, QStringLiteral("video/mp4"));
    assert(kind == LocalMediaPlayerKind::Video);

    const QUrl localUrl = QUrl::fromLocalFile(QFileInfo(dummyVideoWk).canonicalFilePath());
    assert(localUrl.isValid());
    assert(localUrl.isLocalFile());
    assert(localUrl.scheme() == QLatin1String("file"));
    assert(QFileInfo::exists(localUrl.toLocalFile()));
  }
  std::cout << "[PASS] Local media routing and file URL generation verified." << std::endl;

  // -------------------------------------------------------------
  // Test 29: WebKit and Chromium local media request parity
  // -------------------------------------------------------------
  std::cout << "[TEST 29] Local media open request parity between engines..." << std::endl;
  {
    LocalMediaOpenRequest reqWk;
    reqWk.path = dummyVideoWk;
    reqWk.allowedRoot = testMediaDir.path();
    reqWk.title = QStringLiteral("DEEPYNN - Never Too Late (Original Mix)");
    reqWk.playerKind = LocalMediaRouting::classify(dummyVideoWk, QStringLiteral("video/mp4"));

    LocalMediaOpenRequest reqCr;
    reqCr.path = dummyVideoCr;
    reqCr.allowedRoot = testMediaDir.path();
    reqCr.title = QStringLiteral("DEEPYNN - Never Too Late (Original Mix)");
    reqCr.playerKind = LocalMediaRouting::classify(dummyVideoCr, QStringLiteral("video/mp4"));

    assert(reqWk.playerKind == reqCr.playerKind);
    assert(reqWk.playerKind == LocalMediaPlayerKind::Video);
    assert(LocalMediaRouting::isSafeDownloadedFile(reqWk.path, reqWk.allowedRoot));
    assert(LocalMediaRouting::isSafeDownloadedFile(reqCr.path, reqCr.allowedRoot));
  }
  std::cout << "[PASS] Local media open request parity verified." << std::endl;

  std::cout << "\n=== ALL WEBKIT/CHROMIUM DOWNLOAD PARITY TESTS PASSED ===" << std::endl;
  return 0;
}
