#include "onboarding/onboarding_manager.h"
#include "onboarding/onboarding_step.h"
#include "onboarding/onboarding_view.h"
#include "onboarding/welcome_step.h"
#include "onboarding/import_step.h"
#include "onboarding/engine_step.h"
#include "onboarding/layout_step.h"
#include "onboarding/search_privacy_step.h"
#include "onboarding/ready_step.h"
#include "onboarding/browser_data_importer.h"
#include "onboarding/aurora_wave_background.h"
#include "onboarding/onboarding_window.h"
#include "layout/browser_layout.h"
#include "layout/browser_layout_controller.h"
#include "layout/browser_side_panel.h"
#include "core/application_identity.h"
#include "engine/engine_backend.h"
#include "i18n/language_manager.h"
#include "i18n/i18n.h"

#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QHBoxLayout>
#include <QListWidget>
#include <QRadioButton>
#include <QFrame>
#include <QLabel>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextStream>
#include <QToolButton>
#include <sqlite3.h>
#include <cassert>
#include <iostream>

using namespace dalinira::onboarding;

static void testFirstLaunchDetection() {
  std::cout << "[TEST] Running testFirstLaunchDetection...\n";
  QTemporaryDir tempDir;
  assert(tempDir.isValid());

  QSettings settings(tempDir.path() + QStringLiteral("/test_settings.ini"), QSettings::IniFormat);
  OnboardingManager manager(&settings);

  assert(!manager.isOnboardingCompleted());
  assert(manager.currentStepIndex() == 0);
  assert(manager.currentStepId() == QStringLiteral("welcome"));
  assert(manager.onboardingVersion() == OnboardingManager::kCurrentOnboardingVersion);
  assert(manager.shouldShowOnboarding());
  std::cout << "  -> testFirstLaunchDetection PASSED\n";
}

static void testCompletionPersistence() {
  std::cout << "[TEST] Running testCompletionPersistence...\n";
  QTemporaryDir tempDir;
  assert(tempDir.isValid());

  const QString settingsPath = tempDir.path() + QStringLiteral("/test_settings.ini");
  {
    QSettings settings(settingsPath, QSettings::IniFormat);
    OnboardingManager manager(&settings);

    bool signalFired = false;
    QObject::connect(&manager, &OnboardingManager::onboardingCompleted, [&] {
      signalFired = true;
    });

    manager.completeOnboarding();
    assert(signalFired);
    assert(manager.isOnboardingCompleted());
    assert(!manager.shouldShowOnboarding());
    assert(manager.completedAt().isValid());
  }

  // Reload from fresh QSettings instance pointing to same file
  {
    QSettings settings(settingsPath, QSettings::IniFormat);
    OnboardingManager reloaded(&settings);
    assert(reloaded.isOnboardingCompleted());
    assert(!reloaded.shouldShowOnboarding());
    assert(reloaded.completedAt().isValid());
  }
  std::cout << "  -> testCompletionPersistence PASSED\n";
}

static void testStepStateAndSafeResume() {
  std::cout << "[TEST] Running testStepStateAndSafeResume...\n";
  QTemporaryDir tempDir;
  assert(tempDir.isValid());

  const QString settingsPath = tempDir.path() + QStringLiteral("/test_settings.ini");
  {
    QSettings settings(settingsPath, QSettings::IniFormat);
    OnboardingManager manager(&settings);

    assert(!manager.isOnboardingCompleted());
    manager.setCurrentStepIndex(2);
    manager.setCurrentStepId(QStringLiteral("step_preview"));
  }

  // Simulate browser restart during incomplete setup
  {
    QSettings settings(settingsPath, QSettings::IniFormat);
    OnboardingManager resumed(&settings);

    // Setup was not finished, so onboarding must resume!
    assert(!resumed.isOnboardingCompleted());
    assert(resumed.shouldShowOnboarding());
    assert(resumed.currentStepIndex() == 2);
    assert(resumed.currentStepId() == QStringLiteral("step_preview"));
  }
  std::cout << "  -> testStepStateAndSafeResume PASSED\n";
}

static void testSkipOnboarding() {
  std::cout << "[TEST] Running testSkipOnboarding...\n";
  QTemporaryDir tempDir;
  assert(tempDir.isValid());

  const QString settingsPath = tempDir.path() + QStringLiteral("/test_settings.ini");
  QSettings settings(settingsPath, QSettings::IniFormat);
  OnboardingManager manager(&settings);

  bool completedSignal = false;
  bool skippedSignal = false;
  QObject::connect(&manager, &OnboardingManager::onboardingCompleted, [&] {
    completedSignal = true;
  });
  QObject::connect(&manager, &OnboardingManager::onboardingSkipped, [&] {
    skippedSignal = true;
  });

  manager.skipOnboarding();
  assert(completedSignal);
  assert(skippedSignal);
  assert(manager.isOnboardingCompleted());
  assert(!manager.shouldShowOnboarding());
  std::cout << "  -> testSkipOnboarding PASSED\n";
}

static void testReset() {
  std::cout << "[TEST] Running testReset...\n";
  QTemporaryDir tempDir;
  assert(tempDir.isValid());

  const QString settingsPath = tempDir.path() + QStringLiteral("/test_settings.ini");
  QSettings settings(settingsPath, QSettings::IniFormat);
  OnboardingManager manager(&settings);

  manager.setCurrentStepIndex(3);
  manager.completeOnboarding();
  assert(manager.isOnboardingCompleted());

  bool resetSignal = false;
  QObject::connect(&manager, &OnboardingManager::stateReset, [&] {
    resetSignal = true;
  });

  manager.reset();
  assert(resetSignal);
  assert(!manager.isOnboardingCompleted());
  assert(manager.currentStepIndex() == 0);
  assert(manager.shouldShowOnboarding());
  std::cout << "  -> testReset PASSED\n";
}

static void testReducedMotionAndAccessibility() {
  std::cout << "[TEST] Running testReducedMotionAndAccessibility...\n";
  QTemporaryDir tempDir;
  assert(tempDir.isValid());

  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, tempDir.path());

  {
    QSettings settings;
    settings.setValue(QStringLiteral("ui/reduceMotion"), true);
    settings.sync();
    assert(!OnboardingManager::animationsEnabled());
  }

  {
    QSettings settings;
    settings.setValue(QStringLiteral("ui/reduceMotion"), false);
    settings.setValue(QStringLiteral("browser/animationsEnabled"), false);
    settings.sync();
    assert(!OnboardingManager::animationsEnabled());
  }

  {
    QSettings settings;
    settings.setValue(QStringLiteral("ui/reduceMotion"), false);
    settings.setValue(QStringLiteral("browser/animationsEnabled"), true);
    settings.setValue(QStringLiteral("ui/animationsEnabled"), true);
    settings.sync();
    assert(OnboardingManager::animationsEnabled());
  }
  std::cout << "  -> testReducedMotionAndAccessibility PASSED\n";
}

static void testOnboardingViewAndWelcomeStep() {
  std::cout << "[TEST] Running testOnboardingViewAndWelcomeStep...\n";
  QTemporaryDir tempDir;
  assert(tempDir.isValid());

  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, tempDir.path());

  OnboardingManager::instance().reset();

  OnboardingView view;
  assert(view.stepCount() == 6);
  assert(view.currentStepIndex() == 0);

  OnboardingStep *welcome = view.currentStep();
  assert(welcome != nullptr);
  assert(welcome->stepId() == QStringLiteral("welcome"));
  assert(!welcome->stepTitle().isEmpty());
  assert(!welcome->canGoBack());
  assert(welcome->canGoNext());
  assert(welcome->canSkip());

  // Verify 5 feature cards exist on WelcomeStep
  const auto featureCards = welcome->findChildren<QFrame *>(QStringLiteral("onboarding-feature-card"));
  assert(featureCards.size() == 5);

  // Verify application language selection combo exists on WelcomeStep
  auto *langCombo = welcome->findChild<QComboBox *>(QStringLiteral("welcome-language-combo"));
  assert(langCombo != nullptr);
  assert(langCombo->count() >= 2);

  // Verify selecting a language works
  const int targetIndex = langCombo->findData(QStringLiteral("en"));
  if (targetIndex >= 0) {
    langCombo->setCurrentIndex(targetIndex);
    assert(dalinira::i18n::LanguageManager::instance().languagePreference() == QStringLiteral("en"));
  }

  // Transition to step 1 (Import Data)
  view.goToNextStep();
  assert(view.currentStepIndex() == 1);
  assert(view.currentStep()->stepId() == QStringLiteral("import"));
  assert(OnboardingManager::instance().currentStepIndex() == 1);
  assert(OnboardingManager::instance().currentStepId() == QStringLiteral("import"));

  // Transition to step 2 (Engine Selection)
  view.goToNextStep();
  assert(view.currentStepIndex() == 2);
  assert(view.currentStep()->stepId() == QStringLiteral("engine"));
  assert(OnboardingManager::instance().currentStepIndex() == 2);
  assert(OnboardingManager::instance().currentStepId() == QStringLiteral("engine"));

  // Transition to step 3 (Layout & Panels)
  view.goToNextStep();
  assert(view.currentStepIndex() == 3);
  assert(view.currentStep()->stepId() == QStringLiteral("layout"));
  assert(OnboardingManager::instance().currentStepIndex() == 3);
  assert(OnboardingManager::instance().currentStepId() == QStringLiteral("layout"));

  // Transition to step 4 (Search & Privacy), then back to verify navigation.
  view.goToNextStep();
  assert(view.currentStepIndex() == 4);
  assert(view.currentStep()->stepId() == QStringLiteral("search_privacy"));
  assert(OnboardingManager::instance().currentStepId() == QStringLiteral("search_privacy"));
  view.goToPreviousStep();
  assert(view.currentStepIndex() == 3);
  assert(view.currentStep()->stepId() == QStringLiteral("layout"));

  // Transition back to step 2 (Engine)
  view.goToPreviousStep();
  assert(view.currentStepIndex() == 2);
  assert(view.currentStep()->stepId() == QStringLiteral("engine"));

  // Transition back to step 1 (Import)
  view.goToPreviousStep();
  assert(view.currentStepIndex() == 1);
  assert(view.currentStep()->stepId() == QStringLiteral("import"));

  // Transition back to step 0 (Welcome)
  view.goToPreviousStep();
  assert(view.currentStepIndex() == 0);
  assert(view.currentStep()->stepId() == QStringLiteral("welcome"));
  assert(OnboardingManager::instance().currentStepIndex() == 0);

  // Complete onboarding
  bool finishedFired = false;
  QObject::connect(&view, &OnboardingView::finished, [&] {
    finishedFired = true;
  });
  view.complete();
  assert(finishedFired);
  assert(OnboardingManager::instance().isOnboardingCompleted());

  std::cout << "  -> testOnboardingViewAndWelcomeStep PASSED\n";
}


