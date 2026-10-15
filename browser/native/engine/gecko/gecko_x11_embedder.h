#pragma once

#include <QAbstractNativeEventFilter>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <optional>

#include <xcb/xcb.h>

class QWidget;

namespace dalinira::engine {

// Finds the Gecko host's X11 top-level window and embeds it in a Qt widget
// (QWindow::fromWinId + QWidget::createWindowContainer), and moves X input
// focus to it on click (Qt never sees clicks inside a foreign window).
//
// The window is accepted only if exactly one managed top-level has the
// host's _NET_WM_PID and the title "DNG-HOST <tag>" with the random tag only
// the authenticated host received, and the X server itself (X-Resource)
// reports the host as its owner.
//
// Needs Qt on xcb (XWayland on Wayland sessions); native Wayland cannot
// embed a foreign window. Not used by BrowserWindow yet.
class GeckoX11Embedder : public QObject, public QAbstractNativeEventFilter {
  Q_OBJECT

 public:
  struct HostWindow {
    xcb_window_t window = 0;
    qint64 netWmPid = 0;   // _NET_WM_PID (set by the client)
    qint64 serverPid = 0;  // owner according to the X server
    QString name;
    int candidatesForPid = 0;
  };

  explicit GeckoX11Embedder(QObject *parent = nullptr);
  ~GeckoX11Embedder() override;

  bool isUsable(QString *why) const;
  void setClickToFocus(bool enabled) { clickFocus_ = enabled; }

  std::optional<HostWindow> findHostWindow(qint64 pid, const QString &name, QString *why) const;

  QWidget *embed(const HostWindow &window, QWidget *parent);
  void forget();
  xcb_window_t embeddedWindow() const { return window_; }

  QJsonObject inspect(QWidget *topLevel, QWidget *container) const;
  xcb_window_t inputFocus() const;
  // False when the embedded window is not viewable (X11 refuses to focus it).
  bool focusEmbedded();
  void focusQt(QWidget *topLevel);

  // Whether a FocusOut only reports X input focus moving from Qt's top-level
  // into the embedded window inside it. Qt never sees the embedded window's
  // FocusIn (a foreign window has no xcb event listener), so it would take
  // this FocusOut for the window losing the keyboard and deactivate it 100 ms
  // later; Qt's setFocus() (the address bar on Ctrl+L) then only records the
  // focus child, no focus change follows and the page keeps the keyboard.
  // Such a FocusOut is kept from Qt.
  static bool isFocusMoveIntoEmbedded(const xcb_focus_out_event_t &event, xcb_window_t topLevel,
                                      xcb_window_t embedded, xcb_window_t focus);

  bool nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result) override;

 signals:
  void embeddedClicked();
  void embeddedGone(const QString &why);

 private:
  xcb_atom_t atom(const char *name) const;
  QByteArray property(xcb_window_t window, xcb_atom_t prop, xcb_atom_t type) const;
  qint64 serverPidOf(xcb_window_t window) const;
  QList<xcb_window_t> clientList() const;
  QList<xcb_window_t> topLevelCandidates() const;
  bool hasWindowManager() const;
  // The embedded window when X input focus is on it or inside it, else the
  // focus window itself.
  xcb_window_t focusWithinEmbedded() const;

  xcb_connection_t *conn_ = nullptr;
  xcb_window_t root_ = 0;
  xcb_window_t window_ = 0;
  QPointer<QWidget> topLevel_;  // the Qt window the embedded window is in
  bool clickFocus_ = true;
  bool grabbed_ = false;
};

}  // namespace dalinira::engine
