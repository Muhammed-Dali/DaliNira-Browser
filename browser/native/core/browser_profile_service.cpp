#include <QNetworkCookie>
#include "core/application_identity.h"
#include "core/sign_in_compatibility.h"
#include "search_suggestion_service.h"
#include "browser_profile_service.h"
#include "engine/engine_backend.h"
#include "core/engine_ui_profile.h"

#include <QApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHash>
#include <QMessageBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QUrlQuery>
#include <QWebEngineDownloadRequest>
#include <QWebEngineCookieStore>
#include <QWebEnginePage>
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
#include <QWebEnginePermission>
#endif
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
#include <QtWebEngineCore/qwebengineglobalsettings.h>
#endif
#include <QRegularExpression>
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
#include <QWebEngineClientHints>
#endif
#include <QWebEngineProfile>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineSettings>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestInterceptor>

#include <algorithm>

#include "browser_policy.h"
#include "search_engine_definition.h"
#include "security_utils.h"
#include "new_tab_asset_resolver.h"
#include "new_tab_scheme.h"
#include "new_tab_background_store.h"
#include "credential_vault_manager.h"
#include "downloads/general_download_manager.h"
#include "translate/translate_service.h"
#include "onboarding/onboarding_manager.h"

namespace {

class TrackingParameterInterceptor final : public QWebEngineUrlRequestInterceptor {
 public:
  explicit TrackingParameterInterceptor(QObject *parent = nullptr) : QWebEngineUrlRequestInterceptor(parent) {}

  void setEnabled(bool enabled) { enabled_ = enabled; }

  void interceptRequest(QWebEngineUrlRequestInfo &info) override {
    if (!enabled_) return;
    QUrl url = info.requestUrl();
    if (!url.isValid() || (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https"))) return;
    QUrlQuery query(url);
    static const QStringList trackingKeys = {
        QStringLiteral("fbclid"), QStringLiteral("gclid"), QStringLiteral("dclid"),
        QStringLiteral("msclkid"), QStringLiteral("mc_cid"), QStringLiteral("mc_eid"),
        QStringLiteral("_hsenc"), QStringLiteral("_hsmi")
    };
    bool changed = false;
    for (const QString &key : trackingKeys) {
      if (query.hasQueryItem(key)) { query.removeAllQueryItems(key); changed = true; }
    }
    const auto items = query.queryItems(QUrl::FullyDecoded);
    for (const auto &[key, value] : items) {
      Q_UNUSED(value);
      if (key.startsWith(QStringLiteral("utm_"), Qt::CaseInsensitive)) {
        query.removeAllQueryItems(key);
        changed = true;
      }
    }
    if (changed) { url.setQuery(query); info.redirect(url); }
  }

 private:
  bool enabled_ = true;
};


QString frequentSiteKey(const QUrl &url) {
  QString host = url.host().toLower();
  if (host.startsWith(QStringLiteral("www."))) host.remove(0, 4);
  if (host.isEmpty()) return {};
  const int port = url.port();
  if (port > 0 && port != 80 && port != 443) host += QStringLiteral(":%1").arg(port);
  return host;
}

QUrl frequentSiteRootUrl(const QUrl &url) {
  QUrl root;
  root.setScheme(url.scheme());
  root.setHost(url.host().toLower());
  const int port = url.port();
  if (port > 0 && port != 80 && port != 443) root.setPort(port);
  root.setPath(QStringLiteral("/"));
  return root;
}

QString normalizeEngineScope(const QString &engine) {
  if (!engine.isEmpty()) {
    const QString lower = engine.toLower().trimmed();
    if (lower == QLatin1String("webkit") || lower == QLatin1String("chromium")) {
      return lower;
    }
  }
  if (dalinira::engine::EngineBackend::selectedEngineType() == dalinira::engine::EngineType::WebKit) {
    return QStringLiteral("webkit");
  }
  return QStringLiteral("chromium");
}

QString frequentSitesStorageKey(const QString &engine) {
  return QStringLiteral("newtab/%1/frequentSites").arg(normalizeEngineScope(engine));
}

QString hiddenFrequentSitesStorageKey(const QString &engine) {
  return QStringLiteral("newtab/%1/hiddenFrequentSites").arg(normalizeEngineScope(engine));
}

QString quickAccessStorageKey(const QString &engine) {
  return QStringLiteral("newtab/%1/quickAccess").arg(normalizeEngineScope(engine));
}

}  // namespace

#include "dalinira_blocker_service.h"
#include "i18n/i18n.h"

namespace {
// window.name outlives navigations in a tab, so one site can leave an
// identifier there for the next. WebKit (and Brave) start a document of
// another site with an empty name; Chromium only does when it swaps browsing
// context groups, which link and script navigations do not. This document-
// start script does it for Chromium. It is a profile script because only
// those reach a renderer process before its first document (a page script
// arrives after a cross-site document has already run). Without per-tab
// state it decides from the document itself: the referrer's site, or no
// referrer (unknown: cleared, so a no-referrer page cannot carry the name
// over). A tab's first document keeps the name its opener gave it.
// Sites: IP addresses exactly, otherwise the registrable domain by the
// common heuristic (two labels, three under "co.uk"-style ccTLD suffixes).
QWebEngineScript windowNameIsolationScript() {
  QWebEngineScript script;
  script.setName(QStringLiteral("dalinira-window-name-isolation"));
  script.setInjectionPoint(QWebEngineScript::DocumentCreation);
  script.setWorldId(QWebEngineScript::MainWorld);
  script.setRunsOnSubFrames(false);
  script.setSourceCode(QStringLiteral(R"JS((()=>{try{
if(!/^https?:$/.test(location.protocol)||!window.name||history.length<2)return;
const site=h=>{h=String(h).toLowerCase().replace(/\.$/,'');if(/^[\d.]+$/.test(h)||h.includes(':'))return h;
const p=h.split('.');if(p.length<=2)return h;const n=p[p.length-1].length===2&&p[p.length-2].length<=3?3:2;return p.slice(-n).join('.')};
let from='';try{from=document.referrer?new URL(document.referrer).hostname:''}catch(_){}
if(from&&site(from)===site(location.hostname))return;
window.name='';
}catch(_){}})();)JS"));
  return script;
}
}  // namespace

BrowserProfileService::BrowserProfileService(const QString &dataDirectory, const BrowserPolicy *policy, QObject *parent, bool privateMode, QNetworkAccessManager *suggestionNetwork)
    : QObject(parent), policy_(policy), privateMode_(privateMode), dataDirectory_(QFileInfo(dataDirectory).absoluteFilePath()),
      preferences_(dataDirectory_ + "/browser-preferences.ini", QSettings::IniFormat) {
  QDir().mkpath(dataDirectory);
  QFile::setPermissions(dataDirectory_, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
  QFile::setPermissions(dataDirectory_ + QStringLiteral("/browser-preferences.ini"), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  sanitizeStoredPersistentUrls();
  const bool isChromium = (dalinira::engine::EngineBackend::selectedEngineType() == dalinira::engine::EngineType::Chromium);
  // Managed New Tab backgrounds are visual profile data. Preserve Chromium's
  // existing location and give WebKit an independent store.
  const QString appearanceDataDirectory = isChromium
      ? dataDirectory_
      : QDir(dataDirectory_).filePath(QStringLiteral("webkit/ui"));
  newTabBackgroundStore_ = std::make_unique<NewTabBackgroundStore>(appearanceDataDirectory);
  if (isChromium) {
    profile_ = privateMode ? new QWebEngineProfile(this) : new QWebEngineProfile(QStringLiteral("dalinira-browser"), this);
    if (!privateMode) {
      profile_->setPersistentStoragePath(dataDirectory + "/profile");
      profile_->setCachePath(dataDirectory + "/cache");
      profile_->setHttpCacheType(QWebEngineProfile::DiskHttpCache);
      const QString cookiePol = preferences_.value(QStringLiteral("content/cookiePolicy"), QStringLiteral("third_party")).toString();
      if (cookiePol == QStringLiteral("block_all")) {
        profile_->setPersistentCookiesPolicy(QWebEngineProfile::NoPersistentCookies);
      } else {
        profile_->setPersistentCookiesPolicy(QWebEngineProfile::ForcePersistentCookies);
      }
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
      profile_->setPersistentPermissionsPolicy(QWebEngineProfile::PersistentPermissionsPolicy::StoreOnDisk);
#endif
    }
    profile_->setDownloadPath(downloadDirectory());
    profile_->setSpellCheckEnabled(true);
    profile_->setSpellCheckLanguages({QStringLiteral("tr-TR"), QStringLiteral("en-US")});
    auto *settings = profile_->settings();
    const bool jsEnabled = preferences_.value(QStringLiteral("content/javascriptEnabled"), true).toBool();
    settings->setAttribute(QWebEngineSettings::JavascriptEnabled, jsEnabled);
    const bool autoImages = preferences_.value(QStringLiteral("content/autoLoadImages"), true).toBool();
    settings->setAttribute(QWebEngineSettings::AutoLoadImages, autoImages);
    const bool popupsAllowed = preferences_.value(QStringLiteral("content/popupsAllowed"), false).toBool();
    settings->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows, popupsAllowed);
    settings->setAttribute(QWebEngineSettings::AllowWindowActivationFromJavaScript, popupsAllowed);
    const bool pdfViewer = preferences_.value(QStringLiteral("content/openPdfInBrowser"), true).toBool();
    settings->setAttribute(QWebEngineSettings::PdfViewerEnabled, pdfViewer);
    settings->setAttribute(QWebEngineSettings::JavascriptCanAccessClipboard, false);
    settings->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, false);
    settings->setAttribute(QWebEngineSettings::AllowRunningInsecureContent, false);
    settings->setAttribute(QWebEngineSettings::WebRTCPublicInterfacesOnly, true);
    settings->setAttribute(QWebEngineSettings::FullScreenSupportEnabled, true);
    settings->setUnknownUrlSchemePolicy(QWebEngineSettings::DisallowUnknownUrlSchemes);
    profile_->scripts()->insert(windowNameIsolationScript());
    // A plain Chromium User-Agent: no "QtWebEngine/x.y" token that only this
    // embedder sends, and no OS (kernel) version in the client hints. Google
    // sign-in keeps the engine's own User-Agent (core/sign_in_compatibility.h).
    dalinira::sign_in_compatibility::setChromiumEngineUserAgent(profile_->httpUserAgent().toUtf8());
    profile_->setHttpUserAgent(profile_->httpUserAgent().remove(QRegularExpression(QStringLiteral("QtWebEngine/[\\d.]+ "))));
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    // Qt 6.8 added client hints control; older Qt sends Chromium's defaults.
    profile_->clientHints()->setPlatformVersion(QString());
#endif
  } else {
    profile_ = nullptr;
  }

  if (autoRevokeUnusedPermissions()) {
    revokeUnusedPermissionsNow();
  }

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
  sanitizeStoredPermissions();
#endif

  searchSuggestions_ = new SearchSuggestionService(this, suggestionNetwork);
  searchSuggestions_->setEnabled(!privateMode && QSettings().value(QStringLiteral("browser/searchSuggestionsEnabled"), false).toBool());
  blockerService_ = new DaliNiraBlockerService(dataDirectory, this);
  blockerService_->setTrackingParameterStrippingEnabled(stripsTrackingParameters());
  if (profile_) {
    // GPC and fingerprinting protection in every Chromium document; the
    // script follows the blocker settings (exempt sites, the switch).
    refreshPrivacyShield();
    connect(blockerService_->settings(), &DaliNiraBlockerSettings::settingsChanged, this,
            &BrowserProfileService::refreshPrivacyShield);
  }
  credentialVault_ = new CredentialVaultManager(dataDirectory, this);
  translateService_ = new TranslateService(this, nullptr, credentialVault_);
  translateService_->loadPreferences(preferences_);
  applySecureDnsSettings();
  connect(blockerService_->settings(), &DaliNiraBlockerSettings::settingsChanged, this, &BrowserProfileService::refreshCookieFilter);
  connect(this, &BrowserProfileService::contentSettingsChanged, this, &BrowserProfileService::refreshCookieFilter);
  refreshCookieFilter();
  connect(blockerService_, &DaliNiraBlockerService::siteClosed, this, [this](const QString &host) {
    const auto policy = blockerService_->sitePolicy(host);
    if (!blockerService_->settings()->protectionEnabled() || policy.whitelisted || !policy.forgetOnClose) return;
    expireSiteDataForHost(host);
  });
  connect(blockerService_, &DaliNiraBlockerService::temporarySiteDataExpired,
          this, &BrowserProfileService::expireSiteDataForHost);
  if (profile_) {
    connect(blockerService_, &DaliNiraBlockerService::siteOpened, this, [this](const QString &host) { forgetHosts_.remove(host); });
    connect(profile_->cookieStore(), &QWebEngineCookieStore::cookieAdded, this, [this](const QNetworkCookie &cookie) {
      QString domain = cookie.domain();
      if (domain.startsWith(QLatin1Char('.'))) domain.remove(0,1);
      domain = DaliNiraBlockerSettings::normalizeSiteHost(domain);
      for (const auto &host : std::as_const(forgetHosts_)) {
        if (domain == host || domain.endsWith(QLatin1Char('.') + host)) {
          profile_->cookieStore()->deleteCookie(cookie);
          break;
        }
      }
      for (int i = 0; i < cookies_.size(); ++i) {
        if (cookies_[i].name() == cookie.name() && cookies_[i].domain() == cookie.domain() && cookies_[i].path() == cookie.path()) {
          cookies_.removeAt(i);
          break;
        }
      }
      cookies_.append(cookie);
    });
    connect(profile_->cookieStore(), &QWebEngineCookieStore::cookieRemoved, this, [this](const QNetworkCookie &cookie) {
      for (int i = 0; i < cookies_.size(); ++i) {
        if (cookies_[i].name() == cookie.name() && cookies_[i].domain() == cookie.domain() && cookies_[i].path() == cookie.path()) {
          cookies_.removeAt(i);
          break;
        }
      }
    });
    profile_->cookieStore()->loadAllCookies();
    interceptor_ = blockerService_->requestInterceptor();
    profile_->setUrlRequestInterceptor(interceptor_);
    if (DaliNiraBlockerRequestInterceptor::resourceSchemeRegistered())
      profile_->installUrlSchemeHandler(DaliNiraBlockerRequestInterceptor::resourceScheme(),
                                        blockerService_->resourceSchemeHandler());
    if (qEnvironmentVariableIntValue("DALINIRA_FEATURE_DIAGNOSTICS") == 1) {
      qInfo().noquote() << "[BLOCKER] interceptor attached";
    }

    const QString newTabAssetsDirectory = resolveNewTabAssetsDirectory(
        dalinira::application_identity::applicationDirectory());
    profile_->installUrlSchemeHandler("dalinira", createNewTabSchemeHandler(
        newTabAssetsDirectory, newTabBackgroundStore_->managedImagePath(),
        newTabBackgroundStore_->thumbnailPath(), this, this, profile_));
  }
  generalDownloadManager_ = new GeneralDownloadManager(
      dataDirectory_ + QStringLiteral("/general-downloads.json"), nullptr, this);
  connect(generalDownloadManager_, &GeneralDownloadManager::jobsChanged,
          this, &BrowserProfileService::downloadsChanged);
  if (profile_) {
    connect(profile_, &QWebEngineProfile::downloadRequested, this, &BrowserProfileService::handleDownload);
  }
}

BrowserProfileService::~BrowserProfileService() = default;

QWebEngineProfile *BrowserProfileService::profile() const { return profile_; }
QString BrowserProfileService::dataDirectory() const { return dataDirectory_; }
GeneralDownloadManager *BrowserProfileService::downloadManager() const { return generalDownloadManager_; }

NewTabBackgroundStore *BrowserProfileService::newTabBackgroundStore() const { return newTabBackgroundStore_.get(); }

DaliNiraBlockerService *BrowserProfileService::blockerService() const { return blockerService_; }
CredentialVaultManager *BrowserProfileService::credentialVault() const { return credentialVault_; }
TranslateService *BrowserProfileService::translateService() const { return translateService_; }

