// WebKit backend GUI test: real WebKitEngineViews on the desktop display.
//
// Run through scripts/safe-gui-test.sh (the GUI tier). Every case uses the
// harness's single window and at most one view; the view is destroyed and its
// web process has exited before the next case starts (webkit_gui_harness.h).
// Pages are served locally. Opt-in extras:
//   DALINIRA_LIVE_TESTS=1  live DuckDuckGo / YouTube navigation (--live)
// The engine-neutral logic (identities, storage, resolver, UI profiles) is in
// webkit_engine_logic_test.cpp, which runs without any window.

#include <QApplication>
#include <QBuffer>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImage>
#include <QKeyEvent>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <iostream>

#include "blocker/cosmetic_runtime.h"
#include "blocker/element_picker_runtime.h"
#include "blocker/dalinira_blocker_service.h"
#include "core/adult_content_protection.h"
#include "core/search_suggestion_service.h"
#include "engine/engine_backend.h"
#include "engine/engine_storage_manager.h"
#include "engine/webkit/webkit_backend.h"
#include "engine/webkit/webkit_engine_view.h"
#include "newtab/new_tab_suggest_command.h"
#include "newtab/new_tab_shortcuts.h"
#include "newtab/new_tab_scheme.h"
#include "core/browser_profile_data_provider.h"
#include "translate/translate_engine_script.h"
#include "suggestion_test_transport.h"
#include "webkit_gui_harness.h"
#undef signals
#include "engine/webkit/webkit_engine_profile.h"

#include <webkit2/webkit2.h>

using dalinira::engine::EngineBackend;
using dalinira::engine::EngineType;
using dalinira::engine::EngineView;
using dalinira::engine::WebKitBackend;
using dalinira::engine::WebKitEngineView;
using dalinira::test::WebKitGuiHarness;

namespace {

// Serves `responder(path)` as the body of every request, one connection each.
// Another loopback address (127.0.0.2) is another site to WebKit.
struct LocalSite {
  using Responder = std::function<std::pair<QByteArray, QByteArray>(const QByteArray &path)>;  // {type, body}
  QTcpServer server;
  Responder responder;
  explicit LocalSite(Responder r, const QHostAddress &address = QHostAddress(QHostAddress::LocalHost))
      : responder(std::move(r)) {
    WK_CHECK(server.listen(address));
    QObject::connect(&server, &QTcpServer::newConnection, [this] {
      while (QTcpSocket *socket = server.nextPendingConnection()) {
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
          const QByteArray path = socket->readAll().split(' ').value(1);
          const auto [type, body] = responder(path);
          socket->write("HTTP/1.1 200 OK\r\nContent-Type: " + type + "\r\nConnection: close\r\nContent-Length: " +
                        QByteArray::number(body.size()) + "\r\n\r\n" + body);
          socket->disconnectFromHost();
        });
        QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
      }
    });
  }
  QString base() const {
    return QStringLiteral("http://%1:%2").arg(server.serverAddress().toString()).arg(server.serverPort());
  }
};

WebKitBackend *webKitBackend() {
  return static_cast<WebKitBackend *>(EngineBackend::backendForType(EngineType::WebKit));
}

WebKitEngineView *newView(WebKitGuiHarness &h, const char *label) {
  return h.attach(qobject_cast<WebKitEngineView *>(webKitBackend()->createView(h.stack())), label);
}

// ---------------------------------------------------------------------------
// Case 1: one page — load, JavaScript, reload, resize, zoom, find, DOM
// injection, adult-content redirect, favicon accessor, live sites (opt-in).
// ---------------------------------------------------------------------------
void caseBasicPage(WebKitGuiHarness &h, const QUrl &examplePage) {
  std::cout << "[CASE 1] Local page: load, JavaScript, reload, resize, zoom, find..." << std::endl;
  auto *view = newView(h, "basic page");
  WK_CHECK(view->engineType() == EngineType::WebKit);

  bool loadStarted = false;
  QString title;
  QUrl lastUrl;
  QObject::connect(view, &EngineView::loadStarted, [&] { loadStarted = true; });
  QObject::connect(view, &EngineView::urlChanged, [&](const QUrl &u) { lastUrl = u; });
  QObject::connect(view, &EngineView::titleChanged, [&](const QString &t) { title = t; });
  WK_CHECK(h.loadAndWait(view, examplePage));
  h.waitUntil([&] { return !title.isEmpty(); }, 2000);
  WK_CHECK(loadStarted && lastUrl.host() == examplePage.host());
  WK_CHECK(title == "Example Domain" || view->title() == "Example Domain");
  std::cout << "[PASS] Page loaded via the WebKit network stack." << std::endl;

  WK_CHECK(h.js(view, QStringLiteral("document.title + ' | ' + document.querySelector('h1').innerText")).toString() ==
           QStringLiteral("Example Domain | Example Domain"));
  std::cout << "[PASS] JavaScript runs in the real document context." << std::endl;

  bool reloadStarted = false;
  QObject::connect(view, &EngineView::loadStarted, [&] { reloadStarted = true; });
  view->reload();
  WK_CHECK(h.waitUntil([&] { return reloadStarted; }, 3000));
  WK_CHECK(h.waitUntil([&] { return !view->isLoading(); }, 5000));
  std::cout << "[PASS] Reload." << std::endl;

  h.shell().resize(1024, 768);
  h.waitUntil([&] { return view->width() > 900; }, 2000);
  WK_CHECK(view->width() > 0 && view->height() > 0);
  h.shell().resize(800, 600);
  std::cout << "[PASS] Resize." << std::endl;

  view->setZoomFactor(1.5);
  WK_CHECK(qFuzzyCompare(view->zoomFactor(), 1.5));
  view->setZoomFactor(1.0);
  WK_CHECK(qFuzzyCompare(view->zoomFactor(), 1.0));
  view->findText(QStringLiteral("Example"));
  h.pump(50);
  view->clearFind();
  WK_CHECK(h.js(view, QStringLiteral("document.head.appendChild(document.createElement('style')).id='__dalinira_cosmetic';"
                                     "document.getElementById('__dalinira_cosmetic') !== null;"))
               .toBool());
  (void)view->favicon();
  // New-window routing is configured without opening a second view.
  view->setNewTabCallback([](const WebKitEngineView::NewTabRequest &) -> WebKitEngineView * { return nullptr; });
  std::cout << "[PASS] Zoom, find, DOM style injection, favicon accessor, new-window callback." << std::endl;

  auto &adultService = dalinira::core::AdultContentProtectionService::instance();
  adultService.addDomain(QStringLiteral("evil-adult-test-site.org"));
  QSettings().setValue(QStringLiteral("privacy/adultContentProtection"), true);
  view->load(QUrl(QStringLiteral("https://evil-adult-test-site.org/browse")));
  WK_CHECK(view->url().toString().startsWith(QStringLiteral("dalinira://blocked/adult")));
  QSettings().setValue(QStringLiteral("privacy/adultContentProtection"), false);
  view->load(QUrl(QStringLiteral("https://evil-adult-test-site.org/browse")));
  WK_CHECK(view->url().toString() == QStringLiteral("https://evil-adult-test-site.org/browse"));
  QSettings().setValue(QStringLiteral("privacy/adultContentProtection"), true);
  view->load(QUrl(QStringLiteral("https://evil-adult-test-site.org/browse")));
  WK_CHECK(view->url().toString().startsWith(QStringLiteral("dalinira://blocked/adult")));
  std::cout << "[PASS] Adult content protection redirects blocked hosts and follows its toggle." << std::endl;

  if (dalinira::test::liveSiteTestsEnabled()) {
    std::cout << "[LIVE] DuckDuckGo and YouTube navigation..." << std::endl;
    WK_CHECK(h.loadAndWait(view, QUrl(QStringLiteral("https://duckduckgo.com/?q=youtube")), 15000));
    WK_CHECK(view->url().host().contains(QLatin1String("duckduckgo.com")));
    WK_CHECK(h.loadAndWait(view, QUrl(QStringLiteral("https://www.youtube.com/")), 20000));
    WK_CHECK(view->url().host().contains(QLatin1String("youtube.com")));
    std::cout << "[PASS] Live navigation without a web process crash." << std::endl;
  } else {
    std::cout << "[SKIP] Live DuckDuckGo/YouTube navigation (DALINIRA_LIVE_TESTS=1)." << std::endl;
  }
  // Leave a live local document for the harness's service-worker/process
  // lifecycle check. A synthetic blocked URL has no script context.
  WK_CHECK(h.loadAndWait(view, examplePage));
  h.destroyView(view);
}

