// DaliNira memory profiler (developer tool, not shipped).
//
// Drives a real BrowserWindow with the same engine initialization as main.cpp
// and reports PSS / private (USS) / RSS for the browser process and every
// engine child process, grouped by role, after each scenario:
//
//   baseline_newtab   one New Tab
//   static_page       one Wikipedia article (a normal static page)
//   youtube           plus a playing YouTube video
//   inactive_tabs     plus five background articles, measured soon after and
//                     after the background policy had time to act
//   other_tabs_slept  after "Sleep all other tabs"
//   close_cycles      repeated open/load/close of a tab
//
// Usage: dalinira-memory-profile --engine chromium|webkit [--out file.json]
//                                [--late-wait seconds] [--cycles n]

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextStream>
#include <cstdio>
#include <functional>
#include <iostream>
#include <malloc.h>
#include <unistd.h>

#include "audio/web_audio_effects_controller.h"
#include "blocker/dalinira_blocker_service.h"
#include "browser_window.h"
#include "core/adult_content_protection.h"
#include "core/browser_profile_service.h"
#include "core/web_engine_hardware_acceleration.h"
#include "core/web_engine_memory_policy.h"
#include "desktop_tabs/tab_manager.h"
#include "downloads/media_download_service.h"
#include "engine/chromium/chromium_backend.h"
#include "engine/engine_backend.h"
#include "engine/engine_storage_manager.h"
#include "eq/eq_preset_repository.h"
#include "newtab/new_tab_scheme.h"
#include "pulse/song_finder_settings.h"
#include "pulse/song_recognition_service.h"
#include "session/session_store.h"
#if defined(DALINIRA_BUILD_WEBKIT)
#include "engine/webkit/webkit_backend.h"
#endif

