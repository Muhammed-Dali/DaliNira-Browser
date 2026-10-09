#include "browser_window.h"
#include "desktop_tabs/tab_performance_manager.h"
#include "browser_window_internal.h"
#include "core/browser_icons.h"
#include "core/browser_profile_service.h"
#include "desktop_tabs/tab_strip_widget.h"
#include "desktop_tabs/tab_group_model.h"
#include "passwords/credential_vault.h"
#include "passwords/credential_vault_manager.h"
#include "passwords/credential_autofill_controller.h"
#include "engine/engine_view.h"
#include "i18n/i18n.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QGuiApplication>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QSettings>
#include <QToolButton>

using dalinira::i18n::I18n;

void BrowserWindow::showMainMenu() {
  QMenu menu(this);
  menu.setStyleSheet(QStringLiteral("QMenu{background:#1b232d;color:#e8eef5;border:1px solid #3a4857;border-radius:9px;padding:6px;} QMenu::item{min-height:25px;padding:5px 30px 5px 30px;border-radius:6px;} QMenu::item:selected{background:#2b3947;} QMenu::item:disabled{color:#6f7b87;} QMenu::separator{height:1px;background:#33404d;margin:6px 8px;} QMenu::icon{padding-left:7px;}"));

  QAction *newTab = menu.addAction(BrowserIcons::icon(BrowserIcon::NewTab), I18n::text(QStringLiteral("menu.new_tab")));
  newTab->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_T));
  QAction *newWindow = menu.addAction(BrowserIcons::icon(BrowserIcon::Window), I18n::text(QStringLiteral("menu.new_window")));
  newWindow->setObjectName(QStringLiteral("newWindowAction"));
  newWindow->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_N));
  QAction *incognito = menu.addAction(BrowserIcons::icon(BrowserIcon::Incognito), I18n::text(QStringLiteral("menu.new_incognito_window")));
  incognito->setObjectName(QStringLiteral("newIncognitoWindowAction"));
  incognito->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_N));
  incognito->setToolTip(I18n::text(QStringLiteral("menu.new_incognito_tooltip")));

  menu.addSeparator();
  QAction *passwords = menu.addAction(BrowserIcons::icon(BrowserIcon::Password), I18n::text(QStringLiteral("menu.passwords")));
  QAction *fillPassword = menu.addAction(BrowserIcons::icon(BrowserIcon::Password), I18n::text(QStringLiteral("menu.fill_password")));
  fillPassword->setEnabled((currentView() != nullptr || currentEngineView() != nullptr) && services_.profileService && services_.profileService->credentialVault() && !services_.profileService->credentialVault()->isLocked());
  QAction *history = menu.addAction(BrowserIcons::icon(BrowserIcon::History), I18n::text(QStringLiteral("menu.history")));
  history->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_H));

  QMenu *bookmarksMenu = menu.addMenu(BrowserIcons::icon(BrowserIcon::Bookmark), I18n::text(QStringLiteral("menu.bookmarks")));
  QAction *toggleBar = bookmarksMenu->addAction(I18n::text(QStringLiteral("menu.bookmarks_bar")));
  toggleBar->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_B));
  toggleBar->setCheckable(true);
  const QString bmMode = QSettings().value(QStringLiteral("browser/bookmarkBarVisibility"), QStringLiteral("new_tab")).toString();
  toggleBar->setChecked(bmMode != QLatin1String("never") && isCurrentTabNewTab());
  connect(toggleBar, &QAction::triggered, this, &BrowserWindow::toggleBookmarkBar);

  QAction *bookmarks = bookmarksMenu->addAction(BrowserIcons::icon(BrowserIcon::Bookmark), I18n::text(QStringLiteral("menu.bookmarks_manager")));
  bookmarks->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_O));

  bookmarksMenu->addSeparator();
  QAction *importBookmarksAct = bookmarksMenu->addAction(I18n::text(QStringLiteral("menu.import_bookmarks")));
  connect(importBookmarksAct, &QAction::triggered, this, [this] {
    if (!services_.profileService) return;
    const QString filePath = QFileDialog::getOpenFileName(this,
        I18n::text(QStringLiteral("bookmarks.import_title")),
        QDir::homePath(),
        I18n::text(QStringLiteral("settings.bookmarks.html_filter")));
    if (filePath.isEmpty()) return;
    QFile file(filePath);
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
      const QString content = QString::fromUtf8(file.readAll());
      const int count = services_.profileService->importBookmarksFromHtml(content);
      QMessageBox::information(this,
          I18n::text(QStringLiteral("bookmarks.import_title")),
          I18n::text(QStringLiteral("bookmarks.imported_count")).arg(count));
    }
  });

  QAction *exportBookmarksAct = bookmarksMenu->addAction(I18n::text(QStringLiteral("menu.export_bookmarks")));
  connect(exportBookmarksAct, &QAction::triggered, this, [this] {
    if (!services_.profileService) return;
    const QString filePath = QFileDialog::getSaveFileName(this,
        I18n::text(QStringLiteral("bookmarks.export_title")),
        QDir::homePath() + QStringLiteral("/bookmarks.html"),
        I18n::text(QStringLiteral("settings.bookmarks.html_filter")));
    if (filePath.isEmpty()) return;
    QFile file(filePath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
      const QString html = services_.profileService->exportBookmarksToHtml();
      file.write(html.toUtf8());
      QMessageBox::information(this,
          I18n::text(QStringLiteral("bookmarks.export_title")),
          I18n::text(QStringLiteral("bookmarks.exported_success")));
    }
  });

  QAction *downloads = menu.addAction(BrowserIcons::icon(BrowserIcon::Download), I18n::text(QStringLiteral("menu.downloads")));
  downloads->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_J));

  menu.addSeparator();
  QMenu *zoom = menu.addMenu(BrowserIcons::icon(BrowserIcon::Zoom), I18n::text(QStringLiteral("menu.zoom")));
  QAction *zoomOut = zoom->addAction(QStringLiteral("−"));
  const qreal curZoom = currentEngineView() ? currentEngineView()->zoomFactor() : (currentView() ? currentView()->zoomFactor() : 1.0);
  QAction *zoomReset = zoom->addAction(QStringLiteral("%%%1").arg(qRound(curZoom * 100.0)));
  QAction *zoomIn = zoom->addAction(QStringLiteral("+"));
  const bool hasWebContent = (currentView() != nullptr || currentEngineView() != nullptr);
  zoomOut->setEnabled(hasWebContent);
  zoomReset->setEnabled(hasWebContent);
  zoomIn->setEnabled(hasWebContent);

  menu.addSeparator();
  QAction *print = menu.addAction(BrowserIcons::icon(BrowserIcon::Print), I18n::text(QStringLiteral("menu.print")));
  print->setShortcut(QKeySequence::Print);
  print->setEnabled(hasWebContent);
  connect(print, &QAction::triggered, this, &BrowserWindow::printCurrentPage);

  QAction *printPdf = menu.addAction(I18n::text(QStringLiteral("menu.print_pdf")));
  printPdf->setEnabled(hasWebContent);
  connect(printPdf, &QAction::triggered, this, &BrowserWindow::printCurrentPageToPdf);

  QAction *find = menu.addAction(BrowserIcons::icon(BrowserIcon::Search), I18n::text(QStringLiteral("menu.find_in_page")));
  find->setShortcut(QKeySequence::Find);
  find->setEnabled(hasWebContent);
  connect(find, &QAction::triggered, this, &BrowserWindow::showFindBar);

  QAction *translate = menu.addAction(BrowserIcons::icon(BrowserIcon::Language),
      I18n::text(QStringLiteral("menu.translate_page")));
  translate->setObjectName(QStringLiteral("translatePageAction"));
  QUrl currentUrl = currentEngineView() ? currentEngineView()->url() : (currentView() ? currentView()->url() : QUrl{});
  const int currentTabIndex = tabStrip_ ? tabStrip_->currentIndex() : -1;
  if ((currentUrl.isEmpty() || !currentUrl.isValid())
      && currentTabIndex >= 0 && currentTabIndex < tabs_.size()) {
    currentUrl = tabs_[currentTabIndex].url;
  }
  translate->setEnabled(hasWebContent
      && (currentUrl.scheme() == QLatin1String("http") || currentUrl.scheme() == QLatin1String("https")));
  connect(translate, &QAction::triggered, this, &BrowserWindow::showTranslatePopup);

  QAction *save = menu.addAction(BrowserIcons::icon(BrowserIcon::Save), I18n::text(QStringLiteral("context.save_page_as")));
  save->setShortcut(QKeySequence::Save);
  save->setEnabled(hasWebContent);
  connect(save, &QAction::triggered, this, [this] {
    if (auto *view = currentView()) {
      if (view->page()) view->page()->triggerAction(QWebEnginePage::SavePage);
    } else if (currentEngineView()) {
      printCurrentPageToPdf();
    }
  });

  QMenu *tools = menu.addMenu(BrowserIcons::icon(BrowserIcon::Tools), I18n::text(QStringLiteral("menu.other_tools")));
  QAction *reader = tools->addAction(I18n::text(QStringLiteral("menu.reader_mode")));
  reader->setEnabled(hasWebContent && currentUrl.scheme().startsWith(QLatin1String("http")));
  connect(reader, &QAction::triggered, this, &BrowserWindow::showReaderMode);
  QAction *capture = tools->addAction(I18n::text(QStringLiteral("menu.capture_page")));
  capture->setEnabled(hasWebContent);
  connect(capture, &QAction::triggered, this, &BrowserWindow::captureVisiblePage);

  menu.addSeparator();
  QAction *help = menu.addAction(BrowserIcons::icon(BrowserIcon::Help), I18n::text(QStringLiteral("menu.help"))); help->setEnabled(false);
  QAction *audioEffectsAction = menu.addAction(QIcon(QStringLiteral(":/side-widget-icons/sound-effects.svg")), I18n::text(QStringLiteral("menu.audio_effects")));
  QAction *eqPresetsAction = menu.addAction(QIcon(QStringLiteral(":/side-widget-icons/eq-presets.svg")), I18n::text(QStringLiteral("menu.eq_presets")));
  QAction *settings = menu.addAction(BrowserIcons::icon(BrowserIcon::Settings), I18n::text(QStringLiteral("menu.settings")));
  QAction *quit = menu.addAction(BrowserIcons::icon(BrowserIcon::Exit), I18n::text(QStringLiteral("menu.exit")));

  connect(newTab, &QAction::triggered, this, [this] { addNewTab(); });
  connect(newWindow, &QAction::triggered, this, [this] {
    openNewWindow();
  });
  connect(incognito, &QAction::triggered, this, [this] {
    openIncognitoWindow();
  });
  connect(passwords, &QAction::triggered, this, &BrowserWindow::showPasswords);
  connect(fillPassword, &QAction::triggered, this, &BrowserWindow::fillCurrentPageFromVault);
  connect(history, &QAction::triggered, this, [this] { showSettings(SettingsPage::Category::History); });
  connect(bookmarks, &QAction::triggered, this, [this] { showSettings(SettingsPage::Category::Bookmarks); });
  connect(downloads, &QAction::triggered, this, [this] { showDownloadsMenu(); });
  connect(zoomOut, &QAction::triggered, this, [this] { changeCurrentZoom(-0.1); });
  connect(zoomReset, &QAction::triggered, this, [this] { setCurrentZoom(1.0); });
  connect(zoomIn, &QAction::triggered, this, [this] { changeCurrentZoom(0.1); });
  connect(audioEffectsAction, &QAction::triggered, this, &BrowserWindow::showAudioEffects);
  connect(eqPresetsAction, &QAction::triggered, this, &BrowserWindow::showEqPresetBrowser);
  connect(settings, &QAction::triggered, this, [this] { showSettings(); });
  connect(quit, &QAction::triggered, qApp, &QCoreApplication::quit);

  const QPoint execPos = mainMenuBtn_ ? mainMenuBtn_->mapToGlobal(QPoint(0, mainMenuBtn_->height())) : QCursor::pos();
  menu.exec(execPos);
}

