#include <QApplication>
#include <QPointer>
#include <QIcon>
#include <QPalette>
#include <QStyleFactory>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QElapsedTimer>
#include <QThread>
#include <QTimer>

#if defined(Q_OS_UNIX)
#include <cerrno>
#include <csignal>
#endif

#include "browser_window.h"
#include "core/application_identity.h"
#include "core/browser_icons.h"
#include "core/default_browser_integration.h"
#include "core/performance_diagnostics.h"
#include "core/web_engine_hardware_acceleration.h"
#include "core/web_engine_memory_policy.h"
#include "desktop_tabs/tab_drag_controller.h"
#include "engine/engine_backend.h"
#include "engine/engine_storage_manager.h"
#include "engine/engine_resolver.h"
#include "engine/display_platform.h"
#include "layout/browser_layout.h"
#include "engine/chromium/chromium_backend.h"
#if defined(DALINIRA_BUILD_WEBKIT)
#include "engine/webkit/webkit_backend.h"
#include "engine/webkit/webkit_media_runtime.h"
#include "engine/webkit/webkit_private_runtime.h"
#include <cstdio>
#endif
#include "i18n/i18n.h"
#include "i18n/language_manager.h"
#include "i18n/widget_i18n.h"
#include "newtab/new_tab_scheme.h"
#include "blocker/dalinira_blocker_request_interceptor.h"
#include "onboarding/onboarding_manager.h"
#include "onboarding/onboarding_window.h"
#include "onboarding/onboarding_adapters.h"

namespace {

constexpr auto kDaliNiraDesktopFile = "dalinira.desktop";
constexpr auto kDefaultBrowserPromptDisabled = "browser/defaultBrowserPromptDisabled";
constexpr auto kDefaultBrowserLastPromptUtc = "browser/defaultBrowserLastPromptUtc";

void openDefaultApplicationsSettings(QWidget *parent) {
#if defined(Q_OS_LINUX)
  if (QFileInfo(QStringLiteral("/usr/bin/systemsettings")).isExecutable()
      && QProcess::startDetached(QStringLiteral("/usr/bin/systemsettings"),
                                 {QStringLiteral("kcm_componentchooser")})) {
    return;
  }
#endif
  QMessageBox::information(parent, I18n::text(QStringLiteral("app.default_browser_title")),
      I18n::text(QStringLiteral("app.default_browser_manual")));
}

void requestDefaultBrowser(QWidget *parent) {
  auto *integration = new dalinira::core::SystemDefaultBrowserIntegration(parent);
  integration->requestDefaultBrowser([parent, integration](dalinira::core::DefaultBrowserResult result) {
    integration->deleteLater();
    if (result == dalinira::core::DefaultBrowserResult::Success
        || result == dalinira::core::DefaultBrowserResult::AlreadyDefault) {
      QSettings settings;
      settings.remove(QString::fromLatin1(kDefaultBrowserPromptDisabled));
      settings.remove(QString::fromLatin1(kDefaultBrowserLastPromptUtc));
      QMessageBox::information(parent, I18n::text(QStringLiteral("app.default_browser_title")),
          I18n::text(QStringLiteral("app.default_browser_done")));
      return;
    }
    openDefaultApplicationsSettings(parent);
  });
}

void showDefaultBrowserPrompt(QWidget *parent) {
  auto *dialog = new QMessageBox(QMessageBox::Question,
      I18n::text(QStringLiteral("app.make_default_title")),
      I18n::text(QStringLiteral("app.make_default_prompt")),
      QMessageBox::NoButton, parent);
  QPushButton *makeDefault = dialog->addButton(I18n::text(QStringLiteral("app.make_default")), QMessageBox::AcceptRole);
  QPushButton *later = dialog->addButton(I18n::text(QStringLiteral("passwords.not_now")), QMessageBox::RejectRole);
  QPushButton *never = dialog->addButton(I18n::text(QStringLiteral("app.dont_ask_again")), QMessageBox::DestructiveRole);
  dialog->setDefaultButton(makeDefault);
  dialog->setEscapeButton(later);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  QObject::connect(dialog, &QMessageBox::finished, parent,
                   [parent, dialog, makeDefault, never](int) {
    QSettings settings;
    if (dialog->clickedButton() == makeDefault) {
      requestDefaultBrowser(parent);
    } else if (dialog->clickedButton() == never) {
      settings.setValue(QString::fromLatin1(kDefaultBrowserPromptDisabled), true);
    } else {
      settings.setValue(QString::fromLatin1(kDefaultBrowserLastPromptUtc),
                        QDateTime::currentDateTimeUtc());
    }
  });
  dialog->open();
}

void checkDefaultBrowser(QWidget *parent) {
#if defined(Q_OS_LINUX)
  const QSettings settings;
  if (settings.value(QString::fromLatin1(kDefaultBrowserPromptDisabled), false).toBool()) return;
  const QDateTime lastPrompt = settings.value(QString::fromLatin1(kDefaultBrowserLastPromptUtc)).toDateTime();
  if (lastPrompt.isValid() && lastPrompt.daysTo(QDateTime::currentDateTimeUtc()) < 7) return;
  if (!QFileInfo(QStringLiteral("/usr/bin/xdg-settings")).isExecutable()) {
    showDefaultBrowserPrompt(parent);
    return;
  }
  auto *check = new QProcess(parent);
  check->setProgram(QStringLiteral("/usr/bin/xdg-settings"));
  check->setArguments({QStringLiteral("get"), QStringLiteral("default-web-browser")});
  QObject::connect(check, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), parent,
                   [parent, check](int exitCode, QProcess::ExitStatus exitStatus) {
    const QString desktopFile = QString::fromUtf8(check->readAllStandardOutput()).trimmed();
    const bool alreadyDefault = exitStatus == QProcess::NormalExit && exitCode == 0
        && desktopFile == QString::fromLatin1(kDaliNiraDesktopFile);
    check->deleteLater();
    if (!alreadyDefault) showDefaultBrowserPrompt(parent);
  });
  check->start();
#else
  Q_UNUSED(parent);
#endif
}

