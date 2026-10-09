#include "engine/webkit/webkit_engine_view.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QApplication>
#include <QGuiApplication>
#include <QKeySequence>
#include <QPainter>
#include <QShortcut>
#include <QVBoxLayout>
#include <QWindow>
#include <utility>
#include <QResizeEvent>
#include <QFocusEvent>
#include <QHideEvent>
#include <QShowEvent>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QSettings>
#include <QTimer>
#include <QImage>
#include <QPixmap>
#include <QDebug>
#include <QUrlQuery>
#include <QUuid>
#include <QBuffer>

#include "core/adult_content_protection.h"
#include "core/browser_profile_data_provider.h"
#include "core/search_engine_definition.h"
#include "newtab/new_tab_html.h"
#include "newtab/new_tab_asset_resolver.h"
#include "newtab/new_tab_scheme.h"

#undef signals
#include <gtk/gtk.h>
#include <gtk/gtkx.h>
#include <gdk/gdkx.h>
#include <gdk/gdkkeysyms.h>
#include <webkit2/webkit2.h>
#include <jsc/jsc.h>
#include <cairo.h>
#include <xcb/xcb.h>
#include <X11/extensions/XInput2.h>

#undef None
#undef Status
#undef Bool
#undef KeyPress
#undef KeyRelease
#undef FocusIn
#undef FocusOut

namespace dalinira::engine {

namespace {

QIcon iconFromCairoSurface(cairo_surface_t *surf) {
  if (!surf) return QIcon();
  if (cairo_surface_get_type(surf) == CAIRO_SURFACE_TYPE_IMAGE) {
    const int w = cairo_image_surface_get_width(surf);
    const int h = cairo_image_surface_get_height(surf);
    const int stride = cairo_image_surface_get_stride(surf);
    const unsigned char *data = cairo_image_surface_get_data(surf);
    if (data && w > 0 && h > 0) {
      QImage img(data, w, h, stride, QImage::Format_ARGB32_Premultiplied);
      return QIcon(QPixmap::fromImage(img.copy()));
    }
  } else {
    const int w = 32;
    const int h = 32;
    QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    cairo_surface_t *target = cairo_image_surface_create_for_data(
        img.bits(), CAIRO_FORMAT_ARGB32, w, h, img.bytesPerLine());
    cairo_t *cr = cairo_create(target);
    cairo_set_source_surface(cr, surf, 0, 0);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_destroy(target);
    return QIcon(QPixmap::fromImage(img));
  }
  return QIcon();
}

bool isAdultProtectionActive() {
  return QSettings().value(QStringLiteral("privacy/adultContentProtection"), true).toBool();
}

QString webKitNewTabHtml(QString html, const QString &capability,
                         const QString &managedBackgroundPath,
                         const QString &managedThumbnailPath) {
  Q_UNUSED(managedThumbnailPath);
  // Images and icons are loaded as dalinira://newtab/... sub-resources from
  // the allow-list shared with Chromium (serveInternalPage), not inlined as
  // base64: inlining put ~6.6 MB of text into every New Tab document.
  if (!managedBackgroundPath.isEmpty() && QFileInfo::exists(managedBackgroundPath)) {
    const qint64 revision = QFileInfo(managedBackgroundPath).lastModified().toMSecsSinceEpoch();
    html.replace(QStringLiteral("<script>"),
                 QStringLiteral("<script>window.daliniraManagedBackgroundAvailable=true;"
                                "window.daliniraManagedBackgroundRevision=%1;").arg(revision));
  }

  // Chromium receives this per-page token from BrowserWindow. WebKit handles
  // the same internal navigation locally, but the shared page intentionally
  // keeps its submit button disabled until a token exists.
  html.replace(QStringLiteral("window.navigationCapability='';"),
               QStringLiteral("window.navigationCapability='%1';").arg(capability));
  // Suggestion commands (dalinira://suggest) are validated against the same
  // token in decide-policy, so the page can use it from the first keystroke.
  html.replace(QStringLiteral("let suggestionCapability='',"),
               QStringLiteral("let suggestionCapability='%1',").arg(capability));
  return html;
}

QUrl searchUrlForEngine(const QString &engine, const QString &text) {
  const auto &definition = dalinira::core::searchEngineDefinition(engine);
  QUrl url(QString::fromLatin1(definition.searchUrl));
  QUrlQuery query(url);
  query.addQueryItem(QString::fromLatin1(definition.queryParam), text);
  url.setQuery(query);
  return url;
}

void ensureGtkInitialized() {
  static bool s_initialized = false;
  if (!s_initialized) {
    qputenv("GDK_BACKEND", "x11");
    // The GStreamer media runtime is checked against the real plugin registry
    // by WebKitMediaRuntime (see WebKitEngineProfile), not by file paths,
    // which differ per distribution.
    int argc = 0;
    char **argv = nullptr;
    gtk_init_check(&argc, &argv);
    s_initialized = true;
  }
}

constexpr long kXEmbedEmbeddedNotify = 0;
constexpr long kXEmbedWindowActivate = 1;
constexpr long kXEmbedWindowDeactivate = 2;
constexpr long kXEmbedRequestFocus = 3;
constexpr long kXEmbedFocusIn = 4;
constexpr long kXEmbedFocusOut = 5;
constexpr long kXEmbedFocusCurrent = 0;

// The opener whose "create" signal is being answered. The browser creates the
// popup's tab (and with it the WebKitEngineView) synchronously inside that
// signal; the view's WebKitWebView must be constructed related to the opener.
WebKitWebView *s_pendingRelatedView = nullptr;

class RelatedViewScope {
 public:
  explicit RelatedViewScope(WebKitWebView *opener) : previous_(s_pendingRelatedView) { s_pendingRelatedView = opener; }
  ~RelatedViewScope() { s_pendingRelatedView = previous_; }
  RelatedViewScope(const RelatedViewScope &) = delete;
  RelatedViewScope &operator=(const RelatedViewScope &) = delete;

 private:
  WebKitWebView *previous_;
};

bool lifecycleDebug() {
  static const bool enabled = qEnvironmentVariableIntValue("DALINIRA_WEBKIT_DEBUG") == 1;
  return enabled;
}

#define WK_TRACE(...) \
  do { if (lifecycleDebug()) qWarning().nospace() << "[webkit-host " << static_cast<const void *>(this) << "] " << __VA_ARGS__; } while (0)

// Qt's xcb connection buffers requests. Before GTK (a separate Xlib
// connection) references a Qt-created window, make sure the X server has
// processed Qt's pending requests with a round trip.
void syncQtConnection() {
  auto *x11 = qGuiApp ? qGuiApp->nativeInterface<QNativeInterface::QX11Application>() : nullptr;
  xcb_connection_t *connection = x11 ? x11->connection() : nullptr;
  if (!connection) return;
  free(xcb_get_input_focus_reply(connection, xcb_get_input_focus(connection), nullptr));
}

// Qt (xcb) maps, unmaps and destroys the host window on its own connection, so
// a window GTK's connection just saw as viewable can be gone or unmapped by the
// time the X server processes GTK's next request: XSetInputFocus then fails with
// BadMatch, XReparentWindow/XQueryTree with BadWindow. GDK treats any untrapped
// X error as fatal and exits, so every request DaliNira sends on GTK's
// connection about a window Qt controls runs inside a GDK error trap. The
// failed request is harmless: focus stays where it was, and the next map,
// focus or WinIdChange retries it.
class GdkX11ErrorTrap {
 public:
  explicit GdkX11ErrorTrap(GtkWidget *widget) : display_(widget ? gtk_widget_get_display(widget) : nullptr) {
    if (display_) gdk_x11_display_error_trap_push(display_);
  }
  ~GdkX11ErrorTrap() {
    if (display_) gdk_x11_display_error_trap_pop_ignored(display_);
  }
  GdkX11ErrorTrap(const GdkX11ErrorTrap &) = delete;
  GdkX11ErrorTrap &operator=(const GdkX11ErrorTrap &) = delete;

