// Deterministic load test of DaliNira's shared Web Audio effects engine
// (WebAudioEffectsController + its in-page DALI graph) with several pages open,
// without a GUI: offscreen QtWebEngine, output muted (--mute-audio), local
// silent media. Each "tab" is a feed-like page: one looping media element
// playing and several preloaded, paused ones.
//
// Reports and checks, for 1 and several tabs:
//   * DSP graphs / AudioContexts / auto-gain timers per page,
//   * renderer CPU over a fixed window,
//   * scripts and bytes the browser process sends per page,
//   * Chromium's AudioContext.playoutStats glitch counters when available.

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QWebEnginePage>
#include <QWebEngineSettings>
#include <QWebEngineView>
#include <functional>
#include <iostream>
#include <unistd.h>

#include "audio/web_audio_effects_controller.h"

#define CHECK(condition)                                                                    \
  do {                                                                                      \
    if (!(condition)) {                                                                     \
      std::cout << "[FAIL] " << __FILE__ << ":" << __LINE__ << ": " #condition << std::endl; \
      std::_Exit(1);                                                                        \
    }                                                                                       \
  } while (0)

namespace {

constexpr int kElementsPerPage = 6;

bool waitUntil(const std::function<bool()> &condition, int timeoutMs) {
  QElapsedTimer timer;
  timer.start();
  while (!condition()) {
    if (timer.elapsed() >= timeoutMs) return false;
    QApplication::processEvents(QEventLoop::AllEvents | QEventLoop::WaitForMoreEvents, 50);
  }
  return true;
}

void pump(int ms) {
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < ms) QApplication::processEvents(QEventLoop::AllEvents | QEventLoop::WaitForMoreEvents, 50);
}

QByteArray silentWav() {
  const int rate = 48000, channels = 2, seconds = 1;
  const int dataSize = rate * channels * 2 * seconds;
  QByteArray wav;
  auto u32 = [&wav](quint32 v) { for (int i = 0; i < 4; ++i) wav.append(char((v >> (8 * i)) & 0xff)); };
  auto u16 = [&wav](quint16 v) { wav.append(char(v & 0xff)); wav.append(char(v >> 8)); };
  wav.append("RIFF"); u32(36 + dataSize); wav.append("WAVEfmt "); u32(16); u16(1); u16(channels);
  u32(rate); u32(rate * channels * 2); u16(channels * 2); u16(16); wav.append("data"); u32(dataSize);
  wav.append(QByteArray(dataSize, '\0'));
  return wav;
}

class LocalSite {
 public:
  LocalSite() : wav_(silentWav()) {
    CHECK(server_.listen(QHostAddress::LocalHost));
    QObject::connect(&server_, &QTcpServer::newConnection, [this] {
      while (QTcpSocket *socket = server_.nextPendingConnection()) {
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
          const QByteArray request = socket->readAll();
          const QByteArray path = request.split(' ').value(1);
          QByteArray body;
          QByteArray type = "text/html";
          if (path.startsWith("/silence.wav")) {
            body = wav_;
            type = "audio/wav";
          } else {
            // One playing element (the audible media) and preloaded, paused
            // ones, as a social feed or a video page with previews has.
            body = "<!doctype html><title>feed</title><audio id=main src=/silence.wav loop autoplay></audio>";
            for (int i = 1; i < kElementsPerPage; ++i)
              body += "<audio preload=auto src=/silence.wav?e=" + QByteArray::number(i) + "></audio>";
          }
          socket->write("HTTP/1.1 200 OK\r\nContent-Type: " + type + "\r\nConnection: close\r\nContent-Length: " +
                        QByteArray::number(body.size()) + "\r\n\r\n" + body);
          socket->disconnectFromHost();
        });
        QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
      }
    });
  }
  QUrl url(const QString &path) const { return QUrl(QStringLiteral("http://127.0.0.1:%1%2").arg(server_.serverPort()).arg(path)); }

 private:
  QTcpServer server_;
  QByteArray wav_;
};

// CPU ticks (utime + stime) of this process's QtWebEngineProcess descendants.
qint64 rendererCpuTicks() {
  QHash<qint64, qint64> parent;
  QHash<qint64, QByteArray> comm;
  QHash<qint64, qint64> ticks;
  for (const QString &entry : QDir(QStringLiteral("/proc")).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
    QFile stat(QStringLiteral("/proc/%1/stat").arg(entry));
    if (!stat.open(QIODevice::ReadOnly)) continue;
    const QByteArray text = stat.readAll();
    const int open = text.indexOf('(');
    const int close = text.lastIndexOf(')');
    const QList<QByteArray> fields = text.mid(close + 2).split(' ');
    if (open < 0 || close < open || fields.size() < 13) continue;
    comm.insert(entry.toLongLong(), text.mid(open + 1, close - open - 1));
    parent.insert(entry.toLongLong(), fields.at(1).toLongLong());
    ticks.insert(entry.toLongLong(), fields.at(11).toLongLong() + fields.at(12).toLongLong());
  }
  qint64 total = 0;
  for (auto it = comm.cbegin(); it != comm.cend(); ++it) {
    if (!it.value().startsWith("QtWebEngineProc")) continue;
    for (qint64 p = parent.value(it.key()), depth = 0; p > 1 && depth < 16; p = parent.value(p), ++depth)
      if (p == QCoreApplication::applicationPid()) { total += ticks.value(it.key()); break; }
  }
  return total;
}