QStringList normalRelaunchArguments() {
  const QStringList current = qApp->arguments().mid(1);
  QStringList cleaned;
  for (int i = 0; i < current.size(); ++i) {
    if (current[i] == QLatin1String("--engine") ||
        current[i] == QLatin1String("--wait-for-pid")) {
      ++i;
      continue;
    }
    if (current[i] == QLatin1String("--onboarding")) continue;
    cleaned.append(current[i]);
  }
  return cleaned;
}

bool startDetachedQuietly(const QString &program, const QStringList &arguments) {
  QProcess process;
  process.setProgram(program);
  process.setArguments(arguments);
  process.setStandardOutputFile(QProcess::nullDevice());
  process.setStandardErrorFile(QProcess::nullDevice());
  return process.startDetached();
}

#if defined(DALINIRA_BUILD_WEBKIT)
// `dalinira-browser --check-media-runtime`: reports whether the GStreamer
// runtime WebKit's HTML5 media needs is installed (docs/webkit-media-runtime.md).
// Needs no display, so package installation tests can run it. Exit status:
// 0 complete, 2 playable but some formats missing, 1 media disabled.
int checkMediaRuntime() {
  const auto report = dalinira::engine::WebKitMediaRuntime::probe();
  for (const auto &gap : report.missing) {
    std::printf("missing %s (GStreamer plugin %s, %s): %s%s\n", qPrintable(gap.element), qPrintable(gap.plugin),
                qPrintable(gap.module), qPrintable(gap.purpose), gap.fatal ? " [media disabled]" : "");
  }
  if (!report.playbackAvailable) {
    std::printf("WebKit media runtime: INCOMPLETE, HTML5 audio/video is disabled\n");
    return 1;
  }
  std::printf("WebKit media runtime: %s\n", report.missing.isEmpty() ? "complete" : "playable, some formats unsupported");
  return report.missing.isEmpty() ? 0 : 2;
}