// ---------------------------------------------------------------------------
// Step 3: real WebKitGTK new-window creation path ($popup boundary).
// ---------------------------------------------------------------------------
void casePopupFiltering(WebKitGuiHarness &h) {
  std::cout << "[CASE 1a] WebKit popup/window-creation filtering..." << std::endl;
  LocalSite site([](const QByteArray &) -> std::pair<QByteArray, QByteArray> {
    return {"text/html; charset=utf-8",
            "<!doctype html><title>popup fixture</title><body>fixture"
            "<a id='oauth' target='_blank' href='/oauth-login'>login</a></body>"};
  });
  auto *view = newView(h, "popup filtering");
  const QUrl opener(site.base() + QStringLiteral("/opener"));
  WK_CHECK(h.loadAndWait(view, opener));

  QList<QUrl> evaluated;
  QList<QUrl> routed;
  view->setPopupEvaluator([&](const QUrl &target) {
    evaluated.append(target);
    return target.path() != QLatin1String("/blocked-popup");
  });
  // The harness intentionally owns one view. A null callback lets us verify
  // policy/create routing without keeping a second WebKit process alive.
  view->setNewTabCallback([](const WebKitEngineView::NewTabRequest &) -> WebKitEngineView * { return nullptr; });
  QObject::connect(view, &EngineView::newWindowRequested, [&](const QUrl &url) { routed.append(url); });

  const QUrl blocked(site.base() + QStringLiteral("/blocked-popup"));
  view->runJavaScript(QStringLiteral("window.open(%1,'_blank')")
                          .arg(QString::fromUtf8(QJsonDocument(QJsonArray{blocked.toString()})
                                                    .toJson(QJsonDocument::Compact)) + QStringLiteral("[0]")));
  WK_CHECK(h.waitUntil([&] { return !evaluated.isEmpty(); }, 3000));
  WK_CHECK(evaluated.last() == blocked);
  h.pump(200);
  WK_CHECK(view->url() == opener && routed.isEmpty());

  bool deferredBlocked = false;
  QObject::connect(view, &WebKitEngineView::deferredPopupBlocked,
                   [&] { deferredBlocked = true; });
  view->armDeferredPopupCheck([&](const QUrl &target) {
    evaluated.append(target);
    return target.path() != QLatin1String("/blocked-popup");
  });
  view->load(blocked);
  WK_CHECK(h.waitUntil([&] { return deferredBlocked; }, 3000));
  WK_CHECK(evaluated.last() == blocked);
  WK_CHECK(view->url() == opener);

  const QUrl allowed(site.base() + QStringLiteral("/oauth-login"));
  view->runJavaScript(QStringLiteral("document.querySelector('#oauth').click()"));
  WK_CHECK(h.waitUntil([&] { return evaluated.contains(allowed); }, 3000));
  WK_CHECK(h.waitUntil([&] { return routed.contains(allowed) || view->url() == allowed; }, 3000));
  std::cout << "[PASS] Matching popup blocked before creation; nonmatching login popup routed." << std::endl;
  h.destroyView(view);
}

// ---------------------------------------------------------------------------
// Step 3: the real WebKit content world runs the shared picker runtime.
// ---------------------------------------------------------------------------
void caseElementPickerRuntime(WebKitGuiHarness &h) {
  std::cout << "[CASE 1aa] WebKit isolated-world element picker runtime..." << std::endl;
  LocalSite site([](const QByteArray &) -> std::pair<QByteArray, QByteArray> {
    return {"text/html; charset=utf-8",
            "<!doctype html><title>picker fixture</title><body>"
            "<div id='pick-ad'>ad</div><main id='content'>content</main></body>"};
  });
  auto *view = newView(h, "element picker runtime");
  WK_CHECK(h.loadAndWait(view, QUrl(site.base() + QStringLiteral("/picker"))));

  auto isolated = [&](const QString &script) {
    QVariant result;
    bool done = false;
    view->runIsolatedJavaScript(script, [&](const QVariant &value) {
      result = value;
      done = true;
    });
    WK_CHECK(h.waitUntil([&] { return done; }, 3000));
    return result;
  };

  QJsonObject strings;
  for (const QString &key : {QStringLiteral("title"), QStringLiteral("hint"),
                             QStringLiteral("cancel"), QStringLiteral("matches"),
                             QStringLiteral("tooBroad"), QStringLiteral("previewOff"),
                             QStringLiteral("previewOn"), QStringLiteral("parent"),
                             QStringLiteral("repick"), QStringLiteral("create"),
                             QStringLiteral("noCandidate"), QStringLiteral("saved"),
                             QStringLiteral("saveFailed"), QStringLiteral("undo"),
                             QStringLiteral("done")})
    strings.insert(key, key);
  const QJsonObject config{{QStringLiteral("strings"), strings},
                           {QStringLiteral("maxCandidates"), 6},
                           {QStringLiteral("maxSelectorLength"), 256},
                           {QStringLiteral("broadMatches"), 100}};
  QString runtime = QString::fromUtf8(kDaliNiraElementPickerRuntime);
  runtime.replace(QStringLiteral("__DALINIRA_PICKER_CONFIG__"),
                  QString::fromUtf8(QJsonDocument(config).toJson(QJsonDocument::Compact)));
  isolated(runtime);

  WK_CHECK(h.js(view, QStringLiteral("typeof window.__daliniraPicker")).toString() ==
           QStringLiteral("undefined"));
  const QString selected = isolated(QStringLiteral(
      "window.__daliniraPicker.select(document.querySelector('#pick-ad'));"
      "window.__daliniraPicker.state()"))
                               .toString();
  WK_CHECK(selected.contains(QStringLiteral("\"selectedId\":\"pick-ad\"")));
  WK_CHECK(isolated(QStringLiteral("window.__daliniraPicker.confirm();"
                                   "window.__daliniraPicker.takeEvent()"))
               .toString()
               .contains(QStringLiteral("\"type\":\"save\"")));
  isolated(QStringLiteral("window.__daliniraPicker.saved(true)"));
  WK_CHECK(h.js(view, QStringLiteral("getComputedStyle(document.querySelector('#pick-ad')).display==='none'"))
               .toBool());
  h.js(view, QStringLiteral("document.body.insertAdjacentHTML('beforeend','<div id=pick-ad>late ad</div>')"));
  WK_CHECK(h.js(view, QStringLiteral("Array.from(document.querySelectorAll('#pick-ad')).every(e=>getComputedStyle(e).display==='none')"))
               .toBool());
  WK_CHECK(isolated(QStringLiteral("window.__daliniraPicker.undo();"
                                   "window.__daliniraPicker.takeEvent()"))
               .toString()
               .contains(QStringLiteral("\"type\":\"undo\"")));
  WK_CHECK(h.js(view, QStringLiteral("getComputedStyle(document.querySelector('#pick-ad')).display!=='none'"))
               .toBool());
  isolated(QStringLiteral("window.__daliniraPicker.close('test')"));
  std::cout << "[PASS] Picker is isolated, selects, saves, covers dynamic DOM, and undoes in WebKit." << std::endl;
  h.destroyView(view);
}