static void testImportChoicePersistence() {
  std::cout << "[TEST] Running testImportChoicePersistence...\n";
  QTemporaryDir tempDir;
  assert(tempDir.isValid());

  const QString settingsPath = tempDir.path() + QStringLiteral("/test_settings.ini");
  {
    QSettings settings(settingsPath, QSettings::IniFormat);
    OnboardingManager manager(&settings);

    // Initial defaults
    assert(manager.importChoice() == OnboardingManager::ImportChoice::Fresh);
    assert(manager.importBookmarksEnabled());
    assert(manager.importHistoryEnabled());
    assert(!manager.importPasswordsEnabled());
    assert(manager.importHtmlPath().isEmpty());

    // Update settings
    manager.setImportChoice(OnboardingManager::ImportChoice::Import);
    manager.setImportSource(QStringLiteral("brave"));
    manager.setImportBookmarksEnabled(false);
    manager.setImportHistoryEnabled(true);
    manager.setImportPasswordsEnabled(true);
    manager.setImportHtmlPath(QStringLiteral("/home/user/bookmarks.html"));

    assert(manager.importChoice() == OnboardingManager::ImportChoice::Import);
    assert(manager.importSource() == QStringLiteral("brave"));
    assert(!manager.importBookmarksEnabled());
    assert(manager.importHistoryEnabled());
    assert(manager.importPasswordsEnabled());
    assert(manager.importHtmlPath() == QStringLiteral("/home/user/bookmarks.html"));
  }

  // Reload from disk to verify persistence
  {
    QSettings settings(settingsPath, QSettings::IniFormat);
    OnboardingManager resumed(&settings);

    assert(resumed.importChoice() == OnboardingManager::ImportChoice::Import);
    assert(resumed.importSource() == QStringLiteral("brave"));
    assert(!resumed.importBookmarksEnabled());
    assert(resumed.importHistoryEnabled());
    assert(resumed.importPasswordsEnabled());
    assert(resumed.importHtmlPath() == QStringLiteral("/home/user/bookmarks.html"));

    // Reset should revert to defaults
    resumed.reset();
    assert(resumed.importChoice() == OnboardingManager::ImportChoice::Fresh);
    assert(resumed.importBookmarksEnabled());
    assert(resumed.importHistoryEnabled());
    assert(!resumed.importPasswordsEnabled());
  }
  std::cout << "  -> testImportChoicePersistence PASSED\n";
}

static void testBrowserDataImporterParsers() {
  std::cout << "[TEST] Running testBrowserDataImporterParsers...\n";

  // Test 1: JSON parser (Chromium/Brave format)
  const QByteArray sampleJson = R"json({
    "roots": {
      "bookmark_bar": {
        "name": "Bookmarks bar",
        "type": "folder",
        "children": [
          {
            "name": "Google",
            "type": "url",
            "url": "https://www.google.com"
          },
          {
            "name": "Dev",
            "type": "folder",
            "children": [
              {
                "name": "GitHub",
                "type": "url",
                "url": "https://github.com"
              }
            ]
          }
        ]
      }
    }
  })json";

  const auto jsonBookmarks = BrowserDataImporter::parseBookmarksFromJson(sampleJson);
  assert(jsonBookmarks.size() == 2);
  assert(jsonBookmarks[0].title == QStringLiteral("Google"));
  assert(jsonBookmarks[0].url == QUrl(QStringLiteral("https://www.google.com")));
  assert(jsonBookmarks[0].folder == QStringLiteral("Bookmarks bar"));
  assert(jsonBookmarks[1].title == QStringLiteral("GitHub"));
  assert(jsonBookmarks[1].url == QUrl(QStringLiteral("https://github.com")));
  assert(jsonBookmarks[1].folder == QStringLiteral("Bookmarks bar/Dev"));

  // Test 2: HTML parser (Netscape format)
  const QString sampleHtml = QStringLiteral(
      "<!DOCTYPE NETSCAPE-Bookmark-file-1>\n"
      "<META HTTP-EQUIV=\"Content-Type\" CONTENT=\"text/html; charset=UTF-8\">\n"
      "<TITLE>Bookmarks</TITLE>\n"
      "<H1>Bookmarks</H1>\n"
      "<DL><p>\n"
      "  <DT><H3 ADD_DATE=\"1600000000\">Favorites</H3>\n"
      "  <DL><p>\n"
      "    <DT><A HREF=\"https://dalinira.org/\" ADD_DATE=\"1600000001\">DaliNira Browser</A>\n"
      "  </DL><p>\n"
      "  <DT><A HREF=\"https://qt.io/\">Qt Framework</A>\n"
      "</DL><p>\n");

  const auto htmlBookmarks = BrowserDataImporter::parseBookmarksFromHtml(sampleHtml);
  assert(htmlBookmarks.size() == 2);
  assert(htmlBookmarks[0].title == QStringLiteral("DaliNira Browser"));
  assert(htmlBookmarks[0].url == QUrl(QStringLiteral("https://dalinira.org/")));
  assert(htmlBookmarks[0].folder == QStringLiteral("Favorites"));
  assert(htmlBookmarks[1].title == QStringLiteral("Qt Framework"));
  assert(htmlBookmarks[1].url == QUrl(QStringLiteral("https://qt.io/")));

  // Test 3: Invalid / empty inputs
  assert(BrowserDataImporter::parseBookmarksFromJson(QByteArray()).isEmpty());
  assert(BrowserDataImporter::parseBookmarksFromJson("{invalid json}").isEmpty());
  assert(BrowserDataImporter::parseBookmarksFromHtml(QString()).isEmpty());

  std::cout << "  -> testBrowserDataImporterParsers PASSED\n";
}

class MockDataReceiver final : public IBrowserDataReceiver {
 public:
  struct BookmarkRecord {
    QUrl url;
    QString title;
    QString folder;
  };

  bool addBookmark(const QUrl &url, const QString &title, const QString &folder) override {
    addedBookmarks.append({url, title, folder});
    return true;
  }

  void createBookmarkFolder(const QString &folder) override {
    createdFolders.append(folder);
  }

  void recordHistory(const QUrl &url, const QString &title, bool isReload = false) override {
    Q_UNUSED(isReload);
    recordedHistory.append(qMakePair(url, title));
  }

  QList<BookmarkRecord> addedBookmarks;
  QStringList createdFolders;
  QList<QPair<QUrl, QString>> recordedHistory;
};

static void executeSql(sqlite3 *database, const QString &sql) {
  char *error = nullptr;
  const QByteArray encoded = sql.toUtf8();
  const int result = sqlite3_exec(database, encoded.constData(), nullptr, nullptr, &error);
  if (error) sqlite3_free(error);
  assert(result == SQLITE_OK);
}