namespace {

struct Sample {
  qint64 pss = 0;
  qint64 uss = 0;
  qint64 rss = 0;
};

Sample readRollup(qint64 pid) {
  Sample sample;
  QFile rollup(QStringLiteral("/proc/%1/smaps_rollup").arg(pid));
  if (!rollup.open(QIODevice::ReadOnly | QIODevice::Text)) return sample;
  QTextStream input(&rollup);
  QString line;
  while (input.readLineInto(&line)) {
    const QStringList f = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (f.size() < 2) continue;
    const qint64 kib = f[1].toLongLong();
    if (f[0] == QLatin1String("Pss:")) sample.pss = kib;
    else if (f[0] == QLatin1String("Rss:")) sample.rss = kib;
    else if (f[0] == QLatin1String("Private_Clean:") || f[0] == QLatin1String("Private_Dirty:")) sample.uss += kib;
  }
  return sample;
}

qint64 parentOf(qint64 pid) {
  QFile stat(QStringLiteral("/proc/%1/stat").arg(pid));
  if (!stat.open(QIODevice::ReadOnly)) return -1;
  const QByteArray text = stat.readAll();
  const int close = text.lastIndexOf(')');
  const QList<QByteArray> fields = text.mid(close + 2).split(' ');
  return fields.size() > 1 ? fields[1].toLongLong() : -1;
}

QString roleOf(qint64 pid) {
  QFile cmd(QStringLiteral("/proc/%1/cmdline").arg(pid));
  if (!cmd.open(QIODevice::ReadOnly)) return {};
  const QByteArray raw = cmd.readAll();
  const QString line = QString::fromLocal8Bit(raw).replace(QChar(0), QChar(' '));
  if (line.contains(QLatin1String("WebKitWebProcess"))) return QStringLiteral("webkit-web");
  if (line.contains(QLatin1String("WebKitNetworkProcess"))) return QStringLiteral("webkit-network");
  if (line.contains(QLatin1String("WebKitGPUProcess"))) return QStringLiteral("webkit-gpu");
  if (line.contains(QLatin1String("--type=renderer"))) return QStringLiteral("chromium-renderer");
  if (line.contains(QLatin1String("--type=gpu-process"))) return QStringLiteral("chromium-gpu");
  if (line.contains(QLatin1String("--type=utility"))) return QStringLiteral("chromium-utility");
  if (line.contains(QLatin1String("--type=zygote"))) return QStringLiteral("chromium-zygote");
  return QStringLiteral("other:") + QFileInfo(line.section(QLatin1Char(' '), 0, 0)).fileName();
}

QJsonObject measure(const QString &label) {
  const qint64 self = getpid();
  QHash<qint64, qint64> parents;
  for (const QString &entry : QDir(QStringLiteral("/proc")).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
    bool ok = false;
    const qint64 pid = entry.toLongLong(&ok);
    if (ok) parents.insert(pid, parentOf(pid));
  }
  const auto descends = [&](qint64 pid) {
    for (int depth = 0; depth < 16 && pid > 1; ++depth) {
      pid = parents.value(pid, -1);
      if (pid == self) return true;
    }
    return false;
  };
  QJsonObject groups;
  QJsonArray processes;
  qint64 totalPss = 0;
  qint64 totalUss = 0;
  const Sample browser = readRollup(self);
  totalPss += browser.pss;
  totalUss += browser.uss;
  groups.insert(QStringLiteral("browser"), QJsonObject{{QStringLiteral("count"), 1},
                                                       {QStringLiteral("pss_kib"), browser.pss},
                                                       {QStringLiteral("uss_kib"), browser.uss},
                                                       {QStringLiteral("rss_kib"), browser.rss}});
  for (auto it = parents.cbegin(); it != parents.cend(); ++it) {
    if (!descends(it.key())) continue;
    const QString role = roleOf(it.key());
    if (role.isEmpty()) continue;
    const Sample s = readRollup(it.key());
    processes.append(QJsonObject{{QStringLiteral("pid"), it.key()}, {QStringLiteral("role"), role},
                                 {QStringLiteral("pss_kib"), s.pss}, {QStringLiteral("uss_kib"), s.uss}});
    QJsonObject g = groups.value(role).toObject();
    g[QStringLiteral("count")] = g.value(QStringLiteral("count")).toInt() + 1;
    g[QStringLiteral("pss_kib")] = g.value(QStringLiteral("pss_kib")).toInteger() + s.pss;
    g[QStringLiteral("uss_kib")] = g.value(QStringLiteral("uss_kib")).toInteger() + s.uss;
    g[QStringLiteral("rss_kib")] = g.value(QStringLiteral("rss_kib")).toInteger() + s.rss;
    groups.insert(role, g);
    totalPss += s.pss;
    totalUss += s.uss;
  }
  QJsonObject result{{QStringLiteral("label"), label}, {QStringLiteral("groups"), groups},
                     {QStringLiteral("processes"), processes},
                     {QStringLiteral("total_pss_kib"), totalPss}, {QStringLiteral("total_uss_kib"), totalUss}};
  std::cout << "\n== " << label.toStdString() << ": total PSS " << totalPss / 1024 << " MiB, USS "
            << totalUss / 1024 << " MiB" << std::endl;
  for (const QJsonValue &process : std::as_const(processes)) {
    const QJsonObject p = process.toObject();
    std::printf("     pid %-7lld %-20s PSS %6lld MiB\n", static_cast<long long>(p.value(QStringLiteral("pid")).toInteger()),
                qPrintable(p.value(QStringLiteral("role")).toString()),
                static_cast<long long>(p.value(QStringLiteral("pss_kib")).toInteger() / 1024));
  }
  for (auto it = groups.begin(); it != groups.end(); ++it) {
    const QJsonObject g = it.value().toObject();
    std::printf("   %-22s n=%-2d PSS %6lld MiB  USS %6lld MiB  RSS %6lld MiB\n", qPrintable(it.key()),
                g.value(QStringLiteral("count")).toInt(),
                static_cast<long long>(g.value(QStringLiteral("pss_kib")).toInteger() / 1024),
                static_cast<long long>(g.value(QStringLiteral("uss_kib")).toInteger() / 1024),
                static_cast<long long>(g.value(QStringLiteral("rss_kib")).toInteger() / 1024));
  }
  std::fflush(stdout);
  return result;
}

// processEvents() alone never runs deleteLater() outside exec(); the real
// browser runs exec(), so deliver deferred deletions exactly as it would.
void pump(int ms) {
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < ms) {
    QApplication::processEvents(QEventLoop::AllEvents, 50);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  }
}

bool waitUntil(const std::function<bool()> &condition, int timeoutMs) {
  QElapsedTimer timer;
  timer.start();
  while (!condition()) {
    if (timer.elapsed() > timeoutMs) return false;
    QApplication::processEvents(QEventLoop::AllEvents, 50);
  }
  return true;
}