QString BrowserProfileService::downloadDirectory() const {
  const QString configured = preferences_.value(QStringLiteral("downloads/directory")).toString();
  if (!configured.isEmpty() && QFileInfo(configured).isDir()) return configured;
  return QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
}

void BrowserProfileService::handleDownload(QWebEngineDownloadRequest *download) {
  if (!download || !policy_ || !policy_->allowsDownloadPrompt()) { if (download) download->cancel(); return; }
  const QString safeFileName = BrowserSecurity::sanitizeDownloadFileName(download->suggestedFileName());
  const QString suggested = QDir(downloadDirectory()).filePath(safeFileName);
  const bool ask = preferences_.value(QStringLiteral("downloads/askLocation"), true).toBool();
  QString target = suggested;
  if (ask) target = QFileDialog::getSaveFileName(QApplication::activeWindow(), I18n::text(QStringLiteral("downloads.save_download_title")), suggested);
  if (target.isEmpty()) { download->cancel(); return; }
  const QFileInfo info(target);
  if (generalDownloadManager_ && !download->isSavePageDownload()
      && GeneralDownloadManager::canHandleUrl(download->url())) {
    GeneralDownloadRequest request;
    request.sourceEngine = dalinira::engine::EngineType::Chromium;
    request.url = download->url();
    request.suggestedFileName = info.fileName();
    request.targetDirectory = info.absolutePath();
    request.mimeType = download->mimeType();
    request.expectedBytes = download->totalBytes();
    request.connectionCount = 4;
    request.allowParallel = true;
    // The native dialog already obtained explicit overwrite consent for this
    // exact path. The old file remains intact until atomic finalization.
    request.overwriteExisting = true;
    request.userAgent = profile_ ? profile_->httpUserAgent().toUtf8() : QByteArray("DaliNira/8.0.2");
    if (download->page()) {
      request.referrer = download->page()->url().toEncoded(QUrl::FullyEncoded);
      request.pageUrl = download->page()->url();
    }
    request.isPrivate = profile_ && profile_->isOffTheRecord();
    const QString host = request.url.host().toLower();
    const QString path = request.url.path().isEmpty() ? QStringLiteral("/") : request.url.path();
    for (const QNetworkCookie &cookie : std::as_const(cookies_)) {
      QString domain = cookie.domain().toLower();
      if (domain.startsWith(QLatin1Char('.'))) domain.remove(0, 1);
      const bool domainMatches = host == domain || host.endsWith(QLatin1Char('.') + domain);
      const bool pathMatches = path.startsWith(cookie.path().isEmpty()
          ? QStringLiteral("/") : cookie.path());
      if (domainMatches && pathMatches && (!cookie.isSecure() || request.url.scheme() == QLatin1String("https")))
        request.cookies.append(cookie);
    }
    if (!generalDownloadManager_->enqueue(request).isNull()) {
      download->cancel();
      return;
    }
  }
  download->setDownloadDirectory(info.absolutePath());
  download->setDownloadFileName(info.fileName());
  // Saving a page needs the live page until the file is written: the tab
  // must not be put to sleep meanwhile. Other downloads run in the browser
  // process and do not depend on the tab.
  if (download->isSavePageDownload() && download->page()) {
    const QPointer<QWebEnginePage> page(download->page());
    emit pageSaveActiveChanged(page.data(), true);
    connect(download, &QWebEngineDownloadRequest::isFinishedChanged, this, [this, page] {
      if (page) emit pageSaveActiveChanged(page.data(), false);
    });
  }
  download->accept();
  downloads_.prepend({info.fileName(), info.absoluteFilePath(), I18n::text(QStringLiteral("downloads.state_downloading"))});
  while (downloads_.size() > 30) downloads_.removeLast();
  emit downloadsChanged();
  connect(download, &QWebEngineDownloadRequest::stateChanged, this, [this, path = info.absoluteFilePath()](QWebEngineDownloadRequest::DownloadState state) {
    for (BrowserDownloadEntry &entry : downloads_) {
      if (entry.path != path) continue;
      switch (state) {
        case QWebEngineDownloadRequest::DownloadCompleted: entry.state = I18n::text(QStringLiteral("downloads.state_completed")); break;
        case QWebEngineDownloadRequest::DownloadCancelled: entry.state = I18n::text(QStringLiteral("downloads.state_cancelled")); break;
        case QWebEngineDownloadRequest::DownloadInterrupted: entry.state = I18n::text(QStringLiteral("downloads.state_interrupted")); break;
        default: entry.state = I18n::text(QStringLiteral("downloads.state_downloading")); break;
      }
      emit downloadsChanged();
      return;
    }
  });
}

void BrowserProfileService::handleDownloadContext(const dalinira::engine::EngineDownloadContext &context) {
  if (!context.url.isValid() || !policy_ || !policy_->allowsDownloadPrompt()) return;

  const QString rawName = context.suggestedName.isEmpty() ? QFileInfo(context.url.path()).fileName() : context.suggestedName;
  const QString safeFileName = BrowserSecurity::sanitizeDownloadFileName(rawName.isEmpty() ? QStringLiteral("download") : rawName);
  const QString suggested = QDir(downloadDirectory()).filePath(safeFileName);
  const bool ask = preferences_.value(QStringLiteral("downloads/askLocation"), true).toBool();
  QString target = suggested;
  if (ask) target = QFileDialog::getSaveFileName(QApplication::activeWindow(), I18n::text(QStringLiteral("downloads.save_download_title")), suggested);
  if (target.isEmpty()) return;

  const QFileInfo info(target);
  if (generalDownloadManager_ && GeneralDownloadManager::canHandleUrl(context.url)) {
    GeneralDownloadRequest request;
    request.sourceEngine = context.sourceEngine;
    request.sourceTabId = context.sourceTabId;
    request.url = context.url;
    request.pageUrl = context.pageUrl;
    request.originalUrl = context.originalUrl;
    request.suggestedFileName = info.fileName();
    request.targetDirectory = info.absolutePath();
    request.mimeType = context.mimeType;
    request.expectedBytes = context.totalBytes;
    request.connectionCount = 4;
    request.allowParallel = true;
    request.overwriteExisting = true;
    request.userAgent = !context.userAgent.isEmpty() ? context.userAgent : QByteArray("DaliNira/8.0.2 (WebKit)");
    request.referrer = context.referrer;
    request.cookies = context.cookies;
    request.isPrivate = context.isPrivate;
    request.navigationGeneration = context.navigationGeneration;
    if (!generalDownloadManager_->enqueue(request).isNull()) {
      return;
    }
  }

  downloads_.prepend({info.fileName(), info.absoluteFilePath(), I18n::text(QStringLiteral("downloads.state_downloading"))});
  while (downloads_.size() > 30) downloads_.removeLast();
  emit downloadsChanged();
}

void BrowserProfileService::handleDownloadRequest(const QUrl &url, const QString &suggestedName,
                                                  const QString &mimeType, qint64 totalBytes) {
  dalinira::engine::EngineDownloadContext ctx;
  ctx.sourceEngine = dalinira::engine::EngineType::WebKit;
  ctx.url = url;
  ctx.suggestedName = suggestedName;
  ctx.mimeType = mimeType;
  ctx.totalBytes = totalBytes;
  handleDownloadContext(ctx);
}


#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
QString BrowserProfileService::permissionKeyFromType(QWebEnginePermission::PermissionType type) {
  switch (type) {
    case QWebEnginePermission::PermissionType::Geolocation: return QStringLiteral("geolocation");
    case QWebEnginePermission::PermissionType::MediaVideoCapture: return QStringLiteral("camera");
    case QWebEnginePermission::PermissionType::MediaAudioCapture: return QStringLiteral("microphone");
    case QWebEnginePermission::PermissionType::MediaAudioVideoCapture: return QStringLiteral("cameraMicrophone");
    case QWebEnginePermission::PermissionType::Notifications: return QStringLiteral("notifications");
    case QWebEnginePermission::PermissionType::ClipboardReadWrite: return QStringLiteral("clipboard");
    case QWebEnginePermission::PermissionType::LocalFontsAccess: return QStringLiteral("localFonts");
    case QWebEnginePermission::PermissionType::MouseLock: return QStringLiteral("mouseLock");
    case QWebEnginePermission::PermissionType::DesktopVideoCapture:
    case QWebEnginePermission::PermissionType::DesktopAudioVideoCapture: return QStringLiteral("screenShare");
    default: return {};
  }
}

QWebEnginePermission::PermissionType BrowserProfileService::permissionTypeFromKey(const QString &key) {
  if (key == QStringLiteral("geolocation")) return QWebEnginePermission::PermissionType::Geolocation;
  if (key == QStringLiteral("camera")) return QWebEnginePermission::PermissionType::MediaVideoCapture;
  if (key == QStringLiteral("microphone")) return QWebEnginePermission::PermissionType::MediaAudioCapture;
  if (key == QStringLiteral("cameraMicrophone")) return QWebEnginePermission::PermissionType::MediaAudioVideoCapture;
  if (key == QStringLiteral("notifications")) return QWebEnginePermission::PermissionType::Notifications;
  if (key == QStringLiteral("clipboard")) return QWebEnginePermission::PermissionType::ClipboardReadWrite;
  if (key == QStringLiteral("localFonts")) return QWebEnginePermission::PermissionType::LocalFontsAccess;
  if (key == QStringLiteral("mouseLock")) return QWebEnginePermission::PermissionType::MouseLock;
  if (key == QStringLiteral("screenShare")) return QWebEnginePermission::PermissionType::DesktopVideoCapture;
  return QWebEnginePermission::PermissionType::Unsupported;
}

BrowserProfileService::OriginPolicyResult BrowserProfileService::evaluatePermissionPolicy(
    const QUrl &origin, QWebEnginePermission::PermissionType type) const {
  if (type == QWebEnginePermission::PermissionType::MediaAudioVideoCapture) {
    const auto camResult = evaluatePermissionPolicy(origin, QWebEnginePermission::PermissionType::MediaVideoCapture);
    const auto micResult = evaluatePermissionPolicy(origin, QWebEnginePermission::PermissionType::MediaAudioCapture);
    if (camResult == OriginPolicyResult::Deny || micResult == OriginPolicyResult::Deny) {
      return OriginPolicyResult::Deny;
    }
    const QString canon = canonicalOrigin(origin);
    const QString host = origin.host().toLower();
    const QString originStr = origin.toDisplayString();
    const QStringList denied = deniedOrigins(QStringLiteral("cameraMicrophone"));
    for (const QString &d : denied) {
      const QString dClean = d.trimmed().toLower();
      if (dClean == canon || dClean == host || dClean == originStr.toLower() ||
          (dClean.startsWith(QStringLiteral("[*.]")) && host.endsWith(dClean.mid(4)))) {
        return OriginPolicyResult::Deny;
      }
    }
    const QStringList allowed = allowedOrigins(QStringLiteral("cameraMicrophone"));
    for (const QString &a : allowed) {
      const QString aClean = a.trimmed().toLower();
      if (aClean == canon || aClean == host || aClean == originStr.toLower() ||
          (aClean.startsWith(QStringLiteral("[*.]")) && host.endsWith(aClean.mid(4)))) {
        return OriginPolicyResult::Allow;
      }
    }
    if (camResult == OriginPolicyResult::Allow && micResult == OriginPolicyResult::Allow) {
      return OriginPolicyResult::Allow;
    }
    return OriginPolicyResult::Prompt;
  }

  const QString permKey = permissionKeyFromType(type);
  if (permKey.isEmpty()) return OriginPolicyResult::Deny;

  const QString canon = canonicalOrigin(origin);
  const QString host = origin.host().toLower();
  const QString originStr = origin.toDisplayString();

  const QStringList denied = deniedOrigins(permKey);
  for (const QString &d : denied) {
    const QString dClean = d.trimmed().toLower();
    if (dClean == canon || dClean == host || dClean == originStr.toLower() ||
        (dClean.startsWith(QStringLiteral("[*.]")) && host.endsWith(dClean.mid(4)))) {
      return OriginPolicyResult::Deny;
    }
  }

  const QStringList allowed = allowedOrigins(permKey);
  for (const QString &a : allowed) {
    const QString aClean = a.trimmed().toLower();
    if (aClean == canon || aClean == host || aClean == originStr.toLower() ||
        (aClean.startsWith(QStringLiteral("[*.]")) && host.endsWith(aClean.mid(4)))) {
      return OriginPolicyResult::Allow;
    }
  }

  const QString policy = permissionDefaultPolicy(permKey);
  if (policy == QStringLiteral("deny")) {
    return OriginPolicyResult::Deny;
  } else if (policy == QStringLiteral("allow")) {
    return OriginPolicyResult::Allow;
  }

  return OriginPolicyResult::Prompt;
}

void BrowserProfileService::sanitizeStoredPermissions() {
  if (!profile_) return;
  const auto all = profile_->listAllPermissions();
  for (const auto &perm : all) {
    if (!perm.isValid()) continue;
    const QString permKey = permissionKeyFromType(perm.permissionType());
    if (permKey.isEmpty()) {
      perm.reset();
      continue;
    }
    const QString canon = canonicalOrigin(perm.origin());
    const QString host = perm.origin().host().toLower();
    const QString disp = perm.origin().toDisplayString();
    const QStringList allowed = allowedOrigins(permKey);
    const QStringList denied = deniedOrigins(permKey);
    bool hasPermanent = false;
    for (const QString &item : allowed) {
      if (item.compare(canon, Qt::CaseInsensitive) == 0 ||
          item.compare(host, Qt::CaseInsensitive) == 0 ||
          item.compare(disp, Qt::CaseInsensitive) == 0) {
        hasPermanent = true;
        break;
      }
    }
    if (!hasPermanent) {
      for (const QString &item : denied) {
        if (item.compare(canon, Qt::CaseInsensitive) == 0 ||
            item.compare(host, Qt::CaseInsensitive) == 0 ||
            item.compare(disp, Qt::CaseInsensitive) == 0) {
          hasPermanent = true;
          break;
        }
      }
    }
    if (!hasPermanent) {
      perm.reset();
    }
  }
}

void BrowserProfileService::handlePermission(const QWebEnginePermission &permission) {
  if (!permission.isValid()) return;
  if (permission.state() == QWebEnginePermission::State::Granted || permission.state() == QWebEnginePermission::State::Denied) return;

  const OriginPolicyResult result = evaluatePermissionPolicy(permission.origin(), permission.permissionType());
  if (result == OriginPolicyResult::Allow) {
    permission.grant();
  } else if (result == OriginPolicyResult::Deny) {
    permission.deny();
  }
}
#endif

QString BrowserProfileService::canonicalOrigin(const QUrl &url) {
  if (!url.isValid()) return {};
  const QString scheme = url.scheme().toLower();
  const QString host = url.host().toLower();
  if (host.isEmpty()) return {};
  const int port = url.port();
  if (port > 0 && port != 80 && port != 443) {
    return QStringLiteral("%1://%2:%3").arg(scheme, host).arg(port);
  }
  return QStringLiteral("%1://%2").arg(scheme, host);
}

bool BrowserProfileService::isPermissibleWebOrigin(const QUrl &url) {
  if (!url.isValid()) return false;
  const QString scheme = url.scheme().toLower();
  if (scheme == QLatin1String("http") || scheme == QLatin1String("https")) {
    return !url.host().trimmed().isEmpty();
  }
  return false;
}

bool BrowserProfileService::isTrustedInternalScheme(const QUrl &url) {
  if (!url.isValid()) return false;
  return url.scheme().toLower() == QLatin1String("dalinira");
}

QStringList BrowserProfileService::allowedOrigins(const QString &permissionKey) const {
  return preferences_.value(QStringLiteral("customPermissions/") + permissionKey + QStringLiteral("/allowed")).toStringList();
}

QStringList BrowserProfileService::deniedOrigins(const QString &permissionKey) const {
  return preferences_.value(QStringLiteral("customPermissions/") + permissionKey + QStringLiteral("/denied")).toStringList();
}

