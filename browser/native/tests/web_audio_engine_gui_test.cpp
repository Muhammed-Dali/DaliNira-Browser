// DaliNira's shared Web Audio effects engine through the real BrowserWindow,
// on Chromium and then on WebKit, with the same checks: the DALI runtime
// reaches the page, only the playing element gets a graph and its audio flows
// through it, tab switches neither duplicate nor re-attach graphs, a paused
// background tab's AudioContext goes idle and resumes on play, and switching
// the effects off/on at runtime keeps the media playing.
//
// GUI tier (scripts/safe-gui-test.sh). One engine per process, as DaliNira
// runs: the parent serves the local media and runs one child per engine, one
// after the other; each child has one window and at most two media tabs. The
// media is a -80 dBFS tone (inaudible).

#include <memory>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <algorithm>
#include <cmath>
#include <functional>
#include <glib.h>
#include <iostream>

#include "audio/web_audio_effects_controller.h"
#include "browser_window.h"
#include "core/application_identity.h"
#include "core/browser_profile_service.h"
#include "desktop_tabs/tab_manager.h"
#include "desktop_tabs/tab_strip_widget.h"
#include "engine/chromium/chromium_backend.h"
#include "engine/engine_backend.h"
#include "engine/engine_storage_manager.h"
#include "engine/engine_view.h"
#include "newtab/new_tab_scheme.h"
#if defined(DALINIRA_BUILD_WEBKIT)
#include "engine/webkit/webkit_backend.h"
#endif

#define CHECK(condition)                                                                    \
  do {                                                                                      \
    if (!(condition)) {                                                                     \
      std::cout << "[FAIL] " << __FILE__ << ":" << __LINE__ << ": " #condition << std::endl; \
      std::_Exit(1);                                                                        \
    }                                                                                       \
  } while (0)

namespace {

bool waitUntil(const std::function<bool()> &condition, int timeoutMs) {
  QTimer heartbeat;
  heartbeat.start(20);
  QElapsedTimer timer;
  timer.start();
  while (!condition()) {
    if (timer.elapsed() >= timeoutMs) return false;
    const qint64 next = timer.elapsed() + 50;
    while (timer.elapsed() < next) QApplication::processEvents(QEventLoop::AllEvents | QEventLoop::WaitForMoreEvents);
  }
  return true;
}

void pump(int ms) {
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < ms) QApplication::processEvents(QEventLoop::AllEvents | QEventLoop::WaitForMoreEvents, 50);
}

// A 440 Hz tone at -80 dBFS, 2 s, 48 kHz stereo 16-bit.
QByteArray quietToneWav() {
  const int rate = 48000, channels = 2, frames = rate * 2;
  QByteArray pcm;
  pcm.reserve(frames * channels * 2);
  for (int i = 0; i < frames; ++i) {
    const qint16 sample = qint16(std::lround(32767.0 * 1e-4 * std::sin(2.0 * M_PI * 440.0 * i / rate)));
    for (int c = 0; c < channels; ++c) { pcm.append(char(sample & 0xff)); pcm.append(char((sample >> 8) & 0xff)); }
  }
  QByteArray wav;
  auto u32 = [&wav](quint32 v) { for (int i = 0; i < 4; ++i) wav.append(char((v >> (8 * i)) & 0xff)); };
  auto u16 = [&wav](quint16 v) { wav.append(char(v & 0xff)); wav.append(char(v >> 8)); };
  wav.append("RIFF"); u32(36 + pcm.size()); wav.append("WAVEfmt "); u32(16); u16(1); u16(channels);
  u32(rate); u32(rate * channels * 2); u16(channels * 2); u16(16); wav.append("data"); u32(pcm.size());
  return wav + pcm;
}

// ---------------------------------------------------------------------------
// One engine
// ---------------------------------------------------------------------------

const char *kProbe = R"JS((function() {
  const root = window.__DALINIRA_WEB_DALI_OUTPUT__;
  const main = document.getElementById('main');
  const out = { root: !!(root && root.processMedia), playing: !!(main && !main.paused), graphs: 0,
                state: 'none', bypass: false, marked: false, signal: -1,
                time: main ? Number(main.currentTime) || 0 : 0, hidden: !!document.hidden,
                media: main ? ('ready=' + main.readyState + ' net=' + main.networkState +
                              ' err=' + (main.error ? main.error.code : 0) + ' volume=' + main.volume +
                              ' muted=' + main.muted + ' gain=' + (function() {
                                const g = root && root.graphs ? root.graphs.get(main) : null;
                                return g && g.mirrorsVolume ? g.source.gain.value : 'n/a'; })()) : '' };
  if (root && root.audioContext) out.state = String(root.audioContext.state);
  if (!root || !root.graphList) return out;
  for (const graph of root.graphList) {
    if (!graph) continue;
    out.graphs += 1;
    if (graph.element !== main) continue;
    out.bypass = !!graph.bypass;
    out.marked = !!graph.__probeMark;
    graph.__probeMark = true;
    // A side analyser on the element's source: its audio reaches the graph.
    if (!graph.__probeAnalyser) {
      try { graph.__probeAnalyser = new AnalyserNode(graph.ctx, { fftSize: 2048 }); graph.source.connect(graph.__probeAnalyser); } catch (_) {}
    }
    if (graph.__probeAnalyser) {
      const buffer = new Float32Array(2048);
      graph.__probeAnalyser.getFloatTimeDomainData(buffer);
      let sum = 0;
      for (const value of buffer) sum += value * value;
      out.signal = Math.sqrt(sum / buffer.length);
    }
  }
  if (root.audioContext) out.state = String(root.audioContext.state);
  return out;
})())JS";

