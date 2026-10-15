// The experimental Gecko engine in DaliNira's real BrowserWindow, off screen:
// GeckoBackend starts gecko_fake_host (the host side of the Gecko IPC, see
// gecko_adapter_test.cpp) from a test runtime, headless. No display, no GPU,
// no real Gecko; every file goes to a temporary directory.
//
// Checks: tabs bind host tabs (the host's first tab included), DaliNira's New
// Tab page, tab switching selects the host tab, navigation waits for the
// asynchronous host tab, a page-opened tab, closing a tab and the guarded
// close of a page with unsaved changes (cancelled, then confirmed), crash and
// recovery, one window, the keyboard (browser shortcuts typed into a page
// reach the window's actions exactly once and only for the visible active tab,
// every other key reaches the page, rebinding takes effect, the address bar
// takes the keyboard), the Gecko session file, and the exit shutdown: no host
// process left, the run directory removed, the profile kept.

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QLineEdit>
#include <QJsonDocument>
#include <QUrlQuery>
#include <QScreen>
#include "engine/gecko/gecko_presentation_surface.h"
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QTcpServer>
#include <QTcpSocket>

#include <csignal>
#include <functional>
#include <iostream>
#include <optional>
#include <sys/stat.h>

#include "browser_window.h"
#include "core/browser_profile_service.h"
#include "core/keyboard_shortcut_service.h"
#include "desktop_tabs/tab_manager.h"
#include "desktop_tabs/tab_performance_manager.h"
#include "engine/engine_backend.h"
#include "engine/engine_storage_manager.h"
#include "engine/gecko/gecko_backend.h"
#include "engine/gecko/gecko_engine_view.h"
#include "engine/gecko/gecko_session.h"
#include "engine/gecko/gecko_ipc_server.h"
#include "engine/gecko/gecko_shortcut_router.h"
#include "i18n/i18n.h"
#include "session/session_store.h"

#define CHECK(condition)                                                                    \
  do {                                                                                      \
    if (!(condition)) {                                                                     \
      std::cout << "[FAIL] " << __FILE__ << ":" << __LINE__ << ": " #condition << std::endl; \
      std::_Exit(1);                                                                        \
    }                                                                                       \
  } while (false)

namespace dalinira::engine {
class GeckoSessionTestAccess {
 public:
  static QJsonObject pending(GeckoSession *session, const QString &id) { return session->pending_.value(id).message; }
  static void receive(GeckoSession *session, quint64 generation, const QJsonObject &message) { session->receive(generation, message); }
  static void disconnect(GeckoSession *session) { session->ipc_->shutdown(); }
};
}

namespace {

using namespace dalinira::engine;

bool waitUntil(const std::function<bool()> &condition, int timeoutMs = 10000) {
  QElapsedTimer timer;
  timer.start();
  while (!condition()) {
    if (timer.elapsed() > timeoutMs) return false;
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QThread::msleep(5);
  }
  return true;
}

bool alive(qint64 pid) {
  QFile stat(QStringLiteral("/proc/%1/stat").arg(pid));
  if (!stat.open(QIODevice::ReadOnly)) return false;
  const QByteArray text = stat.readAll();
  return text.mid(text.lastIndexOf(')') + 2, 1) != "Z";
}

mode_t modeOf(const QString &path) {
  struct stat st {};
  return ::lstat(QFile::encodeName(path).constData(), &st) == 0 ? st.st_mode & 07777 : 0;
}

QString makeFakeRuntime(const QString &dir) {
  constexpr QFile::Permissions file = QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup | QFile::ReadOther;
  constexpr QFile::Permissions exec = file | QFile::ExeOwner | QFile::ExeGroup | QFile::ExeOther;
  const auto write = [](const QString &path, const QByteArray &content, QFile::Permissions permissions) {
    QFile out(path);
    CHECK(out.open(QIODevice::WriteOnly) && out.write(content) == content.size() && out.setPermissions(permissions));
  };
  CHECK(QDir().mkpath(dir + QStringLiteral("/modules")));
  CHECK(QFile::copy(QStringLiteral(DALINIRA_GECKO_FAKE_HOST), dir + QStringLiteral("/firefox")));
  CHECK(QFile(dir + QStringLiteral("/firefox")).setPermissions(exec));
  write(dir + QStringLiteral("/libxul.so"), {}, file);
  write(dir + QStringLiteral("/plugin-container"), {}, exec);
  write(dir + QStringLiteral("/platform.ini"), "[Build]\nMilestone=160.0a1\n", file);
  write(dir + QStringLiteral("/modules/AppConstants.sys.mjs"), "  NIGHTLY_BUILD: true,\n", file);
  return dir;
}

GeckoEngineView *geckoView(BrowserWindow *window, int index) {
  return index >= 0 && index < window->tabCount()
             ? qobject_cast<GeckoEngineView *>(window->tabInfo(index).engineView.data())
             : nullptr;
}

int indexOfHostTab(BrowserWindow *window, const QString &hostTabId) {
  for (int i = 0; i < window->tabCount(); ++i) {
    if (auto *view = geckoView(window, i); view && view->hostTabId() == hostTabId) return i;
  }
  return -1;
}

}  // namespace