bool BrowserProfileService::addSitePermissionRule(const QString &permissionKey, const QString &origin, bool allow) {
  QString clean = origin.trimmed();
  if (clean.isEmpty()) return false;

  QUrl testUrl(clean);
  if (testUrl.scheme().isEmpty()) {
    testUrl = QUrl(QStringLiteral("https://") + clean);
  }
  if (!isPermissibleWebOrigin(testUrl)) return false;
  clean = canonicalOrigin(testUrl);
  if (clean.isEmpty()) return false;

  QStringList allowed = allowedOrigins(permissionKey);
  QStringList denied = deniedOrigins(permissionKey);
  allowed.removeAll(clean);
  denied.removeAll(clean);
  if (allow) {
    allowed.append(clean);
  } else {
    denied.append(clean);
  }
  preferences_.setValue(QStringLiteral("customPermissions/") + permissionKey + QStringLiteral("/allowed"), allowed);
  preferences_.setValue(QStringLiteral("customPermissions/") + permissionKey + QStringLiteral("/denied"), denied);
  preferences_.sync();

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
  if (profile_) {
    const QUrl targetUrl(clean.startsWith(QStringLiteral("http")) ? clean : QStringLiteral("https://") + clean);
    if (targetUrl.isValid()) {
      const QWebEnginePermission::PermissionType pType = permissionTypeFromKey(permissionKey);
      if (pType != QWebEnginePermission::PermissionType::Unsupported) {
        QWebEnginePermission perm = profile_->queryPermission(targetUrl, pType);
        if (perm.isValid()) {
          if (allow) perm.grant(); else perm.deny();
        }
      }
    }
  }
#endif

  emit permissionsPolicyChanged();
  return true;
}

bool BrowserProfileService::removeSitePermissionRule(const QString &permissionKey, const QString &origin) {
  QString clean = origin.trimmed();
  if (clean.isEmpty()) return false;
  QStringList allowed = allowedOrigins(permissionKey);
  QStringList denied = deniedOrigins(permissionKey);
  const int removedCount = allowed.removeAll(clean) + denied.removeAll(clean);
  if (removedCount == 0) return false;
  preferences_.setValue(QStringLiteral("customPermissions/") + permissionKey + QStringLiteral("/allowed"), allowed);
  preferences_.setValue(QStringLiteral("customPermissions/") + permissionKey + QStringLiteral("/denied"), denied);
  preferences_.sync();

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
  if (profile_) {
    const QUrl targetUrl(clean.startsWith(QStringLiteral("http")) ? clean : QStringLiteral("https://") + clean);
    if (targetUrl.isValid()) {
      const QWebEnginePermission::PermissionType pType = permissionTypeFromKey(permissionKey);
      if (pType != QWebEnginePermission::PermissionType::Unsupported) {
        const QWebEnginePermission permission = profile_->queryPermission(targetUrl, pType);
        if (permission.isValid()) {
          permission.reset();
        }
      }
    }
  }
#endif

  emit permissionsPolicyChanged();
  return true;
}

bool BrowserProfileService::hasSitePermissionRule(const QString &permissionKey, const QString &origin) const {
  const QString clean = origin.trimmed().toLower();
  if (clean.isEmpty() || permissionKey.isEmpty()) return false;
  const QString canon = canonicalOrigin(QUrl(clean.startsWith(QStringLiteral("http")) ? clean : QStringLiteral("https://") + clean));
  const QString host = QUrl(clean.startsWith(QStringLiteral("http")) ? clean : QStringLiteral("https://") + clean).host().toLower();

  const QStringList allowed = allowedOrigins(permissionKey);
  for (const QString &item : allowed) {
    const QString c = item.trimmed().toLower();
    if (c == clean || c == canon || c == host ||
        (c.startsWith(QStringLiteral("[*.]")) && (host.endsWith(c.mid(4)) || clean.endsWith(c.mid(4))))) {
      return true;
    }
  }

  const QStringList denied = deniedOrigins(permissionKey);
  for (const QString &item : denied) {
    const QString c = item.trimmed().toLower();
    if (c == clean || c == canon || c == host ||
        (c.startsWith(QStringLiteral("[*.]")) && (host.endsWith(c.mid(4)) || clean.endsWith(c.mid(4))))) {
      return true;
    }
  }

  return false;
}

QString BrowserProfileService::requestDisplayMode(const QString &permissionKey) const {
  return preferences_.value(QStringLiteral("permissionsDisplayMode/") + permissionKey, QStringLiteral("quiet")).toString();
}

void BrowserProfileService::setRequestDisplayMode(const QString &permissionKey, const QString &mode) {
  if (requestDisplayMode(permissionKey) == mode) return;
  preferences_.setValue(QStringLiteral("permissionsDisplayMode/") + permissionKey, mode);
  preferences_.sync();
  emit permissionsPolicyChanged();
}

QString BrowserProfileService::preferredAudioInputDevice() const {
  return preferences_.value(QStringLiteral("hardware/preferredAudioInputDevice")).toString();
}

void BrowserProfileService::setPreferredAudioInputDevice(const QString &deviceName) {
  if (preferredAudioInputDevice() == deviceName) return;
  preferences_.setValue(QStringLiteral("hardware/preferredAudioInputDevice"), deviceName);
  preferences_.sync();
}

QString BrowserProfileService::permissionDefaultPolicy(const QString &permissionKey) const {
  if (permissionKey == QStringLiteral("notifications")) {
    return preferences_.value(QStringLiteral("permissions/notifications"), QStringLiteral("quiet")).toString();
  }
  return preferences_.value(QStringLiteral("permissions/") + permissionKey, QStringLiteral("prompt")).toString();
}

void BrowserProfileService::setPermissionDefaultPolicy(const QString &permissionKey, const QString &policy) {
  if (permissionDefaultPolicy(permissionKey) == policy) return;
  preferences_.setValue(QStringLiteral("permissions/") + permissionKey, policy);
  preferences_.sync();
  emit permissionsPolicyChanged();
}

bool BrowserProfileService::autoRevokeUnusedPermissions() const {
  return preferences_.value(QStringLiteral("privacy/autoRevokeUnusedPermissions"), true).toBool();
}

void BrowserProfileService::setAutoRevokeUnusedPermissions(bool enabled) {
  if (autoRevokeUnusedPermissions() == enabled) return;
  preferences_.setValue(QStringLiteral("privacy/autoRevokeUnusedPermissions"), enabled);
  preferences_.sync();
  emit permissionsPolicyChanged();
  if (enabled) {
    revokeUnusedPermissionsNow();
  }
}

void BrowserProfileService::revokeUnusedPermissionsNow() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
  if (!profile_) return;
  const auto permissions = sitePermissions();
  if (permissions.isEmpty()) return;
  const auto history = recentHistory();
  QSet<QString> activeHosts;
  const QDateTime threshold = QDateTime::currentDateTimeUtc().addDays(-30);
  for (const auto &entry : history) {
    if (entry.visitedAt >= threshold) {
      activeHosts.insert(entry.url.host().toLower());
    }
  }
  for (const auto &perm : permissions) {
    if (!perm.isValid()) continue;
    const QString host = perm.origin().host().toLower();
    if (!host.isEmpty() && !activeHosts.contains(host)) {
      perm.reset();
    }
  }
#endif
}

QString BrowserProfileService::cookiePolicy() const {
  return preferences_.value(QStringLiteral("content/cookiePolicy"), QStringLiteral("third_party")).toString();
}

void BrowserProfileService::setCookiePolicy(const QString &policy) {
  if (cookiePolicy() == policy) return;
  preferences_.setValue(QStringLiteral("content/cookiePolicy"), policy);
  preferences_.sync();
  if (profile_) {
    if (policy == QStringLiteral("block_all")) {
      profile_->setPersistentCookiesPolicy(QWebEngineProfile::NoPersistentCookies);
      clearCookies();
    } else {
      profile_->setPersistentCookiesPolicy(QWebEngineProfile::ForcePersistentCookies);
    }
  }
  emit contentSettingsChanged();
}

bool BrowserProfileService::isJavascriptEnabled() const {
  return preferences_.value(QStringLiteral("content/javascriptEnabled"), true).toBool();
}

void BrowserProfileService::setJavascriptEnabled(bool enabled) {
  if (isJavascriptEnabled() == enabled) return;
  preferences_.setValue(QStringLiteral("content/javascriptEnabled"), enabled);
  preferences_.sync();
  if (profile_ && profile_->settings()) {
    profile_->settings()->setAttribute(QWebEngineSettings::JavascriptEnabled, enabled);
  }
  emit contentSettingsChanged();
}

bool BrowserProfileService::isAutoLoadImagesEnabled() const {
  return preferences_.value(QStringLiteral("content/autoLoadImages"), true).toBool();
}

void BrowserProfileService::setAutoLoadImagesEnabled(bool enabled) {
  if (isAutoLoadImagesEnabled() == enabled) return;
  preferences_.setValue(QStringLiteral("content/autoLoadImages"), enabled);
  preferences_.sync();
  if (profile_ && profile_->settings()) {
    profile_->settings()->setAttribute(QWebEngineSettings::AutoLoadImages, enabled);
  }
  emit contentSettingsChanged();
}

bool BrowserProfileService::arePopupsAllowed() const {
  return preferences_.value(QStringLiteral("content/popupsAllowed"), false).toBool();
}

void BrowserProfileService::setPopupsAllowed(bool allowed) {
  if (arePopupsAllowed() == allowed) return;
  preferences_.setValue(QStringLiteral("content/popupsAllowed"), allowed);
  preferences_.sync();
  if (profile_ && profile_->settings()) {
    profile_->settings()->setAttribute(QWebEngineSettings::JavascriptCanOpenWindows, allowed);
    profile_->settings()->setAttribute(QWebEngineSettings::AllowWindowActivationFromJavaScript, allowed);
  }
  emit contentSettingsChanged();
}

bool BrowserProfileService::isSoundAllowed() const {
  return preferences_.value(QStringLiteral("content/soundAllowed"), true).toBool();
}

void BrowserProfileService::setSoundAllowed(bool allowed) {
  if (isSoundAllowed() == allowed) return;
  preferences_.setValue(QStringLiteral("content/soundAllowed"), allowed);
  preferences_.sync();
  emit contentSettingsChanged();
}

bool BrowserProfileService::openPdfInBrowser() const {
  return preferences_.value(QStringLiteral("content/openPdfInBrowser"), true).toBool();
}

void BrowserProfileService::setOpenPdfInBrowser(bool enabled) {
  if (openPdfInBrowser() == enabled) return;
  preferences_.setValue(QStringLiteral("content/openPdfInBrowser"), enabled);
  preferences_.sync();
  if (profile_ && profile_->settings()) {
    profile_->settings()->setAttribute(QWebEngineSettings::PdfViewerEnabled, enabled);
  }
  emit contentSettingsChanged();
}

bool BrowserProfileService::isProtectedContentEnabled() const {
  return preferences_.value(QStringLiteral("content/protectedContentEnabled"), true).toBool();
}

void BrowserProfileService::setProtectedContentEnabled(bool enabled) {
  preferences_.setValue(QStringLiteral("content/protectedContentEnabled"), enabled);
  preferences_.sync();
  emit contentSettingsChanged();
}

QString BrowserProfileService::insecureContentPolicy() const {
  return preferences_.value(QStringLiteral("content/insecureContentPolicy"), QStringLiteral("block")).toString();
}

void BrowserProfileService::setInsecureContentPolicy(const QString &policy) {
  preferences_.setValue(QStringLiteral("content/insecureContentPolicy"), policy);
  preferences_.sync();
  emit contentSettingsChanged();
}

QString BrowserProfileService::siteDataPolicy() const {
  return preferences_.value(QStringLiteral("content/siteDataPolicy"), QStringLiteral("allow")).toString();
}

void BrowserProfileService::setSiteDataPolicy(const QString &policy) {
  preferences_.setValue(QStringLiteral("content/siteDataPolicy"), policy);
  preferences_.sync();
  emit contentSettingsChanged();
}

bool BrowserProfileService::isJsOptimizationEnabled() const {
  return preferences_.value(QStringLiteral("content/jsOptimizationEnabled"), true).toBool();
}

void BrowserProfileService::setJsOptimizationEnabled(bool enabled) {
  preferences_.setValue(QStringLiteral("content/jsOptimizationEnabled"), enabled);
  preferences_.sync();
  emit contentSettingsChanged();
}

bool BrowserProfileService::isAutoFullscreenAllowed() const {
  return preferences_.value(QStringLiteral("content/autoFullscreenAllowed"), false).toBool();
}

void BrowserProfileService::setAutoFullscreenAllowed(bool allowed) {
  preferences_.setValue(QStringLiteral("content/autoFullscreenAllowed"), allowed);
  preferences_.sync();
  if (profile_ && profile_->settings()) {
    profile_->settings()->setAttribute(QWebEngineSettings::FullScreenSupportEnabled, allowed);
  }
  emit contentSettingsChanged();
}

bool BrowserProfileService::stripsTrackingParameters() const {
  return preferences_.value(QStringLiteral("privacy/stripTrackingParameters"), true).toBool();
}

void BrowserProfileService::setStripsTrackingParameters(bool enabled) {
  if (stripsTrackingParameters() == enabled) return;
  preferences_.setValue(QStringLiteral("privacy/stripTrackingParameters"), enabled);
  if (blockerService_) blockerService_->setTrackingParameterStrippingEnabled(enabled);
  preferences_.sync();
  emit trackingProtectionChanged();
}

bool BrowserProfileService::isAdultContentProtectionEnabled() const {
  return preferences_.value(QStringLiteral("privacy/adultContentProtection"), true).toBool();
}

void BrowserProfileService::setAdultContentProtectionEnabled(bool enabled) {
  if (isAdultContentProtectionEnabled() == enabled) return;
  preferences_.setValue(QStringLiteral("privacy/adultContentProtection"), enabled);
  preferences_.sync();
  emit adultContentProtectionChanged(enabled);
}

QString BrowserProfileService::configuredDownloadDirectory() const {
  return preferences_.value(QStringLiteral("downloads/directory")).toString();
}

void BrowserProfileService::setDownloadDirectory(const QString &directory) {
  const QFileInfo info(directory);
  if (!info.isDir()) return;
  preferences_.setValue(QStringLiteral("downloads/directory"), info.absoluteFilePath());
  preferences_.sync();
  if (profile_) profile_->setDownloadPath(info.absoluteFilePath());
}

bool BrowserProfileService::asksDownloadLocation() const {
  return preferences_.value(QStringLiteral("downloads/askLocation"), true).toBool();
}

void BrowserProfileService::setAsksDownloadLocation(bool enabled) {
  preferences_.setValue(QStringLiteral("downloads/askLocation"), enabled);
  preferences_.sync();
}

void BrowserProfileService::clearHttpCache() { if (profile_) profile_->clearHttpCache(); }

void BrowserProfileService::clearCookies() {
  cookies_.clear();
  if (profile_ && profile_->cookieStore()) profile_->cookieStore()->deleteAllCookies();
}

int BrowserProfileService::cookiesCountForHost(const QString &host) const {
  const QString cleanHost = host.trimmed().toLower();
  if (cleanHost.isEmpty()) return 0;
  int count = 0;
  for (const auto &c : cookies_) {
    QString d = c.domain().toLower();
    if (d.startsWith(QLatin1Char('.'))) d.remove(0, 1);
    if (d == cleanHost || cleanHost.endsWith(QLatin1Char('.') + d) || d.endsWith(QLatin1Char('.') + cleanHost)) {
      ++count;
    }
  }
  return count;
}

void BrowserProfileService::clearCookiesForHost(const QString &host) {
  const QString cleanHost = host.trimmed().toLower();
  if (cleanHost.isEmpty() || !profile_ || !profile_->cookieStore()) return;
  QList<QNetworkCookie> toDelete;
  for (int i = cookies_.size() - 1; i >= 0; --i) {
    const auto &c = cookies_.at(i);
    QString d = c.domain().toLower();
    if (d.startsWith(QLatin1Char('.'))) d.remove(0, 1);
    if (d == cleanHost || cleanHost.endsWith(QLatin1Char('.') + d) || d.endsWith(QLatin1Char('.') + cleanHost)) {
      toDelete.append(c);
      cookies_.removeAt(i);
    }
  }
  for (const auto &c : toDelete) {
    profile_->cookieStore()->deleteCookie(c);
  }
}