struct Probe {
  bool root = false;
  bool playing = false;
  int graphs = 0;
  QString state;
  bool bypass = false;
  bool marked = false;
  double signal = -1;
  double time = 0;
  bool hidden = false;
  QString media;
};

void runScript(BrowserWindow &window, int index, const QString &script, std::function<void(const QVariant &)> callback = {}) {
  const BrowserTabInfo &tab = window.tabInfo(index);
  if (tab.engineView) tab.engineView->runJavaScript(script, std::move(callback));
  else if (tab.view && tab.view->page()) {
    if (callback) tab.view->page()->runJavaScript(script, std::move(callback));
    else tab.view->page()->runJavaScript(script);
  }
}

Probe probe(BrowserWindow &window, int index) {
  Probe result;
  bool done = false;
  runScript(window, index, QString::fromLatin1(kProbe), [&](const QVariant &value) {
    const QVariantMap map = value.toMap();
    result.root = map.value(QStringLiteral("root")).toBool();
    result.playing = map.value(QStringLiteral("playing")).toBool();
    result.graphs = map.value(QStringLiteral("graphs")).toInt();
    result.state = map.value(QStringLiteral("state")).toString();
    result.bypass = map.value(QStringLiteral("bypass")).toBool();
    result.marked = map.value(QStringLiteral("marked")).toBool();
    result.signal = map.value(QStringLiteral("signal")).toDouble();
    result.time = map.value(QStringLiteral("time")).toDouble();
    result.hidden = map.value(QStringLiteral("hidden")).toBool();
    result.media = map.value(QStringLiteral("media")).toString();
    done = true;
  });
  CHECK(waitUntil([&] { return done; }, 5000));
  return result;
}

QVariantMap evaluate(BrowserWindow &window, int index, const QString &script);

bool playingThroughGraph(BrowserWindow &window, int index) {
  Probe p;
  const bool ok = waitUntil([&] {
    p = probe(window, index);
    return p.playing && p.graphs == 1 && p.state == QLatin1String("running") && !p.bypass && p.signal > 1e-6;
  }, 10000);
  if (!ok) {
    std::cout << "  probe tab " << index << ": root=" << p.root << " playing=" << p.playing << " graphs=" << p.graphs
              << " state=" << p.state.toStdString() << " bypass=" << p.bypass << " signal=" << p.signal
              << " mediaTime=" << p.time << " hidden=" << p.hidden << " " << p.media.toStdString() << std::endl;
  }
  return ok;
}

QList<qint64> webKitProcesses() {
  QHash<qint64, qint64> parent;
  QHash<qint64, QByteArray> comm;
  for (const QString &entry : QDir(QStringLiteral("/proc")).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
    QFile stat(QStringLiteral("/proc/%1/stat").arg(entry));
    if (!stat.open(QIODevice::ReadOnly)) continue;
    const QByteArray text = stat.readAll();
    const int open = text.indexOf('(');
    const int close = text.lastIndexOf(')');
    const QList<QByteArray> fields = text.mid(close + 2).split(' ');
    if (open < 0 || close < open || fields.size() < 2 || fields.at(0) == "Z") continue;
    comm.insert(entry.toLongLong(), text.mid(open + 1, close - open - 1));
    parent.insert(entry.toLongLong(), fields.at(1).toLongLong());
  }
  QList<qint64> result;
  for (auto it = comm.cbegin(); it != comm.cend(); ++it) {
    if (!it.value().startsWith("WebKit")) continue;
    for (qint64 p = parent.value(it.key()), depth = 0; p > 1 && depth < 16; p = parent.value(p), ++depth)
      if (p == QCoreApplication::applicationPid()) { result.append(it.key()); break; }
  }
  return result;
}

