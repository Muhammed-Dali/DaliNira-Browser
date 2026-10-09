#pragma once

#include <QObject>
#include <QDateTime>
#include <QList>
#include <QSettings>
#include <QSet>
#include <QUrl>
#include <QNetworkCookie>
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
#include <QWebEnginePermission>
#endif

#include <optional>
#include <memory>

#include "browser_profile_data_provider.h"
#include "engine/engine_download_context.h"

class SearchSuggestionService;
class QNetworkAccessManager;
class BrowserPolicy;
class QWebEngineDownloadRequest;
class QWebEnginePage;
class QWebEngineProfile;
class QWebEngineUrlRequestInterceptor;
class NewTabBackgroundStore;
class DaliNiraBlockerService;
using AdBlockService = DaliNiraBlockerService;
class CredentialVaultManager;
class TranslateService;
class GeneralDownloadManager;

struct BrowserDownloadEntry {
  QString fileName;
  QString path;
  QString state;
};

struct ClosedTabEntry {
  QString title;
  QUrl url;
  QDateTime closedAt;
};

struct CustomSearchEngine {
  QString name;
  QString urlTemplate;
};

// Owns the persistent Chromium profile and the browser-wide policies that are
// independent of an individual tab or window.
class BrowserProfileService final : public QObject, public dalinira::core::IBrowserProfileDataProvider {
  Q_OBJECT
 public:
  BrowserProfileService(const QString &dataDirectory, const BrowserPolicy *policy, QObject *parent = nullptr, bool privateMode = false, QNetworkAccessManager *suggestionNetwork = nullptr);
  ~BrowserProfileService() override;

  QWebEngineProfile *profile() const;
  NewTabBackgroundStore *newTabBackgroundStore() const;
  DaliNiraBlockerService *blockerService() const;
  DaliNiraBlockerService *adBlockService() const { return blockerService(); }
  CredentialVaultManager *credentialVault() const;
  TranslateService *translateService() const;
  QString dataDirectory() const;
  GeneralDownloadManager *downloadManager() const;

  void handleDownload(QWebEngineDownloadRequest *download);
  void handleDownloadRequest(const QUrl &url, const QString &suggestedName, const QString &mimeType, qint64 totalBytes);
  void handleDownloadContext(const dalinira::engine::EngineDownloadContext &context);
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
  void handlePermission(const QWebEnginePermission &permission);
  enum class OriginPolicyResult { Allow, Deny, Prompt };
  OriginPolicyResult evaluatePermissionPolicy(const QUrl &origin, QWebEnginePermission::PermissionType type) const;
  static QString permissionKeyFromType(QWebEnginePermission::PermissionType type);
  static QWebEnginePermission::PermissionType permissionTypeFromKey(const QString &key);
  void sanitizeStoredPermissions();
#endif

  static QString canonicalOrigin(const QUrl &url);
  static bool isPermissibleWebOrigin(const QUrl &url);
  static bool isTrustedInternalScheme(const QUrl &url);

  bool stripsTrackingParameters() const;
  void setStripsTrackingParameters(bool enabled);

  bool isAdultContentProtectionEnabled() const;
  void setAdultContentProtectionEnabled(bool enabled);

  // Permission policies
  QString permissionDefaultPolicy(const QString &permissionKey) const;
  void setPermissionDefaultPolicy(const QString &permissionKey, const QString &policy);
  bool autoRevokeUnusedPermissions() const;
  void setAutoRevokeUnusedPermissions(bool enabled);
  void revokeUnusedPermissionsNow();

  // Customized site behaviors / exceptions
  QStringList allowedOrigins(const QString &permissionKey) const;
  QStringList deniedOrigins(const QString &permissionKey) const;
  bool addSitePermissionRule(const QString &permissionKey, const QString &origin, bool allow);
  bool removeSitePermissionRule(const QString &permissionKey, const QString &origin);
  bool hasSitePermissionRule(const QString &permissionKey, const QString &origin) const;

  // Request display mode ("collapsed", "quiet", "expanded")
  QString requestDisplayMode(const QString &permissionKey) const;
  void setRequestDisplayMode(const QString &permissionKey, const QString &mode);

  // Preferred audio input device
  QString preferredAudioInputDevice() const;
  void setPreferredAudioInputDevice(const QString &deviceName);

