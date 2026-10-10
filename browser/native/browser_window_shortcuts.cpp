#include "browser_window.h"

#include "core/keyboard_shortcut_service.h"
#include "desktop_tabs/find_bar_widget.h"
#include "desktop_tabs/tab_group_launcher_popup.h"
#include "desktop_tabs/tab_search_popup.h"
#include "desktop_tabs/tab_strip_widget.h"
#include "engine/engine_view.h"
#include "i18n/i18n.h"
#include "layout/browser_layout_controller.h"

#include <QAction>
#include <QDebug>
#include <QLineEdit>
#include <QShortcut>
#include <QToolButton>
#include <QWebEngineHistory>
#include <QWebEnginePage>

using dalinira::KeyboardShortcutService;
using dalinira::i18n::I18n;

// Browser actions are DaliNira-level: the same handler runs whichever engine
// the window uses, and the engine only decides what the current tab's view is.
void BrowserWindow::setupBrowserShortcuts() {
  const auto currentIndex = [this] { return tabStrip_ ? tabStrip_->currentIndex() : -1; };
  const auto currentGroup = [this, currentIndex]() -> std::optional<QUuid> {
    const int index = currentIndex();
    if (index < 0 || index >= tabs_.size()) return std::nullopt;
    return tabs_[index].groupId;
  };

  browserActions_ = {
      {QStringLiteral("navigation.back"), [this] { onBackClicked(); }},
      {QStringLiteral("navigation.forward"), [this] { onForwardClicked(); }},
      {QStringLiteral("navigation.reload"), [this] { onReloadOrStopClicked(); }},
      {QStringLiteral("navigation.stop"), [this] {
         if (auto *engineView = currentEngineView()) {
           if (engineView->isLoading()) engineView->stop();
         } else if (auto *view = currentView()) {
           view->stop();
         }
       }},
      {QStringLiteral("navigation.home"), [this] { onHomeClicked(); }},
      {QStringLiteral("navigation.focus_address"), [this] {
         if (!omnibox_) return;
         omnibox_->setFocus();
         omnibox_->selectAll();
       }},

      {QStringLiteral("tabs.new"), [this] { addNewTab(QUrl(QStringLiteral("dalinira://newtab/"))); }},
      {QStringLiteral("tabs.close"), [this, currentIndex] {
         const int index = currentIndex();
         if (index >= 0 && index < tabs_.size()) closeTab(index);
       }},
      {QStringLiteral("tabs.reopen_closed"), [this] { restoreLastClosedTab(); }},
      {QStringLiteral("tabs.next"), [this, currentIndex] {
         if (tabStrip_ && !tabs_.isEmpty()) switchTab((currentIndex() + 1) % tabs_.size());
       }},
      {QStringLiteral("tabs.previous"), [this, currentIndex] {
         if (tabStrip_ && !tabs_.isEmpty()) switchTab((currentIndex() + tabs_.size() - 1) % tabs_.size());
       }},
      {QStringLiteral("tabs.select_last"), [this] {
         if (!tabs_.isEmpty()) switchTab(tabs_.size() - 1);
       }},
      {QStringLiteral("tabs.duplicate"), [this, currentIndex] {
         // As the tab context menu's "Duplicate".
         const int index = currentIndex();
         if (index >= 0 && index < tabs_.size()) addNewTab(tabs_[index].url, index + 1);
       }},
      {QStringLiteral("tabs.search"), [this] { toggleTabSearchPopup(); }},
      {QStringLiteral("tabs.sleep_others"), [this, currentIndex] {
         const int index = currentIndex();
         if (index >= 0 && index < tabs_.size()) sleepOtherTabs(index);
       }},
      {QStringLiteral("tabs.group_new"), [this] {
         if (tabGroupLauncherPopup_ && tabGroupLauncherPopup_->isVisible()) tabGroupLauncherPopup_->close();
         createNewTabGroupWithNewTab();
       }},
      {QStringLiteral("tabs.group_add_tab"), [this, currentGroup] {
         if (const auto group = currentGroup()) addTabToGroup(*group);
       }},
      {QStringLiteral("tabs.group_close"), [this, currentGroup] {
         if (const auto group = currentGroup()) closeTabGroup(*group);
       }},

      {QStringLiteral("window.new"), [this] { openNewWindow(); }},
      {QStringLiteral("window.new_incognito"), [this] { openIncognitoWindow(); }},
      {QStringLiteral("window.close"), [this] { close(); }},
      {QStringLiteral("window.fullscreen"), [this] { toggleBrowserFullScreen(); }},

      {QStringLiteral("page.save"), [this] { saveCurrentPage(); }},
      {QStringLiteral("page.print"), [this] { printCurrentPage(); }},
      {QStringLiteral("page.view_source"), [this] { viewCurrentPageSource(); }},

      {QStringLiteral("find.open"), [this] { showFindBar(); }},
      {QStringLiteral("find.next"), [this] {
         if (findBar_ && findBar_->isVisible())
           handleFindRequest(findBar_->findText(), true, findBar_->isCaseSensitive());
         else
           showFindBar();
       }},
      {QStringLiteral("find.previous"), [this] {
         if (findBar_ && findBar_->isVisible())
           handleFindRequest(findBar_->findText(), false, findBar_->isCaseSensitive());
       }},

      {QStringLiteral("zoom.in"), [this] { changeCurrentZoom(0.1); }},
      {QStringLiteral("zoom.out"), [this] { changeCurrentZoom(-0.1); }},
      {QStringLiteral("zoom.reset"), [this] { setCurrentZoom(1.0); }},

      {QStringLiteral("downloads.open"), [this] { showDownloadsMenu(); }},
      {QStringLiteral("history.open"), [this] { showSettings(SettingsPage::Category::History); }},
      {QStringLiteral("bookmarks.add_current"), [this] { toggleCurrentBookmark(); }},
      {QStringLiteral("bookmarks.manager"), [this] { showSettings(SettingsPage::Category::Bookmarks); }},
      {QStringLiteral("bookmarks.toggle_bar"), [this] { toggleBookmarkBar(); }},

      {QStringLiteral("sidepanel.toggle"), [this] {
         // The panel's pin toggle: pinned open <-> auto-hide (side layouts).
         if (layoutController_ && layoutController_->currentMode() != dalinira::layout::BrowserLayoutMode::Classic)
           applySidePanelAutoHideToAllWindows(layoutController_->isPinned());
       }},
      {QStringLiteral("settings.open"), [this] { showSettings(); }},
      {QStringLiteral("dalinira.passwords"), [this] { showPasswords(); }},
      {QStringLiteral("dalinira.audio_effects"), [this] { showAudioEffects(); }},
  };

  // Ctrl+1…8: the tab at that position, when there is one (as in other browsers).
  for (int position = 1; position <= 8; ++position) {
    browserActions_.insert(QStringLiteral("tabs.select_%1").arg(position), [this, position] {
      if (position <= tabs_.size()) switchTab(position - 1);
    });
  }

  for (const dalinira::ShortcutActionDefinition &definition : KeyboardShortcutService::registry()) {
    // An action without a window handler gets no shortcut (tests check that
    // every registered action has one).
    if (!browserActions_.contains(definition.id)) {
      qWarning() << "shortcuts: no browser action for" << definition.id;
      continue;
    }
    auto *shortcut = new QShortcut(this);
    shortcut->setObjectName(QStringLiteral("shortcut:") + definition.id);
    shortcut->setContext(Qt::WindowShortcut);
    const QString id = definition.id;
    connect(shortcut, &QShortcut::activated, this, [this, id] { triggerBrowserAction(id); });
    actionShortcuts_.insert(id, shortcut);
  }
  applyShortcutBindings();
  connect(&KeyboardShortcutService::instance(), &KeyboardShortcutService::shortcutsChanged, this, [this] {
    applyShortcutBindings();
    refreshShortcutHints();
  });
}