 private:
  GdkDisplay *display_;
};

int qtKeyFromKeyval(guint keyval) {
  const guint upper = gdk_keyval_to_upper(keyval);
  if (upper >= GDK_KEY_A && upper <= GDK_KEY_Z) return Qt::Key_A + static_cast<int>(upper - GDK_KEY_A);
  if (keyval >= GDK_KEY_0 && keyval <= GDK_KEY_9) return Qt::Key_0 + static_cast<int>(keyval - GDK_KEY_0);
  if (keyval >= GDK_KEY_F1 && keyval <= GDK_KEY_F35) return Qt::Key_F1 + static_cast<int>(keyval - GDK_KEY_F1);
  switch (keyval) {
    case GDK_KEY_Tab:
    case GDK_KEY_ISO_Left_Tab: return Qt::Key_Tab;
    case GDK_KEY_Left: return Qt::Key_Left;
    case GDK_KEY_Right: return Qt::Key_Right;
    case GDK_KEY_Up: return Qt::Key_Up;
    case GDK_KEY_Down: return Qt::Key_Down;
    case GDK_KEY_Page_Up: return Qt::Key_PageUp;
    case GDK_KEY_Page_Down: return Qt::Key_PageDown;
    case GDK_KEY_Home: return Qt::Key_Home;
    case GDK_KEY_End: return Qt::Key_End;
    case GDK_KEY_plus:
    case GDK_KEY_KP_Add: return Qt::Key_Plus;
    case GDK_KEY_minus:
    case GDK_KEY_KP_Subtract: return Qt::Key_Minus;
    case GDK_KEY_equal: return Qt::Key_Equal;
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter: return Qt::Key_Return;
    default: return 0;
  }
}

} // namespace

WebKitEngineView::WebKitEngineView(WebKitEngineProfile *profile, QWidget *parent)
    : EngineView(parent), profile_(profile) {
  ensureGtkInitialized();
  initWebKit();
  if (profile_) {
    connect(profile_, &WebKitEngineProfile::faviconLoaded, this, [this](const QUrl &pageUrl, const QIcon &icon) {
      if (profileData_ && !icon.isNull()) {
        cacheNewTabFavicon(profileData_, pageUrl, icon);
      }
    });
  }
}

WebKitEngineView::~WebKitEngineView() {
  releaseNewTabFaviconProfile(profileData_);
  teardownWebKit();
  if (viewSettings_) g_object_unref(viewSettings_);
}

void WebKitEngineView::setNewTabProfileData(
    dalinira::core::IBrowserProfileDataProvider *profileData) {
  if (profileData_ == profileData) return;
  releaseNewTabFaviconProfile(profileData_);
  profileData_ = profileData;
  retainNewTabFaviconProfile(profileData_);
}

void WebKitEngineView::teardownWebKit() {
  engineActive_ = false;
  if (webFocused_) returnKeyboardFocusToQt(window());
  disconnect(focusChangedConnection_);
  if (QWindow *host = windowHandle()) host->removeEventFilter(this);
  if (contentManager_) {
    g_signal_handlers_disconnect_by_data(contentManager_, this);
    webkit_user_content_manager_unregister_script_message_handler_in_world(
        contentManager_, "daliniraBlocker", contentWorldName());
    webkit_user_content_manager_unregister_script_message_handler_in_world(
        contentManager_, "daliniraCredentials", contentWorldName());
  }
  if (pageId_ && profile_ && profile_->networkFilter()) profile_->networkFilter()->removeRoute(pageId_);
  pageId_ = 0;
  if (webView_) {
    g_object_set_data(G_OBJECT(webView_), "dalinira-engine-view", nullptr);
    // Find results arrive asynchronously from the web process.
    if (WebKitFindController *findCtrl = webkit_web_view_get_find_controller(webView_))
      g_signal_handlers_disconnect_by_data(findCtrl, this);
    g_signal_handlers_disconnect_by_data(webView_, this);
  }
  if (gtkWindow_) {
    g_signal_handlers_disconnect_by_data(gtkWindow_, this);
    // Qt (xcb) and GTK (Xlib) use separate X11 connections. GTK buffers its
    // unmap/destroy requests while ~QWidget destroys the host immediately on
    // Qt's connection, which also destroys the plug; GTK's late UnmapWindow
    // then fails with BadWindow and GDK exits the process. Park the plug on
    // the root window and flush GTK's requests before the host can go away.
    detachPlugFromHost();
    GdkDisplay *gdkDisplay = gtk_widget_get_display(gtkWindow_);
    gtk_widget_destroy(gtkWindow_);
    if (gdkDisplay) XSync(gdk_x11_display_get_xdisplay(gdkDisplay), False);
  }

  // These objects have explicit owner references taken in initWebKit(). The
  // X server may destroy the GtkPlug's native window before Qt destroys this
  // wrapper; keeping the GObjects alive prevents our raw members from turning
  // into dangling pointers during the delayed QWidget teardown.
  plugXid_ = 0;
  focusXid_ = 0;
  if (webView_) g_object_unref(webView_);
  webView_ = nullptr;
  if (contentManager_) g_object_unref(contentManager_);
  contentManager_ = nullptr;
  if (gtkWindow_) g_object_unref(gtkWindow_);
  gtkWindow_ = nullptr;
}

void WebKitEngineView::focusInEvent(QFocusEvent *event) {
  EngineView::focusInEvent(event);
  focusWebContent(event ? event->reason() : Qt::OtherFocusReason);
}

void WebKitEngineView::paintEvent(QPaintEvent *) {
  // Only visible for the moment before the GTK plug covers the host window.
  QPainter(this).fillRect(rect(), QColor(7, 17, 31));
}

bool WebKitEngineView::event(QEvent *event) {
  switch (event->type()) {
    case QEvent::WinIdChange:
      WK_TRACE("WinIdChange host=" << Qt::hex << internalWinId());
      attachPlugToHost();
      break;
    case QEvent::ParentAboutToChange:
      // Qt keeps a WA_NativeWindow widget's X11 window across reparenting; if
      // it recreates the host instead, SurfaceAboutToBeDestroyed parks the
      // plug and WinIdChange re-attaches it. Only keyboard focus needs care.
      if (webFocused_) returnKeyboardFocusToQt(window());
      break;
    default:
      break;
  }
  return EngineView::event(event);
}

bool WebKitEngineView::eventFilter(QObject *watched, QEvent *event) {
  if (watched == windowHandle() && event->type() == QEvent::PlatformSurface &&
      static_cast<QPlatformSurfaceEvent *>(event)->surfaceEventType() ==
          QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
    // Destroying an X11 window destroys its children. Move the GTK surface
    // out of the host before Qt drops it so GTK never loses its XID.
    WK_TRACE("host SurfaceAboutToBeDestroyed");
    detachPlugFromHost();
  }
  return EngineView::eventFilter(watched, event);
}

bool WebKitEngineView::nativeEvent(const QByteArray &eventType, void *message, qintptr *result) {
  if (eventType == "xcb_generic_event_t" && plugXid_) {
    auto *ev = static_cast<xcb_generic_event_t *>(message);
    if ((ev->response_type & ~0x80) == XCB_CLIENT_MESSAGE) {
      auto *cm = reinterpret_cast<xcb_client_message_event_t *>(ev);
      if (cm->type == static_cast<xcb_atom_t>(xembedAtom_) && cm->format == 32 &&
          cm->data.data32[1] == kXEmbedRequestFocus) {
        // XEmbed client (the GTK plug) asks the embedder for keyboard focus,
        // e.g. after a real click into a page text field.
        WK_TRACE("XEMBED_REQUEST_FOCUS webFocused=" << webFocused_);
        if (webFocused_ && nativeFocusInside()) {
          // Already focused: the protocol answer is to (re)confirm focus-in,
          // not to grab again, which would make GTK request focus again.
          sendXEmbedMessage(kXEmbedFocusIn, kXEmbedFocusCurrent);
        } else {
          focusWebContent(Qt::MouseFocusReason);
        }
        return true;
      }
    }
  }
  return EngineView::nativeEvent(eventType, message, result);
}

void WebKitEngineView::sendXEmbedMessage(long message, long detail, long data1, long data2) {
  if (!plugXid_ || !gtkWindow_) return;
  GdkWindow *plugWindow = gtk_widget_get_window(gtkWindow_);
  if (!plugWindow) return;
  Display *display = GDK_WINDOW_XDISPLAY(plugWindow);
  XEvent xev{};
  xev.xclient.type = ClientMessage;
  xev.xclient.window = static_cast<Window>(plugXid_);
  xev.xclient.message_type = static_cast<Atom>(xembedAtom_);
  xev.xclient.format = 32;
  xev.xclient.data.l[0] = CurrentTime;
  xev.xclient.data.l[1] = message;
  xev.xclient.data.l[2] = detail;
  xev.xclient.data.l[3] = data1;
  xev.xclient.data.l[4] = data2;
  XSendEvent(display, static_cast<Window>(plugXid_), False, NoEventMask, &xev);
  XFlush(display);
}

void WebKitEngineView::attachPlugToHost() {
  if (!gtkWindow_ || !plugXid_) return;
  GdkWindow *plugWindow = gtk_widget_get_window(gtkWindow_);
  if (!plugWindow) return;
  // Never call winId() here: during QWidget::destroy() Qt emits WinIdChange,
  // and winId() would recreate a native window inside the dying tree. With
  // no live host the plug simply stays parked on the root window.
  const WId host = internalWinId();
  if (!host || !windowHandle() || !windowHandle()->handle()) return;
  syncQtConnection();
  if (QWindow *handle = windowHandle()) {
    handle->removeEventFilter(this);
    handle->installEventFilter(this);
  }
  Display *display = GDK_WINDOW_XDISPLAY(plugWindow);
  GdkX11ErrorTrap trap(gtkWindow_);
  Window root = 0, parent = 0, *children = nullptr;
  unsigned int count = 0;
  if (XQueryTree(display, static_cast<Window>(plugXid_), &root, &parent, &children, &count)) {
    if (children) XFree(children);
    WK_TRACE("attach plug=" << Qt::hex << plugXid_ << " parent=" << parent << " host=" << host);
    if (parent != static_cast<Window>(host)) {
      XReparentWindow(display, static_cast<Window>(plugXid_), static_cast<Window>(host), 0, 0);
      sendXEmbedMessage(kXEmbedEmbeddedNotify, 0, static_cast<long>(host), 0);
    }
  }
  XMapWindow(display, static_cast<Window>(plugXid_));
  XFlush(display);
  resizeNativeSurface();
}

void WebKitEngineView::detachPlugFromHost() {
  if (!gtkWindow_ || !plugXid_) return;
  GdkWindow *plugWindow = gtk_widget_get_window(gtkWindow_);
  if (!plugWindow) return;
  if (webFocused_) returnKeyboardFocusToQt(window());
  Display *display = GDK_WINDOW_XDISPLAY(plugWindow);
  GdkX11ErrorTrap trap(gtkWindow_);
  XUnmapWindow(display, static_cast<Window>(plugXid_));
  XReparentWindow(display, static_cast<Window>(plugXid_), DefaultRootWindow(display), 0, 0);
  XSync(display, False);
}

void WebKitEngineView::resizeNativeSurface() {
  if (!gtkWindow_ || !plugXid_) return;
  GdkWindow *plugWindow = gtk_widget_get_window(gtkWindow_);
  if (!plugWindow) return;
  const qreal dpr = devicePixelRatioF();
  const int w = qMax(1, qRound(width() * dpr));
  const int h = qMax(1, qRound(height() * dpr));
  gtk_window_resize(GTK_WINDOW(gtkWindow_), w, h);
  Display *display = GDK_WINDOW_XDISPLAY(plugWindow);
  XMoveResizeWindow(display, static_cast<Window>(plugXid_), 0, 0,
                    static_cast<unsigned>(w), static_cast<unsigned>(h));
  XFlush(display);
}

void WebKitEngineView::restrictPlugKeyboardDelivery() {
  if (!gtkWindow_ || !plugXid_) return;
  GdkWindow *plugWindow = gtk_widget_get_window(gtkWindow_);
  if (!plugWindow) return;
  Display *display = GDK_WINDOW_XDISPLAY(plugWindow);
  const Window plug = static_cast<Window>(plugXid_);

  // X11 delivers a key event to the window under the pointer when that window
  // is an inferior of the focus window. The plug is an inferior of the Qt
  // top-level, so while it selected key events every keystroke typed with the
  // mouse over the page went to WebKit even when Qt (e.g. the omnibox) owned
  // the keyboard focus. GTK receives keys through its InputOnly focus window
  // instead, which only gets them while it actually holds the X11 focus.
  XWindowAttributes attributes{};
  if (XGetWindowAttributes(display, plug, &attributes)) {
    const long mask = attributes.your_event_mask & ~(KeyPressMask | KeyReleaseMask);
    if (mask != attributes.your_event_mask) XSelectInput(display, plug, mask);
  }
  int count = 0;
  if (XIEventMask *masks = XIGetSelectedEvents(display, plug, &count)) {
    for (int i = 0; i < count; ++i) {
      XIEventMask &m = masks[i];
      bool changed = false;
      for (int type : {XI_KeyPress, XI_KeyRelease}) {
        if (m.mask_len > (type >> 3) && XIMaskIsSet(m.mask, type)) {
          XIClearMask(m.mask, type);
          changed = true;
        }
      }
      if (changed) XISelectEvents(display, plug, &m, 1);
    }
    XFree(masks);
  }

  Window root = 0, parent = 0, *children = nullptr;
  unsigned int n = 0;
  focusXid_ = plugXid_;
  if (XQueryTree(display, plug, &root, &parent, &children, &n)) {
    for (unsigned int i = 0; i < n; ++i) {
      XWindowAttributes child{};
      if (XGetWindowAttributes(display, children[i], &child) && child.c_class == InputOnly &&
          child.width == 1 && child.height == 1) {
        focusXid_ = static_cast<quintptr>(children[i]);
        break;
      }
    }
    if (children) XFree(children);
  }
  XFlush(display);
  WK_TRACE("keyboard delivery restricted, focus target=" << Qt::hex << focusXid_);
}

bool WebKitEngineView::nativeFocusInside() const {
  if (!gtkWindow_ || !plugXid_) return false;
  GdkWindow *plugWindow = gtk_widget_get_window(gtkWindow_);
  if (!plugWindow) return false;
  Display *display = GDK_WINDOW_XDISPLAY(plugWindow);
  GdkX11ErrorTrap trap(gtkWindow_);
  Window focused = 0;
  int revertTo = 0;
  XGetInputFocus(display, &focused, &revertTo);

  // WebKitGTK focuses either its plug or the 1x1 InputOnly child discovered
  // by restrictPlugKeyboardDelivery(). Do not walk a possibly changing X11
  // ancestry here: during a drag GTK can destroy/recreate that focus child
  // between XQueryTree and XFree. The resulting stale reply was the source of
  // the side-panel drag SIGABRT seen in the coredump.
  return focused == static_cast<Window>(plugXid_) ||
         (focusXid_ && focused == static_cast<Window>(focusXid_));
}

void WebKitEngineView::returnKeyboardFocusToQt(QWidget *target) {
  if (!gtkWindow_ || !plugXid_) return;
  WK_TRACE("returnKeyboardFocusToQt webFocused=" << webFocused_ << " target=" << (target ? target->metaObject()->className() : "null"));
  if (webFocused_) {
    webFocused_ = false;
    sendXEmbedMessage(kXEmbedFocusOut);
    sendXEmbedMessage(kXEmbedWindowDeactivate);
  }
  QWidget *topLevel = target ? target->window() : nullptr;
  // A hidden or minimized top-level is not viewable; X11 then reverts the
  // focus itself when the plug's ancestor is unmapped.
  if (!topLevel || !topLevel->internalWinId() || !topLevel->isVisible() || topLevel->isMinimized() ||
      !nativeFocusInside())
    return;
  GdkWindow *plugWindow = gtk_widget_get_window(gtkWindow_);
  Display *display = GDK_WINDOW_XDISPLAY(plugWindow);
  // The compositor still considers the browser top-level active, so Qt's own
  // activation request is a no-op; hand the X11 keyboard focus back
  // explicitly or keys keep flowing into the GTK surface.
  GdkX11ErrorTrap trap(gtkWindow_);
  XSetInputFocus(display, static_cast<Window>(topLevel->internalWinId()), RevertToParent, CurrentTime);
  XFlush(display);
}

void WebKitEngineView::resizeEvent(QResizeEvent *event) {
  EngineView::resizeEvent(event);
  resizeNativeSurface();
}

bool WebKitEngineView::dispatchBrowserShortcut(unsigned int keyval, unsigned int state) {
  const bool ctrl = state & GDK_CONTROL_MASK;
  const bool alt = state & GDK_MOD1_MASK;
  const bool functionKey = keyval >= GDK_KEY_F1 && keyval <= GDK_KEY_F35;
  if (!ctrl && !alt && !functionKey) return false;
  const int key = qtKeyFromKeyval(keyval);
  if (!key) return false;
  Qt::KeyboardModifiers modifiers;
  if (ctrl) modifiers |= Qt::ControlModifier;
  if (alt) modifiers |= Qt::AltModifier;
  if (state & GDK_SHIFT_MASK) modifiers |= Qt::ShiftModifier;
  const QKeySequence sequence(QKeyCombination(modifiers, static_cast<Qt::Key>(key)));

  QWidget *top = window();
  if (!top) return false;
  const auto shortcuts = top->findChildren<QShortcut *>();
  for (QShortcut *shortcut : shortcuts) {
    if (!shortcut->isEnabled() || !shortcut->keys().contains(sequence)) continue;
    if (shortcut->context() != Qt::WindowShortcut && shortcut->context() != Qt::ApplicationShortcut) continue;
    auto *owner = qobject_cast<QWidget *>(shortcut->parent());
    if (owner && !owner->isVisible()) continue;
    // Browser chrome shortcuts win over page handlers, matching QtWebEngine's
    // ShortcutOverride behaviour on the Chromium backend.
    QMetaObject::invokeMethod(shortcut, "activated", Qt::QueuedConnection);
    return true;
  }
  return false;
}

void WebKitEngineView::initWebKit() {
  // Architecture: this QWidget is a Qt-owned native X11 window. Qt alone maps,
  // unmaps and positions it (QStackedWidget visibility, layouts). The GTK plug
  // is created inside that host, mapped once and resized to fill it; it never
  // gets mapped/unmapped independently, so a hidden tab can never cover or
  // steal input from DaliNira's Qt chrome.
  setAttribute(Qt::WA_NativeWindow, true);
  setAttribute(Qt::WA_OpaquePaintEvent, true);
  setAttribute(Qt::WA_NoSystemBackground, true);
  setFocusPolicy(Qt::StrongFocus);
  const WId host = winId();
  syncQtConnection();

  gtkWindow_ = gtk_plug_new(static_cast<Window>(host));
  // GtkPlug is a top-level widget and otherwise relies on GTK's implicit
  // lifetime. Hold it explicitly because its foreign X11 host can disappear
  // independently of the C++ QWidget wrapper.
  g_object_ref_sink(gtkWindow_);
  gtk_widget_set_can_focus(gtkWindow_, TRUE);

  // Every view owns its settings (the page script policy is per tab) and its
  // user content manager (its document scripts and message handlers), also a
  // popup related to an opener, which would otherwise share the opener's.
  if (!viewSettings_) viewSettings_ = profile_ ? profile_->createViewSettings() : webkit_settings_new();
  webkit_settings_set_enable_javascript_markup(viewSettings_, pageScriptsAllowed_);
  webkit_settings_set_javascript_can_open_windows_automatically(viewSettings_, scriptPopupsAllowed_);
  WebKitUserContentManager *contentManager = webkit_user_content_manager_new();
  // Consumed once: a later re-creation (waking from sleep) is a fresh page.
  relatedOpener_ = std::exchange(s_pendingRelatedView, nullptr);
  if (relatedOpener_) {
    webView_ = WEBKIT_WEB_VIEW(g_object_new(
        WEBKIT_TYPE_WEB_VIEW,
        "related-view", relatedOpener_,
        "settings", viewSettings_,
        "user-content-manager", contentManager,
        nullptr));
  } else if (profile_ && profile_->webContext()) {
    webView_ = WEBKIT_WEB_VIEW(g_object_new(
        WEBKIT_TYPE_WEB_VIEW,
        "web-context", profile_->webContext(),
        "settings", viewSettings_,
        "user-content-manager", contentManager,
        nullptr));
  } else {
    webView_ = WEBKIT_WEB_VIEW(g_object_new(
        WEBKIT_TYPE_WEB_VIEW, "settings", viewSettings_, "user-content-manager", contentManager, nullptr));
  }
  g_object_ref_sink(webView_);
  g_object_unref(contentManager);
  g_object_set_data(G_OBJECT(webView_), "dalinira-engine-view", this);
  updateNetworkRoute();
  // The page id changes when WebKit swaps the page into a new web process.
  g_signal_connect(webView_, "notify::page-id", G_CALLBACK(+[](WebKitWebView *, GParamSpec *, gpointer data) {
    static_cast<WebKitEngineView *>(data)->updateNetworkRoute();
  }), this);

  // Blocker hits arrive only from DaliNira's isolated world; page scripts in
  // the main world cannot see this handler and therefore cannot inflate stats.
  contentManager_ = webkit_web_view_get_user_content_manager(webView_);
  g_object_ref(contentManager_);
  webkit_user_content_manager_register_script_message_handler_in_world(
      contentManager_, "daliniraBlocker", contentWorldName());
  g_signal_connect(contentManager_, "script-message-received::daliniraBlocker",
                   G_CALLBACK(+[](WebKitUserContentManager *, WebKitJavascriptResult *result, gpointer data) {
    auto *self = static_cast<WebKitEngineView *>(data);
    JSCValue *message = result ? webkit_javascript_result_get_js_value(result) : nullptr;
    if (!self || !message || !jsc_value_is_object(message)) return;
    JSCValue *countValue = jsc_value_object_get_property(message, "count");
    JSCValue *typeValue = jsc_value_object_get_property(message, "type");
    const double count = jsc_value_is_number(countValue) ? jsc_value_to_double(countValue) : 0.0;
    QString kind;
    if (jsc_value_is_string(typeValue)) {
      gchar *text = jsc_value_to_string(typeValue);
      kind = QString::fromUtf8(text);
      g_free(text);
    }
    g_object_unref(countValue);
    g_object_unref(typeValue);
    if (count < 1.0 || count > 10000.0 || count != static_cast<double>(static_cast<quint64>(count)) ||
        (kind != QLatin1String("cosmetic") && kind != QLatin1String("procedural")))
      return;
    emit self->contentBlockerHit(static_cast<quint64>(count), kind);
  }), this);
  webkit_user_content_manager_register_script_message_handler_in_world(
      contentManager_, "daliniraCredentials", contentWorldName());
  g_signal_connect(contentManager_, "script-message-received::daliniraCredentials",
                   G_CALLBACK(+[](WebKitUserContentManager *, WebKitJavascriptResult *result, gpointer data) {
    auto *self = static_cast<WebKitEngineView *>(data);
    JSCValue *message = result ? webkit_javascript_result_get_js_value(result) : nullptr;
    if (!self || !message || !jsc_value_is_object(message)) return;
    gchar *json = jsc_value_to_json(message, 0);
    const QJsonObject object = QJsonDocument::fromJson(QByteArray(json ? json : "")).object();
    g_free(json);
    const QString type = object.value(QStringLiteral("type")).toString();
    const QVariantMap payload = object.value(QStringLiteral("payload")).toObject().toVariantMap();
    const bool allowed = type == QLatin1String("candidate") || type == QLatin1String("submit") ||
                         type == QLatin1String("stage") || type == QLatin1String("success") ||
                         type == QLatin1String("state") || type == QLatin1String("fill");
    if (!allowed || payload.isEmpty() ||
        QJsonDocument::fromVariant(payload).toJson(QJsonDocument::Compact).size() > 16 * 1024) return;
    emit self->credentialMessage(type, payload);
  }), this);
  installDocumentScripts(QUrl());

  gtk_container_add(GTK_CONTAINER(gtkWindow_), GTK_WIDGET(webView_));
  gtk_widget_set_can_focus(GTK_WIDGET(webView_), TRUE);
  const GdkRGBA nativeBackground{7.0 / 255.0, 17.0 / 255.0, 31.0 / 255.0, 1.0};
  webkit_web_view_set_background_color(webView_, &nativeBackground);
  gtk_widget_show_all(gtkWindow_);

  GdkWindow *plugWindow = gtk_widget_get_window(gtkWindow_);
  Display *display = GDK_WINDOW_XDISPLAY(plugWindow);
  plugXid_ = static_cast<quintptr>(gdk_x11_window_get_xid(plugWindow));
  xembedAtom_ = static_cast<quintptr>(XInternAtom(display, "_XEMBED", False));
  // XEmbed: the embedder maps the client once and announces the embedding.
  sendXEmbedMessage(kXEmbedEmbeddedNotify, 0, static_cast<long>(host), 0);
  attachPlugToHost();
  restrictPlugKeyboardDelivery();

  focusChangedConnection_ = connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
    if (now && now != this && !isAncestorOf(now)) returnKeyboardFocusToQt(now);
  });

  g_signal_connect(gtkWindow_, "key-press-event", G_CALLBACK(+[](GtkWidget *, GdkEventKey *event, gpointer data) -> gboolean {
    auto *self = static_cast<WebKitEngineView *>(data);
    if (self && lifecycleDebug() && event) {
      qWarning().nospace() << "[webkit-host " << static_cast<const void *>(self) << "] gtk key-press keyval=" << Qt::hex << event->keyval
                           << " send_event=" << event->send_event
                           << " windowActive=" << gtk_window_is_active(GTK_WINDOW(self->gtkWindow_))
                           << " toplevelFocus=" << gtk_window_has_toplevel_focus(GTK_WINDOW(self->gtkWindow_))
                           << " viewHasFocus=" << gtk_widget_has_focus(GTK_WIDGET(self->webView_))
                           << " focusWidget=" << (gtk_window_get_focus(GTK_WINDOW(self->gtkWindow_)) == GTK_WIDGET(self->webView_));
    }
    return self && event && self->dispatchBrowserShortcut(event->keyval, event->state) ? TRUE : FALSE;
  }), this);

  g_signal_connect(webView_, "button-release-event", G_CALLBACK(+[](GtkWidget *, GdkEventButton *, gpointer data) -> gboolean {
    auto *self = static_cast<WebKitEngineView *>(data);
    if (self) {
      QTimer::singleShot(0, self, [self] { self->focusWebContent(Qt::MouseFocusReason); });
    }
    return FALSE;
  }), this);

  // Wire GObject signals to Qt EngineView signals
  g_signal_connect(webView_, "notify::uri", G_CALLBACK(+[](WebKitWebView *wv, GParamSpec *, gpointer data) {
    auto *self = static_cast<WebKitEngineView *>(data);
    const char *uriStr = webkit_web_view_get_uri(wv);
    if (!uriStr || !*uriStr) return;
    const QUrl newUrl(QString::fromUtf8(uriStr));
    if (!newUrl.isValid() || newUrl.isEmpty()) return;
    if (newUrl.toString() == QLatin1String("about:blank") && self->isLoading_ &&
        self->cachedUrl_.isValid() && self->cachedUrl_.toString() != QLatin1String("about:blank")) {
      return;
    }
    self->cachedUrl_ = newUrl;
    emit self->urlChanged(self->cachedUrl_);
  }), this);

  g_signal_connect(webView_, "notify::title", G_CALLBACK(+[](WebKitWebView *wv, GParamSpec *, gpointer data) {
    auto *self = static_cast<WebKitEngineView *>(data);
    const char *titleStr = webkit_web_view_get_title(wv);
    // A page that is (re)loading, e.g. waking from sleep, keeps its known
    // title until the document provides one.
    if ((!titleStr || !*titleStr) && self->isLoading_ && !self->cachedTitle_.isEmpty()) return;
    self->cachedTitle_ = titleStr ? QString::fromUtf8(titleStr) : QString{};
    emit self->titleChanged(self->cachedTitle_);
  }), this);

  g_signal_connect(webView_, "notify::estimated-load-progress", G_CALLBACK(+[](WebKitWebView *wv, GParamSpec *, gpointer data) {
    auto *self = static_cast<WebKitEngineView *>(data);
    const double progress = webkit_web_view_get_estimated_load_progress(wv);
    emit self->loadProgress(qBound(0, qRound(progress * 100.0), 100));
  }), this);

  g_signal_connect(webView_, "notify::favicon", G_CALLBACK(+[](WebKitWebView *wv, GParamSpec *, gpointer data) {
    auto *self = static_cast<WebKitEngineView *>(data);
    if (!self) return;
    // A null favicon means the page has none (or it was cleared by a new
    // navigation); report it so the tab can switch to its fallback icon.
    cairo_surface_t *surf = webkit_web_view_get_favicon(wv);
    emit self->iconChanged(surf ? iconFromCairoSurface(surf) : QIcon());
  }), this);

  // Tabs playing sound are protected from sleeping by the shared policy.
  g_signal_connect(webView_, "notify::is-playing-audio", G_CALLBACK(+[](WebKitWebView *wv, GParamSpec *, gpointer data) {
    auto *self = static_cast<WebKitEngineView *>(data);
    if (self) emit self->audibleChanged(webkit_web_view_is_playing_audio(wv));
  }), this);

  g_signal_connect(webView_, "load-changed", G_CALLBACK(+[](WebKitWebView *wv, WebKitLoadEvent event, gpointer data) {
    auto *self = static_cast<WebKitEngineView *>(data);
    switch (event) {
      case WEBKIT_LOAD_STARTED: {
        self->isLoading_ = true;
        const char *uriStr = webkit_web_view_get_uri(wv);
        if (uriStr && *uriStr && strcmp(uriStr, "about:blank") != 0) {
          self->cachedUrl_ = QUrl(QString::fromUtf8(uriStr));
        }
        // load-changed is main-frame only; install before the document exists.
        self->installDocumentScripts(self->cachedUrl_);
        emit self->loadStarted();
        break;
      }
      case WEBKIT_LOAD_REDIRECTED: {
        const char *uriStr = webkit_web_view_get_uri(wv);
        if (uriStr && *uriStr) {
          const QUrl redUrl(QString::fromUtf8(uriStr));
          if (isAdultProtectionActive() && dalinira::core::AdultContentProtectionService::instance().isBlocked(redUrl)) {
            webkit_web_view_stop_loading(wv);
            self->load(QUrl(QStringLiteral("dalinira://blocked/adult")));
            return;
          }
          self->cachedUrl_ = redUrl;
          self->installDocumentScripts(redUrl);
          emit self->urlChanged(self->cachedUrl_);
        }
        break;
      }
      case WEBKIT_LOAD_COMMITTED: {
        const char *uriStr = webkit_web_view_get_uri(wv);
        if (uriStr && *uriStr) {
          self->cachedUrl_ = QUrl(QString::fromUtf8(uriStr));
          const QString committedKey = self->cachedUrl_.scheme().toLower() + QStringLiteral("://") +
                                       self->cachedUrl_.host().toLower();
          // The committed host can differ from the one scripts were installed
          // for; the document already exists, so apply to it directly.
          if (committedKey != self->documentScriptKey_) self->refreshDocumentScripts();
          emit self->urlChanged(self->cachedUrl_);
        }
        break;
      }
      case WEBKIT_LOAD_FINISHED: {
        self->isLoading_ = false;
        const char *uriStr = webkit_web_view_get_uri(wv);
        const char *titleStr = webkit_web_view_get_title(wv);
        if (uriStr && *uriStr) self->cachedUrl_ = QUrl(QString::fromUtf8(uriStr));
        if (titleStr && *titleStr) self->cachedTitle_ = QString::fromUtf8(titleStr);
        emit self->titleChanged(self->cachedTitle_);
        emit self->urlChanged(self->cachedUrl_);
        cairo_surface_t *surf = webkit_web_view_get_favicon(wv);
        if (surf) {
          const QIcon ico = iconFromCairoSurface(surf);
          if (!ico.isNull()) {
            emit self->iconChanged(ico);
          }
        }
        // Safety net if the document-start install raced the commit. The
        // runtime is idempotent, so this is a no-op when it already runs.
        if (!self->documentScripts_.isEmpty())
          self->evaluateInContentWorld(QStringLiteral("if(!window.__daliniraCosmeticRuntime){") +
                                       self->documentScripts_.join(QLatin1Char('\n')) + QStringLiteral("\n}"));
        emit self->loadFinished(true);
        break;
      }
    }
  }), this);

  g_signal_connect(webView_, "load-failed", G_CALLBACK(+[](WebKitWebView *, WebKitLoadEvent, gchar *, GError *error, gpointer data) -> gboolean {
    auto *self = static_cast<WebKitEngineView *>(data);
    self->isLoading_ = false;
    if (error) {
      qWarning() << "WebKitBackend: Navigation load failed:" << error->message;
    }
    emit self->loadFinished(false);
    return FALSE;
  }), this);

  // New windows and tabs (target=_blank, window.open). WebKit asks for the
  // view to load the popup into; returning a view related to this one keeps
  // the opener relationship. Returning null cancels the popup (window.open
  // then returns null, as for a blocked popup on Chromium).
  g_signal_connect(webView_, "create", G_CALLBACK(+[](WebKitWebView *wv, WebKitNavigationAction *navAction, gpointer data) -> GtkWidget* {
    auto *self = static_cast<WebKitEngineView *>(data);
    if (!self) return nullptr;

    WebKitURIRequest *request = webkit_navigation_action_get_request(navAction);
    const gchar *target = request ? webkit_uri_request_get_uri(request) : nullptr;
    const bool deferredPopup = !target || !*target || g_strcmp0(target, "about:blank") == 0;
    const QUrl targetUrl = deferredPopup ? QUrl() : QUrl(QString::fromUtf8(target));
    if (!deferredPopup) {
      if (isAdultProtectionActive() && dalinira::core::AdultContentProtectionService::instance().isBlocked(targetUrl))
        return nullptr;
      // $popup rules decide before any window exists.
      if (!self->popupAllowed(targetUrl)) return nullptr;
    }

    NewTabRequest tabRequest;
    tabRequest.url = targetUrl;
    const guint modifiers = webkit_navigation_action_get_modifiers(navAction);
    const guint button = webkit_navigation_action_get_mouse_button(navAction);
    if (modifiers & GDK_SHIFT_MASK) tabRequest.disposition = NewTabRequest::Disposition::NewWindow;
    else if ((modifiers & GDK_CONTROL_MASK) || button == 2) tabRequest.disposition = NewTabRequest::Disposition::BackgroundTab;
    WebKitEngineView *newView = nullptr;
    if (self->newTabCallback_) {
      RelatedViewScope related(wv);
      newView = self->newTabCallback_(tabRequest);
    }
    if (!newView || !newView->rawWebView()) {
      // No tab host (engine tests): open it as a plain new tab, without opener.
      if (!deferredPopup) emit self->newWindowRequested(targetUrl);
      return nullptr;
    }
    if (newView->relatedOpener_ != wv)
      qWarning() << "WebKitBackend: popup view is not related to its opener; window.opener will be missing";
    if (deferredPopup) newView->armDeferredPopupCheck(self->popupEvaluator_);
    // The popup's document follows its own top-level URL's script policy.
    if (!deferredPopup) newView->applyPageScriptPolicy(targetUrl);
    return GTK_WIDGET(newView->rawWebView());
  }), this);

  // window.close() from a window a script opened.
  g_signal_connect(webView_, "close", G_CALLBACK(+[](WebKitWebView *, gpointer data) {
    auto *self = static_cast<WebKitEngineView *>(data);
    if (!self) return;
    QMetaObject::invokeMethod(self, [self] { emit self->windowCloseRequested(); }, Qt::QueuedConnection);
  }), this);

  // Policy decisions: intercept Adult Content Protection, route new window actions, and allow legitimate web content
  g_signal_connect(webView_, "decide-policy", G_CALLBACK(+[](WebKitWebView *wv, WebKitPolicyDecision *decision, WebKitPolicyDecisionType decisionType, gpointer data) -> gboolean {
    auto *self = static_cast<WebKitEngineView *>(data);
    if (!self) return FALSE;

    if (decisionType == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
      auto *navDecision = WEBKIT_NAVIGATION_POLICY_DECISION(decision);
      WebKitNavigationAction *action = webkit_navigation_policy_decision_get_navigation_action(navDecision);
      WebKitURIRequest *req = webkit_navigation_action_get_request(action);
      const gchar *uri = req ? webkit_uri_request_get_uri(req) : nullptr;
      if (uri && *uri) {
        const QUrl targetUrl(QString::fromUtf8(uri));
        if (self->deferredPopupEvaluator_ &&
            (targetUrl.scheme() == QLatin1String("http") ||
             targetUrl.scheme() == QLatin1String("https"))) {
          auto evaluator = std::move(self->deferredPopupEvaluator_);
          self->deferredPopupEvaluator_ = {};
          if (!evaluator(targetUrl)) {
            webkit_policy_decision_ignore(decision);
            emit self->deferredPopupBlocked();
            return TRUE;
          }
        }
        if (targetUrl.scheme() == QLatin1String("dalinira")) {
          const QString host = targetUrl.host().toLower();
          if (host == QLatin1String("audio-effects") || host == QLatin1String("eq-presets") ||
              host == QLatin1String("downloads") || host == QLatin1String("settings") ||
              host == QLatin1String("passwords")) {
            webkit_policy_decision_ignore(decision);
            emit self->internalNavigationRequested(targetUrl);
            return TRUE;
          }
          const QUrlQuery query(targetUrl);
          const bool trustedInternalPage = self->cachedUrl_.host() == QLatin1String("newtab") ||
                                           self->cachedUrl_.host() == QLatin1String("incognito");
          const bool validCapability = !self->internalCapability_.isEmpty() &&
              query.queryItemValue(QStringLiteral("cap")) == self->internalCapability_;

          if (host == QLatin1String("navigate")) {
            const QString text = query.queryItemValue(QStringLiteral("q"), QUrl::FullyDecoded)
                                     .trimmed().left(2048);
            const QString engine = query.queryItemValue(QStringLiteral("engine"), QUrl::FullyDecoded);
            if (!text.isEmpty() && trustedInternalPage && validCapability) {
              webkit_policy_decision_ignore(decision);
              self->load(searchUrlForEngine(engine, text));
              return TRUE;
            }
            webkit_policy_decision_ignore(decision);
            return TRUE;
          }
          if (host == QLatin1String("search-engine") || host == QLatin1String("card-settings") ||
              host == QLatin1String("background-preference") ||
              host == QLatin1String("newtab-background") || host == QLatin1String("suggest") ||
              host == QLatin1String("frequent-sites")) {
            if (trustedInternalPage && validCapability && targetUrl.toString().size() <= 4096)
              emit self->internalNavigationRequested(targetUrl);
            webkit_policy_decision_ignore(decision);
            return TRUE;
          }
        }
        // Adult Content Protection: intercept main-frame navigation before content begins loading
        if (isAdultProtectionActive() && dalinira::core::AdultContentProtectionService::instance().isBlocked(targetUrl)) {
          webkit_policy_decision_ignore(decision);
          self->load(QUrl(QStringLiteral("dalinira://blocked/adult")));
          return TRUE;
        }
      }
      return FALSE;
    }

    if (decisionType == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {
      auto *navDecision = WEBKIT_NAVIGATION_POLICY_DECISION(decision);
      WebKitNavigationAction *action = webkit_navigation_policy_decision_get_navigation_action(navDecision);
      WebKitURIRequest *req = webkit_navigation_action_get_request(action);
      const gchar *uri = req ? webkit_uri_request_get_uri(req) : nullptr;
      if (uri && *uri && g_strcmp0(uri, "about:blank") != 0) {
        const QUrl targetUrl(QString::fromUtf8(uri));
        if (isAdultProtectionActive() && dalinira::core::AdultContentProtectionService::instance().isBlocked(targetUrl)) {
          webkit_policy_decision_ignore(decision);
          return TRUE;
        }
        if (!self->popupAllowed(targetUrl)) {
          webkit_policy_decision_ignore(decision);
          return TRUE;
        }
      }
      // Allowed: WebKit continues with "create", which opens the tab with
      // the opener relationship intact.
      return FALSE;
    }

    if (decisionType == WEBKIT_POLICY_DECISION_TYPE_RESPONSE) {
      auto *respDecision = WEBKIT_RESPONSE_POLICY_DECISION(decision);
      WebKitURIResponse *resp = webkit_response_policy_decision_get_response(respDecision);
      QUrl resUrl;
      if (resp) {
        const gchar *uri = webkit_uri_response_get_uri(resp);
        if (uri && *uri) {
          resUrl = QUrl(QString::fromUtf8(uri));
          if (isAdultProtectionActive() && dalinira::core::AdultContentProtectionService::instance().isBlocked(resUrl)) {
            webkit_policy_decision_ignore(decision);
            return TRUE;
          }
        }
      }
      if (!webkit_response_policy_decision_is_mime_type_supported(respDecision)) {
        webkit_policy_decision_download(decision);
        return TRUE;
      }
      // The top-level document's script policy, decided before it exists.
      // Navigation actions cannot be told apart from subframe ones, so the
      // main resource's response is the authoritative point. A page-started
      // cross-site navigation may already have a new web process, created
      // with the previous page's value; restart that load so it starts with
      // the right one (only when the policy differs between the two sites).
      if (resUrl.isValid() && webkit_response_policy_decision_is_main_frame_main_resource(respDecision) &&
          self->desiredPageScripts(resUrl) != self->pageScriptsAllowed_) {
        self->applyPageScriptPolicy(resUrl);
        WebKitURIRequest *request = webkit_response_policy_decision_get_request(respDecision);
        const char *method = request ? webkit_uri_request_get_http_method(request) : nullptr;
        const bool restartable = !method || g_ascii_strcasecmp(method, "GET") == 0;
        if (restartable && self->scriptPolicyRestartUrl_ != resUrl) {
          self->scriptPolicyRestartUrl_ = resUrl;
          webkit_policy_decision_ignore(decision);
          QTimer::singleShot(0, self, [self, resUrl] {
            if (!self->webView_) return;
            const QByteArray bytes = resUrl.toString().toUtf8();
            webkit_web_view_load_uri(self->webView_, bytes.constData());
          });
          return TRUE;
        }
      }
      if (webkit_response_policy_decision_is_main_frame_main_resource(respDecision)) self->scriptPolicyRestartUrl_.clear();
      return FALSE;
    }
    return FALSE;
  }), this);

  // Default safe denial: do not auto-grant camera, mic, geolocation, notifications
  g_signal_connect(webView_, "permission-request", G_CALLBACK(+[](WebKitWebView *, WebKitPermissionRequest *request, gpointer) -> gboolean {
    webkit_permission_request_deny(request);
    return TRUE;
  }), this);

  // Phase 11: TLS certificate error handling - strictly fail
  g_signal_connect(webView_, "load-failed-with-tls-errors", G_CALLBACK(+[](WebKitWebView *wv, const gchar *failingUri, GTlsCertificate *cert, GTlsCertificateFlags errors, gpointer data) -> gboolean {
    auto *self = static_cast<WebKitEngineView *>(data);
    const QUrl failedUrl(QString::fromUtf8(failingUri ? failingUri : ""));
    qWarning() << "WebKitBackend: TLS certificate validation failure for host" << failedUrl.host()
               << "(error flags:" << errors << ") - connection refused for security.";
    self->isLoading_ = false;
    emit self->loadFinished(false);
    return FALSE;
  }), this);

  // Phase 14: Web process termination / crash handling
  g_signal_connect(webView_, "web-process-terminated", G_CALLBACK(+[](WebKitWebView *wv, WebKitWebProcessTerminationReason reason, gpointer data) {
    auto *self = static_cast<WebKitEngineView *>(data);
    qWarning() << "WebKitBackend: Web process terminated with reason:" << reason;
    self->isLoading_ = false;
    emit self->webProcessTerminated(static_cast<int>(reason));
  }), this);

  // Element fullscreen (e.g. a video's fullscreen button). WebKit fullscreens
  // the view's toplevel, which here is the embedded GtkPlug; the Qt browser
  // window is what must go fullscreen, so report both transitions to it.
  g_signal_connect(webView_, "enter-fullscreen", G_CALLBACK(+[](WebKitWebView *, gpointer data) -> gboolean {
    auto *self = static_cast<WebKitEngineView *>(data);
    if (self) emit self->fullScreenRequested(true);
    return FALSE;
  }), this);
  g_signal_connect(webView_, "leave-fullscreen", G_CALLBACK(+[](WebKitWebView *, gpointer data) -> gboolean {
    auto *self = static_cast<WebKitEngineView *>(data);
    if (self) emit self->fullScreenRequested(false);
    return FALSE;
  }), this);

  // Find in page controller
  WebKitFindController *findCtrl = webkit_web_view_get_find_controller(webView_);
  if (findCtrl) {
    g_signal_connect(findCtrl, "found-text", G_CALLBACK(+[](WebKitFindController *, guint match_count, gpointer data) {
      auto *self = static_cast<WebKitEngineView *>(data);
      self->findMatchCount_ = static_cast<int>(match_count);
      self->findActiveMatch_ = self->findMatchCount_ > 0 ? 1 : 0;
      emit self->findResultFound(self->findActiveMatch_, self->findMatchCount_);
    }), this);

    g_signal_connect(findCtrl, "failed-to-find-text", G_CALLBACK(+[](WebKitFindController *, gpointer data) {
      auto *self = static_cast<WebKitEngineView *>(data);
      self->findActiveMatch_ = 0;
      self->findMatchCount_ = 0;
      emit self->findResultFound(0, 0);
    }), this);
  }

  // Intercept context menu to provide unified Qt menu and avoid GTK context menu crash
  g_signal_connect(webView_, "context-menu", G_CALLBACK(+[](WebKitWebView *wv, WebKitContextMenu *, GdkEvent *, WebKitHitTestResult *hitTest, gpointer data) -> gboolean {
    auto *self = static_cast<WebKitEngineView *>(data);
    if (!self) return TRUE;

    const QPoint globalPos = QCursor::pos();

    if (hitTest && webkit_hit_test_result_context_is_link(hitTest)) {
      const gchar *uri = webkit_hit_test_result_get_link_uri(hitTest);
      const gchar *title = webkit_hit_test_result_get_link_title(hitTest);
      const gchar *label = webkit_hit_test_result_get_link_label(hitTest);
      QString linkText;
      if (title && *title) linkText = QString::fromUtf8(title);
      else if (label && *label) linkText = QString::fromUtf8(label);
      const QUrl linkUrl = QUrl::fromUserInput(QString::fromUtf8(uri ? uri : ""));
      QMetaObject::invokeMethod(self, [self, linkUrl, linkText, globalPos] {
        emit self->linkContextMenuRequested(linkUrl, linkText, globalPos);
      }, Qt::QueuedConnection);
      return TRUE;
    }

    if (hitTest && webkit_hit_test_result_context_is_selection(hitTest)) {
      QMetaObject::invokeMethod(self, [self, globalPos] {
        emit self->selectionContextMenuRequested(QString(), globalPos);
      }, Qt::QueuedConnection);
      return TRUE;
    }

    QMetaObject::invokeMethod(self, [self, globalPos] {
      emit self->pageContextMenuRequested(globalPos);
    }, Qt::QueuedConnection);
    return TRUE;
  }), this);
}

