#pragma once

#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

class QWidget;

namespace dalinira::engine {

class GeckoEngineView;
class GeckoSession;
class GeckoX11Embedder;

// The Gecko host's one native window, shared by every GeckoEngineView: it is
// embedded once, when the session is ready and a view of a browser window is
// first shown, and then moved into whichever view is active. Only views of
// that window can show it (one window, experimental).
//
// The surface is never destroyed together with a view or the window: a
// foreign X11 window is destroyed with its parent window, which would close
// the host's only window. A view going away first moves it to a hidden
// child of the window; the window closing moves it out of the window.
class GeckoPresentationSurface : public QObject {
  Q_OBJECT

 public:
  explicit GeckoPresentationSurface(GeckoSession *session, QObject *parent = nullptr);
  ~GeckoPresentationSurface() override;

  // Shows the surface in `view`; false when it cannot be shown there (another
  // window owns it, or there is no X11 surface yet): status() says why.
  bool present(GeckoEngineView *view);
  bool isPresenting(const GeckoEngineView *view) const;
  void viewResized(GeckoEngineView *view);
  // Keyboard focus follows Qt's focus widget: the page gets the X11 input
  // focus while its view has Qt's focus (a click on the page gives the view
  // Qt's focus), and Qt gets it back when Qt's focus moves to another widget
  // of the window or the view is hidden. Only while the page holds the X11
  // focus is it ever moved, so no other application loses the keyboard.
  void focusPage(GeckoEngineView *view);
  void viewHidden(GeckoEngineView *view);
  void release(GeckoEngineView *view);
  QWidget *ownerWindow() const { return owner_; }
  // Where the host's window is in the X11 window tree (diagnostics, tests);
  // empty until it is embedded.
  QJsonObject inspect() const;
  QString status() const { return status_; }

 signals:
  void statusChanged(const QString &status);

 protected:
  bool eventFilter(QObject *watched, QEvent *event) override;

 private:
  void tryEmbed();
  void park();
  QWidget *parking();
  void setStatus(const QString &status);
  void returnKeyboardToQt();
  void onFocusChanged(QWidget *old, QWidget *now);

  QPointer<GeckoSession> session_;
  GeckoX11Embedder *embedder_ = nullptr;
  QPointer<QWidget> owner_;
  QPointer<QWidget> parking_;
  QPointer<QWidget> container_;
  QPointer<GeckoEngineView> current_;
  QTimer findTimer_;
  int findAttempts_ = 0;
  QString status_;
};

}  // namespace dalinira::engine