static void testSqliteBookmarkAndHistoryImport() {
  std::cout << "[TEST] Running testSqliteBookmarkAndHistoryImport...\n";
  QTemporaryDir tempDir;
  assert(tempDir.isValid());

  const QString chromiumPath = tempDir.filePath(QStringLiteral("History"));
  sqlite3 *chromium = nullptr;
  assert(sqlite3_open(QFile::encodeName(chromiumPath).constData(), &chromium) == SQLITE_OK);
  executeSql(chromium,
      QStringLiteral("CREATE TABLE urls(url TEXT, title TEXT, last_visit_time INTEGER, "
                     "visit_count INTEGER, typed_count INTEGER);"
                     "INSERT INTO urls VALUES('https://chromium.example/path', 'Chromium entry', "
                     "13344473600000000, 7, 2);"));
  sqlite3_close(chromium);

  const auto chromiumHistory = BrowserDataImporter::readHistoryFromDatabase(
      chromiumPath, QStringLiteral("chromium"));
  assert(chromiumHistory.size() == 1);
  assert(chromiumHistory[0].visitedAt.toMSecsSinceEpoch() == 1700000000000LL);
  assert(chromiumHistory[0].visitCount == 7);
  assert(chromiumHistory[0].typedCount == 2);

  const QString firefoxPath = tempDir.filePath(QStringLiteral("places.sqlite"));
  sqlite3 *firefox = nullptr;
  assert(sqlite3_open(QFile::encodeName(firefoxPath).constData(), &firefox) == SQLITE_OK);
  executeSql(firefox,
      QStringLiteral("CREATE TABLE moz_places(id INTEGER PRIMARY KEY, url TEXT, title TEXT, "
                     "last_visit_date INTEGER, visit_count INTEGER, typed INTEGER);"
                     "CREATE TABLE moz_bookmarks(id INTEGER PRIMARY KEY, parent INTEGER, type INTEGER, "
                     "fk INTEGER, title TEXT);"
                     "INSERT INTO moz_places VALUES(10, 'https://firefox.example/docs', 'Firefox entry', "
                     "1700000100000000, 4, 1);"
                     "INSERT INTO moz_bookmarks VALUES(2, 1, 2, NULL, 'Toolbar');"
                     "INSERT INTO moz_bookmarks VALUES(3, 2, 2, NULL, 'Development');"
                     "INSERT INTO moz_bookmarks VALUES(4, 3, 1, 10, 'Firefox bookmark');"));
  sqlite3_close(firefox);

  const auto firefoxBookmarks = BrowserDataImporter::parseBookmarksFromSqlite(firefoxPath);
  assert(firefoxBookmarks.size() == 1);
  assert(firefoxBookmarks[0].folder == QStringLiteral("Toolbar/Development"));
  const auto firefoxHistory = BrowserDataImporter::readHistoryFromDatabase(
      firefoxPath, QStringLiteral("firefox"));
  assert(firefoxHistory.size() == 1);
  assert(firefoxHistory[0].visitedAt.toMSecsSinceEpoch() == 1700000100000LL);
  assert(firefoxHistory[0].visitCount == 4);

  std::cout << "  -> testSqliteBookmarkAndHistoryImport PASSED\n";
}

static void testDetectedBrowsersAndRealSystemDetection() {
  std::cout << "[TEST] Running testDetectedBrowsersAndRealSystemDetection...\n";

  const auto browsers = BrowserDataImporter::detectBrowsers();
  std::cout << "  Detected " << browsers.size() << " browsers on system:\n";
  bool foundChrome = false;
  bool foundBrave = false;
  for (const auto &b : browsers) {
    std::cout << "    - ID: " << b.id.toStdString() << " | Name: " << b.displayName.toStdString()
              << " | Bookmarks: " << (b.hasBookmarks ? "YES" : "NO")
              << " | History: " << (b.hasHistory ? "YES" : "NO")
              << " | Installed: " << (b.installed ? "YES" : "NO")
              << " | Icon: " << b.iconName.toStdString() << "\n";
    if (b.id == QStringLiteral("chrome")) foundChrome = true;
    if (b.id == QStringLiteral("brave")) foundBrave = true;
    if (b.installed) {
      assert(!b.desktopFilePath.isEmpty());
      assert(!b.iconName.isEmpty());
    }
  }

  Q_UNUSED(foundChrome);
  Q_UNUSED(foundBrave);
  for (const auto &browser : browsers) {
    if (browser.id == QLatin1String("html_file")) continue;
    assert(browser.hasBookmarks || browser.hasHistory);
    assert(browser.browserType == QLatin1String("chromium") ||
           browser.browserType == QLatin1String("firefox") ||
           browser.browserType == QLatin1String("zen") ||
           browser.browserType == QLatin1String("falkon"));
  }

  // Test real Chrome bookmark and history parsing if found
  for (const auto &b : browsers) {
    if (b.id == QStringLiteral("chrome")) {
      if (b.hasBookmarks) {
        const auto chromeBookmarks = BrowserDataImporter::parseBookmarksFromFile(b.bookmarksPath);
        assert(!chromeBookmarks.isEmpty());
        std::cout << "    Verified Chrome parsed " << chromeBookmarks.size() << " bookmarks!\n";
      }
      if (b.hasHistory) {
        MockDataReceiver histReceiver;
        const int count = BrowserDataImporter::importHistoryFromDatabase(b.historyPath, b.browserType, &histReceiver);
        assert(count == histReceiver.recordedHistory.size());
        std::cout << "    Chrome profile contains " << count << " importable history items.\n";
      }
    }
  }

  std::cout << "  -> testDetectedBrowsersAndRealSystemDetection PASSED\n";
}

class MockSearchPrivacyPreferences final : public ISearchPrivacyPreferences {
 public:
  QString searchEngine() const override { return engine; }
  void setSearchEngine(const QString &value) override {
    engine = value;
    ++searchWrites;
  }
  bool blockerEnabled() const override { return blocker; }
  void setBlockerEnabled(bool enabled) override {
    blocker = enabled;
    ++blockerWrites;
  }

  QString engine{QStringLiteral("DuckDuckGo")};
  bool blocker = true;
  int searchWrites = 0;
  int blockerWrites = 0;
};

class MockDefaultBrowserIntegration final
    : public dalinira::core::IDefaultBrowserIntegration {
 public:
  bool isSupported() const override {
    return result != dalinira::core::DefaultBrowserResult::Unsupported;
  }
  void requestDefaultBrowser(
      std::function<void(dalinira::core::DefaultBrowserResult)> completion) override {
    ++requests;
    completion(result);
  }

  dalinira::core::DefaultBrowserResult result =
      dalinira::core::DefaultBrowserResult::Success;
  int requests = 0;
};

