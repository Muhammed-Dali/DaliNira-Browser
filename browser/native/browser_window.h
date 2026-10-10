#ifndef BROWSER_WINDOW_H_
#define BROWSER_WINDOW_H_

#include <functional>
#include <memory>
#include <QFrame>
#include <QCache>
#include <QHash>
#include <QIcon>
#include <QLabel>
#include <QList>
#include <QMainWindow>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QWebEngineProfile>
#include <QWebEngineView>
#include <QWebEngineFullScreenRequest>

class QShortcut;

namespace dalinira::core {
class INavigationCandidateProvider;
}

namespace dalinira::desktop_tabs {
class FindBarWidget;
}

namespace dalinira::engine {
class EngineView;
}

namespace dalinira::layout {
class BrowserLayoutController;
class BrowserSidePanel;
enum class BrowserLayoutMode;
}

#include "audio/audio_effects_page.h"
#include "audio/web_audio_effects_controller.h"
#include "blocker/dalinira_blocker_page.h"
#include "blocker/dalinira_blocker_shield_button.h"
#include "core/browser_policy.h"
#include "core/browser_profile_service.h"
#include "desktop_tabs/tab_manager.h"
#include "desktop_tabs/tab_group_model.h"
#include "downloads/media_download_page.h"
#include "downloads/media_download_service.h"
#include "downloads/local_media_routing.h"
#include "downloads/media_platform_registry.h"
#include "eq/eq_preset_page.h"
#include "eq/eq_preset_repository.h"
#include "passwords/password_manager_page.h"
#include "pulse/pulse_toolbar_button.h"
#include "pulse/song_finder_page.h"
#include "pulse/song_finder_settings.h"
#include "pulse/song_recognition_service.h"
#include "session/session_store.h"
#include "blocker/dalinira_blocker_element_picker.h"
#include "settings/settings_page.h"
#include "translate/page_translator.h"
#include "translate/translate_bubble_popup.h"
#include "site_permission_prompt_bubble.h"
#include "site_controls_bubble.h"

struct TabSessionPermissionKey {
  uint64_t tabId = 0;
  QString canonicalOrigin;
  QString permissionKey;

  bool operator==(const TabSessionPermissionKey &other) const {
    return tabId == other.tabId && canonicalOrigin == other.canonicalOrigin && permissionKey == other.permissionKey;
  }
};

