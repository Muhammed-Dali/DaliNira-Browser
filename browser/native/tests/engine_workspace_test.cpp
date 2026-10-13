// Chromium and WebKit are two independent browsing workspaces in one DaliNira:
// each engine has its own tabs (order, active tab, pinned and sleeping state),
// session restore, New Tab Quick Access / shortcut row and Frequent Sites.
// The bookmark library and other browser-wide data stay shared.
//
// --logic (default tier, no window, no engine process):
//   SessionStore workspaces, legacy migration, shortcut row / bookmark
//   library / Frequent Sites separation.
// --gui (GUI tier, scripts/safe-gui-test.sh): the real engine switch. DaliNira
//   runs one engine per process and switching engine relaunches it, so each
//   phase is a separate child process with one BrowserWindow, run strictly one
//   after another on the same profile: Chromium -> WebKit -> Chromium -> WebKit.
//   Local pages only.

#include <QApplication>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QMimeData>
#include <QToolButton>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <functional>
#include <iostream>

#include "browser_window.h"
#include "core/application_identity.h"
#include "core/bookmark_bar_reorder.h"
#include "core/browser_profile_service.h"
#include "desktop_tabs/tab_manager.h"
#include "desktop_tabs/tab_performance_manager.h"
#include "desktop_tabs/tab_strip_widget.h"
#include "engine/chromium/chromium_backend.h"
#include "engine/engine_backend.h"
#include "engine/engine_storage_manager.h"
#include "newtab/new_tab_background_store.h"
#include "newtab/new_tab_scheme.h"
#include "session/session_store.h"
#if defined(DALINIRA_BUILD_WEBKIT)
#include "engine/webkit/webkit_backend.h"
#endif

#define CHECK(condition)                                                                    \
  do {                                                                                      \
    if (!(condition)) {                                                                     \
      std::cout << "[FAIL] " << __FILE__ << ":" << __LINE__ << ": " #condition << std::endl; \
      std::_Exit(1);                                                                        \
    }                                                                                       \
  } while (0)