static void testDataReceiverImportExecution() {
  std::cout << "[TEST] Running testDataReceiverImportExecution...\n";
  QTemporaryDir tempDir;
  assert(tempDir.isValid());

  // Create temporary HTML bookmark file
  const QString tempBookmarkPath = tempDir.path() + QStringLiteral("/bookmarks.html");
  {
    QFile file(tempBookmarkPath);
    assert(file.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream out(&file);
    out << "<!DOCTYPE NETSCAPE-Bookmark-file-1>\n";
    out << "<DL><p>\n";
    out << "<DT><H3>Tech</H3>\n<DL><p>\n";
    out << "<DT><A HREF=\"https://kde.org\">KDE</A>\n";
    out << "</DL><p>\n";
    out << "<DT><A HREF=\"https://kernel.org\">Linux Kernel</A>\n";
    out << "</DL><p>\n";
  }

  MockDataReceiver receiver;

  // Import bookmarks via mock receiver
  const int count = BrowserDataImporter::importBookmarks(tempBookmarkPath, &receiver);
  assert(count == 2);
  assert(receiver.addedBookmarks.size() == 2);
  assert(receiver.createdFolders.contains(QStringLiteral("Tech")));
  assert(receiver.addedBookmarks[0].title == QStringLiteral("KDE"));
  assert(receiver.addedBookmarks[0].folder == QStringLiteral("Tech"));
  assert(receiver.addedBookmarks[1].title == QStringLiteral("Linux Kernel"));

  const QString secondBookmarkPath = tempDir.path() + QStringLiteral("/account-bookmarks.html");
  {
    QFile file(secondBookmarkPath);
    assert(file.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream out(&file);
    out << "<DL><p>\n";
    out << "<DT><A HREF=\"https://kde.org\">Duplicate KDE</A>\n";
    out << "<DT><A HREF=\"https://qt.io\">Qt</A>\n";
    out << "</DL><p>\n";
  }
  MockDataReceiver mergedReceiver;
  const int mergedCount = BrowserDataImporter::importBookmarks(
      QStringList{tempBookmarkPath, secondBookmarkPath}, &mergedReceiver);
  assert(mergedCount == 3);
  assert(mergedReceiver.addedBookmarks.size() == 3);

  // Import history via mock receiver
  QList<QPair<QUrl, QString>> historyEntries = {
    {QUrl(QStringLiteral("https://doc.qt.io")), QStringLiteral("Qt Docs")},
    {QUrl(QStringLiteral("https://cppreference.com")), QStringLiteral("cppreference")}
  };
  const int histCount = BrowserDataImporter::importHistory(historyEntries, &receiver);
  assert(histCount == 2);
  assert(receiver.recordedHistory.size() == 2);
  assert(receiver.recordedHistory[0].first == QUrl(QStringLiteral("https://doc.qt.io")));
  assert(receiver.recordedHistory[1].first == QUrl(QStringLiteral("https://cppreference.com")));

  std::cout << "  -> testDataReceiverImportExecution PASSED\n";
}

static void testEngineSelectionAndPersistence() {
  std::cout << "[TEST] Running testEngineSelectionAndPersistence...\n";
  QTemporaryDir tempDir;
  assert(tempDir.isValid());

  const QString settingsPath = tempDir.path() + QStringLiteral("/test_settings.ini");
  {
    QSettings settings(settingsPath, QSettings::IniFormat);
    OnboardingManager manager(&settings);

    // Initial default should be chromium
    assert(manager.selectedEngine() == QStringLiteral("chromium"));

    bool signalFired = false;
    QString signaledEngine;
    QObject::connect(&manager, &OnboardingManager::engineSelectionChanged, [&](const QString &engine) {
      signalFired = true;
      signaledEngine = engine;
    });

    // Select webkit
    manager.setSelectedEngine(QStringLiteral("webkit"));
    assert(signalFired);
    assert(signaledEngine == QStringLiteral("webkit"));
    assert(manager.selectedEngine() == QStringLiteral("webkit"));
  }

  // Reload from disk to verify persistence using existing browser/engine settings architecture
  {
    QSettings settings(settingsPath, QSettings::IniFormat);
    OnboardingManager resumed(&settings);
    assert(resumed.selectedEngine() == QStringLiteral("webkit"));

    // Select chromium
    resumed.setSelectedEngine(QStringLiteral("chromium"));
    assert(resumed.selectedEngine() == QStringLiteral("chromium"));
  }

  // Check again
  {
    QSettings settings(settingsPath, QSettings::IniFormat);
    OnboardingManager reloaded(&settings);
    assert(reloaded.selectedEngine() == QStringLiteral("chromium"));

    // Reset should revert to default chromium
    reloaded.setSelectedEngine(QStringLiteral("webkit"));
    reloaded.reset();
    assert(reloaded.selectedEngine() == QStringLiteral("chromium"));
  }
  std::cout << "  -> testEngineSelectionAndPersistence PASSED\n";
}

static void testEngineStepSelectionAndAnimationPolicy() {
  std::cout << "[TEST] Running testEngineStepSelectionAndAnimationPolicy...\n";
  QTemporaryDir tempDir;
  assert(tempDir.isValid());

  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, tempDir.path());

  OnboardingManager::instance().reset();

  EngineStep step;
  assert(step.stepId() == QStringLiteral("engine"));
  assert(!step.stepTitle().isEmpty());
  assert(!step.stepSubtitle().isEmpty());
  assert(step.canGoBack());
  assert(step.canGoNext());
  assert(step.canSkip());
  assert(!step.nextButtonText().isEmpty());

  // Test selecting WebKit
  step.selectEngine(QStringLiteral("webkit"), false);
  assert(step.selectedEngine() == QStringLiteral("webkit"));
  assert(OnboardingManager::instance().selectedEngine() == QStringLiteral("webkit"));

  // Test selecting Chromium
  step.selectEngine(QStringLiteral("chromium"), false);
  assert(step.selectedEngine() == QStringLiteral("chromium"));
  assert(OnboardingManager::instance().selectedEngine() == QStringLiteral("chromium"));

  // Setup does not offer Gecko yet: a Gecko key, from a call or saved
  // settings, selects Chromium.
  step.selectEngine(QStringLiteral("webkit"), false);
  step.selectEngine(QStringLiteral(" Gecko "), false);
  assert(step.selectedEngine() == QStringLiteral("chromium"));
  assert(OnboardingManager::instance().selectedEngine() == QStringLiteral("chromium"));
  OnboardingManager::instance().setSelectedEngine(QStringLiteral("gecko"));
  step.onStepEntering();
  assert(step.selectedEngine() == QStringLiteral("chromium"));
  assert(OnboardingManager::instance().selectedEngine() == QStringLiteral("chromium"));
  assert(step.findChild<QRadioButton *>(QStringLiteral("gecko-radio")) == nullptr);

  // Exercise the real controls: the radios are inside different card parents
  // and must still behave as one exclusive choice group.
  auto *webkitRadio = step.findChild<QRadioButton *>(QStringLiteral("webkit-radio"));
  auto *chromiumRadio = step.findChild<QRadioButton *>(QStringLiteral("chromium-radio"));
  assert(webkitRadio != nullptr);
  assert(chromiumRadio != nullptr);
  webkitRadio->click();
  assert(step.selectedEngine() == QStringLiteral("webkit"));
  assert(webkitRadio->isChecked());
  assert(!chromiumRadio->isChecked());
  chromiumRadio->click();
  assert(step.selectedEngine() == QStringLiteral("chromium"));
  assert(chromiumRadio->isChecked());
  assert(!webkitRadio->isChecked());

  // Test onStepEntering and onStepLeaving
  OnboardingManager::instance().setSelectedEngine(QStringLiteral("webkit"));
  step.onStepEntering();
  assert(step.selectedEngine() == QStringLiteral("webkit"));

  step.selectEngine(QStringLiteral("chromium"), false);
  step.onStepLeaving();
  assert(OnboardingManager::instance().selectedEngine() == QStringLiteral("chromium"));

  std::cout << "  -> testEngineStepSelectionAndAnimationPolicy PASSED\n";
}

// Keyboard navigation and assistive technologies (AT-SPI "Toggle") check a
// radio without a click; every onboarding choice must still be saved.
static QRadioButton *radioInside(QWidget *root, const QString &ancestorName) {
  for (QRadioButton *radio : root->findChildren<QRadioButton *>()) {
    for (QWidget *w = radio->parentWidget(); w && w != root; w = w->parentWidget())
      if (w->objectName() == ancestorName) return radio;
  }
  return nullptr;
}

static void testRadiosSelectWithoutClick() {
  std::cout << "[TEST] Running testRadiosSelectWithoutClick...\n";
  QTemporaryDir tempDir;
  assert(tempDir.isValid());
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, tempDir.path());
  OnboardingManager::instance().reset();

  EngineStep engine;
  engine.findChild<QRadioButton *>(QStringLiteral("webkit-radio"))->setChecked(true);
  assert(engine.selectedEngine() == QStringLiteral("webkit"));
  assert(OnboardingManager::instance().selectedEngine() == QStringLiteral("webkit"));
  engine.findChild<QRadioButton *>(QStringLiteral("chromium-radio"))->setChecked(true);
  assert(OnboardingManager::instance().selectedEngine() == QStringLiteral("chromium"));

  ImportStep import;
  import.findChild<QRadioButton *>(QStringLiteral("import-radio"))->setChecked(true);
  assert(OnboardingManager::instance().importChoice() == OnboardingManager::ImportChoice::Import);
  import.findChild<QRadioButton *>(QStringLiteral("fresh-radio"))->setChecked(true);
  assert(OnboardingManager::instance().importChoice() == OnboardingManager::ImportChoice::Fresh);

  LayoutStep layout;
  QRadioButton *left = radioInside(&layout, QStringLiteral("leftLayoutCard"));
  assert(left);
  left->setChecked(true);
  assert(layout.selectedLayoutMode() == dalinira::layout::BrowserLayoutMode::LeftPanel);
  assert(OnboardingManager::instance().selectedLayout() ==
         dalinira::layout::layoutModeToString(dalinira::layout::BrowserLayoutMode::LeftPanel));
  QList<QRadioButton *> themeRadios;
  for (QRadioButton *radio : layout.findChildren<QRadioButton *>()) {
    bool inCard = false;
    for (QWidget *w = radio->parentWidget(); w && w != &layout; w = w->parentWidget())
      inCard = inCard || w->objectName().endsWith(QLatin1String("LayoutCard"));
    if (!inCard) themeRadios.append(radio);
  }
  assert(themeRadios.size() == 3);  // System, Light, Dark
  themeRadios[2]->setChecked(true);
  assert(layout.selectedTheme() == dalinira::layout::AppearanceTheme::Dark);
  assert(OnboardingManager::instance().selectedTheme() == QStringLiteral("dark"));

  MockSearchPrivacyPreferences preferences;
  SearchPrivacyStep search(&preferences);
  const QList<QRadioButton *> searchRadios = search.findChildren<QRadioButton *>();
  assert(searchRadios.size() == 6);  // Google, DuckDuckGo, Startpage, Mojeek; blocker on, off
  searchRadios[2]->setChecked(true);
  assert(preferences.engine == QStringLiteral("Startpage") && preferences.searchWrites == 1);
  searchRadios[5]->setChecked(true);
  assert(!preferences.blocker && preferences.blockerWrites == 1);
  searchRadios[2]->setChecked(true);  // already selected: no second write
  assert(preferences.searchWrites == 1);

  OnboardingManager::instance().reset();
  std::cout << "  -> testRadiosSelectWithoutClick PASSED\n";
}

// With nothing saved yet the engine step offers the engine this session runs
// (e.g. started with --engine webkit), so finishing setup does not switch.
static void testEngineChoiceDefaultsToRunningEngine() {
  std::cout << "[TEST] Running testEngineChoiceDefaultsToRunningEngine...\n";
  QTemporaryDir tempDir;
  assert(tempDir.isValid());
  QSettings settings(tempDir.path() + QStringLiteral("/engine-default.ini"), QSettings::IniFormat);
  OnboardingManager manager(&settings);
  using dalinira::engine::EngineBackend;
  using dalinira::engine::EngineType;
  const EngineType before = EngineBackend::selectedEngineType();
  EngineBackend::setSelectedEngineType(EngineType::WebKit);
  assert(manager.selectedEngine() == QStringLiteral("webkit"));
  EngineBackend::setSelectedEngineType(EngineType::Chromium);
  assert(manager.selectedEngine() == QStringLiteral("chromium"));
  manager.setSelectedEngine(QStringLiteral("webkit"));  // a saved choice wins
  assert(manager.selectedEngine() == QStringLiteral("webkit"));
  EngineBackend::setSelectedEngineType(before);
  std::cout << "  -> testEngineChoiceDefaultsToRunningEngine PASSED\n";
}