QUrl WebKitEngineView::url() const {
  if (webView_) {
    const char *uriStr = webkit_web_view_get_uri(webView_);
    if (uriStr) cachedUrl_ = QUrl(QString::fromUtf8(uriStr));
  }
  return cachedUrl_;
}

QString WebKitEngineView::title() const {
  if (webView_) {
    const char *titleStr = webkit_web_view_get_title(webView_);
    if (titleStr) cachedTitle_ = QString::fromUtf8(titleStr);
  }
  return cachedTitle_;
}

bool WebKitEngineView::isLoading() const {
  return isLoading_;
}

void WebKitEngineView::load(const QUrl &url) {
  ensureAwake(false);
  QUrl finalUrl = url;
  if (isAdultProtectionActive() && dalinira::core::AdultContentProtectionService::instance().isBlocked(finalUrl)) {
    finalUrl = QUrl(QStringLiteral("dalinira://blocked/adult"));
  }
  if (finalUrl.scheme() == QLatin1String("dalinira") &&
      (finalUrl.host().isEmpty() || finalUrl.host().compare(QLatin1String("newtab"), Qt::CaseInsensitive) == 0)) {
    finalUrl = QUrl(QStringLiteral("dalinira://newtab/"));
  }
  // Browser-initiated loads (address bar, startup URL, bookmarks) are known
  // top-level GET navigations here.  Decide them before WebKit opens a socket;
  // its send-request signal does not consistently carry Sec-Fetch-Dest for a
  // provisional main resource.  Page-initiated navigations are covered by the
  // web-process extension's top-document classification.
  if (networkEvaluator_ &&
      (finalUrl.scheme() == QLatin1String("http") || finalUrl.scheme() == QLatin1String("https"))) {
    WebKitNetworkFilter::Request request;
    request.pageId = pageId_;
    request.method = QStringLiteral("GET");
    request.destination = QStringLiteral("document");
    request.url = finalUrl;
    request.firstPartyUrl = finalUrl;
    const WebKitNetworkFilter::Verdict verdict = networkEvaluator_(request);
    if (verdict.action == WebKitNetworkFilter::Action::Redirect &&
        verdict.redirectUrl.isValid() && !verdict.redirectUrl.isEmpty())
      finalUrl = verdict.redirectUrl;
  }
  cachedUrl_ = finalUrl;
  applyPageScriptPolicy(finalUrl);
  if (webView_) {
    // Internal pages go through the registered dalinira:// scheme handler
    // (serveInternalPage) so reload, history and tab re-selection re-request a
    // real document instead of an unregistered base URI.
    const QByteArray uriBytes = finalUrl.toString().toUtf8();
    webkit_web_view_load_uri(webView_, uriBytes.constData());
  }
}

