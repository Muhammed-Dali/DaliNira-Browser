// Unit tests of the shared Web Audio effects engine (WebAudioEffectsController)
// without pages or engine processes: DALI module validation and load-once
// cache, the cached page runtime, engine-neutral pages (a fake EngineView, as
// WebKit tabs register), effects-off and runtime enable, coalescing of
// parameter bursts, and bounded per-page work whatever the tab count.

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <iostream>

#include "audio/web_audio_effects_controller.h"
#include "engine/engine_view.h"

#define CHECK(condition)                                                                    \
  do {                                                                                      \
    if (!(condition)) {                                                                     \
      std::cout << "[FAIL] " << __FILE__ << ":" << __LINE__ << ": " #condition << std::endl; \
      std::_Exit(1);                                                                        \
    }                                                                                       \
  } while (0)

namespace {

// A tab page of a non-Chromium engine: records the scripts it receives and
// answers like a page whose DALI graph is live.
class FakeEngineView final : public dalinira::engine::EngineView {
 public:
  explicit FakeEngineView(const QUrl &url) : url_(url) {}
  QUrl url() const override { return url_; }
  dalinira::engine::EngineType engineType() const override { return type; }
  dalinira::engine::EngineType type = dalinira::engine::EngineType::Chromium;
  void runJavaScript(const QString &script, std::function<void(const QVariant &)> callback) override {
    scripts.append(script);
    if (callback) callback(QVariantMap{{QStringLiteral("ok"), true}, {QStringLiteral("moduleLoaded"), true},
                                       {QStringLiteral("enabled"), true}, {QStringLiteral("mediaCount"), 1}});
  }
  void refreshDocumentScripts() override { ++refreshes; }
  int runtimeDeliveries() const {
    int count = 0;
    for (const QString &script : scripts) count += script.contains(QStringLiteral("daliniraDaliBootstrap")) ? 1 : 0;
    return count;
  }
  QStringList scripts;
  int refreshes = 0;

 private:
  QUrl url_;
};

void pump(int ms) {
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < ms) QApplication::processEvents(QEventLoop::AllEvents, 10);
}

QByteArray readModule(const char *name) {
  QFile file(QCoreApplication::applicationDirPath() + QStringLiteral("/dali/") + QString::fromLatin1(name));
  CHECK(file.open(QIODevice::ReadOnly));
  return file.readAll();
}

const QUrl kMediaPage(QStringLiteral("https://www.youtube.com/watch?v=fixture"));
const QUrl kOtherPage(QStringLiteral("https://example.org/"));

void checkModuleValidation() {
  const char *modules[] = {"web-output-audiophile.generated.js", "web-equalizer-32.generated.js",
                           "web-dynamic-compressor.generated.js", "web-limiter.generated.js",
                           "web-bass-enhancer.generated.js", "web-auto-gain.generated.js"};
  for (const char *name : modules) {
    QString error;
    CHECK(WebAudioEffectsController::validateDaliModule(readModule(name), &error) && error.isEmpty());
  }
  const QByteArray valid = readModule("web-limiter.generated.js");
  QString error;
  CHECK(!WebAudioEffectsController::validateDaliModule(QByteArray(), &error) && !error.isEmpty());
  CHECK(!WebAudioEffectsController::validateDaliModule(valid.left(valid.size() * 7 / 10), &error));  // truncated
  CHECK(error.contains(QStringLiteral("truncated")));
  CHECK(!WebAudioEffectsController::validateDaliModule(valid + QByteArray(1, '\0'), &error));        // binary
  CHECK(!WebAudioEffectsController::validateDaliModule(QByteArray("\xff\xfe\x00garbage", 10), &error));
  QByteArray badUtf8 = valid;
  badUtf8.insert(200, "\xc3\x28");
  CHECK(!WebAudioEffectsController::validateDaliModule(badUtf8, &error) && error.contains(QStringLiteral("UTF-8")));
  CHECK(!WebAudioEffectsController::validateDaliModule("module.exports = { buildGraph: x };", &error));  // foreign
  QByteArray otherVersion = valid;
  otherVersion.replace("dali-lang compiler (v0)", "dali-lang compiler (v9)");
  CHECK(!WebAudioEffectsController::validateDaliModule(otherVersion, &error) && error.contains(QStringLiteral("version")));
  QByteArray noExports = valid;
  noExports.replace("module.exports = {", "const unused = {");
  CHECK(!WebAudioEffectsController::validateDaliModule(noExports, &error));
  QByteArray huge = valid;
  huge.append(QByteArray(WebAudioEffectsController::kMaxDaliModuleBytes, ' '));
  CHECK(!WebAudioEffectsController::validateDaliModule(huge, &error) && error.contains(QStringLiteral("large")));
  std::cout << "[PASS] DALI modules: valid compiled .dl presets accepted; empty/truncated/binary/non-UTF-8/"
               "foreign/other-version/oversized rejected" << std::endl;
}

}  // namespace