int runEngine(const QString &engine, const QString &base, int argc, char *argv[]) {
  const bool webkit = engine == QLatin1String("webkit");
  const auto engineType = webkit ? dalinira::engine::EngineType::WebKit : dalinira::engine::EngineType::Chromium;
  dalinira::engine::EngineBackend::setSelectedEngineType(engineType);
  if (webkit) QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
  else registerDaliNiraUrlSchemes();
  QApplication app(argc, argv);
  dalinira::application_identity::apply();
  const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  QDir().mkpath(dataDir);
  const std::string tag = "[" + engine.toStdString() + "] ";

  BrowserProfileService profileService(dataDir, nullptr, &app);
  dalinira::engine::EngineStorageManager engineStorage(dataDir);
  if (!webkit) {
    auto backend = std::make_unique<dalinira::engine::ChromiumBackend>(profileService.profile(), &app);
    CHECK(backend->initialize(engineStorage));
    dalinira::engine::EngineBackend::registerBackend(dalinira::engine::EngineType::Chromium, std::move(backend));
    // Tests start playback from script, without a user gesture.
    profileService.profile()->settings()->setAttribute(QWebEngineSettings::PlaybackRequiresUserGesture, false);
  }
#if defined(DALINIRA_BUILD_WEBKIT)
  else {
    auto backend = std::make_unique<dalinira::engine::WebKitBackend>(&app);
    CHECK(backend->initialize(engineStorage));
    CHECK(backend->enableNetworkFiltering());
    dalinira::engine::EngineBackend::registerBackend(dalinira::engine::EngineType::WebKit, std::move(backend));
  }
#else
  else {
    std::cout << tag << "SKIP: built without WebKit" << std::endl;
    return 0;
  }
#endif

  TabManager tabManager(&app);
  WebAudioEffectsController effects(&app, false);
  effects.setEnabled(true);
  effects.setEqualizerBand(12, 3.0);
  effects.setCompressorEnabled(true);
  BrowserServices services;
  services.profile = webkit ? nullptr : profileService.profile();
  services.profileService = &profileService;
  services.tabManager = &tabManager;
  services.audioEffects = &effects;

  auto *window = new BrowserWindow(services);
  window->resize(1100, 720);
  window->show();
  window->ensureInitialTab();
  const QUrl media(base + QStringLiteral("/media"));
  const auto loaded = [&](int index) {
    return waitUntil([&] {
      return index < window->tabCount() && window->tabInfo(index).title == QLatin1String("Media fixture") &&
             !window->tabInfo(index).isLoading;
    }, 20000);
  };
  const auto play = [&](int index, bool on) {
    // WebKitGTK mirrors an element's volume to its PipeWire stream, and the
    // desktop restores the last stream volume per application (this test
    // drives it to 0 below). Set the site's volume, as a player does, instead
    // of relying on the restored desktop state.
    runScript(*window, index, on ? QStringLiteral("(function(){const m=document.getElementById('main');m.volume=1;m.play()})()")
                                 : QStringLiteral("document.getElementById('main').pause()"));
  };

  // 1. A media page: the runtime is there, nothing attached before playback
  //    (the two preloaded elements never get a graph).
  window->navigateCurrentTab(media);
  CHECK(loaded(0));
  CHECK(waitUntil([&] { return probe(*window, 0).root; }, 5000));
  CHECK(probe(*window, 0).graphs == 0);
  play(0, true);
  CHECK(playingThroughGraph(*window, 0));
  CHECK(probe(*window, 0).marked);  // the same graph as the previous probe
  std::cout << tag << "tab 1: runtime present, one graph for the playing element, audio flows through it" << std::endl;

  // 1b. The page's own volume and mute (the site's player controls) keep
  //     working through the effects: the routed audio follows them, and a
  //     muted or silent but playing element never loses its audio.
  {
    const auto level = [&](const QString &setup) {
      runScript(*window, 0, setup);
      pump(700);
      const Probe p = probe(*window, 0);
      std::cout << tag << "  after [" << setup.toStdString() << "]: " << p.media.toStdString() << " signal=" << p.signal << std::endl;
      return p.signal;
    };
    const double full = level(QStringLiteral("document.getElementById('main').volume=1"));
    const double quarter = level(QStringLiteral("document.getElementById('main').volume=0.25"));
    const double muted = level(QStringLiteral("document.getElementById('main').volume=1;document.getElementById('main').muted=true"));
    const double unmuted = level(QStringLiteral("document.getElementById('main').muted=false"));
    std::cout << tag << "site volume: 1 -> " << full << ", 0.25 -> " << quarter << ", muted -> " << muted
              << ", unmuted -> " << unmuted << std::endl;
    CHECK(full > 1e-6);
    CHECK(quarter > full * 0.18 && quarter < full * 0.32);
    CHECK(muted < full * 0.05);
    CHECK(unmuted > full * 0.8);
    runScript(*window, 0, QStringLiteral("document.getElementById('main').volume=0.4"));
    CHECK(waitUntil([&] {
      return std::abs(evaluate(*window, 0, QStringLiteral("({v: document.getElementById('main').volume})"))
                          .value(QStringLiteral("v")).toDouble() - 0.4) < 1e-6;
    }, 2000));
    runScript(*window, 0, QStringLiteral("document.getElementById('main').volume=1"));
    // The reported sequence: the site's slider down to 0 (page timers, no
    // gesture), longer than the idle delay, then back up.
    runScript(*window, 0, QStringLiteral(
        "(function(){const m=document.getElementById('main');[0.5,0.2,0].forEach(function(v,i){"
        "setTimeout(function(){m.volume=v},200*(i+1))})})()"));
    pump(6500);
    const Probe silent = probe(*window, 0);
    CHECK(silent.playing && silent.state == QLatin1String("running") && silent.signal < full * 0.05);
    runScript(*window, 0, QStringLiteral("setTimeout(function(){document.getElementById('main').volume=1},100)"));
    CHECK(waitUntil([&] {
      const Probe p = probe(*window, 0);
      return p.state == QLatin1String("running") && p.signal > full * 0.8;
    }, 2000));
    // Muted for longer than the idle delay, then unmuted from the page.
    runScript(*window, 0, QStringLiteral("document.getElementById('main').muted=true"));
    pump(6500);
    CHECK(probe(*window, 0).state == QLatin1String("running"));
    runScript(*window, 0, QStringLiteral("setTimeout(function(){document.getElementById('main').muted=false},100)"));
    CHECK(waitUntil([&] { return probe(*window, 0).signal > full * 0.8; }, 2000));
    std::cout << tag << "site volume/mute follow through the effects; volume 0 or mute for 6.5 s, then back up: "
                 "audio returns" << std::endl;
  }

  // 2. A second media tab, and tab switches.
  window->addNewTab(media);
  CHECK(loaded(1));
  play(1, true);
  CHECK(playingThroughGraph(*window, 1));
  for (const int index : {0, 1, 0}) {
    window->switchTab(index);
    pump(300);
  }
  const Probe first = probe(*window, 0);
  CHECK(first.graphs == 1 && first.marked && first.state == QLatin1String("running") && first.signal > 1e-6);
  CHECK(probe(*window, 1).graphs == 1);
  std::cout << tag << "two tabs + switches: one graph each, never re-attached or duplicated" << std::endl;

  // 3. Background tab paused: its context goes idle, the front one keeps playing.
  play(1, false);
  CHECK(waitUntil([&] { return probe(*window, 1).state == QLatin1String("suspended"); }, 9000));
  CHECK(probe(*window, 0).state == QLatin1String("running"));
  play(1, true);
  CHECK(playingThroughGraph(*window, 1));
  std::cout << tag << "paused background tab: AudioContext idle-suspended, resumed on play" << std::endl;

  // 4. Effects off/on at runtime: media keeps playing, graph bypassed then back.
  effects.setEnabled(false);
  CHECK(waitUntil([&] {
    const Probe p = probe(*window, 0);
    return p.bypass && p.playing && p.signal > 1e-6;
  }, 5000));
  effects.setEnabled(true);
  CHECK(waitUntil([&] {
    const Probe p = probe(*window, 0);
    return !p.bypass && p.playing && p.graphs == 1;
  }, 5000));
  std::cout << tag << "effects off/on at runtime: bypass and restore, media never stopped" << std::endl;

  if (webkit) {
    std::cout << tag << "WebKit processes: " << webKitProcesses().size() << std::endl;
    CHECK(webKitProcesses().size() <= 5);  // two page processes + network (+ a brief swap)
  }
  window->closeTab(1);
  pump(300);
  CHECK(probe(*window, 0).state == QLatin1String("running"));
  play(0, false);
  window->close();
  delete window;
  dalinira::engine::EngineBackend::shutdown();
  if (webkit) CHECK(waitUntil([] { return webKitProcesses().isEmpty(); }, 25000));
  std::cout << tag << "PASS" << std::endl;
  return 0;
}

