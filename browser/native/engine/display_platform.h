#pragma once

#include <QByteArray>
#include <QString>

#include "engine/engine_type.h"

namespace dalinira::engine {

// Which display server the desktop session runs, and which Qt / GDK windowing
// backends DaliNira uses on it. Decided once, before QApplication exists,
// because Qt and GDK read QT_QPA_PLATFORM / GDK_BACKEND only at start-up.
//
//   Chromium (QtWebEngine) works with any Qt platform plugin: native X11,
//   XWayland (xcb on a Wayland session) and native Wayland. On a Wayland
//   session DaliNira prefers XWayland unless the user chose a platform
//   (QT_QPA_PLATFORM=wayland), because tab dragging between windows needs
//   global window positions, which native Wayland does not expose.
//
//   WebKit (WebKitGTK 4.1) is embedded as an XEmbed GtkPlug inside a native
//   Qt child window. That needs Qt on xcb and GDK on x11 talking to the same
//   X server; a native Wayland surface cannot host a foreign GTK window. On a
//   Wayland session WebKit therefore always runs through XWayland, and when
//   the session has no XWayland at all WebKit is unavailable.
//
//   Gecko (experimental) runs in a separate host process whose X11 window is
//   embedded into a Qt window: Qt on xcb, XWayland on Wayland sessions, and
//   unavailable without an X server, as WebKit.
enum class DisplayServer { X11, Wayland, Unknown };

struct DisplayEnvironment {
  QByteArray sessionType;     // XDG_SESSION_TYPE
  QByteArray waylandDisplay;  // WAYLAND_DISPLAY
  QByteArray x11Display;      // DISPLAY
  QByteArray qpaPlatform;     // QT_QPA_PLATFORM as the user set it
  QByteArray gdkBackend;      // GDK_BACKEND as the user set it

  static DisplayEnvironment fromProcess();
  DisplayServer session() const;
};

struct DisplayPlan {
  QByteArray qpaPlatform;  // QT_QPA_PLATFORM to set; empty: keep the current value / Qt's choice
  QByteArray gdkBackend;   // GDK_BACKEND to set; empty: keep the current value
  bool engineSupported = true;  // false: the engine cannot show pages on this display
  QString note;            // why the plan overrides the user's or Qt's choice
};

DisplayPlan planDisplay(EngineType engine, const DisplayEnvironment &environment);
void applyDisplayPlan(const DisplayPlan &plan);

// After QGuiApplication exists: the windowing backend actually in use.
bool qtUsesX11();
// "x11", "xwayland", "wayland" or Qt's platform name (e.g. "offscreen").
QString activeDisplayBackend();

}  // namespace dalinira::engine