const char *WebKitEngineView::contentWorldName() {
  return "dalinira";
}

void WebKitEngineView::installDocumentScripts(const QUrl &url) {
  if (!contentManager_) return;
  documentScriptKey_ = url.scheme().toLower() + QStringLiteral("://") + url.host().toLower();
  installedScripts_ = documentScriptProvider_ ? documentScriptProvider_(url) : QList<DocumentScript>{};
  documentScripts_.clear();
  for (const DocumentScript &script : std::as_const(installedScripts_)) {
    if (script.reapply && !script.mainWorld) documentScripts_.append(script.source);
  }
  // This view owns its user content manager, and these are its only scripts.
  webkit_user_content_manager_remove_all_scripts(contentManager_);
  if (!credentialCaptureScript_.isEmpty()) {
    const QByteArray bytes = credentialCaptureScript_.toUtf8();
    WebKitUserScript *credentialScript = webkit_user_script_new_for_world(
        bytes.constData(), WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, contentWorldName(), nullptr, nullptr);
    webkit_user_content_manager_add_script(contentManager_, credentialScript);
    webkit_user_script_unref(credentialScript);
  }
  if (!WebKitEngineProfile::mediaPlaybackAvailable()) {
    // With media disabled (no GStreamer audio sink) WebKitGTK 2.52 still builds a
    // media player for the legacy Audio() factory and then aborts the web
    // process. Make that one path report "not supported" like the others.
    static const char kNoAudioFactory[] =
        "(()=>{const Audio=function Audio(){throw new DOMException("
        "'Media playback is unavailable','NotSupportedError')};"
        "try{Object.defineProperty(window,'Audio',{value:Audio,writable:true,configurable:true})}catch(_){}})();";
    WebKitUserScript *script = webkit_user_script_new(
        kNoAudioFactory, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, nullptr);
    webkit_user_content_manager_add_script(contentManager_, script);
    webkit_user_script_unref(script);
  }
  if (!desiredPageScripts(url)) {
    // With page scripts off Chromium renders <noscript> fallback content.
    // With only enable-javascript-markup off, WebKit still treats scripting as
    // enabled and does not render <noscript>. Unwrap it from DaliNira's world:
    // its element children, or its raw text parsed as markup where the parser
    // kept it as text. Scripts and handlers in it stay inert (page markup
    // cannot run).
    static const char kNoScriptFallback[] =
        "document.addEventListener('DOMContentLoaded',()=>{"
        "for(const n of Array.from(document.querySelectorAll('noscript'))){"
        "if(n.children.length){n.replaceWith(...Array.from(n.childNodes));continue;}"
        "const t=document.createElement('template');t.innerHTML=n.textContent;"
        "n.replaceWith(t.content);}},{once:true});";
    WebKitUserScript *script = webkit_user_script_new_for_world(
        kNoScriptFallback, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
        contentWorldName(), nullptr, nullptr);
    webkit_user_content_manager_add_script(contentManager_, script);
    webkit_user_script_unref(script);
  }
  for (const DocumentScript &documentScript : std::as_const(installedScripts_)) {
    const QByteArray bytes = documentScript.source.toUtf8();
    const WebKitUserContentInjectedFrames frames =
        documentScript.allFrames ? WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES : WEBKIT_USER_CONTENT_INJECT_TOP_FRAME;
    WebKitUserScript *script =
        documentScript.mainWorld
            ? webkit_user_script_new(bytes.constData(), frames, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
                                     nullptr, nullptr)
            : webkit_user_script_new_for_world(bytes.constData(), frames, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
                                               contentWorldName(), nullptr, nullptr);
    webkit_user_content_manager_add_script(contentManager_, script);
    webkit_user_script_unref(script);
  }
}