// The exact document-language probe used by PageTranslator must execute in a
// real WebKit document and return JSON (WebKit's QVariant bridge serializes JS
// objects as strings). C++ detector confidence/fallback rules are covered by
// browser-translate-service.
void caseTranslationLanguageProbe(WebKitGuiHarness &h) {
  std::cout << "[CASE 1b] WebKit translation language probe..." << std::endl;
  LocalSite site([](const QByteArray &path) -> std::pair<QByteArray, QByteArray> {
    QByteArray lang;
    QByteArray text;
    if (path.startsWith("/tr")) { lang = "tr"; text = "Bu sayfa Türkçe olarak hazırlanmıştır ve ziyaretçiler için yeterli metin içerir."; }
    else if (path.startsWith("/ar")) { lang = "ar"; text = "هذه صفحة عربية تحتوي على نص كاف لاختبار اكتشاف اللغة بشكل موثوق."; }
    else if (path.startsWith("/fr")) { lang = "fr"; text = "Cette page est rédigée en français et contient assez de texte pour une détection fiable de la langue."; }
    else { lang = path.startsWith("/nometa") ? QByteArray{} : QByteArray("en"); text = "This local page contains enough English text for reliable automatic language detection in the browser."; }
    return {"text/html; charset=utf-8", "<!doctype html><html" + (lang.isEmpty() ? QByteArray{} : " lang='" + lang + "'") + "><body><p>" + text + "</p></body></html>"};
  });
  auto *view = newView(h, "translation language probe");
  for (const QByteArray &path : {QByteArray("/en"), QByteArray("/tr"), QByteArray("/ar"), QByteArray("/fr"), QByteArray("/nometa")}) {
    WK_CHECK(h.loadAndWait(view, QUrl(site.base() + QString::fromLatin1(path))));
    view->runJavaScript(translateEngineScript());
    const QString json = h.js(view, QStringLiteral("JSON.stringify(window.__daliniraTranslate.detect())")).toString();
    const QJsonObject probe = QJsonDocument::fromJson(json.toUtf8()).object();
    WK_CHECK(!probe.value(QStringLiteral("sampleText")).toString().isEmpty());
    if (path == "/nometa") WK_CHECK(probe.value(QStringLiteral("htmlLang")).toString().isEmpty());
    else WK_CHECK(probe.value(QStringLiteral("htmlLang")).toString() == QString::fromLatin1(path.mid(1)));
  }
  std::cout << "[PASS] WebKit exposes metadata and fallback text through the shared translation probe." << std::endl;
  h.destroyView(view);
}

// ---------------------------------------------------------------------------
// Case 2: the shared DaliNira New Tab — keyboard, shared UI, appearance write,
// reload through dalinira://, suggestion bridge.
// ---------------------------------------------------------------------------
void caseCustomShortcuts(WebKitGuiHarness &h) {
  namespace shortcuts = dalinira::newtab::shortcuts;
  QImage image(32,32,QImage::Format_ARGB32);image.fill(Qt::green);
  QByteArray png;QBuffer buffer(&png);buffer.open(QIODevice::WriteOnly);WK_CHECK(image.save(&buffer,"PNG"));
  int iconRequests=0;
  LocalSite icons([&](const QByteArray &path) -> std::pair<QByteArray,QByteArray> { WK_CHECK(path=="/favicon.ico");++iconRequests;return {"image/png",png}; });
  const QString fixtureUrl=icons.base()+QStringLiteral("/");
  class IconData final : public dalinira::core::IBrowserProfileDataProvider {
   public:
    QList<BrowserHistoryEntry> recentHistory() const override { return {}; }
    QList<BrowserFrequentSite> frequentSites(int) const override { return {}; }
    QList<QUrl> bookmarks() const override { return {}; }
  } iconData;
  auto *view = newView(h, "custom shortcuts");view->setNewTabProfileData(&iconData);
  QList<QUrl> commands;
  const auto connection = QObject::connect(view, &EngineView::internalNavigationRequested, [&](const QUrl &url) {
    if (url.host() != QLatin1String("frequent-sites")) return;
    commands.append(url);
    const QUrlQuery query(url);
    if (query.queryItemValue(QStringLiteral("op")) == QLatin1String("add-shortcut"))
      WK_CHECK(shortcuts::add(query.queryItemValue(QStringLiteral("name"), QUrl::FullyDecoded),
          query.queryItemValue(QStringLiteral("url"), QUrl::FullyDecoded)));
    else if (query.queryItemValue(QStringLiteral("op")) == QLatin1String("edit-shortcut"))
      WK_CHECK(shortcuts::edit(query.queryItemValue(QStringLiteral("original"), QUrl::FullyDecoded), query.queryItemValue(QStringLiteral("name"), QUrl::FullyDecoded), query.queryItemValue(QStringLiteral("url"), QUrl::FullyDecoded)));
    else if (query.queryItemValue(QStringLiteral("op")) == QLatin1String("remove-shortcut"))
      shortcuts::remove(QUrl(query.queryItemValue(QStringLiteral("url"), QUrl::FullyDecoded)));
  });
  WK_CHECK(h.loadAndWait(view, QUrl(QStringLiteral("dalinira://newtab/")), 5000));
  WK_CHECK(h.js(view, QStringLiteral(R"JS((()=>{
    const add=document.querySelector('#add-shortcut');if(!add)return false;add.click();
    document.querySelector('#shortcut-name').value='WebKit shortcut';
    document.querySelector('#shortcut-url').value='javascript:alert(1)';validateShortcut();
    if(!document.querySelector('#shortcut-save').disabled)return false;
    document.querySelector('#shortcut-url').value='%1';validateShortcut();
    if(document.querySelector('#shortcut-save').disabled)return false;
    document.querySelector('#shortcut-form').requestSubmit();
    return document.querySelector('#shortcut-overlay').hidden;
  })())JS").arg(fixtureUrl)).toBool());
  WK_CHECK(h.waitUntil([&] { return commands.size() == 1; }, 3000));
  WK_CHECK(shortcuts::load().size() == 1);
  bool fetched=false;requestNewTabShortcutFavicon(&iconData,QUrl(fixtureUrl),view,[&]{fetched=true;});
  WK_CHECK(h.waitUntil([&]{return fetched;},6000));
  const QString iconJson=QString::fromUtf8(QJsonDocument(QJsonArray{newTabFaviconUrl(&iconData,QUrl(fixtureUrl))}).toJson(QJsonDocument::Compact));
  WK_CHECK(h.js(view,QStringLiteral("p.customShortcuts[0].icon=%1[0];renderFrequentSites();true").arg(iconJson)).toBool());
  WK_CHECK(h.waitUntil([&] { return h.js(view, QStringLiteral("document.querySelector('#shortcut-list .shortcut-icon img')?.naturalWidth>0")).toBool(); },3000));
  WK_CHECK(iconRequests>0);
  WK_CHECK(h.js(view, QStringLiteral("document.querySelector('#shortcut-list a').textContent.includes('WebKit shortcut')")).toBool());
  WK_CHECK(h.loadAndWait(view, QUrl(QStringLiteral("dalinira://newtab/")), 5000));
  WK_CHECK(h.js(view, QStringLiteral("p.topSitesSource==='custom'&&document.querySelector('#shortcut-list a').href==='%1'").arg(fixtureUrl)).toBool());
  WK_CHECK(h.js(view, QStringLiteral(R"JS((()=>{
    document.querySelector('#shortcut-list .shortcut-more').click();
    if(document.querySelector('#shortcut-menu').hidden)return false;
    document.querySelector('#shortcut-menu-edit').click();
    document.querySelector('#shortcut-name').value='Edited WebKit';validateShortcut();document.querySelector('#shortcut-form').requestSubmit();
    return p.customShortcuts.length===1;
  })())JS")).toBool());
  WK_CHECK(h.waitUntil([&] { return commands.size()==2; },3000));
  WK_CHECK(shortcuts::load().first().toObject().value(QStringLiteral("name"))==QLatin1String("Edited WebKit"));
  WK_CHECK(h.js(view, QStringLiteral("document.querySelector('#shortcut-list .shortcut-more').click();document.querySelector('#shortcut-menu-remove').click();true")).toBool());
  WK_CHECK(h.waitUntil([&] { return commands.size() == 3; }, 3000));
  WK_CHECK(shortcuts::load().isEmpty());
  WK_CHECK(h.js(view, QStringLiteral("!document.querySelector('#shortcut-list a')&&!!document.querySelector('#add-shortcut')")).toBool());
  QObject::disconnect(connection);
  h.destroyView(view);
  std::cout << "[PASS] WebKit custom shortcut creation, validation, persistence and removal." << std::endl;
}