const char *kProbe = R"JS(
(function() {
  const root = window.__DALINIRA_WEB_DALI_OUTPUT__;
  const media = document.querySelectorAll('audio,video').length;
  const main = document.getElementById('main');
  const out = { root: !!root, media: media, playing: !!(main && !main.paused), graphs: 0, contexts: 0,
                running: 0, autoGainTimers: 0, playout: null };
  if (!root || !root.graphList) return out;
  const contexts = new Set();
  for (const graph of root.graphList) {
    if (!graph) continue;
    out.graphs += 1;
    if (graph.ctx) contexts.add(graph.ctx);
    if (graph.autoGainTimer) out.autoGainTimers += 1;
  }
  if (root.audioContext) contexts.add(root.audioContext);
  out.contexts = contexts.size;
  for (const ctx of contexts) {
    if (ctx.state === 'running') out.running += 1;
    if (ctx.playoutStats && !out.playout) {
      const s = ctx.playoutStats;
      out.playout = { fallbackFramesEvents: Number(s.fallbackFramesEvents) || 0,
                      fallbackDuration: Number(s.fallbackDuration) || 0,
                      totalDuration: Number(s.totalDuration) || 0 };
    }
  }
  return out;
})()
)JS";

struct PageProbe {
  bool root = false;
  bool playing = false;
  int graphs = 0;
  int contexts = 0;
  int running = 0;
  int autoGainTimers = 0;
  bool hasPlayout = false;
  double fallbackEvents = 0;
  double fallbackMs = 0;
};

PageProbe probe(QWebEngineView *view) {
  PageProbe result;
  bool done = false;
  view->page()->runJavaScript(QString::fromLatin1(kProbe), [&](const QVariant &value) {
    const QVariantMap map = value.toMap();
    result.root = map.value(QStringLiteral("root")).toBool();
    result.playing = map.value(QStringLiteral("playing")).toBool();
    result.graphs = map.value(QStringLiteral("graphs")).toInt();
    result.contexts = map.value(QStringLiteral("contexts")).toInt();
    result.running = map.value(QStringLiteral("running")).toInt();
    result.autoGainTimers = map.value(QStringLiteral("autoGainTimers")).toInt();
    const QVariantMap playout = map.value(QStringLiteral("playout")).toMap();
    result.hasPlayout = !playout.isEmpty();
    result.fallbackEvents = playout.value(QStringLiteral("fallbackFramesEvents")).toDouble();
    result.fallbackMs = playout.value(QStringLiteral("fallbackDuration")).toDouble();
    done = true;
  });
  CHECK(waitUntil([&] { return done; }, 5000));
  return result;
}

void runScript(QWebEngineView *view, const QString &script) {
  bool done = false;
  view->page()->runJavaScript(script, [&done](const QVariant &) { done = true; });
  CHECK(waitUntil([&] { return done; }, 5000));
}

struct ScenarioResult {
  int tabs = 0;
  int idleGraphs = 0;
  int idleRunningContexts = 0;
  int idleAutoGainTimers = 0;
  double idleCpuSeconds = 0;
  bool resumedAfterPlay = false;
  int graphs = 0;
  int runningContexts = 0;
  int autoGainTimers = 0;
  double cpuSeconds = 0;
  quint64 scripts = 0;
  quint64 scriptBytes = 0;
  bool hasPlayout = false;
  double fallbackEvents = 0;
};

