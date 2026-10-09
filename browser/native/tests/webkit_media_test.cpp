// WebKit media runtime test.
//
// Verifies that DaliNira detects the GStreamer runtime WebKitGTK needs, and,
// when that runtime is installed, that real HTML5 media works end to end in a
// DaliNira WebKitEngineView: decode + audio output, pause/resume, seeking,
// Media Source Extensions (YouTube's delivery path), hiding the tab during
// playback, closing a playing tab (its audio stream and web process are
// released; a page with a service worker leaves an idle process that WebKit
// ends after its worker delay) and shutting down during playback.
//
// GUI tier (scripts/safe-gui-test.sh): one window, one view at a time
// (webkit_gui_harness.h). Opt-in extras:
//   DALINIRA_GUI_MANUAL=1        fullscreen enter/exit (goes through the window manager)
//   DALINIRA_LIVE_MEDIA_TEST=1   live YouTube playback, pause, seek, tab switch, close
//
// The fixture is VP9 video + Opus audio in WebM, YouTube's default formats.

#include <QApplication>
#include <QFile>
#include <QProcess>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <cmath>
#include <iostream>
#include <memory>

#include "engine/engine_backend.h"
#include "engine/engine_storage_manager.h"
#include "engine/webkit/webkit_backend.h"
#include "engine/webkit/webkit_engine_view.h"
#include "engine/webkit/webkit_media_runtime.h"
#include "webkit_gui_harness.h"

#undef signals
#include <webkit2/webkit2.h>

using dalinira::engine::EngineView;
using dalinira::engine::WebKitEngineView;
using dalinira::engine::WebKitMediaRuntime;
using dalinira::test::WebKitGuiHarness;