void caseNewTab(WebKitGuiHarness &h) {
  std::cout << "[CASE 2] Shared New Tab: keyboard, UI, appearance, reload, suggestions..." << std::endl;
  auto *view = newView(h, "new tab");
  WK_CHECK(h.loadAndWait(view, QUrl(QStringLiteral("dalinira://newtab/")), 5000));
  WK_CHECK(view->url().host() == QLatin1String("newtab"));

  WK_CHECK(h.js(view, QStringLiteral("document.getElementById('query').focus();document.activeElement.id;")).toString() ==
           QStringLiteral("query"));
  // Keyboard input reaches WebKitGTK only through real X11 focus on the
  // embedded GTK surface; a Qt key event on the host widget must never be
  // re-injected into the page as a synthetic GDK event.
  QKeyEvent keyA(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, QStringLiteral("a"));
  QKeyEvent keyRelA(QEvent::KeyRelease, Qt::Key_A, Qt::NoModifier);
  QCoreApplication::sendEvent(view, &keyA);
  QCoreApplication::sendEvent(view, &keyRelA);
  h.pump(50);
  WK_CHECK(h.js(view, QStringLiteral("document.getElementById('query').value")).toString().isEmpty());
  WK_CHECK(view->internalWinId() != 0 && "WebKit view must be a Qt-owned native host window");
  std::cout << "[PASS] No synthetic key re-injection; the view is a native X11 host." << std::endl;

  WK_CHECK(h.js(view, QStringLiteral("!!document.querySelector('#query') && !!document.querySelector('.brand img') && "
                                     "document.documentElement.dataset.customizationReady === 'true'"))
               .toBool());
  WK_CHECK(h.js(view, QStringLiteral("document.querySelector('[data-background=\"solid-teal\"]').click();"
                                     "JSON.parse(localStorage.getItem('dalinira.newtab')).backgroundSource;"))
               .toString() == QStringLiteral("solid-teal"));
  std::cout << "[PASS] Shared New Tab UI rendered; appearance is writable." << std::endl;

  // Internal pages come from the registered dalinira:// scheme, so a reload
  // re-renders the New Tab instead of WebKit's "URL can't be shown" page.
  bool reloaded = false;
  const auto connection = QObject::connect(view, &EngineView::loadFinished, [&](bool ok) { reloaded = ok; });
  view->reload();
  WK_CHECK(h.waitUntil([&] { return reloaded; }, 5000));
  QObject::disconnect(connection);
  WK_CHECK(h.js(view, QStringLiteral("!!document.querySelector('#query') && location.href")).toString() ==
           QStringLiteral("dalinira://newtab/"));
  std::cout << "[PASS] New Tab survives reload via the dalinira:// scheme handler." << std::endl;

  // The page owns a valid token, decide-policy forwards dalinira://suggest, and
  // the shared parser + SearchSuggestionService turn it into a UTF-8 request.
  QList<QUrl> commands;
  const auto suggest = QObject::connect(view, &EngineView::internalNavigationRequested, [&](const QUrl &url) {
    if (url.host() == QLatin1String("suggest")) commands.append(url);
  });
  WK_CHECK(h.js(view, QStringLiteral("suggestionCapability.length>0&&suggestionCapability===window.navigationCapability")).toBool());
  view->runJavaScript(QStringLiteral(
      "window.daliniraSuggestionsEnabled=true;const q=document.getElementById('query');q.focus();"
      "q.value='çiğköfte';requestSuggestions();"));
  WK_CHECK(h.waitUntil([&] { return !commands.isEmpty(); }, 3000));
  const auto command = NewTabSuggestCommand::parse(commands.last());
  WK_CHECK(command.op == NewTabSuggestCommand::Op::Query && command.id > 0);
  WK_CHECK(command.text == QStringLiteral("çiğköfte"));
  Network network;
  network.body = R"(["çiğköfte",["çiğköfte tarifi"]])";
  SearchSuggestionService service(nullptr, &network);
  service.setEnabled(true);
  QObject owner;
  QStringList results;
  service.request(&owner, command.text, QStringLiteral("Google"), false, [&](const QStringList &list) { results = list; });
  WK_CHECK(h.waitUntil([&] { return !results.isEmpty(); }, 2000));
  WK_CHECK(network.requests == 1);
  WK_CHECK(network.last.url().toString(QUrl::FullyEncoded).endsWith(QStringLiteral("&q=%C3%A7i%C4%9Fk%C3%B6fte")));
  WK_CHECK(results == QStringList{QStringLiteral("çiğköfte tarifi")});
  commands.clear();
  view->runJavaScript(QStringLiteral(
      "const t=document.getElementById('suggestions-toggle');t.checked=false;t.dispatchEvent(new Event('change'));"));
  WK_CHECK(h.waitUntil([&] { return !commands.isEmpty(); }, 3000));
  const auto consent = NewTabSuggestCommand::parse(commands.last());
  WK_CHECK(consent.op == NewTabSuggestCommand::Op::Consent && !consent.enabled);
  QObject::disconnect(suggest);
  std::cout << "[PASS] WebKit New Tab suggestions and consent reach the shared service." << std::endl;
  h.destroyView(view);
}

// Case 3: New Tab appearance persists in WebKit's website-data profile, across
// views and web processes (case 2's view and its process are gone).
void caseNewTabPersistence(WebKitGuiHarness &h) {
  std::cout << "[CASE 3] New Tab appearance persists into a new view..." << std::endl;
  auto *view = newView(h, "new tab (persistence)");
  WK_CHECK(h.loadAndWait(view, QUrl(QStringLiteral("dalinira://newtab/")), 5000));
  WK_CHECK(h.js(view, QStringLiteral("JSON.parse(localStorage.getItem('dalinira.newtab')).backgroundSource")).toString() ==
           QStringLiteral("solid-teal"));
  // Switch to another background for the later cases.
  WK_CHECK(h.js(view, QStringLiteral("document.querySelector('[data-background=\"solid-indigo\"]').click();"
                                     "JSON.parse(localStorage.getItem('dalinira.newtab')).backgroundSource"))
               .toString() == QStringLiteral("solid-indigo"));
  std::cout << "[PASS] Appearance persisted independently of the view that set it." << std::endl;
  h.destroyView(view);
}

// ---------------------------------------------------------------------------
// Case 4: native visibility lifecycle and a page with media. A hidden tab
// unmaps its X11 surface and remaps when selected; media elements (including
// the legacy Audio() factory) never kill the web process, whether or not the
// GStreamer runtime is complete (without it, media is disabled instead).
// ---------------------------------------------------------------------------
void caseVisibilityAndMediaPage(WebKitGuiHarness &h) {
  std::cout << "[CASE 4] Visibility lifecycle and media page stability..." << std::endl;
  auto *profile = webKitBackend()->webKitProfile();
  const bool mediaAvailable = dalinira::engine::WebKitEngineProfile::mediaPlaybackAvailable();
  WK_CHECK(static_cast<bool>(webkit_settings_get_enable_media(profile->settings())) == mediaAvailable);
  WK_CHECK(static_cast<bool>(webkit_settings_get_enable_webaudio(profile->settings())) == mediaAvailable);
  std::cout << "  GStreamer media runtime usable: " << (mediaAvailable ? "yes" : "no (media disabled)") << std::endl;

  LocalSite site([](const QByteArray &path) -> std::pair<QByteArray, QByteArray> {
    if (path.startsWith("/clip.webm")) return {"video/webm", QByteArray(4096, '\0')};  // need not decode
    return {"text/html",
            "<html><head><title>media page</title></head><body>"
            "<p id=content>content</p><video id=v autoplay muted loop src=/missing.webm></video>"
            "<audio src=/missing.ogg></audio><script>try{document.getElementById('v').play().catch(()=>{})}catch(_){}"
            // Script-created media, including the legacy Audio() factory that
            // bypasses WebKit's enable-media setting (what GitHub hit).
            "setTimeout(()=>{const v=document.createElement('video');v.muted=true;v.src='/x.webm';"
            "document.body.appendChild(v);try{new Audio('/clip.webm').load()}catch(_){}},300);</script>"
            "</body></html>"};
  });

  auto *view = newView(h, "visibility + media page");
  const auto waitMapped = [&](bool mapped) {
    return h.waitUntil([&] { return view->nativeSurfaceMapped() == mapped; }, 3000);
  };
  WK_CHECK(waitMapped(true) && "selected WebKit surface must be mapped");
  WK_CHECK(h.loadAndWait(view, QUrl(site.base() + QStringLiteral("/"))));
  h.pump(1500);  // media element setup happens after load
  WK_CHECK(h.js(view, QStringLiteral("document.getElementById('content').textContent==='content'")).toBool());

  h.hide(view);
  WK_CHECK(waitMapped(false) && "hidden WebKit surface must be unmapped");
  WK_CHECK(h.stack()->currentWidget() == h.otherTab() && h.otherTab()->isVisible());
  h.select(view);
  WK_CHECK(waitMapped(true) && "re-selected WebKit surface must remap");
  WK_CHECK(h.js(view, QStringLiteral("document.getElementById('content').textContent==='content'")).toBool());

  h.shell().resize(1000, 700);
  h.pump(200);
  h.shell().resize(800, 600);
  h.pump(200);
  WK_CHECK(view->nativeSurfaceMapped());
  WK_CHECK(h.js(view, QStringLiteral("document.getElementById('content').textContent==='content'")).toBool());
  std::cout << "[PASS] Surface unmaps/remaps with selection; the media page survives load, tab switch and resize." << std::endl;
  h.destroyView(view);
}