  // Content settings
  QString cookiePolicy() const;
  // Gives engines that enforce cookies in their own store (WebKit) this
  // profile's cookie policy. Called on every policy change; call it once
  // more after registering an engine backend. Private profiles leave the
  // shared engine policy alone.
  void applyEngineCookiePolicy();
  void setCookiePolicy(const QString &policy);
  bool isJavascriptEnabled() const;
  void setJavascriptEnabled(bool enabled);
  bool isAutoLoadImagesEnabled() const;
  void setAutoLoadImagesEnabled(bool enabled);
  bool arePopupsAllowed() const;
  void setPopupsAllowed(bool allowed);
  bool isSoundAllowed() const;
  void setSoundAllowed(bool allowed);
  bool openPdfInBrowser() const;
  void setOpenPdfInBrowser(bool enabled);
  bool isProtectedContentEnabled() const;
  void setProtectedContentEnabled(bool enabled);
  QString insecureContentPolicy() const;
  void setInsecureContentPolicy(const QString &policy);
  QString siteDataPolicy() const;
  void setSiteDataPolicy(const QString &policy);
  bool isJsOptimizationEnabled() const;
  void setJsOptimizationEnabled(bool enabled);
  bool isAutoFullscreenAllowed() const;
  void setAutoFullscreenAllowed(bool allowed);

  QString configuredDownloadDirectory() const;
  void setDownloadDirectory(const QString &directory);
  bool asksDownloadLocation() const;
  void setAsksDownloadLocation(bool enabled);
  void clearHttpCache();
  void clearCookies();
  int cookiesCountForHost(const QString &host) const;
  void clearCookiesForHost(const QString &host);

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
  QList<QWebEnginePermission> sitePermissions() const;
  bool resetSitePermission(const QUrl &origin, QWebEnginePermission::PermissionType type);
#endif

  void recordHistory(const QUrl &url, const QString &title = QString{}, bool isTyped = false, const QString &engine = QString{});
  struct HistoryImportEntry {
    QUrl url;
    QString title;
    QDateTime visitedAt;
    int visitCount = 1;
    int typedCount = 0;
  };
  void importHistoryEntries(const QList<HistoryImportEntry> &entries,
                            const QString &engine = QString{});
  bool updateHistoryTitle(const QUrl &url, const QString &title);
  QList<BrowserHistoryEntry> recentHistory() const override;
  QList<BrowserFrequentSite> frequentSites(int limit = 6) const override;
  QList<BrowserFrequentSite> frequentSites(int limit, const QString &engine) const override;
  QString profilePath() const override { return dataDirectory(); }
  QStringList hiddenFrequentSites() const override;
  QStringList hiddenFrequentSites(const QString &engine) const override;
  bool hideFrequentSite(const QUrl &url) override;
  bool hideFrequentSite(const QUrl &url, const QString &engine) override;
  bool restoreHiddenFrequentSites() override;
  bool restoreHiddenFrequentSites(const QString &engine) override;
  bool isFrequentSiteHidden(const QUrl &url, const QString &engine = QString{}) const;

  struct QuickAccessItem {
    QUrl url;
    QString title;
  };
  QList<QuickAccessItem> quickAccessItems(const QString &engine = QString{}) const;
  QList<QUrl> quickAccess(const QString &engine = QString{}) const override;
  bool isQuickAccess(const QUrl &url, const QString &engine = QString{}) const override;
  bool addQuickAccess(const QUrl &url, const QString &title = QString{}, const QString &engine = QString{}) override;
  bool removeQuickAccess(const QUrl &url, const QString &engine = QString{}) override;
  bool toggleQuickAccess(const QUrl &url, const QString &title = QString{}, const QString &engine = QString{});
  // Moves an entry to `toIndex` (clamped) in the engine's order; the New Tab
  // shortcuts and the bookmark bar both follow it.
  bool moveQuickAccess(const QUrl &url, int toIndex, const QString &engine = QString{});
  void recordSearch(const QString &query);
  QStringList recentSearches(const QString &query = QString{}, int limit = 8) const;
  bool removeSearchHistory(const QString &query);
  bool removeSuggestion(const QString &type, const QString &text, const QUrl &url);
  void clearSearchHistory();
  void clearHistory();

  QList<BrowserDownloadEntry> recentDownloads() const;
  int recentDownloadCount() const override;
  QList<BrowserDownloadEntry> nativeDownloads() const;

