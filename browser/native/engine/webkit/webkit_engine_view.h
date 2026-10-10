#pragma once

#include "engine/engine_view.h"
#include "engine/webkit/webkit_engine_profile.h"
#include "engine/webkit/webkit_network_filter.h"
#include <QPointer>
#include <QStringList>
#include <QVariantMap>

typedef struct _GtkWidget GtkWidget;
typedef struct _WebKitWebView WebKitWebView;
typedef struct _WebKitURISchemeRequest WebKitURISchemeRequest;
typedef struct _WebKitUserContentManager WebKitUserContentManager;
typedef struct _WebKitSettings WebKitSettings;

namespace dalinira::core {
class IBrowserProfileDataProvider;
}

namespace dalinira::engine {

class WebKitEngineView : public EngineView {
  Q_OBJECT
 public:
  explicit WebKitEngineView(WebKitEngineProfile *profile = nullptr, QWidget *parent = nullptr);
  ~WebKitEngineView() override;

  EngineType engineType() const override { return EngineType::WebKit; }
  QUrl url() const override;
  QString title() const override;
  bool isLoading() const override;

  void load(const QUrl &url) override;
  void reload() override;
  void stop() override;
  void back() override;
  void forward() override;
  bool canGoBack() const override;
  bool canGoForward() const override;
  void setEngineActive(bool active) override;
  bool isEngineActive() const override { return engineActive_; }

  // Shared tab lifecycle. Frozen: the page runs hidden (WebKit throttles its
  // timers and stops rendering). Discarded: the WebKitWebView and with it the
  // web process are destroyed; the serialized session state (back/forward
  // history with scroll positions and form data) restores the page on waking.
  TabLifecycleState lifecycleState() const override { return lifecycleState_; }
  bool setLifecycleState(TabLifecycleState state) override;
  bool isAudible() const override;
  // The web process hosting the page, as registered by DaliNira's web process
  // extension with the network filter; 0 while discarded or not yet known.
  qint64 contentProcessId() const override;

  void runJavaScript(const QString &script,
                     std::function<void(const QVariant &)> callback = {}) override;

  qreal zoomFactor() const override;
  void setZoomFactor(qreal factor) override;

  void findText(const QString &text, bool forward = true, bool caseSensitive = false) override;
  void clearFind() override;
  void exitFullScreen() override;
  void print() override;
  void printToPdf(const QString &filePath) override;

  QIcon favicon() const;
  QIcon icon() const override { return favicon(); }

  WebKitWebView *rawWebView() const { return webView_; }
  bool nativeSurfaceMapped() const;
  void setManagedBackgroundPaths(const QString &imagePath, const QString &thumbnailPath) {
    managedBackgroundPath_ = imagePath;
    managedBackgroundThumbnailPath_ = thumbnailPath;
  }
  void setNewTabProfileData(dalinira::core::IBrowserProfileDataProvider *profileData);
  WebKitEngineProfile *profile() const { return profile_.data(); }

  // Registers DaliNira's application-owned internal pages (newtab, incognito,
  // blocked) on a WebKit context. Called once per WebKitEngineProfile.
  static void registerInternalScheme(WebKitWebContext *context);

  // A new window or tab the page opened (target=_blank, window.open). The
  // callback creates the browser tab and returns its view WITHOUT loading
  // anything: WebKit loads the popup into it, related to this view, so the
  // opener relationship (window.opener, the WindowProxy window.open returns,
  // window.close) works as on Chromium. The disposition follows the click
  // modifiers as QtWebEngine reports them.
  struct NewTabRequest {
    enum class Disposition { ForegroundTab, BackgroundTab, NewWindow };
    QUrl url;  // empty for about:blank popups
    Disposition disposition = Disposition::ForegroundTab;
  };
  using NewTabCallback = std::function<WebKitEngineView *(const NewTabRequest &)>;
  void setNewTabCallback(NewTabCallback cb) { newTabCallback_ = std::move(cb); }

  // Whether pages may run their own JavaScript (script elements, inline
  // handlers, javascript: URLs) for a top-level URL: the browser's JavaScript
  // setting and the per-site "block scripts" policy, the same decision as
  // Chromium's per-page JavascriptEnabled. DaliNira's own document scripts
  // keep running, as Chromium's isolated-world scripts do.
  using PageScriptPolicy = std::function<bool(const QUrl &)>;
  void setPageScriptPolicy(PageScriptPolicy policy) { pageScriptPolicy_ = std::move(policy); }
  bool pageScriptsAllowed() const { return pageScriptsAllowed_; }
  // Script-opened windows without a user gesture: Chromium's
  // JavascriptCanOpenWindows (the "pop-ups" content setting).
  void setScriptPopupsAllowed(bool allowed);

  // Document-start scripts (DaliNira content blocking) for a top-level URL.
  // Isolated-world scripts run in DaliNira's JS world, where the
  // daliniraBlocker message handler reports hits through contentBlockerHit();
  // main-world scripts (scriptlets) run in the page's world, as on Chromium.
  using DocumentScriptProvider = std::function<QList<DocumentScript>(const QUrl &)>;
  void setDocumentScriptProvider(DocumentScriptProvider provider) { documentScriptProvider_ = std::move(provider); }
  void setCredentialCaptureScript(const QString &source) { credentialCaptureScript_ = source; refreshDocumentScripts(); }
  // Re-query the provider (e.g. after a protection setting changed) and apply
  // the result to the current document as well as future loads.
  void refreshDocumentScripts() override;
  static const char *contentWorldName();