void BrowserWindow::showHistoryMenu() {
  if (!services_.profileService) return;
  QMenu menu(this);
  menu.setStyleSheet(QStringLiteral("QMenu{background:#1b232d;color:#e8eef5;border:1px solid #3a4857;border-radius:9px;padding:6px;} QMenu::item{min-height:25px;padding:5px 30px 5px 30px;border-radius:6px;} QMenu::item:disabled{color:#6f7b87;}"));

  QAction *openAllHistory = menu.addAction(BrowserIcons::icon(BrowserIcon::History),
      I18n::text(QStringLiteral("menu.show_full_history")) + QStringLiteral("\tCtrl+H"));
  connect(openAllHistory, &QAction::triggered, this, [this] {
    showSettings(SettingsPage::Category::History);
  });
  menu.addSeparator();

  if (!isIncognito() && services_.profileService->hasClosedTabs()) {
    QAction *restoreTab = menu.addAction(BrowserIcons::icon(BrowserIcon::History),
        I18n::text(QStringLiteral("menu.reopen_closed_tab")) + QStringLiteral("\tCtrl+Shift+T"));
    connect(restoreTab, &QAction::triggered, this, &BrowserWindow::restoreLastClosedTab);
    menu.addSeparator();
  }

  const auto entries = services_.profileService->recentHistory();
  if (entries.isEmpty()) {
    QAction *empty = menu.addAction(I18n::text(QStringLiteral("menu.history_empty")));
    empty->setEnabled(false);
  } else {
    for (const auto &entry : entries.mid(0, std::min<qsizetype>(30, entries.size()))) {
      const QString label = entry.title.isEmpty() ? entry.url.host() : entry.title;
      QMenu *itemMenu = menu.addMenu(label.left(90));
      itemMenu->setStyleSheet(menu.styleSheet());
      QAction *openAct = itemMenu->addAction(I18n::text(QStringLiteral("menu.open")));
      openAct->setToolTip(QStringLiteral("%1\n%2").arg(entry.url.toDisplayString(), entry.visitedAt.toLocalTime().toString(QStringLiteral("dd.MM.yyyy HH:mm"))));
      connect(openAct, &QAction::triggered, this, [this, url = entry.url] {
        navigateCurrentTab(url);
      });
      QAction *openNewTabAct = itemMenu->addAction(I18n::text(QStringLiteral("menu.open_in_new_tab")));
      connect(openNewTabAct, &QAction::triggered, this, [this, url = entry.url] {
        addNewTab(url);
      });
      QAction *deleteAct = itemMenu->addAction(BrowserIcons::icon(BrowserIcon::Close), I18n::text(QStringLiteral("menu.delete_from_history")));
      connect(deleteAct, &QAction::triggered, this, [this, url = entry.url, visitedAt = entry.visitedAt] {
        if (services_.profileService) {
          services_.profileService->removeHistoryEntry(url, visitedAt);
        }
      });
    }
    menu.addSeparator();
    QAction *clear = menu.addAction(I18n::text(QStringLiteral("menu.clear_history")));
    connect(clear, &QAction::triggered, this, [this] {
      if (services_.profileService) services_.profileService->clearHistory();
    });
  }
  menu.exec(QCursor::pos());
}