// ---------------------------------------------------------------------------
// Case 5: cosmetic filtering. Document-start scripts run in DaliNira's
// isolated world, hide matching elements, report real hits once per element,
// and can be withdrawn live.
// ---------------------------------------------------------------------------
void caseCosmeticFiltering(WebKitGuiHarness &h) {
  std::cout << "[CASE 5] Isolated-world cosmetic filtering..." << std::endl;
  LocalSite site([](const QByteArray &) -> std::pair<QByteArray, QByteArray> {
    return {"text/html",
            "<html><head><title>ads</title></head><body>"
            "<div class=\"ad-slot\">a</div><div class=\"ad-slot\">b</div><p id=\"content\">ok</p></body></html>"};
  });
  auto *view = newView(h, "cosmetic filtering");
  bool filteringEnabled = true;
  QList<QUrl> providerUrls;
  view->setDocumentScriptProvider([&](const QUrl &url) -> QList<dalinira::engine::DocumentScript> {
    providerUrls.append(url);
    if (!filteringEnabled || url.scheme() != QLatin1String("http")) return {};
    dalinira::engine::DocumentScript cosmetic;
    cosmetic.source = QString::fromUtf8(kDaliNiraCosmeticRuntime).arg(QStringLiteral("\".ad-slot{display:none!important}\""));
    cosmetic.reapply = true;
    return {cosmetic};
  });
  quint64 hits = 0;
  QObject::connect(view, &EngineView::contentBlockerHit, [&](quint64 count, const QString &kind) {
    WK_CHECK(kind == QLatin1String("cosmetic"));
    hits += count;
  });
  WK_CHECK(h.loadAndWait(view, QUrl(site.base() + QStringLiteral("/"))));
  h.waitUntil([&] { return hits >= 2; }, 3000);
  WK_CHECK(!providerUrls.isEmpty() && providerUrls.first().host() == QLatin1String("127.0.0.1"));
  WK_CHECK(hits == 2 && "each hidden element is reported exactly once");
  const auto display = [&](const char *selector) {
    return h.js(view, QStringLiteral("getComputedStyle(document.querySelector('%1')).display").arg(QLatin1String(selector))).toString();
  };
  WK_CHECK(display(".ad-slot") == QStringLiteral("none"));
  WK_CHECK(display("#content") == QStringLiteral("block"));
  // Page scripts (main world) can neither see the runtime nor post fake hits.
  WK_CHECK(h.js(view, QStringLiteral("typeof window.__daliniraCosmeticRuntime")).toString() == QStringLiteral("undefined"));
  WK_CHECK(h.js(view, QStringLiteral("String(!!(window.webkit&&window.webkit.messageHandlers&&window.webkit.messageHandlers.daliniraBlocker))"))
               .toString() == QStringLiteral("false"));
  // A site toggle (provider now returns nothing) withdraws hiding live, without new hits.
  filteringEnabled = false;
  view->refreshDocumentScripts();
  WK_CHECK(h.waitUntil([&] { return display(".ad-slot") == QStringLiteral("block"); }, 3000));
  WK_CHECK(hits == 2);
  std::cout << "[PASS] Cosmetic filtering is isolated, counted once per element, and toggleable." << std::endl;
  h.destroyView(view);
}

// ---------------------------------------------------------------------------
// Case 6: network filtering through the web process extension. Every http(s)
// request reaches the browser-side evaluator with its page id and fetch
// destination; block, redirect and URL rewriting happen in the web process;
// a preloaded-then-parsed blocked script is evaluated (counted) once; a view
// without an evaluator is unaffected. The two views run one after the other.
// ---------------------------------------------------------------------------
void caseNetworkFiltering(WebKitGuiHarness &h) {
  std::cout << "[CASE 6] Network filtering through the web process extension..." << std::endl;
  auto *backend = webKitBackend();
  WK_CHECK(backend->enableNetworkFiltering());
  WK_CHECK(backend->supportsCapability(dalinira::engine::EngineCapability::AdBlock));
  WK_CHECK(QFileInfo::exists(dalinira::engine::WebKitNetworkFilter::extensionDirectory() +
                             QStringLiteral("/libdalinira-webkit-extension.so")));

  QMutex receivedMutex;
  QStringList received;
  LocalSite site([&](const QByteArray &path) -> std::pair<QByteArray, QByteArray> {
    {
      QMutexLocker locker(&receivedMutex);
      received.append(QString::fromLatin1(path));
    }
    if (path == "/" || path.startsWith("/page"))
      return {"text/html",
              "<html><head><title>filter</title>"
              "<script src=/ads/ad.js></script><script src=/stub/lib.js></script><script src=/ok.js></script>"
              "</head><body><img src='/px.gif?utm_source=news&keep=1'>"
              "<script>fetch('/api/data').then(r=>r.text()).then(t=>window.api=t)</script></body></html>"};
    if (path.endsWith(".js")) return {"text/javascript", path == "/ok.js" ? "window.ok=1;" : "window.adLoaded=1;"};
    if (path.startsWith("/api")) return {"text/plain", "api-ok"};
    return {"image/gif", QByteArray::fromBase64("R0lGODlhAQABAAAAACw=")};
  });
  const QString base = site.base();

  struct Seen {
    quint64 pageId;
    QString destination;
    QString url;
  };
  QMutex seenMutex;
  QList<Seen> seen;
  using Filter = dalinira::engine::WebKitNetworkFilter;
  auto evaluator = [&](const Filter::Request &request) -> Filter::Verdict {
    {
      QMutexLocker locker(&seenMutex);
      seen.append(Seen{request.pageId, request.destination, request.url.toString()});
    }
    const QString path = request.url.path();
    if (path.startsWith(QLatin1String("/ads/"))) return {Filter::Action::Block, {}};
    if (path.startsWith(QLatin1String("/stub/")))
      return {Filter::Action::Redirect, QUrl(QStringLiteral("data:text/javascript,window.stubbed=1"))};
    if (request.url.query().contains(QLatin1String("utm_source"))) {
      QUrl clean = request.url;
      clean.setQuery(QStringLiteral("keep=1"));
      return {Filter::Action::Redirect, clean};
    }
    return {};
  };

  auto *filtered = newView(h, "network filtering (filtered)");
  filtered->setNetworkRequestEvaluator(evaluator);
  WK_CHECK(filtered->pageId() != 0);
  const quint64 filteredPage = filtered->pageId();
  WK_CHECK(h.loadAndWait(filtered, QUrl(base + QStringLiteral("/page"))));
  h.waitUntil([&] { return h.js(filtered, QStringLiteral("window.api||''")).toString() == QLatin1String("api-ok"); }, 3000);

  QStringList serverPaths;
  {
    QMutexLocker locker(&receivedMutex);
    serverPaths = received;
  }
  std::cout << "  server saw: " << serverPaths.join(QStringLiteral(" ")).toStdString() << std::endl;
  WK_CHECK(!serverPaths.contains(QStringLiteral("/ads/ad.js")) && "blocked request must never leave the browser");
  WK_CHECK(!serverPaths.contains(QStringLiteral("/stub/lib.js")) && "redirected request goes to its replacement");
  WK_CHECK(serverPaths.contains(QStringLiteral("/px.gif?keep=1")) && "tracking parameters are stripped");
  WK_CHECK(!serverPaths.join(QString()).contains(QStringLiteral("utm_source")));
  WK_CHECK(serverPaths.contains(QStringLiteral("/ok.js")) && serverPaths.contains(QStringLiteral("/api/data")));
  WK_CHECK(h.js(filtered, QStringLiteral("window.ok===1&&window.stubbed===1&&window.adLoaded===undefined")).toBool());
  WK_CHECK(h.js(filtered, QStringLiteral("window.api")).toString() == QLatin1String("api-ok"));

  QList<Seen> seenCopy;
  {
    QMutexLocker locker(&seenMutex);
    seenCopy = seen;
  }
  QStringList destinations;
  int adEvaluations = 0;
  for (const Seen &entry : seenCopy) {
    WK_CHECK(entry.pageId == filteredPage && "requests are attributed to their own page");
    if (!destinations.contains(entry.destination)) destinations.append(entry.destination);
    if (entry.url.endsWith(QLatin1String("/ads/ad.js"))) ++adEvaluations;
  }
  std::cout << "  destinations: " << destinations.join(QLatin1Char(',')).toStdString() << " ad evaluations: " << adEvaluations
            << " answered: " << backend->networkFilter()->answeredRequests() << std::endl;
  for (const char *destination : {"document", "script", "image", "empty"})
    WK_CHECK(destinations.contains(QLatin1String(destination)));
  WK_CHECK(adEvaluations == 1 && "the parser's repeat of a blocked preload is not evaluated (counted) again");
  WK_CHECK(backend->networkFilter()->connectedProcesses() >= 1);
  h.destroyView(filtered);

  // Closing a view removes its route; the filter keeps serving others.
  WK_CHECK(backend->networkFilter()->evaluate({filteredPage, QStringLiteral("GET"), QStringLiteral("script"), false,
                                               QUrl(base + QStringLiteral("/ads/x.js")), QUrl(base)})
               .action == Filter::Action::Allow);

  // A view without an evaluator (no blocker for its window) is not filtered.
  {
    QMutexLocker locker(&receivedMutex);
    received.clear();
  }
  auto *unfiltered = newView(h, "network filtering (unfiltered)");
  WK_CHECK(h.loadAndWait(unfiltered, QUrl(base + QStringLiteral("/page2"))));
  WK_CHECK(h.js(unfiltered, QStringLiteral("window.adLoaded===1")).toBool());
  {
    QMutexLocker locker(&seenMutex);
    for (const Seen &entry : std::as_const(seen)) WK_CHECK(entry.pageId != unfiltered->pageId());
  }
  std::cout << "[PASS] WebKit requests are filtered in the web process by the browser's decision." << std::endl;
  h.destroyView(unfiltered);
}

