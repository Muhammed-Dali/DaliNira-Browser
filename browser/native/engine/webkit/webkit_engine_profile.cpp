#include "engine/webkit/webkit_engine_profile.h"
#include "engine/webkit/webkit_engine_view.h"
#include "engine/webkit/webkit_media_runtime.h"
#include "engine/webkit/webkit_network_filter.h"

#undef signals
#include <webkit2/webkit2.h>
#include <libsoup/soup.h>
#include <QDir>
#include <QTemporaryDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkCookie>
#include <QSettings>
#include <QThread>
#include <QDebug>
#include <QImage>
#include <QPixmap>
#include <QPointer>
#include "core/adult_content_protection.h"

namespace dalinira::engine {

namespace {
struct PendingFaviconLookup {
  WebKitFaviconDatabase *database = nullptr;
  WebKitEngineProfile::FaviconCallback callback;
};

struct PendingDownloadState {
  // The response can arrive after the profile is gone.
  QPointer<WebKitEngineProfile> profile;
  WebKitDownload *download = nullptr;
  gulong notifyId = 0;
  gulong failedId = 0;
};

static void dispatchWebKitDownload(WebKitEngineProfile *self, WebKitDownload *download, WebKitURIResponse *resp) {
  if (!self || !download) return;
  WebKitURIRequest *req = webkit_download_get_request(download);
  const gchar *uri = req ? webkit_uri_request_get_uri(req) : nullptr;
  if (!uri || !*uri) {
    webkit_download_cancel(download);
    return;
  }
  const QUrl url(QString::fromUtf8(uri));
  const QString host = dalinira::core::AdultContentProtectionService::normalizeHost(url.host());

  const bool adultProtection = QSettings().value(QStringLiteral("privacy/adultContentProtection"), true).toBool();
  if (adultProtection && dalinira::core::AdultContentProtectionService::instance().isBlockedHost(host)) {
    qWarning() << "WebKitBackend: Download blocked by Adult Content Protection for host:" << host;
    webkit_download_cancel(download);
    return;
  }

  dalinira::engine::WebKitDownloadContext ctx;
  ctx.sourceEngine = EngineType::WebKit;
  ctx.url = url;
  ctx.isPrivate = self->isEphemeral();

  if (req) {
    SoupMessageHeaders *headers = webkit_uri_request_get_http_headers(req);
    if (headers) {
      const char *ref = soup_message_headers_get_one(headers, "Referer");
      if (ref) ctx.referrer = QByteArray(ref);
      const char *ua = soup_message_headers_get_one(headers, "User-Agent");
      if (ua) ctx.userAgent = QByteArray(ua);
    }
  }

  WebKitWebView *wv = webkit_download_get_web_view(download);
  if (wv) {
    const char *pageUri = webkit_web_view_get_uri(wv);
    if (pageUri && *pageUri) {
      ctx.pageUrl = QUrl(QString::fromUtf8(pageUri));
      if (ctx.referrer.isEmpty()) ctx.referrer = ctx.pageUrl.toEncoded(QUrl::FullyEncoded);
    }
  }

  if (ctx.userAgent.isEmpty()) {
    ctx.userAgent = self->userAgent();
  }

  ctx.cookies = self->cookiesForUrl(url);

  if (resp) {
    const gchar *sugg = webkit_uri_response_get_suggested_filename(resp);
    if (sugg && *sugg) ctx.suggestedName = QString::fromUtf8(sugg);
    const gchar *m = webkit_uri_response_get_mime_type(resp);
    if (m) ctx.mimeType = QString::fromUtf8(m);
    ctx.totalBytes = webkit_uri_response_get_content_length(resp);
  }
  if (ctx.suggestedName.isEmpty()) {
    ctx.suggestedName = QFileInfo(url.path()).fileName();
  }
  if (ctx.suggestedName.isEmpty()) ctx.suggestedName = QStringLiteral("download");

  emit self->downloadContextRequested(ctx);
  emit self->downloadRequested(ctx.url, ctx.suggestedName, ctx.mimeType, ctx.totalBytes);
  webkit_download_cancel(download);
}

static void onPendingDownloadResponse(WebKitDownload *dl, GParamSpec *, gpointer data) {
  auto *p = static_cast<PendingDownloadState *>(data);
  if (!p) return;
  if (p->failedId) g_signal_handler_disconnect(dl, p->failedId);
  if (p->profile) {
    WebKitURIResponse *r = webkit_download_get_response(dl);
    dispatchWebKitDownload(p->profile, dl, r);
  } else {
    webkit_download_cancel(dl);
  }
  delete p;
}

static void onPendingDownloadFailed(WebKitDownload *dl, GError *, gpointer data) {
  auto *p = static_cast<PendingDownloadState *>(data);
  if (!p) return;
  if (p->notifyId) g_signal_handler_disconnect(dl, p->notifyId);
  delete p;
}

static void onDownloadStartedCallback(WebKitWebContext *, WebKitDownload *download, gpointer user_data) {
  auto *self = static_cast<WebKitEngineProfile *>(user_data);
  if (!self || !download) return;

  WebKitURIResponse *resp = webkit_download_get_response(download);
  if (resp) {
    dispatchWebKitDownload(self, download, resp);
  } else {
    auto *pending = new PendingDownloadState;
    pending->profile = self;
    pending->download = download;
    pending->notifyId = g_signal_connect(download, "notify::response", G_CALLBACK(onPendingDownloadResponse), pending);
    pending->failedId = g_signal_connect(download, "failed", G_CALLBACK(onPendingDownloadFailed), pending);
  }
}
}

QByteArray WebKitEngineProfile::userAgent() const {
  if (settings_) {
    const char *ua = webkit_settings_get_user_agent(settings_);
    if (ua && *ua) return QByteArray(ua);
  }
  return QByteArray("DaliNira/8.0.2 (WebKit)");
}

void WebKitEngineProfile::setCookiePolicy(const QString &policy) {
  WebKitCookieManager *cookies = dataManager_ ? webkit_website_data_manager_get_cookie_manager(dataManager_) : nullptr;
  if (!cookies) return;
  const WebKitCookieAcceptPolicy accept = policy == QLatin1String("allow_all") ? WEBKIT_COOKIE_POLICY_ACCEPT_ALWAYS
                                          : policy == QLatin1String("block_all") ? WEBKIT_COOKIE_POLICY_ACCEPT_NEVER
                                                                                 : WEBKIT_COOKIE_POLICY_ACCEPT_NO_THIRD_PARTY;
  webkit_cookie_manager_set_accept_policy(cookies, accept);
  // The accept policy only keeps a third party from setting cookies: one set
  // while the user visited it directly would still be sent to it, and be
  // readable by it, wherever it is embedded. Intelligent Tracking Prevention
  // blocks third-party cookies in both directions (allowing them back only
  // through the Storage Access API). It decides instead of the accept policy.
  webkit_website_data_manager_set_itp_enabled(dataManager_, accept == WEBKIT_COOKIE_POLICY_ACCEPT_NO_THIRD_PARTY);
}

bool WebKitEngineProfile::clearSiteData(const QString &host) {
  const QString target = host.trimmed().toLower();
  if (!dataManager_ || target.isEmpty()) return false;
  struct ClearState {
    QString host;
    GList *all = nullptr;
    GList *selected = nullptr;
  };
  auto *state = new ClearState{target};
  constexpr WebKitWebsiteDataTypes types = static_cast<WebKitWebsiteDataTypes>(
      WEBKIT_WEBSITE_DATA_MEMORY_CACHE | WEBKIT_WEBSITE_DATA_DISK_CACHE |
      WEBKIT_WEBSITE_DATA_OFFLINE_APPLICATION_CACHE | WEBKIT_WEBSITE_DATA_SESSION_STORAGE |
      WEBKIT_WEBSITE_DATA_LOCAL_STORAGE | WEBKIT_WEBSITE_DATA_WEBSQL_DATABASES |
      WEBKIT_WEBSITE_DATA_INDEXEDDB_DATABASES | WEBKIT_WEBSITE_DATA_COOKIES |
      WEBKIT_WEBSITE_DATA_DEVICE_ID_HASH_SALT | WEBKIT_WEBSITE_DATA_ITP |
      WEBKIT_WEBSITE_DATA_SERVICE_WORKER_REGISTRATIONS | WEBKIT_WEBSITE_DATA_DOM_CACHE);
  webkit_website_data_manager_fetch(dataManager_, types, nullptr,
      +[](GObject *source, GAsyncResult *result, gpointer data) {
        auto *state = static_cast<ClearState *>(data);
        GError *error = nullptr;
        state->all = webkit_website_data_manager_fetch_finish(WEBKIT_WEBSITE_DATA_MANAGER(source), result, &error);
        if (error) g_error_free(error);
        for (GList *item = state->all; item; item = item->next) {
          const char *raw = webkit_website_data_get_name(static_cast<WebKitWebsiteData *>(item->data));
          QString name = raw ? QString::fromUtf8(raw).trimmed().toLower() : QString();
          const QUrl asUrl(name);
          if (asUrl.isValid() && !asUrl.host().isEmpty()) name = asUrl.host().toLower();
          if (name == state->host || name.endsWith(QLatin1Char('.') + state->host))
            state->selected = g_list_prepend(state->selected, item->data);
        }
        if (!state->selected) {
          g_list_free_full(state->all, reinterpret_cast<GDestroyNotify>(webkit_website_data_unref));
          delete state;
          return;
        }
        webkit_website_data_manager_remove(WEBKIT_WEBSITE_DATA_MANAGER(source), types, state->selected, nullptr,
            +[](GObject *removeSource, GAsyncResult *removeResult, gpointer removeData) {
              auto *state = static_cast<ClearState *>(removeData);
              GError *removeError = nullptr;
              webkit_website_data_manager_remove_finish(
                  WEBKIT_WEBSITE_DATA_MANAGER(removeSource), removeResult, &removeError);
              if (removeError) g_error_free(removeError);
              g_list_free(state->selected);
              g_list_free_full(state->all, reinterpret_cast<GDestroyNotify>(webkit_website_data_unref));
              delete state;
            }, state);
      }, state);
  return true;
}

QList<QNetworkCookie> WebKitEngineProfile::cookiesForUrl(const QUrl &url) const {
  if (!dataManager_ || !url.isValid()) return {};
  WebKitCookieManager *cm = webkit_website_data_manager_get_cookie_manager(dataManager_);
  if (!cm) return {};

  struct CookieState {
    bool done = false;
    QList<QNetworkCookie> cookies;
  } state;

  const QByteArray uri = url.toEncoded();
  webkit_cookie_manager_get_cookies(cm, uri.constData(), nullptr,
    +[](GObject *source, GAsyncResult *res, gpointer data) {
      auto *s = static_cast<CookieState *>(data);
      GError *err = nullptr;
      GList *list = webkit_cookie_manager_get_cookies_finish(WEBKIT_COOKIE_MANAGER(source), res, &err);
      if (list) {
        for (GList *l = list; l; l = l->next) {
          auto *c = static_cast<SoupCookie *>(l->data);
          if (c) {
            QNetworkCookie qc(QByteArray(soup_cookie_get_name(c)), QByteArray(soup_cookie_get_value(c)));
            const char *domain = soup_cookie_get_domain(c);
            if (domain) qc.setDomain(QString::fromUtf8(domain));
            const char *path = soup_cookie_get_path(c);
            if (path) qc.setPath(QString::fromUtf8(path));
            qc.setSecure(soup_cookie_get_secure(c));
            qc.setHttpOnly(soup_cookie_get_http_only(c));
            s->cookies.append(qc);
          }
        }
        g_list_free_full(list, (GDestroyNotify)soup_cookie_free);
      }
      if (err) g_error_free(err);
      s->done = true;
    }, &state);

  int iterations = 0;
  while (!state.done && iterations++ < 200) {
    g_main_context_iteration(nullptr, FALSE);
    if (!state.done) {
      QThread::msleep(1);
    }
  }
  return state.cookies;
}

void WebKitEngineProfile::requestFavicon(const QUrl &pageUrl, FaviconCallback callback) const {
  if (!context_ || !callback || !pageUrl.isValid()) {
    if (callback) callback({});
    return;
  }
  WebKitFaviconDatabase *database = webkit_web_context_get_favicon_database(context_);
  if (!database) {
    callback({});
    return;
  }
  auto *pending = new PendingFaviconLookup{database, std::move(callback)};
  const QByteArray encoded = pageUrl.toEncoded(QUrl::FullyEncoded);
  webkit_favicon_database_get_favicon(
      database, encoded.constData(), nullptr,
      +[](GObject *, GAsyncResult *result, gpointer data) {
        std::unique_ptr<PendingFaviconLookup> pending(static_cast<PendingFaviconLookup *>(data));
        GError *error = nullptr;
        cairo_surface_t *surface = webkit_favicon_database_get_favicon_finish(
            pending->database, result, &error);
        QIcon icon;
        if (surface) {
          const int width = cairo_image_surface_get_width(surface);
          const int height = cairo_image_surface_get_height(surface);
          const unsigned char *pixels = cairo_image_surface_get_data(surface);
          const int stride = cairo_image_surface_get_stride(surface);
          if (pixels && width > 0 && height > 0) {
            QImage image(pixels, width, height, stride, QImage::Format_ARGB32_Premultiplied);
            icon = QIcon(QPixmap::fromImage(image.copy()));
          }
          cairo_surface_destroy(surface);
        }
        if (error) g_error_free(error);
        pending->callback(icon);
      },
      pending);
}

WebKitEngineProfile::WebKitEngineProfile(bool isEphemeral,
                                         const QString &storagePath,
                                         const QString &cachePath,
                                         const QString &cookiesPath,
                                         QObject *parent)
    : EngineProfile(EngineType::WebKit, isEphemeral ? QStringLiteral(":memory:") : storagePath, parent),
      isEphemeral_(isEphemeral) {
  // Avoid direct video DMA-BUF import (driver-dependent plane/stride and
  // synchronization artifacts). WebKit still uses glupload/glcolorconvert,
  // hardware decoders and its accelerated page compositor. This is distinct
  // from WEBKIT_DISABLE_DMABUF_RENDERER, which must remain unset.
  if (!qEnvironmentVariableIsSet("WEBKIT_GST_DMABUF_SINK_DISABLED"))
    qputenv("WEBKIT_GST_DMABUF_SINK_DISABLED", "1");
  if (isEphemeral_) {
    dataManager_ = webkit_website_data_manager_new_ephemeral();
  } else {
    QDir().mkpath(storagePath);
    QDir().mkpath(cachePath);
    QDir().mkpath(QFileInfo(cookiesPath).dir().absolutePath());

    const QByteArray storageBytes = storagePath.toUtf8();
    const QByteArray cacheBytes = cachePath.toUtf8();
    const QByteArray cookiesBytes = cookiesPath.toUtf8();

    dataManager_ = webkit_website_data_manager_new(
        "base-data-directory", storageBytes.constData(),
        "base-cache-directory", cacheBytes.constData(),
        nullptr);

    if (dataManager_) {
      WebKitCookieManager *cookieMgr = webkit_website_data_manager_get_cookie_manager(dataManager_);
      if (cookieMgr) {
        webkit_cookie_manager_set_persistent_storage(
            cookieMgr, cookiesBytes.constData(), WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
      }
    }
  }

  // Third-party cookies are blocked until the browser applies its policy.
  setCookiePolicy(QStringLiteral("third_party"));
  context_ = webkit_web_context_new_with_website_data_manager(dataManager_);
  // WebKitGTK only records and reports page favicons once the context has a
  // favicon database. Private profiles keep it in a directory removed with them.
  if (isEphemeral_) ephemeralFavicons_ = std::make_unique<QTemporaryDir>();
  const QString faviconPath = isEphemeral_
      ? (ephemeralFavicons_->isValid() ? ephemeralFavicons_->path() : QString())
      : QDir(cachePath).filePath(QStringLiteral("favicons"));
  if (!faviconPath.isEmpty()) {
    QDir().mkpath(faviconPath);
    webkit_web_context_set_favicon_database_directory(context_, faviconPath.toUtf8().constData());
    WebKitFaviconDatabase *database = webkit_web_context_get_favicon_database(context_);
    if (database) {
      faviconDatabase_ = WEBKIT_FAVICON_DATABASE(g_object_ref(database));
      g_signal_connect(database, "favicon-changed", G_CALLBACK(+[](WebKitFaviconDatabase *, const gchar *page_uri, const gchar *, gpointer data) {
        auto *self = static_cast<WebKitEngineProfile *>(data);
        if (!self || !page_uri) return;
        const QUrl pageUrl(QString::fromUtf8(page_uri));
        if (!pageUrl.isValid()) return;
        // The lookup is asynchronous; the profile may be destroyed before it ends.
        const QPointer<WebKitEngineProfile> guarded(self);
        self->requestFavicon(pageUrl, [guarded, pageUrl](const QIcon &icon) {
          if (guarded && !icon.isNull()) {
            emit guarded->faviconLoaded(pageUrl, icon);
          }
        });
      }), this);
    }
  }
  WebKitEngineView::registerInternalScheme(context_);

  // Emitted before each web process of this context starts. The extension is
  // always applies the Web Audio latency policy; network hooks additionally
  // require the profile's authenticated filter configuration.
  g_signal_connect(context_, "initialize-web-extensions", G_CALLBACK(+[](WebKitWebContext *context, gpointer data) {
    auto *self = static_cast<WebKitEngineProfile *>(data);
    WebKitNetworkFilter *filter = self ? self->networkFilter_ : nullptr;
    const QByteArray directory = QFile::encodeName(WebKitNetworkFilter::extensionDirectory());
    webkit_web_context_set_web_extensions_directory(context, directory.constData());
    if (!filter || !filter->isRunning()) return;
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&builder, "{sv}", "network-filter-socket",
                          g_variant_new_string(QFile::encodeName(filter->socketPath()).constData()));
    g_variant_builder_add(&builder, "{sv}", "network-filter-token", g_variant_new_string(filter->token().constData()));
    webkit_web_context_set_web_extensions_initialization_user_data(context, g_variant_builder_end(&builder));
  }), this);

  // Intercept and route WebKit downloads into DaliNira's GeneralDownloadManager
  g_signal_connect(context_, "download-started", G_CALLBACK(onDownloadStartedCallback), this);

  // Strictly enforce TLS verification (Phase 11): Never ignore TLS certificate errors
  webkit_website_data_manager_set_tls_errors_policy(dataManager_, WEBKIT_TLS_ERRORS_POLICY_FAIL);

  settings_ = createViewSettings();
  const WebKitMediaRuntime::Report &media = WebKitMediaRuntime::cached();
  static bool reported = false;
  if (!reported && !media.missing.isEmpty()) {
    reported = true;
    qWarning().noquote() << (media.playbackAvailable
                                 ? QStringLiteral("WebKitBackend: some media formats are unsupported; missing GStreamer elements:")
                                 : QStringLiteral("WebKitBackend: HTML5 media is disabled; missing GStreamer elements:"))
                         << WebKitMediaRuntime::describe(media);
  }
}

WebKitSettings *WebKitEngineProfile::createViewSettings() const {
  WebKitSettings *settings = webkit_settings_new();
  webkit_settings_set_enable_developer_extras(settings, TRUE);
  webkit_settings_set_enable_webgl(settings, TRUE);
  webkit_settings_set_enable_smooth_scrolling(settings, TRUE);
  webkit_settings_set_enable_javascript(settings, TRUE);
  webkit_settings_set_enable_media(settings, TRUE);
  webkit_settings_set_enable_mediasource(settings, TRUE);
  webkit_settings_set_media_playback_allows_inline(settings, TRUE);
  webkit_settings_set_media_playback_requires_user_gesture(settings, FALSE);
  webkit_settings_set_allow_file_access_from_file_urls(settings, TRUE);
  webkit_settings_set_allow_universal_access_from_file_urls(settings, TRUE);
  webkit_settings_set_enable_webaudio(settings, TRUE);
  webkit_settings_set_enable_encrypted_media(settings, TRUE);
  webkit_settings_set_enable_media_capabilities(settings, TRUE);
  webkit_settings_set_enable_page_cache(settings, TRUE);
  webkit_settings_set_hardware_acceleration_policy(settings, WEBKIT_HARDWARE_ACCELERATION_POLICY_ON_DEMAND);
  // WebKitGTK's own User-Agent, without a "DaliNira/x.y" application token.
  webkit_settings_set_user_agent_with_application_details(settings, nullptr, nullptr);

  const WebKitMediaRuntime::Report &media = WebKitMediaRuntime::cached();
  if (!media.playbackAvailable) {
    // WebKitGTK 2.52 aborts the web process (RELEASE_ASSERT in
    // MediaPlayerPrivateGStreamer::createAudioSink) when a page creates a media
    // player and GStreamer has no autoaudiosink. Keep pages usable instead of
    // leaving a blank tab; media stays unavailable until the runtime exists.
    // DaliNira's packages depend on the runtime, so this is a broken install.
    webkit_settings_set_enable_media(settings, FALSE);
    webkit_settings_set_enable_mediasource(settings, FALSE);
    webkit_settings_set_enable_webaudio(settings, FALSE);
    webkit_settings_set_enable_media_stream(settings, FALSE);
    webkit_settings_set_enable_encrypted_media(settings, FALSE);
  }
  return settings;
}

bool WebKitEngineProfile::mediaPlaybackAvailable() {
  return WebKitMediaRuntime::cached().playbackAvailable;
}

WebKitEngineProfile::~WebKitEngineProfile() {
  // Web views, downloads and pending lookups can keep the context and its
  // favicon database alive; none of their signals may reach this object again.
  if (faviconDatabase_) {
    g_signal_handlers_disconnect_by_data(faviconDatabase_, this);
    g_object_unref(faviconDatabase_);
    faviconDatabase_ = nullptr;
  }
  if (context_) g_signal_handlers_disconnect_by_data(context_, this);
  if (settings_) {
    g_object_unref(settings_);
    settings_ = nullptr;
  }
  if (context_) {
    g_object_unref(context_);
    context_ = nullptr;
  }
  if (dataManager_) {
    g_object_unref(dataManager_);
    dataManager_ = nullptr;
  }
}

} // namespace dalinira::engine
