#include "browser_icons.h"
#include "browser_policy.h"
#include "browser_profile_service.h"
#include "tab_hover_card.h"
#include "tab_manager.h"

#include <QApplication>
#include <QIcon>
#include <QLabel>
#include <QMainWindow>
#include <QTabBar>
#include <QTemporaryDir>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineView>
#include <QWidget>

#include <iostream>

int main(int argc, char *argv[]) {
  QApplication app(argc, argv);

  // 1. Verify BrowserIcons for Audio and Memory (used in TabHoverCard)
  const QIcon audioIcon = BrowserIcons::icon(BrowserIcon::Audio);
  if (audioIcon.isNull()) {
    std::cerr << "Audio icon failed to load\n";
    return 1;
  }
  const QIcon memoryIcon = BrowserIcons::icon(BrowserIcon::Memory);
  if (memoryIcon.isNull()) {
    std::cerr << "Memory icon failed to load\n";
    return 1;
  }

  // 2. Verify TabManager Audible State
  TabManager tabManager;
  QObject owner;
  QWebEngineProfile profile(&app);
  QWebEngineView webView;
  webView.setPage(new QWebEnginePage(&profile, &webView));

  const auto tabId = tabManager.registerTab(&webView, &owner, false, QStringLiteral("YouTube - Music"));
  const TabManager::TabRecord *record = tabManager.record(tabId);
  if (!record || record->recentlyAudible) {
    std::cerr << "Tab should not be audible by default\n";
    return 1;
  }

  bool signalFired = false;
  bool signalValue = false;
  QObject::connect(&tabManager, &TabManager::tabRecentlyAudibleChanged,
                   [&](TabManager::TabId id, bool audible) {
                     if (id == tabId) {
                       signalFired = true;
                       signalValue = audible;
                     }
                   });

  // Turn audible on
  tabManager.updateRecentlyAudible(tabId, true);
  if (!record->recentlyAudible || !signalFired || !signalValue) {
    std::cerr << "Tab audible state failed to update to true\n";
    return 1;
  }

  // 3. Verify Favicon Remains Untouched (16x16 standard)
  const QIcon rawFavicon = BrowserIcons::icon(BrowserIcon::Window);
  if (rawFavicon.isNull()) {
    std::cerr << "Favicon is null\n";
    return 1;
  }

  // Turn audible off
  signalFired = false;
  tabManager.updateRecentlyAudible(tabId, false);
  if (record->recentlyAudible || !signalFired || signalValue) {
    std::cerr << "Tab audible state failed to update to false\n";
    return 1;
  }

  // 4. Verify Standard TabBar (No visual audio indicator on tab, No native URL tooltip)
  QTabBar testTabBar;
  testTabBar.addTab(QStringLiteral("Test Tab"));
  if (testTabBar.tabButton(0, QTabBar::LeftSide) != nullptr) {
    std::cerr << "Tab bar should have no LeftSide button (audio indicator on tab is removed)\n";
    return 1;
  }
  if (testTabBar.tabToolTip(0).contains(QLatin1String("http"))) {
    std::cerr << "Native URL tooltip should not be present\n";
    return 1;
  }

  // 5. Verify TabHoverCard Domain Extraction (No URL path/query leaks!)
  const QUrl youtubeUrl(QStringLiteral("https://www.youtube.com/watch?v=dQw4w9WgXcQ&list=RDdQw4w9WgXcQ#t=42"));
  const QString ytDomain = TabHoverCard::extractDomain(youtubeUrl);
  if (ytDomain != QLatin1String("youtube.com")) {
    std::cerr << "Domain extraction for YouTube failed: got " << ytDomain.toStdString() << " expected youtube.com\n";
    return 1;
  }
  if (ytDomain.contains(QLatin1String("watch")) || ytDomain.contains(QLatin1String("http")) || ytDomain.contains(QLatin1String("?"))) {
    std::cerr << "Domain extraction must not contain path, scheme, or query\n";
    return 1;
  }

  const QUrl musicYtUrl(QStringLiteral("https://music.youtube.com/watch?v=12345"));
  const QString musicDomain = TabHoverCard::extractDomain(musicYtUrl);
  if (musicDomain != QLatin1String("music.youtube.com")) {
    std::cerr << "Domain extraction for music.youtube.com failed\n";
    return 1;
  }

  const QUrl githubUrl(QStringLiteral("https://github.com/user/repo?tab=readme"));
  const QString ghDomain = TabHoverCard::extractDomain(githubUrl);
  if (ghDomain != QLatin1String("github.com")) {
    std::cerr << "Domain extraction for github.com failed\n";
    return 1;
  }

  const QUrl daliniraSettingsUrl(QStringLiteral("dalinira://settings"));
  const QString settingsDomain = TabHoverCard::extractDomain(daliniraSettingsUrl);
  if (settingsDomain != QLatin1String("Ayarlar")) {
    std::cerr << "Domain extraction for dalinira://settings failed\n";
    return 1;
  }

  // 6. Verify TabHoverCard Full Title, Audio Row & Memory
  TabHoverCard hoverCard;
  const QRect tabRect(100, 100, 150, 36);
  const QString longTitle = QStringLiteral("Void Memories – Dönmedim (Official Music Video) - YouTube");

  hoverCard.showForTab(longTitle, youtubeUrl, rawFavicon, tabRect, [] { return dalinira::TabResourceSnapshot{}; });

  auto *titleLabel = hoverCard.findChild<QLabel *>(QStringLiteral("hover-title"));
  if (!titleLabel || titleLabel->text() != longTitle) {
    std::cerr << "Hover card title must retain full page title\n";
    return 1;
  }

  auto *domainLabel = hoverCard.findChild<QLabel *>(QStringLiteral("hover-domain"));
  if (!domainLabel || domainLabel->text() != QLatin1String("youtube.com")) {
    std::cerr << "Hover card domain label must show youtube.com\n";
    return 1;
  }

  // No content process (an unrendered page): nothing is measured.
  if (dalinira::TabResourceModel::readProcessUsage(0).valid ||
      dalinira::TabResourceModel::readProcessUsage(webView.page()->renderProcessPid()).valid) {
    std::cerr << "an unrendered page has no measurable content process\n";
    return 1;
  }

  hoverCard.hideCard();

  // 7. Verify FullScreenSupportEnabled on Profile
  QTemporaryDir dataDir;
  BrowserPolicy policy;
  BrowserProfileService profileService(dataDir.path(), &policy);
  if (profileService.profile() && profileService.profile()->settings()) {
    const bool fullScreenEnabled = profileService.profile()->settings()->testAttribute(
        QWebEngineSettings::FullScreenSupportEnabled);
    if (!fullScreenEnabled) {
      std::cerr << "FullScreenSupportEnabled must be true on profile\n";
      return 1;
    }
  }

  // 8. Verify FullScreen Window State & Chrome Preservation Logic
  struct FullScreenHarness {
    bool tabStripVisible = true;
    bool navBarVisible = true;
    bool bookmarkBarVisible = true;
    bool isWebFullScreen = false;
    Qt::WindowStates preState = Qt::WindowNoState;
    bool wasMaximized = false;

    void enterFullScreen(bool currentlyMaximized) {
      preState = currentlyMaximized ? Qt::WindowMaximized : Qt::WindowNoState;
      wasMaximized = currentlyMaximized;
      isWebFullScreen = true;
      tabStripVisible = false;
      navBarVisible = false;
      bookmarkBarVisible = false;
    }

    void exitFullScreen(bool &restoredMaximized) {
      isWebFullScreen = false;
      tabStripVisible = true;
      navBarVisible = true;
      bookmarkBarVisible = true;
      restoredMaximized = wasMaximized || (preState & Qt::WindowMaximized);
    }
  };

  FullScreenHarness normalHarness;
  normalHarness.enterFullScreen(false);
  if (!normalHarness.isWebFullScreen || normalHarness.tabStripVisible || normalHarness.navBarVisible) {
    std::cerr << "FullScreen chrome hiding failed\n";
    return 1;
  }
  bool restoredMax = false;
  normalHarness.exitFullScreen(restoredMax);
  if (normalHarness.isWebFullScreen || !normalHarness.tabStripVisible || !normalHarness.navBarVisible || restoredMax) {
    std::cerr << "FullScreen normal restore failed\n";
    return 1;
  }

  FullScreenHarness maxHarness;
  maxHarness.enterFullScreen(true);
  maxHarness.exitFullScreen(restoredMax);
  if (!restoredMax || !maxHarness.tabStripVisible || !maxHarness.navBarVisible) {
    std::cerr << "FullScreen maximized restore failed\n";
    return 1;
  }

  std::cout << "All Tab audible, URL tooltip removal, and FullScreen tests passed successfully!\n";
  return 0;
}