// ---------------------------------------------------------------------------
// Effect activation latency (--latency): from the moment media plays (or the
// user turns effects on) until the DALI chain is active on that element.
// ---------------------------------------------------------------------------

// Monotonic browser clock (CLOCK_MONOTONIC), ms.
QElapsedTimer &browserClock() {
  static QElapsedTimer clock = [] { QElapsedTimer t; t.start(); return t; }();
  return clock;
}
double browserNowMs() { return double(browserClock().nsecsElapsed()) / 1e6; }

// Records, in page time (performance.now(), monotonic), when element `id`
// starts playing and when its DALI chain becomes active: a graph, not
// bypassed, DSP built, context running. Checked at every runtime entry point
// that can change that, on the context's statechange, and every frame.
const char *kWatch = R"JS((function(id) {
  const root = window.__DALINIRA_WEB_DALI_OUTPUT__;
  const el = document.getElementById(id);
  if (!root || !root.graphs || !el) return false;
  const m = { playing: -1, engaged: -1, play: -1, ctx: -1, running: -1, attached: -1 };
  (window.__lat = window.__lat || {})[id] = m;
  el.addEventListener('play', function() { if (m.play < 0) m.play = performance.now(); });
  const active = function() {
    const g = root.graphs.get(el);
    const c = root.audioContext;
    return !!(g && !g.bypass && g.runtimeGain && c && c.state === 'running');
  };
  const check = function() {
    const now = performance.now();
    const c = root.audioContext;
    if (m.play >= 0 && m.ctx < 0 && c) m.ctx = now;
    if (m.play >= 0 && m.running < 0 && c && c.state === 'running') m.running = now;
    if (m.play >= 0 && m.attached < 0 && root.graphs.get(el)) m.attached = now;
    if (m.engaged < 0 && active()) m.engaged = now;
  };
  el.addEventListener('playing', function() { if (m.playing < 0) m.playing = performance.now(); check(); });
  if (!root.__latHooks) {
    root.__latHooks = [];
    const run = function() { for (const f of root.__latHooks) f(); };
    for (const name of ['processMedia', 'applyActiveParams', 'setGraphBypass', 'wakeAudio', 'resumeGraph']) {
      const original = root[name];
      if (typeof original !== 'function') continue;
      root[name] = function() { const r = original.apply(this, arguments); run(); return r; };
    }
    root.__latHooks.push(function() {
      const c = root.audioContext;
      if (c && !c.__latWatched) { c.__latWatched = true; c.addEventListener('statechange', run); }
    });
    const frame = function() { run(); requestAnimationFrame(frame); };
    requestAnimationFrame(frame);
  }
  root.__latHooks.push(check);
  for (const f of root.__latHooks) f();
  return true;
}))JS";

// For a document without the runtime yet (effects were off when it loaded):
// every frame, whether the chain is active on #main.
const char *kFrameWatch = R"JS((function() {
  const w = window.__frameLat = { engaged: -1, runtime: -1, running: -1 };
  const el = document.getElementById('main');
  const f = function() {
    const root = window.__DALINIRA_WEB_DALI_OUTPUT__;
    const g = root && root.graphs ? root.graphs.get(el) : null;
    const c = root ? root.audioContext : null;
    const now = performance.now();
    if (w.runtime < 0 && root && root.processMedia) w.runtime = now;
    if (w.running < 0 && c && c.state === 'running') w.running = now;
    if (g && !g.bypass && g.runtimeGain && c && c.state === 'running') { w.engaged = now; return; }
    requestAnimationFrame(f);
  };
  requestAnimationFrame(f);
  return true;
})())JS";

QVariantMap evaluate(BrowserWindow &window, int index, const QString &script) {
  QVariant result;
  bool done = false;
  runScript(window, index, script, [&](const QVariant &value) { result = value; done = true; });
  CHECK(waitUntil([&] { return done; }, 5000));
  return result.toMap();
}

double evaluateNumber(BrowserWindow &window, int index, const QString &script) {
  QVariant result;
  bool done = false;
  runScript(window, index, script, [&](const QVariant &value) { result = value; done = true; });
  CHECK(waitUntil([&] { return done; }, 5000));
  return result.toDouble();
}