void BrowserProfileService::expireSiteDataForHost(const QString &host) {
  const QString cleanHost = DaliNiraBlockerSettings::normalizeSiteHost(host);
  if (cleanHost.isEmpty()) return;
  if (dalinira::engine::EngineBackend::selectedEngineType() == dalinira::engine::EngineType::WebKit) {
    if (auto *backend = dalinira::engine::EngineBackend::backendForType(dalinira::engine::EngineType::WebKit))
      backend->clearSiteData(cleanHost);
    return;
  }
  // Qt WebEngine has no origin-scoped website-data API. Delete the origin's
  // cookies natively; the document-start/pagehide listener clears its
  // script-visible storage while the renderer still owns that origin.
  if (!profile_ || forgetHosts_.size() >= 128) return;
  forgetHosts_.insert(cleanHost);
  clearCookiesForHost(cleanHost);
  if (profile_->cookieStore()) profile_->cookieStore()->loadAllCookies();
  QTimer::singleShot(5000, this, [this, cleanHost] { forgetHosts_.remove(cleanHost); });
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
QList<QWebEnginePermission> BrowserProfileService::sitePermissions() const {
  if (!profile_) return {};
  QList<QWebEnginePermission> result;
  QSet<QString> seenKeys;

  static const QStringList kAllPermissionKeys = {
    QStringLiteral("geolocation"),
    QStringLiteral("camera"),
    QStringLiteral("microphone"),
    QStringLiteral("cameraMicrophone"),
    QStringLiteral("notifications"),
    QStringLiteral("clipboard"),
    QStringLiteral("localFonts"),
    QStringLiteral("mouseLock"),
    QStringLiteral("screenShare")
  };

  for (const QString &key : kAllPermissionKeys) {
    const QWebEnginePermission::PermissionType pType = permissionTypeFromKey(key);
    if (pType == QWebEnginePermission::PermissionType::Unsupported) continue;

    const QStringList allowed = allowedOrigins(key);
    for (const QString &originStr : allowed) {
      const QString trimmed = originStr.trimmed();
      if (trimmed.isEmpty()) continue;
      const QUrl url(trimmed.startsWith(QStringLiteral("http")) ? trimmed : QStringLiteral("https://") + trimmed);
      if (!url.isValid()) continue;
      const QString uniqueKey = QStringLiteral("%1|%2").arg(url.toDisplayString(), QString::number(static_cast<int>(pType)));
      if (seenKeys.contains(uniqueKey)) continue;
      seenKeys.insert(uniqueKey);

      QWebEnginePermission perm = profile_->queryPermission(url, pType);
      if (perm.isValid()) {
        if (perm.state() != QWebEnginePermission::State::Granted) {
          perm.grant();
        }
        result.append(perm);
      }
    }

    const QStringList denied = deniedOrigins(key);
    for (const QString &originStr : denied) {
      const QString trimmed = originStr.trimmed();
      if (trimmed.isEmpty()) continue;
      const QUrl url(trimmed.startsWith(QStringLiteral("http")) ? trimmed : QStringLiteral("https://") + trimmed);
      if (!url.isValid()) continue;
      const QString uniqueKey = QStringLiteral("%1|%2").arg(url.toDisplayString(), QString::number(static_cast<int>(pType)));
      if (seenKeys.contains(uniqueKey)) continue;
      seenKeys.insert(uniqueKey);

      QWebEnginePermission perm = profile_->queryPermission(url, pType);
      if (perm.isValid()) {
        if (perm.state() != QWebEnginePermission::State::Denied) {
          perm.deny();
        }
        result.append(perm);
      }
    }
  }

  return result;
}

bool BrowserProfileService::resetSitePermission(const QUrl &origin, QWebEnginePermission::PermissionType type) {
  if (!origin.isValid()) return false;
  if (profile_) {
    const QWebEnginePermission permission = profile_->queryPermission(origin, type);
    if (permission.isValid()) {
      permission.reset();
    }
  }
  const QString permKey = permissionKeyFromType(type);
  if (!permKey.isEmpty()) {
    QStringList allowed = allowedOrigins(permKey);
    QStringList denied = deniedOrigins(permKey);
    const QString canon = canonicalOrigin(origin);
    const QString raw = origin.toDisplayString();
    const QString host = origin.host();
    bool changed = false;
    for (const QString &item : {canon, raw, host}) {
      if (!item.isEmpty()) {
        if (allowed.removeAll(item) > 0) changed = true;
        if (denied.removeAll(item) > 0) changed = true;
      }
    }
    if (changed) {
      preferences_.setValue(QStringLiteral("customPermissions/") + permKey + QStringLiteral("/allowed"), allowed);
      preferences_.setValue(QStringLiteral("customPermissions/") + permKey + QStringLiteral("/denied"), denied);
      preferences_.sync();
    }
  }
  emit permissionsPolicyChanged();
  return true;
}
#endif

void BrowserProfileService::recordHistory(const QUrl &url, const QString &title, bool isTyped, const QString &engine) {
  if (privateMode_ || (profile_ && profile_->isOffTheRecord())) return;
  const QUrl persistentUrl = BrowserSecurity::sanitizeUrlForPersistence(url);
  if (!persistentUrl.isValid() || (persistentUrl.scheme() != QLatin1String("http") && persistentUrl.scheme() != QLatin1String("https"))) return;
  if (dalinira::core::frequent_sites::isExcludedUrl(persistentUrl)) return;
  QJsonArray values;
  const QJsonDocument existing = QJsonDocument::fromJson(preferences_.value(QStringLiteral("history/entries")).toByteArray());
  if (existing.isArray()) values = existing.array();
  const QString normalized = persistentUrl.adjusted(QUrl::RemoveFragment).toString(QUrl::FullyEncoded);
  const QString cleanTitle = title.trimmed().left(180);
  const QString recordTitle = (cleanTitle.isEmpty() || I18n::isTranslationOf(QStringLiteral("newtab.title"), cleanTitle))
      ? persistentUrl.host() : cleanTitle;
  int typedCount = 0;
  for (const QJsonValue &value : values) {
    const QJsonObject item = value.toObject();
    if (item.value(QStringLiteral("url")).toString() == normalized) {
      typedCount = std::max(0, item.value(QStringLiteral("typedCount")).toInt());
      break;
    }
  }
  if (isTyped) ++typedCount;
  QJsonArray next;
  QJsonObject current{{QStringLiteral("url"), normalized}, {QStringLiteral("title"), recordTitle},
                      {QStringLiteral("visitedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                      {QStringLiteral("isTyped"), isTyped},
                      {QStringLiteral("typedCount"), typedCount}};
  next.append(current);
  for (const QJsonValue &value : values) {
    const QJsonObject item = value.toObject();
    if (item.value(QStringLiteral("url")).toString() == normalized) continue;
    next.append(item);
    if (next.size() >= 300) break;
  }
  preferences_.setValue(QStringLiteral("history/entries"), QJsonDocument(next).toJson(QJsonDocument::Compact));

  const QString scope = normalizeEngineScope(engine);
  const QString scopeKey = frequentSitesStorageKey(scope);
  QHash<QString, QJsonObject> sites;
  QJsonDocument frequentDocument = QJsonDocument::fromJson(preferences_.value(scopeKey).toByteArray());
  if (!frequentDocument.isArray() && scope == QLatin1String("chromium")) {
    frequentDocument = QJsonDocument::fromJson(preferences_.value(QStringLiteral("history/frequentSites")).toByteArray());
  }
  if (frequentDocument.isArray()) {
    for (const QJsonValue &value : frequentDocument.array()) {
      const QJsonObject site = value.toObject();
      const QString key = site.value(QStringLiteral("key")).toString();
      if (!key.isEmpty()) sites.insert(key, site);
    }
  } else if (scope == QLatin1String("chromium")) {
    // Seed the new counter from history created by older builds for chromium only
    for (const QJsonValue &value : values) {
      const QJsonObject historyItem = value.toObject();
      const QUrl historyUrl(historyItem.value(QStringLiteral("url")).toString());
      if (dalinira::core::frequent_sites::isExcludedUrl(historyUrl)) continue;
      const QString key = frequentSiteKey(historyUrl);
      if (key.isEmpty()) continue;
      QJsonObject site = sites.value(key);
      site.insert(QStringLiteral("key"), key);
      site.insert(QStringLiteral("url"), frequentSiteRootUrl(historyUrl).toString(QUrl::FullyEncoded));
      site.insert(QStringLiteral("iconLookupUrl"), historyUrl.toString(QUrl::FullyEncoded));
      site.insert(QStringLiteral("title"), historyItem.value(QStringLiteral("title")).toString().left(180));
      site.insert(QStringLiteral("visitCount"), site.value(QStringLiteral("visitCount")).toInt() + 1);
      site.insert(QStringLiteral("lastVisitedAt"), historyItem.value(QStringLiteral("visitedAt")).toString());
      if (historyItem.value(QStringLiteral("isTyped")).toBool()) {
        site.insert(QStringLiteral("typedCount"), site.value(QStringLiteral("typedCount")).toInt() + 1);
      }
      sites.insert(key, site);
    }
  }
  const QString siteKey = frequentSiteKey(persistentUrl);
  if (!siteKey.isEmpty()) {
    QJsonObject site = sites.value(siteKey);
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const QDateTime lastVisited = QDateTime::fromString(site.value(QStringLiteral("lastVisitedAt")).toString(), Qt::ISODate);
    int rapidReloads = site.value(QStringLiteral("rapidReloads")).toInt();
    if (lastVisited.isValid() && !isTyped) {
      const qint64 secsSinceLast = lastVisited.secsTo(now);
      if (secsSinceLast >= 0 && secsSinceLast < 10) {
        ++rapidReloads;
      }
    }
    site.insert(QStringLiteral("key"), siteKey);
    site.insert(QStringLiteral("url"), frequentSiteRootUrl(persistentUrl).toString(QUrl::FullyEncoded));
    site.insert(QStringLiteral("iconLookupUrl"), normalized);
    const QString existingTitle = site.value(QStringLiteral("title")).toString();
    const bool isAuthOrError = recordTitle.contains(QLatin1String("login"), Qt::CaseInsensitive)
        || recordTitle.contains(QLatin1String("sign in"), Qt::CaseInsensitive)
        || recordTitle.contains(QLatin1String("404"), Qt::CaseInsensitive)
        || recordTitle.contains(QLatin1String("error"), Qt::CaseInsensitive);
    if (existingTitle.isEmpty() || (!isAuthOrError && (existingTitle == persistentUrl.host() || existingTitle.isEmpty()))) {
      site.insert(QStringLiteral("title"), recordTitle);
    }
    site.insert(QStringLiteral("visitCount"), site.value(QStringLiteral("visitCount")).toInt() + 1);
    site.insert(QStringLiteral("rapidReloads"), rapidReloads);
    site.insert(QStringLiteral("lastVisitedAt"), now.toString(Qt::ISODate));
    if (isTyped) {
      site.insert(QStringLiteral("typedCount"), site.value(QStringLiteral("typedCount")).toInt() + 1);
    }
    sites.insert(siteKey, site);
  }
  QList<QJsonObject> orderedSites = sites.values();
  const QDateTime now = QDateTime::currentDateTimeUtc();
  std::sort(orderedSites.begin(), orderedSites.end(), [&](const QJsonObject &left, const QJsonObject &right) {
    const double leftScore = dalinira::core::frequent_sites::calculateScore(
        left.value(QStringLiteral("visitCount")).toInt(),
        left.value(QStringLiteral("typedCount")).toInt(),
        QDateTime::fromString(left.value(QStringLiteral("lastVisitedAt")).toString(), Qt::ISODate),
        left.value(QStringLiteral("rapidReloads")).toInt(),
        now);
    const double rightScore = dalinira::core::frequent_sites::calculateScore(
        right.value(QStringLiteral("visitCount")).toInt(),
        right.value(QStringLiteral("typedCount")).toInt(),
        QDateTime::fromString(right.value(QStringLiteral("lastVisitedAt")).toString(), Qt::ISODate),
        right.value(QStringLiteral("rapidReloads")).toInt(),
        now);
    if (std::abs(leftScore - rightScore) > 0.001) return leftScore > rightScore;
    return left.value(QStringLiteral("lastVisitedAt")).toString() > right.value(QStringLiteral("lastVisitedAt")).toString();
  });
  QJsonArray frequentValues;
  const qsizetype maxCount = std::min<qsizetype>(100, orderedSites.size());
  for (qsizetype i = 0; i < maxCount; ++i) frequentValues.append(orderedSites.at(i));
  preferences_.setValue(scopeKey, QJsonDocument(frequentValues).toJson(QJsonDocument::Compact));
  if (scope == QLatin1String("chromium")) {
    preferences_.setValue(QStringLiteral("history/frequentSites"), QJsonDocument(frequentValues).toJson(QJsonDocument::Compact));
  }
  preferences_.sync();
  emit historyChanged();
}

void BrowserProfileService::importHistoryEntries(const QList<HistoryImportEntry> &entries,
                                                 const QString &engine) {
  if (privateMode_ || (profile_ && profile_->isOffTheRecord()) || entries.isEmpty()) return;

  QHash<QString, QJsonObject> historyByUrl;
  const QJsonDocument existingHistory = QJsonDocument::fromJson(
      preferences_.value(QStringLiteral("history/entries")).toByteArray());
  if (existingHistory.isArray()) {
    for (const QJsonValue &value : existingHistory.array()) {
      const QJsonObject item = value.toObject();
      const QString url = item.value(QStringLiteral("url")).toString();
      if (!url.isEmpty()) historyByUrl.insert(url, item);
    }
  }

  const QString scope = normalizeEngineScope(engine);
  const QString scopeKey = frequentSitesStorageKey(scope);
  QHash<QString, QJsonObject> sites;
  const QJsonDocument existingSites = QJsonDocument::fromJson(preferences_.value(scopeKey).toByteArray());
  if (existingSites.isArray()) {
    for (const QJsonValue &value : existingSites.array()) {
      const QJsonObject item = value.toObject();
      const QString key = item.value(QStringLiteral("key")).toString();
      if (!key.isEmpty()) sites.insert(key, item);
    }
  }

  for (const auto &entry : entries) {
    const QUrl url = BrowserSecurity::sanitizeUrlForPersistence(entry.url);
    if (!url.isValid() || (url.scheme() != QLatin1String("http") &&
                           url.scheme() != QLatin1String("https")) ||
        dalinira::core::frequent_sites::isExcludedUrl(url)) {
      continue;
    }

    const QString normalized = url.adjusted(QUrl::RemoveFragment).toString(QUrl::FullyEncoded);
    const QDateTime visitedAt = entry.visitedAt.isValid()
        ? entry.visitedAt.toUTC() : QDateTime::currentDateTimeUtc();
    const QString title = entry.title.trimmed().left(180).isEmpty()
        ? url.host() : entry.title.trimmed().left(180);
    const QJsonObject previous = historyByUrl.value(normalized);
    const QDateTime previousVisit = QDateTime::fromString(
        previous.value(QStringLiteral("visitedAt")).toString(), Qt::ISODate);
    if (!previousVisit.isValid() || visitedAt >= previousVisit) {
      historyByUrl.insert(normalized, QJsonObject{
          {QStringLiteral("url"), normalized},
          {QStringLiteral("title"), title},
          {QStringLiteral("visitedAt"), visitedAt.toString(Qt::ISODate)},
          {QStringLiteral("isTyped"), entry.typedCount > 0},
          {QStringLiteral("typedCount"), std::max(0, entry.typedCount)}});
    }

    const QString key = frequentSiteKey(url);
    if (key.isEmpty()) continue;
    QJsonObject site = sites.value(key);
    const QDateTime previousSiteVisit = QDateTime::fromString(
        site.value(QStringLiteral("lastVisitedAt")).toString(), Qt::ISODate);
    site.insert(QStringLiteral("key"), key);
    site.insert(QStringLiteral("url"), frequentSiteRootUrl(url).toString(QUrl::FullyEncoded));
    site.insert(QStringLiteral("iconLookupUrl"), normalized);
    site.insert(QStringLiteral("title"), title);
    site.insert(QStringLiteral("visitCount"),
                std::max(site.value(QStringLiteral("visitCount")).toInt(),
                         std::max(1, entry.visitCount)));
    site.insert(QStringLiteral("typedCount"),
                std::max(site.value(QStringLiteral("typedCount")).toInt(),
                         std::max(0, entry.typedCount)));
    if (!previousSiteVisit.isValid() || visitedAt >= previousSiteVisit) {
      site.insert(QStringLiteral("lastVisitedAt"), visitedAt.toString(Qt::ISODate));
    }
    sites.insert(key, site);
  }

  QList<QJsonObject> orderedHistory = historyByUrl.values();
  std::sort(orderedHistory.begin(), orderedHistory.end(), [](const QJsonObject &left,
                                                             const QJsonObject &right) {
    return left.value(QStringLiteral("visitedAt")).toString() >
           right.value(QStringLiteral("visitedAt")).toString();
  });
  QJsonArray historyValues;
  const qsizetype historyCount = std::min<qsizetype>(300, orderedHistory.size());
  for (qsizetype i = 0; i < historyCount; ++i) historyValues.append(orderedHistory.at(i));

  QList<QJsonObject> orderedSites = sites.values();
  const QDateTime now = QDateTime::currentDateTimeUtc();
  std::sort(orderedSites.begin(), orderedSites.end(), [&](const QJsonObject &left,
                                                          const QJsonObject &right) {
    return dalinira::core::frequent_sites::calculateScore(
               left.value(QStringLiteral("visitCount")).toInt(),
               left.value(QStringLiteral("typedCount")).toInt(),
               QDateTime::fromString(left.value(QStringLiteral("lastVisitedAt")).toString(), Qt::ISODate),
               left.value(QStringLiteral("rapidReloads")).toInt(), now) >
           dalinira::core::frequent_sites::calculateScore(
               right.value(QStringLiteral("visitCount")).toInt(),
               right.value(QStringLiteral("typedCount")).toInt(),
               QDateTime::fromString(right.value(QStringLiteral("lastVisitedAt")).toString(), Qt::ISODate),
               right.value(QStringLiteral("rapidReloads")).toInt(), now);
  });
  QJsonArray frequentValues;
  const qsizetype siteCount = std::min<qsizetype>(100, orderedSites.size());
  for (qsizetype i = 0; i < siteCount; ++i) frequentValues.append(orderedSites.at(i));

  preferences_.setValue(QStringLiteral("history/entries"),
                        QJsonDocument(historyValues).toJson(QJsonDocument::Compact));
  preferences_.setValue(scopeKey, QJsonDocument(frequentValues).toJson(QJsonDocument::Compact));
  if (scope == QLatin1String("chromium")) {
    preferences_.setValue(QStringLiteral("history/frequentSites"),
                          QJsonDocument(frequentValues).toJson(QJsonDocument::Compact));
  }
  preferences_.sync();
  emit historyChanged();
}

bool BrowserProfileService::updateHistoryTitle(const QUrl &url, const QString &title) {
  const QUrl persistentUrl = BrowserSecurity::sanitizeUrlForPersistence(url);
  if (!persistentUrl.isValid() || (persistentUrl.scheme() != QLatin1String("http") && persistentUrl.scheme() != QLatin1String("https"))) return false;

  const QString cleanTitle = title.trimmed().left(180);
  if (cleanTitle.isEmpty() || I18n::isTranslationOf(QStringLiteral("newtab.title"), cleanTitle)
      || cleanTitle == persistentUrl.toString()) return false;

  const QString normalized = persistentUrl.adjusted(QUrl::RemoveFragment).toString(QUrl::FullyEncoded);
  bool changed = false;

  // 1. Update title in history/entries in-place (do not modify visitedAt or visit count)
  const QJsonDocument existing = QJsonDocument::fromJson(preferences_.value(QStringLiteral("history/entries")).toByteArray());
  if (existing.isArray()) {
    QJsonArray next;
    for (const QJsonValue &value : existing.array()) {
      QJsonObject item = value.toObject();
      if (item.value(QStringLiteral("url")).toString() == normalized) {
        if (item.value(QStringLiteral("title")).toString() != cleanTitle) {
          item.insert(QStringLiteral("title"), cleanTitle);
          changed = true;
        }
      }
      next.append(item);
    }
    if (changed) {
      preferences_.setValue(QStringLiteral("history/entries"), QJsonDocument(next).toJson(QJsonDocument::Compact));
    }
  }

  // 2. Update title in frequent sites across all engine scopes
  const QString siteKey = frequentSiteKey(persistentUrl);
  if (!siteKey.isEmpty()) {
    const QStringList keys = {QStringLiteral("history/frequentSites"),
                              QStringLiteral("newtab/chromium/frequentSites"),
                              QStringLiteral("newtab/webkit/frequentSites")};
    for (const QString &k : keys) {
      const QJsonDocument frequentDoc = QJsonDocument::fromJson(preferences_.value(k).toByteArray());
      if (frequentDoc.isArray()) {
        QJsonArray nextFrequent;
        bool frequentUpdated = false;
        for (const QJsonValue &val : frequentDoc.array()) {
          QJsonObject site = val.toObject();
          if (site.value(QStringLiteral("key")).toString() == siteKey) {
            if (site.value(QStringLiteral("title")).toString() != cleanTitle) {
              site.insert(QStringLiteral("title"), cleanTitle);
              frequentUpdated = true;
              changed = true;
            }
          }
          nextFrequent.append(site);
        }
        if (frequentUpdated) {
          preferences_.setValue(k, QJsonDocument(nextFrequent).toJson(QJsonDocument::Compact));
        }
      }
    }
  }

  if (changed) {
    preferences_.sync();
    emit historyChanged();
    return true;
  }
  return false;
}

QList<BrowserHistoryEntry> BrowserProfileService::recentHistory() const {
  QList<BrowserHistoryEntry> entries;
  const QJsonDocument document = QJsonDocument::fromJson(preferences_.value(QStringLiteral("history/entries")).toByteArray());
  if (!document.isArray()) return entries;
  for (const QJsonValue &value : document.array()) {
    const QJsonObject item = value.toObject();
    const QUrl url = BrowserSecurity::sanitizeUrlForPersistence(QUrl(item.value(QStringLiteral("url")).toString()));
    if (!url.isValid()) continue;
    entries.append({item.value(QStringLiteral("title")).toString(), url,
                    QDateTime::fromString(item.value(QStringLiteral("visitedAt")).toString(), Qt::ISODate)});
  }
  return entries;
}

QList<BrowserFrequentSite> BrowserProfileService::frequentSites(int limit) const {
  return frequentSites(limit, QString{});
}

QList<BrowserFrequentSite> BrowserProfileService::frequentSites(int limit, const QString &engine) const {
  QList<BrowserFrequentSite> sites;
  if (limit <= 0) return sites;
  const QString scope = normalizeEngineScope(engine);
  const QString scopeKey = frequentSitesStorageKey(scope);
  QJsonDocument document = QJsonDocument::fromJson(preferences_.value(scopeKey).toByteArray());
  if (!document.isArray() && scope == QLatin1String("chromium")) {
    document = QJsonDocument::fromJson(preferences_.value(QStringLiteral("history/frequentSites")).toByteArray());
  }
  if (document.isArray()) {
    for (const QJsonValue &value : document.array()) {
      const QJsonObject item = value.toObject();
      const QUrl url = BrowserSecurity::sanitizeUrlForPersistence(QUrl(item.value(QStringLiteral("url")).toString()));
      const QUrl iconLookupUrl = BrowserSecurity::sanitizeUrlForPersistence(QUrl(item.value(QStringLiteral("iconLookupUrl")).toString()));
      const int visitCount = item.value(QStringLiteral("visitCount")).toInt();
      const int typedCount = item.value(QStringLiteral("typedCount")).toInt();
      if (!url.isValid() || visitCount <= 0) continue;
      sites.append({item.value(QStringLiteral("title")).toString(), url,
                    iconLookupUrl.isValid() ? iconLookupUrl : url, visitCount, typedCount,
                    QDateTime::fromString(item.value(QStringLiteral("lastVisitedAt")).toString(), Qt::ISODate)});
    }
  } else if (scope == QLatin1String("chromium")) {
    QHash<QString, BrowserFrequentSite> migrated;
    for (const BrowserHistoryEntry &entry : recentHistory()) {
      const QString key = frequentSiteKey(entry.url);
      if (key.isEmpty()) continue;
      BrowserFrequentSite site = migrated.value(key);
      site.title = entry.title;
      site.url = frequentSiteRootUrl(entry.url);
      site.iconLookupUrl = entry.url;
      ++site.visitCount;
      if (!site.lastVisitedAt.isValid() || entry.visitedAt > site.lastVisitedAt) site.lastVisitedAt = entry.visitedAt;
      migrated.insert(key, site);
    }
    sites = migrated.values();
  }
  const QDateTime now = QDateTime::currentDateTimeUtc();
  std::sort(sites.begin(), sites.end(), [&](const BrowserFrequentSite &left, const BrowserFrequentSite &right) {
    const double leftScore = dalinira::core::frequent_sites::calculateScore(
        left.visitCount, left.typedCount, left.lastVisitedAt, 0, now);
    const double rightScore = dalinira::core::frequent_sites::calculateScore(
        right.visitCount, right.typedCount, right.lastVisitedAt, 0, now);
    if (std::abs(leftScore - rightScore) > 0.001) return leftScore > rightScore;
    return left.lastVisitedAt > right.lastVisitedAt;
  });
  if (sites.isEmpty() || limit <= 0) return {};
  const qsizetype count = std::min<qsizetype>(limit, sites.size());
  return sites.mid(0, count);
}

QStringList BrowserProfileService::hiddenFrequentSites() const {
  return hiddenFrequentSites(QString{});
}

QStringList BrowserProfileService::hiddenFrequentSites(const QString &engine) const {
  const QString scope = normalizeEngineScope(engine);
  const QString scopeKey = hiddenFrequentSitesStorageKey(scope);
  if (preferences_.contains(scopeKey)) {
    return preferences_.value(scopeKey).toStringList();
  }
  if (scope == QLatin1String("chromium")) {
    if (preferences_.contains(QStringLiteral("newtab/hiddenFrequentSites"))) {
      return preferences_.value(QStringLiteral("newtab/hiddenFrequentSites")).toStringList();
    }
    return dalinira::ui_profile::getVisualValue(QStringLiteral("browser/hiddenFrequentSites")).toStringList();
  }
  return dalinira::ui_profile::getVisualValue(QStringLiteral("browser/%1/hiddenFrequentSites").arg(scope)).toStringList();
}

bool BrowserProfileService::hideFrequentSite(const QUrl &url) {
  return hideFrequentSite(url, QString{});
}

bool BrowserProfileService::hideFrequentSite(const QUrl &url, const QString &engine) {
  if (privateMode_ || (profile_ && profile_->isOffTheRecord())) return false;
  QUrl normalizedUrl = url;
  if (normalizedUrl.scheme().isEmpty() && !normalizedUrl.path().isEmpty()) {
    normalizedUrl = QUrl::fromUserInput(url.toString());
  }
  const QString key = dalinira::core::frequent_sites::frequentSiteKey(normalizedUrl);
  if (key.isEmpty()) return false;
  const QString scope = normalizeEngineScope(engine);
  const QString scopeKey = hiddenFrequentSitesStorageKey(scope);
  QStringList hidden = hiddenFrequentSites(scope);
  bool found = false;
  for (const QString &item : hidden) {
    if (item.compare(key, Qt::CaseInsensitive) == 0) {
      found = true;
      break;
    }
  }
  if (!found) {
    hidden.append(key);
    preferences_.setValue(scopeKey, hidden);
    dalinira::ui_profile::setVisualValue(QStringLiteral("browser/%1/hiddenFrequentSites").arg(scope), hidden);
    if (scope == QLatin1String("chromium")) {
      preferences_.setValue(QStringLiteral("newtab/hiddenFrequentSites"), hidden);
      dalinira::ui_profile::setVisualValue(QStringLiteral("browser/hiddenFrequentSites"), hidden);
    }
    preferences_.sync();
    emit historyChanged();
    return true;
  }
  return false;
}

bool BrowserProfileService::restoreHiddenFrequentSites() {
  return restoreHiddenFrequentSites(QString{});
}

bool BrowserProfileService::restoreHiddenFrequentSites(const QString &engine) {
  if (privateMode_ || (profile_ && profile_->isOffTheRecord())) return false;
  const QString scope = normalizeEngineScope(engine);
  const QString scopeKey = hiddenFrequentSitesStorageKey(scope);
  preferences_.remove(scopeKey);
  dalinira::ui_profile::removeVisualValue(QStringLiteral("browser/%1/hiddenFrequentSites").arg(scope));
  if (scope == QLatin1String("chromium")) {
    preferences_.remove(QStringLiteral("newtab/hiddenFrequentSites"));
    dalinira::ui_profile::removeVisualValue(QStringLiteral("browser/hiddenFrequentSites"));
  }
  preferences_.sync();
  emit historyChanged();
  return true;
}

bool BrowserProfileService::isFrequentSiteHidden(const QUrl &url, const QString &engine) const {
  QUrl normalizedUrl = url;
  if (normalizedUrl.scheme().isEmpty() && !normalizedUrl.path().isEmpty()) {
    normalizedUrl = QUrl::fromUserInput(url.toString());
  }
  const QString key = dalinira::core::frequent_sites::frequentSiteKey(normalizedUrl);
  if (key.isEmpty()) return false;
  for (const QString &item : hiddenFrequentSites(engine)) {
    if (item.compare(key, Qt::CaseInsensitive) == 0) return true;
    const QString itemKey = dalinira::core::frequent_sites::frequentSiteKey(QUrl::fromUserInput(item));
    if (!itemKey.isEmpty() && itemKey.compare(key, Qt::CaseInsensitive) == 0) return true;
  }
  return false;
}

QList<BrowserProfileService::QuickAccessItem> BrowserProfileService::quickAccessItems(const QString &engine) const {
  const QString scope = normalizeEngineScope(engine);
  const QString scopeKey = quickAccessStorageKey(scope);
  QList<QuickAccessItem> items;
  if (preferences_.contains(scopeKey)) {
    const QJsonDocument doc = QJsonDocument::fromJson(preferences_.value(scopeKey).toByteArray());
    if (doc.isArray()) {
      for (const QJsonValue &val : doc.array()) {
        const QJsonObject obj = val.toObject();
        const QUrl u = BrowserSecurity::sanitizeUrlForPersistence(QUrl(obj.value(QStringLiteral("url")).toString()));
        if (u.isValid()) {
          QString t = obj.value(QStringLiteral("title")).toString();
          if (t.isEmpty()) t = u.host();
          items.append({u, t});
        }
      }
    }
    return items;
  }
  if (scope == QLatin1String("chromium")) {
    for (const auto &bm : bookmarkItems()) {
      if (bm.folder.isEmpty()) {
        items.append({bm.url, bm.title.isEmpty() ? bm.url.host() : bm.title});
      }
    }
    if (items.isEmpty()) {
      for (const QUrl &u : bookmarks()) {
        items.append({u, u.host()});
      }
    }
    return items;
  }
  return {};
}

QList<QUrl> BrowserProfileService::quickAccess(const QString &engine) const {
  QList<QUrl> urls;
  for (const auto &item : quickAccessItems(engine)) {
    if (!urls.contains(item.url)) urls.append(item.url);
  }
  return urls;
}

bool BrowserProfileService::isQuickAccess(const QUrl &url, const QString &engine) const {
  const QString scope = normalizeEngineScope(engine);
  const QString normalized = url.adjusted(QUrl::RemoveFragment).toString(QUrl::FullyEncoded);
  for (const auto &item : quickAccessItems(scope)) {
    if (item.url.adjusted(QUrl::RemoveFragment).toString(QUrl::FullyEncoded) == normalized) return true;
  }
  return false;
}

bool BrowserProfileService::toggleQuickAccess(const QUrl &url, const QString &title, const QString &engine) {
  const QString scope = normalizeEngineScope(engine);
  if (isQuickAccess(url, scope)) {
    return removeQuickAccess(url, scope);
  }
  return addQuickAccess(url, title, scope);
}

bool BrowserProfileService::moveQuickAccess(const QUrl &url, int toIndex, const QString &engine) {
  if (privateMode_ || (profile_ && profile_->isOffTheRecord())) return false;
  const QUrl safe = BrowserSecurity::sanitizeUrlForPersistence(url);
  if (!safe.isValid()) return false;
  const QString scope = normalizeEngineScope(engine);
  QJsonArray list;
  for (const auto &item : quickAccessItems(scope)) {
    list.append(QJsonObject{{QStringLiteral("url"), item.url.toString(QUrl::FullyEncoded)}, {QStringLiteral("title"), item.title}});
  }
  const QString encoded = safe.toString(QUrl::FullyEncoded);
  int from = -1;
  for (int i = 0; i < list.size(); ++i) {
    if (list.at(i).toObject().value(QStringLiteral("url")).toString() == encoded) {
      from = i;
      break;
    }
  }
  if (from < 0) return false;
  const int to = std::clamp(toIndex, 0, int(list.size()) - 1);
  if (to == from) return false;
  const QJsonValue moved = list.takeAt(from);
  list.insert(to, moved);
  preferences_.setValue(quickAccessStorageKey(scope), QJsonDocument(list).toJson(QJsonDocument::Compact));
  preferences_.sync();
  emit bookmarksChanged();
  emit historyChanged();
  return true;
}

bool BrowserProfileService::addQuickAccess(const QUrl &url, const QString &title, const QString &engine) {
  if (privateMode_ || (profile_ && profile_->isOffTheRecord())) return false;
  const QUrl safe = BrowserSecurity::sanitizeUrlForPersistence(url);
  if (!safe.isValid()) return false;
  const QString scope = normalizeEngineScope(engine);
  const QString scopeKey = quickAccessStorageKey(scope);
  QJsonArray list;
  if (preferences_.contains(scopeKey)) {
    list = QJsonDocument::fromJson(preferences_.value(scopeKey).toByteArray()).array();
  } else if (scope == QLatin1String("chromium")) {
    for (const auto &b : quickAccessItems(scope)) {
      list.append(QJsonObject{{QStringLiteral("url"), b.url.toString(QUrl::FullyEncoded)}, {QStringLiteral("title"), b.title}});
    }
  }
  const QString encoded = safe.toString(QUrl::FullyEncoded);
  for (const QJsonValue &v : list) {
    if (v.toObject().value(QStringLiteral("url")).toString() == encoded) return false;
  }
  QString t = title.trimmed();
  if (t.isEmpty()) t = safe.host();
  list.append(QJsonObject{{QStringLiteral("url"), encoded}, {QStringLiteral("title"), t}});
  preferences_.setValue(scopeKey, QJsonDocument(list).toJson(QJsonDocument::Compact));
  preferences_.sync();
  emit bookmarksChanged();
  emit historyChanged();
  return true;
}

bool BrowserProfileService::removeQuickAccess(const QUrl &url, const QString &engine) {
  if (privateMode_ || (profile_ && profile_->isOffTheRecord())) return false;
  const QUrl safe = BrowserSecurity::sanitizeUrlForPersistence(url);
  if (!safe.isValid()) return false;
  const QString scope = normalizeEngineScope(engine);
  const QString scopeKey = quickAccessStorageKey(scope);
  QJsonArray list;
  if (preferences_.contains(scopeKey)) {
    list = QJsonDocument::fromJson(preferences_.value(scopeKey).toByteArray()).array();
  } else if (scope == QLatin1String("chromium")) {
    for (const auto &b : quickAccessItems(scope)) {
      if (b.url != safe) {
        list.append(QJsonObject{{QStringLiteral("url"), b.url.toString(QUrl::FullyEncoded)}, {QStringLiteral("title"), b.title}});
      }
    }
    preferences_.setValue(scopeKey, QJsonDocument(list).toJson(QJsonDocument::Compact));
    preferences_.sync();
    emit bookmarksChanged();
    emit historyChanged();
    return true;
  }
  const QString encoded = safe.toString(QUrl::FullyEncoded);
  QJsonArray updated;
  bool removed = false;
  for (const QJsonValue &v : list) {
    if (v.toObject().value(QStringLiteral("url")).toString() == encoded) {
      removed = true;
      continue;
    }
    updated.append(v);
  }
  if (removed) {
    preferences_.setValue(scopeKey, QJsonDocument(updated).toJson(QJsonDocument::Compact));
    preferences_.sync();
    emit bookmarksChanged();
    emit historyChanged();
    return true;
  }
  return false;
}

void BrowserProfileService::recordSearch(const QString &query) {
  const QString cleanQuery = query.simplified().left(256);
  if (cleanQuery.isEmpty() || (profile_ && profile_->isOffTheRecord())) return;

  QJsonArray existingValues;
  const QJsonDocument existing = QJsonDocument::fromJson(
      preferences_.value(QStringLiteral("search/history")).toByteArray());
  if (existing.isArray()) existingValues = existing.array();

  int useCount = 1;
  for (const QJsonValue &value : existingValues) {
    const QJsonObject item = value.toObject();
    if (item.value(QStringLiteral("query")).toString().compare(cleanQuery, Qt::CaseInsensitive) == 0) {
      useCount = std::max(0, item.value(QStringLiteral("useCount")).toInt()) + 1;
      break;
    }
  }

  QJsonArray next;
  next.append(QJsonObject{
      {QStringLiteral("query"), cleanQuery},
      {QStringLiteral("searchedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
      {QStringLiteral("useCount"), useCount}});
  for (const QJsonValue &value : existingValues) {
    const QJsonObject item = value.toObject();
    const QString storedQuery = item.value(QStringLiteral("query")).toString().simplified().left(256);
    if (storedQuery.isEmpty() || storedQuery.compare(cleanQuery, Qt::CaseInsensitive) == 0) continue;
    next.append(item);
    if (next.size() >= 100) break;
  }
  preferences_.setValue(QStringLiteral("search/history"),
                        QJsonDocument(next).toJson(QJsonDocument::Compact));
  preferences_.sync();
  emit historyChanged();
}

QStringList BrowserProfileService::recentSearches(const QString &query, int limit) const {
  QStringList result;
  if (limit <= 0 || (profile_ && profile_->isOffTheRecord())) return result;
  const QString needle = query.simplified();
  const QJsonDocument document = QJsonDocument::fromJson(
      preferences_.value(QStringLiteral("search/history")).toByteArray());
  if (!document.isArray()) return result;
  for (const QJsonValue &value : document.array()) {
    const QString storedQuery = value.toObject().value(QStringLiteral("query")).toString().simplified().left(256);
    if (storedQuery.isEmpty() || (!needle.isEmpty() && !storedQuery.contains(needle, Qt::CaseInsensitive))) continue;
    result.append(storedQuery);
    if (result.size() >= limit) break;
  }
  return result;
}

bool BrowserProfileService::removeSearchHistory(const QString &query) {
  // The WebKit backend has no QWebEngineProfile; its regular profile is never off the record.
  if (profile_ && profile_->isOffTheRecord()) return false;
  const QString cleanQuery = query.simplified().left(256);
  if (cleanQuery.isEmpty()) return false;
  const QJsonDocument document = QJsonDocument::fromJson(
      preferences_.value(QStringLiteral("search/history")).toByteArray());
  if (!document.isArray()) return false;
  QJsonArray remaining;
  bool removed = false;
  for (const QJsonValue &value : document.array()) {
    const QString storedQuery = value.toObject().value(QStringLiteral("query")).toString().simplified();
    if (storedQuery.compare(cleanQuery, Qt::CaseInsensitive) == 0) {
      removed = true;
      continue;
    }
    remaining.append(value);
  }
  if (!removed) return false;
  preferences_.setValue(QStringLiteral("search/history"),
                        QJsonDocument(remaining).toJson(QJsonDocument::Compact));
  preferences_.sync();
  emit historyChanged();
  return true;
}

void BrowserProfileService::clearSearchHistory() {
  preferences_.remove(QStringLiteral("search/history"));
  preferences_.sync();
  emit historyChanged();
}

void BrowserProfileService::clearHistory() {
  preferences_.remove(QStringLiteral("history/entries"));
  preferences_.remove(QStringLiteral("history/frequentSites"));
  preferences_.remove(QStringLiteral("search/history"));
  preferences_.sync();
  emit historyChanged();
}

QList<BrowserDownloadEntry> BrowserProfileService::recentDownloads() const {
  QList<BrowserDownloadEntry> result = downloads_;
  if (generalDownloadManager_) {
    for (const GeneralDownloadJob &job : generalDownloadManager_->jobs())
      result.prepend({job.fileName, job.targetPath, GeneralDownloadManager::stateText(job.state)});
  }
  return result;
}

int BrowserProfileService::recentDownloadCount() const {
  return static_cast<int>(recentDownloads().size());
}

QList<BrowserDownloadEntry> BrowserProfileService::nativeDownloads() const { return downloads_; }

void BrowserProfileService::rememberClosedTab(const QUrl &url, const QString &title) {
  if (!url.isValid()) return;
  closedTabs_.prepend({title.left(180), url, QDateTime::currentDateTimeUtc()});
  while (closedTabs_.size() > 25) closedTabs_.removeLast();
  emit closedTabsChanged();
}

bool BrowserProfileService::hasClosedTabs() const { return !closedTabs_.isEmpty(); }

std::optional<ClosedTabEntry> BrowserProfileService::takeMostRecentClosedTab() {
  if (closedTabs_.isEmpty()) return std::nullopt;
  const auto entry = closedTabs_.takeFirst();
  emit closedTabsChanged();
  return entry;
}

const QList<ClosedTabEntry> &BrowserProfileService::closedTabs() const {
  return closedTabs_;
}

std::optional<ClosedTabEntry> BrowserProfileService::takeClosedTab(int index) {
  if (index < 0 || index >= closedTabs_.size()) return std::nullopt;
  const auto entry = closedTabs_.takeAt(index);
  emit closedTabsChanged();
  return entry;
}

QString BrowserProfileService::searchEngine() const {
  const QString stored = preferences_.value(QStringLiteral("browser/searchEngine"), QStringLiteral("DuckDuckGo")).toString();
  if (stored.compare(QLatin1String("Brave Search"), Qt::CaseInsensitive) == 0 ||
      stored.compare(QLatin1String("Brave"), Qt::CaseInsensitive) == 0 ||
      stored.compare(QLatin1String("Bing"), Qt::CaseInsensitive) == 0) {
    return QStringLiteral("Google");
  }
  static const QStringList builtIns{QStringLiteral("Google"), QStringLiteral("DuckDuckGo"), QStringLiteral("Startpage"), QStringLiteral("Mojeek")};
  if (builtIns.contains(stored)) return stored;
  for (const auto &custom : customSearchEngines()) if (custom.name == stored) return stored;
  return QStringLiteral("DuckDuckGo");
}

quint64 BrowserProfileService::totalBlockedCount() const {
  return blockerService_ ? blockerService_->totalBlockedCount() : 0;
}

quint64 BrowserProfileService::sessionBlockedCount() const {
  return blockerService_ ? blockerService_->sessionBlockedCount() : 0;
}

void BrowserProfileService::setSearchEngine(const QString &engine) {
  QString trimmed = engine.trimmed();
  if (trimmed.isEmpty()) return;
  if (trimmed.compare(QLatin1String("Brave Search"), Qt::CaseInsensitive) == 0 ||
      trimmed.compare(QLatin1String("Brave"), Qt::CaseInsensitive) == 0 ||
      trimmed.compare(QLatin1String("Bing"), Qt::CaseInsensitive) == 0) {
    trimmed = QStringLiteral("Google");
  }
  static const QStringList builtIns{QStringLiteral("Google"), QStringLiteral("DuckDuckGo"), QStringLiteral("Startpage"), QStringLiteral("Mojeek")};
  bool known = builtIns.contains(trimmed);
  for (const auto &custom : customSearchEngines()) known = known || custom.name == trimmed;
  if (!known) return;
  if (searchEngine() == trimmed) return;
  preferences_.setValue(QStringLiteral("browser/searchEngine"), trimmed);
  preferences_.sync();
  emit searchEngineChanged(trimmed);
}

QList<CustomSearchEngine> BrowserProfileService::customSearchEngines() const {
  QList<CustomSearchEngine> result;
  const QJsonArray stored = QJsonDocument::fromJson(
      preferences_.value(QStringLiteral("browser/customSearchEngines")).toByteArray()).array();
  for (const QJsonValue &value : stored) {
    const QJsonObject item = value.toObject();
    const QString name = item.value(QStringLiteral("name")).toString().trimmed();
    const QString urlTemplate = item.value(QStringLiteral("template")).toString().trimmed();
    if (!name.isEmpty() && isValidSearchTemplate(urlTemplate)) result.push_back({name, urlTemplate});
  }
  return result;
}

bool BrowserProfileService::isValidSearchTemplate(const QString &urlTemplate) {
  const QString value = urlTemplate.trimmed();
  if (value.count(QStringLiteral("%s")) != 1) return false;
  QString probe = value;
  probe.replace(QStringLiteral("%s"), QStringLiteral("dalinira-query"));
  const QUrl url(probe, QUrl::StrictMode);
  return url.isValid() && !url.host().isEmpty() && url.userInfo().isEmpty()
      && (url.scheme() == QLatin1String("https") || url.scheme() == QLatin1String("http"));
}

bool BrowserProfileService::saveCustomSearchEngine(const QString &name, const QString &urlTemplate,
                                                   const QString &previousName) {
  const QString cleanName = name.trimmed().left(80);
  const QString cleanTemplate = urlTemplate.trimmed();
  static const QStringList builtIns{QStringLiteral("DuckDuckGo"), QStringLiteral("Google")};
  if (cleanName.isEmpty() || builtIns.contains(cleanName, Qt::CaseInsensitive)
      || !isValidSearchTemplate(cleanTemplate)) return false;
  const bool replacingDefault = !previousName.isEmpty()
      && preferences_.value(QStringLiteral("browser/searchEngine")).toString() == previousName;
  auto engines = customSearchEngines();
  int replaceIndex = -1;
  for (int i = 0; i < engines.size(); ++i) {
    if ((!previousName.isEmpty() && engines[i].name == previousName)
        || engines[i].name.compare(cleanName, Qt::CaseInsensitive) == 0) {
      replaceIndex = i;
      break;
    }
  }
  if (replaceIndex >= 0) engines[replaceIndex] = {cleanName, cleanTemplate};
  else engines.push_back({cleanName, cleanTemplate});
  QJsonArray json;
  for (const auto &engine : engines)
    json.append(QJsonObject{{QStringLiteral("name"), engine.name}, {QStringLiteral("template"), engine.urlTemplate}});
  preferences_.setValue(QStringLiteral("browser/customSearchEngines"), QJsonDocument(json).toJson(QJsonDocument::Compact));
  if (replacingDefault) {
    preferences_.setValue(QStringLiteral("browser/searchEngine"), cleanName);
    emit searchEngineChanged(cleanName);
  }
  preferences_.sync();
  return true;
}

bool BrowserProfileService::removeCustomSearchEngine(const QString &name) {
  auto engines = customSearchEngines();
  const qsizetype before = engines.size();
  engines.removeIf([&](const CustomSearchEngine &engine) { return engine.name == name; });
  if (engines.size() == before) return false;
  QJsonArray json;
  for (const auto &engine : engines)
    json.append(QJsonObject{{QStringLiteral("name"), engine.name}, {QStringLiteral("template"), engine.urlTemplate}});
  preferences_.setValue(QStringLiteral("browser/customSearchEngines"), QJsonDocument(json).toJson(QJsonDocument::Compact));
  if (preferences_.value(QStringLiteral("browser/searchEngine")).toString() == name) {
    preferences_.setValue(QStringLiteral("browser/searchEngine"), QStringLiteral("DuckDuckGo"));
    emit searchEngineChanged(QStringLiteral("DuckDuckGo"));
  }
  preferences_.sync();
  return true;
}

QUrl BrowserProfileService::searchUrlForEngine(const QString &engine, const QString &query) const {
  for (const auto &custom : customSearchEngines()) {
    if (custom.name != engine) continue;
    QString resolved = custom.urlTemplate;
    resolved.replace(QStringLiteral("%s"), QString::fromLatin1(QUrl::toPercentEncoding(query.trimmed())));
    const QUrl url(resolved, QUrl::StrictMode);
    return isValidSearchTemplate(custom.urlTemplate) ? url : QUrl{};
  }
  const auto &def = dalinira::core::searchEngineDefinition(engine);
  QUrl url(QString::fromLatin1(def.searchUrl));
  QUrlQuery parameters;
  const QString paramName = (def.queryParam && def.queryParam[0]) ? QString::fromLatin1(def.queryParam) : QStringLiteral("q");
  parameters.addQueryItem(paramName, query.trimmed());
  url.setQuery(parameters);
  return url;
}


QList<QUrl> BrowserProfileService::bookmarks() const {
  QList<QUrl> result;
  const QStringList stored = preferences_.value(QStringLiteral("bookmarks/urls")).toStringList();
  for (const QString &value : stored) {
    const QUrl url = BrowserSecurity::sanitizeUrlForPersistence(QUrl(value));
    if (url.isValid() && (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https"))) result.append(url);
  }
  return result;
}

bool BrowserProfileService::isBookmarked(const QUrl &url) const {
  const QString normalized = BrowserSecurity::sanitizeUrlForPersistence(url).adjusted(QUrl::RemoveFragment).toString(QUrl::FullyEncoded);
  for (const QUrl &bookmark : bookmarks()) {
    if (bookmark.adjusted(QUrl::RemoveFragment).toString(QUrl::FullyEncoded) == normalized) return true;
  }
  return false;
}

bool BrowserProfileService::toggleBookmark(const QUrl &url) {
  if (isBookmarked(url)) {
    removeBookmark(url);
    return false;
  }
  return addBookmark(url, url.host(), QString{});
}

void BrowserProfileService::sanitizeStoredPersistentUrls() {
  const auto isPersistentWebUrl = [](const QUrl &url) {
    return url.isValid() && (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https"));
  };
  bool changed = false;
  QJsonArray history;
  const QJsonDocument historyDocument = QJsonDocument::fromJson(preferences_.value(QStringLiteral("history/entries")).toByteArray());
  if (historyDocument.isArray()) {
    for (const QJsonValue &value : historyDocument.array()) {
      QJsonObject item = value.toObject();
      const QString original = item.value(QStringLiteral("url")).toString();
      const QUrl safe = BrowserSecurity::sanitizeUrlForPersistence(QUrl(original));
      if (!isPersistentWebUrl(safe)) { changed = true; continue; }
      const QString encoded = safe.toString(QUrl::FullyEncoded);
      if (encoded != original) { item.insert(QStringLiteral("url"), encoded); changed = true; }
      history.append(item);
    }
    if (changed) preferences_.setValue(QStringLiteral("history/entries"), QJsonDocument(history).toJson(QJsonDocument::Compact));
  }

  bool frequentChanged = false;
  QJsonArray frequent;
  const QJsonDocument frequentDocument = QJsonDocument::fromJson(preferences_.value(QStringLiteral("history/frequentSites")).toByteArray());
  if (frequentDocument.isArray()) {
    for (const QJsonValue &value : frequentDocument.array()) {
      QJsonObject item = value.toObject();
      for (const QString &field : {QStringLiteral("url"), QStringLiteral("iconLookupUrl")}) {
        const QString original = item.value(field).toString();
        if (original.isEmpty()) continue;
        const QUrl safe = BrowserSecurity::sanitizeUrlForPersistence(QUrl(original));
        if (!isPersistentWebUrl(safe)) { item.remove(field); frequentChanged = true; continue; }
        const QString encoded = safe.toString(QUrl::FullyEncoded);
        if (encoded != original) { item.insert(field, encoded); frequentChanged = true; }
      }
      frequent.append(item);
    }
    if (frequentChanged) preferences_.setValue(QStringLiteral("history/frequentSites"), QJsonDocument(frequent).toJson(QJsonDocument::Compact));
  }

  QStringList safeBookmarks;
  const QStringList bookmarks = preferences_.value(QStringLiteral("bookmarks/urls")).toStringList();
  for (const QString &original : bookmarks) {
    const QUrl safe = BrowserSecurity::sanitizeUrlForPersistence(QUrl(original));
    if (isPersistentWebUrl(safe)) safeBookmarks.append(safe.toString(QUrl::FullyEncoded));
  }
  if (safeBookmarks != bookmarks) { preferences_.setValue(QStringLiteral("bookmarks/urls"), safeBookmarks); changed = true; }
  if (changed || frequentChanged) preferences_.sync();
}

SearchSuggestionService *BrowserProfileService::searchSuggestions() const { return searchSuggestions_; }
void BrowserProfileService::setSearchSuggestionsEnabled(bool enabled) {
  if (privateMode_ || (profile_ && profile_->isOffTheRecord())) return;
  QSettings().setValue(QStringLiteral("browser/searchSuggestionsEnabled"), enabled);
  searchSuggestions_->setEnabled(enabled);
  emit searchSuggestionsChanged(enabled);
}

void BrowserProfileService::refreshPrivacyShield() {
  if (!profile_ || !blockerService_) return;
  static const QString kName = QStringLiteral("dalinira-privacy-shield");
  QWebEngineScriptCollection *scripts = profile_->scripts();
  for (const QWebEngineScript &installed : scripts->find(kName)) scripts->remove(installed);
  QWebEngineScript script;
  script.setName(kName);
  script.setInjectionPoint(QWebEngineScript::DocumentCreation);
  script.setWorldId(QWebEngineScript::MainWorld);
  script.setRunsOnSubFrames(true);
  script.setSourceCode(blockerService_->privacyShieldScript());
  scripts->insert(script);
}

void BrowserProfileService::applyEngineCookiePolicy() {
  if (privateMode_) return;
  if (auto *backend = dalinira::engine::EngineBackend::backendForType(dalinira::engine::EngineType::WebKit))
    backend->setCookiePolicy(cookiePolicy());
}

void BrowserProfileService::refreshCookieFilter() {
  applyEngineCookiePolicy();
  if (!profile_ || !blockerService_) return;
  // Qt invokes the filter on its IO thread: capture immutable policy values.
  const auto policies = blockerService_->settings()->sitePolicies();
  const bool protection = blockerService_->settings()->protectionEnabled();
  const QString global = cookiePolicy();
  profile_->cookieStore()->setCookieFilter([policies,protection,global](const QWebEngineCookieStore::FilterRequest &request) {
    QString effective = global;
    if (protection) {
      const auto policy = DaliNiraBlockerSettings::findSitePolicy(request.firstPartyUrl.host(), policies);
      if (policy && !policy->whitelisted) effective = policy->cookiePolicy;
    }
    if (effective == QLatin1String("block_all")) return false;
    return effective == QLatin1String("allow_all") || !request.thirdParty;
  });
}

QList<BrowserHistoryEntry> BrowserProfileService::searchHistory(const QString &query, int maxResults) const {
  const auto all = recentHistory();
  const QString cleanQuery = query.trimmed();
  if (cleanQuery.isEmpty()) {
    return all.mid(0, maxResults);
  }
  const QStringList tokens = cleanQuery.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
  QList<BrowserHistoryEntry> matched;
  for (const auto &entry : all) {
    const QString title = entry.title;
    const QString urlStr = entry.url.toString();
    const QString displayStr = entry.url.toDisplayString();
    const QString host = entry.url.host();
    const QString path = entry.url.path();

    bool allMatch = true;
    for (const QString &tok : tokens) {
      if (!title.contains(tok, Qt::CaseInsensitive) &&
          !urlStr.contains(tok, Qt::CaseInsensitive) &&
          !displayStr.contains(tok, Qt::CaseInsensitive) &&
          !host.contains(tok, Qt::CaseInsensitive) &&
          !path.contains(tok, Qt::CaseInsensitive)) {
        allMatch = false;
        break;
      }
    }
    if (allMatch) {
      matched.append(entry);
      if (matched.size() >= maxResults) break;
    }
  }
  return matched;
}

bool BrowserProfileService::removeHistoryEntry(const QUrl &url, const QDateTime &visitedAt) {
  const QJsonDocument existing = QJsonDocument::fromJson(preferences_.value(QStringLiteral("history/entries")).toByteArray());
  if (!existing.isArray()) return false;
  QJsonArray next;
  bool removed = false;
  const QString targetIso = visitedAt.isValid() ? visitedAt.toUTC().toString(Qt::ISODate) : QString();
  const QString targetUrlStr = url.toString(QUrl::FullyEncoded);
  const QString targetUrlPlain = url.toString();
  const QUrl cleanTargetUrl = url.adjusted(QUrl::RemoveFragment);

  for (const QJsonValue &val : existing.array()) {
    const QJsonObject obj = val.toObject();
    const QString itemUrl = obj.value(QStringLiteral("url")).toString();
    const QString itemTime = obj.value(QStringLiteral("visitedAt")).toString();
    const QUrl parsedItem(itemUrl);

    const bool urlMatches = (itemUrl == targetUrlStr || itemUrl == targetUrlPlain ||
                             parsedItem == url || parsedItem.adjusted(QUrl::RemoveFragment) == cleanTargetUrl);

    bool timeMatches = targetIso.isEmpty();
    if (!timeMatches && !itemTime.isEmpty()) {
      if (itemTime == targetIso) {
        timeMatches = true;
      } else {
        const QDateTime itemDt = QDateTime::fromString(itemTime, Qt::ISODate);
        if (itemDt.isValid() && visitedAt.isValid()) {
          timeMatches = (itemDt == visitedAt || qAbs(itemDt.toSecsSinceEpoch() - visitedAt.toSecsSinceEpoch()) <= 2);
        }
      }
    }

    if ((!removed || targetIso.isEmpty()) && urlMatches && timeMatches) {
      removed = true;
      continue;
    }
    next.append(val);
  }

  if (removed) {
    preferences_.setValue(QStringLiteral("history/entries"), QJsonDocument(next).toJson(QJsonDocument::Compact));
    bool hasRemaining = false;
    for (const QJsonValue &val : next) {
      const QString remainingUrl = val.toObject().value(QStringLiteral("url")).toString();
      if (remainingUrl == targetUrlStr || remainingUrl == targetUrlPlain || QUrl(remainingUrl) == url) {
        hasRemaining = true;
        break;
      }
    }
    if (!hasRemaining) {
      for (const QString &key : {QStringLiteral("history/frequentSites"),
                                 QStringLiteral("newtab/chromium/frequentSites"),
                                 QStringLiteral("newtab/webkit/frequentSites")}) {
        const QJsonDocument freqDoc = QJsonDocument::fromJson(preferences_.value(key).toByteArray());
        if (freqDoc.isArray()) {
          QJsonArray nextFreq;
          for (const QJsonValue &fVal : freqDoc.array()) {
            const QJsonObject fObj = fVal.toObject();
            const QString fUrl = fObj.value(QStringLiteral("url")).toString();
            const QString fIcon = fObj.value(QStringLiteral("iconLookupUrl")).toString();
            if (fUrl != targetUrlStr && fUrl != targetUrlPlain && QUrl(fUrl) != url &&
                fIcon != targetUrlStr && fIcon != targetUrlPlain && QUrl(fIcon) != url) {
              nextFreq.append(fVal);
            }
          }
          preferences_.setValue(key, QJsonDocument(nextFreq).toJson(QJsonDocument::Compact));
        }
      }
    }
    preferences_.sync();
    emit historyChanged();
  }
  return removed;
}

bool BrowserProfileService::removeHistoryEntriesForUrl(const QUrl &url) {
  return removeHistoryEntry(url, QDateTime{});
}

bool BrowserProfileService::removeSuggestion(const QString &type, const QString &text, const QUrl &url) {
  if (isPrivate() || (profile_ && profile_->isOffTheRecord())) return false;
  if (type == QLatin1String("search-history")) return removeSearchHistory(text);
  if (type != QLatin1String("history") && type != QLatin1String("frequent")) return false;
  if (!url.isValid() || !url.userInfo().isEmpty() || url.host().isEmpty() ||
      (url.scheme() != QLatin1String("https") && url.scheme() != QLatin1String("http"))) return false;
  bool removed = removeHistoryEntriesForUrl(url);
  // Frequent rows point at a site root, which may have no exact history visit.
  // Remove that aggregate without deleting unrelated paths on the same site.
  if (type == QLatin1String("frequent")) {
    for (const QString &key : {QStringLiteral("history/frequentSites"),
                               QStringLiteral("newtab/chromium/frequentSites"),
                               QStringLiteral("newtab/webkit/frequentSites")}) {
      const auto document = QJsonDocument::fromJson(preferences_.value(key).toByteArray());
      if (!document.isArray()) continue;
      QJsonArray kept;
      for (const auto &value : document.array()) {
        if (QUrl(value.toObject().value(QStringLiteral("url")).toString()) == url) removed = true;
        else kept.append(value);
      }
      preferences_.setValue(key, QJsonDocument(kept).toJson(QJsonDocument::Compact));
    }
    if (removed) { preferences_.sync(); emit historyChanged(); }
  }
  return removed;
}

QList<BrowserProfileService::BookmarkItem> BrowserProfileService::bookmarkItems() const {
  QList<BookmarkItem> result;
  QSet<QString> urlsSeen;

  const QJsonDocument doc = QJsonDocument::fromJson(preferences_.value(QStringLiteral("bookmarks/items")).toByteArray());
  if (doc.isArray()) {
    for (const QJsonValue &val : doc.array()) {
      const QJsonObject obj = val.toObject();
      const QUrl url = BrowserSecurity::sanitizeUrlForPersistence(QUrl(obj.value(QStringLiteral("url")).toString()));
      if (url.isValid() && (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https"))) {
        BookmarkItem item;
        item.url = url;
        item.title = obj.value(QStringLiteral("title")).toString();
        if (item.title.isEmpty()) item.title = url.host();
        item.folder = obj.value(QStringLiteral("folder")).toString();
        item.dateAdded = QDateTime::fromString(obj.value(QStringLiteral("dateAdded")).toString(), Qt::ISODate);
        if (!item.dateAdded.isValid()) item.dateAdded = QDateTime::currentDateTimeUtc();
        result.append(item);
        urlsSeen.insert(url.toString(QUrl::FullyEncoded));
      }
    }
  }

  for (const QUrl &bmUrl : bookmarks()) {
    const QString encoded = bmUrl.toString(QUrl::FullyEncoded);
    if (!urlsSeen.contains(encoded)) {
      BookmarkItem item;
      item.url = bmUrl;
      item.title = bmUrl.host();
      item.folder = QString();
      item.dateAdded = QDateTime::currentDateTimeUtc();
      result.append(item);
      urlsSeen.insert(encoded);
    }
  }

  return result;
}

bool BrowserProfileService::addBookmark(const QUrl &url, const QString &title, const QString &folder) {
  const QUrl persistentUrl = BrowserSecurity::sanitizeUrlForPersistence(url);
  if (!persistentUrl.isValid() || (persistentUrl.scheme() != QLatin1String("http") && persistentUrl.scheme() != QLatin1String("https"))) return false;

  const QString normalized = persistentUrl.adjusted(QUrl::RemoveFragment).toString(QUrl::FullyEncoded);
  const QString effectiveTitle = title.isEmpty() ? persistentUrl.host() : title;

  QStringList urls = preferences_.value(QStringLiteral("bookmarks/urls")).toStringList();
  if (!urls.contains(normalized)) {
    urls.append(normalized);
    preferences_.setValue(QStringLiteral("bookmarks/urls"), urls);
  }

  QList<BookmarkItem> items = bookmarkItems();
  bool found = false;
  for (auto &item : items) {
    if (item.url.adjusted(QUrl::RemoveFragment).toString(QUrl::FullyEncoded) == normalized) {
      item.title = effectiveTitle;
      item.folder = folder;
      found = true;
      break;
    }
  }
  if (!found) {
    BookmarkItem newItem;
    newItem.url = persistentUrl;
    newItem.title = effectiveTitle;
    newItem.folder = folder;
    newItem.dateAdded = QDateTime::currentDateTimeUtc();
    items.append(newItem);
  }

  QJsonArray arr;
  for (const auto &item : items) {
    QJsonObject obj;
    obj.insert(QStringLiteral("url"), item.url.toString(QUrl::FullyEncoded));
    obj.insert(QStringLiteral("title"), item.title);
    obj.insert(QStringLiteral("folder"), item.folder);
    obj.insert(QStringLiteral("dateAdded"), item.dateAdded.toUTC().toString(Qt::ISODate));
    arr.append(obj);
  }
  preferences_.setValue(QStringLiteral("bookmarks/items"), QJsonDocument(arr).toJson(QJsonDocument::Compact));
  preferences_.sync();
  emit bookmarksChanged();
  return true;
}

bool BrowserProfileService::removeBookmark(const QUrl &url) {
  const QUrl persistentUrl = BrowserSecurity::sanitizeUrlForPersistence(url);
  if (!persistentUrl.isValid()) return false;
  const QString normalized = persistentUrl.adjusted(QUrl::RemoveFragment).toString(QUrl::FullyEncoded);

  QStringList urls = preferences_.value(QStringLiteral("bookmarks/urls")).toStringList();
  const int removedCount = urls.removeAll(normalized);

  const QJsonDocument doc = QJsonDocument::fromJson(preferences_.value(QStringLiteral("bookmarks/items")).toByteArray());
  QJsonArray nextArr;
  if (doc.isArray()) {
    for (const QJsonValue &val : doc.array()) {
      const QJsonObject obj = val.toObject();
      const QString itemUrl = obj.value(QStringLiteral("url")).toString();
      if (itemUrl != normalized && QUrl(itemUrl).adjusted(QUrl::RemoveFragment).toString(QUrl::FullyEncoded) != normalized) {
        nextArr.append(val);
      }
    }
  }
  preferences_.setValue(QStringLiteral("bookmarks/urls"), urls);
  preferences_.setValue(QStringLiteral("bookmarks/items"), QJsonDocument(nextArr).toJson(QJsonDocument::Compact));
  preferences_.sync();
  emit bookmarksChanged();
  return removedCount > 0;
}

bool BrowserProfileService::createBookmarkFolder(const QString &folderName) {
  const QString clean = folderName.trimmed();
  if (clean.isEmpty()) return false;
  QStringList folders = preferences_.value(QStringLiteral("bookmarks/folders")).toStringList();
  if (!folders.contains(clean)) {
    folders.append(clean);
    folders.sort();
    preferences_.setValue(QStringLiteral("bookmarks/folders"), folders);
    preferences_.sync();
    emit bookmarksChanged();
    return true;
  }
  return false;
}

bool BrowserProfileService::removeBookmarkFolder(const QString &folderName, bool deleteContents) {
  const QString clean = folderName.trimmed();
  if (clean.isEmpty()) return false;
  QStringList folders = preferences_.value(QStringLiteral("bookmarks/folders")).toStringList();
  folders.removeAll(clean);
  preferences_.setValue(QStringLiteral("bookmarks/folders"), folders);

  QList<BookmarkItem> items = bookmarkItems();
  QList<BookmarkItem> nextItems;
  QStringList urls = preferences_.value(QStringLiteral("bookmarks/urls")).toStringList();

  for (auto item : items) {
    if (item.folder.trimmed() == clean) {
      if (deleteContents) {
        urls.removeAll(item.url.adjusted(QUrl::RemoveFragment).toString(QUrl::FullyEncoded));
        continue;
      } else {
        item.folder = QString();
        nextItems.append(item);
      }
    } else {
      nextItems.append(item);
    }
  }

  QJsonArray arr;
  for (const auto &item : nextItems) {
    QJsonObject obj;
    obj.insert(QStringLiteral("url"), item.url.toString(QUrl::FullyEncoded));
    obj.insert(QStringLiteral("title"), item.title);
    obj.insert(QStringLiteral("folder"), item.folder);
    obj.insert(QStringLiteral("dateAdded"), item.dateAdded.toUTC().toString(Qt::ISODate));
    arr.append(obj);
  }
  preferences_.setValue(QStringLiteral("bookmarks/urls"), urls);
  preferences_.setValue(QStringLiteral("bookmarks/items"), QJsonDocument(arr).toJson(QJsonDocument::Compact));
  preferences_.sync();
  emit bookmarksChanged();
  return true;
}

bool BrowserProfileService::moveBookmarkToFolder(const QUrl &url, const QString &targetFolder) {
  const QUrl persistentUrl = BrowserSecurity::sanitizeUrlForPersistence(url);
  if (!persistentUrl.isValid()) return false;
  const QString normalized = persistentUrl.adjusted(QUrl::RemoveFragment).toString(QUrl::FullyEncoded);

  QList<BookmarkItem> items = bookmarkItems();
  bool found = false;
  for (auto &item : items) {
    if (item.url.adjusted(QUrl::RemoveFragment).toString(QUrl::FullyEncoded) == normalized) {
      item.folder = targetFolder.trimmed();
      found = true;
      break;
    }
  }
  if (!found) return false;

  if (!targetFolder.trimmed().isEmpty()) {
    createBookmarkFolder(targetFolder.trimmed());
  }

  QJsonArray arr;
  for (const auto &item : items) {
    QJsonObject obj;
    obj.insert(QStringLiteral("url"), item.url.toString(QUrl::FullyEncoded));
    obj.insert(QStringLiteral("title"), item.title);
    obj.insert(QStringLiteral("folder"), item.folder);
    obj.insert(QStringLiteral("dateAdded"), item.dateAdded.toUTC().toString(Qt::ISODate));
    arr.append(obj);
  }
  preferences_.setValue(QStringLiteral("bookmarks/items"), QJsonDocument(arr).toJson(QJsonDocument::Compact));
  preferences_.sync();
  emit bookmarksChanged();
  return true;
}

bool BrowserProfileService::renameBookmarkFolder(const QString &oldName, const QString &newName) {
  const QString cleanOld = oldName.trimmed();
  const QString cleanNew = newName.trimmed();
  if (cleanOld.isEmpty() || cleanNew.isEmpty() || cleanOld == cleanNew) return false;

  QStringList folders = preferences_.value(QStringLiteral("bookmarks/folders")).toStringList();
  folders.removeAll(cleanOld);
  if (!folders.contains(cleanNew)) folders.append(cleanNew);
  folders.sort();
  preferences_.setValue(QStringLiteral("bookmarks/folders"), folders);

  QList<BookmarkItem> items = bookmarkItems();
  for (auto &item : items) {
    if (item.folder.trimmed() == cleanOld) {
      item.folder = cleanNew;
    }
  }

  QJsonArray arr;
  for (const auto &item : items) {
    QJsonObject obj;
    obj.insert(QStringLiteral("url"), item.url.toString(QUrl::FullyEncoded));
    obj.insert(QStringLiteral("title"), item.title);
    obj.insert(QStringLiteral("folder"), item.folder);
    obj.insert(QStringLiteral("dateAdded"), item.dateAdded.toUTC().toString(Qt::ISODate));
    arr.append(obj);
  }
  preferences_.setValue(QStringLiteral("bookmarks/items"), QJsonDocument(arr).toJson(QJsonDocument::Compact));
  preferences_.sync();
  emit bookmarksChanged();
  return true;
}

QStringList BrowserProfileService::bookmarkFolders() const {
  QSet<QString> folders;
  const QStringList savedFolders = preferences_.value(QStringLiteral("bookmarks/folders")).toStringList();
  for (const QString &f : savedFolders) {
    if (!f.trimmed().isEmpty()) folders.insert(f.trimmed());
  }
  for (const auto &item : bookmarkItems()) {
    if (!item.folder.trimmed().isEmpty()) {
      folders.insert(item.folder.trimmed());
    }
  }
  QStringList list = folders.values();
  list.sort();
  return list;
}

QString BrowserProfileService::exportBookmarksToHtml() const {
  QString html = QStringLiteral("<!DOCTYPE NETSCAPE-Bookmark-file-1>\n"
                                "<!-- This is an automatically generated file. -->\n"
                                "<META HTTP-EQUIV=\"Content-Type\" CONTENT=\"text/html; charset=UTF-8\">\n"
                                "<TITLE>Bookmarks</TITLE>\n"
                                "<H1>Bookmarks</H1>\n"
                                "<DL><p>\n");
  const auto items = bookmarkItems();
  QMap<QString, QList<BookmarkItem>> grouped;
  for (const auto &item : items) {
    grouped[item.folder].append(item);
  }
  const QStringList allFolders = bookmarkFolders();
  for (const QString &f : allFolders) {
    if (!grouped.contains(f)) grouped[f] = {};
  }

  if (grouped.contains(QString())) {
    for (const auto &item : grouped.value(QString())) {
      const qint64 ts = item.dateAdded.isValid() ? item.dateAdded.toSecsSinceEpoch() : QDateTime::currentSecsSinceEpoch();
      html += QStringLiteral("    <DT><A HREF=\"%1\" ADD_DATE=\"%2\">%3</A>\n")
                  .arg(item.url.toString(QUrl::FullyEncoded).toHtmlEscaped())
                  .arg(ts)
                  .arg(item.title.toHtmlEscaped());
    }
  }

  for (auto it = grouped.begin(); it != grouped.end(); ++it) {
    if (it.key().isEmpty()) continue;
    html += QStringLiteral("    <DT><H3 ADD_DATE=\"%1\">%2</H3>\n    <DL><p>\n")
                .arg(QDateTime::currentSecsSinceEpoch())
                .arg(it.key().toHtmlEscaped());
    for (const auto &item : it.value()) {
      const qint64 ts = item.dateAdded.isValid() ? item.dateAdded.toSecsSinceEpoch() : QDateTime::currentSecsSinceEpoch();
      html += QStringLiteral("        <DT><A HREF=\"%1\" ADD_DATE=\"%2\">%3</A>\n")
                  .arg(item.url.toString(QUrl::FullyEncoded).toHtmlEscaped())
                  .arg(ts)
                  .arg(item.title.toHtmlEscaped());
    }
    html += QStringLiteral("    </DL><p>\n");
  }

  html += QStringLiteral("</DL><p>\n");
  return html;
}

int BrowserProfileService::importBookmarksFromHtml(const QString &htmlContent) {
  int importedCount = 0;
  QString currentFolder;
  const QStringList lines = htmlContent.split(QLatin1Char('\n'));
  static const QRegularExpression folderRegex(QStringLiteral("<H3[^>]*>([^<]+)</H3>"), QRegularExpression::CaseInsensitiveOption);
  static const QRegularExpression bookmarkRegex(QStringLiteral("<A\\s+[^>]*HREF=[\"']([^\"']+)[\"'][^>]*>([^<]*)</A>"), QRegularExpression::CaseInsensitiveOption);
  static const QRegularExpression closeDlRegex(QStringLiteral("</DL>"), QRegularExpression::CaseInsensitiveOption);

  for (const QString &line : lines) {
    const QString trimmed = line.trimmed();
    auto folderMatch = folderRegex.match(trimmed);
    if (folderMatch.hasMatch()) {
      currentFolder = folderMatch.captured(1).trimmed();
      if (!currentFolder.isEmpty()) {
        createBookmarkFolder(currentFolder);
      }
      continue;
    }
    if (closeDlRegex.match(trimmed).hasMatch()) {
      currentFolder.clear();
      continue;
    }
    auto bmMatch = bookmarkRegex.match(trimmed);
    if (bmMatch.hasMatch()) {
      const QUrl url(bmMatch.captured(1).trimmed());
      QString title = bmMatch.captured(2).trimmed();
      if (title.isEmpty()) title = url.host();
      if (url.isValid() && (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https"))) {
        if (addBookmark(url, title, currentFolder)) {
          importedCount++;
        }
      }
    }
  }
  return importedCount;
}

QString BrowserProfileService::secureDnsMode() const {
  return preferences_.value(QStringLiteral("security/secureDnsMode"), QStringLiteral("system")).toString();
}

void BrowserProfileService::setSecureDnsMode(const QString &mode) {
  preferences_.setValue(QStringLiteral("security/secureDnsMode"), mode);
  preferences_.sync();
  applySecureDnsSettings();
  emit secureDnsChanged();
}

QString BrowserProfileService::secureDnsTemplate() const {
  return preferences_.value(QStringLiteral("security/secureDnsTemplate"), QStringLiteral("https://cloudflare-dns.com/dns-query")).toString();
}

void BrowserProfileService::setSecureDnsTemplate(const QString &templateUrl) {
  preferences_.setValue(QStringLiteral("security/secureDnsTemplate"), templateUrl);
  preferences_.sync();
  applySecureDnsSettings();
  emit secureDnsChanged();
}

void BrowserProfileService::applySecureDnsSettings() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
  const QString mode = secureDnsMode();
  QWebEngineGlobalSettings::DnsMode dnsMode;
  if (mode == QLatin1String("secure")) {
    dnsMode.secureMode = QWebEngineGlobalSettings::SecureDnsMode::SecureOnly;
    const QString templ = secureDnsTemplate().trimmed();
    if (!templ.isEmpty()) {
      dnsMode.serverTemplates = QStringList{templ};
    }
  } else if (mode == QLatin1String("fallback")) {
    dnsMode.secureMode = QWebEngineGlobalSettings::SecureDnsMode::SecureWithFallback;
    const QString templ = secureDnsTemplate().trimmed();
    if (!templ.isEmpty()) {
      dnsMode.serverTemplates = QStringList{templ};
    }
  } else {
    // "System default": use DNS-over-HTTPS with fallback to system DNS.
    // Providing a valid serverTemplate ensures QWebEngineGlobalSettings::setDnsMode succeeds
    // and enables DoH (which resolves HTTPS/SVCB records for ECH).
    dnsMode.secureMode = QWebEngineGlobalSettings::SecureDnsMode::SecureWithFallback;
    const QString templ = secureDnsTemplate().trimmed();
    if (!templ.isEmpty()) {
      dnsMode.serverTemplates = QStringList{templ};
    }
  }
  if (!QWebEngineGlobalSettings::setDnsMode(dnsMode) &&
      dnsMode.secureMode == QWebEngineGlobalSettings::SecureDnsMode::SecureWithFallback && dnsMode.serverTemplates.isEmpty()) {
    dnsMode.secureMode = QWebEngineGlobalSettings::SecureDnsMode::SystemOnly;
    QWebEngineGlobalSettings::setDnsMode(dnsMode);
  }
#endif
}

bool BrowserProfileService::isOnboardingCompleted() const {
  return dalinira::onboarding::OnboardingManager::instance().isOnboardingCompleted();
}

void BrowserProfileService::setOnboardingCompleted(bool completed) {
  dalinira::onboarding::OnboardingManager::instance().setOnboardingCompleted(completed);
  emit onboardingCompletedChanged(completed);
}

int BrowserProfileService::onboardingCurrentStep() const {
  return dalinira::onboarding::OnboardingManager::instance().currentStepIndex();
}

void BrowserProfileService::setOnboardingCurrentStep(int step) {
  dalinira::onboarding::OnboardingManager::instance().setCurrentStepIndex(step);
}