// ---------------------------------------------------------------------------
// Case 6b: the blocker's filter syntax end to end in WebKit: a real
// DaliNiraBlockerService decides for a real page through the web process
// extension ($removeparam, a packaged $redirect substitute, $badfilter,
// a $redirect-rule exception and $method).
// ---------------------------------------------------------------------------
void caseBlockerFilterSyntax(WebKitGuiHarness &h) {
  std::cout << "[CASE 6b] Blocker filter syntax through the web process extension..." << std::endl;
  QTemporaryDir profileDir;
  WK_CHECK(profileDir.isValid());
  DaliNiraBlockerService blocker(profileDir.path());
  blocker.settings()->setAutoReloadOnModeChange(false);
  const QStringList rules = {QStringLiteral("||127.0.0.1^$removeparam=ptrk"),
                             QStringLiteral("/lib/cb.js$script,redirect=chartbeat.js"),
                             QStringLiteral("/lib/bf.js$script"), QStringLiteral("/m$method=post"),
                             QStringLiteral("/api/stub$xmlhttprequest,redirect=noopjson")};
  blocker.settings()->setCustomFilters(rules);

  QMutex receivedMutex;
  QStringList received;
  LocalSite site([&](const QByteArray &path) -> std::pair<QByteArray, QByteArray> {
    {
      QMutexLocker locker(&receivedMutex);
      received.append(QString::fromLatin1(path));
    }
    // Each page uses its own script URLs, so WebKit's memory cache cannot
    // answer the second page with the first page's resources.
    if (path.startsWith("/page"))
      return {"text/html",
              "<!doctype html><html><body><img src=\"/assets/photo.gif?id=1&ptrk=abc&keep=%2F\">"
              "<script src=\"/lib/cb.js?" + path.mid(5) + "\"></script><script src=\"/lib/bf.js?" + path.mid(5) +
                  "\"></script>"
              "<script>window.fetchDone=0;"
              "fetch('/m?post',{method:'POST',body:'x'}).catch(()=>{}).finally(()=>{window.fetchDone|=1});"
              "fetch('/m?get').catch(()=>{}).finally(()=>{window.fetchDone|=2});"
              "fetch('/api/stub').then(r=>r.text()).then(t=>{window.stubJson=t},()=>{window.stubJson='error'})"
              ".finally(()=>{window.fetchDone|=4});</script></body></html>"};
    if (path.endsWith(".js")) return {"text/javascript", "window.fromServer=1;"};
    return {"image/gif", QByteArray::fromBase64("R0lGODlhAQABAAAAACw=")};
  });
  auto seen = [&] {
    QMutexLocker locker(&receivedMutex);
    return received;
  };

  auto *view = newView(h, "blocker filter syntax");
  const std::shared_ptr<DaliNiraBlockerNetworkGate> gate = blocker.networkGate();
  const quint64 tabId = reinterpret_cast<quintptr>(view);
  // The evaluator BrowserWindow installs for every WebKit tab.
  view->setNetworkRequestEvaluator([gate, tabId](const dalinira::engine::WebKitNetworkFilter::Request &request) {
    using Filter = dalinira::engine::WebKitNetworkFilter;
    const NetworkRequestVerdict verdict = gate->decide(
        request.url, resourceTypeFromFetchDestination(request.destination), request.firstPartyUrl, tabId,
        request.method.toLower());
    switch (verdict.kind) {
      case NetworkVerdictKind::Block: return Filter::Verdict{Filter::Action::Block, {}};
      case NetworkVerdictKind::Redirect: return Filter::Verdict{Filter::Action::Redirect, verdict.redirectUrl};
      case NetworkVerdictKind::Allow: break;
    }
    return Filter::Verdict{};
  });
  auto load = [&](const char *page) {
    {
      QMutexLocker locker(&receivedMutex);
      received.clear();
    }
    WK_CHECK(h.loadAndWait(view, QUrl(site.base() + QString::fromLatin1(page))));
    WK_CHECK(h.waitUntil([&] { return h.js(view, QStringLiteral("window.fetchDone")).toInt() == 7; }, 5000));
  };

  load("/page1");
  QStringList paths = seen();
  std::cout << "  server saw: " << paths.join(QStringLiteral(" ")).toStdString() << std::endl;
  WK_CHECK(paths.contains(QStringLiteral("/assets/photo.gif?id=1&keep=%2F")) && "the parameter is removed, the rest kept as encoded");
  WK_CHECK(!paths.join(QString()).contains(QStringLiteral("ptrk")));
  WK_CHECK(!paths.join(QString()).contains(QStringLiteral("/lib/cb.js")));
  WK_CHECK(h.js(view, QStringLiteral("typeof window.pSUPERFLY")).toString() == QLatin1String("object") &&
           "the packaged chartbeat.js substitute ran");
  WK_CHECK(!paths.join(QString()).contains(QStringLiteral("/lib/bf.js")));
  WK_CHECK(paths.contains(QStringLiteral("/m?get")) && !paths.contains(QStringLiteral("/m?post")));
  std::cout << "  fetch() substitute: " << h.js(view, QStringLiteral("window.stubJson")).toString().toStdString() << std::endl;
  WK_CHECK(!paths.contains(QStringLiteral("/api/stub")));
  WK_CHECK(h.js(view, QStringLiteral("window.stubJson")).toString() == QLatin1String("{}") &&
           "fetch() receives the packaged noop.json");

  blocker.settings()->setCustomFilters(rules + QStringList{QStringLiteral("/lib/bf.js$script,badfilter"),
                                                           QStringLiteral("@@/lib/cb.js$redirect-rule")});
  load("/page2");
  paths = seen();
  WK_CHECK(paths.contains(QStringLiteral("/lib/bf.js?2")) && "the $badfilter lifted the block");
  WK_CHECK(!paths.join(QString()).contains(QStringLiteral("/lib/cb.js")));
  WK_CHECK(h.js(view, QStringLiteral("typeof window.pSUPERFLY")).toString() == QLatin1String("undefined") &&
           "the redirect exception keeps the block without the substitute");

  // Debouncing ($urlskip): a top-level navigation to a redirector lands on
  // its destination, another site (127.0.0.2); the redirector is never asked.
  QMutex destinationMutex;
  QStringList destinationSeen;
  LocalSite destination([&](const QByteArray &path) -> std::pair<QByteArray, QByteArray> {
    QMutexLocker locker(&destinationMutex);
    destinationSeen.append(QString::fromLatin1(path));
    return {"text/html", "<!doctype html><html><head><title>landing</title></head><body>landed</body></html>"};
  }, QHostAddress(QStringLiteral("127.0.0.2")));
  blocker.settings()->setCustomFilters(rules + QStringList{QStringLiteral("/bounce?$urlskip=?to")});
  const QString landing = destination.base() + QStringLiteral("/landing");
  {
    QMutexLocker locker(&receivedMutex);
    received.clear();
  }
  WK_CHECK(h.loadAndWait(view, QUrl(site.base() + QStringLiteral("/bounce?to=") +
                                    QString::fromLatin1(QUrl::toPercentEncoding(landing)))));
  WK_CHECK(h.waitUntil([&] { return h.js(view, QStringLiteral("location.href")).toString() == landing; }, 5000));
  WK_CHECK(!seen().join(QString()).contains(QStringLiteral("/bounce")) && "the redirector is never contacted");
  {
    QMutexLocker locker(&destinationMutex);
    WK_CHECK(destinationSeen.contains(QStringLiteral("/landing")));
  }
  h.destroyView(view);
  std::cout << "[PASS] WebKit applies $removeparam, packaged $redirect, $badfilter, redirect exceptions, $method "
               "and $urlskip debouncing." << std::endl;
}