ScenarioResult runScenario(WebAudioEffectsController &controller, const LocalSite &site, int tabs) {
  ScenarioResult result;
  result.tabs = tabs;
  controller.resetDiagnostics();
  QList<QWebEngineView *> views;
  for (int i = 0; i < tabs; ++i) {
    auto *view = new QWebEngineView;
    view->page()->settings()->setAttribute(QWebEngineSettings::PlaybackRequiresUserGesture, false);
    view->resize(800, 600);
    const QUrl url = site.url(QStringLiteral("/feed?tab=%1").arg(i));
    controller.registerWebView(view, url);
    bool loaded = false;
    QObject::connect(view, &QWebEngineView::loadFinished, view, [&loaded](bool) { loaded = true; });
    view->load(url);
    CHECK(waitUntil([&] { return loaded; }, 20000));
    views.append(view);
  }
  // Only the first tab is "in front"; its media is the audible one.
  for (QWebEngineView *view : views) {
    CHECK(waitUntil([&] { return probe(view).playing; }, 10000));
  }
  pump(1500);  // graphs attach; auto gain starts
  const qint64 before = rendererCpuTicks();
  pump(6000);
  const qint64 after = rendererCpuTicks();
  result.cpuSeconds = double(after - before) / double(sysconf(_SC_CLK_TCK));
  for (QWebEngineView *view : views) {
    const PageProbe page = probe(view);
    result.graphs += page.graphs;
    result.runningContexts += page.running;
    result.autoGainTimers += page.autoGainTimers;
    if (page.hasPlayout) {
      result.hasPlayout = true;
      result.fallbackEvents += page.fallbackEvents;
    }
  }
  const WebAudioEffectsController::Diagnostics diagnostics = controller.diagnostics();
  result.scripts = diagnostics.scriptsSent;
  result.scriptBytes = diagnostics.scriptBytesSent;

  // The usual many-tab case: one audible tab in front, the media of every
  // other tab paused. Their graphs stay (no re-attachment, no truncation) but
  // their AudioContexts go idle, auto gain included.
  for (int i = 1; i < views.size(); ++i) runScript(views[i], QStringLiteral("document.getElementById('main').pause()"));
  pump(5500);  // idle grace (4 s) + slack
  const qint64 idleBefore = rendererCpuTicks();
  pump(6000);
  const qint64 idleAfter = rendererCpuTicks();
  result.idleCpuSeconds = double(idleAfter - idleBefore) / double(sysconf(_SC_CLK_TCK));
  for (QWebEngineView *view : views) {
    const PageProbe page = probe(view);
    result.idleGraphs += page.graphs;
    result.idleRunningContexts += page.running;
    result.idleAutoGainTimers += page.autoGainTimers;
  }
  // Playing again resumes the same graph at once.
  if (views.size() > 1) {
    runScript(views[1], QStringLiteral("document.getElementById('main').play()"));
    result.resumedAfterPlay = waitUntil([&] {
      const PageProbe page = probe(views[1]);
      return page.playing && page.running == 1 && page.graphs == 1 && page.autoGainTimers == 1;
    }, 3000);
  }
  for (QWebEngineView *view : views) {
    controller.unregisterWebView(view);
    delete view;
  }
  pump(500);
  return result;
}

void print(const ScenarioResult &r) {
  std::cout << "  tabs=" << r.tabs << " graphs=" << r.graphs << " runningContexts=" << r.runningContexts
            << " autoGainTimers=" << r.autoGainTimers << " rendererCpu=" << r.cpuSeconds << "s/6s"
            << " scripts=" << r.scripts << " scriptKB=" << (r.scriptBytes / 1024);
  if (r.hasPlayout) std::cout << " playoutFallbackEvents=" << r.fallbackEvents;
  std::cout << std::endl;
  if (r.tabs > 1)
    std::cout << "    background paused: graphs=" << r.idleGraphs << " runningContexts=" << r.idleRunningContexts
              << " autoGainTimers=" << r.idleAutoGainTimers << " rendererCpu=" << r.idleCpuSeconds << "s/6s"
              << " resumedOnPlay=" << (r.resumedAfterPlay ? "yes" : "no") << std::endl;
}

}  // namespace

int main(int argc, char *argv[]) {
  qputenv("DALINIRA_ALLOW_LOCAL_AUDIO_TEST", "1");
  QApplication app(argc, argv);
  QTemporaryDir directory;
  CHECK(directory.isValid());
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
  QCoreApplication::setOrganizationName(QStringLiteral("DaliNiraTest"));
  QCoreApplication::setApplicationName(QStringLiteral("WebAudioEngineStress"));
  std::cout << "=== WEB AUDIO EFFECTS ENGINE LOAD (offscreen, muted, local media) ===" << std::endl;

  LocalSite site;
  WebAudioEffectsController controller(nullptr, false);
  const bool control = qEnvironmentVariableIntValue("DALINIRA_AUDIO_STRESS_CONTROL") == 1;  // effects off
  controller.setEnabled(!control);
  controller.setEqualizerBand(3, 4.0);
  controller.setCompressorEnabled(true);
  controller.setLimiterEnabled(true);
  controller.setAutoGainEnabled(true);

  const ScenarioResult one = runScenario(controller, site, 1);
  print(one);
  const int many = qEnvironmentVariableIntValue("DALINIRA_AUDIO_STRESS_TABS") > 0
      ? qEnvironmentVariableIntValue("DALINIRA_AUDIO_STRESS_TABS") : 6;
  const ScenarioResult several = runScenario(controller, site, many);
  print(several);

  if (control) {
    // Effects off: pages receive nothing and get no graph.
    CHECK(several.graphs == 0 && several.runningContexts == 0 && several.scripts == 0);
    std::cout << "=== CONTROL RUN (effects off) PASSED ===" << std::endl;
    return 0;
  }
  // One graph per page (the element that plays), never one per preloaded
  // element; one running context per page with audible media.
  CHECK(one.graphs == 1 && one.runningContexts == 1);
  CHECK(several.graphs == many && several.runningContexts == many);
  CHECK(several.autoGainTimers <= many);
  // Paused background tabs: graphs kept, one running context (the front tab).
  CHECK(several.idleGraphs == many);
  CHECK(several.idleRunningContexts == 1 && several.idleAutoGainTimers == 1);
  CHECK(several.resumedAfterPlay);
  // The browser sends each page its bootstrap once, not once per event.
  CHECK(several.scripts <= quint64(many) * 8);
  std::cout << "=== WEB AUDIO EFFECTS ENGINE LOAD PASSED ===" << std::endl;
  return 0;
}
