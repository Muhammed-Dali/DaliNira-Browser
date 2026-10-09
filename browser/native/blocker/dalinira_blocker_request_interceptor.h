#pragma once

#include <QMutex>
#include <QObject>
#include <QString>
#include <QWebEngineUrlRequestInfo>
#include <QWebEngineUrlRequestInterceptor>
#include <QWebEngineUrlSchemeHandler>
#include <functional>

#include "dalinira_blocker_types.h"

class DaliNiraBlockerRedirectResources;
class DaliNiraBlockerService;
class QWebEnginePage;

// Chromium adapter: applies the shared network verdict to QtWebEngine
// requests. Chromium does not follow a subresource redirect to a data: URL
// (the load fails, so a packaged substitute would act as a plain block); a
// redirect to a packaged resource therefore goes to the resource scheme below
// instead, which serves the same bytes WebKit receives as data: URLs.
class DaliNiraBlockerRequestInterceptor final : public QWebEngineUrlRequestInterceptor {
  Q_OBJECT
 public:
  explicit DaliNiraBlockerRequestInterceptor(DaliNiraBlockerService *service, QObject *parent = nullptr);
  ~DaliNiraBlockerRequestInterceptor() override = default;

  void interceptRequest(QWebEngineUrlRequestInfo &info) override;

  // Registers the resource scheme with QtWebEngine; call once at start-up,
  // before any QtWebEngine object exists. Without it resource redirects stay
  // data: URLs.
  static void registerResourceScheme();
  static bool resourceSchemeRegistered();
  static QByteArray resourceScheme();

 private:
  DaliNiraBlockerService *service_ = nullptr;
};

// Serves "<scheme>:<session token>/<packaged file>", read-only, and nothing
// else: unknown names, other tokens and any other form are refused. The token
// is random per blocker service, so pages cannot probe for the scheme without
// having been redirected to it.
class DaliNiraBlockerResourceSchemeHandler final : public QWebEngineUrlSchemeHandler {
  Q_OBJECT
 public:
  DaliNiraBlockerResourceSchemeHandler(const DaliNiraBlockerRedirectResources *resources, const QString &token,
                                       QObject *parent = nullptr);
  void requestStarted(QWebEngineUrlRequestJob *job) override;

 private:
  const DaliNiraBlockerRedirectResources *resources_ = nullptr;
  QString token_;
};

// Chromium adapter for $popup rules. QtWebEngine reports a page's request for
// a new window or tab (window.open, target=_blank) with its target URL through
// newWindowRequested before it falls back to createWindow(). The guard asks
// the shared DaliNiraBlockerService::decidePopup() there; for a blocked popup
// the page's createWindow() must return no page (consumeBlockedWindow()), so
// no tab or window is created.
class DaliNiraBlockerPopupGuard final : public QObject {
  Q_OBJECT
 public:
  using ServiceProvider = std::function<DaliNiraBlockerService *()>;
  DaliNiraBlockerPopupGuard(QWebEnginePage *page, ServiceProvider provider);
  // Call first in createWindow(): true when this window must not be created.
  bool consumeBlockedWindow();

 private:
  ServiceProvider provider_;
  bool blockNext_ = false;
};

using AdBlockRequestInterceptor = DaliNiraBlockerRequestInterceptor;