void BrowserWindow::applyShortcutBindings() {
  const KeyboardShortcutService &service = KeyboardShortcutService::instance();
  for (auto it = actionShortcuts_.cbegin(); it != actionShortcuts_.cend(); ++it) {
    const QList<QKeySequence> keys = service.shortcuts(it.key());
    // setKeys replaces the old binding: it stops working at once and the
    // action keeps a single handler.
    it.value()->setKeys(keys);
    it.value()->setEnabled(!keys.isEmpty());
  }
}

bool BrowserWindow::triggerBrowserAction(const QString &actionId) {
  const auto handler = browserActions_.value(actionId);
  if (!handler) return false;
  handler();
  return true;
}

QString BrowserWindow::shortcutMenuHint(const QString &actionId) {
  const QList<QKeySequence> keys = KeyboardShortcutService::instance().shortcuts(actionId);
  return keys.isEmpty() ? QString() : QStringLiteral("\t") + KeyboardShortcutService::displayText(keys.first());
}

void BrowserWindow::addShortcutHint(QAction *action, const QString &actionId) {
  // A text hint, not QAction::setShortcuts: QMenu would print Qt's own,
  // desktop-translated key names, and the window's QShortcut already owns the key.
  if (action) action->setText(action->text() + shortcutMenuHint(actionId));
}

QString BrowserWindow::shortcutTooltip(const QString &labelKey, const QString &actionId) const {
  const QList<QKeySequence> keys = KeyboardShortcutService::instance().shortcuts(actionId);
  if (keys.isEmpty()) return I18n::text(labelKey);
  return QStringLiteral("%1 (%2)").arg(I18n::text(labelKey), KeyboardShortcutService::displayText(keys.first()));
}

void BrowserWindow::refreshShortcutHints() {
  if (tabSearchBtn_) tabSearchBtn_->setToolTip(shortcutTooltip(QStringLiteral("toolbar.search_tabs"), QStringLiteral("tabs.search")));
  if (backBtn_) backBtn_->setToolTip(shortcutTooltip(QStringLiteral("toolbar.back"), QStringLiteral("navigation.back")));
  if (forwardBtn_) forwardBtn_->setToolTip(shortcutTooltip(QStringLiteral("toolbar.forward"), QStringLiteral("navigation.forward")));
  if (reloadBtn_) updateReloadStopButton(isCurrentTabLoading());
  if (tabSearchPopup_) {
    const QList<QKeySequence> keys = KeyboardShortcutService::instance().shortcuts(QStringLiteral("tabs.search"));
    tabSearchPopup_->setShortcutHint(keys.isEmpty() ? QString() : KeyboardShortcutService::displayText(keys.first()));
  }
}

void BrowserWindow::saveCurrentPage() {
  if (auto *view = currentView()) {
    if (view->page()) view->page()->triggerAction(QWebEnginePage::SavePage);
  } else if (currentEngineView()) {
    printCurrentPageToPdf();
  }
}

void BrowserWindow::viewCurrentPageSource() {
  if (auto *view = currentView()) {
    if (view->page()) view->page()->triggerAction(QWebEnginePage::ViewSource);
    return;
  }
  const QUrl pageUrl = currentTabUrl();
  if (!currentEngineView() || !pageUrl.isValid() || pageUrl.isEmpty()) return;
  addNewTab(QUrl(QStringLiteral("view-source:") + pageUrl.toString()));
}