// First-run state belongs to the profile's settings, not to the machine.
static void testFirstRunStateIsPerProfile() {
  std::cout << "[TEST] Running testFirstRunStateIsPerProfile...\n";
  QTemporaryDir tempDir;
  assert(tempDir.isValid());
  QSettings first(tempDir.path() + QStringLiteral("/a.ini"), QSettings::IniFormat);
  QSettings second(tempDir.path() + QStringLiteral("/b.ini"), QSettings::IniFormat);
  OnboardingManager a(&first);
  OnboardingManager b(&second);
  assert(a.shouldShowOnboarding() && b.shouldShowOnboarding());
  a.completeOnboarding();
  assert(!a.shouldShowOnboarding() && b.shouldShowOnboarding());
  b.skipOnboarding();
  assert(!b.shouldShowOnboarding());
  QSettings reopened(tempDir.path() + QStringLiteral("/a.ini"), QSettings::IniFormat);
  assert(reopened.value(QStringLiteral("onboarding/completed")).toBool());
  std::cout << "  -> testFirstRunStateIsPerProfile PASSED\n";
}

// Word-wrapped card text must not be clipped, in any language, and the cards
// must not grow far past their text either.
static void testWelcomeCardsFitTheirText() {
  std::cout << "[TEST] Running testWelcomeCardsFitTheirText...\n";
  QWidget host;
  auto *layout = new QHBoxLayout(&host);
  auto *step = new WelcomeStep(&host);
  layout->addWidget(step);
  host.resize(1280, 800);
  host.show();
  for (int i = 0; i < 5; ++i) QCoreApplication::processEvents();
  const auto cards = step->findChildren<QFrame *>(QStringLiteral("onboarding-feature-card"));
  assert(cards.size() == 5);
  for (QFrame *card : cards) {
    const QList<QLabel *> labels = card->findChildren<QLabel *>();
    QLabel *title = nullptr;
    QLabel *description = nullptr;
    for (QLabel *label : labels) {
      if (!label->wordWrap()) continue;
      if (!title) title = label;
      else if (!description) description = label;
    }
    assert(title && description);
    const QMargins m = card->layout()->contentsMargins();
    const int width = card->width() - m.left() - m.right();
    const int needed = m.top() + m.bottom() + 32 + 2 * card->layout()->spacing() +
                       title->heightForWidth(width) + description->heightForWidth(width);
    if (card->height() < needed || card->height() > std::max(needed, 154) + 40) {
      std::cerr << "card height " << card->height() << " for needed " << needed << "\n";
      assert(false && "welcome card does not fit its text");
    }
  }
  std::cout << "  -> testWelcomeCardsFitTheirText PASSED\n";
}

static void testLayoutModePersistence() {
  std::cout << "[TEST] Running testLayoutModePersistence...\n";
  QTemporaryDir tempDir;
  assert(tempDir.isValid());

  const QString settingsPath = tempDir.path() + QStringLiteral("/test_settings.ini");
  {
    QSettings settings(settingsPath, QSettings::IniFormat);
    OnboardingManager manager(&settings);

    // Initial default layout must be classic
    assert(manager.selectedLayout() == QStringLiteral("classic"));

    // Set layout to left_panel
    manager.setSelectedLayout(QStringLiteral("left_panel"));
    assert(manager.selectedLayout() == QStringLiteral("left_panel"));

    // Set layout to right_panel
    manager.setSelectedLayout(QStringLiteral("right_panel"));
    assert(manager.selectedLayout() == QStringLiteral("right_panel"));

    manager.setSelectedTheme(QStringLiteral("dark"));
    assert(manager.selectedTheme() == QStringLiteral("dark"));

    // Reset reverts to classic
    manager.reset();
    assert(manager.selectedLayout() == QStringLiteral("classic"));
    assert(manager.selectedTheme() == QStringLiteral("system"));
  }

  // Layout mode string conversions
  using namespace dalinira::layout;
  assert(layoutModeToString(BrowserLayoutMode::Classic) == QStringLiteral("classic"));
  assert(layoutModeToString(BrowserLayoutMode::LeftPanel) == QStringLiteral("left_panel"));
  assert(layoutModeToString(BrowserLayoutMode::RightPanel) == QStringLiteral("right_panel"));
  assert(layoutModeToString(BrowserLayoutMode::FloatingSidebar) == QStringLiteral("floating_panel"));

  assert(layoutModeFromString(QStringLiteral("classic")) == BrowserLayoutMode::Classic);
  assert(layoutModeFromString(QStringLiteral("left_panel")) == BrowserLayoutMode::LeftPanel);
  assert(layoutModeFromString(QStringLiteral("left")) == BrowserLayoutMode::LeftPanel);
  assert(layoutModeFromString(QStringLiteral("right_panel")) == BrowserLayoutMode::RightPanel);
  assert(layoutModeFromString(QStringLiteral("right")) == BrowserLayoutMode::RightPanel);
  assert(layoutModeFromString(QStringLiteral("floating")) == BrowserLayoutMode::FloatingSidebar);
  assert(layoutModeFromString(QStringLiteral("unknown")) == BrowserLayoutMode::Classic);
  assert(appearanceThemeFromString(QStringLiteral("system")) == AppearanceTheme::System);
  assert(appearanceThemeFromString(QStringLiteral("light")) == AppearanceTheme::Light);
  assert(appearanceThemeFromString(QStringLiteral("dark")) == AppearanceTheme::Dark);

  std::cout << "  -> testLayoutModePersistence PASSED\n";
}

static void testLayoutStepSelectionAndPreview() {
  std::cout << "[TEST] Running testLayoutStepSelectionAndPreview...\n";
  QTemporaryDir tempDir;
  assert(tempDir.isValid());

  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, tempDir.path());

  OnboardingManager::instance().reset();

  LayoutStep step;
  assert(step.stepId() == QStringLiteral("layout"));
  assert(!step.stepTitle().isEmpty());
  assert(!step.stepSubtitle().isEmpty());
  assert(step.canGoBack());
  assert(step.canGoNext());
  assert(!step.nextButtonText().isEmpty());

  // Default is classic
  assert(step.selectedLayoutMode() == dalinira::layout::BrowserLayoutMode::Classic);

  // Select Left Panel
  step.setSelectedLayoutMode(dalinira::layout::BrowserLayoutMode::LeftPanel);
  assert(step.selectedLayoutMode() == dalinira::layout::BrowserLayoutMode::LeftPanel);
  assert(OnboardingManager::instance().selectedLayout() == QStringLiteral("left_panel"));
  assert(dalinira::layout::currentLayoutMode() == dalinira::layout::BrowserLayoutMode::LeftPanel);

  // Select Right Panel
  step.setSelectedLayoutMode(dalinira::layout::BrowserLayoutMode::RightPanel);
  assert(step.selectedLayoutMode() == dalinira::layout::BrowserLayoutMode::RightPanel);
  assert(OnboardingManager::instance().selectedLayout() == QStringLiteral("right_panel"));
  assert(dalinira::layout::currentLayoutMode() == dalinira::layout::BrowserLayoutMode::RightPanel);

  // Select Classic again
  step.setSelectedLayoutMode(dalinira::layout::BrowserLayoutMode::Classic);
  assert(step.selectedLayoutMode() == dalinira::layout::BrowserLayoutMode::Classic);
  assert(OnboardingManager::instance().selectedLayout() == QStringLiteral("classic"));

  step.setSelectedLayoutMode(dalinira::layout::BrowserLayoutMode::FloatingSidebar);
  assert(step.selectedLayoutMode() == dalinira::layout::BrowserLayoutMode::FloatingSidebar);
  assert(OnboardingManager::instance().selectedLayout() == QStringLiteral("floating_panel"));

  step.setSelectedTheme(dalinira::layout::AppearanceTheme::Light);
  assert(step.selectedTheme() == dalinira::layout::AppearanceTheme::Light);
  assert(OnboardingManager::instance().selectedTheme() == QStringLiteral("light"));

  // onStepEntering / onStepLeaving
  OnboardingManager::instance().setSelectedLayout(QStringLiteral("left_panel"));
  step.onStepEntering();
  assert(step.selectedLayoutMode() == dalinira::layout::BrowserLayoutMode::LeftPanel);

  step.setSelectedLayoutMode(dalinira::layout::BrowserLayoutMode::RightPanel);
  step.onStepLeaving();
  assert(OnboardingManager::instance().selectedLayout() == QStringLiteral("right_panel"));

  // Test Arabic translation of layout preview indicators
  dalinira::i18n::LanguageManager::instance().setLanguagePreference(QStringLiteral("ar"));
  step.retranslateUi();

  auto *liveBadge = step.findChild<QLabel *>(QStringLiteral("miniLiveBadge"));
  assert(liveBadge != nullptr);
  assert(liveBadge->text().contains(QString::fromUtf8("معاينة")));

  auto *heroText = step.findChild<QLabel *>(QStringLiteral("miniHeroText"));
  assert(heroText != nullptr);
  assert(heroText->text().contains(QString::fromUtf8("تجربة ويب")));

  auto *searchText = step.findChild<QLabel *>(QStringLiteral("miniSearchText"));
  assert(searchText != nullptr);
  assert(searchText->text().contains(QString::fromUtf8("ابحث")));

  // Restore Turkish
  dalinira::i18n::LanguageManager::instance().setLanguagePreference(QStringLiteral("tr"));
  step.retranslateUi();

  std::cout << "  -> testLayoutStepSelectionAndPreview PASSED\n";
}