bool tabLoaded(BrowserWindow &window, int index) {
  return index < window.tabCount() && !window.tabInfo(index).isLoading;
}

}  // namespace

void stage(const char *name) {
  const Sample self = readRollup(getpid());
  std::printf("[stage] %-34s browser USS %6lld MiB  PSS %6lld MiB\n", name, static_cast<long long>(self.uss / 1024),
              static_cast<long long>(self.pss / 1024));
  std::fflush(stdout);
}

int main(int argc, char *argv[]) {
  QString engineName = QStringLiteral("chromium");
  QString outPath;
  int lateWaitSeconds = 75;
  int cycles = 20;
  for (int i = 1; i < argc; ++i) {
    const QString a = QString::fromLocal8Bit(argv[i]);
    if (a == QLatin1String("--engine") && i + 1 < argc) engineName = QString::fromLocal8Bit(argv[++i]);
    else if (a == QLatin1String("--out") && i + 1 < argc) outPath = QString::fromLocal8Bit(argv[++i]);
    else if (a == QLatin1String("--late-wait") && i + 1 < argc) lateWaitSeconds = QString::fromLocal8Bit(argv[++i]).toInt();
    else if (a == QLatin1String("--cycles") && i + 1 < argc) cycles = QString::fromLocal8Bit(argv[++i]).toInt();
  }
  const bool webkit = engineName == QLatin1String("webkit");

  // Isolated configuration and data, as a fresh profile.
  QTemporaryDir home;
  qputenv("XDG_CONFIG_HOME", home.filePath(QStringLiteral("config")).toUtf8());
  qputenv("XDG_DATA_HOME", home.filePath(QStringLiteral("data")).toUtf8());
  qputenv("XDG_CACHE_HOME", home.filePath(QStringLiteral("cache")).toUtf8());
  if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "xcb");
  qputenv("GDK_BACKEND", "x11");

  const auto engineType = webkit ? dalinira::engine::EngineType::WebKit : dalinira::engine::EngineType::Chromium;
  dalinira::engine::EngineBackend::setSelectedEngineType(engineType);
  if (!webkit) {
    const QString appDir = QFileInfo(QString::fromLocal8Bit(argv[0])).dir().absolutePath();
    dalinira::WebEngineMemoryPolicy::configureSubprocessLauncher(appDir);
    dalinira::WebEngineHardwareAcceleration::initializeEarlyRuntime();
    registerDaliNiraUrlSchemes();
  } else {
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
  }
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName(QStringLiteral("DaliNira"));
  QCoreApplication::setApplicationName(QStringLiteral("DaliNira Browser"));
  stage("QApplication");
  if (qEnvironmentVariableIntValue("DALINIRA_PROFILE_ATTRIBUTE") == 1) {
    // Component attribution of DaliNira's own browser-process services.
    QTemporaryDir scratch;
    dalinira::core::AdultContentProtectionService::instance().isBlockedHost(QStringLiteral("example.com"));
    stage("adult-content snapshot loaded");
    {
      DaliNiraBlockerService blocker(scratch.path());
      stage("blocker service (rules compiled)");
      malloc_trim(0);
      stage("  after malloc_trim");
      blocker.cosmeticCssForHost(QStringLiteral("www.youtube.com"));
      blocker.createScriptingScriptsForHost(QStringLiteral("www.youtube.com"));
      stage("blocker scripts for one host");
      std::printf("        blocker estimatedMemoryBytes=%lld MiB rules=%d\n",
                  static_cast<long long>(blocker.estimatedMemoryBytes() / (1024 * 1024)), blocker.filterEngine()->ruleCount());
    }
    stage("blocker destroyed");
    malloc_trim(0);
    stage("  after malloc_trim");
    return 0;
  }

  const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  QDir().mkpath(dataDir);
  BrowserProfileService profileService(dataDir, nullptr, &app);
  stage("BrowserProfileService (blocker, history)");
  dalinira::engine::EngineStorageManager engineStorage(dataDir);
  if (!webkit) {
    auto backend = std::make_unique<dalinira::engine::ChromiumBackend>(profileService.profile(), &app);
    backend->initialize(engineStorage);
    dalinira::engine::EngineBackend::registerBackend(dalinira::engine::EngineType::Chromium, std::move(backend));
  }
