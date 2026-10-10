#pragma once

#include <QWidget>
#include <QUrl>
#include <QString>
#include <QIcon>
#include <QVariant>
#include <QPoint>
#include <functional>
#include "engine/engine_type.h"
#include "engine/tab_lifecycle_state.h"

namespace dalinira::engine {

// A script injected at document start, described independently of engine
// script types (QWebEngineScript on Chromium, WebKitUserScript on WebKit).
struct DocumentScript {
  QString source;
  bool mainWorld = false;  // The page's own world (scriptlets patching page APIs).
  bool allFrames = false;
  bool reapply = false;    // Idempotent; re-run in the live document on refresh.
};

class EngineView : public QWidget {
  Q_OBJECT
 public:
  explicit EngineView(QWidget *parent = nullptr) : QWidget(parent) {}
  ~EngineView() override = default;

  virtual EngineType engineType() const { return EngineType::Chromium; }
  virtual QUrl url() const { return QUrl(); }
  virtual QString title() const { return QString(); }
  virtual bool isLoading() const { return false; }
  virtual QIcon icon() const { return QIcon(); }

  virtual void load(const QUrl &url) { Q_UNUSED(url); }
  virtual void reload() {}
  virtual void stop() {}
  virtual void back() {}
  virtual void forward() {}
  virtual bool canGoBack() const { return false; }
  virtual bool canGoForward() const { return false; }

  // Native engine views may own a surface outside QWidget's paint stack.
  // BrowserWindow calls this before and after changing the active stack page
  // so hidden native surfaces cannot remain mapped above shared Qt UI.
  virtual void setEngineActive(bool active) { Q_UNUSED(active); }
  virtual bool isEngineActive() const { return isVisible(); }

  // Shared tab lifecycle (see tab_lifecycle_state.h). Engines without a
  // mechanism stay Active and refuse other states.
  virtual TabLifecycleState lifecycleState() const { return TabLifecycleState::Active; }
  virtual bool setLifecycleState(TabLifecycleState state) { return state == TabLifecycleState::Active; }
  // Whether the page is currently producing sound.
  virtual bool isAudible() const { return false; }
  // A modal page dialog (alert/confirm/prompt/beforeunload) is open.
  virtual bool hasOpenDialog() const { return false; }
  // The OS process rendering this page (Chromium renderer, WebKit web
  // process); 0 while there is none (sleeping, not started) or it is unknown.
  // Feeds the shared tab resource model (desktop_tabs/tab_resource_model.h).
  virtual qint64 contentProcessId() const { return 0; }

  // The document scripts this view installs at each navigation changed (a
  // browser service's answer for the current URL): reinstall them for the
  // next document. Engines without per-view document scripts ignore it.
  virtual void refreshDocumentScripts() {}

  virtual void runJavaScript(const QString &script,
                             std::function<void(const QVariant &)> callback = {}) {
    Q_UNUSED(script);
    if (callback) callback(QVariant());
  }
  // Runs `script` in DaliNira's isolated JavaScript world of the page (the
  // world of the blocker's cosmetic runtime): the page's own scripts can
  // neither read nor call what lives there.
  virtual void runIsolatedJavaScript(const QString &script,
                                     std::function<void(const QVariant &)> callback = {}) {
    Q_UNUSED(script);
    if (callback) callback(QVariant());
  }

  virtual qreal zoomFactor() const { return 1.0; }
  virtual void setZoomFactor(qreal factor) { Q_UNUSED(factor); }

  virtual void findText(const QString &text, bool forward = true, bool caseSensitive = false) {
    Q_UNUSED(text);
    Q_UNUSED(forward);
    Q_UNUSED(caseSensitive);
  }
  virtual void clearFind() {}

  // Leaves page (element) fullscreen, e.g. when the user presses Escape.
  virtual void exitFullScreen() {}

  virtual void print() {}
  virtual void printToPdf(const QString &filePath) { Q_UNUSED(filePath); }

 signals:
  void urlChanged(const QUrl &url);
  void titleChanged(const QString &title);
  void loadStarted();
  void loadFinished(bool success);
  void loadProgress(int progress);
  void iconChanged(const QIcon &icon);
  void newWindowRequested(const QUrl &url);
  void webProcessTerminated(int reason);
  void findResultFound(int activeMatch, int numberOfMatches);
  void internalNavigationRequested(const QUrl &url);
  // Real content-blocker hits observed in this view (e.g. "cosmetic",
  // "procedural"); BrowserWindow routes them to the shared blocker stats.
  void contentBlockerHit(quint64 count, const QString &kind);
  // The page entered (true) or left (false) element fullscreen; the browser
  // window follows, exactly like QWebEnginePage::fullScreenRequested.
  void fullScreenRequested(bool on);
  // Pointer motion over the page, in global Qt coordinates, from engines whose
  // page is a separate native window that hides pointer moves from Qt
  // (WebKit's XEmbed surface). Event-driven: one per real pointer move.
  // Chromium pages are Qt widgets; Qt's own mouse events cover them.
  void pagePointerMoved(const QPoint &globalPos);
  void lifecycleStateChanged(dalinira::TabLifecycleState state);
  void audibleChanged(bool audible);
  void dialogStateChanged(bool open);
  void linkContextMenuRequested(const QUrl &linkUrl, const QString &linkText, const QPoint &globalPos);
  void selectionContextMenuRequested(const QString &selectedText, const QPoint &globalPos);
  void pageContextMenuRequested(const QPoint &globalPos);
  // The page called window.close() on a window it was allowed to close (one
  // a script opened); the browser closes the tab.
  void windowCloseRequested();
};

} // namespace dalinira::engine
