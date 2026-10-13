#include <QEventLoopLocker>
#include <QDesktopServices>
#include <QDir>
#include <QDateTime>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QCompleter>
#include <QStandardItemModel>
#include <QAbstractItemView>
#include <QCursor>
#include <QGuiApplication>
#include <QScreen>
#include "core/search_suggestion_service.h"
#include "core/suggestion_remove_delegate.h"
#include "newtab/new_tab_shortcuts.h"
#include "newtab/new_tab_suggest_command.h"
#include "core/search_engine_definition.h"
#include "core/default_browser_integration.h"
#include "browser_window.h"
#include "browser_window_internal.h"
#include "engine/engine_backend.h"
#include "engine/engine_view.h"
#if defined(DALINIRA_BUILD_WEBKIT)
#include "engine/webkit/webkit_engine_view.h"
#endif
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QImageReader>
#include <QBuffer>
#include <QProcess>
#include "i18n/i18n.h"
#include "i18n/language_manager.h"
#include "onboarding/onboarding_view.h"
#include "onboarding/onboarding_manager.h"
#include "onboarding/browser_data_importer.h"
#include "onboarding/search_privacy_step.h"
#include "engine/engine_resolver.h"
#include "layout/browser_layout.h"
#include "layout/browser_layout_controller.h"
#include "layout/browser_side_panel.h"

using dalinira::i18n::LanguageManager;
using dalinira::i18n::I18n;

#include <QApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QAction>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QWebEngineContextMenuRequest>
#include <QMessageBox>
#include <QInputDialog>
#include <QMouseEvent>
#include <QProgressBar>
#include <QStackedWidget>
#include <QStyle>
#include <QPainter>
#include <QPainterPath>
#include <QParallelAnimationGroup>
#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>
#include <QSettings>
#include <QScopedValueRollback>
#include <QToolButton>
#include <QToolBar>
#include <QUrl>
#include <QScopeGuard>
#include <QUrlQuery>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QWebEngineHistory>
#include <QWebEngineNewWindowRequest>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineScriptCollection>
#include <QWebChannel>
#include <QWindow>
#include <atomic>
#include <algorithm>
#include <utility>

#include "desktop_tabs/tab_drag_controller.h"
#include "desktop_tabs/tab_strip_widget.h"
#include "desktop_tabs/tab_window_registry.h"
#include "desktop_tabs/tab_search_popup.h"
#include "desktop_tabs/tab_group_model.h"
#include "desktop_tabs/tab_group_popup.h"
#include "desktop_tabs/tab_group_launcher_popup.h"
#include "desktop_tabs/tab_hover_card.h"
#include "desktop_tabs/tab_performance_manager.h"
#include "desktop_tabs/tab_throbber.h"
#include "core/browser_icons.h"
#if QT_VERSION < QT_VERSION_CHECK(6, 8, 0)
#include "core/browser_permission_policy.h"
#endif
#include "core/browser_ui_metrics.h"
#include "core/address_input_resolver.h"
#include "core/domain_normalizer.h"
#include "core/composite_navigation_candidate_provider.h"
#include "core/history_candidate_provider.h"
#include "core/bookmark_candidate_provider.h"
#include "core/frequent_sites_candidate_provider.h"
#include "core/adult_content_protection.h"
#include "core/engine_ui_profile.h"
#include "newtab/new_tab_html.h"
#include "newtab/new_tab_background_store.h"
#include "newtab/new_tab_scheme.h"
#include "blocker/dalinira_blocker_engine.h"
#include "blocker/dalinira_blocker_service.h"
#include "passwords/credential_vault_manager.h"
#include "passwords/credential_autofill_controller.h"
#include "passwords/webkit_credential_controller.h"
#include "pulse/song_finder_settings_page.h"
#include "downloads/download_desktop_actions.h"
#include "downloads/download_toolbar_ui.h"
#include "downloads/download_ui_model.h"
#include "downloads/local_media_player_page.h"
#include "downloads/media_page_url_resolver.h"
#include "translate/translate_service.h"
#include "desktop_tabs/find_bar_widget.h"
#include "core/bookmark_bar_reorder.h"
#include <QShortcut>
#include <QCheckBox>
#include <QSignalBlocker>
#include <QtMath>
#include <cmath>
#include <QPrinter>
#include <QPrintDialog>
#include <QFileDialog>
#include <QPushButton>
#include <QTextBrowser>
#include <QVBoxLayout>
#include <QWebEngineFindTextResult>

namespace {
static std::atomic<uint64_t> s_tabIdSequence{1};

bool startDetachedBrowserQuietly(const QString &program,
                                 const QStringList &arguments) {
  QProcess process;
  process.setProgram(program);
  process.setArguments(arguments);
  process.setStandardOutputFile(QProcess::nullDevice());
  process.setStandardErrorFile(QProcess::nullDevice());
  return process.startDetached();
}

// NOTE: isNewTabUrl is defined inline in browser_window_internal.h


QString navigationUrlKey(const QUrl &url) {
  if (!url.isValid()) return {};
  return url.adjusted(QUrl::NormalizePathSegments | QUrl::RemoveFragment | QUrl::StripTrailingSlash)
      .toString(QUrl::FullyEncoded);
}

bool isInternalOrNonWebUrl(const QUrl &url) {
  if (!url.isValid() || url.isEmpty()) return true;
  const QString scheme = url.scheme().toLower();
  if (scheme == QLatin1String("dalinira") ||
      scheme == QLatin1String("about") ||
      scheme == QLatin1String("data") ||
      scheme == QLatin1String("file") ||
      scheme == QLatin1String("chrome") ||
      scheme == QLatin1String("edge")) {
    return true;
  }
  return isNewTabUrl(url);
}


QString bookmarkDisplayName(const QUrl &url) {
  const QString host = url.host().toLower();
  if (host.endsWith(QStringLiteral("youtube.com"))) return QStringLiteral("YouTube");
  if (host.endsWith(QStringLiteral("github.com"))) return QStringLiteral("GitHub");
  if (host.endsWith(QStringLiteral("wikipedia.org"))) return QStringLiteral("Wikipedia");
  if (host.endsWith(QStringLiteral("google.com"))) return QStringLiteral("Google");
  if (host.endsWith(QStringLiteral("duckduckgo.com"))) return QStringLiteral("DuckDuckGo");
  if (host.endsWith(QStringLiteral("startpage.com"))) return QStringLiteral("Startpage");
  if (host.endsWith(QStringLiteral("mojeek.com"))) return QStringLiteral("Mojeek");
  if (host.endsWith(QStringLiteral("facebook.com"))) return QStringLiteral("Facebook");
  if (host.endsWith(QStringLiteral("instagram.com"))) return QStringLiteral("Instagram");
  if (host.endsWith(QStringLiteral("openai.com")) || host.endsWith(QStringLiteral("chatgpt.com"))) return QStringLiteral("ChatGPT");
  if (host.endsWith(QStringLiteral("gitlab.com"))) return QStringLiteral("GitLab");
  if (host.endsWith(QStringLiteral("twitter.com")) || host.endsWith(QStringLiteral("x.com"))) return QStringLiteral("X");
  if (host.endsWith(QStringLiteral("threads.net"))) return QStringLiteral("Threads");
  if (host.endsWith(QStringLiteral("reddit.com"))) return QStringLiteral("Reddit");
  if (host.endsWith(QStringLiteral("tiktok.com"))) return QStringLiteral("TikTok");
  if (host.endsWith(QStringLiteral("linkedin.com"))) return QStringLiteral("LinkedIn");
  QString clean = host;
  if (clean.startsWith(QStringLiteral("www."))) clean.remove(0, 4);
  return clean.isEmpty() ? url.toDisplayString() : clean;
}

QIcon bookmarkIcon(bool bookmarked) {
  // The DaliNira honeycomb cell: a regular, upright hexagon centred in the
  // icon (it was stretched taller than wide), with a proportional mark.
  QIcon icon;
  for (const int size : {32, 64}) {
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    const qreal unit = size / 32.0;
    const QPointF centre(16 * unit, 16 * unit);
    const qreal radius = 12.5 * unit;
    QPainterPath honeycomb;
    for (int corner = 0; corner < 6; ++corner) {
      const qreal angle = qDegreesToRadians(-90.0 + 60.0 * corner);
      const QPointF point(centre.x() + radius * std::cos(angle), centre.y() + radius * std::sin(angle));
      if (corner == 0) honeycomb.moveTo(point);
      else honeycomb.lineTo(point);
    }
    honeycomb.closeSubpath();
    painter.setPen(QPen(QColor(bookmarked ? QStringLiteral("#4fc3f7") : QStringLiteral("#d8dce0")),
                        2.0 * unit, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(bookmarked ? QBrush(QColor(QStringLiteral("#1688ad"))) : Qt::NoBrush);
    painter.drawPath(honeycomb);
    painter.setPen(QPen(QColor(bookmarked ? QStringLiteral("#ffffff") : QStringLiteral("#4fc3f7")),
                        2.2 * unit, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    if (bookmarked) {
      painter.drawLine(QPointF(11 * unit, 16.5 * unit), QPointF(14.5 * unit, 20 * unit));
      painter.drawLine(QPointF(14.5 * unit, 20 * unit), QPointF(21 * unit, 12.5 * unit));
    } else {
      painter.drawLine(QPointF(11.5 * unit, 16 * unit), QPointF(20.5 * unit, 16 * unit));
      painter.drawLine(QPointF(16 * unit, 11.5 * unit), QPointF(16 * unit, 20.5 * unit));
    }
    painter.end();
    icon.addPixmap(pixmap);
  }
  return icon;
}


class BrowserWebPage final : public QWebEnginePage {
 public:
  BrowserWebPage(QWebEngineProfile *profile, BrowserWindow *window, QObject *parent = nullptr)
      : QWebEnginePage(profile, parent), window_(window) {
    setBackgroundColor(QColor(7, 17, 31));
    installMediaCaptureHook();
    credentialChannel_ = new QWebChannel(this);
    credentialBridge_ = new CredentialMessageBridge(
        this, window ? window->autofillController() : nullptr, credentialChannel_);
    credentialChannel_->registerObject(QStringLiteral("daliniraCredentials"), credentialBridge_);
    setWebChannel(credentialChannel_, QWebEngineScript::ApplicationWorld);
    scripts().insert(CredentialAutofillController::candidateCaptureScript());
    popupGuard_ = new DaliNiraBlockerPopupGuard(this, [window = QPointer<BrowserWindow>(window)]() -> DaliNiraBlockerService * {
      return window && window->services().profileService ? window->services().profileService->adBlockService() : nullptr;
    });
    // createWindow() deliberately returns null below, so Qt emits this richer
    // request before any destination page is adopted. The popup guard runs
    // first and records the shared filter decision; only an allowed request
    // is given a real browser tab/window here.
    connect(this, &QWebEnginePage::newWindowRequested, this,
            [this](QWebEngineNewWindowRequest &request) {
      if (!window_ || (popupGuard_ && popupGuard_->consumeBlockedWindow())) return;
      QWebEnginePage *destinationPage = nullptr;
      if (request.destination() == QWebEngineNewWindowRequest::InNewWindow ||
          request.destination() == QWebEngineNewWindowRequest::InNewDialog) {
        auto *newWindow = BrowserWindow::createWindow(window_->services());
        newWindow->addNewTab(QUrl{});
        newWindow->show();
        if (newWindow->currentView()) destinationPage = newWindow->currentView()->page();
      } else {
        int oldIndex = -1;
        const QWebEngineView *oldView = window_->currentView();
        const auto &oldTabs = window_->allTabs();
        for (int i = 0; i < oldTabs.size(); ++i) {
          if (oldTabs.at(i).view == oldView) { oldIndex = i; break; }
        }
        const int newIndex = window_->addNewTab(QUrl{});
        if (newIndex >= 0 && newIndex < window_->tabCount()) {
          const auto tab = window_->allTabs().at(newIndex);
          if (tab.view) destinationPage = tab.view->page();
        }
        if (request.destination() == QWebEngineNewWindowRequest::InNewBackgroundTab && oldIndex >= 0)
          window_->switchTab(oldIndex);
      }
      if (destinationPage) {
        const QUrl requestedUrl = request.requestedUrl();
        if (!requestedUrl.isValid() || requestedUrl.isEmpty() ||
            requestedUrl == QUrl(QStringLiteral("about:blank"))) {
          if (auto *popupPage = dynamic_cast<BrowserWebPage *>(destinationPage))
            popupPage->armDeferredPopupCheck(url());
        }
        request.openIn(destinationPage);
      }
    });
  }

  void armDeferredPopupCheck(const QUrl &openerUrl) {
    deferredPopupOpener_ = openerUrl;
    deferredPopupCheckPending_ = true;
  }

  void setMediaCaptureCallback(std::function<void(bool, bool)> cb) {
    onMediaCaptureChanged_ = std::move(cb);
  }

  void resetMediaCapture() {
    if (onMediaCaptureChanged_) onMediaCaptureChanged_(false, false);
  }

 protected:
  // A tab showing a page dialog is protected from sleeping (the dialog and
  // the page's pending script would be lost).
  void javaScriptAlert(const QUrl &securityOrigin, const QString &msg) override {
    if (window_) window_->setPageDialogActive(this, true);
    const QPointer<BrowserWindow> window(window_);
    QWebEnginePage::javaScriptAlert(securityOrigin, msg);
    if (window) window->setPageDialogActive(this, false);
  }
  bool javaScriptConfirm(const QUrl &securityOrigin, const QString &msg) override {
    if (window_) window_->setPageDialogActive(this, true);
    const QPointer<BrowserWindow> window(window_);
    const bool accepted = QWebEnginePage::javaScriptConfirm(securityOrigin, msg);
    if (window) window->setPageDialogActive(this, false);
    return accepted;
  }
  bool javaScriptPrompt(const QUrl &securityOrigin, const QString &msg, const QString &defaultValue,
                        QString *result) override {
    if (window_) window_->setPageDialogActive(this, true);
    const QPointer<BrowserWindow> window(window_);
    const bool accepted = QWebEnginePage::javaScriptPrompt(securityOrigin, msg, defaultValue, result);
    if (window) window->setPageDialogActive(this, false);
    return accepted;
  }

  void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel level,
                                const QString &message,
                                int lineNumber,
                                const QString &sourceID) override {
    if (message.startsWith(QLatin1String("__DALINIRA_MEDIA_CAPTURE__:"))) {
      const bool v = message.contains(QLatin1String("V1"));
      const bool a = message.contains(QLatin1String("A1"));
      if (onMediaCaptureChanged_) onMediaCaptureChanged_(v, a);
      return;
    }
    if (message.startsWith(QLatin1String("__DALINIRA_ADBLOCK_HIT__:"))) {
      const QStringList parts = message.split(QLatin1Char(':'));
      quint64 count = 1;
      if (parts.size() > 1) {
        bool ok = false;
        const quint64 parsed = parts[1].toULongLong(&ok);
        if (ok && parsed > 0) count = parsed;
      }
      auto *view = qobject_cast<QWebEngineView *>(parent());
      if (view && window_) {
        const quint64 tabId = reinterpret_cast<quintptr>(view);
        auto *blocker = window_->services().profileService ? window_->services().profileService->adBlockService() : nullptr;
        if (blocker) {
          const QString subType = parts.value(2);
          blocker->reportBlockedEvent(tabId, DaliNiraBlockType::Cosmetic, count, subType);
        }
      }
      return;
    }
    QWebEnginePage::javaScriptConsoleMessage(level, message, lineNumber, sourceID);
  }

  bool acceptNavigationRequest(const QUrl &url, NavigationType type, bool isMainFrame) override {
    if (isMainFrame && onMediaCaptureChanged_) {
      onMediaCaptureChanged_(false, false);
    }
    if (url.scheme() == QLatin1String("dalinira") && url.host() == QLatin1String("suggest")) {
      const QUrlQuery params(url);
      const QString capability = property("dalinira-suggest-capability").toString();
      auto *view = qobject_cast<QWebEngineView *>(parent());
      auto *window = view ? qobject_cast<BrowserWindow *>(view->window()) : nullptr;
      if (!isNewTabUrl(this->url()) || capability.isEmpty() ||
          params.queryItemValue(QStringLiteral("cap")) != capability || !window ||
          window->currentView() != view || profile() != window->services().profile || url.toString().size() > 4096)
        return false;
      const auto command = NewTabSuggestCommand::parse(url);
      auto *service = window->services().profileService;
      using Op = NewTabSuggestCommand::Op;
      if (command.op == Op::Consent && service && !profile()->isOffTheRecord()) {
        service->setSearchSuggestionsEnabled(command.enabled);
      } else if (command.op == Op::Query) {
        if (view->hasFocus() || view->isAncestorOf(QApplication::focusWidget()))
          window->requestNewTabSuggestions(this, command.text, command.id);
      } else if (command.op == Op::Activate) {
        const QPointer<QWebEngineView> guardedView(view);
        const QString text = command.text;
        QMetaObject::invokeMethod(view, [guardedView, text] {
          if (!guardedView || !isNewTabUrl(guardedView->url())) return;
          if (auto *owner = qobject_cast<BrowserWindow *>(guardedView->window());
              owner && owner->currentView() == guardedView)
            owner->activateSearchSuggestion(text);
        }, Qt::QueuedConnection);
      } else if ((command.op == Op::DeleteHistory || command.op == Op::DeleteSuggestion) && service && !profile()->isOffTheRecord()) {
        if (service->removeSuggestion(command.op == Op::DeleteHistory ? QStringLiteral("search-history") : command.type,
                                      command.text, command.url))
          window->requestNewTabSuggestions(this, command.input, command.id);
      }
      return false;
    }
    if (url.scheme() == QLatin1String("dalinira") && url.host() == QLatin1String("search-engine")) {
      const QString engine = QUrlQuery(url).queryItemValue(QStringLiteral("engine"));
      const bool allowed = QStringList{QStringLiteral("Google"), QStringLiteral("DuckDuckGo"), QStringLiteral("Startpage"), QStringLiteral("Mojeek")}.contains(engine);
      const QPointer<QWebEngineView> view(qobject_cast<QWebEngineView *>(parent()));
      const QString capability = property("dalinira-suggest-capability").toString();
      if (isMainFrame && isNewTabUrl(this->url()) && allowed && view && !capability.isEmpty() &&
          QUrlQuery(url).queryItemValue(QStringLiteral("cap")) == capability) {
        QMetaObject::invokeMethod(view, [view, engine] {
          if (!view || !isNewTabUrl(view->url())) return;
          if (auto *window = qobject_cast<BrowserWindow *>(view->window())) window->setSearchEngine(engine);
        }, Qt::QueuedConnection);
      }
      return false;
    }
    if (url.scheme() == QLatin1String("dalinira") && url.host() == QLatin1String("card-settings")) {
      const QUrlQuery q(url);
      const bool downloads = q.queryItemValue(QStringLiteral("downloads")) == QLatin1String("1");
      const bool blocked = q.queryItemValue(QStringLiteral("blocked")) == QLatin1String("1");
      QString mode = q.queryItemValue(QStringLiteral("mode"));
      if (mode != QLatin1String("session") && mode != QLatin1String("all_time")) mode = QStringLiteral("all_time");

      const QPointer<QWebEngineView> view(qobject_cast<QWebEngineView *>(parent()));
      const QString capability = property("dalinira-suggest-capability").toString();
      if (isMainFrame && isNewTabUrl(this->url()) && view && !capability.isEmpty() &&
          q.queryItemValue(QStringLiteral("cap")) == capability) {
        QMetaObject::invokeMethod(view, [view, downloads, blocked, mode] {
          if (!view || !isNewTabUrl(view->url())) return;
          dalinira::ui_profile::setVisualValue(QStringLiteral("browser/showDownloadsCard"), downloads);
          dalinira::ui_profile::setVisualValue(QStringLiteral("browser/showBlockedCard"), blocked);
          dalinira::ui_profile::setVisualValue(QStringLiteral("browser/blockedCounterMode"), mode);
          dalinira::ui_profile::setVisualValue(QStringLiteral("browser/cards"), downloads || blocked);
          if (auto *window = qobject_cast<BrowserWindow *>(view->window())) {
            window->syncNewTabViews();
          }
        }, Qt::QueuedConnection);
      }
      return false;
    }
    if (url.scheme() == QLatin1String("dalinira") && url.host() == QLatin1String("background-preference")) {
      const QUrlQuery query(url);
      const QString source = query.queryItemValue(QStringLiteral("source"));
      static const QStringList allowedSources{
          QStringLiteral("builtin"), QStringLiteral("custom"), QStringLiteral("solid-indigo"),
          QStringLiteral("solid-magenta"), QStringLiteral("solid-teal"), QStringLiteral("solid-ember"),
          QStringLiteral("gradient-violet"), QStringLiteral("gradient-aurora"),
          QStringLiteral("gradient-ocean"), QStringLiteral("gradient-sunset")};
      const QPointer<QWebEngineView> view(qobject_cast<QWebEngineView *>(parent()));
      const QString capability = property("dalinira-suggest-capability").toString();
      if (isMainFrame && isNewTabUrl(this->url()) && view && allowedSources.contains(source) &&
          !capability.isEmpty() && query.queryItemValue(QStringLiteral("cap")) == capability) {
        QMetaObject::invokeMethod(view, [view, source, visible = query.queryItemValue(QStringLiteral("visible")) == QLatin1String("1")] {
          if (!view || !isNewTabUrl(view->url())) return;
          dalinira::ui_profile::setVisualValue(QStringLiteral("browser/backgroundSource"), source);
          dalinira::ui_profile::setVisualValue(QStringLiteral("browser/backgroundVisible"), visible);
          dalinira::ui_profile::setVisualValue(QStringLiteral("browser/backgroundPreferenceSet"), true);
          if (auto *window = qobject_cast<BrowserWindow *>(view->window())) window->syncNewTabViews();
        }, Qt::QueuedConnection);
      }
      return false;
    }
    if (url.scheme() == QLatin1String("dalinira") && url.host() == QLatin1String("frequent-sites")) {
      const QUrlQuery q(url);
      const QString op = q.queryItemValue(QStringLiteral("op"));
      const QString targetUrlStr = q.queryItemValue(QStringLiteral("url"), QUrl::FullyDecoded);
      const QString name = q.queryItemValue(QStringLiteral("name"), QUrl::FullyDecoded);
      const QString original = q.queryItemValue(QStringLiteral("original"), QUrl::FullyDecoded);
      const QPointer<QWebEngineView> view(qobject_cast<QWebEngineView *>(parent()));
      const QString capability = property("dalinira-suggest-capability").toString();
      if (isMainFrame && isNewTabUrl(this->url()) && view &&
          !capability.isEmpty() && q.queryItemValue(QStringLiteral("cap")) == capability && url.toString().size() <= 4096) {
        QMetaObject::invokeMethod(view, [view, op, targetUrlStr, name, original] {
          if (!view || !isNewTabUrl(view->url())) return;
          if (auto *window = qobject_cast<BrowserWindow *>(view->window())) {
            window->handleFrequentSitesCommand(op, QUrl(targetUrlStr), QStringLiteral("chromium"), name, original);
          }
        }, Qt::QueuedConnection);
      }
      return false;
    }
    if (url.scheme() == QLatin1String("dalinira") && url.host() == QLatin1String("newtab-background")) {
      const QUrlQuery query(url);
      const QString command = query.queryItemValue(QStringLiteral("op"));
      const QString capability = property("dalinira-suggest-capability").toString();
      auto *view = qobject_cast<QWebEngineView *>(parent());
      auto *window = view ? qobject_cast<BrowserWindow *>(view->window()) : nullptr;
      if (isMainFrame && isNewTabUrl(this->url()) && view && window &&
          window->currentView() == view && profile() == window->services().profile &&
          !profile()->isOffTheRecord() && !capability.isEmpty() &&
          query.queryItemValue(QStringLiteral("cap")) == capability &&
          (command == QLatin1String("pick") || command == QLatin1String("remove")) &&
          url.toString().size() <= 1024) {
        const QPointer<QWebEnginePage> guardedPage(this);
        QMetaObject::invokeMethod(view, [guardedPage, command, capability] {
          if (!guardedPage) return;
          auto *guardedView = qobject_cast<QWebEngineView *>(guardedPage->parent());
          auto *owner = guardedView ? qobject_cast<BrowserWindow *>(guardedView->window()) : nullptr;
          if (owner) owner->handleNewTabBackgroundCommand(guardedPage, command, capability);
        }, Qt::QueuedConnection);
      }
      return false;
    }
    if (url.scheme().compare(QLatin1String("dalinira"), Qt::CaseInsensitive) == 0 &&
        url.host().compare(QLatin1String("navigate"), Qt::CaseInsensitive) == 0) {
      // 1. Strict origin validation: Only dalinira://newtab or dalinira://newtab/ is permitted
      const QUrl sourceUrl = this->url();
      const QString capability = property("dalinira-suggest-capability").toString();
      auto *sourceView = qobject_cast<QWebEngineView *>(parent());
      auto *owner = sourceView ? qobject_cast<BrowserWindow *>(sourceView->window()) : nullptr;
      const bool trustedSource = isNewTabUrl(sourceUrl) && !capability.isEmpty() && owner &&
          owner->currentView() == sourceView && profile() == owner->services().profile &&
          QUrlQuery(url).queryItemValue(QStringLiteral("cap")) == capability;
      if (!trustedSource) {
        qWarning() << "[Security] Denied unauthorized internal navigation";
        return false;
      }

      // 2. Query size & input safety
      const QUrlQuery query(url);
      const QString rawQuery = query.queryItemValue(QStringLiteral("q"), QUrl::FullyDecoded);
      if (rawQuery.trimmed().isEmpty() || rawQuery.length() > 4096) {
        return false;
      }

      // 3. Search engine parameter validation (whitelist)
      const QString rawEngine = query.queryItemValue(QStringLiteral("engine"), QUrl::FullyDecoded).trimmed();
      QString validatedEngine;
      if (rawEngine.compare(QLatin1String("Google"), Qt::CaseInsensitive) == 0) {
        validatedEngine = QStringLiteral("Google");
      } else if (rawEngine.compare(QLatin1String("DuckDuckGo"), Qt::CaseInsensitive) == 0) {
        validatedEngine = QStringLiteral("DuckDuckGo");
      } else if (rawEngine.compare(QLatin1String("Startpage"), Qt::CaseInsensitive) == 0) {
        validatedEngine = QStringLiteral("Startpage");
      } else if (rawEngine.compare(QLatin1String("Mojeek"), Qt::CaseInsensitive) == 0) {
        validatedEngine = QStringLiteral("Mojeek");
      } else if (rawEngine.compare(QLatin1String("Brave Search"), Qt::CaseInsensitive) == 0 ||
                 rawEngine.compare(QLatin1String("Brave"), Qt::CaseInsensitive) == 0 ||
                 rawEngine.compare(QLatin1String("Bing"), Qt::CaseInsensitive) == 0) {
        validatedEngine = QStringLiteral("Google");
      } else if (window_) {
        validatedEngine = window_->currentSearchEngine();
      } else {
        validatedEngine = QStringLiteral("DuckDuckGo");
      }

      // Revalidate the current native owner at execution time (tabs may move).
      const QPointer<QWebEngineView> view(qobject_cast<QWebEngineView *>(parent()));
      if (view) {
        QMetaObject::invokeMethod(view, [view, rawQuery, validatedEngine, capability] {
          if (!view || !isNewTabUrl(view->url()) ||
              view->page()->property("dalinira-suggest-capability").toString() != capability) return;
          auto *window = qobject_cast<BrowserWindow *>(view->window());
          if (window && window->currentView() == view && view->page()->profile() == window->services().profile)
            window->navigateFromUserInput(rawQuery, validatedEngine);
        }, Qt::QueuedConnection);
      }
      return false;
    }
    if (isMainFrame) {
      if (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https")) {
        auto *view = qobject_cast<QWebEngineView *>(parent());
        auto *window = window_ ? window_.data() : (view ? qobject_cast<BrowserWindow *>(view->window()) : nullptr);
        auto *profileService = window ? window->services().profileService : nullptr;
        if (deferredPopupCheckPending_) {
          deferredPopupCheckPending_ = false;
          auto *blocker = profileService ? profileService->adBlockService() : nullptr;
          const quint64 tabId = reinterpret_cast<quintptr>(view);
          if (blocker && blocker->decidePopup(url, deferredPopupOpener_, tabId).action ==
                             DaliNiraBlockerAction::Block) {
            const QPointer<QWebEngineView> guardedView(view);
            const QPointer<BrowserWindow> guardedWindow(window);
            QMetaObject::invokeMethod(this, [guardedView, guardedWindow] {
              if (!guardedView || !guardedWindow) return;
              const auto &tabs = guardedWindow->allTabs();
              for (int i = 0; i < tabs.size(); ++i) {
                if (tabs.at(i).view == guardedView) {
                  if (tabs.size() > 1) guardedWindow->closeTab(i);
                  else guardedWindow->close();
                  return;
                }
              }
            }, Qt::QueuedConnection);
            return false;
          }
        }
        const bool adultProtection = profileService ? profileService->isAdultContentProtectionEnabled() : true;
        if (adultProtection && dalinira::core::AdultContentProtectionService::instance().isBlocked(url)) {
          const QPointer<BrowserWebPage> guardedPage(this);
          QMetaObject::invokeMethod(this, [guardedPage] {
            if (guardedPage) {
              guardedPage->setUrl(QUrl(QStringLiteral("dalinira://blocked/adult")));
            }
          }, Qt::QueuedConnection);
          return false;
        }
      }
      if (auto *view = qobject_cast<QWebEngineView *>(parent())) {
        if (auto *window = window_ ? window_.data() : qobject_cast<BrowserWindow *>(view->window())) {
          if ((url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https")) &&
              profile() == window->services().profile) {
            window->prepareAdBlockScripts(this, url);
          }
          window->notifyNavigationStarted(view, url);
        }
      }
    }
    return QWebEnginePage::acceptNavigationRequest(url, type, isMainFrame);
  }

 private:
  void installMediaCaptureHook() {
    static const QString s_hookScript = QStringLiteral(R"JS(
(function() {
  if (!window.location || (window.location.protocol !== 'http:' && window.location.protocol !== 'https:')) return;
  if (window.__daliniraMediaHookInstalled) return;
  window.__daliniraMediaHookInstalled = true;
  if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) return;
  var origGetUserMedia = navigator.mediaDevices.getUserMedia.bind(navigator.mediaDevices);
  var activeVideoTracks = 0;
  var activeAudioTracks = 0;
  function updateCapture() {
    console.debug('__DALINIRA_MEDIA_CAPTURE__:V' + (activeVideoTracks > 0 ? '1' : '0') + 'A' + (activeAudioTracks > 0 ? '1' : '0'));
  }
  navigator.mediaDevices.getUserMedia = function(constraints) {
    return origGetUserMedia(constraints).then(function(stream) {
      var vt = stream.getVideoTracks();
      var at = stream.getAudioTracks();
      if (vt.length > 0) {
        activeVideoTracks += vt.length;
        vt.forEach(function(t) {
          t.addEventListener('ended', function() {
            activeVideoTracks = Math.max(0, activeVideoTracks - 1);
            updateCapture();
          });
          var origStop = t.stop.bind(t);
          t.stop = function() {
            activeVideoTracks = Math.max(0, activeVideoTracks - 1);
            updateCapture();
            return origStop();
          };
        });
      }
      if (at.length > 0) {
        activeAudioTracks += at.length;
        at.forEach(function(t) {
          t.addEventListener('ended', function() {
            activeAudioTracks = Math.max(0, activeAudioTracks - 1);
            updateCapture();
          });
          var origStop = t.stop.bind(t);
          t.stop = function() {
            activeAudioTracks = Math.max(0, activeAudioTracks - 1);
            updateCapture();
            return origStop();
          };
        });
      }
      updateCapture();
      return stream;
    });
  };
})();
)JS");

    QWebEngineScript script;
    script.setName(QStringLiteral("dalinira-media-capture-hook"));
    script.setInjectionPoint(QWebEngineScript::DocumentCreation);
    script.setWorldId(QWebEngineScript::MainWorld);
    script.setRunsOnSubFrames(true);
    script.setSourceCode(s_hookScript);
    scripts().insert(script);
  }

  QWebEnginePage *createWindow(WebWindowType type) override {
    Q_UNUSED(type);
    // Returning null makes Qt emit newWindowRequested(), whose URL is needed
    // for the native $popup decision before a destination is created.
    return nullptr;
  }

  QPointer<BrowserWindow> window_;
  DaliNiraBlockerPopupGuard *popupGuard_ = nullptr;
  QUrl deferredPopupOpener_;
  bool deferredPopupCheckPending_ = false;
  std::function<void(bool, bool)> onMediaCaptureChanged_;
  QWebChannel *credentialChannel_ = nullptr;
  CredentialMessageBridge *credentialBridge_ = nullptr;
};

class BrowserWebView final : public QWebEngineView {
 public:
  explicit BrowserWebView(BrowserWindow *window, QWidget *parent = nullptr)
      : QWebEngineView(parent), window_(window) {
    setStyleSheet(QStringLiteral("background-color: #07111f;"));
    QPalette pal = palette();
    pal.setColor(QPalette::Window, QColor(7, 17, 31));
    pal.setColor(QPalette::Base, QColor(7, 17, 31));
    setPalette(pal);
    setAutoFillBackground(true);
  }

 protected:
  void contextMenuEvent(QContextMenuEvent *event) override {
    const auto *req = lastContextMenuRequest();
    if (req && !req->linkUrl().isEmpty() && req->linkUrl().isValid()) {
      if (window_) window_->showLinkContextMenu(req->linkUrl(), req->linkText(), event->globalPos());
      event->accept();
      return;
    }
    if (req && !req->selectedText().trimmed().isEmpty()) {
      if (window_) window_->showSelectionContextMenu(req->selectedText().trimmed(), event->globalPos());
      event->accept();
      return;
    }
    if (window_) window_->showPageContextMenu(event->globalPos());
    event->accept();
  }

 private:
  QPointer<BrowserWindow> window_;
};

// A new window's place when nothing is restored: a regular desktop window like
// Chromium's first window, not an edge-to-edge one. About two thirds of the
// work area wide and four fifths high, within 1350x900 and above the minimum
// size, centered on the screen under the pointer (logical pixels, so HiDPI
// screens get the same proportions).
QRect defaultWindowGeometry() {
  QScreen *screen = QGuiApplication::screenAt(QCursor::pos());
  if (!screen) screen = QGuiApplication::primaryScreen();
  const QRect available = screen ? screen->availableGeometry() : QRect(0, 0, 1280, 800);
  const int width = qBound(qMin(640, available.width()), available.width() * 2 / 3,
                           qMin(1350, available.width()));
  const int height = qBound(qMin(420, available.height()), available.height() * 4 / 5,
                            qMin(900, available.height()));
  QRect geometry(0, 0, width, height);
  geometry.moveCenter(available.center());
  return geometry;
}
}  // namespace

void BrowserWindow::showLinkContextMenu(const QUrl &linkUrl, const QString &linkText, const QPoint &globalPos) {
  Q_UNUSED(linkText);
  QMenu menu(this);
  menu.setStyleSheet(QStringLiteral(
      "QMenu { background-color: #1b232d; color: #e8eef5; border: 1px solid #3a4857; border-radius: 9px; padding: 6px 4px; font-size: 13px; }"
      "QMenu::item { min-height: 25px; padding: 4px 26px 4px 12px; border-radius: 6px; margin: 1px 3px; }"
      "QMenu::item:selected { background-color: #2b3947; color: #ffffff; }"
      "QMenu::item:disabled { color: #6f7b87; background-color: transparent; }"
      "QMenu::separator { height: 1px; background-color: #33404d; margin: 5px 8px; }"
      "QMenu::icon { padding-left: 6px; }"
  ));

  // 1. Bağlantıyı yeni sekmede aç
  QAction *openTabAct = menu.addAction(BrowserIcons::icon(BrowserIcon::NewTab), I18n::text(QStringLiteral("context.open_link_tab")));
  connect(openTabAct, &QAction::triggered, this, [this, linkUrl] {
    int nextSlot = tabStrip() ? tabStrip()->currentIndex() + 1 : -1;
    addNewTab(linkUrl, nextSlot);
  });

  // 2. Bağlantıyı yeni pencerede aç
  QAction *openWinAct = menu.addAction(BrowserIcons::icon(BrowserIcon::Window), I18n::text(QStringLiteral("context.open_link_window")));
  connect(openWinAct, &QAction::triggered, this, [this, linkUrl] {
    auto *newWin = BrowserWindow::createWindow(services());
    newWin->addNewTab(linkUrl);
    newWin->show();
  });

  // 3. Bağlantıyı bölünmüş görünümde aç
  QAction *splitAct = menu.addAction(BrowserIcons::icon(BrowserIcon::Cards), I18n::text(QStringLiteral("context.open_link_split")));
  splitAct->setEnabled(false);

  // 4. Bağlantıyı Gizli pencerede aç
  QAction *incognitoAct = menu.addAction(BrowserIcons::icon(BrowserIcon::Incognito), I18n::text(QStringLiteral("context.open_link_incognito")));
  connect(incognitoAct, &QAction::triggered, this, [this, linkUrl] {
    openIncognitoWindow(linkUrl);
  });

  // 5. Bağlantıyı farklı aç
  QAction *openAsAct = menu.addAction(I18n::text(QStringLiteral("context.open_link_as")));
  openAsAct->setEnabled(false);

  menu.addSeparator();

  // 6. Bağlantıyı farklı kaydet...
  QAction *saveAct = menu.addAction(BrowserIcons::icon(BrowserIcon::Save), I18n::text(QStringLiteral("context.save_link_as")));
  connect(saveAct, &QAction::triggered, this, [this, linkUrl] {
    if (currentView() && currentView()->page()) {
      currentView()->page()->download(linkUrl);
    }
  });

  // 7. Bağlantı adresini kopyala
  QAction *copyAct = menu.addAction(BrowserIcons::icon(BrowserIcon::Clipboard), I18n::text(QStringLiteral("context.copy_link_address")));
  connect(copyAct, &QAction::triggered, [linkUrl] {
    QGuiApplication::clipboard()->setText(linkUrl.toString());
  });

  // 8. Bağlantıyı okuma listesine ekle
  QAction *readingListAct = menu.addAction(BrowserIcons::icon(BrowserIcon::Bookmark), I18n::text(QStringLiteral("context.add_link_reading_list")));
  connect(readingListAct, &QAction::triggered, this, [this, linkUrl] {
    if (services_.profileService) {
      services_.profileService->toggleBookmark(linkUrl);
      updateBookmarkButtonState();
      renderBookmarks();
    }
  });

  menu.addSeparator();

  // 9. İncele
  QAction *inspectAct = menu.addAction(BrowserIcons::icon(BrowserIcon::Tools), I18n::text(QStringLiteral("context.inspect")));
  connect(inspectAct, &QAction::triggered, this, [this] {
    if (currentView() && currentView()->page()) {
      openDevToolsForPage(currentView()->page());
    }
  });

  menu.exec(globalPos);
}

void BrowserWindow::showSelectionContextMenu(const QString &selectedText, const QPoint &globalPos) {
  QMenu menu(this);
  menu.setStyleSheet(QStringLiteral(
      "QMenu { background-color: #1b232d; color: #e8eef5; border: 1px solid #3a4857; border-radius: 9px; padding: 6px 4px; font-size: 13px; }"
      "QMenu::item { min-height: 25px; padding: 4px 26px 4px 12px; border-radius: 6px; margin: 1px 3px; }"
      "QMenu::item:selected { background-color: #2b3947; color: #ffffff; }"
      "QMenu::item:disabled { color: #6f7b87; background-color: transparent; }"
      "QMenu::separator { height: 1px; background-color: #33404d; margin: 5px 8px; }"
      "QMenu::icon { padding-left: 6px; }"
  ));

  if (!selectedText.isEmpty()) {
    QAction *copyAct = menu.addAction(BrowserIcons::icon(BrowserIcon::Clipboard), I18n::text(QStringLiteral("context.copy")));
    copyAct->setShortcut(QKeySequence::Copy);
    connect(copyAct, &QAction::triggered, [selectedText] {
      QGuiApplication::clipboard()->setText(selectedText);
    });

    const QString engine = currentSearchEngine();
    const QString truncated = selectedText.length() > 24 ? selectedText.left(21) + QStringLiteral("...") : selectedText;
    QAction *searchAct = menu.addAction(BrowserIcons::icon(BrowserIcon::Search),
                                        I18n::text(QStringLiteral("context.search_for")).arg(engine, truncated));
    connect(searchAct, &QAction::triggered, this, [this, selectedText] {
      int nextSlot = tabStrip() ? tabStrip()->currentIndex() + 1 : -1;
      addNewTab(QUrl(QStringLiteral("dalinira://newtab/")), nextSlot);
      navigateFromUserInput(selectedText);
    });

    menu.addSeparator();
  }

  QAction *inspectAct = menu.addAction(BrowserIcons::icon(BrowserIcon::Tools), I18n::text(QStringLiteral("context.inspect")));
  connect(inspectAct, &QAction::triggered, this, [this] {
    if (currentView() && currentView()->page()) {
      openDevToolsForPage(currentView()->page());
    }
  });

  menu.exec(globalPos);
}

void BrowserWindow::showPageContextMenu(const QPoint &globalPos) {
  QMenu menu(this);
  menu.setStyleSheet(QStringLiteral(
      "QMenu { background-color: #1b232d; color: #e8eef5; border: 1px solid #3a4857; border-radius: 9px; padding: 6px 4px; font-size: 13px; }"
      "QMenu::item { min-height: 25px; padding: 4px 26px 4px 12px; border-radius: 6px; margin: 1px 3px; }"
      "QMenu::item:selected { background-color: #2b3947; color: #ffffff; }"
      "QMenu::item:disabled { color: #6f7b87; background-color: transparent; }"
      "QMenu::separator { height: 1px; background-color: #33404d; margin: 5px 8px; }"
      "QMenu::icon { padding-left: 6px; }"
  ));

  bool canBack = false;
  bool canFwd = false;
  if (auto *ev = currentEngineView()) {
    canBack = ev->canGoBack();
    canFwd = ev->canGoForward();
  } else if (auto *v = currentView()) {
    canBack = v->history() ? v->history()->canGoBack() : false;
    canFwd = v->history() ? v->history()->canGoForward() : false;
  }

  QAction *backAct = menu.addAction(BrowserIcons::icon(BrowserIcon::ArrowLeft), I18n::text(QStringLiteral("context.back")));
  backAct->setEnabled(canBack);
  connect(backAct, &QAction::triggered, this, &BrowserWindow::onBackClicked);

  QAction *forwardAct = menu.addAction(I18n::text(QStringLiteral("context.forward")));
  forwardAct->setEnabled(canFwd);
  connect(forwardAct, &QAction::triggered, this, &BrowserWindow::onForwardClicked);

  QAction *reloadAct = menu.addAction(I18n::text(QStringLiteral("context.reload")));
  addShortcutHint(reloadAct, QStringLiteral("navigation.reload"));
  connect(reloadAct, &QAction::triggered, this, &BrowserWindow::onReloadOrStopClicked);

  menu.addSeparator();

  QAction *savePageAct = menu.addAction(BrowserIcons::icon(BrowserIcon::Save), I18n::text(QStringLiteral("context.save_page_as")));
  addShortcutHint(savePageAct, QStringLiteral("page.save"));
  connect(savePageAct, &QAction::triggered, this, &BrowserWindow::saveCurrentPage);

  QAction *printAct = menu.addAction(BrowserIcons::icon(BrowserIcon::Print), I18n::text(QStringLiteral("context.print")));
  addShortcutHint(printAct, QStringLiteral("page.print"));
  printAct->setEnabled(true);
  connect(printAct, &QAction::triggered, this, &BrowserWindow::printCurrentPage);

  QAction *captureAct = menu.addAction(I18n::text(QStringLiteral("context.capture_page")));
  connect(captureAct, &QAction::triggered, this, &BrowserWindow::captureVisiblePage);

  QAction *readerAct = menu.addAction(I18n::text(QStringLiteral("context.reader_mode")));
  const QUrl pageUrl = currentTabUrl();
  readerAct->setEnabled(pageUrl.scheme() == QLatin1String("http") || pageUrl.scheme() == QLatin1String("https"));
  connect(readerAct, &QAction::triggered, this, &BrowserWindow::showReaderMode);

  QAction *translateAct = menu.addAction(BrowserIcons::icon(BrowserIcon::Language),
      I18n::text(QStringLiteral("context.translate_page")));
  translateAct->setObjectName(QStringLiteral("contextTranslatePageAction"));
  translateAct->setEnabled(pageUrl.scheme() == QLatin1String("http") || pageUrl.scheme() == QLatin1String("https"));
  connect(translateAct, &QAction::triggered, this, &BrowserWindow::showTranslatePopup);

  menu.addSeparator();

  QAction *viewSourceAct = menu.addAction(I18n::text(QStringLiteral("context.view_source")));
  addShortcutHint(viewSourceAct, QStringLiteral("page.view_source"));
  connect(viewSourceAct, &QAction::triggered, this, &BrowserWindow::viewCurrentPageSource);

  QAction *inspectAct = menu.addAction(BrowserIcons::icon(BrowserIcon::Tools), I18n::text(QStringLiteral("context.inspect")));
  connect(inspectAct, &QAction::triggered, this, [this] {
    if (currentView() && currentView()->page()) {
      openDevToolsForPage(currentView()->page());
    }
  });

  menu.exec(globalPos);
}

BrowserWindow::BrowserWindow(const BrowserServices &services, bool isCaptureShell, QWidget *parent)
    : QMainWindow(parent), services_(services), isCaptureShell_(isCaptureShell) {
  setObjectName(QStringLiteral("BrowserWindow"));
  setWindowFlag(Qt::FramelessWindowHint, true);
  setAttribute(Qt::WA_Hover, true);
  setMouseTracking(true);
  setMinimumSize(640, 420);
  setGeometry(defaultWindowGeometry());
  lastNormalSize_ = size();
  lastNormalGeometry_ = geometry();
  setProperty("daliniraRestoredSize", lastNormalSize_);

  currentWorkspaceEngine_ = dalinira::engine::EngineBackend::selectedEngineType();

  if (currentWorkspaceEngine_ == dalinira::engine::EngineType::Chromium) {
    if (!services_.profile) {
      services_.profile = QWebEngineProfile::defaultProfile();
    }
  }

  auto composite = std::make_unique<dalinira::core::CompositeNavigationCandidateProvider>(this);
  composite->addProvider(std::make_shared<dalinira::core::BookmarkCandidateProvider>(services_.profileService, services_.profileService, composite.get()));
  composite->addProvider(std::make_shared<dalinira::core::FrequentSitesCandidateProvider>(services_.profileService, services_.profileService, composite.get()));
  composite->addProvider(std::make_shared<dalinira::core::HistoryCandidateProvider>(services_.profileService, services_.profileService, composite.get()));
  composite->addProvider(std::make_shared<dalinira::core::BootstrapWellKnownSiteProvider>());
  candidateProvider_ = std::move(composite);
  autofillController_ = std::make_unique<CredentialAutofillController>(
      services_.profileService ? services_.profileService->credentialVault() : nullptr, this, this);
  webKitCredentialController_ = std::make_unique<WebKitCredentialController>(
      services_.profileService ? services_.profileService->credentialVault() : nullptr, this, this);
  connect(autofillController_.get(), &CredentialAutofillController::openPasswordManagerRequested,
          this, &BrowserWindow::showPasswords);

  setupUi();
  downloadUiModel_ = new DownloadUiModel(services_.profileService, services_.mediaDownload, this);
  downloadPopup_ = new DownloadPopup(downloadUiModel_, this);
  connect(downloadUiModel_, &DownloadUiModel::changed, this, &BrowserWindow::updateDownloadToolbar);
  connect(downloadUiModel_, &DownloadUiModel::downloadStarted, this, [this](const QString &) {
    if (QApplication::activeWindow() == this || isActiveWindow()) showDownloadStartedAnimation();
  });
  connect(downloadUiModel_, &DownloadUiModel::downloadCompleted, this, [this](const QString &) {
    if (mediaDownload_) mediaDownload_->acknowledge(downloadAnimationsEnabled());
  });
  connect(downloadUiModel_, &DownloadUiModel::downloadFailed, this, [this](const QString &) {
    if (mediaDownload_) mediaDownload_->acknowledge(downloadAnimationsEnabled());
  });
  connect(downloadPopup_, &DownloadPopup::openDownloadsRequested, this,
          [this] { showMediaDownloads(); });
  connect(downloadPopup_, &DownloadPopup::openMediaDownloadRequested, this,
          [this](const QUrl &url) { showMediaDownloads(url, true); });
  connect(downloadPopup_, &DownloadPopup::openLocalMediaRequested, this,
          &BrowserWindow::openLocalMedia);
  updateDownloadToolbar();
  connect(autofillController_.get(), &CredentialAutofillController::saveBubbleShown,
          this, [this] {
            QTimer::singleShot(0, this, [this] { updateSaveBubblePosition(); });
          });
  setupStyle();
  setupTabStripSignals();
  if (services_.tabManager && services_.tabManager->performanceManager()) {
    if (services_.profileService) {
      connect(services_.profileService, &BrowserProfileService::pageSaveActiveChanged, this,
              [this](QWebEnginePage *page, bool active) {
                const TabManager::TabId id = services_.tabManager->idForPage(page);
                if (!id.isNull()) services_.tabManager->performanceManager()->setBrowserDownloadActive(id, active);
              });
    }
    // Sleeping tabs look asleep in the strip, for either engine.
    connect(services_.tabManager->performanceManager(), &dalinira::TabPerformanceManager::tabLifecycleStateChanged,
            this, [this](TabManager::TabId id, dalinira::TabLifecycleState) { syncTabSleepingIndicator(id); });
  }
  if (services_.profileService) {
    connect(services_.profileService, &BrowserProfileService::searchSuggestionsChanged, this, [this](bool) {
      syncNewTabViews();
      if (omnibox_->hasFocus()) updateOmniboxSuggestions(omnibox_->text());
    });
    connect(services_.profileService, &BrowserProfileService::searchEngineChanged, this, [this](const QString &) {
      services_.profileService->searchSuggestions()->cancel();
      syncNewTabViews();
      if (omnibox_->hasFocus()) updateOmniboxSuggestions(omnibox_->text());
    });
    connect(services_.profileService, &BrowserProfileService::downloadsChanged,
            this, &BrowserWindow::syncNewTabViews);
    if (auto *blocker = services_.profileService->blockerService()) {
      connect(blocker, &DaliNiraBlockerService::globalStatsChanged, this,
              [this](quint64 sessionBlocked, quint64 totalBlocked) {
        const QString script = newTabProtectionStatsUpdateScript(
            totalBlocked, services_.profileService->recentDownloadCount(), sessionBlocked);
        for (const BrowserTabInfo &tab : std::as_const(tabs_)) {
          if (tab.view && isNewTabUrl(tab.view->url())) tab.view->page()->runJavaScript(script);
        }
      });
    }
  }

  connect(&TabThrobber::instance(), &TabThrobber::throbberTick, this, &BrowserWindow::onThrobberTick);

  // Register in global registry for tab drag & attach
  dalinira::desktop_tabs::TabWindowRegistry::instance().registerWindow(this, tabStrip_);

  if (services_.privateProfileOwner) {
    // Keep the shared private profile alive until this window's page children
    // have been destroyed, including when its tabs move to another window.
    auto *lifetime = new QObject(this);
    connect(lifetime, &QObject::destroyed, [owner = services_.privateProfileOwner] {});
  }
  if (isCaptureShell_) {
    setProperty("daliniraDragCaptureShell", true);
  }

  if (qApp) {
    qApp->installEventFilter(this);
  }
}

BrowserWindow::~BrowserWindow() {
  if (hasOverrideCursor_) {
    QGuiApplication::restoreOverrideCursor();
    hasOverrideCursor_ = false;
  }
  if (autofillController_) {
    autofillController_->clearAllSensitiveData();
  }
  if (qApp) {
    qApp->removeEventFilter(this);
  }
  if (hoverCard_) {
    hoverCard_->hideCard();
    delete hoverCard_;
    hoverCard_ = nullptr;
  }
  for (const auto &tab : std::as_const(tabs_)) {
    if (tab.view) {
      TabThrobber::instance().removeView(tab.view.data());
    }
  }
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
  if (currentActivePermissionRequest_.has_value()) {
    if (currentActivePermissionRequest_->permission.isValid()) {
      currentActivePermissionRequest_->permission.deny();
    }
    currentActivePermissionRequest_.reset();
  }
  for (auto &req : pendingPermissionQueue_) {
    if (req.permission.isValid()) req.permission.deny();
  }
  pendingPermissionQueue_.clear();
#endif
  tabSessionGrants_.clear();
  dalinira::desktop_tabs::TabWindowRegistry::instance().unregisterWindow(this);
}

void BrowserWindow::setupUi() {
  using Metrics = dalinira::ui::BrowserChromeMetrics;

  auto *central = new QWidget(this);
  central->setObjectName(QStringLiteral("centralRoot"));
  central->setMouseTracking(true);
  auto *rootLayout = new QVBoxLayout(central);
  rootLayout->setContentsMargins(0, 0, 0, 0);
  rootLayout->setSpacing(0);

  // 1. Chrome Unified Titlebar / Top Bar
  topBar_ = new QWidget(central);
  topBar_->setObjectName(QStringLiteral("topBar"));
  topBar_->setFixedHeight(Metrics::topBarHeight);
  topBar_->setMouseTracking(true);
  auto *topLayout = new QHBoxLayout(topBar_);
  topLayout->setContentsMargins(8, 0, 0, 0);
  topLayout->setSpacing(4);

  // Tab Search Button (Chrome top-left)
  tabSearchBtn_ = new QToolButton(topBar_);
  tabSearchBtn_->setObjectName(QStringLiteral("tabSearchBtn"));
  tabSearchBtn_->setText(QString::fromUtf8("⌵"));
  tabSearchBtn_->setToolTip(shortcutTooltip(QStringLiteral("toolbar.search_tabs"), QStringLiteral("tabs.search")));
  tabSearchBtn_->setFixedSize(Metrics::tabSearchButtonSize,
                              Metrics::tabSearchButtonSize);
  connect(tabSearchBtn_, &QToolButton::clicked, this, &BrowserWindow::toggleTabSearchPopup);
  topLayout->addWidget(tabSearchBtn_, 0, Qt::AlignVCenter);

  // Every keyboard shortcut comes from KeyboardShortcutService: one QShortcut
  // per action, rebound in place when the user changes it in Settings.
  setupBrowserShortcuts();

  // Tab Strip (Chromium TabStripWidget)
  tabStrip_ = new dalinira::desktop_tabs::TabStripWidget(topBar_);
  tabStrip_->setObjectName(QStringLiteral("tabStrip"));
  tabStrip_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  groupModel_ = new dalinira::desktop_tabs::TabGroupModel(this);
  tabStrip_->setGroupModel(groupModel_);
  connect(tabStrip_, &dalinira::desktop_tabs::TabStripWidget::groupChipClicked,
          this, [this](const QUuid &groupId, const QPoint &globalPos) {
    showTabGroupPopup(groupId, globalPos);
  });
  topLayout->addWidget(tabStrip_, 1);

  // Window Caption Controls (Chrome top-right: Minimize, Maximize, Close)
  auto *captionBox = new QWidget(topBar_);
  captionBox->setObjectName(QStringLiteral("captionBox"));
  captionBox->setFixedHeight(Metrics::topBarHeight);
  auto *captionLayout = new QHBoxLayout(captionBox);
  captionLayout->setContentsMargins(0, 0, 0, 0);
  captionLayout->setSpacing(0);

  minBtn_ = new QToolButton(captionBox);
  minBtn_->setObjectName(QStringLiteral("windowControlMin"));
  minBtn_->setText(QString::fromUtf8("−"));
  minBtn_->setToolTip(I18n::text(QStringLiteral("toolbar.minimize")));
  minBtn_->setFixedSize(46, Metrics::topBarHeight);

  maxBtn_ = new QToolButton(captionBox);
  maxBtn_->setObjectName(QStringLiteral("windowControlMax"));
  maxBtn_->setText(QString::fromUtf8("□"));
  maxBtn_->setToolTip(I18n::text(QStringLiteral("toolbar.maximize")));
  maxBtn_->setFixedSize(46, Metrics::topBarHeight);

  closeBtn_ = new QToolButton(captionBox);
  closeBtn_->setObjectName(QStringLiteral("windowControlClose"));
  closeBtn_->setText(QString::fromUtf8("✕"));
  closeBtn_->setToolTip(I18n::text(QStringLiteral("common.close")));
  closeBtn_->setFixedSize(46, Metrics::topBarHeight);

  captionLayout->addWidget(minBtn_);
  captionLayout->addWidget(maxBtn_);
  captionLayout->addWidget(closeBtn_);
  topLayout->addWidget(captionBox, 0, Qt::AlignTop | Qt::AlignRight);

  rootLayout->addWidget(topBar_);

  // Connect Window Caption Buttons
  connect(minBtn_, &QToolButton::clicked, this, &BrowserWindow::onMinimizeClicked);
  connect(maxBtn_, &QToolButton::clicked, this, &BrowserWindow::onMaximizeRestoreClicked);
  connect(closeBtn_, &QToolButton::clicked, this, &BrowserWindow::onCloseWindowClicked);

  // 2. Navigation Bar
  navBar_ = new QWidget(central);
  navBar_->setObjectName(QStringLiteral("navBar"));
  navBar_->setFixedHeight(Metrics::navigationBarHeight);
  auto *navLayout = new QHBoxLayout(navBar_);
  navLayout->setContentsMargins(10, 6, 10, 6);
  navLayout->setSpacing(7);

  const auto configureNavigationButton = [](QToolButton *button) {
    button->setFixedSize(Metrics::navigationButtonSize,
                         Metrics::navigationButtonSize);
    button->setIconSize(QSize(Metrics::navigationIconSize,
                              Metrics::navigationIconSize));
  };

  backBtn_ = new QToolButton(navBar_);
  backBtn_->setObjectName(QStringLiteral("backBtn"));
  backBtn_->setIcon(BrowserIcons::backIcon());
  backBtn_->setToolTip(shortcutTooltip(QStringLiteral("toolbar.back"), QStringLiteral("navigation.back")));
  backBtn_->setEnabled(false);
  configureNavigationButton(backBtn_);
  navLayout->addWidget(backBtn_);

  forwardBtn_ = new QToolButton(navBar_);
  forwardBtn_->setObjectName(QStringLiteral("forwardBtn"));
  forwardBtn_->setIcon(BrowserIcons::forwardIcon());
  forwardBtn_->setToolTip(shortcutTooltip(QStringLiteral("toolbar.forward"), QStringLiteral("navigation.forward")));
  forwardBtn_->setEnabled(false);
  configureNavigationButton(forwardBtn_);
  navLayout->addWidget(forwardBtn_);

  reloadBtn_ = new QToolButton(navBar_);
  reloadBtn_->setObjectName(QStringLiteral("reloadBtn"));
  reloadBtn_->setIcon(BrowserIcons::reloadIcon());
  reloadBtn_->setToolTip(shortcutTooltip(QStringLiteral("toolbar.reload"), QStringLiteral("navigation.reload")));
  configureNavigationButton(reloadBtn_);
  navLayout->addWidget(reloadBtn_);

  bookmarkBtn_ = new QToolButton(navBar_);
  bookmarkBtn_->setObjectName(QStringLiteral("bookmark-button"));
  bookmarkBtn_->setIcon(bookmarkIcon(false));
  bookmarkBtn_->setToolTip(I18n::text(QStringLiteral("toolbar.bookmark_add")));
  configureNavigationButton(bookmarkBtn_);
  bookmarkBtn_->setIconSize(QSize(24, 24));

  // Google Search / Omnibox
  omnibox_ = new QLineEdit(navBar_);
  omnibox_->setObjectName(QStringLiteral("omnibox"));
  omnibox_->setPlaceholderText(searchEnginePlaceholder(currentSearchEngine()));
  omnibox_->setClearButtonEnabled(true);
  omnibox_->setFixedHeight(Metrics::omniboxHeight);
  omnibox_->setMaximumWidth(1100);
  omnibox_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  searchEngineAction_ = omnibox_->addAction(BrowserIcons::searchEngineIcon(currentSearchEngine()), QLineEdit::LeadingPosition);
  searchEngineAction_->setToolTip(I18n::text(QStringLiteral("toolbar.search_engine")).arg(currentSearchEngine()));
  connect(searchEngineAction_, &QAction::triggered, this, [this] {
    toggleSiteControlsBubble();
  });
  if (services_.profileService) {
    connect(services_.profileService, &BrowserProfileService::searchEngineChanged, this, [this](const QString &) {
      updateSearchEngineIcon();
    });
  }
  navLayout->addStretch(1);
  navLayout->addWidget(bookmarkBtn_);
  navLayout->addWidget(omnibox_, 10);
  navLayout->addStretch(1);

  // Feature buttons in Toolbar
  zoomButton_ = new QToolButton(navBar_);
  zoomButton_->setObjectName(QStringLiteral("zoomButton"));
  zoomButton_->setIcon(BrowserIcons::icon(BrowserIcon::Zoom));
  zoomButton_->setToolTip(I18n::text(QStringLiteral("toolbar.zoom")));
  configureNavigationButton(zoomButton_);
  zoomButton_->hide();
  navLayout->addWidget(zoomButton_);

  translateButton_ = new QToolButton(navBar_);
  translateButton_->setObjectName(QStringLiteral("translateButton"));
  translateButton_->setIcon(BrowserIcons::icon(BrowserIcon::Language));
  translateButton_->setToolTip(I18n::text(QStringLiteral("toolbar.translate_page")));
  configureNavigationButton(translateButton_);
  translateButton_->hide();
  navLayout->addWidget(translateButton_);

  findButton_ = new QToolButton(navBar_);
  findButton_->setObjectName(QStringLiteral("findInPageButton"));
  findButton_->setIcon(BrowserIcons::icon(BrowserIcon::FindInPage));
  findButton_->setToolTip(I18n::text(QStringLiteral("toolbar.find_in_page")));
  configureNavigationButton(findButton_);
  connect(findButton_, &QToolButton::clicked, this, &BrowserWindow::showFindBar);
  // The find popup hangs under this button; follow it when the toolbar reflows.
  findButton_->installEventFilter(this);
  navLayout->addWidget(findButton_);

  // AdBlock Shield Button
  auto *adblock = services_.profileService ? services_.profileService->adBlockService() : nullptr;
  adBlockShield_ = new DaliNiraBlockerShieldButton(adblock, navBar_);
  configureNavigationButton(adBlockShield_);
  navLayout->addWidget(adBlockShield_);

  // Pulse (Song recognition) Button
  pulseButton_ = new PulseToolbarButton(services_.songRecognition, services_.songFinderSettings, navBar_);
  configureNavigationButton(pulseButton_);
  navLayout->addWidget(pulseButton_);

  // Unified Downloads entry point; media analysis remains opt-in/event-driven.
  mediaDownload_ = new DownloadToolbarButton(navBar_);
  mediaDownload_->setObjectName(QStringLiteral("mediaDownloadButton"));
  configureNavigationButton(mediaDownload_);
  navLayout->addWidget(mediaDownload_);

  // Password Manager Button
  passwordsBtn_ = new QToolButton(navBar_);
  passwordsBtn_->setObjectName(QStringLiteral("passwordsButton"));
  passwordsBtn_->setIcon(BrowserIcons::icon(BrowserIcon::Password));
  passwordsBtn_->setToolTip(I18n::text(QStringLiteral("toolbar.passwords")));
  configureNavigationButton(passwordsBtn_);
  navLayout->addWidget(passwordsBtn_);

  // Main Menu (Hamburger) Button
  mainMenuBtn_ = new QToolButton(navBar_);
  mainMenuBtn_->setObjectName(QStringLiteral("mainMenuButton"));
  mainMenuBtn_->setText(QString::fromUtf8("☰"));
  mainMenuBtn_->setToolTip(I18n::text(QStringLiteral("toolbar.main_menu")));
  configureNavigationButton(mainMenuBtn_);
  navLayout->addWidget(mainMenuBtn_);

  rootLayout->addWidget(navBar_);

  // Bookmark Bar (Under navigation bar)
  bookmarkBar_ = new QToolBar(central);
  bookmarkBar_->setObjectName(QStringLiteral("bookmark-bar"));
  bookmarkBar_->setMovable(false);
  bookmarkBar_->setIconSize(QSize(Metrics::bookmarkIconSize,
                                  Metrics::bookmarkIconSize));
  bookmarkBar_->setFixedHeight(Metrics::bookmarkBarHeight);
  bookmarkBar_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  if (bookmarkBar_->layout()) {
    bookmarkBar_->layout()->setContentsMargins(6, 0, 6, 0);
    bookmarkBar_->layout()->setSpacing(3);
  }
  bookmarkBar_->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(bookmarkBar_, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
    showBookmarkContextMenu(QUrl(), QString(), bookmarkBar_->mapToGlobal(pos));
  });
  // Shortcut items are dragged into a new order; the move is stored per engine
  // and re-renders the bar (and the New Tab shortcuts) through bookmarksChanged.
  bookmarkReorder_ = new BookmarkBarReorder(bookmarkBar_, [this](const QUrl &url, int toIndex) {
    if (services_.profileService) services_.profileService->moveQuickAccess(url, toIndex, currentEngineScope());
  }, this);
  rootLayout->addWidget(bookmarkBar_);

  renderBookmarks();
  bookmarkBar_->setVisible(false);
  updateBookmarkBarVisibility();

  // 3. Thin loading progress bar (Chrome blue accent)
  progressBar_ = new QProgressBar(central);
  progressBar_->setObjectName(QStringLiteral("loadProgressBar"));
  progressBar_->setFixedHeight(2);
  progressBar_->setTextVisible(false);
  progressBar_->setRange(0, 100);
  progressBar_->hide();
  rootLayout->addWidget(progressBar_);

  // 4. Main Body Container (SidePanel & Page Content Stack)
  bodyContainer_ = new QWidget(central);
  bodyContainer_->setObjectName(QStringLiteral("bodyContainer"));
  bodyLayout_ = new QHBoxLayout(bodyContainer_);
  bodyLayout_->setContentsMargins(0, 0, 0, 0);
  bodyLayout_->setSpacing(0);

  sidePanel_ = new dalinira::layout::BrowserSidePanel(bodyContainer_);
  sidePanel_->hide();

  pageStack_ = new QStackedWidget(bodyContainer_);
  pageStack_->setObjectName(QStringLiteral("pageStack"));
  pageStack_->setStyleSheet(QStringLiteral("#pageStack { background-color: #07111f; }"));
  bodyLayout_->addWidget(pageStack_, 1);
  sidePanel_->setBackdropPageArea(pageStack_);

  rootLayout->addWidget(bodyContainer_, 1);

  layoutController_ = new dalinira::layout::BrowserLayoutController(
      this, bodyContainer_, bodyLayout_, sidePanel_, pageStack_, topBar_, tabStrip_, navBar_, this);
  applyLayoutMode(dalinira::layout::currentLayoutMode());

  connect(sidePanel_, &dalinira::layout::BrowserSidePanel::tabSelected, this, &BrowserWindow::switchTab);
  connect(sidePanel_, &dalinira::layout::BrowserSidePanel::tabCloseRequested, this, &BrowserWindow::closeTab);
  // The panel's pin toggle flips the shared auto-hide preference.
  connect(sidePanel_, &dalinira::layout::BrowserSidePanel::pinToggleRequested, this, [this] {
    if (layoutController_) applySidePanelAutoHideToAllWindows(layoutController_->isPinned());
  });
  connect(layoutController_, &dalinira::layout::BrowserLayoutController::pinnedChanged, sidePanel_,
          &dalinira::layout::BrowserSidePanel::setPinned);
  connect(sidePanel_, &dalinira::layout::BrowserSidePanel::tabContextMenuRequested,
          this, &BrowserWindow::onTabContextMenuRequested);
  // A tab drag keeps an auto-hiding panel open until it ends.
  connect(sidePanel_, &dalinira::layout::BrowserSidePanel::tabDragActiveChanged, layoutController_,
          &dalinira::layout::BrowserLayoutController::setTabDragActive);
  connect(sidePanel_, &dalinira::layout::BrowserSidePanel::tabMoved, this, &BrowserWindow::moveTab);
  connect(sidePanel_, &dalinira::layout::BrowserSidePanel::tabDetachRequested,
          this, [this](int index, const QPoint &globalPosition) {
    if (index < 0 || index >= tabs_.size()) return;

    QScreen *screen = QGuiApplication::screenAt(globalPosition);
    if (!screen) screen = QGuiApplication::primaryScreen();
    const QRect available = screen ? screen->availableGeometry()
                                   : QRect(0, 0, 1200, 800);
    const int minimumWidth = qMin(720, available.width());
    const int maximumWidth = qMin(1200, available.width());
    const int minimumHeight = qMin(520, available.height());
    const int maximumHeight = qMin(800, available.height());
    const QSize detachedSize(
        qBound(minimumWidth, available.width() * 3 / 5, maximumWidth),
        qBound(minimumHeight, available.height() * 3 / 5, maximumHeight));
    QPoint detachedPosition = globalPosition - QPoint(detachedSize.width() / 3, 28);
    detachedPosition.setX(qBound(available.left(), detachedPosition.x(),
                                 available.right() - detachedSize.width() + 1));
    detachedPosition.setY(qBound(available.top(), detachedPosition.y(),
                                 available.bottom() - detachedSize.height() + 1));

    auto *destination = createWindow(services_);
    destination->resize(detachedSize);
    destination->move(detachedPosition);

    destination->show();
    if (!transferSidePanelTabTo(tabs_[index].id, destination, 0)) {
      destination->close();  // deletes it (createWindow)
    } else {
      destination->raise();
    }
  });
  connect(sidePanel_, &dalinira::layout::BrowserSidePanel::tabTransferRequested,
          this, [this](dalinira::layout::BrowserSidePanel *sourcePanel,
                      int sourceIndex, int targetIndex) {
    auto *sourceWindow = sourcePanel
        ? qobject_cast<BrowserWindow *>(sourcePanel->window()) : nullptr;
    if (!sourceWindow || sourceIndex < 0 || sourceIndex >= sourceWindow->tabCount()) return;
    sourceWindow->transferSidePanelTabTo(
        sourceWindow->tabInfo(sourceIndex).id, this, targetIndex);
  });
  connect(sidePanel_, &dalinira::layout::BrowserSidePanel::newTabRequested, this, [this] {
    addNewTab(QUrl(QStringLiteral("dalinira://newtab/")));
  });
  connect(sidePanel_, &dalinira::layout::BrowserSidePanel::bookmarksRequested, this, &BrowserWindow::toggleBookmarkBar);
  connect(sidePanel_, &dalinira::layout::BrowserSidePanel::historyRequested, this, [this] {
    showSettings(SettingsPage::Category::History);
  });
  connect(sidePanel_, &dalinira::layout::BrowserSidePanel::settingsRequested, this, [this] {
    showSettings();
  });

  setCentralWidget(central);

  // Connect navigation buttons
  connect(backBtn_, &QToolButton::clicked, this, &BrowserWindow::onBackClicked);
  connect(forwardBtn_, &QToolButton::clicked, this, &BrowserWindow::onForwardClicked);
  connect(reloadBtn_, &QToolButton::clicked, this, &BrowserWindow::onReloadOrStopClicked);
  if (homeBtn_) connect(homeBtn_, &QToolButton::clicked, this, &BrowserWindow::onHomeClicked);
  suggestionModel_ = new QStandardItemModel(this);
  suggestionCompleter_ = new QCompleter(suggestionModel_, this);
  suggestionCompleter_->setCompletionMode(QCompleter::UnfilteredPopupCompletion);
  suggestionCompleter_->setMaxVisibleItems(10);
  suggestionPopup_ = suggestionCompleter_->popup();
  omnibox_->setCompleter(suggestionCompleter_);
  suggestionPopup_->setObjectName(QStringLiteral("omnibox-suggestions"));
  suggestionPopup_->setStyleSheet(QStringLiteral(
      "#omnibox-suggestions { background:#16151d; color:#fbfbfe;"
      " border:2px solid #8ab4f8; border-top:none;"
      " border-bottom-left-radius:12px; border-bottom-right-radius:12px;"
      " padding:4px 0 6px; outline:none; }"
      "#omnibox-suggestions::item { min-height:28px; padding:3px 8px; }"
      "#omnibox-suggestions::item:selected { background:rgba(138, 180, 248, 0.16); color:#fbfbfe; }"));
  suggestionPopup_->setItemDelegate(new SuggestionRemoveDelegate(suggestionPopup_, [this](const QModelIndex &index) {
    if (!usesPersistentProfile() || !services_.profileService) return;
    const QString input = omnibox_->text();
    if (services_.profileService->removeSuggestion(index.data(Qt::UserRole + 2).toString(),
            index.data(Qt::DisplayRole).toString(), index.data(Qt::UserRole + 1).toUrl()))
      updateOmniboxSuggestions(input);
  }));
  connect(suggestionCompleter_, qOverload<const QModelIndex &>(&QCompleter::activated), this,
          [this](const QModelIndex &index) {
            activateSuggestion(index.data(Qt::UserRole + 1).toUrl(), index.data(Qt::DisplayRole).toString(),
                               index.data(Qt::UserRole + 2).toString());
          });
  connect(omnibox_, &QLineEdit::textEdited, this, &BrowserWindow::updateOmniboxSuggestions);
  connect(omnibox_, &QLineEdit::returnPressed, this, &BrowserWindow::onOmniboxReturnPressed);

  // Connect feature buttons
  connect(adBlockShield_, &DaliNiraBlockerShieldButton::openSettingsRequested, this, [this] {
    showDaliNiraBlockerSettings(DaliNiraBlockerPage::Tab::Settings);
  });
  connect(adBlockShield_, &DaliNiraBlockerShieldButton::openRulesetsRequested, this, [this] {
    showDaliNiraBlockerSettings(DaliNiraBlockerPage::Tab::Rulesets);
  });
  connect(adBlockShield_, &DaliNiraBlockerShieldButton::pickElementRequested, this, &BrowserWindow::startElementPicker);
  connect(adBlockShield_, &DaliNiraBlockerShieldButton::openLoggerRequested, this, [this] {
    showDaliNiraBlockerSettings(DaliNiraBlockerPage::Tab::Logger);
  });
  connect(adBlockShield_, &DaliNiraBlockerShieldButton::reloadRequested, this, [this] {
    if (auto *view = currentView()) {
      prepareAdBlockScripts(view->page(), view->url(), true);
      view->reload();
    } else if (auto *ev = currentEngineView()) {
      ev->reload();
    }
  });
  if (adblock) {
    connect(adblock->settings(), &DaliNiraBlockerSettings::settingsChanged, this, [this] {
      for (const auto &tab : std::as_const(tabs_)) {
        if (tab.view && (tab.view->url().scheme() == QLatin1String("http") || tab.view->url().scheme() == QLatin1String("https")))
          prepareAdBlockScripts(tab.view->page(), tab.view->url(), true);
#if defined(DALINIRA_BUILD_WEBKIT)
        if (auto *webkitView = qobject_cast<dalinira::engine::WebKitEngineView *>(tab.engineView.data()))
          webkitView->refreshDocumentScripts();
#endif
      }
    });
    connect(adblock, &DaliNiraBlockerService::tabStatsChanged, this, [this](quint64 tabId, const TabBlockerStats &stats) {
      if (auto *engineView = currentEngineView()) {
        if (tabId == reinterpret_cast<quintptr>(engineView)) updateBlockerControls();
      } else if (auto *view = currentView()) {
        const quint64 currentId = reinterpret_cast<quintptr>(view);
        if (tabId == currentId && adBlockShield_) {
          if (isInternalOrNonWebUrl(view->url())) {
            adBlockShield_->setInternalPage(true);
            adBlockShield_->setActiveHost(QString());
            adBlockShield_->setBlockedCount(0);
          } else {
            adBlockShield_->setInternalPage(false);
            adBlockShield_->setActiveHost(view->url().host().toLower());
            const quint64 total = stats.totalBlocked();
            adBlockShield_->setBlockedCount(total);
            adBlockShield_->setToolTip(total > 0
                ? I18n::text(QStringLiteral("toolbar.adblock_status_blocked")).arg(view->url().host()).arg(total)
                : I18n::text(QStringLiteral("toolbar.adblock_status_active")).arg(view->url().host()));
          }
        }
      }
    });
    connect(adblock, &DaliNiraBlockerService::autoReloadRequested, this, [this] {
      if (auto *view = currentView()) {
        prepareAdBlockScripts(view->page(), view->url(), true);
        view->reload();
      } else if (auto *engineView = currentEngineView()) {
        engineView->refreshDocumentScripts();
        engineView->reload();
      }
    });
  }

  connect(pulseButton_, &PulseToolbarButton::openFullPageRequested, this, &BrowserWindow::showSongFinder);
  connect(pulseButton_, &PulseToolbarButton::openSettingsRequested, this, &BrowserWindow::showSongFinderSettings);
  connect(pulseButton_, &PulseToolbarButton::openUrlRequested, this, [this](const QUrl &url) {
    addNewTab(url);
  });
  if (services_.songRecognition) {
    connect(services_.songRecognition, &SongRecognitionService::stateChanged, this,
            [this](SongRecognitionService::State state, const QString &) {
      if (state != SongRecognitionService::State::Listening) return;
      QWidget *active = qApp ? qApp->activeWindow() : nullptr;
      bool belongsToThisWindow = isActiveWindow();
      for (QWidget *widget = active; widget && !belongsToThisWindow; widget = widget->parentWidget()) {
        belongsToThisWindow = widget == this;
      }
      if (!belongsToThisWindow) return;
      const int index = tabStrip_ ? tabStrip_->currentIndex() : -1;
      if (index < 0 || index >= tabs_.size() || !tabs_[index].view) {
        services_.songRecognition->clearWebContextMetadata();
        return;
      }
      QString cleanTitle = tabs_[index].title.trimmed();
      if (cleanTitle.endsWith(QStringLiteral(" - YouTube"), Qt::CaseInsensitive)) cleanTitle.chop(10);
      const QStringList parts = cleanTitle.split(QStringLiteral(" - "));
      if (parts.size() >= 2) {
        services_.songRecognition->setWebContextMetadata(
            parts.mid(1).join(QStringLiteral(" - ")).trimmed(), parts.first().trimmed());
      } else {
        services_.songRecognition->setWebContextMetadata(cleanTitle, QString());
      }
    });
  }

  connect(mediaDownload_, &QToolButton::clicked, this, [this] {
    if (!downloadPopup_) return;
    if (downloadPopup_->isVisible()) {
      downloadPopup_->hide();
    } else {
      const QUrl activeUrl = currentActiveWebUrl();
      const bool pageHasMedia = currentTabAudible();
      if (!activeUrl.isEmpty() && MediaPlatformRegistry::shouldAutoAnalyzeMedia(activeUrl, pageHasMedia)) {
        const QString pageTitle = currentTabTitle();
        // Resolve while the source page is still the active tab. The popup's
        // second click must only forward this captured permalink; it must not
        // depend on whichever tab happens to be active later.
        resolveActiveMediaPageUrl(activeUrl, [this, activeUrl, pageTitle](const QUrl &resolvedUrl) {
          if (!downloadPopup_ || !mediaDownload_) return;
          if (MediaPlatformRegistry::isGenericPlatformFeedUrl(resolvedUrl)) {
            downloadPopup_->setSuggestedMedia(QUrl{}, QString{});
          } else {
            const QString popupDetail = resolvedUrl != activeUrl
                ? resolvedUrl.toDisplayString(QUrl::RemoveScheme | QUrl::RemoveQuery
                                              | QUrl::RemoveFragment).left(160)
                : pageTitle;
            downloadPopup_->setSuggestedMedia(resolvedUrl, popupDetail);
          }
          downloadPopup_->showAnchored(mediaDownload_, false);
        });
        return;
      } else {
        downloadPopup_->setSuggestedMedia(QUrl{}, QString{});
      }
      downloadPopup_->showAnchored(mediaDownload_, false);
    }
  });

  connect(passwordsBtn_, &QToolButton::clicked, this, &BrowserWindow::showPasswords);
  connect(bookmarkBtn_, &QToolButton::clicked, this, &BrowserWindow::toggleCurrentBookmark);
  connect(mainMenuBtn_, &QToolButton::clicked, this, &BrowserWindow::showMainMenu);
  if (services_.profileService) {
    connect(services_.profileService, &BrowserProfileService::bookmarksChanged, this, &BrowserWindow::renderBookmarks);
    connect(services_.profileService, &BrowserProfileService::bookmarksChanged, this, &BrowserWindow::syncNewTabViews);
    connect(services_.profileService, &BrowserProfileService::historyChanged, this, &BrowserWindow::syncNewTabViews);
  }

  connect(translateButton_, &QToolButton::clicked, this, &BrowserWindow::showTranslatePopup);
  connect(zoomButton_, &QToolButton::clicked, this, &BrowserWindow::showZoomPopup);
}

void BrowserWindow::setupStyle() {
  setStyleSheet(QStringLiteral(
      "QMainWindow { background-color: #1c1b22; }"
      "#centralRoot { background-color: #1c1b22; }"
      "#topBar { background-color: #1c1b22; }"
      "#tabStrip { background-color: #1c1b22; }"
      "#pageStack { background-color: #07111f; }"
      "#tabSearchBtn {"
      "  color: #c4c7c5;"
      "  background: transparent;"
      "  border: none;"
      "  border-radius: 14px;"
      "  font-size: 13px;"
      "  font-weight: bold;"
      "}"
      "#tabSearchBtn:hover { background-color: rgba(255, 255, 255, 0.12); }"
      "#captionBox { background: transparent; }"
      "#windowControlMin, #windowControlMax, #windowControlClose {"
      "  color: #c4c7c5;"
      "  background: transparent;"
      "  border: none;"
      "  font-size: 15px;"
      "  font-family: monospace, sans-serif;"
      "}"
      "#windowControlMin:hover, #windowControlMax:hover {"
      "  background-color: rgba(255, 255, 255, 0.12);"
      "  color: #ffffff;"
      "}"
      "#windowControlClose:hover {"
      "  background-color: #e81123;"
      "  color: #ffffff;"
      "}"
      "#navBar {"
      "  background-color: #2b2a33;"
      "  border: none;"
      "}"
      "#bookmark-bar {"
      "  background-color: #2b2a33;"
      "  border: none;"
      "  border-bottom: 1px solid #1c1b22;"
      "  spacing: 3px;"
      "  padding: 0px 6px;"
      "  min-height: 34px;"
      "  max-height: 34px;"
      "}"
      "#bookmark-bar QToolButton {"
      "  color: #d8dce0;"
      "  background: transparent;"
      "  border: none;"
      "  border-radius: 4px;"
      "  padding: 2px 7px;"
      "  font-size: 12px;"
      "  font-weight: 500;"
      "  min-width: 20px;"
      "  max-width: 220px;"
      "  min-height: 22px;"
      "  max-height: 22px;"
      "  qproperty-toolButtonStyle: ToolButtonTextBesideIcon;"
      "}"
      "#bookmark-bar QToolButton:hover {"
      "  background-color: rgba(255, 255, 255, 0.12);"
      "  color: #ffffff;"
      "}"
      "#bookmark-bar #appsButton {"
      "  min-width: 28px;"
      "  max-width: 28px;"
      "  min-height: 22px;"
      "  max-height: 22px;"
      "  padding: 2px;"
      "  border-radius: 4px;"
      "  qproperty-toolButtonStyle: ToolButtonIconOnly;"
      "}"
      "#bookmark-bar #bookmarkItemButton {"
      "  padding-right: 3px;"
      "  border-top-right-radius: 0px;"
      "  border-bottom-right-radius: 0px;"
      "}"
      "#bookmark-bar #bookmarkRemoveButton {"
      "  color: #aeb7c2;"
      "  min-width: 20px; max-width: 20px;"
      "  min-height: 22px; max-height: 22px;"
      "  padding: 2px;"
      "  border-top-left-radius: 0px;"
      "  border-bottom-left-radius: 0px;"
      "  qproperty-toolButtonStyle: ToolButtonIconOnly;"
      "}"
      "#bookmark-bar #bookmarkRemoveButton:hover {"
      "  color: #ffffff;"
      "  background-color: rgba(220, 65, 65, 0.42);"
      "}"
      "#mediaDownloadButton[activeMedia=\"true\"] {"
      "  color: #4fc3f7;"
      "  background-color: rgba(79, 195, 247, 0.22);"
      "  border-radius: 14px;"
      "}"
      "#navBar > QToolButton, #tabSearchBtn {"
      "  color: #fbfbfe;"
      "  background: transparent;"
      "  border: none;"
      "  border-radius: 16px;"
      "  min-width: 32px;"
      "  max-width: 32px;"
      "  min-height: 32px;"
      "  max-height: 32px;"
      "  font-size: 18px;"
      "  font-weight: bold;"
      "}"
      "#navBar > QToolButton:hover, #tabSearchBtn:hover {"
      "  background-color: rgba(255, 255, 255, 0.12);"
      "}"
      "#navBar > QToolButton:disabled, #tabSearchBtn:disabled { color: #5b5b66; }"
      "#omnibox {"
      "  background-color: #1c1b22;"
      "  color: #fbfbfe;"
      "  border: 1px solid #3c4043;"
      "  border-top-left-radius: 12px;"
      "  border-top-right-radius: 12px;"
      "  border-bottom-left-radius: 0px;"
      "  border-bottom-right-radius: 0px;"
      "  padding: 0px 10px;"
      "  font-size: 14px;"
      "  selection-background-color: #8ab4f8;"
      "  selection-color: #1c1b22;"
      "}"
      "#omnibox:focus {"
      "  border: 2px solid #8ab4f8;"
      "  border-top-left-radius: 12px;"
      "  border-top-right-radius: 12px;"
      "  border-bottom-left-radius: 0px;"
      "  border-bottom-right-radius: 0px;"
      "  background-color: #16151d;"
      "  padding: 0px 9px;"
      "}"
      "#omnibox[suggestionsOpen=\"true\"], #omnibox:focus[suggestionsOpen=\"true\"] {"
      "  border-bottom: none;"
      "  border-bottom-left-radius: 0px;"
      "  border-bottom-right-radius: 0px;"
      "  padding-bottom: 2px;"
      "}"
      "#omnibox QToolButton {"
      "  background: transparent;"
      "  border: none;"
      "  border-radius: 4px;"
      "  min-width: 16px;"
      "  max-width: 24px;"
      "  min-height: 16px;"
      "  max-height: 24px;"
      "  padding: 0px;"
      "  margin: 0px;"
      "}"
      "#omnibox QToolButton:hover {"
      "  background-color: rgba(255, 255, 255, 0.10);"
      "}"
      "#loadProgressBar {"
      "  border: none;"
      "  background: transparent;"
      "  height: 2px;"
      "}"
      "#loadProgressBar::chunk {"
      "  background-color: #8ab4f8;"
      "}"
  ));
  if (!dalinira::layout::isDarkAppearance(dalinira::layout::currentAppearanceTheme())) {
    setStyleSheet(styleSheet() + QStringLiteral(
        "QMainWindow, #centralRoot, #topBar, #tabStrip { background-color:#eef2f7; }"
        "#navBar, #bookmark-bar { background-color:#ffffff; color:#202a36; }"
        "#bookmark-bar { border-bottom:1px solid #d3dbe5; }"
        "#bookmark-bar QToolButton, #windowControlMin, #windowControlMax, #windowControlClose,"
        "#navBar > QToolButton, #tabSearchBtn { color:#263442; }"
        "#windowControlMin:hover, #windowControlMax:hover, #navBar > QToolButton:hover,"
        "#tabSearchBtn:hover, #bookmark-bar QToolButton:hover { background-color:rgba(25,70,110,.10); color:#101820; }"
        "#omnibox { background:#f3f6fa; color:#17212b; border-color:#c1ccd8; selection-color:white; }"
        "#omnibox:focus { background:#ffffff; border-color:#1873d1; }"
        "#pageStack { background:#ffffff; }"));
  }
  if (sidePanel_) sidePanel_->refreshAppearance();
}

void BrowserWindow::setupTabStripSignals() {
  connect(tabStrip_, &dalinira::desktop_tabs::TabStripWidget::newTabRequested, this, [this] {
    addNewTab(QUrl(QStringLiteral("dalinira://newtab/")));
  });

  connect(tabStrip_, &dalinira::desktop_tabs::TabStripWidget::tabCloseRequested, this, [this](int index) {
    closeTab(index);
  });

  connect(tabStrip_, &dalinira::desktop_tabs::TabStripWidget::currentChanged, this, [this](int index) {
    // A picker belongs to the tab it was opened on.
    if (elementPicker_) elementPicker_->cancel();
    switchTab(index);
  });

  connect(tabStrip_, &dalinira::desktop_tabs::TabStripWidget::tabMoved, this, [this](int from, int to) {
    moveTab(from, to);
  });

  connect(tabStrip_, &dalinira::desktop_tabs::TabStripWidget::tabHovered, this,
          &BrowserWindow::onTabHovered);

  connect(tabStrip_, &dalinira::desktop_tabs::TabStripWidget::tabHoverLeave, this,
          &BrowserWindow::onTabHoverLeave);

  connect(tabStrip_, &dalinira::desktop_tabs::TabStripWidget::dragInitiated, this,
          [this](int index, const QPoint &screenPosition, const QPoint &pressOffsetInTab, const QSize &) {
    if (hoverCard_) hoverCard_->hideCard();
    if (index < 0 || index >= tabs_.size()) return;
    const QPoint offsetInWindow = mapFromGlobal(screenPosition);
    dalinira::desktop_tabs::TabDragController::instance().handleMousePress(
        this, tabStrip_, index, screenPosition, pressOffsetInTab, offsetInWindow);
  });

  connect(tabStrip_, &dalinira::desktop_tabs::TabStripWidget::tabContextMenuRequested,
          this, &BrowserWindow::onTabContextMenuRequested);

  connect(&LanguageManager::instance(), &LanguageManager::languageChanged,
          this, &BrowserWindow::retranslateUi);
  retranslateUi();
}

namespace {
// Regenerates a New Tab in the current UI language. Chromium does not
// re-request a loaded custom-scheme document on reload(), so navigate to it
// again instead (a new request reaches the dalinira:// handler).
void reloadNewTabDocument(QWebEngineView *view, dalinira::engine::EngineView *engineView, const QUrl &url) {
  if (view) view->setUrl(url);
  else if (engineView) engineView->reload();
}
}  // namespace

void BrowserWindow::retranslateUi() {
  // The application language is the authoritative target for both rendering
  // engines. A separate engine/profile preference must not make the two
  // browser modes offer different target languages.
  if (services_.profileService && services_.profileService->translateService()) {
    services_.profileService->translateService()->setDefaultTargetLanguage(
        LanguageManager::instance().activeLanguageCode());
  }
  for (auto &tab : tabs_) {
    if (isNewTabUrl(tab.url)) {
      tab.title = isIncognito()
          ? I18n::text(QStringLiteral("tab.new_private"))
          : I18n::text(QStringLiteral("tab.new"));
      const int index = findIndexByTabId(tab.id);
      if (index >= 0 && tabStrip_) tabStrip_->setTabTitle(index, tab.title);
      // The document is generated per request in the active language. A
      // background tab may be frozen by tab sleep, which drops a reload, and a
      // sleeping tab must not be woken just to relocalize it: reload the
      // visible New Tab now and the others when they are activated.
      if (index >= 0 && tabStrip_ && index == tabStrip_->currentIndex()) {
        reloadNewTabDocument(tab.view, tab.engineView, tab.url);
      } else {
        tab.pendingLanguageReload = true;
      }
    }
    if (!tab.translator) continue;
    tab.translator->reset();
    const QString scheme = tab.url.scheme().toLower();
    if (scheme == QLatin1String("http") || scheme == QLatin1String("https"))
      tab.translator->detectLanguage();
  }
  // Back, forward, reload and tab search tooltips name their current shortcut.
  refreshShortcutHints();
  if (zoomButton_) {
    zoomButton_->setToolTip(I18n::text(QStringLiteral("toolbar.zoom")));
  }
  if (translateButton_) {
    translateButton_->setToolTip(I18n::text(QStringLiteral("toolbar.translate_page")));
  }
  if (findButton_) {
    findButton_->setToolTip(I18n::text(QStringLiteral("toolbar.find_in_page")));
  }
  if (passwordsBtn_) {
    passwordsBtn_->setToolTip(I18n::text(QStringLiteral("toolbar.passwords")));
  }
  if (mainMenuBtn_) {
    mainMenuBtn_->setToolTip(I18n::text(QStringLiteral("toolbar.main_menu")));
  }
  if (omnibox_) {
    // Keep Omnibox / address bar / URL input strictly LeftToRight even in RTL languages
    omnibox_->setLayoutDirection(Qt::LeftToRight);
    omnibox_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  }
  // Internal pages (Settings, Downloads, Pulse settings, ...) carry catalog
  // titles; web page titles are left alone.
  for (int index = 0; index < tabs_.size(); ++index) {
    BrowserTabInfo &tab = tabs_[index];
    if (!tab.isInternal) continue;
    const QString title = I18n::retranslated(tab.title);
    if (title == tab.title) continue;
    tab.title = title;
    if (tabStrip_) tabStrip_->setTabTitle(index, title);
  }
  // Bookmark-bar tooltips are composed ("%1 (folder)"); rebuild the bar.
  renderBookmarks();
  updateBookmarkButtonState();
  updateBlockerControls();
  updateOmniboxLeadingIcon();
  updateSidePanelTabs();
}

void BrowserWindow::startElementPicker() {
  if (elementPicker_) elementPicker_->cancel();
  auto *blocker = services_.profileService ? services_.profileService->adBlockService() : nullptr;
  if (!blocker) return;
  // The only engine-specific part: where the picker's script runs (the
  // blocker's isolated world of the page).
  QUrl url;
  QObject *view = nullptr;
  DaliNiraElementPickerSession::ScriptRunner runner;
  if (QWebEngineView *webView = currentView()) {
    url = webView->url();
    view = webView;
    const QPointer<QWebEnginePage> page(webView->page());
    runner = [page](const QString &script, std::function<void(const QVariant &)> callback) {
      if (!page) {
        if (callback) callback(QVariant());
        return;
      }
      if (callback) page->runJavaScript(script, QWebEngineScript::ApplicationWorld, callback);
      else page->runJavaScript(script, QWebEngineScript::ApplicationWorld);
    };
  } else if (dalinira::engine::EngineView *engineView = currentEngineView()) {
    url = engineView->url();
    view = engineView;
    const QPointer<dalinira::engine::EngineView> guarded(engineView);
    runner = [guarded](const QString &script, std::function<void(const QVariant &)> callback) {
      if (!guarded) {
        if (callback) callback(QVariant());
        return;
      }
      guarded->runIsolatedJavaScript(script, std::move(callback));
    };
  }
  if (!runner || !DaliNiraElementPickerSession::unavailableReason(blocker, url).isEmpty()) return;

  auto *session = new DaliNiraElementPickerSession(blocker, url, runner, this);
  elementPicker_ = session;
  connect(session, &DaliNiraElementPickerSession::finished, session, &QObject::deleteLater);
  // One document only: navigation (including same-document URL changes),
  // a reload or the tab closing ends it.
  auto urlChanged = [session](const QUrl &now) {
    if (now != session->pageUrl()) session->cancel();
  };
  if (auto *webView = qobject_cast<QWebEngineView *>(view)) {
    connect(webView, &QWebEngineView::urlChanged, session, urlChanged);
    connect(webView, &QWebEngineView::loadStarted, session, &DaliNiraElementPickerSession::cancel);
  } else if (auto *engineView = qobject_cast<dalinira::engine::EngineView *>(view)) {
    connect(engineView, &dalinira::engine::EngineView::urlChanged, session, urlChanged);
    connect(engineView, &dalinira::engine::EngineView::loadStarted, session, &DaliNiraElementPickerSession::cancel);
  }
  connect(view, &QObject::destroyed, session, &DaliNiraElementPickerSession::cancel);
  session->start();
}

// The browser's JavaScript setting and the site's "block scripts" policy, for
// a web page's own scripts. Chromium applies it as the page's JavascriptEnabled
// (prepareAdBlockScripts), WebKit as the view's page script policy.
bool BrowserWindow::pageScriptsAllowed(const QUrl &url) const {
  if (!services_.profileService) return true;
  if (!services_.profileService->isJavascriptEnabled()) return false;
  DaliNiraBlockerService *blocker = services_.profileService->adBlockService();
  if (!blocker || !blocker->settings()->protectionEnabled()) return true;
  const auto policy = blocker->sitePolicy(url.host().toLower());
  return policy.whitelisted || !policy.blockScripts;
}

QUrl BrowserWindow::httpsUpgradeResult(const QList<QUrl> &urls, bool ok) {
  auto *blocker = services_.profileService ? services_.profileService->adBlockService() : nullptr;
  if (!blocker) return {};
  for (const QUrl &url : urls) {
    if (ok) {
      blocker->noteHttpsLoadSucceeded(url);
      continue;
    }
    const QUrl fallback = blocker->httpsUpgradeFallback(url);
    if (fallback.isValid()) return fallback;
  }
  return {};
}

void BrowserWindow::prepareAdBlockScripts(QWebEnginePage *page, const QUrl &url, bool force) {
  if (!page) return;
  const QString planKey = QStringLiteral("%1://%2").arg(url.scheme().toLower(), url.host().toLower());
  const QString pageScheme = url.scheme().toLower();
  // Applied on every navigation, not only when the host changes: the
  // JavaScript setting or the site policy may have changed since the last one.
  if (pageScheme == QLatin1String("http") || pageScheme == QLatin1String("https"))
    page->settings()->setAttribute(QWebEngineSettings::JavascriptEnabled, pageScriptsAllowed(url));
  DaliNiraBlockerService *blocker = services_.profileService
      ? services_.profileService->adBlockService() : nullptr;
  const auto *owningView = qobject_cast<QWebEngineView *>(page->parent());
  const QString temporaryStorage = blocker
      ? blocker->temporaryStorageCleanupScript(reinterpret_cast<quintptr>(owningView), url) : QString();
  // acceptNavigationRequest prepares the ordinary plan before urlChanged
  // confirms the new top-level site. Do not let that cached plan suppress a
  // cleanup listener that became necessary at commit time.
  if (!force && page->property("dalinira-adblock-script-plan").toString() == planKey &&
      temporaryStorage.isEmpty()) return;
  page->setProperty("dalinira-adblock-script-plan", planKey);
  const bool refreshDocument = force || page->url() == url;

  static const QStringList names = {
      QStringLiteral("dalinira-adblock-cosmetic"),
      QStringLiteral("dalinira-adblock-scriptlets-main"),
      QStringLiteral("dalinira-adblock-scriptlets-isolated"),
      QStringLiteral("dalinira-adblock-procedural"),
      QStringLiteral("dalinira-adblock-youtube-guardian"),
      QStringLiteral("dalinira-fingerprint-protection"),
      QStringLiteral("dalinira-forget-site-storage")};
  for (const QString &name : names) {
    const auto installed = page->scripts().find(name);
    for (const QWebEngineScript &script : installed) page->scripts().remove(script);
  }
  if (!blocker) return;
  const QString scheme = url.scheme().toLower();
  if (scheme != QLatin1String("http") && scheme != QLatin1String("https")) return;
  if (!temporaryStorage.isEmpty()) {
    QWebEngineScript cleanup;
    cleanup.setName(QStringLiteral("dalinira-forget-site-storage"));
    cleanup.setInjectionPoint(QWebEngineScript::DocumentCreation);
    cleanup.setWorldId(QWebEngineScript::ApplicationWorld);
    cleanup.setRunsOnSubFrames(false);
    cleanup.setSourceCode(temporaryStorage);
    page->scripts().insert(cleanup);
    // acceptNavigationRequest can install the document-start copy; urlChanged
    // can arrive after DocumentCreation, so arm the same guarded listener in
    // the newly committed document as well.
    if (page->url() == url)
      page->runJavaScript(temporaryStorage, QWebEngineScript::ApplicationWorld);
  }
  if (refreshDocument) page->runJavaScript(QStringLiteral("if(window.__daliniraCosmeticRuntime){window.__daliniraCosmeticRuntime.pause();}window.__daliniraProceduralRules=0;"), QWebEngineScript::ApplicationWorld);
  const QString host = url.host().toLower();
  QList<QWebEngineScript> siteScripts;
  for (QWebEngineScript script : blocker->createScriptingScriptsForHost(host, url)) {
    if (refreshDocument && (script.name() == QLatin1String("dalinira-adblock-cosmetic") || script.name() == QLatin1String("dalinira-adblock-procedural")))
      page->runJavaScript(script.sourceCode(), script.worldId());
    // Once per document and world: the page's copy and a staged profile
    // copy (below) may both reach the same document.
    const QString marker = QStringLiteral("__dnSite%1").arg(qHash(script.name() + QLatin1Char('@') + host), 0, 16);
    script.setSourceCode(QStringLiteral("if(!window.%1){Object.defineProperty(window,'%1',{value:1});\n").arg(marker) +
                         script.sourceCode() + QStringLiteral("\n}"));
    page->scripts().insert(script);
    siteScripts.append(script);
  }
  // Page scripts reach the new renderer process of a cross-site navigation
  // the page itself started only after its first document began, so they do
  // not run there (a browser-started navigation creates that process early
  // enough). Profile scripts added now race that document as well (measured:
  // the large ones lose). So the site's scripts also run in the new document
  // when it commits (urlChanged), scoped to its host; each runs once per
  // document whichever copy comes first.
  const QUrl current = page->url();
  const bool newSite = (current.scheme() != QLatin1String("http") && current.scheme() != QLatin1String("https")) ||
      DaliNiraBlockerEngine::siteForHost(current.host()) != DaliNiraBlockerEngine::siteForHost(host);
  if (refreshDocument || !newSite || siteScripts.isEmpty()) return;
  const QString hostJson = QString::fromUtf8(QJsonDocument(QJsonArray{host}).toJson(QJsonDocument::Compact)).mid(1).chopped(1);
  QVariantList pending;
  for (const QWebEngineScript &script : std::as_const(siteScripts))
    pending.append(QVariantMap{{QStringLiteral("world"), script.worldId()},
                               {QStringLiteral("source"), QStringLiteral("if(location.hostname.toLowerCase()===%1){\n").arg(hostJson) +
                                                              script.sourceCode() + QStringLiteral("\n}")}});
  page->setProperty("dalinira-commit-scripts-host", host);
  page->setProperty("dalinira-commit-scripts", pending);
  if (page->property("dalinira-commit-scripts-hooked").toBool()) return;
  page->setProperty("dalinira-commit-scripts-hooked", true);
  connect(page, &QWebEnginePage::urlChanged, page, [page](const QUrl &committed) {
    if (committed.host().toLower() != page->property("dalinira-commit-scripts-host").toString()) return;
    const QVariantList scripts = page->property("dalinira-commit-scripts").toList();
    page->setProperty("dalinira-commit-scripts", QVariantList{});
    for (const QVariant &entry : scripts) {
      const QVariantMap script = entry.toMap();
      page->runJavaScript(script.value(QStringLiteral("source")).toString(), script.value(QStringLiteral("world")).toUInt());
    }
  });
}

int BrowserWindow::addNewTab(const QUrl &url, int insertIndex, bool initiallyPinned,
                             const QString &initialTitle) {
  QUrl targetUrl = url;
  if (targetUrl.isEmpty()) {
    targetUrl = QUrl(QStringLiteral("dalinira://newtab/"));
  }

  // Handle internal scheme navigations directly
  const QString scheme = targetUrl.scheme().toLower();
  const QString host = targetUrl.host().toLower();
  if (scheme == QLatin1String("dalinira") && host != QLatin1String("newtab")) {
    if (host == QLatin1String("welcome") || host == QLatin1String("onboarding")) { showOnboarding(); return tabStrip_ ? tabStrip_->currentIndex() : 0; }
    if (host == QLatin1String("settings")) { showSettings(); return tabStrip_->currentIndex(); }
    if (host == QLatin1String("passwords")) { showPasswords(); return tabStrip_->currentIndex(); }
    if (host == QLatin1String("audio-effects")) { showAudioEffects(); return tabStrip_->currentIndex(); }
    if (host == QLatin1String("eq-presets")) { showEqPresetBrowser(); return tabStrip_->currentIndex(); }
    if (host == QLatin1String("blocker")) { showDaliNiraBlockerSettings(); return tabStrip_->currentIndex(); }
    if (host == QLatin1String("downloads")) { showMediaDownloads(); return tabStrip_->currentIndex(); }
    if (host == QLatin1String("listen") || host == QLatin1String("pulse")) { showSongFinder(); return tabStrip_->currentIndex(); }
    if (host == QLatin1String("listen-settings")) { showSongFinderSettings(); return tabStrip_->currentIndex(); }
  }

  if (dalinira::engine::EngineBackend::selectedEngineType() == dalinira::engine::EngineType::WebKit &&
      dalinira::engine::EngineBackend::instance()) {
    QUrl loadUrl = targetUrl;
    if (loadUrl.isEmpty()) {
      loadUrl = QUrl(QStringLiteral("dalinira://newtab/"));
    }

    auto *backend = dalinira::engine::EngineBackend::instance();
    auto *engineView = isIncognito()
        ? backend->createEphemeralView(pageStack_)
        : backend->createView(pageStack_);
#if defined(DALINIRA_BUILD_WEBKIT)
    if (auto *webkitView = qobject_cast<dalinira::engine::WebKitEngineView *>(engineView)) {
      webkitView->setNewTabProfileData(services_.profileService);
    }
#endif
    const uint64_t tabId = s_tabIdSequence.fetch_add(1);
    BrowserTabInfo info;
    info.id = tabId;
    info.title = initialTitle.trimmed().isEmpty()
        ? (isIncognito() ? I18n::text(QStringLiteral("tab.new_private")) : I18n::text(QStringLiteral("newtab.title")))
        : initialTitle.trimmed();
    info.url = loadUrl;
    info.view = nullptr;
    info.content = engineView;
    info.engineView = engineView;
    info.isInternal = false;
    info.isPinned = initiallyPinned;
    info.icon = isIncognito() ? BrowserIcons::incognitoIcon() : BrowserIcons::appIcon();
    info.uuid = services_.tabManager
        ? services_.tabManager->registerEngineTab(engineView, this, false, info.title, loadUrl)
        : QUuid::createUuid();

    wireEngineViewSignals(engineView, tabId);

#if defined(DALINIRA_BUILD_WEBKIT)
    if (auto *wkev = qobject_cast<dalinira::engine::WebKitEngineView *>(engineView)) {
      if (services_.profileService && services_.profileService->newTabBackgroundStore()) {
        auto *backgroundStore = services_.profileService->newTabBackgroundStore();
        wkev->setManagedBackgroundPaths(backgroundStore->managedImagePath(),
                                        backgroundStore->thumbnailPath());
      }
      // Same cosmetic + procedural scripts and site policy as Chromium's
      // prepareAdBlockScripts, injected at document start in an isolated world.
      const QPointer<DaliNiraBlockerService> blocker(
          services_.profileService ? services_.profileService->adBlockService() : nullptr);
      const QPointer<WebAudioEffectsController> audioEffects(services_.audioEffects);
      const QPointer<dalinira::engine::WebKitEngineView> scriptedView(wkev);
      const quint64 blockerTabId = reinterpret_cast<quintptr>(engineView);
      wkev->setDocumentScriptProvider([blocker, audioEffects, scriptedView, blockerTabId](const QUrl &url) -> QList<dalinira::engine::DocumentScript> {
        // The shared audio effects runtime, as Chromium's document bootstrap.
        QList<dalinira::engine::DocumentScript> scripts =
            audioEffects ? audioEffects->documentScriptsFor(scriptedView, url) : QList<dalinira::engine::DocumentScript>{};
        const QString scheme = url.scheme().toLower();
        // GPC and fingerprinting protection: the source Chromium runs as a
        // profile script, in every frame and the page's own world.
        if (blocker && (scheme == QLatin1String("http") || scheme == QLatin1String("https"))) {
          // WebKit asks for document-start sources before urlChanged. Advance
          // the shared top-level chain here so a newly committed bounce can
          // receive its temporary-storage listener in time.
          blocker->updateTabUrl(blockerTabId, url);
          dalinira::engine::DocumentScript shield;
          shield.source = blocker->privacyShieldScript();
          shield.mainWorld = true;
          shield.allFrames = true;
          scripts.prepend(shield);
          const QString temporaryStorage = blocker->temporaryStorageCleanupScript(blockerTabId, url);
          if (!temporaryStorage.isEmpty()) {
            dalinira::engine::DocumentScript cleanup;
            cleanup.source = temporaryStorage;
            cleanup.mainWorld = false;
            cleanup.allFrames = false;
            scripts.prepend(cleanup);
          }
        }
        if (!blocker || url.host().isEmpty() ||
            (scheme != QLatin1String("http") && scheme != QLatin1String("https"))) return scripts;
        // Exactly the scripts Chromium installs (cosmetic, procedural,
        // scriptlets in both worlds, fingerprint protection), same worlds.
        for (const QWebEngineScript &script : blocker->createScriptingScriptsForHost(url.host().toLower(), url)) {
          dalinira::engine::DocumentScript documentScript;
          documentScript.source = script.sourceCode();
          documentScript.mainWorld = script.worldId() == QWebEngineScript::MainWorld;
          documentScript.allFrames = script.runsOnSubFrames();
          documentScript.reapply = script.name() == QLatin1String("dalinira-adblock-cosmetic") ||
                                   script.name() == QLatin1String("dalinira-adblock-procedural");
          scripts.append(documentScript);
        }
        return scripts;
      });
      // Network requests: the same DaliNiraBlockerService decision as the
      // Chromium interceptor, counted for this tab (its blocker tab id).
      if (blocker) {
        const std::shared_ptr<DaliNiraBlockerNetworkGate> gate = blocker->networkGate();
        wkev->setNetworkRequestEvaluator([gate, blockerTabId](const dalinira::engine::WebKitNetworkFilter::Request &request) {
          using Filter = dalinira::engine::WebKitNetworkFilter;
          const NetworkRequestVerdict verdict = gate->decide(
              request.url, resourceTypeFromFetchDestination(request.destination), request.firstPartyUrl,
              blockerTabId, request.method.toLower());
          switch (verdict.kind) {
            case NetworkVerdictKind::Block: return Filter::Verdict{Filter::Action::Block, {}};
            case NetworkVerdictKind::Redirect: return Filter::Verdict{Filter::Action::Redirect, verdict.redirectUrl};
            case NetworkVerdictKind::Allow: break;
          }
          return Filter::Verdict{};
        });
      }
      if (blocker) {
        // $popup rules: the same shared decision the Chromium pages ask for.
        const QPointer<DaliNiraBlockerService> popupBlocker(blocker);
        const quint64 popupTabId = reinterpret_cast<quintptr>(engineView);
        const QPointer<dalinira::engine::EngineView> opener(engineView);
        wkev->setPopupEvaluator([popupBlocker, popupTabId, opener](const QUrl &target) {
          if (!popupBlocker || !opener) return true;
          return popupBlocker->decidePopup(target, opener->url(), popupTabId).action != DaliNiraBlockerAction::Block;
        });
      }
      if (BrowserProfileService *profileService = services_.profileService) {
        wkev->setScriptPopupsAllowed(profileService->arePopupsAllowed());
        connect(profileService, &BrowserProfileService::contentSettingsChanged, wkev,
                [wkev, profileService] { wkev->setScriptPopupsAllowed(profileService->arePopupsAllowed()); });
      }
    }
#endif

    const int index = (insertIndex >= 0 && insertIndex <= tabs_.size())
        ? insertIndex : tabs_.size();

    tabs_.insert(index, info);
    pageStack_->insertWidget(index, engineView);
    tabStrip_->insertTab(index, tabId, info.title, info.icon, info.isPinned);
    if (info.isPinned && services_.tabManager && services_.tabManager->performanceManager())
      services_.tabManager->performanceManager()->setTabPinned(info.uuid, true);

    if (services_.profileService && services_.profileService->adBlockService()) {
      const quint64 adBlockTabId = reinterpret_cast<quintptr>(engineView);
      services_.profileService->adBlockService()->registerTab(adBlockTabId, loadUrl);
      services_.profileService->adBlockService()->setActiveTabId(adBlockTabId);
    }

    // The same audio effects engine as Chromium tabs.
    if (services_.audioEffects) services_.audioEffects->registerEngineView(engineView, loadUrl);

    switchTab(index);
    // Make the foreign WebKit surface the selected stack page before loading.
    // WebKitGTK may complete a local load while an X11 child is unmapped and
    // then provide no first composited frame when the tab is selected, which
    // presents as a blank page or stale pixels from the preceding Qt page.
    // A popup's tab is loaded by WebKit itself, from its opener.
    if (!adoptingEnginePopup_) engineView->load(loadUrl);
    return index;
  }

  auto *view = new BrowserWebView(this, pageStack_);
  auto *page = new BrowserWebPage(services_.profile ? services_.profile : QWebEngineProfile::defaultProfile(), this, view);
  page->setBackgroundColor(QColor(7, 17, 31));
  view->setPage(page);

  // Audio Effects registration
  if (services_.audioEffects) {
    services_.audioEffects->registerWebView(view, targetUrl);
  }

  // Blocker tab registration
  if (services_.profileService && services_.profileService->adBlockService()) {
    const quint64 adBlockTabId = reinterpret_cast<quintptr>(view);
    services_.profileService->adBlockService()->registerTab(adBlockTabId, targetUrl);
    services_.profileService->adBlockService()->setActiveTabId(adBlockTabId);
  }

  // Permission handling (microphone, media capture, fullscreen)
  if (view->page()) {
    view->page()->settings()->setAttribute(QWebEngineSettings::FullScreenSupportEnabled, true);
    connect(view->page(), &QWebEnginePage::fullScreenRequested, this,
            [this, view](const QWebEngineFullScreenRequest &request) {
      handleFullScreenRequest(view, request);
    });
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    connect(view->page(), &QWebEnginePage::permissionRequested, this,
            [this, view](const QWebEnginePermission &permission) {
      handleTabPermissionRequested(view, permission);
    });
#else
    connect(view->page(), &QWebEnginePage::featurePermissionRequested, this,
            [this, view](const QUrl &origin, QWebEnginePage::Feature feature) {
      if (!view || !view->page()) return;
      if (!permissionBubble_) {
        permissionBubble_ = new SitePermissionPromptBubble(this);
      }
      permissionBubble_->setupPromptLegacy(origin, BrowserPermissionPolicy::featureName(feature), BrowserIcon::Privacy);
      updatePermissionBubblePosition();
      permissionBubble_->show();
      permissionBubble_->raise();
      auto conn = std::make_shared<QMetaObject::Connection>();
      *conn = connect(permissionBubble_, &SitePermissionPromptBubble::choiceMade, this,
                      [this, view, origin, feature, conn](SitePermissionChoice choice) {
        disconnect(*conn);
        if (!view || !view->page()) return;
        const bool grant = (choice == SitePermissionChoice::AllowThisVisit || choice == SitePermissionChoice::AlwaysAllow);
        view->page()->setFeaturePermission(
            origin, feature,
            grant ? QWebEnginePage::PermissionGrantedByUser : QWebEnginePage::PermissionDeniedByUser);
      });
    });
#endif
  }

  const uint64_t tabId = s_tabIdSequence.fetch_add(1);
  BrowserTabInfo info;
  info.id = tabId;
  info.title = initialTitle.trimmed().isEmpty()
      ? (isIncognito() ? I18n::text(QStringLiteral("tab.new_private")) : I18n::text(QStringLiteral("newtab.title")))
      : initialTitle.trimmed();
  info.url = targetUrl;
  info.view = view;
  info.isInternal = false;
  info.isPinned = initiallyPinned;
  info.icon = isIncognito() ? BrowserIcons::incognitoIcon() : BrowserIcons::appIcon();
  info.uuid = services_.tabManager
      ? services_.tabManager->registerTab(view, this, false, info.title)
      : QUuid::createUuid();

  wireViewSignals(view, tabId);

  if (auto *bp = dynamic_cast<BrowserWebPage *>(view->page())) {
    bp->setMediaCaptureCallback([this, tabId](bool activeCam, bool activeMic) {
      const int idx = findIndexByTabId(tabId);
      if (idx != -1) {
        tabs_[idx].activeCamera = activeCam;
        tabs_[idx].activeMicrophone = activeMic;
        if (tabStrip_ && idx == tabStrip_->currentIndex()) {
          updateOmniboxLeadingIcon();
          if (siteControlsBubble_ && siteControlsBubble_->isVisible()) {
            const auto &t = tabs_[idx];
            const QString canon = BrowserProfileService::canonicalOrigin(t.url);
            siteControlsBubble_->updateForTab(
                t.id, t.url, t.url.scheme() == QLatin1String("https"),
                t.activeCamera, t.activeMicrophone,
                [this, id = t.id, canon](const QString &k) {
                  return hasTabSessionPermission(id, canon, k);
                });
          }
        }
      }
    });
  }

  const int index = (insertIndex >= 0 && insertIndex <= tabs_.size())
      ? insertIndex : tabs_.size();

  tabs_.insert(index, info);
  pageStack_->insertWidget(index, view);
  tabStrip_->insertTab(index, tabId, info.title, info.icon, info.isPinned);
  if (info.isPinned && services_.tabManager && services_.tabManager->performanceManager())
    services_.tabManager->performanceManager()->setTabPinned(info.uuid, true);

  prepareAdBlockScripts(view->page(), targetUrl);
  view->load(targetUrl);
  switchTab(index);

  return index;
}

// The connections and callbacks that tie an engine page to the window
// holding it. Made when the tab is created and again when it moves to
// another window (adoptTab), as wireViewSignals does for Chromium pages.
void BrowserWindow::wireEngineViewSignals(dalinira::engine::EngineView *engineView, uint64_t tabId) {
  // A native page (WebKit) reports pointer moves itself: the side panel's
  // edge reveal follows them, event-driven, as Qt's events on Chromium pages.
  connect(engineView, &dalinira::engine::EngineView::pagePointerMoved, this, [this, engineView](const QPoint &globalPos) {
    if (layoutController_ && engineView->window() == this) layoutController_->updatePanelForPointer(globalPos);
  });

  connect(engineView, &dalinira::engine::EngineView::iconChanged, this, [this, tabId, engineView](const QIcon &icon) {
    const int idx = findIndexByTabId(tabId);
    if (idx >= 0 && tabs_[idx].engineView == engineView) {
      applyTabFavicon(idx, engineView, icon, true);
      syncNewTabViews();
    }
  });

  connect(engineView, &dalinira::engine::EngineView::urlChanged, this, [this, tabId, engineView](const QUrl &u) {
    const int idx = findIndexByTabId(tabId);
    if (idx >= 0 && tabs_[idx].engineView == engineView) {
      if (u.isValid() && !u.isEmpty() && u.toString() != QLatin1String("about:blank")) {
        tabs_[idx].url = u;
      }
      if (services_.profileService && services_.profileService->adBlockService()) {
        services_.profileService->adBlockService()->updateTabUrl(reinterpret_cast<quintptr>(engineView), tabs_[idx].url);
      }
      if (services_.profileService && !isIncognito()) {
        bool isTyped = false;
        const QString scheme = u.scheme().toLower();
        if (scheme == QLatin1String("http") || scheme == QLatin1String("https")) {
          isTyped = !tabs_[idx].expectedTypedUrl.isEmpty()
              && navigationUrlKey(tabs_[idx].expectedTypedUrl) == navigationUrlKey(u);
          tabs_[idx].expectedTypedUrl = QUrl{};
        }
        services_.profileService->recordHistory(u, u.host(), isTyped,
                                                dalinira::engine::engineKey(engineView->engineType()));
      }
      if (services_.tabManager) services_.tabManager->updateUrl(tabs_[idx].uuid, tabs_[idx].url);
      if (isNewTabUrl(tabs_[idx].url)) applyTabFavicon(idx, engineView, QIcon(), false);
      if (tabStrip_ && tabStrip_->currentIndex() == idx) {
        if (u.isValid() && !u.isEmpty() && !isNewTabUrl(u) && u.scheme() != QLatin1String("dalinira")) {
          lastActiveWebUrl_ = u;
        }
        updateOmniboxForCurrentTab();
        updateOmniboxLeadingIcon();
        updateBookmarkButtonState();
        updateNavButtons();
        updateBookmarkBarVisibility();
        updateBlockerControls();
      }
      tabs_[idx].translationOffered = false;
      if (tabs_[idx].translator) tabs_[idx].translator->reset();
    }
  });

  connect(engineView, &dalinira::engine::EngineView::audibleChanged, this, [this, tabId, engineView](bool audible) {
    const int idx = findIndexByTabId(tabId);
    if (idx >= 0 && tabs_[idx].engineView == engineView) {
      if (tabStrip_) tabStrip_->setTabAudible(idx, audible);
      if (tabStrip_ && tabStrip_->currentIndex() == idx) {
        updateNavButtons();
      }
    }
  });

  connect(engineView, &dalinira::engine::EngineView::titleChanged, this, [this, tabId, engineView](const QString &t) {
    const int idx = findIndexByTabId(tabId);
    if (idx >= 0 && tabs_[idx].engineView == engineView) {
      QString displayTitle = t.trimmed();
      if (displayTitle.isEmpty()) {
        if (tabs_[idx].internalId == QLatin1String("local-media-player") && !tabs_[idx].title.isEmpty()) {
          displayTitle = tabs_[idx].title;
        } else {
          displayTitle = isIncognito() ? I18n::text(QStringLiteral("tab.new_private")) : I18n::text(QStringLiteral("newtab.title"));
        }
      }
      tabs_[idx].title = displayTitle;
      if (services_.profileService && !isIncognito() && !isNewTabUrl(tabs_[idx].url)) {
        services_.profileService->updateHistoryTitle(tabs_[idx].url, displayTitle);
      }
      if (tabStrip_) tabStrip_->setTabTitle(idx, displayTitle);
      if (services_.tabManager) services_.tabManager->updateTitle(tabs_[idx].uuid, displayTitle);
      updateSidePanelTabs();
      if (tabStrip_ && tabStrip_->currentIndex() == idx) {
        setWindowTitle(displayTitle + QStringLiteral(" — DaliNira Browser"));
      }
    }
  });

  connect(engineView, &dalinira::engine::EngineView::loadStarted, this, [this, tabId, engineView]() {
    const int idx = findIndexByTabId(tabId);
    if (idx >= 0 && tabs_[idx].engineView == engineView) {
      tabs_[idx].isLoading = true;
      if (!tabs_[idx].isInternal && !isNewTabUrl(tabs_[idx].url)) {
        TabThrobber::instance().startLoading(engineView, this);
        if (tabStrip_) tabStrip_->setTabLoading(idx, true);
      }
      if (tabStrip_ && tabStrip_->currentIndex() == idx) {
        updateReloadStopButton(true);
        updateOmniboxLeadingIcon();
        updateBookmarkBarVisibility();
      }
    }
  });

  connect(engineView, &dalinira::engine::EngineView::loadProgress, this, [this, tabId](int progress) {
    const int idx = findIndexByTabId(tabId);
    if (idx >= 0 && tabStrip_ && tabStrip_->currentIndex() == idx) {
      const bool loading = (progress < 100);
      if (progressBar_) {
        if (loading) {
          progressBar_->setValue(progress);
          progressBar_->show();
        } else {
          progressBar_->hide();
        }
      }
      updateReloadStopButton(loading);
    }
  });

  connect(engineView, &dalinira::engine::EngineView::loadFinished, this, [this, tabId, engineView](bool ok) {
    const QUrl httpFallback = httpsUpgradeResult({engineView->url()}, ok);
    if (httpFallback.isValid()) {
      const QPointer<dalinira::engine::EngineView> guarded(engineView);
      QMetaObject::invokeMethod(this, [guarded, httpFallback] { if (guarded) guarded->load(httpFallback); },
                                Qt::QueuedConnection);
    }
    const int idx = findIndexByTabId(tabId);
    if (idx >= 0 && tabs_[idx].engineView == engineView) {
      tabs_[idx].isLoading = false;
      const QIcon cachedLoadIcon = TabThrobber::instance().cachedFavicon(engineView);
      TabThrobber::instance().finishLoading(engineView, ok);
      if (tabStrip_) {
        tabStrip_->setTabLoading(idx, false);
      }
      applyTabFavicon(idx, engineView, !cachedLoadIcon.isNull() ? cachedLoadIcon : engineView->icon(), false);
      if (tabStrip_ && tabStrip_->currentIndex() == idx) {
        if (progressBar_) progressBar_->hide();
        updateReloadStopButton(false);
        updateOmniboxLeadingIcon();
        updateNavButtons();
        updateBlockerControls();
      }
      if (ok && isNewTabUrl(tabs_[idx].url)) syncNewTabViews();
      if (ok && !tabs_[idx].isInternal && !isNewTabUrl(tabs_[idx].url)) {
        const QString scheme = tabs_[idx].url.scheme().toLower();
        if ((scheme == QLatin1String("http") || scheme == QLatin1String("https")) &&
            services_.profileService && services_.profileService->translateService() &&
            services_.profileService->translateService()->isEnabled()) {
          if (auto *translator = getOrCreatePageTranslator(idx)) translator->detectLanguage();
        }
      }
    }
  });

  connect(engineView, &dalinira::engine::EngineView::newWindowRequested, this, [this](const QUrl &u) {
    if (u.isValid() && !u.isEmpty()) {
      addNewTab(u);
    }
  });

  connect(engineView, &dalinira::engine::EngineView::linkContextMenuRequested,
          this, &BrowserWindow::showLinkContextMenu);
  connect(engineView, &dalinira::engine::EngineView::selectionContextMenuRequested,
          this, &BrowserWindow::showSelectionContextMenu);
  connect(engineView, &dalinira::engine::EngineView::pageContextMenuRequested,
          this, &BrowserWindow::showPageContextMenu);

  connect(engineView, &dalinira::engine::EngineView::webProcessTerminated, this, [this, tabId, engineView](int reason) {
    qWarning() << "WebProcess terminated in tab" << tabId << "reason" << reason;
    const int idx = findIndexByTabId(tabId);
    if (idx >= 0 && tabs_[idx].engineView == engineView) {
      tabs_[idx].title = I18n::text(QStringLiteral("tab.process_terminated"));
      if (tabStrip_) tabStrip_->setTabTitle(idx, tabs_[idx].title);
      // A dead page never finishes loading; clear the loading UI so the tab
      // shows Reload instead of an endless Stop/throbber.
      tabs_[idx].isLoading = false;
      TabThrobber::instance().finishLoading(engineView, false);
      if (tabStrip_) tabStrip_->setTabLoading(idx, false);
      updateSidePanelTabs();
      if (tabStrip_ && tabStrip_->currentIndex() == idx) {
        if (progressBar_) progressBar_->hide();
        updateReloadStopButton(false);
      }
    }
  });

  connect(engineView, &dalinira::engine::EngineView::findResultFound, this, [this](int active, int total) {
    if (findBar_) {
      findBar_->setMatchCount(active, total);
    }
  });

  connect(engineView, &dalinira::engine::EngineView::fullScreenRequested, this, [this, engineView](bool on) {
    // Only the selected tab may take the window fullscreen.
    if (!on || currentEngineView() == engineView) setPageFullScreen(on, engineView);
  });

  connect(engineView, &dalinira::engine::EngineView::windowCloseRequested, this, [this, tabId] {
    closeTabForWindowClose(tabId);
  });

  connect(engineView, &dalinira::engine::EngineView::internalNavigationRequested,
          this, [this, engineView](const QUrl &internalUrl) {
    handleEngineInternalNavigation(engineView, internalUrl);
  });

  connect(engineView, &dalinira::engine::EngineView::contentBlockerHit,
          this, [this, engineView](quint64 count, const QString &kind) {
    if (services_.profileService && services_.profileService->adBlockService())
      services_.profileService->adBlockService()->reportBlockedEvent(
          reinterpret_cast<quintptr>(engineView), DaliNiraBlockType::Cosmetic, count, kind);
  });

#if defined(DALINIRA_BUILD_WEBKIT)
  if (auto *wkev = qobject_cast<dalinira::engine::WebKitEngineView *>(engineView)) {
    if (wkev->profile()) {
      connect(wkev->profile(), &dalinira::engine::WebKitEngineProfile::faviconLoaded,
              this, &BrowserWindow::onEngineFaviconLoaded, Qt::UniqueConnection);
    }
    webKitCredentialController_->registerView(wkev);
    connect(wkev, &dalinira::engine::WebKitEngineView::deferredPopupBlocked,
            this, [this, wkev] {
      const int idx = indexOfEngineView(wkev);
      if (idx >= 0) {
        if (tabs_.size() > 1) closeTab(idx);
        else close();
      }
    });
    wkev->setPageScriptPolicy([window = QPointer<BrowserWindow>(this)](const QUrl &url) {
      return !window || window->pageScriptsAllowed(url);
    });
    // Same placement as Chromium's newWindowRequested handler: a new
    // window, a background tab (the opener stays selected) or a tab.
    const QPointer<dalinira::engine::EngineView> opener(engineView);
    wkev->setNewTabCallback([this, opener](const dalinira::engine::WebKitEngineView::NewTabRequest &request)
                                -> dalinira::engine::WebKitEngineView * {
      using Disposition = dalinira::engine::WebKitEngineView::NewTabRequest::Disposition;
      BrowserWindow *host = this;
      if (request.disposition == Disposition::NewWindow) {
        host = createWindow(services_);
        host->show();
      }
      const QUrl initialUrl = request.url.isValid() ? request.url : QUrl(QStringLiteral("about:blank"));
      int newIdx = -1;
      {
        QScopedValueRollback<bool> adopting(host->adoptingEnginePopup_, true);
        newIdx = host->addNewTab(initialUrl);
      }
      if (newIdx < 0 || newIdx >= host->tabs_.size()) return nullptr;
      auto *popup = qobject_cast<dalinira::engine::WebKitEngineView *>(host->tabs_[newIdx].engineView.data());
      if (request.disposition == Disposition::BackgroundTab && opener) {
        const int openerIdx = indexOfEngineView(opener.data());
        if (openerIdx >= 0) switchTab(openerIdx);
      }
      return popup;
    });
  }
#endif
}

int BrowserWindow::addInternalTab(QWidget *page, const QString &title, const QIcon &icon,
                                  const QString &internalId, int insertIndex) {
  if (!page) return -1;

  const uint64_t tabId = s_tabIdSequence.fetch_add(1);
  BrowserTabInfo info;
  info.id = tabId;
  info.title = title;
  info.icon = icon.isNull() ? BrowserIcons::appIcon() : icon;
  info.content = page;
  info.view = nullptr;
  info.isInternal = true;
  info.internalId = internalId;
  info.uuid = services_.tabManager
      ? services_.tabManager->registerInternalTab(page, this, title, internalId, {true, true, false, false})
      : QUuid::createUuid();

  const int index = (insertIndex >= 0 && insertIndex <= tabs_.size())
      ? insertIndex : tabs_.size();

  tabs_.insert(index, info);
  pageStack_->insertWidget(index, page);
  tabStrip_->insertTab(index, tabId, title, info.icon);

  switchTab(index);
  return index;
}

void BrowserWindow::closeTab(int index) {
  if (translateBubble_ && translateBubble_->isVisible()) {
    translateBubble_->close();
  }
  dismissSiteControlsBubble();
  if (hoverCard_) hoverCard_->hideCard();
  if (tabStrip_) tabStrip_->cancelHover();
  if (index < 0 || index >= tabs_.size()) return;

  BrowserTabInfo info = tabs_.takeAt(index);
  // The page that holds element fullscreen is going away: give the window
  // back (browser F11 fullscreen has no owner and is kept).
  if (pageFullScreenOwner_ && (pageFullScreenOwner_ == info.view.data() || pageFullScreenOwner_ == info.engineView.data()))
    setPageFullScreen(false);
  clearTabSessionPermissions(info.id);
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
  if (currentActivePermissionRequest_.has_value() && currentActivePermissionRequest_->tabId == info.id) {
    dismissActivePermissionPrompt(true);
  }
  for (int i = pendingPermissionQueue_.size() - 1; i >= 0; --i) {
    if (pendingPermissionQueue_[i].tabId == info.id) {
      if (pendingPermissionQueue_[i].permission.isValid()) {
        pendingPermissionQueue_[i].permission.deny();
      }
      pendingPermissionQueue_.removeAt(i);
    }
  }
#endif
  if (info.groupId.has_value() && groupModel_) {
    const QUuid gid = *info.groupId;
    groupModel_->removeTabFromGroup(info.id);
    if (groupModel_->groupTabCount(gid) == 0) {
      groupModel_->removeGroup(gid);
    }
  }
  if (services_.profileService && !info.url.isEmpty() && info.url.isValid()) {
    services_.profileService->rememberClosedTab(info.url, info.title);
  }
  if (services_.tabManager && !info.uuid.isNull()) {
    services_.tabManager->remove(info.uuid);
  }

  if (info.view) {
    info.view->stop();
    info.view->disconnect(this);
    if (info.view->page()) {
      info.view->page()->disconnect(this);
    }
    if (services_.audioEffects) {
      services_.audioEffects->unregisterWebView(info.view.data());
    }
    if (autofillController_) {
      autofillController_->onViewClosed(info.view.data());
    }
    TabThrobber::instance().removeView(info.view.data());
    if (services_.profileService && services_.profileService->adBlockService()) {
      const quint64 adBlockTabId = reinterpret_cast<quintptr>(info.view.data());
      services_.profileService->adBlockService()->unregisterTab(adBlockTabId);
    }
    pageStack_->removeWidget(info.view);
    if (!beginForgetClosedView(info.view)) info.view->deleteLater();
  } else if (info.content) {
    if (info.engineView) {
      if (services_.profileService && services_.profileService->adBlockService()) {
        const quint64 adBlockTabId = reinterpret_cast<quintptr>(info.engineView.data());
        services_.profileService->adBlockService()->unregisterTab(adBlockTabId);
      }
      if (services_.audioEffects) services_.audioEffects->unregisterEngineView(info.engineView.data());
      info.engineView->stop();
      info.engineView->setEngineActive(false);
      info.engineView->disconnect(this);
    }
    if (info.internalId == QLatin1String("song-finder")
        && services_.songRecognition && services_.songRecognition->isListening()) {
      services_.songRecognition->stopListening();
    }
    pageStack_->removeWidget(info.content);
    info.content->deleteLater();
  }
  tabStrip_->removeTab(index);

  if (tabs_.isEmpty()) {
    close();
  } else {
    const int newIndex = std::min(index, static_cast<int>(tabs_.size()) - 1);
    switchTab(newIndex);
  }
}

void BrowserWindow::switchTab(int index) {
  if (isSwitchingTab_) return;
  if (translateBubble_ && translateBubble_->isVisible()) {
    translateBubble_->close();
  }
  dismissSiteControlsBubble();
  if (suggestionCompleter_ && suggestionCompleter_->popup()) suggestionCompleter_->popup()->hide();
  if (hoverCard_) hoverCard_->hideCard();
  if (index < 0 || index >= tabs_.size()) return;

  QScopedValueRollback<bool> switchingGuard(isSwitchingTab_, true);

  tabStrip_->setCurrentIndex(index);
  auto &info = tabs_[index];

  // QStackedWidget controls QWidget visibility, but a WebKitGTK tab also owns
  // a foreign X11 surface. Unmap every non-selected native surface before the
  // Qt stack changes so it cannot paint over or intercept input from Settings
  // and other shared DaliNira pages.
  for (int i = 0; i < tabs_.size(); ++i) {
    if (tabs_[i].engineView) tabs_[i].engineView->setEngineActive(i == index);
  }

  if (info.view) {
    pageStack_->setCurrentWidget(info.view);
    setWindowTitle(info.title + QStringLiteral(" — DaliNira Browser"));
    updateOmniboxForCurrentTab(true);
    updateNavButtons();
    updateReloadStopButton(TabThrobber::instance().isLoading(info.view));
    if (findBar_ && findBar_->isVisible()) {
      if (!findBar_->findText().isEmpty()) {
        handleFindRequest(findBar_->findText(), true, findBar_->isCaseSensitive());
      } else {
        findBar_->clearMatchCount();
      }
    }
    if (bookmarkBtn_ && services_.profileService) {
      const bool bm = services_.profileService->isBookmarked(info.url);
      bookmarkBtn_->setToolTip(bm ? I18n::text(QStringLiteral("toolbar.bookmark_remove")) : I18n::text(QStringLiteral("toolbar.bookmark_add")));
    }
    updateBlockerControls();
    if (services_.songRecognition) {
      QString cleanTitle = info.title.trimmed();
      if (cleanTitle.endsWith(QStringLiteral(" - YouTube"), Qt::CaseInsensitive)) cleanTitle.chop(10);
      const QStringList parts = cleanTitle.split(QStringLiteral(" - "));
      if (parts.size() >= 2) {
        services_.songRecognition->setWebContextMetadata(parts.mid(1).join(QStringLiteral(" - ")).trimmed(), parts.first().trimmed());
      } else {
        services_.songRecognition->setWebContextMetadata(cleanTitle, QString());
      }
    }
    if (services_.audioEffects) {
      services_.audioEffects->applyToView(info.view.data());
    }
  } else if (info.content) {
    pageStack_->setCurrentWidget(info.content);
    if (info.engineView) {
      info.engineView->setEngineActive(true);
      if (info.url.isValid() && !info.url.isEmpty() && !isNewTabUrl(info.url) && info.url.scheme() != QLatin1String("dalinira")) {
        lastActiveWebUrl_ = info.url;
      }
      if (services_.songRecognition) {
        QString cleanTitle = info.title.trimmed();
        if (cleanTitle.endsWith(QStringLiteral(" - YouTube"), Qt::CaseInsensitive)) cleanTitle.chop(10);
        const QStringList parts = cleanTitle.split(QStringLiteral(" - "));
        if (parts.size() >= 2) {
          services_.songRecognition->setWebContextMetadata(parts.mid(1).join(QStringLiteral(" - ")).trimmed(), parts.first().trimmed());
        } else {
          services_.songRecognition->setWebContextMetadata(cleanTitle, QString());
        }
      }
      updateOmniboxForCurrentTab(true);
      updateNavButtons();
      updateReloadStopButton(info.engineView->isLoading());
      setWindowTitle(info.title + QStringLiteral(" — DaliNira Browser"));
      info.engineView->setFocus();
      if (services_.profileService && services_.profileService->adBlockService()) {
        services_.profileService->adBlockService()->setActiveTabId(reinterpret_cast<quintptr>(info.engineView.data()));
      }
      if (services_.audioEffects) services_.audioEffects->applyToEngineView(info.engineView.data());
    } else {
      info.content->show();
      info.content->raise();
      info.content->update();
      omnibox_->setText(QStringLiteral("dalinira://") + info.internalId);
      backBtn_->setEnabled(false);
      forwardBtn_->setEnabled(false);
    }
    updateBlockerControls();
  }

  updateOmniboxLeadingIcon();

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
  if (currentActivePermissionRequest_.has_value()) {
    if (currentActivePermissionRequest_->tabId == info.id) {
      if (permissionBubble_) {
        updatePermissionBubblePosition();
        permissionBubble_->show();
        permissionBubble_->raise();
      }
    } else {
      if (permissionBubble_) {
        permissionBubble_->hide();
      }
    }
  } else {
    processNextPermissionRequest();
  }
  syncProfilePermissionsForTab(info.id);
#endif

  updateBookmarkBarVisibility();

  if (translateButton_) {
    if (info.translator && (info.translator->state() == PageTranslator::State::Detected ||
                            info.translator->state() == PageTranslator::State::Translated ||
                            info.translator->state() == PageTranslator::State::Translating)) {
      translateButton_->show();
      if (!info.translationOffered && info.translator->state() == PageTranslator::State::Detected) {
        const QString offerKey = info.url.host().toLower() + QLatin1Char('|') +
            info.translator->sourceLanguage().toLower();
        if (!dismissedTranslationOffers_.contains(offerKey)) {
          info.translationOffered = true;
          dismissedTranslationOffers_.insert(offerKey);
          showTranslatePopup();
        }
      }
    } else {
      translateButton_->hide();
    }
  }

  if (autofillController_ && autofillController_->activeSaveBubble()) {
    auto *bubble = autofillController_->activeSaveBubble();
    if (info.view && autofillController_->activeBubbleView() == info.view.data()) {
      bubble->show();
      updateSaveBubblePosition();
    } else {
      bubble->hide();
    }
  }

  if (services_.tabManager && !info.uuid.isNull()) {
    services_.tabManager->activate(info.uuid);
  }
  if (info.pendingLanguageReload) {
    // After activation has unfrozen the page (see retranslateUi).
    info.pendingLanguageReload = false;
    const QPointer<QWebEngineView> view = info.view;
    const QPointer<dalinira::engine::EngineView> engineView = info.engineView;
    const QUrl url = info.url;
    QTimer::singleShot(0, this, [view, engineView, url] { reloadNewTabDocument(view, engineView, url); });
  }
  pageStack_->update();
  if (sidePanel_) sidePanel_->setActiveTab(index);
  updateSidePanelTabs();
}

void BrowserWindow::moveTab(int fromIndex, int toIndex) {
  if (isMovingTab_) return;
  if (hoverCard_) hoverCard_->hideCard();
  if (fromIndex < 0 || fromIndex >= tabs_.size() ||
      toIndex < 0 || toIndex >= tabs_.size() || fromIndex == toIndex) {
    return;
  }

  isMovingTab_ = true;

  QWidget *movedContent = tabs_[fromIndex].view
      ? static_cast<QWidget *>(tabs_[fromIndex].view.data())
      : tabs_[fromIndex].content.data();
  tabs_.move(fromIndex, toIndex);

  // Keep the stack order identical to the tab model. Several internal-page
  // lookup paths intentionally translate a content widget back to a tab index.
  if (movedContent && pageStack_->indexOf(movedContent) >= 0) {
    pageStack_->removeWidget(movedContent);
    pageStack_->insertWidget(toIndex, movedContent);
  }

  if (tabStrip_ && fromIndex < tabStrip_->count() && toIndex < tabStrip_->count()) {
    if (tabStrip_->tabId(toIndex) != tabs_[toIndex].id) {
      tabStrip_->moveTab(fromIndex, toIndex);
    }
  }

  syncTabManagerOrder();

  isMovingTab_ = false;
  switchTab(toIndex);
}

void BrowserWindow::syncTabManagerOrder() {
  if (!services_.tabManager) return;
  QVector<TabManager::TabId> orderedIds;
  orderedIds.reserve(tabs_.size());
  for (const auto &tab : std::as_const(tabs_)) orderedIds.push_back(tab.uuid);
  services_.tabManager->reorder(this, orderedIds);
}

bool BrowserWindow::transferSidePanelTabTo(uint64_t tabId,
                                           BrowserWindow *destination,
                                           int targetIndex) {
  if (!destination || destination == this ||
      destination->services_.profile != services_.profile) {
    return false;
  }

  const int sourceIndex = findIndexByTabId(tabId);
  if (sourceIndex < 0 || sourceIndex >= tabs_.size()) return false;
  const BrowserTabInfo sourceInfo = tabs_[sourceIndex];
  if (!sourceInfo.engineView ||
      sourceInfo.engineView->engineType() != dalinira::engine::EngineType::WebKit) {
    return transferTabTo(tabId, destination, targetIndex);
  }

  // A WebKitEngineView owns a foreign GTK/XEmbed surface. Reparenting that
  // live QWidget between top-level windows can leave GTK and Qt with different
  // native hosts and grab the X server. Recreate the page in the destination;
  // this intentionally reloads the URL but keeps the desktop responsive.
  const QUrl sourceUrl = sourceInfo.engineView->url().isValid()
      ? sourceInfo.engineView->url() : sourceInfo.url;
  const int newIndex = destination->addNewTab(
      sourceUrl, targetIndex, sourceInfo.isPinned, sourceInfo.title);
  if (newIndex < 0) return false;
  if (!sourceInfo.icon.isNull() && newIndex < destination->tabs_.size()) {
    destination->tabs_[newIndex].icon = sourceInfo.icon;
    destination->tabs_[newIndex].faviconHost = sourceInfo.faviconHost;
    destination->tabStrip_->setTabIcon(newIndex, sourceInfo.icon);
    destination->updateSidePanelTabs();
  }

  QTimer::singleShot(0, this, [this, tabId] {
    const int oldIndex = findIndexByTabId(tabId);
    if (oldIndex >= 0) closeTab(oldIndex);
  });
  return true;
}

bool BrowserWindow::transferTabTo(uint64_t tabId, BrowserWindow *destination, int targetIndex) {
  if (!destination || destination == this || destination->services_.profile != services_.profile) return false;

  int sourceIndex = findIndexByTabId(tabId);
  if (sourceIndex < 0) {
    const auto &session = dalinira::desktop_tabs::TabDragController::instance().session();
    if (session.isActive() && session.sourceWindow() == this) {
      sourceIndex = session.sourceTabIndex();
    }
  }
  if (sourceIndex < 0 && tabStrip_) {
    sourceIndex = tabStrip_->currentIndex();
  }
  if (sourceIndex < 0 || sourceIndex >= tabs_.size()) return false;

  BrowserTabInfo info = tabs_.takeAt(sourceIndex);
  if (info.groupId.has_value() && groupModel_) {
    const QUuid oldGid = *info.groupId;
    groupModel_->removeTabFromGroup(info.id);
    if (groupModel_->groupTabCount(oldGid) == 0) {
      groupModel_->removeGroup(oldGid);
    }
    info.groupId = std::nullopt; // Single detached tab leaves old group
  }
  if (info.view) {
    info.view->disconnect(this);
    pageStack_->removeWidget(info.view);
  } else if (info.content) {
    info.content->disconnect(this);
#if defined(DALINIRA_BUILD_WEBKIT)
    if (auto *wkev = qobject_cast<dalinira::engine::WebKitEngineView *>(info.engineView.data()))
      webKitCredentialController_->unregisterView(wkev);
#endif
    pageStack_->removeWidget(info.content);
  }
  tabStrip_->removeTab(sourceIndex);

  // The destination window owns the tab from now on (sleep policy, active
  // tab, internal-page lookups are per window).
  if (services_.tabManager && !info.uuid.isNull())
    services_.tabManager->transfer(info.uuid, destination, false);
  destination->adoptTab(info, targetIndex);

  if (tabs_.isEmpty()) {
    if (!isCaptureShell_) {
      close();
    } else {
      hide();
    }
  } else {
    const int newIdx = std::clamp(sourceIndex, 0, static_cast<int>(tabs_.size()) - 1);
    switchTab(newIdx);
  }

  return true;
}

void BrowserWindow::adoptTab(BrowserTabInfo info, int targetIndex) {
  const int idx = (targetIndex >= 0 && targetIndex <= tabs_.size())
      ? targetIndex : tabs_.size();

  if (info.icon.isNull()) {
    info.icon = BrowserIcons::appIcon();
  }

  tabs_.insert(idx, info);
  if (info.view) {
    info.view->setParent(pageStack_);
    pageStack_->insertWidget(idx, info.view);
    wireViewSignals(info.view, info.id);
    if (services_.audioEffects) {
      services_.audioEffects->registerWebView(info.view.data(), info.url);
      services_.audioEffects->applyToView(info.view.data());
    }
    if (services_.profileService && services_.profileService->adBlockService()) {
      const quint64 adBlockTabId = reinterpret_cast<quintptr>(info.view.data());
      services_.profileService->adBlockService()->registerTab(adBlockTabId, info.url);
      services_.profileService->adBlockService()->setActiveTabId(adBlockTabId);
    }
    if (info.view->page()) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
      auto *view = info.view.data();
      connect(info.view->page(), &QWebEnginePage::permissionRequested, this,
              [this, view](const QWebEnginePermission &permission) {
        handleTabPermissionRequested(view, permission);
      });
#else
      auto *view = info.view.data();
      connect(view->page(), &QWebEnginePage::featurePermissionRequested, this,
              [this, view](const QUrl &origin, QWebEnginePage::Feature feature) {
        if (!view || !view->page()) return;
        if (!permissionBubble_) {
          permissionBubble_ = new SitePermissionPromptBubble(this);
        }
        permissionBubble_->setupPromptLegacy(origin, BrowserPermissionPolicy::featureName(feature), BrowserIcon::Privacy);
        updatePermissionBubblePosition();
        permissionBubble_->show();
        permissionBubble_->raise();
        auto conn = std::make_shared<QMetaObject::Connection>();
        *conn = connect(permissionBubble_, &SitePermissionPromptBubble::choiceMade, this,
                        [this, view, origin, feature, conn](SitePermissionChoice choice) {
          disconnect(*conn);
          if (!view || !view->page()) return;
          const bool grant = (choice == SitePermissionChoice::AllowThisVisit || choice == SitePermissionChoice::AlwaysAllow);
          view->page()->setFeaturePermission(
              origin, feature,
              grant ? QWebEnginePage::PermissionGrantedByUser : QWebEnginePage::PermissionDeniedByUser);
        });
      });
#endif
    }
  } else if (info.content) {
    info.content->setParent(pageStack_);
    pageStack_->insertWidget(idx, info.content);
    if (info.engineView) wireEngineViewSignals(info.engineView.data(), info.id);
  }

  tabStrip_->insertTab(idx, info.id, info.title, info.icon, info.isPinned);
  syncTabManagerOrder();
  if (info.groupId.has_value() && groupModel_ && groupModel_->hasGroup(*info.groupId)) {
    groupModel_->setTabGroup(info.id, *info.groupId);
  }
  switchTab(idx);
  if (!isVisible()) show();
  raise();
}

int BrowserWindow::indexOfEngineView(const dalinira::engine::EngineView *view) const {
  for (int i = 0; i < tabs_.size(); ++i) {
    if (view && tabs_[i].engineView == view) return i;
  }
  return -1;
}

int BrowserWindow::findIndexByTabId(uint64_t tabId) const {
  for (int i = 0; i < tabs_.size(); ++i) {
    if (tabs_[i].id == tabId) return i;
  }
  return -1;
}

uint64_t BrowserWindow::findTabIdByIndex(int index) const {
  if (index >= 0 && index < tabs_.size()) {
    return tabs_[index].id;
  }
  return 0;
}

QWebEngineView *BrowserWindow::currentView() const {
  const int idx = tabStrip_->currentIndex();
  if (idx >= 0 && idx < tabs_.size() && !tabs_[idx].isInternal) {
    return tabs_[idx].view.data();
  }
  return nullptr;
}

dalinira::engine::EngineView *BrowserWindow::currentEngineView() const {
  const int idx = tabStrip_ ? tabStrip_->currentIndex() : -1;
  if (idx >= 0 && idx < tabs_.size() && tabs_[idx].engineView) {
    return tabs_[idx].engineView.data();
  }
  return nullptr;
}

QUrl BrowserWindow::currentTabUrl() const {
  if (auto *view = currentView()) return view->url();
  if (auto *ev = currentEngineView()) return ev->url();
  const int idx = tabStrip_ ? tabStrip_->currentIndex() : -1;
  if (idx >= 0 && idx < tabs_.size()) return tabs_[idx].url;
  return QUrl{};
}

QString BrowserWindow::currentTabTitle() const {
  if (auto *view = currentView()) return view->title();
  if (auto *ev = currentEngineView()) return ev->title();
  const int idx = tabStrip_ ? tabStrip_->currentIndex() : -1;
  if (idx >= 0 && idx < tabs_.size()) return tabs_[idx].title;
  return QString{};
}

bool BrowserWindow::currentTabAudible() const {
  if (auto *view = currentView()) return view->page() && view->page()->recentlyAudible();
  if (auto *ev = currentEngineView()) return ev->isAudible();
  return false;
}

QUrl BrowserWindow::currentActiveWebUrl() const {
  const QUrl tabUrl = currentTabUrl();
  if (!tabUrl.isEmpty() && !isNewTabUrl(tabUrl) && tabUrl.scheme() != QLatin1String("dalinira")) {
    return tabUrl;
  }
  return lastActiveWebUrl_;
}

// A page closed its own window (window.close() in a popup). Both engines only
// allow it for windows a script opened. The last tab closes the window.
void BrowserWindow::closeTabForWindowClose(uint64_t tabId) {
  const int idx = findIndexByTabId(tabId);
  if (idx < 0) return;
  if (tabs_.size() > 1) closeTab(idx);
  else close();
}

void BrowserWindow::wireViewSignals(QWebEngineView *view, uint64_t tabId) {
  if (!view) return;

  if (view->page()) {
    // window.close() from a window a script opened (Blink decides which).
    connect(view->page(), &QWebEnginePage::windowCloseRequested, this, [this, tabId] {
      closeTabForWindowClose(tabId);
    });
  }

  connect(view, &QWebEngineView::titleChanged, this, [this, tabId, view](const QString &title) {
    const int idx = findIndexByTabId(tabId);
    if (idx >= 0 && tabs_[idx].view == view) {
      if (title.isEmpty() || isNewTabUrl(view->url()) || I18n::isTranslationOf(QStringLiteral("newtab.title"), title)) {
        tabs_[idx].title = isIncognito() ? I18n::text(QStringLiteral("tab.new_private")) : I18n::text(QStringLiteral("newtab.title"));
      } else {
        tabs_[idx].title = title;
      }
      tabStrip_->setTabText(idx, tabs_[idx].title);
      updateSidePanelTabs();
      if (services_.tabManager && !tabs_[idx].uuid.isNull()) {
        services_.tabManager->updateTitle(tabs_[idx].uuid, tabs_[idx].title);
      }
      // The window title follows the current page, as on WebKit.
      if (idx == tabStrip_->currentIndex()) setWindowTitle(tabs_[idx].title + QStringLiteral(" — DaliNira Browser"));
      if (idx == tabStrip_->currentIndex() && services_.songRecognition) {
        QString cleanTitle = tabs_[idx].title.trimmed();
        if (cleanTitle.endsWith(QStringLiteral(" - YouTube"), Qt::CaseInsensitive)) cleanTitle.chop(10);
        const QStringList parts = cleanTitle.split(QStringLiteral(" - "));
        if (parts.size() >= 2) {
          services_.songRecognition->setWebContextMetadata(parts.mid(1).join(QStringLiteral(" - ")).trimmed(), parts.first().trimmed());
        } else {
          services_.songRecognition->setWebContextMetadata(cleanTitle, QString());
        }
      }
      if (services_.profileService) {
        services_.profileService->updateHistoryTitle(view->url(), title);
      }
    }
  });

  connect(view, &QWebEngineView::loadStarted, this, [this, tabId, view] {
    const int idx = findIndexByTabId(tabId);
    if (idx >= 0) {
      tabs_[idx].isLoading = true;  // as for engine views (side panel, tab state)
      tabs_[idx].pageIconReported = false;
    }
    if (idx >= 0 && !tabs_[idx].isInternal && !isNewTabUrl(view->url())) {
      TabThrobber::instance().startLoading(view, this);
      tabStrip_->setTabLoading(idx, true);
    }
    if (idx == tabStrip_->currentIndex()) {
      updateReloadStopButton(true);
      updateBookmarkBarVisibility();
    }
  });

  connect(view, &QWebEngineView::iconChanged, this, [this, tabId, view](const QIcon &icon) {
    const int idx = findIndexByTabId(tabId);
    if (idx >= 0) {
      applyTabFavicon(idx, view, icon, true);
      syncNewTabViews();
    }
  });

  connect(view, &QWebEngineView::urlChanged, this, [this, tabId, view](const QUrl &url) {
    const int idx = findIndexByTabId(tabId);
    if (idx >= 0) {
      tabs_[idx].url = url;
      tabs_[idx].activeCamera = false;
      tabs_[idx].activeMicrophone = false;
      if (auto *bp = dynamic_cast<BrowserWebPage *>(view->page())) {
        bp->resetMediaCapture();
      }
      const QString newCanon = BrowserProfileService::canonicalOrigin(url);
      clearTabSessionPermissionsForOrigin(tabId, newCanon);
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
      if (currentActivePermissionRequest_.has_value() &&
          currentActivePermissionRequest_->tabId == tabId &&
          currentActivePermissionRequest_->canonicalOrigin != newCanon) {
        dismissActivePermissionPrompt(true);
      }
      for (int i = pendingPermissionQueue_.size() - 1; i >= 0; --i) {
        if (pendingPermissionQueue_[i].tabId == tabId &&
            pendingPermissionQueue_[i].canonicalOrigin != newCanon) {
          if (pendingPermissionQueue_[i].permission.isValid()) {
            pendingPermissionQueue_[i].permission.deny();
          }
          pendingPermissionQueue_.removeAt(i);
        }
      }
#endif
      if (isNewTabUrl(url) || url.isEmpty()) {
        tabs_[idx].icon = isIncognito() ? BrowserIcons::incognitoIcon() : BrowserIcons::appIcon();
        tabStrip_->setTabIcon(idx, tabs_[idx].icon);
      }
      if (!isNewTabUrl(url) && url.scheme() != QLatin1String("dalinira") && url.isValid()) {
        lastActiveWebUrl_ = url;
      }
      if (services_.profileService) {
        bool isTyped = false;
        const QString scheme = url.scheme().toLower();
        if (scheme == QLatin1String("http") || scheme == QLatin1String("https")) {
          isTyped = !tabs_[idx].expectedTypedUrl.isEmpty()
              && navigationUrlKey(tabs_[idx].expectedTypedUrl) == navigationUrlKey(url);
          tabs_[idx].expectedTypedUrl = QUrl{};
        }
        services_.profileService->recordHistory(url, url.host(), isTyped, QStringLiteral("chromium"));
        if (services_.profileService->adBlockService()) {
          const quint64 adBlockTabId = reinterpret_cast<quintptr>(view);
          services_.profileService->adBlockService()->updateTabUrl(adBlockTabId, url);
        }
      }
      if (idx == tabStrip_->currentIndex()) {
        updateOmniboxForCurrentTab();
        updateOmniboxLeadingIcon();
        updateNavButtons();
        updateBlockerControls();
        updateBookmarkBarVisibility();
        if (translateBubble_ && translateBubble_->isVisible()) {
          translateBubble_->close();
        }
        if (translateButton_) {
          translateButton_->hide();
        }
        if (siteControlsBubble_ && siteControlsBubble_->isVisible()) {
          const auto &t = tabs_[idx];
          const QString canon = BrowserProfileService::canonicalOrigin(t.url);
          siteControlsBubble_->updateForTab(
              t.id, t.url, t.url.scheme() == QLatin1String("https"),
              t.activeCamera, t.activeMicrophone,
              [this, id = t.id, canon](const QString &k) {
                return hasTabSessionPermission(id, canon, k);
              });
        }
      }
      prepareAdBlockScripts(tabs_[idx].view ? tabs_[idx].view->page() : nullptr, url);
      if (services_.tabManager && !tabs_[idx].uuid.isNull()) {
        services_.tabManager->updateUrl(tabs_[idx].uuid, url);
      }
      if (services_.audioEffects) {
        services_.audioEffects->applyToView(view);
      }
      if (autofillController_) {
        autofillController_->onUrlChanged(view, url);
      }
      tabs_[idx].translationOffered = false;
      if (tabs_[idx].translator) {
        tabs_[idx].translator->reset();
      }
    }
  });

  connect(view, &QWebEngineView::loadProgress, this, [this, tabId](int progress) {
    const int idx = findIndexByTabId(tabId);
    if (idx == tabStrip_->currentIndex()) {
      const bool loading = (progress < 100);
      if (loading) {
        progressBar_->setValue(progress);
        progressBar_->show();
      } else {
        progressBar_->hide();
      }
      updateReloadStopButton(loading);
    }
  });

  connect(view, &QWebEngineView::loadFinished, this, [this, tabId, view](bool success) {
    const QUrl httpFallback = httpsUpgradeResult({view->url(), view->page()->requestedUrl()}, success);
    if (httpFallback.isValid()) {
      const QPointer<QWebEngineView> guarded(view);
      QMetaObject::invokeMethod(this, [guarded, httpFallback] { if (guarded) guarded->load(httpFallback); },
                                Qt::QueuedConnection);
    }
    const int idx = findIndexByTabId(tabId);
    if (idx >= 0) {
      // QWebEngineView::icon() may dereference an already-detached WebContents
      // adapter while a navigation is finishing. iconChanged has already cached
      // any favicon that arrived, so preserve that value before finishLoading()
      // removes the throbber state and avoid querying WebEngine synchronously.
      tabs_[idx].isLoading = false;
      const QIcon cachedLoadIcon = TabThrobber::instance().cachedFavicon(view);
      TabThrobber::instance().finishLoading(view, success);
      tabStrip_->setTabLoading(idx, false);

      applyTabFavicon(idx, view, cachedLoadIcon, false);
      if (services_.profile && !isNewTabUrl(view->url()) && !tabs_[idx].isInternal) {
        const QPointer<BrowserWindow> guardedWindow(this);
        const QPointer<QWebEngineView> guardedView(view);
        const QUrl pageUrl = view->url();
        services_.profile->requestIconForPageURL(pageUrl, 32, [guardedWindow, tabId, guardedView, pageUrl](const QIcon &ico, const QUrl &, const QUrl &) {
          if (!guardedWindow || !guardedView || guardedView->url() != pageUrl) return;
          const int curIdx = guardedWindow->findIndexByTabId(tabId);
          if (curIdx < 0) return;
          if (!ico.isNull()) {
            guardedWindow->applyTabFavicon(curIdx, guardedView, ico, false);
          } else if (!guardedWindow->tabs_[curIdx].pageIconReported) {
            // QtWebEngine reports no icon change for a page that has none
            // when the view never had one (a fresh tab). Its favicon
            // database having none for the loaded page is that answer: show
            // the generic site icon, not the site's cached favicon (as WebKit
            // does from its commit). A favicon arriving later replaces it.
            guardedWindow->applyTabFavicon(curIdx, guardedView, QIcon(), true);
          }
        });
      }
    }

    if (success) {
      if (isNewTabUrl(view->url())) {
        const QString capability = QUuid::createUuid().toString(QUuid::WithoutBraces);
        view->page()->setProperty("dalinira-suggest-capability", capability);
        const QString json = QString::fromUtf8(QJsonDocument(QJsonArray{capability}).toJson(QJsonDocument::Compact));
        view->page()->runJavaScript(QStringLiteral(
            "window.navigationCapability=%1[0];"
            "if(window.daliniraSuggestionBridge)window.daliniraSuggestionBridge(%1[0]);").arg(json));
      }
      syncNewTabViews();
      updateSearchEngineIcon();
      if (autofillController_) {
        autofillController_->onPageLoadFinished(view, success);
      }
      if (idx >= 0 && idx < tabs_.size() && !tabs_[idx].isInternal && !isNewTabUrl(view->url())) {
        const QString scheme = view->url().scheme().toLower();
        if (scheme == QLatin1String("http") || scheme == QLatin1String("https")) {
          if (services_.profileService && services_.profileService->translateService() &&
              services_.profileService->translateService()->isEnabled()) {
            auto *translator = getOrCreatePageTranslator(idx);
            if (translator) {
              translator->detectLanguage();
            }
          }
        }
      }
    }
    // Always reset the button to Reload after load finishes (success or failure)
    if (findIndexByTabId(tabId) == tabStrip_->currentIndex()) {
      updateReloadStopButton(false);
    }
  });
}

void BrowserWindow::onEngineFaviconLoaded(const QUrl &pageUrl, const QIcon &icon) {
  if (services_.profileService && !icon.isNull()) {
    cacheNewTabFavicon(services_.profileService, pageUrl, icon);
    syncNewTabViews();
    updateBookmarkIconsForUrl(pageUrl, icon);
  }
}

void BrowserWindow::syncNewTabViews() {
  if (!services_.profileService) return;
  const auto makeTopSitesScript = [&](const QString &engine) {
    return newTabTopSitesUpdateScript(collectNewTabFrequentSites(services_.profileService, 6, engine),
                                      collectNewTabBookmarks(services_.profileService, 6, engine));
  };
  const QString chromiumKey = dalinira::engine::engineKey(dalinira::engine::EngineType::Chromium);
  const QString webkitKey = dalinira::engine::engineKey(dalinira::engine::EngineType::WebKit);
  const QString chromiumScript = makeTopSitesScript(chromiumKey);
  // An engine view's New Tab shows its own engine's workspace data, by engine
  // key; another engine's is made when one of its pages needs it.
  QHash<QString, QString> engineViewTopSitesScripts{{webkitKey, makeTopSitesScript(webkitKey)}};
  const QString protectionScript = newTabProtectionStatsUpdateScript(
      services_.profileService->totalBlockedCount(),
      services_.profileService->recentDownloadCount(),
      services_.profileService->sessionBlockedCount());

  const bool legacyCards = dalinira::ui_profile::getVisualValue(QStringLiteral("browser/cards"), false).toBool();
  const bool showDownloads = dalinira::ui_profile::getVisualValue(QStringLiteral("browser/showDownloadsCard"), legacyCards).toBool();
  const bool showBlocked = dalinira::ui_profile::getVisualValue(QStringLiteral("browser/showBlockedCard"), legacyCards).toBool();
  const QString counterMode = dalinira::ui_profile::getVisualValue(
      QStringLiteral("browser/blockedCounterMode"), QStringLiteral("session")).toString();
  const QString cardSettingsScript = newTabCardSettingsUpdateScript(showDownloads, showBlocked, counterMode);

  const auto makeAppearanceScript = [&](const QString &engine) {
    QJsonArray customShortcuts;
    for (const auto &value : dalinira::newtab::shortcuts::load()) {
      auto entry = value.toObject();
      requestNewTabShortcutFavicon(services_.profileService, QUrl(entry.value(QStringLiteral("url")).toString()), this, [this] { syncNewTabViews(); });
      entry.insert(QStringLiteral("icon"), newTabFaviconUrl(services_.profileService, QUrl(entry.value(QStringLiteral("url")).toString())));
      customShortcuts.append(entry);
    }
    QJsonObject appearanceConfig{
        {QStringLiteral("topSitesSource"), dalinira::newtab::shortcuts::source()},
        {QStringLiteral("customShortcuts"), customShortcuts},
        {QStringLiteral("visible"), dalinira::ui_profile::getVisualValue(QStringLiteral("browser/showFrequentSites"), true).toBool()},
        {QStringLiteral("panelOpacity"), dalinira::ui_profile::getVisualValue(QStringLiteral("browser/frequentSitesPanelOpacity"), 72).toInt()},
        {QStringLiteral("iconOpacity"), dalinira::ui_profile::getVisualValue(QStringLiteral("browser/frequentSitesIconOpacity"), 82).toInt()},
        {QStringLiteral("hiddenSites"), QJsonArray::fromStringList(services_.profileService->hiddenFrequentSites(engine))},
        {QStringLiteral("showDownloadsCard"), showDownloads},
        {QStringLiteral("showBlockedCard"), showBlocked},
        {QStringLiteral("blockedCounterMode"), counterMode},
    };
    if (!dalinira::ui_profile::getVisualValue(QStringLiteral("browser/topSitesSource")).isValid())
      appearanceConfig.remove(QStringLiteral("topSitesSource"));
    if (dalinira::ui_profile::getVisualValue(
            QStringLiteral("browser/backgroundPreferenceSet"), false).toBool()) {
      appearanceConfig.insert(QStringLiteral("backgroundVisible"), dalinira::ui_profile::getVisualValue(
          QStringLiteral("browser/backgroundVisible"), false).toBool());
      appearanceConfig.insert(QStringLiteral("backgroundSource"), dalinira::ui_profile::getVisualValue(
          QStringLiteral("browser/backgroundSource"), QStringLiteral("builtin")).toString());
    }
    const QString appearanceJson = QString::fromUtf8(QJsonDocument(appearanceConfig).toJson(QJsonDocument::Compact));
    return QStringLiteral(
        "window.daliniraFrequentSiteConfig=%1;"
        "if(typeof window.applyFrequentConfig==='function')window.applyFrequentConfig();").arg(appearanceJson);
  };
  const QString chromiumAppearanceScript = makeAppearanceScript(chromiumKey);
  QHash<QString, QString> engineViewAppearanceScripts{{webkitKey, makeAppearanceScript(webkitKey)}};

  auto *backgroundStore = services_.profileService->newTabBackgroundStore();
  const bool backgroundAvailable = backgroundStore && backgroundStore->hasValidManagedImage();
  const QFileInfo backgroundInfo(backgroundStore ? backgroundStore->managedImagePath() : QString{});
  const quint64 backgroundRevision = backgroundAvailable
      ? static_cast<quint64>(backgroundInfo.lastModified().toMSecsSinceEpoch())
          ^ static_cast<quint64>(backgroundInfo.size())
      : 0;
  const QString backgroundScript = newTabBackgroundStateUpdateScript(backgroundAvailable, backgroundRevision);

  for (const BrowserTabInfo &tab : std::as_const(tabs_)) {
    if (tab.view && isNewTabUrl(tab.view->url())) {
      tab.view->page()->runJavaScript(chromiumScript);
      tab.view->page()->runJavaScript(protectionScript);
      tab.view->page()->runJavaScript(cardSettingsScript);
      tab.view->page()->runJavaScript(chromiumAppearanceScript);
      tab.view->page()->runJavaScript(backgroundScript);
      const bool enabled = services_.profileService->searchSuggestions()->isEnabled() && !tab.view->page()->profile()->isOffTheRecord();
      tab.view->page()->runJavaScript(QStringLiteral("if(window.daliniraSuggestionConsent)window.daliniraSuggestionConsent(%1,%2);")
          .arg(enabled ? QStringLiteral("true") : QStringLiteral("false"), tab.view->page()->profile()->isOffTheRecord() ? QStringLiteral("false") : QStringLiteral("true")));
    } else if (tab.engineView && isNewTabUrl(tab.engineView->url())) {
      const QString engine = dalinira::engine::engineKey(tab.engineView->engineType());
      if (!engineViewTopSitesScripts.contains(engine)) {
        engineViewTopSitesScripts.insert(engine, makeTopSitesScript(engine));
        engineViewAppearanceScripts.insert(engine, makeAppearanceScript(engine));
      }
      tab.engineView->runJavaScript(engineViewTopSitesScripts.value(engine));
      tab.engineView->runJavaScript(protectionScript);
      tab.engineView->runJavaScript(cardSettingsScript);
      tab.engineView->runJavaScript(engineViewAppearanceScripts.value(engine));
      tab.engineView->runJavaScript(backgroundScript);
      const bool enabled = services_.profileService->searchSuggestions()->isEnabled() && !isIncognito();
      tab.engineView->runJavaScript(QStringLiteral(
          "if(window.daliniraSuggestionConsent)window.daliniraSuggestionConsent(%1,%2);")
          .arg(enabled ? QStringLiteral("true") : QStringLiteral("false"),
               isIncognito() ? QStringLiteral("false") : QStringLiteral("true")));
    }
  }
  scheduleSidePanelBackdrop();
}

void BrowserWindow::handleFrequentSitesCommand(const QString &op, const QUrl &url, const QString &engine, const QString &name, const QString &original) {
  if (!services_.profileService || isIncognito()) return;
  if (op == QLatin1String("add-shortcut")) {
    if (!dalinira::newtab::shortcuts::add(name, url.toString())) return;
    syncNewTabViews();
  } else if (op == QLatin1String("edit-shortcut")) {
    if (!dalinira::newtab::shortcuts::edit(original, name, url.toString())) return;
    syncNewTabViews();
  } else if (op == QLatin1String("remove-shortcut")) {
    dalinira::newtab::shortcuts::remove(url);
    syncNewTabViews();
  } else if (op == QLatin1String("set-source")) {
    if (!QStringList{QStringLiteral("frequent"), QStringLiteral("bookmarks"), QStringLiteral("custom")}.contains(name)) return;
    dalinira::ui_profile::setVisualValue(QStringLiteral("browser/topSitesSource"), name);
    syncNewTabViews();
  } else if (op == QLatin1String("set-visible")) {
    if (name != QLatin1String("1") && name != QLatin1String("0")) return;
    dalinira::ui_profile::setVisualValue(QStringLiteral("browser/showFrequentSites"), name == QLatin1String("1"));
    syncNewTabViews();
  } else if (op == QLatin1String("hide")) {
    services_.profileService->hideFrequentSite(url, engine);
    syncNewTabViews();
  } else if (op == QLatin1String("restore")) {
    services_.profileService->restoreHiddenFrequentSites(engine);
    syncNewTabViews();
  } else if (op == QLatin1String("remove-bookmark") || op == QLatin1String("remove-quick-access")) {
    services_.profileService->removeQuickAccess(url, engine);
    syncNewTabViews();
  }
}

void BrowserWindow::handleEngineInternalNavigation(dalinira::engine::EngineView *sourceView,
                                                   const QUrl &url) {
  if (!sourceView || url.scheme() != QLatin1String("dalinira")) return;

  const QString host = url.host().toLower();
  if (host == QLatin1String("audio-effects")) { showAudioEffects(); return; }
  if (host == QLatin1String("eq-presets")) { showEqPresetBrowser(); return; }
  if (host == QLatin1String("downloads")) { showMediaDownloads(); return; }
  if (host == QLatin1String("settings")) { showSettings(); return; }
  if (host == QLatin1String("passwords")) { showPasswords(); return; }

  if (currentEngineView() != sourceView || !isNewTabUrl(sourceView->url())) return;

  const QUrlQuery query(url);
  if (host == QLatin1String("search-engine")) {
    const QString engine = query.queryItemValue(QStringLiteral("engine"));
    if (QStringList{QStringLiteral("Google"), QStringLiteral("DuckDuckGo"),
                    QStringLiteral("Startpage"), QStringLiteral("Mojeek")}.contains(engine))
      setSearchEngine(engine);
    return;
  }
  if (host == QLatin1String("card-settings")) {
    const bool downloads = query.queryItemValue(QStringLiteral("downloads")) == QLatin1String("1");
    const bool blocked = query.queryItemValue(QStringLiteral("blocked")) == QLatin1String("1");
    QString mode = query.queryItemValue(QStringLiteral("mode"));
    if (mode != QLatin1String("session") && mode != QLatin1String("all_time"))
      mode = QStringLiteral("all_time");
    dalinira::ui_profile::setVisualValue(QStringLiteral("browser/showDownloadsCard"), downloads);
    dalinira::ui_profile::setVisualValue(QStringLiteral("browser/showBlockedCard"), blocked);
    dalinira::ui_profile::setVisualValue(QStringLiteral("browser/blockedCounterMode"), mode);
    dalinira::ui_profile::setVisualValue(QStringLiteral("browser/cards"), downloads || blocked);
    syncNewTabViews();
    return;
  }
  if (host == QLatin1String("suggest")) {
    handleEngineSuggestCommand(sourceView, url);
    return;
  }
  if (host == QLatin1String("frequent-sites")) {
    const QString op = query.queryItemValue(QStringLiteral("op"));
    const QString targetUrlStr = query.queryItemValue(QStringLiteral("url"), QUrl::FullyDecoded);
    handleFrequentSitesCommand(op, QUrl(targetUrlStr), QStringLiteral("webkit"),
        query.queryItemValue(QStringLiteral("name"), QUrl::FullyDecoded),
        query.queryItemValue(QStringLiteral("original"), QUrl::FullyDecoded));
    return;
  }
  if (host == QLatin1String("background-preference")) {
    const QString source = query.queryItemValue(QStringLiteral("source"));
    static const QStringList allowedSources{
        QStringLiteral("builtin"), QStringLiteral("custom"), QStringLiteral("solid-indigo"),
        QStringLiteral("solid-magenta"), QStringLiteral("solid-teal"), QStringLiteral("solid-ember"),
        QStringLiteral("gradient-violet"), QStringLiteral("gradient-aurora"),
        QStringLiteral("gradient-ocean"), QStringLiteral("gradient-sunset")};
    if (!allowedSources.contains(source)) return;
    dalinira::ui_profile::setVisualValue(QStringLiteral("browser/backgroundSource"), source);
    dalinira::ui_profile::setVisualValue(QStringLiteral("browser/backgroundVisible"),
                                         query.queryItemValue(QStringLiteral("visible")) == QLatin1String("1"));
    dalinira::ui_profile::setVisualValue(QStringLiteral("browser/backgroundPreferenceSet"), true);
    syncNewTabViews();
    return;
  }
  if (host == QLatin1String("newtab-background")) {
    const QString command = query.queryItemValue(QStringLiteral("op"));
    if (command == QLatin1String("pick") || command == QLatin1String("remove"))
      handleEngineNewTabBackgroundCommand(sourceView, command);
  }
}

void BrowserWindow::handleEngineNewTabBackgroundCommand(dalinira::engine::EngineView *sourceView,
                                                        const QString &command) {
  if (!sourceView || currentEngineView() != sourceView || !isNewTabUrl(sourceView->url()) ||
      isIncognito() || !services_.profileService) return;
  auto *store = services_.profileService->newTabBackgroundStore();
  if (!store) return;

  bool ok = false;
  bool selectCustom = false;
  QString message;
  if (command == QLatin1String("pick")) {
    const QString selectedPath = QFileDialog::getOpenFileName(
        this, I18n::text(QStringLiteral("newtab.choose_background")),
        QStandardPaths::writableLocation(QStandardPaths::PicturesLocation),
        I18n::text(QStringLiteral("newtab.image_filter")));
    if (selectedPath.isEmpty()) {
      sourceView->runJavaScript(QStringLiteral(
          "if(window.daliniraBackgroundCancelled)window.daliniraBackgroundCancelled();"));
      return;
    }
    const auto result = store->importImage(selectedPath);
    ok = result.ok();
    selectCustom = ok;
    message = ok ? I18n::text(QStringLiteral("newtab.background_applied")) : result.message;
  } else if (command == QLatin1String("remove")) {
    ok = store->removeManagedImage();
    message = ok ? I18n::text(QStringLiteral("newtab.background_removed")) : I18n::text(QStringLiteral("newtab.background_remove_failed"));
  } else {
    return;
  }

  const bool available = store->hasValidManagedImage();
  const QFileInfo info(store->managedImagePath());
  const quint64 revision = available
      ? static_cast<quint64>(info.lastModified().toMSecsSinceEpoch()) ^ static_cast<quint64>(info.size())
      : 0;
  const QString resultScript = newTabBackgroundResultScript(ok, message, available, revision, selectCustom);

  if (ok) {
    dalinira::ui_profile::setVisualValue(QStringLiteral("browser/backgroundSource"),
                                         selectCustom ? QStringLiteral("custom") : QStringLiteral("builtin"));
    dalinira::ui_profile::setVisualValue(QStringLiteral("browser/backgroundVisible"), true);
    dalinira::ui_profile::setVisualValue(QStringLiteral("browser/backgroundPreferenceSet"), true);
  }

  for (const BrowserTabInfo &tab : std::as_const(tabs_)) {
    if (!tab.engineView || !isNewTabUrl(tab.engineView->url())) continue;
#if defined(DALINIRA_BUILD_WEBKIT)
    if (auto *webkitView = qobject_cast<dalinira::engine::WebKitEngineView *>(tab.engineView.data()))
      webkitView->setManagedBackgroundPaths(store->managedImagePath(), store->thumbnailPath());
#endif
    // The result script changes the CSS URL with a new revision, so WebKit can
    // paint the imported image immediately. Reloading here races localStorage
    // persistence and restores the previous background on slower web processes.
    tab.engineView->runJavaScript(resultScript);
  }
}

void BrowserWindow::handleNewTabBackgroundCommand(QWebEnginePage *sourcePage,
                                                   const QString &command,
                                                   const QString &capability) {
  auto *sourceView = sourcePage ? qobject_cast<QWebEngineView *>(sourcePage->parent()) : nullptr;
  if (!sourcePage || !sourceView || currentView() != sourceView ||
      !isNewTabUrl(sourcePage->url()) || sourcePage->profile() != services_.profile ||
      sourcePage->profile()->isOffTheRecord() || capability.isEmpty() ||
      sourcePage->property("dalinira-suggest-capability").toString() != capability ||
      !services_.profileService) return;

  auto *store = services_.profileService->newTabBackgroundStore();
  if (!store) return;
  bool ok = false;
  bool selectCustom = false;
  QString message;

  if (command == QLatin1String("pick")) {
    const QString selectedPath = QFileDialog::getOpenFileName(
        this, I18n::text(QStringLiteral("newtab.choose_background")),
        QStandardPaths::writableLocation(QStandardPaths::PicturesLocation),
        I18n::text(QStringLiteral("newtab.image_filter")));
    if (selectedPath.isEmpty()) {
      if (sourcePage && isNewTabUrl(sourcePage->url()))
        sourcePage->runJavaScript(QStringLiteral("if(window.daliniraBackgroundCancelled)window.daliniraBackgroundCancelled();"));
      return;
    }
    const auto result = store->importImage(selectedPath);
    ok = result.ok();
    selectCustom = ok;
    message = ok ? I18n::text(QStringLiteral("newtab.background_applied")) : result.message;
  } else if (command == QLatin1String("remove")) {
    ok = store->removeManagedImage();
    message = ok ? I18n::text(QStringLiteral("newtab.background_removed")) : I18n::text(QStringLiteral("newtab.background_remove_failed"));
  } else {
    return;
  }

  const bool available = store->hasValidManagedImage();
  const QFileInfo backgroundInfo(store->managedImagePath());
  const quint64 revision = available
      ? static_cast<quint64>(backgroundInfo.lastModified().toMSecsSinceEpoch())
          ^ static_cast<quint64>(backgroundInfo.size())
      : 0;
  const QString resultScript = newTabBackgroundResultScript(ok, message, available, revision, selectCustom);

  if (ok) {
    dalinira::ui_profile::setVisualValue(QStringLiteral("browser/backgroundSource"),
                                         selectCustom ? QStringLiteral("custom") : QStringLiteral("builtin"));
    dalinira::ui_profile::setVisualValue(QStringLiteral("browser/backgroundVisible"), true);
    dalinira::ui_profile::setVisualValue(QStringLiteral("browser/backgroundPreferenceSet"), true);
  }

  for (const auto &registered : dalinira::desktop_tabs::TabWindowRegistry::instance().registeredWindows()) {
    auto *window = registered.window ? qobject_cast<BrowserWindow *>(registered.window.data()) : nullptr;
    if (!window || window->services().profileService != services_.profileService) continue;
    for (const BrowserTabInfo &tab : window->allTabs()) {
      if (tab.view && isNewTabUrl(tab.view->url())) tab.view->page()->runJavaScript(resultScript);
    }
  }
}

void BrowserWindow::onOmniboxReturnPressed() {
  if (suggestionActivated_) return;
  const auto index = suggestionCompleter_->popup()->currentIndex();
  if (suggestionCompleter_->popup()->isVisible() && index.isValid()) {
    activateSuggestion(index.data(Qt::UserRole + 1).toUrl(), index.data(Qt::DisplayRole).toString(),
                       index.data(Qt::UserRole + 2).toString());
    return;
  }
  navigateFromUserInput(omnibox_->text());
}

void BrowserWindow::navigateFromUserInput(const QString &rawInput, const QString &searchEngine) {
  const QString input = rawInput.trimmed();
  if (input.isEmpty()) return;
  // The edit is committed: let the resulting navigation update the omnibox.
  if (omnibox_) omnibox_->setModified(false);

  // Prevent recursion or loop if dalinira://navigate is passed
  if (input.startsWith(QStringLiteral("dalinira://navigate"), Qt::CaseInsensitive)) {
    return;
  }

  // Check for internal dalinira:// schemes
  if (input.startsWith(QStringLiteral("dalinira://"), Qt::CaseInsensitive)) {
    const QUrl internalUrl(input);
    const QString host = internalUrl.host().toLower();
    if (host == QLatin1String("welcome") || host == QLatin1String("onboarding")) { showOnboarding(); return; }
    if (host == QLatin1String("settings")) { showSettings(); return; }
    if (host == QLatin1String("passwords")) { showPasswords(); return; }
    if (host == QLatin1String("audio-effects")) { showAudioEffects(); return; }
    if (host == QLatin1String("eq-presets")) { showEqPresetBrowser(); return; }
    if (host == QLatin1String("blocker")) { showDaliNiraBlockerSettings(); return; }
    if (host == QLatin1String("downloads")) { showMediaDownloads(); return; }
    if (host == QLatin1String("listen") || host == QLatin1String("pulse")) { showSongFinder(); return; }
    if (host == QLatin1String("listen-settings")) { showSongFinderSettings(); return; }
    if (host == QLatin1String("newtab") || host == QLatin1String("incognito")) {
      const QUrl newTabUrl(isIncognito() ? QStringLiteral("dalinira://incognito/") : QStringLiteral("dalinira://newtab/"));
      if (auto *view = currentView()) {
        const int idx = tabStrip_ ? tabStrip_->currentIndex() : -1;
        if (idx >= 0 && idx < tabs_.size() && tabs_[idx].view == view) {
          tabs_[idx].url = newTabUrl;
          updateBookmarkBarVisibility();
        }
        view->load(newTabUrl);
      } else {
        addNewTab(newTabUrl);
      }
      return;
    }
  }

  const QString engine = searchEngine.isEmpty() ? currentSearchEngine() : searchEngine;
  auto resolution = dalinira::core::AddressInputResolver::resolve(
      input, engine, QLocale::system(), candidateProvider_.get());
  if (resolution.classification == dalinira::core::AddressInputClassification::Search
      && services_.profileService) {
    const QUrl customUrl = services_.profileService->searchUrlForEngine(
        engine, resolution.searchQuery.isEmpty() ? input : resolution.searchQuery);
    if (customUrl.isValid()) resolution.url = customUrl;
  }
  QUrl url = resolution.url;
  if (!url.isValid() || url.isEmpty()) return;

  const bool adultProtection = services_.profileService
      ? services_.profileService->isAdultContentProtectionEnabled() : true;
  if (adultProtection && dalinira::core::AdultContentProtectionService::instance().isBlocked(url)) {
    url = QUrl(QStringLiteral("dalinira://blocked/adult"));
  }

  if (resolution.classification == dalinira::core::AddressInputClassification::Search &&
      usesPersistentProfile()) {
    services_.profileService->recordSearch(resolution.searchQuery.isEmpty() ? input : resolution.searchQuery);
  }

  navigateCurrentTab(url, resolution.classification != dalinira::core::AddressInputClassification::Search);
}

void BrowserWindow::navigateCurrentTab(const QUrl &url, bool typed) {
  if (!url.isValid() || url.isEmpty()) return;
  if (omnibox_) omnibox_->setModified(false);
  const int idx = tabStrip_ ? tabStrip_->currentIndex() : -1;
  const auto markCurrentTab = [&] {
    tabs_[idx].expectedTypedUrl = typed ? url : QUrl{};
    tabs_[idx].url = url;
    updateBookmarkBarVisibility();
  };
  if (auto *ev = currentEngineView()) {
    if (idx >= 0 && idx < tabs_.size() && tabs_[idx].engineView == ev) markCurrentTab();
    ev->load(url);
    return;
  }
  if (auto *view = currentView()) {
    if (idx >= 0 && idx < tabs_.size() && tabs_[idx].view == view) markCurrentTab();
    prepareAdBlockScripts(view->page(), url);
    view->load(url);
    return;
  }
  // The current tab is an internal Qt page (Settings, ...) that cannot show web content.
  const int newIdx = addNewTab(url);
  if (newIdx >= 0 && newIdx < tabs_.size() && !tabs_[newIdx].isInternal)
    tabs_[newIdx].expectedTypedUrl = typed ? url : QUrl{};
}

void BrowserWindow::openBookmarkUrl(const QUrl &url, bool newTab) {
  if (!url.isValid() || url.isEmpty()) return;
  if (newTab) addNewTab(url);
  else navigateCurrentTab(url);
}

void BrowserWindow::activateSearchSuggestion(const QString &query) {
  const QString text = query.simplified().left(256);
  if (text.isEmpty()) return;
  const QString engine = currentSearchEngine();
  // A search suggestion is already the final query: build the engine's search
  // URL exactly once. Never re-resolve it as typed input (e.g. "github" must
  // search, not open a well-known site).
  const QUrl url = services_.profileService
      ? services_.profileService->searchUrlForEngine(engine, text)
      : dalinira::core::AddressInputResolver::searchUrlForEngine(engine, text);
  if (!url.isValid()) return;
  if (usesPersistentProfile()) services_.profileService->recordSearch(text);
  navigateCurrentTab(url);
}

void BrowserWindow::onBackClicked() {
  if (auto *ev = currentEngineView()) {
    ev->back();
    return;
  }
  if (auto *view = currentView()) {
    if (view->history() && view->history()->canGoBack()) {
      const QUrl backUrl = view->history()->backItem().url();
      const int idx = tabStrip_ ? tabStrip_->currentIndex() : -1;
      if (idx >= 0 && idx < tabs_.size() && tabs_[idx].view == view) {
        tabs_[idx].url = backUrl;
        updateBookmarkBarVisibility();
      }
    }
    view->back();
  }
}

void BrowserWindow::onForwardClicked() {
  if (auto *ev = currentEngineView()) {
    ev->forward();
    return;
  }
  if (auto *view = currentView()) {
    if (view->history() && view->history()->canGoForward()) {
      const QUrl forwardUrl = view->history()->forwardItem().url();
      const int idx = tabStrip_ ? tabStrip_->currentIndex() : -1;
      if (idx >= 0 && idx < tabs_.size() && tabs_[idx].view == view) {
        tabs_[idx].url = forwardUrl;
        updateBookmarkBarVisibility();
      }
    }
    view->forward();
  }
}

void BrowserWindow::onReloadOrStopClicked() {
  if (auto *ev = currentEngineView()) {
    if (ev->isLoading()) {
      ev->stop();
    } else {
      ev->reload();
    }
    return;
  }
  if (auto *view = currentView()) {
    if (isCurrentTabLoading()) {
      view->stop();
    } else {
      view->reload();
    }
  }
}

void BrowserWindow::onHomeClicked() {
  const QUrl homeUrl(isIncognito() ? QStringLiteral("dalinira://incognito/") : QStringLiteral("dalinira://newtab/"));
  if (auto *ev = currentEngineView()) {
    const int idx = tabStrip_ ? tabStrip_->currentIndex() : -1;
    if (idx >= 0 && idx < tabs_.size() && tabs_[idx].engineView == ev) {
      tabs_[idx].url = homeUrl;
      updateBookmarkBarVisibility();
    }
    ev->load(homeUrl);
    return;
  }
  if (auto *view = currentView()) {
    const int idx = tabStrip_ ? tabStrip_->currentIndex() : -1;
    if (idx >= 0 && idx < tabs_.size() && tabs_[idx].view == view) {
      tabs_[idx].url = homeUrl;
      updateBookmarkBarVisibility();
    }
    view->load(homeUrl);
  } else {
    addNewTab(homeUrl);
  }
}

void BrowserWindow::updateNavButtons() {
  if (auto *ev = currentEngineView()) {
    backBtn_->setEnabled(ev->canGoBack());
    forwardBtn_->setEnabled(ev->canGoForward());
    const bool isMedia = MediaDownloadService::isSupportedMediaUrl(ev->url()) &&
                         !isNewTabUrl(ev->url()) &&
                         ev->url().scheme() != QLatin1String("dalinira");
    if (mediaDownload_) {
      mediaDownload_->setEnabled(true);
      mediaDownload_->setProperty("activeMedia", isMedia);
      mediaDownload_->style()->unpolish(mediaDownload_);
      mediaDownload_->style()->polish(mediaDownload_);
      updateDownloadToolbar();
    }
    updateBookmarkButtonState();
    updateBlockerControls();
    return;
  }
  if (auto *view = currentView()) {
    if (auto *hist = view->history()) {
      backBtn_->setEnabled(hist->canGoBack());
      forwardBtn_->setEnabled(hist->canGoForward());
    } else {
      backBtn_->setEnabled(false);
      forwardBtn_->setEnabled(false);
    }
    const bool isMedia = MediaDownloadService::isSupportedMediaUrl(view->url()) &&
                         !isNewTabUrl(view->url()) &&
                         view->url().scheme() != QLatin1String("dalinira");
    if (mediaDownload_) {
      mediaDownload_->setEnabled(true);
      mediaDownload_->setProperty("activeMedia", isMedia);
      mediaDownload_->style()->unpolish(mediaDownload_);
      mediaDownload_->style()->polish(mediaDownload_);
      updateDownloadToolbar();
    }
  } else {
    backBtn_->setEnabled(false);
    forwardBtn_->setEnabled(false);
    if (mediaDownload_) {
      mediaDownload_->setEnabled(true);
      mediaDownload_->setProperty("activeMedia", false);
      mediaDownload_->style()->unpolish(mediaDownload_);
      mediaDownload_->style()->polish(mediaDownload_);
      updateDownloadToolbar();
    }
  }
  updateBookmarkButtonState();
  updateBlockerControls();
}

bool BrowserWindow::isCurrentTabLoading() const {
  if (auto *ev = currentEngineView()) {
    return ev->isLoading();
  }
  if (auto *view = currentView()) {
    return TabThrobber::instance().isLoading(view);
  }
  return false;
}

void BrowserWindow::updateReloadStopButton(bool isLoading) {
  // Load state of the active tab: leaving New Tab collapses an auto-hiding
  // panel once that navigation has completed.
  updateSidePanelAutoHide();
  if (!reloadBtn_) return;
  if (isLoading) {
    reloadBtn_->setIcon(BrowserIcons::stopIcon());
    reloadBtn_->setToolTip(shortcutTooltip(QStringLiteral("toolbar.stop"), QStringLiteral("navigation.stop")));
  } else {
    reloadBtn_->setIcon(BrowserIcons::reloadIcon());
    reloadBtn_->setToolTip(shortcutTooltip(QStringLiteral("toolbar.reload"), QStringLiteral("navigation.reload")));
  }
}

void BrowserWindow::updateOmniboxForCurrentTab(bool force) {
  if (!force && omnibox_->hasFocus() && omnibox_->isModified()) return;
  const int idx = tabStrip_ ? tabStrip_->currentIndex() : -1;
  if (idx >= 0 && idx < tabs_.size() && tabs_[idx].internalId == QLatin1String("local-media-player")) {
    omnibox_->setText(QStringLiteral("dalinira://local-media-player"));
    return;
  }
  if (auto *ev = currentEngineView()) {
    const QString urlStr = ev->url().toString();
    if (!urlStr.isEmpty() && urlStr != QLatin1String("about:blank") && !isNewTabUrl(ev->url())) {
      omnibox_->setText(urlStr);
    } else {
      omnibox_->clear();
    }
    return;
  }
  if (auto *view = currentView()) {
    const QString urlStr = view->url().toString();
    if (!urlStr.isEmpty() && urlStr != QLatin1String("about:blank") && !isNewTabUrl(view->url())) {
      omnibox_->setText(urlStr);
    } else {
      omnibox_->clear();
    }
  }
}

// -----------------------------------------------------------------
// Feature Page Navigations
// -----------------------------------------------------------------
void BrowserWindow::updateDownloadToolbar() {
  if (!mediaDownload_ || !downloadUiModel_) return;
  mediaDownload_->setModelState(downloadUiModel_->activeCount(),
                                downloadUiModel_->aggregateProgress(),
                                downloadUiModel_->hasPaused(),
                                downloadUiModel_->hasErrors());
}

bool BrowserWindow::downloadAnimationsEnabled() const {
  const QSettings settings;
  if (settings.value(QStringLiteral("ui/reduceMotion"), false).toBool()) return false;
  return settings.value(QStringLiteral("ui/animationsEnabled"), true).toBool();
}

void BrowserWindow::showDownloadStartedAnimation() {
  if (!mediaDownload_ || !downloadPopup_) return;
  const auto finishAcknowledgement = [this] {
    if (!mediaDownload_ || !downloadPopup_) return;
    const bool wasAlreadyOpen = downloadPopup_->isVisible();
    const QUrl activeUrl = currentActiveWebUrl();
    const bool pageHasMedia = currentTabAudible();
    if (!activeUrl.isEmpty() && MediaPlatformRegistry::shouldAutoAnalyzeMedia(activeUrl, pageHasMedia)) {
      downloadPopup_->setSuggestedMedia(activeUrl, currentTabTitle());
    } else {
      downloadPopup_->setSuggestedMedia(QUrl{}, QString{});
    }
    mediaDownload_->acknowledge(downloadAnimationsEnabled());
    downloadPopup_->showAnchored(mediaDownload_, !wasAlreadyOpen);
  };
  if (!downloadAnimationsEnabled() || !pageStack_) {
    finishAcknowledgement();
    return;
  }

  auto *ghost = new QLabel(this);
  ghost->setAttribute(Qt::WA_TransparentForMouseEvents);
  ghost->setAlignment(Qt::AlignCenter);
  ghost->setPixmap(BrowserIcons::icon(BrowserIcon::Download).pixmap(20, 20));
  ghost->setStyleSheet(QStringLiteral(
      "background:rgba(31,45,58,225);border:1px solid #4fc3f7;border-radius:10px;"));
  ghost->resize(38, 38);
  const QPoint source = pageStack_->mapTo(this, pageStack_->rect().center()) - QPoint(19, 19);
  const QPoint target = mediaDownload_->mapTo(this, mediaDownload_->rect().center()) - QPoint(8, 8);
  ghost->setGeometry(QRect(source, QSize(38, 38)));
  ghost->show();
  ghost->raise();

  auto *opacity = new QGraphicsOpacityEffect(ghost);
  ghost->setGraphicsEffect(opacity);
  auto *group = new QParallelAnimationGroup(ghost);
  auto *geometry = new QPropertyAnimation(ghost, "geometry", group);
  geometry->setDuration(430);
  geometry->setStartValue(ghost->geometry());
  geometry->setEndValue(QRect(target, QSize(16, 16)));
  geometry->setEasingCurve(QEasingCurve::InOutCubic);
  auto *fade = new QPropertyAnimation(opacity, "opacity", group);
  fade->setDuration(430);
  fade->setStartValue(0.92);
  fade->setKeyValueAt(0.68, 0.82);
  fade->setEndValue(0.0);
  connect(group, &QParallelAnimationGroup::finished, this, [ghost, finishAcknowledgement] {
    ghost->deleteLater();
    finishAcknowledgement();
  });
  group->start();
}

void BrowserWindow::showOnboarding() {
  if (services_.tabManager) {
    const auto existingId = services_.tabManager->findInternal(this, QStringLiteral("onboarding"));
    if (!existingId.isNull()) {
      const auto *record = services_.tabManager->record(existingId);
      if (record && record->content) {
        const int idx = pageStack_->indexOf(record->content);
        if (idx >= 0) {
          switchTab(idx);
          return;
        }
      }
    }
  }

  class ProfileDataReceiverAdapter final : public QObject, public dalinira::onboarding::IBrowserDataReceiver {
   public:
    explicit ProfileDataReceiverAdapter(BrowserProfileService *svc, QObject *parent = nullptr)
        : QObject(parent), svc_(svc) {}

    bool addBookmark(const QUrl &url, const QString &title, const QString &folder) override {
      return svc_ ? svc_->addBookmark(url, title, folder) : false;
    }

    void createBookmarkFolder(const QString &folder) override {
      if (svc_) svc_->createBookmarkFolder(folder);
    }

    void recordHistory(const QUrl &url, const QString &title, bool isReload = false) override {
      if (svc_) svc_->recordHistory(url, title, isReload);
    }

   private:
    BrowserProfileService *svc_ = nullptr;
  };

  class ProfileSearchPrivacyAdapter final
      : public QObject,
        public dalinira::onboarding::ISearchPrivacyPreferences {
   public:
    explicit ProfileSearchPrivacyAdapter(BrowserProfileService *service, QObject *parent = nullptr)
        : QObject(parent), service_(service) {}

    QString searchEngine() const override {
      return service_ ? service_->searchEngine() : QStringLiteral("DuckDuckGo");
    }
    void setSearchEngine(const QString &engine) override {
      if (service_) service_->setSearchEngine(engine);
    }
    bool blockerEnabled() const override {
      return service_ && service_->blockerService()
          ? service_->blockerService()->settings()->protectionEnabled() : true;
    }
    void setBlockerEnabled(bool enabled) override {
      if (service_ && service_->blockerService())
        service_->blockerService()->settings()->setProtectionEnabled(enabled);
    }

   private:
    BrowserProfileService *service_ = nullptr;
  };

  auto *receiver = new ProfileDataReceiverAdapter(services_.profileService, this);
  auto *searchPrivacy = new ProfileSearchPrivacyAdapter(services_.profileService, this);
  auto *defaultBrowser = new dalinira::core::SystemDefaultBrowserIntegration(this);
  auto *onboardingView = new dalinira::onboarding::OnboardingView(
      receiver, searchPrivacy, this, defaultBrowser);
  connect(onboardingView, &dalinira::onboarding::OnboardingView::finished, this, [this] {
    finishOnboarding();
  });
  // The theme picked in the layout step restyles the browser chrome at once,
  // as the same choice in Settings does.
  connect(&dalinira::onboarding::OnboardingManager::instance(),
          &dalinira::onboarding::OnboardingManager::themeSelectionChanged,
          onboardingView, &BrowserWindow::applyAppearanceToAllWindows);

  addInternalTab(onboardingView,
                 I18n::text(QStringLiteral("onboarding.tab_title")),
                 BrowserIcons::appIcon(),
                 QStringLiteral("onboarding"));
}

void BrowserWindow::applyAppearanceToAllWindows(const QString &themeString) {
  const auto theme = dalinira::layout::appearanceThemeFromString(themeString);
  dalinira::layout::applyApplicationTheme(qobject_cast<QApplication *>(qApp), theme);
  for (QWidget *widget : QApplication::topLevelWidgets()) {
    if (auto *window = qobject_cast<BrowserWindow *>(widget)) {
      window->setupStyle();
      const auto settingsPages = window->findChildren<SettingsPage *>();
      for (SettingsPage *page : settingsPages) page->refreshAppearance();
    }
  }
}

void BrowserWindow::finishOnboarding() {
  int onboardingIndex = -1;
  for (int i = 0; i < tabs_.size(); ++i) {
    if (tabs_[i].isInternal && tabs_[i].internalId == QLatin1String("onboarding")) {
      onboardingIndex = i;
      break;
    }
  }

  const QString preferredEngine = dalinira::onboarding::OnboardingManager::instance().selectedEngine();
  const QString runningEngine = dalinira::engine::engineKey(currentWorkspaceEngine_);

  if (!preferredEngine.isEmpty() && preferredEngine != runningEngine) {
    dalinira::engine::EngineResolver::setPersistedEngine(preferredEngine);

    const QString program = QCoreApplication::applicationFilePath();
    const QStringList currentArgs = qApp->arguments().mid(1);
    QStringList args;
    for (int i = 0; i < currentArgs.size(); ++i) {
      if (currentArgs[i] == QLatin1String("--engine") ||
          currentArgs[i] == QLatin1String("--wait-for-pid")) {
        ++i;
        continue;
      }
      if (currentArgs[i] == QLatin1String("--onboarding")) continue;
      args.append(currentArgs[i]);
    }
    startDetachedBrowserQuietly(program, args);
    qApp->quit();
    return;
  }

  const QString preferredLayout = dalinira::onboarding::OnboardingManager::instance().selectedLayout();
  const auto layoutMode = dalinira::layout::layoutModeFromString(preferredLayout, dalinira::layout::BrowserLayoutMode::Classic);
  dalinira::layout::setPersistedLayoutMode(layoutMode);
  applyLayoutMode(layoutMode);

  addNewTab(QUrl(QStringLiteral("dalinira://newtab/")));

  if (onboardingIndex >= 0 && onboardingIndex < tabs_.size()) {
    closeTab(onboardingIndex);
  }
}

void BrowserWindow::applyLayoutMode(dalinira::layout::BrowserLayoutMode mode) {
  if (tabSearchBtn_) {
    tabSearchBtn_->setVisible(mode == dalinira::layout::BrowserLayoutMode::Classic);
  }
  if (layoutController_) {
    const bool anim = dalinira::onboarding::OnboardingManager::animationsEnabled();
    if (sidePanel_) sidePanel_->setPanelOnRight(mode == dalinira::layout::BrowserLayoutMode::VerticalTabsRight);
    layoutController_->setLayoutMode(mode, anim);
    dalinira::layout::setPersistedLayoutMode(mode);
    placeNavigationControls(mode != dalinira::layout::BrowserLayoutMode::Classic);
    updateSidePanelTabs();
  }
}

dalinira::layout::BrowserLayoutMode BrowserWindow::currentLayoutMode() const {
  return layoutController_ ? layoutController_->currentMode() : dalinira::layout::BrowserLayoutMode::Classic;
}

void BrowserWindow::updateSidePanelAutoHide() {
  // Shared tab/page state: the same for Chromium and WebKit tabs.
  if (layoutController_) layoutController_->setNewTabActive(isCurrentTabNewTab(), isCurrentTabLoading());
  scheduleSidePanelBackdrop();
}

void BrowserWindow::scheduleSidePanelBackdrop() {
  if (backdropQueryPending_) return;
  backdropQueryPending_ = true;
  QTimer::singleShot(0, this, [this] {
    backdropQueryPending_ = false;
    updateSidePanelBackdrop();
  });
}

// The docked side panel shows the New Tab page's own background through a
// tinted glass. The page reports what it paints (its computed CSS) through the
// same script on both engines; nothing is composited through the panel.
void BrowserWindow::updateSidePanelBackdrop() {
  if (!sidePanel_) return;
  const int index = tabStrip_ ? tabStrip_->currentIndex() : -1;
  const bool sideLayout = layoutController_ && layoutController_->currentMode() != dalinira::layout::BrowserLayoutMode::Classic;
  if (!sideLayout || index < 0 || index >= tabs_.size() || !isCurrentTabNewTab() || isCurrentTabLoading()) {
    sidePanel_->setNewTabBackdrop({});
    return;
  }
  const uint64_t tabId = tabs_[index].id;
  const QString script = QStringLiteral(
      "(()=>{if(!document.body)return '';const b=getComputedStyle(document.body);"
      "return JSON.stringify({image:b.backgroundImage,color:b.backgroundColor,"
      "overlay:getComputedStyle(document.body,'::before').backgroundColor})})()");
  const QPointer<BrowserWindow> self(this);
  const auto apply = [self, tabId](const QVariant &value) {
    if (!self || !self->sidePanel_) return;
    const int current = self->tabStrip_ ? self->tabStrip_->currentIndex() : -1;
    if (current < 0 || current >= self->tabs_.size() || self->tabs_[current].id != tabId || !self->isCurrentTabNewTab()) return;
    const QJsonObject css = QJsonDocument::fromJson(value.toString().toUtf8()).object();
    dalinira::layout::NewTabBackdrop backdrop;
    backdrop.valid = !css.isEmpty();
    backdrop.cssImage = css.value(QStringLiteral("image")).toString();
    static const QRegularExpression rgb(QStringLiteral(R"(rgba?\(\s*([\d.]+)[ ,]+([\d.]+)[ ,]+([\d.]+)(?:[ ,/]+([\d.]+))?\s*\))"));
    const auto toColor = [](const QString &text) {
      const auto match = rgb.match(text);
      if (!match.hasMatch()) return QColor(text);
      QColor color(qRound(match.captured(1).toDouble()), qRound(match.captured(2).toDouble()), qRound(match.captured(3).toDouble()));
      if (!match.captured(4).isEmpty()) color.setAlphaF(match.captured(4).toDouble());
      return color;
    };
    backdrop.baseColor = toColor(css.value(QStringLiteral("color")).toString());
    backdrop.overlay = toColor(css.value(QStringLiteral("overlay")).toString());
    // url(...) images: the built-in Flow picture or the user's own background.
    if (backdrop.cssImage.contains(QLatin1String("managed-background")) && self->services_.profileService &&
        self->services_.profileService->newTabBackgroundStore())
      backdrop.imagePath = self->services_.profileService->newTabBackgroundStore()->managedImagePath();
    else if (backdrop.cssImage.contains(QLatin1String("dalinira-flow-blue")))
      backdrop.imagePath = QStringLiteral(":/new-tab/dalinira-flow-blue.png");
    self->sidePanel_->setNewTabBackdrop(backdrop);
  };
  if (auto *engineView = tabs_[index].engineView.data()) engineView->runJavaScript(script, apply);
  else if (auto *view = tabs_[index].view.data(); view && view->page()) view->page()->runJavaScript(script, apply);
}

void BrowserWindow::applySidePanelAutoHideToAllWindows(bool enabled) {
  dalinira::layout::setSidePanelAutoHideEnabled(enabled);
  for (QWidget *widget : QApplication::topLevelWidgets()) {
    auto *window = qobject_cast<BrowserWindow *>(widget);
    if (!window || !window->layoutController_) continue;
    window->layoutController_->setAutoHideEnabled(enabled);
    // An open Settings page shows the same choice as the panel's pin toggle.
    if (auto *checkbox = window->findChild<QCheckBox *>(QStringLiteral("settings-side-panel-auto-hide"))) {
      const QSignalBlocker blocker(checkbox);
      checkbox->setChecked(enabled);
    }
  }
}

// Side-panel layouts keep the navigation controls in the panel's top bar (as
// Zen's sidebar top buttons), so they never show twice; the classic layout
// puts them back in the toolbar. The widgets themselves move: their state
// (enabled, reload/stop, add-site) and connections stay as they are.
void BrowserWindow::placeNavigationControls(bool inSidePanel) {
  auto *navLayout = navBar_ ? qobject_cast<QHBoxLayout *>(navBar_->layout()) : nullptr;
  if (!sidePanel_ || !navLayout || !backBtn_ || !forwardBtn_ || !reloadBtn_ || !bookmarkBtn_ || !mainMenuBtn_) return;
  const QList<QToolButton *> leading{backBtn_, forwardBtn_, reloadBtn_, bookmarkBtn_};
  if (inSidePanel == navigationInSidePanel_) return;
  navigationInSidePanel_ = inSidePanel;
  // Without the navigation buttons the address bar starts at the toolbar's
  // edge; the classic toolbar centres it between flexible spacers.
  for (int index = 0; index < navLayout->count(); ++index)
    if (navLayout->itemAt(index)->spacerItem()) navLayout->setStretch(index, inSidePanel ? 0 : 1);
  using Metrics = dalinira::ui::BrowserChromeMetrics;
  if (inSidePanel) {
    for (QToolButton *button : leading) navLayout->removeWidget(button);
    navLayout->removeWidget(mainMenuBtn_);
    for (QToolButton *button : leading) {
      button->setFixedSize(30, 30);
      button->setIconSize(QSize(18, 18));
    }
    bookmarkBtn_->setIconSize(QSize(20, 20));
    mainMenuBtn_->setFixedSize(30, 30);
    sidePanel_->setChromeControls({backBtn_, forwardBtn_, reloadBtn_, bookmarkBtn_}, {mainMenuBtn_});
    return;
  }
  sidePanel_->setChromeControls({}, {});
  for (QToolButton *button : {backBtn_, forwardBtn_, reloadBtn_, bookmarkBtn_, mainMenuBtn_}) {
    button->setParent(navBar_);
    button->setFixedSize(Metrics::navigationButtonSize, Metrics::navigationButtonSize);
    button->setIconSize(QSize(Metrics::navigationIconSize, Metrics::navigationIconSize));
  }
  bookmarkBtn_->setIconSize(QSize(24, 24));
  navLayout->insertWidget(0, backBtn_);
  navLayout->insertWidget(1, forwardBtn_);
  navLayout->insertWidget(2, reloadBtn_);
  navLayout->insertWidget(navLayout->indexOf(omnibox_), bookmarkBtn_);
  navLayout->addWidget(mainMenuBtn_);
  for (QToolButton *button : {backBtn_, forwardBtn_, reloadBtn_, bookmarkBtn_, mainMenuBtn_}) button->show();
}

void BrowserWindow::updateSidePanelTabs() {
  if (!sidePanel_) return;
  QVector<dalinira::layout::SidePanelTabInfo> sideTabs;
  sideTabs.reserve(tabs_.size());
  for (int i = 0; i < tabs_.size(); ++i) {
    const auto &t = tabs_[i];
    sideTabs.push_back({t.id, t.title, t.icon, t.isLoading, isTabSleeping(i)});
  }
  const int activeIdx = tabStrip_ ? tabStrip_->currentIndex() : -1;
  sidePanel_->setTabs(sideTabs, activeIdx);
}

void BrowserWindow::showSettings(SettingsPage::Category category) {
  if (services_.tabManager) {
    const auto existingId = services_.tabManager->findInternal(this, QStringLiteral("settings"));
    if (!existingId.isNull()) {
      const auto *record = services_.tabManager->record(existingId);
      if (record && record->content) {
        const int idx = pageStack_->indexOf(record->content);
        if (idx >= 0) {
          if (auto *page = qobject_cast<SettingsPage *>(record->content.data())) {
            page->refreshPreferences();
            page->setCategory(category);
          }
          switchTab(idx);
          return;
        }
      }
    }
  }

  SettingsPage::Hooks hooks;
  hooks.searchEngine = [this] { return currentSearchEngine(); };
  hooks.setSearchEngine = [this](const QString &engine) { setSearchEngine(engine); };
  hooks.syncNewTabs = [this] { syncNewTabViews(); };
  hooks.refreshBookmarks = [this] { renderBookmarks(); };
  hooks.refreshBookmarkBarVisibility = [this] { updateBookmarkBarVisibility(); };
  hooks.refreshTabStyle = [] {
    dalinira::desktop_tabs::TabWindowRegistry::instance().reloadTabAppearances();
  };
  hooks.setLayoutMode = [this](const QString &modeStr) {
    applyLayoutMode(dalinira::layout::layoutModeFromString(modeStr));
  };
  hooks.setTheme = &BrowserWindow::applyAppearanceToAllWindows;
  hooks.setSidePanelAutoHide = &BrowserWindow::applySidePanelAutoHideToAllWindows;
  hooks.reopenOnboarding = [this] {
    dalinira::onboarding::OnboardingManager::instance().reopenOnboarding();
    saveSessionNow();
    QSettings settings;
    if (!isMinimized()) {
      settings.setValue(QStringLiteral("browser/mainWindowGeometry"), saveGeometry());
      settings.sync();
    }

    // Setup runs as its own application phase. The replacement process waits
    // for this process to release the browser profile and WebKit/XEmbed
    // resources before it creates the standalone onboarding window.
    const QString program = QCoreApplication::applicationFilePath();
    const QString runningEngine = dalinira::engine::engineKey(currentWorkspaceEngine_);
    const QStringList arguments{
        QStringLiteral("--onboarding"),
        QStringLiteral("--engine"), runningEngine,
        QStringLiteral("--wait-for-pid"),
        QString::number(QCoreApplication::applicationPid())};
    if (startDetachedBrowserQuietly(program, arguments)) {
      qApp->quit();
      return;
    }

    qWarning() << "Could not restart DaliNira for standalone onboarding";
    QMessageBox::warning(this, I18n::text(QStringLiteral("onboarding.tab_title")),
                         I18n::text(QStringLiteral("onboarding.restart_failed")));
  };
  hooks.performanceManager = [this] { return services_.tabManager ? services_.tabManager->performanceManager() : nullptr; };

  auto *page = new SettingsPage(services_.profileService, std::move(hooks));
  page->setCategory(category);
  connect(page, &SettingsPage::navigateRequested, this, [this](const QUrl &url) {
    if (url == QUrl(QStringLiteral("dalinira://passwords"))) showPasswords();
    else addNewTab(url);
  });
  connect(page, &SettingsPage::emailRequested, this,
          [](const QUrl &url) { QDesktopServices::openUrl(url); });

  addInternalTab(page, I18n::text(QStringLiteral("menu.settings")), BrowserIcons::icon(BrowserIcon::Settings), QStringLiteral("settings"));
}

void BrowserWindow::showPasswords() {
  if (services_.tabManager) {
    const auto existingId = services_.tabManager->findInternal(this, QStringLiteral("passwords"));
    if (!existingId.isNull()) {
      const auto *record = services_.tabManager->record(existingId);
      if (record && record->content) {
        const int idx = pageStack_->indexOf(record->content);
        if (idx >= 0) {
          if (auto *passwords = qobject_cast<PasswordManagerPage *>(record->content.data())) {
            passwords->refresh();
          }
          switchTab(idx);
          return;
        }
      }
    }
  }
  if (!services_.profileService || !services_.profileService->credentialVault()) return;
  auto *page = new PasswordManagerPage(services_.profileService->credentialVault(),
                                       services_.profileService->profile());
  addInternalTab(page, I18n::text(QStringLiteral("toolbar.passwords")), BrowserIcons::icon(BrowserIcon::Password), QStringLiteral("passwords"));
}

void BrowserWindow::showAudioEffects() {
  if (services_.tabManager) {
    const auto existingId = services_.tabManager->findInternal(this, QStringLiteral("audio-effects"));
    if (!existingId.isNull()) {
      const auto *record = services_.tabManager->record(existingId);
      if (record && record->content) {
        const int idx = pageStack_->indexOf(record->content);
        if (idx >= 0) { switchTab(idx); return; }
      }
    }
  }
  auto *page = new AudioEffectsPage(services_.audioEffects);
  connect(page, &AudioEffectsPage::eqPresetBrowserRequested, this, &BrowserWindow::showEqPresetBrowser);
  addInternalTab(page, I18n::text(QStringLiteral("menu.audio_effects")), QIcon(QStringLiteral(":/side-widget-icons/sound-effects.svg")), QStringLiteral("audio-effects"));
}

void BrowserWindow::showEqPresetBrowser() {
  if (services_.tabManager) {
    const auto existingId = services_.tabManager->findInternal(this, QStringLiteral("eq-presets"));
    if (!existingId.isNull()) {
      const auto *record = services_.tabManager->record(existingId);
      if (record && record->content) {
        const int idx = pageStack_->indexOf(record->content);
        if (idx >= 0) { switchTab(idx); return; }
      }
    }
  }
  auto *page = new EqPresetPage(services_.audioEffects);
  addInternalTab(page, I18n::text(QStringLiteral("menu.eq_presets")), QIcon(QStringLiteral(":/side-widget-icons/eq-presets.svg")), QStringLiteral("eq-presets"));
}

void BrowserWindow::showDaliNiraBlockerSettings(DaliNiraBlockerPage::Tab tab) {
  if (!services_.profileService || !services_.profileService->adBlockService()) return;
  if (services_.tabManager) {
    const auto existingId = services_.tabManager->findInternal(this, QStringLiteral("blocker"));
    if (!existingId.isNull()) {
      const auto *record = services_.tabManager->record(existingId);
      if (record && record->content) {
        const int idx = pageStack_->indexOf(record->content);
        if (idx >= 0) {
          if (auto *page = qobject_cast<DaliNiraBlockerPage *>(record->content.data())) page->setActiveTab(tab);
          switchTab(idx);
          return;
        }
      }
    }
  }
  auto *page = new DaliNiraBlockerPage(services_.profileService->adBlockService());
  page->setActiveTab(tab);
  addInternalTab(page, QStringLiteral("DaliNira Blocker"), QIcon(QStringLiteral(":/side-widget-icons/deliblock.svg")), QStringLiteral("blocker"));
}

void BrowserWindow::showSongFinder() {
  if (services_.tabManager) {
    const auto existingId = services_.tabManager->findInternal(this, QStringLiteral("song-finder"));
    if (!existingId.isNull()) {
      const auto *record = services_.tabManager->record(existingId);
      if (record && record->content) {
        const int idx = pageStack_->indexOf(record->content);
        if (idx >= 0) { switchTab(idx); return; }
      }
    }
  }
  auto *page = new SongFinderPage(services_.songRecognition);
  connect(page, &SongFinderPage::openPreferencesRequested, this, &BrowserWindow::showSongFinderSettings);
  connect(page, &SongFinderPage::openUrlRequested, this, [this](const QUrl &url) { addNewTab(url); });
  addInternalTab(page, QStringLiteral("DaliNira Pulse"), QIcon(QStringLiteral(":/side-widget-icons/pulse.svg")), QStringLiteral("song-finder"));
}

void BrowserWindow::showSongFinderSettings() {
  if (services_.tabManager) {
    const auto existingId = services_.tabManager->findInternal(this, QStringLiteral("song-finder-settings"));
    if (!existingId.isNull()) {
      const auto *record = services_.tabManager->record(existingId);
      if (record && record->content) {
        const int idx = pageStack_->indexOf(record->content);
        if (idx >= 0) { switchTab(idx); return; }
      }
    }
  }
  auto *page = new SongFinderSettingsPage(services_.songFinderSettings);
  connect(page, &SongFinderSettingsPage::closeTabRequested, this, [this] { showSongFinder(); });
  addInternalTab(page, I18n::text(QStringLiteral("pulse.settings_tab_title")), BrowserIcons::icon(BrowserIcon::Settings), QStringLiteral("song-finder-settings"));
}

void BrowserWindow::showMediaDownloads(const QUrl &sourceUrl, bool analyzeImmediately) {
  if (!services_.mediaDownload) return;

  QUrl targetUrl = sourceUrl;
  bool shouldAnalyze = analyzeImmediately;

  if (targetUrl.isEmpty()) {
    const QUrl activeUrl = currentActiveWebUrl();
    const bool pageHasMedia = currentTabAudible();
    if (!activeUrl.isEmpty() && MediaPlatformRegistry::shouldAutoAnalyzeMedia(activeUrl, pageHasMedia)
        && !MediaPlatformRegistry::isGenericPlatformFeedUrl(activeUrl)) {
      targetUrl = activeUrl;
      shouldAnalyze = true;
    }
  } else {
    shouldAnalyze = analyzeImmediately || (MediaPlatformRegistry::shouldAutoAnalyzeMedia(targetUrl, false)
                                           && !MediaPlatformRegistry::isGenericPlatformFeedUrl(targetUrl));
    if (!shouldAnalyze && !MediaPlatformRegistry::shouldAutoAnalyzeMedia(targetUrl, false)) {
      targetUrl.clear();
    }
  }

  if (services_.tabManager) {
    const auto existingId = services_.tabManager->findInternal(this, QStringLiteral("downloads"));
    if (!existingId.isNull()) {
      const auto *record = services_.tabManager->record(existingId);
      if (record && record->content) {
        const int idx = pageStack_->indexOf(record->content);
        if (idx >= 0) {
          if (auto *page = qobject_cast<MediaDownloadPage *>(record->content.data())) {
            if (!targetUrl.isEmpty()) {
              page->setSourceUrl(targetUrl, shouldAnalyze);
            }
          }
          switchTab(idx);
          return;
        }
      }
    }
  }
  auto *page = new MediaDownloadPage(services_.mediaDownload, services_.profileService);
  connect(page, &MediaDownloadPage::internalMediaOpenRequested,
          this, &BrowserWindow::openLocalMedia);
  if (!targetUrl.isEmpty()) {
    page->setSourceUrl(targetUrl, shouldAnalyze);
  }
  addInternalTab(page, I18n::text(QStringLiteral("newtab.downloads")), BrowserIcons::icon(BrowserIcon::Download), QStringLiteral("downloads"));
}

void BrowserWindow::resolveActiveMediaPageUrl(
    const QUrl &fallbackUrl, std::function<void(const QUrl &)> callback) {
  if (!callback) return;
  QPointer<BrowserWindow> guard(this);
  if (auto *sourceEngineView = currentEngineView()) {
    if (fallbackUrl.isEmpty() || sourceEngineView->url() != fallbackUrl) {
      callback(fallbackUrl);
      return;
    }
    QPointer<dalinira::engine::EngineView> guardedEngineView(sourceEngineView);
    sourceEngineView->runJavaScript(
        MediaPageUrlResolver::extractionScript(),
        [guard, guardedEngineView, fallbackUrl, callback = std::move(callback)](const QVariant &result) {
          if (!guard) return;
          const QUrl resolved = MediaPageUrlResolver::validatedResult(result, fallbackUrl);
          QUrl logUrl(resolved);
          logUrl.setQuery(QString{});
          logUrl.setFragment(QString{});
          qInfo().noquote() << "[MediaDownload] Çözülen aktif medya URL'si:"
                            << logUrl.toString(QUrl::FullyEncoded);
          Q_UNUSED(guardedEngineView);
          callback(resolved);
        });
    return;
  }
  QPointer<QWebEngineView> sourceView(currentView());
  QPointer<QWebEnginePage> sourcePage(sourceView ? sourceView->page() : nullptr);
  if (!sourcePage || fallbackUrl.isEmpty() || sourceView->url() != fallbackUrl) {
    callback(fallbackUrl);
    return;
  }

  sourcePage->runJavaScript(
      MediaPageUrlResolver::extractionScript(), QWebEngineScript::ApplicationWorld,
      [guard, sourceView, fallbackUrl, callback = std::move(callback)](const QVariant &result) {
        if (!guard) return;
        const QUrl resolved = MediaPageUrlResolver::validatedResult(result, fallbackUrl);
        QUrl logUrl(resolved);
        logUrl.setQuery(QString{});
        logUrl.setFragment(QString{});
        qInfo().noquote() << "[MediaDownload] Çözülen aktif medya URL'si:"
                          << logUrl.toString(QUrl::FullyEncoded);
        // Holding the guarded view until completion prevents its page from
        // disappearing midway through resolution; never dereference it here.
        Q_UNUSED(sourceView);
        callback(resolved);
      });
}

void BrowserWindow::openLocalMedia(const LocalMediaOpenRequest &request) {
  if (!LocalMediaRouting::isSafeDownloadedFile(request.path, request.allowedRoot)
      || request.playerKind == LocalMediaPlayerKind::External) {
    return;
  }
  const QString canonicalPath = QFileInfo(request.path).canonicalFilePath();
  const QString id = QStringLiteral("local-media-player");
  const bool audio = request.playerKind == LocalMediaPlayerKind::Audio;
  const QString title = request.title.trimmed().isEmpty()
      ? QFileInfo(canonicalPath).completeBaseName() : request.title;
  const QIcon icon = BrowserIcons::icon(audio ? BrowserIcon::Music : BrowserIcon::Video);

  const bool isWebKit = (currentWorkspaceEngine_ == dalinira::engine::EngineType::WebKit)
      || (dalinira::engine::EngineBackend::selectedEngineType() == dalinira::engine::EngineType::WebKit);
  if (isWebKit) {
    const bool mediaAvailable =
#if defined(DALINIRA_BUILD_WEBKIT)
        dalinira::engine::WebKitEngineProfile::mediaPlaybackAvailable();
#else
        true;
#endif
    if (!mediaAvailable) {
      DownloadDesktopActions::openFile(canonicalPath, request.allowedRoot);
      return;
    }

    const QString playerHtmlPath = LocalMediaPlayerPage::createOrUpdateWebKitPlayerHtml(
        request, services_.mediaDownload ? services_.mediaDownload->ffmpegPath() : QString{});
    const QUrl playerUrl = !playerHtmlPath.isEmpty()
        ? QUrl::fromLocalFile(playerHtmlPath)
        : QUrl::fromLocalFile(canonicalPath);

    for (int i = 0; i < tabs_.size(); ++i) {
      if (tabs_[i].url == playerUrl || tabs_[i].url == QUrl::fromLocalFile(canonicalPath)) {
        switchTab(i);
        return;
      }
    }
    const int newIndex = addNewTab(playerUrl, -1, false, title.left(80));
    if (newIndex >= 0 && newIndex < tabs_.size()) {
      tabs_[newIndex].icon = icon;
      tabs_[newIndex].isInternal = true;
      tabs_[newIndex].internalId = QStringLiteral("local-media-player");
      tabs_[newIndex].title = title.left(80);
      tabStrip_->setTabIcon(newIndex, icon);
      tabStrip_->setTabTitle(newIndex, tabs_[newIndex].title);
      if (tabs_[newIndex].engineView) {
        tabs_[newIndex].engineView->setProperty("dalinira-trusted-local-media", true);
        if (services_.audioEffects) {
          services_.audioEffects->registerEngineView(tabs_[newIndex].engineView.data(), playerUrl);
          services_.audioEffects->applyToEngineView(tabs_[newIndex].engineView.data());
        }
      }
      if (services_.tabManager) {
        services_.tabManager->updateIcon(tabs_[newIndex].uuid, icon);
        services_.tabManager->updateTitle(tabs_[newIndex].uuid, tabs_[newIndex].title);
      }
    }
    return;
  }

  if (services_.tabManager) {
    const QUuid existingId = services_.tabManager->findInternal(this, id);
    if (!existingId.isNull()) {
      const auto *record = services_.tabManager->record(existingId);
      const int index = record && record->content ? pageStack_->indexOf(record->content) : -1;
      if (index >= 0) {
        if (auto *page = qobject_cast<LocalMediaPlayerPage *>(record->content.data())) {
          page->loadMedia(request);
        }
        tabs_[index].title = title.left(80);
        tabs_[index].icon = icon;
        tabStrip_->setTabTitle(index, tabs_[index].title);
        tabStrip_->setTabIcon(index, icon);
        services_.tabManager->updateTitle(existingId, tabs_[index].title);
        services_.tabManager->updateIcon(existingId, icon);
        switchTab(index);
        return;
      }
    }
  }
  auto *page = new LocalMediaPlayerPage(
      request, services_.profile, services_.audioEffects,
      services_.mediaDownload ? services_.mediaDownload->ffmpegPath() : QString{});
  connect(page, &LocalMediaPlayerPage::audioEffectsRequested,
          this, &BrowserWindow::showAudioEffects);
  addInternalTab(page, title.left(80), icon, id);
}

PageTranslator *BrowserWindow::getOrCreatePageTranslator(int tabIndex) {
  if (tabIndex < 0 || tabIndex >= tabs_.size()) return nullptr;
  auto &info = tabs_[tabIndex];
  if (info.isInternal || (!info.view && !info.engineView)) return nullptr;
  if (!services_.profileService || !services_.profileService->translateService()) return nullptr;

  if (!info.translator) {
    if (info.view) {
      info.translator = new PageTranslator(info.view.data(), services_.profileService->translateService(), info.view.data());
    } else {
      info.translator = new PageTranslator(info.engineView.data(), services_.profileService->translateService(), info.engineView.data());
    }

    connect(info.translator, &PageTranslator::languageDetected, this, [this, tabId = info.id](const QString &sourceLang, const QString &targetLang) {
      Q_UNUSED(targetLang);
      const int idx = findIndexByTabId(tabId);
      if (idx < 0) return;

      const QString offerKey = tabs_[idx].url.host().toLower() + QLatin1Char('|') + sourceLang.toLower();
      if (idx == tabStrip_->currentIndex()) {
        if (translateButton_) {
          translateButton_->show();
        }
        if (!tabs_[idx].translationOffered && !dismissedTranslationOffers_.contains(offerKey)) {
          tabs_[idx].translationOffered = true;
          dismissedTranslationOffers_.insert(offerKey);
          showTranslatePopup();
        }
      }
    });

    connect(info.translator, &PageTranslator::stateChanged, this, [this, tabId = info.id](PageTranslator::State state) {
      const int idx = findIndexByTabId(tabId);
      if (idx < 0) return;
      if (idx == tabStrip_->currentIndex() && translateButton_) {
        if (state == PageTranslator::State::Detected || state == PageTranslator::State::Translated || state == PageTranslator::State::Translating) {
          translateButton_->show();
        } else if (state == PageTranslator::State::Idle) {
          translateButton_->hide();
        }
      }
    });
  }

  return info.translator.data();
}

void BrowserWindow::showTranslatePopup() {
  auto *view = currentView();
  auto *engineView = currentEngineView();
  QUrl targetUrl = view ? view->url() : (engineView ? engineView->url() : QUrl{});
  const int currentTabIndex = tabStrip_ ? tabStrip_->currentIndex() : -1;
  if ((targetUrl.isEmpty() || !targetUrl.isValid())
      && currentTabIndex >= 0 && currentTabIndex < tabs_.size()) {
    targetUrl = tabs_[currentTabIndex].url;
  }
  if ((!view && !engineView) || currentTabIndex < 0 || currentTabIndex >= tabs_.size()) return;
  if (tabs_[currentTabIndex].isInternal || isNewTabUrl(targetUrl)) return;
  const QString scheme = targetUrl.scheme().toLower();
  if (scheme != QLatin1String("http") && scheme != QLatin1String("https")) return;

  if (!translateBubble_) {
    translateBubble_ = new TranslateBubblePopup(this);
    connect(translateBubble_, &TranslateBubblePopup::openSettingsRequested, this, [this] {
      showSettings(SettingsPage::Category::Languages);
    });
  }

  auto *translator = getOrCreatePageTranslator(currentTabIndex);
  if (!translator) return;

  pageTranslator_ = translator;
  translateBubble_->setTranslator(translator);

  if (translator->sourceLanguage().isEmpty() && translator->state() == PageTranslator::State::Idle) {
    translator->detectLanguage();
  }

  QToolButton *anchor = (translateButton_ && translateButton_->isVisible())
      ? translateButton_ : mainMenuBtn_;
  const int popupWidth = translateBubble_->width() > 0 ? translateBubble_->width() : 290;
  const QPoint anchorPos = anchor
      ? anchor->mapToGlobal(QPoint(anchor->width() - popupWidth, anchor->height() + 4))
      : QCursor::pos();
  translateBubble_->showAtAnchor(anchorPos);
}

void BrowserWindow::showZoomPopup() {
  if (auto *ev = currentEngineView()) {
    setCurrentZoom(ev->zoomFactor() == 1.0 ? 1.25 : 1.0);
    return;
  }
  if (auto *view = currentView()) {
    setCurrentZoom(view->zoomFactor() == 1.0 ? 1.25 : 1.0);
  }
}

void BrowserWindow::changeCurrentZoom(qreal delta) {
  if (auto *ev = currentEngineView()) {
    setCurrentZoom(ev->zoomFactor() + delta);
    return;
  }
  if (auto *view = currentView()) {
    setCurrentZoom(view->zoomFactor() + delta);
  }
}

void BrowserWindow::setCurrentZoom(qreal factor) {
  const qreal clamped = std::clamp(factor, 0.25, 5.0);
  if (auto *ev = currentEngineView()) {
    ev->setZoomFactor(clamped);
    if (clamped != 1.0) zoomButton_->show();
    else zoomButton_->hide();
    return;
  }
  if (auto *view = currentView()) {
    view->setZoomFactor(clamped);
    if (clamped != 1.0) zoomButton_->show();
    else zoomButton_->hide();
  }
}

void BrowserWindow::saveSessionNow() {
  if (services_.sessionStore && services_.tabManager) {
    services_.sessionStore->save(*services_.tabManager, this, currentEngineScope());
  }
}

QString BrowserWindow::currentEngineScope() const {
  return dalinira::engine::engineKey(currentWorkspaceEngine_);
}

void BrowserWindow::restoreSession(const QVector<SavedTab> &savedTabs) {
  uint64_t activeTabId = 0;
  QList<uint64_t> sleepingTabIds;
  // TabLayoutModel reserves the first N slots for pinned-tab geometry. Restore
  // pinned tabs first (stable within each class), and insert each item with its
  // final presentation state so a normal tab is never placed in a pinned slot.
  for (const bool restorePinned : {true, false}) {
    for (const auto &tab : savedTabs) {
      if (tab.pinned != restorePinned) continue;
      const int idx = addNewTab(tab.url, -1, tab.pinned, tab.title);
      if (idx >= 0 && idx < tabs_.size() && tab.active) activeTabId = tabs_[idx].id;
      if (idx >= 0 && idx < tabs_.size() && tab.sleeping && !tab.active) sleepingTabIds.append(tabs_[idx].id);
      if (idx >= 0 && idx < tabs_.size() && tab.groupId.has_value() && !tab.groupId->isNull()) {
        tabs_[idx].groupId = *tab.groupId;
        if (groupModel_) {
          if (!groupModel_->hasGroup(*tab.groupId)) {
            groupModel_->addOrUpdateGroup({*tab.groupId, tab.groupName, tab.groupColor, tab.groupCollapsed});
          }
          groupModel_->setTabGroup(tabs_[idx].id, *tab.groupId);
        }
      }
    }
  }
  if (tabs_.isEmpty()) {
    ensureInitialTab();
  }
  if (tabStrip_) {
    tabStrip_->update();
  }
  const int activeIndex = findIndexByTabId(activeTabId);
  if (activeIndex >= 0) {
    switchTab(activeIndex);
  } else if (!tabs_.isEmpty()) {
    switchTab(0);
  }
  updateBookmarkBarVisibility();
  resleepRestoredTabs(sleepingTabIds);
}

void BrowserWindow::resleepRestoredTabs(const QList<uint64_t> &tabIds) {
  // Tabs that were fully sleeping when the workspace was saved go back to
  // sleep through the shared Memory Saver path once their first load settles
  // (a loading page cannot sleep). sleepTab keeps its own protections: the
  // tab on screen, unsaved input, downloads, dialogs, etc. stay awake.
  auto *perf = services_.tabManager ? services_.tabManager->performanceManager() : nullptr;
  if (!perf) return;
  for (const uint64_t tabId : tabIds) {
    const int idx = findIndexByTabId(tabId);
    if (idx < 0) continue;
    const QUuid uuid = tabs_[idx].uuid;
    const TabManager::TabRecord *record = services_.tabManager->record(uuid);
    if (!record || !record->lifecycle) continue;
    auto connection = std::make_shared<QMetaObject::Connection>();
    *connection = connect(record->lifecycle, &dalinira::TabLifecycleAdapter::loadFinished, this,
                          [this, uuid, connection](bool) {
      disconnect(*connection);
      QTimer::singleShot(0, this, [this, uuid] {
        auto *manager = services_.tabManager ? services_.tabManager->performanceManager() : nullptr;
        if (manager && manager->sleepTab(uuid)) syncTabSleepingIndicator(uuid);
      });
    });
  }
}

void BrowserWindow::ensureInitialTab() {
  if (tabs_.isEmpty()) {
    addNewTab(QUrl(QStringLiteral("dalinira://newtab/")));
  }
}

void BrowserWindow::openStartupUrl(const QUrl &url) {
  addNewTab(url);
}

// -----------------------------------------------------------------
// Window events, Edge Resizing & Window Caption Controls
// -----------------------------------------------------------------
void BrowserWindow::closeEvent(QCloseEvent *event) {
  if (hoverCard_) hoverCard_->hideCard();
  if (services_.songRecognition && services_.songRecognition->isListening()) {
    const bool ownsOpenPulsePage = std::any_of(tabs_.cbegin(), tabs_.cend(), [](const BrowserTabInfo &tab) {
      return tab.isInternal && tab.internalId == QLatin1String("song-finder");
    });
    if (ownsOpenPulsePage) services_.songRecognition->stopListening();
  }
  saveSessionNow();
  if (!isCaptureShell_ && !isMinimized()) {
    QSettings settings;
    settings.setValue(QStringLiteral("browser/mainWindowGeometry"), saveGeometry());
    settings.sync();
  }
  dalinira::desktop_tabs::TabWindowRegistry::instance().unregisterWindow(this);
  QMainWindow::closeEvent(event);
  if (event->isAccepted() && services_.profileService) {
    auto *blocker = services_.profileService->blockerService();
    for (const auto &tab : std::as_const(tabs_))
      if (tab.view) blocker->unregisterTab(reinterpret_cast<quintptr>(tab.view.data()));
    for (const auto &tab : std::as_const(tabs_))
      if (tab.view) beginForgetClosedView(tab.view);
  }
  if (event->isAccepted() &&
      dalinira::desktop_tabs::TabWindowRegistry::instance().registeredWindows().isEmpty()) {
    // Foreign QWindows created for WebKitGTK may remain in Qt's top-level
    // window list until their owner is deleted. Do not let them keep the
    // application event loop alive after the final browser window closes.
    QTimer::singleShot(0, qApp, &QCoreApplication::quit);
  }
}

void BrowserWindow::keyPressEvent(QKeyEvent *event) {
  if (event->key() == Qt::Key_Escape && isWebFullScreen_) {
    if (auto *view = currentView()) {
      if (view->page()) {
        view->page()->triggerAction(QWebEnginePage::ExitFullScreen);
      }
    } else if (auto *engineView = currentEngineView()) {
      engineView->exitFullScreen();
    } else {
      const int idx = tabStrip_ ? tabStrip_->currentIndex() : -1;
      if (idx >= 0 && idx < tabs_.size() && tabs_[idx].content) {
        if (auto *lmp = qobject_cast<LocalMediaPlayerPage *>(tabs_[idx].content.data())) {
          if (lmp->webView() && lmp->webView()->page()) {
            lmp->webView()->page()->triggerAction(QWebEnginePage::ExitFullScreen);
          }
        }
      }
    }
    setPageFullScreen(false);
    event->accept();
    return;
  }
  if (event->key() == Qt::Key_Escape && progressBar_->isVisible()) {
    if (auto *view = currentView()) {
      view->stop();
    }
    event->accept();
    return;
  }
  QMainWindow::keyPressEvent(event);
}

void BrowserWindow::onMinimizeClicked() {
  showMinimized();
}

void BrowserWindow::onMaximizeRestoreClicked() {
  if (isMaximized()) {
    showNormal();
    if (maxBtn_) maxBtn_->setText(QString::fromUtf8("□"));
  } else {
    showMaximized();
    if (maxBtn_) maxBtn_->setText(QString::fromUtf8("❐"));
  }
}

void BrowserWindow::onCloseWindowClicked() {
  close();
}

void BrowserWindow::changeEvent(QEvent *event) {
  if (event->type() == QEvent::WindowStateChange) {
    if (maxBtn_) {
      maxBtn_->setText(isMaximized() ? QString::fromUtf8("❐") : QString::fromUtf8("□"));
    }
    if (isMaximized() || isFullScreen()) {
      if (hasOverrideCursor_) {
        QGuiApplication::restoreOverrideCursor();
        hasOverrideCursor_ = false;
      }
      if (resizing_) {
        resizing_ = false;
        resizeEdges_ = {};
        releaseMouse();
      }
    }
    if (hoverCard_) hoverCard_->hideCard();
    dismissSiteControlsBubble();
  } else if (event->type() == QEvent::ActivationChange && !isActiveWindow()) {
    if (hasOverrideCursor_) {
      QGuiApplication::restoreOverrideCursor();
      hasOverrideCursor_ = false;
    }
    if (resizing_) {
      resizing_ = false;
      resizeEdges_ = {};
      releaseMouse();
    }
    if (manualMoving_) {
      manualMoving_ = false;
      releaseMouse();
    }
    if (hoverCard_) hoverCard_->hideCard();
    dismissSiteControlsBubble();
  }
  QMainWindow::changeEvent(event);
}

void BrowserWindow::showEvent(QShowEvent *event) {
  QMainWindow::showEvent(event);
  updateBookmarkBarVisibility();
}

void BrowserWindow::resizeEvent(QResizeEvent *event) {
  if (hoverCard_) hoverCard_->hideCard();
  QMainWindow::resizeEvent(event);
  updatePermissionBubblePosition();
  updateSiteControlsBubblePosition();
  updateSaveBubblePosition();
  updateFindBarPosition();
  if (downloadPopup_ && downloadPopup_->isVisible()) downloadPopup_->reposition(mediaDownload_);
  if (!isMaximized() && !isFullScreen() && !(windowState() & (Qt::WindowMaximized | Qt::WindowFullScreen))) {
    lastNormalSize_ = size();
    lastNormalGeometry_ = geometry();
    setProperty("daliniraRestoredSize", lastNormalSize_);
  } else if (isMaximized()) {
    const QRect norm = normalGeometry();
    if (norm.isValid() && norm.width() >= 600 && norm.height() >= 400) {
      lastNormalSize_ = norm.size();
      lastNormalGeometry_ = norm;
      setProperty("daliniraRestoredSize", lastNormalSize_);
    }
  }
}

void BrowserWindow::moveEvent(QMoveEvent *event) {
  if (hoverCard_) hoverCard_->hideCard();
  QMainWindow::moveEvent(event);
  updatePermissionBubblePosition();
  updateSiteControlsBubblePosition();
  updateSaveBubblePosition();
  if (downloadPopup_ && downloadPopup_->isVisible()) downloadPopup_->reposition(mediaDownload_);
  if (!isMaximized() && !isFullScreen() && !(windowState() & (Qt::WindowMaximized | Qt::WindowFullScreen))) {
    lastNormalGeometry_ = geometry();
    lastNormalSize_ = size();
    setProperty("daliniraRestoredSize", lastNormalSize_);
  }
}

QSize BrowserWindow::restoredSize() const {
  if (lastNormalSize_.isValid() && lastNormalSize_.width() >= 600 && lastNormalSize_.height() >= 400) {
    return lastNormalSize_;
  }
  const QRect norm = normalGeometry();
  if (norm.isValid() && norm.width() >= 600 && norm.height() >= 400) {
    return norm.size();
  }
  return QSize(1200, 800);
}

QRect BrowserWindow::restoredGeometry() const {
  if (lastNormalGeometry_.isValid() && lastNormalGeometry_.width() >= 600 && lastNormalGeometry_.height() >= 400) {
    return lastNormalGeometry_;
  }
  const QRect norm = normalGeometry();
  if (norm.isValid() && norm.width() >= 600 && norm.height() >= 400) {
    return norm;
  }
  return QRect(100, 100, 1200, 800);
}

Qt::Edges BrowserWindow::calculateEdges(const QPoint &pos) const {
  if (isMaximized() || isFullScreen() || (windowState() & (Qt::WindowMaximized | Qt::WindowFullScreen))) {
    return {};
  }
  Qt::Edges edges;
  const int margin = 6;
  if (pos.x() <= margin) edges |= Qt::LeftEdge;
  if (pos.x() >= width() - margin) edges |= Qt::RightEdge;
  if (pos.y() <= margin) edges |= Qt::TopEdge;
  if (pos.y() >= height() - margin) edges |= Qt::BottomEdge;
  return edges;
}

void BrowserWindow::updateCursorShape(const QPoint &pos) {
  if (isMaximized() || isFullScreen() ||
      property("daliniraDragCaptureShell").toBool() ||
      dalinira::desktop_tabs::TabDragController::instance().isActive()) {
    if (hasOverrideCursor_) {
      QGuiApplication::restoreOverrideCursor();
      hasOverrideCursor_ = false;
    }
    unsetCursor();
    return;
  }
  const Qt::Edges edges = calculateEdges(pos);
  if (edges != 0) {
    Qt::CursorShape shape = Qt::ArrowCursor;
    if ((edges & Qt::LeftEdge && edges & Qt::TopEdge) ||
        (edges & Qt::RightEdge && edges & Qt::BottomEdge)) {
      shape = Qt::SizeFDiagCursor;
    } else if ((edges & Qt::RightEdge && edges & Qt::TopEdge) ||
               (edges & Qt::LeftEdge && edges & Qt::BottomEdge)) {
      shape = Qt::SizeBDiagCursor;
    } else if (edges & (Qt::LeftEdge | Qt::RightEdge)) {
      shape = Qt::SizeHorCursor;
    } else if (edges & (Qt::TopEdge | Qt::BottomEdge)) {
      shape = Qt::SizeVerCursor;
    }
    if (!hasOverrideCursor_) {
      QGuiApplication::setOverrideCursor(shape);
      hasOverrideCursor_ = true;
      currentOverrideShape_ = shape;
    } else if (currentOverrideShape_ != shape) {
      QGuiApplication::changeOverrideCursor(shape);
      currentOverrideShape_ = shape;
    }
  } else {
    if (hasOverrideCursor_) {
      QGuiApplication::restoreOverrideCursor();
      hasOverrideCursor_ = false;
    }
    unsetCursor();
  }
}

void BrowserWindow::handleManualResize(const QPoint &globalPos) {
  if (!resizing_ || resizeEdges_ == 0) return;
  const QPoint diff = globalPos - resizeStartPos_;
  QRect newGeo = resizeStartGeometry_;
  const QSize minSz = minimumSizeHint().expandedTo(minimumSize());
  const int minW = std::max(640, minSz.width());
  const int minH = std::max(420, minSz.height());

  if (resizeEdges_ & Qt::LeftEdge) {
    const int newWidth = std::max(minW, resizeStartGeometry_.width() - diff.x());
    newGeo.setLeft(resizeStartGeometry_.right() - newWidth + 1);
  }
  if (resizeEdges_ & Qt::RightEdge) {
    const int newWidth = std::max(minW, resizeStartGeometry_.width() + diff.x());
    newGeo.setWidth(newWidth);
  }
  if (resizeEdges_ & Qt::TopEdge) {
    const int newHeight = std::max(minH, resizeStartGeometry_.height() - diff.y());
    newGeo.setTop(resizeStartGeometry_.bottom() - newHeight + 1);
  }
  if (resizeEdges_ & Qt::BottomEdge) {
    const int newHeight = std::max(minH, resizeStartGeometry_.height() + diff.y());
    newGeo.setHeight(newHeight);
  }

  setGeometry(newGeo);
}

void BrowserWindow::mousePressEvent(QMouseEvent *event) {
  if (event->button() == Qt::LeftButton) {
    const QPoint pos = event->position().toPoint();
    const Qt::Edges edges = calculateEdges(pos);
    if (edges != 0) {
      bool started = false;
      if (windowHandle()) {
        started = windowHandle()->startSystemResize(edges);
      }
      if (!started) {
        resizing_ = true;
        resizeEdges_ = edges;
        resizeStartPos_ = event->globalPosition().toPoint();
        resizeStartGeometry_ = geometry();
        grabMouse();
      }
      event->accept();
      return;
    }
    // Empty space in the title area (controls take their own presses). A
    // click is not a drag: the move starts once the pointer really moves.
    if (pos.y() <= 40 && !isFullScreen()) {
      titleMovePending_ = true;
      titleMovePressPos_ = event->globalPosition().toPoint();
      event->accept();
      return;
    }
  }
  QMainWindow::mousePressEvent(event);
}

void BrowserWindow::mouseMoveEvent(QMouseEvent *event) {
  if (titleMovePending_) {
    const QPoint globalPos = event->globalPosition().toPoint();
    if (!(event->buttons() & Qt::LeftButton)) {
      titleMovePending_ = false;
    } else if ((globalPos - titleMovePressPos_).manhattanLength() >= QApplication::startDragDistance()) {
      titleMovePending_ = false;
      // The window manager moves the window (X11 _NET_WM_MOVERESIZE, Wayland
      // xdg_toplevel.move): KWin and others then snap, tile, maximize at the
      // top edge and restore a dragged maximized window, as for any window.
      if (windowHandle() && windowHandle()->startSystemMove()) {
        event->accept();
        return;
      }
      // Fallback for a window manager without a native move.
      if (!isMaximized()) {
        manualMoving_ = true;
        manualMoveOffset_ = titleMovePressPos_ - this->pos();
        grabMouse();
      }
    }
    event->accept();
    return;
  }
  if (manualMoving_) {
    if (event->buttons() & Qt::LeftButton) {
      move(event->globalPosition().toPoint() - manualMoveOffset_);
      event->accept();
      return;
    }
    manualMoving_ = false;
    releaseMouse();
  }
  if (resizing_) {
    handleManualResize(event->globalPosition().toPoint());
    event->accept();
    return;
  }
  if (!property("daliniraDragCaptureShell").toBool() &&
      !dalinira::desktop_tabs::TabDragController::instance().isActive()) {
    updateCursorShape(event->position().toPoint());
  } else {
    if (hasOverrideCursor_) {
      QGuiApplication::restoreOverrideCursor();
      hasOverrideCursor_ = false;
    }
    unsetCursor();
  }
  QMainWindow::mouseMoveEvent(event);
}

void BrowserWindow::mouseReleaseEvent(QMouseEvent *event) {
  titleMovePending_ = false;
  if (manualMoving_) {
    manualMoving_ = false;
    releaseMouse();
    event->accept();
    return;
  }
  if (resizing_) {
    resizing_ = false;
    resizeEdges_ = {};
    releaseMouse();
    event->accept();
    return;
  }
  if (hasOverrideCursor_) {
    QGuiApplication::restoreOverrideCursor();
    hasOverrideCursor_ = false;
  }
  unsetCursor();
  QMainWindow::mouseReleaseEvent(event);
}

QIcon BrowserWindow::tabIconForRecord(const BrowserTabInfo &info) const {
  if (info.isInternal && !info.icon.isNull()) {
    return info.icon;
  }
  const bool isNewTab = (info.view && (isNewTabUrl(info.view->url()) || info.view->property("dalinira-is-newtab-intent").toBool()))
                     || isNewTabUrl(info.url) || info.url.isEmpty();
  if (isNewTab) {
    return BrowserIcons::appIcon();
  }
  if (!info.icon.isNull()) {
    return info.icon;
  }
  return BrowserIcons::appIcon();
}

void BrowserWindow::updateBookmarkButtonState() {
  if (!bookmarkBtn_) return;
  const QUrl url = currentView() ? currentView()->url() : (tabStrip_->currentIndex() >= 0 && tabStrip_->currentIndex() < tabs_.size() ? tabs_[tabStrip_->currentIndex()].url : QUrl{});
  const QString engine = currentEngineScope();
  const bool bookmarked = services_.profileService && !isNewTabUrl(url) && url.isValid() && services_.profileService->isQuickAccess(url, engine);
  bookmarkBtn_->setIcon(bookmarkIcon(bookmarked));
  bookmarkBtn_->setToolTip(bookmarked
      ? I18n::text(QStringLiteral("toolbar.bookmark_remove"))
      : I18n::text(QStringLiteral("toolbar.bookmark_add")));
}

void BrowserWindow::updateBlockerControls() {
  if (!adBlockShield_) return;

  // The blocker shield button must ALWAYS remain enabled and clickable
  adBlockShield_->setEnabled(true);

  auto *view = currentView();
  auto *engineView = currentEngineView();
  const int idx = tabStrip_ ? tabStrip_->currentIndex() : -1;
  const bool isInternalTab = (idx >= 0 && idx < tabs_.size() && tabs_[idx].isInternal);

  const QUrl url = (idx >= 0 && idx < tabs_.size() && tabs_[idx].url.isValid() && !tabs_[idx].url.isEmpty())
      ? tabs_[idx].url
      : (engineView ? engineView->url() : (view ? view->url() : QUrl{}));
  if ((!view && !engineView) || isInternalTab || isInternalOrNonWebUrl(url)) {
    adBlockShield_->setInternalPage(true);
    adBlockShield_->setActiveHost(QString());
    adBlockShield_->setBlockedCount(0);
    adBlockShield_->setToolTip(I18n::text(QStringLiteral("toolbar.adblock")));
    return;
  }

  // Normal supported web tab
  const QString host = url.host().toLower();
  adBlockShield_->setInternalPage(false);
  adBlockShield_->setActiveHost(host);

  const quint64 tabId = engineView ? reinterpret_cast<quintptr>(engineView) : reinterpret_cast<quintptr>(view);
  if (services_.profileService && services_.profileService->adBlockService()) {
    services_.profileService->adBlockService()->setActiveTabId(tabId);
    const auto stats = services_.profileService->adBlockService()->statsForTab(tabId);
    const quint64 total = stats.totalBlocked();
    adBlockShield_->setBlockedCount(total);
    adBlockShield_->setToolTip(total > 0
        ? I18n::text(QStringLiteral("toolbar.adblock_status")).arg(host) + QStringLiteral(" (") + I18n::text(QStringLiteral("toolbar.adblock_blocked_count")).arg(total) + QStringLiteral(")")
        : I18n::text(QStringLiteral("toolbar.adblock_status")).arg(host));
  } else {
    adBlockShield_->setBlockedCount(0);
    adBlockShield_->setToolTip(I18n::text(QStringLiteral("toolbar.adblock_status")).arg(host));
  }
}

void BrowserWindow::toggleCurrentBookmark() {
  if (!services_.profileService) return;
  const QUrl url = currentView() ? currentView()->url() : (tabStrip_->currentIndex() >= 0 && tabStrip_->currentIndex() < tabs_.size() ? tabs_[tabStrip_->currentIndex()].url : QUrl{});
  if (!url.isValid() || isNewTabUrl(url)) return;

  const int curIdx = tabStrip_ ? tabStrip_->currentIndex() : -1;
  QIcon currentIcon;
  if (curIdx >= 0 && curIdx < tabs_.size() && !tabs_[curIdx].icon.isNull()) {
    currentIcon = tabs_[curIdx].icon;
  } else if (currentEngineView() && !currentEngineView()->icon().isNull()) {
    currentIcon = currentEngineView()->icon();
  }
  if (!currentIcon.isNull()) {
    cacheNewTabFavicon(services_.profileService, url, currentIcon);
  }

  const QString engine = currentEngineScope();
  services_.profileService->toggleQuickAccess(url, bookmarkDisplayName(url), engine);
  updateBookmarkButtonState();
  renderBookmarks();
}

void BrowserWindow::renderBookmarks() {
  using Metrics = dalinira::ui::BrowserChromeMetrics;
  if (!bookmarkBar_ || !services_.profileService) return;
  if (bookmarkReorder_) bookmarkReorder_->clearItems();
  // QToolBar::clear() only detaches the widget actions: every previous button
  // would stay alive, hidden and parentless, until the window closes. Delete
  // them, deferred: a rebuild can run from a click handler of one of them
  // (the shortcut remove button).
  const QList<QAction *> previousActions = bookmarkBar_->actions();
  bookmarkBar_->clear();
  for (QAction *action : previousActions)
    if (action->parent() == bookmarkBar_) action->deleteLater();

  // 1. Far-left Tab Group button ("Yeni sekme grubu oluştur")
  appsBtn_ = new QToolButton(bookmarkBar_);
  appsBtn_->setObjectName(QStringLiteral("appsButton"));
  appsBtn_->setIcon(BrowserIcons::icon(BrowserIcon::Grid));
  appsBtn_->setToolTip(I18n::text(QStringLiteral("toolbar.new_tab_group")));
  appsBtn_->setFixedSize(28, Metrics::bookmarkButtonHeight);
  appsBtn_->setIconSize(QSize(Metrics::bookmarkIconSize,
                              Metrics::bookmarkIconSize));
  appsBtn_->setAutoRaise(true);
  const bool showTabGroups = QSettings().value(QStringLiteral("browser/showTabGroupsOnBookmarkBar"), true).toBool();
  appsBtn_->setVisible(showTabGroups);
  appsBtn_->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(appsBtn_, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
    showBookmarkContextMenu(QUrl(), QString(), appsBtn_->mapToGlobal(pos));
  });
  connect(appsBtn_, &QToolButton::clicked, this, &BrowserWindow::toggleTabGroupLauncher);
  bookmarkBar_->addWidget(appsBtn_);

  // 2. Bookmark Folders
  const QStringList folders = services_.profileService->bookmarkFolders();
  const auto allItems = services_.profileService->bookmarkItems();

  for (const QString &folderName : folders) {
    auto *folderBtn = new QToolButton(bookmarkBar_);
    folderBtn->setObjectName(QStringLiteral("bookmarkFolderButton"));
    folderBtn->setText(folderName);
    folderBtn->setToolTip(I18n::text(QStringLiteral("bookmark.folder_tooltip")).arg(folderName));
    folderBtn->setIcon(BrowserIcons::icon(BrowserIcon::Folder));
    folderBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    folderBtn->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    folderBtn->setIconSize(QSize(Metrics::bookmarkIconSize, Metrics::bookmarkIconSize));
    folderBtn->setFixedHeight(Metrics::bookmarkButtonHeight);
    folderBtn->setCursor(Qt::PointingHandCursor);
    folderBtn->setPopupMode(QToolButton::InstantPopup);

    auto *menu = new QMenu(folderBtn);
    menu->setStyleSheet(QStringLiteral("QMenu{background:#1b232d;color:#e8eef5;border:1px solid #3a4857;border-radius:6px;padding:4px;} QMenu::item{padding:4px 24px;} QMenu::item:selected{background:#2a3644;}"));

    int count = 0;
    for (const auto &item : allItems) {
      if (item.folder.trimmed() == folderName) {
        count++;
        const QString title = item.title.isEmpty() ? bookmarkDisplayName(item.url) : item.title;
        QAction *act = menu->addAction(bookmarkFavicon(item.url), title);
        act->setProperty("dalinira-bookmark-url", item.url);
        act->setToolTip(item.url.toDisplayString());
        const QPointer<QAction> guardedAct(act);
        requestBookmarkFavicon(item.url, [guardedAct](const QIcon &icon) {
          if (guardedAct && !icon.isNull()) {
            guardedAct->setIcon(icon);
          }
        });
        connect(act, &QAction::triggered, this, [this, url = item.url] {
          openBookmarkUrl(url, QGuiApplication::keyboardModifiers() & (Qt::ControlModifier | Qt::MetaModifier));
        });
      }
    }
    if (count == 0) {
      QAction *empty = menu->addAction(I18n::text(QStringLiteral("bookmark.folder_empty")));
      empty->setEnabled(false);
    }
    folderBtn->setMenu(menu);

    folderBtn->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(folderBtn, &QWidget::customContextMenuRequested, this, [this, folderName, folderBtn](const QPoint &pos) {
      QMenu ctxMenu(folderBtn);
      ctxMenu.setStyleSheet(QStringLiteral("QMenu{background:#1b232d;color:#e8eef5;border:1px solid #3a4857;border-radius:6px;padding:4px;} QMenu::item{padding:4px 20px;} QMenu::item:selected{background:#2a3644;}"));
      QAction *openAll = ctxMenu.addAction(I18n::text(QStringLiteral("bookmark.open_all_new_tabs")));
      ctxMenu.addSeparator();
      QAction *delFolder = ctxMenu.addAction(BrowserIcons::icon(BrowserIcon::Close), I18n::text(QStringLiteral("bookmark.delete_folder")));
      QAction *chosen = ctxMenu.exec(folderBtn->mapToGlobal(pos));
      if (chosen == openAll) {
        for (const auto &item : services_.profileService->bookmarkItems()) {
          if (item.folder.trimmed() == folderName) addNewTab(item.url);
        }
      } else if (chosen == delFolder) {
        services_.profileService->removeBookmarkFolder(folderName, true);
        renderBookmarks();
      }
    });

    bookmarkBar_->addWidget(folderBtn);
  }

  // 3. Engine Quick Access / Shortcut items with icon, site name, and remove button.
  const QString currentEngine = currentEngineScope();
  const auto items = services_.profileService->quickAccessItems(currentEngine);

  for (const auto &itemEntry : items) {
    const QUrl &url = itemEntry.url;
    const QString title = itemEntry.title.isEmpty() ? bookmarkDisplayName(url) : itemEntry.title;
    auto *item = new QWidget(bookmarkBar_);
    item->setContextMenuPolicy(Qt::CustomContextMenu);
    auto *itemLayout = new QHBoxLayout(item);
    itemLayout->setContentsMargins(0, 0, 0, 0);
    itemLayout->setSpacing(0);
    auto *btn = new QToolButton(item);
    btn->setObjectName(QStringLiteral("bookmarkItemButton"));
    btn->setText(title);
    btn->setToolTip(url.toDisplayString());
    btn->setIcon(bookmarkFavicon(url));
    btn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    btn->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    btn->setIconSize(QSize(Metrics::bookmarkIconSize, Metrics::bookmarkIconSize));
    btn->setFixedHeight(Metrics::bookmarkButtonHeight);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setContextMenuPolicy(Qt::CustomContextMenu);

    connect(btn, &QWidget::customContextMenuRequested, this, [this, url, title, btn](const QPoint &pos) {
      showBookmarkContextMenu(url, title, btn->mapToGlobal(pos));
    });
    connect(item, &QWidget::customContextMenuRequested, this, [this, url, title, item](const QPoint &pos) {
      showBookmarkContextMenu(url, title, item->mapToGlobal(pos));
    });

    auto *remove = new QToolButton(item);
    remove->setObjectName(QStringLiteral("bookmarkRemoveButton"));
    remove->setIcon(BrowserIcons::icon(BrowserIcon::Close));
    remove->setIconSize(QSize(12, 12));
    remove->setFixedSize(20, Metrics::bookmarkButtonHeight);
    remove->setToolTip(I18n::text(QStringLiteral("bookmark.remove_shortcut")).arg(title));
    remove->setCursor(Qt::PointingHandCursor);
    itemLayout->addWidget(btn);
    itemLayout->addWidget(remove);
    bookmarkBar_->addWidget(item);
    if (bookmarkReorder_) bookmarkReorder_->addItem(item, btn, url);

    const QPointer<QToolButton> guardedButton(btn);
    requestBookmarkFavicon(url, [guardedButton](const QIcon &icon) {
      if (guardedButton && !icon.isNull()) {
        guardedButton->setIcon(icon);
      }
    });

    // Plain click navigates the current tab (both engines); Ctrl/Meta-click
    // and middle-click (see eventFilter) open a new tab, as in Brave.
    btn->setProperty("dalinira-bookmark-url", url);
    btn->installEventFilter(this);
    connect(btn, &QToolButton::clicked, this, [this, url] {
      openBookmarkUrl(url, QGuiApplication::keyboardModifiers() & (Qt::ControlModifier | Qt::MetaModifier));
    });
    connect(remove, &QToolButton::clicked, this, [this, url, currentEngine] {
      if (!services_.profileService || !services_.profileService->isQuickAccess(url, currentEngine)) return;
      services_.profileService->removeQuickAccess(url, currentEngine);
      updateBookmarkButtonState();
      renderBookmarks();
    });
  }
}

QIcon BrowserWindow::bookmarkFavicon(const QUrl &url) const {
  if (services_.profileService) {
    const QIcon cached = cachedNewTabFavicon(services_.profileService, url);
    if (!cached.isNull()) return cached;
  }
  for (const auto &tab : tabs_) {
    if (!tab.icon.isNull() && !tab.url.isEmpty()) {
      if (tab.url.host().compare(url.host(), Qt::CaseInsensitive) == 0 ||
          dalinira::core::frequent_sites::frequentSiteKey(tab.url) == dalinira::core::frequent_sites::frequentSiteKey(url)) {
        if (services_.profileService) {
          cacheNewTabFavicon(services_.profileService, url, tab.icon);
        }
        return tab.icon;
      }
    }
  }
  return platformIconForBookmark(url, services_.profileService);
}

void BrowserWindow::updateBookmarkIconsForUrl(const QUrl &url, const QIcon &icon) {
  if (!bookmarkBar_ || icon.isNull() || !url.isValid()) return;
  const QString targetHost = url.host().toLower();
  QString cleanTarget = targetHost;
  if (cleanTarget.startsWith(QLatin1String("www."))) cleanTarget = cleanTarget.mid(4);

  const auto buttons = bookmarkBar_->findChildren<QToolButton*>();
  for (QToolButton *btn : buttons) {
    const QVariant var = btn->property("dalinira-bookmark-url");
    if (!var.isValid()) continue;
    const QUrl bmUrl = var.toUrl();
    const QString bmHost = bmUrl.host().toLower();
    QString cleanBm = bmHost;
    if (cleanBm.startsWith(QLatin1String("www."))) cleanBm = cleanBm.mid(4);
    if (cleanTarget == cleanBm || targetHost == bmHost) {
      btn->setIcon(icon);
    }
  }

  const auto actions = bookmarkBar_->findChildren<QAction*>();
  for (QAction *act : actions) {
    const QVariant var = act->property("dalinira-bookmark-url");
    if (!var.isValid()) continue;
    const QUrl bmUrl = var.toUrl();
    const QString bmHost = bmUrl.host().toLower();
    QString cleanBm = bmHost;
    if (cleanBm.startsWith(QLatin1String("www."))) cleanBm = cleanBm.mid(4);
    if (cleanTarget == cleanBm || targetHost == bmHost) {
      act->setIcon(icon);
    }
  }
}

void BrowserWindow::requestBookmarkFavicon(const QUrl &url, std::function<void(const QIcon &)> callback) {
  if (!url.isValid() || isNewTabUrl(url)) return;

  if (services_.profileService) {
    const QIcon cached = cachedNewTabFavicon(services_.profileService, url);
    if (!cached.isNull()) {
      if (callback) callback(cached);
      return;
    }
  }

  for (const auto &tab : tabs_) {
    if (!tab.icon.isNull() && !tab.url.isEmpty() &&
        (tab.url.host().compare(url.host(), Qt::CaseInsensitive) == 0 ||
         dalinira::core::frequent_sites::frequentSiteKey(tab.url) == dalinira::core::frequent_sites::frequentSiteKey(url))) {
      if (services_.profileService) {
        cacheNewTabFavicon(services_.profileService, url, tab.icon);
      }
      if (callback) callback(tab.icon);
      return;
    }
  }

#if defined(DALINIRA_BUILD_WEBKIT)
  dalinira::engine::WebKitEngineProfile *wkProfile = nullptr;
  for (const auto &tab : tabs_) {
    if (auto *wkView = qobject_cast<dalinira::engine::WebKitEngineView *>(tab.engineView.data())) {
      if (wkView->profile()) {
        wkProfile = wkView->profile();
        break;
      }
    }
  }
  if (wkProfile) {
    const QPointer<BrowserWindow> guardedThis(this);
    wkProfile->requestFavicon(url, [guardedThis, url, callback](const QIcon &icon) {
      if (!guardedThis) return;
      if (!icon.isNull()) {
        if (guardedThis->services_.profileService) {
          cacheNewTabFavicon(guardedThis->services_.profileService, url, icon);
        }
        if (callback) callback(icon);
        guardedThis->updateBookmarkIconsForUrl(url, icon);
        return;
      }
      guardedThis->fetchFaviconFromNetwork(url, callback);
    });
    return;
  }
#endif

  if (services_.profile) {
    const QPointer<BrowserWindow> guardedThis(this);
    services_.profile->requestIconForPageURL(url, 64, [guardedThis, url, callback](const QIcon &icon, const QUrl &, const QUrl &) {
      if (!guardedThis) return;
      if (!icon.isNull()) {
        if (guardedThis->services_.profileService) {
          cacheNewTabFavicon(guardedThis->services_.profileService, url, icon);
        }
        if (callback) callback(icon);
        guardedThis->updateBookmarkIconsForUrl(url, icon);
        return;
      }
      guardedThis->fetchFaviconFromNetwork(url, callback);
    });
    return;
  }

  fetchFaviconFromNetwork(url, callback);
}

void BrowserWindow::fetchFaviconFromNetwork(const QUrl &url, std::function<void(const QIcon &)> callback) {
  if (!url.isValid() || (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https"))) return;
  const QString host = url.host().toLower();
  if (host.isEmpty()) return;

  auto attempts = property("dalinira-bookmark-favicon-attempts").toMap();
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  if (attempts.contains(host) && now - attempts.value(host).toLongLong() < 60000) return;
  attempts.insert(host, now);
  setProperty("dalinira-bookmark-favicon-attempts", attempts);

  QUrl target;
  target.setScheme(url.scheme().isEmpty() ? QStringLiteral("https") : url.scheme());
  target.setHost(host);
  target.setPath(QStringLiteral("/favicon.ico"));

  auto *network = new QNetworkAccessManager(this);
  QNetworkRequest request(target);
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
  request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
  request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
  request.setTransferTimeout(5000);

  auto *reply = network->get(request);
  constexpr qint64 maxBytes = 2 * 1024 * 1024;
  connect(reply, &QNetworkReply::readyRead, reply, [reply] {
    if (reply->bytesAvailable() > maxBytes) reply->abort();
  });
  connect(reply, &QNetworkReply::finished, this, [this, reply, network, url, callback = std::move(callback)] {
    if (reply->error() == QNetworkReply::NoError && reply->bytesAvailable() <= maxBytes) {
      const QByteArray bytes = reply->readAll();
      QImage image;
      if (!image.loadFromData(bytes)) {
        QBuffer buffer;
        QByteArray copy = bytes;
        buffer.setData(copy);
        buffer.open(QIODevice::ReadOnly);
        QImageReader reader(&buffer);
        image = reader.read();
      }
      if (!image.isNull() && image.width() <= 1024 && image.height() <= 1024) {
        const QIcon icon(QPixmap::fromImage(image));
        if (services_.profileService) {
          cacheNewTabFavicon(services_.profileService, url, icon);
        }
        if (callback) callback(icon);
        updateBookmarkIconsForUrl(url, icon);
      }
    }
    reply->deleteLater();
    network->deleteLater();
  });
}

void BrowserWindow::notifyNavigationStarted(QWebEngineView *view, const QUrl &url) {
  if (!view) return;
  for (int i = 0; i < tabs_.size(); ++i) {
    if (tabs_[i].view == view) {
      tabs_[i].url = url;
      if (i == tabStrip_->currentIndex()) {
        updateBookmarkBarVisibility();
      }
      break;
    }
  }
}

bool BrowserWindow::isCurrentTabNewTab() const {
  if (tabs_.isEmpty()) return false;
  const int idx = tabStrip_ ? tabStrip_->currentIndex() : -1;
  if (idx < 0 || idx >= tabs_.size()) return false;
  const auto &info = tabs_[idx];
  if (info.isInternal) return false;

  // If the target or pending tab URL is set and is NOT a new tab URL, it cannot be a new tab.
  if (!info.url.isEmpty() && !isNewTabUrl(info.url)) {
    return false;
  }

  // If the web view has loaded a URL and it is NOT a new tab URL, it cannot be a new tab.
  if (info.view) {
    const QUrl viewUrl = info.view->url();
    if (!viewUrl.isEmpty() && viewUrl.toString() != QLatin1String("about:blank")) {
      if (!isNewTabUrl(viewUrl)) {
        return false;
      }
    }
  }

  return isNewTabUrl(info.url);
}

void BrowserWindow::updateBookmarkBarVisibility() {
  // Called whenever the active tab may have become (or left) the New Tab page.
  updateSidePanelAutoHide();
  if (!bookmarkBar_) return;

  const QString mode = QSettings().value(
      QStringLiteral("browser/bookmarkBarVisibility"),
      QStringLiteral("new_tab")).toString();

  // Show the bookmark bar ONLY while the active tab is the DaliNira New Tab page.
  // Hide it on every normal website, search results page, internal non-New-Tab page, and local page.
  const bool shouldBeVisible = (mode != QLatin1String("never")) && isCurrentTabNewTab();

  bookmarkBar_->setVisible(shouldBeVisible);
}

void BrowserWindow::toggleBookmarkBar() {
  QSettings settings;
  const QString currentMode = settings.value(
      QStringLiteral("browser/bookmarkBarVisibility"),
      QStringLiteral("new_tab")).toString();

  const QString newMode = (currentMode == QLatin1String("never"))
      ? QStringLiteral("new_tab")
      : QStringLiteral("never");

  settings.setValue(QStringLiteral("browser/bookmarkBarVisibility"), newMode);
  settings.sync();
  updateBookmarkBarVisibility();
}


// Menus extracted to browser_window_menus.cpp


void BrowserWindow::fillCurrentPageFromVault() {
  if (autofillController_ && currentView()) {
    autofillController_->triggerFillForView(currentView());
  }
}

QString BrowserWindow::currentSearchEngine() const {
  if (services_.profileService) {
    return services_.profileService->searchEngine();
  }
  return QSettings().value(QStringLiteral("browser/searchEngine"), QStringLiteral("DuckDuckGo")).toString();
}

void BrowserWindow::setSearchEngine(const QString &engine) {
  if (services_.profileService) {
    services_.profileService->setSearchEngine(engine);
  } else {
    QSettings().setValue(QStringLiteral("browser/searchEngine"), engine);
    updateSearchEngineIcon();
  }
}

void BrowserWindow::updateSearchEngineIcon() {
  const QString engine = currentSearchEngine();
  if (omnibox_) omnibox_->setPlaceholderText(searchEnginePlaceholder(engine));
  const QString json = QString::fromUtf8(QJsonDocument(QJsonArray{engine}).toJson(QJsonDocument::Compact));
  for (const auto &tab : std::as_const(tabs_)) {
    if (tab.view && isNewTabUrl(tab.view->url())) {
      tab.view->page()->runJavaScript(QStringLiteral("if(location.protocol==='dalinira:'&&location.hostname==='newtab'&&window.daliniraSetSearchEngine)window.daliniraSetSearchEngine(%1[0]);").arg(json));
    }
  }
  updateOmniboxLeadingIcon();
}

void BrowserWindow::updateOmniboxLeadingIcon() {
  if (!searchEngineAction_ || !omnibox_) return;
  const int idx = tabStrip_ ? tabStrip_->currentIndex() : -1;
  if (idx < 0 || idx >= tabs_.size()) {
    searchEngineAction_->setIcon(BrowserIcons::searchEngineIcon(currentSearchEngine()));
    searchEngineAction_->setToolTip(I18n::text(QStringLiteral("toolbar.search_engine")).arg(currentSearchEngine()));
    return;
  }

  const auto &tab = tabs_.at(idx);
  const bool isWeb = (!tab.isInternal && !isNewTabUrl(tab.url) && !tab.url.isEmpty() &&
                      (tab.url.scheme() == QLatin1String("http") || tab.url.scheme() == QLatin1String("https") || tab.url.scheme() == QLatin1String("file")));

  if (!isWeb) {
    searchEngineAction_->setIcon(BrowserIcons::searchEngineIcon(currentSearchEngine()));
    searchEngineAction_->setToolTip(I18n::text(QStringLiteral("toolbar.search_engine")).arg(currentSearchEngine()));
    return;
  }

  const QString host = tab.url.host().isEmpty() ? tab.url.toString() : tab.url.host();

  if (tab.activeCamera && tab.activeMicrophone) {
    searchEngineAction_->setIcon(BrowserIcons::combinedMediaCaptureIcon());
    searchEngineAction_->setToolTip(I18n::text(QStringLiteral("toolbar.camera_mic_in_use")).arg(host));
  } else if (tab.activeCamera) {
    searchEngineAction_->setIcon(BrowserIcons::icon(BrowserIcon::Camera));
    searchEngineAction_->setToolTip(I18n::text(QStringLiteral("toolbar.camera_in_use")).arg(host));
  } else if (tab.activeMicrophone) {
    searchEngineAction_->setIcon(BrowserIcons::icon(BrowserIcon::Microphone));
    searchEngineAction_->setToolTip(I18n::text(QStringLiteral("toolbar.mic_in_use")).arg(host));
  } else if (tab.url.scheme() == QLatin1String("http")) {
    searchEngineAction_->setIcon(BrowserIcons::icon(BrowserIcon::InsecureContent));
    searchEngineAction_->setToolTip(I18n::text(QStringLiteral("toolbar.insecure_connection")).arg(host));
  } else {
    searchEngineAction_->setIcon(BrowserIcons::icon(BrowserIcon::Tune));
    searchEngineAction_->setToolTip(I18n::text(QStringLiteral("toolbar.site_info")).arg(host));
  }
}

void BrowserWindow::toggleSiteControlsBubble() {
  const int idx = tabStrip_ ? tabStrip_->currentIndex() : -1;
  if (idx < 0 || idx >= tabs_.size()) return;
  const auto &tab = tabs_.at(idx);
  const bool isWeb = (!tab.isInternal && !isNewTabUrl(tab.url) && !tab.url.isEmpty() &&
                      (tab.url.scheme() == QLatin1String("http") || tab.url.scheme() == QLatin1String("https") || tab.url.scheme() == QLatin1String("file")));
  if (!isWeb) {
    showSettings(SettingsPage::Category::Search);
    return;
  }

  if (siteControlsBubble_ && siteControlsBubble_->isVisible()) {
    dismissSiteControlsBubble();
    return;
  }

  if (!siteControlsBubble_) {
    siteControlsBubble_ = new SiteControlsBubble(this);
    siteControlsBubble_->setProfileService(services_.profileService);
    connect(siteControlsBubble_, &SiteControlsBubble::permissionsResetRequested, this,
            &BrowserWindow::clearAllTabSessionPermissionsForOrigin);
    connect(siteControlsBubble_, &SiteControlsBubble::permissionRuleChanged, this,
            [this](const QString &key, const QString &origin, int choice) {
      if (choice != 0) {
        const int cIdx = tabStrip_ ? tabStrip_->currentIndex() : -1;
        if (cIdx != -1) {
          tabSessionGrants_.remove(TabSessionPermissionKey{tabs_[cIdx].id, origin, key});
        }
      }
    });
  }

  const QString canon = BrowserProfileService::canonicalOrigin(tab.url);
  const uint64_t currentId = tab.id;
  siteControlsBubble_->updateForTab(
      tab.id, tab.url, tab.url.scheme() == QLatin1String("https"),
      tab.activeCamera, tab.activeMicrophone,
      [this, currentId, canon](const QString &permKey) {
        return hasTabSessionPermission(currentId, canon, permKey);
      });

  siteControlsBubble_->show();
  updateSiteControlsBubblePosition();
  siteControlsBubble_->raise();
  siteControlsBubble_->setFocus();
}

void BrowserWindow::dismissSiteControlsBubble() {
  if (siteControlsBubble_ && siteControlsBubble_->isVisible()) {
    siteControlsBubble_->hide();
  }
}

bool BrowserWindow::isInsideSiteControls(QWidget *target, const QPoint &globalPos) const {
  if (!siteControlsBubble_ || !siteControlsBubble_->isVisible()) return false;

  if (target) {
    if (target == siteControlsBubble_ || siteControlsBubble_->isAncestorOf(target)) {
      return true;
    }
    const auto combos = siteControlsBubble_->findChildren<QComboBox *>();
    for (auto *combo : combos) {
      if (!combo) continue;
      QWidget *v = combo->view();
      if (v && (target == v || v->isAncestorOf(target))) {
        return true;
      }
      QWidget *w = v ? v->window() : nullptr;
      if (w && (target == w || w->isAncestorOf(target))) {
        return true;
      }
    }
  }

  const QRect bubbleGlobalRect(siteControlsBubble_->mapToGlobal(QPoint(0, 0)), siteControlsBubble_->size());
  if (bubbleGlobalRect.contains(globalPos)) {
    return true;
  }

  const auto combos = siteControlsBubble_->findChildren<QComboBox *>();
  for (auto *combo : combos) {
    if (!combo) continue;
    QWidget *v = combo->view();
    if (v && v->isVisible()) {
      const QRect vRect(v->mapToGlobal(QPoint(0, 0)), v->size());
      if (vRect.contains(globalPos)) {
        return true;
      }
      QWidget *w = v->window();
      if (w && w->isVisible()) {
        const QRect wRect(w->mapToGlobal(QPoint(0, 0)), w->size());
        if (wRect.contains(globalPos)) {
          return true;
        }
      }
    }
  }

  return false;
}

bool BrowserWindow::eventFilter(QObject *watched, QEvent *event) {
  static thread_local bool inEventFilter = false;
  if (inEventFilter) return false;
  struct FilterGuard {
    bool &flag;
    FilterGuard(bool &f) : flag(f) { flag = true; }
    ~FilterGuard() { flag = false; }
  } filterGuard(inEventFilter);

  if (watched == findButton_ && (event->type() == QEvent::Move || event->type() == QEvent::Resize ||
                                 event->type() == QEvent::Show || event->type() == QEvent::Hide)) {
    if (findBar_ && findBar_->isVisible()) updateFindBarPosition();
  }

  if (watched == suggestionPopup_ &&
      (event->type() == QEvent::Show || event->type() == QEvent::Hide)) {
    omnibox_->setProperty("suggestionsOpen", event->type() == QEvent::Show);
    omnibox_->style()->unpolish(omnibox_);
    omnibox_->style()->polish(omnibox_);
  }

  if (event->type() == QEvent::MouseButtonRelease &&
      static_cast<QMouseEvent *>(event)->button() == Qt::MiddleButton) {
    const QUrl bookmarkUrl = watched->property("dalinira-bookmark-url").toUrl();
    if (bookmarkUrl.isValid()) {
      openBookmarkUrl(bookmarkUrl, true);
      return true;
    }
  }

  if (watched == omnibox_ && event->type() == QEvent::FocusIn && omnibox_->text().trimmed().isEmpty()) {
    QTimer::singleShot(0, this, [this] {
      if (omnibox_ && omnibox_->hasFocus() && omnibox_->text().trimmed().isEmpty()) {
        updateOmniboxSuggestions(QString{});
      }
    });
  }

  if (watched == omnibox_ && event->type() == QEvent::FocusOut) {
    QTimer::singleShot(150, this, [this] {
      if (!omnibox_) return;
      if (suggestionPopup_ && suggestionPopup_->isVisible()) {
        if (!omnibox_->hasFocus() && !suggestionPopup_->hasFocus()) {
          suggestionPopup_->hide();
        }
      }
    });
  }

  if (event->type() == QEvent::KeyPress) {
    auto *keyEvent = static_cast<QKeyEvent *>(event);
    if (keyEvent->key() == Qt::Key_Escape) {
      if (suggestionPopup_ && suggestionPopup_->isVisible()) {
        suggestionPopup_->hide();
        return true;
      }
    }
  }

  if (suggestionPopup_ && suggestionPopup_->isVisible()) {
    if (event->type() == QEvent::MouseButtonPress) {
      auto *mouseEvent = static_cast<QMouseEvent *>(event);
      const QPoint globalPos = mouseEvent->globalPosition().toPoint();
      auto *widget = qobject_cast<QWidget *>(watched);

      bool isOmniOrPopup = false;
      if (widget == omnibox_ || widget == suggestionPopup_ || suggestionPopup_->isAncestorOf(widget)) {
        isOmniOrPopup = true;
      } else {
        if (omnibox_) {
          const QRect omniRect(omnibox_->mapToGlobal(QPoint(0, 0)), omnibox_->size());
          if (omniRect.contains(globalPos)) isOmniOrPopup = true;
        }
        if (suggestionPopup_) {
          const QRect popupRect(suggestionPopup_->mapToGlobal(QPoint(0, 0)), suggestionPopup_->size());
          if (popupRect.contains(globalPos)) isOmniOrPopup = true;
        }
      }

      if (!isOmniOrPopup) {
        suggestionPopup_->hide();
      }
    } else if (event->type() == QEvent::WindowDeactivate) {
      suggestionPopup_->hide();
    }
  }

  if (siteControlsBubble_ && siteControlsBubble_->isVisible()) {
    if (event->type() == QEvent::MouseButtonPress) {
      auto *mouseEvent = static_cast<QMouseEvent *>(event);
      const QPoint globalPos = mouseEvent->globalPosition().toPoint();
      auto *widget = qobject_cast<QWidget *>(watched);

      // If the click is on the omnibox leading action (toggle button), let toggleSiteControlsBubble handle it
      if (searchEngineAction_ && omnibox_) {
        const QPoint omniGlobal = omnibox_->mapToGlobal(QPoint(0, 0));
        const QRect leadingActionRect(omniGlobal.x(), omniGlobal.y(), 42, omnibox_->height());
        if (leadingActionRect.contains(globalPos)) {
          return false;
        }
      }

      if (!isInsideSiteControls(widget, globalPos)) {
        dismissSiteControlsBubble();
        // Do not consume the event; allow it to reach the target widget (e.g. tabs, page, toolbar)
        return false;
      }
    } else if (event->type() == QEvent::WindowDeactivate) {
      if (watched == this) {
        dismissSiteControlsBubble();
      }
    }
  }


  if (watched == this && event->type() == QEvent::WindowDeactivate) {
    if (hasOverrideCursor_) {
      QGuiApplication::restoreOverrideCursor();
      hasOverrideCursor_ = false;
    }
    if (resizing_) {
      resizing_ = false;
      resizeEdges_ = {};
      releaseMouse();
    }
  }

  if (!property("daliniraDragCaptureShell").toBool() &&
      !dalinira::desktop_tabs::TabDragController::instance().isActive() &&
      !isMaximized() && !isFullScreen() && !(windowState() & (Qt::WindowMaximized | Qt::WindowFullScreen))) {
    auto *widget = qobject_cast<QWidget *>(watched);
    if (widget && (widget == this || this->isAncestorOf(widget)) && (!widget->isWindow() || widget == this)) {
      if (event->type() == QEvent::MouseMove) {
        auto *mouseEvent = static_cast<QMouseEvent *>(event);
        const QPoint globalPos = mouseEvent->globalPosition().toPoint();
        if (resizing_) {
          handleManualResize(globalPos);
          return true;
        }
        const QPoint localPos = mapFromGlobal(globalPos);
        if (rect().contains(localPos)) {
          updateCursorShape(localPos);
        } else if (hasOverrideCursor_) {
          QGuiApplication::restoreOverrideCursor();
          hasOverrideCursor_ = false;
        }
      } else if (event->type() == QEvent::MouseButtonPress) {
        auto *mouseEvent = static_cast<QMouseEvent *>(event);
        if (mouseEvent->button() == Qt::LeftButton) {
          const QPoint globalPos = mouseEvent->globalPosition().toPoint();
          const QPoint localPos = mapFromGlobal(globalPos);
          if (rect().contains(localPos)) {
            const Qt::Edges edges = calculateEdges(localPos);
            if (edges != 0) {
              bool started = false;
              if (windowHandle()) {
                started = windowHandle()->startSystemResize(edges);
              }
              if (!started) {
                resizing_ = true;
                resizeEdges_ = edges;
                resizeStartPos_ = globalPos;
                resizeStartGeometry_ = geometry();
                grabMouse();
              }
              return true;
            }
          }
        }
      } else if (event->type() == QEvent::MouseButtonRelease) {
        if (resizing_) {
          resizing_ = false;
          resizeEdges_ = {};
          releaseMouse();
          if (hasOverrideCursor_) {
            QGuiApplication::restoreOverrideCursor();
            hasOverrideCursor_ = false;
          }
          return true;
        }
      } else if (event->type() == QEvent::Leave) {
        if (watched == this && hasOverrideCursor_ && !resizing_) {
          QGuiApplication::restoreOverrideCursor();
          hasOverrideCursor_ = false;
        }
      }
    }
  }

  return QMainWindow::eventFilter(watched, event);
}

// ============================================================================
// INVARIANT (Credential Save Bubble Lifetime & Isolation):
// - Once a valid credential save decision is pending, post-login navigation or
//   redirects MUST NOT dismiss the bubble before explicit user resolution
//   (Save / Not Now / close button).
// - Switching tabs hides and re-shows the bubble appropriately based on the active view.
// - Verified origin remains bound to the candidate throughout decision lifetime.
// - Tab destruction safely cleans pending decision without leaks.
// ============================================================================
void BrowserWindow::dismissCredentialSaveBubble() {
  if (autofillController_) {
    if (auto *bubble = autofillController_->activeSaveBubble()) {
      bubble->clickClose();
    } else {
      autofillController_->dismissSaveBubble();
    }
  }
}

void BrowserWindow::updateSaveBubblePosition() {
  if (!autofillController_ || !autofillController_->activeSaveBubble() || !omnibox_) return;
  auto *bubble = autofillController_->activeSaveBubble();
  if (currentView() != autofillController_->activeBubbleView()) {
    bubble->hide();
    return;
  }
  if (!bubble->isVisible()) return;
  QPoint omniLocal = omnibox_->mapTo(this, QPoint(0, omnibox_->height() + 4));
  omniLocal.setX(std::max(10, (width() - bubble->width()) / 2));
  bubble->move(omniLocal);
  bubble->raise();
}

void BrowserWindow::updateSiteControlsBubblePosition() {
  if (!siteControlsBubble_ || !omnibox_) return;
  QPoint omniLocal = omnibox_->mapTo(this, QPoint(4, omnibox_->height() + 4));
  if (omniLocal.x() + siteControlsBubble_->width() > width() - 10) {
    omniLocal.setX(std::max(10, width() - 10 - siteControlsBubble_->width()));
  }
  if (omniLocal.x() < 10) omniLocal.setX(10);
  siteControlsBubble_->move(omniLocal);
  siteControlsBubble_->raise();
}

void BrowserWindow::toggleTabSearchPopup() {
  if (tabSearchPopup_ && tabSearchPopup_->isVisible()) {
    tabSearchPopup_->close();
    return;
  }
  if (!tabSearchPopup_) {
    tabSearchPopup_ = new dalinira::desktop_tabs::TabSearchPopup(this);
    refreshShortcutHints();
  }
  tabSearchPopup_->showBelow(tabSearchBtn_);
}

void BrowserWindow::toggleTabGroupLauncher() {
  if (tabGroupLauncherPopup_ && tabGroupLauncherPopup_->isVisible()) {
    tabGroupLauncherPopup_->close();
    return;
  }
  if (!tabGroupLauncherPopup_) {
    tabGroupLauncherPopup_ = new dalinira::desktop_tabs::TabGroupLauncherPopup(this);
    connect(tabGroupLauncherPopup_, &dalinira::desktop_tabs::TabGroupLauncherPopup::createGroupRequested,
            this, &BrowserWindow::createNewTabGroupWithNewTab);
  }
  tabGroupLauncherPopup_->showBelow(appsBtn_);
}

void BrowserWindow::createNewTabGroupWithNewTab() {
  if (!groupModel_) return;

  // 1. Create a brand new normal DaliNira tab at the end of tabs using existing creation path
  const int newIdx = addNewTab(QUrl(QStringLiteral("dalinira://newtab/")), -1);
  if (newIdx < 0 || newIdx >= tabs_.size()) return;

  // 2. Generate a new stable Group UID
  const QColor defaultColor = dalinira::desktop_tabs::tabGroupColorPalette().value(0, QColor("#757b82"));
  const QUuid groupId = groupModel_->createGroup(QString(), defaultColor);

  // 3. Associate NEW Tab UID -> Group UID
  auto &newTab = tabs_[newIdx];
  newTab.groupId = groupId;
  groupModel_->setTabGroup(newTab.id, groupId);

  // 4. Update tab strip rendering
  tabStrip_->update();

  // 5. Open TabGroupPopup anchored below the group chip / new tab header
  QPoint targetPos;
  const QRect chipRect = tabStrip_->groupChipRect(groupId);
  if (!chipRect.isEmpty()) {
    targetPos = tabStrip_->mapToGlobal(QPoint(chipRect.left(), tabStrip_->height()));
  } else {
    const QRect tr = tabStrip_->tabRect(newIdx);
    targetPos = tabStrip_->mapToGlobal(QPoint(tr.left(), tabStrip_->height()));
  }

  showTabGroupPopup(groupId, targetPos);
}

void BrowserWindow::createGroupFromExistingTab(uint64_t tabId) {
  if (!groupModel_ || tabId == 0) return;
  const int idx = findIndexByTabId(tabId);
  if (idx < 0 || idx >= tabs_.size()) return;

  auto &tab = tabs_[idx];
  QUuid groupId;
  if (tab.groupId.has_value() && groupModel_->hasGroup(*tab.groupId)) {
    groupId = *tab.groupId;
  } else {
    const QColor defaultColor = dalinira::desktop_tabs::tabGroupColorPalette().value(0, QColor("#757b82"));
    groupId = groupModel_->createGroup(QString(), defaultColor);
    tab.groupId = groupId;
    groupModel_->setTabGroup(tab.id, groupId);
    tabStrip_->update();
  }

  QPoint targetPos;
  const QRect chipRect = tabStrip_->groupChipRect(groupId);
  if (!chipRect.isEmpty()) {
    targetPos = tabStrip_->mapToGlobal(QPoint(chipRect.left(), tabStrip_->height()));
  } else {
    const QRect tr = tabStrip_->tabRect(idx);
    targetPos = tabStrip_->mapToGlobal(QPoint(tr.left(), tabStrip_->height()));
  }

  showTabGroupPopup(groupId, targetPos);
}

void BrowserWindow::showTabGroupPopup(const QUuid &groupId, const QPoint &globalPos) {
  if (!groupModel_ || groupId.isNull()) return;
  if (!tabGroupPopup_) {
    tabGroupPopup_ = new dalinira::desktop_tabs::TabGroupPopup(groupModel_, this);
    connect(tabGroupPopup_, &dalinira::desktop_tabs::TabGroupPopup::newTabInGroupRequested,
            this, &BrowserWindow::addTabToGroup);
    connect(tabGroupPopup_, &dalinira::desktop_tabs::TabGroupPopup::moveGroupToNewWindowRequested,
            this, &BrowserWindow::moveGroupToNewWindow);
    connect(tabGroupPopup_, &dalinira::desktop_tabs::TabGroupPopup::closeGroupRequested,
            this, &BrowserWindow::closeTabGroup);
    connect(tabGroupPopup_, &dalinira::desktop_tabs::TabGroupPopup::ungroupRequested,
            this, &BrowserWindow::ungroupTabs);
    connect(tabGroupPopup_, &dalinira::desktop_tabs::TabGroupPopup::deleteGroupRequested,
            this, &BrowserWindow::deleteTabGroup);
  }

  QPoint targetPos = globalPos;
  if (targetPos.isNull()) {
    const QRect chipRect = tabStrip_->groupChipRect(groupId);
    if (!chipRect.isEmpty()) {
      targetPos = tabStrip_->mapToGlobal(QPoint(chipRect.left(), tabStrip_->height()));
    } else {
      const int idx = tabStrip_->currentIndex();
      const QRect tr = tabStrip_->tabRect(idx >= 0 ? idx : 0);
      targetPos = tabStrip_->mapToGlobal(QPoint(tr.left(), tabStrip_->height()));
    }
  }

  tabGroupPopup_->showForGroup(groupId, targetPos);
}

void BrowserWindow::addTabToGroup(const QUuid &groupId) {
  if (!groupModel_ || groupId.isNull()) return;

  int lastGroupIdx = -1;
  for (int i = 0; i < tabs_.size(); ++i) {
    if (tabs_[i].groupId == groupId) {
      lastGroupIdx = i;
    }
  }

  const int insertIdx = (lastGroupIdx >= 0) ? lastGroupIdx + 1 : tabs_.size();
  const int newIdx = addNewTab(QUrl(QStringLiteral("dalinira://newtab/")), insertIdx);
  if (newIdx >= 0 && newIdx < tabs_.size()) {
    tabs_[newIdx].groupId = groupId;
    groupModel_->setTabGroup(tabs_[newIdx].id, groupId);
    tabStrip_->update();
  }
}

void BrowserWindow::moveGroupToNewWindow(const QUuid &groupId) {
  if (!groupModel_ || groupId.isNull()) return;
  const auto optGroup = groupModel_->group(groupId);
  if (!optGroup.has_value()) return;

  const QList<uint64_t> memberTabIds = groupModel_->tabsInGroup(groupId);
  if (memberTabIds.isEmpty()) return;

  const dalinira::desktop_tabs::TabGroup groupToMove = *optGroup;

  auto *newWindow = createWindow(services_);
  newWindow->groupModel()->addOrUpdateGroup(groupToMove);

  for (uint64_t tid : memberTabIds) {
    const int idx = findIndexByTabId(tid);
    if (idx < 0 || idx >= tabs_.size()) continue;

    BrowserTabInfo info = tabs_.takeAt(idx);
    if (info.view) {
      info.view->disconnect(this);
      pageStack_->removeWidget(info.view);
    } else if (info.content) {
      info.content->disconnect(this);
      pageStack_->removeWidget(info.content);
    }
    tabStrip_->removeTab(idx);
    groupModel_->removeTabFromGroup(info.id);

    info.groupId = groupId;
    newWindow->adoptTab(info, -1);
    newWindow->groupModel()->setTabGroup(info.id, groupId);
  }

  groupModel_->removeGroup(groupId);

  if (tabs_.isEmpty()) {
    if (!isCaptureShell_) {
      close();
    } else {
      hide();
    }
  } else {
    const int current = std::clamp(tabStrip_->currentIndex(), 0, static_cast<int>(tabs_.size()) - 1);
    switchTab(current);
  }

  newWindow->show();
  newWindow->raise();
}

void BrowserWindow::closeTabGroup(const QUuid &groupId) {
  if (!groupModel_ || groupId.isNull()) return;
  const QList<uint64_t> memberTabIds = groupModel_->tabsInGroup(groupId);
  for (uint64_t tid : memberTabIds) {
    const int idx = findIndexByTabId(tid);
    if (idx >= 0) {
      closeTab(idx);
    }
  }
  groupModel_->removeGroup(groupId);
  tabStrip_->update();
}

void BrowserWindow::ungroupTabs(const QUuid &groupId) {
  if (!groupModel_ || groupId.isNull()) return;
  for (auto &tab : tabs_) {
    if (tab.groupId == groupId) {
      tab.groupId = std::nullopt;
    }
  }
  groupModel_->removeGroup(groupId);
  tabStrip_->update();
}

void BrowserWindow::deleteTabGroup(const QUuid &groupId) {
  closeTabGroup(groupId);
}

std::optional<dalinira::desktop_tabs::TabGroup> BrowserWindow::groupForTab(uint64_t tabId) const {
  if (!groupModel_) return std::nullopt;
  const auto optGid = groupModel_->groupIdForTab(tabId);
  if (!optGid.has_value() || optGid->isNull()) return std::nullopt;
  return groupModel_->group(*optGid);
}

void BrowserWindow::onTabHovered(int index, const QPoint &globalPos, const QRect &globalTabRect) {
  Q_UNUSED(globalPos);
  if (index < 0 || index >= tabs_.size()) {
    if (hoverCard_) hoverCard_->hideCard();
    return;
  }
  if (dalinira::desktop_tabs::TabDragController::instance().isActive()) {
    if (hoverCard_) hoverCard_->hideCard();
    return;
  }

  const auto &info = tabs_[index];
  if (!hoverCard_) {
    hoverCard_ = new TabHoverCard(this);
  }

  // One card for both engines: it renders the shared resource model.
  const QUuid tabUuid = info.uuid;
  hoverCard_->showForTab(info.title, info.url, tabIconForRecord(info), globalTabRect,
                         [this, tabUuid] {
                           return services_.tabManager ? services_.tabManager->resourceSnapshot(tabUuid)
                                                       : dalinira::TabResourceSnapshot{};
                         },
                         info.isInternal);
}

void BrowserWindow::onTabHoverLeave() {
  if (hoverCard_) {
    hoverCard_->hideCard();
  }
}

void BrowserWindow::onThrobberTick() {
  if (!tabStrip_) return;
  auto &throbber = TabThrobber::instance();
  const int activeIndex = tabStrip_->currentIndex();
  const qreal dpr = devicePixelRatioF();
  const QPalette pal = palette();

  for (int i = 0; i < tabs_.size(); ++i) {
    const QObject *viewObj = tabs_[i].engineView
        ? static_cast<const QObject *>(tabs_[i].engineView.data())
        : static_cast<const QObject *>(tabs_[i].view.data());
    if (!viewObj) continue;

    if (throbber.isThrobberVisible(viewObj)) {
      const bool isActive = (i == activeIndex);
      const QIcon throbberIcon = TabThrobber::renderThrobberIcon(throbber.frameStep(), pal, isActive, dpr, 16);
      tabStrip_->setTabIcon(i, throbberIcon);
    }
  }
}

QJsonArray BrowserWindow::searchRows(const QString &query, const QStringList &remote) const {
  QJsonArray rows;
  if (query.size() > 256) return rows;
  const QString cleanQuery = query.trimmed();
  QSet<QString> seen;
  auto append = [&](const QString &text, const QUrl &url, const QString &type) {
    if (rows.size() >= 12 || !url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty() ||
        (url.scheme() != QLatin1String("https") && url.scheme() != QLatin1String("http"))) return;
    const QString key = url.toString(QUrl::FullyEncoded);
    if (seen.contains(key)) {
      // Preserve the history identity when it duplicates the typed action.
      for (int i = 0; i < rows.size(); ++i) {
        auto existing = rows[i].toObject();
        const bool savedSearch = type == QLatin1String("search-history") && existing.value("type") == QLatin1String("search");
        const bool visitedSite = (type == QLatin1String("history") || type == QLatin1String("frequent")) &&
                                 existing.value("type") == QLatin1String("site");
        if (existing.value("url") == key && (savedSearch || visitedSite)) {
          existing.insert("type", type);
          existing.insert("removable", true);
          rows[i] = existing;
          break;
        }
      }
      return;
    }
    seen.insert(key);
    QJsonObject row{{"text",text.left(256)},{"url",key},{"type",type},
                    {"removable", usesPersistentProfile() && (type == QLatin1String("history") ||
                        type == QLatin1String("frequent") || type == QLatin1String("search-history"))}};
    if (type != QLatin1String("remote") && type != QLatin1String("search") &&
        type != QLatin1String("search-history"))
      row.insert(QStringLiteral("icon"), newTabFaviconUrl(services_.profileService, url));
    rows.append(row);
  };
  const auto search = [&](const QString &text, const QString &type) {
    const QUrl url = services_.profileService
        ? services_.profileService->searchUrlForEngine(currentSearchEngine(), text)
        : dalinira::core::AddressInputResolver::searchUrlForEngine(currentSearchEngine(), text);
    append(text, url, type);
  };
  if (!cleanQuery.isEmpty()) {
    const auto resolution = dalinira::core::AddressInputResolver::resolve(
        cleanQuery, currentSearchEngine(), QLocale::system(), candidateProvider_.get());
    if (resolution.classification != dalinira::core::AddressInputClassification::Search &&
        resolution.url.isValid() && (resolution.url.scheme() == QLatin1String("https") || resolution.url.scheme() == QLatin1String("http"))) {
      append(cleanQuery, resolution.url, QStringLiteral("site"));
    }
  }
  if (!cleanQuery.isEmpty()) search(cleanQuery, QStringLiteral("search"));
  // Local data is available only to its owning regular profile.
  if (usesPersistentProfile()) {
    for (const QString &savedQuery : services_.profileService->recentSearches(cleanQuery, 8)) {
      search(savedQuery, QStringLiteral("search-history"));
    }
    for (const auto &tab : tabs_) {
      if (!cleanQuery.isEmpty() && (tab.title.contains(cleanQuery, Qt::CaseInsensitive) || tab.url.host().contains(cleanQuery, Qt::CaseInsensitive)))
        append(tab.title.isEmpty() ? tab.url.host() : tab.title, tab.url, QStringLiteral("tab"));
      if (rows.size() >= 3) break;
    }
    for (const auto &url : services_.profileService->bookmarks()) {
      if (!cleanQuery.isEmpty() && url.host().contains(cleanQuery, Qt::CaseInsensitive)) append(url.host(), url, QStringLiteral("bookmark"));
      if (rows.size() >= 4) break;
    }
    for (const auto &site : services_.profileService->frequentSites(30)) {
      if (!cleanQuery.isEmpty() && (site.title.contains(cleanQuery, Qt::CaseInsensitive) || site.url.host().contains(cleanQuery, Qt::CaseInsensitive)))
        append(site.title.isEmpty() ? site.url.host() : site.title, site.url, QStringLiteral("frequent"));
      if (rows.size() >= 5) break;
    }
    for (const auto &entry : services_.profileService->recentHistory()) {
      if (!cleanQuery.isEmpty() && (entry.title.contains(cleanQuery, Qt::CaseInsensitive) || entry.url.host().contains(cleanQuery, Qt::CaseInsensitive)))
        append(entry.title.isEmpty() ? entry.url.host() : entry.title, entry.url, QStringLiteral("history"));
      if (rows.size() >= 6) break;
    }
  }
  if (!cleanQuery.isEmpty()) for (const auto &text : remote) search(text, QStringLiteral("remote"));
  return rows;
}

void BrowserWindow::applyTabFavicon(int idx, const QObject *throbberKey, const QIcon &pageIcon, bool engineReported) {
  if (idx < 0 || idx >= tabs_.size()) return;
  BrowserTabInfo &tab = tabs_[idx];
  const QString scheme = tab.url.scheme().toLower();
  const bool internalPage = tab.isInternal || tab.url.isEmpty() || isNewTabUrl(tab.url) ||
                            scheme == QLatin1String("dalinira") || scheme == QLatin1String("about");
  const QString host = tab.url.host().toLower();
  if (tab.internalId == QLatin1String("local-media-player")) {
    if (tab.icon.isNull()) {
      const LocalMediaPlayerKind kind = LocalMediaRouting::classify(tab.url.toLocalFile(), QString{});
      tab.icon = BrowserIcons::icon(kind == LocalMediaPlayerKind::Audio ? BrowserIcon::Music : BrowserIcon::Video);
    }
    tab.faviconHost.clear();
  } else if (internalPage) {
    tab.icon = isIncognito() ? BrowserIcons::incognitoIcon() : BrowserIcons::appIcon();
    tab.faviconHost.clear();
  } else if (!pageIcon.isNull()) {
    tab.icon = pageIcon;
    tab.faviconHost = host;
    tab.pageIconReported = true;
    cacheNewTabFavicon(services_.profileService, tab.url, pageIcon);
    TabThrobber::instance().cacheFavicon(throbberKey, pageIcon);
    updateBookmarkIconsForUrl(tab.url, pageIcon);
  } else if (scheme == QLatin1String("file")) {
    const LocalMediaPlayerKind kind = LocalMediaRouting::classify(tab.url.toLocalFile(), QString{});
    if (kind == LocalMediaPlayerKind::Audio) {
      tab.icon = BrowserIcons::icon(BrowserIcon::Music);
    } else if (kind == LocalMediaPlayerKind::Video) {
      tab.icon = BrowserIcons::icon(BrowserIcon::Video);
    } else {
      tab.icon = platformIconForBookmark(tab.url, services_.profileService);
    }
    tab.faviconHost.clear();
  } else if (engineReported || tab.icon.isNull() || tab.faviconHost != host) {
    // Keep a real favicon across reloads of the same site; never show the
    // previous site's favicon or the DaliNira icon for a web page. When the
    // engine reports that this page has no favicon, show the generic site
    // icon rather than another page's icon cached for the same origin.
    tab.icon = engineReported ? platformIconForBookmark(tab.url) : bookmarkFavicon(tab.url);
    // Remember the engine's "no favicon" answer so the load-finished refresh
    // for the same host keeps the generic icon instead of the cached one.
    tab.faviconHost = engineReported ? host : QString();
  }
  if (tabStrip_ && !TabThrobber::instance().isLoading(throbberKey)) tabStrip_->setTabIcon(idx, tab.icon);
  if (services_.tabManager && !tab.uuid.isNull()) services_.tabManager->updateIcon(tab.uuid, tab.icon);
  updateSidePanelTabs();
}

void BrowserWindow::activateSuggestion(const QUrl &url, const QString &text, const QString &type) {
  if (suggestionActivated_ || !url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty() ||
      (url.scheme() != QLatin1String("https") && url.scheme() != QLatin1String("http"))) return;
  suggestionActivated_ = true;
  QTimer::singleShot(0, this, [this] { suggestionActivated_ = false; });
  suggestionCompleter_->popup()->hide();
  if (type == QLatin1String("search") || type == QLatin1String("remote") ||
      type == QLatin1String("search-history")) {
    activateSearchSuggestion(text);
    return;
  }
  // Site/tab/bookmark/history rows already carry their destination URL.
  navigateCurrentTab(url, type == QLatin1String("site"));
}

void BrowserWindow::updateOmniboxSuggestions(const QString &query) {
  if (!services_.profileService || !services_.profileService->searchSuggestions()->isEnabled()) {
    if (suggestionModel_) suggestionModel_->clear();
    if (suggestionCompleter_ && suggestionCompleter_->popup()) suggestionCompleter_->popup()->hide();
    return;
  }
  const QPointer<BrowserWindow> guard(this);
  const QString engine = currentSearchEngine();
  auto render = [guard, query, engine](const QStringList &remote) {
    if (!guard || !guard->omnibox_->hasFocus() || guard->omnibox_->text() != query || guard->currentSearchEngine() != engine) return;
    guard->suggestionModel_->clear();
    for (const auto &value : guard->searchRows(query, remote)) {
      const auto row = value.toObject();
      const QString type = row.value("type").toString();
      const bool history = SuggestionRemoveDelegate::usesHistoryIcon(type);
      const QIcon icon = history ? BrowserIcons::icon(BrowserIcon::History)
          : (type == QLatin1String("search") || type == QLatin1String("remote"))
              ? BrowserIcons::searchEngineIcon(engine)
              : BrowserIcons::icon(type == QLatin1String("bookmark") ? BrowserIcon::Bookmark : BrowserIcon::Window);
      auto *item = new QStandardItem(icon, row.value("text").toString());
      item->setData(QUrl(row.value("url").toString()), Qt::UserRole + 1);
      item->setData(row.value("type").toString(), Qt::UserRole + 2);
      item->setData(row.value("removable").toBool(), SuggestionRemoveDelegate::RemovableRole);
      guard->suggestionModel_->appendRow(item);
      if (!history && type != QLatin1String("remote") && type != QLatin1String("search")) {
        const QString key = row.value("url").toString();
        if (const auto *cached = guard->suggestionIconCache_.object(key)) {
          if (!cached->isNull()) item->setIcon(*cached);
        } else if (guard->services_.profile && guard->pendingSuggestionIcons_ < 16) {
          ++guard->pendingSuggestionIcons_;
          const QPersistentModelIndex index(item->index());
          guard->services_.profile->requestIconForPageURL(QUrl(key), 24,
            [guard,index,key](const QIcon &icon, const QUrl &, const QUrl &) {
              if (!guard) return;
              --guard->pendingSuggestionIcons_;
              guard->suggestionIconCache_.insert(key, new QIcon(icon));
              if (index.isValid() && !icon.isNull()) guard->suggestionModel_->setData(index, icon, Qt::DecorationRole);
            });
        }
      }
    }
    if (guard->suggestionModel_->rowCount())
      guard->suggestionCompleter_->complete(QRect(0, 0, guard->omnibox_->width(), guard->omnibox_->height()));
    else guard->suggestionCompleter_->popup()->hide();
  };
  render({});
  if (services_.profileService) services_.profileService->searchSuggestions()->request(
      this, query, engine, !usesPersistentProfile(), render);
}

void BrowserWindow::requestNewTabSuggestions(QWebEnginePage *page, const QString &query, int requestId) {
  if (!page || !services_.profileService || !currentView() || currentView()->page() != page ||
      !isNewTabUrl(page->url())) return;
  const QPointer<QWebEnginePage> target(page);
  const QString capability = page->property("dalinira-suggest-capability").toString();
  NewTabSuggestionTarget suggestionTarget;
  suggestionTarget.owner = page;
  suggestionTarget.privateMode =
      page->profile()->isOffTheRecord() || page->profile() != services_.profileService->profile();
  suggestionTarget.isCurrent = [target, capability] {
    return target && isNewTabUrl(target->url()) &&
           target->property("dalinira-suggest-capability").toString() == capability;
  };
  suggestionTarget.runScript = [target](const QString &script) {
    if (target) target->runJavaScript(script);
  };
  deliverNewTabSuggestions(suggestionTarget, query, requestId);
}

void BrowserWindow::requestEngineNewTabSuggestions(dalinira::engine::EngineView *view,
                                                   const QString &query, int requestId) {
  if (!view || !services_.profileService || currentEngineView() != view || !isNewTabUrl(view->url())) return;
  const QPointer<dalinira::engine::EngineView> target(view);
  const QPointer<BrowserWindow> guard(this);
  NewTabSuggestionTarget suggestionTarget;
  suggestionTarget.owner = view;
  suggestionTarget.privateMode = isIncognito() || services_.profileService->isPrivate();
  suggestionTarget.isCurrent = [guard, target] {
    return guard && target && guard->currentEngineView() == target && isNewTabUrl(target->url());
  };
  suggestionTarget.runScript = [target](const QString &script) {
    if (target) target->runJavaScript(script);
  };
  deliverNewTabSuggestions(suggestionTarget, query, requestId);
}

void BrowserWindow::deliverNewTabSuggestions(const NewTabSuggestionTarget &target,
                                             const QString &query, int requestId) {
  if (!target.owner || !services_.profileService || !services_.profileService->searchSuggestions()->isEnabled() ||
      query.trimmed().isEmpty()) return;
  const QPointer<QObject> owner(target.owner);
  const QPointer<BrowserWindow> guard(this);
  const QString engine = currentSearchEngine();
  owner->setProperty("dalinira-suggest-id", requestId);
  auto render = [guard,owner,target,engine,query,requestId](const QStringList &remote) {
    if (!guard || !owner || !target.isCurrent() || owner->property("dalinira-suggest-id").toInt()!=requestId ||
        guard->currentSearchEngine()!=engine) return;
    const QJsonArray rows = guard->searchRows(query, remote);
    bool hasSuggestions = false;
    for (const auto &val : rows) {
      const auto obj = val.toObject();
      const QString type = obj.value(QStringLiteral("type")).toString();
      if (type != QLatin1String("search") || obj.value(QStringLiteral("text")).toString().trimmed() != query.trimmed()) {
        hasSuggestions = true;
        break;
      }
    }
    const QString json = QString::fromUtf8(QJsonDocument(QJsonArray{requestId,query,hasSuggestions ? rows : QJsonArray{}}).toJson(QJsonDocument::Compact));
    target.runScript(QStringLiteral("if(window.daliniraShowSuggestions)window.daliniraShowSuggestions(...%1);").arg(json));
  };
  const QJsonArray localRows = searchRows(query, {});
  bool hasLocal = false;
  for (const auto &val : localRows) {
    if (val.toObject().value(QStringLiteral("type")).toString() != QLatin1String("search")) {
      hasLocal = true;
      break;
    }
  }
  if (hasLocal) {
    render({});
  }
  services_.profileService->searchSuggestions()->request(owner, query, engine, target.privateMode, render);
}

void BrowserWindow::handleEngineSuggestCommand(dalinira::engine::EngineView *sourceView, const QUrl &url) {
  auto *service = services_.profileService;
  if (!service) return;
  const auto command = NewTabSuggestCommand::parse(url);
  const bool privateWindow = isIncognito() || service->isPrivate();
  using Op = NewTabSuggestCommand::Op;
  if (command.op == Op::Consent && !privateWindow) {
    service->setSearchSuggestionsEnabled(command.enabled);
  } else if (command.op == Op::Query) {
    requestEngineNewTabSuggestions(sourceView, command.text, command.id);
  } else if (command.op == Op::Activate) {
    const QPointer<BrowserWindow> guard(this);
    const QPointer<dalinira::engine::EngineView> view(sourceView);
    const QString text = command.text;
    QMetaObject::invokeMethod(this, [guard, view, text] {
      if (guard && view && guard->currentEngineView() == view && isNewTabUrl(view->url()))
        guard->activateSearchSuggestion(text);
    }, Qt::QueuedConnection);
  } else if ((command.op == Op::DeleteHistory || command.op == Op::DeleteSuggestion) && !privateWindow) {
    if (service->removeSuggestion(command.op == Op::DeleteHistory ? QStringLiteral("search-history") : command.type,
                                  command.text, command.url))
      requestEngineNewTabSuggestions(sourceView, command.input, command.id);
  }
}

bool BrowserWindow::beginForgetClosedView(QWebEngineView *view) {
  if (!view || !services_.profileService ||
      !services_.profileService->blockerService()->shouldForgetClosedHost(view->url().host())) return false;
  pageStack_->removeWidget(view);
  view->hide();
  view->setParent(nullptr);
  // Finish bounded, origin-local cleanup before the page/profile is destroyed.
  // A reload or same-document route does not invoke this path.
  const auto quitLock = std::make_shared<QEventLoopLocker>();
  connect(view, &QObject::destroyed, [owner=services_.privateProfileOwner,quitLock] {});
  view->page()->runJavaScript(QStringLiteral(R"JS((()=>{
    try{localStorage.clear();sessionStorage.clear()}catch(_){}
    const tasks=[];
    try{tasks.push(indexedDB.databases().then(list=>Promise.all(list.slice(0,256).map(db=>new Promise(resolve=>{const request=indexedDB.deleteDatabase(db.name);request.onsuccess=request.onerror=request.onblocked=resolve})))))}catch(_){}
    try{tasks.push(caches.keys().then(keys=>Promise.all(keys.slice(0,256).map(key=>caches.delete(key)))))}catch(_){}
    try{tasks.push(navigator.serviceWorker.getRegistrations().then(list=>Promise.all(list.slice(0,256).map(reg=>reg.unregister()))))}catch(_){}
    Promise.allSettled(tasks).then(()=>{window.__daliniraForgetDone=true});
  })())JS"), QWebEngineScript::ApplicationWorld);
  const QPointer<QWebEngineView> guard(view);
  auto *timer = new QTimer(view);
  timer->setInterval(200);
  connect(timer, &QTimer::timeout, view, [guard,timer] {
    timer->stop();
    if (!guard) return;
    guard->page()->runJavaScript(QStringLiteral("window.__daliniraForgetDone===true"), QWebEngineScript::ApplicationWorld,
      [guard,timer](const QVariant &done) { if (!guard) return; if (done.toBool()) guard->deleteLater(); else timer->start(); });
  });
  timer->start();
  QTimer::singleShot(2000, view, &QObject::deleteLater);
  return true;
}

void BrowserWindow::updatePermissionBubblePosition() {
  if (!permissionBubble_ || !permissionBubble_->isVisible() || !omnibox_) return;
  const QPoint omniLocal = omnibox_->mapTo(this, QPoint(12, omnibox_->height() + 4));
  permissionBubble_->move(omniLocal);
  permissionBubble_->raise();
}

bool BrowserWindow::hasTabSessionPermission(uint64_t tabId, const QString &canonicalOrigin, const QString &permissionKey) const {
  return tabSessionGrants_.contains(TabSessionPermissionKey{tabId, canonicalOrigin, permissionKey});
}

void BrowserWindow::clearTabSessionPermissions(uint64_t tabId) {
  QList<TabSessionPermissionKey> removedGrants;
  for (auto it = tabSessionGrants_.begin(); it != tabSessionGrants_.end();) {
    if (it->tabId == tabId) {
      removedGrants.append(*it);
      it = tabSessionGrants_.erase(it);
    } else {
      ++it;
    }
  }

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
  bool anyReset = false;
  for (const auto &grant : removedGrants) {
    bool otherTabHasGrant = false;
    for (const auto &remaining : tabSessionGrants_) {
      if (remaining.canonicalOrigin == grant.canonicalOrigin &&
          remaining.permissionKey == grant.permissionKey) {
        otherTabHasGrant = true;
        break;
      }
    }
    if (otherTabHasGrant) continue;

    bool hasPersistentAllow = false;
    if (services_.profileService) {
      hasPersistentAllow = services_.profileService->hasSitePermissionRule(grant.permissionKey, grant.canonicalOrigin);
    }

    if (!hasPersistentAllow) {
      QWebEngineProfile *targetProfile = services_.profile
          ? services_.profile
          : (services_.profileService ? services_.profileService->profile() : QWebEngineProfile::defaultProfile());
      if (targetProfile) {
        const QWebEnginePermission::PermissionType pType =
            BrowserProfileService::permissionTypeFromKey(grant.permissionKey);
        if (pType != QWebEnginePermission::PermissionType::Unsupported) {
          const QUrl targetUrl(grant.canonicalOrigin.startsWith(QStringLiteral("http"))
                                   ? grant.canonicalOrigin
                                   : QStringLiteral("https://") + grant.canonicalOrigin);
          QWebEnginePermission p = targetProfile->queryPermission(targetUrl, pType);
          if (p.isValid()) {
            p.reset();
            anyReset = true;
          }
        }
      }
    }
  }
  if (anyReset && services_.profileService) {
    emit services_.profileService->permissionsPolicyChanged();
  }
#endif
}

void BrowserWindow::clearTabSessionPermissionsForOrigin(uint64_t tabId, const QString &canonicalOrigin) {
  QList<TabSessionPermissionKey> removedGrants;
  for (auto it = tabSessionGrants_.begin(); it != tabSessionGrants_.end();) {
    if (it->tabId == tabId && it->canonicalOrigin != canonicalOrigin) {
      removedGrants.append(*it);
      it = tabSessionGrants_.erase(it);
    } else {
      ++it;
    }
  }

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
  bool anyReset = false;
  for (const auto &grant : removedGrants) {
    bool otherTabHasGrant = false;
    for (const auto &remaining : tabSessionGrants_) {
      if (remaining.canonicalOrigin == grant.canonicalOrigin &&
          remaining.permissionKey == grant.permissionKey) {
        otherTabHasGrant = true;
        break;
      }
    }
    if (otherTabHasGrant) continue;

    bool hasPersistentAllow = false;
    if (services_.profileService) {
      hasPersistentAllow = services_.profileService->hasSitePermissionRule(grant.permissionKey, grant.canonicalOrigin);
    }

    if (!hasPersistentAllow) {
      QWebEngineProfile *targetProfile = services_.profile
          ? services_.profile
          : (services_.profileService ? services_.profileService->profile() : QWebEngineProfile::defaultProfile());
      if (targetProfile) {
        const QWebEnginePermission::PermissionType pType =
            BrowserProfileService::permissionTypeFromKey(grant.permissionKey);
        if (pType != QWebEnginePermission::PermissionType::Unsupported) {
          const QUrl targetUrl(grant.canonicalOrigin.startsWith(QStringLiteral("http"))
                                   ? grant.canonicalOrigin
                                   : QStringLiteral("https://") + grant.canonicalOrigin);
          QWebEnginePermission p = targetProfile->queryPermission(targetUrl, pType);
          if (p.isValid()) {
            p.reset();
            anyReset = true;
          }
        }
      }
    }
  }
  if (anyReset && services_.profileService) {
    emit services_.profileService->permissionsPolicyChanged();
  }
#endif
}

void BrowserWindow::grantTabSessionPermission(uint64_t tabId, const QString &canonicalOrigin, const QString &permissionKey) {
  tabSessionGrants_.insert(TabSessionPermissionKey{tabId, canonicalOrigin, permissionKey});
}

void BrowserWindow::clearAllTabSessionPermissionsForOrigin(const QString &canonicalOrigin) {
  for (auto it = tabSessionGrants_.begin(); it != tabSessionGrants_.end();) {
    if (it->canonicalOrigin == canonicalOrigin) {
      it = tabSessionGrants_.erase(it);
    } else {
      ++it;
    }
  }

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
  QWebEngineProfile *targetProfile = services_.profile
      ? services_.profile
      : (services_.profileService ? services_.profileService->profile() : QWebEngineProfile::defaultProfile());
  if (targetProfile) {
    const QUrl originUrl(canonicalOrigin.startsWith(QStringLiteral("http")) ? canonicalOrigin : QStringLiteral("https://") + canonicalOrigin);
    if (originUrl.isValid()) {
      static const QWebEnginePermission::PermissionType kTypes[] = {
        QWebEnginePermission::PermissionType::Geolocation,
        QWebEnginePermission::PermissionType::MediaVideoCapture,
        QWebEnginePermission::PermissionType::MediaAudioCapture,
        QWebEnginePermission::PermissionType::MediaAudioVideoCapture,
        QWebEnginePermission::PermissionType::Notifications,
        QWebEnginePermission::PermissionType::ClipboardReadWrite,
        QWebEnginePermission::PermissionType::LocalFontsAccess,
        QWebEnginePermission::PermissionType::MouseLock,
        QWebEnginePermission::PermissionType::DesktopVideoCapture
      };
      for (auto t : kTypes) {
        QWebEnginePermission p = targetProfile->queryPermission(originUrl, t);
        if (p.isValid()) p.reset();
      }
    }
  }
  if (services_.profileService) {
    emit services_.profileService->permissionsPolicyChanged();
  }
#endif
}

void BrowserWindow::setTabActiveMediaForTesting(int tabIndex, bool camera, bool mic) {
  if (tabIndex >= 0 && tabIndex < tabs_.size()) {
    tabs_[tabIndex].activeCamera = camera;
    tabs_[tabIndex].activeMicrophone = mic;
    updateOmniboxLeadingIcon();
    if (siteControlsBubble_ && siteControlsBubble_->isVisible()) {
      const auto &t = tabs_[tabIndex];
      const QString cOrig = BrowserProfileService::canonicalOrigin(t.url);
      const uint64_t tId = t.id;
      siteControlsBubble_->updateForTab(
          t.id, t.url, t.url.scheme() == QLatin1String("https"),
          t.activeCamera, t.activeMicrophone,
          [this, tId, cOrig](const QString &permKey) {
            return hasTabSessionPermission(tId, cOrig, permKey);
          });
    }
  }
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
void BrowserWindow::syncProfilePermissionsForTab(uint64_t tabId) {
  if (dalinira::engine::EngineBackend::selectedEngineType() != dalinira::engine::EngineType::Chromium) return;
  QWebEngineProfile *targetProfile = services_.profile
      ? services_.profile
      : (services_.profileService ? services_.profileService->profile() : QWebEngineProfile::defaultProfile());
  if (!targetProfile) return;

  for (const auto &grant : tabSessionGrants_) {
    if (services_.profileService &&
        services_.profileService->hasSitePermissionRule(grant.permissionKey, grant.canonicalOrigin)) {
      continue;
    }

    const QWebEnginePermission::PermissionType pType =
        BrowserProfileService::permissionTypeFromKey(grant.permissionKey);
    if (pType == QWebEnginePermission::PermissionType::Unsupported) continue;

    const QUrl targetUrl(grant.canonicalOrigin.startsWith(QStringLiteral("http"))
                             ? grant.canonicalOrigin
                             : QStringLiteral("https://") + grant.canonicalOrigin);
    QWebEnginePermission p = targetProfile->queryPermission(targetUrl, pType);
    if (!p.isValid()) continue;

    if (hasTabSessionPermission(tabId, grant.canonicalOrigin, grant.permissionKey)) {
      if (p.state() != QWebEnginePermission::State::Granted) {
        p.grant();
      }
    } else {
      if (p.state() != QWebEnginePermission::State::Ask) {
        p.reset();
      }
    }
  }
}

void BrowserWindow::handleTabPermissionRequested(QWebEngineView *view, const QWebEnginePermission &permission) {
  if (!permission.isValid()) return;
  if (permission.state() == QWebEnginePermission::State::Granted ||
      permission.state() == QWebEnginePermission::State::Denied) {
    return;
  }
  if (!view || !view->page()) {
    permission.deny();
    return;
  }

  const QUrl origin = permission.origin();
  if (!BrowserProfileService::isPermissibleWebOrigin(origin)) {
    if (!BrowserProfileService::isTrustedInternalScheme(origin)) {
      permission.deny();
      return;
    }
  }

  uint64_t viewTabId = 0;
  for (const auto &tab : tabs_) {
    if (tab.view == view) {
      viewTabId = tab.id;
      break;
    }
  }
  if (viewTabId == 0) {
    permission.deny();
    return;
  }

  const QString canon = BrowserProfileService::canonicalOrigin(origin);
  const QString permKey = BrowserProfileService::permissionKeyFromType(permission.permissionType());

  // 1. Check tab-scoped session permission
  const bool hasTemp = hasTabSessionPermission(viewTabId, canon, permKey) ||
                       (permission.permissionType() == QWebEnginePermission::PermissionType::MediaAudioVideoCapture &&
                        hasTabSessionPermission(viewTabId, canon, QStringLiteral("camera")) &&
                        hasTabSessionPermission(viewTabId, canon, QStringLiteral("microphone")));
  if (hasTemp) {
    permission.grant();
    return;
  }

  // 2. Check profile policy (permanent rules or default policy)
  if (services_.profileService) {
    const auto policyResult = services_.profileService->evaluatePermissionPolicy(origin, permission.permissionType());
    if (policyResult == BrowserProfileService::OriginPolicyResult::Allow) {
      permission.grant();
      return;
    } else if (policyResult == BrowserProfileService::OriginPolicyResult::Deny) {
      permission.deny();
      return;
    }
  }

  // 3. Prevent duplicate prompts for the same (tabId, canonicalOrigin, type)
  if (currentActivePermissionRequest_.has_value() &&
      currentActivePermissionRequest_->tabId == viewTabId &&
      currentActivePermissionRequest_->canonicalOrigin == canon &&
      currentActivePermissionRequest_->type == permission.permissionType()) {
    return;
  }
  for (const auto &pending : pendingPermissionQueue_) {
    if (pending.tabId == viewTabId &&
        pending.canonicalOrigin == canon &&
        pending.type == permission.permissionType()) {
      return;
    }
  }

  PendingPermissionRequest req;
  req.permission = permission;
  req.view = view;
  req.page = view->page();
  req.tabId = viewTabId;
  req.requestedOrigin = origin;
  req.canonicalOrigin = canon;
  req.type = permission.permissionType();
  req.requestedAt = QDateTime::currentDateTimeUtc();

  pendingPermissionQueue_.append(req);
  updatePermissionProtection();

  if (!currentActivePermissionRequest_.has_value()) {
    processNextPermissionRequest();
  }
}

void BrowserWindow::processNextPermissionRequest() {
  const auto protection = qScopeGuard([this] { updatePermissionProtection(); });
  if (currentActivePermissionRequest_.has_value()) return;
  if (pendingPermissionQueue_.isEmpty()) {
    if (permissionBubble_) permissionBubble_->hide();
    return;
  }

  const int activeIndex = tabStrip_->currentIndex();
  const uint64_t activeTabId = (activeIndex >= 0 && activeIndex < tabs_.size()) ? tabs_[activeIndex].id : 0;

  int chosenIndex = -1;
  for (int i = 0; i < pendingPermissionQueue_.size(); ++i) {
    if (pendingPermissionQueue_[i].tabId == activeTabId) {
      chosenIndex = i;
      break;
    }
  }
  if (chosenIndex == -1) {
    chosenIndex = 0;
  }

  PendingPermissionRequest req = pendingPermissionQueue_.takeAt(chosenIndex);

  if (!req.view || !req.page || !req.permission.isValid() || findIndexByTabId(req.tabId) == -1) {
    if (req.permission.isValid()) req.permission.deny();
    processNextPermissionRequest();
    return;
  }
  const QString currentCanon = BrowserProfileService::canonicalOrigin(req.view->url());
  if (currentCanon != req.canonicalOrigin) {
    if (req.permission.isValid()) req.permission.deny();
    processNextPermissionRequest();
    return;
  }

  currentActivePermissionRequest_ = req;

  if (!permissionBubble_) {
    permissionBubble_ = new SitePermissionPromptBubble(this);
    connect(permissionBubble_, &SitePermissionPromptBubble::choiceMade, this,
            &BrowserWindow::resolveActivePermissionRequest);
  }

  permissionBubble_->setupPrompt(req.requestedOrigin, req.type);

  if (req.tabId == activeTabId) {
    updatePermissionBubblePosition();
    permissionBubble_->show();
    permissionBubble_->raise();
  } else {
    permissionBubble_->hide();
  }
}

void BrowserWindow::resolveActivePermissionRequest(SitePermissionChoice choice) {
  if (!currentActivePermissionRequest_.has_value()) return;

  PendingPermissionRequest req = *currentActivePermissionRequest_;
  currentActivePermissionRequest_.reset();

  if (permissionBubble_) {
    permissionBubble_->hide();
  }

  const bool viewValid = (!req.view.isNull() && !req.page.isNull() && req.permission.isValid());
  const bool tabValid = (findIndexByTabId(req.tabId) != -1);
  const QString currentCanon = (viewValid) ? BrowserProfileService::canonicalOrigin(req.view->url()) : QString{};
  const bool originMatches = (currentCanon == req.canonicalOrigin);

  if (!viewValid || !tabValid || !originMatches) {
    if (req.permission.isValid()) {
      req.permission.deny();
    }
    processNextPermissionRequest();
    return;
  }

  const QString permKey = BrowserProfileService::permissionKeyFromType(req.type);

  switch (choice) {
    case SitePermissionChoice::AllowThisVisit: {
      tabSessionGrants_.insert(TabSessionPermissionKey{req.tabId, req.canonicalOrigin, permKey});
      if (req.type == QWebEnginePermission::PermissionType::MediaAudioVideoCapture) {
        tabSessionGrants_.insert(TabSessionPermissionKey{req.tabId, req.canonicalOrigin, QStringLiteral("camera")});
        tabSessionGrants_.insert(TabSessionPermissionKey{req.tabId, req.canonicalOrigin, QStringLiteral("microphone")});
      }
      req.permission.grant();
      break;
    }
    case SitePermissionChoice::AlwaysAllow: {
      req.permission.grant();
      if (services_.profileService && !permKey.isEmpty()) {
        services_.profileService->addSitePermissionRule(permKey, req.canonicalOrigin, true);
        if (req.type == QWebEnginePermission::PermissionType::MediaAudioVideoCapture) {
          services_.profileService->addSitePermissionRule(QStringLiteral("camera"), req.canonicalOrigin, true);
          services_.profileService->addSitePermissionRule(QStringLiteral("microphone"), req.canonicalOrigin, true);
        }
      }
      break;
    }
    case SitePermissionChoice::Block: {
      req.permission.deny();
      if (services_.profileService && !permKey.isEmpty()) {
        services_.profileService->addSitePermissionRule(permKey, req.canonicalOrigin, false);
        if (req.type == QWebEnginePermission::PermissionType::MediaAudioVideoCapture) {
          services_.profileService->addSitePermissionRule(QStringLiteral("camera"), req.canonicalOrigin, false);
          services_.profileService->addSitePermissionRule(QStringLiteral("microphone"), req.canonicalOrigin, false);
        }
      }
      break;
    }
    case SitePermissionChoice::Dismissed: {
      req.permission.deny();
      break;
    }
  }

  if (siteControlsBubble_ && siteControlsBubble_->isVisible()) {
    siteControlsBubble_->refreshPermissions();
  }

  processNextPermissionRequest();
}

void BrowserWindow::dismissActivePermissionPrompt(bool cancelRequest) {
  if (!currentActivePermissionRequest_.has_value()) return;
  auto req = *currentActivePermissionRequest_;
  currentActivePermissionRequest_.reset();
  if (permissionBubble_) {
    permissionBubble_->hide();
  }
  if (cancelRequest && req.permission.isValid()) {
    req.permission.deny();
  }
  processNextPermissionRequest();
}
#endif

BrowserWindow *BrowserWindow::createWindow(const BrowserServices &services) {
  auto *window = new BrowserWindow(services);
  window->setAttribute(Qt::WA_DeleteOnClose);
  return window;
}

BrowserWindow *BrowserWindow::openNewWindow(const QUrl &url) {
  auto *window = createWindow(services_);
  if (url.isValid() && !url.isEmpty()) {
    window->addNewTab(url);
  } else {
    window->ensureInitialTab();
  }
  window->show();
  return window;
}

BrowserWindow *BrowserWindow::openIncognitoWindow(const QUrl &url) {
  auto incognitoServices = services_;
  const auto directory = std::make_shared<QTemporaryDir>();
  if (!directory->isValid()) return nullptr;
  auto privateOwner = std::shared_ptr<BrowserProfileService>(
      new BrowserProfileService(directory->path(), services_.policy, nullptr, true),
      [directory](BrowserProfileService *service) { delete service; });
  auto *privateService = privateOwner.get();
  if (services_.profileService) {
    privateService->setAdultContentProtectionEnabled(services_.profileService->isAdultContentProtectionEnabled());
  }
  incognitoServices.privateProfileOwner = privateOwner;
  incognitoServices.profileService = privateService;
  incognitoServices.profile = privateService->profile();
  incognitoServices.sessionStore = nullptr;
  incognitoServices.audioEffects = new WebAudioEffectsController(privateService, false);
  incognitoServices.songFinderSettings = new SongFinderSettings(privateService, false);
  incognitoServices.songRecognition = new SongRecognitionService(
      incognitoServices.songFinderSettings, privateService, false);
  const QString privateMediaHistory = QDir(directory->path()).filePath(
      QStringLiteral("media-download-history.json"));
  const QString downloadDirectory = services_.mediaDownload
      ? services_.mediaDownload->defaultDownloadDirectory()
      : privateService->profile()->downloadPath();
  incognitoServices.mediaDownload = new MediaDownloadService(
      downloadDirectory, privateService, {}, {}, privateMediaHistory);
  auto *window = new BrowserWindow(incognitoServices);

  window->setAttribute(Qt::WA_DeleteOnClose);
  window->setWindowTitle(I18n::text(QStringLiteral("window.incognito_title")));
  if (url.isValid() && !url.isEmpty()) {
    window->addNewTab(url);
  } else {
    window->ensureInitialTab();
  }
  window->show();
  return window;
}

void BrowserWindow::openDevToolsForPage(QWebEnginePage *page) {
  if (!page) return;
  auto *devWindow = new QMainWindow(this);
  devWindow->setAttribute(Qt::WA_DeleteOnClose);
  devWindow->setWindowTitle(I18n::text(QStringLiteral("window.devtools_title")));
  devWindow->resize(960, 640);
  devWindow->setStyleSheet(QStringLiteral("QMainWindow { background: #121820; color: #e6edf3; }"));
  auto *devView = new QWebEngineView(devWindow);
  devWindow->setCentralWidget(devView);
  page->setDevToolsPage(devView->page());
  page->triggerAction(QWebEnginePage::InspectElement);
  connect(page, &QObject::destroyed, devWindow, &QWidget::close);
  devWindow->show();
  devWindow->raise();
  devWindow->activateWindow();
}

void BrowserWindow::toggleTabPin(int index) {
  if (index < 0 || index >= tabs_.size()) return;
  const bool willBePinned = !tabs_[index].isPinned;
  tabs_[index].isPinned = willBePinned;
  tabStrip_->setTabPinned(index, willBePinned);
  // Pinned tabs never sleep automatically (they can still be slept manually).
  if (services_.tabManager && services_.tabManager->performanceManager())
    services_.tabManager->performanceManager()->setTabPinned(tabs_[index].uuid, willBePinned);

  if (willBePinned) {
    int targetSlot = 0;
    for (int i = 0; i < tabs_.size(); ++i) {
      if (i != index && tabs_[i].isPinned) {
        targetSlot++;
      }
    }
    if (index != targetSlot) {
      tabStrip_->moveTab(index, targetSlot);
      switchTab(targetSlot);
    }
  } else {
    int firstUnpinnedSlot = 0;
    for (int i = 0; i < tabs_.size(); ++i) {
      if (i != index && tabs_[i].isPinned) {
        firstUnpinnedSlot++;
      }
    }
    if (index != firstUnpinnedSlot) {
      tabStrip_->moveTab(index, firstUnpinnedSlot);
      switchTab(firstUnpinnedSlot);
    }
  }
}

void BrowserWindow::closeOtherTabs(int index) {
  if (index < 0 || index >= tabs_.size()) return;
  const uint64_t keepId = tabs_[index].id;
  for (int i = tabs_.size() - 1; i >= 0; --i) {
    if (tabs_[i].id != keepId && !tabs_[i].isPinned) {
      closeTab(i);
    }
  }
}

void BrowserWindow::closeTabsToRight(int index) {
  if (index < 0 || index >= tabs_.size()) return;
  for (int i = tabs_.size() - 1; i > index; --i) {
    if (!tabs_[i].isPinned) {
      closeTab(i);
    }
  }
}


// Context menus extracted to browser_window_menus.cpp


void BrowserWindow::restoreLastClosedTab() {
  if (isIncognito() || !services_.profileService) return;
  const auto closed = services_.profileService->takeMostRecentClosedTab();
  if (!closed.has_value()) return;
  addNewTab(closed->url);
}


// Page actions (FindBar, Print, PDF, Capture, Reader Mode, Fullscreen) extracted to browser_window_page_actions.cpp

// -----------------------------------------------------------------
// Tab sleeping (shared memory saver, both engines)
// -----------------------------------------------------------------
bool BrowserWindow::isTabSleeping(int index) const {
  if (index < 0 || index >= tabs_.size() || !services_.tabManager) return false;
  auto *perf = services_.tabManager->performanceManager();
  return perf && perf->hasMetadata(tabs_[index].uuid) &&
         perf->metadata(tabs_[index].uuid).lifecycleState == dalinira::TabLifecycleState::Discarded;
}

bool BrowserWindow::wakeTabAt(int index) {
  if (index < 0 || index >= tabs_.size() || !services_.tabManager) return false;
  auto *perf = services_.tabManager->performanceManager();
  return perf && perf->wakeTab(tabs_[index].uuid);
}

dalinira::TabResourceSnapshot BrowserWindow::tabResourceSnapshot(int index) const {
  if (index < 0 || index >= tabs_.size() || !services_.tabManager) return {};
  return services_.tabManager->resourceSnapshot(tabs_[index].uuid);
}

bool BrowserWindow::sleepTabAt(int index) {
  if (index < 0 || index >= tabs_.size() || !services_.tabManager) return false;
  auto *perf = services_.tabManager->performanceManager();
  return perf && perf->sleepTab(tabs_[index].uuid);
}

int BrowserWindow::sleepOtherTabs(int index) {
  if (index < 0 || index >= tabs_.size() || !services_.tabManager) return 0;
  auto *perf = services_.tabManager->performanceManager();
  return perf ? perf->sleepOtherTabs(this, tabs_[index].uuid) : 0;
}

void BrowserWindow::setNeverSleepSite(int index, bool neverSleep) {
  if (index < 0 || index >= tabs_.size() || !services_.tabManager) return;
  auto *perf = services_.tabManager->performanceManager();
  const QString site = dalinira::TabPerformanceManager::normalizeSitePattern(tabs_[index].url.host());
  if (!perf || site.isEmpty()) return;
  QStringList list = perf->siteAllowlist();
  if (neverSleep && !list.contains(site)) list.append(site);
  if (!neverSleep) list.removeAll(site);
  perf->setSiteAllowlist(list);
}

void BrowserWindow::showSleepingTabsManager() {
  showSettings(SettingsPage::Category::Performance);
}

void BrowserWindow::setPageDialogActive(QWebEnginePage *page, bool active) {
  if (!page || !services_.tabManager || !services_.tabManager->performanceManager()) return;
  const TabManager::TabId id = services_.tabManager->idForPage(page);
  if (!id.isNull()) services_.tabManager->performanceManager()->setPageDialogActive(id, active);
}

void BrowserWindow::updatePermissionProtection() {
  if (!services_.tabManager || !services_.tabManager->performanceManager()) return;
  auto *perf = services_.tabManager->performanceManager();
  for (const BrowserTabInfo &tab : std::as_const(tabs_)) {
    bool pending = false;
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    pending = currentActivePermissionRequest_.has_value() && currentActivePermissionRequest_->tabId == tab.id;
    for (const PendingPermissionRequest &request : std::as_const(pendingPermissionQueue_))
      pending = pending || request.tabId == tab.id;
#endif
    if (!tab.uuid.isNull()) perf->setPermissionPromptActive(tab.uuid, pending);
  }
}

void BrowserWindow::syncTabSleepingIndicator(const QUuid &uuid) {
  for (int i = 0; i < tabs_.size(); ++i) {
    if (tabs_[i].uuid == uuid) {
      if (tabStrip_) tabStrip_->setTabSleeping(i, isTabSleeping(i));
      if (layoutController_ && layoutController_->currentMode() != dalinira::layout::BrowserLayoutMode::Classic)
        updateSidePanelTabs();
      return;
    }
  }
}