// `dalinira-browser --check-webkit-runtime`: reports which WebKitGTK libraries
// the loader mapped and, in release packages, whether they are DaliNira's
// private patched runtime (packaging/webkit-runtime/SOURCE.md). Needs no
// display. Exit status: 0 in use (or a build without a private runtime),
// 1 broken (the WebKit engine would be disabled).
int checkWebKitRuntime() {
  const auto status = dalinira::engine::WebKitPrivateRuntime::probe();
  std::printf("%s\n", qPrintable(dalinira::engine::WebKitPrivateRuntime::describe(status)));
  return status.usable() ? 0 : 1;
}
#endif

}  // namespace

int main(int argc, char *argv[]) {
  // Set application and organization names early so QSettings resolves the correct configuration paths
  dalinira::application_identity::apply();
  bool forceStandaloneOnboarding = false;
  qint64 waitForProcessId = 0;
  for (int i = 1; i < argc; ++i) {
    // Package smoke test: the dynamic loader resolved every linked library.
    if (qstrcmp(argv[i], "--version") == 0) {
      std::printf("DaliNira Browser %s\n", DALINIRA_BROWSER_VERSION);
      return 0;
    }
#if defined(DALINIRA_BUILD_WEBKIT)
    if (qstrcmp(argv[i], "--check-media-runtime") == 0) return checkMediaRuntime();
    if (qstrcmp(argv[i], "--check-webkit-runtime") == 0) return checkWebKitRuntime();
#endif
    if (qstrcmp(argv[i], "--onboarding") == 0) {
      forceStandaloneOnboarding = true;
    } else if (qstrcmp(argv[i], "--wait-for-pid") == 0 && i + 1 < argc) {
      waitForProcessId = QString::fromLocal8Bit(argv[++i]).toLongLong();
    }
  }

#if defined(Q_OS_UNIX)
  if (waitForProcessId > 1) {
    QElapsedTimer waitTimer;
    waitTimer.start();
    while (::kill(static_cast<pid_t>(waitForProcessId), 0) == 0 || errno == EPERM) {
      if (waitTimer.elapsed() >= 30000) {
        qCritical() << "Previous DaliNira process did not exit; standalone onboarding aborted"
                    << waitForProcessId;
        return 1;
      }
      QThread::msleep(25);
    }
  }
#else
  Q_UNUSED(waitForProcessId);
#endif

  // Development-only override for isolated local GStreamer plugin testing
  if (qEnvironmentVariableIntValue("DALINIRA_DEV_GST_FALLBACK") == 1) {
    const QString localGstPath = QDir::homePath() + QStringLiteral("/.local/usr/lib/gstreamer-1.0");
    if (QDir(localGstPath).exists()) {
      const QByteArray currentGstPath = qgetenv("GST_PLUGIN_PATH_1_0");
      if (currentGstPath.isEmpty()) {
        qputenv("GST_PLUGIN_PATH_1_0", localGstPath.toUtf8() + ":/usr/lib/gstreamer-1.0");
      } else if (!currentGstPath.contains(localGstPath.toUtf8())) {
        qputenv("GST_PLUGIN_PATH_1_0", localGstPath.toUtf8() + ":" + currentGstPath);
      }
    }
  }

  // Authoritative single-engine resolution: CLI override > persisted settings > Chromium default
  QStringList cmdArgs;
  cmdArgs.reserve(argc);
  for (int i = 0; i < argc; ++i) {
    cmdArgs.append(QString::fromLocal8Bit(argv[i]));
  }
  const auto resolution = dalinira::engine::EngineResolver::resolve(cmdArgs);
  auto initialEngine = resolution.engine;

  // Windowing backend per engine and session (X11, XWayland, native Wayland);
  // Qt and GDK read it only at start-up. See engine/display_platform.h.
  const auto displayEnvironment = dalinira::engine::DisplayEnvironment::fromProcess();
  auto displayPlan = dalinira::engine::planDisplay(initialEngine, displayEnvironment);
  if (!displayPlan.engineSupported) {
    qWarning().noquote() << displayPlan.note << "Starting with Chromium for this run.";
    initialEngine = dalinira::engine::EngineType::Chromium;
    displayPlan = dalinira::engine::planDisplay(initialEngine, displayEnvironment);
  } else if (!displayPlan.note.isEmpty()) {
    qWarning().noquote() << displayPlan.note;
  }
#if defined(DALINIRA_BUILD_WEBKIT)
  // Release packages run WebKit only on their private patched runtime; never
  // fall back to whatever WebKitGTK the system has.
  if (initialEngine == dalinira::engine::EngineType::WebKit) {
    const auto webkitRuntime = dalinira::engine::WebKitPrivateRuntime::probe();
    if (!webkitRuntime.usable()) {
      qWarning().noquote() << dalinira::engine::WebKitPrivateRuntime::describe(webkitRuntime)
                           << "\nStarting with Chromium for this run.";
      initialEngine = dalinira::engine::EngineType::Chromium;
      displayPlan = dalinira::engine::planDisplay(initialEngine, displayEnvironment);
    }
  }
#endif
  dalinira::engine::applyDisplayPlan(displayPlan);
  dalinira::engine::EngineBackend::setSelectedEngineType(initialEngine);

  if (initialEngine == dalinira::engine::EngineType::Chromium) {
    // Configure WebEngine subprocess allocator policy (MALLOC_ARENA_MAX=2, MALLOC_TRIM_THRESHOLD=128KB)
    // before QtWebEngine process is spawned.
    const QString appDir = QFileInfo(QString::fromLocal8Bit(argv[0])).dir().absolutePath();
    dalinira::WebEngineMemoryPolicy::configureSubprocessLauncher(appDir);

    // Initialize hardware video decoding and GPU flags early before QApplication / QtWebEngine init
    dalinira::WebEngineHardwareAcceleration::initializeEarlyRuntime();
    // Cache and connection partitioning by top-level site.
    dalinira::WebEngineHardwareAcceleration::applyPrivacyFlags();

    // Register dalinira:// URL scheme before QGuiApplication
    registerDaliNiraUrlSchemes();
    // ... and the blocker's packaged redirect substitutes.
    DaliNiraBlockerRequestInterceptor::registerResourceScheme();
  } else {
    // Do not set WEBKIT_DISABLE_DMABUF_RENDERER: WebKitGTK 2.52 then creates no
    // AcceleratedBackingStore, and a page entering compositing mode crashes the
    // UI process (AcceleratedBackingStore::update with a null store).
    // Each WebKit tab is a native X11 child window (see WebKitEngineView).
    // Keep its siblings (Settings and other internal Qt pages) alien.
    QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
  }

  QApplication app(argc, argv);
  dalinira::application_identity::apply();
  app.setApplicationVersion(QStringLiteral(DALINIRA_BROWSER_VERSION));

  // Initialize central i18n / multi-language system
  dalinira::i18n::LanguageManager::instance().initialize();
  // One shared catalog drives both engines; a live switch refreshes every
  // open window without recreating tabs.
  dalinira::i18n::installApplicationRetranslator();

  const QIcon appIcon = BrowserIcons::appIcon();
  if (!appIcon.isNull()) {
    app.setWindowIcon(appIcon);
  }

  // The persisted appearance is shared by Chromium and WebKit browser chrome.
  app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
  dalinira::layout::applyApplicationTheme(
      &app, dalinira::layout::currentAppearanceTheme());

  // Load browser policy
  QString policyError;
  const QString installedPolicyPath = QCoreApplication::applicationDirPath() + QStringLiteral("/browser_policy.json");
  const QString policyPath = QFileInfo::exists(installedPolicyPath)
      ? installedPolicyPath
      : QStringLiteral(DALINIRA_BROWSER_POLICY_PATH);
  const BrowserPolicy policy = BrowserPolicy::load(policyPath, &policyError);

  const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  QDir().mkpath(dataDir);

  // Initialize Core Services
  BrowserProfileService profileService(dataDir, &policy, &app);

  dalinira::engine::EngineStorageManager engineStorage(dataDir);

  // Initialize ONLY the selected backend (single active engine per process)
  if (initialEngine == dalinira::engine::EngineType::Chromium) {
    auto chromiumBackend = std::make_unique<dalinira::engine::ChromiumBackend>(profileService.profile(), &app);
    chromiumBackend->initialize(engineStorage);
    dalinira::engine::EngineBackend::registerBackend(dalinira::engine::EngineType::Chromium, std::move(chromiumBackend));
  }
#if defined(DALINIRA_BUILD_WEBKIT)
  else if (initialEngine == dalinira::engine::EngineType::WebKit) {
    auto webkitBackend = std::make_unique<dalinira::engine::WebKitBackend>(&app);
    webkitBackend->initialize(engineStorage);
    // Network ad/tracker blocking through DaliNira's web process extension.
    // It must be running before any web process starts.
    if (!webkitBackend->enableNetworkFiltering())
      qWarning() << "WebKit network filtering is unavailable; only cosmetic filtering will run.";
    if (webkitBackend->webKitProfile()) {
      QObject::connect(webkitBackend->webKitProfile(), &dalinira::engine::WebKitEngineProfile::downloadContextRequested,
                       &profileService, &BrowserProfileService::handleDownloadContext);
    }
    dalinira::engine::EngineBackend::registerBackend(dalinira::engine::EngineType::WebKit, std::move(webkitBackend));
    // WebKit's cookie store enforces the profile's cookie policy itself.
    profileService.applyEngineCookiePolicy();
  }
#endif

  TabManager tabManager(&app);
  const QString initialEngineScope = (initialEngine == dalinira::engine::EngineType::WebKit)
      ? QStringLiteral("webkit") : QStringLiteral("chromium");
  // Chromium and WebKit are separate browsing workspaces with their own tabs.
  // The session saved before workspaces existed becomes Chromium's, once;
  // WebKit starts with a fresh New Tab.
  SessionStore::migrateLegacySessionIfNeeded(dataDir);
  SessionStore sessionStore(dataDir, initialEngineScope);
  auto *audioEffects = new WebAudioEffectsController(&app);
  auto *eqPresetRepo = new EqPresetRepository;
  auto *songFinderSettings = new SongFinderSettings(&app);
  auto *songRecognition = new SongRecognitionService(songFinderSettings, &app);
  auto *mediaDownload = new MediaDownloadService(dataDir, &app);
  if (qEnvironmentVariableIntValue("DALINIRA_FEATURE_DIAGNOSTICS") == 1) {
    auto *diagnostics = new PerformanceDiagnostics(
        &tabManager, profileService.adBlockService(), audioEffects, songRecognition, &app);
    diagnostics->start();
  }

  BrowserServices services;
  services.profile = profileService.profile();
  services.profileService = &profileService;
  services.tabManager = &tabManager;
  services.policy = &policy;
  services.sessionStore = &sessionStore;
  services.audioEffects = audioEffects;
  services.eqPresetRepo = eqPresetRepo;
  services.songFinderSettings = songFinderSettings;
  services.songRecognition = songRecognition;
  services.mediaDownload = mediaDownload;

  // Wire TabDragController delegates for Chromium parity tab dragging
  auto &dragController = dalinira::desktop_tabs::TabDragController::instance();

  dragController.setDetachedWindowFactory(
      [services](QWidget *originWin, uint64_t /*tabId*/) -> QWidget * {
        auto *origin = dynamic_cast<BrowserWindow *>(originWin);
        if (!origin) return nullptr;
        auto *captureShell = new BrowserWindow(origin->services(), /*isCaptureShell=*/true);
        return captureShell;
      });

  dragController.setDetachTransferDelegate(
      [](QWidget *destWin, QWidget *originWin, uint64_t tabId, int targetIndex) -> bool {
        auto *origin = dynamic_cast<BrowserWindow *>(originWin);
        auto *dest   = dynamic_cast<BrowserWindow *>(destWin);
        if (!origin || !dest) return false;
        return origin->transferTabTo(tabId, dest, targetIndex);
      });

  dragController.setTabTransferDelegate(
      [](QWidget *fromWin, QWidget *toWin, uint64_t tabId, int targetIndex) -> bool {
        auto *source = dynamic_cast<BrowserWindow *>(fromWin);
        auto *dest   = dynamic_cast<BrowserWindow *>(toWin);
        if (!source || !dest) return false;
        return source->transferTabTo(tabId, dest, targetIndex);
      });

  dragController.setTabMoveDelegate(
      [](QWidget *win, int fromIndex, int toIndex) {
        auto *bw = dynamic_cast<BrowserWindow *>(win);
        if (!bw) return;
        bw->moveTab(fromIndex, toIndex);
      });

  // Parse command line startup URL if provided
  QUrl startupUrl;
  for (int i = 1; i < argc; ++i) {
    const QString argument = QString::fromLocal8Bit(argv[i]).trimmed();
    if (argument == QLatin1String("--engine") ||
        argument == QLatin1String("--wait-for-pid")) {
      ++i;
      continue;
    }
    if (argument.startsWith(QLatin1Char('-'))) continue;
    const QUrl candidate = QUrl::fromUserInput(argument);
    const QString scheme = candidate.scheme().toLower();
    if (candidate.isValid() && (scheme == QLatin1String("http") || scheme == QLatin1String("https")
                                || scheme == QLatin1String("dalinira"))) {
      startupUrl = candidate;
      break;
    }
  }

  QObject::connect(&app, &QCoreApplication::aboutToQuit, &app, [] {
    for (QWidget *widget : QApplication::topLevelWidgets()) {
      if (auto *bw = qobject_cast<BrowserWindow *>(widget)) {
        if (!bw->isMinimized()) {
          bw->saveSessionNow();
          QSettings settings;
          settings.setValue(QStringLiteral("browser/mainWindowGeometry"), bw->saveGeometry());
          settings.sync();
          break;
        }
      }
    }
  });

  const bool onboardingNeeded = forceStandaloneOnboarding ||
      dalinira::onboarding::OnboardingManager::shouldShow();
  if (onboardingNeeded && startupUrl.isEmpty()) {
    // Show ONLY the dedicated standalone setup window (like Zen Browser)
    auto *receiver = new dalinira::onboarding::ProfileDataReceiverAdapter(&profileService, &app);
    auto *searchPrivacy = new dalinira::onboarding::ProfileSearchPrivacyAdapter(&profileService, &app);
    auto *defaultIntegration = new dalinira::core::SystemDefaultBrowserIntegration(&app);

    auto *onboardingWindow = new dalinira::onboarding::OnboardingWindow(
        receiver, searchPrivacy, defaultIntegration);

    QObject::connect(onboardingWindow, &dalinira::onboarding::OnboardingWindow::onboardingFinished,
                     [services, initialEngine] {
      // Check if user selected a different engine during onboarding
      const QString preferredEngine = dalinira::onboarding::OnboardingManager::instance().selectedEngine();
      const QString runningEngine = (initialEngine == dalinira::engine::EngineType::WebKit)
          ? QStringLiteral("webkit") : QStringLiteral("chromium");

      if (!preferredEngine.isEmpty() && preferredEngine != runningEngine) {
        dalinira::engine::EngineResolver::setPersistedEngine(preferredEngine);

        const QString program = QCoreApplication::applicationFilePath();
        startDetachedQuietly(program, normalRelaunchArguments());
        qApp->quit();
        return;
      }

      // Apply selected layout and appearance theme
      const QString preferredLayout = dalinira::onboarding::OnboardingManager::instance().selectedLayout();
      const auto layoutMode = dalinira::layout::layoutModeFromString(
          preferredLayout, dalinira::layout::BrowserLayoutMode::Classic);
      dalinira::layout::setPersistedLayoutMode(layoutMode);

      const QString preferredTheme = dalinira::onboarding::OnboardingManager::instance().selectedTheme();
      const auto theme = dalinira::layout::appearanceThemeFromString(preferredTheme);
      dalinira::layout::applyApplicationTheme(qobject_cast<QApplication *>(qApp), theme);

      // Launch main browser window now that onboarding is completed
      auto *mainWindow = new BrowserWindow(services);
      const QByteArray savedWindowGeometry =
          QSettings().value(QStringLiteral("browser/mainWindowGeometry")).toByteArray();
      if (!savedWindowGeometry.isEmpty()) {
        mainWindow->restoreGeometry(savedWindowGeometry);
      }
      mainWindow->addNewTab(QUrl(QStringLiteral("dalinira://newtab/")));
      mainWindow->setAttribute(Qt::WA_DeleteOnClose);
      mainWindow->show();
      // No default-browser prompt on this run: setup just offered the choice
      // (or the user skipped setup); the regular check runs from next start.
    });

    QObject::connect(onboardingWindow, &dalinira::onboarding::OnboardingWindow::onboardingDismissed,
                     [&app] {
      app.quit();
    });

    onboardingWindow->setAttribute(Qt::WA_DeleteOnClose);
    onboardingWindow->show();
  } else {
    // Normal browser launch (or URL provided via command line)
    auto *window = new BrowserWindow(services);
    const QByteArray savedWindowGeometry =
        QSettings().value(QStringLiteral("browser/mainWindowGeometry")).toByteArray();
    if (!savedWindowGeometry.isEmpty()) {
      window->restoreGeometry(savedWindowGeometry);
    }

    bool restored = false;
    if (startupUrl.isEmpty() && policy.allowsSessionRestore() && QSettings().value(QStringLiteral("browser/restoreSession"), true).toBool()) {
      const auto savedTabs = sessionStore.load(initialEngineScope);
      if (!savedTabs.isEmpty()) {
        window->restoreSession(savedTabs);
        restored = true;
      }
    }
    if (!restored) {
      if (!startupUrl.isEmpty()) {
        window->openStartupUrl(startupUrl);
      } else {
        window->ensureInitialTab();
      }
    }

    window->setAttribute(Qt::WA_DeleteOnClose);
    window->show();
    QTimer::singleShot(1200, window, [window] { checkDefaultBrowser(window); });
  }

  const int exitCode = app.exec();

  // A WebKitGTK view is a foreign native child window. Tear down every browser
  // window first, then the selected backend, before QApplication dismantles
  // the X11/GLib platform integration.
  // Collect the windows first: deleting one also deletes top-level widgets
  // it owns, which a raw list of top-level widgets would still hold.
  QList<QPointer<BrowserWindow>> browserWindows;
  for (QWidget *widget : QApplication::topLevelWidgets()) {
    if (auto *browserWindow = qobject_cast<BrowserWindow *>(widget)) browserWindows.append(browserWindow);
  }
  for (const QPointer<BrowserWindow> &browserWindow : std::as_const(browserWindows)) {
    delete browserWindow.data();  // null once an earlier deletion took it
  }
  dalinira::engine::EngineBackend::shutdown();
#if defined(DALINIRA_BUILD_WEBKIT)
  // The windows are gone; a web process WebKit still keeps for a service
  // worker must end before this process does.
  if (initialEngine == dalinira::engine::EngineType::WebKit)
    dalinira::engine::WebKitBackend::waitForRetainedWebProcesses();
#endif
  return exitCode;
}