#if defined(DALINIRA_BUILD_WEBKIT)
  else {
    auto backend = std::make_unique<dalinira::engine::WebKitBackend>(&app);
    backend->initialize(engineStorage);
    backend->enableNetworkFiltering();
    dalinira::engine::EngineBackend::registerBackend(dalinira::engine::EngineType::WebKit, std::move(backend));
  }
#endif

  stage("engine backend");
  TabManager tabManager(&app);
  SessionStore sessionStore(dataDir, webkit ? QStringLiteral("webkit") : QStringLiteral("chromium"));
  auto *audioEffects = new WebAudioEffectsController(&app);
  auto *eqPresetRepo = new EqPresetRepository;
  auto *songFinderSettings = new SongFinderSettings(&app);
  auto *songRecognition = new SongRecognitionService(songFinderSettings, &app);
  auto *mediaDownload = new MediaDownloadService(dataDir, &app);
  BrowserServices services;
  services.profile = profileService.profile();
  services.profileService = &profileService;
  services.tabManager = &tabManager;
  services.sessionStore = &sessionStore;
  services.audioEffects = audioEffects;
  services.eqPresetRepo = eqPresetRepo;
  services.songFinderSettings = songFinderSettings;
  services.songRecognition = songRecognition;
  services.mediaDownload = mediaDownload;
  stage("feature services");

  QJsonArray results;
  {
    auto *window = new BrowserWindow(services);
    window->resize(1280, 820);
    window->show();
    pump(1500);
    stage("BrowserWindow shown");
    window->addNewTab(QUrl(QStringLiteral("dalinira://newtab/")));
    pump(8000);
    stage("first New Tab");
    results.append(measure(QStringLiteral("baseline_newtab")));

    window->navigateCurrentTab(QUrl(QStringLiteral("https://en.wikipedia.org/wiki/Web_browser")));
    waitUntil([&] { return tabLoaded(*window, 0); }, 30000);
    pump(8000);
    results.append(measure(QStringLiteral("static_page")));

    window->addNewTab(QUrl(QStringLiteral("https://www.youtube.com/watch?v=aqz-KE-bpKQ")));
    waitUntil([&] { return tabLoaded(*window, 1); }, 30000);
    pump(15000);
    results.append(measure(QStringLiteral("youtube_playing")));

    const QStringList articles = {QStringLiteral("Memory_management"), QStringLiteral("Operating_system"),
                                  QStringLiteral("Linux"), QStringLiteral("Qt_(software)"),
                                  QStringLiteral("WebKit")};
    for (const QString &article : articles) {
      window->addNewTab(QUrl(QStringLiteral("https://en.wikipedia.org/wiki/") + article));
      waitUntil([&] { return tabLoaded(*window, window->tabCount() - 1); }, 30000);
    }
    // Back to the first article: the YouTube tab keeps playing in the
    // background and the five new articles become inactive.
    window->switchTab(0);
    pump(10000);
    results.append(measure(QStringLiteral("inactive_tabs_10s")));
    pump(lateWaitSeconds * 1000);
    results.append(measure(QStringLiteral("inactive_tabs_%1s").arg(10 + lateWaitSeconds)));

    // Explicitly sleep every other tab (tab context menu action).
    const int slept = window->sleepOtherTabs(0);
    pump(10000);
    std::cout << "slept " << slept << " tab(s)" << std::endl;
    results.append(measure(QStringLiteral("other_tabs_slept")));

    while (window->tabCount() > 1) window->closeTab(window->tabCount() - 1);
    pump(8000);
    results.append(measure(QStringLiteral("after_closing_to_one_tab")));

    for (int i = 1; i <= cycles; ++i) {
      window->addNewTab(QUrl(QStringLiteral("https://en.wikipedia.org/wiki/Special:Random")));
      waitUntil([&] { return tabLoaded(*window, window->tabCount() - 1); }, 30000);
      pump(1000);
      window->closeTab(window->tabCount() - 1);
      pump(500);
      if (i == cycles / 2 || i == cycles) {
        pump(6000);
        results.append(measure(QStringLiteral("close_cycles_%1").arg(i)));
      }
    }
    delete window;
    app.processEvents();
  }
  dalinira::engine::EngineBackend::shutdown();

  const QJsonObject report{{QStringLiteral("engine"), engineName}, {QStringLiteral("results"), results}};
  if (!outPath.isEmpty()) {
    QFile out(outPath);
    if (out.open(QIODevice::WriteOnly)) out.write(QJsonDocument(report).toJson());
  }
  return 0;
}