int main(int argc, char *argv[]) {
  QApplication app(argc, argv);
  QTemporaryDir directory;
  CHECK(directory.isValid());
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
  QCoreApplication::setOrganizationName(QStringLiteral("DaliNiraTest"));
  QCoreApplication::setApplicationName(QStringLiteral("WebAudioEngineUnit"));
  std::cout << "=== WEB AUDIO EFFECTS ENGINE (UNIT) ===" << std::endl;

  checkModuleValidation();

  {
    // Effects off (the default): pages receive nothing at all.
    WebAudioEffectsController controller(nullptr, false);
    CHECK(!controller.enabled());
    FakeEngineView page(kMediaPage);
    controller.registerEngineView(&page, kMediaPage);
    controller.applyToEngineView(&page);
    emit page.loadFinished(true);
    CHECK(controller.documentScriptsFor(&page, kMediaPage).isEmpty());
    CHECK(page.scripts.isEmpty() && controller.diagnostics().scriptsSent == 0);

    // Enabled at runtime: the open page gets the runtime once, immediately.
    controller.setEnabled(true);
    CHECK(page.runtimeDeliveries() == 1);
    CHECK(page.refreshes == 1);  // its next documents get it at document start
    pump(1200);                  // recovery passes: small parameter scripts only
    CHECK(page.runtimeDeliveries() == 1);
    CHECK(controller.activeGraphViewCount() == 1);

    // The same runtime for every engine: WebKit's document script is exactly
    // what Chromium installs (QWebEngineScript) and bootstraps.
    const QList<dalinira::engine::DocumentScript> scripts = controller.documentScriptsFor(&page, kMediaPage);
    CHECK(scripts.size() == 1 && scripts.front().mainWorld && !scripts.front().allFrames);
    CHECK(scripts.front().source == controller.injectionScript());
    page.type = dalinira::engine::EngineType::WebKit;
    const auto webkitScripts = controller.documentScriptsFor(&page, kMediaPage);
    CHECK(webkitScripts.size() == 1);
    CHECK(webkitScripts.front().source.contains(QStringLiteral("mediaLatencyHint: 'interactive'")));
    CHECK(webkitScripts.front().source.endsWith(controller.injectionScript()));
    page.type = dalinira::engine::EngineType::Chromium;
    CHECK(controller.documentScriptsFor(&page, kOtherPage).isEmpty());  // pages without platform media

    // A loaded document (WebKit loadFinished) gets the same parameter update
    // Chromium's pages get.
    page.scripts.clear();
    emit page.loadFinished(true);
    CHECK(page.scripts.size() == 1 && page.scripts.front() == controller.parameterUpdateScript());

    // A page that starts playing sound gets its parameters from the browser
    // again (that script may start the page's AudioContext on WebKit).
    page.scripts.clear();
    emit page.audibleChanged(true);
    CHECK(page.scripts.size() == 1 && page.scripts.front() == controller.parameterUpdateScript());
    emit page.audibleChanged(false);
    CHECK(page.scripts.size() == 1);

    // Disabled at runtime: live graphs are bypassed at once.
    page.scripts.clear();
    controller.setEnabled(false);
    CHECK(page.scripts.size() == 1 && page.scripts.front().contains(QStringLiteral("const enabled = false")));
    CHECK(controller.documentScriptsFor(&page, kMediaPage).isEmpty());
    std::cout << "[PASS] effects off sends nothing; runtime enable/disable applies immediately; "
                 "WebKit pages get the same runtime and updates as Chromium" << std::endl;
  }

  {
    // Modules are read once; the runtime is built once per configuration.
    WebAudioEffectsController controller(nullptr, false);
    controller.setEnabled(true);
    controller.resetDiagnostics();
    for (int i = 0; i < 5; ++i) controller.injectionScript();
    CHECK(controller.diagnostics().injectionBuilds <= 1);
    CHECK(controller.diagnostics().moduleLoads <= 6);
    const quint64 loads = controller.diagnostics().moduleLoads;
    controller.setBassDb(3.0);
    for (int i = 0; i < 5; ++i) controller.injectionScript();
    CHECK(controller.diagnostics().injectionBuilds <= 2);
    CHECK(controller.diagnostics().moduleLoads == loads);  // no re-read

    // Bursts: 25 slider steps reach each page as one coalesced update.
    FakeEngineView page(kMediaPage);
    controller.registerEngineView(&page, kMediaPage);
    controller.applyToEngineView(&page);
    pump(1000);  // the enable recovery passes (90/300/850 ms) are over
    page.scripts.clear();
    for (int i = 0; i < 25; ++i) controller.setBassDb(-6.0 + i * 0.5);
    pump(400);
    CHECK(page.scripts.size() == 1);
    std::cout << "[PASS] modules read once, runtime built once per configuration, parameter bursts coalesced" << std::endl;
  }

  {
    // Tab count: per-page work stays constant and pages leave cleanly.
    WebAudioEffectsController controller(nullptr, false);
    controller.setEnabled(true);
    const auto workFor = [&controller](int tabs) {
      controller.resetDiagnostics();
      QList<FakeEngineView *> pages;
      for (int i = 0; i < tabs; ++i) {
        auto *page = new FakeEngineView(kMediaPage);
        controller.registerEngineView(page, kMediaPage);
        emit page->loadFinished(true);
        pages.append(page);
      }
      CHECK(controller.registeredPageCount() == tabs);
      controller.setBassDb(double(tabs % 7));
      pump(200);
      const quint64 scripts = controller.diagnostics().scriptsSent;
      for (int i = 0; i < tabs; ++i) {
        if (i % 2) controller.unregisterEngineView(pages[i]);
        delete pages[i];  // the other half leave by destruction
      }
      QApplication::processEvents();
      CHECK(controller.registeredPageCount() == 0);
      return scripts;
    };
    const quint64 one = workFor(1);
    const quint64 ten = workFor(10);
    const quint64 fifty = workFor(50);
    std::cout << "  scripts sent: 1 tab=" << one << " 10 tabs=" << ten << " 50 tabs=" << fifty << std::endl;
    CHECK(ten <= one * 10 && fifty <= one * 50);  // linear in pages, nothing per event per page
    CHECK(controller.diagnostics().injectionBuilds <= 1);  // one runtime build shared by all pages
    std::cout << "[PASS] 1/10/50 pages: constant work per page, one shared runtime, no leaked pages" << std::endl;
  }

  {
    // An unavailable optional module leaves the rest of the engine working;
    // an unavailable core module disables delivery without crashing.
    WebAudioEffectsController controller(nullptr, false);
    controller.modules_[WebAudioEffectsController::CompressorModule].relativePath = "dali/missing-compressor.js";
    controller.setEnabled(true);
    FakeEngineView page(kMediaPage);
    controller.registerEngineView(&page, kMediaPage);
    CHECK(controller.documentScriptsFor(&page, kMediaPage).size() == 1);
    CHECK(controller.diagnostics().moduleRejects == 1);

    WebAudioEffectsController broken(nullptr, false);
    broken.modules_[WebAudioEffectsController::OutputModule].relativePath = "dali/missing-output.js";
    FakeEngineView other(kMediaPage);
    broken.registerEngineView(&other, kMediaPage);
    broken.setEnabled(true);  // bootstrap attempt: refused, not crashed
    CHECK(broken.documentScriptsFor(&other, kMediaPage).isEmpty());
    CHECK(other.runtimeDeliveries() == 0 && broken.diagnostics().bootstrapsSent == 0);
    CHECK(broken.diagnostics().moduleRejects == 1);
    std::cout << "[PASS] missing optional module: engine still runs; missing core module: no delivery, no crash"
              << std::endl;
  }

  std::cout << "=== ALL WEB AUDIO EFFECTS ENGINE UNIT TESTS PASSED ===" << std::endl;
  return 0;
}