void WebKitEngineView::setNetworkRequestEvaluator(WebKitNetworkFilter::Evaluator evaluator) {
  networkEvaluator_ = std::move(evaluator);
  updateNetworkRoute();
}

void WebKitEngineView::updateNetworkRoute() {
  WebKitNetworkFilter *filter = profile_ ? profile_->networkFilter() : nullptr;
  const quint64 current = webView_ ? webkit_web_view_get_page_id(webView_) : 0;
  if (filter && pageId_ && pageId_ != current) filter->removeRoute(pageId_);
  pageId_ = current;
  if (filter && pageId_) filter->setRoute(pageId_, networkEvaluator_);
}

void WebKitEngineView::refreshDocumentScripts() {
  installDocumentScripts(url());
  // Same contract as Chromium's refreshDocument: pause the live runtime, then
  // re-run the current scripts so a disabled site stops hiding content at once.
  evaluateInContentWorld(
      QStringLiteral("if(window.__daliniraCosmeticRuntime){window.__daliniraCosmeticRuntime.pause();}"
                     "window.__daliniraProceduralRules=0;\n") +
      documentScripts_.join(QLatin1Char('\n')));
}

void WebKitEngineView::evaluateInContentWorld(const QString &script) {
  if (!webView_) return;
  const QByteArray bytes = script.toUtf8();
  webkit_web_view_evaluate_javascript(webView_, bytes.constData(), bytes.size(), contentWorldName(),
                                      nullptr, nullptr, nullptr, nullptr);
}