// Page time -> browser time offset from the round trip with the smallest RTT
// (error <= RTT / 2).
struct ClockMap { double offset = 0; double rtt = 1e9; };
ClockMap calibrate(BrowserWindow &window, int index) {
  ClockMap best;
  for (int i = 0; i < 9; ++i) {
    double pageNow = -1;
    double received = -1;
    const double sent = browserNowMs();
    runScript(window, index, QStringLiteral("performance.now()"), [&](const QVariant &value) {
      received = browserNowMs();
      pageNow = value.toDouble();
    });
    CHECK(waitUntil([&] { return received >= 0; }, 5000));
    const double rtt = received - sent;
    if (rtt < best.rtt) best = ClockMap{sent + rtt / 2 - pageNow, rtt};
  }
  return best;
}

bool watch(BrowserWindow &window, int index, const char *id) {
  return evaluateNumber(window, index, QString::fromLatin1(kWatch) + QStringLiteral("('%1') ? 1 : 0").arg(QLatin1String(id))) == 1;
}

// {playing, engaged} of a watched element, once both are known.
QVariantMap watched(BrowserWindow &window, int index, const char *id, int timeoutMs = 10000) {
  QVariantMap m;
  const QString read = QStringLiteral("(function(){const m=(window.__lat||{})['%1'];return m?{playing:m.playing,engaged:m.engaged,"
                                     "play:m.play,ctx:m.ctx,running:m.running,attached:m.attached}:{}})()")
                           .arg(QLatin1String(id));
  waitUntil([&] {
    m = evaluate(window, index, read);
    return m.value(QStringLiteral("playing")).toDouble() >= 0 && m.value(QStringLiteral("engaged")).toDouble() >= 0;
  }, timeoutMs);
  return m;
}

struct Samples {
  QString name;
  QList<double> ms;
  int engagedBeforePlaying = 0;
  QString note;
};

void report(const std::string &tag, const Samples &samples) {
  QList<double> sorted = samples.ms;
  std::sort(sorted.begin(), sorted.end());
  const int n = int(sorted.size());
  const double median = n % 2 ? sorted[n / 2] : (sorted[n / 2 - 1] + sorted[n / 2]) / 2;
  std::cout << tag << "LATENCY " << samples.name.toStdString() << ": min " << sorted.front() << " ms, median "
            << median << " ms, max " << sorted.back() << " ms (n=" << n;
  if (samples.engagedBeforePlaying) std::cout << ", active before 'playing' in " << samples.engagedBeforePlaying;
  std::cout << ")";
  if (!samples.note.isEmpty()) std::cout << " " << samples.note.toStdString();
  std::cout << "  [";
  for (int i = 0; i < samples.ms.size(); ++i) std::cout << (i ? " " : "") << samples.ms[i];
  std::cout << "]" << std::endl;
}

// Latency from 'playing' to active chain (0 when the chain was ready first).
double playToActive(const QVariantMap &m, Samples &samples) {
  const double latency = m.value(QStringLiteral("engaged")).toDouble() - m.value(QStringLiteral("playing")).toDouble();
  if (latency < 0) ++samples.engagedBeforePlaying;
  return std::max(0.0, latency);
}