void BrowserWindow::showDownloadsMenu() {
  showMediaDownloads();
}

void BrowserWindow::onTabContextMenuRequested(int index, const QPoint &globalPos) {
  if (index < 0 || index >= tabs_.size()) return;
  const auto &tab = tabs_[index];
  const uint64_t tabId = tab.id;

  QMenu menu(this);
  menu.setStyleSheet(QStringLiteral(
      "QMenu { background-color: #1b232d; color: #e8eef5; border: 1px solid #3a4857; border-radius: 9px; padding: 6px 4px; font-size: 13px; }"
      "QMenu::item { min-height: 25px; padding: 4px 26px 4px 12px; border-radius: 6px; margin: 1px 3px; }"
      "QMenu::item:selected { background-color: #2b3947; color: #ffffff; }"
      "QMenu::item:disabled { color: #6f7b87; background-color: transparent; }"
      "QMenu::separator { height: 1px; background-color: #33404d; margin: 5px 8px; }"
      "QMenu::icon { padding-left: 6px; }"
  ));

  // 1. Sağa yeni sekme
  QAction *newTabRight = menu.addAction(BrowserIcons::icon(BrowserIcon::NewTab), I18n::text(QStringLiteral("tab.context.new_tab_right")));
  connect(newTabRight, &QAction::triggered, this, [this, index] {
    addNewTab(QUrl(QStringLiteral("dalinira://newtab/")), index + 1);
  });

  // 2. Mevcut sekmeyle yeni bölünmüş görünüm
  QAction *splitViewAction = menu.addAction(BrowserIcons::icon(BrowserIcon::Cards), I18n::text(QStringLiteral("tab.context.split_view")));
  splitViewAction->setEnabled(false);

  // 3. Sekmeyi yeni gruba ekle / Sekmeyi gruptan çıkar / Gruplar alt menüsü
  if (tab.groupId.has_value() && groupModel_ && groupModel_->hasGroup(*tab.groupId)) {
    QAction *ungroup = menu.addAction(BrowserIcons::icon(BrowserIcon::Grid), I18n::text(QStringLiteral("tab.context.ungroup")));
    connect(ungroup, &QAction::triggered, this, [this, tabId] {
      if (groupModel_) {
        const int idx = findIndexByTabId(tabId);
        if (idx >= 0 && tabs_[idx].groupId.has_value()) {
          const QUuid gid = *tabs_[idx].groupId;
          groupModel_->removeTabFromGroup(tabId);
          tabs_[idx].groupId = std::nullopt;
          if (groupModel_->groupTabCount(gid) == 0) {
            groupModel_->removeGroup(gid);
          }
          tabStrip_->update();
        }
      }
    });
  } else {
    const auto groups = groupModel_ ? groupModel_->allGroups() : QList<dalinira::desktop_tabs::TabGroup>{};
    if (groups.isEmpty()) {
      QAction *newGroup = menu.addAction(BrowserIcons::icon(BrowserIcon::Grid), I18n::text(QStringLiteral("tab.context.add_to_new_group")));
      connect(newGroup, &QAction::triggered, this, [this, tabId] {
        createGroupFromExistingTab(tabId);
      });
    } else {
      QMenu *groupSub = menu.addMenu(BrowserIcons::icon(BrowserIcon::Grid), I18n::text(QStringLiteral("tab.context.add_to_group")));
      groupSub->setStyleSheet(menu.styleSheet());
      QAction *createGroupAct = groupSub->addAction(I18n::text(QStringLiteral("tab.context.new_group")));
      connect(createGroupAct, &QAction::triggered, this, [this, tabId] {
        createGroupFromExistingTab(tabId);
      });
      groupSub->addSeparator();
      for (const auto &g : groups) {
        QString title = g.name.trimmed().isEmpty() ? I18n::text(QStringLiteral("tabgroup.unnamed")) : g.name;
        QAction *gAct = groupSub->addAction(title);
        const QUuid gid = g.id;
        connect(gAct, &QAction::triggered, this, [this, tabId, gid] {
          if (groupModel_) {
            const int idx = findIndexByTabId(tabId);
            if (idx >= 0) {
              tabs_[idx].groupId = gid;
              groupModel_->setTabGroup(tabId, gid);
              tabStrip_->update();
            }
          }
        });
      }
    }
  }

  // 4. Sekmeyi yeni pencereye taşı
  QAction *moveWindow = menu.addAction(BrowserIcons::icon(BrowserIcon::Window), I18n::text(QStringLiteral("tab.context.move_to_new_window")));
  moveWindow->setEnabled(tabs_.size() > 1);
  connect(moveWindow, &QAction::triggered, this, [this, tabId] {
    auto *newWin = new BrowserWindow(services_);
    newWin->show();
    transferTabTo(tabId, newWin, 0);
  });

  menu.addSeparator();

  // 5. Yeniden Yükle
  QAction *reloadAct = menu.addAction(I18n::text(QStringLiteral("tab.context.reload")));
  reloadAct->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
  connect(reloadAct, &QAction::triggered, this, [this, tabId] {
    const int idx = findIndexByTabId(tabId);
    if (idx >= 0 && tabs_[idx].view) {
      tabs_[idx].view->reload();
    } else if (idx >= 0 && tabs_[idx].engineView) {
      tabs_[idx].engineView->reload();
    }
  });

  // 6. Yinele
  QAction *duplicateAct = menu.addAction(I18n::text(QStringLiteral("tab.context.duplicate")));
  connect(duplicateAct, &QAction::triggered, this, [this, tabId] {
    const int idx = findIndexByTabId(tabId);
    if (idx >= 0) {
      addNewTab(tabs_[idx].url, idx + 1);
    }
  });

  // 7. Sabitle / Sabitlemeyi kaldır
  const bool isPinned = tab.isPinned;
  QAction *pinAct = menu.addAction(isPinned
      ? I18n::text(QStringLiteral("tab.context.unpin"))
      : I18n::text(QStringLiteral("tab.context.pin")));
  connect(pinAct, &QAction::triggered, this, [this, tabId] {
    const int idx = findIndexByTabId(tabId);
    if (idx >= 0) {
      toggleTabPin(idx);
    }
  });

  // 8. Sitenin sesini kapat / Sitenin sesini aç
  bool isMuted = tab.view && tab.view->page() && tab.view->page()->isAudioMuted();
  QAction *muteAct = menu.addAction(BrowserIcons::icon(BrowserIcon::Audio),
                                    isMuted
                                        ? I18n::text(QStringLiteral("tab.context.unmute"))
                                        : I18n::text(QStringLiteral("tab.context.mute")));
  connect(muteAct, &QAction::triggered, this, [this, tabId, isMuted] {
    const int idx = findIndexByTabId(tabId);
    if (idx >= 0 && tabs_[idx].view && tabs_[idx].view->page()) {
      const bool newMuted = !isMuted;
      tabs_[idx].view->page()->setAudioMuted(newMuted);
      tabStrip_->setTabAudible(idx, !newMuted && tabs_[idx].view->page()->recentlyAudible());
    }
  });

  // 8b. Uyku / Bellek: the shared tab sleeping actions (both engines).
  QMenu *sleepMenu = menu.addMenu(BrowserIcons::icon(BrowserIcon::Performance),
                                  I18n::text(QStringLiteral("tab.context.sleep_menu")));
  sleepMenu->setStyleSheet(menu.styleSheet());
  populateTabSleepMenu(sleepMenu, index);

  menu.addSeparator();

  // 9. Okuma listesine sekme ekle
  QAction *readingListAct = menu.addAction(BrowserIcons::icon(BrowserIcon::Bookmark), I18n::text(QStringLiteral("tab.context.add_to_reading_list")));
  connect(readingListAct, &QAction::triggered, this, [this, tabId] {
    const int idx = findIndexByTabId(tabId);
    if (idx >= 0 && services_.profileService) {
      services_.profileService->toggleBookmark(tabs_[idx].url);
      updateBookmarkButtonState();
      renderBookmarks();
    }
  });

  // 10. Cihazıma gönder
  QAction *sendDeviceAct = menu.addAction(I18n::text(QStringLiteral("tab.context.send_to_device")));
  sendDeviceAct->setEnabled(false);

  menu.addSeparator();

  // 11. Sekmeleri dikey olarak göster
  QAction *verticalTabsAct = menu.addAction(I18n::text(QStringLiteral("tab.context.vertical_tabs")));
  verticalTabsAct->setEnabled(false);

  menu.addSeparator();

  // 12. Kapat
  QAction *closeAct = menu.addAction(BrowserIcons::icon(BrowserIcon::Close),
      I18n::text(QStringLiteral("tab.context.close")) + QStringLiteral("\tCtrl+W"));
  connect(closeAct, &QAction::triggered, this, [this, tabId] {
    const int idx = findIndexByTabId(tabId);
    if (idx >= 0) {
      closeTab(idx);
    }
  });

  // Kapatılan sekmeyi yeniden aç
  QAction *reopenClosedAct = menu.addAction(BrowserIcons::icon(BrowserIcon::History),
      I18n::text(QStringLiteral("tab.context.reopen_closed")) + QStringLiteral("\tCtrl+Shift+T"));
  reopenClosedAct->setEnabled(!isIncognito() && services_.profileService && services_.profileService->hasClosedTabs());
  connect(reopenClosedAct, &QAction::triggered, this, &BrowserWindow::restoreLastClosedTab);

  // 13. Diğer sekmeleri kapat
  QAction *closeOthersAct = menu.addAction(I18n::text(QStringLiteral("tab.context.close_others")));
  closeOthersAct->setEnabled(tabs_.size() > 1);
  connect(closeOthersAct, &QAction::triggered, this, [this, tabId] {
    const int idx = findIndexByTabId(tabId);
    if (idx >= 0) {
      closeOtherTabs(idx);
    }
  });

  // 14. Sağdaki sekmeleri kapat
  QAction *closeRightAct = menu.addAction(I18n::text(QStringLiteral("tab.context.close_right")));
  closeRightAct->setEnabled(index < tabs_.size() - 1);
  connect(closeRightAct, &QAction::triggered, this, [this, tabId] {
    const int idx = findIndexByTabId(tabId);
    if (idx >= 0) {
      closeTabsToRight(idx);
    }
  });

  menu.exec(globalPos);
}