static void testBrowserLayoutControllerRuntimeSwitching() {
  std::cout << "[TEST] Running testBrowserLayoutControllerRuntimeSwitching...\n";
  auto *window = new QWidget();
  auto *bodyContainer = new QWidget(window);
  auto *bodyLayout = new QHBoxLayout(bodyContainer);
  auto *sidePanel = new dalinira::layout::BrowserSidePanel(bodyContainer);
  auto *pageStack = new QWidget(bodyContainer);
  auto *livePage = new QWidget(pageStack);
  bodyLayout->addWidget(pageStack, 1);

  dalinira::layout::BrowserLayoutController controller(window, bodyContainer, bodyLayout, sidePanel, pageStack);

  // Initially classic: side panel is hidden
  controller.setLayoutMode(dalinira::layout::BrowserLayoutMode::Classic, false);
  assert(controller.currentMode() == dalinira::layout::BrowserLayoutMode::Classic);
  assert(!controller.isSidePanelVisible());

  // Switch to LeftPanel: side panel is visible on left
  bool signalEmitted = false;
  QObject::connect(&controller, &dalinira::layout::BrowserLayoutController::layoutModeChanged, [&](dalinira::layout::BrowserLayoutMode m) {
    if (m == dalinira::layout::BrowserLayoutMode::LeftPanel) signalEmitted = true;
  });
  controller.setLayoutMode(dalinira::layout::BrowserLayoutMode::LeftPanel, false);
  assert(signalEmitted);
  assert(controller.currentMode() == dalinira::layout::BrowserLayoutMode::LeftPanel);
  assert(controller.isSidePanelVisible());
  assert(controller.isSidePanelOnLeft());
  assert(!controller.isSidePanelOnRight());
  // Verify sidePanel is first child in bodyLayout
  assert(bodyLayout->indexOf(sidePanel) == 0);
  assert(bodyLayout->indexOf(pageStack) == 1);

  // Switch to RightPanel: side panel is visible on right
  controller.setLayoutMode(dalinira::layout::BrowserLayoutMode::RightPanel, false);
  assert(controller.currentMode() == dalinira::layout::BrowserLayoutMode::RightPanel);
  assert(controller.isSidePanelVisible());
  assert(controller.isSidePanelOnRight());
  assert(!controller.isSidePanelOnLeft());
  // Verify pageStack is before sidePanel
  assert(bodyLayout->indexOf(pageStack) == 0);
  assert(bodyLayout->indexOf(sidePanel) == 1);

  // Switch back to Classic: side panel is hidden, pageStack remains intact
  controller.setLayoutMode(dalinira::layout::BrowserLayoutMode::Classic, false);
  assert(controller.currentMode() == dalinira::layout::BrowserLayoutMode::Classic);
  assert(!controller.isSidePanelVisible());
  assert(bodyLayout->indexOf(pageStack) >= 0);
  assert(controller.preservesPageWidget(livePage));

  controller.setLayoutMode(dalinira::layout::BrowserLayoutMode::FloatingSidebar, false);
  assert(controller.isFloatingPanelEnabled());
  assert(!controller.isHorizontalTabsVisible());
  assert(controller.preservesPageWidget(livePage));

  QVector<dalinira::layout::SidePanelTabInfo> tabs{
      {11, QStringLiteral("One"), QApplication::style()->standardIcon(QStyle::SP_FileIcon), false},
      {12, QStringLiteral("Two"), QApplication::style()->standardIcon(QStyle::SP_DirIcon), false}};
  sidePanel->setTabs(tabs, 1);
  assert(sidePanel->tabCount() == 2);
  assert(sidePanel->tabTitle(0) == QStringLiteral("One"));
  auto *sideList = sidePanel->findChild<QListWidget *>(QStringLiteral("sidePanelTabList"));
  assert(sideList != nullptr);
  // Native QDrag enters a nested X11 loop and is unsafe while WebKitGTK is
  // hosted through XEmbed. Vertical tabs use the manual, release-deferred
  // drag path instead.
  assert(!sideList->dragEnabled());
  assert(!sideList->acceptDrops());
  assert(sideList->dragDropMode() == QAbstractItemView::NoDragDrop);
  assert(sideList->item(0)->text().isEmpty());
  assert(sideList->itemWidget(sideList->item(0)) != nullptr);

  bool reordered = false;
  bool detached = false;
  QObject::connect(sidePanel, &dalinira::layout::BrowserSidePanel::tabMoved,
                   [&](int from, int to) { reordered = from == 0 && to == 1; });
  QObject::connect(sidePanel, &dalinira::layout::BrowserSidePanel::tabDetachRequested,
                   [&](int index, const QPoint &) { detached = index == 1; });
  sidePanel->requestTabMove(0, 1);
  sidePanel->requestTabDetach(1, QPoint(100, 100));
  assert(reordered);
  assert(detached);

  auto *destinationPanel = new dalinira::layout::BrowserSidePanel(bodyContainer);
  destinationPanel->setTabs(QStringList{QStringLiteral("Existing")}, 0);
  bool reattached = false;
  QObject::connect(destinationPanel, &dalinira::layout::BrowserSidePanel::tabTransferRequested,
                   [&](dalinira::layout::BrowserSidePanel *source, int sourceIndex, int targetIndex) {
    reattached = source == sidePanel && sourceIndex == 0 && targetIndex == 1;
  });
  destinationPanel->requestTabTransfer(sidePanel, 0, 1);
  assert(reattached);

  delete window;
  std::cout << "  -> testBrowserLayoutControllerRuntimeSwitching PASSED\n";
}

static void testSearchPrivacySelectionAndPersistence() {
  std::cout << "[TEST] Running testSearchPrivacySelectionAndPersistence...\n";
  MockSearchPrivacyPreferences preferences;
  SearchPrivacyStep step(&preferences);

  assert(step.stepId() == QStringLiteral("search_privacy"));
  assert(step.selectedSearchEngine() == QStringLiteral("DuckDuckGo"));
  assert(step.blockerEnabled());

  step.selectSearchEngine(QStringLiteral("Startpage"), false);
  step.setBlockerEnabled(false, false);
  assert(preferences.engine == QStringLiteral("Startpage"));
  assert(!preferences.blocker);
  assert(preferences.searchWrites == 1);
  assert(preferences.blockerWrites == 1);

  // Recreating the step reads the authoritative services, not onboarding keys.
  SearchPrivacyStep resumed(&preferences);
  assert(resumed.selectedSearchEngine() == QStringLiteral("Startpage"));
  assert(!resumed.blockerEnabled());

  // Invalid providers cannot enter the existing search service.
  resumed.selectSearchEngine(QStringLiteral("Unsupported"), false);
  assert(preferences.engine == QStringLiteral("Startpage"));
  std::cout << "  -> testSearchPrivacySelectionAndPersistence PASSED\n";
}