namespace {

const QString kChromium = QStringLiteral("chromium");
const QString kWebKit = QStringLiteral("webkit");
const QString kGecko = QStringLiteral("gecko");

bool waitUntil(const std::function<bool()> &condition, int timeoutMs) {
  QTimer heartbeat;
  heartbeat.start(20);
  QElapsedTimer timer;
  timer.start();
  while (!condition()) {
    if (timer.elapsed() >= timeoutMs) return false;
    const qint64 next = timer.elapsed() + 50;
    while (timer.elapsed() < next) QApplication::processEvents(QEventLoop::AllEvents | QEventLoop::WaitForMoreEvents);
  }
  return true;
}

QStringList urls(const QVector<SavedTab> &tabs) {
  QStringList result;
  for (const SavedTab &tab : tabs) result.append(tab.url.toString());
  return result;
}

bool containsUrl(const QList<BrowserProfileService::QuickAccessItem> &items, const QUrl &url) {
  for (const auto &item : items)
    if (item.url == url) return true;
  return false;
}

bool containsHost(const QList<BrowserFrequentSite> &sites, const QString &host) {
  for (const BrowserFrequentSite &site : sites)
    if (site.url.host() == host) return true;
  return false;
}

QByteArray readAll(const QString &path) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

SavedTab savedTab(const QString &url, const QString &title, bool active = false, bool pinned = false,
                  bool sleeping = false) {
  SavedTab tab;
  tab.url = QUrl(url);
  tab.title = title;
  tab.active = active;
  tab.pinned = pinned;
  tab.sleeping = sleeping;
  return tab;
}

// ---------------------------------------------------------------------------
// Logic tier
// ---------------------------------------------------------------------------

void checkWorkspaceStore() {
  QTemporaryDir root;
  CHECK(root.isValid());
  SessionStore store(root.path());
  CHECK(store.sessionFilePath(kChromium) == root.path() + QStringLiteral("/session/chromium/tabs.session.json"));
  CHECK(store.sessionFilePath(kWebKit) == root.path() + QStringLiteral("/session/webkit/tabs.session.json"));
  CHECK(store.sessionFilePath() == store.sessionFilePath(kChromium));
  CHECK(SessionStore(root.path(), kWebKit).sessionFilePath() == store.sessionFilePath(kWebKit));

  QVector<SavedTab> chromium{savedTab(QStringLiteral("https://www.youtube.com/"), QStringLiteral("YouTube"), false, true),
                             savedTab(QStringLiteral("https://github.com/"), QStringLiteral("GitHub"), true),
                             savedTab(QStringLiteral("https://www.amazon.com/"), QStringLiteral("Amazon"), false, false, true),
                             savedTab(QStringLiteral("dalinira://newtab/"), QStringLiteral("Yeni Sekme"))};
  chromium[2].groupId = QUuid::createUuid();
  chromium[2].groupName = QStringLiteral("Shopping");
  chromium[2].groupColor = QColor(QStringLiteral("#ff8800"));
  const QVector<SavedTab> webkit{savedTab(QStringLiteral("https://www.instagram.com/"), QStringLiteral("Instagram"), true),
                                 savedTab(QStringLiteral("https://www.facebook.com/"), QStringLiteral("Facebook"), false, true)};

  // Chromium's tabs never appear in the WebKit workspace, and the reverse.
  CHECK(store.saveSavedTabs(chromium, kChromium));
  CHECK(store.load(kWebKit).isEmpty());
  CHECK(store.saveSavedTabs(webkit, kWebKit));

  const auto checkChromium = [&](const QVector<SavedTab> &loaded) {
    CHECK(urls(loaded) == urls(chromium));  // order kept, nothing duplicated
    CHECK(loaded[0].pinned && !loaded[1].pinned && !loaded[2].pinned && !loaded[3].pinned);
    CHECK(!loaded[0].active && loaded[1].active && !loaded[2].active && !loaded[3].active);
    CHECK(loaded[0].title == QStringLiteral("YouTube") && loaded[3].title == QStringLiteral("Yeni Sekme"));
    CHECK(!loaded[0].sleeping && !loaded[1].sleeping && loaded[2].sleeping);
    CHECK(loaded[2].groupId == chromium[2].groupId && loaded[2].groupName == QStringLiteral("Shopping"));
  };
  const auto checkWebKit = [&](const QVector<SavedTab> &loaded) {
    CHECK(urls(loaded) == urls(webkit));
    CHECK(loaded[0].active && !loaded[1].active);
    CHECK(!loaded[0].pinned && loaded[1].pinned);
    CHECK(loaded[0].title == QStringLiteral("Instagram"));
  };
  checkChromium(store.load(kChromium));
  checkWebKit(store.load(kWebKit));
  std::cout << "[PASS] each engine loads only its own tabs, order, active, pinned, titles, sleeping, groups" << std::endl;

  // Chromium -> WebKit -> Chromium: a Chromium save leaves WebKit's workspace
  // byte-for-byte unchanged, and a WebKit save leaves Chromium's unchanged.
  const QByteArray webkitBytes = readAll(store.sessionFilePath(kWebKit));
  QVector<SavedTab> chromiumChanged = chromium;
  chromiumChanged.append(savedTab(QStringLiteral("https://example.org/"), QStringLiteral("Example")));
  CHECK(store.saveSavedTabs(chromiumChanged, kChromium));
  CHECK(readAll(store.sessionFilePath(kWebKit)) == webkitBytes);
  CHECK(store.saveSavedTabs(chromium, kChromium));
  const QByteArray chromiumBytes = readAll(store.sessionFilePath(kChromium));
  QVector<SavedTab> webkitChanged = webkit;
  webkitChanged[0].active = false;
  webkitChanged[1].active = true;
  CHECK(store.saveSavedTabs(webkitChanged, kWebKit));
  CHECK(readAll(store.sessionFilePath(kChromium)) == chromiumBytes);
  CHECK(store.load(kWebKit)[1].active && !store.load(kWebKit)[0].active);  // WebKit's own active tab
  checkChromium(store.load(kChromium));                                    // Chromium's active tab unchanged
  CHECK(store.saveSavedTabs(webkit, kWebKit));
  std::cout << "[PASS] Chromium->WebKit->Chromium and WebKit->Chromium->WebKit leave the other workspace untouched" << std::endl;

  // Restart: new stores (as main.cpp creates for either engine) restore both.
  checkChromium(SessionStore(root.path(), kChromium).load());
  checkWebKit(SessionStore(root.path(), kWebKit).load());
  // Saving the same workspace again never duplicates tabs.
  CHECK(store.saveSavedTabs(store.load(kWebKit), kWebKit));
  checkWebKit(store.load(kWebKit));
  std::cout << "[PASS] both workspaces survive a restart without duplication" << std::endl;

  // SessionStore::save from the live tab model, per workspace.
  QTemporaryDir liveRoot;
  CHECK(liveRoot.isValid());
  SessionStore liveStore(liveRoot.path());
  TabManager manager;
  QObject chromiumWindow;
  QObject webkitWindow;
  QWidget c1, c2, w1;
  const auto c1Id = manager.registerEngineTab(&c1, &chromiumWindow, false, QStringLiteral("GitHub"),
                                              QUrl(QStringLiteral("https://github.com/")));
  const auto c2Id = manager.registerEngineTab(&c2, &chromiumWindow, false, QStringLiteral("Amazon"),
                                              QUrl(QStringLiteral("https://www.amazon.com/")));
  const auto w1Id = manager.registerEngineTab(&w1, &webkitWindow, false, QStringLiteral("Instagram"),
                                              QUrl(QStringLiteral("https://www.instagram.com/")));
  CHECK(!c1Id.isNull() && !c2Id.isNull() && !w1Id.isNull());
  CHECK(manager.activate(c1Id) && manager.activate(w1Id));
  CHECK(liveStore.save(manager, &chromiumWindow, kChromium));
  CHECK(liveStore.save(manager, &webkitWindow, kWebKit));
  const QVector<SavedTab> liveChromium = liveStore.load(kChromium);
  const QVector<SavedTab> liveWebKit = liveStore.load(kWebKit);
  CHECK(urls(liveChromium) == QStringList({QStringLiteral("https://github.com/"), QStringLiteral("https://www.amazon.com/")}));
  CHECK(liveChromium[0].active && !liveChromium[1].active);
  CHECK(urls(liveWebKit) == QStringList{QStringLiteral("https://www.instagram.com/")} && liveWebKit[0].active);
  const auto permissions = QFileInfo(liveStore.sessionFilePath(kWebKit)).permissions();
  CHECK(!(permissions & (QFileDevice::ReadGroup | QFileDevice::ReadOther | QFileDevice::WriteGroup | QFileDevice::WriteOther)));
  std::cout << "[PASS] live tab model saves each window to its engine workspace only (owner-only files)" << std::endl;

  // A single-file store (tests, tools) is one session whatever the engine.
  QTemporaryDir fileRoot;
  SessionStore single(fileRoot.path() + QStringLiteral("/tabs.session.json"));
  CHECK(single.sessionFilePath(kWebKit) == single.sessionFilePath(kChromium));
}

void writeLegacySession(const QString &dir, const QStringList &tabUrls) {
  QJsonArray tabs;
  for (int i = 0; i < tabUrls.size(); ++i)
    tabs.append(QJsonObject{{QStringLiteral("url"), tabUrls[i]}, {QStringLiteral("title"), QStringLiteral("Legacy %1").arg(i)},
                            {QStringLiteral("active"), i == 0}});
  QFile file(dir + QStringLiteral("/tabs.session.json"));
  CHECK(file.open(QIODevice::WriteOnly));
  file.write(QJsonDocument(QJsonObject{{QStringLiteral("version"), 1}, {QStringLiteral("tabs"), tabs}}).toJson());
}

void checkLegacyMigration() {
  const QStringList legacy{QStringLiteral("https://www.youtube.com/"), QStringLiteral("https://www.instagram.com/")};

  // The shared pre-workspace session becomes Chromium's, once; WebKit starts clean.
  {
    QTemporaryDir root;
    writeLegacySession(root.path(), legacy);
    CHECK(SessionStore::migrateLegacySessionIfNeeded(root.path()));
    SessionStore store(root.path());
    CHECK(urls(store.load(kChromium)) == legacy);
    CHECK(store.load(kWebKit).isEmpty());
    CHECK(!QFile::exists(root.path() + QStringLiteral("/tabs.session.json")));
    CHECK(QFile::exists(root.path() + QStringLiteral("/tabs.session.json.migrated")));
    // One time only: a later start changes nothing.
    CHECK(store.saveSavedTabs({savedTab(QStringLiteral("https://github.com/"), QStringLiteral("GitHub"), true)}, kChromium));
    CHECK(!SessionStore::migrateLegacySessionIfNeeded(root.path()));
    CHECK(urls(store.load(kChromium)) == QStringList{QStringLiteral("https://github.com/")});
    CHECK(store.load(kWebKit).isEmpty());
  }
  // An explicit WebKit workspace is preserved.
  {
    QTemporaryDir root;
    SessionStore store(root.path());
    CHECK(store.saveSavedTabs({savedTab(QStringLiteral("https://www.facebook.com/"), QStringLiteral("Facebook"), true)}, kWebKit));
    writeLegacySession(root.path(), legacy);
    CHECK(SessionStore::migrateLegacySessionIfNeeded(root.path()));
    CHECK(urls(store.load(kWebKit)) == QStringList{QStringLiteral("https://www.facebook.com/")});
    CHECK(urls(store.load(kChromium)) == legacy);
  }
  // An existing Chromium workspace is never overwritten by a legacy file.
  {
    QTemporaryDir root;
    SessionStore store(root.path());
    CHECK(store.saveSavedTabs({savedTab(QStringLiteral("https://github.com/"), QStringLiteral("GitHub"), true)}, kChromium));
    writeLegacySession(root.path(), legacy);
    CHECK(!SessionStore::migrateLegacySessionIfNeeded(root.path()));
    CHECK(urls(store.load(kChromium)) == QStringList{QStringLiteral("https://github.com/")});
    CHECK(store.load(kWebKit).isEmpty());
  }
  std::cout << "[PASS] legacy session migrates once into Chromium only; WebKit and existing workspaces are preserved" << std::endl;
}

void checkProfileData() {
  QTemporaryDir root;
  CHECK(root.isValid());
  const QUrl youtube(QStringLiteral("https://www.youtube.com/"));
  const QUrl docs(QStringLiteral("https://docs.example.org/"));
  const QUrl amazon(QStringLiteral("https://www.amazon.com/"));
  const QUrl github(QStringLiteral("https://github.com/"));
  const QUrl instagram(QStringLiteral("https://www.instagram.com/"));
  const QUrl facebook(QStringLiteral("https://www.facebook.com/"));
  {
    BrowserProfileService service(root.path(), nullptr, nullptr, false, nullptr);
    // Library bookmarks from before workspaces: one at the root (the old bar
    // item) and one in a folder.
    CHECK(service.addBookmark(youtube, QStringLiteral("YouTube")));
    CHECK(service.addBookmark(docs, QStringLiteral("Docs"), QStringLiteral("Work")));
    const int libraryItems = service.bookmarkItems().size();

    // The shortcut row (Quick Access): the old root items belong to Chromium
    // only; WebKit's row starts empty.
    CHECK(containsUrl(service.quickAccessItems(kChromium), youtube));
    CHECK(service.quickAccessItems(kWebKit).isEmpty());

    CHECK(service.addQuickAccess(amazon, QStringLiteral("Amazon"), kChromium));
    CHECK(service.addQuickAccess(github, QStringLiteral("GitHub"), kChromium));
    CHECK(service.addQuickAccess(instagram, QStringLiteral("Instagram"), kWebKit));
    CHECK(service.addQuickAccess(facebook, QStringLiteral("Facebook"), kWebKit));
    CHECK(containsUrl(service.quickAccessItems(kChromium), amazon) && containsUrl(service.quickAccessItems(kChromium), github));
    CHECK(!containsUrl(service.quickAccessItems(kChromium), instagram) && !containsUrl(service.quickAccessItems(kChromium), facebook));
    CHECK(containsUrl(service.quickAccessItems(kWebKit), instagram) && containsUrl(service.quickAccessItems(kWebKit), facebook));
    CHECK(!containsUrl(service.quickAccessItems(kWebKit), amazon) && !containsUrl(service.quickAccessItems(kWebKit), github));

    // Reordering (dragging on the bookmark bar) moves within one engine's row.
    const auto urlsOf = [&](const QString &engine) {
      QList<QUrl> urls;
      for (const auto &item : service.quickAccessItems(engine)) urls.append(item.url);
      return urls;
    };
    CHECK(urlsOf(kChromium) == (QList<QUrl>{youtube, amazon, github}));
    CHECK(service.moveQuickAccess(github, 0, kChromium));
    CHECK(urlsOf(kChromium) == (QList<QUrl>{github, youtube, amazon}));
    CHECK(service.moveQuickAccess(github, 99, kChromium));  // clamped to the end
    CHECK(urlsOf(kChromium) == (QList<QUrl>{youtube, amazon, github}));
    CHECK(!service.moveQuickAccess(github, 2, kChromium));  // already there
    CHECK(!service.moveQuickAccess(instagram, 0, kChromium));  // not in this engine's row
    CHECK(service.moveQuickAccess(facebook, 0, kWebKit));
    CHECK(urlsOf(kWebKit) == (QList<QUrl>{facebook, instagram}));
    CHECK(urlsOf(kChromium) == (QList<QUrl>{youtube, amazon, github}));

    // Removing a shortcut edits only that engine's row, never the library.
    CHECK(service.removeQuickAccess(youtube, kChromium));
    CHECK(!service.isQuickAccess(youtube, kChromium));
    CHECK(service.isBookmarked(youtube) && service.isBookmarked(docs));
    CHECK(service.bookmarkItems().size() == libraryItems);
    CHECK(service.bookmarkFolders().contains(QStringLiteral("Work")));
    CHECK(!service.isBookmarked(amazon) && !service.isBookmarked(instagram));  // shortcuts are not library entries
    // A library bookmark added later does not become a WebKit shortcut.
    CHECK(service.addBookmark(QUrl(QStringLiteral("https://later.example.org/")), QStringLiteral("Later")));
    CHECK(!containsUrl(service.quickAccessItems(kWebKit), QUrl(QStringLiteral("https://later.example.org/"))));

    // Frequent Sites and their hidden list stay per engine.
    for (int i = 0; i < 3; ++i) service.recordHistory(amazon, QStringLiteral("Amazon"), true, kChromium);
    for (int i = 0; i < 3; ++i) service.recordHistory(instagram, QStringLiteral("Instagram"), true, kWebKit);
    CHECK(containsHost(service.frequentSites(10, kChromium), amazon.host()));
    CHECK(!containsHost(service.frequentSites(10, kChromium), instagram.host()));
    CHECK(containsHost(service.frequentSites(10, kWebKit), instagram.host()));
    CHECK(!containsHost(service.frequentSites(10, kWebKit), amazon.host()));
    CHECK(service.hideFrequentSite(amazon, kChromium));
    CHECK(service.isFrequentSiteHidden(amazon, kChromium) && !service.isFrequentSiteHidden(amazon, kWebKit));
  }
  {
    // Restart: every engine keeps its own row; the library is intact.
    BrowserProfileService reloaded(root.path(), nullptr, nullptr, false, nullptr);
    CHECK(containsUrl(reloaded.quickAccessItems(kChromium), amazon) && !containsUrl(reloaded.quickAccessItems(kChromium), youtube));
    CHECK(!containsUrl(reloaded.quickAccessItems(kChromium), instagram));
    CHECK(containsUrl(reloaded.quickAccessItems(kWebKit), instagram) && !containsUrl(reloaded.quickAccessItems(kWebKit), amazon));
    CHECK(reloaded.quickAccessItems(kWebKit).first().url == facebook);  // the dragged order persists
    CHECK(reloaded.isBookmarked(youtube) && reloaded.isBookmarked(docs));
    CHECK(reloaded.isFrequentSiteHidden(amazon, kChromium) && !reloaded.isFrequentSiteHidden(amazon, kWebKit));
  }
  std::cout << "[PASS] shortcut row / Quick Access and Frequent Sites are per engine; the bookmark library is shared and intact"
            << std::endl;
}

// Engine names from callers and settings select the same workspace whatever
// their case or surrounding whitespace. An unknown name is Chromium's
// session, but the running engine's profile data.
void checkEngineNameNormalization() {
  using dalinira::engine::EngineBackend;
  using dalinira::engine::EngineType;
  CHECK(SessionStore::normalizeEngine(QStringLiteral(" WebKit ")) == kWebKit);
  CHECK(SessionStore::normalizeEngine(QStringLiteral("CHROMIUM")) == kChromium);
  CHECK(SessionStore::normalizeEngine(QStringLiteral(" Gecko ")) == kGecko);
  CHECK(SessionStore::normalizeEngine(QString()) == kChromium);
  CHECK(SessionStore::normalizeEngine(QStringLiteral("blink")) == kChromium);

  QTemporaryDir root;
  CHECK(root.isValid());
  const SessionStore store(root.path(), QStringLiteral("WEBKIT"));
  CHECK(store.sessionFilePath() == store.sessionFilePath(kWebKit));
  CHECK(store.sessionFilePath(QStringLiteral("\twebkit\n")) == store.sessionFilePath(kWebKit));
  CHECK(store.sessionFilePath(QStringLiteral("blink")) == store.sessionFilePath(kChromium));
  CHECK(store.sessionFilePath(QStringLiteral("GECKO")) == root.path() + QStringLiteral("/session/gecko/tabs.session.json"));

  BrowserProfileService service(root.path(), nullptr, nullptr, false, nullptr);
  const QUrl cased(QStringLiteral("https://cased.example.org/"));
  CHECK(service.addQuickAccess(cased, QStringLiteral("Cased"), QStringLiteral(" WebKit ")));
  CHECK(service.quickAccess(kWebKit).contains(cased) && !service.quickAccess(kChromium).contains(cased));
  for (const EngineType running : {EngineType::Chromium, EngineType::WebKit, EngineType::Gecko}) {
    EngineBackend::setSelectedEngineType(running);
    const QString runningKey = dalinira::engine::engineKey(running);
    const QUrl unknown(QStringLiteral("https://unknown-%1.example.org/").arg(runningKey));
    const QUrl unnamed(QStringLiteral("https://unnamed-%1.example.org/").arg(runningKey));
    CHECK(service.addQuickAccess(unknown, QStringLiteral("Unknown"), QStringLiteral("blink")));
    CHECK(service.addQuickAccess(unnamed, QStringLiteral("Unnamed"), QString()));
    for (const QString &key : {kChromium, kWebKit, kGecko}) {
      const bool own = key == runningKey;
      CHECK(service.quickAccess(key).contains(unknown) == own && service.quickAccess(key).contains(unnamed) == own);
    }
  }
  EngineBackend::setSelectedEngineType(EngineType::Chromium);
  std::cout << "PASS engine name normalization" << std::endl;
}

// Gecko is a third workspace: its session, history, shortcut row and New Tab
// background never read or change Chromium's or WebKit's, and theirs never
// change it.
void checkGeckoWorkspaceIsolation() {
  using dalinira::engine::EngineBackend;
  using dalinira::engine::EngineType;
  QTemporaryDir root;
  CHECK(root.isValid());
  SessionStore store(root.path());
  const QVector<SavedTab> chromium{savedTab(QStringLiteral("https://github.com/"), QStringLiteral("GitHub"), true)};
  const QVector<SavedTab> webkit{savedTab(QStringLiteral("https://www.instagram.com/"), QStringLiteral("Instagram"), true)};
  const QVector<SavedTab> gecko{savedTab(QStringLiteral("https://www.mozilla.org/"), QStringLiteral("Mozilla"), false, true),
                                savedTab(QStringLiteral("https://example.org/"), QStringLiteral("Example"), true)};
  CHECK(store.saveSavedTabs(chromium, kChromium));
  CHECK(store.saveSavedTabs(webkit, kWebKit));
  const QByteArray chromiumBytes = readAll(store.sessionFilePath(kChromium));
  const QByteArray webkitBytes = readAll(store.sessionFilePath(kWebKit));
  CHECK(store.load(kGecko).isEmpty());  // a new workspace starts empty
  CHECK(store.saveSavedTabs(gecko, kGecko));
  CHECK(store.sessionFilePath(kGecko) == root.path() + QStringLiteral("/session/gecko/tabs.session.json"));
  CHECK(readAll(store.sessionFilePath(kChromium)) == chromiumBytes);
  CHECK(readAll(store.sessionFilePath(kWebKit)) == webkitBytes);
  CHECK(urls(store.load(kGecko)) == urls(gecko) && store.load(kGecko)[0].pinned && store.load(kGecko)[1].active);
  const QByteArray geckoBytes = readAll(store.sessionFilePath(kGecko));
  CHECK(store.saveSavedTabs({savedTab(QStringLiteral("https://www.amazon.com/"), QStringLiteral("Amazon"), true)}, kChromium));
  CHECK(store.saveSavedTabs({savedTab(QStringLiteral("https://www.facebook.com/"), QStringLiteral("Facebook"), true)}, kWebKit));
  CHECK(readAll(store.sessionFilePath(kGecko)) == geckoBytes);
  CHECK(urls(store.load(kChromium)) == QStringList{QStringLiteral("https://www.amazon.com/")});

  // Legacy migration only ever seeds Chromium.
  QTemporaryDir legacyRoot;
  CHECK(legacyRoot.isValid());
  writeLegacySession(legacyRoot.path(), {QStringLiteral("https://legacy.example.org/")});
  CHECK(SessionStore::migrateLegacySessionIfNeeded(legacyRoot.path()));
  CHECK(SessionStore(legacyRoot.path()).load(kGecko).isEmpty());

  // History and the shortcut row.
  const QUrl mozilla(QStringLiteral("https://www.mozilla.org/"));
  const QUrl github(QStringLiteral("https://github.com/"));
  {
    BrowserProfileService service(root.path(), nullptr, nullptr, false, nullptr);
    // Chromium's and WebKit's counters exist before Gecko's first visit, as in
    // every profile that used them. (The first Chromium counter is seeded from
    // the shared history list, whichever engine recorded it.)
    const QUrl instagram(QStringLiteral("https://www.instagram.com/"));
    for (int i = 0; i < 3; ++i) {
      service.recordHistory(github, QStringLiteral("GitHub"), true, kChromium);
      service.recordHistory(instagram, QStringLiteral("Instagram"), true, kWebKit);
    }
    // Gecko's counter starts empty: it takes nothing from the shared history.
    CHECK(service.frequentSites(10, kGecko).isEmpty());
    for (int i = 0; i < 3; ++i) service.recordHistory(mozilla, QStringLiteral("Mozilla"), true, kGecko);
    CHECK(!containsHost(service.frequentSites(10, kGecko), instagram.host()));
    CHECK(containsHost(service.frequentSites(10, kWebKit), instagram.host()));
    CHECK(containsHost(service.frequentSites(10, kGecko), mozilla.host()));
    CHECK(!containsHost(service.frequentSites(10, kChromium), mozilla.host()));
    CHECK(!containsHost(service.frequentSites(10, kWebKit), mozilla.host()));
    CHECK(containsHost(service.frequentSites(10, kChromium), github.host()));
    CHECK(!containsHost(service.frequentSites(10, kGecko), github.host()));
    CHECK(service.hideFrequentSite(mozilla, kGecko));
    CHECK(service.isFrequentSiteHidden(mozilla, kGecko) && !service.isFrequentSiteHidden(mozilla, kChromium));

    CHECK(service.addQuickAccess(mozilla, QStringLiteral("Mozilla"), kGecko));
    CHECK(service.quickAccess(kGecko).contains(mozilla));
    CHECK(!service.quickAccess(kChromium).contains(mozilla) && !service.quickAccess(kWebKit).contains(mozilla));
    CHECK(service.addQuickAccess(github, QStringLiteral("GitHub"), kChromium));
    CHECK(!service.quickAccess(kGecko).contains(github));
  }
  {
    BrowserProfileService reloaded(root.path(), nullptr, nullptr, false, nullptr);
    CHECK(reloaded.quickAccess(kGecko).contains(mozilla) && !reloaded.quickAccess(kGecko).contains(github));
    CHECK(reloaded.isFrequentSiteHidden(mozilla, kGecko));
  }

  // The managed New Tab background: Chromium keeps its location, WebKit and
  // Gecko have their own.
  const QString base = QFileInfo(root.path()).absoluteFilePath();
  QStringList backgrounds;
  for (const EngineType running : {EngineType::Chromium, EngineType::WebKit, EngineType::Gecko}) {
    EngineBackend::setSelectedEngineType(running);
    BrowserProfileService service(root.path(), nullptr, nullptr, false, nullptr);
    CHECK(service.newTabBackgroundStore());
    backgrounds.append(QFileInfo(service.newTabBackgroundStore()->managedImagePath()).absolutePath());
  }
  EngineBackend::setSelectedEngineType(EngineType::Chromium);
  CHECK(!backgrounds[0].contains(QStringLiteral("/webkit/")) && !backgrounds[0].contains(QStringLiteral("/gecko/")));
  CHECK(backgrounds[1].startsWith(base + QStringLiteral("/webkit/ui")));
  CHECK(backgrounds[2].startsWith(base + QStringLiteral("/gecko/ui")));
  CHECK(QSet<QString>(backgrounds.begin(), backgrounds.end()).size() == 3);
  std::cout << "PASS gecko workspace isolation" << std::endl;
}

// The bookmark bar's drag-and-drop ordering, on plain widgets.
void checkBookmarkBarReorder() {
  const QList<QUrl> urls{QUrl(QStringLiteral("https://a.example/")), QUrl(QStringLiteral("https://b.example/")),
                         QUrl(QStringLiteral("https://c.example/"))};
  QWidget bar;
  auto *layout = new QHBoxLayout(&bar);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  QList<QPair<QUrl, int>> moves;
  BookmarkBarReorder reorder(&bar, [&](const QUrl &url, int toIndex) { moves.append({url, toIndex}); });
  for (const QUrl &url : urls) {
    auto *item = new QWidget(&bar);
    item->setFixedSize(100, 28);
    auto *button = new QToolButton(item);
    layout->addWidget(item);
    reorder.addItem(item, button, url);
  }
  bar.resize(300, 28);
  bar.show();
  layout->activate();
  QCoreApplication::processEvents();

  CHECK(reorder.insertionSlot(QPoint(10, 10)) == 0);
  CHECK(reorder.insertionSlot(QPoint(60, 10)) == 1);
  CHECK(reorder.insertionSlot(QPoint(290, 10)) == 3);
  CHECK(BookmarkBarReorder::targetIndex(0, 3) == 2 && BookmarkBarReorder::targetIndex(2, 0) == 0);
  CHECK(BookmarkBarReorder::targetIndex(1, 1) == 1 && BookmarkBarReorder::targetIndex(1, 2) == 1);

  // Dropping the first item after the last asks for index 2, after the drop
  // returns (the re-render deletes the dragged widget).
  QMimeData mime;
  mime.setData(QString::fromLatin1(BookmarkBarReorder::kMimeType), urls[0].toString(QUrl::FullyEncoded).toUtf8());
  QDragEnterEvent enter(QPoint(290, 10), Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(&bar, &enter);
  CHECK(enter.isAccepted() && reorder.isIndicatorVisible());
  QDropEvent drop(QPointF(290, 10), Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(&bar, &drop);
  CHECK(drop.isAccepted() && moves.isEmpty() && !reorder.isIndicatorVisible());
  QCoreApplication::processEvents();
  CHECK(moves.size() == 1 && moves[0].first == urls[0] && moves[0].second == 2);

  // Dropping an item onto its own slot moves nothing; foreign drags are refused.
  QDropEvent sameSlot(QPointF(10, 10), Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(&bar, &sameSlot);
  QCoreApplication::processEvents();
  CHECK(moves.size() == 1);
  QMimeData text;
  text.setText(QStringLiteral("x"));
  QDragEnterEvent foreign(QPoint(10, 10), Qt::MoveAction, &text, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(&bar, &foreign);
  CHECK(!foreign.isAccepted());
  std::cout << "PASS bookmark bar drag reorder" << std::endl;
}

int runLogic(int argc, char *argv[]) {
  QApplication app(argc, argv);
  QTemporaryDir settingsRoot;
  CHECK(settingsRoot.isValid());
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsRoot.path());
  app.setOrganizationName(QStringLiteral("DaliNiraTest"));
  app.setApplicationName(QStringLiteral("EngineWorkspaceTest"));
  dalinira::engine::EngineBackend::setSelectedEngineType(dalinira::engine::EngineType::Chromium);
  std::cout << "=== ENGINE WORKSPACES (LOGIC) ===" << std::endl;
  checkWorkspaceStore();
  checkLegacyMigration();
  checkProfileData();
  checkEngineNameNormalization();
  checkGeckoWorkspaceIsolation();
  checkBookmarkBarReorder();
  std::cout << "=== ALL ENGINE WORKSPACE LOGIC TESTS PASSED ===" << std::endl;
  return 0;
}

// ---------------------------------------------------------------------------
// GUI tier: one engine per process, as DaliNira runs.
// ---------------------------------------------------------------------------

// Engine processes started by this process (descendants whose name starts
// with one of the prefixes; /proc/<pid>/stat truncates names to 15 chars).
QList<qint64> descendantProcesses(const QList<QByteArray> &prefixes) {
  QHash<qint64, qint64> parent;
  QHash<qint64, QByteArray> comm;
  for (const QString &entry : QDir(QStringLiteral("/proc")).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
    QFile stat(QStringLiteral("/proc/%1/stat").arg(entry));
    if (!stat.open(QIODevice::ReadOnly)) continue;
    const QByteArray text = stat.readAll();
    const int open = text.indexOf('(');
    const int close = text.lastIndexOf(')');
    const QList<QByteArray> fields = text.mid(close + 2).split(' ');
    if (open < 0 || close < open || fields.size() < 2 || fields.at(0) == "Z") continue;
    comm.insert(entry.toLongLong(), text.mid(open + 1, close - open - 1));
    parent.insert(entry.toLongLong(), fields.at(1).toLongLong());
  }
  QList<qint64> result;
  for (auto it = comm.cbegin(); it != comm.cend(); ++it) {
    bool matches = false;
    for (const QByteArray &prefix : prefixes) matches = matches || it.value().startsWith(prefix);
    if (!matches) continue;
    for (qint64 p = parent.value(it.key()), depth = 0; p > 1 && depth < 16; p = parent.value(p), ++depth)
      if (p == QCoreApplication::applicationPid()) { result.append(it.key()); break; }
  }
  return result;
}
const QList<QByteArray> kWebKitProcesses{"WebKitWebProces", "WebKitNetworkPr", "WebKitGPUProces"};
const QList<QByteArray> kChromiumProcesses{"QtWebEngineProc"};

QStringList tabTitles(BrowserWindow &window) {
  QStringList titles;
  for (int i = 0; i < window.tabCount(); ++i) titles.append(window.tabInfo(i).title);
  return titles;
}

bool pageReady(BrowserWindow &window, int index, const QString &title) {
  return waitUntil([&] {
    return index < window.tabCount() && window.tabInfo(index).title == title && !window.tabInfo(index).isLoading;
  }, 20000);
}

int indexOfTitle(BrowserWindow &window, const QString &title) {
  for (int i = 0; i < window.tabCount(); ++i)
    if (window.tabInfo(i).title == title) return i;
  return -1;
}

void checkPinned(BrowserWindow &window, const QStringList &pinnedTitles) {
  for (int i = 0; i < window.tabCount(); ++i) {
    const bool pinned = pinnedTitles.contains(window.tabInfo(i).title);
    CHECK(window.tabInfo(i).isPinned == pinned);
    CHECK(window.tabStrip()->isTabPinned(i) == pinned);
  }
}

void checkRow(BrowserProfileService &profile, const QString &engine, const QUrl &own, const QUrl &other) {
  CHECK(containsUrl(profile.quickAccessItems(engine), own));
  CHECK(!containsUrl(profile.quickAccessItems(engine), other));
}

// One DaliNira run: `phase` is chromium-1, webkit-1, chromium-2 or webkit-2.
int runPhase(const QString &phase, const QString &base, int argc, char *argv[]) {
  const bool webkit = phase.startsWith(kWebKit);
  const QString engine = webkit ? kWebKit : kChromium;
  const auto engineType = webkit ? dalinira::engine::EngineType::WebKit : dalinira::engine::EngineType::Chromium;
  // As main.cpp: only the selected engine is ever initialized in the process.
  dalinira::engine::EngineBackend::setSelectedEngineType(engineType);
  if (webkit) QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
  else registerDaliNiraUrlSchemes();
  QApplication app(argc, argv);
  dalinira::application_identity::apply();
  const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  QDir().mkpath(dataDir);
  std::cout << "[" << phase.toStdString() << "] profile " << dataDir.toStdString() << std::endl;

  const QUrl youtube(base + QStringLiteral("/youtube"));
  const QUrl github(base + QStringLiteral("/github"));
  const QUrl amazon(base + QStringLiteral("/amazon"));
  const QUrl instagram(base + QStringLiteral("/instagram"));
  const QUrl facebook(base + QStringLiteral("/facebook"));
  const QString youtubeTitle = QStringLiteral("YouTube fixture");
  const QString githubTitle = QStringLiteral("GitHub fixture");
  const QString amazonTitle = QStringLiteral("Amazon fixture");
  const QString instagramTitle = QStringLiteral("Instagram fixture");
  const QString facebookTitle = QStringLiteral("Facebook fixture");

  BrowserProfileService profileService(dataDir, nullptr, &app);
  dalinira::engine::EngineStorageManager engineStorage(dataDir);
  if (!webkit) {
    auto backend = std::make_unique<dalinira::engine::ChromiumBackend>(profileService.profile(), &app);
    CHECK(backend->initialize(engineStorage));
    dalinira::engine::EngineBackend::registerBackend(dalinira::engine::EngineType::Chromium, std::move(backend));
  }
#if defined(DALINIRA_BUILD_WEBKIT)
  else {
    auto backend = std::make_unique<dalinira::engine::WebKitBackend>(&app);
    CHECK(backend->initialize(engineStorage));
    CHECK(backend->enableNetworkFiltering());
    dalinira::engine::EngineBackend::registerBackend(dalinira::engine::EngineType::WebKit, std::move(backend));
  }
#else
  else {
    std::cout << "[SKIP] built without WebKit" << std::endl;
    return 77;
  }
#endif

  TabManager tabManager(&app);
  tabManager.performanceManager()->setPolicyMode(dalinira::PerformancePolicyMode::Off);  // explicit sleep only
  SessionStore::migrateLegacySessionIfNeeded(dataDir);
  SessionStore sessionStore(dataDir, engine);
  BrowserServices services;
  services.profile = webkit ? nullptr : profileService.profile();
  services.profileService = &profileService;
  services.tabManager = &tabManager;
  services.sessionStore = &sessionStore;

  auto *window = new BrowserWindow(services);
  window->resize(1100, 720);
  window->show();
  CHECK(window->currentEngineScope() == engine);

  // Startup restore, as main.cpp does it.
  const QVector<SavedTab> saved = sessionStore.load(engine);
  if (!saved.isEmpty()) window->restoreSession(saved);
  else window->ensureInitialTab();

  // The other engine is not running in this process at all.
  const auto &otherProcesses = webkit ? kChromiumProcesses : kWebKitProcesses;

  if (phase == QLatin1String("chromium-1")) {
    CHECK(saved.isEmpty() && window->tabCount() == 1);
    window->navigateCurrentTab(youtube);
    CHECK(pageReady(*window, 0, youtubeTitle));
    window->addNewTab(github);
    CHECK(pageReady(*window, 1, githubTitle));
    window->addNewTab(amazon);
    CHECK(pageReady(*window, 2, amazonTitle));
    window->toggleTabPin(0);  // YouTube pinned (stays in slot 0)
    window->switchTab(1);     // GitHub active
    CHECK(waitUntil([&] { return window->sleepTabAt(2); }, 5000) && window->isTabSleeping(2));  // Amazon sleeping
    window->toggleCurrentBookmark();  // GitHub on Chromium's shortcut row
    CHECK(profileService.createBookmarkFolder(QStringLiteral("Fixtures")));
    CHECK(profileService.addBookmark(youtube, youtubeTitle, QStringLiteral("Fixtures")));  // global library
  } else if (phase == QLatin1String("webkit-1")) {
    // WebKit starts with its own clean workspace: none of Chromium's tabs.
    CHECK(saved.isEmpty());
    CHECK(window->tabCount() == 1 && window->tabInfo(0).url == QUrl(QStringLiteral("dalinira://newtab/")));
    CHECK(profileService.quickAccessItems(engine).isEmpty());
    CHECK(profileService.bookmarkFolders().contains(QStringLiteral("Fixtures")) && profileService.isBookmarked(youtube));
    window->navigateCurrentTab(instagram);
    CHECK(pageReady(*window, 0, instagramTitle));
    window->addNewTab(facebook);
    CHECK(pageReady(*window, 1, facebookTitle));
    window->toggleTabPin(1);  // Facebook pinned: moves to slot 0
    CHECK(tabTitles(*window) == QStringList({facebookTitle, instagramTitle}));
    window->switchTab(1);     // Instagram active
    window->toggleCurrentBookmark();  // Instagram on WebKit's shortcut row
    checkRow(profileService, engine, instagram, github);
    checkRow(profileService, kChromium, github, instagram);
    CHECK(descendantProcesses(kWebKitProcesses).size() <= 4);
  } else if (phase == QLatin1String("chromium-2")) {
    CHECK(urls(saved) == QStringList({youtube.toString(), github.toString(), amazon.toString()}));
    CHECK(!saved[0].active && saved[1].active && !saved[2].active && saved[2].sleeping);
    CHECK(tabTitles(*window) == QStringList({youtubeTitle, githubTitle, amazonTitle}));
    CHECK(window->tabStrip()->currentIndex() == 1);
    checkPinned(*window, {youtubeTitle});
    // The tab that slept before the restart sleeps again once loaded.
    CHECK(waitUntil([&] { return window->isTabSleeping(2); }, 20000));
    CHECK(window->tabStrip()->isTabSleeping(2));
    CHECK(!window->isTabSleeping(0) && !window->isTabSleeping(1));
    checkRow(profileService, engine, github, instagram);
    CHECK(profileService.isBookmarked(youtube));
  } else if (phase == QLatin1String("webkit-2")) {
    CHECK(urls(saved) == QStringList({facebook.toString(), instagram.toString()}));
    CHECK(tabTitles(*window) == QStringList({facebookTitle, instagramTitle}));
    CHECK(window->tabStrip()->currentIndex() == 1);
    checkPinned(*window, {facebookTitle});
    CHECK(pageReady(*window, 1, instagramTitle));
    checkRow(profileService, engine, instagram, github);
    CHECK(profileService.isBookmarked(youtube));
    CHECK(descendantProcesses(kWebKitProcesses).size() <= 4);
  } else {
    CHECK(false && "unknown phase");
  }
  CHECK(indexOfTitle(*window, webkit ? githubTitle : instagramTitle) < 0);  // never the other workspace's tabs
  const QStringList titles = tabTitles(*window);
  CHECK(QSet<QString>(titles.cbegin(), titles.cend()).size() == window->tabCount());  // no duplicates
  CHECK(descendantProcesses(otherProcesses).isEmpty());
  std::cout << "[" << phase.toStdString() << "] tabs: " << tabTitles(*window).join(QStringLiteral(" | ")).toStdString()
            << " (active " << window->tabStrip()->currentIndex() << ")" << std::endl;

  // Quit as DaliNira does: save the workspace, close the window, then the backend.
  window->saveSessionNow();
  window->close();
  delete window;
  dalinira::engine::EngineBackend::shutdown();
  if (webkit) CHECK(waitUntil([] { return descendantProcesses(kWebKitProcesses).isEmpty(); }, 25000));
  std::cout << "[" << phase.toStdString() << "] PASS" << std::endl;
  return 0;
}

class LocalSite {
 public:
  LocalSite() {
    CHECK(server_.listen(QHostAddress::LocalHost));
    QObject::connect(&server_, &QTcpServer::newConnection, [this] {
      while (QTcpSocket *socket = server_.nextPendingConnection()) {
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket] {
          const QByteArray path = socket->readAll().split(' ').value(1);
          static const QHash<QByteArray, QByteArray> titles{
              {"/youtube", "YouTube fixture"}, {"/github", "GitHub fixture"}, {"/amazon", "Amazon fixture"},
              {"/instagram", "Instagram fixture"}, {"/facebook", "Facebook fixture"}};
          const QByteArray title = titles.value(path);
          const QByteArray status = title.isEmpty() ? "404 Not Found" : "200 OK";
          const QByteArray body = "<!doctype html><title>" + title + "</title><p>" + title + "</p>";
          socket->write("HTTP/1.1 " + status + "\r\nContent-Type: text/html\r\nConnection: close\r\nContent-Length: " +
                        QByteArray::number(body.size()) + "\r\n\r\n" + body);
          socket->disconnectFromHost();
        });
        QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
      }
    });
  }
  QString base() const { return QStringLiteral("http://127.0.0.1:%1").arg(server_.serverPort()); }

 private:
  QTcpServer server_;
};

int runGui(int argc, char *argv[]) {
  QCoreApplication app(argc, argv);
  QTemporaryDir home;
  CHECK(home.isValid());
  LocalSite site;
  std::cout << "=== ENGINE WORKSPACE SWITCH (GUI, one engine per process) ===" << std::endl;
  QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
  environment.insert(QStringLiteral("XDG_CONFIG_HOME"), home.filePath(QStringLiteral("config")));
  environment.insert(QStringLiteral("XDG_DATA_HOME"), home.filePath(QStringLiteral("data")));
  environment.insert(QStringLiteral("XDG_CACHE_HOME"), home.filePath(QStringLiteral("cache")));

  for (const QString &phase : {QStringLiteral("chromium-1"), QStringLiteral("webkit-1"),
                               QStringLiteral("chromium-2"), QStringLiteral("webkit-2")}) {
    QProcess child;
    child.setProcessEnvironment(environment);
    child.setProcessChannelMode(QProcess::ForwardedChannels);
    QEventLoop loop;  // keeps the local site serving while the phase runs
    QObject::connect(&child, &QProcess::finished, &loop, &QEventLoop::quit);
    QObject::connect(&child, &QProcess::errorOccurred, &loop, &QEventLoop::quit);
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    child.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--phase"), phase, site.base()});
    timeout.start(90000);
    loop.exec();
    if (child.state() != QProcess::NotRunning) {
      std::cout << "[FAIL] phase " << phase.toStdString() << " timed out" << std::endl;
      child.kill();
      child.waitForFinished(5000);
      return 1;
    }
    if (child.exitStatus() != QProcess::NormalExit || child.exitCode() != 0) {
      std::cout << "[FAIL] phase " << phase.toStdString() << " exit " << child.exitCode() << std::endl;
      return child.exitCode() == 77 ? 77 : 1;
    }
  }
  std::cout << "=== ENGINE WORKSPACE SWITCH PASSED ===" << std::endl;
  return 0;
}

}  // namespace

int main(int argc, char *argv[]) {
  for (int i = 1; i < argc; ++i) {
    const QByteArray arg(argv[i]);
    if (arg == "--gui") return runGui(argc, argv);
    if (arg == "--phase" && i + 2 < argc)
      return runPhase(QString::fromLocal8Bit(argv[i + 1]), QString::fromLocal8Bit(argv[i + 2]), argc, argv);
  }
  return runLogic(argc, argv);
}