// The shortcut items of the bar under the omnibox are this engine's Quick
// Access (the New Tab shortcuts); only its folders come from the global
// bookmark library. Editing a shortcut here never changes the other engine's
// shortcuts or the bookmark library.
void BrowserWindow::showBookmarkContextMenu(const QUrl &url, const QString &title, const QPoint &globalPos) {
  const QString engine = currentEngineScope();
  QMenu menu(this);
  menu.setStyleSheet(QStringLiteral(
      "QMenu { background-color: #1b232d; color: #e8eef5; border: 1px solid #3a4857; border-radius: 9px; padding: 6px 4px; font-size: 13px; }"
      "QMenu::item { min-height: 25px; padding: 4px 26px 4px 12px; border-radius: 6px; margin: 1px 3px; }"
      "QMenu::item:selected { background-color: #2b3947; color: #ffffff; }"
      "QMenu::item:disabled { color: #6f7b87; background-color: transparent; }"
      "QMenu::separator { height: 1px; background-color: #33404d; margin: 5px 8px; }"
  ));

  const bool hasUrl = url.isValid() && !url.isEmpty();

  if (hasUrl) {
    // 1. Yeni sekmede aç
    QAction *newTabAct = menu.addAction(I18n::text(QStringLiteral("bookmark.open_tab")));
    connect(newTabAct, &QAction::triggered, this, [this, url] {
      addNewTab(url);
    });

    // 2. Yeni pencerede aç
    QAction *newWinAct = menu.addAction(I18n::text(QStringLiteral("bookmark.open_window")));
    connect(newWinAct, &QAction::triggered, this, [this, url] {
      openNewWindow(url);
    });

    // 3. Bölünmüş görünümde aç
    QAction *splitAct = menu.addAction(I18n::text(QStringLiteral("bookmark.open_split")));
    splitAct->setEnabled(false);

    // 4. Gizli pencerede aç
    QAction *incognitoAct = menu.addAction(I18n::text(QStringLiteral("bookmark.open_incognito")));
    connect(incognitoAct, &QAction::triggered, this, [this, url] {
      openIncognitoWindow(url);
    });

    menu.addSeparator();

    // 5. Düzenle...
    QAction *editAct = menu.addAction(I18n::text(QStringLiteral("bookmark.edit")));
    connect(editAct, &QAction::triggered, this, [this, url, title, engine] {
      QInputDialog dlg(this);
      dlg.setWindowTitle(I18n::text(QStringLiteral("bookmark.edit_dialog_title")));
      dlg.setLabelText(I18n::text(QStringLiteral("bookmark.url_label")));
      dlg.setTextValue(url.toString());
      dlg.setStyleSheet(QStringLiteral(
          "QDialog { background-color: #1b232d; color: #e8eef5; border: 1px solid #3a4857; border-radius: 8px; }"
          "QLabel { color: #e8eef5; font-size: 13px; }"
          "QLineEdit { background: #121820; color: #ffffff; border: 1px solid #3a4857; border-radius: 6px; padding: 6px; font-size: 13px; }"
          "QPushButton { background: #263342; color: #ffffff; border: 1px solid #3a4857; border-radius: 6px; padding: 6px 14px; font-size: 13px; }"
          "QPushButton:hover { background: #324458; }"
      ));
      if (dlg.exec() == QDialog::Accepted) {
        const QString newText = dlg.textValue().trimmed();
        if (!newText.isEmpty()) {
          const QUrl newUrl = QUrl::fromUserInput(newText);
          if (newUrl.isValid() && services_.profileService) {
            services_.profileService->removeQuickAccess(url, engine);
            services_.profileService->addQuickAccess(newUrl, newUrl == url ? title : QString{}, engine);
            updateBookmarkButtonState();
            renderBookmarks();
          }
        }
      }
    });

    menu.addSeparator();

    // 6. Kes
    QAction *cutAct = menu.addAction(I18n::text(QStringLiteral("bookmark.cut")));
    connect(cutAct, &QAction::triggered, this, [this, url, engine] {
      QGuiApplication::clipboard()->setText(url.toString());
      if (services_.profileService) {
        services_.profileService->removeQuickAccess(url, engine);
        updateBookmarkButtonState();
        renderBookmarks();
      }
    });

    // 7. Kopyala
    QAction *copyAct = menu.addAction(I18n::text(QStringLiteral("bookmark.copy")));
    connect(copyAct, &QAction::triggered, this, [url] {
      QGuiApplication::clipboard()->setText(url.toString());
    });
  }

  // 8. Yapıştır
  const QString clipText = QGuiApplication::clipboard()->text().trimmed();
  const QUrl clipUrl = QUrl::fromUserInput(clipText);
  const bool canPaste = !clipText.isEmpty() && clipUrl.isValid() &&
                        (clipUrl.scheme() == QLatin1String("http") || clipUrl.scheme() == QLatin1String("https"));
  QAction *pasteAct = menu.addAction(I18n::text(QStringLiteral("bookmark.paste")));
  pasteAct->setEnabled(canPaste);
  connect(pasteAct, &QAction::triggered, this, [this, clipUrl, engine] {
    if (services_.profileService && !services_.profileService->isQuickAccess(clipUrl, engine)) {
      services_.profileService->addQuickAccess(clipUrl, clipUrl.host(), engine);
      updateBookmarkButtonState();
      renderBookmarks();
    }
  });

  if (hasUrl) {
    menu.addSeparator();

    // 9. Sil
    QAction *deleteAct = menu.addAction(I18n::text(QStringLiteral("bookmark.delete")));
    connect(deleteAct, &QAction::triggered, this, [this, url, engine] {
      if (services_.profileService) {
        services_.profileService->removeQuickAccess(url, engine);
        updateBookmarkButtonState();
        renderBookmarks();
      }
    });

    // 9b. Klasöre taşı...
    QAction *moveToFolderAct = menu.addAction(I18n::text(QStringLiteral("settings.bookmarks.move_to_folder_button")));
    connect(moveToFolderAct, &QAction::triggered, this, [this, url, title, engine] {
      if (!services_.profileService) return;
      QStringList folderOptions = services_.profileService->bookmarkFolders();
      folderOptions.prepend(I18n::text(QStringLiteral("bookmark.root_folder")));
      folderOptions.append(I18n::text(QStringLiteral("bookmark.create_new_folder")));
      bool ok = false;
      const QString choice = QInputDialog::getItem(
          this, I18n::text(QStringLiteral("settings.bookmarks.move_to_folder_title")), I18n::text(QStringLiteral("bookmark.choose_target_folder")), folderOptions, 0, false, &ok);
      if (ok) {
        QString targetFolder;
        if (choice == I18n::text(QStringLiteral("bookmark.create_new_folder"))) {
          const QString newF = QInputDialog::getText(
              this, I18n::text(QStringLiteral("bookmark.add_folder_title")), I18n::text(QStringLiteral("bookmark.folder_name_prompt")), QLineEdit::Normal, QString(), &ok);
          if (ok && !newF.trimmed().isEmpty()) {
            targetFolder = newF.trimmed();
          } else {
            return;
          }
        } else if (choice != I18n::text(QStringLiteral("bookmark.root_folder"))) {
          targetFolder = choice;
        }
        // Files the page in the (global) bookmark library. A shortcut moved into
        // a folder leaves this engine's shortcut row; the other engine's row
        // is untouched.
        if (services_.profileService->isBookmarked(url)) {
          services_.profileService->moveBookmarkToFolder(url, targetFolder);
        } else {
          services_.profileService->addBookmark(url, title, targetFolder);
        }
        if (!targetFolder.isEmpty()) services_.profileService->removeQuickAccess(url, engine);
        updateBookmarkButtonState();
        renderBookmarks();
      }
    });
  }

  menu.addSeparator();

  // 10. Sayfa ekle...
  QAction *addPageAct = menu.addAction(I18n::text(QStringLiteral("bookmark.add_page")));
  connect(addPageAct, &QAction::triggered, this, [this, engine] {
    QInputDialog dlg(this);
    dlg.setWindowTitle(I18n::text(QStringLiteral("bookmark.add_page_title")));
    dlg.setLabelText(I18n::text(QStringLiteral("bookmark.url_label")));
    const QUrl cur = currentEngineView() ? currentEngineView()->url() : (currentView() ? currentView()->url() : QUrl{});
    dlg.setTextValue(cur.isValid() && !isNewTabUrl(cur) ? cur.toString() : QStringLiteral("https://"));
    dlg.setStyleSheet(QStringLiteral(
        "QDialog { background-color: #1b232d; color: #e8eef5; border: 1px solid #3a4857; border-radius: 8px; }"
        "QLabel { color: #e8eef5; font-size: 13px; }"
        "QLineEdit { background: #121820; color: #ffffff; border: 1px solid #3a4857; border-radius: 6px; padding: 6px; font-size: 13px; }"
        "QPushButton { background: #263342; color: #ffffff; border: 1px solid #3a4857; border-radius: 6px; padding: 6px 14px; font-size: 13px; }"
        "QPushButton:hover { background: #324458; }"
    ));
    if (dlg.exec() == QDialog::Accepted) {
      const QString newText = dlg.textValue().trimmed();
      if (!newText.isEmpty()) {
        const QUrl newUrl = QUrl::fromUserInput(newText);
        if (newUrl.isValid() && (newUrl.scheme() == QLatin1String("http") || newUrl.scheme() == QLatin1String("https"))) {
          if (services_.profileService && !services_.profileService->isQuickAccess(newUrl, engine)) {
            services_.profileService->addQuickAccess(newUrl, QString{}, engine);
            updateBookmarkButtonState();
            renderBookmarks();
          }
        }
      }
    }
  });

  // 11. Klasör ekle...
  QAction *addFolderAct = menu.addAction(I18n::text(QStringLiteral("bookmark.add_folder")));
  connect(addFolderAct, &QAction::triggered, this, [this] {
    bool ok = false;
    const QString folderName = QInputDialog::getText(
        this,
        I18n::text(QStringLiteral("bookmark.add_folder_title")),
        I18n::text(QStringLiteral("bookmark.folder_name_prompt")),
        QLineEdit::Normal,
        QString(),
        &ok);
    if (ok && !folderName.trimmed().isEmpty() && services_.profileService) {
      services_.profileService->createBookmarkFolder(folderName.trimmed());
      renderBookmarks();
    }
  });

  menu.addSeparator();

  // 12. Yer işareti yöneticisini aç
  QAction *managerAct = menu.addAction(I18n::text(QStringLiteral("bookmark.open_manager")));
  connect(managerAct, &QAction::triggered, this, [this] {
    showSettings(SettingsPage::Category::Bookmarks);
  });

  // 13. Uygulamalar kısayolunu göster
  QAction *appsShortcutAct = menu.addAction(I18n::text(QStringLiteral("bookmark.show_apps_shortcut")));
  appsShortcutAct->setCheckable(true);
  const bool showApps = QSettings().value(QStringLiteral("browser/showAppsShortcut"), false).toBool();
  appsShortcutAct->setChecked(showApps);
  connect(appsShortcutAct, &QAction::toggled, this, [](bool checked) {
    QSettings settings;
    settings.setValue(QStringLiteral("browser/showAppsShortcut"), checked);
    settings.sync();
  });

  // 14. Sekme gruplarını göster
  QAction *tabGroupsAct = menu.addAction(I18n::text(QStringLiteral("bookmark.show_tab_groups")));
  tabGroupsAct->setCheckable(true);
  const bool showGroups = QSettings().value(QStringLiteral("browser/showTabGroupsOnBookmarkBar"), true).toBool();
  tabGroupsAct->setChecked(showGroups);
  connect(tabGroupsAct, &QAction::toggled, this, [this](bool checked) {
    QSettings settings;
    settings.setValue(QStringLiteral("browser/showTabGroupsOnBookmarkBar"), checked);
    settings.sync();
    if (appsBtn_) {
      appsBtn_->setVisible(checked);
    }
  });

  // 15. Yer işaretleri çubuğunu göster
  QAction *toggleBarAct = menu.addAction(I18n::text(QStringLiteral("bookmark.show_bar")));
  toggleBarAct->setCheckable(true);
  const QString bmMode = QSettings().value(QStringLiteral("browser/bookmarkBarVisibility"), QStringLiteral("new_tab")).toString();
  const bool barVisible = (bmMode != QLatin1String("never") && isCurrentTabNewTab());
  toggleBarAct->setChecked(barVisible);
  connect(toggleBarAct, &QAction::triggered, this, &BrowserWindow::toggleBookmarkBar);

  menu.exec(globalPos);
}