int main(int argc, char *argv[]) {
  const bool guiNewTab=argc>1&&QString::fromLocal8Bit(argv[1])==QLatin1String("--newtab-x11");
  if(guiNewTab&&qEnvironmentVariable("DALINIRA_GUI_TESTS")!=QLatin1String("1"))return 77;
  if(!guiNewTab&&argc>1&&QString::fromLocal8Bit(argv[1])==QLatin1String("--real-newtab")&&qEnvironmentVariable("DALINIRA_GECKO_HOST_TESTS")!=QLatin1String("1"))return 77;
  if(!guiNewTab)qputenv("QT_QPA_PLATFORM", "offscreen");
  // As main.cpp: only the selected engine runs in the process.
  EngineBackend::setSelectedEngineType(EngineType::Gecko);
  QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
  QApplication app(argc, argv);
  QTemporaryDir root(QDir::tempPath() + QStringLiteral("/gc-XXXXXX"));
  CHECK(root.isValid());
  const QString base = QFileInfo(root.path()).canonicalFilePath();
  QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, base + QStringLiteral("/settings"));
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, base + QStringLiteral("/settings"));
  app.setOrganizationName(QStringLiteral("DaliNiraTest"));
  app.setApplicationName(QStringLiteral("GeckoBrowserWindowTest"));
  const QString dataDir = base + QStringLiteral("/data");
  CHECK(QDir().mkpath(dataDir));
  CHECK(QDir().mkpath(base + QStringLiteral("/ipc")) && ::chmod(QFile::encodeName(base + QStringLiteral("/ipc")).constData(), 0700) == 0);
  std::cout << (guiNewTab ? "=== GECKO NEW TAB IN BROWSERWINDOW (real X11 display, real Gecko) ==="
                         : "=== GECKO IN BROWSERWINDOW (off screen) ===") << std::endl;

  BrowserProfileService profileService(dataDir, nullptr, &app);
  EngineStorageManager storage(dataDir);
  GeckoBackend::Config config;
  const bool realNewTab = guiNewTab || app.arguments().contains(QStringLiteral("--real-newtab"));
  config.runtime = realNewTab ? GeckoRuntime::developmentPaths() : GeckoRuntimePaths{makeFakeRuntime(base + QStringLiteral("/gre")), QStringLiteral(DALINIRA_GECKO_HOST_APP_DIR)};
  config.ipcRoot = base + QStringLiteral("/ipc");
  config.display = guiNewTab ? GeckoHostProcess::Display::X11 : GeckoHostProcess::Display::Headless;
  config.testInput = true;  // "test_key": keys typed into the fake host's page
  auto ownedBackend = std::make_unique<GeckoBackend>(config, &app);
  GeckoBackend *gecko = ownedBackend.get();
  bool discard = false;
  int asked = 0;
  gecko->setDiscardConfirmation([&](QWidget *, const QString &) {
    ++asked;
    return discard;
  });
  const bool initialized = gecko->initialize(storage);
  if (!initialized) std::cout << gecko->lastError().toStdString() << std::endl;
  CHECK(initialized);
  EngineBackend::registerBackend(EngineType::Gecko, std::move(ownedBackend));
  GeckoSession *session = gecko->session();
  CHECK(waitUntil([&] { return session->state() == GeckoSession::State::Ready; }));
  const QString geckoData = storage.engineDir(EngineType::Gecko);
  CHECK((modeOf(geckoData) & 0022) == 0 && modeOf(geckoData + QStringLiteral("/runtime")) == 0700 &&
        modeOf(geckoData + QStringLiteral("/profile")) == 0700 && modeOf(geckoData + QStringLiteral("/host-output.log")) == 0600);
  CHECK(!gecko->createEphemeralView(nullptr) && gecko->versionString() == QStringLiteral("Gecko 160.0a1"));
  std::cout << "PASS backend started; private Gecko storage" << std::endl;

  if (realNewTab) {
    CHECK(session->supportsInternalDocuments());
    const auto result=[&](GeckoRequest *request){CHECK(waitUntil([&]{return request->isFinished();}));if(request->status()!=QLatin1String("completed"))std::cout<<QJsonDocument(request->result()).toJson().constData()<<std::endl;CHECK(request->status()==QLatin1String("completed"));return request->result();};
    QTcpServer pages;CHECK(pages.listen(QHostAddress::LocalHost,0));
    QObject::connect(&pages,&QTcpServer::newConnection,&app,[&]{while(auto *socket=pages.nextPendingConnection())QObject::connect(socket,&QTcpSocket::readyRead,socket,[socket]{const QByteArray request=socket->readAll();QByteArray body="<!doctype html><title>Native New Tab Destination</title><h1>native navigation</h1>";if(request.contains("/dirty"))body+="<script>addEventListener('beforeunload',e=>{e.preventDefault();e.returnValue='unsaved';});</script>";if(request.contains("frames=1"))body+="<iframe src=\"/frame\"></iframe><iframe src=\"http://localhost:"+QByteArray::number(socket->localPort())+"/frame\"></iframe>";socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: "+QByteArray::number(body.size())+"\r\nConnection: close\r\n\r\n"+body);socket->disconnectFromHost();});});
    const QString endpoint=QStringLiteral("http://127.0.0.1:%1/").arg(pages.serverPort());
    CHECK(profileService.saveCustomSearchEngine(QStringLiteral("NativeLocal"),endpoint+QStringLiteral("search?q=%s")));
    profileService.setSearchEngine(QStringLiteral("NativeLocal"));
    QSettings settings;settings.setValue(QStringLiteral("ui_gecko/browser/topSitesSource"),QStringLiteral("custom"));
    settings.setValue(QStringLiteral("ui_gecko/browser/customShortcuts"),QJsonDocument(QJsonArray{QJsonObject{{"name","Local speed dial"},{"url",endpoint+"site"}}}).toJson(QJsonDocument::Compact));
    BrowserServices services;services.profileService=&profileService;
    auto *window=new BrowserWindow(services);window->show();window->activateWindow();
    window->resize(1000,700);
    int nativeActions=0;QJsonObject lastAction;
    QObject::connect(session,&GeckoSession::eventReceived,window,[&](const QJsonObject &event){
      if(event.value("type")==QLatin1String("internal-action")){++nativeActions;lastAction=event;}
      if(event.value("type")==QLatin1String("error"))std::cout<<"Native rejected command "<<QJsonDocument(event).toJson(QJsonDocument::Compact).constData()<<std::endl;
    });
    QJsonObject nativeState;QString nativeBundle;
    QObject::connect(session,&GeckoSession::requestSent,window,[&](const QString &id){const auto m=GeckoSessionTestAccess::pending(session,id);if(m.value("type")==QLatin1String("load_internal_page"))nativeBundle=m.value("bundleId").toString();if(m.value("type")==QLatin1String("load_internal_page")||m.value("type")==QLatin1String("internal_state"))nativeState=m.value("state").toObject();});
    const int first=window->addNewTab(QUrl(QStringLiteral("dalinira://newtab/")));
    CHECK(waitUntil([&]{return geckoView(window,first)&&geckoView(window,first)->url()==QUrl(QStringLiteral("dalinira://newtab/"))&&session->view(geckoView(window,first)->hostTabId())&&!session->view(geckoView(window,first)->hostTabId())->snapshot().value("internalInstance").toString().isEmpty();},30000));
    auto *view=geckoView(window,first);const QString tab=view->hostTabId();
    auto operation=[&](const QString &name){return result(session->submit(QStringLiteral("test_newtab"),{{"tabId",tab},{"operation",name}})).value("data").toObject();};
    const auto facts=operation(QStringLiteral("inspect"));CHECK(facts.value("query").toBool()&&facts.value("items").toInt()==1&&facts.value("images").toInt()>2&&facts.value("content").toBool()&&!facts.value("system").toBool()&&facts.value("Services")==QLatin1String("undefined"));
    std::cout<<"PASS real shared New Tab DOM/assets and unprivileged content"<<std::endl;
    CHECK(waitUntil([&]{return !facts.value("title").toString().isEmpty()&&facts.value("title").toString()==view->title();}));
    if(!guiNewTab){const auto image=operation(QStringLiteral("render"));CHECK(image.value("width").toInt()>0&&image.value("height").toInt()>0);const QString evidence=qEnvironmentVariable("DALINIRA_GECKO_EVIDENCE_ROOT");if(!evidence.isEmpty())CHECK(QFile::copy(session->runDirectory()+QStringLiteral("/newtab/newtab-render.png"),evidence+QStringLiteral("/newtab-render.png")));std::cout<<"PASS actual headless Gecko raster "<<image.value("width").toInt()<<"x"<<image.value("height").toInt()<<std::endl;}
    const int initialTabs=window->tabCount();
    const QString initialDocument=session->view(tab)->documentId();
    for(const QString &bundle:{QStringLiteral("../profile/prefs"),QString(32,QLatin1Char('f'))}){
      auto *bad=session->view(tab)->loadNewTab(bundle,nativeState);CHECK(waitUntil([&]{return bad->isFinished();}));CHECK(bad->status()==QLatin1String("rejected")&&session->view(tab)->documentId()==initialDocument);
    }
    // Native resolver rejects malformed, oversized, injected and symlinked bundles
    // before replacing a live document. These fixture files stay in this run.
    const auto rejectBundle=[&](const QString &id){auto *r=session->view(tab)->loadNewTab(id,nativeState);CHECK(waitUntil([&]{return r->isFinished();}));CHECK(r->status()==QLatin1String("rejected")&&session->view(tab)->documentId()==initialDocument);};
    const QList<QByteArray> malformed={QByteArray("{"),QByteArray("\xff"),QByteArray("{}"),QByteArray("{\"version\":2,\"html\":\"\",\"sha256\":\"bad\"}")};
    int fixture=1;
    for(const auto &bytes:malformed){const QString id=QStringLiteral("%1").arg(fixture++,32,16,QLatin1Char('0'));QFile f(session->runDirectory()+"/newtab/"+id+".json");CHECK(f.open(QIODevice::WriteOnly)&&f.setPermissions(QFile::ReadOwner|QFile::WriteOwner));CHECK(f.write(bytes)==bytes.size());f.close();rejectBundle(id);}
    for(const QByteArray &html:{QByteArray("<script>bad</script>"),QByteArray("<img onerror='bad' src='data:image/png;base64,AA=='>"),QByteArray("<img src='file:///etc/passwd'>"),QByteArray("<iframe src='https://example.com'>"),QByteArray("<meta http-equiv='refresh' content='0;url=file:///etc/passwd'>")}){
      const QString id=QStringLiteral("%1").arg(fixture++,32,16,QLatin1Char('0'));QFile f(session->runDirectory()+"/newtab/"+id+".json");CHECK(f.open(QIODevice::WriteOnly)&&f.setPermissions(QFile::ReadOwner|QFile::WriteOwner));const QByteArray bytes=QJsonDocument(QJsonObject{{"version",1},{"html",QString::fromUtf8(html)},{"sha256",QString::fromLatin1(QCryptographicHash::hash(html,QCryptographicHash::Sha256).toHex())}}).toJson();CHECK(f.write(bytes)==bytes.size());f.close();rejectBundle(id);
    }
    const QString largeId=QString(32,QLatin1Char('d'));{QFile f(session->runDirectory()+"/newtab/"+largeId+".json");CHECK(f.open(QIODevice::WriteOnly)&&f.setPermissions(QFile::ReadOwner|QFile::WriteOwner)&&f.resize(16*1024*1024+1));}rejectBundle(largeId);
    const QString linkId=QString(32,QLatin1Char('c'));CHECK(QFile::link(session->runDirectory()+"/newtab/"+nativeBundle+".json",session->runDirectory()+"/newtab/"+linkId+".json"));rejectBundle(linkId);
    for(const auto &url:{QStringLiteral("about:dalinira-newtab"),QStringLiteral("chrome://global/content/"),QStringLiteral("resource://gre/modules/Services.sys.mjs"),QStringLiteral("file:///etc/passwd"),QStringLiteral("javascript:alert(1)"),QStringLiteral("data:text/html,bad")}){
      auto *r=session->view(tab)->load(QUrl(url));CHECK(waitUntil([&]{return r->isFinished();}));CHECK(r->status()==QLatin1String("rejected")&&session->view(tab)->documentId()==initialDocument);
    }
    auto *stale=session->submit(QStringLiteral("load_internal_page"),{{"tabId",tab},{"documentId","stale"},{"page","newtab"},{"bundleId",QString(32,QLatin1Char('e'))},{"state",nativeState}});
    CHECK(waitUntil([&]{return stale->isFinished();})&&stale->status()==QLatin1String("rejected"));
    std::cout<<"PASS malformed/traversal descriptors and stale documents retain the live tab"<<std::endl;
    if(app.arguments().contains(QStringLiteral("--security-fixture"))){
      CHECK(waitUntil([&]{return !operation(QStringLiteral("inspect")).value("security").toString().isEmpty();}));
      const auto report=QJsonDocument::fromJson(operation(QStringLiteral("inspect")).value("security").toString().toUtf8()).object();
      CHECK(report.value("evalBlocked").toBool()&&report.value("injectionBlocked").toBool()&&report.value("networkBlocked").toBool()&&report.value("fileBlocked").toBool()&&report.value("trustedImmutable").toBool()&&report.value("noPageAPIs").toBool());
      CHECK(report.value("cspDirectives").toArray().contains(QStringLiteral("connect-src")));
      std::cout<<"CSP violation directives "<<QJsonDocument(report.value("cspDirectives").toArray()).toJson(QJsonDocument::Compact).constData()<<std::endl;
      CHECK(nativeActions==0&&session->view(tab)->documentId()==initialDocument&&view->url()==QUrl(QStringLiteral("dalinira://newtab/")));
      std::cout<<"PASS production actor rejects page synthetic click/key/submit/message requests; CSP blocks injection/eval/network/file"<<std::endl;
    }
    if(guiNewTab){
      CHECK(waitUntil([&]{return gecko->surface()->isPresenting(view)&&gecko->surface()->inspect().value("viewable").toBool();}));
      // Keyboard focus. The window must be the active one for Qt to give focus.
      window->activateWindow();
      if(!waitUntil([&]{return window->isActiveWindow();},5000)){std::cout<<"the test window did not become the active window (window manager focus policy)"<<std::endl;CHECK(window->isActiveWindow());}
      const auto pageHasKeyboard=[&]{return gecko->surface()->inspect().value("inputFocusOnEmbedded").toBool();};
      const QString topLevel=QStringLiteral("0x%1").arg(window->winId(),0,16);
      auto *omnibox=window->findChild<QLineEdit *>(QStringLiteral("omnibox"));CHECK(omnibox);
      const auto qtHasKeyboard=[&]{return omnibox->hasFocus()&&gecko->surface()->inspect().value("inputFocus").toString()==topLevel;};
      // Past Qt's 100 ms focus-in delay: the page holding the keyboard must
      // not deactivate the window (M3-B5 X11 regression).
      const auto dwell=[]{QElapsedTimer t;t.start();waitUntil([&]{return t.elapsed()>=400;});};
      CHECK(window->triggerBrowserAction(QStringLiteral("navigation.focus_address")));CHECK(waitUntil(qtHasKeyboard,5000));
      view->setFocus();CHECK(waitUntil(pageHasKeyboard,5000));
      dwell();CHECK(pageHasKeyboard()&&window->isActiveWindow()&&view->hasFocus());
      CHECK(window->triggerBrowserAction(QStringLiteral("navigation.focus_address")));
      CHECK(waitUntil(qtHasKeyboard,5000));
      view->setFocus();CHECK(waitUntil(pageHasKeyboard,5000));
      dwell();CHECK(pageHasKeyboard()&&window->isActiveWindow());
      // Ctrl+L typed into the page: the address bar takes the keyboard.
      CHECK(waitUntil([&]{return !gecko->shortcutRouter()->hostShortcuts().isEmpty();}));
      result(session->submit(QStringLiteral("test_key"),{{"tabId",tab},{"key","l"},{"code","KeyL"},{"modifiers",QJsonArray{"Control"}}}));
      CHECK(waitUntil(qtHasKeyboard,5000));
      view->setFocus();CHECK(waitUntil(pageHasKeyboard,5000));
      std::cout<<"PASS the page and the address bar take the X11 keyboard in turn, also after the page held it"<<std::endl;
      const QString evidence=qEnvironmentVariable("DALINIRA_GECKO_GUI_ROOT");CHECK(!evidence.isEmpty());
      CHECK(window->screen()->grabWindow(window->winId()).save(evidence+QStringLiteral("/newtab.png")));
      const auto security=operation(QStringLiteral("security"));QFile facts(evidence+QStringLiteral("/runtime.json"));CHECK(facts.open(QIODevice::WriteOnly));facts.write(QJsonDocument(security).toJson());facts.close();
      CHECK(security.value("runtime").toObject().value("sandbox").toObject().value("effectiveContentSandboxLevel").toInt()==6);
      CHECK(security.value("runtime").toObject().value("app").toObject().value("fissionAutostart").toBool());
      CHECK(security.value("runtime").toObject().value("graphics").toObject().value("windowLayerManagerType")==QLatin1String("WebRender"));
      CHECK(security.value("graphics").toObject().value("features").toObject().value("hwCompositing").toObject().value("status")==QLatin1String("available"));
    }
    CHECK(operation(QStringLiteral("focus-search")).value("focused").toBool());
    const auto key=[&](const QString &text,const QString &code){return result(session->submit(QStringLiteral("test_key"),{{"tabId",tab},{"key",text},{"code",code},{"modifiers",QJsonArray{}}}));};
    key(QStringLiteral("a"),QStringLiteral("KeyA"));key(QStringLiteral("&"),QStringLiteral("Digit7"));key(QStringLiteral("b"),QStringLiteral("KeyB"));key(QStringLiteral("Enter"),QStringLiteral("Enter"));
    CHECK(waitUntil([&]{return view->url().toString().startsWith(endpoint+QStringLiteral("search?"));},10000));
    CHECK(QUrlQuery(view->url()).queryItemValue(QStringLiteral("q"))==QStringLiteral("a&b"));
    std::cout<<"PASS native search uses configured custom provider with encoding"<<std::endl;
    view->back();CHECK(waitUntil([&]{return session->view(tab)->snapshot().value("internalPage")==QLatin1String("newtab");}));
    CHECK(operation(QStringLiteral("focus-first-site")).value("focused").toBool());key(QStringLiteral("Enter"),QStringLiteral("Enter"));
    CHECK(waitUntil([&]{return view->url()==QUrl(endpoint+QStringLiteral("site"));}));
    CHECK(window->tabCount()==initialTabs&&view->hostTabId()==tab);
    std::cout<<"PASS speed dial navigates the source tab; native history returns New Tab"<<std::endl;
    view->back();CHECK(waitUntil([&]{return session->view(tab)->snapshot().value("internalPage")==QLatin1String("newtab");}));
    view->forward();CHECK(waitUntil([&]{return view->url()==QUrl(endpoint+QStringLiteral("site"));}));
    view->back();CHECK(waitUntil([&]{return session->view(tab)->snapshot().value("internalPage")==QLatin1String("newtab");}));
    const QString beforeStop=session->view(tab)->documentId();result(session->view(tab)->stop());
    CHECK(session->view(tab)->documentId()==beforeStop&&operation(QStringLiteral("inspect")).value("queryValue")==QLatin1String("a&b"));
    std::cout<<"PASS real forward/back/stop preserve internal document state and draft"<<std::endl;
    const QString beforeReload=session->view(tab)->documentId();view->reload();
    CHECK(waitUntil([&]{return session->view(tab)->documentId()!=beforeReload&&session->view(tab)->snapshot().value("internalPage")==QLatin1String("newtab");}));
    CHECK(operation(QStringLiteral("inspect")).value("queryValue")==QLatin1String("a&b"));
    CHECK(operation(QStringLiteral("focus-customize")).value("focused").toBool());key(QStringLiteral("Enter"),QStringLiteral("Enter"));
    CHECK(operation(QStringLiteral("focus-theme")).value("focused").toBool());key(QStringLiteral("Enter"),QStringLiteral("Enter"));
    CHECK(waitUntil([&]{QSettings s;return s.value(QStringLiteral("ui_gecko/browser/geckoNewTab/theme")).toString()==QLatin1String("flow");}));
    CHECK(operation(QStringLiteral("inspect")).value("queryValue")==QLatin1String("a&b"));
    CHECK(operation(QStringLiteral("focus-close-customize")).value("focused").toBool());key(QStringLiteral("Enter"),QStringLiteral("Enter"));
    std::cout<<"PASS revisioned appearance commit preserves the query draft; real reload creates a new document"<<std::endl;
    if(!guiNewTab){
      const auto type=[&](const QString &text){for(const QChar c:text){QString code;if(c.isLetter())code=QStringLiteral("Key")+c.toUpper();else if(c.isDigit())code=QStringLiteral("Digit")+c;key(QString(c),code);}};
      const auto count=[&]{return QJsonDocument::fromJson(QSettings().value(QStringLiteral("ui_gecko/browser/customShortcuts")).toByteArray()).array().size();};
      CHECK(operation(QStringLiteral("focus-add")).value("focused").toBool());key(QStringLiteral("Enter"),QStringLiteral("Enter"));
      CHECK(operation(QStringLiteral("focus-shortcut-name")).value("focused").toBool());type(QStringLiteral("Extra"));
      CHECK(operation(QStringLiteral("focus-shortcut-url")).value("focused").toBool());type(endpoint+QStringLiteral("extra"));key(QStringLiteral("Enter"),QStringLiteral("Enter"));
      CHECK(waitUntil([&]{return count()==2&&operation(QStringLiteral("inspect")).value("items").toInt()==2;}));
      CHECK(operation(QStringLiteral("focus-last-more")).value("focused").toBool());key(QStringLiteral("Enter"),QStringLiteral("Enter"));
      CHECK(operation(QStringLiteral("focus-shortcut-edit")).value("focused").toBool());key(QStringLiteral("Enter"),QStringLiteral("Enter"));
      CHECK(operation(QStringLiteral("focus-shortcut-name")).value("focused").toBool());result(session->submit(QStringLiteral("test_key"),{{"tabId",tab},{"key","a"},{"code","KeyA"},{"modifiers",QJsonArray{"Control"}}}));type(QStringLiteral("Edited"));QElapsedTimer editCooldown;editCooldown.start();CHECK(waitUntil([&]{return editCooldown.elapsed()>=100;}));key(QStringLiteral("Enter"),QStringLiteral("Enter"));
      CHECK(waitUntil([&]{const auto entries=QJsonDocument::fromJson(QSettings().value(QStringLiteral("ui_gecko/browser/customShortcuts")).toByteArray()).array();return entries.size()==2&&entries.last().toObject().value("name")==QLatin1String("Edited");}));
      CHECK(operation(QStringLiteral("focus-last-more")).value("focused").toBool());key(QStringLiteral("Enter"),QStringLiteral("Enter"));
      QElapsedTimer cooldown;cooldown.start();CHECK(waitUntil([&]{return cooldown.elapsed()>=100;}));
      CHECK(operation(QStringLiteral("focus-shortcut-remove")).value("focused").toBool());key(QStringLiteral("Enter"),QStringLiteral("Enter"));
      CHECK(waitUntil([&]{return count()==1&&operation(QStringLiteral("inspect")).value("items").toInt()==1;}));
      CHECK(operation(QStringLiteral("inspect")).value("queryValue")==QLatin1String("a&b"));
      std::cout<<"PASS native shortcut add/edit/remove persists only Gecko UI data and preserves search draft"<<std::endl;
    }
    for(const auto &provider:{QStringLiteral("Bing"),QStringLiteral("Brave Search"),QStringLiteral("UnknownProvider")}){
      QSettings persisted(dataDir+QStringLiteral("/browser-preferences.ini"),QSettings::IniFormat);persisted.setValue(QStringLiteral("browser/searchEngine"),provider);persisted.sync();
      CHECK(waitUntil([&]{return nativeState.value("providerId")==provider&&session->view(tab)->snapshot().value("internalRevision")==nativeState.value("revision");}));
      const int before=nativeActions;CHECK(operation(QStringLiteral("focus-search")).value("focused").toBool());key(QStringLiteral("Enter"),QStringLiteral("Enter"));
      CHECK(waitUntil([&]{return nativeActions>before;}));CHECK(view->url()==QUrl(QStringLiteral("dalinira://newtab/"))&&profileService.configuredSearchEngineId()==provider);
    }
    profileService.setSearchEngine(QStringLiteral("NativeLocal"));CHECK(waitUntil([&]{return nativeState.value("providerId")==QLatin1String("NativeLocal")&&session->view(tab)->snapshot().value("internalRevision")==nativeState.value("revision");}));
    std::cout<<"PASS unsupported Bing/Brave/unknown configuration rejects search without provider substitution"<<std::endl;
    const QString stable=session->view(tab)->documentId();
    const int second=window->addNewTab(QUrl(QStringLiteral("dalinira://newtab/")));
    CHECK(waitUntil([&]{auto *v=geckoView(window,second);return v&&session->view(v->hostTabId())&&session->view(v->hostTabId())->snapshot().value("internalPage")==QLatin1String("newtab")&&session->activeTabId()==v->hostTabId()&&v->isHostSelectionConfirmed();}));
    const QString secondTab=geckoView(window,second)->hostTabId(),secondDoc=session->view(secondTab)->documentId();
    for(int i=0;i<(guiNewTab?2:8);i++) { window->switchTab(i%2?second:first); }
    window->switchTab(first);
    CHECK(waitUntil([&]{return session->activeTabId()==tab&&view->isHostSelectionConfirmed();}));
    CHECK(session->view(tab)->documentId()==stable&&session->view(secondTab)->documentId()==secondDoc&&operation(QStringLiteral("inspect")).value("queryValue")==QLatin1String("a&b"));
    std::cout<<"PASS normal New Tab activation and rapid last-click switching retain both documents"<<std::endl;
    if(!guiNewTab){
      // The collector observes real native input, but the hidden Qt window must
      // not commit it. A later selection invalidates that exact proposal.
      window->hide();const int before=nativeActions;
      CHECK(operation(QStringLiteral("focus-search")).value("focused").toBool());key(QStringLiteral("Enter"),QStringLiteral("Enter"));
      CHECK(waitUntil([&]{return nativeActions>before;}));const auto proposal=lastAction;
      window->show();window->activateWindow();window->switchTab(second);
      CHECK(waitUntil([&]{return session->activeTabId()==secondTab&&geckoView(window,second)->isHostSelectionConfirmed();}));
      auto *staleCommit=session->view(tab)->commitNewTabAction(proposal,{{"kind","navigate"},{"url",endpoint+QStringLiteral("should-not-load")}});
      CHECK(waitUntil([&]{return staleCommit->isFinished();}));CHECK(staleCommit->status()==QLatin1String("rejected")&&session->view(tab)->documentId()==stable);
      window->switchTab(first);CHECK(waitUntil([&]{return session->activeTabId()==tab&&view->isHostSelectionConfirmed();}));
      std::cout<<"PASS hidden Qt source rejects native action and selection cancels its delayed commit"<<std::endl;
    }
    if(!guiNewTab&&!app.arguments().contains(QStringLiteral("--security-fixture"))){
      // Ctrl+T from a live New Tab, then Ctrl+W: exactly that tab closes, both
      // once its New Tab document is bound and as soon as it is the active host
      // tab (its document may still be replacing about:blank).
      QStringList closeReplies;
      auto closeWatch=QObject::connect(session,&GeckoSession::requestFinished,window,[&](const QString &,const QString &status,const QJsonObject &reply){if(reply.value("command")==QLatin1String("close_tab"))closeReplies<<status+QLatin1Char(' ')+QString::fromUtf8(QJsonDocument(reply).toJson(QJsonDocument::Compact));});
      const auto shortcut=[&](const QString &tabId,const QString &k,const QString &code){result(session->submit(QStringLiteral("test_key"),{{"tabId",tabId},{"key",k},{"code",code},{"modifiers",QJsonArray{"Control"}}}));};
      CHECK(waitUntil([&]{return !gecko->shortcutRouter()->hostShortcuts().isEmpty();}));
      for(const bool waitForDocument:{true,false,false,false}){
        const int before=window->tabCount();closeReplies.clear();
        shortcut(tab,QStringLiteral("t"),QStringLiteral("KeyT"));
        CHECK(waitUntil([&]{return window->tabCount()==before+1;}));
        auto *created=qobject_cast<GeckoEngineView *>(window->currentEngineView());CHECK(created&&created!=view);
        CHECK(waitUntil([&]{return !created->hostTabId().isEmpty()&&session->activeTabId()==created->hostTabId()&&(!waitForDocument||(session->view(created->hostTabId())->snapshot().value("internalPage")==QLatin1String("newtab")&&created->isHostSelectionConfirmed()));},30000));
        shortcut(created->hostTabId(),QStringLiteral("w"),QStringLiteral("KeyW"));
        if(!waitUntil([&]{return window->tabCount()==before;}))std::cout<<"Ctrl+W left the tab open; close_tab replies: "<<closeReplies.join(QStringLiteral("; ")).toStdString()<<std::endl;
        CHECK(window->tabCount()==before&&geckoView(window,first)==view&&session->view(tab)->documentId()==stable);
        window->switchTab(first);CHECK(waitUntil([&]{return session->activeTabId()==tab&&view->isHostSelectionConfirmed();}));
      }
      QObject::disconnect(closeWatch);
      std::cout<<"PASS Ctrl+T then Ctrl+W from a Gecko New Tab closes exactly that tab, bound or still loading"<<std::endl;
    }
    if(app.arguments().contains(QStringLiteral("--security-fixture"))){
      const auto native=operation(QStringLiteral("fixture-security"));
      std::cout<<"Native security facts "<<QJsonDocument(native).toJson(QJsonDocument::Compact).constData()<<std::endl;
      for(const auto &name:{"nativePrincipal","nativeBinding","replayRejected","crossTabRejected","staleInstanceRejected","staleActivationRejected","staleRevisionRejected","remoteRejected","framesRejected","staleWindowGlobalRejected","latePresentedRejected","reloadedBound"})CHECK(native.value(QLatin1String(name)).toBool());
      CHECK(waitUntil([&]{return session->view(tab)->documentId()==native.value("documentId").toString();}));
      std::cout<<"PASS actual native sender/principal/top-level binding, foreign and stale actor rejection"<<std::endl;
      auto *background=session->view(secondTab);
      auto *older=background->loadNewTab(nativeBundle,nativeState);auto *newer=background->load(QUrl(endpoint+QStringLiteral("race-winner")));
      result(newer);CHECK(waitUntil([&]{return older->isFinished()&&background->url()==QUrl(endpoint+QStringLiteral("race-winner"));}));
      CHECK(older->status()==QLatin1String("rejected"));
      const QString raceDocument=background->documentId();
      auto *cancelled=background->loadNewTab(nativeBundle,nativeState);
      auto cancelSent=QObject::connect(session,&GeckoSession::requestSent,window,[&](const QString &id){if(id==cancelled->id())cancelled->cancel();});
      CHECK(waitUntil([&]{return cancelled->isFinished();}));QObject::disconnect(cancelSent);
      CHECK(cancelled->status()==QLatin1String("cancelled")&&operation(QStringLiteral("fixture-idle")).value("idle").toBool());
      CHECK(background->documentId()==raceDocument&&background->url()==QUrl(endpoint+QStringLiteral("race-winner")));
      std::cout<<"PASS delayed native bundle reads cannot replace newer navigation or a cancelled document"<<std::endl;

    }
    if(guiNewTab){const QString evidence=qEnvironmentVariable("DALINIRA_GECKO_GUI_ROOT");CHECK(window->screen()->grabWindow(window->winId()).save(evidence+QStringLiteral("/two-tabs.png")));}
    auto *secondView=geckoView(window,second);secondView->load(QUrl(endpoint+QStringLiteral("dirty")));
    CHECK(waitUntil([&]{return session->view(secondTab)->hasBeforeUnload();}));
    const QString dirtyDocument=session->view(secondTab)->documentId();
    auto *blocked=session->view(secondTab)->loadNewTab(nativeBundle,nativeState);CHECK(waitUntil([&]{return blocked->isFinished();}));
    CHECK(blocked->status()==QLatin1String("rejected")&&blocked->result().value("code")==QLatin1String("beforeunload-navigation-blocked")&&session->view(secondTab)->documentId()==dirtyDocument);
    const int beforeAsk=asked;window->closeTab(second);CHECK(waitUntil([&]{return asked>beforeAsk;}));
    CHECK(session->view(secondTab)&&session->view(secondTab)->documentId()==dirtyDocument&&session->hostIsRunning());
    discard=true;window->closeTab(second);CHECK(waitUntil([&]{return session->view(secondTab)==nullptr;}));discard=false;
    std::cout<<"PASS named internal load cannot bypass native beforeunload; denied close retains document, confirmed close is exact"<<std::endl;

    CHECK(session->hostIsRunning()&&(app.arguments().contains(QStringLiteral("--security-fixture"))||session->view(tab)->documentId()==stable));
    // Deliberate content SIGKILL targets only a verified descendant of this host.
    // Recovery is explicit, with the original logical tab ID and a new document.
    if(!guiNewTab){
      const qint64 content=view->contentProcessId(),parent=session->hostPid();bool owned=false;
      for(const auto &process:session->hostProcesses())if(process.pid==content)owned=true;
      CHECK(owned&&content>0&&content!=parent);const QString lost=session->view(tab)->documentId();
      CHECK(::kill(static_cast<pid_t>(content),SIGKILL)==0);CHECK(waitUntil([&]{return view->isCrashed();}));
      bool recovered=false;auto recoveredEvent=QObject::connect(session,&GeckoSession::eventReceived,window,[&](const QJsonObject &event){if(event.value("type")==QLatin1String("recovered")&&event.value("tabId").toString()==tab)recovered=true;});
      view->reload();CHECK(waitUntil([&]{return recovered&&!view->isCrashed()&&session->view(tab)->documentId()!=lost&&session->view(tab)->snapshot().value("internalPage")==QLatin1String("newtab");},30000));
      QObject::disconnect(recoveredEvent);CHECK(view->url()==QUrl(QStringLiteral("dalinira://newtab/")));
      CHECK(session->hostPid()==parent&&view->hostTabId()==tab&&operation(QStringLiteral("inspect")).value("content").toBool());
      std::cout<<"PASS explicit internal-document content recovery retains tab ID and host parent"<<std::endl;
    }
    // M3-B4.1 window close: the window stays until the host acknowledged the
    // guarded shutdown (Stopping); the host then exits with its processes.
    QPointer<BrowserWindow> closingWindow(window);QList<qint64> closingTree{session->hostPid()};for(const auto &process:session->hostProcesses())closingTree.append(process.pid);
    window->close();CHECK(closingWindow&&closingWindow->isVisible());
    CHECK(waitUntil([&]{return !closingWindow||!closingWindow->isVisible();},10000));
    CHECK(session->state()==GeckoSession::State::Stopping||!session->hostIsRunning());
    CHECK(waitUntil([&]{return !session->hostIsRunning();},10000));for(const qint64 pid:closingTree)CHECK(waitUntil([&]{return !alive(pid);},15000));
    std::cout<<"PASS window close waits for the acknowledged guarded host shutdown; every host process exits"<<std::endl;
    if(closingWindow)delete closingWindow.data();
    if(!guiNewTab){
      CHECK(gecko->initialize(storage));CHECK(waitUntil([&]{return session->state()==GeckoSession::State::Ready;}));
      auto *restartWindow=new BrowserWindow(services);restartWindow->show();restartWindow->activateWindow();
      const int index=restartWindow->addNewTab(QUrl(QStringLiteral("dalinira://newtab/")));
      CHECK(waitUntil([&]{auto *v=geckoView(restartWindow,index);return v&&session->view(v->hostTabId())&&session->view(v->hostTabId())->snapshot().value("internalPage")==QLatin1String("newtab");},30000));
      auto *old=session->view(geckoView(restartWindow,index)->hostTabId());const quint64 generation=session->generation();const qint64 ownedParent=session->hostPid();
      CHECK(ownedParent>0&&session->hostIsRunning()&&::kill(static_cast<pid_t>(ownedParent),SIGKILL)==0);
      CHECK(waitUntil([&]{return !session->hostIsRunning()&&old->lifecycleState()==GeckoSessionView::State::Lost;}));
      CHECK(gecko->initialize(storage));CHECK(waitUntil([&]{return session->state()==GeckoSession::State::Ready&&session->view(session->activeTabId())&&!session->view(session->activeTabId())->documentId().isEmpty();}));
      CHECK(session->generation()>generation&&session->views().size()==1&&session->views().first()->snapshot().value("internalPage").isUndefined());
      auto *oldClose=old->close();CHECK(waitUntil([&]{return oldClose->isFinished();}));CHECK(oldClose->status()==QLatin1String("rejected"));
      result(session->shutdown());CHECK(waitUntil([&]{return !session->hostIsRunning();}));delete restartWindow;
      std::cout<<"PASS real native parent crash/restart loses session explicitly; stale view cannot close replacement"<<std::endl;
    }
    EngineBackend::shutdown();
    std::cout<<"PASS native New Tab guarded window shutdown"<<std::endl;return 0;
  }

  // The host's names of DaliNira's shortcuts; every default binding has one.
  using Router = GeckoShortcutRouter;
  const auto keys = [](QKeyCombination combination) { return QKeySequence(combination); };
  CHECK(Router::hostShortcut(keys(Qt::CTRL | Qt::Key_T)) == QStringLiteral("Ctrl+T"));
  CHECK(Router::hostShortcut(keys(Qt::CTRL | Qt::SHIFT | Qt::Key_Tab)) == QStringLiteral("Ctrl+Shift+Tab"));
  CHECK(Router::hostShortcut(keys(Qt::CTRL | Qt::Key_Backtab)) == QStringLiteral("Ctrl+Shift+Tab"));
  CHECK(Router::hostShortcut(keys(Qt::ALT | Qt::Key_Left)) == QStringLiteral("Alt+ArrowLeft"));
  CHECK(Router::hostShortcut(keys(Qt::CTRL | Qt::Key_PageDown)) == QStringLiteral("Ctrl+PageDown"));
  CHECK(Router::hostShortcut(keys(Qt::CTRL | Qt::Key_Plus)) == QStringLiteral("Ctrl++"));
  CHECK(Router::hostShortcut(keys(Qt::CTRL | Qt::SHIFT | Qt::Key_Equal)) == QStringLiteral("Ctrl+="));
  CHECK(Router::hostShortcut(keys(Qt::SHIFT | Qt::Key_F3)) == QStringLiteral("Shift+F3"));
  CHECK(Router::hostShortcut(keys(QKeyCombination(Qt::Key_F5))) == QStringLiteral("F5"));
  for (const QKeySequence &none : {keys(Qt::CTRL | Qt::Key_C), keys(Qt::META | Qt::Key_T), keys(Qt::CTRL | Qt::ALT | Qt::Key_Q),
                                   keys(Qt::CTRL | Qt::Key_Odiaeresis), keys(QKeyCombination(Qt::Key_T)), QKeySequence()})
    CHECK(Router::hostShortcut(none).isEmpty());
  Router *router = gecko->shortcutRouter();
  CHECK(waitUntil([&] { return !router->hostShortcuts().isEmpty(); }));
  auto &shortcutService = dalinira::KeyboardShortcutService::instance();
  for (const auto &definition : dalinira::KeyboardShortcutService::registry()) {
    for (const QKeySequence &sequence : shortcutService.shortcuts(definition.id))
      CHECK(router->hostShortcuts().contains(Router::hostShortcut(sequence)));
  }
  std::cout << "PASS every browser shortcut has a host name and is given to the host" << std::endl;

  TabManager tabManager(&app);
  tabManager.performanceManager()->setPolicyMode(dalinira::PerformancePolicyMode::Off);
  SessionStore sessionStore(dataDir, QStringLiteral("gecko"));
  BrowserServices services;
  services.profileService = &profileService;
  services.tabManager = &tabManager;
  services.sessionStore = &sessionStore;
  auto *window = new BrowserWindow(services);
  window->resize(1000, 700);
  window->show();
  CHECK(window->currentEngineScope() == QStringLiteral("gecko"));

  // The first tab shows the host's own first tab, then loads.
  window->addNewTab(QUrl(QStringLiteral("http://fake.test/a")));
  GeckoEngineView *a = geckoView(window, 0);
  CHECK(a);
  CHECK(waitUntil([&] { return !a->hostTabId().isEmpty() && a->url() == QUrl(QStringLiteral("http://fake.test/a")); }));
  CHECK(session->views().size() == 1);
  // A legacy host without internal-document-v1 retains an empty tab,
  // reports unsupported, and never claims a native internal-document binding.
  window->addNewTab(QUrl(QStringLiteral("dalinira://newtab/")));
  GeckoEngineView *b = geckoView(window, 1);
  CHECK(b && waitUntil([&] { return !b->hostTabId().isEmpty(); }));
  CHECK(waitUntil([&]{return b->status().contains(QStringLiteral("does not support"));}));
  CHECK(!session->supportsInternalDocuments() && b->hostTabId() != a->hostTabId() && b->url().isEmpty() &&
        session->views().size() == 2);
  // Selecting a tab selects its host tab.
  window->switchTab(0);
  CHECK(waitUntil([&] { return session->activeTabId() == a->hostTabId(); }));
  window->switchTab(1);
  CHECK(waitUntil([&] { return session->activeTabId() == b->hostTabId(); }));
  std::cout << "PASS tabs bind host tabs; switching selects them" << std::endl;

  // Navigation before the host tab exists waits for it, in order.
  window->addNewTab(QUrl(QStringLiteral("http://fake.test/q1")));
  GeckoEngineView *c = geckoView(window, 2);
  CHECK(c && c->hostTabId().isEmpty());
  c->load(QUrl(QStringLiteral("http://fake.test/q2")));
  c->load(QUrl(QStringLiteral("http://fake.test/q3")));
  CHECK(waitUntil([&] { return c->url() == QUrl(QStringLiteral("http://fake.test/q3")) && !c->isLoading(); }));
  CHECK(waitUntil([&] { return c->canGoBack(); }));
  CHECK(waitUntil([&] { return window->tabInfo(2).url == QUrl(QStringLiteral("http://fake.test/q3")); }));
  c->back();
  CHECK(waitUntil([&] { return c->url() == QUrl(QStringLiteral("http://fake.test/q2")); }));
  std::cout << "PASS queued navigation and history" << std::endl;

  // A page-opened tab becomes a tab of this window, bound to that host tab.
  const int beforePopup = window->tabCount();
  a->load(QUrl(QStringLiteral("http://fake.test/open-popup")));
  CHECK(waitUntil([&] { return window->tabCount() == beforePopup + 1; }));
  GeckoEngineView *popup = geckoView(window, window->tabCount() - 1);
  CHECK(popup && waitUntil([&] { return popup->url() == QUrl(QStringLiteral("http://fake.test/popup")); }));
  const QString popupHostTab = popup->hostTabId();
  CHECK(session->views().size() == 4);
  // Closing a clean tab closes its host tab.
  window->closeTab(indexOfHostTab(window, popupHostTab));
  CHECK(waitUntil([&] { return window->tabCount() == beforePopup && !session->view(popupHostTab); }));
  std::cout << "PASS page-opened tab adopted; closing a tab closes its host tab" << std::endl;

  // Unsaved changes: the user decides; cancelled keeps the tab and its page.
  c->load(QUrl(QStringLiteral("http://fake.test/dirty")));
  CHECK(waitUntil([&] { return session->view(c->hostTabId()) && session->view(c->hostTabId())->hasBeforeUnload(); }));
  const QString dirtyHostTab = c->hostTabId();
  const int tabsBefore = window->tabCount();
  window->closeTab(indexOfHostTab(window, dirtyHostTab));
  CHECK(waitUntil([&] { return asked == 1; }));
  QThread::msleep(100);
  QCoreApplication::processEvents();
  CHECK(window->tabCount() == tabsBefore && session->view(dirtyHostTab) && indexOfHostTab(window, dirtyHostTab) >= 0);
  discard = true;
  window->closeTab(indexOfHostTab(window, dirtyHostTab));
  CHECK(waitUntil([&] { return asked == 2 && window->tabCount() == tabsBefore - 1 && !session->view(dirtyHostTab); }));
  std::cout << "PASS guarded close: cancelled, then confirmed" << std::endl;

  // A content process crash: the tab says so; reload recovers the page.
  const qint64 victim = b->contentProcessId();
  CHECK(victim > 0);
  const int bIndex = indexOfHostTab(window, b->hostTabId());
  CHECK(::kill(static_cast<pid_t>(victim), SIGKILL) == 0);
  CHECK(waitUntil([&] { return b->isCrashed(); }));
  CHECK(waitUntil([&] {
    return window->tabInfo(bIndex).title == dalinira::i18n::I18n::text(QStringLiteral("tab.process_terminated"));
  }));
  b->reload();
  CHECK(waitUntil([&] { return !b->isCrashed() && b->contentProcessId() > 0 && b->contentProcessId() != victim; }));
  std::cout << "PASS crash shown; reload recovers" << std::endl;

  // One window: Gecko tabs stay in it; a new window is a new tab here.
  CHECK(!window->canMoveTabToAnotherWindow(window->tabInfo(0).id));
  const int beforeNewWindow = window->tabCount();
  CHECK(window->openNewWindow(QUrl(QStringLiteral("http://fake.test/window"))) == window);
  CHECK(window->tabCount() == beforeNewWindow + 1);
  std::cout << "PASS one window" << std::endl;

  // The keyboard. "test_key" types into the host's active tab as a keyboard
  // would; a browser shortcut runs the window's action once, any other key
  // reaches the page (whose title lists what it received).
  const auto finished = [](GeckoRequest *request) {
    CHECK(waitUntil([&] { return request->isFinished(); }));
    return request->status();
  };
  const auto type = [&](const QString &key, const QStringList &modifiers) {
    CHECK(finished(session->submit(QStringLiteral("test_key"),
                                   {{QStringLiteral("tabId"), session->activeTabId()}, {QStringLiteral("key"), key},
                                    {QStringLiteral("code"), QString()},
                                    {QStringLiteral("modifiers"), QJsonArray::fromStringList(modifiers)}})) ==
          QStringLiteral("completed"));
  };
  const auto settle = [] {
    // Room for a second (wrong) action to run.
    for (int i = 0; i < 20; ++i) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
      QThread::msleep(5);
    }
  };
  const auto current = [&] { return qobject_cast<GeckoEngineView *>(window->currentEngineView()); };
  const auto hostActive = [&](GeckoEngineView *view) {
    return waitUntil([&] { return view && current() == view && session->activeTabId() == view->hostTabId(); });
  };
  const QStringList ctrl{QStringLiteral("Control")};
  window->activateWindow();
  window->switchTab(indexOfHostTab(window, a->hostTabId()));
  CHECK(hostActive(a));

  // Ctrl+T: one new tab, in this window.
  int tabs = window->tabCount();
  type(QStringLiteral("t"), ctrl);
  CHECK(waitUntil([&] { return window->tabCount() == tabs + 1; }));
  settle();
  CHECK(window->tabCount() == tabs + 1);
  GeckoEngineView *opened = current();
  CHECK(opened && opened != a && waitUntil([&] { return !opened->hostTabId().isEmpty(); }) && hostActive(opened));
  // Ctrl+W closes it (and only it).
  type(QStringLiteral("w"), ctrl);
  CHECK(waitUntil([&] { return window->tabCount() == tabs; }));
  settle();
  CHECK(window->tabCount() == tabs && indexOfHostTab(window, a->hostTabId()) >= 0);
  std::cout << "PASS Ctrl+T and Ctrl+W from the page: one action each" << std::endl;

  // Ctrl+Tab and Ctrl+Shift+Tab go to the next and previous tab.
  window->switchTab(indexOfHostTab(window, a->hostTabId()));
  CHECK(hostActive(a));
  const int aIndex = indexOfHostTab(window, a->hostTabId());
  GeckoEngineView *next = geckoView(window, (aIndex + 1) % window->tabCount());
  type(QStringLiteral("Tab"), ctrl);
  CHECK(hostActive(next));
  type(QStringLiteral("Tab"), {QStringLiteral("Control"), QStringLiteral("Shift")});
  CHECK(hostActive(a));
  std::cout << "PASS Ctrl+Tab and Ctrl+Shift+Tab from the page" << std::endl;

  // Ctrl+R reloads the page; Alt+Left goes back.
  int reloads = 0;
  QObject::connect(session, &GeckoSession::requestFinished, &app, [&](const QString &, const QString &, const QJsonObject &reply) {
    if (reply.value(QStringLiteral("command")).toString() == QLatin1String("reload")) ++reloads;
  });
  // (The fake host's one content process crashed every tab above: recover this one.)
  if (a->isCrashed()) a->reload();
  CHECK(waitUntil([&] { return !a->isCrashed(); }));
  type(QStringLiteral("r"), ctrl);
  CHECK(waitUntil([&] { return reloads == 1; }));
  const QUrl beforeA2 = a->url();
  a->load(QUrl(QStringLiteral("http://fake.test/a2")));
  CHECK(waitUntil([&] { return a->url() == QUrl(QStringLiteral("http://fake.test/a2")) && a->canGoBack(); }));
  type(QStringLiteral("ArrowLeft"), {QStringLiteral("Alt")});
  CHECK(waitUntil([&] { return a->url() == beforeA2; }));
  settle();
  CHECK(reloads == 1);
  std::cout << "PASS Ctrl+R and Alt+Left from the page" << std::endl;

  // Other keys are the page's: text and the reserved editing keys.
  type(QStringLiteral("c"), ctrl);
  type(QStringLiteral("x"), {});
  CHECK(waitUntil([&] { return a->title() == QStringLiteral("keys:Ctrl+C,X"); }));
  CHECK(window->tabCount() == tabs);
  // Ctrl+L: the address bar takes the keyboard; the page gets it back.
  auto *omnibox = window->findChild<QLineEdit *>(QStringLiteral("omnibox"));
  CHECK(omnibox);
  a->setFocus();
  CHECK(waitUntil([&] { return a->hasFocus(); }));
  type(QStringLiteral("l"), ctrl);
  CHECK(waitUntil([&] { return omnibox->hasFocus(); }));
  a->setFocus();
  CHECK(waitUntil([&] { return a->hasFocus(); }));
  std::cout << "PASS page keys stay the page's; Ctrl+L focuses the address bar" << std::endl;

  // Reports for a tab that is not the visible active one are dropped (a key
  // pressed just before a tab switch); so are reports while the window is hidden.
  GeckoEngineView *background = next;
  CHECK(background != a && session->activeTabId() == a->hostTabId());
  int sentBackground = 0;
  QObject::connect(session, &GeckoSession::shortcutPressed, &app, [&] { ++sentBackground; });
  background->load(QUrl(QStringLiteral("http://fake.test/shortcut-from-background")));
  CHECK(waitUntil([&] { return sentBackground == 1; }));
  settle();
  CHECK(window->tabCount() == tabs);
  window->hide();
  type(QStringLiteral("t"), ctrl);
  CHECK(waitUntil([&] { return sentBackground == 2; }));
  settle();
  CHECK(window->tabCount() == tabs);
  window->show();
  window->activateWindow();
  std::cout << "PASS shortcuts of a background tab or hidden window are dropped" << std::endl;

  // Rebinding takes effect at once: the old key goes to the page, the new one works.
  int published = 0;
  QObject::connect(router, &GeckoShortcutRouter::shortcutsPublished, &app, [&] { ++published; });
  CHECK(shortcutService.setShortcut(QStringLiteral("tabs.new"), keys(Qt::CTRL | Qt::SHIFT | Qt::Key_Y)).ok());
  CHECK(waitUntil([&] { return published == 1; }));
  CHECK(!router->hostShortcuts().contains(QStringLiteral("Ctrl+T")) && router->hostShortcuts().contains(QStringLiteral("Ctrl+Shift+Y")));
  type(QStringLiteral("t"), ctrl);
  CHECK(waitUntil([&] { return a->title() == QStringLiteral("keys:Ctrl+C,X,Ctrl+T"); }));
  CHECK(window->tabCount() == tabs);
  type(QStringLiteral("Y"), {QStringLiteral("Control"), QStringLiteral("Shift")});
  CHECK(waitUntil([&] { return window->tabCount() == tabs + 1; }));
  settle();
  CHECK(window->tabCount() == tabs + 1);
  GeckoEngineView *rebound = current();
  CHECK(rebound && waitUntil([&] { return !rebound->hostTabId().isEmpty(); }) && hostActive(rebound));
  type(QStringLiteral("w"), ctrl);
  CHECK(waitUntil([&] { return window->tabCount() == tabs; }));
  shortcutService.resetShortcut(QStringLiteral("tabs.new"));
  CHECK(waitUntil([&] { return published == 2 && router->hostShortcuts().contains(QStringLiteral("Ctrl+T")); }));
  std::cout << "PASS rebinding takes effect in the page at once" << std::endl;

  // M3-B4.1: failed close attempts never remove a live host document.
  for (const QString &fault : {QStringLiteral("close-error"), QStringLiteral("close-ipc-failure"),
                              QStringLiteral("close-timeout"), QStringLiteral("close-cancel"),
                              QStringLiteral("close-ack-without-close"), QStringLiteral("close-unload-refused"),
                              QStringLiteral("close-missing-tab"), QStringLiteral("close-missing-document"),
                              QStringLiteral("close-wrong-command"), QStringLiteral("close-wrong-document")}) {
    window->addNewTab(QUrl(QStringLiteral("http://fake.test/dirty-%1").arg(fault)));
    auto *v = current();
    CHECK(v && waitUntil([&] { return !v->hostTabId().isEmpty() && session->view(v->hostTabId())->hasBeforeUnload(); }));
    const QString id = v->hostTabId();
    const QString document = session->view(id)->documentId();
    int results = 0;
    auto sent = QObject::connect(session, &GeckoSession::requestSent, &app, [&](const QString &requestId) {
      const auto msg = GeckoSessionTestAccess::pending(session, requestId);
      if (fault == QLatin1String("close-cancel") && msg.value("type") == "close_tab" && msg.value("tabId") == id)
        session->cancel(requestId);
    });
    v->requestClose([&](bool allowed) { CHECK(!allowed); ++results; });
    // A second close while unresolved is denied, without another host command.
    if (fault == QLatin1String("close-timeout")) {
      bool duplicate = true;
      v->requestClose([&](bool allowed) { duplicate = allowed; });
      CHECK(!duplicate);
    }
    CHECK(waitUntil([&] { return results == 1; }));
    QObject::disconnect(sent);
    CHECK(session->view(id) && session->view(id)->documentId() == document && indexOfHostTab(window, id) >= 0);
    // No stuck pending flag: retry requires the real beforeunload decision.
    const int previousAsked = asked;
    discard = false;
    window->closeTab(indexOfHostTab(window, id));
    CHECK(waitUntil([&] { return asked == previousAsked + 1; }));
    CHECK(indexOfHostTab(window, id) >= 0 && session->view(id));
    discard = true;
    window->closeTab(indexOfHostTab(window, id));
    CHECK(waitUntil([&] { return !session->view(id) && indexOfHostTab(window, id) < 0; }));
    std::cout << "PASS fail-closed " << fault.toStdString() << "; retry remains guarded" << std::endl;
  }

  // M3-B5 X11 Ctrl+W: the host's tab moved on to a document Qt had not yet
  // observed (the internal New Tab replacing about:blank), so the plain close
  // is refused as stale. It is sent once more only for the document the host
  // named, after Qt observed it; any other outcome keeps the tab.
  {
    discard = false;
    const auto staleSetup = [&](const QString &url) {
      window->addNewTab(QUrl(url));
      auto *v = current();
      CHECK(v && waitUntil([&] { return !v->hostTabId().isEmpty() && !session->view(v->hostTabId())->documentId().isEmpty(); }));
      return v;
    };
    const auto observe = [&](GeckoEngineView *v, const QString &documentId) {
      auto state = session->view(v->hostTabId())->snapshot();
      state.insert("documentId", documentId);
      GeckoSessionTestAccess::receive(session, session->generation(), state);
    };
    int staleReplies = 0;
    auto staleWatch = QObject::connect(session, &GeckoSession::requestFinished, &app, [&](const QString &, const QString &status, const QJsonObject &reply) {
      if (reply.value("command") == QLatin1String("close_tab") && status == QLatin1String("rejected") && reply.value("code") == QLatin1String("stale-document")) ++staleReplies;
    });

    // The race: the close names the earlier document; Qt then observes the
    // host's current one, and the retried close closes exactly that tab.
    auto *raced = staleSetup(QStringLiteral("http://fake.test/stale-retry"));
    const QString racedId = raced->hostTabId(), racedDocument = session->view(racedId)->documentId();
    observe(raced, QStringLiteral("observed-earlier"));
    std::optional<bool> racedAllowed;
    raced->requestClose([&](bool allowed) { racedAllowed = allowed; });
    CHECK(waitUntil([&] { return staleReplies == 1; }));
    CHECK(!racedAllowed && session->view(racedId));
    observe(raced, racedDocument);
    CHECK(waitUntil([&] { return racedAllowed.has_value(); }));
    CHECK(*racedAllowed && !session->view(racedId));
    window->closeTab(indexOfHostTab(window, racedId));
    CHECK(waitUntil([&] { return indexOfHostTab(window, racedId) < 0; }));

    // Qt observes yet another document: no retry.
    auto *third = staleSetup(QStringLiteral("http://fake.test/stale-third"));
    const QString thirdId = third->hostTabId(), thirdDocument = session->view(thirdId)->documentId();
    observe(third, QStringLiteral("observed-earlier"));
    std::optional<bool> thirdAllowed;
    third->requestClose([&](bool allowed) { thirdAllowed = allowed; });
    CHECK(waitUntil([&] { return staleReplies == 2; }));
    observe(third, QStringLiteral("another-document"));
    CHECK(waitUntil([&] { return thirdAllowed.has_value(); }));
    CHECK(!*thirdAllowed && session->view(thirdId) && indexOfHostTab(window, thirdId) >= 0);
    observe(third, thirdDocument);

    // Nothing observed in time: no retry.
    auto *silent = staleSetup(QStringLiteral("http://fake.test/stale-silent"));
    const QString silentId = silent->hostTabId(), silentDocument = session->view(silentId)->documentId();
    observe(silent, QStringLiteral("observed-earlier"));
    std::optional<bool> silentAllowed;
    QElapsedTimer silentTimer;
    silentTimer.start();
    silent->requestClose([&](bool allowed) { silentAllowed = allowed; });
    CHECK(waitUntil([&] { return silentAllowed.has_value(); }, 5000));
    CHECK(!*silentAllowed && silentTimer.elapsed() >= 1900 && session->view(silentId) && staleReplies == 3);
    observe(silent, silentDocument);

    // The retried close keeps the beforeunload guard: unsaved changes ask, a
    // refusal keeps the exact document, and only one stale retry is sent.
    auto *guarded = staleSetup(QStringLiteral("http://fake.test/dirty-stale-retry"));
    const QString guardedId = guarded->hostTabId();
    CHECK(waitUntil([&] { return session->view(guardedId)->hasBeforeUnload(); }));
    const QString guardedDocument = session->view(guardedId)->documentId();
    observe(guarded, QStringLiteral("observed-earlier"));
    const int askedBefore = asked;
    std::optional<bool> guardedAllowed;
    guarded->requestClose([&](bool allowed) { guardedAllowed = allowed; });
    CHECK(waitUntil([&] { return staleReplies == 4; }));
    observe(guarded, guardedDocument);
    CHECK(waitUntil([&] { return guardedAllowed.has_value(); }));
    CHECK(!*guardedAllowed && asked == askedBefore + 1 && staleReplies == 4 && session->view(guardedId) &&
          session->view(guardedId)->documentId() == guardedDocument);
    QObject::disconnect(staleWatch);
    for (const QString &id : {thirdId, silentId, guardedId}) {
      discard = true;
      window->closeTab(indexOfHostTab(window, id));
      CHECK(waitUntil([&] { return !session->view(id) && indexOfHostTab(window, id) < 0; }));
    }
    discard = false;
    std::cout << "PASS a stale plain close is retried once for the observed current document; otherwise the tab stays" << std::endl;
  }

  // Confirmation binds to the requesting document, not a later one.
  window->addNewTab(QUrl(QStringLiteral("http://fake.test/dirty-stale-confirmation")));
  auto *staleView = current();
  CHECK(staleView && waitUntil([&] { return !staleView->hostTabId().isEmpty() && session->view(staleView->hostTabId())->hasBeforeUnload(); }));
  const QString staleId = staleView->hostTabId();
  const auto original = session->view(staleId)->snapshot();
  const quint64 generation = session->generation();
  gecko->setDiscardConfirmation([&](QWidget *, const QString &) {
    auto changed = original;
    changed.insert("documentId", "replacement-document");
    GeckoSessionTestAccess::receive(session, generation, changed);
    return true;
  });
  bool staleDone = false;
  staleView->requestClose([&](bool allowed) { CHECK(!allowed); staleDone = true; });
  CHECK(waitUntil([&] { return staleDone; }));
  CHECK(session->view(staleId));
  GeckoSessionTestAccess::receive(session, generation, original);
  // Destroying a widget in a nested confirmation does not dereference it or
  // discard the host document. Its tab remains session-owned (not spare/reused).
  auto *detached = qobject_cast<GeckoEngineView *>(gecko->createView(window));
  detached->load(QUrl(QStringLiteral("http://fake.test/dirty-destroy")));
  CHECK(waitUntil([&] { return !detached->hostTabId().isEmpty() && session->view(detached->hostTabId())->hasBeforeUnload(); }));
  const QString detachedId = detached->hostTabId();
  QPointer<GeckoEngineView> detachedGuard(detached);
  bool detachedCallback = false;
  gecko->setDiscardConfirmation([&](QWidget *widget, const QString &) { delete widget; return true; });
  detached->requestClose([&](bool) { detachedCallback = true; });
  CHECK(waitUntil([&] { return detachedGuard.isNull(); }, 100));
  settle();
  CHECK(!detachedCallback && session->view(detachedId));
  gecko->setDiscardConfirmation([&](QWidget *, const QString &) { ++asked; return discard; });
  discard = true;
  window->closeTab(indexOfHostTab(window, staleId));
  CHECK(waitUntil([&] { return !session->view(staleId); }));
  std::cout << "PASS document-bound confirmation and destroyed-widget retention" << std::endl;

  // Destruction before asynchronous creation neither force-closes nor reuses
  // the resulting host document.
  QPointer<EngineView> unbound(gecko->createView(window));
  unbound->load(QUrl(QStringLiteral("http://fake.test/dirty-orphan")));
  auto disposeOnSend = QObject::connect(session, &GeckoSession::requestSent, &app, [&](const QString &id) {
    const auto msg = GeckoSessionTestAccess::pending(session, id);
    if (msg.value("type") == "create_tab" && msg.value("url") == "http://fake.test/dirty-orphan") delete unbound.data();
  });
  CHECK(waitUntil([&] { return unbound.isNull(); }));
  QObject::disconnect(disposeOnSend);
  CHECK(waitUntil([&] {
    for (auto *v : session->views()) if (v->url() == QUrl("http://fake.test/dirty-orphan")) return v->hasBeforeUnload();
    return false;
  }));
  CHECK(session->state() == GeckoSession::State::Ready);
  std::cout << "PASS asynchronous widget disposal retains the newly created document" << std::endl;

  auto *cancelledCreation = qobject_cast<GeckoEngineView *>(gecko->createView(window));
  cancelledCreation->load(QUrl("http://fake.test/dirty-late-creation"));
  auto cancelCreation = QObject::connect(session, &GeckoSession::requestSent, &app, [&](const QString &id) {
    if (GeckoSessionTestAccess::pending(session, id).value("url") == "http://fake.test/dirty-late-creation") session->cancel(id);
  });
  QString lateId;
  CHECK(waitUntil([&] {
    for (auto *v : session->views()) if (v->url() == QUrl("http://fake.test/dirty-late-creation")) { lateId = v->tabId(); return true; }
    return false;
  }));
  QObject::disconnect(cancelCreation);
  CHECK(cancelledCreation->hostTabId().isEmpty());
  delete cancelledCreation;
  auto *fresh = qobject_cast<GeckoEngineView *>(gecko->createView(window));
  CHECK(waitUntil([&] { return !fresh->hostTabId().isEmpty(); }));
  CHECK(fresh->hostTabId() != lateId && session->view(lateId)->hasBeforeUnload());
  delete fresh;
  CHECK(session->view(lateId));
  std::cout << "PASS cancelled late creation retained and not recycled into a different widget" << std::endl;

  // A stale/forged shutdown acknowledgement cannot authorize cleanup.
  GeckoSessionTestAccess::receive(session, generation,
      {{"type", "ack"}, {"command", "shutdown"}, {"requestId", "nobody-sent-this"}});
  CHECK(session->state() == GeckoSession::State::Ready);
  discard = false;
  bool shutdownAnswered = false;
  gecko->requestShutdown(window, [&](bool allowed) { CHECK(!allowed); shutdownAnswered = true; });
  CHECK(waitUntil([&] { return shutdownAnswered; }));
  CHECK(session->hostPid() > 0 && session->view(detachedId));
  window->close();
  settle();
  CHECK(window->isVisible() && session->state() == GeckoSession::State::Ready);
  QEvent quit(QEvent::Quit);
  QCoreApplication::sendEvent(&app, &quit);
  settle();
  CHECK(session->state() == GeckoSession::State::Ready && window->isVisible());
  const int asksBeforeQuit = asked;
  bool guardedLoopStayedAlive = false;
  QTimer::singleShot(0, &app, &QCoreApplication::quit);
  QTimer::singleShot(300, &app, [&] { guardedLoopStayedAlive = true; app.exit(7); });
  CHECK(app.exec() == 7 && guardedLoopStayedAlive && asked > asksBeforeQuit);
  CHECK(window->isVisible() && session->state() == GeckoSession::State::Ready);
  discard = true;
  std::cout << "PASS shutdown/window/application quit denial keeps the live session" << std::endl;

  // The Gecko workspace's session file only.
  window->saveSessionNow();
  QFile sessionFile(dataDir + QStringLiteral("/session/gecko/tabs.session.json"));
  CHECK(sessionFile.open(QIODevice::ReadOnly) && sessionFile.readAll().contains("http://fake.test/"));
  CHECK(!QFileInfo::exists(dataDir + QStringLiteral("/session/chromium")) &&
        !QFileInfo::exists(dataDir + QStringLiteral("/session/webkit")));
  std::cout << "PASS the Gecko session file only" << std::endl;

  // Exit, as main.cpp: the host first, then the window, then the backend.
  QList<qint64> tree{session->hostPid()};
  for (const auto &process : session->hostProcesses()) tree.append(process.pid);
  const QString runDirectory = session->runDirectory();
  CHECK(QFileInfo(runDirectory).isDir());
  CHECK(gecko->shutdownHost(5000));
  for (const qint64 pid : tree) CHECK(waitUntil([&] { return !alive(pid); }));
  CHECK(!QFileInfo::exists(runDirectory));
  CHECK(QDir(geckoData + QStringLiteral("/runtime")).entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty());
  CHECK(QDir(base + QStringLiteral("/ipc")).entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty());
  CHECK(QFileInfo(geckoData + QStringLiteral("/profile/user.js")).isFile());
  // No host, no shortcuts: nothing of the old set is acted on.
  CHECK(router->hostShortcuts().isEmpty());
  delete window;
  // Real transport loss and host death: deny while the live parent remains;
  // permit disposal only after it has definitively exited. A new launch cannot
  // make old widget/request identities target its tabs.
  {
    auto extraConfig = config;
    extraConfig.dataDirectory = base + QStringLiteral("/extra-gecko");
    GeckoBackend extra(extraConfig, &app);
    extra.setDiscardConfirmation([](QWidget *, const QString &) { return false; });
    CHECK(extra.initialize(storage));
    auto *extraSession = extra.session();
    CHECK(waitUntil([&] { return extraSession->state() == GeckoSession::State::Ready; }));
    auto *v = qobject_cast<GeckoEngineView *>(extra.createView(nullptr));
    v->load(QUrl("http://fake.test/dirty-close-timeout"));
    CHECK(waitUntil([&] { return !v->hostTabId().isEmpty() && extraSession->view(v->hostTabId())->hasBeforeUnload(); }));
    bool answered = false;
    auto disconnectOnClose = QObject::connect(extraSession, &GeckoSession::requestSent, &app, [&](const QString &id) {
      if (GeckoSessionTestAccess::pending(extraSession, id).value("type") == "close_tab")
        GeckoSessionTestAccess::disconnect(extraSession);
    });
    v->requestClose([&](bool allowed) { CHECK(!allowed); answered = true; });
    CHECK(waitUntil([&] { return answered; }));
    QObject::disconnect(disconnectOnClose);
    CHECK(waitUntil([&] { return !extraSession->hostIsRunning(); }));
    bool ceased = false;
    v->requestClose([&](bool allowed) { ceased = allowed; });
    CHECK(ceased && extra.shutdownHost(1000));
    const quint64 oldGeneration = extraSession->generation();
    CHECK(extra.initialize(storage));
    CHECK(waitUntil([&] { return extraSession->state() == GeckoSession::State::Ready; }));
    GeckoSessionTestAccess::receive(extraSession, oldGeneration,
        {{"type", "ack"}, {"command", "shutdown"}, {"requestId", "stale-generation"}});
    CHECK(extraSession->state() == GeckoSession::State::Ready);
    bool oldAllowed = true;
    bool oldAnswered = false;
    v->requestClose([&](bool allowed) { oldAllowed = allowed; oldAnswered = true; });
    CHECK(waitUntil([&] { return oldAnswered; }) && !oldAllowed);
    delete v;
    auto *live = qobject_cast<GeckoEngineView *>(extra.createView(nullptr));
    live->load(QUrl("http://fake.test/dirty-host-crash"));
    CHECK(waitUntil([&] { return !live->hostTabId().isEmpty() && extraSession->view(live->hostTabId())->hasBeforeUnload(); }));
    const qint64 ownedHost = extraSession->hostPid();
    CHECK(ownedHost > 0 && ::kill(static_cast<pid_t>(ownedHost), SIGKILL) == 0);
    CHECK(waitUntil([&] { return !extraSession->hostIsRunning(); }));
    bool deadAllowed = false;
    live->requestClose([&](bool allowed) { deadAllowed = allowed; });
    CHECK(deadAllowed && extra.shutdownHost(1000));
    delete live;
    std::cout << "PASS IPC failure, definitive host death, and stale session generation" << std::endl;
  }
  EngineBackend::shutdown();
  std::cout << "PASS exit: no host process, run directory removed, profile kept" << std::endl;
  std::cout << "=== ALL GECKO BROWSERWINDOW TESTS PASSED ===" << std::endl;
  return 0;
}