  QList<BrowserHistoryEntry> searchHistory(const QString &query, int maxResults = 100) const;
  bool removeHistoryEntry(const QUrl &url, const QDateTime &visitedAt = QDateTime{});
  bool removeHistoryEntriesForUrl(const QUrl &url);

  void rememberClosedTab(const QUrl &url, const QString &title);
  bool hasClosedTabs() const;
  std::optional<ClosedTabEntry> takeMostRecentClosedTab();
  const QList<ClosedTabEntry> &closedTabs() const;
  std::optional<ClosedTabEntry> takeClosedTab(int index);

  SearchSuggestionService *searchSuggestions() const;
  void setSearchSuggestionsEnabled(bool enabled);
  // Engine-independent: WebKit private profiles have no QWebEngineProfile.
  bool isPrivate() const { return privateMode_; }
  QString searchEngine() const override;
  quint64 totalBlockedCount() const override;
  quint64 sessionBlockedCount() const override;
  void setSearchEngine(const QString &engine);
  QList<CustomSearchEngine> customSearchEngines() const;
  bool saveCustomSearchEngine(const QString &name, const QString &urlTemplate,
                              const QString &previousName = QString{});
  bool removeCustomSearchEngine(const QString &name);
  QUrl searchUrlForEngine(const QString &engine, const QString &query) const;
  static bool isValidSearchTemplate(const QString &urlTemplate);

  QList<QUrl> bookmarks() const override;
  bool isBookmarked(const QUrl &url) const;
  bool toggleBookmark(const QUrl &url);

  struct BookmarkItem {
    QString title;
    QUrl url;
    QString folder;
    QDateTime dateAdded;
  };
  QList<BookmarkItem> bookmarkItems() const;
  bool addBookmark(const QUrl &url, const QString &title = QString{}, const QString &folder = QString{});
  bool removeBookmark(const QUrl &url);
  bool createBookmarkFolder(const QString &folderName);
  bool removeBookmarkFolder(const QString &folderName, bool deleteContents = true);
  bool moveBookmarkToFolder(const QUrl &url, const QString &targetFolder);
  bool renameBookmarkFolder(const QString &oldName, const QString &newName);
  QStringList bookmarkFolders() const;
  QString exportBookmarksToHtml() const;
  int importBookmarksFromHtml(const QString &htmlContent);

  // Onboarding status
  bool isOnboardingCompleted() const;
  void setOnboardingCompleted(bool completed);
  int onboardingCurrentStep() const;
  void setOnboardingCurrentStep(int step);

  QString secureDnsMode() const;
  void setSecureDnsMode(const QString &mode);
  QString secureDnsTemplate() const;
  void setSecureDnsTemplate(const QString &templateUrl);
  void applySecureDnsSettings();

 signals:
  void pageSaveActiveChanged(QWebEnginePage *page, bool active);
  void downloadsChanged();
  void bookmarksChanged();
  void historyChanged();
  void trackingProtectionChanged();
  void contentSettingsChanged();
  void permissionsPolicyChanged();
  void searchSuggestionsChanged(bool enabled);
  void searchEngineChanged(const QString &engine);
  void closedTabsChanged();
  void secureDnsChanged();
  void adultContentProtectionChanged(bool enabled);
  void onboardingCompletedChanged(bool completed);

 private:
  QString downloadDirectory() const;
  void sanitizeStoredPersistentUrls();
  void refreshCookieFilter();
  void refreshPrivacyShield();
  void expireSiteDataForHost(const QString &host);

  const BrowserPolicy *policy_ = nullptr;
  bool privateMode_ = false;
  QString dataDirectory_;
  QSettings preferences_;
  QWebEngineProfile *profile_ = nullptr;
  DaliNiraBlockerService *blockerService_ = nullptr;
  CredentialVaultManager *credentialVault_ = nullptr;
  TranslateService *translateService_ = nullptr;
  GeneralDownloadManager *generalDownloadManager_ = nullptr;
  SearchSuggestionService *searchSuggestions_ = nullptr;
  QWebEngineUrlRequestInterceptor *interceptor_ = nullptr;
  std::unique_ptr<NewTabBackgroundStore> newTabBackgroundStore_;
  QList<BrowserDownloadEntry> downloads_;
  QList<ClosedTabEntry> closedTabs_;
  QSet<QString> forgetHosts_;
  QList<QNetworkCookie> cookies_;
};