  // Decides this view's network requests (see WebKitNetworkFilter). The id is
  // the tab's blocker id, used for per-tab counters.
  void setNetworkRequestEvaluator(WebKitNetworkFilter::Evaluator evaluator);
  quint64 pageId() const { return pageId_; }
  // Decides a new window or tab this page asks for (target=_blank,
  // window.open) before one is created: false keeps it from opening.
  using PopupEvaluator = std::function<bool(const QUrl &target)>;
  void setPopupEvaluator(PopupEvaluator evaluator) { popupEvaluator_ = std::move(evaluator); }
  void armDeferredPopupCheck(PopupEvaluator evaluator) {
    deferredPopupEvaluator_ = std::move(evaluator);
  }
  void runIsolatedJavaScript(const QString &script, std::function<void(const QVariant &)> callback = {}) override;
  // The GTK key path (the plug's key-press-event) into the window's browser
  // shortcuts, for tests that cannot type into the native page.
  bool dispatchBrowserShortcutForTesting(unsigned int keyval, unsigned int state) {
    return dispatchBrowserShortcut(keyval, state);
  }
  // Sends a GTK motion event (as the X server would) to the page at `local`
  // (Qt coordinates of this view), through the same signal path.
  bool synthesizePagePointerForTesting(const QPoint &local);

 signals:
  void credentialMessage(const QString &type, const QVariantMap &payload);
  void deferredPopupBlocked();

 protected:
  bool event(QEvent *event) override;
  void focusInEvent(QFocusEvent *event) override;
  void paintEvent(QPaintEvent *event) override;
  void resizeEvent(QResizeEvent *event) override;
  void hideEvent(QHideEvent *event) override;
  void showEvent(QShowEvent *event) override;
  bool eventFilter(QObject *watched, QEvent *event) override;
  bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;

 private:
  void initWebKit();
  void teardownWebKit();
  void attachPlugToHost();
  void detachPlugFromHost();
  void resizeNativeSurface();
  void sendXEmbedMessage(long message, long detail = 0, long data1 = 0, long data2 = 0);
  bool nativeFocusInside() const;
  void restrictPlugKeyboardDelivery();
  void focusWebContent(Qt::FocusReason reason);
  void returnKeyboardFocusToQt(QWidget *target);
  bool dispatchBrowserShortcut(unsigned int keyval, unsigned int state);
  void forwardPagePointer(double rootX, double rootY);
  void serveInternalPage(WebKitURISchemeRequest *request);
  void serveInternalAsset(WebKitURISchemeRequest *request, const QUrl &url);
  void installDocumentScripts(const QUrl &url);
  void applyWebViewVisibility();
  bool discardWebContent();
  void restoreWebContent(bool navigateToCurrentEntry);
  void ensureAwake(bool navigateToCurrentEntry);
  void evaluateInContentWorld(const QString &script);
  void updateNetworkRoute();
  bool desiredPageScripts(const QUrl &url) const;
  bool applyPageScriptPolicy(const QUrl &url);

  QPointer<WebKitEngineProfile> profile_;
  dalinira::core::IBrowserProfileDataProvider *profileData_ = nullptr;
  GtkWidget *gtkWindow_ = nullptr;
  WebKitWebView *webView_ = nullptr;
  quintptr plugXid_ = 0;
  quintptr xembedAtom_ = 0;
  quintptr focusXid_ = 0;
  bool webFocused_ = false;
  bool isLoading_ = false;
  mutable QString cachedTitle_;
  mutable QUrl cachedUrl_;
  NewTabCallback newTabCallback_;
  WebKitSettings *viewSettings_ = nullptr;
  WebKitWebView *relatedOpener_ = nullptr;  // identity only: the opener this view was created for
  PageScriptPolicy pageScriptPolicy_;
  bool pageScriptsAllowed_ = true;
  bool scriptPopupsAllowed_ = false;
  QUrl scriptPolicyRestartUrl_;
  QString lastSearchText_;
  bool lastSearchCaseSensitive_ = false;
  int findActiveMatch_ = 0;
  int findMatchCount_ = 0;
  QString internalCapability_;
  QString managedBackgroundPath_;
  QString managedBackgroundThumbnailPath_;
  bool engineActive_ = false;
  WebKitUserContentManager *contentManager_ = nullptr;
  DocumentScriptProvider documentScriptProvider_;
  QString documentScriptKey_;
  QList<DocumentScript> installedScripts_;
  QStringList documentScripts_;  // Sources of the re-appliable isolated-world scripts.
  QString credentialCaptureScript_;
  WebKitNetworkFilter::Evaluator networkEvaluator_;
  PopupEvaluator popupEvaluator_;
  PopupEvaluator deferredPopupEvaluator_;
  // WebKit can ask about one popup twice (new-window policy, then create).
  QUrl lastPopupUrl_;
  bool lastPopupAllowed_ = true;
  qint64 lastPopupAtMs_ = 0;
  bool popupAllowed(const QUrl &target);
  void evaluateJavaScript(const QString &script, const char *world, std::function<void(const QVariant &)> callback);
  quint64 pageId_ = 0;
  TabLifecycleState lifecycleState_ = TabLifecycleState::Active;
  QByteArray discardedSession_;
  QIcon discardedIcon_;
  qreal discardedZoom_ = 1.0;
  QMetaObject::Connection focusChangedConnection_;
};

} // namespace dalinira::engine