namespace {

void testRuntimeEvaluation() {
  std::cout << "[TEST 1] Media runtime requirement evaluation..." << std::endl;
  const auto without = [](std::initializer_list<const char *> absent) {
    return WebKitMediaRuntime::evaluate([absent](const char *element) {
      for (const char *name : absent)
        if (qstrcmp(element, name) == 0) return false;
      return true;
    });
  };
  // Everything present: playback is available and nothing is reported missing.
  const auto all = without({});
  WK_CHECK(all.playbackAvailable && all.missing.isEmpty());

  // Without an audio sink WebKitGTK aborts the web process, so media must be
  // reported unavailable, naming the element and the package that provides it.
  const auto noSink = without({"autoaudiosink"});
  WK_CHECK(!noSink.playbackAvailable);
  WK_CHECK(noSink.missing.size() == 1);
  WK_CHECK(noSink.missing.first().element == QLatin1String("autoaudiosink"));
  WK_CHECK(noSink.missing.first().plugin == QLatin1String("autodetect"));
  WK_CHECK(noSink.missing.first().module == QLatin1String("gst-plugins-good"));

  // A missing demuxer does not crash WebKit (the type is simply unsupported),
  // but it breaks YouTube, so it is reported while playback stays enabled.
  const auto noDemuxer = without({"matroskademux"});
  WK_CHECK(noDemuxer.playbackAvailable);
  WK_CHECK(noDemuxer.missing.size() == 1 && noDemuxer.missing.first().plugin == QLatin1String("matroska"));

  // MSE only advertises Opus when an Opus parser exists (gst-plugins-bad).
  const auto noOpusParse = without({"opusparse"});
  WK_CHECK(noOpusParse.playbackAvailable);
  WK_CHECK(noOpusParse.missing.size() == 1 && noOpusParse.missing.first().module == QLatin1String("gst-plugins-bad"));

  // Either VP9 decoder is enough (libvpx from -good or FFmpeg from gst-libav).
  WK_CHECK(without({"vp9dec"}).missing.isEmpty());
  const auto noVp9 = without({"vp9dec", "avdec_vp9"});
  WK_CHECK(noVp9.missing.size() == 1 && noVp9.missing.first().element == QLatin1String("vp9dec"));
  std::cout << "[PASS] Requirement evaluation distinguishes fatal and non-fatal gaps." << std::endl;
}

double number(WebKitGuiHarness &h, EngineView *view, const QString &expression) {
  bool ok = false;
  const double value = h.js(view, expression, 5000).toDouble(&ok);
  return ok ? value : std::nan("");
}

// Serves the page and the fixture over HTTP with byte ranges, the way media
// reaches WebKit from real sites (network process + GStreamer web source).
class MediaSite {
 public:
  MediaSite(const QByteArray &clip, const QByteArray &page, const QByteArray &serviceWorkerPage)
      : clip_(clip), page_(page), serviceWorkerPage_(serviceWorkerPage) {
    WK_CHECK(server_.listen(QHostAddress::LocalHost));
    QObject::connect(&server_, &QTcpServer::newConnection, [this] {
      while (QTcpSocket *socket = server_.nextPendingConnection()) {
        auto buffer = std::make_shared<QByteArray>();
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer] { serve(socket, *buffer); });
        QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
      }
    });
  }
  QUrl pageUrl() const { return url(QStringLiteral("/index.html")); }
  // The media page, plus a registered service worker (as on YouTube).
  QUrl serviceWorkerPageUrl() const { return url(QStringLiteral("/sw.html")); }
  QUrl plainPageUrl() const { return url(QStringLiteral("/plain.html")); }
  QUrl url(const QString &path) const { return QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(server_.serverPort()).arg(path)); }

 private:
  // Answers every complete request in the buffer: WebKit reuses keep-alive
  // connections, so a range request (e.g. restarting an ended clip) can
  // arrive in the same read as the previous one.
  void serve(QTcpSocket *socket, QByteArray &buffer) {
    buffer.append(socket->readAll());
    for (int end = buffer.indexOf("\r\n\r\n"); end >= 0; end = buffer.indexOf("\r\n\r\n")) {
      const QByteArray head = buffer.left(end);
      buffer.remove(0, end + 4);
      respond(socket, head);
    }
  }

  void respond(QTcpSocket *socket, const QByteArray &head) {
    if (head.startsWith("GET /clip.webm")) {
      const int rangeAt = head.toLower().indexOf("range: ");
      std::cout << "  [server] clip request #" << ++clipRequests_ << " on socket " << socket
                << (rangeAt >= 0 ? " " + head.mid(rangeAt, head.indexOf('\r', rangeAt) - rangeAt).toStdString() : std::string(" (no range)"))
                << std::endl;
    }
    const bool isClip = head.startsWith("GET /clip.webm");
    static const QByteArray serviceWorker =
        "self.addEventListener('install',e=>self.skipWaiting());"
        "self.addEventListener('activate',e=>e.waitUntil(clients.claim()));"
        "self.addEventListener('fetch',()=>{});";
    static const QByteArray plain = "<!doctype html><title>plain</title><p id=content>plain page</p>";
    QByteArray type = isClip ? "video/webm" : "text/html";
    const QByteArray *bodyPtr = isClip ? &clip_ : &page_;
    if (head.startsWith("GET /sw.js")) {
      bodyPtr = &serviceWorker;
      type = "text/javascript";
    } else if (head.startsWith("GET /sw.html")) {
      bodyPtr = &serviceWorkerPage_;
    } else if (head.startsWith("GET /plain.html")) {
      bodyPtr = &plain;
    }
    const QByteArray &body = *bodyPtr;
    qint64 first = 0;
    qint64 last = body.size() - 1;
    bool partial = false;
    const int rangeAt = head.toLower().indexOf("range: bytes=");
    if (isClip && rangeAt >= 0) {
      const QList<QByteArray> bounds = head.mid(rangeAt + 13).split('\r').first().split('-');
      first = bounds.value(0).toLongLong();
      if (bounds.size() > 1 && !bounds.at(1).isEmpty()) last = qMin(last, bounds.at(1).toLongLong());
      partial = true;
    }
    QByteArray response = partial ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n";
    response += "Content-Type: " + type +
                "\r\nAccept-Ranges: bytes\r\nContent-Length: " + QByteArray::number(last - first + 1) + "\r\n";
    if (partial)
      response += "Content-Range: bytes " + QByteArray::number(first) + "-" + QByteArray::number(last) + "/" +
                  QByteArray::number(body.size()) + "\r\n";
    response += "\r\n" + body.mid(first, last - first + 1);
    socket->write(response);
  }

  QTcpServer server_;
  QByteArray clip_;
  QByteArray page_;
  QByteArray serviceWorkerPage_;
  int clipRequests_ = 0;
};