// ---------------------------------------------------------------------------
// Case 7: background throttling, sleeping (discard) and waking, and the New
// Tab loading its images by URL instead of inlined base64.
// ---------------------------------------------------------------------------
void caseSleepAndWake(WebKitGuiHarness &h) {
  std::cout << "[CASE 7] Background throttling, sleep and wake..." << std::endl;
  QByteArray redIcon;
  {
    QImage red(16, 16, QImage::Format_ARGB32);
    red.fill(QColor(255, 0, 0));
    QBuffer buffer(&redIcon);
    buffer.open(QIODevice::WriteOnly);
    red.save(&buffer, "PNG");
  }
  LocalSite site([&](const QByteArray &path) -> std::pair<QByteArray, QByteArray> {
    if (path == "/fav.png") return {"image/png", redIcon};
    if (path == "/first") return {"text/html", "<html><head><title>First page</title></head><body>first</body></html>"};
    return {"text/html",
            "<html><head><title>Long page</title><link rel=icon href=/fav.png></head><body>"
            "<form><input id=field name=field></form><div style='height:6000px'>long</div>"
            "<script>window.ticks=0;window.frames2=0;setInterval(()=>ticks++,100);"
            "(function f(){frames2++;requestAnimationFrame(f)})();</script></body></html>"};
  });
  const QString base = site.base();

  auto *view = newView(h, "sleep and wake");
  QIcon lastIcon;
  QObject::connect(view, &EngineView::iconChanged, [&](const QIcon &icon) {
    if (!icon.isNull()) lastIcon = icon;
  });
  WK_CHECK(h.loadAndWait(view, QUrl(base + QStringLiteral("/first"))));
  WK_CHECK(h.loadAndWait(view, QUrl(base + QStringLiteral("/long"))));
  h.waitUntil([&] { return !lastIcon.isNull(); }, 5000);
  h.js(view, QStringLiteral("document.getElementById('field').value='kept text';window.scrollTo(0,1500);0"));
  h.pump(300);
  WK_CHECK(h.js(view, QStringLiteral("document.visibilityState")).toString() == QLatin1String("visible"));

  // Background: WebKit itself treats the page as hidden and stops rendering.
  h.hide(view);
  h.pump(500);
  const double frames0 = h.js(view, QStringLiteral("frames2")).toDouble();
  h.pump(1500);
  const double frames = h.js(view, QStringLiteral("frames2")).toDouble() - frames0;
  std::cout << "  hidden: visibility=" << h.js(view, QStringLiteral("document.visibilityState")).toString().toStdString()
            << " rAF/1.5s=" << frames << std::endl;
  WK_CHECK(h.js(view, QStringLiteral("document.visibilityState")).toString() == QLatin1String("hidden"));
  WK_CHECK(frames < 3 && "a background tab does not render");

  // Sleep: the web process goes away; URL, title and favicon stay.
  const QUrl urlBefore = view->url();
  const QString titleBefore = view->title();
  const QList<int> awakePids = WebKitGuiHarness::webProcessPids();
  WK_CHECK(view->setLifecycleState(dalinira::TabLifecycleState::Discarded));
  WK_CHECK(view->lifecycleState() == dalinira::TabLifecycleState::Discarded && view->rawWebView() == nullptr);
  WK_CHECK(h.waitUntil([&] { return !WebKitGuiHarness::anyAlive(awakePids); }, 8000) && "sleeping releases the web process");
  WK_CHECK(view->url() == urlBefore && view->title() == titleBefore && !view->icon().isNull());

  // Wake: history, scroll position and form state come back.
  int loads = 0;
  QObject::connect(view, &EngineView::loadFinished, [&](bool) { ++loads; });
  h.select(view);
  WK_CHECK(view->setLifecycleState(dalinira::TabLifecycleState::Active));
  WK_CHECK(h.waitUntil([&] { return loads > 0; }, 10000));
  h.pump(500);
  const double scrollY = h.js(view, QStringLiteral("window.scrollY")).toDouble();
  const QString field = h.js(view, QStringLiteral("document.getElementById('field').value")).toString();
  std::cout << "  woke: url=" << view->url().path().toStdString() << " scrollY=" << scrollY << " field='"
            << field.toStdString() << "' canGoBack=" << view->canGoBack() << std::endl;
  WK_CHECK(view->lifecycleState() == dalinira::TabLifecycleState::Active && view->rawWebView());
  WK_CHECK(view->url().path() == QLatin1String("/long") && view->title() == titleBefore);
  WK_CHECK(view->canGoBack() && "session history survives sleep");
  WK_CHECK(scrollY > 1000 && "scroll position is restored");
  WK_CHECK(field == QLatin1String("kept text") && "form state is restored");
  WK_CHECK(h.js(view, QStringLiteral("document.visibilityState")).toString() == QLatin1String("visible"));

  // New Tab: images are dalinira://newtab sub-resources, not base64 text.
  WK_CHECK(h.loadAndWait(view, QUrl(QStringLiteral("dalinira://newtab/")), 5000));
  const double htmlSize = h.js(view, QStringLiteral("document.documentElement.outerHTML.length")).toDouble();
  const auto imagesReady = [&] {
    return h.js(view, QStringLiteral(
        "[...document.images].some(i=>i.src.startsWith('dalinira://newtab/')&&i.complete&&i.naturalWidth>0)")).toBool();
  };
  const bool imagesLoaded = h.waitUntil(imagesReady, 3000);
  const QString background = h.js(view, QStringLiteral("getComputedStyle(document.body).backgroundImage")).toString();
  std::cout << "  New Tab document: " << static_cast<qint64>(htmlSize) << " characters, images loaded=" << imagesLoaded
            << ", background=" << background.left(60).toStdString() << std::endl;
  WK_CHECK(htmlSize < 500000 && "no inlined base64 images");
  WK_CHECK(imagesLoaded || background.contains(QLatin1String("gradient")) || background.contains(QLatin1String("none")));
  std::cout << "[PASS] Hidden tabs are throttled; sleep frees the web process and waking restores the page." << std::endl;
  h.destroyView(view);
}

// Removed on 2026-10-08: a manual case that showed three bare WebKit views as
// separate top-level windows at once. It hung every time: the view in an
// inactive top-level window answers the GTK plug's XEMBED_REQUEST_FOCUS with
// a focus grab that cannot take effect, GTK requests focus again, and the
// exchange never ends (WebKitEngineView::nativeEvent -> gtk_widget_grab_focus;
// the test process spun and grew, and the desktop stopped mapping windows
// until a hard reset, twice). DaliNira never shows a WebKit view as its own
// top-level window. Real multi-window use is covered by browser-webkit-lifecycle.
// Do not bring this case back without fixing that focus exchange first.

