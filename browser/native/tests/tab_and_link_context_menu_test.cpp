#ifdef NDEBUG
#undef NDEBUG
#endif
#include <QApplication>
#include <QEventLoop>
#include <QTimer>
#include <QTemporaryDir>
#include <QSettings>
#include <cassert>
#include <iostream>

#include "browser_window.h"
#include "desktop_tabs/tab_strip_widget.h"
#include "core/browser_profile_service.h"
#include "blocker/dalinira_blocker_service.h"
#include "blocker/dalinira_blocker_settings.h"
#include "engine/engine_view.h"
#include <QMenu>
#include <QAction>

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  app.setApplicationName(QStringLiteral("DaliNiraContextMenuTest"));

  QTemporaryDir root;
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, root.path());

  BrowserProfileService profile(root.path() + "/normal", nullptr, nullptr, false);
  BrowserServices services;
  services.profile = profile.profile();
  services.profileService = &profile;

  BrowserWindow window(services, true);
  window.resize(1000, 700);

  // 1. Initial tab check
  window.addNewTab(QUrl(QStringLiteral("https://example.com/tab0")));
  window.addNewTab(QUrl(QStringLiteral("https://example.com/tab1")));
  window.addNewTab(QUrl(QStringLiteral("https://example.com/tab2")));
  window.addNewTab(QUrl(QStringLiteral("https://example.com/tab3")));

  assert(window.tabCount() == 4);
  assert(window.tabStrip()->count() == 4);

  // 2. Tab Pinning test
  assert(!window.allTabs()[0].isPinned);
  assert(!window.tabStrip()->isTabPinned(0));

  window.toggleTabPin(2); // Pin tab2 (url: example.com/tab2)
  assert(window.allTabs()[0].isPinned);
  assert(window.tabStrip()->isTabPinned(0));
  assert(window.allTabs()[0].url.toString() == QStringLiteral("https://example.com/tab2"));

  // Unpin it
  window.toggleTabPin(0);
  assert(!window.allTabs()[0].isPinned);
  assert(!window.tabStrip()->isTabPinned(0));

  // 3. Close tabs to the right
  // We have 4 tabs: 0, 1, 2, 3.
  window.closeTabsToRight(1);
  assert(window.tabCount() == 2);
  assert(window.tabStrip()->count() == 2);

  // 4. Close other tabs
  window.addNewTab(QUrl(QStringLiteral("https://example.com/tabExtra")));
  assert(window.tabCount() == 3);
  window.closeOtherTabs(1);
  assert(window.tabCount() == 1);
  assert(window.tabStrip()->count() == 1);

  // 5. Open incognito window
  BrowserWindow *incognitoWin = window.openIncognitoWindow();
  assert(incognitoWin != nullptr);
  assert(incognitoWin->isIncognito());
  assert(incognitoWin->tabCount() >= 1);
  assert(incognitoWin->tabInfo(0).title == QStringLiteral("Yeni Gizli Sekme"));
  assert(incognitoWin->tabStrip()->tabText(0) == QStringLiteral("Yeni Gizli Sekme"));
  delete incognitoWin;

  // 7. Bookmark context menu test
  const QUrl bmUrl(QStringLiteral("https://example.com/testbookmark"));
  profile.toggleBookmark(bmUrl);
  assert(profile.isBookmarked(bmUrl));

  QTimer::singleShot(10, [] {
    if (auto *w = QApplication::activePopupWidget()) {
      w->close();
    }
  });
  window.showBookmarkContextMenu(bmUrl, QStringLiteral("Test BM"), QPoint(100, 100));

  QTimer::singleShot(10, [] {
    if (auto *w = QApplication::activePopupWidget()) {
      w->close();
    }
  });
  window.showBookmarkContextMenu(QUrl(), QString(), QPoint(100, 100));

  // 8. User-initiated "Open link in new tab" with popup blocking ENABLED
  assert(profile.blockerService() != nullptr);
  profile.blockerService()->settings()->setProtectionEnabled(true);
  profile.blockerService()->settings()->setPopupBlock(true);
  assert(profile.blockerService()->settings()->popupBlock() == true);

  const int beforeNewTabCount = window.tabCount();
  const QUrl youtubeVideoUrl(QStringLiteral("https://www.youtube.com/watch?v=dQw4w9WgXcQ"));

  // Trigger user-initiated "Open link in new tab" from context menu
  QTimer::singleShot(10, [] {
    if (auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
      for (QAction *act : menu->actions()) {
        if (act->text().contains(QStringLiteral("yeni sekmede"), Qt::CaseInsensitive) ||
            act->text().contains(QStringLiteral("new tab"), Qt::CaseInsensitive) ||
            act->text().contains(QStringLiteral("open_link_tab"), Qt::CaseInsensitive)) {
          act->trigger();
          break;
        }
      }
      menu->close();
    }
  });
  window.showLinkContextMenu(youtubeVideoUrl, QStringLiteral("Never Gonna Give You Up"), QPoint(200, 200));

  // 1. Exactly one new tab must be created
  assert(window.tabCount() == beforeNewTabCount + 1);
  const auto &newTab = window.tabInfo(window.tabCount() - 1);
  // 2. Correct destination loaded
  assert(newTab.url == youtubeVideoUrl);
  // 3. Browser alive, view and page non-null
  assert(newTab.view != nullptr);
  assert(newTab.view->page() != nullptr);
  // 4. Blocker remains registered and active for child tab
  assert(profile.blockerService()->activeTabId() == reinterpret_cast<quintptr>(newTab.view.data()));

  // 9. Blocker popup decision checks
  // Legitimate user / target=_blank navigation is allowed
  assert(profile.blockerService()->decidePopup(youtubeVideoUrl, QUrl(QStringLiteral("https://www.youtube.com/")), 0x123).action == DaliNiraBlockerAction::Allow);

  // 10. Ordinary browser new tab
  const int beforeOrdinaryNewTab = window.tabCount();
  window.addNewTab(QUrl(QStringLiteral("dalinira://newtab/")));
  assert(window.tabCount() == beforeOrdinaryNewTab + 1);
  assert(window.tabInfo(window.tabCount() - 1).url == QUrl(QStringLiteral("dalinira://newtab/")));

  // 11. EngineView context menu signal dispatch (WebKit / Chromium abstraction)
  {
    bool linkSignalReceived = false;
    bool selectionSignalReceived = false;
    bool pageSignalReceived = false;
    QUrl receivedUrl;

    dalinira::engine::EngineView dummyEngineView;
    QObject::connect(&dummyEngineView, &dalinira::engine::EngineView::linkContextMenuRequested,
                     [&linkSignalReceived, &receivedUrl](const QUrl &url, const QString &, const QPoint &) {
      linkSignalReceived = true;
      receivedUrl = url;
    });
    QObject::connect(&dummyEngineView, &dalinira::engine::EngineView::selectionContextMenuRequested,
                     [&selectionSignalReceived](const QString &, const QPoint &) {
      selectionSignalReceived = true;
    });
    QObject::connect(&dummyEngineView, &dalinira::engine::EngineView::pageContextMenuRequested,
                     [&pageSignalReceived](const QPoint &) {
      pageSignalReceived = true;
    });

    emit dummyEngineView.linkContextMenuRequested(youtubeVideoUrl, QStringLiteral("Video"), QPoint(100, 100));
    assert(linkSignalReceived && receivedUrl == youtubeVideoUrl);

    emit dummyEngineView.selectionContextMenuRequested(QStringLiteral("text"), QPoint(100, 100));
    assert(selectionSignalReceived);

    emit dummyEngineView.pageContextMenuRequested(QPoint(100, 100));
    assert(pageSignalReceived);
  }

  std::cout << "All context menu and tab/bookmark management tests passed successfully!\n";
  return 0;
}
