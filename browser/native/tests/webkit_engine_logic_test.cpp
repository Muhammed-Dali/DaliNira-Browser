// Engine selection, storage isolation, UI-profile isolation, engine resolution
// and adult-content domain rules: the engine-neutral logic the WebKit backend
// relies on. No window and no engine process is created (offscreen, part of
// the default ctest run). The real WebKit page tests are in
// webkit_backend_test.cpp (GUI tier, scripts/safe-gui-test.sh).

#include <QApplication>
#include <QSettings>
#include <QTemporaryDir>
#include <cassert>
#include <iostream>

#include "core/adult_content_protection.h"
#include "core/engine_ui_profile.h"
#include "engine/engine_backend.h"
#include "engine/engine_resolver.h"
#include "engine/engine_storage_manager.h"
#include "engine/webkit/webkit_network_filter.h"

#include <QElapsedTimer>
#include <functional>
#include <QLocalSocket>
#include <unistd.h>
#include "engine/webkit/extension/web_audio_latency.h"

int main(int argc, char *argv[]) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app(argc, argv);
  QTemporaryDir settingsDirectory;
  assert(settingsDirectory.isValid());
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDirectory.path());
  QCoreApplication::setOrganizationName(QStringLiteral("DaliNiraTests"));
  QCoreApplication::setApplicationName(QStringLiteral("WebKitEngineLogic"));
  using namespace dalinira::engine;
  std::cout << "=== RUNNING ENGINE LOGIC TEST SUITE ===" << std::endl;
  {
    using namespace dalinira::engine::webkit;
    installWebAudioLatencyPolicy();
    // Construct sinks without opening the sound server or any audio device.
    GstElement *sink = gst_element_factory_make("pulsesink", nullptr);
    assert(sink);
    g_object_set(sink, "buffer-time", static_cast<gint64>(100000),
                 "latency-time", static_cast<gint64>(20000), nullptr);
    GstElement *normal = gst_pipeline_new("regular-media");
    gst_bin_add(GST_BIN(normal), sink);
    gint64 buffer = 0, latency = 0;
    g_object_get(sink, "buffer-time", &buffer, nullptr);
    assert(buffer == 100000);
    gst_object_ref(sink);
    gst_bin_remove(GST_BIN(normal), sink);
    GstElement *audio = gst_pipeline_new("audio-destination-0");
    GstElement *output = gst_bin_new("auto-output");
    gst_bin_add(GST_BIN(output), sink);
    gst_bin_add(GST_BIN(audio), output);
    g_object_get(sink, "buffer-time", &buffer, "latency-time", &latency, nullptr);
    assert(buffer == 30000 && latency == 10000);
    g_object_set(sink, "buffer-time", static_cast<gint64>(15000), nullptr);
    tuneWebAudioOutput(output);
    g_object_get(sink, "buffer-time", &buffer, nullptr);
    assert(buffer == 15000); // never increase a smaller user/device buffer
    gst_object_unref(audio);
    gst_object_unref(normal);
    std::cout << "[PASS] Only Web Audio output buffers are reduced before playback." << std::endl;
  }

  std::cout << "[TEST 1] Engine identities..." << std::endl;
  assert(engineTypeName(EngineType::Chromium) == "Chromium");
  assert(engineLayoutEngineName(EngineType::Chromium) == "Blink");
  assert(engineJsEngineName(EngineType::Chromium) == "V8");
  assert(engineTypeName(EngineType::WebKit) == "WebKit");
  assert(engineLayoutEngineName(EngineType::WebKit) == "WebCore");
  assert(engineJsEngineName(EngineType::WebKit) == "JavaScriptCore");
  std::cout << "[PASS] WebKit -> WebCore + JavaScriptCore, Chromium -> Blink + V8." << std::endl;

  std::cout << "[TEST 2] Default engine is Chromium..." << std::endl;
  assert(EngineBackend::selectedEngineType() == EngineType::Chromium);
  std::cout << "[PASS] Chromium is the default engine." << std::endl;

  std::cout << "[TEST 3] Profile storage isolation..." << std::endl;
  QTemporaryDir tempDir;
  assert(tempDir.isValid());
  EngineStorageManager storage(tempDir.path());
  assert(storage.sharedDir().endsWith("/shared"));
  assert(storage.engineDir(EngineType::Chromium).endsWith("/chromium"));
  assert(storage.engineDir(EngineType::WebKit).endsWith("/webkit"));
  assert(storage.cookiesPath(EngineType::WebKit).contains("/webkit/cookies"));
  assert(storage.cacheDir(EngineType::WebKit).contains("/webkit/cache"));
  storage.ensureDirectories(EngineType::WebKit);
  storage.ensureDirectories(EngineType::Chromium);
  std::cout << "[PASS] Chromium and WebKit profile directories are isolated." << std::endl;

  std::cout << "[TEST 4] Adult content protection domain rules..." << std::endl;
  auto &adultService = dalinira::core::AdultContentProtectionService::instance();
  adultService.addDomain(QStringLiteral("evil-adult-test-site.org"));
  adultService.addAllowDomain(QStringLiteral("safe-adult-education.org"));
  assert(adultService.isBlocked(QUrl(QStringLiteral("https://evil-adult-test-site.org/watch"))));
  assert(adultService.isBlocked(QUrl(QStringLiteral("https://media.sub.evil-adult-test-site.org/stream"))));
  assert(!adultService.isBlocked(QUrl(QStringLiteral("https://not-evil-adult-test-site.org/home"))));
  // Search queries and paths that merely contain a blocked domain are not blocked.
  assert(!adultService.isBlocked(QUrl(QStringLiteral("https://www.google.com/search?q=evil-adult-test-site.org"))));
  assert(!adultService.isBlocked(QUrl(QStringLiteral("https://duckduckgo.com/?q=evil-adult-test-site.org"))));
  assert(!adultService.isBlocked(QUrl(QStringLiteral("https://example.com/research/evil-adult-test-site.org"))));
  assert(!adultService.isBlocked(QUrl(QStringLiteral("https://safe-adult-education.org/study"))));
  std::cout << "[PASS] Hosts, subdomains, lookalikes, queries and the allowlist behave correctly." << std::endl;

  std::cout << "[TEST 5] Engine settings persistence..." << std::endl;
  QSettings().setValue(QStringLiteral("browser/engine"), QStringLiteral("webkit"));
  assert(QSettings().value(QStringLiteral("browser/engine")).toString() == QStringLiteral("webkit"));
  QSettings().setValue(QStringLiteral("browser/engine"), QStringLiteral("chromium"));
  assert(QSettings().value(QStringLiteral("browser/engine")).toString() == QStringLiteral("chromium"));
  std::cout << "[PASS] browser/engine persists." << std::endl;

  std::cout << "[TEST 6] Engine UI profile visual setting isolation..." << std::endl;
  {
    EngineBackend::setSelectedEngineType(EngineType::Chromium);
    dalinira::ui_profile::setVisualValue(QStringLiteral("tabStyle"), QStringLiteral("classic"));
    assert(dalinira::ui_profile::getVisualValue(QStringLiteral("tabStyle")).toString() == "classic");
    EngineBackend::setSelectedEngineType(EngineType::WebKit);
    dalinira::ui_profile::setVisualValue(QStringLiteral("tabStyle"), QStringLiteral("compact"));
    assert(dalinira::ui_profile::getVisualValue(QStringLiteral("tabStyle")).toString() == "compact");
    EngineBackend::setSelectedEngineType(EngineType::Chromium);
    assert(dalinira::ui_profile::getVisualValue(QStringLiteral("tabStyle")).toString() == "classic");
    EngineBackend::setSelectedEngineType(EngineType::WebKit);
    assert(dalinira::ui_profile::getVisualValue(QStringLiteral("tabStyle")).toString() == "compact");

    // Legacy unscoped appearance belongs only to Chromium and must never seed
    // a fresh WebKit UI profile.
    QSettings settings;
    settings.setValue(QStringLiteral("legacyVisualIsolationProbe"), QStringLiteral("chromium-legacy"));
    EngineBackend::setSelectedEngineType(EngineType::Chromium);
    assert(dalinira::ui_profile::getVisualValue(QStringLiteral("legacyVisualIsolationProbe")).toString() ==
           QStringLiteral("chromium-legacy"));
    EngineBackend::setSelectedEngineType(EngineType::WebKit);
    assert(dalinira::ui_profile::getVisualValue(QStringLiteral("legacyVisualIsolationProbe"),
                                                QStringLiteral("webkit-default")).toString() ==
           QStringLiteral("webkit-default"));
    settings.remove(QStringLiteral("legacyVisualIsolationProbe"));
    EngineBackend::setSelectedEngineType(EngineType::Chromium);
  }
  std::cout << "[PASS] Visual settings are isolated between Chromium and WebKit." << std::endl;

  std::cout << "[TEST 7] EngineResolver precedence and persistence..." << std::endl;
  {
    const QStringList plain{QStringLiteral("dalinira-browser")};
    const auto rDefault = EngineResolver::resolvePure(QString{}, plain);
    assert(rDefault.engine == EngineType::Chromium && !rDefault.isCliOverride &&
           rDefault.source == EngineResolutionSource::Default);
    const auto rPersistedWebKit = EngineResolver::resolvePure(QStringLiteral("webkit"), plain);
    assert(rPersistedWebKit.engine == EngineType::WebKit && !rPersistedWebKit.isCliOverride &&
           rPersistedWebKit.source == EngineResolutionSource::Settings);
    const auto rPersistedChromium = EngineResolver::resolvePure(QStringLiteral("chromium"), plain);
    assert(rPersistedChromium.engine == EngineType::Chromium && !rPersistedChromium.isCliOverride &&
           rPersistedChromium.source == EngineResolutionSource::Settings);
    const auto rCliWebKit = EngineResolver::resolvePure(QStringLiteral("chromium"),
        QStringList{QStringLiteral("dalinira-browser"), QStringLiteral("--engine"), QStringLiteral("webkit")});
    assert(rCliWebKit.engine == EngineType::WebKit && rCliWebKit.isCliOverride &&
           rCliWebKit.source == EngineResolutionSource::CliOverride);
    const auto rCliChromium = EngineResolver::resolvePure(QStringLiteral("webkit"),
        QStringList{QStringLiteral("dalinira-browser"), QStringLiteral("--engine"), QStringLiteral("chromium")});
    assert(rCliChromium.engine == EngineType::Chromium && rCliChromium.isCliOverride &&
           rCliChromium.source == EngineResolutionSource::CliOverride);

    EngineResolver::setPersistedEngine(QStringLiteral("webkit"));
    assert(EngineResolver::getPersistedEngine() == "webkit");
    assert(EngineResolver::resolve(plain).engine == EngineType::WebKit);
    EngineResolver::setPersistedEngine(QStringLiteral("chromium"));
    assert(EngineResolver::getPersistedEngine() == "chromium");
    assert(EngineResolver::resolve(plain).engine == EngineType::Chromium);
  }
  std::cout << "[PASS] CLI > Settings > Default, with persistence." << std::endl;

  std::cout << "[TEST 8] WebKit page -> web process registry (tab memory attribution)..." << std::endl;
  {
    // The extension's side of the protocol, spoken by this process: the pid
    // must come from the socket (SO_PEERCRED), i.e. be this process's.
    dalinira::engine::WebKitNetworkFilter filter;
    assert(filter.start());
    const auto waitFor = [](const std::function<bool()> &condition) {
      QElapsedTimer timer;
      timer.start();
      while (!condition() && timer.elapsed() < 3000) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
      return condition();
    };
    QLocalSocket process;
    process.connectToServer(filter.socketPath());
    assert(process.waitForConnected(2000));
    process.write("P\t41\n");  // before HELLO: rejected, the connection is dropped
    process.flush();
    assert(waitFor([&] { return process.state() == QLocalSocket::UnconnectedState; }));
    assert(filter.processIdForPage(41) == 0);

    process.connectToServer(filter.socketPath());
    assert(process.waitForConnected(2000));
    process.write("HELLO\t" + filter.token() + "\nP\t42\nP\t43\n");
    process.flush();
    assert(waitFor([&] { return filter.processIdForPage(42) == getpid() && filter.processIdForPage(43) == getpid(); }));
    process.write("X\t42\n");  // the page left the process
    process.flush();
    assert(waitFor([&] { return filter.processIdForPage(42) == 0; }));
    assert(filter.processIdForPage(43) == getpid());
    process.disconnectFromServer();  // the process exited: none of its pages remain
    assert(waitFor([&] { return filter.processIdForPage(43) == 0; }));

    // Authenticated peers still have strict framing and may query only pages
    // they registered on their own kernel-authenticated connection.
    QLocalSocket malformed;
    malformed.connectToServer(filter.socketPath());
    assert(malformed.waitForConnected(2000));
    malformed.write("HELLO\t" + filter.token() + "\nQ\t1\t99\tGET\tscript\t0\thttps://example.test/a.js\thttps://example.test/\n");
    malformed.flush();
    assert(waitFor([&] { return malformed.state() == QLocalSocket::UnconnectedState; }));

    // A document without a web URL (an about:blank popup its opener wrote)
    // reports about:blank as first party: answered, the connection stays.
    QLocalSocket blankDocument;
    blankDocument.connectToServer(filter.socketPath());
    assert(blankDocument.waitForConnected(2000));
    blankDocument.write("HELLO\t" + filter.token() + "\nP\t44\n");
    blankDocument.flush();
    assert(waitFor([&] { return filter.processIdForPage(44) == getpid(); }));
    blankDocument.write("Q\t1\t44\tGET\timage\t0\thttp://example.test/favicon.ico\tabout:blank\n");
    blankDocument.flush();
    assert(waitFor([&] { return blankDocument.canReadLine(); }) && blankDocument.readLine() == "1\tA\n");
    assert(blankDocument.state() == QLocalSocket::ConnectedState);
    blankDocument.disconnectFromServer();

    QLocalSocket oversized;
    oversized.connectToServer(filter.socketPath());
    assert(oversized.waitForConnected(2000));
    oversized.write(QByteArray(161 * 1024, 'x'));
    oversized.flush();
    assert(waitFor([&] { return oversized.state() == QLocalSocket::UnconnectedState; }));
  }
  std::cout << "[PASS] Kernel-authenticated page ownership, removals, exits, and protocol bounds enforced."
            << std::endl;

  std::cout << "=== ALL ENGINE LOGIC TESTS PASSED ===" << std::endl;
  return 0;
}