int runLatency(const QString &engine, const QString &base, int argc, char *argv[]) {
  browserClock();
  const bool webkit = engine == QLatin1String("webkit");
  const auto engineType = webkit ? dalinira::engine::EngineType::WebKit : dalinira::engine::EngineType::Chromium;
  dalinira::engine::EngineBackend::setSelectedEngineType(engineType);
  if (webkit) QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
  else registerDaliNiraUrlSchemes();
  // Its own audio stream identity: the desktop restores stream volumes per
  // application name, and the effects test run by this same binary leaves
  // its streams at other volumes, which would make these media elements silent.
  g_set_application_name("dalinira-browser-web-audio-latency-test");
  QApplication app(argc, argv);
  dalinira::application_identity::apply();
  const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  QDir().mkpath(dataDir);
  const std::string tag = "[" + engine.toStdString() + "] ";

  BrowserProfileService profileService(dataDir, nullptr, &app);
  dalinira::engine::EngineStorageManager engineStorage(dataDir);
  if (!webkit) {
    auto backend = std::make_unique<dalinira::engine::ChromiumBackend>(profileService.profile(), &app);
    CHECK(backend->initialize(engineStorage));
    dalinira::engine::EngineBackend::registerBackend(dalinira::engine::EngineType::Chromium, std::move(backend));
    profileService.profile()->settings()->setAttribute(QWebEngineSettings::PlaybackRequiresUserGesture, false);
  }
#if defined(DALINIRA_BUILD_WEBKIT)
  else {
    auto backend = std::make_unique<dalinira::engine::WebKitBackend>(&app);
    CHECK(backend->initialize(engineStorage));
    CHECK(backend->enableNetworkFiltering());
    dalinira::engine::EngineBackend::registerBackend(dalinira::engine::EngineType::WebKit, std::move(backend));
  }
#else
  else return 0;
#endif

  TabManager tabManager(&app);
  WebAudioEffectsController effects(&app, false);
  effects.setEnabled(true);
  effects.setEqualizerBand(12, 3.0);
  effects.setCompressorEnabled(true);
  BrowserServices services;
  services.profile = webkit ? nullptr : profileService.profile();
  services.profileService = &profileService;
  services.tabManager = &tabManager;
  services.audioEffects = &effects;

  auto *window = new BrowserWindow(services);
  window->resize(1100, 720);
  window->show();
  window->ensureInitialTab();
  const QUrl media(base + QStringLiteral("/media"));
  constexpr int kRepetitions = 6;
  const auto js = [&](const QString &script) { runScript(*window, 0, script); };
  int documentSerial = 0;
  const auto openFresh = [&](bool expectRuntime) {
    // A distinct URL per document, confirmed by the page itself: never the
    // previous document still on screen.
    const QString search = QStringLiteral("?doc=%1").arg(++documentSerial);
    window->navigateCurrentTab(QUrl(media.toString() + search));
    CHECK(waitUntil([&] {
      if (window->tabInfo(0).isLoading) return false;
      // Polled with a timeout: a late result must not outlive this frame.
      const auto state = std::make_shared<std::pair<bool, QVariant>>(false, QVariant());
      runScript(*window, 0, QStringLiteral("location.search + '|' + document.readyState"),
                [state](const QVariant &v) { state->second = v; state->first = true; });
      waitUntil([&] { return state->first; }, 3000);
      return state->second.toString() == search + QStringLiteral("|complete");
    }, 20000));
    if (expectRuntime) CHECK(waitUntil([&] { return probe(*window, 0).root; }, 5000));
  };
  // Whether the engine lets a page start media without a user gesture
  // (WebKit refuses: NotAllowedError); then playback starts as a user click
  // does (a script of the browser, which engines treat as a user gesture).
  bool autoplayRefused = false;
  const auto playResult = [&] {
    return evaluate(*window, 0, QStringLiteral("({r: String(window.__playResult)})")).value(QStringLiteral("r")).toString();
  };
  // Plays from a page timer: no user gesture (autoplay, next track).
  const auto playLater = [&](const char *id) {
    if (autoplayRefused) {
      js(QStringLiteral("document.getElementById('%1').play()").arg(QLatin1String(id)));
      return;
    }
    // The site sets its volume, as a player does: WebKitGTK mirrors element
    // volume to its PipeWire stream, and the desktop restores this test
    // binary's last stream volume, which browser-web-audio-engine-gui drives
    // to 0 (the effects runtime leaves silent elements alone).
    js(QStringLiteral("setTimeout(function(){const m=document.getElementById('%1');m.volume=1;const p=m.play();"
                      "window.__playResult='pending';if(p&&p.then)p.then(function(){window.__playResult='ok'},"
                      "function(e){window.__playResult=e.name+': '+e.message})},1200)").arg(QLatin1String(id)));
  };

  // 1. Cold start: new document, no AudioContext yet; playback without a gesture.
  Samples cold{QStringLiteral("cold start (autoplay, no gesture)")};
  for (int i = 0; i < kRepetitions; ++i) {
    openFresh(true);
    CHECK(watch(*window, 0, "main"));
    playLater("main");
    if (i == 0) {
      waitUntil([&] { return playResult() != QLatin1String("pending") && playResult() != QLatin1String("undefined"); }, 5000);
      if (playResult().startsWith(QLatin1String("NotAllowedError"))) {
        autoplayRefused = true;
        std::cout << tag << "LATENCY " << cold.name.toStdString()
                  << ": not applicable, the engine refuses play() without a user gesture (" << playResult().toStdString()
                  << ")" << std::endl;
        break;
      }
    }
    const QVariantMap m = watched(*window, 0, "main");
    if (m.value(QStringLiteral("engaged")).toDouble() < 0) {
      const Probe p = probe(*window, 0);
      std::cout << tag << "cold start not active: playing=" << p.playing << " graphs=" << p.graphs
                << " state=" << p.state.toStdString() << " mediaTime=" << p.time << " audible(engine)="
                << (window->tabInfo(0).engineView ? window->tabInfo(0).engineView->isAudible() : false) << " play(): "
                << evaluate(*window, 0, QStringLiteral("({r: String(window.__playResult)})")).value(QStringLiteral("r")).toString().toStdString()
                << std::endl;
    }
    CHECK(m.value(QStringLiteral("engaged")).toDouble() >= 0);
    cold.ms.append(playToActive(m, cold));
  }
  if (!autoplayRefused) report(tag, cold);

  // 1b. Cold start, playback started by the user (a gesture).
  Samples coldGesture{QStringLiteral("cold start (user-initiated play)")};
  for (int i = 0; i < kRepetitions; ++i) {
    openFresh(true);
    CHECK(watch(*window, 0, "main"));
    js(QStringLiteral("document.getElementById('main').play()"));
    const QVariantMap m = watched(*window, 0, "main");
    CHECK(m.value(QStringLiteral("engaged")).toDouble() >= 0);
    coldGesture.ms.append(playToActive(m, coldGesture));
    const double play = m.value(QStringLiteral("play")).toDouble();
    const auto since = [&](const char *key) {
      const double t = m.value(QString::fromLatin1(key)).toDouble();
      return t >= 0 ? QString::number(t - play, 'f', 1) : QStringLiteral("-");
    };
    std::cout << tag << "  breakdown from 'play': context " << since("ctx").toStdString() << " ms, running "
              << since("running").toStdString() << " ms, graph " << since("attached").toStdString() << " ms, 'playing' "
              << since("playing").toStdString() << " ms, active " << since("engaged").toStdString() << " ms" << std::endl;
  }
  report(tag, coldGesture);

  // 2. Warm start: the page's context runs (#main playing); a new element starts.
  Samples warm{QStringLiteral("warm start (context running, new element)")};
  CHECK(playingThroughGraph(*window, 0));
  for (int i = 0; i < kRepetitions; ++i) {
    const QString id = QStringLiteral("w%1").arg(i);
    js(QStringLiteral("(function(){const a=document.createElement('audio');a.id='%1';a.loop=true;"
                      "a.src='/tone.wav?w=%1';document.body.appendChild(a);})()").arg(id));
    CHECK(watch(*window, 0, id.toLatin1().constData()));
    if (autoplayRefused) js(QStringLiteral("document.getElementById('%1').play()").arg(id));
    else js(QStringLiteral("setTimeout(function(){document.getElementById('%1').play()},300)").arg(id));
    const QVariantMap m = watched(*window, 0, id.toLatin1().constData());
    CHECK(m.value(QStringLiteral("engaged")).toDouble() >= 0);
    warm.ms.append(playToActive(m, warm));
    js(QStringLiteral("document.getElementById('%1').pause()").arg(id));
  }
  if (autoplayRefused) warm.note = QStringLiteral("(user-initiated play)");
  report(tag, warm);

  // 3. Paused -> resumed after the idle suspend (the context was suspended;
  //    until it runs again the element is silent).
  Samples resume{QStringLiteral("pause -> resume (context idle-suspended)")};
  for (int i = 0; i < kRepetitions; ++i) {
    js(QStringLiteral("document.querySelectorAll('audio').forEach(function(a){a.pause()})"));
    CHECK(waitUntil([&] { return probe(*window, 0).state == QLatin1String("suspended"); }, 9000));
    CHECK(watch(*window, 0, "main"));
    playLater("main");
    const QVariantMap m = watched(*window, 0, "main");
    CHECK(m.value(QStringLiteral("engaged")).toDouble() >= 0);
    resume.ms.append(playToActive(m, resume));
  }
  if (autoplayRefused) resume.note = QStringLiteral("(user-initiated play)");
  report(tag, resume);

  // 4. Effects OFF -> ON while #main plays (runtime already in the page, graph bypassed).
  Samples toggle{QStringLiteral("effects OFF -> ON while playing (graph bypassed)")};
  double worstRtt = 0;
  for (int i = 0; i < kRepetitions; ++i) {
    effects.setEnabled(false);
    CHECK(waitUntil([&] { const Probe p = probe(*window, 0); return p.bypass && p.playing; }, 5000));
    pump(400);
    CHECK(watch(*window, 0, "main"));
    const ClockMap clock = calibrate(*window, 0);
    worstRtt = std::max(worstRtt, clock.rtt);
    const double t0 = browserNowMs();
    effects.setEnabled(true);
    const QVariantMap m = watched(*window, 0, "main", 5000);
    const double engaged = m.value(QStringLiteral("engaged")).toDouble();
    CHECK(engaged >= 0);
    toggle.ms.append(engaged + clock.offset - t0);
  }
  toggle.note = QStringLiteral("(clock error <= %1 ms)").arg(worstRtt / 2, 0, 'f', 2);
  report(tag, toggle);

  // 4b. Effects OFF -> ON while playing on a page loaded with effects off
  //     (no runtime in the page yet; detection per animation frame).
  Samples fresh{QStringLiteral("effects OFF -> ON while playing (page loaded with effects off)")};
  worstRtt = 0;
  for (int i = 0; i < kRepetitions; ++i) {
    effects.setEnabled(false);
    openFresh(false);
    CHECK(!probe(*window, 0).root);
    js(QStringLiteral("document.getElementById('main').play()"));
    CHECK(waitUntil([&] { const Probe p = probe(*window, 0); return p.playing && p.time > 0.2; }, 8000));
    CHECK(evaluateNumber(*window, 0, QString::fromLatin1(kFrameWatch) + QStringLiteral(" ? 1 : 0")) == 1);
    const ClockMap clock = calibrate(*window, 0);
    worstRtt = std::max(worstRtt, clock.rtt);
    const double t0 = browserNowMs();
    effects.setEnabled(true);
    double engaged = -1;
    CHECK(waitUntil([&] {
      engaged = evaluateNumber(*window, 0, QStringLiteral("window.__frameLat ? window.__frameLat.engaged : -1"));
      return engaged >= 0;
    }, 8000));
    fresh.ms.append(engaged + clock.offset - t0);
    const QVariantMap w = evaluate(*window, 0, QStringLiteral("({runtime: window.__frameLat.runtime, running: window.__frameLat.running})"));
    std::cout << tag << "  breakdown from switch-on: runtime in page "
              << (w.value(QStringLiteral("runtime")).toDouble() + clock.offset - t0) << " ms, context running "
              << (w.value(QStringLiteral("running")).toDouble() + clock.offset - t0) << " ms, active "
              << (engaged + clock.offset - t0) << " ms" << std::endl;
  }
  fresh.note = QStringLiteral("(frame resolution ~17 ms; clock error <= %1 ms)").arg(worstRtt / 2, 0, 'f', 2);
  report(tag, fresh);

  js(QStringLiteral("document.querySelectorAll('audio').forEach(function(a){a.pause()})"));
  window->close();
  delete window;
  dalinira::engine::EngineBackend::shutdown();
  if (webkit) CHECK(waitUntil([] { return webKitProcesses().isEmpty(); }, 25000));
  std::cout << tag << "latency measurement done" << std::endl;
  return 0;
}

