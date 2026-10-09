#pragma once

#include "engine/engine_profile.h"
#include "engine/engine_download_context.h"
#include <QIcon>
#include <QUrl>
#include <functional>
#include <memory>

class QTemporaryDir;

typedef struct _WebKitWebsiteDataManager WebKitWebsiteDataManager;
typedef struct _WebKitWebContext WebKitWebContext;
typedef struct _WebKitSettings WebKitSettings;
typedef struct _WebKitFaviconDatabase WebKitFaviconDatabase;

namespace dalinira::engine {

class WebKitNetworkFilter;

class WebKitEngineProfile : public EngineProfile {
  Q_OBJECT
 public:
  explicit WebKitEngineProfile(bool isEphemeral,
                               const QString &storagePath,
                               const QString &cachePath,
                               const QString &cookiesPath,
                               QObject *parent = nullptr);
  explicit WebKitEngineProfile(const QString &storagePath,
                               const QString &cachePath,
                               const QString &cookiesPath,
                               QObject *parent = nullptr)
      : WebKitEngineProfile(false, storagePath, cachePath, cookiesPath, parent) {}
  ~WebKitEngineProfile() override;

  bool isEphemeral() const { return isEphemeral_; }
  WebKitWebsiteDataManager *dataManager() const { return dataManager_; }
  WebKitWebContext *webContext() const { return context_; }
  WebKitSettings *settings() const { return settings_; }
  // A new settings object configured like settings(), owned by the caller.
  // Each view gets its own, because per-site page script policy is a view
  // setting (enable-javascript-markup) that must not leak into other tabs.
  WebKitSettings *createViewSettings() const;
  using FaviconCallback = std::function<void(const QIcon &)>;
  void requestFavicon(const QUrl &pageUrl, FaviconCallback callback) const;
  // Whether the GStreamer runtime has the audio sink WebKit's media players
  // require (see WebKitMediaRuntime). When false, media is disabled in the
  // settings instead of crashing pages.
  static bool mediaPlaybackAvailable();

  // User-Agent and Cookies bound specifically to this WebKit profile context.
  QByteArray userAgent() const;
  QList<QNetworkCookie> cookiesForUrl(const QUrl &url) const;
  // DaliNira's cookie policy for this profile's cookie store: "third_party"
  // (the default) accepts and sends cookies only for the top-level site,
  // "allow_all" accepts all, "block_all" none.
  void setCookiePolicy(const QString &policy);
  // Asynchronously removes cookies and script-visible website data belonging
  // to `host`. WebKitGTK exposes website records, but not individual top-level
  // partition keys; this deliberately never broad-clears the profile.
  bool clearSiteData(const QString &host);

  WebKitNetworkFilter *networkFilter() const { return networkFilter_; }
  void setNetworkFilter(WebKitNetworkFilter *filter) { networkFilter_ = filter; }

 signals:
  void downloadContextRequested(const dalinira::engine::WebKitDownloadContext &context);
  void downloadRequested(const QUrl &url, const QString &suggestedName, const QString &mimeType, qint64 totalBytes);
  void faviconLoaded(const QUrl &pageUrl, const QIcon &icon);

 private:
  bool isEphemeral_ = false;
  WebKitWebsiteDataManager *dataManager_ = nullptr;
  WebKitWebContext *context_ = nullptr;
  WebKitSettings *settings_ = nullptr;
  // Held so its "favicon-changed" handler can be disconnected on destruction.
  WebKitFaviconDatabase *faviconDatabase_ = nullptr;
  std::unique_ptr<QTemporaryDir> ephemeralFavicons_;
  WebKitNetworkFilter *networkFilter_ = nullptr;
};

} // namespace dalinira::engine