inline size_t qHash(const TabSessionPermissionKey &key, size_t seed = 0) {
  return qHash(key.tabId, seed) ^ qHash(key.canonicalOrigin, seed) ^ qHash(key.permissionKey, seed);
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
struct PendingPermissionRequest {
  QWebEnginePermission permission;
  QPointer<QWebEngineView> view;
  QPointer<QWebEnginePage> page;
  uint64_t tabId = 0;
  QUrl requestedOrigin;
  QString canonicalOrigin;
  QWebEnginePermission::PermissionType type;
  QDateTime requestedAt;
};
#endif

class BookmarkBarReorder;
class DaliNiraBlockerService;
class CredentialVaultManager;
class CredentialAutofillController;
class WebKitCredentialController;
class SongFinderSettingsPage;
class TranslateService;
class QLineEdit;
class QCompleter;
class QStandardItemModel;
class QStackedWidget;
class QToolButton;
class QToolBar;
class QProgressBar;
class TabHoverCard;
class DownloadUiModel;
class DownloadPopup;
class DownloadToolbarButton;

namespace dalinira::desktop_tabs {
class TabStripWidget;
class TabSearchPopup;
class TabGroupPopup;
class TabGroupLauncherPopup;
} // namespace dalinira::desktop_tabs

struct BrowserServices {
  std::shared_ptr<BrowserProfileService> privateProfileOwner;
  QWebEngineProfile *profile = nullptr;
  BrowserProfileService *profileService = nullptr;
  TabManager *tabManager = nullptr;
  const BrowserPolicy *policy = nullptr;
  SessionStore *sessionStore = nullptr;
  WebAudioEffectsController *audioEffects = nullptr;
  EqPresetRepository *eqPresetRepo = nullptr;
  SongRecognitionService *songRecognition = nullptr;
  SongFinderSettings *songFinderSettings = nullptr;
  MediaDownloadService *mediaDownload = nullptr;
};

// Authoritative tab state held within a BrowserWindow.
// INVARIANT (Dual-Identity Tab Model):
// - `id` (uint64_t): Window-scoped monotonic identifier for fast tab strip
//   lookup, tab drag/transfer routing, and TabSessionPermission grants.
// - `uuid` (QUuid): Global application-scoped identifier matching
//   TabManager::TabRecord::id and TabPerformanceManager tracking across windows.
struct BrowserTabInfo {
  uint64_t id = 0;
  QUuid uuid;
  QString title;
  QUrl url;
  QIcon icon;
  QString faviconHost;  // Host whose favicon state `icon` holds: its real favicon, or the engine reported none.
  QPointer<QWidget> content;
  QPointer<QWebEngineView> view;
  QPointer<dalinira::engine::EngineView> engineView;
  bool isInternal = false;
  QString internalId;
  std::optional<QUuid> groupId;
  QUrl expectedTypedUrl;
  bool activeCamera = false;
  bool activeMicrophone = false;
  bool isPinned = false;
  bool translationOffered = false;
  // The current page's own favicon reached the tab (Chromium: reset per load).
  bool pageIconReported = false;
  QPointer<PageTranslator> translator;
  bool isLoading = false;
  // Set when the UI language changed while this New Tab was in the
  // background (and possibly frozen by tab sleep); reloaded on activation.
  bool pendingLanguageReload = false;
};

class BrowserWindow : public QMainWindow {
  Q_OBJECT
public:
  explicit BrowserWindow(const BrowserServices &services = {},
                         bool isCaptureShell = false,
                         QWidget *parent = nullptr);
  ~BrowserWindow() override;

  const BrowserServices &services() const { return services_; }

  // Tab management
  int addNewTab(const QUrl &url = QUrl(QStringLiteral("dalinira://newtab/")),
                int insertIndex = -1, bool initiallyPinned = false,
                const QString &initialTitle = QString{});
  int addInternalTab(QWidget *page, const QString &title, const QIcon &icon,
                     const QString &internalId, int insertIndex = -1);
  void closeTab(int index);
  void restoreLastClosedTab();
  void switchTab(int index);
  void moveTab(int fromIndex, int toIndex);

  // Tab drag transfer (Chromium parity)
  bool transferTabTo(uint64_t tabId, BrowserWindow *destination,
                     int targetIndex);
  void adoptTab(BrowserTabInfo info, int targetIndex);
  int findIndexByTabId(uint64_t tabId) const;
  int indexOfEngineView(const dalinira::engine::EngineView *view) const;
  uint64_t findTabIdByIndex(int index) const;

  // Tab Context Menu & Actions
  void onTabContextMenuRequested(int index, const QPoint &globalPos);
  void toggleTabPin(int index);

  // Tab sleeping (shared memory saver for both engines, TabPerformanceManager).
  bool sleepTabAt(int index);
  int sleepOtherTabs(int index);
  void setNeverSleepSite(int index, bool neverSleep);
  bool isTabSleeping(int index) const;
  // Wakes a sleeping tab in place, through the same path as activating it.
  bool wakeTabAt(int index);
  void showSleepingTabsManager();
  // The tab context menu's "Uyku / Bellek" submenu; its actions follow the
  // tab's real lifecycle state (both engines).
  void populateTabSleepMenu(QMenu *sleepMenu, int index);
  // The shared resource model of a tab (lifecycle, memory, CPU), as the hover
  // card shows it.
  dalinira::TabResourceSnapshot tabResourceSnapshot(int index) const;
  // A page dialog (alert/confirm/prompt) is open in a Chromium page.
  void setPageDialogActive(QWebEnginePage *page, bool active);
  void closeOtherTabs(int index);
  void closeTabsToRight(int index);
  BrowserWindow *openNewWindow(const QUrl &url = QUrl());
  // A further window the user opens (New Window, a link or popup in a new
  // window, a moved or detached tab): deleted when it is closed, so its tabs'
  // pages and WebKit web processes end with it, as for the first window.
  static BrowserWindow *createWindow(const BrowserServices &services);
  BrowserWindow *openIncognitoWindow(const QUrl &url = QUrl());
  bool isIncognito() const {
    return (services_.profile && services_.profile->isOffTheRecord()) || services_.privateProfileOwner != nullptr;
  }
  // Whether local search/history data belongs to this window's profile. The
  // WebKit backend runs without any QWebEngineProfile, so services_.profile is
  // null there and must not be dereferenced.
  bool usesPersistentProfile() const {
    if (!services_.profileService || isIncognito()) return false;
    return !services_.profile || services_.profile == services_.profileService->profile();
  }
  void openDevToolsForPage(QWebEnginePage *page);
  void showBookmarkContextMenu(const QUrl &url, const QString &title, const QPoint &globalPos);
  void showLinkContextMenu(const QUrl &linkUrl, const QString &linkText, const QPoint &globalPos);
  void showSelectionContextMenu(const QString &selectedText, const QPoint &globalPos);
  void showPageContextMenu(const QPoint &globalPos);

  // Whether a web page may run its own JavaScript: the JavaScript setting and
  // the site's "block scripts" policy. Shared by both engines.
  bool pageScriptsAllowed(const QUrl &url) const;

  // Shared browser actions behind the configurable keyboard shortcuts
  // (KeyboardShortcutService ids). Both engines run the same handler; the
  // window holds exactly one QShortcut per action and rebinds it in place.
  bool triggerBrowserAction(const QString &actionId);
  QShortcut *shortcutForAction(const QString &actionId) const { return actionShortcuts_.value(actionId); }
  // The key hint a menu item shows for an action ("\tCtrl+H"), empty without one.
  static QString shortcutMenuHint(const QString &actionId);
  // Appends that hint to a menu item's text.
  static void addShortcutHint(QAction *action, const QString &actionId);
  void saveCurrentPage();
  void viewCurrentPageSource();

  void showFindBar();
  void hideFindBar();
  void printCurrentPage();
  void printCurrentPageToPdf();
  void captureVisiblePage();
  void showReaderMode();
  void handleFullScreenRequest(QWebEngineView *view, const QWebEngineFullScreenRequest &request);
  // Shared page (element) fullscreen for both engines: hides the browser
  // chrome and fullscreens the window, or restores both.
  // `owner` is the page that asked; closing its tab leaves fullscreen.
  void setPageFullScreen(bool on, QWidget *owner = nullptr);

  dalinira::desktop_tabs::TabStripWidget *tabStrip() const { return tabStrip_; }
  int tabCount() const { return tabs_.size(); }
  const BrowserTabInfo &tabInfo(int index) const { return tabs_[index]; }
  const QVector<BrowserTabInfo> &allTabs() const { return tabs_; }
  QWebEngineView *currentView() const;
  dalinira::engine::EngineView *currentEngineView() const;
  QUrl currentTabUrl() const;
  QString currentTabTitle() const;
  bool currentTabAudible() const;
  QUrl currentActiveWebUrl() const;

  // The engine workspace this window's tabs belong to ("chromium"/"webkit").
  // One engine runs per process; switching engine relaunches DaliNira, which
  // then restores the other engine's own workspace (SessionStore).
  dalinira::engine::EngineType currentWorkspaceEngine() const { return currentWorkspaceEngine_; }
  QString currentEngineScope() const;

  QString currentSearchEngine() const;
  void navigateFromUserInput(const QString &rawInput, const QString &searchEngine = QString{});
  // Shared by both engines. Loads an already-resolved URL in the current tab
  // (a new tab only when the current tab is an internal Qt page).
  void navigateCurrentTab(const QUrl &url, bool typed = false);
  // Bookmark/quick-access activation: current tab unless `newTab` is requested.
  void openBookmarkUrl(const QUrl &url, bool newTab);
  // A search suggestion is the final query; navigates to its search URL once.
  void activateSearchSuggestion(const QString &query);
  dalinira::core::INavigationCandidateProvider *candidateProvider() const { return candidateProvider_.get(); }
  void requestNewTabSuggestions(QWebEnginePage *page, const QString &query, int requestId);
  void requestEngineNewTabSuggestions(dalinira::engine::EngineView *view, const QString &query, int requestId);
  void setSearchEngine(const QString &engine);
  void syncNewTabViews();
  void onEngineFaviconLoaded(const QUrl &pageUrl, const QIcon &icon);
  void handleFrequentSitesCommand(const QString &op, const QUrl &url, const QString &engine = QString{}, const QString &name = QString{}, const QString &original = QString{});
  void handleNewTabBackgroundCommand(QWebEnginePage *sourcePage,
                                     const QString &command,
                                     const QString &capability);
  void handleEngineInternalNavigation(dalinira::engine::EngineView *sourceView,
                                      const QUrl &url);
  void handleEngineNewTabBackgroundCommand(dalinira::engine::EngineView *sourceView,
                                           const QString &command);
  void updateSearchEngineIcon();
  void toggleTabSearchPopup();

  // Tab Groups
  dalinira::desktop_tabs::TabGroupModel *groupModel() const { return groupModel_; }
  void toggleTabGroupLauncher();
  void createNewTabGroupWithNewTab();
  void createGroupFromExistingTab(uint64_t tabId);
  void showTabGroupPopup(const QUuid &groupId, const QPoint &globalPos = QPoint());
  void addTabToGroup(const QUuid &groupId);
  void moveGroupToNewWindow(const QUuid &groupId);
  void closeTabGroup(const QUuid &groupId);
  void ungroupTabs(const QUuid &groupId);
  void deleteTabGroup(const QUuid &groupId);
  std::optional<dalinira::desktop_tabs::TabGroup> groupForTab(uint64_t tabId) const;

  // Feature page navigations
  void showOnboarding();
  void finishOnboarding();
  void showSettings(
      SettingsPage::Category category = SettingsPage::Category::Startup);
  void showPasswords();
  void showAudioEffects();
  void showEqPresetBrowser();
  void showDaliNiraBlockerSettings(
      DaliNiraBlockerPage::Tab tab = DaliNiraBlockerPage::Tab::Settings);
  void showSongFinder();
  void showSongFinderSettings();
  void showMediaDownloads(const QUrl &sourceUrl = {},
                          bool analyzeImmediately = false);
  void openLocalMedia(const LocalMediaOpenRequest &request);
  void showTranslatePopup();
  void showZoomPopup();
  void changeCurrentZoom(qreal delta);
  void setCurrentZoom(qreal factor);
  void showMainMenu();
  void showHistoryMenu();
  void showDownloadsMenu();
  void toggleCurrentBookmark();
  void renderBookmarks();
  QIcon bookmarkFavicon(const QUrl &url) const;
  void requestBookmarkFavicon(const QUrl &url, std::function<void(const QIcon &)> callback);
  void fetchFaviconFromNetwork(const QUrl &url, std::function<void(const QIcon &)> callback);
  void updateBookmarkIconsForUrl(const QUrl &url, const QIcon &icon);
  bool isCurrentTabNewTab() const;
  void updateBookmarkBarVisibility();
  void toggleBookmarkBar();
  QToolBar *bookmarkBar() const { return bookmarkBar_; }
  void notifyNavigationStarted(QWebEngineView *view, const QUrl &url);

  bool isCurrentTabLoading() const;
  void updateReloadStopButton(bool isLoading);

public slots:
  void onBackClicked();
  void onForwardClicked();
  void onReloadOrStopClicked();
  void onHomeClicked();

public:
  void fillCurrentPageFromVault();
  CredentialAutofillController *autofillController() const { return autofillController_.get(); }
  void updateSaveBubblePosition();

  // Layout mode management
  void applyLayoutMode(dalinira::layout::BrowserLayoutMode mode);
  dalinira::layout::BrowserLayoutMode currentLayoutMode() const;
  void updateSidePanelTabs();
  // Feeds the side panel's auto-hide: New Tab active, active tab loading.
  void updateSidePanelAutoHide();
  // The docked panel's glass shows the active New Tab page's background.
  void scheduleSidePanelBackdrop();
  void updateSidePanelBackdrop();
  // The "Auto-hide side tabs" preference, applied to every window at once.
  static void applySidePanelAutoHideToAllWindows(bool enabled);
  // Back/forward/reload/add-site/main menu: the side panel's top bar in side
  // layouts, the toolbar in the classic layout (never both).
  void placeNavigationControls(bool inSidePanel);
  dalinira::layout::BrowserLayoutController *layoutController() const { return layoutController_; }
  void dismissCredentialSaveBubble();
  QIcon tabIconForRecord(const BrowserTabInfo &info) const;

  // Tab-scoped session permission management
  void clearTabSessionPermissions(uint64_t tabId);
  void clearTabSessionPermissionsForOrigin(uint64_t tabId, const QString &canonicalOrigin);
  void clearAllTabSessionPermissionsForOrigin(const QString &canonicalOrigin);
  void grantTabSessionPermission(uint64_t tabId, const QString &canonicalOrigin, const QString &permissionKey);
  bool hasTabSessionPermission(uint64_t tabId, const QString &canonicalOrigin, const QString &permissionKey) const;

  // Site controls & omnibox leading icon
  SiteControlsBubble *siteControlsBubble() const { return siteControlsBubble_.data(); }
  void toggleSiteControlsBubble();
  void dismissSiteControlsBubble();
  bool isInsideSiteControls(QWidget *target, const QPoint &globalPos) const;
  void updateOmniboxLeadingIcon();
  QAction *omniboxLeadingAction() const { return searchEngineAction_; }
  void setTabActiveMediaForTesting(int tabIndex, bool camera, bool mic);

  void prepareAdBlockScripts(QWebEnginePage *page, const QUrl &url,
                             bool force = false);
  // HTTPS by default, after a tab's load finished: a successful upgrade is
  // settled; for a failed one, the http:// URL the tab retries (else empty).
  QUrl httpsUpgradeResult(const QList<QUrl> &urls, bool ok);
  // "Block an element" on the current tab (DaliNiraElementPickerSession).
  void startElementPicker();
  DaliNiraElementPickerSession *elementPicker() const { return elementPicker_.data(); }
  void saveSessionNow();
  void restoreSession(const QVector<SavedTab> &savedTabs);
  void ensureInitialTab();
  void openStartupUrl(const QUrl &url);

  QSize restoredSize() const;
  QRect restoredGeometry() const;
  void updateBookmarkButtonState();
  void updateBlockerControls();

protected:
  bool eventFilter(QObject *watched, QEvent *event) override;
  void closeEvent(QCloseEvent *event) override;
  void keyPressEvent(QKeyEvent *event) override;
  void showEvent(QShowEvent *event) override;
  void changeEvent(QEvent *event) override;
  void resizeEvent(QResizeEvent *event) override;
  void moveEvent(QMoveEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;

private slots:
  void onOmniboxReturnPressed();
  void onMinimizeClicked();
  void onMaximizeRestoreClicked();
  void onCloseWindowClicked();
  void onTabHovered(int index, const QPoint &globalPos, const QRect &globalTabRect);
  void onTabHoverLeave();

private:
  QPointer<DaliNiraElementPickerSession> elementPicker_;
  // Vertical-tab transfers must recreate WebKit/XEmbed views instead of
  // reparenting their foreign native surface between top-level windows.
  bool transferSidePanelTabTo(uint64_t tabId, BrowserWindow *destination,
                              int targetIndex);
  void setupUi();
  void setupBrowserShortcuts();
  void applyShortcutBindings();
  void refreshShortcutHints();
  QString shortcutTooltip(const QString &labelKey, const QString &actionId) const;
  void setupStyle();
  // Applies an appearance theme ("system"/"light"/"dark") to the application
  // palette and restyles every browser window's chrome (Settings, onboarding).
  static void applyAppearanceToAllWindows(const QString &themeString);
  void setupTabStripSignals();
  void wireViewSignals(QWebEngineView *view, uint64_t tabId);
  void wireEngineViewSignals(dalinira::engine::EngineView *engineView, uint64_t tabId);
  // TabManager's tab order for this window follows tabs_.
  void syncTabManagerOrder();
  void updateNavButtons();
  void retranslateUi();
  // Page URL updates never replace text the user is still editing; tab switches force it.
  void updateOmniboxForCurrentTab(bool force = false);
  Qt::Edges calculateEdges(const QPoint &pos) const;
  void updateCursorShape(const QPoint &pos);
  void handleManualResize(const QPoint &globalPos);
  void onThrobberTick();

  BrowserServices services_;
  QHash<QString, QShortcut *> actionShortcuts_;
  QHash<QString, std::function<void()>> browserActions_;
  bool isCaptureShell_ = false;
  bool navigationInSidePanel_ = false;
  bool backdropQueryPending_ = false;
  bool isMovingTab_ = false;
  bool isSwitchingTab_ = false;
  bool adoptingEnginePopup_ = false;  // addNewTab() for a page-opened WebKit popup: WebKit loads it
  void closeTabForWindowClose(uint64_t tabId);
  QList<BrowserTabInfo> tabs_;

  dalinira::engine::EngineType currentWorkspaceEngine_ = dalinira::engine::EngineType::Chromium;

  // Frameless Top Bar
  QWidget *topBar_ = nullptr;
  QToolButton *tabSearchBtn_ = nullptr;
  dalinira::desktop_tabs::TabStripWidget *tabStrip_ = nullptr;
  QPointer<TabHoverCard> hoverCard_;
  QToolButton *minBtn_ = nullptr;
  QToolButton *maxBtn_ = nullptr;
  QToolButton *closeBtn_ = nullptr;

  // Navigation Bar & Features
  QWidget *navBar_ = nullptr;
  QToolButton *backBtn_ = nullptr;
  QToolButton *forwardBtn_ = nullptr;
  QToolButton *reloadBtn_ = nullptr;
  QToolButton *homeBtn_ = nullptr;
  QToolButton *bookmarkBtn_ = nullptr;
  QLineEdit *omnibox_ = nullptr;
  QCompleter *suggestionCompleter_ = nullptr;
  QPointer<QAbstractItemView> suggestionPopup_;
  QStandardItemModel *suggestionModel_ = nullptr;
  bool suggestionActivated_ = false;
  QCache<QString, QIcon> suggestionIconCache_{64};
  int pendingSuggestionIcons_ = 0;
  QJsonArray searchRows(const QString &query, const QStringList &remote) const;
  void updateOmniboxSuggestions(const QString &query);
  // Shared tab favicon policy for both engines: DaliNira icon on internal
  // pages, the page's favicon on web pages, a per-site fallback otherwise.
  // `engineReported`: the icon comes from the engine's icon signal, so a null
  // icon means the page has no favicon. Otherwise null only means "not known
  // yet" and a favicon already shown for the same site is kept (e.g. reload).
  void applyTabFavicon(int idx, const QObject *throbberKey, const QIcon &pageIcon, bool engineReported);
  // Engine-independent New Tab suggestion delivery. Each engine adapter
  // supplies its page object, privacy state, liveness check and JS runner.
  struct NewTabSuggestionTarget {
    QPointer<QObject> owner;
    bool privateMode = true;
    std::function<bool()> isCurrent;
    std::function<void(const QString &)> runScript;
  };
  void deliverNewTabSuggestions(const NewTabSuggestionTarget &target, const QString &query, int requestId);
  void handleEngineSuggestCommand(dalinira::engine::EngineView *sourceView, const QUrl &url);
  void activateSuggestion(const QUrl &url, const QString &text = QString{}, const QString &type = QString{});
  bool beginForgetClosedView(QWebEngineView *view);
  QAction *searchEngineAction_ = nullptr;
  QToolBar *bookmarkBar_ = nullptr;
  BookmarkBarReorder *bookmarkReorder_ = nullptr;
  QToolButton *appsBtn_ = nullptr;
  QPointer<dalinira::desktop_tabs::TabSearchPopup> tabSearchPopup_;
  dalinira::desktop_tabs::TabGroupModel *groupModel_ = nullptr;
  QPointer<dalinira::desktop_tabs::TabGroupPopup> tabGroupPopup_;
  QPointer<dalinira::desktop_tabs::TabGroupLauncherPopup> tabGroupLauncherPopup_;
  QProgressBar *progressBar_ = nullptr;
  QWidget *bodyContainer_ = nullptr;
  QBoxLayout *bodyLayout_ = nullptr;
  dalinira::layout::BrowserSidePanel *sidePanel_ = nullptr;
  dalinira::layout::BrowserLayoutController *layoutController_ = nullptr;
  QStackedWidget *pageStack_ = nullptr;


  // Feature buttons in navBar_
  QToolButton *zoomButton_ = nullptr;
  QFrame *zoomPopup_ = nullptr;
  QLabel *zoomPercent_ = nullptr;
  QToolButton *translateButton_ = nullptr;
  QToolButton *findButton_ = nullptr;
  DaliNiraBlockerShieldButton *adBlockShield_ = nullptr;
  PulseToolbarButton *pulseButton_ = nullptr;
  DownloadToolbarButton *mediaDownload_ = nullptr;
  DownloadUiModel *downloadUiModel_ = nullptr;
  DownloadPopup *downloadPopup_ = nullptr;
  QToolButton *passwordsBtn_ = nullptr;
  QToolButton *mainMenuBtn_ = nullptr;
  std::unique_ptr<CredentialAutofillController> autofillController_;
  std::unique_ptr<WebKitCredentialController> webKitCredentialController_;

  void updateDownloadToolbar();
  void showDownloadStartedAnimation();
  void resolveActiveMediaPageUrl(const QUrl &fallbackUrl,
                                 std::function<void(const QUrl &)> callback);
  bool downloadAnimationsEnabled() const;

  TranslateBubblePopup *translateBubble_ = nullptr;
  PageTranslator *pageTranslator_ = nullptr;
  QSet<QString> dismissedTranslationOffers_;
  PageTranslator *getOrCreatePageTranslator(int tabIndex);

  QPointer<SitePermissionPromptBubble> permissionBubble_;
  QPointer<SiteControlsBubble> siteControlsBubble_;
  QSet<TabSessionPermissionKey> tabSessionGrants_;
  void updateSiteControlsBubblePosition();
  void updatePermissionBubblePosition();
  void updatePermissionProtection();
  void syncTabSleepingIndicator(const QUuid &uuid);
  void resleepRestoredTabs(const QList<uint64_t> &tabIds);
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
  QList<PendingPermissionRequest> pendingPermissionQueue_;
  std::optional<PendingPermissionRequest> currentActivePermissionRequest_;
  void handleTabPermissionRequested(QWebEngineView *view, const QWebEnginePermission &permission);
  void processNextPermissionRequest();
  void resolveActivePermissionRequest(SitePermissionChoice choice);
  void dismissActivePermissionPrompt(bool cancelRequest = true);
  void syncProfilePermissionsForTab(uint64_t tabId);
#endif

  bool resizing_ = false;
  Qt::Edges resizeEdges_{};
  QPoint resizeStartPos_;
  QRect resizeStartGeometry_;
  bool manualMoving_ = false;
  QPoint manualMoveOffset_;
  bool titleMovePending_ = false;
  QPoint titleMovePressPos_;
  bool hasOverrideCursor_ = false;
  Qt::CursorShape currentOverrideShape_ = Qt::ArrowCursor;

  QSize lastNormalSize_{1280, 800};
  QRect lastNormalGeometry_{100, 100, 1280, 800};
  QUrl lastActiveWebUrl_;

  std::unique_ptr<dalinira::core::INavigationCandidateProvider> candidateProvider_;

  QPointer<dalinira::desktop_tabs::FindBarWidget> findBar_;
  void updateFindBarPosition();
  void handleFindRequest(const QString &text, bool forward, bool caseSensitive);
  void handleClearFind();
  void toggleBrowserFullScreen();

  bool isWebFullScreen_ = false;
  QPointer<QWidget> pageFullScreenOwner_;  // the page holding element fullscreen, if any
  Qt::WindowStates windowStateBeforeFullScreen_;
  QRect geometryBeforeFullScreen_;
};

#endif // BROWSER_WINDOW_H_