static void testSixStepNavigationAndResume() {
  std::cout << "[TEST] Running testSixStepNavigationAndResume...\n";
  QTemporaryDir tempDir;
  assert(tempDir.isValid());

  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, tempDir.path());

  OnboardingManager::instance().reset();

  MockDataReceiver receiver;
  MockSearchPrivacyPreferences searchPrivacy;
  {
    OnboardingView view(&receiver, &searchPrivacy);
    assert(view.stepCount() == 6);
    assert(view.currentStepIndex() == 0);
    assert(view.currentStep()->stepId() == QStringLiteral("welcome"));

    // Step 0: Welcome
    assert(!view.currentStep()->canGoBack());
    assert(view.currentStep()->canGoNext());

    // Step 0 -> Step 1: Import Data
    view.goToNextStep();
    assert(view.currentStepIndex() == 1);
    assert(view.currentStep()->stepId() == QStringLiteral("import"));
    assert(OnboardingManager::instance().currentStepIndex() == 1);

    // Step 1 -> Step 2: Choose Engine
    view.goToNextStep();
    assert(view.currentStepIndex() == 2);
    assert(view.currentStep()->stepId() == QStringLiteral("engine"));
    assert(OnboardingManager::instance().currentStepIndex() == 2);

    // Select WebKit on Step 2
    auto *engineStep = qobject_cast<EngineStep *>(view.currentStep());
    assert(engineStep != nullptr);
    engineStep->selectEngine(QStringLiteral("webkit"), false);
    assert(OnboardingManager::instance().selectedEngine() == QStringLiteral("webkit"));

    // Step 2 -> Step 3: Choose Layout & Panels
    view.goToNextStep();
    assert(view.currentStepIndex() == 3);
    assert(view.currentStep()->stepId() == QStringLiteral("layout"));
    assert(OnboardingManager::instance().currentStepIndex() == 3);

    // Step 3 allows Back and Next
    assert(view.currentStep()->canGoBack());
    assert(view.currentStep()->canGoNext());

    // Back to Step 2: Engine
    view.goToPreviousStep();
    assert(view.currentStepIndex() == 2);
    assert(view.currentStep()->stepId() == QStringLiteral("engine"));
    assert(OnboardingManager::instance().selectedEngine() == QStringLiteral("webkit"));

    // Forward back to Step 3: Layout
    view.goToNextStep();
    assert(view.currentStepIndex() == 3);
    assert(view.currentStep()->stepId() == QStringLiteral("layout"));

    // User chooses Left Panel on Step 3
    auto *layoutStep = qobject_cast<LayoutStep *>(view.currentStep());
    assert(layoutStep != nullptr);
    layoutStep->setSelectedLayoutMode(dalinira::layout::BrowserLayoutMode::LeftPanel);
    assert(OnboardingManager::instance().selectedLayout() == QStringLiteral("left_panel"));
    layoutStep->setSelectedTheme(dalinira::layout::AppearanceTheme::Dark);
    assert(OnboardingManager::instance().selectedTheme() == QStringLiteral("dark"));

    // Step 3 -> Step 4: Search & Privacy. Selections apply immediately to the
    // existing profile/blocker preferences and the step index is resumable.
    view.goToNextStep();
    assert(view.currentStepIndex() == 4);
    assert(view.currentStep()->stepId() == QStringLiteral("search_privacy"));
    auto *searchStep = qobject_cast<SearchPrivacyStep *>(view.currentStep());
    assert(searchStep != nullptr);
    searchStep->selectSearchEngine(QStringLiteral("Mojeek"), false);
    searchStep->setBlockerEnabled(false, false);
    assert(searchPrivacy.engine == QStringLiteral("Mojeek"));
    assert(!searchPrivacy.blocker);
  }

  // Simulate browser restart while user was on Search & Privacy.
  {
    OnboardingView resumedView(&receiver, &searchPrivacy);
    assert(resumedView.currentStepIndex() == 4);
    assert(resumedView.currentStep()->stepId() == QStringLiteral("search_privacy"));
    // Engine and Layout choices are preserved across restart
    assert(OnboardingManager::instance().selectedEngine() == QStringLiteral("webkit"));
    assert(OnboardingManager::instance().selectedLayout() == QStringLiteral("left_panel"));
    assert(OnboardingManager::instance().selectedTheme() == QStringLiteral("dark"));
    auto *resumedSearchStep = qobject_cast<SearchPrivacyStep *>(resumedView.currentStep());
    assert(resumedSearchStep != nullptr);
    assert(resumedSearchStep->selectedSearchEngine() == QStringLiteral("Mojeek"));
    assert(!resumedSearchStep->blockerEnabled());

    // Advance to the final summary step. Completion only happens after one of
    // its explicit default-browser choices.
    bool finished = false;
    QObject::connect(&resumedView, &OnboardingView::finished, [&] {
      finished = true;
    });

    resumedView.goToNextStep();
    assert(!finished);
    assert(resumedView.currentStepIndex() == 5);
    auto *readyStep = qobject_cast<ReadyStep *>(resumedView.currentStep());
    assert(readyStep != nullptr);
    assert(readyStep->engineSummary() == QStringLiteral("WebKit"));
    assert(readyStep->layoutThemeSummary().contains(
        dalinira::i18n::I18n::text(QStringLiteral("settings.appearance.theme_dark"))));
    assert(readyStep->searchSummary() == QStringLiteral("Mojeek"));
    readyStep->chooseNotNow();
    assert(finished);
    assert(OnboardingManager::instance().isOnboardingCompleted());
    assert(!OnboardingManager::instance().shouldShowOnboarding());
    assert(OnboardingManager::instance().selectedEngine() == QStringLiteral("webkit"));
    assert(OnboardingManager::instance().selectedLayout() == QStringLiteral("left_panel"));
    assert(OnboardingManager::instance().selectedTheme() == QStringLiteral("dark"));
    assert(searchPrivacy.engine == QStringLiteral("Mojeek"));
    assert(!searchPrivacy.blocker);
  }

  std::cout << "  -> testSixStepNavigationAndResume PASSED\n";
}

static void testEngineParityLayoutSupport() {
  std::cout << "[TEST] Running testEngineParityLayoutSupport...\n";
  QTemporaryDir tempDir;
  assert(tempDir.isValid());

  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, tempDir.path());

  // Test layout under WebKit engine
  OnboardingManager::instance().setSelectedEngine(QStringLiteral("webkit"));
  dalinira::layout::setPersistedLayoutMode(dalinira::layout::BrowserLayoutMode::LeftPanel);
  dalinira::layout::setPersistedAppearanceTheme(dalinira::layout::AppearanceTheme::Dark);
  assert(dalinira::layout::currentLayoutMode() == dalinira::layout::BrowserLayoutMode::LeftPanel);
  assert(dalinira::layout::currentAppearanceTheme() == dalinira::layout::AppearanceTheme::Dark);

  // Test layout under Chromium engine
  OnboardingManager::instance().setSelectedEngine(QStringLiteral("chromium"));
  dalinira::layout::setPersistedLayoutMode(dalinira::layout::BrowserLayoutMode::RightPanel);
  dalinira::layout::setPersistedAppearanceTheme(dalinira::layout::AppearanceTheme::Light);
  assert(dalinira::layout::currentLayoutMode() == dalinira::layout::BrowserLayoutMode::RightPanel);
  assert(dalinira::layout::currentAppearanceTheme() == dalinira::layout::AppearanceTheme::Light);

  // Both engines share identical layout behavior and settings
  dalinira::layout::setPersistedLayoutMode(dalinira::layout::BrowserLayoutMode::Classic);
  dalinira::layout::setPersistedAppearanceTheme(dalinira::layout::AppearanceTheme::System);
  assert(dalinira::layout::currentLayoutMode() == dalinira::layout::BrowserLayoutMode::Classic);

  MockSearchPrivacyPreferences sharedPreferences;
  OnboardingManager::instance().setSelectedEngine(QStringLiteral("webkit"));
  SearchPrivacyStep webKitStep(&sharedPreferences);
  webKitStep.selectSearchEngine(QStringLiteral("Google"), false);
  webKitStep.setBlockerEnabled(false, false);
  OnboardingManager::instance().setSelectedEngine(QStringLiteral("chromium"));
  SearchPrivacyStep chromiumStep(&sharedPreferences);
  assert(chromiumStep.selectedSearchEngine() == QStringLiteral("Google"));
  assert(!chromiumStep.blockerEnabled());

  std::cout << "  -> testEngineParityLayoutSupport PASSED\n";
}

static void testReadySummaryAndDefaultBrowserPaths() {
  std::cout << "[TEST] Running testReadySummaryAndDefaultBrowserPaths...\n";
  QTemporaryDir tempDir;
  assert(tempDir.isValid());
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, tempDir.path());
  OnboardingManager::instance().reset();
  OnboardingManager::instance().setSelectedEngine(QStringLiteral("webkit"));
  OnboardingManager::instance().setSelectedLayout(QStringLiteral("left_panel"));
  OnboardingManager::instance().setSelectedTheme(QStringLiteral("dark"));

  MockSearchPrivacyPreferences preferences;
  preferences.engine = QStringLiteral("Startpage");
  preferences.blocker = false;

  for (const auto result : {dalinira::core::DefaultBrowserResult::Success,
                            dalinira::core::DefaultBrowserResult::AlreadyDefault,
                            dalinira::core::DefaultBrowserResult::Failed,
                            dalinira::core::DefaultBrowserResult::Unsupported}) {
    MockDefaultBrowserIntegration integration;
    integration.result = result;
    ReadyStep step(&preferences, &integration);
    step.onStepEntering();
    assert(step.engineSummary() == QStringLiteral("WebKit"));
    assert(step.layoutThemeSummary().contains(
        dalinira::i18n::I18n::text(QStringLiteral("settings.appearance.theme_dark"))));
    assert(step.searchSummary() == QStringLiteral("Startpage"));
    assert(step.privacySummary() == dalinira::i18n::I18n::text(
        QStringLiteral("onboarding.search_privacy.blocker_off")));

    const auto icons = step.findChildren<QLabel *>(QStringLiteral("readySummaryIcon"));
    assert(icons.size() == 4);
    for (auto *iconLabel : icons) {
      assert(!iconLabel->pixmap().isNull());
      assert(iconLabel->pixmap().width() > 0);
    }

    bool finished = false;
    QObject::connect(&step, &OnboardingStep::stepCompleted, [&] { finished = true; });
    step.chooseMakeDefault();
    assert(integration.requests == 1);
    assert(finished);
  }

  ReadyStep notNow(&preferences, nullptr);
  bool skippedDefault = false;
  QObject::connect(&notNow, &OnboardingStep::stepCompleted,
                   [&] { skippedDefault = true; });
  notNow.chooseNotNow();
  assert(skippedDefault);
  assert(QSettings().value(QString::fromLatin1(
      dalinira::core::kDefaultBrowserLastPromptUtcKey)).toDateTime().isValid());
  std::cout << "  -> testReadySummaryAndDefaultBrowserPaths PASSED\n";
}