// ---------------------------------------------------------------------------
// Parent: local media, one child per engine in turn.
// ---------------------------------------------------------------------------

class LocalSite {
 public:
  LocalSite() : tone_(quietToneWav()) {
    CHECK(server_.listen(QHostAddress::LocalHost));
    QObject::connect(&server_, &QTcpServer::newConnection, [this] {
      while (QTcpSocket *socket = server_.nextPendingConnection()) {
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
          // Headers may arrive in several packets.
          QByteArray request = socket->property("request").toByteArray() + socket->readAll();
          socket->setProperty("request", request);
          if (!request.contains("\r\n\r\n")) return;
          const QByteArray path = request.split(' ').value(1);
          QByteArray body;
          QByteArray type = "text/html";
          if (path.startsWith("/tone.wav")) {
            body = tone_;
            type = "audio/wav";
          } else if (path == "/media" || path.startsWith("/media?")) {
            body = "<!doctype html><title>Media fixture</title><audio id=main src=/tone.wav loop></audio>"
                   "<audio id=b preload=auto src=/tone.wav?a=1></audio><audio id=c preload=auto src=/tone.wav?a=2></audio>";
          }
          QByteArray status = body.isEmpty() ? "404 Not Found" : "200 OK";
          QByteArray extra = "Accept-Ranges: bytes\r\n";
          // Media stacks (GStreamer's HTTP source) read media by ranges.
          const int rangeAt = request.toLower().indexOf("\r\nrange: bytes=");
          if (!body.isEmpty() && rangeAt >= 0) {
            const QByteArray spec = request.mid(rangeAt + 15).split('\r').value(0).trimmed();
            const qint64 total = body.size();
            const qint64 first = spec.split('-').value(0).toLongLong();
            const QByteArray lastText = spec.split('-').value(1);
            const qint64 last = lastText.isEmpty() ? total - 1 : std::min(total - 1, lastText.toLongLong());
            if (first >= total || first > last) {
              status = "416 Range Not Satisfiable";
              extra += "Content-Range: bytes */" + QByteArray::number(total) + "\r\n";
              body.clear();
            } else {
              status = "206 Partial Content";
              extra += "Content-Range: bytes " + QByteArray::number(first) + "-" + QByteArray::number(last) + "/" +
                       QByteArray::number(total) + "\r\n";
              body = body.mid(first, last - first + 1);
            }
          }
          const bool head = request.startsWith("HEAD ");
          socket->write("HTTP/1.1 " + status + "\r\nContent-Type: " + type + "\r\n" + extra +
                        "Connection: close\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" +
                        (head ? QByteArray() : body));
          socket->disconnectFromHost();
        });
        QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
      }
    });
  }
  QString base() const { return QStringLiteral("http://127.0.0.1:%1").arg(server_.serverPort()); }

 private:
  QTcpServer server_;
  QByteArray tone_;
};