// Pids that currently own a stream (sink input) on the sound server.
QList<int> soundServerStreamPids() {
  QProcess pactl;
  pactl.start(QStringLiteral("pactl"), {QStringLiteral("list"), QStringLiteral("sink-inputs")});
  QList<int> pids;
  if (!pactl.waitForStarted(2000) || !pactl.waitForFinished(3000)) return pids;
  for (const QByteArray &line : pactl.readAllStandardOutput().split('\n')) {
    const int at = line.indexOf("application.process.id = \"");
    if (at >= 0) pids.append(line.mid(at + 26).split('"').value(0).toInt());
  }
  return pids;
}

// A closed tab must not keep playing: none of its processes owns a stream.
void checkAudioReleased(const QList<int> &pids, const char *what) {
  const QList<int> streams = soundServerStreamPids();
  bool released = true;
  for (int pid : pids) released = released && !streams.contains(pid);
  std::cout << "  " << what << ": sound server streams from the closed tab's process: " << (released ? "none" : "STILL OPEN")
            << std::endl;
  WK_CHECK(released && "a closed tab's media must stop");
}

void liveYouTube(WebKitGuiHarness &h, dalinira::engine::WebKitBackend &backend) {
  std::cout << "[LIVE] YouTube playback..." << std::endl;
  auto *yt = h.attach(qobject_cast<WebKitEngineView *>(backend.createView(h.stack())), "live YouTube");
  yt->load(QUrl(QStringLiteral("https://www.youtube.com/watch?v=aqz-KE-bpKQ")));
  const QString videoJs = QStringLiteral("document.querySelector('video.html5-main-video')||document.querySelector('video')");
  const bool started = h.waitUntil([&] {
    return number(h, yt, QStringLiteral("(()=>{const v=%1;return v?v.currentTime:-1})()").arg(videoJs)) > 2.0;
  }, 45000);
  const auto state = [&] {
    return h.js(yt, QStringLiteral(
        "(()=>{const v=%1;if(!v)return 'no video';const p=document.querySelector('.html5-video-player');"
        "return `t=${v.currentTime.toFixed(1)} paused=${v.paused} src=${(v.currentSrc||'').slice(0,5)} "
        "size=${v.videoWidth}x${v.videoHeight} muted=${v.muted} ad=${p?p.classList.contains('ad-showing'):'?'} "
        "frames=${v.getVideoPlaybackQuality().totalVideoFrames} dropped=${v.getVideoPlaybackQuality().droppedVideoFrames}`})()")
        .arg(videoJs), 5000).toString();
  };
  std::cout << "  started=" << started << " " << state().toStdString() << std::endl;
  WK_CHECK(started);
  // Wait out a pre-roll ad if one is playing (filtering is tested elsewhere).
  h.waitUntil([&] { return !state().contains(QLatin1String("ad=true")); }, 40000);
  h.js(yt, QStringLiteral("(%1).pause();0").arg(videoJs));
  WK_CHECK(h.waitUntil([&] { return state().contains(QLatin1String("paused=true")); }, 5000));
  // Seek through YouTube's own player API, as its seek bar does.
  h.js(yt, QStringLiteral("(()=>{const p=document.getElementById('movie_player');p.seekTo(120,true);p.playVideo()})();0"));
  const bool seeked = h.waitUntil([&] { return number(h, yt, QStringLiteral("(%1).currentTime").arg(videoJs)) > 121; }, 30000);
  if (!seeked) std::cout << "  seek stalled: " << state().toStdString() << std::endl;
  WK_CHECK(seeked);
  h.hide(yt);
  h.pump(2000);
  h.select(yt);
  WK_CHECK(h.waitUntil([&] { return state().contains(QLatin1String("paused=false")); }, 5000));
  if (dalinira::test::manualGuiTestsEnabled()) {
    int fullScreenOn = 0;
    QObject::connect(yt, &EngineView::fullScreenRequested, [&](bool on) { fullScreenOn += on ? 1 : 0; });
    h.js(yt, QStringLiteral("document.querySelector('.html5-video-player').requestFullscreen();0"));
    WK_CHECK(h.waitUntil([&] { return fullScreenOn > 0; }, 5000));
    yt->exitFullScreen();
    WK_CHECK(h.waitUntil([&] { return !h.js(yt, QStringLiteral("!!document.fullscreenElement")).toBool(); }, 5000));
  }
  std::cout << "  final: " << state().toStdString() << std::endl;
  // The formats YouTube picked (its own stats), e.g. "vp09.00.30.08 (244) / opus (251)".
  std::cout << "  YouTube formats: "
            << h.js(yt, QStringLiteral("(()=>{try{const p=document.getElementById('movie_player');const n=p.getStatsForNerds&&p.getStatsForNerds();"
                                       "return n&&n.codecs?n.codecs:JSON.stringify(p.getVideoStats&&p.getVideoStats())}catch(e){return 'unavailable: '+e}})()"))
                   .toString().toStdString()
            << std::endl;
  const QList<int> ytPids = h.destroyView(yt, [](const QList<int> &pids) { checkAudioReleased(pids, "YouTube closed"); });
  checkAudioReleased(ytPids, "YouTube closed");
  std::cout << "[PASS] YouTube: MSE playback, pause, seek, tab switch" << (dalinira::test::manualGuiTestsEnabled() ? ", fullscreen" : "")
            << ", close." << std::endl;
}

}  // namespace