void BrowserWindow::populateTabSleepMenu(QMenu *sleepMenu, int index) {
  if (!sleepMenu || index < 0 || index >= tabs_.size()) return;
  const BrowserTabInfo &tab = tabs_[index];
  const uint64_t tabId = tab.id;
  auto *perf = services_.tabManager ? services_.tabManager->performanceManager() : nullptr;

  // Sleep and Wake follow the tab's real lifecycle state: exactly one of them
  // is available (neither on a tab that may not sleep, e.g. the current one).
  QAction *sleepAct = sleepMenu->addAction(I18n::text(QStringLiteral("tab.context.sleep_tab")));
  sleepAct->setObjectName(QStringLiteral("tab-sleep"));
  sleepAct->setEnabled(perf && perf->canSleepNow(tab.uuid));
  connect(sleepAct, &QAction::triggered, this, [this, tabId] {
    const int idx = findIndexByTabId(tabId);
    if (idx >= 0) sleepTabAt(idx);
  });
  QAction *wakeAct = sleepMenu->addAction(I18n::text(QStringLiteral("tab.context.wake_tab")));
  wakeAct->setObjectName(QStringLiteral("tab-wake"));
  wakeAct->setEnabled(perf && perf->canWakeNow(tab.uuid));
  connect(wakeAct, &QAction::triggered, this, [this, tabId] {
    const int idx = findIndexByTabId(tabId);
    if (idx >= 0) wakeTabAt(idx);
  });

  const QString site = dalinira::TabPerformanceManager::normalizeSitePattern(tab.url.host());
  const bool webSite = !tab.isInternal && dalinira::TabPerformanceManager::isSupportedWebScheme(tab.url);
  QAction *neverAct = sleepMenu->addAction(I18n::text(QStringLiteral("tab.context.never_sleep_site")));
  neverAct->setObjectName(QStringLiteral("tab-never-sleep-site"));
  neverAct->setCheckable(true);
  neverAct->setChecked(perf && webSite && perf->siteAllowlist().contains(site));
  neverAct->setEnabled(perf && webSite && !site.isEmpty());
  connect(neverAct, &QAction::toggled, this, [this, tabId](bool checked) {
    const int idx = findIndexByTabId(tabId);
    if (idx >= 0) setNeverSleepSite(idx, checked);
  });
  QAction *sleepOthersAct = sleepMenu->addAction(I18n::text(QStringLiteral("tab.context.sleep_other_tabs")));
  sleepOthersAct->setObjectName(QStringLiteral("tab-sleep-others"));
  sleepOthersAct->setEnabled(perf && tabs_.size() > 1);
  connect(sleepOthersAct, &QAction::triggered, this, [this, tabId] {
    const int idx = findIndexByTabId(tabId);
    if (idx >= 0) sleepOtherTabs(idx);
  });
  sleepMenu->addSeparator();
  const int sleepingCount = perf ? perf->sleepingTabs().size() : 0;
  QAction *manageAct = sleepMenu->addAction(
      sleepingCount > 0
          ? I18n::text(QStringLiteral("tab.context.manage_sleeping_count")).arg(sleepingCount)
          : I18n::text(QStringLiteral("tab.context.manage_sleeping")));
  manageAct->setObjectName(QStringLiteral("tab-manage-sleeping"));
  connect(manageAct, &QAction::triggered, this, &BrowserWindow::showSleepingTabsManager);
}
