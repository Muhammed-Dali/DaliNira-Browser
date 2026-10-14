#include "engine/gecko/gecko_x11_embedder.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QJsonArray>
#include <QLoggingCategory>
#include <QWidget>
#include <QWindow>

#include <xcb/res.h>

#include <cstdlib>
#include <cstring>
#include <memory>

Q_LOGGING_CATEGORY(lcGeckoX11, "dalinira.gecko.x11")

namespace dalinira::engine {

namespace {

template <typename T>
using XcbReply = std::unique_ptr<T, decltype(&std::free)>;

template <typename T>
XcbReply<T> reply(T *ptr) {
  return XcbReply<T>(ptr, &std::free);
}

QString hexWindow(xcb_window_t window) {
  return QStringLiteral("0x%1").arg(window, 0, 16);
}

}  // namespace

GeckoX11Embedder::GeckoX11Embedder(QObject *parent) : QObject(parent) {
  // Other platforms (offscreen, Wayland) may still answer the X11 interface,
  // without a connection.
  auto *x11 = qGuiApp ? qGuiApp->nativeInterface<QNativeInterface::QX11Application>() : nullptr;
  if (x11 && x11->connection()) {
    conn_ = x11->connection();
    root_ = xcb_setup_roots_iterator(xcb_get_setup(conn_)).data->root;
  }
  if (QCoreApplication::instance()) QCoreApplication::instance()->installNativeEventFilter(this);
}

GeckoX11Embedder::~GeckoX11Embedder() {
  if (QCoreApplication::instance()) {
    QCoreApplication::instance()->removeNativeEventFilter(this);
  }
}

bool GeckoX11Embedder::isUsable(QString *why) const {
  if (!conn_) {
    *why = QStringLiteral("Qt is not running on xcb");
    return false;
  }
  const xcb_query_extension_reply_t *res = xcb_get_extension_data(conn_, &xcb_res_id);
  if (!res || !res->present) {
    *why = QStringLiteral("X server lacks the X-Resource extension (window ownership check)");
    return false;
  }
  return true;
}

xcb_atom_t GeckoX11Embedder::atom(const char *name) const {
  auto r = reply(xcb_intern_atom_reply(conn_, xcb_intern_atom(conn_, 0, std::strlen(name), name), nullptr));
  return r ? r->atom : static_cast<xcb_atom_t>(XCB_ATOM_NONE);
}

QByteArray GeckoX11Embedder::property(xcb_window_t window, xcb_atom_t prop, xcb_atom_t type) const {
  auto r = reply(xcb_get_property_reply(
      conn_, xcb_get_property(conn_, 0, window, prop, type, 0, 4096), nullptr));
  if (!r || r->type == XCB_ATOM_NONE) {
    return {};
  }
  return QByteArray(static_cast<const char *>(xcb_get_property_value(r.get())),
                    xcb_get_property_value_length(r.get()));
}

QList<xcb_window_t> GeckoX11Embedder::clientList() const {
  const QByteArray data = property(root_, atom("_NET_CLIENT_LIST"), XCB_ATOM_WINDOW);
  QList<xcb_window_t> windows;
  const auto *ids = reinterpret_cast<const xcb_window_t *>(data.constData());
  for (qsizetype i = 0; i < data.size() / qsizetype(sizeof(xcb_window_t)); ++i) {
    windows.append(ids[i]);
  }
  return windows;
}

QList<xcb_window_t> GeckoX11Embedder::topLevelCandidates() const {
  if (hasWindowManager()) {
    return clientList();
  }
  // Without a window manager every top-level is a direct child of the root.
  QList<xcb_window_t> windows;
  auto tree = reply(xcb_query_tree_reply(conn_, xcb_query_tree(conn_, root_), nullptr));
  if (tree) {
    const xcb_window_t *children = xcb_query_tree_children(tree.get());
    for (int i = 0; i < xcb_query_tree_children_length(tree.get()); ++i) {
      windows.append(children[i]);
    }
  }
  return windows;
}

bool GeckoX11Embedder::hasWindowManager() const {
  return !property(root_, atom("_NET_SUPPORTING_WM_CHECK"), XCB_ATOM_WINDOW).isEmpty();
}

qint64 GeckoX11Embedder::serverPidOf(xcb_window_t window) const {
  auto version = reply(xcb_res_query_version_reply(conn_, xcb_res_query_version(conn_, 1, 2), nullptr));
  if (!version) {
    return -1;
  }
  xcb_res_client_id_spec_t spec{window, XCB_RES_CLIENT_ID_MASK_LOCAL_CLIENT_PID};
  auto r = reply(xcb_res_query_client_ids_reply(conn_, xcb_res_query_client_ids(conn_, 1, &spec), nullptr));
  if (!r) {
    return -1;
  }
  for (auto it = xcb_res_query_client_ids_ids_iterator(r.get()); it.rem; xcb_res_client_id_value_next(&it)) {
    if ((it.data->spec.mask & XCB_RES_CLIENT_ID_MASK_LOCAL_CLIENT_PID) &&
        xcb_res_client_id_value_value_length(it.data) >= 1) {
      return xcb_res_client_id_value_value(it.data)[0];
    }
  }
  return -1;
}

std::optional<GeckoX11Embedder::HostWindow> GeckoX11Embedder::findHostWindow(qint64 pid, const QString &name,
                                                                   QString *why) const {
  if (!isUsable(why)) return std::nullopt;
  const xcb_atom_t pidAtom = atom("_NET_WM_PID");
  const xcb_atom_t nameAtom = atom("_NET_WM_NAME");
  const xcb_atom_t utf8 = atom("UTF8_STRING");
  QList<HostWindow> matches;
  int forPid = 0;
  for (xcb_window_t window : topLevelCandidates()) {
    const QByteArray pidData = property(window, pidAtom, XCB_ATOM_CARDINAL);
    if (pidData.size() != 4 || *reinterpret_cast<const quint32 *>(pidData.constData()) != pid) {
      continue;
    }
    ++forPid;
    const QString windowName = QString::fromUtf8(property(window, nameAtom, utf8));
    if (windowName == name) {
      matches.append({window, pid, 0, windowName, 0});
    }
  }
  if (matches.size() != 1) {
    *why = QStringLiteral("%1 windows match pid %2 and the window tag (%3 windows for that pid)")
               .arg(matches.size())
               .arg(pid)
               .arg(forPid);
    return std::nullopt;
  }
  HostWindow found = matches.first();
  found.candidatesForPid = forPid;
  found.serverPid = serverPidOf(found.window);
  if (found.serverPid != pid) {
    *why = QStringLiteral("X server reports owner pid %1 for %2, expected %3")
               .arg(found.serverPid)
               .arg(hexWindow(found.window))
               .arg(pid);
    return std::nullopt;
  }
  auto attrs = reply(xcb_get_window_attributes_reply(
      conn_, xcb_get_window_attributes(conn_, found.window), nullptr));
  if (!attrs || attrs->override_redirect) {
    *why = QStringLiteral("candidate window is override-redirect or gone");
    return std::nullopt;
  }
  return found;
}

QWidget *GeckoX11Embedder::embed(const HostWindow &host, QWidget *parent) {
  if (!conn_ || !host.window) return nullptr;
  window_ = host.window;
  // Observe the window's lifecycle on our own connection without disturbing
  // whatever event mask Qt itself selected on it.
  auto attrs = reply(xcb_get_window_attributes_reply(
      conn_, xcb_get_window_attributes(conn_, window_), nullptr));
  const uint32_t mask = (attrs ? attrs->your_event_mask : 0) | XCB_EVENT_MASK_STRUCTURE_NOTIFY |
                        XCB_EVENT_MASK_FOCUS_CHANGE;
  xcb_change_window_attributes(conn_, window_, XCB_CW_EVENT_MASK, &mask);

  QWindow *foreign = QWindow::fromWinId(window_);
  foreign->setObjectName(QStringLiteral("gecko-host-window"));
  QWidget *container = QWidget::createWindowContainer(foreign, parent);
  container->setObjectName(QStringLiteral("gecko-container"));
  container->setFocusPolicy(Qt::StrongFocus);
  container->setMinimumSize(200, 150);

  grabbed_ = false;
  if (clickFocus_) {
    // Click-to-focus as X11 window managers do it: a synchronous passive
    // button grab lets us move X input focus to the embedded window, after
    // which ReplayPointer delivers the original click to it unchanged.
    xcb_void_cookie_t cookie = xcb_grab_button_checked(
        conn_, 0, window_, XCB_EVENT_MASK_BUTTON_PRESS, XCB_GRAB_MODE_SYNC, XCB_GRAB_MODE_ASYNC,
        XCB_NONE, XCB_NONE, XCB_BUTTON_INDEX_ANY, XCB_MOD_MASK_ANY);
    std::unique_ptr<xcb_generic_error_t, decltype(&std::free)> error(xcb_request_check(conn_, cookie),
                                                                    &std::free);
    grabbed_ = !error;
  }
  xcb_flush(conn_);
  qCDebug(lcGeckoX11) << "embedded" << hexWindow(window_) << "owner" << host.serverPid
                    << "windows for pid" << host.candidatesForPid << "click focus grab" << grabbed_;
  return container;
}

void GeckoX11Embedder::forget() {
  if (window_ && grabbed_) {
    xcb_ungrab_button(conn_, XCB_BUTTON_INDEX_ANY, window_, XCB_MOD_MASK_ANY);
    xcb_flush(conn_);
  }
  window_ = 0;
  grabbed_ = false;
}

xcb_window_t GeckoX11Embedder::inputFocus() const {
  if (!conn_) return 0;
  auto r = reply(xcb_get_input_focus_reply(conn_, xcb_get_input_focus(conn_), nullptr));
  return r ? r->focus : 0;
}

void GeckoX11Embedder::focusEmbedded() {
  if (window_) {
    xcb_set_input_focus(conn_, XCB_INPUT_FOCUS_PARENT, window_, XCB_CURRENT_TIME);
    xcb_flush(conn_);
  }
}

void GeckoX11Embedder::focusQt(QWidget *topLevel) {
  if (!conn_ || !topLevel) return;
  xcb_set_input_focus(conn_, XCB_INPUT_FOCUS_PARENT, static_cast<xcb_window_t>(topLevel->winId()),
                      XCB_CURRENT_TIME);
  xcb_flush(conn_);
}

QJsonObject GeckoX11Embedder::inspect(QWidget *topLevel, QWidget *container) const {
  QJsonObject out;
  if (!window_) {
    return out;
  }
  out.insert(QStringLiteral("window"), hexWindow(window_));
  const xcb_window_t qtTop = topLevel ? static_cast<xcb_window_t>(topLevel->winId()) : 0;
  const xcb_window_t containerNative =
      container && container->internalWinId() ? static_cast<xcb_window_t>(container->internalWinId()) : 0;
  QJsonArray chain;
  bool insideQtTopLevel = false;
  xcb_window_t current = window_;
  for (int depth = 0; depth < 16; ++depth) {
    auto tree = reply(xcb_query_tree_reply(conn_, xcb_query_tree(conn_, current), nullptr));
    if (!tree) {
      break;
    }
    const xcb_window_t parent = tree->parent;
    QString label = hexWindow(parent);
    if (parent == qtTop) {
      label += QStringLiteral(" (Qt top-level)");
      insideQtTopLevel = true;
    } else if (parent == containerNative) {
      label += QStringLiteral(" (container)");
    } else if (parent == root_) {
      label += QStringLiteral(" (root)");
    }
    chain.append(label);
    if (parent == root_ || parent == 0) {
      break;
    }
    current = parent;
  }
  out.insert(QStringLiteral("parentChain"), chain);
  out.insert(QStringLiteral("insideQtTopLevel"), insideQtTopLevel);

  auto geometry = reply(xcb_get_geometry_reply(conn_, xcb_get_geometry(conn_, window_), nullptr));
  auto translated = reply(xcb_translate_coordinates_reply(
      conn_, xcb_translate_coordinates(conn_, window_, root_, 0, 0), nullptr));
  if (geometry && translated) {
    out.insert(QStringLiteral("geometry"), QJsonObject{{"x", translated->dst_x},
                                                       {"y", translated->dst_y},
                                                       {"width", geometry->width},
                                                       {"height", geometry->height}});
  }
  auto attrs = reply(xcb_get_window_attributes_reply(
      conn_, xcb_get_window_attributes(conn_, window_), nullptr));
  if (attrs) {
    out.insert(QStringLiteral("viewable"), attrs->map_state == XCB_MAP_STATE_VIEWABLE);
    out.insert(QStringLiteral("overrideRedirect"), bool(attrs->override_redirect));
  }
  out.insert(QStringLiteral("windowManagerPresent"), hasWindowManager());
  out.insert(QStringLiteral("managedByWindowManager"), clientList().contains(window_));
  const xcb_window_t focus = inputFocus();
  out.insert(QStringLiteral("inputFocus"), hexWindow(focus));
  out.insert(QStringLiteral("inputFocusOnEmbedded"), focus == window_);
  return out;
}

bool GeckoX11Embedder::nativeEventFilter(const QByteArray &eventType, void *message, qintptr *) {
  if (!window_ || eventType != "xcb_generic_event_t") {
    return false;
  }
  auto *event = static_cast<xcb_generic_event_t *>(message);
  switch (event->response_type & ~0x80) {
    case XCB_BUTTON_PRESS: {
      auto *press = reinterpret_cast<xcb_button_press_event_t *>(event);
      if (grabbed_ && press->event == window_) {
        xcb_set_input_focus(conn_, XCB_INPUT_FOCUS_PARENT, window_, press->time);
        xcb_allow_events(conn_, XCB_ALLOW_REPLAY_POINTER, press->time);
        xcb_flush(conn_);
        emit embeddedClicked();
        return true;
      }
      break;
    }
    case XCB_FOCUS_IN:
    case XCB_FOCUS_OUT: {
      auto *focus = reinterpret_cast<xcb_focus_in_event_t *>(event);
      if (focus->event == window_) {
        qCDebug(lcGeckoX11) << "focus" << ((event->response_type & ~0x80) == XCB_FOCUS_IN ? "in" : "out")
                          << focus->detail << focus->mode;
      }
      break;
    }
    case XCB_REPARENT_NOTIFY: {
      auto *rep = reinterpret_cast<xcb_reparent_notify_event_t *>(event);
      if (rep->window == window_) {
        qCDebug(lcGeckoX11) << "reparented to" << hexWindow(rep->parent);
      }
      break;
    }
    case XCB_UNMAP_NOTIFY: {
      auto *unmap = reinterpret_cast<xcb_unmap_notify_event_t *>(event);
      if (unmap->window == window_) {
        qCDebug(lcGeckoX11) << "unmapped";
      }
      break;
    }
    case XCB_MAP_NOTIFY: {
      auto *map = reinterpret_cast<xcb_map_notify_event_t *>(event);
      if (map->window == window_) {
        qCDebug(lcGeckoX11) << "mapped";
      }
      break;
    }
    case XCB_DESTROY_NOTIFY: {
      auto *destroy = reinterpret_cast<xcb_destroy_notify_event_t *>(event);
      if (destroy->window == window_) {
        window_ = 0;
        grabbed_ = false;
        emit embeddedGone(QStringLiteral("embedded window destroyed"));
      }
      break;
    }
  }
  return false;
}

}  // namespace dalinira::engine