void caseOriginScopedSiteDataClear(WebKitGuiHarness &h) {
  std::cout << "[CASE] Origin-scoped ephemeral bounce data deletion..." << std::endl;
  LocalSite site([](const QByteArray &) -> std::pair<QByteArray, QByteArray> {
    return {"text/html", "<!doctype html><title>temporary storage</title>"};
  });
  const QUrl page(site.base() + QStringLiteral("/bounce"));
  auto *view = newView(h, "temporary bounce storage seed");
  WK_CHECK(h.loadAndWait(view, page));
  h.js(view, QStringLiteral(
      "localStorage.setItem('bounce','1');document.cookie='bounce=1; path=/';"
      "window.seeded=false;Promise.all([caches.open('bounce-cache').then(c=>c.put('/marker',new Response('x'))),"
      "new Promise(resolve=>{const r=indexedDB.open('bounce-db',1);r.onsuccess=r.onerror=()=>resolve()})])"
      ".then(()=>window.seeded=true);0"));
  WK_CHECK(h.waitUntil([&] { return h.js(view, QStringLiteral("window.seeded===true")).toBool(); }, 3000));
  WK_CHECK(h.js(view, QStringLiteral("localStorage.getItem('bounce')")).toString() == QLatin1String("1"));
  WK_CHECK(h.js(view, QStringLiteral("document.cookie")).toString().contains(QLatin1String("bounce=1")));
  h.destroyView(view);

  WK_CHECK(webKitBackend()->clearSiteData(page.host()));
  WK_CHECK(h.waitUntil([&] { return webKitBackend()->webKitProfile()->cookiesForUrl(page).isEmpty(); }, 5000));

  view = newView(h, "temporary bounce storage verify");
  WK_CHECK(h.loadAndWait(view, page));
  WK_CHECK(h.js(view, QStringLiteral("localStorage.getItem('bounce')===null")).toBool());
  WK_CHECK(!h.js(view, QStringLiteral("document.cookie")).toString().contains(QLatin1String("bounce=1")));
  h.js(view, QStringLiteral(
      "window.remainingData=-1;Promise.all([caches.keys(),indexedDB.databases()])"
      ".then(([c,d])=>window.remainingData=c.filter(x=>x==='bounce-cache').length+d.filter(x=>x.name==='bounce-db').length);0"));
  WK_CHECK(h.waitUntil([&] { return h.js(view, QStringLiteral("window.remainingData")).toInt() == 0; }, 3000));
  h.destroyView(view);
  std::cout << "[PASS] WebKit removed only the bounce host's cookies, local storage, CacheStorage and IndexedDB." << std::endl;
}

}  // namespace

int main(int argc, char *argv[]) {
  WebKitGuiHarness::disableCoreDumps();
  qputenv("QT_QPA_PLATFORM", "xcb");
  qputenv("GDK_BACKEND", "x11");
  QApplication app(argc, argv);
  QTemporaryDir shortcutSettings;
  if (app.arguments().contains(QStringLiteral("--newtab-shortcuts"))) {
    WK_CHECK(shortcutSettings.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, shortcutSettings.path());
    app.setOrganizationName(QStringLiteral("DaliNiraShortcutTest"));
    app.setApplicationName(QStringLiteral("WebKitShortcuts"));
    EngineBackend::setSelectedEngineType(EngineType::WebKit);
  }
  std::cout << "=== RUNNING WEBKIT BACKEND GUI TEST SUITE ===" << std::endl;

  LocalSite exampleSite([](const QByteArray &) -> std::pair<QByteArray, QByteArray> {
    return {"text/html",
            "<!doctype html><html><head><title>Example Domain</title></head>"
            "<body><h1>Example Domain</h1><p>Local DaliNira test page.</p></body></html>"};
  });
  const QUrl examplePage(exampleSite.base() + QStringLiteral("/"));

  std::cout << "[SETUP] WebKitBackend and capabilities..." << std::endl;
  QTemporaryDir tempDir;
  WK_CHECK(tempDir.isValid());
  dalinira::engine::EngineStorageManager storage(tempDir.path());
  storage.ensureDirectories(EngineType::WebKit);
  auto backend = std::make_unique<WebKitBackend>(nullptr);
  WK_CHECK(backend->engineType() == EngineType::WebKit);
  WK_CHECK(backend->layoutEngine() == "WebCore" && backend->jsEngine() == "JavaScriptCore");
  WK_CHECK(backend->initialize(storage));
  using dalinira::engine::EngineCapability;
  for (EngineCapability capability : {EngineCapability::Navigation, EngineCapability::JavaScript, EngineCapability::History,
                                      EngineCapability::Popups, EngineCapability::PrivateProfile, EngineCapability::Favicons,
                                      EngineCapability::AdultContentProtection, EngineCapability::Downloads,
                                      EngineCapability::Zoom, EngineCapability::FindInPage, EngineCapability::Print})
    WK_CHECK(backend->supportsCapability(capability));
  WK_CHECK(!backend->supportsCapability(EngineCapability::AudioEffects));
  // Network blocking needs the web process extension, enabled in case 6.
  WK_CHECK(!backend->supportsCapability(EngineCapability::AdBlock));
  WK_CHECK(!backend->supportsCapability(EngineCapability::Permissions));
  WK_CHECK(!backend->supportsCapability(EngineCapability::DevTools));
  EngineBackend::registerBackend(EngineType::WebKit, std::move(backend));
  WK_CHECK(EngineBackend::backendForType(EngineType::WebKit) != nullptr);
  std::cout << "[PASS] WebKitBackend initialized; capabilities advertised accurately." << std::endl;

  WebKitGuiHarness h(app);
  if (app.arguments().contains(QStringLiteral("--newtab-shortcuts"))) {
    caseCustomShortcuts(h);
    return 0;
  }
  if (app.arguments().contains(QStringLiteral("--site-data-only"))) {
    caseOriginScopedSiteDataClear(h);
    h.shell().close();
    EngineBackend::shutdown();
    h.waitForAllWebKitProcessesToExit();
    return 0;
  }

  std::cout << "[CASE 0] Ephemeral (private) profile view..." << std::endl;
  auto *ephemeral = h.attach(qobject_cast<WebKitEngineView *>(webKitBackend()->createEphemeralView(h.stack())), "ephemeral");
  WK_CHECK(webKitBackend()->ephemeralProfile()->isEphemeral());
  h.destroyView(ephemeral);
  std::cout << "[PASS] Ephemeral WebKit profile view." << std::endl;

  // Web views, downloads and pending favicon lookups can keep a profile's
  // context and favicon database alive after the profile is destroyed; no
  // signal handler may still point at the destroyed profile.
  std::cout << "[CASE 0b] Profile destruction disconnects its WebKit signal handlers..." << std::endl;
  {
    QTemporaryDir profileRoot;
    WK_CHECK(profileRoot.isValid());
    auto *profile = new dalinira::engine::WebKitEngineProfile(
        true, profileRoot.filePath(QStringLiteral("data")), profileRoot.filePath(QStringLiteral("cache")),
        profileRoot.filePath(QStringLiteral("cookies")));
    WebKitWebContext *context = WEBKIT_WEB_CONTEXT(g_object_ref(profile->webContext()));
    WebKitFaviconDatabase *favicons = webkit_web_context_get_favicon_database(context);
    WK_CHECK(favicons);
    g_object_ref(favicons);
    const auto handlersFor = [](gpointer instance, gpointer data) {
      return g_signal_handler_find(instance, G_SIGNAL_MATCH_DATA, 0, 0, nullptr, nullptr, data);
    };
    WK_CHECK(handlersFor(favicons, profile) != 0 && handlersFor(context, profile) != 0);
    gpointer const destroyed = profile;
    delete profile;
    WK_CHECK(handlersFor(favicons, destroyed) == 0 && "favicon-changed no longer reaches the profile");
    WK_CHECK(handlersFor(context, destroyed) == 0 && "context signals no longer reach the profile");
    // A late favicon notification is now harmless.
    g_signal_emit_by_name(favicons, "favicon-changed", "https://example.com/", "https://example.com/favicon.ico");
    g_object_unref(favicons);
    g_object_unref(context);
  }
  std::cout << "[PASS] Destroyed WebKit profiles leave no live signal handlers." << std::endl;

  caseBasicPage(h, examplePage);
  casePopupFiltering(h);
  caseElementPickerRuntime(h);
  caseTranslationLanguageProbe(h);
  caseNewTab(h);
  caseNewTabPersistence(h);
  caseVisibilityAndMediaPage(h);
  caseCosmeticFiltering(h);
  caseNetworkFiltering(h);
  caseBlockerFilterSyntax(h);
  caseSleepAndWake(h);

  h.shell().close();
  EngineBackend::shutdown();
  h.waitForAllWebKitProcessesToExit();
  std::cout << "  [harness] " << h.viewsCreated() << " views, one at a time; all WebKit processes exited" << std::endl;
  std::cout << "=== ALL WEBKIT BACKEND GUI TESTS PASSED ===" << std::endl;
  return 0;
}
