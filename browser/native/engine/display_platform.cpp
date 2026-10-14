#include "engine/display_platform.h"

#include <QGuiApplication>
#include <QList>

namespace dalinira::engine {

namespace {

// QT_QPA_PLATFORM may list fallbacks ("wayland;xcb"); Qt uses the first one
// that loads, and DaliNira plans for the first.
QByteArray firstPlatform(const QByteArray &value, char separator = ';') {
  const QList<QByteArray> entries = value.split(separator);
  return entries.isEmpty() ? QByteArray() : entries.first().trimmed().toLower();
}

bool isWaylandPlatform(const QByteArray &platform) { return platform.startsWith("wayland"); }

void addNote(DisplayPlan &plan, const QString &note) {
  plan.note = plan.note.isEmpty() ? note : plan.note + QLatin1Char(' ') + note;
}

}  // namespace

DisplayEnvironment DisplayEnvironment::fromProcess() {
  DisplayEnvironment environment;
  environment.sessionType = qgetenv("XDG_SESSION_TYPE").trimmed().toLower();
  environment.waylandDisplay = qgetenv("WAYLAND_DISPLAY");
  environment.x11Display = qgetenv("DISPLAY");
  environment.qpaPlatform = qgetenv("QT_QPA_PLATFORM");
  environment.gdkBackend = qgetenv("GDK_BACKEND");
  return environment;
}

DisplayServer DisplayEnvironment::session() const {
  if (!waylandDisplay.isEmpty() || sessionType == "wayland") return DisplayServer::Wayland;
  if (!x11Display.isEmpty() || sessionType == "x11") return DisplayServer::X11;
  return DisplayServer::Unknown;
}

DisplayPlan planDisplay(EngineType engine, const DisplayEnvironment &environment) {
  DisplayPlan plan;
  const QByteArray requested = firstPlatform(environment.qpaPlatform);
  const bool hasX11 = !environment.x11Display.isEmpty();

  if (engine == EngineType::WebKit) {
    if (requested == "offscreen" || requested == "minimal") return plan;  // headless tooling
    if (!hasX11) {
      plan.engineSupported = false;
      addNote(plan, QStringLiteral("WebKit needs an X11 display (XWayland on Wayland sessions); none is available."));
      return plan;
    }
    if (requested != "xcb") {
      plan.qpaPlatform = "xcb";
      if (!requested.isEmpty())
        addNote(plan, QStringLiteral("WebKit is embedded through XEmbed; using xcb instead of QT_QPA_PLATFORM=%1.")
                          .arg(QString::fromLatin1(environment.qpaPlatform)));
    }
    // GTK and Qt must share the X server the GtkPlug lives on.
    if (firstPlatform(environment.gdkBackend, ',') != "x11") {
      plan.gdkBackend = "x11";
      if (!environment.gdkBackend.isEmpty())
        addNote(plan, QStringLiteral("WebKit is embedded through XEmbed; using GDK_BACKEND=x11 instead of %1.")
                          .arg(QString::fromLatin1(environment.gdkBackend)));
    }
    return plan;
  }

  // Gecko runs in its own host process, whose X11 window is embedded into a
  // Qt window: Qt must be on the same X server (XWayland on Wayland sessions).
  if (engine == EngineType::Gecko) {
    if (requested == "offscreen" || requested == "minimal") return plan;  // headless tooling
    if (!hasX11) {
      plan.engineSupported = false;
      addNote(plan, QStringLiteral("Gecko needs an X11 display (XWayland on Wayland sessions); none is available."));
      return plan;
    }
    if (requested != "xcb") {
      plan.qpaPlatform = "xcb";
      if (!requested.isEmpty())
        addNote(plan, QStringLiteral("Gecko pages are embedded through X11; using xcb instead of QT_QPA_PLATFORM=%1.")
                          .arg(QString::fromLatin1(environment.qpaPlatform)));
    }
    // Qt's GTK dialogs on the same X server (the host chooses its own backend).
    if (environment.gdkBackend.isEmpty()) plan.gdkBackend = "x11";
    return plan;
  }

  // Chromium: honour an explicit platform choice; otherwise prefer XWayland on
  // a Wayland session when it exists, and let Qt pick (native Wayland) when not.
  QByteArray platform = requested;
  if (platform.isEmpty() && environment.session() == DisplayServer::Wayland && hasX11) {
    plan.qpaPlatform = "xcb";
    platform = "xcb";
  }
  // GTK is only used by Qt's GTK platform theme dialogs here; keep it on the
  // same windowing system as Qt so dialogs get a transient parent.
  if (environment.gdkBackend.isEmpty()) {
    if (platform == "xcb") plan.gdkBackend = "x11";
    else if (isWaylandPlatform(platform) ||
             (platform.isEmpty() && environment.session() == DisplayServer::Wayland))
      plan.gdkBackend = "wayland";
  }
  return plan;
}

void applyDisplayPlan(const DisplayPlan &plan) {
  if (!plan.qpaPlatform.isEmpty()) qputenv("QT_QPA_PLATFORM", plan.qpaPlatform);
  if (!plan.gdkBackend.isEmpty()) qputenv("GDK_BACKEND", plan.gdkBackend);
}

bool qtUsesX11() {
  return QGuiApplication::platformName() == QLatin1String("xcb");
}

QString activeDisplayBackend() {
  const QString platform = QGuiApplication::platformName();
  if (platform == QLatin1String("xcb"))
    return DisplayEnvironment::fromProcess().session() == DisplayServer::Wayland ? QStringLiteral("xwayland")
                                                                                 : QStringLiteral("x11");
  if (platform.startsWith(QLatin1String("wayland"))) return QStringLiteral("wayland");
  return platform;
}

}  // namespace dalinira::engine