int runAll(int argc, char *argv[], bool latency) {
  QCoreApplication app(argc, argv);
  QTemporaryDir home;
  CHECK(home.isValid());
  LocalSite site;
  std::cout << "=== WEB AUDIO EFFECTS ENGINE (GUI, Chromium then WebKit) ===" << std::endl;
  QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
  environment.insert(QStringLiteral("XDG_CONFIG_HOME"), home.filePath(QStringLiteral("config")));
  environment.insert(QStringLiteral("XDG_DATA_HOME"), home.filePath(QStringLiteral("data")));
  environment.insert(QStringLiteral("XDG_CACHE_HOME"), home.filePath(QStringLiteral("cache")));
  environment.insert(QStringLiteral("DALINIRA_ALLOW_LOCAL_AUDIO_TEST"), QStringLiteral("1"));
  QStringList engines{QStringLiteral("chromium"), QStringLiteral("webkit")};
  if (qEnvironmentVariableIsSet("DALINIRA_AUDIO_GUI_ENGINES"))
    engines = QString::fromLocal8Bit(qgetenv("DALINIRA_AUDIO_GUI_ENGINES")).split(QLatin1Char(','));
  for (const QString &engine : std::as_const(engines)) {
    QProcess child;
    child.setProcessEnvironment(environment);
    child.setProcessChannelMode(QProcess::ForwardedChannels);
    QEventLoop loop;
    QObject::connect(&child, &QProcess::finished, &loop, &QEventLoop::quit);
    QObject::connect(&child, &QProcess::errorOccurred, &loop, &QEventLoop::quit);
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    child.start(QCoreApplication::applicationFilePath(),
                {latency ? QStringLiteral("--latency-engine") : QStringLiteral("--engine"), engine, site.base()});
    timeout.start(latency ? 240000 : 120000);
    loop.exec();
    if (child.state() != QProcess::NotRunning) {
      std::cout << "[FAIL] " << engine.toStdString() << " timed out" << std::endl;
      child.kill();
      child.waitForFinished(5000);
      return 1;
    }
    if (child.exitStatus() != QProcess::NormalExit || child.exitCode() != 0) {
      std::cout << "[FAIL] " << engine.toStdString() << " exit " << child.exitCode() << std::endl;
      return 1;
    }
  }
  std::cout << "=== WEB AUDIO EFFECTS ENGINE GUI TEST PASSED (same checks on both engines) ===" << std::endl;
  return 0;
}

}  // namespace

int main(int argc, char *argv[]) {
  for (int i = 1; i + 2 < argc; ++i) {
    if (QByteArray(argv[i]) == "--engine")
      return runEngine(QString::fromLocal8Bit(argv[i + 1]), QString::fromLocal8Bit(argv[i + 2]), argc, argv);
    if (QByteArray(argv[i]) == "--latency-engine")
      return runLatency(QString::fromLocal8Bit(argv[i + 1]), QString::fromLocal8Bit(argv[i + 2]), argc, argv);
  }
  bool latency = false;
  for (int i = 1; i < argc; ++i) latency = latency || QByteArray(argv[i]) == "--latency";
  return runAll(argc, argv, latency);
}