void WebKitEngineView::registerInternalScheme(WebKitWebContext *context) {
  if (!context) return;
  webkit_web_context_register_uri_scheme(
      context, "dalinira",
      +[](WebKitURISchemeRequest *request, gpointer) {
        WebKitWebView *wv = webkit_uri_scheme_request_get_web_view(request);
        auto *self = wv ? static_cast<WebKitEngineView *>(
                              g_object_get_data(G_OBJECT(wv), "dalinira-engine-view"))
                        : nullptr;
        if (!self) {
          GError *error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "No DaliNira view");
          webkit_uri_scheme_request_finish_error(request, error);
          g_error_free(error);
          return;
        }
        self->serveInternalPage(request);
      },
      nullptr, nullptr);
  // Web content must not be able to load or frame DaliNira's internal pages.
  WebKitSecurityManager *security = webkit_web_context_get_security_manager(context);
  webkit_security_manager_register_uri_scheme_as_local(security, "dalinira");
  webkit_security_manager_register_uri_scheme_as_secure(security, "dalinira");
}

void WebKitEngineView::serveInternalPage(WebKitURISchemeRequest *request) {
  const QUrl url(QString::fromUtf8(webkit_uri_scheme_request_get_uri(request)));
  const QString host = url.host().toLower();
  const QString path = url.path();
  WK_TRACE("scheme request " << url.adjusted(QUrl::RemoveUserInfo | QUrl::RemoveQuery | QUrl::RemoveFragment).toString());
  if ((host == QLatin1String("newtab") || host == QLatin1String("incognito")) && !path.isEmpty() &&
      path != QLatin1String("/")) {
    serveInternalAsset(request, url);
    return;
  }
  if (!path.isEmpty() && path != QLatin1String("/") && host != QLatin1String("blocked")) {
    // Only the document itself is served. Sub-resource requests such as the
    // automatic dalinira://newtab/favicon.ico must not regenerate the page's
    // navigation capability, which would invalidate the search box token.
    GError *error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Unknown DaliNira resource");
    webkit_uri_scheme_request_finish_error(request, error);
    g_error_free(error);
    return;
  }
  // The profile's engine, as the Chromium New Tab and the omnibox use: the
  // application QSettings key is not where the choice is stored.
  const QString searchEngine = profileData_ ? profileData_->searchEngine() : QStringLiteral("DuckDuckGo");
  QString html;
  if (host == QLatin1String("newtab") || host.isEmpty()) {
    internalCapability_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QSettings settings;
    const bool legacyCards = settings.value(QStringLiteral("browser/cards"), true).toBool();
    const bool showDownloadsCard = settings.value(QStringLiteral("browser/showDownloadsCard"), legacyCards).toBool();
    const bool showBlockedCard = settings.value(QStringLiteral("browser/showBlockedCard"), legacyCards).toBool();
    const QString blockedCounterMode = settings.value(QStringLiteral("browser/blockedCounterMode"), QStringLiteral("all_time")).toString();

    html = webKitNewTabHtml(
        newTabHtml(searchEngine,
                   collectNewTabFrequentSites(profileData_, 6, QStringLiteral("webkit")),
                   collectNewTabBookmarks(profileData_, 6, QStringLiteral("webkit")),
                   profileData_ ? profileData_->totalBlockedCount() : 0,
                   profileData_ ? profileData_->recentDownloadCount() : 0,
                   profileData_ ? profileData_->sessionBlockedCount() : 0,
                   showDownloadsCard, showBlockedCard,
                   blockedCounterMode, internalCapability_),
        internalCapability_, managedBackgroundPath_, managedBackgroundThumbnailPath_);
  } else if (host == QLatin1String("incognito")) {
    internalCapability_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    html = incognitoNewTabHtml(searchEngine);
    html.replace(QStringLiteral("let suggestionCapability = '';"),
                 QStringLiteral("let suggestionCapability = '%1';").arg(internalCapability_));
  } else if (host == QLatin1String("blocked")) {
    html = adultBlockedWarningHtml();
  } else {
    GError *error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Unknown DaliNira page");
    webkit_uri_scheme_request_finish_error(request, error);
    g_error_free(error);
    return;
  }

  const QByteArray bytes = html.toUtf8();
  GBytes *data = g_bytes_new(bytes.constData(), static_cast<gsize>(bytes.size()));
  GInputStream *stream = g_memory_input_stream_new_from_bytes(data);
  WebKitURISchemeResponse *response = webkit_uri_scheme_response_new(stream, bytes.size());
  webkit_uri_scheme_response_set_content_type(response, "text/html; charset=utf-8");
  webkit_uri_scheme_request_finish_with_response(request, response);
  g_object_unref(response);
  g_object_unref(stream);
  g_bytes_unref(data);
}