int main(int argc, char *argv[]) {
  WebKitGuiHarness::disableCoreDumps();
  qputenv("QT_QPA_PLATFORM", "xcb");
  qputenv("GDK_BACKEND", "x11");
  QApplication app(argc, argv);
  std::cout << "=== RUNNING WEBKIT MEDIA RUNTIME TEST SUITE ===" << std::endl;

  testRuntimeEvaluation();

  std::cout << "[TEST 2] Detecting the installed GStreamer runtime..." << std::endl;
  const auto report = WebKitMediaRuntime::probe();
  for (const auto &gap : report.missing) {
    std::cout << "  missing " << gap.element.toStdString() << " (" << gap.plugin.toStdString() << " from "
              << gap.module.toStdString() << ")" << std::endl;
  }
  std::cout << "  playback available: " << (report.playbackAvailable ? "yes" : "no") << std::endl;
  WK_CHECK(report.playbackAvailable == dalinira::engine::WebKitEngineProfile::mediaPlaybackAvailable());

  QTemporaryDir profileDir;
  WK_CHECK(profileDir.isValid());
  dalinira::engine::EngineStorageManager storage(profileDir.path());
  const QByteArray videoSinkOverride = qgetenv("WEBKIT_GST_DMABUF_SINK_DISABLED");
  const bool videoSinkWasSet = qEnvironmentVariableIsSet("WEBKIT_GST_DMABUF_SINK_DISABLED");
  auto backend = std::make_unique<dalinira::engine::WebKitBackend>();
  WK_CHECK(backend->initialize(storage));
  WK_CHECK(qgetenv("WEBKIT_GST_DMABUF_SINK_DISABLED") == (videoSinkWasSet ? videoSinkOverride : QByteArray("1")));
  WebKitSettings *settings = backend->webKitProfile()->settings();
  WK_CHECK(webkit_settings_get_hardware_acceleration_policy(settings) != WEBKIT_HARDWARE_ACCELERATION_POLICY_NEVER);
  WK_CHECK(static_cast<bool>(webkit_settings_get_enable_media(settings)) == report.playbackAvailable);
  WK_CHECK(static_cast<bool>(webkit_settings_get_enable_mediasource(settings)) == report.playbackAvailable);
  std::cout << "[PASS] Profile media settings follow the detected runtime." << std::endl;

  QFile clipFile(QStringLiteral(DALINIRA_TEST_MEDIA_DIR "/vp9_opus_4s.webm"));
  WK_CHECK(clipFile.open(QIODevice::ReadOnly));
  const QByteArray mediaPage =
      "<!doctype html><title>media</title><body style='margin:0;background:#000'>"
      "<video id=v src=clip.webm preload=auto playsinline style='width:320px;height:180px'></video>"
      "<script>window.events=[];const v=document.getElementById('v');"
      "for(const e of ['playing','pause','seeked','ended','error'])v.addEventListener(e,()=>events.push(e));"
      "document.addEventListener('fullscreenchange',()=>events.push(document.fullscreenElement?'fs-on':'fs-off'));"
      "</script></body>";
  MediaSite site(clipFile.readAll(), mediaPage,
                 QByteArray(mediaPage).replace("</body>",
                     "<script>window.swState='pending';navigator.serviceWorker.register('/sw.js')"
                     ".then(()=>navigator.serviceWorker.ready).then(()=>swState='ready',e=>swState='error:'+e);</script></body>"));

  WebKitGuiHarness h(app);
  const auto openPage = [&](const QUrl &url, const char *label) {
    auto *view = h.attach(qobject_cast<WebKitEngineView *>(backend->createView(h.stack())), label);
    WK_CHECK(h.loadAndWait(view, url));
    return view;
  };

  // Baseline for the close cases below: an ordinary page without media.
  std::cout << "[TEST 3a] Closing an ordinary page..." << std::endl;
  h.destroyView(openPage(site.plainPageUrl(), "ordinary page"));
  std::cout << "[PASS] An ordinary page releases its web process promptly." << std::endl;

  auto *view = openPage(site.pageUrl(), "media page");

  if (!report.playbackAvailable) {
    // Without the runtime, media must be reported as unsupported instead of
    // crashing the page, including the legacy Audio() factory.
    std::cout << "[TEST 3] Media runtime missing: verifying pages degrade safely..." << std::endl;
    WK_CHECK(h.js(view, QStringLiteral("document.createElement('video').canPlayType('video/webm; codecs=\"vp9, opus\"')")).toString().isEmpty());
    WK_CHECK(h.js(view, QStringLiteral("(()=>{try{new Audio();return 'created'}catch(e){return e.name}})()")).toString() ==
             QLatin1String("NotSupportedError"));
    std::cout << "[PASS] Missing runtime: media unsupported, page alive. Playback tests SKIPPED." << std::endl;
    h.destroyView(view);
    backend.reset();
    h.waitForAllWebKitProcessesToExit();
    std::cout << "=== WEBKIT MEDIA RUNTIME TESTS PASSED (playback skipped: runtime not installed) ===" << std::endl;
    return 0;
  }

  std::cout << "[TEST 3] Codec support advertised to pages..." << std::endl;
  const QString vp9 = h.js(view, QStringLiteral("document.createElement('video').canPlayType('video/webm; codecs=\"vp9, opus\"')")).toString();
  const bool mseVp9 = h.js(view, QStringLiteral("!!window.MediaSource && MediaSource.isTypeSupported('video/webm; codecs=\"vp9\"')")).toBool();
  const bool mseOpus = h.js(view, QStringLiteral("!!window.MediaSource && MediaSource.isTypeSupported('audio/webm; codecs=\"opus\"')")).toBool();
  std::cout << "  canPlayType(webm vp9+opus)=" << vp9.toStdString() << " MSE vp9=" << mseVp9 << " MSE opus=" << mseOpus << std::endl;
  for (const char *type : {"video/mp4; codecs=\"avc1.42E01E\"", "audio/mp4; codecs=\"mp4a.40.2\"",
                           "video/mp4; codecs=\"av01.0.05M.08\"", "video/webm; codecs=\"vp8, vorbis\""}) {
    const QString quoted = QString::fromLatin1(type).replace(QLatin1Char('\''), QLatin1String("\\'"));
    const QString answer = h.js(view, QStringLiteral("document.createElement('video').canPlayType('%1')").arg(quoted)).toString();
    const bool mse = h.js(view, QStringLiteral("MediaSource.isTypeSupported('%1')").arg(quoted)).toBool();
    std::cout << "  " << type << " canPlayType=" << answer.toStdString() << " MSE=" << mse << std::endl;
  }
  WK_CHECK(!vp9.isEmpty() && mseVp9 && mseOpus);
  std::cout << "[PASS] VP9/Opus WebM is playable, including through MSE." << std::endl;

  std::cout << "[TEST 4] Playback with decoded video and audio output..." << std::endl;
  // A muted element skips the audio sink; keep it unmuted at an inaudible volume.
  h.js(view, QStringLiteral("v.volume=0.001;v.muted=false;window.playResult='pending';"
                            "v.play().then(()=>playResult='ok',e=>playResult=e.name);0"));
  WK_CHECK(h.waitUntil([&] { return number(h, view, QStringLiteral("v.currentTime")) > 1.0; }, 10000));
  WK_CHECK(h.js(view, QStringLiteral("playResult")).toString() == QLatin1String("ok"));
  WK_CHECK(number(h, view, QStringLiteral("v.videoWidth")) == 160.0);
  std::cout << "  frames decoded=" << number(h, view, QStringLiteral("v.getVideoPlaybackQuality().totalVideoFrames"))
            << " dropped=" << number(h, view, QStringLiteral("v.getVideoPlaybackQuality().droppedVideoFrames")) << std::endl;
  WK_CHECK(number(h, view, QStringLiteral("v.getVideoPlaybackQuality().totalVideoFrames")) > 0);
  WK_CHECK(number(h, view, QStringLiteral("v.audioTracks.length")) == 1);
  // The audio really reaches the sound server: the web process owns a sink input.
  QProcess pactl;
  pactl.start(QStringLiteral("pactl"), {QStringLiteral("list"), QStringLiteral("sink-inputs")});
  if (pactl.waitForStarted(2000) && pactl.waitForFinished(3000)) {
    const QByteArray inputs = pactl.readAllStandardOutput();
    std::cout << "  sound server sink input from WebKitWebProcess: " << inputs.contains("WebKitWebProcess") << std::endl;
    WK_CHECK(inputs.contains("WebKitWebProcess"));
  } else {
    std::cout << "  pactl unavailable; audio output not verified at the sound server" << std::endl;
  }
  std::cout << "[PASS] Video decoded (160x90), audio decoded and played through the sink." << std::endl;

  std::cout << "[TEST 5] Pause and resume..." << std::endl;
  h.js(view, QStringLiteral("v.pause();0"));
  WK_CHECK(h.waitUntil([&] { return h.js(view, QStringLiteral("v.paused")).toBool(); }, 3000));
  const double pausedAt = number(h, view, QStringLiteral("v.currentTime"));
  h.pump(400);
  WK_CHECK(std::abs(number(h, view, QStringLiteral("v.currentTime")) - pausedAt) < 0.05);
  h.js(view, QStringLiteral("v.play();0"));
  WK_CHECK(h.waitUntil([&] { return number(h, view, QStringLiteral("v.currentTime")) > pausedAt + 0.3; }, 5000));
  std::cout << "[PASS] Pause holds the position; resume continues playback." << std::endl;

  std::cout << "[TEST 6] Seeking..." << std::endl;
  h.js(view, QStringLiteral("v.pause();events=[];v.currentTime=0.5;0"));
  WK_CHECK(h.waitUntil([&] { return h.js(view, QStringLiteral("events.includes('seeked')")).toBool(); }, 5000));
  WK_CHECK(std::abs(number(h, view, QStringLiteral("v.currentTime")) - 0.5) < 0.1);
  h.js(view, QStringLiteral("events=[];v.currentTime=3.0;v.play();0"));
  WK_CHECK(h.waitUntil([&] { return h.js(view, QStringLiteral("events.includes('seeked')")).toBool(); }, 5000));
  WK_CHECK(h.waitUntil([&] { return number(h, view, QStringLiteral("v.currentTime")) > 3.1; }, 5000));
  std::cout << "[PASS] Backward and forward seeks land on the requested position." << std::endl;

  std::cout << "[TEST 7] Media Source Extensions playback (YouTube path)..." << std::endl;
  h.js(view, QStringLiteral(
      "window.mseState='pending';(async()=>{try{"
      "const data=await (await fetch('clip.webm')).arrayBuffer();"
      "const m=document.createElement('video');m.id='m';m.volume=0.001;document.body.appendChild(m);"
      "const ms=new MediaSource();m.src=URL.createObjectURL(ms);"
      "await new Promise(r=>ms.addEventListener('sourceopen',r,{once:true}));"
      "const sb=ms.addSourceBuffer('video/webm; codecs=\"vp9, opus\"');"
      "sb.appendBuffer(data);await new Promise((r,j)=>{sb.addEventListener('updateend',r,{once:true});sb.addEventListener('error',j,{once:true})});"
      "ms.endOfStream();await m.play();mseState='playing'}catch(e){mseState='error:'+e}})();0"));
  WK_CHECK(h.waitUntil([&] { return h.js(view, QStringLiteral("mseState")).toString() != QLatin1String("pending"); }, 10000));
  const QString mseState = h.js(view, QStringLiteral("mseState")).toString();
  std::cout << "  MSE state: " << mseState.toStdString() << std::endl;
  WK_CHECK(mseState == QLatin1String("playing"));
  WK_CHECK(h.waitUntil([&] { return number(h, view, QStringLiteral("document.getElementById('m').currentTime")) > 0.5; }, 5000));
  h.js(view, QStringLiteral("document.getElementById('m').pause();0"));
  std::cout << "[PASS] MSE SourceBuffer playback advances." << std::endl;

  if (dalinira::test::manualGuiTestsEnabled()) {
    std::cout << "[MANUAL] Fullscreen enter and exit..." << std::endl;
    int fullScreenOn = 0;
    int fullScreenOff = 0;
    QObject::connect(view, &EngineView::fullScreenRequested, [&](bool on) { (on ? fullScreenOn : fullScreenOff)++; });
    h.js(view, QStringLiteral("events=[];v.requestFullscreen?v.requestFullscreen():v.webkitRequestFullscreen();0"));
    WK_CHECK(h.waitUntil([&] { return fullScreenOn > 0 && h.js(view, QStringLiteral("!!document.fullscreenElement")).toBool(); }, 5000));
    h.js(view, QStringLiteral("document.exitFullscreen();0"));
    WK_CHECK(h.waitUntil([&] { return fullScreenOff > 0 && !h.js(view, QStringLiteral("!!document.fullscreenElement")).toBool(); }, 5000));
    std::cout << "[PASS] Page fullscreen reaches the browser and exits cleanly." << std::endl;
  } else {
    std::cout << "[SKIP] Fullscreen goes through the window manager (DALINIRA_GUI_MANUAL=1)." << std::endl;
  }

  std::cout << "[TEST 8] Tab switch during playback..." << std::endl;
  // A fresh pipeline at 0: the 1.2 s measured below stays clear of the end
  // of the 4 s clip (replaying an ended clip is TEST 8b).
  WK_CHECK(h.loadAndWait(view, site.pageUrl()));
  h.js(view, QStringLiteral("v.volume=0.001;v.play();0"));
  WK_CHECK(h.waitUntil([&] { return number(h, view, QStringLiteral("v.currentTime")) > 0.3; }, 5000) &&
           "playback must be running before the tab is hidden");
  h.hide(view);
  const double hiddenAt = number(h, view, QStringLiteral("v.currentTime"));
  h.pump(1200);
  // Background audio keeps playing, as in Chromium and Brave.
  const double hiddenNow = number(h, view, QStringLiteral("v.currentTime"));
  std::cout << "  hidden at t=" << hiddenAt << ", 1.2 s later t=" << hiddenNow
            << " paused=" << h.js(view, QStringLiteral("v.paused")).toBool()
            << " readyState=" << number(h, view, QStringLiteral("v.readyState")) << std::endl;
  WK_CHECK(hiddenNow > hiddenAt + 0.5 || hiddenNow < hiddenAt);  // advanced, or looped
  h.select(view);
  h.pump(300);
  WK_CHECK(view->nativeSurfaceMapped() && !h.js(view, QStringLiteral("v.paused")).toBool());
  std::cout << "[PASS] Playback continues in a background tab and after switching back." << std::endl;

  std::cout << "[TEST 8b] Replaying a clip that has ended..." << std::endl;
  h.js(view, QStringLiteral("events=[];v.loop=false;v.currentTime=3.5;v.play();0"));
  WK_CHECK(h.waitUntil([&] { return h.js(view, QStringLiteral("v.ended")).toBool(); }, 5000));
  // Decoded frames are sampled in the page; they must keep climbing.
  h.js(view, QStringLiteral("window.frameSamples=[];v.loop=true;v.play();"
                            "window.frameSampler=setInterval(()=>frameSamples.push(v.getVideoPlaybackQuality().totalVideoFrames),200);0"));
  h.pump(1500);
  const QString samples = h.js(view, QStringLiteral("clearInterval(frameSampler);frameSamples.join(',')")).toString();
  const bool framesAdvance = h.js(view, QStringLiteral("frameSamples.length>=4&&frameSamples.slice(-3).every((f,i,a)=>i==0||f>a[i-1])")).toBool();
  const double replayPosition = number(h, view, QStringLiteral("v.currentTime"));
  const double duration = number(h, view, QStringLiteral("v.duration"));
  std::cout << "  decoded frames every 200 ms: " << samples.toStdString() << "; currentTime " << replayPosition << " of "
            << duration << std::endl;
  WK_CHECK(framesAdvance && !h.js(view, QStringLiteral("v.paused")).toBool() && "an ended clip must play again");
  if (replayPosition >= duration - 0.05) {
    // WebKitGTK 2.52.6: after the EOS pipeline reset, playbackPosition()
    // keeps returning the duration while playback runs (upstream, not
    // DaliNira; MSE players such as YouTube's are not affected).
    std::cout << "  [KNOWN ISSUE] WebKitGTK reports currentTime at the end after replaying an ended progressive clip" << std::endl;
  }
  std::cout << "[PASS] An ended clip plays again (decoding restarts)." << std::endl;

  std::cout << "[TEST 9] Closing a tab while it plays..." << std::endl;
  WK_CHECK(!h.js(view, QStringLiteral("v.paused")).toBool());
  checkAudioReleased(h.destroyView(view), "media page closed");  // fails unless the web process exits cleanly
  std::cout << "[PASS] A playing tab closes; its audio stream and web process are released." << std::endl;

  std::cout << "[TEST 9b] Closing a playing tab whose site has a service worker..." << std::endl;
  auto *swView = openPage(site.serviceWorkerPageUrl(), "media page + service worker");
  WK_CHECK(h.waitUntil([&] { return h.js(swView, QStringLiteral("swState")).toString() != QLatin1String("pending"); }, 5000));
  std::cout << "  service worker: " << h.js(swView, QStringLiteral("swState")).toString().toStdString() << std::endl;
  WK_CHECK(h.js(swView, QStringLiteral("swState")).toString() == QLatin1String("ready"));
  h.js(swView, QStringLiteral("v.volume=0.001;v.loop=true;v.play();0"));
  WK_CHECK(h.waitUntil([&] { return number(h, swView, QStringLiteral("v.currentTime")) > 0.3; }, 5000));
  const QList<int> swPids =
      h.destroyView(swView, [](const QList<int> &pids) { checkAudioReleased(pids, "service worker page closed"); });
  checkAudioReleased(swPids, "service worker page closed");
  std::cout << "[PASS] The tab is released at once; WebKit's idle worker process ends on its own." << std::endl;

  if (qEnvironmentVariableIntValue("DALINIRA_LIVE_MEDIA_TEST") == 1) liveYouTube(h, *backend);

  // A playing page whose site has a service worker, ready to be torn down.
  const auto openPlayingServiceWorkerPage = [&](const char *label) {
    auto *page = openPage(site.serviceWorkerPageUrl(), label);
    WK_CHECK(h.waitUntil([&] { return h.js(page, QStringLiteral("swState")).toString() == QLatin1String("ready"); }, 5000));
    h.js(page, QStringLiteral("v.volume=0.001;v.loop=true;v.play();0"));
    WK_CHECK(h.waitUntil([&] { return number(h, page, QStringLiteral("v.currentTime")) > 0.3; }, 5000));
    return page;
  };

  std::cout << "[TEST 10] Releasing the process pool while media plays (site with a service worker)..." << std::endl;
  auto *last = openPlayingServiceWorkerPage("pool release while playing");
  // The view and the backend (web context) go away back to back. Media stops
  // at once; WebKit's worker process keeps the pool (and its network
  // process) until the worker delay ends, then everything exits.
  const QList<int> lastPids = WebKitGuiHarness::webProcessPids();
  h.closeWithoutWaiting(last);
  backend.reset();
  h.pump(500);
  checkAudioReleased(lastPids, "pool released");
  h.waitForAllWebKitProcessesToExit("process pool released");
  std::cout << "[PASS] Media stopped with the view; every WebKit process exited once WebKit ended the worker." << std::endl;

  std::cout << "[TEST 11] Browser exit while WebKit keeps a worker process..." << std::endl;
  backend = std::make_unique<dalinira::engine::WebKitBackend>();
  WK_CHECK(backend->initialize(storage));
  auto *exiting = openPlayingServiceWorkerPage("exit while playing");
  h.closeWithoutWaiting(exiting);
  backend.reset();
  h.shell().close();
  // The process exits now; the helpers see their connections close and exit.
  // scripts/safe-gui-test.sh fails the run if any WebKit process outlives it.
  std::cout << "  exiting with " << WebKitGuiHarness::webKitProcessPids().size()
            << " WebKit process(es) still running (they must follow the browser)" << std::endl;
  std::cout << "=== ALL WEBKIT MEDIA RUNTIME TESTS PASSED ===" << std::endl;
  return 0;
}