static void testReopenPreservesPreferences() {
  std::cout << "[TEST] Running testReopenPreservesPreferences...\n";
  QTemporaryDir tempDir;
  assert(tempDir.isValid());
  QSettings settings(tempDir.path() + QStringLiteral("/test_settings.ini"),
                     QSettings::IniFormat);
  OnboardingManager manager(&settings);
  manager.setSelectedEngine(QStringLiteral("webkit"));
  manager.setSelectedLayout(QStringLiteral("floating"));
  manager.setSelectedTheme(QStringLiteral("dark"));
  manager.completeOnboarding();
  assert(!manager.shouldShowOnboarding());

  bool reopened = false;
  QObject::connect(&manager, &OnboardingManager::onboardingReopened,
                   [&] { reopened = true; });
  manager.reopenOnboarding();
  assert(reopened);
  assert(manager.shouldShowOnboarding());
  assert(manager.currentStepIndex() == 0);
  assert(manager.currentStepId() == QStringLiteral("welcome"));
  assert(manager.selectedEngine() == QStringLiteral("webkit"));
  assert(manager.selectedLayout() == QStringLiteral("floating"));
  assert(manager.selectedTheme() == QStringLiteral("dark"));
  std::cout << "  -> testReopenPreservesPreferences PASSED\n";
}

static void testStandaloneOnboardingWindowAndAuroraBackground() {
  std::cout << "[TEST] Running testStandaloneOnboardingWindowAndAuroraBackground...\n";
  QTemporaryDir tempDir;
  assert(tempDir.isValid());
  QSettings settings(tempDir.path() + QStringLiteral("/test_settings.ini"), QSettings::IniFormat);
  OnboardingManager manager(&settings);

  // 1. Test AuroraWaveBackground widget directly
  AuroraWaveBackground aurora;
  aurora.resize(800, 600);
  aurora.show();
  QApplication::processEvents();

  // Trigger paint event to verify rendering path without crash
  QPixmap pixmap(800, 600);
  aurora.render(&pixmap);
  assert(!pixmap.isNull());

  // 2. Test OnboardingWindow standalone setup window
  OnboardingWindow window;
  assert(window.windowFlags().testFlag(Qt::FramelessWindowHint));
  assert(!window.windowState().testFlag(Qt::WindowMaximized));
  window.resize(1000, 680);
  assert(window.view() != nullptr);
  window.show();
  QApplication::processEvents();

  auto *closeButton = window.findChild<QToolButton *>(QStringLiteral("onboarding-close-button"));
  assert(closeButton != nullptr);
  assert(closeButton->size() == QSize(44, 44));
  assert(closeButton->x() == window.width() - closeButton->width() - 10);
  assert(closeButton->y() == 10);

  auto *welcomeSubtitle = window.findChild<QLabel *>(QStringLiteral("welcome-subtitle"));
  assert(welcomeSubtitle != nullptr);
  assert(welcomeSubtitle->height() >= 40);
  assert(welcomeSubtitle->wordWrap());

  // Navigate to step 1 (ImportStep) to inspect widgets
  window.view()->goToStep(1, TransitionDirection::Immediate);
  QApplication::processEvents();
  window.grab().save(QStringLiteral("/tmp/dalinira-import-step-preview.png"));

  auto *importCard = window.findChild<QFrame *>(QStringLiteral("choice-card-import"));
  auto *freshCard = window.findChild<QFrame *>(QStringLiteral("choice-card-fresh"));
  assert(importCard != nullptr);
  assert(freshCard != nullptr);

  auto *importTitle = window.findChild<QLabel *>(QStringLiteral("import-choice-title"));
  auto *importDesc = window.findChild<QLabel *>(QStringLiteral("import-choice-desc"));
  assert(importTitle != nullptr && !importTitle->text().isEmpty());
  assert(importDesc != nullptr && !importDesc->text().isEmpty());
  assert(importTitle->height() >= 16);
  assert(importDesc->height() >= 14);

  auto *freshTitle = window.findChild<QLabel *>(QStringLiteral("fresh-choice-title"));
  auto *freshDesc = window.findChild<QLabel *>(QStringLiteral("fresh-choice-desc"));
  assert(freshTitle != nullptr && !freshTitle->text().isEmpty());
  assert(freshDesc != nullptr && !freshDesc->text().isEmpty());
  assert(freshTitle->height() >= 16);
  assert(freshDesc->height() >= 14);

  auto *importRadio = window.findChild<QRadioButton *>(QStringLiteral("import-radio"));
  auto *freshRadio = window.findChild<QRadioButton *>(QStringLiteral("fresh-radio"));
  assert(importRadio != nullptr && importRadio->width() == 24 && importRadio->height() == 24);
  assert(freshRadio != nullptr && freshRadio->width() == 24 && freshRadio->height() == 24);

  // Click import to expand options container and take screenshot
  importRadio->click();
  QApplication::sendPostedEvents();
  QApplication::processEvents();
  window.layout()->activate();
  window.grab().save(QStringLiteral("/tmp/dalinira-import-step-preview.png"));

  // Also capture with combobox popup opened
  auto *combo = window.findChild<QComboBox *>(QStringLiteral("import-browser-selector"));
  assert(combo != nullptr);
  for (const QString &browserId : {QStringLiteral("brave"), QStringLiteral("chrome"), QStringLiteral("zen")}) {
    const int browserIndex = combo->findData(browserId);
    if (browserIndex >= 0) {
      assert(!combo->itemIcon(browserIndex).isNull());
    }
  }
  auto *bookmarksCheck = window.findChild<QCheckBox *>(QStringLiteral("import-check-bookmarks"));
  assert(bookmarksCheck != nullptr);
  assert(window.findChild<QCheckBox *>(QStringLiteral("import-check-passwords")) == nullptr);
  const QRect comboWindowRect(combo->mapTo(&window, QPoint(0, 0)), combo->size());
  const QRect bookmarksWindowRect(bookmarksCheck->mapTo(&window, QPoint(0, 0)), bookmarksCheck->size());
  assert(bookmarksCheck->height() >= bookmarksCheck->minimumSizeHint().height());
  assert(bookmarksWindowRect.top() > comboWindowRect.bottom());
  combo->showPopup();
  QApplication::sendPostedEvents();
  QApplication::processEvents();
  window.grab().save(QStringLiteral("/tmp/dalinira-import-step-popup-preview.png"));
  combo->hidePopup();
  QApplication::processEvents();

  // Navigate to step 2 (EngineStep) and verify modern engine radio buttons
  window.view()->goToStep(2);
  QApplication::processEvents();

  auto *chromiumRadio = window.findChild<QRadioButton *>(QStringLiteral("chromium-radio"));
  auto *webkitRadio = window.findChild<QRadioButton *>(QStringLiteral("webkit-radio"));
  assert(chromiumRadio != nullptr && chromiumRadio->width() == 24 && chromiumRadio->height() == 24);
  assert(webkitRadio != nullptr && webkitRadio->width() == 24 && webkitRadio->height() == 24);

  bool finishedFired = false;
  QObject::connect(&window, &OnboardingWindow::onboardingFinished, [&] {
    finishedFired = true;
  });

  // Verify completing via view triggers window signal
  window.view()->complete();
  assert(finishedFired);

  std::cout << "  -> testStandaloneOnboardingWindowAndAuroraBackground PASSED\n";
}

int main(int argc, char *argv[]) {
  dalinira::application_identity::apply();
  QApplication app(argc, argv);
  dalinira::i18n::LanguageManager::instance().initialize();

  testFirstLaunchDetection();
  testCompletionPersistence();
  testStepStateAndSafeResume();
  testSkipOnboarding();
  testReset();
  testReducedMotionAndAccessibility();
  testOnboardingViewAndWelcomeStep();
  testImportChoicePersistence();
  testBrowserDataImporterParsers();
  testSqliteBookmarkAndHistoryImport();
  testDetectedBrowsersAndRealSystemDetection();
  testDataReceiverImportExecution();
  testEngineSelectionAndPersistence();
  testEngineStepSelectionAndAnimationPolicy();
  testRadiosSelectWithoutClick();
  testEngineChoiceDefaultsToRunningEngine();
  testFirstRunStateIsPerProfile();
  testWelcomeCardsFitTheirText();
  testLayoutModePersistence();
  testLayoutStepSelectionAndPreview();
  testBrowserLayoutControllerRuntimeSwitching();
  testSearchPrivacySelectionAndPersistence();
  testSixStepNavigationAndResume();
  testEngineParityLayoutSupport();
  testReadySummaryAndDefaultBrowserPaths();
  testReopenPreservesPreferences();
  testStandaloneOnboardingWindowAndAuroraBackground();

  std::cout << "\nALL ONBOARDING COMPLETION, DEFAULT, SEARCH, LAYOUT & RESUME TESTS PASSED!\n";
  return 0;
}