void WebKitEngineView::serveInternalAsset(WebKitURISchemeRequest *request, const QUrl &url) {
  // Sub-resources of DaliNira's internal pages. Serving them never
  // regenerates the page's navigation capability.
  QByteArray data;
  QByteArray mimeType;
  NewTabStaticAsset asset;
  const QString path = url.path();
  if (path == QLatin1String("/favicon")) {
    QUrl page;
    if (!profile_ || !authorizeNewTabFaviconUrl(profileData_, url, &page)) {
      GError *error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                          "Unauthorized favicon request");
      webkit_uri_scheme_request_finish_error(request, error);
      g_error_free(error);
      return;
    }
    const auto finish = [](WebKitURISchemeRequest *pendingRequest, const QIcon &icon) {
      if (icon.isNull()) {
        GError *error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Favicon not found");
        webkit_uri_scheme_request_finish_error(pendingRequest, error);
        g_error_free(error);
      } else {
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        if (!icon.pixmap(64, 64).save(&buffer, "PNG") || png.isEmpty()) {
          GError *error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Favicon encoding failed");
          webkit_uri_scheme_request_finish_error(pendingRequest, error);
          g_error_free(error);
        } else {
          GBytes *bytes = g_bytes_new(png.constData(), static_cast<gsize>(png.size()));
          GInputStream *stream = g_memory_input_stream_new_from_bytes(bytes);
          WebKitURISchemeResponse *response = webkit_uri_scheme_response_new(stream, png.size());
          webkit_uri_scheme_response_set_content_type(response, "image/png");
          webkit_uri_scheme_request_finish_with_response(pendingRequest, response);
          g_object_unref(response);
          g_object_unref(stream);
          g_bytes_unref(bytes);
        }
      }
      g_object_unref(pendingRequest);
    };
    const QIcon shared = cachedNewTabFavicon(profileData_, page);
    if (!shared.isNull()) {
      g_object_ref(request);
      finish(request, shared);
      return;
    }
    g_object_ref(request);
    const QPointer<WebKitEngineView> guardedThis(this);
    QUrl root = page;
    root.setPath(QLatin1String("/"));
    root.setQuery(QString());
    root.setFragment(QString());
    root.setUserInfo(QString());
    profile_->requestFavicon(page, [guardedThis, request, page, root, finish](const QIcon &icon) {
      if (!guardedThis) {
        finish(request, {});
        return;
      }
      if (!icon.isNull()) {
        cacheNewTabFavicon(guardedThis->profileData_, page, icon);
        finish(request, icon);
        return;
      }
      if (page == root || !guardedThis->profile_) {
        const QIcon platform = platformPresetFavicon(page);
        finish(request, platform);
        return;
      }
      guardedThis->profile_->requestFavicon(
          root, [guardedThis, request, page, finish](const QIcon &rootIcon) {
            if (guardedThis && !rootIcon.isNull()) {
              cacheNewTabFavicon(guardedThis->profileData_, page, rootIcon);
              finish(request, rootIcon);
              return;
            }
            const QIcon platform = platformPresetFavicon(page);
            finish(request, platform);
          });
    });
    return;
  } else if (newTabStaticAsset(path, &asset)) {
    data = readNewTabStaticAsset(asset, resolveNewTabAssetsDirectory(QCoreApplication::applicationDirPath()));
    mimeType = asset.mimeType;
  } else if ((path == QLatin1String("/managed-background") || path == QLatin1String("/managed-background-thumbnail")) &&
             !internalCapability_.isEmpty() &&
             QUrlQuery(url).queryItemValue(QStringLiteral("cap")) == internalCapability_) {
    const bool thumbnail = path.endsWith(QLatin1String("thumbnail"));
    QFile file(thumbnail ? managedBackgroundThumbnailPath_ : managedBackgroundPath_);
    if (file.open(QIODevice::ReadOnly)) data = file.readAll();
    mimeType = thumbnail ? QByteArrayLiteral("image/jpeg") : QByteArrayLiteral("image/png");
  }
  if (data.isEmpty()) {
    GError *error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Unknown DaliNira resource");
    webkit_uri_scheme_request_finish_error(request, error);
    g_error_free(error);
    return;
  }
  GBytes *bytes = g_bytes_new(data.constData(), static_cast<gsize>(data.size()));
  GInputStream *stream = g_memory_input_stream_new_from_bytes(bytes);
  WebKitURISchemeResponse *response = webkit_uri_scheme_response_new(stream, data.size());
  webkit_uri_scheme_response_set_content_type(response, mimeType.constData());
  webkit_uri_scheme_request_finish_with_response(request, response);
  g_object_unref(response);
  g_object_unref(stream);
  g_bytes_unref(bytes);
}

void WebKitEngineView::reload() {
  // Waking a sleeping page already reloads its current history entry.
  if (lifecycleState_ == TabLifecycleState::Discarded) {
    ensureAwake(true);
    return;
  }
  if (!webView_) return;
  // A changed site policy (shield "block scripts") applies on reload, as on
  // Chromium, where the shield reloads the page.
  applyPageScriptPolicy(url());
  webkit_web_view_reload(webView_);
}

void WebKitEngineView::stop() {
  if (webView_) webkit_web_view_stop_loading(webView_);
}

void WebKitEngineView::back() {
  ensureAwake(false);
  if (!webView_) return;
  // History navigations can be served from the page cache without a new
  // response, so apply the destination's script policy before going there.
  WebKitBackForwardList *list = webkit_web_view_get_back_forward_list(webView_);
  if (WebKitBackForwardListItem *item = list ? webkit_back_forward_list_get_back_item(list) : nullptr)
    applyPageScriptPolicy(QUrl(QString::fromUtf8(webkit_back_forward_list_item_get_uri(item))));
  webkit_web_view_go_back(webView_);
}

void WebKitEngineView::forward() {
  ensureAwake(false);
  if (!webView_) return;
  WebKitBackForwardList *list = webkit_web_view_get_back_forward_list(webView_);
  if (WebKitBackForwardListItem *item = list ? webkit_back_forward_list_get_forward_item(list) : nullptr)
    applyPageScriptPolicy(QUrl(QString::fromUtf8(webkit_back_forward_list_item_get_uri(item))));
  webkit_web_view_go_forward(webView_);
}

void WebKitEngineView::setScriptPopupsAllowed(bool allowed) {
  scriptPopupsAllowed_ = allowed;
  if (viewSettings_) webkit_settings_set_javascript_can_open_windows_automatically(viewSettings_, allowed);
}

bool WebKitEngineView::desiredPageScripts(const QUrl &url) const {
  const QString scheme = url.scheme().toLower();
  // DaliNira's internal pages and local documents always run their scripts;
  // the policy is about web sites, as Chromium's prepareAdBlockScripts.
  if (scheme != QLatin1String("http") && scheme != QLatin1String("https")) return true;
  return !pageScriptPolicy_ || pageScriptPolicy_(url);
}

// enable-javascript-markup turns off the page's own JavaScript (script
// elements, inline and attribute handlers, javascript: URLs) while user
// scripts and evaluate_javascript keep working, as Chromium keeps running
// DaliNira's isolated-world scripts on a page with JavaScript disabled. It is
// a page preference: it reaches the committed page's process at once, and a
// process created for a cross-site navigation starts with the current value.
bool WebKitEngineView::applyPageScriptPolicy(const QUrl &url) {
  const bool allowed = desiredPageScripts(url);
  if (allowed == pageScriptsAllowed_) return false;
  pageScriptsAllowed_ = allowed;
  if (viewSettings_) webkit_settings_set_enable_javascript_markup(viewSettings_, allowed);
  WK_TRACE("page scripts " << (allowed ? "allowed" : "blocked") << " for " << url.host());
  return true;
}

bool WebKitEngineView::canGoBack() const {
  return webView_ && webkit_web_view_can_go_back(webView_);
}

bool WebKitEngineView::canGoForward() const {
  return webView_ && webkit_web_view_can_go_forward(webView_);
}

QIcon WebKitEngineView::favicon() const {
  if (webView_) {
    cairo_surface_t *surf = webkit_web_view_get_favicon(webView_);
    return iconFromCairoSurface(surf);
  }
  return discardedIcon_;
}

qreal WebKitEngineView::zoomFactor() const {
  return webView_ ? webkit_web_view_get_zoom_level(webView_) : 1.0;
}

void WebKitEngineView::setZoomFactor(qreal factor) {
  if (webView_) {
    webkit_web_view_set_zoom_level(webView_, factor);
  }
}

void WebKitEngineView::setEngineActive(bool active) {
  WK_TRACE("setEngineActive " << active << " visible=" << isVisible() << " mapped=" << nativeSurfaceMapped());
  engineActive_ = active;
  applyWebViewVisibility();
  // Visibility is owned by Qt (QStackedWidget hides the host window, which
  // makes the embedded GTK surface unviewable). Only focus needs handling.
  if (!active) {
    returnKeyboardFocusToQt(window());
  } else if (webView_) {
    gtk_widget_queue_draw(GTK_WIDGET(webView_));
  }
}

void WebKitEngineView::applyWebViewVisibility() {
  if (!webView_) return;
  // Unmapping the Qt host hides the X11 surface, but GTK still considers the
  // WebKitWebView mapped, so WebKit kept the page "visible": full-rate timers,
  // requestAnimationFrame and rendering in every background tab. Hiding the
  // widget itself makes WebKit treat the page as hidden (document.hidden,
  // throttled timers, no rendering), as Chromium does for background tabs.
  // Media keeps playing.
  GtkWidget *widget = GTK_WIDGET(webView_);
  if (engineActive_ && !gtk_widget_get_visible(widget)) {
    gtk_widget_show(widget);
    gtk_widget_queue_draw(widget);
  } else if (!engineActive_ && gtk_widget_get_visible(widget)) {
    gtk_widget_hide(widget);
  }
}

bool WebKitEngineView::isAudible() const {
  return webView_ && webkit_web_view_is_playing_audio(webView_);
}

qint64 WebKitEngineView::contentProcessId() const {
  WebKitNetworkFilter *filter = profile_ ? profile_->networkFilter() : nullptr;
  // The live page id: it follows process swaps on cross-site navigation.
  return webView_ && filter ? filter->processIdForPage(webkit_web_view_get_page_id(webView_)) : 0;
}

bool WebKitEngineView::setLifecycleState(TabLifecycleState state) {
  if (state == lifecycleState_) return true;
  switch (state) {
    case TabLifecycleState::Active:
      if (lifecycleState_ == TabLifecycleState::Discarded) {
        restoreWebContent(true);
      } else {
        lifecycleState_ = TabLifecycleState::Active;
        emit lifecycleStateChanged(lifecycleState_);
      }
      return true;
    case TabLifecycleState::Frozen:
      // A background page: WebKit already runs it hidden (throttled). A
      // sleeping page is not woken up just to be frozen.
      if (lifecycleState_ == TabLifecycleState::Discarded || engineActive_ || !webView_) return false;
      lifecycleState_ = TabLifecycleState::Frozen;
      emit lifecycleStateChanged(lifecycleState_);
      return true;
    case TabLifecycleState::Discarded:
      return discardWebContent();
  }
  return false;
}

bool WebKitEngineView::discardWebContent() {
  // The page on screen is never put to sleep.
  if (!webView_ || engineActive_) return false;
  cachedUrl_ = url();
  cachedTitle_ = title();
  discardedIcon_ = favicon();
  discardedZoom_ = zoomFactor();
  discardedSession_.clear();
  if (WebKitWebViewSessionState *state = webkit_web_view_get_session_state(webView_)) {
    if (GBytes *bytes = webkit_web_view_session_state_serialize(state)) {
      gsize size = 0;
      const auto *data = static_cast<const char *>(g_bytes_get_data(bytes, &size));
      discardedSession_ = QByteArray(data, static_cast<qsizetype>(size));
      g_bytes_unref(bytes);
    }
    webkit_web_view_session_state_unref(state);
  }
  isLoading_ = false;
  // Destroys the WebKitWebView; its web process exits with its last page.
  teardownWebKit();
  lifecycleState_ = TabLifecycleState::Discarded;
  WK_TRACE("discarded; session state " << discardedSession_.size() << " bytes");
  emit lifecycleStateChanged(lifecycleState_);
  return true;
}

