#ifdef NDEBUG
#undef NDEBUG
#endif

#include <QApplication>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QTimer>
#include <QTemporaryDir>
#include <QSettings>
#include <QLineEdit>
#include <QCompleter>
#include <QAbstractItemView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QSharedPointer>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QGuiApplication>
#include <QScreen>
#include <QKeyEvent>

#include <cassert>
#include <cmath>
#include <iostream>

#include "browser_window.h"
#include "downloads/download_toolbar_ui.h"
#include "dalinira_blocker_service.h"
#include "new_tab_scheme.h"
#include "search_suggestion_service.h"
#include "suggestion_test_transport.h"

static void wait(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

struct JavaScriptCallState
{
    QVariant result;
    bool completed = false;
};

static QVariant js(
    QWebEnginePage *page,
    const QString &code,
    int timeoutMs = 5000)
{
    auto state =
        QSharedPointer<JavaScriptCallState>::create();
    QEventLoop loop;
    const QPointer<QEventLoop> guardedLoop(&loop);

    page->runJavaScript(
        code,
        [state, guardedLoop](const QVariant &value) {
            state->result = value;
            state->completed = true;

            if (guardedLoop)
                guardedLoop->quit();
        });

    if (!state->completed) {
        QTimer::singleShot(
            qMax(0, timeoutMs),
            &loop,
            &QEventLoop::quit);
        loop.exec();
    }

    return state->result;
}

static bool waitForJs(
    QWebEnginePage *page,
    const QString &condition,
    int timeoutMs = 5000)
{
    QElapsedTimer timer;
    timer.start();

    do {
        const int remainingMs =
            timeoutMs - static_cast<int>(timer.elapsed());

        if (remainingMs <= 0)
            break;

        // Bound each renderer round trip so a delayed callback cannot consume
        // the entire polling budget. js() keeps its callback state alive if
        // this probe times out and Qt WebEngine invokes it later.
        constexpr int probeTimeoutMs = 1000;

        if (js(
                page,
                condition,
                qMin(probeTimeoutMs, remainingMs))
                .toBool())
            return true;

        const int waitMs =
            qMin(
                50,
                timeoutMs - static_cast<int>(timer.elapsed()));

        if (waitMs > 0)
            wait(waitMs);
    } while (timer.elapsed() < timeoutMs);

    return false;
}

static void key(QWidget *widget, int code)
{
    QKeyEvent press(
        QEvent::KeyPress,
        code,
        Qt::NoModifier);

    QKeyEvent release(
        QEvent::KeyRelease,
        code,
        Qt::NoModifier);

    QApplication::sendEvent(widget, &press);
    QApplication::sendEvent(widget, &release);
}

int main(int argc, char **argv)
{
    registerDaliNiraUrlSchemes();

    QApplication app(argc, argv);
    std::cerr << "Qt platform: " << QGuiApplication::platformName().toStdString() << std::endl;
    app.setApplicationName("DaliNiraPhase22dIntegration");
    app.setOrganizationName("DaliNiraTest");

    QTemporaryDir root;

    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(
        QSettings::IniFormat,
        QSettings::UserScope,
        root.path());

    Network network;

    BrowserProfileService profile(
        root.path() + "/normal",
        nullptr,
        nullptr,
        false,
        &network);

    BrowserServices services;
    services.profile = profile.profile();
    services.profileService = &profile;

    BrowserWindow window(services, true);
    // A fresh profile must not be populated with product/personal bookmarks.
    assert(profile.bookmarks().isEmpty());
    window.resize(1000, 700);
    window.show();
    window.addNewTab();

    wait(1000);

    // The Step 4 native-Wayland blocker smoke deliberately bypasses this
    // unrelated X11-oriented popup-positioning case; the default integration
    // test still executes it unchanged.
    if (qEnvironmentVariableIntValue("DALINIRA_BLOCKER_WAYLAND_SMOKE") != 1) {
      // Unified native download bubble stays anchored and reuses one popup widget.
      auto *downloadButton =
          window.findChild<DownloadToolbarButton *>("mediaDownloadButton");

    auto *downloadPopup =
        window.findChild<DownloadPopup *>();

    assert(downloadButton && downloadPopup);

    downloadButton->setModelState(1, 0.0, false, false);
    assert(downloadButton->displayedProgress() == 0.0);

    downloadButton->setModelState(1, 50.0, false, false);
    assert(downloadButton->displayedProgress() == 50.0);

    downloadButton->setModelState(1, 100.0, false, false);
    assert(downloadButton->displayedProgress() == 100.0);

    downloadButton->setModelState(0, 0.0, false, false);

    downloadPopup->showAnchored(downloadButton, false);
    wait(30);

    assert(downloadPopup->isVisible());

    QPoint downloadEdge =
        downloadButton->mapToGlobal(
            QPoint(
                downloadButton->width(),
                downloadButton->height()));

    QRect available =
        QGuiApplication::primaryScreen()->availableGeometry();

    assert(
        available
            .adjusted(-1, -1, 1, 1)
            .contains(downloadPopup->geometry().topLeft()));

    assert(
        available
            .adjusted(-1, -1, 1, 1)
            .contains(downloadPopup->geometry().bottomRight()));

    if (available.contains(downloadEdge) &&
        downloadEdge.x() - downloadPopup->width() >= available.left()) {
        assert(
            std::abs(
                downloadPopup->geometry().right() -
                downloadEdge.x()) <= 2);
    }

    window.resize(920, 640);
    window.move(window.pos() + QPoint(7, 5));

    wait(30);

    downloadEdge =
        downloadButton->mapToGlobal(
            QPoint(
                downloadButton->width(),
                downloadButton->height()));

    assert(
        available
            .adjusted(-1, -1, 1, 1)
            .contains(downloadPopup->geometry().topLeft()));

    assert(
        available
            .adjusted(-1, -1, 1, 1)
            .contains(downloadPopup->geometry().bottomRight()));

    if (available.contains(downloadEdge) &&
        downloadEdge.x() - downloadPopup->width() >= available.left()) {
        assert(
            std::abs(
                downloadPopup->geometry().right() -
                downloadEdge.x()) <= 2);
    }

    for (int i = 0; i < 12; ++i) {
        downloadPopup->hide();
        downloadPopup->showAnchored(downloadButton, false);
    }

    assert(
        window.findChildren<DownloadPopup *>().size() == 1);

      downloadPopup->hide();
    }

    auto *page = window.currentView()->page();

    assert(
        js(
            page,
            "typeof window.daliniraSuggestionBridge")
            .toString() == "function");

    assert(
        !page
             ->property("dalinira-suggest-capability")
             .toString()
             .isEmpty());

    profile.toggleBookmark(
        QUrl("https://fixture.example/"));

    auto input = [&](const QString &text) {
        window.currentView()->setFocus(Qt::MouseFocusReason);

        wait(30);

        js(
            page,
            QString(
                "document.querySelector('#query').focus();"
                "document.querySelector('#query').value=%1[0];"
                "document.querySelector('#query').dispatchEvent("
                "new Event('input',{bubbles:true}));")
                .arg(
                    QString::fromUtf8(
                        QJsonDocument(
                            QJsonArray{text})
                            .toJson(
                                QJsonDocument::Compact))));
    };

    input("fixture");
    wait(400);

    assert(network.requests == 0);

    assert(
        js(
            page,
            "document.querySelectorAll('.suggestion-row').length===0"
            "&&document.querySelector('#search-suggestions').hidden")
            .toBool());

    // Consent uses the native allowlisted bridge, not a separate localStorage key.
    js(
        page,
        "document.querySelector('#suggestions-toggle').checked=true;"
        "document.querySelector('#suggestions-toggle').dispatchEvent("
        "new Event('change'));");

    wait(100);

    assert(
        profile.searchSuggestions()->isEnabled());

    input("hav");
    wait(450);

    std::cerr
        << "bridge state: enabled="
        << profile.searchSuggestions()->isEnabled()
        << " nativeFocus="
        << window.currentView()->hasFocus()
        << " nativeId="
        << page->property("dalinira-suggest-id").toInt()
        << " js="
        << js(
               page,
               "JSON.stringify({"
               "id:suggestionId,"
               "focus:document.hasFocus(),"
               "active:document.activeElement?.id,"
               "rows:document.querySelectorAll('.suggestion-row').length"
               "})")
               .toString()
               .toStdString()
        << std::endl;

    assert(network.requests > 0);

    const QString snapshotPath =
        qEnvironmentVariable(
            "DALINIRA_PHASE22D_SNAPSHOT");

    if (!snapshotPath.isEmpty()) {
        assert(
            window.grab().save(snapshotPath));
    }

    assert(
        js(
            page,
            "Array.from(document.querySelectorAll('.suggestion-row'))"
            ".some(x=>x.textContent==='hava durumu')")
            .toBool());

    input("hava durumu");
    wait(450);

    assert(
        network.last.url().query().contains("hava durumu") ||
        network.last.url().toString().contains("hava durumu"));

    // Untrusted suggestion HTML is displayed as text, never parsed as markup.
    network.body =
        R"JSON(["markup",["<img src=x onerror=alert(1)>"]])JSON";

    input("markup");
    wait(450);

    assert(
        !js(
             page,
             "!!document.querySelector('.suggestion-row img[onerror]')")
             .toBool());

    // Search submission has its own bridge and capability. A pending/failed
    // suggestion request, an unsupported provider, or a broken suggestion
    // callback must not prevent navigation to the selected engine.
    const QList<QPair<QString, QString>> searchEngines{
        {QStringLiteral("Google"), QStringLiteral("www.google.com")},
        {QStringLiteral("DuckDuckGo"), QStringLiteral("duckduckgo.com")},
        {QStringLiteral("Startpage"), QStringLiteral("www.startpage.com")},
        {QStringLiteral("Mojeek"), QStringLiteral("www.mojeek.com")},
    };
    network.delay = 10000;
    network.body = QByteArrayLiteral("malformed suggestion response");
    for (const auto &[engine, expectedHost] : searchEngines) {
        const int searchTabIndex = window.addNewTab();
        wait(400);
        auto *searchView = window.currentView();
        assert(waitForJs(searchView->page(),
                         "!!window.navigationCapability&&!!document.querySelector('#search')",
                         10000));
        const QString setupScript = QStringLiteral(
            "(()=>{setEngine(%1[0],false);"
            "window.daliniraSuggestionBridge=()=>{throw new Error('fixture bridge failure')};"
            "const input=document.querySelector('#query');input.focus();"
            "input.value='independent search submission';"
            "input.dispatchEvent(new Event('input',{bubbles:true}));return true})()")
            .arg(QString::fromUtf8(QJsonDocument(QJsonArray{engine}).toJson(QJsonDocument::Compact)));
        assert(js(searchView->page(), setupScript).toBool());
        wait(300);
        assert(js(searchView->page(),
                  "document.querySelector('#search').requestSubmit();true").toBool());

        QElapsedTimer searchTimer;
        searchTimer.start();
        while (searchView->page()->requestedUrl().host() != expectedHost
               && searchTimer.elapsed() < 10000) {
            wait(50);
        }
        assert(searchView->page()->requestedUrl().host() == expectedHost);
        searchView->stop();
        window.closeTab(searchTabIndex);
        wait(100);
    }
    network.delay = 0;
    network.body = R"JSON(["hav",["hava","hava durumu"]])JSON";

    js(
        page,
        "location.href='dalinira://suggest?"
        "op=consent&enabled=false&cap=wrong'");

    wait(100);

    assert(
        profile.searchSuggestions()->isEnabled());

    profile.setSearchSuggestionsEnabled(false);

    const int before =
        network.requests;

    input("offline");
    wait(350);

    assert(
        network.requests == before);
    assert(js(page,
              "document.querySelectorAll('.suggestion-row').length===0"
              "&&document.querySelector('#search-suggestions').hidden").toBool());

    // The new-tab suggestion selection path must wait for each asynchronous
    // WebEngine/native-bridge step instead of assuming fixed timing.
    const int selectionIndex =
        window.addNewTab();

    wait(400);

    auto *selectionView =
        window.currentView();

    profile.setSearchSuggestionsEnabled(true);

    network.body =
        R"(["choose",["choose one","choose two"]])";

    selectionView->setFocus(Qt::MouseFocusReason);
    wait(100);

    assert(
        waitForJs(
            selectionView->page(),
            "typeof window.daliniraSuggestionBridge==='function'"
            "&&!!document.querySelector('#query')",
            10000));

    js(
        selectionView->page(),
        "document.querySelector('#query').focus();");

    assert(
        waitForJs(
            selectionView->page(),
            "document.activeElement===document.querySelector('#query')",
            5000));

    const int requestsBeforeSelection =
        network.requests;
    const int responsesBeforeSelection =
        network.responses;

    bool selectionSuggestionsReady = false;
    QElapsedTimer selectionTimer;
    selectionTimer.start();
    // Arch's containerized WebEngine renderer may apply an already completed
    // suggestion response after the normal polling window.
    const int selectionBudgetMs =
        qEnvironmentVariableIntValue("DALINIRA_SLOW_WEBENGINE_CI") ? 60000 : 30000;

    const auto selectionRemaining = [&] {
        return qMax(
            0,
            selectionBudgetMs -
                static_cast<int>(selectionTimer.elapsed()));
    };

    for (int attempt = 0;
         attempt < 3 &&
         !selectionSuggestionsReady &&
         selectionRemaining() > 0;
         ++attempt) {

        const int requestsBeforeAttempt =
            network.requests;

        selectionView->setFocus(Qt::MouseFocusReason);

        js(
            selectionView->page(),
            "(() => {"
            "const query=document.querySelector('#query');"
            "if(!query) return false;"
            "query.focus();"
            "query.value='choose';"
            "query.dispatchEvent("
            "new Event('input',{bubbles:true}));"
            "return true;"
            "})()");

        const bool selectionInputReady =
            waitForJs(
                selectionView->page(),
                "document.querySelector('#query')"
                "&&document.querySelector('#query').value==='choose'",
                qMin(2500, selectionRemaining()));

        if (!selectionInputReady) {
            continue;
        }

        while (
            network.requests == requestsBeforeAttempt &&
            selectionRemaining() > 0) {
            wait(qMin(25, selectionRemaining()));
        }

        if (network.requests == requestsBeforeAttempt)
            continue;

        const int selectionRequest =
            network.requests;

        while (
            network.lastResponseRequest != selectionRequest &&
            selectionRemaining() > 0) {
            wait(qMin(25, selectionRemaining()));
        }

        if (network.lastResponseRequest != selectionRequest)
            continue;

        // The fake reply has completed and the native completion has queued
        // daliniraShowSuggestions. Let that renderer task run before DOM polling;
        // redispatching the same value here would increment suggestionId and
        // deliberately make the completed response stale.
        if (selectionRemaining() > 0)
            wait(qMin(150, selectionRemaining()));

        selectionSuggestionsReady =
            waitForJs(
                selectionView->page(),
                "(() => {"
                "const rows=Array.from("
                "document.querySelectorAll('.suggestion-row'));"
                "const text=rows.map("
                "x=>x.textContent.trim());"
                "return rows.length>1"
                "&&text.includes('choose one')"
                "&&text.includes('choose two');"
                "})()",
                selectionRemaining());

        // A completed response owns the rest of the shared deadline. Starting
        // another request would invalidate its id while rendering is delayed.
        break;
    }

    if (!selectionSuggestionsReady) {
        std::cerr
            << "selection suggestion timeout: requests="
            << network.requests
            << " responses="
            << network.responses
            << " responsesBefore="
            << responsesBeforeSelection
            << " lastResponseRequest="
            << network.lastResponseRequest
            << " lastUrl="
            << network.last.url().toString().toStdString()
            << " pageUrl="
            << selectionView->page()->url().toString().toStdString()
            << " requestedUrl="
            << selectionView->page()->requestedUrl().toString().toStdString()
            << " nativeId="
            << selectionView->page()->property("dalinira-suggest-id").toInt()
            << " js="
            << js(
                   selectionView->page(),
                   "JSON.stringify({"
                   "ready:document.readyState,"
                   "focus:document.hasFocus(),"
                   "active:document.activeElement?.id,"
                   "queryExists:!!document.querySelector('#query'),"
                   "value:document.querySelector('#query')?.value,"
                   "suggestionId:suggestionId,"
                   "rows:Array.from("
                   "document.querySelectorAll('.suggestion-row'))"
                   ".map(x=>x.textContent)"
                   "})")
                   .toString()
                   .toStdString()
            << std::endl;
    }

    assert(selectionSuggestionsReady);
    assert(
        network.requests > requestsBeforeSelection);

    js(
        selectionView->page(),
        "document.querySelector('#query').dispatchEvent("
        "new KeyboardEvent("
        "'keydown',"
        "{key:'ArrowDown',bubbles:true}"
        "));");

    js(
        selectionView->page(),
        "document.querySelector('#query').dispatchEvent("
        "new KeyboardEvent("
        "'keydown',"
        "{key:'ArrowDown',bubbles:true}"
        "));");

    assert(
        waitForJs(
            selectionView->page(),
            "document.querySelector('#query')"
            ".getAttribute('aria-activedescendant')==='suggestion-1'"
            "&&document.querySelector('#query').value==='choose one'"
            "&&document.querySelector('#query').selectionStart===6"
            "&&document.querySelector('#query').selectionEnd===10",
            5000));

    js(
        selectionView->page(),
        "document.querySelector('#query').dispatchEvent("
        "new KeyboardEvent("
        "'keydown',"
        "{key:'Enter',bubbles:true,cancelable:true}"
        "));");

    QElapsedTimer navigationTimer;
    navigationTimer.start();

    while (
        selectionView->page()->requestedUrl().host() !=
            QStringLiteral("duckduckgo.com") &&
        selectionView->page()->requestedUrl().host() !=
            QStringLiteral("www.duckduckgo.com") &&
        selectionView->page()->requestedUrl().host() !=
            QStringLiteral("www.google.com") &&
        selectionView->page()->requestedUrl().host() !=
            QStringLiteral("google.com") &&
        navigationTimer.elapsed() < 10000) {
        wait(50);
    }

    assert(
        selectionView->page()->requestedUrl().host() ==
            QStringLiteral("duckduckgo.com") ||
        selectionView->page()->requestedUrl().host() ==
            QStringLiteral("www.duckduckgo.com") ||
        selectionView->page()->requestedUrl().host() ==
            QStringLiteral("www.google.com") ||
        selectionView->page()->requestedUrl().host() ==
            QStringLiteral("google.com"));

    selectionView->stop();

    window.closeTab(selectionIndex);
    wait(100);

    // Pointer activation commits immediately while retaining input focus long
    // enough for the trusted New Tab navigation bridge to validate the source.
    profile.recordSearch(QStringLiteral("mouse choice"));
    profile.recordSearch(QStringLiteral("remove me"));
    const int pointerIndex = window.addNewTab();
    wait(400);
    auto *pointerView = window.currentView();
    assert(waitForJs(pointerView->page(),
                     "typeof window.daliniraSuggestionBridge==='function'"
                     "&&!!document.querySelector('#query')", 10000));
    assert(js(pointerView->page(),
       "(()=>{const query=document.querySelector('#query');query.focus();query.value='focus';"
       "suggestionTypedValue='focus';window.daliniraShowSuggestions(suggestionId,'focus',["
       "{text:'focus result',url:'https://duckduckgo.com/?q=focus%20result',type:'search-history'}]);"
       "query.dispatchEvent(new Event('blur'));return document.activeElement===query})()").toBool());
    wait(325);
    assert(js(pointerView->page(),
              "!document.querySelector('#search-suggestions').hidden").toBool());
    assert(js(pointerView->page(),
       "(()=>{const query=document.querySelector('#query');query.focus();query.value='stable';"
       "suggestionTypedValue='stable';window.daliniraShowSuggestions(suggestionId,'stable',["
       "{text:'stable result',url:'https://duckduckgo.com/?q=stable%20result',type:'search-history'}]);"
       "query.value='stable x';query.dispatchEvent(new Event('input',{bubbles:true}));"
       "const stayedOpen=!document.querySelector('#search-suggestions').hidden;"
       "clearTimeout(suggestionRequestTimer);return stayedOpen})()").toBool());
    // Rows offer removal only when BrowserWindow marks them removable
    // (persistent profiles); an unmarked row must not show the button.
    assert(js(pointerView->page(),
              "!document.querySelector('#search-suggestions .suggestion-remove')").toBool());
    assert(js(pointerView->page(),
       "(()=>{const query=document.querySelector('#query');query.focus();query.value='remove';"
       "suggestionTypedValue='remove';window.daliniraShowSuggestions(suggestionId,'remove',["
       "{text:'remove me',url:'https://duckduckgo.com/?q=remove%20me',type:'search-history',removable:true}]);"
       "const remove=document.querySelector('.suggestion-remove');query.blur();"
       "if(document.querySelector('#search-suggestions').hidden)return false;"
       "remove.dispatchEvent(new PointerEvent('pointerdown',{button:0,bubbles:true,cancelable:true}));return true})()").toBool());
    QElapsedTimer deletionTimer;
    deletionTimer.start();
    while (profile.recentSearches().contains(QStringLiteral("remove me"))
           && deletionTimer.elapsed() < 5000) {
        wait(25);
    }
    assert(!profile.recentSearches().contains(QStringLiteral("remove me")));
    assert(pointerView->page()->requestedUrl().host() == QStringLiteral("newtab"));

    assert(js(pointerView->page(),
       "(()=>{const query=document.querySelector('#query');query.focus();query.value='mouse';"
       "suggestionTypedValue='mouse';window.daliniraShowSuggestions(suggestionId,'mouse',["
       "{text:'mouse choice',url:'https://duckduckgo.com/?q=mouse%20choice',type:'search-history'}]);"
       "const action=document.querySelector('.suggestion-action');query.blur();"
       "if(document.querySelector('#search-suggestions').hidden)return false;"
       "action.dispatchEvent(new PointerEvent('pointerdown',{button:0,bubbles:true,cancelable:true}));return true})()").toBool());
    QElapsedTimer pointerNavigationTimer;
    pointerNavigationTimer.start();
    while (pointerView->page()->requestedUrl().host() != QStringLiteral("duckduckgo.com")
           && pointerNavigationTimer.elapsed() < 10000) {
        wait(50);
    }
    assert(pointerView->page()->requestedUrl().host() == QStringLiteral("duckduckgo.com"));
    pointerView->stop();
    window.closeTab(pointerIndex);
    wait(100);

    // Both surfaces use the same transport; the native completion model
    // retains types and local rows.
    auto *omnibox =
        window.findChild<QLineEdit *>("omnibox");

    if (!omnibox) {
        for (auto *edit :
             window.findChildren<QLineEdit *>()) {
            if (edit->completer()) {
                omnibox = edit;
                break;
            }
        }
    }

    assert(omnibox);

    profile.setSearchSuggestionsEnabled(true);

    network.body =
        R"(["hav",["hava","hava durumu"]])";

    omnibox->setFocus();
    omnibox->setText("hav");

    QMetaObject::invokeMethod(
        omnibox,
        "textEdited",
        Qt::DirectConnection,
        Q_ARG(QString, QString("hav")));

    wait(450);

    auto *completer =
        omnibox->completer();

    std::cerr
        << "omnibox focus="
        << omnibox->hasFocus()
        << " rows="
        << (completer
                ? completer->model()->rowCount()
                : -1)
        << " requests="
        << network.requests
        << " popup="
        << (completer &&
            completer->popup()->isVisible())
        << std::endl;

    assert(
        completer &&
        completer->model()->rowCount() >= 3);

    bool remote = false;

    for (int row = 0;
         row < completer->model()->rowCount();
         ++row) {
        remote |=
            completer
                ->model()
                ->index(row, 0)
                .data(Qt::UserRole + 2)
                .toString() == "remote";
    }

    assert(remote);

    const QModelIndex chosen =
        completer
            ->completionModel()
            ->index(2, 0);

    const QUrl selectedUrl =
        chosen
            .data(Qt::UserRole + 1)
            .toUrl();

    completer
        ->popup()
        ->setCurrentIndex(chosen);

    key(
        omnibox,
        Qt::Key_Return);

    wait(50);

    assert(
        page->requestedUrl() ==
        selectedUrl);

    page->triggerAction(
        QWebEnginePage::Stop);

    // A renderer changing history/reloading must not trigger the close-only policy.
    profile
        .blockerService()
        ->settings()
        ->setAutoReloadOnModeChange(false);

    SitePolicy forget;
    forget.forgetOnClose = true;

    profile
        .blockerService()
        ->settings()
        ->setSitePolicy(
            "forget.example",
            forget);

    const int forgetIndex =
        window.addNewTab();

    wait(200);

    auto *forgetView =
        window.currentView();

    const QString fixture =
        QStringLiteral(
            "<html><body>"
            "Storage fixture"
            "</body></html>");

    forgetView->setHtml(
        fixture,
        QUrl("https://forget.example/"));

    wait(300);

    assert(
        js(
            forgetView->page(),
            "localStorage.setItem('phase22d','keep');true")
            .toBool());

    js(
        forgetView->page(),
        "history.pushState(null,'','/route')");

    assert(
        js(
            forgetView->page(),
            "localStorage.getItem('phase22d')")
            .toString() == "keep");

    forgetView->setHtml(
        fixture,
        QUrl("https://forget.example/"));

    wait(300);

    assert(
        js(
            forgetView->page(),
            "localStorage.getItem('phase22d')")
            .toString() == "keep");

    window.closeTab(forgetIndex);
    wait(700);

    const int reopen =
        window.addNewTab();

    wait(150);

    window
        .currentView()
        ->setHtml(
            fixture,
            QUrl("https://forget.example/"));

    wait(300);

    assert(
        js(
            window.currentView()->page(),
            "localStorage.getItem('phase22d')===null")
            .toBool());

    window.closeTab(reopen);
    wait(250);

    // A first navigation from New Tab must install the destination's cosmetic plan.
    const int youtubeIndex =
        window.addNewTab();

    wait(150);

    auto *youtube =
        window.currentView();

    youtube->setHtml(
        QStringLiteral(
            "<html><body>"
            "<ytd-rich-item-renderer "
            "id='normal'>"
            "Normal"
            "</ytd-rich-item-renderer>"
            "<ytd-rich-item-renderer "
            "id='ad'>"
            "<ytd-ad-slot-renderer>"
            "</ytd-ad-slot-renderer>"
            "</ytd-rich-item-renderer>"
            "</body></html>"),
        QUrl("https://youtube.com/"));

    assert(
        waitForJs(
            youtube->page(),
            "document.readyState==='complete'"
            "&&!!document.getElementById('ad')"
            "&&!!document.getElementById('normal')",
            10000));

    // Cosmetic filtering is installed asynchronously by the navigation/blocker
    // pipeline. Slower Qt WebEngine builds used by Debian/Ubuntu CI may finish
    // loading the synthetic document before the cosmetic plan reaches the
    // renderer, so wait for the observable filtering result rather than relying
    // on fixed post-load timing.
    const bool youtubeCosmeticApplied =
        waitForJs(
            youtube->page(),
            "(() => {"
            "const ad=document.getElementById('ad');"
            "const normal=document.getElementById('normal');"
            "if(!ad||!normal) return false;"
            "const content=ad.querySelector('ytd-ad-slot-renderer');"
            "return (getComputedStyle(ad).display==='none'"
            "||!!content&&getComputedStyle(content).display==='none')"
            "&&getComputedStyle(normal).display!=='none';"
            "})()",
            30000);

    if (!youtubeCosmeticApplied) {
        std::cerr
            << "youtube cosmetic filter timeout: "
            << js(
                   youtube->page(),
                   "(() => {"
                   "const ad=document.getElementById('ad');"
                   "const normal=document.getElementById('normal');"
                   "return JSON.stringify({"
                   "ready:document.readyState,"
                   "url:location.href,"
                   "adExists:!!ad,"
                   "normalExists:!!normal,"
                   "adDisplay:ad?getComputedStyle(ad).display:null,"
                   "adContentDisplay:ad&&ad.querySelector('ytd-ad-slot-renderer')?"
                   "getComputedStyle(ad.querySelector('ytd-ad-slot-renderer')).display:null,"
                   "normalDisplay:normal?"
                   "getComputedStyle(normal).display:null"
                   "});"
                   "})()")
                   .toString()
                   .toStdString()
            << std::endl;
    }

    assert(youtubeCosmeticApplied);

    window.closeTab(youtubeIndex);
    wait(100);

    // Step 3 popup filtering must run on QtWebEngine's real new-window
    // request path. A matching script-created popup creates no tab; a normal
    // login-style popup and a browser-created New Tab remain functional.
    profile.blockerService()->settings()->setAutoReloadOnModeChange(false);
    profile.blockerService()->settings()->setCustomFilters({
        QStringLiteral("||ads.popup.test^$popup"),
        QStringLiteral("@@||login.ads.popup.test^$popup")});
    profile.blockerService()->reloadRules();
    auto *popupOpener = window.currentView();
    assert(popupOpener && popupOpener->page());
    popupOpener->page()->settings()->setAttribute(
        QWebEngineSettings::JavascriptCanOpenWindows, true);
    popupOpener->setHtml(
        QStringLiteral("<!doctype html><title>popup opener</title>"
                       "<a id='oauth' target='_blank' href='https://login.ads.popup.test/oauth'>login</a>"),
        QUrl(QStringLiteral("https://opener.test/")));
    assert(waitForJs(popupOpener->page(), QStringLiteral("document.readyState==='complete'"), 10000));
    const int beforeBlockedPopup = window.tabCount();
    js(popupOpener->page(), QStringLiteral("window.open('https://ads.popup.test/offer','_blank');true"));
    wait(500);
    assert(window.tabCount() == beforeBlockedPopup);

    // A child initially opened at about:blank must not bypass $popup by
    // assigning its real destination after creation.
    js(popupOpener->page(),
       QStringLiteral("{const p=window.open('about:blank','_blank');"
                      "if(p)p.location='https://ads.popup.test/deferred';}true"));
    wait(800);
    assert(window.tabCount() == beforeBlockedPopup);

    js(popupOpener->page(), QStringLiteral("document.querySelector('#oauth').click();true"));
    QElapsedTimer popupTimer;
    popupTimer.start();
    while (window.tabCount() == beforeBlockedPopup && popupTimer.elapsed() < 5000) wait(20);
    assert(window.tabCount() == beforeBlockedPopup + 1);
    window.closeTab(window.tabCount() - 1);
    const int beforeBrowserTab = window.tabCount();
    const int browserTab = window.addNewTab();
    assert(browserTab >= 0 && window.tabCount() == beforeBrowserTab + 1);
    window.closeTab(browserTab);


    // Profile objects, stores, counters and suggestion policy stay separate.
    BrowserProfileService privateProfile(
        root.path() + "/private",
        nullptr,
        nullptr,
        true);

    assert(
        privateProfile
            .profile()
            ->isOffTheRecord());

    assert(
        privateProfile
            .recentHistory()
            .isEmpty());

    assert(
        privateProfile
            .bookmarks()
            .isEmpty());

    assert(
        !privateProfile
             .searchSuggestions()
             ->isEnabled());

    privateProfile.recordHistory(
        QUrl("https://private.example/"),
        "Private",
        true);

    for (const auto &entry :
         profile.recentHistory()) {
        assert(
            entry
                .url
                .host() !=
                QStringLiteral("private.example"));
    }

    SitePolicy privatePolicy;
    privatePolicy.whitelisted = true;

    privateProfile
        .blockerService()
        ->settings()
        ->setSitePolicy(
            "private.example",
            privatePolicy);

    assert(
        !profile
             .blockerService()
             ->settings()
             ->sitePolicy("private.example")
             .whitelisted);

    BrowserServices privateServices;
    privateServices.profile =
        privateProfile.profile();
    privateServices.profileService =
        &privateProfile;

    BrowserWindow privateWindow(
        privateServices,
        true);

    assert(
        !window.transferTabTo(
            window.findTabIdByIndex(0),
            &privateWindow,
            0));

    std::cout
        << "New Tab bridge/consent, local and remote results, "
           "whitespace, keyboard, injection defense, omnibox "
           "and profile isolation passed\n";

    return 0;
}