void WebKitEngineView::restoreWebContent(bool navigateToCurrentEntry) {
  const bool active = engineActive_;
  initWebKit();
  engineActive_ = active;
  if (!discardedSession_.isEmpty()) {
    GBytes *bytes = g_bytes_new(discardedSession_.constData(), static_cast<gsize>(discardedSession_.size()));
    if (WebKitWebViewSessionState *state = webkit_web_view_session_state_new(bytes)) {
      webkit_web_view_restore_session_state(webView_, state);
      webkit_web_view_session_state_unref(state);
    }
    g_bytes_unref(bytes);
  }
  if (!qFuzzyCompare(discardedZoom_, 1.0)) webkit_web_view_set_zoom_level(webView_, discardedZoom_);
  lifecycleState_ = TabLifecycleState::Active;
  discardedSession_.clear();
  discardedIcon_ = QIcon();
  applyWebViewVisibility();
  if (navigateToCurrentEntry) {
    // Loading the history entry restores its scroll position and form state.
    WebKitBackForwardList *list = webkit_web_view_get_back_forward_list(webView_);
    WebKitBackForwardListItem *current = list ? webkit_back_forward_list_get_current_item(list) : nullptr;
    if (current) {
      applyPageScriptPolicy(QUrl(QString::fromUtf8(webkit_back_forward_list_item_get_uri(current))));
      webkit_web_view_go_to_back_forward_list_item(webView_, current);
    } else if (cachedUrl_.isValid()) {
      applyPageScriptPolicy(cachedUrl_);
      const QByteArray uri = cachedUrl_.toString().toUtf8();
      webkit_web_view_load_uri(webView_, uri.constData());
    }
  }
  WK_TRACE("restored from sleep");
  emit lifecycleStateChanged(lifecycleState_);
}

void WebKitEngineView::ensureAwake(bool navigateToCurrentEntry) {
  if (lifecycleState_ == TabLifecycleState::Discarded) restoreWebContent(navigateToCurrentEntry);
}

bool WebKitEngineView::nativeSurfaceMapped() const {
  if (!gtkWindow_ || !gtk_widget_get_realized(gtkWindow_)) return false;
  GdkWindow *window = gtk_widget_get_window(gtkWindow_);
  if (!window) return false;
  XWindowAttributes attributes{};
  Display *display = GDK_WINDOW_XDISPLAY(window);
  const Window xid = gdk_x11_window_get_xid(window);
  return display && xid && XGetWindowAttributes(display, xid, &attributes) != 0 &&
         attributes.map_state == IsViewable;
}

void WebKitEngineView::focusWebContent(Qt::FocusReason reason) {
  if (!webView_ || !plugXid_ || !isVisible()) return;
  // Let the X server see Qt's pending map/unmap of the host first, so the
  // viewability check below matches what XSetInputFocus will find.
  syncQtConnection();
  if (!nativeSurfaceMapped()) return;
  WK_TRACE("focusWebContent reason=" << reason << " geometry=" << geometry());
  if (!hasFocus()) setFocus(reason);
  GdkWindow *plugWindow = gtk_widget_get_window(gtkWindow_);
  Display *display = GDK_WINDOW_XDISPLAY(plugWindow);
  // Real hardware key events are delivered by the X server to the focused
  // window; no key or pointer event is ever re-injected by DaliNira.
  {
    GdkX11ErrorTrap trap(gtkWindow_);
    XSetInputFocus(display, static_cast<Window>(focusXid_ ? focusXid_ : plugXid_), RevertToParent, CurrentTime);
    XFlush(display);
  }
  if (!webFocused_) {
    webFocused_ = true;
    sendXEmbedMessage(kXEmbedWindowActivate);
    sendXEmbedMessage(kXEmbedFocusIn, kXEmbedFocusCurrent);
  }
  if (!gtk_widget_has_focus(GTK_WIDGET(webView_))) gtk_widget_grab_focus(GTK_WIDGET(webView_));
}

void WebKitEngineView::hideEvent(QHideEvent *event) {
  WK_TRACE("hideEvent");
  if (webFocused_) returnKeyboardFocusToQt(window());
  EngineView::hideEvent(event);
}

void WebKitEngineView::showEvent(QShowEvent *event) {
  EngineView::showEvent(event);
  WK_TRACE("showEvent size=" << size());
  // GDK may recompute the plug's native event mask; keep keys off it.
  restrictPlugKeyboardDelivery();
  resizeNativeSurface();
  if (webView_) gtk_widget_queue_draw(GTK_WIDGET(webView_));
}

struct JsEvalContext {
  std::function<void(const QVariant &)> callback;
};

void WebKitEngineView::runJavaScript(const QString &script,
                                     std::function<void(const QVariant &)> callback) {
  evaluateJavaScript(script, nullptr, std::move(callback));
}

void WebKitEngineView::runIsolatedJavaScript(const QString &script,
                                             std::function<void(const QVariant &)> callback) {
  evaluateJavaScript(script, contentWorldName(), std::move(callback));
}

bool WebKitEngineView::popupAllowed(const QUrl &target) {
  if (!popupEvaluator_) return true;
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  if (target == lastPopupUrl_ && now - lastPopupAtMs_ < 1000) return lastPopupAllowed_;
  lastPopupUrl_ = target;
  lastPopupAtMs_ = now;
  lastPopupAllowed_ = popupEvaluator_(target);
  return lastPopupAllowed_;
}

void WebKitEngineView::evaluateJavaScript(const QString &script, const char *world,
                                          std::function<void(const QVariant &)> callback) {
  if (!webView_) {
    if (callback) callback(QVariant());
    return;
  }

  auto *ctx = new JsEvalContext{std::move(callback)};
  const QByteArray scriptBytes = script.toUtf8();

  webkit_web_view_evaluate_javascript(
      webView_,
      scriptBytes.constData(),
      scriptBytes.length(),
      world,
      nullptr,
      nullptr,
      +[](GObject *src, GAsyncResult *res, gpointer userData) {
        auto *evalCtx = static_cast<JsEvalContext *>(userData);
        GError *error = nullptr;
        JSCValue *val = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(src), res, &error);
        QVariant resultVariant;

        if (error) {
          qDebug() << "WebKit JS Error:" << error->message;
          g_error_free(error);
        } else if (val) {
          if (jsc_value_is_string(val)) {
            gchar *s = jsc_value_to_string(val);
            resultVariant = QString::fromUtf8(s);
            g_free(s);
          } else if (jsc_value_is_boolean(val)) {
            resultVariant = static_cast<bool>(jsc_value_to_boolean(val));
          } else if (jsc_value_is_number(val)) {
            resultVariant = jsc_value_to_double(val);
          } else if (jsc_value_is_null(val) || jsc_value_is_undefined(val)) {
            resultVariant = QVariant();
          } else if (jsc_value_is_array(val) || jsc_value_is_object(val)) {
            // Objects and arrays as QVariantMap / QVariantList, as QtWebEngine
            // returns them (a Promise or function has no JSON form: null).
            gchar *json = jsc_value_to_json(val, 0);
            if (json) {
              const QJsonDocument document = QJsonDocument::fromJson(QByteArray(json));
              if (document.isObject()) resultVariant = document.object().toVariantMap();
              else if (document.isArray()) resultVariant = document.array().toVariantList();
              g_free(json);
            }
          } else {
            gchar *s = jsc_value_to_string(val);
            resultVariant = QString::fromUtf8(s);
            g_free(s);
          }
          g_object_unref(val);
        }

        if (evalCtx && evalCtx->callback) {
          evalCtx->callback(resultVariant);
        }
        delete evalCtx;
      },
      ctx);
}

void WebKitEngineView::findText(const QString &text, bool forward, bool caseSensitive) {
  if (!webView_) return;
  WebKitFindController *findCtrl = webkit_web_view_get_find_controller(webView_);
  if (!findCtrl) return;

  if (text.isEmpty()) {
    clearFind();
    return;
  }

  guint32 options = WEBKIT_FIND_OPTIONS_WRAP_AROUND;
  if (!caseSensitive) {
    options |= WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE;
  }
  if (!forward) {
    options |= WEBKIT_FIND_OPTIONS_BACKWARDS;
  }

  if (lastSearchText_ == text && lastSearchCaseSensitive_ == caseSensitive) {
    if (forward) {
      webkit_find_controller_search_next(findCtrl);
      if (findMatchCount_ > 0)
        findActiveMatch_ = (findActiveMatch_ % findMatchCount_) + 1;
    } else {
      webkit_find_controller_search_previous(findCtrl);
      if (findMatchCount_ > 0)
        findActiveMatch_ = ((findActiveMatch_ + findMatchCount_ - 2) % findMatchCount_) + 1;
    }
    if (findMatchCount_ > 0) emit findResultFound(findActiveMatch_, findMatchCount_);
  } else {
    lastSearchText_ = text;
    lastSearchCaseSensitive_ = caseSensitive;
    findActiveMatch_ = 0;
    findMatchCount_ = 0;
    webkit_find_controller_search(findCtrl, text.toUtf8().constData(), options, 1000);
  }
}

void WebKitEngineView::clearFind() {
  if (!webView_) return;
  WebKitFindController *findCtrl = webkit_web_view_get_find_controller(webView_);
  if (findCtrl) {
    webkit_find_controller_search_finish(findCtrl);
  }
  lastSearchText_.clear();
  lastSearchCaseSensitive_ = false;
  findActiveMatch_ = 0;
  findMatchCount_ = 0;
  emit findResultFound(0, 0);
}

void WebKitEngineView::exitFullScreen() {
  if (!webView_) return;
  static const char kExit[] = "if(document.fullscreenElement||document.webkitFullscreenElement)"
                              "(document.exitFullscreen||document.webkitExitFullscreen).call(document);";
  webkit_web_view_evaluate_javascript(webView_, kExit, -1, contentWorldName(), nullptr, nullptr, nullptr, nullptr);
}

void WebKitEngineView::print() {
  if (!webView_) return;
  WebKitPrintOperation *printOp = webkit_print_operation_new(webView_);
  webkit_print_operation_run_dialog(printOp, nullptr);
  g_object_unref(printOp);
}

void WebKitEngineView::printToPdf(const QString &filePath) {
  if (!webView_ || filePath.isEmpty()) return;
  WebKitPrintOperation *printOp = webkit_print_operation_new(webView_);
  GtkPrintSettings *settings = gtk_print_settings_new();
  const QUrl fileUrl = QUrl::fromLocalFile(filePath);
  gtk_print_settings_set(settings, GTK_PRINT_SETTINGS_OUTPUT_URI, fileUrl.toString().toUtf8().constData());
  gtk_print_settings_set(settings, GTK_PRINT_SETTINGS_OUTPUT_FILE_FORMAT, "pdf");
  const char *printerName = g_dgettext("gtk30", "Print to File");
  gtk_print_settings_set(settings, GTK_PRINT_SETTINGS_PRINTER,
                         (printerName && *printerName) ? printerName : "Print to File");
  webkit_print_operation_set_print_settings(printOp, settings);
  g_signal_connect_swapped(printOp, "finished", G_CALLBACK(g_object_unref), printOp);
  g_signal_connect_swapped(printOp, "failed", G_CALLBACK(g_object_unref), printOp);
  webkit_print_operation_print(printOp);
  g_object_unref(settings);
}

} // namespace dalinira::engine
