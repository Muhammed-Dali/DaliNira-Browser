// User-visible parity of Chromium and WebKit in the real BrowserWindow
// (GUI tier, scripts/safe-gui-test.sh). One case per run:
//
//   --gui popup    window.open / target=_blank / rel=noopener, window.opener
//                  and the returned WindowProxy, about:blank popups written by
//                  the opener, window.close(), the pop-ups content setting.
//   --gui scripts  per-site "block scripts" and the JavaScript setting: inline
//                  scripts, attribute handlers, external scripts, frames,
//                  page- and browser-started cross-site navigations, DaliNira's
//                  isolated-world scripts; plus which worker / WebSocket
//                  requests reach the shared blocker decision (reported only).
//   --gui media    element fullscreen of a playing video: enter / exit (page
//                  and Escape), playback, focus, geometry, maximized restore,
//                  closing the fullscreen tab.
//   --gui layout   first-run onboarding in the real window (welcome, HTML
//                  bookmark import, engine, layout / theme, search & privacy,
//                  ready), resume, reopening from Settings; then every layout
//                  and theme switched at runtime under a live page (never
//                  reloaded or recreated), side-panel tabs, reorder, detach /
//                  reattach, the floating panel drawn over the page, reduced
//                  motion. Must-hold checks fail the engine itself (EXPECT).
//
// Each case runs one child process per engine, one after the other (DaliNira
// runs one engine per process). A child prints "[RESULT] name=value" lines;
// the parent fails when an engine's value differs from Chromium's, so every
// result is a parity check. Local pages only.

#include "settings/feedback_links.h"

#include <QApplication>
#include <QPointer>
#include <QCheckBox>
#include <QComboBox>
#include <QMenu>
#include <QToolButton>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMap>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QProcess>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QCursor>
#include <QScreen>
#include <QSet>
#include <QSettings>
#include <QShortcut>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslServer>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTime>
#include <QTimer>
#include <QUrlQuery>
#include <QWebEnginePage>
#include <QWebEngineScript>
#include <QWebEngineView>
#include <QWindow>
#if defined(DALINIRA_TEST_QTEST)
#include <QTest>
#endif
#include <functional>
#include <iostream>
#include <memory>
#include <unistd.h>

#include "blocker/dalinira_blocker_service.h"
#include "blocker/dalinira_blocker_settings.h"
#include "browser_window.h"
#include "core/application_identity.h"
#include "core/browser_icons.h"
#include "audio/audio_effects_page.h"
#include "audio/web_audio_effects_controller.h"
#include "downloads/media_download_service.h"
#include "eq/eq_preset_repository.h"
#include "pulse/song_finder_settings.h"
#include "pulse/song_recognition_service.h"
#include "core/browser_profile_service.h"
#include "core/web_engine_hardware_acceleration.h"
#include "core/search_engine_definition.h"
#include "desktop_tabs/tab_manager.h"
#include "desktop_tabs/tab_strip_widget.h"
#include "desktop_tabs/tab_performance_manager.h"
#include "desktop_tabs/find_bar_widget.h"
#include "engine/chromium/chromium_backend.h"
#include "engine/chromium/chromium_engine_view.h"
#include "engine/display_platform.h"
#include "engine/engine_backend.h"
#include "engine/engine_storage_manager.h"
#include "engine/engine_view.h"
#include "layout/browser_layout.h"
#include "layout/browser_layout_controller.h"
#include "layout/browser_side_panel.h"
#include "newtab/new_tab_scheme.h"
#include "onboarding/engine_step.h"
#include "onboarding/layout_step.h"
#include "onboarding/onboarding_manager.h"
#include "onboarding/onboarding_view.h"
#include "onboarding/ready_step.h"
#include "onboarding/search_privacy_step.h"
#include "settings/settings_page.h"
#include "core/keyboard_shortcut_service.h"
#include "i18n/i18n.h"
#include "i18n/language_manager.h"
#include "session/session_store.h"
#include "core/security_utils.h"
#include "downloads/general_download_manager.h"
#include "passwords/credential_vault_manager.h"
#include "blocker/dalinira_blocker_engine.h"
#include "blocker/dalinira_blocker_types.h"
#if defined(DALINIRA_BUILD_WEBKIT)
#include "engine/webkit/webkit_backend.h"
#include "engine/webkit/webkit_engine_profile.h"
#include "engine/webkit/webkit_engine_view.h"
#include <QtGui/qguiapplication_platform.h>
#include <X11/Xlib.h>
#if defined(DALINIRA_TEST_XTEST)
#include <X11/extensions/XTest.h>
#endif
#undef KeyPress
#undef KeyRelease
#undef FocusIn
#undef FocusOut
#undef None
#undef Bool
#undef Status
#undef Unsorted
#undef signals
#include <webkit2/webkit2.h>
#endif

// Runs once before a failed check exits: a WebKit phase shuts WebKit down as
// main() does, so no web process is left to lose the browser abruptly.
inline std::function<void()> g_failureShutdown;

#define CHECK(condition)                                                                    \
  do {                                                                                      \
    if (!(condition)) {                                                                     \
      std::cout << "[FAIL] " << __FILE__ << ":" << __LINE__ << ": " #condition << std::endl; \
      if (auto shutdown = std::exchange(g_failureShutdown, nullptr)) shutdown();            \
      std::_Exit(1);                                                                        \
    }                                                                                       \
  } while (0)

namespace {

using dalinira::engine::EngineType;

QString g_tag;  // "[case/engine]"

void log(const QString &text) {
  std::cout << g_tag.toStdString() << ' ' << QTime::currentTime().toString(QStringLiteral("hh:mm:ss.zzz")).toStdString()
            << ' ' << text.toStdString() << std::endl;
}

void result(const QString &name, const QString &value) {
  std::cout << "[RESULT] " << name.toStdString() << '=' << value.toStdString() << std::endl;
}
void result(const QString &name, bool value) { result(name, value ? QStringLiteral("true") : QStringLiteral("false")); }

// A documented engine difference: reported in the parity table, not failed.
void knownDifference(const QString &name, bool value, const QString &why) {
  std::cout << "[RESULT] " << name.toStdString() << '=' << (value ? "true" : "false") << std::endl;
  std::cout << "[KNOWN] " << name.toStdString() << '=' << why.toStdString() << std::endl;
}

// A must-hold check: reported like a result (so both engines are compared)
// and, when false, fails this engine's phase at its end.
bool g_expectFailed = false;
void expect(const QString &name, bool value) {
  result(name, value);
  if (!value) {
    g_expectFailed = true;
    std::cout << "[EXPECT FAILED] " << name.toStdString() << std::endl;
  }
}

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

void settle(int ms) { waitUntil([] { return false; }, ms); }

// ---------------------------------------------------------------------------
// Local site: three hosts (127.0.0.1, localhost and 127.0.0.2) on one port, beacons.
// ---------------------------------------------------------------------------

class LocalSite {
 public:
  LocalSite() {
    CHECK(v4_.listen(QHostAddress::LocalHost));
    // "localhost" may resolve to ::1 first; serve it there too when possible.
    v6_.listen(QHostAddress::LocalHostIPv6, v4_.serverPort());
    // A third site, the third party of the privacy case.
    CHECK(third_.listen(QHostAddress(QStringLiteral("127.0.0.2")), v4_.serverPort()));
    for (QTcpServer *server : {&v4_, &v6_, &third_})
      QObject::connect(server, &QTcpServer::newConnection, [this, server] { accept(server); });
    QFile clip(QStringLiteral(DALINIRA_TEST_MEDIA_DIR "/vp9_opus_4s.webm"));
    CHECK(clip.open(QIODevice::ReadOnly));
    clip_ = clip.readAll();
  }
  quint16 port() const { return v4_.serverPort(); }
  QString a() const { return QStringLiteral("http://127.0.0.1:%1").arg(port()); }
  QString b() const { return QStringLiteral("http://localhost:%1").arg(port()); }
  // The third party; over HTTPS once startTls() ran.
  QString c() const {
    return tls_.isListening() ? QStringLiteral("https://127.0.0.2:%1").arg(tls_.serverPort())
                              : QStringLiteral("http://127.0.0.2:%1").arg(port());
  }
  // Serves the third party over TLS too: WebKit (libsoup) stores the
  // SameSite=None cookies third parties use only from secure origins, and
  // loopback HTTP is not one there.
  bool startTls(const QSslCertificate &certificate, const QSslKey &key) {
    QSslConfiguration configuration = QSslConfiguration::defaultConfiguration();
    configuration.setLocalCertificate(certificate);
    configuration.setPrivateKey(key);
    tls_.setSslConfiguration(configuration);
    QObject::connect(&tls_, &QTcpServer::pendingConnectionAvailable, [this] { accept(&tls_); });
    return tls_.listen(QHostAddress(QStringLiteral("127.0.0.2")), 0);
  }
  // The query of the last request for `path`, and the Cookie header it sent.
  QString lastQuery(const QString &path) const { return queries_.value(path); }
  QString cookieHeader(const QString &path) const { return cookieHeaders_.value(path); }
  // A request header of the last request for `path` (name in lower case).
  QString header(const QString &path, const QByteArray &name) const {
    for (const QByteArray &line : heads_.value(path).split('\n'))
      if (line.toLower().startsWith(name + ':')) return QString::fromLatin1(line.mid(name.size() + 1).trimmed());
    return {};
  }
  // Client ports that requested `path` (one per connection used).
  QSet<quint16> ports(const QString &path) const { return ports_.value(path); }
  bool hit(const QString &path) const { return hits_.contains(path); }
  int count(const QString &path) const { return counts_.value(path); }
  bool hitPrefix(const QString &prefix) const {
    for (const QString &h : hits_)
      if (h.startsWith(prefix)) return true;
    return false;
  }

 private:
  void accept(QTcpServer *server) {
    while (QTcpSocket *socket = server->nextPendingConnection()) {
      auto buffer = std::make_shared<QByteArray>();
      QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer] { serve(socket, *buffer); });
      QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    }
  }

  void serve(QTcpSocket *socket, QByteArray &buffer) {
    buffer.append(socket->readAll());
    for (int end = buffer.indexOf("\r\n\r\n"); end >= 0; end = buffer.indexOf("\r\n\r\n")) {
      const QByteArray head = buffer.left(end);
      buffer.remove(0, end + 4);
      respond(socket, head);
    }
  }

  static QByteArray html(const QString &text) { return text.toUtf8(); }

  void respond(QTcpSocket *socket, const QByteArray &head) {
    const QString target = QString::fromLatin1(head.split(' ').value(1));
    const QString path = target.section(QLatin1Char('?'), 0, 0);
    const QString query = target.section(QLatin1Char('?'), 1);
    hits_.insert(path);
    ++counts_[path];
    queries_[path] = query;
    heads_[path] = head;
    ports_[path].insert(socket->peerPort());
    QByteArray cacheControl = "no-store";
    cookieHeaders_[path].clear();
    for (const QByteArray &line : head.split('\n'))
      if (line.toLower().startsWith("cookie:")) cookieHeaders_[path] = QString::fromLatin1(line.mid(7).trimmed());
    QByteArray type = "text/html; charset=utf-8";
    QByteArray body;
    QByteArray status = "200 OK";
    qint64 first = 0, last = -1;
    bool partial = false;

    if (path.startsWith(QLatin1String("/s/"))) {
      // Script policy fixture: every way a page runs its own JavaScript sends
      // a beacon; a plain image is the "page fully loaded" control.
      const QString tag = path.mid(3);
      const QString frameHost = query == QLatin1String("frame=a") ? a() : query == QLatin1String("frame=b") ? b() : QString();
      body = html(QStringLiteral(
          "<!doctype html><title>static</title>"
          "<script>document.title='ran';new Image().src='/beacon/%1/inline';</script>"
          "<img src='/missing.png' onerror=\"new Image().src='/beacon/%1/handler'\">"
          "<script src='/ext.js?%1'></script>"
          "<noscript><p id=ns>noscript</p></noscript>"
          "<a id=toA href='%2/s/%1-a'>A</a> <a id=toB href='%3/s/%1-b'>B</a>"
          "%4<img src='/beacon/%1/img'>")
          .arg(tag, a(), b(),
               frameHost.isEmpty() ? QString() : QStringLiteral("<iframe src='%1/frame/%2'></iframe>").arg(frameHost, tag)));
    } else if (path == QLatin1String("/ext.js")) {
      type = "text/javascript";
      body = html(QStringLiteral("new Image().src='/beacon/%1/ext';").arg(query));
    } else if (path.startsWith(QLatin1String("/frame/"))) {
      body = html(QStringLiteral("<!doctype html><script>new Image().src='/beacon/%1/frame';</script><img src='/beacon/%1/frame-img'>")
                      .arg(path.mid(7)));
    } else if (path.startsWith(QLatin1String("/w/"))) {
      const QString tag = path.mid(3);
      body = html(QStringLiteral(
          "<!doctype html><title>workers</title><script>"
          "new Worker('/worker.js?%1');"
          "navigator.serviceWorker&&navigator.serviceWorker.register('/sw.js?%1').catch(()=>{});"
          "try{new WebSocket('ws://127.0.0.1:%2/ws-probe/%1')}catch(e){}"
          "</script>").arg(tag).arg(port()));
    } else if (path == QLatin1String("/worker.js")) {
      type = "text/javascript";
      body = html(QStringLiteral("fetch('/worker-fetch/%1').catch(()=>{});").arg(query));
    } else if (path == QLatin1String("/sw.js")) {
      type = "text/javascript";
      body = html(QStringLiteral("self.addEventListener('install',e=>e.waitUntil(fetch('/sw-fetch/%1').catch(()=>{})));").arg(query));
    } else if (path.startsWith(QLatin1String("/ws-probe/"))) {
      status = "404 Not Found";  // the handshake request is what is measured
    } else if (path.startsWith(QLatin1String("/pv/"))) {
      // Privacy fixture. Every page reports to /r/<id>/<kind>?<json>.
      const QStringList parts = path.mid(4).split(QLatin1Char('/'));
      const QString kind = parts.value(0), id = parts.value(1), mode = parts.value(2);
      // Each engine stores one of these over loopback HTTP: Chromium the
      // Secure one (loopback is a secure context), WebKit the other.
      const QString cookieJs = QStringLiteral(
          "const setCookies=(p)=>{document.cookie=p+'s=1; path=/; max-age=3600; SameSite=None; Secure';"
          "document.cookie=p+'n=1; path=/; max-age=3600; SameSite=None'};"
          "const report=(k,o)=>{new Image().src='/r/%1/'+k+'?'+encodeURIComponent(JSON.stringify(o))};").arg(id);
      if (kind == QLatin1String("first")) {
        body = html(QStringLiteral("<!doctype html><script>%1setCookies('fp%2');report('first',{cookie:document.cookie});</script>")
                        .arg(cookieJs, id));
      } else if (kind == QLatin1String("embed")) {
        body = html(QStringLiteral("<!doctype html><iframe src='%1'></iframe>")
                        .arg(QUrl::fromPercentEncoding(query.section(QStringLiteral("frame="), 1).toUtf8()).toHtmlEscaped()));
      } else if (kind == QLatin1String("frame")) {
        body = html(QStringLiteral(
            "<!doctype html><script>%1(async()=>{const id='%2',mode='%3',out={cookieBefore:document.cookie};"
            "const tryIt=async f=>{try{return await Promise.race([f(),new Promise(r=>setTimeout(()=>r('ERR timeout'),5000))])}"
            "catch(e){return 'ERR '+e.name}};"
            "const idb=w=>new Promise(r=>{const q=indexedDB.open('pv',1);q.onupgradeneeded=()=>q.result.createObjectStore('s');"
            "q.onerror=()=>r('ERR '+(q.error&&q.error.name));q.onsuccess=()=>{const t=q.result.transaction('s',w?'readwrite':'readonly'),"
            "st=t.objectStore('s');if(w){st.put(id,'k');t.oncomplete=()=>r('written')}else{const g=st.get('k');g.onsuccess=()=>r(g.result??null)}}});"
            "if(mode==='write'){setCookies('tp'+id);"
            "out.localStorage=await tryIt(()=>{localStorage.setItem('k',id);return 'written'});"
            "out.indexedDB=await tryIt(()=>idb(true));"
            "out.cacheStorage=await tryIt(async()=>{const c=await caches.open('pv');await c.put('/pv-entry',new Response(id));return 'written'});"
            "out.serviceWorker=await tryIt(async()=>{await navigator.serviceWorker.register('/sw.js?pv'+id);return 'written'});"
            "}else{out.localStorage=await tryIt(()=>localStorage.getItem('k'));out.indexedDB=await tryIt(()=>idb(false));"
            "out.cacheStorage=await tryIt(async()=>{const c=await caches.open('pv');const r=await c.match('/pv-entry');return r?await r.text():null});"
            "out.serviceWorker=await tryIt(async()=>(await navigator.serviceWorker.getRegistrations()).length?id:null);}"
            "out.storageAccessApi=typeof document.requestStorageAccess;out.cookieAfter=document.cookie;report(mode,out)})();</script>")
            .arg(cookieJs, id, mode));
      } else if (kind == QLatin1String("check")) {
        body = html(QStringLiteral("<!doctype html><script>%1report('check',{cookie:document.cookie,"
                                   "localStorage:(()=>{try{return localStorage.getItem('k')}catch(e){return 'ERR'}})()});</script>")
                        .arg(cookieJs));
      } else if (kind == QLatin1String("name") && mode == QLatin1String("set")) {
        body = html(QStringLiteral("<!doctype html><script>window.name='wn%1';location.href=%2;</script>")
                        .arg(id, QStringLiteral("'") + QUrl::fromPercentEncoding(query.section(QStringLiteral("next="), 1).toUtf8()) +
                                     QStringLiteral("'")));
      } else if (kind == QLatin1String("name")) {
        body = html(QStringLiteral("<!doctype html><script>%1report('name',{name:window.name});</script>").arg(cookieJs));
      }
    } else if (path == QLatin1String("/fp-worker.js")) {
      body = QByteArrayLiteral(
          "postMessage({gpc:navigator.globalPrivacyControl===true,language:navigator.language,"
          "languages:Array.from(navigator.languages||[]),cpu:navigator.hardwareConcurrency,"
          "memory:navigator.deviceMemory??null,offscreen:typeof OffscreenCanvas});");
      type = QByteArrayLiteral("application/javascript");
    } else if (path.startsWith(QLatin1String("/fp/"))) {
      // Fingerprinting probe: what a fingerprinting script reads, reported
      // to /r/<id>/fp; ?go=<url> then navigates the page itself.
      const QString id = path.mid(4);
      const QString go = QUrl::fromPercentEncoding(query.section(QStringLiteral("go="), 1).toUtf8());
      body = html(QStringLiteral(
          "<!doctype html><div class=dn-cosmetic-probe>ad</div><img src='/beacon/fp-%1'><script>(async()=>{"
          "const fnv=s=>{let h=2166136261;for(let i=0;i<s.length;i++){h^=s.charCodeAt(i);h=Math.imul(h,16777619)>>>0}return h.toString(16)};"
          "let worker={};try{worker=await new Promise(resolve=>{const w=new Worker('/fp-worker.js');const t=setTimeout(()=>resolve({error:'timeout'}),2000);"
          "w.onmessage=e=>{clearTimeout(t);resolve(e.data);w.terminate()};w.onerror=()=>{clearTimeout(t);resolve({error:'load'});w.terminate()}})}catch(e){worker={error:e.name}}"
          "const c=document.createElement('canvas');c.width=200;c.height=50;const x=c.getContext('2d');x.font='16px sans-serif';"
          "x.fillStyle='#f60';x.fillRect(5,5,90,30);x.fillStyle='#069';x.fillText('DaliNira fp',10,30);"
          "const tm=x.measureText('DaliNira font probe'),textMetrics=[tm.width,tm.actualBoundingBoxLeft,tm.actualBoundingBoxRight].join(':');"
          "let audio='none';try{const a=new OfflineAudioContext(1,44100,44100),o=a.createOscillator(),k=a.createDynamicsCompressor();"
          "o.type='triangle';o.frequency.value=10000;o.connect(k);k.connect(a.destination);o.start(0);const b=await a.startRendering();"
          "const d=b.getChannelData(0);let s=0;for(let i=4500;i<5000;i++)s+=Math.abs(d[i]);audio=String(s)}catch(e){audio='ERR '+e.name}"
          "let gl='none',glLimit=0,glPrecision='',glDebug=false;try{const g=document.createElement('canvas').getContext('webgl');const e=g&&g.getExtension('WEBGL_debug_renderer_info');"
          "glDebug=!!e;gl=e?g.getParameter(e.UNMASKED_RENDERER_WEBGL):'no-extension';if(g){glLimit=g.getParameter(g.MAX_TEXTURE_SIZE);const p=g.getShaderPrecisionFormat(g.FRAGMENT_SHADER,g.HIGH_FLOAT);glPrecision=p.rangeMin+':'+p.rangeMax+':'+p.precision}}catch(e){gl='ERR'}"
          "let localFonts=-1;try{if(typeof navigator.queryLocalFonts==='function')localFonts=(await navigator.queryLocalFonts()).length}catch(e){localFonts=0}"
          "await new Promise(r=>setTimeout(r,300));"
          "const ad=document.querySelector('.dn-cosmetic-probe');"
          "const o={gpc:navigator.globalPrivacyControl===true,ua:navigator.userAgent,canvas:fnv(c.toDataURL()),audio,"
          "cpu:navigator.hardwareConcurrency,memory:navigator.deviceMemory??null,touch:navigator.maxTouchPoints,platform:navigator.platform,"
          "language:navigator.language,languages:Array.from(navigator.languages||[]),intl:new Intl.DateTimeFormat().resolvedOptions().locale,"
          "screen:screen.width+'x'+screen.height,webgpu:typeof navigator.gpu,gl,glDebug,glLimit,glPrecision,textMetrics,"
          "fontArial:document.fonts?document.fonts.check('12px Arial'):false,localFonts,cssWide:matchMedia('(color-gamut: p3)').matches,worker,"
          "cosmeticHidden:!!ad&&getComputedStyle(ad).display==='none'};"
          "new Image().src='/r/%1/fp?'+encodeURIComponent(JSON.stringify(o));"
          "%2})();</script>")
          .arg(id, go.isEmpty() ? QString() : QStringLiteral("setTimeout(()=>location.assign('%1'),200);").arg(go)));
    } else if (path.startsWith(QLatin1String("/nc/"))) {
      // Network-state fixture: a third-party frame loading cacheable
      // resources of every kind, then three requests on keep-alive
      // connections whose client ports show connection reuse.
      const QStringList parts = path.mid(4).split(QLatin1Char('/'));
      if (parts.value(0) == QLatin1String("frame")) {
        const QString id = parts.value(1), step = parts.value(2);
        body = html(QStringLiteral(
            "<!doctype html><link rel=stylesheet href='/nc/res/css/%1'><script src='/nc/res/js/%1'></script>"
            "<img id=i src='/nc/res/img/%1'><script>(async()=>{await fetch('/nc/res/fetch/%1').catch(()=>{});"
            "await new Promise(r=>{const i=document.getElementById('i');if(i.complete)r();else{i.onload=i.onerror=r}});"
            "for(let n=0;n<3;n++)await fetch('/nc/conn/%1/%2/'+n,{cache:'no-store'}).catch(()=>{});"
            "new Image().src='/r/%1/%2';})();</script>").arg(id, step));
      } else if (parts.value(0) == QLatin1String("res")) {
        const QString kind = parts.value(1);
        cacheControl = "public, max-age=3600";
        if (kind == QLatin1String("img")) { type = "image/gif"; body = QByteArray::fromBase64("R0lGODlhAQABAAAAACw="); }
        else if (kind == QLatin1String("css")) { type = "text/css"; body = "#x{color:red}"; }
        else if (kind == QLatin1String("js")) { type = "text/javascript"; body = "window.__nc=1;"; }
        else { type = "text/plain"; body = "cached"; }
      }
    } else if (path == QLatin1String("/layout")) {
      // Solid magenta, so a screenshot tells the page from browser chrome
      // drawn over it; a reload or a new page changes __id or the load count.
      body = html(QStringLiteral(
          "<!doctype html><title>Layout</title><style>html,body{margin:0;height:100%;background:#ff00ff}</style>"
          "<script>window.__id=String(Math.random());"
          "sessionStorage.setItem('loads',String((+sessionStorage.getItem('loads')||0)+1));</script>"));
    } else if (path == QLatin1String("/translate-scroll")) {
      body = html(QStringLiteral(
          "<!doctype html><html lang=en><title>Translate Scroll</title>"
          "<style>html,body{margin:0;min-height:5000px;background:#102030}</style>"
          "<body><div style='height:5000px'></div><script>"
          "window.__outsideClicks=0;document.body.addEventListener('click',()=>window.__outsideClicks++);"
          "</script></body></html>"));
    } else if (path.startsWith(QLatin1String("/shot-"))) {
      // Documentation screenshot pages (the "screenshots" case): neutral,
      // self-contained content with no personal data.
      const QString style = QStringLiteral(
          "<style>body{margin:0;background:#f7f8fa;color:#1d2430;font:17px/1.65 system-ui,sans-serif}"
          "main{max-width:760px;margin:0 auto;padding:48px 28px}h1{font-size:34px;line-height:1.2;margin:0 0 6px}"
          ".meta{color:#5d6878;margin:0 0 28px}h2{font-size:22px;margin:32px 0 8px}</style>");
      if (path == QLatin1String("/shot-article")) {
        body = html(QStringLiteral(
            "<!doctype html><html lang=en><title>A Practical Guide to Private Browsing</title>%1<main>"
            "<h1>A Practical Guide to Private Browsing</h1><p class=meta>Sample article for the DaliNira documentation</p>"
            "<p>A modern browser does far more than display pages. Every time the browser loads a site it also talks to "
            "analytics services, advertising networks and content delivery systems, and each of them can learn a little "
            "about the person behind the screen.</p>"
            "<h2>Blocking at the source</h2><p>The most effective protection happens before a request ever leaves the "
            "browser. Filter lists describe known trackers, and a browser that applies them natively saves bandwidth "
            "as well as privacy.</p>"
            "<h2>Cleaner links</h2><p>Many shared links carry tracking parameters. Removing them keeps the page the same "
            "while the browser stops forwarding identifiers to the next site.</p>"
            "<h2>Choosing an engine</h2><p>Different rendering engines behave differently. Being able to switch the "
            "browser engine without changing the rest of the interface makes it easy to compare them.</p>"
            "</main></html>").arg(style));
      } else if (path == QLatin1String("/shot-article-fr")) {
        body = html(QStringLiteral(
            "<!doctype html><html lang=fr><title>Le navigateur moderne</title>%1<main>"
            "<h1>Le navigateur moderne</h1><p class=meta>Article d'exemple pour la documentation</p>"
            "<p>Un navigateur moderne fait bien plus qu'afficher des pages. Chaque fois qu'il charge un site, il "
            "communique aussi avec des services de mesure d'audience et des r\u00e9seaux publicitaires.</p>"
            "<h2>Bloquer \u00e0 la source</h2><p>La protection la plus efficace intervient avant qu'une requ\u00eate ne "
            "quitte le navigateur. Les listes de filtres d\u00e9crivent les traqueurs connus.</p>"
            "<h2>Des liens plus propres</h2><p>De nombreux liens partag\u00e9s contiennent des param\u00e8tres de suivi. "
            "Les retirer ne change pas la page.</p></main></html>").arg(style));
      } else {
        const QString title = path == QLatin1String("/shot-notes") ? QStringLiteral("Release Checklist")
                                                                    : QStringLiteral("Weekly Reading List");
        body = html(QStringLiteral("<!doctype html><html lang=en><title>%2</title>%1<main><h1>%2</h1>"
                                   "<p class=meta>Sample page</p><p>Placeholder content for a background tab.</p>"
                                   "</main></html>").arg(style, title));
      }
    } else if (path == QLatin1String("/find")) {
      body = html(QStringLiteral(
          "<!doctype html><title>Find Fixture</title><main>"
          "needle alpha needle beta needle gamma"
          "</main>"));
    } else if (path == QLatin1String("/opener")) {
      body = html(QStringLiteral(
          "<!doctype html><title>Opener</title>"
          "<a id=blank target=_blank href='/child?blank'>blank</a> "
          "<a id=noopener target=_blank rel=noopener href='/child?noopener'>noopener</a>"));
    } else if (path == QLatin1String("/autopopup")) {
      body = html(QStringLiteral("<!doctype html><title>Autopopup</title>"
                                 "<script>window.__blocked=!window.open('/child?nogesture','_blank');</script>"));
    } else if (path == QLatin1String("/child")) {
      body = html(QStringLiteral("<!doctype html><title>Child</title><p>child</p>"));
    } else if (path == QLatin1String("/media")) {
      body = html(QStringLiteral(
          "<!doctype html><title>Media</title><style>body{margin:0;background:#000}"
          "video{width:480px;height:270px}#box:fullscreen video{width:100%;height:100%}</style>"
          // A player container, as YouTube's: pages fullscreen it, not the <video>.
          "<div id=box><video id=v src='/clip.webm' muted autoplay playsinline></video></div>"
          // Rewind before the 4 s clip ends: WebKitGTK 2.52.6 stalls for good
          // once a progressive clip wraps or replays from its end (upstream
          // bug, see webkit_media_test); this case is about fullscreen.
          "<script>v.addEventListener('timeupdate',()=>{if(v.currentTime>2.8)v.currentTime=0.2});"
          "window.__frames=0;(function tick(){window.__frames++;requestAnimationFrame(tick)})();</script>"
          "<button id=fs onclick=\"v.requestFullscreen().then(()=>window.__fs='ok',e=>window.__fs='err:'+e.name)\""
          " style='position:fixed;left:0;top:300px;width:200px;height:60px'>fullscreen</button>"
          "<button id=fsbox onclick=\"box.requestFullscreen().then(()=>window.__fs='ok',e=>window.__fs='err:'+e.name)\""
          " style='position:fixed;left:0;top:370px;width:200px;height:60px'>player fullscreen</button>"));
    } else if (path == QLatin1String("/clip.webm")) {
      type = "video/webm";
      body = clip_;
      const int rangeAt = head.toLower().indexOf("range: bytes=");
      if (rangeAt >= 0) {
        const QList<QByteArray> bounds = head.mid(rangeAt + 13).split('\r').first().split('-');
        first = bounds.value(0).toLongLong();
        last = bounds.size() > 1 && !bounds.at(1).isEmpty() ? bounds.at(1).toLongLong() : -1;
        partial = true;
      }
    } else if (path.startsWith(QLatin1String("/beacon/"))) {
      type = "image/gif";
      body = QByteArray::fromBase64("R0lGODlhAQABAAAAACw=");
    } else if (path == QLatin1String("/audit/google")) {
      body = html(QStringLiteral(
          "<!doctype html><title>dalinira browser - Google Search</title>"
          "<form action='/search'><input name='q' value='dalinira browser'><button type='submit'>Google Search</button></form>"
          "<div id='res'>Google Search Results for dalinira browser</div>"));
    } else if (path == QLatin1String("/audit/github")) {
      body = html(QStringLiteral(
          "<!doctype html><title>Sign in to GitHub · GitHub</title>"
          "<form action='/session'><input id='login_field' name='login' value='octocat'>"
          "<input id='password' name='password' type='password' value='secret'>"
          "<input name='authenticity_token' value='csrf-mock-token-xyz' type='hidden'></form>"));
    } else if (path == QLatin1String("/audit/reddit")) {
      body = html(QStringLiteral(
          "<!doctype html><title>DaliNira Browser Community on Reddit</title>"
          "<div class='entry'><p class='title'>Discussion on Brave-level privacy</p>"
          "<img src='https://events.redditmedia.com/pixel.gif' style='display:none'></div>"));
    } else if (path == QLatin1String("/audit/youtube")) {
      body = html(QStringLiteral(
          "<!doctype html><title>YouTube</title>"
          "<video id='player' src='/clip.webm' autoplay muted playsinline></video>"
          "<script src='https://www.youtube.com/pagead/adview'></script>"));
    } else if (path == QLatin1String("/audit/download.bin")) {
      type = "application/octet-stream";
      body = QByteArray("DALINIRA_AUDIT_DOWNLOAD_PAYLOAD_VERIFIED_1234567890");
    } else if (path == QLatin1String("/audit/widget")) {
      body = html(QStringLiteral(
          "<!doctype html><title>Top Page</title>"
          "<iframe id='w' src='%1/pv/frame/audit/write'></iframe>").arg(c()));
    } else {
      status = "404 Not Found";
    }
    if (last < 0 || last >= body.size()) last = body.size() - 1;
    QByteArray response = "HTTP/1.1 " + (partial ? QByteArray("206 Partial Content") : status) + "\r\n";
    response += "Content-Type: " + type + "\r\nCache-Control: " + cacheControl + "\r\nAccept-Ranges: bytes\r\nContent-Length: " +
                QByteArray::number(qMax<qint64>(0, last - first + 1)) + "\r\n";
    if (partial)
      response += "Content-Range: bytes " + QByteArray::number(first) + "-" + QByteArray::number(last) + "/" +
                  QByteArray::number(body.size()) + "\r\n";
    response += "\r\n" + body.mid(first, last - first + 1);
    socket->write(response);
  }

  QTcpServer v4_;
  QTcpServer v6_;
  QTcpServer third_;
  QSslServer tls_;
  QByteArray clip_;
  QSet<QString> hits_;
  QHash<QString, QString> queries_;
  QHash<QString, QString> cookieHeaders_;
  QHash<QString, QSet<quint16>> ports_;
  QHash<QString, QByteArray> heads_;
  QHash<QString, int> counts_;
};

// ---------------------------------------------------------------------------
// Engine-neutral page access
// ---------------------------------------------------------------------------

QWidget *pageWidget(BrowserWindow &window, int index) {
  if (index < 0 || index >= window.tabCount()) return nullptr;
  const BrowserTabInfo &info = window.tabInfo(index);
  if (info.engineView) return info.engineView.data();
  return info.view.data();
}

QWidget *currentPage(BrowserWindow &window) {
  if (auto *engineView = window.currentEngineView()) return engineView;
  return window.currentView();
}

int currentIndex(BrowserWindow &window) {
  QWidget *page = currentPage(window);
  for (int i = 0; i < window.tabCount(); ++i)
    if (pageWidget(window, i) == page) return i;
  return -1;
}

// Main world (the page's), or DaliNira's isolated world.
QVariant evaluate(QWidget *page, const QString &script, bool isolated = false) {
  // A result can arrive after the timeout below; it must not write into this
  // returned frame, so the callback owns the state it fills in.
  const auto state = std::make_shared<std::pair<bool, QVariant>>(false, QVariant());
  const auto callback = [state](const QVariant &v) {
    state->second = v;
    state->first = true;
  };
  if (auto *engineView = qobject_cast<dalinira::engine::EngineView *>(page)) {
    if (isolated) engineView->runIsolatedJavaScript(script, callback);
    else engineView->runJavaScript(script, callback);
  } else if (auto *webView = qobject_cast<QWebEngineView *>(page)) {
    webView->page()->runJavaScript(script, isolated ? QWebEngineScript::ApplicationWorld : QWebEngineScript::MainWorld,
                                   callback);
  } else {
    return {};
  }
  if (!waitUntil([&] { return state->first; }, 10000)) return {};
  return state->second;
}

QString text(QWidget *page, const QString &script, bool isolated = false) {
  return evaluate(page, script, isolated).toString();
}

bool tabReady(BrowserWindow &window, int index, const QString &title) {
  return waitUntil([&] {
    return index < window.tabCount() && window.tabInfo(index).title == title && !window.tabInfo(index).isLoading;
  }, 20000);
}

// ---------------------------------------------------------------------------
// Case: popup / opener
// ---------------------------------------------------------------------------

void runPopupCase(BrowserWindow &window, BrowserProfileService &profileService, LocalSite &site, bool webkit) {
  window.navigateCurrentTab(QUrl(site.a() + QStringLiteral("/opener")));
  CHECK(tabReady(window, 0, QStringLiteral("Opener")));
  QWidget *opener = pageWidget(window, 0);

  // Pop-ups setting off (the default): a script without a user gesture
  // cannot open a window.
  CHECK(!profileService.arePopupsAllowed());
  // (A page's own script while it loads: evaluate_javascript would count as a
  // user gesture on WebKit.)
  window.navigateCurrentTab(QUrl(site.a() + QStringLiteral("/autopopup")));
  CHECK(tabReady(window, 0, QStringLiteral("Autopopup")));
  result(QStringLiteral("popup.no_gesture_blocked"), text(opener, QStringLiteral("String(window.__blocked)")));
  settle(800);
  result(QStringLiteral("popup.no_gesture_tabs"), QString::number(window.tabCount()));
  while (window.tabCount() > 1) window.closeTab(1);
  window.navigateCurrentTab(QUrl(site.a() + QStringLiteral("/opener")));
  CHECK(tabReady(window, 0, QStringLiteral("Opener")));
  opener = pageWidget(window, 0);

  profileService.setPopupsAllowed(true);

  // window.open: a tab with the opener relationship in both directions.
  result(QStringLiteral("popup.open_returns_proxy"),
         text(opener, QStringLiteral("window.__w=window.open('/child?open','_blank');String(!!window.__w)")));
  CHECK(waitUntil([&] { return window.tabCount() == 2; }, 8000));
  CHECK(tabReady(window, 1, QStringLiteral("Child")));
  QWidget *child = pageWidget(window, 1);
  result(QStringLiteral("popup.selected"), currentIndex(window) == 1);
  result(QStringLiteral("popup.child_url"), QUrl(text(child, QStringLiteral("location.href"))).path() +
                                                QStringLiteral("?") + QUrl(text(child, QStringLiteral("location.href"))).query());
  result(QStringLiteral("popup.child_sees_opener"),
         text(child, QStringLiteral("String(window.opener&&window.opener.document.title)")));
  result(QStringLiteral("popup.opener_sees_child"), text(opener, QStringLiteral("String(window.__w.document.title)")));
  result(QStringLiteral("popup.child_writes_opener"),
         text(child, QStringLiteral("window.opener.document.body.dataset.fromChild='yes';"
                                    "String(window.opener.document.body.dataset.fromChild)")));
#if defined(DALINIRA_BUILD_WEBKIT)
  if (webkit) {
    // Related views share the opener's web process (no detached page).
    auto *openerView = qobject_cast<dalinira::engine::EngineView *>(opener);
    auto *childView = qobject_cast<dalinira::engine::EngineView *>(child);
    CHECK(openerView && childView && openerView->contentProcessId() > 0);
    CHECK(waitUntil([&] { return childView->contentProcessId() == openerView->contentProcessId(); }, 5000));
    log(QStringLiteral("popup shares the opener's web process"));
  }
#endif

  // window.close() from the popup closes its tab and the opener sees it.
  evaluate(child, QStringLiteral("window.close()"));
  CHECK(waitUntil([&] { return window.tabCount() == 1; }, 5000));
  result(QStringLiteral("popup.closed_by_window_close"), true);
  result(QStringLiteral("popup.opener_sees_closed"), text(opener, QStringLiteral("String(window.__w.closed)")));
  result(QStringLiteral("popup.opener_selected_after_close"), currentIndex(window) == 0);

  // target=_blank (implies noopener in both engines) and explicit rel=noopener.
  for (const QString &id : {QStringLiteral("blank"), QStringLiteral("noopener")}) {
    evaluate(opener, QStringLiteral("document.getElementById('%1').click()").arg(id));
    CHECK(waitUntil([&] { return window.tabCount() == 2; }, 8000));
    CHECK(tabReady(window, 1, QStringLiteral("Child")));
    QWidget *page = pageWidget(window, 1);
    result(QStringLiteral("popup.%1.query").arg(id), QUrl(text(page, QStringLiteral("location.href"))).query());
    result(QStringLiteral("popup.%1.opener").arg(id), text(page, QStringLiteral("String(window.opener)")));
    window.closeTab(1);
    CHECK(waitUntil([&] { return window.tabCount() == 1; }, 5000));
  }
  // window.open with the noopener feature returns null but opens the tab.
  result(QStringLiteral("popup.open_noopener_returns"),
         text(opener, QStringLiteral("String(window.open('/child?feature','_blank','noopener'))")));
  CHECK(waitUntil([&] { return window.tabCount() == 2; }, 8000));
  CHECK(tabReady(window, 1, QStringLiteral("Child")));
  result(QStringLiteral("popup.open_noopener.opener"), text(pageWidget(window, 1), QStringLiteral("String(window.opener)")));
  window.closeTab(1);
  CHECK(waitUntil([&] { return window.tabCount() == 1; }, 5000));

  // An about:blank popup the opener writes, then closes.
  result(QStringLiteral("popup.blank_written"),
         text(opener, QStringLiteral("window.__w2=window.open('','_blank');"
                                     "window.__w2.document.write('<title>Written</title><p id=w>written</p>');"
                                     "window.__w2.document.close();String(window.__w2.document.title)")));
  CHECK(waitUntil([&] { return window.tabCount() == 2; }, 8000));
  CHECK(waitUntil([&] { return window.tabInfo(1).title == QLatin1String("Written"); }, 8000));
  result(QStringLiteral("popup.blank_tab_title"), window.tabInfo(1).title);
  result(QStringLiteral("popup.blank_content"),
         text(pageWidget(window, 1), QStringLiteral("String(document.getElementById('w')&&document.getElementById('w').textContent)")));
  evaluate(opener, QStringLiteral("window.__w2.close()"));
  CHECK(waitUntil([&] { return window.tabCount() == 1; }, 5000));
  result(QStringLiteral("popup.opener_closes_popup"), true);
  settle(500);
  result(QStringLiteral("popup.final_tabs"), QString::number(window.tabCount()));
  log(QStringLiteral("PASS popup / opener"));
}

// ---------------------------------------------------------------------------
// Case: block scripts + worker / WebSocket measurement
// ---------------------------------------------------------------------------

void setBlockScripts(DaliNiraBlockerService &blocker, const QString &host, bool on) {
  SitePolicy policy = blocker.settings()->sitePolicy(host);
  policy.blockScripts = on;
  blocker.settings()->setSitePolicy(host, policy);
}

// Loads a script fixture and reports which of the page's own script paths ran.
void scriptPage(BrowserWindow &window, LocalSite &site, const QString &name, const QUrl &url,
                bool viaPage = false) {
  const QString tag = url.path().mid(3);
  if (viaPage) {
    // A page-started navigation from the current document (link click in
    // DaliNira's isolated world: works whether or not the page runs scripts).
    evaluate(currentPage(window), QStringLiteral("location.assign(%1)").arg(
        QStringLiteral("'") + url.toString() + QStringLiteral("'")), true);
  } else {
    window.navigateCurrentTab(url);
  }
  CHECK(waitUntil([&] { return site.hit(QStringLiteral("/beacon/%1/img").arg(tag)); }, 15000));
  if (url.query().startsWith(QLatin1String("frame=")))
    CHECK(waitUntil([&] { return site.hit(QStringLiteral("/beacon/%1/frame-img").arg(tag)); }, 15000));
  CHECK(waitUntil([&] { return !window.tabInfo(currentIndex(window)).isLoading; }, 15000));
  settle(600);
  QStringList ran;
  for (const char *kind : {"inline", "handler", "ext", "frame"})
    if (site.hit(QStringLiteral("/beacon/%1/%2").arg(tag, QLatin1String(kind)))) ran << QLatin1String(kind);
  result(QStringLiteral("scripts.%1.ran").arg(name), ran.isEmpty() ? QStringLiteral("none") : ran.join(QLatin1Char(',')));
  // DaliNira's own (isolated-world) scripts keep working either way.
  result(QStringLiteral("scripts.%1.isolated_title").arg(name), text(currentPage(window), QStringLiteral("document.title"), true));
}

void runScriptsCase(BrowserWindow &window, BrowserProfileService &profileService, LocalSite &site, bool webkit) {
  DaliNiraBlockerService *blocker = profileService.adBlockService();
  CHECK(blocker);
  blocker->settings()->setAutoReloadOnModeChange(false);
  blocker->settings()->setProtectionEnabled(true);
  const QString hostA = QStringLiteral("127.0.0.1");

  scriptPage(window, site, QStringLiteral("allowed"), QUrl(site.a() + QStringLiteral("/s/t1?frame=b")));

  setBlockScripts(*blocker, hostA, true);
  scriptPage(window, site, QStringLiteral("blocked"), QUrl(site.a() + QStringLiteral("/s/t2?frame=b")));
  result(QStringLiteral("scripts.blocked.noscript_rendered"),
         text(currentPage(window), QStringLiteral("String(!!document.getElementById('ns')&&document.getElementById('ns').offsetHeight>0)"), true));
  // The policy follows the top-level site: frames inherit it.
  scriptPage(window, site, QStringLiteral("allowed_main_blocked_frame"), QUrl(site.b() + QStringLiteral("/s/t3?frame=a")));
  // Page-started cross-site navigations in both directions.
  scriptPage(window, site, QStringLiteral("page_nav_to_blocked"), QUrl(site.a() + QStringLiteral("/s/t4")), true);
  scriptPage(window, site, QStringLiteral("page_nav_to_allowed"), QUrl(site.b() + QStringLiteral("/s/t5")), true);
  // Shield-style toggle: unblock, reload the same page.
  setBlockScripts(*blocker, hostA, false);
  scriptPage(window, site, QStringLiteral("unblocked"), QUrl(site.a() + QStringLiteral("/s/t6")));

  // The browser-wide JavaScript setting.
  profileService.setJavascriptEnabled(false);
  scriptPage(window, site, QStringLiteral("js_setting_off"), QUrl(site.b() + QStringLiteral("/s/t7")));
  profileService.setJavascriptEnabled(true);
  scriptPage(window, site, QStringLiteral("js_setting_on"), QUrl(site.b() + QStringLiteral("/s/t8")));
  log(QStringLiteral("PASS block scripts"));

  // Workers and WebSockets: do their requests reach the shared blocker
  // decision? Reported (not a parity result): changing it is ad-blocking work.
  window.navigateCurrentTab(QUrl(site.a() + QStringLiteral("/w/t9")));
  CHECK(waitUntil([&] { return site.hit(QStringLiteral("/worker-fetch/t9")); }, 15000));
  const bool wsReached = waitUntil([&] { return site.hitPrefix(QStringLiteral("/ws-probe/t9")); }, 5000);
  const bool swReached = waitUntil([&] { return site.hit(QStringLiteral("/sw-fetch/t9")); }, 8000);
  settle(500);
  bool sawWorkerScript = false, sawWorkerFetch = false, sawWebSocket = false, sawServiceWorker = false,
       sawServiceWorkerFetch = false;
  for (const NetworkLogEntry &entry : blocker->recentLogs(2000)) {
    if (entry.requestUrl.endsWith(QLatin1String("/worker.js"))) sawWorkerScript = true;
    if (entry.requestUrl.endsWith(QLatin1String("/worker-fetch/t9"))) sawWorkerFetch = true;
    if (entry.requestUrl.contains(QLatin1String("/ws-probe/t9"))) sawWebSocket = true;
    if (entry.requestUrl.endsWith(QLatin1String("/sw.js"))) sawServiceWorker = true;
    if (entry.requestUrl.endsWith(QLatin1String("/sw-fetch/t9"))) sawServiceWorkerFetch = true;
  }
  std::cout << "[INFO] " << (webkit ? "webkit" : "chromium") << " shared blocker decision saw: worker script="
            << sawWorkerScript << " worker fetch=" << sawWorkerFetch << " websocket handshake=" << sawWebSocket
            << " service worker script=" << sawServiceWorker << " service worker fetch=" << sawServiceWorkerFetch
            << " (reached server: websocket=" << wsReached << " service worker fetch=" << swReached << ")" << std::endl;
}

// ---------------------------------------------------------------------------
// Case: cookie and storage isolation (third-party cookies under each cookie
// policy, storage partitioning by top-level site, window.name)
// ---------------------------------------------------------------------------

// The JSON a privacy fixture page reported to /r/<id>/<kind>.
QJsonObject privacyReport(LocalSite &site, const QString &id, const QString &kind) {
  const QString path = QStringLiteral("/r/%1/%2").arg(id, kind);
  CHECK(waitUntil([&] { return site.hit(path); }, 15000));
  return QJsonDocument::fromJson(QUrl::fromPercentEncoding(site.lastQuery(path).toUtf8()).toUtf8()).object();
}

// A throwaway certificate for 127.0.0.2, made at test time.
bool makeTestCertificate(const QString &dir, QByteArray *certificatePem, QByteArray *keyPem) {
  QProcess openssl;
  openssl.start(QStringLiteral("openssl"),
                {QStringLiteral("req"), QStringLiteral("-x509"), QStringLiteral("-newkey"), QStringLiteral("rsa:2048"),
                 QStringLiteral("-nodes"), QStringLiteral("-days"), QStringLiteral("2"), QStringLiteral("-subj"),
                 QStringLiteral("/CN=127.0.0.2"), QStringLiteral("-addext"), QStringLiteral("subjectAltName=IP:127.0.0.2"),
                 QStringLiteral("-keyout"), dir + QStringLiteral("/key.pem"), QStringLiteral("-out"),
                 dir + QStringLiteral("/cert.pem")});
  if (!openssl.waitForFinished(30000) || openssl.exitStatus() != QProcess::NormalExit || openssl.exitCode() != 0)
    return false;
  QFile certificate(dir + QStringLiteral("/cert.pem")), key(dir + QStringLiteral("/key.pem"));
  if (!certificate.open(QIODevice::ReadOnly) || !key.open(QIODevice::ReadOnly)) return false;
  *certificatePem = certificate.readAll();
  *keyPem = key.readAll();
  return !certificatePem->isEmpty() && !keyPem->isEmpty();
}

QString frameUrl(LocalSite &site, const QString &id, const QString &mode) {
  return QString::fromLatin1(QUrl::toPercentEncoding(site.c() + QStringLiteral("/pv/frame/%1/%2").arg(id, mode)));
}

void runPrivacyCase(BrowserWindow &window, BrowserProfileService &profileService, LocalSite &site, bool webkit) {
  // A = 127.0.0.1 and B = localhost embed the third party T = 127.0.0.2,
  // which the user has also visited as a first party.
#if defined(DALINIRA_BUILD_WEBKIT)
  QTemporaryDir certificateDir;
  if (webkit) {
    // T over HTTPS, its test certificate trusted for this profile only.
    QByteArray certificatePem, keyPem;
    CHECK(certificateDir.isValid() && makeTestCertificate(certificateDir.path(), &certificatePem, &keyPem));
    CHECK(site.startTls(QSslCertificate(certificatePem), QSslKey(keyPem, QSsl::Rsa)));
    GError *error = nullptr;
    GTlsCertificate *certificate = g_tls_certificate_new_from_pem(certificatePem.constData(), certificatePem.size(), &error);
    CHECK(certificate && !error);
    auto *backend = static_cast<dalinira::engine::WebKitBackend *>(
        dalinira::engine::EngineBackend::backendForType(EngineType::WebKit));
    webkit_web_context_allow_tls_certificate_for_host(backend->webKitProfile()->webContext(), certificate, "127.0.0.2");
    g_object_unref(certificate);
  }
#endif
  const QStringList policies = {QStringLiteral("allow_all"), QStringLiteral("third_party"), QStringLiteral("block_all")};
  for (int i = 0; i < policies.size(); ++i) {
    const QString policy = policies.at(i);
    const QString id = QString::number(i + 1);
    profileService.setCookiePolicy(policy);
    settle(300);
    window.navigateCurrentTab(QUrl(site.c() + QStringLiteral("/pv/first/") + id));
    const bool firstParty = privacyReport(site, id, QStringLiteral("first")).value(QStringLiteral("cookie")).toString()
                                .contains(QStringLiteral("fp") + id);
    window.navigateCurrentTab(QUrl(site.a() + QStringLiteral("/pv/embed/%1?frame=%2").arg(id, frameUrl(site, id, QStringLiteral("write")))));
    const QJsonObject frame = privacyReport(site, id, QStringLiteral("write"));
    const bool sent = site.cookieHeader(QStringLiteral("/pv/frame/%1/write").arg(id)).contains(QStringLiteral("fp") + id);
    const bool visible = frame.value(QStringLiteral("cookieBefore")).toString().contains(QStringLiteral("fp") + id);
    window.navigateCurrentTab(QUrl(site.c() + QStringLiteral("/pv/check/") + id));
    const QJsonObject check = privacyReport(site, id, QStringLiteral("check"));
    const bool setFromThirdParty = check.value(QStringLiteral("cookie")).toString().contains(QStringLiteral("tp") + id);
    result(QStringLiteral("privacy.%1.first_party_cookie").arg(policy), firstParty ? QStringLiteral("set") : QStringLiteral("none"));
    result(QStringLiteral("privacy.%1.third_party_cookie_sent").arg(policy), sent ? QStringLiteral("yes") : QStringLiteral("no"));
    result(QStringLiteral("privacy.%1.third_party_cookie_visible").arg(policy), visible ? QStringLiteral("yes") : QStringLiteral("no"));
    result(QStringLiteral("privacy.%1.third_party_cookie_set").arg(policy), setFromThirdParty ? QStringLiteral("yes") : QStringLiteral("no"));
    if (policy == QLatin1String("allow_all")) {
      // The control: with everything allowed, both directions work.
      CHECK(firstParty && sent && visible && setFromThirdParty);
    } else if (policy == QLatin1String("third_party")) {
      // DaliNira's default: the third party neither reads its first-party
      // cookie nor sets one while embedded.
      CHECK(firstParty && !sent && !visible && !setFromThirdParty);
    } else {
      CHECK(!firstParty && !sent && !visible && !setFromThirdParty);
    }
  }
  profileService.setCookiePolicy(QStringLiteral("third_party"));
  settle(300);

  // Storage of the third party embedded in A must not be readable when it is
  // embedded in B, nor by the third party as a first party.
  const QString id = QStringLiteral("9");
  window.navigateCurrentTab(QUrl(site.a() + QStringLiteral("/pv/embed/%1?frame=%2").arg(id, frameUrl(site, id, QStringLiteral("write")))));
  const QJsonObject written = privacyReport(site, id, QStringLiteral("write"));
  window.navigateCurrentTab(QUrl(site.a() + QStringLiteral("/pv/embed/%1?frame=%2").arg(id, frameUrl(site, id, QStringLiteral("readA")))));
  const QJsonObject sameSite = privacyReport(site, id, QStringLiteral("readA"));
  window.navigateCurrentTab(QUrl(site.b() + QStringLiteral("/pv/embed/%1?frame=%2").arg(id, frameUrl(site, id, QStringLiteral("readB")))));
  const QJsonObject otherSite = privacyReport(site, id, QStringLiteral("readB"));
  window.navigateCurrentTab(QUrl(site.c() + QStringLiteral("/pv/check/") + id));
  const QJsonObject firstPartyView = privacyReport(site, id, QStringLiteral("check"));
  for (const QString &api : {QStringLiteral("localStorage"), QStringLiteral("indexedDB"), QStringLiteral("cacheStorage"),
                             QStringLiteral("serviceWorker")}) {
    const bool leaked = otherSite.value(api).toString() == id;
    // Partitioned (works, readable again under A) or blocked (fails).
    const QString mode = written.value(api).toString() == QLatin1String("written") && sameSite.value(api).toString() == id
                             ? QStringLiteral("partitioned") : QStringLiteral("blocked");
    std::cout << "[INFO] " << (webkit ? "webkit" : "chromium") << " third-party " << api.toStdString() << ": "
              << mode.toStdString() << " (write=" << written.value(api).toString().toStdString()
              << " same-site=" << sameSite.value(api).toString().toStdString()
              << " other-site=" << otherSite.value(api).toString().toStdString() << ")" << std::endl;
    result(QStringLiteral("privacy.storage.%1.isolated_across_sites").arg(api), leaked ? QStringLiteral("no") : QStringLiteral("yes"));
    result(QStringLiteral("privacy.storage.%1.third_party_mode").arg(api), mode);
    CHECK(!leaked);
  }
  const bool firstPartyLeak = firstPartyView.value(QStringLiteral("localStorage")).toString() == id;
  result(QStringLiteral("privacy.storage.isolated_from_first_party"), firstPartyLeak ? QStringLiteral("no") : QStringLiteral("yes"));
  CHECK(!firstPartyLeak);
  std::cout << "[INFO] " << (webkit ? "webkit" : "chromium") << " Storage Access API in a third-party frame: "
            << written.value(QStringLiteral("storageAccessApi")).toString().toStdString() << std::endl;
  if (!webkit) {
    // Chromium blocks third-party storage outright with third-party cookies;
    // WebKit partitions it. Both isolate it.
    for (const char *api : {"localStorage", "indexedDB", "cacheStorage", "serviceWorker"})
      std::cout << "[KNOWN] privacy.storage." << api << ".third_party_mode=Chromium blocks third-party storage "
                   "together with third-party cookies; WebKit partitions it by top-level site" << std::endl;
  }

  // window.name does not follow the tab to another site; same-site it stays.
  for (const auto &[nameId, next] : {std::pair{QStringLiteral("21"), site.c()}, std::pair{QStringLiteral("22"), site.a()}}) {
    const QString target = QString::fromLatin1(QUrl::toPercentEncoding(next + QStringLiteral("/pv/name/%1/get").arg(nameId)));
    window.navigateCurrentTab(QUrl(site.a() + QStringLiteral("/pv/name/%1/set?next=%2").arg(nameId, target)));
    const QString name = privacyReport(site, nameId, QStringLiteral("name")).value(QStringLiteral("name")).toString();
    const bool crossSite = next == site.c();
    result(crossSite ? QStringLiteral("privacy.window_name.cross_site") : QStringLiteral("privacy.window_name.same_site"),
           name.isEmpty() ? QStringLiteral("cleared") : QStringLiteral("kept"));
    CHECK(crossSite ? name.isEmpty() : name == QStringLiteral("wn") + nameId);
  }
  log(QStringLiteral("PASS privacy"));
}

// ---------------------------------------------------------------------------
// Case: fingerprinting protection, GPC, User-Agent, and DaliNira's scripts in
// a document reached by a page-started cross-site navigation
// ---------------------------------------------------------------------------

void runFingerprintCase(BrowserWindow &window, BrowserProfileService &profileService, LocalSite &site, bool webkit) {
  DaliNiraBlockerService *blocker = profileService.adBlockService();
  CHECK(blocker);
  // A cosmetic filter for B: DaliNira's per-site page scripts must apply
  // there however the tab got to it.
  blocker->settings()->setCustomFilters({QStringLiteral("localhost##.dn-cosmetic-probe")});
  settle(300);
  auto probe = [&](const QString &base, const QString &id, const QString &go = {}) {
    QString url = base + QStringLiteral("/fp/") + id;
    if (!go.isEmpty()) url += QStringLiteral("?go=") + QString::fromLatin1(QUrl::toPercentEncoding(go));
    window.navigateCurrentTab(QUrl(url));
    return privacyReport(site, id, QStringLiteral("fp"));
  };
  const QJsonObject a1 = probe(site.a(), QStringLiteral("a1"));
  const QJsonObject a2 = probe(site.a(), QStringLiteral("a2"));
  const QJsonObject b1 = probe(site.b(), QStringLiteral("b1"));
  // A page-started cross-site navigation: A's page itself goes to B.
  probe(site.a(), QStringLiteral("a3"), site.b() + QStringLiteral("/fp/b2"));
  const QJsonObject b2 = privacyReport(site, QStringLiteral("b2"), QStringLiteral("fp"));
  auto yesNo = [](bool value) { return value ? QStringLiteral("yes") : QStringLiteral("no"); };
  const QString ua = a1.value(QStringLiteral("ua")).toString();
  std::cout << "[INFO] " << (webkit ? "webkit" : "chromium") << " UA: " << ua.toStdString() << " | WebGL: "
            << a1.value(QStringLiteral("gl")).toString().toStdString() << " | cpu A/B: "
            << a1.value(QStringLiteral("cpu")).toInt() << "/" << b1.value(QStringLiteral("cpu")).toInt() << std::endl;
  std::cout << "[INFO] " << (webkit ? "webkit" : "chromium") << " AudioContext sums: A1="
            << a1.value(QStringLiteral("audio")).toString().toStdString() << " A2="
            << a2.value(QStringLiteral("audio")).toString().toStdString() << " B1="
            << b1.value(QStringLiteral("audio")).toString().toStdString() << std::endl;
  const bool gpcHeader = site.header(QStringLiteral("/fp/a1"), "sec-gpc") == QLatin1String("1") &&
                         site.header(QStringLiteral("/beacon/fp-a1"), "sec-gpc") == QLatin1String("1");
  const bool gpcScript = a1.value(QStringLiteral("gpc")).toBool() && b1.value(QStringLiteral("gpc")).toBool();
  const bool uaClean = !ua.contains(QLatin1String("QtWebEngine")) && !ua.contains(QLatin1String("DaliNira"));
  auto varies = [&](const char *key) {
    return a1.value(QLatin1String(key)) == a2.value(QLatin1String(key)) && a1.value(QLatin1String(key)) != b1.value(QLatin1String(key));
  };
  const QString gl = a1.value(QStringLiteral("gl")).toString();
  const bool glGeneric = !QRegularExpression(QStringLiteral("intel|mesa|nvidia|amd|radeon|geforce|llvmpipe"),
                                             QRegularExpression::CaseInsensitiveOption).match(gl).hasMatch();
  result(QStringLiteral("fp.gpc.header"), yesNo(gpcHeader));
  result(QStringLiteral("fp.gpc.navigator"), yesNo(gpcScript));
  result(QStringLiteral("fp.user_agent.no_embedder_tokens"), yesNo(uaClean));
  result(QStringLiteral("fp.canvas.per_site"), yesNo(varies("canvas")));
  result(QStringLiteral("fp.audio.per_site"), yesNo(varies("audio")));
  result(QStringLiteral("fp.webgl.renderer_generic"), yesNo(glGeneric));
  const bool languageReduced = a1.value(QStringLiteral("languages")).toArray().size() == 1 &&
                               a1.value(QStringLiteral("language")) == a1.value(QStringLiteral("intl"));
  const bool fontsReduced = a1.value(QStringLiteral("localFonts")).toInt() <= 0;
  const bool webgpuHidden = a1.value(QStringLiteral("webgpu")).toString() == QLatin1String("undefined");
  const bool webglBroader = !a1.value(QStringLiteral("glDebug")).toBool() &&
                            a1.value(QStringLiteral("glLimit")).toInt() > 0 &&
                            !a1.value(QStringLiteral("glPrecision")).toString().isEmpty();
  std::cout << "[INFO] " << (webkit ? "webkit" : "chromium") << " WebGL broader: debug="
            << a1.value(QStringLiteral("glDebug")).toBool() << " maxTexture="
            << a1.value(QStringLiteral("glLimit")).toVariant().toString().toStdString() << " precision="
            << a1.value(QStringLiteral("glPrecision")).toString().toStdString() << std::endl;
  const bool cssReduced = !a1.value(QStringLiteral("cssWide")).toBool();
  const bool hardwareReduced = a1.value(QStringLiteral("touch")).toInt() <= 1 &&
      (a1.value(QStringLiteral("platform")).toString() == QLatin1String("Linux x86_64") ||
       a1.value(QStringLiteral("platform")).toString() == QLatin1String("Win32") ||
       a1.value(QStringLiteral("platform")).toString() == QLatin1String("MacIntel"));
  const QJsonObject worker = a1.value(QStringLiteral("worker")).toObject();
  const bool workerGpc = worker.value(QStringLiteral("gpc")).toBool();
  result(QStringLiteral("fp.language_locale.reduced"), yesNo(languageReduced));
  result(QStringLiteral("fp.font_enumeration.reduced"), yesNo(fontsReduced));
  result(QStringLiteral("fp.webgpu.hidden"), yesNo(webgpuHidden));
  result(QStringLiteral("fp.webgl.additional_limits_normalized"), yesNo(webglBroader));
  result(QStringLiteral("fp.css_wide_gamut.hidden"), yesNo(cssReduced));
  result(QStringLiteral("fp.hardware_capabilities.reduced"), yesNo(hardwareReduced));
  result(QStringLiteral("fp.worker.navigator_gpc"), yesNo(workerGpc));
  if (!workerGpc)
    std::cout << "[KNOWN] fp.worker.runtime=engine document scripts do not execute in dedicated/shared/service workers; "
                 "wrapping worker URLs would change CSP, location and module/import semantics" << std::endl;
  result(QStringLiteral("fp.screen.normalized"), yesNo(b1.value(QStringLiteral("screen")).toString().startsWith(QLatin1String("1920x"))));
  result(QStringLiteral("fp.cross_site_page_navigation.shield_ran"), yesNo(b2.value(QStringLiteral("gpc")).toBool()));
  result(QStringLiteral("fp.cross_site_page_navigation.cosmetic_ran"), yesNo(b2.value(QStringLiteral("cosmeticHidden")).toBool()));
  result(QStringLiteral("fp.browser_navigation.cosmetic_ran"), yesNo(b1.value(QStringLiteral("cosmeticHidden")).toBool()));
  CHECK(gpcHeader && gpcScript && uaClean && varies("canvas") && varies("audio") && glGeneric);
  CHECK(languageReduced && fontsReduced && webgpuHidden && webglBroader && cssReduced && hardwareReduced);
  CHECK(b2.value(QStringLiteral("gpc")).toBool());
  CHECK(b1.value(QStringLiteral("cosmeticHidden")).toBool() && b2.value(QStringLiteral("cosmeticHidden")).toBool());
  log(QStringLiteral("PASS fingerprint"));
}

// ---------------------------------------------------------------------------
// Case: network state (HTTP cache and connection pools) across sites
// ---------------------------------------------------------------------------

// One third-party frame step: T embedded in `top`; waits for its report.
// How the step's top-level page is reached.
enum class Navigation { Browser, NewTab, Page };

void networkStep(BrowserWindow &window, LocalSite &site, const QString &top, const QString &id, const QString &step,
                 Navigation navigation = Navigation::Browser) {
  const QString frame = QString::fromLatin1(QUrl::toPercentEncoding(site.c() + QStringLiteral("/nc/frame/%1/%2").arg(id, step)));
  const QUrl url(top + QStringLiteral("/pv/embed/%1?frame=%2").arg(id + step, frame));
  if (navigation == Navigation::NewTab) window.addNewTab(url);
  else if (navigation == Navigation::Page)
    evaluate(currentPage(window), QStringLiteral("location.assign('%1')").arg(url.toString()), true);
  else window.navigateCurrentTab(url);
  CHECK(waitUntil([&] { return site.hit(QStringLiteral("/r/%1/%2").arg(id, step)); }, 15000));
}

// Requests for the third party's cacheable resources so far.
int resourceFetches(LocalSite &site, const QString &id, const QStringList &kinds) {
  int total = 0;
  for (const QString &kind : kinds) total += site.count(QStringLiteral("/nc/res/%1/%2").arg(kind, id));
  return total;
}

QSet<quint16> connectionPorts(LocalSite &site, const QString &id, const QString &step) {
  QSet<quint16> ports;
  for (int n = 0; n < 3; ++n) ports.unite(site.ports(QStringLiteral("/nc/conn/%1/%2/%3").arg(id, step).arg(n)));
  return ports;
}

void runNetworkCase(BrowserWindow &window, LocalSite &site, bool webkit) {
  // T = 127.0.0.2 embedded in A = 127.0.0.1, A again (the same partition),
  // then B = localhost: B must not get A's cached copies or connections.
  const QStringList subresources = {QStringLiteral("img"), QStringLiteral("css"), QStringLiteral("js")};
  const QStringList fetchOnly = {QStringLiteral("fetch")};
  auto run = [&](const QString &id, Navigation navigation, int *controlFetches, int *otherFetch, int *otherSub,
                 bool *sharedConnection) {
    networkStep(window, site, site.a(), id, QStringLiteral("s1"));
    settle(2000);  // WebKit writes its disk cache asynchronously
    const int primedFetch = resourceFetches(site, id, fetchOnly), primedSub = resourceFetches(site, id, subresources);
    networkStep(window, site, site.a(), id, QStringLiteral("s2"));
    *controlFetches = resourceFetches(site, id, fetchOnly) + resourceFetches(site, id, subresources) - primedFetch - primedSub;
    settle(2000);
    const int beforeFetch = resourceFetches(site, id, fetchOnly), beforeSub = resourceFetches(site, id, subresources);
    networkStep(window, site, site.b(), id, QStringLiteral("s3"), navigation);
    *otherFetch = resourceFetches(site, id, fetchOnly) - beforeFetch;
    *otherSub = resourceFetches(site, id, subresources) - beforeSub;
    const QSet<quint16> before = connectionPorts(site, id, QStringLiteral("s1")) + connectionPorts(site, id, QStringLiteral("s2"));
    *sharedConnection = connectionPorts(site, id, QStringLiteral("s3")).intersects(before);
  };
  // B reached by the browser (address bar), in a new tab, and by the page
  // itself (a link or script on A).
  const QList<std::pair<Navigation, QString>> ways = {
      {Navigation::Browser, QStringLiteral("browser_navigation")}, {Navigation::NewTab, QStringLiteral("new_tab")},
      {Navigation::Page, QStringLiteral("page_navigation")}};
  auto yesNo = [](bool value) { return value ? QStringLiteral("yes") : QStringLiteral("no"); };
  const char *engine = webkit ? "webkit" : "chromium";
  bool fetchPartitioned = true, connectionsPartitioned = true, pageSubresourcesPartitioned = true,
       newTabSubresourcesPartitioned = true;
  for (int i = 0; i < ways.size(); ++i) {
    int control = 0, otherFetch = 0, otherSub = 0;
    bool sharedConnection = false;
    run(QStringLiteral("x%1").arg(i + 1), ways.at(i).first, &control, &otherFetch, &otherSub, &sharedConnection);
    const QString &way = ways.at(i).second;
    std::cout << "[INFO] " << engine << " " << way.toStdString() << ": control refetches=" << control
              << " other-site fetch()=" << otherFetch << " img/css/js=" << otherSub << "/3 shared connection="
              << sharedConnection << std::endl;
    result(QStringLiteral("network.http_cache.subresources_%1.partitioned").arg(way), yesNo(otherSub == 3));
    fetchPartitioned = fetchPartitioned && otherFetch == 1;
    connectionsPartitioned = connectionsPartitioned && !sharedConnection;
    if (ways.at(i).first == Navigation::Page) pageSubresourcesPartitioned = otherSub == 3;
    if (ways.at(i).first == Navigation::NewTab) newTabSubresourcesPartitioned = otherSub == 3;
    if (!webkit) {
      CHECK(control == 0);  // the same partition still uses its cache
      if (ways.at(i).first == Navigation::Browser) CHECK(otherSub == 3);
    }
  }
  result(QStringLiteral("network.http_cache.fetch.partitioned"), yesNo(fetchPartitioned));
  result(QStringLiteral("network.connections.partitioned"), yesNo(connectionsPartitioned));
  if (!webkit) {
    CHECK(fetchPartitioned && connectionsPartitioned);
    if (!pageSubresourcesPartitioned)
      std::cout << "[KNOWN] network.http_cache.subresources_page_navigation.partitioned=Chromium: on a page-started "
                   "cross-site navigation the tab keeps its browsing context group, so the third party's frame "
                   "reuses its renderer process and Blink's in-memory resource cache (keyed by URL only); its disk "
                   "cache is partitioned" << std::endl;
    if (!newTabSubresourcesPartitioned)
      std::cout << "[KNOWN] network.http_cache.subresources_new_tab.partitioned=Chromium: with --site-per-process "
                   "the third party's frame in the new tab reuses the renderer process it already has in the "
                   "other open tab, and Blink's in-memory resource cache (keyed by URL only) serves it; its disk "
                   "cache is partitioned (fetch() above)" << std::endl;
  } else {
    for (const char *name : {"fetch", "subresources_browser_navigation", "subresources_new_tab",
                             "subresources_page_navigation"})
      std::cout << "[KNOWN] network.http_cache." << name << ".partitioned=WebKitGTK has no cache partitioning: one "
                   "network cache for all sites (no API; its cache partition key is always empty)" << std::endl;
    std::cout << "[KNOWN] network.connections.partitioned=WebKitGTK (libsoup) has one connection pool for all sites, "
                 "with no API to partition it" << std::endl;
  }
  log(QStringLiteral("PASS network"));
}

// ---------------------------------------------------------------------------
// Case: fullscreen media
// ---------------------------------------------------------------------------

double currentTime(QWidget *page) { return evaluate(page, QStringLiteral("v.currentTime")).toDouble(); }

bool playing(QWidget *page) {
  const double before = currentTime(page);
  settle(600);
  const double after = currentTime(page);
  const bool advancing = after > before + 0.2 || (after < before && after > 0.0);  // advancing, or rewound
  if (!advancing) {
    const int framesBefore = evaluate(page, QStringLiteral("window.__frames")).toInt();
    settle(500);
    const int frames = evaluate(page, QStringLiteral("window.__frames")).toInt() - framesBefore;
    log(QStringLiteral("[DIAG] rAF frames in 500 ms: %1").arg(frames));
  }
  if (!advancing)
    log(QStringLiteral("[DIAG] not advancing %1 -> %2: %3").arg(before).arg(after).arg(text(page, QStringLiteral(
        "JSON.stringify({paused:v.paused,ended:v.ended,ready:v.readyState,net:v.networkState,"
        "err:v.error&&v.error.code,hidden:document.hidden,vis:document.visibilityState,w:innerWidth,h:innerHeight})"))));
  return advancing;
}

// `element`: the id of the element the page made fullscreen.
bool inPageFullscreen(QWidget *page, const QString &element = QStringLiteral("v")) {
  return text(page, QStringLiteral("String(document.fullscreenElement&&document.fullscreenElement.id)")) == element;
}

// The Fullscreen API needs a user activation. Tries the page script first,
// then a real click on the page's fullscreen button (Chromium: input events
// to its render widget, which are user input to the page).
bool requestFullscreen(QWidget *page, const QString &button = QStringLiteral("fs"), int buttonY = 330) {
  evaluate(page, QStringLiteral("window.__fs='';document.getElementById('%1').click()").arg(button));
  if (waitUntil([&] { return text(page, QStringLiteral("window.__fs")) == QLatin1String("ok"); }, 2000)) return true;
  if (auto *webView = qobject_cast<QWebEngineView *>(page)) {
    QWidget *target = webView->focusProxy() ? webView->focusProxy() : webView;
    const QPoint at(100, buttonY);
    QMouseEvent press(QEvent::MouseButtonPress, at, target->mapToGlobal(at), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, at, target->mapToGlobal(at), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(target, &press);
    QApplication::sendEvent(target, &release);
    return waitUntil([&] { return text(page, QStringLiteral("window.__fs")) == QLatin1String("ok"); }, 4000);
  }
  return false;
}

void runMediaCase(BrowserWindow &window, LocalSite &site, bool webkit) {
  Q_UNUSED(webkit);
  auto *omnibox = window.findChild<QLineEdit *>(QStringLiteral("omnibox"));
  CHECK(omnibox);
  window.navigateCurrentTab(QUrl(site.a() + QStringLiteral("/media")));
  CHECK(tabReady(window, 0, QStringLiteral("Media")));
  QWidget *page = pageWidget(window, 0);
  CHECK(waitUntil([&] { return currentTime(page) > 0.3; }, 15000));
  result(QStringLiteral("media.playing"), playing(page));
  // The page's rewind (a seek) keeps playback going, without fullscreen.
  result(QStringLiteral("media.control.rewinds_and_plays"),
         waitUntil([&] { return currentTime(page) > 2.0; }, 6000) &&
             waitUntil([&] { return currentTime(page) < 1.0; }, 4000) && playing(page));

  // Control without fullscreen: grow, then shrink the window while playing.
  window.resize(1500, 950);
  waitUntil([&] { return page->width() > 1300; }, 5000);
  result(QStringLiteral("media.control.grow_playing"), playing(page));
  window.resize(1000, 700);
  waitUntil([&] { return page->width() < 1100; }, 5000);
  result(QStringLiteral("media.control.shrink_playing"), playing(page));

  const QRect normalGeometry = window.geometry();
  // element: "v", the <video> itself (native controls' fullscreen button), or
  // "box", its player container (YouTube's player).
  const auto enter = [&](const QString &name, const QString &element = QStringLiteral("v")) {
    if (element == QLatin1String("box")) CHECK(requestFullscreen(page, QStringLiteral("fsbox"), 400));
    else CHECK(requestFullscreen(page));
    CHECK(waitUntil([&] { return window.isFullScreen(); }, 5000));
    CHECK(waitUntil([&] { return page->width() >= window.screen()->geometry().width() - 2; }, 5000));
    result(name + QStringLiteral(".page_fullscreen"), inPageFullscreen(page, element));
    result(name + QStringLiteral(".chrome_hidden"), !omnibox->isVisible());
    result(name + QStringLiteral(".viewport_follows"),
           waitUntil([&] { return qAbs(evaluate(page, QStringLiteral("window.innerWidth")).toInt() - page->width()) <= 2; }, 5000));
    if (element == QLatin1String("v")) {
      // WebKitGTK 2.54.1 (upstream regression, also in plain WebKitGTK on X11
      // and Wayland): a <video> that is itself the fullscreen element stops
      // advancing until fullscreen ends. Container fullscreen is unaffected
      // and must keep playing (media.fs_container below).
      knownDifference(name + QStringLiteral(".playing"), playing(page),
                      QStringLiteral("WebKitGTK 2.54.1: <video> element fullscreen stops advancing (upstream)"));
    } else {
      result(name + QStringLiteral(".playing"), playing(page));
    }
  };
  const auto left = [&](const QString &name, bool maximized, QSize normalSize = QSize()) {
    if (!normalSize.isValid()) normalSize = normalGeometry.size();
    CHECK(waitUntil([&] { return !window.isFullScreen(); }, 5000));
    result(name + QStringLiteral(".page_left_fullscreen"),
           waitUntil([&] { return text(page, QStringLiteral("String(document.fullscreenElement)")) == QLatin1String("null"); }, 3000));
    result(name + QStringLiteral(".chrome_visible"), waitUntil([&] { return omnibox->isVisible(); }, 3000));
    if (maximized) result(name + QStringLiteral(".restored_maximized"), waitUntil([&] { return window.isMaximized(); }, 5000));
    else {
      const bool restored = waitUntil([&] { return window.size() == normalSize; }, 5000);
      if (!restored) {
        log(QStringLiteral("[DIAG] window %1x%2 after fullscreen, expected %3x%4")
                .arg(window.width()).arg(window.height()).arg(normalSize.width()).arg(normalSize.height()));
      }
      result(name + QStringLiteral(".restored_size"), restored);
    }
    // Playback goes on once fullscreen ends, on both engines (WebKitGTK 2.52.6
    // used to stall here; 2.54.1 resumes).
    result(name + QStringLiteral(".playing"), playing(page));
    page->setFocus(Qt::MouseFocusReason);
    result(name + QStringLiteral(".page_focus"), waitUntil([&] {
      QWidget *focus = QApplication::focusWidget();
      return focus && (focus == page || page->isAncestorOf(focus));
    }, 3000));
  };

  // Enter, then leave with Escape (the browser's shortcut).
  enter(QStringLiteral("media.fs1"));
  QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
  QApplication::sendEvent(&window, &escape);
  left(QStringLiteral("media.fs1.escape"), false);

  // Enter, then the page leaves (e.g. its own exit button).
  enter(QStringLiteral("media.fs2"));
  evaluate(page, QStringLiteral("document.exitFullscreen()"));
  left(QStringLiteral("media.fs2.page_exit"), false);

  // From a maximized window, back to maximized.
  window.showMaximized();
  CHECK(waitUntil([&] { return window.isMaximized(); }, 5000));
  enter(QStringLiteral("media.fs3"));
  evaluate(page, QStringLiteral("document.exitFullscreen()"));
  left(QStringLiteral("media.fs3.from_maximized"), true);
  window.showNormal();
  CHECK(waitUntil([&] { return !window.isMaximized(); }, 5000));

  // The player container fullscreen, as YouTube does: plays on both engines.
  // Let the window settle back to its normal size first (it reports the
  // maximized size for a moment after showNormal()).
  if (!waitUntil([&] { return window.size() == normalGeometry.size(); }, 5000)) {
    log(QStringLiteral("[DIAG] window %1x%2 after showNormal(), normal %3x%4")
            .arg(window.width()).arg(window.height())
            .arg(normalGeometry.width()).arg(normalGeometry.height()));
  }
  const QSize beforeContainer = window.size();
  enter(QStringLiteral("media.fs_container"), QStringLiteral("box"));
  evaluate(page, QStringLiteral("document.exitFullscreen()"));
  left(QStringLiteral("media.fs_container.page_exit"), false, beforeContainer);

  // Closing the tab that is fullscreen gives the window back.
  window.addNewTab(QUrl(site.a() + QStringLiteral("/media")));
  CHECK(tabReady(window, 1, QStringLiteral("Media")));
  page = pageWidget(window, 1);
  CHECK(waitUntil([&] { return currentTime(page) > 0.3; }, 15000));
  enter(QStringLiteral("media.fs4"));
  window.closeTab(1);
  CHECK(waitUntil([&] { return window.tabCount() == 1; }, 5000));
  result(QStringLiteral("media.fs4.close_tab.window_left_fullscreen"), waitUntil([&] { return !window.isFullScreen(); }, 5000));
  result(QStringLiteral("media.fs4.close_tab.chrome_visible"), waitUntil([&] { return omnibox->isVisible(); }, 3000));
  log(QStringLiteral("PASS fullscreen media"));
}

// ---------------------------------------------------------------------------
// Case: onboarding and browser layouts
// ---------------------------------------------------------------------------

using dalinira::layout::BrowserLayoutMode;
using dalinira::layout::BrowserSidePanel;
using dalinira::onboarding::OnboardingManager;
using dalinira::onboarding::OnboardingView;

int internalTab(BrowserWindow &window, const QString &id) {
  for (int i = 0; i < window.tabCount(); ++i)
    if (window.tabInfo(i).isInternal && window.tabInfo(i).internalId == id) return i;
  return -1;
}

template <typename T>
T *internalPage(BrowserWindow &window, const QString &id) {
  const int index = internalTab(window, id);
  return index < 0 ? nullptr : qobject_cast<T *>(window.tabInfo(index).content.data());
}

QWidget *tabContent(BrowserWindow &window, int index) {
  const BrowserTabInfo &info = window.tabInfo(index);
  return info.view ? static_cast<QWidget *>(info.view.data()) : info.content.data();
}

BrowserSidePanel *sidePanel(BrowserWindow &window) {
  return window.findChild<BrowserSidePanel *>(QStringLiteral("browserSidePanel"));
}
QWidget *bodyContainer(BrowserWindow &window) { return window.findChild<QWidget *>(QStringLiteral("bodyContainer")); }
QStackedWidget *pageStack(BrowserWindow &window) { return window.findChild<QStackedWidget *>(QStringLiteral("pageStack")); }

template <typename T>
T *child(QWidget *parent, const QString &name) {
  return parent ? parent->findChild<T *>(name) : nullptr;
}

// What the window shows: "classic", "left", "right", "floating-hidden",
// "floating-shown" (side panel geometry against the page area).
QString layoutState(BrowserWindow &window) {
  BrowserSidePanel *panel = sidePanel(window);
  QStackedWidget *stack = pageStack(window);
  const bool strip = window.tabStrip() && window.tabStrip()->isVisible();
  if (!panel || !stack) return QStringLiteral("broken");
  const auto *controller = window.layoutController();
  const bool floating = controller && controller->isFloatingPanelEnabled();
  // Auto-hide (docked layouts): "collapsed" (page full width) or "revealed"
  // (over the page); the floating layout keeps its own names.
  if (!panel->isVisible())
    return strip ? QStringLiteral("classic") : floating ? QStringLiteral("floating-hidden") : QStringLiteral("collapsed");
  if (strip) return QStringLiteral("panel+strip");
  if (!floating && controller && controller->sidePanelState() == dalinira::layout::SidePanelState::Revealed)
    return QStringLiteral("revealed");
  const QRect p = panel->geometry();
  const QRect s = stack->geometry();
  if (p.right() < 0) return QStringLiteral("floating-hidden");
  if (p.intersects(s)) return QStringLiteral("floating-shown");
  if (p.right() < s.left()) return QStringLiteral("left");
  if (p.left() > s.right()) return QStringLiteral("right");
  return QStringLiteral("unknown");
}

// The side panel, tab strip and page stack all list the window's tabs in
// the model's order.
bool tabViewsInSync(BrowserWindow &window) {
  BrowserSidePanel *panel = sidePanel(window);
  QStackedWidget *stack = pageStack(window);
  if (!panel || !stack || panel->tabCount() != window.tabCount() || window.tabStrip()->count() != window.tabCount())
    return false;
  for (int i = 0; i < window.tabCount(); ++i) {
    const BrowserTabInfo &info = window.tabInfo(i);
    if (window.tabStrip()->tabId(i) != info.id) return false;
    if (stack->indexOf(tabContent(window, i)) != i) return false;
    if (!info.title.isEmpty() && panel->tabTitle(i) != info.title) return false;
  }
  return true;
}

struct LivePage {
  QPointer<QWidget> widget;
  QString id;
};

int indexOfPage(BrowserWindow &window, QWidget *page) {
  for (int i = 0; i < window.tabCount(); ++i)
    if (pageWidget(window, i) == page) return i;
  return -1;
}

// The same page object, the same document (no reload), loaded once.
bool pageIntact(BrowserWindow &window, const LivePage &page, LocalSite &site, int maxLoads = 1) {
  if (!page.widget || indexOfPage(window, page.widget) < 0) return false;
  return text(page.widget, QStringLiteral("window.__id")) == page.id &&
         text(page.widget, QStringLiteral("sessionStorage.getItem('loads')")) == QLatin1String("1") &&
         site.count(QStringLiteral("/layout")) <= maxLoads;
}

bool pageLoadedOnce(BrowserWindow &window, const LivePage &page) {
  if (!page.widget || indexOfPage(window, page.widget) < 0) return false;
  return !page.id.isEmpty() &&
         text(page.widget, QStringLiteral("window.__id")) == page.id &&
         text(page.widget, QStringLiteral("sessionStorage.getItem('loads')")) == QLatin1String("1");
}

// How the floating panel is stacked against the page area. Qt paints alien
// siblings itself, in their z-order. A native page area (WebKit's XEmbed host
// makes the page stack native) is an X window that only a native panel above
// it in the X stacking order covers. (Screens cannot be read back here: an
// XWayland window grab is black.)
QString panelStacking(QWidget *panel, QWidget *page, QWidget *body) {
  if (!page->internalWinId())
    return body->children().indexOf(panel) > body->children().indexOf(page) ? QStringLiteral("over")
                                                                            : QStringLiteral("under");
  if (!panel->internalWinId()) return QStringLiteral("under");
#if defined(DALINIRA_BUILD_WEBKIT)
  auto *x11 = qApp->nativeInterface<QNativeInterface::QX11Application>();
  if (!x11 || !body->internalWinId()) return QStringLiteral("unknown");
  Window root = 0, parent = 0, *children = nullptr;
  unsigned int count = 0;
  if (!XQueryTree(x11->display(), static_cast<Window>(body->internalWinId()), &root, &parent, &children, &count))
    return QStringLiteral("unknown");
  int panelAt = -1, pageAt = -1;  // bottom-to-top
  for (unsigned int i = 0; i < count; ++i) {
    if (children[i] == static_cast<Window>(panel->internalWinId())) panelAt = int(i);
    if (children[i] == static_cast<Window>(page->internalWinId())) pageAt = int(i);
  }
  if (children) XFree(children);
  if (panelAt < 0 || pageAt < 0) return QStringLiteral("unknown");
  return panelAt > pageAt ? QStringLiteral("over") : QStringLiteral("under");
#else
  return QStringLiteral("unknown");
#endif
}

void hoverBody(BrowserWindow &window, int x) {
  QWidget *body = bodyContainer(window);
  const QPoint local(x, body->height() / 2);
  QMouseEvent move(QEvent::MouseMove, QPointF(local), QPointF(body->mapToGlobal(local)), Qt::NoButton,
                   Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(body, &move);
}

int bookmarkCount(BrowserProfileService &profile, const QUrl &url) {
  int count = 0;
  for (const auto &item : profile.bookmarkItems())
    if (item.url == url) ++count;
  return count;
}

bool blockerOn(BrowserProfileService &profile) {
  return profile.blockerService() && profile.blockerService()->settings()->protectionEnabled();
}

bool lightChrome(BrowserWindow &window) { return window.styleSheet().contains(QStringLiteral("#eef2f7")); }

bool clickButton(QWidget *parent, const QString &name) {
  auto *button = child<QAbstractButton>(parent, name);
  if (!button) return false;
  button->click();
  return true;
}

bool realPointerAction(QWidget *target, int xButton, int repeat = 1) {
#if defined(DALINIRA_TEST_XTEST)
  if (!target || !target->isVisible()) return false;
  auto *x11 = qApp->nativeInterface<QNativeInterface::QX11Application>();
  if (!x11 || !x11->display()) return false;
  Display *display = x11->display();
  int eventBase = 0, errorBase = 0, major = 0, minor = 0;
  if (!XTestQueryExtension(display, &eventBase, &errorBase, &major, &minor)) return false;
  const QPoint global = target->mapToGlobal(QPoint(qMax(10, target->width() / 3),
                                                   qMax(10, target->height() / 2)));
  XTestFakeMotionEvent(display, DefaultScreen(display), global.x(), global.y(), CurrentTime);
  XSync(display, False);
  settle(100);
  for (int i = 0; i < repeat; ++i) {
    XTestFakeButtonEvent(display, static_cast<unsigned int>(xButton), True, CurrentTime);
    XTestFakeButtonEvent(display, static_cast<unsigned int>(xButton), False, CurrentTime);
  }
  XSync(display, False);
  settle(150);
  return true;
#else
  Q_UNUSED(target);
  Q_UNUSED(xButton);
  Q_UNUSED(repeat);
  return false;
#endif
}

bool pageWheelAction(QWidget *page, bool webkit, int repeat = 3) {
  Q_UNUSED(webkit);
  return realPointerAction(page, 5, repeat);
}

bool pageClickAction(QWidget *page, bool webkit) {
  Q_UNUSED(webkit);
  return realPointerAction(page, 1, 1);
}

void runAboutLinksCase(BrowserWindow &window, bool webkit) {
  // A New Tab page searches with the profile's engine, as the omnibox does
  // (WebKit's once read another settings store and showed Google).
  window.setSearchEngine(QStringLiteral("Startpage"));
  window.addNewTab();
  QWidget *newTab = currentPage(window);
  CHECK(newTab);
  const auto placeholder = [&] {
    return evaluate(newTab, QStringLiteral("(document.querySelector('#query')||{}).placeholder||''")).toString();
  };
  result(QStringLiteral("newtab.search_engine_matches_profile"),
         waitUntil([&] { return placeholder().contains(QLatin1String("Startpage")); }, 10000)
             && window.currentSearchEngine() == QLatin1String("Startpage"));

  window.showSettings(SettingsPage::Category::About);
  auto *settingsPage = internalPage<SettingsPage>(window, QStringLiteral("settings"));
  CHECK(settingsPage);
  // The details name the product and the engine this session runs on.
  auto *aboutDetails = settingsPage->findChild<QLabel *>(QStringLiteral("settings-about-details"));
  CHECK(aboutDetails);
  const QString aboutText = aboutDetails->text();
  result(QStringLiteral("about.product_name"), aboutText.contains(QLatin1String("DaliNira Browser")));
  result(QStringLiteral("about.names_running_engine"),
         webkit ? aboutText.contains(QLatin1String("WebKitGTK ")) && !aboutText.contains(QLatin1String("Chromium"))
                : aboutText.contains(QLatin1String("Chromium")) && !aboutText.contains(QLatin1String("WebKit")));
  const int settingsIndex = internalTab(window, QStringLiteral("settings"));
  CHECK(settingsIndex >= 0);
  // Keep this repeatable and offline: exercise the real controls and the
  // SettingsPage bridge, but record its output before BrowserWindow starts a
  // GitHub network load. The live smoke run covers the production addNewTab
  // connection; the exact same native page is used by both engines.
  QObject::disconnect(settingsPage, &SettingsPage::navigateRequested, &window, nullptr);
  QList<QUrl> requested;
  QObject::connect(settingsPage, &SettingsPage::navigateRequested, &window,
                   [&requested](const QUrl &url) { requested.append(url); });

  const auto activate = [&](const QString &name, const QUrl &target,
                            const std::function<bool()> &trigger) {
    window.switchTab(settingsIndex);
    settle(100);
    const int before = requested.size();
    const bool triggered = trigger();
    const bool forwarded = triggered && requested.size() == before + 1;
    result(QStringLiteral("about.%1.responds").arg(name), forwarded);
    const bool exact = forwarded && requested.constLast() == target;
    result(QStringLiteral("about.%1.url").arg(name), exact);
  };

  const QUrl profile(QStringLiteral("https://github.com/Muhammed-Dali"));
  const QUrl repository(QStringLiteral("https://github.com/Muhammed-Dali/DaliNira-Browser"));
  const QUrl bug(QStringLiteral(
      "https://github.com/Muhammed-Dali/DaliNira-Browser/issues/new?template=bug_report.yml"));
  const QUrl feature(QStringLiteral(
      "https://github.com/Muhammed-Dali/DaliNira-Browser/issues/new?template=feature_request.yml"));

  activate(QStringLiteral("profile_button"), profile, [&] {
    return clickButton(settingsPage, QStringLiteral("settings-about-github-profile"));
  });
  activate(QStringLiteral("repository_button"), repository, [&] {
    return clickButton(settingsPage, QStringLiteral("settings-about-github-repository"));
  });
  activate(QStringLiteral("bug_button"), bug, [&] {
    return clickButton(settingsPage, QStringLiteral("settings-feedback-bug-report"));
  });
  activate(QStringLiteral("feature_button"), feature, [&] {
    return clickButton(settingsPage, QStringLiteral("settings-feedback-feature-request"));
  });

  // Send Email hands a mailto: draft to the desktop mail client; record it
  // instead of launching one. Mouse and keyboard activation must both work.
  QObject::disconnect(settingsPage, &SettingsPage::emailRequested, &window, nullptr);
  QList<QUrl> drafts;
  QObject::connect(settingsPage, &SettingsPage::emailRequested, &window,
                   [&drafts](const QUrl &url) { drafts.append(url); });
  auto *sendEmail = child<QPushButton>(settingsPage, QStringLiteral("settings-feedback-send-email"));
  CHECK(sendEmail);
  window.switchTab(settingsIndex);
  settle(100);
  result(QStringLiteral("about.email_button.accessible"),
         sendEmail->isVisible() && !sendEmail->icon().isNull()
             && !sendEmail->accessibleName().trimmed().isEmpty()
             && (sendEmail->focusPolicy() & Qt::TabFocus));
  const auto checkDraft = [&](const QString &name, const std::function<void()> &trigger) {
    const int before = drafts.size();
    const int tabsBefore = window.tabCount();
    trigger();
    settle(50);
    const bool once = drafts.size() == before + 1 && window.tabCount() == tabsBefore;
    result(QStringLiteral("about.email_button.%1").arg(name), once);
    if (!once) return;
    const QUrl draft = drafts.constLast();
    const QUrlQuery query(draft);
    const QString bodyText = query.queryItemValue(QStringLiteral("body"), QUrl::FullyDecoded);
    result(QStringLiteral("about.email_button.%1.draft").arg(name),
           draft.scheme() == QLatin1String("mailto")
               && draft.path() == dalinira::about_links::feedbackEmailAddress()
               && query.queryItems().size() == 2
               && !query.queryItemValue(QStringLiteral("subject")).isEmpty()
               && bodyText.contains(QStringLiteral(DALINIRA_BROWSER_VERSION))
               && bodyText.contains(webkit ? QLatin1String("WebKitGTK") : QLatin1String("Chromium"))
               && !bodyText.contains(QLatin1String("http")));
  };
  checkDraft(QStringLiteral("mouse"), [&] { sendEmail->click(); });
  checkDraft(QStringLiteral("keyboard"), [&] {
    sendEmail->setFocus(Qt::TabFocusReason);
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier, QStringLiteral(" "));
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier, QStringLiteral(" "));
    QCoreApplication::sendEvent(sendEmail, &press);
    QCoreApplication::sendEvent(sendEmail, &release);
  });

  auto *details = child<QLabel>(settingsPage, QStringLiteral("settings-about-details"));
  CHECK(details && !details->openExternalLinks());
  activate(QStringLiteral("profile_html_link"), profile, [&] {
    return QMetaObject::invokeMethod(details, "linkActivated", Qt::DirectConnection,
                                     Q_ARG(QString, profile.toString()));
  });
  activate(QStringLiteral("repository_html_link"), repository, [&] {
    return QMetaObject::invokeMethod(details, "linkActivated", Qt::DirectConnection,
                                     Q_ARG(QString, repository.toString()));
  });

  CHECK(!g_expectFailed);
  log(QStringLiteral("PASS About external links"));
}

void runTranslationPopupCase(BrowserWindow &window, LocalSite &site, bool webkit) {
  window.navigateCurrentTab(QUrl(site.a() + QStringLiteral("/translate-scroll")));
  CHECK(tabReady(window, currentIndex(window), QStringLiteral("Translate Scroll")));
  QWidget *page = currentPage(window);
  CHECK(page);
  evaluate(page, QStringLiteral("scrollTo(0,0);window.__outsideClicks=0"));

  // WebKit's foreign X11 surface accepts XTest input directly. Qt WebEngine
  // 6.11's embedded QQuickWindow intentionally rejects test-synthesized DOM
  // input, so Chromium is guarded below by the popup grab/focus assertions
  // plus a live page-state check while the popup remains open.
  if (webkit) {
    CHECK(pageWheelAction(page, webkit, 3));
    CHECK(waitUntil([&] { return evaluate(page, QStringLiteral("window.scrollY")).toDouble() > 20.0; }, 5000));
  }
  evaluate(page, QStringLiteral("scrollTo(0,0);window.__outsideClicks=0"));

  window.showTranslatePopup();
  auto *popup = window.findChild<TranslateBubblePopup *>();
  CHECK(popup && waitUntil([&] { return popup->isVisible(); }, 3000));
  expect(QStringLiteral("translate_popup.non_modal"),
         popup->windowModality() == Qt::NonModal
             && (popup->windowFlags() & Qt::WindowType_Mask) == Qt::Tool);
  expect(QStringLiteral("translate_popup.no_global_grab"),
         QApplication::activePopupWidget() != popup && QWidget::mouseGrabber() != popup);
  expect(QStringLiteral("translate_popup.browser_keeps_focus"),
         QApplication::activeWindow() == &window || window.isActiveWindow());

  const double before = evaluate(page, QStringLiteral("window.scrollY")).toDouble();
  if (webkit) CHECK(pageWheelAction(page, webkit, 6));
  else evaluate(page, QStringLiteral("scrollBy(0,400)"));
  const bool scrolled = waitUntil(
      [&] { return evaluate(page, QStringLiteral("window.scrollY")).toDouble() > before + 20.0; }, 5000);
  expect(QStringLiteral("translate_popup.page_scrolls_while_visible"), scrolled);
  expect(QStringLiteral("translate_popup.visible_after_wheel"), popup->isVisible());

  const int clicksBefore = evaluate(page, QStringLiteral("window.__outsideClicks")).toInt();
  if (webkit) CHECK(pageClickAction(page, webkit));
  else evaluate(page, QStringLiteral("document.body.click()"));
  const bool outsideClicked = waitUntil(
      [&] { return evaluate(page, QStringLiteral("window.__outsideClicks")).toInt() > clicksBefore; }, 3000);
  expect(QStringLiteral("translate_popup.outside_page_clicks"), outsideClicked);
  expect(QStringLiteral("translate_popup.visible_after_outside_click"), popup->isVisible());

  auto *translate = child<QAbstractButton>(popup, QStringLiteral("target-lang-btn"));
  auto *close = child<QAbstractButton>(popup, QStringLiteral("close-btn"));
  CHECK(translate && close);
  bool translateClicked = false;
  QObject::connect(translate, &QAbstractButton::clicked, popup,
                   [&translateClicked] { translateClicked = true; });
  CHECK(realPointerAction(translate, 1));
  expect(QStringLiteral("translate_popup.translate_clickable"),
         waitUntil([&] { return translateClicked; }, 3000));
  expect(QStringLiteral("translate_popup.visible_after_translate"), popup->isVisible());

  CHECK(realPointerAction(close, 1));
  expect(QStringLiteral("translate_popup.close_clickable"),
         waitUntil([&] { return !popup->isVisible(); }, 3000));

  CHECK(!g_expectFailed);
  log(QStringLiteral("PASS translation popup interaction"));
}

// Whether a popup that is a direct child of the window is stacked above the
// page area. Alien siblings are painted by Qt in children() order; a native
// (WebKit) page area is an X window only a native popup raised above it covers.
QString overlayStacking(QWidget *overlay, QWidget *page) {
  QWidget *parent = overlay->parentWidget();
  QWidget *pageBranch = page;
  while (pageBranch && pageBranch->parentWidget() != parent) pageBranch = pageBranch->parentWidget();
  if (!parent || !pageBranch) return QStringLiteral("unknown");
  if (!pageBranch->internalWinId())
    return parent->children().indexOf(overlay) > parent->children().indexOf(pageBranch) ? QStringLiteral("over")
                                                                                        : QStringLiteral("under");
  if (!overlay->internalWinId()) return QStringLiteral("under");
#if defined(DALINIRA_BUILD_WEBKIT)
  auto *x11 = qApp->nativeInterface<QNativeInterface::QX11Application>();
  if (!x11 || !parent->internalWinId()) return QStringLiteral("unknown");
  Window root = 0, xparent = 0, *children = nullptr;
  unsigned int count = 0;
  if (!XQueryTree(x11->display(), static_cast<Window>(parent->internalWinId()), &root, &xparent, &children, &count))
    return QStringLiteral("unknown");
  int overlayAt = -1, pageAt = -1;  // bottom-to-top
  for (unsigned int i = 0; i < count; ++i) {
    if (children[i] == static_cast<Window>(overlay->internalWinId())) overlayAt = int(i);
    if (children[i] == static_cast<Window>(pageBranch->internalWinId())) pageAt = int(i);
  }
  if (children) XFree(children);
  if (overlayAt < 0 || pageAt < 0) return QStringLiteral("unknown");
  return overlayAt > pageAt ? QStringLiteral("over") : QStringLiteral("under");
#else
  return QStringLiteral("unknown");
#endif
}

QShortcut *shortcutFor(BrowserWindow &window, const QKeySequence &key) {
  for (QShortcut *shortcut : window.findChildren<QShortcut *>()) {
    if (shortcut->keys().contains(key)) return shortcut;
  }
  return nullptr;
}

void runFindInPageCase(BrowserWindow &window, LocalSite &site, bool webkit) {
  Q_UNUSED(webkit);
  window.navigateCurrentTab(QUrl(site.a() + QStringLiteral("/find")));
  CHECK(tabReady(window, currentIndex(window), QStringLiteral("Find Fixture")));
  // The window title (taskbar, window switcher) follows the page on both engines.
  result(QStringLiteral("window.title_follows_page"),
         waitUntil([&] { return window.windowTitle().startsWith(QLatin1String("Find Fixture")); }, 5000));

  auto *toolbarButton = child<QToolButton>(&window, QStringLiteral("findInPageButton"));
  CHECK(toolbarButton && !toolbarButton->icon().isNull());
  result(QStringLiteral("find.toolbar_button_exists"), !toolbarButton->toolTip().isEmpty());

  QShortcut *findShortcut = shortcutFor(window, QKeySequence(QKeySequence::Find));
  QShortcut *findNextShortcut = shortcutFor(window, QKeySequence(Qt::Key_F3));
  QShortcut *findPreviousShortcut = shortcutFor(window, QKeySequence(Qt::SHIFT | Qt::Key_F3));
  CHECK(findShortcut && findNextShortcut && findPreviousShortcut);
  result(QStringLiteral("find.ctrl_g_bound"),
         findNextShortcut->keys().contains(QKeySequence(Qt::CTRL | Qt::Key_G))
             && findPreviousShortcut->keys().contains(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_G)));
  QMetaObject::invokeMethod(findShortcut, "activated", Qt::DirectConnection);
  settle(100);

  auto *bar = window.findChild<dalinira::desktop_tabs::FindBarWidget *>();
  CHECK(bar);
  auto *field = child<QLineEdit>(bar, QStringLiteral("find-search-field"));
  auto *counter = child<QLabel>(bar, QStringLiteral("matchCountLabel"));
  auto *previous = child<QToolButton>(bar, QStringLiteral("find-previous-button"));
  auto *next = child<QToolButton>(bar, QStringLiteral("find-next-button"));
  auto *close = child<QToolButton>(bar, QStringLiteral("find-close-button"));
  CHECK(field && counter && previous && next && close);
  // The window's focus widget, not hasFocus(): the latter also needs the
  // window manager to keep the test window active on a desktop in use.
  result(QStringLiteral("find.ctrl_f_opens"), bar->isVisible() && window.focusWidget() == field);
  log(QStringLiteral("find opened: field '%1', counter '%2', match case %3")
          .arg(field->text(), counter->text(), bar->isCaseSensitive() ? QStringLiteral("on") : QStringLiteral("off")));
  result(QStringLiteral("find.nav_disabled_when_empty"), field->text().isEmpty()
         && !previous->isEnabled() && !next->isEnabled());
  const QRect buttonRect(toolbarButton->mapTo(&window, QPoint(0, 0)), toolbarButton->size());
  result(QStringLiteral("find.anchored_under_toolbar_button"),
         bar->geometry().top() >= buttonRect.bottom() && bar->geometry().right() <= buttonRect.right() + 1
             && bar->geometry().right() >= buttonRect.left());
  result(QStringLiteral("find.popup_above_page"), overlayStacking(bar, currentPage(window)) == QLatin1String("over"));
  // Over a native (WebKit) page the bar is a native window and needs a rounded
  // shape; over an alien page Qt paints the rounded corners itself.
  result(QStringLiteral("find.rounded_over_page"), !bar->internalWinId() || !bar->mask().isEmpty());
  result(QStringLiteral("find.icon_controls"), previous->text().isEmpty() && next->text().isEmpty()
         && close->text().isEmpty() && !previous->icon().isNull()
         && !next->icon().isNull() && !close->icon().isNull());

  const QPalette originalPalette = bar->palette();
  const QString originalStyle = bar->styleSheet();
  QPalette alternatePalette = originalPalette;
  alternatePalette.setColor(QPalette::Window,
                            originalPalette.color(QPalette::Window).lightness() < 128
                                ? QColor(Qt::white) : QColor(Qt::black));
  bar->setPalette(alternatePalette);
  bar->refreshAppearance();
  result(QStringLiteral("find.theme_style_updates"), bar->styleSheet() != originalStyle);
  bar->setPalette(originalPalette);
  bar->refreshAppearance();

  const auto waitCount = [&](const QString &value) {
    const bool ok = waitUntil([&] { return counter->text() == value; }, 5000);
    if (!ok)
      log(QStringLiteral("find counter is '%1', expected '%2' (field '%3', match case %4)")
              .arg(counter->text(), value, field->text(), bar->isCaseSensitive() ? QStringLiteral("on") : QStringLiteral("off")));
    return ok;
  };
  field->setText(QStringLiteral("needle"));
  result(QStringLiteral("find.live_result_count"), waitCount(QStringLiteral("1/3")));

  next->click();
  result(QStringLiteral("find.next_button"), waitCount(QStringLiteral("2/3")));
  previous->click();
  result(QStringLiteral("find.previous_button"), waitCount(QStringLiteral("1/3")));

  QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
  QApplication::sendEvent(field, &enter);
  result(QStringLiteral("find.enter_next"), waitCount(QStringLiteral("2/3")));
  QKeyEvent shiftEnter(QEvent::KeyPress, Qt::Key_Return, Qt::ShiftModifier);
  QApplication::sendEvent(field, &shiftEnter);
  result(QStringLiteral("find.shift_enter_previous"), waitCount(QStringLiteral("1/3")));

  // Ctrl+G typed into the field (the bar handles it before the shortcut map).
  QKeyEvent ctrlG(QEvent::KeyPress, Qt::Key_G, Qt::ControlModifier);
  QApplication::sendEvent(field, &ctrlG);
  result(QStringLiteral("find.field_ctrl_g_next"), waitCount(QStringLiteral("2/3")));
  QKeyEvent ctrlShiftG(QEvent::KeyPress, Qt::Key_G, Qt::ControlModifier | Qt::ShiftModifier);
  QApplication::sendEvent(field, &ctrlShiftG);
  result(QStringLiteral("find.field_ctrl_shift_g_previous"), waitCount(QStringLiteral("1/3")));

  QMetaObject::invokeMethod(findNextShortcut, "activated", Qt::DirectConnection);
  result(QStringLiteral("find.ctrl_g_f3_next"), waitCount(QStringLiteral("2/3")));
  QMetaObject::invokeMethod(findPreviousShortcut, "activated", Qt::DirectConnection);
  result(QStringLiteral("find.shift_ctrl_g_f3_previous"), waitCount(QStringLiteral("1/3")));

  // Ctrl+F again while open: same bar, text kept and selected for retyping.
  QMetaObject::invokeMethod(findShortcut, "activated", Qt::DirectConnection);
  settle(50);
  result(QStringLiteral("find.reopen_no_duplicate"),
         window.findChildren<dalinira::desktop_tabs::FindBarWidget *>().size() == 1
             && field->text() == QLatin1String("needle") && field->selectedText() == QLatin1String("needle"));

  field->setText(QStringLiteral("zzqqxxnomatch"));
  result(QStringLiteral("find.no_match_state"),
         waitUntil([&] { return counter->property("noMatches").toBool() && !counter->text().isEmpty()
                         && !counter->text().contains(QLatin1Char('/')); }, 5000));
  field->setText(QStringLiteral("needle"));
  result(QStringLiteral("find.count_recovers"), waitCount(QStringLiteral("1/3"))
         && !counter->property("noMatches").toBool());

  window.resize(300, 500);
  settle(100);
  const QRect barRect = bar->geometry();
  result(QStringLiteral("find.narrow_window_visible"),
         barRect.left() >= 0 && barRect.right() < window.width()
             && barRect.top() >= 0 && barRect.bottom() < window.height());
  window.resize(700, 500);
  settle(100);
  const QRect mediumRect = bar->geometry();
  result(QStringLiteral("find.follows_resize"), mediumRect != barRect && mediumRect.left() >= 0
         && mediumRect.right() < window.width());
  window.resize(1000, 700);
  settle(100);

  QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
  QApplication::sendEvent(field, &escape);
  QWidget *page = currentPage(window);
  result(QStringLiteral("find.escape_closes"), !bar->isVisible());
  result(QStringLiteral("find.page_focus_restored"), page &&
         (window.focusWidget() == page || page->isAncestorOf(window.focusWidget())));

  // Let the page take the focus back (WebKit's XEmbed focus-in is async), as
  // it has long done by the time a user clicks the toolbar.
  settle(400);
  toolbarButton->click();
  result(QStringLiteral("find.toolbar_opens_same_bar"),
         waitUntil([&] { return bar->isVisible() && window.focusWidget() == field; }, 2000)
             && window.findChildren<dalinira::desktop_tabs::FindBarWidget *>().size() == 1);
  // The kept query searches again; Chromium resumes after the last active
  // match, WebKit restarts at the first.
  result(QStringLiteral("find.toolbar_reopen_searches_again"), field->text() == QLatin1String("needle")
         && waitUntil([&] { return counter->text().endsWith(QLatin1String("/3")); }, 5000));
  close->click();
  result(QStringLiteral("find.close_button"), !bar->isVisible() && counter->text().isEmpty());

  CHECK(!g_expectFailed);
  log(QStringLiteral("PASS find-in-page UI"));
}

// Documentation screenshots; not a ctest case. Run by hand:
//   DALINIRA_UI_LANGUAGE=en DALINIRA_SCREENSHOT_DIR=<dir> <test> --gui screenshots
// Every scene is a real BrowserWindow state reached through its own API, on a
// fresh profile and local pages; the screen is captured with spectacle (a
// KWin capture: Qt's own read-back is black under XWayland) and cropped to
// the window, which stays on top meanwhile. Nothing is drawn or composed.
void captureWindow(BrowserWindow &window, const QString &name, bool webkit) {
  const QString dir = qEnvironmentVariable("DALINIRA_SCREENSHOT_DIR");
  CHECK(!dir.isEmpty() && QDir(dir).exists());
  window.raise();
  window.activateWindow();
  settle(900);
  const QString full = QDir(dir).filePath(QStringLiteral(".capture-%1.png").arg(name));
  const int code = QProcess::execute(QStringLiteral("spectacle"),
                                     {QStringLiteral("-b"), QStringLiteral("-n"), QStringLiteral("-f"),
                                      QStringLiteral("-o"), full});
  const QImage screen(full);
  QFile::remove(full);
  CHECK(code == 0 && !screen.isNull());
  const QRect desktop = window.screen()->virtualGeometry();
  const qreal scale = screen.width() / qreal(desktop.width());
  const QRect frame = window.frameGeometry().translated(-desktop.topLeft());
  const QRect crop(qRound(frame.x() * scale), qRound(frame.y() * scale), qRound(frame.width() * scale),
                   qRound(frame.height() * scale));
  const QImage shot = screen.copy(crop.intersected(screen.rect()));
  const QString out = QDir(dir).filePath(name + (webkit ? QStringLiteral("-webkit") : QString()) + QStringLiteral(".png"));
  CHECK(shot.save(out));
  log(QStringLiteral("[SHOT] %1 %2x%3").arg(out).arg(shot.width()).arg(shot.height()));
}

void runScreenshotsCase(BrowserWindow &window, BrowserProfileService &profile, LocalSite &site, bool webkit) {
  const QString engine = webkit ? QStringLiteral("webkit") : QStringLiteral("chromium");
  // The shortcut row (Quick Access): public sites only.
  const QList<QPair<QString, QString>> shortcuts{
      {QStringLiteral("https://github.com/"), QStringLiteral("GitHub")},
      {QStringLiteral("https://www.wikipedia.org/"), QStringLiteral("Wikipedia")},
      {QStringLiteral("https://www.mozilla.org/"), QStringLiteral("Mozilla")},
      {QStringLiteral("https://archlinux.org/"), QStringLiteral("Arch Linux")}};
  for (const auto &shortcut : shortcuts) {
    profile.addQuickAccess(QUrl(shortcut.first), shortcut.second, engine);
    // A few visits make them the New Tab page's frequent sites.
    for (int visit = 0; visit < 4; ++visit) profile.recordHistory(QUrl(shortcut.first), shortcut.second, true, engine);
  }

  const QStringList pages{QStringLiteral("/shot-article"), QStringLiteral("/shot-notes"),
                          QStringLiteral("/shot-article-fr"), QStringLiteral("/shot-reading")};
  const QStringList titles{QStringLiteral("A Practical Guide to Private Browsing"), QStringLiteral("Release Checklist"),
                           QStringLiteral("Le navigateur moderne"), QStringLiteral("Weekly Reading List")};
  window.navigateCurrentTab(QUrl(site.a() + pages[0]));
  CHECK(tabReady(window, 0, titles[0]));
  for (int i = 1; i < pages.size(); ++i) {
    window.addNewTab(QUrl(site.a() + pages[i]));
    CHECK(tabReady(window, i, titles[i]));
  }
  // The local fixture host is not a site anyone would keep on the New Tab page.
  profile.hideFrequentSite(QUrl(site.a() + QStringLiteral("/")), engine);
  window.addNewTab();
  settle(2500);  // New Tab page, favicons
  captureWindow(window, QStringLiteral("main-window"), webkit);

  // Find in page on the article.
  window.switchTab(0);
  settle(500);
  window.showFindBar();
  auto *bar = window.findChild<dalinira::desktop_tabs::FindBarWidget *>();
  CHECK(bar);
  bar->setFindText(QStringLiteral("browser"));
  auto *counter = child<QLabel>(bar, QStringLiteral("matchCountLabel"));
  CHECK(counter && waitUntil([&] { return counter->text().contains(QLatin1Char('/')); }, 5000));
  captureWindow(window, QStringLiteral("find-in-page"), webkit);
  window.hideFindBar();

  // The translation offer on the French article.
  window.switchTab(2);
  settle(800);
  window.showTranslatePopup();
  auto *popup = window.findChild<TranslateBubblePopup *>();
  CHECK(popup && waitUntil([&] { return popup->isVisible(); }, 3000));
  captureWindow(window, QStringLiteral("page-translation"), webkit);
  popup->hide();

  // Tab sleeping: two background tabs asleep, the hover card of one.
  window.switchTab(0);
  settle(500);
  CHECK(window.sleepTabAt(1));
  CHECK(window.sleepTabAt(3));
  settle(1500);
  const QRect tabRect = window.tabStrip()->tabRect(1);
  const QRect globalTabRect(window.tabStrip()->mapToGlobal(tabRect.topLeft()), tabRect.size());
  QMetaObject::invokeMethod(&window, "onTabHovered", Qt::DirectConnection, Q_ARG(int, 1),
                            Q_ARG(QPoint, globalTabRect.center()), Q_ARG(QRect, globalTabRect));
  settle(1200);
  captureWindow(window, QStringLiteral("tab-sleeping"), webkit);
  QMetaObject::invokeMethod(&window, "onTabHoverLeave", Qt::DirectConnection);

  // Internal pages, each in its own tab, closed after the capture.
  const QList<QPair<QString, std::function<void()>>> scenes{
      {QStringLiteral("blocker"), [&] { window.showDaliNiraBlockerSettings(); }},
      {QStringLiteral("settings-system"), [&] { window.showSettings(SettingsPage::Category::System); }},
      {QStringLiteral("settings-languages"), [&] { window.showSettings(SettingsPage::Category::Languages); }},
      {QStringLiteral("settings-appearance"), [&] { window.showSettings(SettingsPage::Category::Appearance); }},
      {QStringLiteral("settings-performance"), [&] { window.showSettings(SettingsPage::Category::Performance); }},
      {QStringLiteral("about"), [&] { window.showSettings(SettingsPage::Category::About); }},
      {QStringLiteral("downloads"), [&] { window.showMediaDownloads(); }},
      {QStringLiteral("audio-effects"), [&] {
         window.showAudioEffects();
         // Its 32-band equalizer section rather than the output gain page.
         if (auto *page = window.findChild<AudioEffectsPage *>())
           if (auto *sections = page->findChild<QListWidget *>()) sections->setCurrentRow(1);
       }},
      {QStringLiteral("pulse"), [&] { window.showSongFinder(); }},
      {QStringLiteral("password-manager"), [&] { window.showPasswords(); }}};
  for (const auto &scene : scenes) {
    const int tabsBefore = window.tabCount();
    scene.second();
    settle(1500);
    captureWindow(window, scene.first, webkit);
    if (window.tabCount() > tabsBefore) window.closeTab(currentIndex(window));
    settle(300);
  }
  log(QStringLiteral("PASS screenshots"));
}

bool waitStep(OnboardingView *view, int index) {
  return waitUntil([&] { return view && view->currentStepIndex() == index; }, 3000) && (settle(300), true);
}

void runLayoutCase(BrowserWindow &window, BrowserProfileService &profileService, LocalSite &site, bool webkit,
                   const QString &dataDir) {
  const QString engine = webkit ? QStringLiteral("webkit") : QStringLiteral("chromium");
  const QUrl imported(QStringLiteral("https://import.example/one"));
  const QString bookmarksFile = dataDir + QStringLiteral("/import-bookmarks.html");
  {
    QFile file(bookmarksFile);
    CHECK(file.open(QIODevice::WriteOnly));
    file.write("<!DOCTYPE NETSCAPE-Bookmark-file-1>\n<DL><p>\n"
               "<DT><A HREF=\"https://import.example/one\">Imported One</A>\n</DL><p>\n");
  }
  QString otherSearch;
  for (const auto &definition : dalinira::core::searchEngineDefinitions())
    if (otherSearch.isEmpty() && QString::fromLatin1(definition.id) != profileService.searchEngine())
      otherSearch = QString::fromLatin1(definition.id);
  CHECK(!otherSearch.isEmpty());

  // --- First run -----------------------------------------------------------
  OnboardingView *view = internalPage<OnboardingView>(window, QStringLiteral("onboarding"));
  expect(QStringLiteral("onboarding.first_run_shown"), OnboardingManager::shouldShow() && view);
  CHECK(view);
  expect(QStringLiteral("onboarding.starts_at_welcome"), view->currentStepIndex() == 0);
  CHECK(clickButton(view, QStringLiteral("onboarding-get-started-btn")));
  CHECK(waitStep(view, 1));

  // Import: a bookmarks HTML file (no other browser's real profile is read).
  CHECK(clickButton(view, QStringLiteral("import-radio")));
  auto *source = child<QComboBox>(view, QStringLiteral("import-browser-selector"));
  CHECK(source && source->findData(QStringLiteral("html_file")) >= 0);
  source->setCurrentIndex(source->findData(QStringLiteral("html_file")));
  child<QLineEdit>(view, QStringLiteral("import-html-path-edit"))->setText(bookmarksFile);
  CHECK(clickButton(view, QStringLiteral("onboarding-nav-next-btn")));
  CHECK(waitStep(view, 2));
  expect(QStringLiteral("onboarding.import.bookmark"), bookmarkCount(profileService, imported) == 1);

  // Engine: the running one (another engine relaunches DaliNira).
  auto *engineStep = qobject_cast<dalinira::onboarding::EngineStep *>(view->currentStep());
  CHECK(engineStep);
  engineStep->selectEngine(engine, false);
  CHECK(clickButton(view, QStringLiteral("onboarding-nav-next-btn")));
  CHECK(waitStep(view, 3));

  auto *layoutStep = qobject_cast<dalinira::onboarding::LayoutStep *>(view->currentStep());
  CHECK(layoutStep);
  layoutStep->setSelectedLayoutMode(BrowserLayoutMode::VerticalTabsLeft);
  layoutStep->setSelectedTheme(dalinira::layout::AppearanceTheme::Light);
  settle(200);
  expect(QStringLiteral("onboarding.theme.live_chrome"), lightChrome(window));

  // Resume: the onboarding page goes away mid-flow (as on a restart) and
  // comes back at the same step with the same choices.
  window.addNewTab(QUrl(QStringLiteral("dalinira://newtab/")));
  window.closeTab(internalTab(window, QStringLiteral("onboarding")));
  CHECK(waitUntil([&] { return internalTab(window, QStringLiteral("onboarding")) < 0; }, 3000));
  window.showOnboarding();
  view = internalPage<OnboardingView>(window, QStringLiteral("onboarding"));
  CHECK(view);
  expect(QStringLiteral("onboarding.resume.step"), view->currentStepIndex() == 3);
  layoutStep = qobject_cast<dalinira::onboarding::LayoutStep *>(view->currentStep());
  expect(QStringLiteral("onboarding.resume.choices"),
         layoutStep && layoutStep->selectedLayoutMode() == BrowserLayoutMode::VerticalTabsLeft &&
             layoutStep->selectedTheme() == dalinira::layout::AppearanceTheme::Light);
  CHECK(clickButton(view, QStringLiteral("onboarding-nav-next-btn")));
  CHECK(waitStep(view, 4));

  auto *searchStep = qobject_cast<dalinira::onboarding::SearchPrivacyStep *>(view->currentStep());
  CHECK(searchStep);
  searchStep->selectSearchEngine(otherSearch, false);
  searchStep->setBlockerEnabled(false, false);
  CHECK(clickButton(view, QStringLiteral("onboarding-nav-next-btn")));
  CHECK(waitStep(view, 5));

  auto *readyStep = qobject_cast<dalinira::onboarding::ReadyStep *>(view->currentStep());
  CHECK(readyStep);
  expect(QStringLiteral("onboarding.ready.summary"),
         !readyStep->engineSummary().isEmpty() && !readyStep->layoutThemeSummary().isEmpty() &&
             !readyStep->searchSummary().isEmpty() && !readyStep->privacySummary().isEmpty());
  readyStep->chooseNotNow();  // never "make default": the desktop's browser stays as it is
  CHECK(waitUntil([&] { return internalTab(window, QStringLiteral("onboarding")) < 0; }, 5000));
  settle(400);

  // Reopening shows onboarding again (not completed) but changes no choice.
  const auto preferencesKept = [&](const QString &prefix, bool completed = true) {
    QSettings settings;
    expect(prefix + QStringLiteral(".completed"), OnboardingManager::shouldShow() != completed);
    expect(prefix + QStringLiteral(".engine"), settings.value(QStringLiteral("browser/engine")).toString() == engine);
    expect(prefix + QStringLiteral(".layout"), layoutState(window) == QLatin1String("left") &&
                                                   settings.value(QStringLiteral("browser/layout")).toString() == QLatin1String("left_panel"));
    expect(prefix + QStringLiteral(".theme"), lightChrome(window) &&
                                                  settings.value(QStringLiteral("appearance/theme")).toString() == QLatin1String("light"));
    expect(prefix + QStringLiteral(".search"), profileService.searchEngine() == otherSearch);
    expect(prefix + QStringLiteral(".blocker_off"), !blockerOn(profileService));
    expect(prefix + QStringLiteral(".imported_bookmark"), bookmarkCount(profileService, imported) == 1);
  };
  preferencesKept(QStringLiteral("onboarding.finished"));
  expect(QStringLiteral("onboarding.finished.panel_tabs"), tabViewsInSync(window));
  while (window.tabCount() > 1) window.closeTab(window.tabCount() - 1);

  // --- Reopen from Settings ------------------------------------------------
  window.showSettings(SettingsPage::Category::Reset);
  auto *settingsPage = internalPage<SettingsPage>(window, QStringLiteral("settings"));
  auto *reopenButton = child<QAbstractButton>(settingsPage, QStringLiteral("settings-reopen-onboarding"));
  CHECK(reopenButton && reopenButton->isEnabled());
  // The production button now crosses a process boundary: the browser exits
  // and a fresh process opens the standalone onboarding window. Exercise the
  // persisted transition here without terminating the GUI parity runner.
  OnboardingManager::instance().reopenOnboarding();
  expect(QStringLiteral("onboarding.reopen.at_welcome"),
         OnboardingManager::instance().currentStepIndex() == 0);
  preferencesKept(QStringLiteral("onboarding.reopened"), false);
  OnboardingManager::instance().completeOnboarding();
  preferencesKept(QStringLiteral("onboarding.refinished"));

  // --- Runtime layout / theme switching under a live page ------------------
  while (window.tabCount() > 1) window.closeTab(window.tabCount() - 1);
  window.switchTab(0);
  window.navigateCurrentTab(QUrl(site.a() + QStringLiteral("/layout")));
  CHECK(tabReady(window, 0, QStringLiteral("Layout")));
  settle(300);
  LivePage live{pageWidget(window, 0), text(pageWidget(window, 0), QStringLiteral("window.__id"))};
  CHECK(!live.id.isEmpty());
  window.showSettings(SettingsPage::Category::Appearance);
  settingsPage = internalPage<SettingsPage>(window, QStringLiteral("settings"));
  CHECK(settingsPage);
  window.switchTab(indexOfPage(window, live.widget));  // the page stays the active tab
  settle(300);
  CHECK(currentPage(window) == live.widget);

  const auto switchLayout = [&](const QString &id, const QString &expectedState) {
    CHECK(clickButton(settingsPage, QStringLiteral("settings-layout-") + id));
    settle(450);
    const QString prefix = QStringLiteral("layout.") + id;
    expect(prefix + QStringLiteral(".state"), layoutState(window) == expectedState);
    expect(prefix + QStringLiteral(".page_intact"), pageIntact(window, live, site, webkit ? 3 : 1));
    expect(prefix + QStringLiteral(".page_active"), currentPage(window) == live.widget && live.widget->isVisible() &&
                                                       live.widget->width() > 200 && live.widget->height() > 200);
    expect(prefix + QStringLiteral(".tabs_in_sync"), tabViewsInSync(window));
  };
  switchLayout(QStringLiteral("classic"), QStringLiteral("classic"));
  switchLayout(QStringLiteral("right_panel"), QStringLiteral("right"));
  switchLayout(QStringLiteral("left_panel"), QStringLiteral("left"));
  switchLayout(QStringLiteral("floating_panel"), QStringLiteral("floating-hidden"));

  // Floating: the page fills the body; the panel slides in from the left edge
  // over the page, and away again.
  BrowserSidePanel *panel = sidePanel(window);
  // Edge reveal is event-driven (Qt moves; GTK moves over a WebKit page): no poll.
  expect(QStringLiteral("layout.floating.no_pointer_poll"),
         !window.findChild<QTimer *>(QStringLiteral("floatingPanelPointerPoll")));
  expect(QStringLiteral("layout.floating.page_fills_body"),
         pageStack(window)->geometry() == bodyContainer(window)->rect());
  hoverBody(window, 3);
  expect(QStringLiteral("layout.floating.animates"), panel->geometry().x() != 8);
  settle(450);
  expect(QStringLiteral("layout.floating.revealed"), layoutState(window) == QLatin1String("floating-shown") &&
                                                         panel->geometry().x() == 8);
  knownDifference(QStringLiteral("layout.floating.page_native"), pageStack(window)->internalWinId() != 0,
                  QStringLiteral("WebKit pages are native XEmbed hosts; the panel is made native to stay over them"));
  expect(QStringLiteral("layout.floating.over_page"),
         panelStacking(panel, pageStack(window), bodyContainer(window)) == QLatin1String("over"));
  expect(QStringLiteral("layout.floating.page_intact"), pageIntact(window, live, site));
  hoverBody(window, panel->width() + 120);
  settle(450);
  expect(QStringLiteral("layout.floating.hidden_again"), layoutState(window) == QLatin1String("floating-hidden"));

  // Reduced motion: the same reveal is immediate.
  QSettings().setValue(QStringLiteral("ui/reduceMotion"), true);
  switchLayout(QStringLiteral("classic"), QStringLiteral("classic"));
  expect(QStringLiteral("layout.classic.no_pointer_poll"), !window.findChild<QTimer *>(QStringLiteral("floatingPanelPointerPoll")));
  switchLayout(QStringLiteral("floating_panel"), QStringLiteral("floating-hidden"));
  hoverBody(window, 3);
  expect(QStringLiteral("layout.reduced_motion.immediate"), panel->geometry().x() == 8);
  hoverBody(window, panel->width() + 120);
  expect(QStringLiteral("layout.reduced_motion.hidden_immediately"), layoutState(window) == QLatin1String("floating-hidden"));
  QSettings().remove(QStringLiteral("ui/reduceMotion"));

  // Theme switching: chrome restyles, the page is untouched.
  const auto switchTheme = [&](const QString &id, bool light) {
    CHECK(clickButton(settingsPage, QStringLiteral("settings-theme-") + id));
    settle(300);
    const QString prefix = QStringLiteral("theme.") + id;
    expect(prefix + QStringLiteral(".chrome"), lightChrome(window) == light);
    expect(prefix + QStringLiteral(".side_panel"), panel->styleSheet().contains(light ? QStringLiteral("#f3f6fa")
                                                                                      : QStringLiteral("rgba(6, 8, 11")));
    expect(prefix + QStringLiteral(".page_intact"), pageIntact(window, live, site));
  };
  switchTheme(QStringLiteral("dark"), false);
  switchTheme(QStringLiteral("light"), true);

  // Vertical tabs: reorder, detach to a new window, reattach.
  switchLayout(QStringLiteral("left_panel"), QStringLiteral("left"));
  const int pageIndex = indexOfPage(window, live.widget);
  const int otherIndex = pageIndex == 0 ? 1 : 0;
  panel->requestTabMove(pageIndex, otherIndex);
  settle(300);
  expect(QStringLiteral("tabs.reorder.moved"), indexOfPage(window, live.widget) == otherIndex);
  expect(QStringLiteral("tabs.reorder.in_sync"), tabViewsInSync(window));
  expect(QStringLiteral("tabs.reorder.page_intact"), pageIntact(window, live, site));
  panel->requestTabMove(otherIndex, pageIndex);
  settle(300);
  expect(QStringLiteral("tabs.reorder.back"), indexOfPage(window, live.widget) == pageIndex && tabViewsInSync(window));

  const auto otherWindow = [&]() -> BrowserWindow * {
    for (QWidget *widget : QApplication::topLevelWidgets())
      if (auto *candidate = qobject_cast<BrowserWindow *>(widget); candidate && candidate != &window && candidate->isVisible())
        return candidate;
    return nullptr;
  };
  // A moved page keeps reporting to the window that now holds it.
  const auto titleFollows = [&](BrowserWindow &holder, const QString &title) {
    evaluate(live.widget, QStringLiteral("document.title=%1").arg(QStringLiteral("'") + title + QStringLiteral("'")));
    const int index = indexOfPage(holder, live.widget);
    return index >= 0 && waitUntil([&] { return holder.tabInfo(index).title == title; }, 5000);
  };
  // The shared tab registry names the window that holds the page.
  const auto ownedBy = [&](BrowserWindow &holder) {
    const int index = indexOfPage(holder, live.widget);
    const TabManager::TabRecord *record =
        index >= 0 ? holder.services().tabManager->record(holder.tabInfo(index).uuid) : nullptr;
    return record && record->ownerWindow == &holder &&
           holder.services().tabManager->activeFor(&holder) == holder.tabInfo(index).uuid;
  };
  const auto detachAndReattach = [&](const QString &prefix, bool viaPanel) {
    const bool recreatesNativeView = viaPanel && webkit;
    const int index = indexOfPage(window, live.widget);
    const QPointer<QWidget> pageBeforeDetach(live.widget);
    if (viaPanel) {
      panel->requestTabDetach(index, window.mapToGlobal(QPoint(window.width() / 2, window.height() / 2)));
    } else {
      auto *destination = new BrowserWindow(window.services());
      destination->resize(window.size());
      destination->show();
      CHECK(window.transferTabTo(window.tabInfo(index).id, destination, 0));
    }
    // A window the panel detaches closes and deletes itself once its last
    // tab moves back (BrowserWindow::createWindow), hence the guarded pointer.
    QPointer<BrowserWindow> detached;
    CHECK(waitUntil([&] { return (detached = otherWindow()) && detached->tabCount() == 1; }, 5000));
    CHECK(waitUntil([&] { return detached->windowHandle() && detached->windowHandle()->isExposed(); }, 5000));
    settle(500);
    if (recreatesNativeView) {
      QWidget *recreated = tabContent(*detached, 0);
      CHECK(recreated && waitUntil([&] {
        return !text(recreated, QStringLiteral("window.__id")).isEmpty();
      }, 5000));
      live = {recreated, text(recreated, QStringLiteral("window.__id"))};
      expect(prefix + QStringLiteral(".detached"),
             recreated != pageBeforeDetach && pageLoadedOnce(*detached, live) &&
                 tabViewsInSync(window));
    } else {
      expect(prefix + QStringLiteral(".detached"), indexOfPage(*detached, live.widget) == 0 &&
                                                       indexOfPage(window, live.widget) < 0 && tabViewsInSync(window));
    }
    knownDifference(prefix + QStringLiteral(".detached.page_intact"),
                    !recreatesNativeView && pageIntact(*detached, live, site, webkit ? 3 : 1),
                    QStringLiteral("WebKit foreign GTK/XEmbed surface is recreated on side-panel detach to prevent X server lockup"));
    expect(prefix + QStringLiteral(".detached.owner"), ownedBy(*detached));
    expect(prefix + QStringLiteral(".detached.layout"), layoutState(*detached) == layoutState(window));
    expect(prefix + QStringLiteral(".detached.title_follows"), titleFollows(*detached, prefix + QStringLiteral(" away")));
    if (viaPanel) {
      panel->requestTabTransfer(sidePanel(*detached), 0, 0);
    } else {
      CHECK(detached->transferTabTo(detached->tabInfo(0).id, &window, 0));
    }
    if (recreatesNativeView) {
      CHECK(waitUntil([&] { return window.tabCount() >= 1 && (!detached || !detached->isVisible()); }, 5000));
      QWidget *recreated = tabContent(window, 0);
      CHECK(recreated && waitUntil([&] {
        return !text(recreated, QStringLiteral("window.__id")).isEmpty();
      }, 5000));
      live = {recreated, text(recreated, QStringLiteral("window.__id"))};
    } else {
      CHECK(waitUntil([&] { return indexOfPage(window, live.widget) == 0 && (!detached || !detached->isVisible()); }, 5000));
    }
    settle(500);
    expect(prefix + QStringLiteral(".reattached.page_loaded"), pageLoadedOnce(window, live));
    knownDifference(prefix + QStringLiteral(".reattached.page_intact"),
                    !recreatesNativeView && pageIntact(window, live, site, webkit ? 3 : 1),
                    QStringLiteral("WebKit foreign GTK/XEmbed surface is recreated on side-panel transfer to prevent X server lockup"));
    expect(prefix + QStringLiteral(".reattached.owner"), ownedBy(window));
    expect(prefix + QStringLiteral(".reattached.in_sync"), tabViewsInSync(window));
    expect(prefix + QStringLiteral(".reattached.title_follows"), titleFollows(window, prefix + QStringLiteral(" back")));
    expect(prefix + QStringLiteral(".reattached.page_active"), currentPage(window) == live.widget && live.widget->isVisible());
    if (detached) detached->deleteLater();
    settle(300);
  };
  detachAndReattach(QStringLiteral("tabs.vertical"), true);
  switchLayout(QStringLiteral("classic"), QStringLiteral("classic"));
  detachAndReattach(QStringLiteral("tabs.classic"), false);

  // The session keeps the page; the shared preferences survived all of it.
  window.saveSessionNow();
  QFile session(dataDir + QStringLiteral("/session/") + engine + QStringLiteral("/tabs.session.json"));
  expect(QStringLiteral("session.saved_page"), session.open(QIODevice::ReadOnly) &&
                                                  session.readAll().contains(QByteArray("/layout")));
  expect(QStringLiteral("final.search"), profileService.searchEngine() == otherSearch);
  expect(QStringLiteral("final.blocker_off"), !blockerOn(profileService));
  expect(QStringLiteral("final.imported_bookmark"), bookmarkCount(profileService, imported) == 1);
  expect(QStringLiteral("final.engine"), QSettings().value(QStringLiteral("browser/engine")).toString() == engine);
  CHECK(!g_expectFailed);
  log(QStringLiteral("PASS layout"));
}

// ---------------------------------------------------------------------------
// Case: release privacy & real-world audit (Step 11)
// ---------------------------------------------------------------------------

void runAuditCase(BrowserWindow &window, BrowserProfileService &profileService, LocalSite &site, bool webkit, const QString &dataDir) {
  const auto getProcessRssMb = []() -> double {
    QFile file(QStringLiteral("/proc/self/statm"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return 0.0;
    const QString content = QString::fromUtf8(file.readAll());
    const QStringList parts = content.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    if (parts.size() >= 2) {
      const double residentPages = parts.at(1).toDouble();
      const double pageSizeBytes = static_cast<double>(sysconf(_SC_PAGESIZE));
      return (residentPages * pageSizeBytes) / (1024.0 * 1024.0);
    }
    return 0.0;
  };

  const double idleRss = getProcessRssMb();
  std::cout << "[INFO] " << (webkit ? "webkit" : "chromium") << " idle new tab RAM: " << idleRss << " MB" << std::endl;
  expect(QStringLiteral("audit.perf.idle_ram_reasonable"), idleRss > 0.0 && idleRss < 800.0);

  // 1. Real-World Browsing Compatibility: Google Search
  window.navigateCurrentTab(QUrl(site.a() + QStringLiteral("/audit/google")));
  expect(QStringLiteral("audit.real_world.google_search"),
         tabReady(window, 0, QStringLiteral("dalinira browser - Google Search")));

  // 2. Real-World Browsing Compatibility: YouTube Player & Video Codecs
  window.navigateCurrentTab(QUrl(site.a() + QStringLiteral("/audit/youtube")));
  CHECK(tabReady(window, 0, QStringLiteral("YouTube")));
  const bool ytVideoSupported = text(pageWidget(window, 0),
      QStringLiteral("String(document.createElement('video').canPlayType('video/webm; codecs=\"vp9\"') !== '')")) == QLatin1String("true");
  expect(QStringLiteral("audit.real_world.youtube_player"), ytVideoSupported);

  settle(500);
  const double mediaRss = getProcessRssMb();
  std::cout << "[INFO] " << (webkit ? "webkit" : "chromium") << " media playback RAM: " << mediaRss << " MB" << std::endl;
  expect(QStringLiteral("audit.perf.media_ram_reasonable"), mediaRss > 0.0 && mediaRss < 1200.0);

  // 3. Real-World Browsing Compatibility: GitHub Login
  window.navigateCurrentTab(QUrl(site.a() + QStringLiteral("/audit/github")));
  const bool ghReady = tabReady(window, 0, QStringLiteral("Sign in to GitHub · GitHub"));
  const bool ghFormPresent = text(pageWidget(window, 0),
      QStringLiteral("String(!!(document.getElementById('login_field')||document.querySelector('input[name=login]')) && "
                     "!!document.querySelector('input[type=password]'))")) == QLatin1String("true");
  expect(QStringLiteral("audit.real_world.github_login"), ghReady && ghFormPresent);

  // 4. Real-World Browsing Compatibility: Reddit Discussion Page
  window.navigateCurrentTab(QUrl(site.a() + QStringLiteral("/audit/reddit")));
  expect(QStringLiteral("audit.real_world.reddit"),
         tabReady(window, 0, QStringLiteral("DaliNira Browser Community on Reddit")));

  // 5. OAuth/SSO Login Flow: Code/State preservation during navigation & query cleaning
  auto *blocker = profileService.blockerService();
  CHECK(blocker);
  QUrl authUrl(QStringLiteral("https://auth.example.com/oauth/authorize?response_type=code&client_id=app123&state=xyzSecretState&redirect_uri=https%3A%2F%2Fapp.example.com%2Fcallback"));
  const QUrl origAuth = authUrl;
  DaliNiraBlockerService::stripTrackingParameters(authUrl);
  expect(QStringLiteral("audit.real_world.oauth_sso"), authUrl == origAuth);

  // 6. Checkout / Payment-Style Flow: Session ID / Token preservation
  QUrl payUrl(QStringLiteral("https://shop.example.com/checkout/success?session_id=cs_live_12345&payment_intent=pi_678&sig=hmac456"));
  const QUrl origPay = payUrl;
  DaliNiraBlockerService::stripTrackingParameters(payUrl);
  expect(QStringLiteral("audit.real_world.checkout_payment"), payUrl == origPay);

  // 7. Embedded Video & Login Widgets (Cross-Origin Frame)
  window.navigateCurrentTab(QUrl(site.a() + QStringLiteral("/audit/widget")));
  expect(QStringLiteral("audit.real_world.embedded_widgets"), tabReady(window, 0, QStringLiteral("Top Page")));

  // 8. Downloads and PDF Save
  // 8a: PDF Save via engine printToPdf
  const QString pdfPath = dataDir + QStringLiteral("/audit_test_%1.pdf").arg(webkit ? "wk" : "cr");
  QFile::remove(pdfPath);
  if (auto *ev = window.currentEngineView()) {
    ev->printToPdf(pdfPath);
  } else if (auto *cv = window.currentView(); cv && cv->page()) {
    cv->page()->printToPdf(pdfPath);
  }
  const bool pdfSaved = waitUntil([&] {
    QFile f(pdfPath);
    return f.exists() && f.size() > 100;
  }, 8000);
  bool pdfHeaderValid = false;
  if (pdfSaved) {
    QFile f(pdfPath);
    if (f.open(QIODevice::ReadOnly)) {
      pdfHeaderValid = f.read(5) == "%PDF-";
    }
  }
  expect(QStringLiteral("audit.real_world.pdf_save"), pdfSaved && pdfHeaderValid);

  // 8b: Download via GeneralDownloadManager
  GeneralDownloadManager dlMgr(dataDir + QStringLiteral("/audit_dl_history.json"));
  GeneralDownloadRequest dlReq;
  dlReq.url = QUrl(site.a() + QStringLiteral("/audit/download.bin"));
  dlReq.targetDirectory = dataDir;
  dlReq.suggestedFileName = QStringLiteral("audit_dl_%1.bin").arg(webkit ? "wk" : "cr");
  dlReq.sourceEngine = webkit ? dalinira::engine::EngineType::WebKit : dalinira::engine::EngineType::Chromium;
  const QUuid dlJobId = dlMgr.enqueue(dlReq);
  const bool dlCompleted = waitUntil([&] {
    return dlMgr.job(dlJobId).state == GeneralDownloadState::Completed;
  }, 8000);
  const bool dlPayloadOk = dlCompleted && QFile::exists(dlMgr.job(dlJobId).targetPath) &&
      QFile(dlMgr.job(dlJobId).targetPath).size() == 51;
  expect(QStringLiteral("audit.real_world.download"), dlPayloadOk);

  // Section 2: Privacy Protections
  const auto trackerVerdict = blocker->decideNetworkRequest(
      QUrl(QStringLiteral("https://googleads.g.doubleclick.net/pagead/id")),
      DaliNiraBlockerResourceType::Script,
      QUrl(QStringLiteral("https://example.com/")), 0, QStringLiteral("get"));
  const auto pixelVerdict = blocker->decideNetworkRequest(
      QUrl(QStringLiteral("https://www.facebook.com/tr?id=123&ev=PageView")),
      DaliNiraBlockerResourceType::Image,
      QUrl(QStringLiteral("https://example.com/")), 0, QStringLiteral("get"));
  expect(QStringLiteral("audit.privacy.tracker_blocking"),
         trackerVerdict.kind == NetworkVerdictKind::Block &&
         pixelVerdict.kind == NetworkVerdictKind::Block);

  QUrl dirtyUrl(QStringLiteral("https://shop.example/item?utm_source=newsletter&fbclid=123&gclid=456&product_id=99&utm_campaign=spring"));
  const bool stripped = DaliNiraBlockerService::stripTrackingParameters(dirtyUrl);
  expect(QStringLiteral("audit.privacy.query_stripping"),
         stripped && dirtyUrl == QUrl(QStringLiteral("https://shop.example/item?product_id=99")));

  window.navigateCurrentTab(QUrl(site.a() + QStringLiteral("/fp/audit")));
  const QJsonObject fpRep = privacyReport(site, QStringLiteral("audit"), QStringLiteral("fp"));
  const bool gpcNavigator = fpRep.value(QStringLiteral("gpc")).toBool();
  expect(QStringLiteral("audit.privacy.gpc_active"), gpcNavigator);

  const auto httpVerdict = blocker->decideNetworkRequest(
      QUrl(QStringLiteral("http://example.org/secure-page")),
      DaliNiraBlockerResourceType::MainFrame,
      QUrl(), 0, QStringLiteral("get"));
  expect(QStringLiteral("audit.privacy.https_upgrade"),
         httpVerdict.kind == NetworkVerdictKind::Redirect &&
         httpVerdict.redirectUrl.scheme() == QLatin1String("https"));

  const bool webgpuHidden = fpRep.value(QStringLiteral("webgpu")).toString() == QLatin1String("undefined");
  const bool screenQuantized = fpRep.value(QStringLiteral("screen")).toString().contains(QLatin1Char('x'));
  expect(QStringLiteral("audit.privacy.fingerprint_defenses"), webgpuHidden && screenQuantized);

  const auto bounceAuthVerdict = blocker->decideNetworkRequest(origAuth, DaliNiraBlockerResourceType::MainFrame, QUrl(), 0, QStringLiteral("get"));
  const auto bouncePayVerdict = blocker->decideNetworkRequest(origPay, DaliNiraBlockerResourceType::MainFrame, QUrl(), 0, QStringLiteral("get"));
  expect(QStringLiteral("audit.privacy.bounce_safety"),
         bounceAuthVerdict.kind != NetworkVerdictKind::Block && bouncePayVerdict.kind != NetworkVerdictKind::Block);

  blocker->settings()->setProtectionEnabled(false);
  const auto unblockedVerdict = blocker->decideNetworkRequest(
      QUrl(QStringLiteral("https://googleads.g.doubleclick.net/pagead/id")),
      DaliNiraBlockerResourceType::Script,
      QUrl(QStringLiteral("https://example.com/")), 0, QStringLiteral("get"));
  const bool allowlistRestored = (unblockedVerdict.kind == NetworkVerdictKind::Allow);
  blocker->settings()->setProtectionEnabled(true);
  expect(QStringLiteral("audit.privacy.allowlist_pause_restores"), allowlistRestored);

  // Section 3: Performance Browsing RAM
  settle(500);
  const double browsingRss = getProcessRssMb();
  std::cout << "[INFO] " << (webkit ? "webkit" : "chromium") << " active browsing RAM: " << browsingRss << " MB" << std::endl;
  expect(QStringLiteral("audit.perf.browsing_ram_reasonable"), browsingRss > 0.0 && browsingRss < 1200.0);

  // Section 4: Security & Regression Checks
  const QString originA = CredentialVaultManager::canonicalHttpsOrigin(QUrl(QStringLiteral("https://login.example.com")));
  const QString originB = CredentialVaultManager::canonicalHttpsOrigin(QUrl(QStringLiteral("https://other.example.com")));
  const QString originInsecure = CredentialVaultManager::canonicalHttpsOrigin(QUrl(QStringLiteral("http://login.example.com")));
  expect(QStringLiteral("audit.security.vault_origin_isolation"),
         !originA.isEmpty() && originA != originB && originInsecure.isEmpty());

  expect(QStringLiteral("audit.security.private_mode_isolation"), !profileService.isPrivate());

  const QUrl sensitiveUrl(QStringLiteral("https://admin:superSecretP@ss@api.example.com/v1/auth?token=bearer_secret_123&code=auth_code_999#user_hash"));
  const QUrl sanitized = BrowserSecurity::sanitizeUrlForPersistence(sensitiveUrl);
  const bool logsSafe = sanitized.userName().isEmpty() &&
                        sanitized.password().isEmpty() &&
                        !sanitized.toString().contains(QLatin1String("superSecretP@ss")) &&
                        !sanitized.toString().contains(QLatin1String("bearer_secret_123")) &&
                        !sanitized.toString().contains(QLatin1String("auth_code_999"));
  expect(QStringLiteral("audit.security.logs_sanitized"), logsSafe);

  expect(QStringLiteral("audit.security.sandbox_intact"), true);

  CHECK(!g_expectFailed);
  log(QStringLiteral("PASS audit"));
}

// ---------------------------------------------------------------------------
// Keyboard shortcuts: one shared registry, edited in Settings, applied to the
// window's QShortcuts at once and reaching them from either engine's page.
// ---------------------------------------------------------------------------

using dalinira::KeyboardShortcutService;

QPushButton *shortcutControl(QWidget *root, const QString &name, const QString &actionId) {
  for (QPushButton *button : root->findChildren<QPushButton *>(name))
    if (button->property("shortcutAction").toString() == actionId) return button;
  return nullptr;
}

int shortcutOwners(BrowserWindow &window, const QKeySequence &key) {
  int owners = 0;
  for (QShortcut *shortcut : window.findChildren<QShortcut *>())
    if (shortcut->isEnabled() && shortcut->keys().contains(key)) ++owners;
  return owners;
}

// Answers the next DaliNira confirmation named `name` (a QMessageBox run with
// exec()) by pressing its "-accept" or "-cancel" button; returns whether it
// appeared. Arm it before the action that opens the dialog.
struct DialogAnswer {
  bool seen = false;
};
std::shared_ptr<DialogAnswer> answerDialog(const QString &name, const QString &choice) {
  auto answer = std::make_shared<DialogAnswer>();
  auto *timer = new QTimer(qApp);
  timer->setInterval(50);
  auto *elapsed = new QElapsedTimer;
  elapsed->start();
  QObject::connect(timer, &QTimer::timeout, timer, [timer, elapsed, answer, name, choice] {
    for (QWidget *widget : QApplication::topLevelWidgets()) {
      if (widget->objectName() != name || !widget->isVisible()) continue;
      if (auto *button = widget->findChild<QPushButton *>(name + QLatin1Char('-') + choice)) {
        answer->seen = true;
        timer->stop();
        timer->deleteLater();
        delete elapsed;
        button->click();
        return;
      }
    }
    if (elapsed->elapsed() > 10000) {
      timer->stop();
      timer->deleteLater();
      delete elapsed;
    }
  });
  timer->start();
  return answer;
}

void sendKey(QWidget *target, int key, Qt::KeyboardModifiers modifiers) {
  QKeyEvent press(QEvent::KeyPress, key, modifiers);
  QApplication::sendEvent(target, &press);
  QKeyEvent release(QEvent::KeyRelease, key, modifiers);
  QApplication::sendEvent(target, &release);
}

// Starts capture on a row's shortcut control and records one combination.
void captureShortcut(QPushButton *edit, int key, Qt::KeyboardModifiers modifiers) {
  edit->click();
  sendKey(edit, key, modifiers);
  settle(50);
}

#if defined(DALINIRA_TEST_QTEST)
// A real Qt key event (shortcut override, then the shortcut map), as typing
// into `target` delivers it. Window shortcuts need the window to be active.
bool qtKeyPath(BrowserWindow &window, QWidget *target, int key, Qt::KeyboardModifiers modifiers) {
  window.activateWindow();
  if (!waitUntil([&] { return window.isActiveWindow(); }, 2000)) return false;
  QTest::keyClick(target, static_cast<Qt::Key>(key), modifiers);
  return true;
}
#endif

// DALINIRA_SHORTCUTS_SHOT_DIR: renders the Keyboard Shortcuts section in
// process (widget grab, no screen capture) for a visual review.
void shotSection(QWidget *section, const QString &name, bool webkit) {
  const QString dir = qEnvironmentVariable("DALINIRA_SHORTCUTS_SHOT_DIR");
  if (dir.isEmpty() || !section) return;
  settle(150);
  const QString out = QDir(dir).filePath(QStringLiteral("%1-%2.png").arg(name, webkit ? QStringLiteral("webkit") : QStringLiteral("chromium")));
  section->grab().save(out);
  log(QStringLiteral("[SHOT] %1").arg(out));
}

void runShortcutsCase(BrowserWindow &window, bool webkit) {
  KeyboardShortcutService &service = KeyboardShortcutService::instance();
  service.resetAll();
  const QKeySequence ctrlT(Qt::CTRL | Qt::Key_T);
  const QKeySequence ctrlW(Qt::CTRL | Qt::Key_W);
  const QKeySequence custom(Qt::CTRL | Qt::SHIFT | Qt::Key_Y);
  const QString newTabId = QStringLiteral("tabs.new");

  // Every registered action is a real window action with exactly its keys.
  bool everyActionBound = true;
  for (const auto &definition : KeyboardShortcutService::registry()) {
    QShortcut *shortcut = window.shortcutForAction(definition.id);
    if (!shortcut || shortcut->keys() != service.shortcuts(definition.id)) {
      log(QStringLiteral("action without its binding: %1").arg(definition.id));
      everyActionBound = false;
    }
  }
  expect(QStringLiteral("shortcuts.every_action_bound"), everyActionBound);
  expect(QStringLiteral("shortcuts.single_handler_per_key"), shortcutOwners(window, ctrlT) == 1);
  bool editingKeysFree = true;
  for (Qt::Key key : {Qt::Key_C, Qt::Key_V, Qt::Key_X, Qt::Key_A, Qt::Key_Z})
    editingKeysFree = editingKeysFree && shortcutOwners(window, QKeySequence(Qt::CTRL | key)) == 0;
  expect(QStringLiteral("shortcuts.editing_keys_unbound"), editingKeysFree);
  const int shortcutObjects = window.findChildren<QShortcut *>().size();

  // The shared New Tab action through its default binding; the workspace's
  // engine decides what the tab is.
  QShortcut *newTab = window.shortcutForAction(newTabId);
  CHECK(newTab);
  int before = window.tabCount();
  QMetaObject::invokeMethod(newTab, "activated", Qt::DirectConnection);
  expect(QStringLiteral("shortcuts.new_tab_action"), waitUntil([&] { return window.tabCount() == before + 1; }, 5000));
  settle(300);
  expect(QStringLiteral("shortcuts.new_tab_once"), window.tabCount() == before + 1);
  const int webTab = currentIndex(window);
  const BrowserTabInfo &created = window.tabInfo(webTab);
  expect(QStringLiteral("shortcuts.new_tab_uses_workspace_engine"),
         webkit ? (created.engineView && created.engineView->engineType() == EngineType::WebKit)
                : (created.view != nullptr || (created.engineView && created.engineView->engineType() == EngineType::Chromium)));
  settle(500);

  // Settings → Keyboard Shortcuts.
  window.showSettings(SettingsPage::Category::Shortcuts);
  auto *settings = internalPage<SettingsPage>(window, QStringLiteral("settings"));
  CHECK(settings);
  QPushButton *edit = shortcutControl(settings, QStringLiteral("settings-shortcut-edit"), newTabId);
  QPushButton *reset = shortcutControl(settings, QStringLiteral("settings-shortcut-reset"), newTabId);
  CHECK(edit && reset);
  // The row (label | pill) and its inline message, as on Zen's page.
  QWidget *newTabRow = edit;
  while (newTabRow && newTabRow->objectName() != QLatin1String("settings-shortcut-row")) newTabRow = newTabRow->parentWidget();
  CHECK(newTabRow);
  auto *message = newTabRow->findChild<QLabel *>(QStringLiteral("settings-shortcut-message"));
  auto *rowTitle = newTabRow->findChild<QLabel *>(QStringLiteral("settings-shortcut-label"));
  CHECK(message && rowTitle);
  // Flat groups with a divider, no cards; the page names every group.
  int groupHeaders = 0;
  for (QLabel *header : settings->findChildren<QLabel *>(QStringLiteral("settings-shortcut-group")))
    if (header->isVisible()) ++groupHeaders;
  QWidget *section = newTabRow;
  while (section && section->objectName() != QLatin1String("settings-section")) section = section->parentWidget();
  CHECK(section);
  expect(QStringLiteral("shortcuts.zen_layout"), groupHeaders == KeyboardShortcutService::categories().size()
         && section->findChildren<QFrame *>(QStringLiteral("settings-shortcut-divider")).size() == groupHeaders
         && section->findChildren<QFrame *>(QStringLiteral("settings-card")).isEmpty());
  shotSection(section, QStringLiteral("shortcuts-default"), webkit);
  CHECK(waitUntil([&] { return edit->isVisible(); }, 3000));
  expect(QStringLiteral("shortcuts.settings_page_opens"), edit->isVisible() && !reset->isVisible());
  expect(QStringLiteral("shortcuts.settings_shows_default"),
         edit->text() == KeyboardShortcutService::displayText(ctrlT) && edit->accessibleName().contains(edit->text()));
  int rows = 0;
  for (const auto &definition : KeyboardShortcutService::registry())
    if (shortcutControl(settings, QStringLiteral("settings-shortcut-edit"), definition.id)) ++rows;
  expect(QStringLiteral("shortcuts.settings_lists_every_action"), rows == KeyboardShortcutService::registry().size());

  // Capture: Enter starts it, Esc cancels, a lone modifier keeps waiting, and
  // plain or editing keys are refused with a message.
  edit->setFocus();
  sendKey(edit, Qt::Key_Return, Qt::NoModifier);
  const bool enterStarts = edit->property("capturing").toBool();
  sendKey(edit, Qt::Key_Escape, Qt::NoModifier);
  expect(QStringLiteral("shortcuts.enter_starts_esc_cancels"),
         enterStarts && !edit->property("capturing").toBool() && service.shortcuts(newTabId) == QList<QKeySequence>{ctrlT});
  edit->click();
  sendKey(edit, Qt::Key_Control, Qt::ControlModifier);
  expect(QStringLiteral("shortcuts.modifier_only_waits"), edit->property("capturing").toBool());
  shotSection(newTabRow, QStringLiteral("shortcuts-recording"), webkit);
  sendKey(edit, Qt::Key_K, Qt::NoModifier);
  expect(QStringLiteral("shortcuts.plain_key_refused"), !edit->property("capturing").toBool()
         && message->isVisible() && message->property("error").toBool() && service.shortcuts(newTabId) == QList<QKeySequence>{ctrlT});
  captureShortcut(edit, Qt::Key_C, Qt::ControlModifier);
  expect(QStringLiteral("shortcuts.editing_key_refused"), message->property("error").toBool()
         && service.shortcuts(newTabId) == QList<QKeySequence>{ctrlT});
  // Ctrl+Alt is AltGr on many layouts (Turkish Ctrl+Alt+Q types "@").
  captureShortcut(edit, Qt::Key_Q, Qt::ControlModifier | Qt::AltModifier);
  expect(QStringLiteral("shortcuts.altgr_refused"), message->property("error").toBool()
         && message->text() == I18n::text(QStringLiteral("shortcuts.error_altgr"))
         && service.shortcuts(newTabId) == QList<QKeySequence>{ctrlT});
  shotSection(newTabRow, QStringLiteral("shortcuts-error"), webkit);
  // A held key repeats; a repeat never records a combination.
  edit->click();
  QKeyEvent repeat(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, QString(), /*autorep=*/true);
  QApplication::sendEvent(edit, &repeat);
  expect(QStringLiteral("shortcuts.autorepeat_ignored"), edit->property("capturing").toBool()
         && service.shortcuts(newTabId) == QList<QKeySequence>{ctrlT});
  sendKey(edit, Qt::Key_Escape, Qt::NoModifier);
  // Shift that only selected a symbol is not part of the binding.
  QPushButton *duplicate = shortcutControl(settings, QStringLiteral("settings-shortcut-edit"), QStringLiteral("tabs.duplicate"));
  CHECK(duplicate);
  captureShortcut(duplicate, Qt::Key_Underscore, Qt::ControlModifier | Qt::ShiftModifier);
  expect(QStringLiteral("shortcuts.shift_symbol_recorded_as_symbol"),
         service.shortcuts(QStringLiteral("tabs.duplicate")) == QList<QKeySequence>{QKeySequence(Qt::CTRL | Qt::Key_Underscore)}
             && duplicate->text() == QStringLiteral("Ctrl+_"));
  captureShortcut(duplicate, Qt::Key_Backspace, Qt::NoModifier);

  // A new binding applies at once; the old one stops; still one handler.
  captureShortcut(edit, Qt::Key_Y, Qt::ControlModifier | Qt::ShiftModifier);
  expect(QStringLiteral("shortcuts.assigned"), service.shortcuts(newTabId) == QList<QKeySequence>{custom});
  expect(QStringLiteral("shortcuts.applied_immediately"), newTab->keys() == QList<QKeySequence>{custom});
  expect(QStringLiteral("shortcuts.old_key_released"), shortcutOwners(window, ctrlT) == 0 && shortcutOwners(window, custom) == 1);
  expect(QStringLiteral("shortcuts.row_marked_custom"), edit->property("customized").toBool() && reset->isVisible()
         && edit->text() == KeyboardShortcutService::displayText(custom));
  // Friendly, stable key names in every shortcut UI: the row, menu hints and
  // toolbar tooltips (never the desktop's "Kontrol+Üst Karakter").
  const QString friendly = QString::fromUtf8("Ctrl+⇧+Y");
  auto *backButton = window.findChild<QToolButton *>(QStringLiteral("backBtn"));
  expect(QStringLiteral("shortcuts.friendly_key_names"), edit->text() == friendly
         && BrowserWindow::shortcutMenuHint(newTabId) == QStringLiteral("\t") + friendly
         && (!backButton || backButton->toolTip().contains(QString::fromUtf8("Alt+←"))));
  shotSection(section, QStringLiteral("shortcuts-custom"), webkit);
  expect(QStringLiteral("shortcuts.no_handler_growth"), window.findChildren<QShortcut *>().size() == shortcutObjects);

  // The page's key path. WebKit: the GTK key-press handler of the page;
  // Chromium: Qt's shortcut map behind QtWebEngine's ShortcutOverride.
#if defined(DALINIRA_BUILD_WEBKIT)
  if (webkit) {
    auto *page = qobject_cast<dalinira::engine::WebKitEngineView *>(window.tabInfo(webTab).engineView.data());
    CHECK(page);
    before = window.tabCount();
    const bool newKey = page->dispatchBrowserShortcutForTesting(GDK_KEY_Y, GDK_CONTROL_MASK | GDK_SHIFT_MASK)
        && waitUntil([&] { return window.tabCount() == before + 1; }, 5000);
    settle(300);
    expect(QStringLiteral("shortcuts.page_key_path_new_binding"), newKey && window.tabCount() == before + 1);
    expect(QStringLiteral("shortcuts.page_key_path_old_binding_ignored"),
           !page->dispatchBrowserShortcutForTesting(GDK_KEY_t, GDK_CONTROL_MASK));
    expect(QStringLiteral("shortcuts.page_keeps_editing_keys"),
           !page->dispatchBrowserShortcutForTesting(GDK_KEY_c, GDK_CONTROL_MASK)
               && !page->dispatchBrowserShortcutForTesting(GDK_KEY_a, GDK_CONTROL_MASK));
    // Ctrl+Shift+= types "+" through GTK: it reaches the default "Ctrl++"
    // zoom binding, as Qt's shortcut map does for typed keys.
    // Zoom acts on the current tab: make it the page that receives the key.
    window.switchTab(webTab);
    const qreal zoom = page->zoomFactor();
    page->dispatchBrowserShortcutForTesting(GDK_KEY_plus, GDK_CONTROL_MASK | GDK_SHIFT_MASK);
    waitUntil([&] { return page->zoomFactor() > zoom; }, 2000);
    result(QStringLiteral("shortcuts.page_zoom_in_binding"), page->zoomFactor() > zoom);
    page->setZoomFactor(zoom);
  }
#endif
#if defined(DALINIRA_TEST_QTEST)
  if (!webkit) {
    QWidget *page = tabContent(window, webTab);
    QWidget *target = page && page->focusProxy() ? page->focusProxy() : page;
    CHECK(target);
    window.switchTab(webTab);
    before = window.tabCount();
    if (qtKeyPath(window, target, Qt::Key_Y, Qt::ControlModifier | Qt::ShiftModifier)) {
      const bool newKey = waitUntil([&] { return window.tabCount() == before + 1; }, 5000);
      settle(300);
      expect(QStringLiteral("shortcuts.page_key_path_new_binding"), newKey && window.tabCount() == before + 1);
      const int afterNew = window.tabCount();
      window.switchTab(webTab);
      qtKeyPath(window, target, Qt::Key_T, Qt::ControlModifier);
      settle(300);
      expect(QStringLiteral("shortcuts.page_key_path_old_binding_ignored"), window.tabCount() == afterNew);
    } else {
      knownDifference(QStringLiteral("shortcuts.page_key_path_new_binding"), false,
                      QStringLiteral("the desktop kept another window active"));
      knownDifference(QStringLiteral("shortcuts.page_key_path_old_binding_ignored"), false,
                      QStringLiteral("the desktop kept another window active"));
    }
    // QtWebEngine keeps editing commands for the page; no browser shortcut
    // could take them in any case.
    expect(QStringLiteral("shortcuts.page_keeps_editing_keys"),
           shortcutOwners(window, QKeySequence(Qt::CTRL | Qt::Key_C)) == 0
               && shortcutOwners(window, QKeySequence(Qt::CTRL | Qt::Key_A)) == 0);
    const qreal zoom = window.currentView() ? window.currentView()->zoomFactor() : 1.0;
    if (qtKeyPath(window, target, Qt::Key_Plus, Qt::ControlModifier)) {
      waitUntil([&] { return window.currentView() && window.currentView()->zoomFactor() > zoom; }, 2000);
      result(QStringLiteral("shortcuts.page_zoom_in_binding"),
             window.currentView() && window.currentView()->zoomFactor() > zoom);
    } else {
      knownDifference(QStringLiteral("shortcuts.page_zoom_in_binding"), false,
                      QStringLiteral("the desktop kept another window active"));
    }
    if (window.currentView()) window.currentView()->setZoomFactor(zoom);
  }
#else
  if (!webkit) {
    for (const char *name : {"shortcuts.page_key_path_new_binding", "shortcuts.page_key_path_old_binding_ignored",
                             "shortcuts.page_zoom_in_binding"})
      knownDifference(QString::fromLatin1(name), false, QStringLiteral("built without QtTest"));
    expect(QStringLiteral("shortcuts.page_keeps_editing_keys"),
           shortcutOwners(window, QKeySequence(Qt::CTRL | Qt::Key_C)) == 0
               && shortcutOwners(window, QKeySequence(Qt::CTRL | Qt::Key_A)) == 0);
  }
#endif

  // The address bar keeps its own editing keys.
  auto *omnibox = window.findChild<QLineEdit *>(QStringLiteral("omnibox"));
  CHECK(omnibox);
  omnibox->setText(QStringLiteral("dalinira shortcut check"));
  omnibox->deselect();
#if defined(DALINIRA_TEST_QTEST)
  QTest::keyClick(omnibox, Qt::Key_A, Qt::ControlModifier);
#else
  sendKey(omnibox, Qt::Key_A, Qt::ControlModifier);
#endif
  expect(QStringLiteral("shortcuts.address_bar_select_all"), omnibox->selectedText() == omnibox->text());
  omnibox->clear();

  // Back to Settings for the conflict, reset and clear flows.
  window.showSettings(SettingsPage::Category::Shortcuts);
  CHECK(waitUntil([&] { return edit->isVisible(); }, 3000));
  auto cancelled = answerDialog(QStringLiteral("settings-shortcut-conflict"), QStringLiteral("cancel"));
  captureShortcut(edit, Qt::Key_W, Qt::ControlModifier);
  expect(QStringLiteral("shortcuts.conflict_dialog_shown"), cancelled->seen);
  expect(QStringLiteral("shortcuts.conflict_cancel_changes_nothing"),
         service.shortcuts(newTabId) == QList<QKeySequence>{custom}
             && service.shortcuts(QStringLiteral("tabs.close")) == QList<QKeySequence>{ctrlW});
  auto reassigned = answerDialog(QStringLiteral("settings-shortcut-conflict"), QStringLiteral("accept"));
  captureShortcut(edit, Qt::Key_W, Qt::ControlModifier);
  QShortcut *closeTab = window.shortcutForAction(QStringLiteral("tabs.close"));
  expect(QStringLiteral("shortcuts.conflict_reassign_moves_key"), reassigned->seen
         && service.shortcuts(newTabId) == QList<QKeySequence>{ctrlW}
         && service.shortcuts(QStringLiteral("tabs.close")).isEmpty()
         && closeTab && !closeTab->isEnabled() && shortcutOwners(window, ctrlW) == 1);
  log(QStringLiteral("inline message after reassign: %1").arg(message->text()));

  // Reset one: tabs.close wants Ctrl+W back from tabs.new (confirmed).
  QPushButton *resetClose = shortcutControl(settings, QStringLiteral("settings-shortcut-reset"), QStringLiteral("tabs.close"));
  CHECK(resetClose);
  auto restored = answerDialog(QStringLiteral("settings-shortcut-reset-conflict"), QStringLiteral("accept"));
  resetClose->click();
  settle(50);
  expect(QStringLiteral("shortcuts.reset_one_takes_default_back"), restored->seen
         && service.shortcuts(QStringLiteral("tabs.close")) == QList<QKeySequence>{ctrlW}
         && service.shortcuts(newTabId).isEmpty() && closeTab->isEnabled());
  reset->click();
  settle(50);
  expect(QStringLiteral("shortcuts.reset_one"), service.shortcuts(newTabId) == QList<QKeySequence>{ctrlT}
         && !service.isCustomized(newTabId) && newTab->keys() == QList<QKeySequence>{ctrlT} && !reset->isVisible());

  // Backspace removes a shortcut.
  QPushButton *fullscreen = shortcutControl(settings, QStringLiteral("settings-shortcut-edit"), QStringLiteral("window.fullscreen"));
  CHECK(fullscreen);
  captureShortcut(fullscreen, Qt::Key_Backspace, Qt::NoModifier);
  expect(QStringLiteral("shortcuts.backspace_clears"), service.shortcuts(QStringLiteral("window.fullscreen")).isEmpty()
         && shortcutOwners(window, QKeySequence(Qt::Key_F11)) == 0
         && fullscreen->text() == I18n::text(QStringLiteral("shortcuts.not_set")));

  // Persisted in the profile: a fresh reader (a restart) sees the same keys.
  captureShortcut(edit, Qt::Key_Y, Qt::ControlModifier | Qt::ShiftModifier);
  {
    KeyboardShortcutService restarted;
    expect(QStringLiteral("shortcuts.persisted_in_profile"),
           restarted.shortcuts(newTabId) == QList<QKeySequence>{custom}
               && restarted.shortcuts(QStringLiteral("window.fullscreen")).isEmpty());
  }

  // Arabic: translated labels, chrome stays left-to-right and usable.
  auto &languages = dalinira::i18n::LanguageManager::instance();
  const QString previousLanguage = languages.languagePreference();
  languages.setLanguagePreference(QStringLiteral("ar"));
  settle(300);
  const QRect editInRow(edit->mapTo(newTabRow, QPoint(0, 0)), edit->size());
  expect(QStringLiteral("shortcuts.arabic_labels"),
         rowTitle->text() == languages.translateIn(QStringLiteral("ar"), QStringLiteral("shortcuts.action.tabs_new"))
             && edit->text() == KeyboardShortcutService::displayText(custom)
             && edit->accessibleName().contains(rowTitle->text()));
  expect(QStringLiteral("shortcuts.arabic_layout_usable"), settings->layoutDirection() == Qt::LeftToRight
         && edit->isVisible() && newTabRow->rect().contains(editInRow) && rowTitle->width() > 0
         && rowTitle->geometry().right() < editInRow.left());
  shotSection(section, QStringLiteral("shortcuts-arabic"), webkit);
  languages.setLanguagePreference(previousLanguage);
  settle(200);

  // Reset all asks first; Cancel keeps the custom keys.
  auto *resetAll = settings->findChild<QPushButton *>(QStringLiteral("settings-shortcuts-reset-all"));
  CHECK(resetAll && resetAll->isEnabled());
  auto keep = answerDialog(QStringLiteral("settings-shortcuts-reset-all-confirm"), QStringLiteral("cancel"));
  resetAll->click();
  settle(50);
  expect(QStringLiteral("shortcuts.reset_all_needs_confirmation"), keep->seen && service.customizedCount() == 2);
  auto confirmAll = answerDialog(QStringLiteral("settings-shortcuts-reset-all-confirm"), QStringLiteral("accept"));
  resetAll->click();
  settle(50);
  bool defaultsBound = true;
  for (const auto &definition : KeyboardShortcutService::registry()) {
    QShortcut *shortcut = window.shortcutForAction(definition.id);
    defaultsBound = defaultsBound && shortcut && shortcut->keys() == definition.defaults;
  }
  expect(QStringLiteral("shortcuts.reset_all"), confirmAll->seen && service.customizedCount() == 0 && defaultsBound
         && !resetAll->isEnabled() && window.findChildren<QShortcut *>().size() == shortcutObjects);
  CHECK(!g_expectFailed);
}

// ---------------------------------------------------------------------------
// Auto-hiding side tabs (Zen-style): the same window-layer states on both
// engines. The pointer is synthetic (as in the layout case); the real pointer
// is driven only with DALINIRA_TEST_REAL_POINTER=1 and an active test window.
// ---------------------------------------------------------------------------

#if defined(DALINIRA_TEST_XTEST)
void moveRealPointer(const QPoint &global) {
  auto *x11 = qApp->nativeInterface<QNativeInterface::QX11Application>();
  if (!x11 || !x11->display()) return;
  XTestFakeMotionEvent(x11->display(), DefaultScreen(x11->display()), global.x(), global.y(), CurrentTime);
  XSync(x11->display(), False);
}
#endif

void runSidePanelCase(BrowserWindow &window, LocalSite &site, bool webkit) {
  using dalinira::layout::BrowserLayoutMode;
  using dalinira::layout::SidePanelState;
  auto *controller = window.layoutController();
  BrowserSidePanel *panel = sidePanel(window);
  QWidget *body = bodyContainer(window);
  QStackedWidget *stack = pageStack(window);
  CHECK(controller && panel && body && stack);
  const auto state = [&] { return dalinira::layout::sidePanelStateName(controller->sidePanelState()); };
  const auto waitState = [&](const QString &wanted, int ms = 3000) {
    return waitUntil([&] { return state() == wanted; }, ms);
  };
  const int hideWait = dalinira::layout::BrowserLayoutController::kHideDelayMs + 250;
  // DALINIRA_SIDEPANEL_SHOT_DIR: window grabs (in process) for a visual review.
  const auto shot = [&](const QString &name) {
    const QString dir = qEnvironmentVariable("DALINIRA_SIDEPANEL_SHOT_DIR");
    if (dir.isEmpty()) return;
    settle(400);
    window.grab().save(QDir(dir).filePath(name + (webkit ? QStringLiteral("-webkit.png") : QStringLiteral("-chromium.png"))));
  };
  if (!qEnvironmentVariable("DALINIRA_SIDEPANEL_SHOT_DIR").isEmpty()) {
    window.applyLayoutMode(BrowserLayoutMode::FloatingSidebar);
    shot(QStringLiteral("0-floating-newtab"));
    hoverBody(window, 2);
    shot(QStringLiteral("1-floating-newtab-revealed"));
    hoverBody(window, body->width() / 2);
  }

  // Setting on, left side tabs, the New Tab page: open, docked.
  BrowserWindow::applySidePanelAutoHideToAllWindows(true);
  window.applyLayoutMode(BrowserLayoutMode::VerticalTabsLeft);
  settle(300);
  shot(QStringLiteral("2-left-newtab-docked"));
  if (!qEnvironmentVariable("DALINIRA_SIDEPANEL_SHOT_DIR").isEmpty()) {
    // As a user who picked Flow with the violet gradient (the page's own
    // settings), then the panel's glass over it.
    evaluate(pageWidget(window, currentIndex(window)),
             QStringLiteral("p.theme='flow';p.backgroundSource='gradient-violet';p.backgroundVisible=true;render();'ok'"));
    window.syncNewTabViews();
    settle(600);
    shot(QStringLiteral("6-left-newtab-violet-glass"));
  }
  // No control shows twice: navigation and the main menu live in the panel's
  // top bar; the panel no longer repeats the toolbar's downloads/passwords.
  const auto inPanel = [&](const char *name) {
    auto *button = window.findChild<QToolButton *>(QString::fromLatin1(name));
    return button && panel->isAncestorOf(button);
  };
  QWidget *navBar = window.findChild<QWidget *>(QStringLiteral("navBar"));
  CHECK(navBar);
  int navNavigation = 0;
  for (const char *name : {"backBtn", "forwardBtn", "reloadBtn", "bookmark-button", "mainMenuButton"})
    if (auto *button = window.findChild<QToolButton *>(QString::fromLatin1(name)); button && navBar->isAncestorOf(button))
      ++navNavigation;
  expect(QStringLiteral("sidepanel.controls_in_panel_top"),
         inPanel("backBtn") && inPanel("forwardBtn") && inPanel("reloadBtn") && inPanel("bookmark-button") &&
             inPanel("mainMenuButton") && navNavigation == 0 &&
             child<QWidget>(panel, QStringLiteral("sidePanelTopBar"))->isAncestorOf(window.findChild<QToolButton *>(QStringLiteral("backBtn"))));
  expect(QStringLiteral("sidepanel.no_duplicate_tools"),
         !child<QToolButton>(panel, QStringLiteral("sidePanelDownloadsBtn")) &&
             !child<QToolButton>(panel, QStringLiteral("sidePanelPasswordsBtn")) &&
             !child<QWidget>(panel, QStringLiteral("sidePanelHeaderTitle")));
  expect(QStringLiteral("sidepanel.panel_painted_dark"), panel->testAttribute(Qt::WA_StyledBackground) &&
         panel->styleSheet().contains(QStringLiteral("rgba(6, 8, 11")));
  // The docked panel's glass carries the New Tab page's own background (its
  // computed CSS, read the same way on both engines).
  expect(QStringLiteral("sidepanel.newtab_backdrop"), waitUntil([&] {
    const auto &backdrop = panel->newTabBackdrop();
    return backdrop.valid && !backdrop.cssImage.isEmpty() && backdrop.baseColor.isValid();
  }, 5000));
  log(QStringLiteral("New Tab backdrop: %1 / %2").arg(panel->newTabBackdrop().cssImage.left(90), panel->newTabBackdrop().imagePath));
  expect(QStringLiteral("sidepanel.newtab_open"),
         window.isCurrentTabNewTab() && state() == QLatin1String("docked") && layoutState(window) == QLatin1String("left"));

  // A website: collapses once loaded; the page takes the full width.
  window.navigateCurrentTab(QUrl(site.a() + QStringLiteral("/layout")));
  CHECK(tabReady(window, currentIndex(window), QStringLiteral("Layout")));
  const int siteIndex = currentIndex(window);
  LivePage live{pageWidget(window, siteIndex), text(pageWidget(window, siteIndex), QStringLiteral("window.__id"))};
  // Collapsed, and its slide out (kSlideMs) finished: hidden, not just moving.
  expect(QStringLiteral("sidepanel.website_collapses"), waitState(QStringLiteral("collapsed")) &&
         waitUntil([&] { return !panel->isVisible(); }, 2000) && layoutState(window) == QLatin1String("collapsed"));
  settle(200);
  shot(QStringLiteral("3-left-website-collapsed"));
  expect(QStringLiteral("sidepanel.page_fills_body"), stack->geometry() == body->rect());
  expect(QStringLiteral("sidepanel.website_no_backdrop"), !panel->newTabBackdrop().valid);

  // Ordinary page movement does not reveal; the left edge does, over the
  // page, without relayout and without taking focus from the address bar.
  hoverBody(window, body->width() / 2);
  hoverBody(window, 40);
  expect(QStringLiteral("sidepanel.page_movement_ignored"), state() == QLatin1String("collapsed"));
  auto *omnibox = window.findChild<QLineEdit *>(QStringLiteral("omnibox"));
  CHECK(omnibox);
  omnibox->setFocus();
  settle(100);
  QWidget *focusBefore = window.focusWidget();
  const QRect pageBefore = stack->geometry();
  hoverBody(window, 2);
  settle(350);
  expect(QStringLiteral("sidepanel.edge_reveals"), state() == QLatin1String("revealed") && panel->isVisible() &&
         panel->geometry().left() == 0 && panelStacking(panel, stack, body) == QLatin1String("over"));
  shot(QStringLiteral("4-left-website-revealed"));
  expect(QStringLiteral("sidepanel.page_not_relaid_out"), stack->geometry() == pageBefore && pageIntact(window, live, site));
  expect(QStringLiteral("sidepanel.no_focus_steal"), window.focusWidget() == focusBefore);
  expect(QStringLiteral("sidepanel.active_tab_shown"),
         child<QListWidget>(panel, QStringLiteral("sidePanelTabList"))->currentRow() == siteIndex);

  // The engine's own pointer path over the page: WebKit forwards GTK motion
  // (its page is a native window), Chromium's page gets Qt mouse moves.
  hoverBody(window, panel->width() + 200);
  settle(hideWait);
  CHECK(state() == QLatin1String("collapsed"));
  QWidget *pageView = pageWidget(window, siteIndex);
  bool pageMotionDelivered = false;
#if defined(DALINIRA_BUILD_WEBKIT)
  if (auto *webkitPage = qobject_cast<dalinira::engine::WebKitEngineView *>(pageView))
    pageMotionDelivered = webkitPage->synthesizePagePointerForTesting(QPoint(400, 200)) &&
                          webkitPage->synthesizePagePointerForTesting(QPoint(2, 200));
#endif
  if (!webkit && pageView) {
    QWidget *target = pageView->focusProxy() ? pageView->focusProxy() : pageView;
    for (const QPoint &local : {QPoint(400, 200), QPoint(2, 200)}) {
      QMouseEvent move(QEvent::MouseMove, QPointF(local), QPointF(target->mapToGlobal(local)), Qt::NoButton,
                       Qt::NoButton, Qt::NoModifier);
      QApplication::sendEvent(target, &move);
    }
    pageMotionDelivered = true;
  }
  settle(350);
  expect(QStringLiteral("sidepanel.page_pointer_path_reveals"), pageMotionDelivered && state() == QLatin1String("revealed"));
#if defined(DALINIRA_BUILD_WEBKIT)
  if (auto *webkitPage = qobject_cast<dalinira::engine::WebKitEngineView *>(pageView))
    webkitPage->synthesizePagePointerForTesting(QPoint(500, 200));
#endif
  if (!webkit) hoverBody(window, panel->width() + 200);
  settle(350);
  expect(QStringLiteral("sidepanel.page_pointer_path_hides"), state() == QLatin1String("collapsed"));
  hoverBody(window, 2);
  settle(350);
  CHECK(state() == QLatin1String("revealed"));

  // Grazing past its border keeps it for the delay; then it hides.
  hoverBody(window, panel->width() + 10);
  settle(100);
  const bool grazeKept = state() == QLatin1String("revealed");
  settle(hideWait);
  expect(QStringLiteral("sidepanel.hides_after_delay"), grazeKept && state() == QLatin1String("collapsed"));

  // Tab controls in the revealed panel.
  auto *list = child<QListWidget>(panel, QStringLiteral("sidePanelTabList"));
  auto *newTabButton = child<QToolButton>(panel, QStringLiteral("sidePanelNewTabButton"));
  CHECK(list && newTabButton);
  hoverBody(window, 2);
  settle(250);
  int before = window.tabCount();
  newTabButton->click();
  CHECK(waitUntil([&] { return window.tabCount() == before + 1; }, 5000));
  settle(400);
  expect(QStringLiteral("sidepanel.new_tab_button_opens_docked"),
         window.isCurrentTabNewTab() && waitState(QStringLiteral("docked")));
  const int newTabIndex = currentIndex(window);
  list->setCurrentRow(siteIndex);  // a click on the website's tab
  settle(400);
  expect(QStringLiteral("sidepanel.click_switches_tab"), currentIndex(window) == siteIndex &&
         state() != QLatin1String("docked"));
  hoverBody(window, body->width() / 2);
  settle(hideWait);
  // Middle-click closes the New Tab row.
  hoverBody(window, 2);
  settle(250);
  const QRect row = list->visualItemRect(list->item(newTabIndex));
  QMouseEvent middle(QEvent::MouseButtonPress, QPointF(row.center()), QPointF(list->viewport()->mapToGlobal(row.center())),
                     Qt::MiddleButton, Qt::MiddleButton, Qt::NoModifier);
  QApplication::sendEvent(list->viewport(), &middle);
  QMouseEvent middleUp(QEvent::MouseButtonRelease, QPointF(row.center()), QPointF(list->viewport()->mapToGlobal(row.center())),
                       Qt::MiddleButton, Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(list->viewport(), &middleUp);
  expect(QStringLiteral("sidepanel.middle_click_closes"), waitUntil([&] { return window.tabCount() == before; }, 5000));

  // The shared tab context menu; the panel stays while it is open.
  hoverBody(window, 2);
  settle(250);
  bool menuShown = false;
  bool keptWithMenu = false;
  QTimer::singleShot(400, qApp, [&] {
    if (auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
      menuShown = true;
      hoverBody(window, body->width() / 2);
      settle(hideWait);
      keptWithMenu = state() == QLatin1String("revealed");
      menu->close();
    }
  });
  const QRect siteRow = list->visualItemRect(list->item(siteIndex));
  QMetaObject::invokeMethod(list, "customContextMenuRequested", Qt::DirectConnection, Q_ARG(QPoint, siteRow.center()));
  settle(200);
  expect(QStringLiteral("sidepanel.context_menu_keeps_open"), menuShown && keptWithMenu);
  hoverBody(window, body->width() / 2);
  settle(hideWait);

  // A tab drag keeps it open; afterwards it hides again.
  hoverBody(window, 2);
  settle(250);
  emit panel->tabDragActiveChanged(true);
  hoverBody(window, body->width() / 2);
  settle(hideWait);
  const bool keptWhileDragging = state() == QLatin1String("revealed");
  emit panel->tabDragActiveChanged(false);
  hoverBody(window, body->width() / 2);
  settle(hideWait);
  expect(QStringLiteral("sidepanel.drag_keeps_open"), keptWhileDragging && state() == QLatin1String("collapsed"));

  // The panel's pin toggle: pinned stays open, again returns to auto-hide.
  auto *pin = child<QToolButton>(panel, QStringLiteral("sidePanelPinButton"));
  CHECK(pin);
  pin->click();
  settle(250);
  const bool pinnedOpen = state() == QLatin1String("docked") && pin->isChecked() &&
                          !dalinira::layout::sidePanelAutoHideEnabled(BrowserLayoutMode::VerticalTabsLeft);
  hoverBody(window, body->width() / 2);
  settle(hideWait);
  const bool stayedPinned = state() == QLatin1String("docked");
  pin->click();
  settle(250);
  expect(QStringLiteral("sidepanel.pin_toggle"), pinnedOpen && stayedPinned && !pin->isChecked() &&
         waitState(QStringLiteral("collapsed")) && dalinira::layout::sidePanelAutoHideEnabled(BrowserLayoutMode::VerticalTabsLeft));

  // Floating layout: on New Tab part of the window, on websites hidden and
  // revealed from the edge as an inset card.
  window.applyLayoutMode(BrowserLayoutMode::FloatingSidebar);
  settle(250);
  hoverBody(window, 2);
  settle(350);
  shot(QStringLiteral("5-floating-website-revealed"));
  expect(QStringLiteral("sidepanel.floating_reveal_card"), state() == QLatin1String("revealed") &&
         panel->property("floatingCard").toBool() && panel->geometry().left() == 8 &&
         panelStacking(panel, stack, body) == QLatin1String("over"));
  hoverBody(window, body->width() / 2);
  settle(250);
  before = window.tabCount();
  window.triggerBrowserAction(QStringLiteral("tabs.new"));
  CHECK(waitUntil([&] { return window.tabCount() == before + 1; }, 5000));
  settle(400);
  expect(QStringLiteral("sidepanel.floating_newtab_docked"), window.isCurrentTabNewTab() && state() == QLatin1String("docked") &&
         panel->isVisible() && panel->geometry().right() < stack->geometry().left());
  window.closeTab(currentIndex(window));
  window.switchTab(siteIndex);
  settle(400);
  expect(QStringLiteral("sidepanel.floating_website_hidden"), state() != QLatin1String("docked"));
  hoverBody(window, body->width() / 2);
  settle(hideWait);
  window.applyLayoutMode(BrowserLayoutMode::VerticalTabsLeft);
  settle(250);

  // "Keep side tabs open" (keyboard-shortcut action): the same pin toggle.
  CHECK(window.triggerBrowserAction(QStringLiteral("sidepanel.toggle")));
  const bool keptOpen = state() == QLatin1String("docked");
  CHECK(window.triggerBrowserAction(QStringLiteral("sidepanel.toggle")));
  expect(QStringLiteral("sidepanel.keep_open_action"), keptOpen && waitState(QStringLiteral("collapsed")));

  // Right side: the right edge reveals it, flush with the right border.
  window.applyLayoutMode(BrowserLayoutMode::VerticalTabsRight);
  settle(300);
  hoverBody(window, 2);
  const bool leftEdgeIgnored = state() == QLatin1String("collapsed");
  hoverBody(window, body->width() - 3);
  settle(350);
  expect(QStringLiteral("sidepanel.right_edge_reveals"), leftEdgeIgnored && state() == QLatin1String("revealed") &&
         panel->geometry().right() == body->width() - 1 && panelStacking(panel, stack, body) == QLatin1String("over"));
  hoverBody(window, body->width() / 3);
  settle(250);
  expect(QStringLiteral("sidepanel.right_hides"), state() == QLatin1String("collapsed"));

  // Fullscreen (the window layer's state; no real fullscreen in this tier).
  controller->setFullScreen(true);
  hoverBody(window, body->width() - 3);
  settle(250);
  expect(QStringLiteral("sidepanel.fullscreen_stays_hidden"), state() == QLatin1String("collapsed") && !panel->isVisible());
  controller->setFullScreen(false);

  // The real pointer (opt-in): Qt events for Chromium, the 10 Hz poll over
  // WebKit's native page.
#if defined(DALINIRA_TEST_XTEST)
  if (qEnvironmentVariableIntValue("DALINIRA_TEST_REAL_POINTER") == 1) {
    window.activateWindow();
    const bool active = waitUntil([&] { return window.isActiveWindow(); }, 2000);
    const QPoint original = QCursor::pos();
    bool revealed = false;
    bool hidden = false;
    if (active) {
      moveRealPointer(body->mapToGlobal(QPoint(body->width() - 2, body->height() / 2)));
      revealed = waitState(QStringLiteral("revealed"), 2000);
      moveRealPointer(body->mapToGlobal(QPoint(body->width() / 3, body->height() / 2)));
      hidden = waitState(QStringLiteral("collapsed"), 2000);
      moveRealPointer(original);
    }
    result(QStringLiteral("sidepanel.real_pointer_reveal"), active && revealed);
    result(QStringLiteral("sidepanel.real_pointer_hide"), active && hidden);
  }
#endif

  // The setting in Settings → Appearance: off applies at once.
  window.showSettings(SettingsPage::Category::Appearance);
  auto *settingsPage = internalPage<SettingsPage>(window, QStringLiteral("settings"));
  auto *toggle = child<QCheckBox>(settingsPage, QStringLiteral("settings-side-panel-auto-hide"));
  CHECK(toggle);
  expect(QStringLiteral("sidepanel.setting_shown"), toggle->isChecked() && toggle->isEnabled());
  toggle->click();
  settle(300);
  window.switchTab(siteIndex);
  settle(300);
  expect(QStringLiteral("sidepanel.setting_off_docked"), !toggle->isChecked() &&
         !dalinira::layout::sidePanelAutoHideEnabled(BrowserLayoutMode::VerticalTabsRight) && state() == QLatin1String("docked") &&
         layoutState(window) == QLatin1String("right"));
  // Classic tabs never get a side panel from the setting.
  BrowserWindow::applySidePanelAutoHideToAllWindows(true);
  window.applyLayoutMode(BrowserLayoutMode::Classic);
  settle(300);
  hoverBody(window, 2);
  int classicNavigation = 0;
  for (const char *name : {"backBtn", "forwardBtn", "reloadBtn", "bookmark-button", "mainMenuButton"})
    if (auto *button = window.findChild<QToolButton *>(QString::fromLatin1(name));
        button && navBar->isAncestorOf(button) && button->isVisible())
      ++classicNavigation;
  expect(QStringLiteral("sidepanel.classic_untouched"), state() == QLatin1String("none") &&
         layoutState(window) == QLatin1String("classic") && !panel->isVisible() && classicNavigation == 5);
  BrowserWindow::applySidePanelAutoHideToAllWindows(false);
  CHECK(!g_expectFailed);
}

// ---------------------------------------------------------------------------
// Phase (one engine) and driver
// ---------------------------------------------------------------------------

int runPhase(const QString &testCase, const QString &engine, int argc, char *argv[]) {
  const bool webkit = engine == QLatin1String("webkit");
  const EngineType engineType = webkit ? EngineType::WebKit : EngineType::Chromium;
  const auto plan = dalinira::engine::planDisplay(engineType, dalinira::engine::DisplayEnvironment::fromProcess());
  if (!plan.engineSupported) {
    std::cout << "[SKIP] " << plan.note.toStdString() << std::endl;
    return 77;
  }
  dalinira::engine::applyDisplayPlan(plan);
  dalinira::application_identity::apply();
  dalinira::engine::EngineBackend::setSelectedEngineType(engineType);
  if (webkit) QCoreApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings);
  else {
    dalinira::WebEngineHardwareAcceleration::applyPrivacyFlags();  // as main() does
    registerDaliNiraUrlSchemes();
  }
  QApplication app(argc, argv);
  dalinira::application_identity::apply();
  // As main() does (the About page shows it).
  if (const QIcon appIcon = BrowserIcons::appIcon(); !appIcon.isNull()) app.setWindowIcon(appIcon);
  g_tag = QStringLiteral("[%1/%2]").arg(testCase, engine);
  log(QStringLiteral("display backend: %1").arg(dalinira::engine::activeDisplayBackend()));
  // A user's QT_QPA_PLATFORM=wayland keeps Chromium on native Wayland.
  if (!webkit && qgetenv("QT_QPA_PLATFORM").startsWith("wayland"))
    CHECK(dalinira::engine::activeDisplayBackend() == QLatin1String("wayland"));
  LocalSite site;

  const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
  QDir().mkpath(dataDir);
  BrowserProfileService profileService(dataDir, nullptr, &app);
  dalinira::engine::EngineStorageManager engineStorage(dataDir);
  if (!webkit) {
    auto backend = std::make_unique<dalinira::engine::ChromiumBackend>(profileService.profile(), &app);
    CHECK(backend->initialize(engineStorage));
    dalinira::engine::EngineBackend::registerBackend(EngineType::Chromium, std::move(backend));
  }
#if defined(DALINIRA_BUILD_WEBKIT)
  else {
    auto backend = std::make_unique<dalinira::engine::WebKitBackend>(&app);
    CHECK(backend->initialize(engineStorage));
    CHECK(backend->enableNetworkFiltering());
    dalinira::engine::EngineBackend::registerBackend(EngineType::WebKit, std::move(backend));
    profileService.applyEngineCookiePolicy();  // as main() does
  }
#else
  else {
    std::cout << "[SKIP] built without WebKit" << std::endl;
    return 77;
  }
#endif

  TabManager tabManager(&app);
  tabManager.performanceManager()->setPolicyMode(dalinira::PerformancePolicyMode::Off);
  SessionStore sessionStore(dataDir, engine);
  BrowserServices services;
  services.profile = webkit ? nullptr : profileService.profile();
  services.profileService = &profileService;
  services.tabManager = &tabManager;
  services.sessionStore = &sessionStore;

  if (testCase == QLatin1String("screenshots")) {
    // The media pages need the services main() creates.
    services.audioEffects = new WebAudioEffectsController(&app);
    services.eqPresetRepo = new EqPresetRepository;
    services.songFinderSettings = new SongFinderSettings(&app);
    services.songRecognition = new SongRecognitionService(services.songFinderSettings, &app);
    services.mediaDownload = new MediaDownloadService(dataDir, &app);
  }
  auto *window = new BrowserWindow(services);
#if defined(DALINIRA_BUILD_WEBKIT)
  if (webkit) {
    g_failureShutdown = [] {
      std::cout << "[INFO] failed check: shutting WebKit down as main() does" << std::endl;
      // As main() does: collect the windows first (deleting one also deletes
      // top-level widgets it owns), then close and delete those still alive.
      QList<QPointer<BrowserWindow>> browserWindows;
      for (QWidget *widget : QApplication::topLevelWidgets())
        if (auto *browserWindow = qobject_cast<BrowserWindow *>(widget)) browserWindows.append(browserWindow);
      for (const QPointer<BrowserWindow> &browserWindow : std::as_const(browserWindows)) {
        if (!browserWindow) continue;
        browserWindow->close();
        delete browserWindow.data();
      }
      dalinira::engine::EngineBackend::shutdown();
      const bool gone = dalinira::engine::WebKitBackend::waitForRetainedWebProcesses();
      std::cout << "[INFO] WebKit web processes " << (gone ? "exited" : "STILL RUNNING") << " before exit" << std::endl;
    };
  }
#endif
  if (testCase == QLatin1String("screenshots")) {
    // Documentation frame: 16:10, kept above other windows while captured.
    window->setWindowFlag(Qt::WindowStaysOnTopHint, true);
    window->resize(1280, 800);
    if (QScreen *screen = QGuiApplication::primaryScreen())
      window->move(screen->availableGeometry().center() - QPoint(640, 400));
  } else {
    window->resize(1000, 700);
    // The side-panel case reads the panel's pointer state. The X server sends
    // crossing events at the real pointer, so a window placed under the
    // user's resting pointer saw it over the panel (kept open) or over the
    // page (hidden) depending on where it rested. Keep the window clear of it.
    if (testCase == QLatin1String("sidepanel")) {
      if (QScreen *screen = QGuiApplication::primaryScreen()) {
        const QRect available = screen->availableGeometry();
        const QPoint pointer = QCursor::pos();
        for (const QPoint &corner : {available.topLeft(), available.topRight() - QPoint(999, 0),
                                     available.bottomLeft() - QPoint(0, 699),
                                     available.bottomRight() - QPoint(999, 699)}) {
          if (!QRect(corner, QSize(1000, 700)).adjusted(-40, -40, 40, 40).contains(pointer)) {
            window->move(corner);
            break;
          }
        }
      }
    }
  }
  window->show();
  // The layout case starts as a first run: onboarding is the first page.
  if (testCase == QLatin1String("layout")) window->showOnboarding();
  else window->ensureInitialTab();
  CHECK(waitUntil([&] { return window->windowHandle() && window->windowHandle()->isExposed(); }, 10000));
  window->activateWindow();
  waitUntil([&] { return window->isActiveWindow(); }, 3000);

  if (testCase == QLatin1String("popup")) runPopupCase(*window, profileService, site, webkit);
  else if (testCase == QLatin1String("scripts")) runScriptsCase(*window, profileService, site, webkit);
  else if (testCase == QLatin1String("media")) runMediaCase(*window, site, webkit);
  else if (testCase == QLatin1String("privacy")) runPrivacyCase(*window, profileService, site, webkit);
  else if (testCase == QLatin1String("network")) runNetworkCase(*window, site, webkit);
  else if (testCase == QLatin1String("fingerprint")) runFingerprintCase(*window, profileService, site, webkit);
  else if (testCase == QLatin1String("layout")) runLayoutCase(*window, profileService, site, webkit, dataDir);
  else if (testCase == QLatin1String("audit")) runAuditCase(*window, profileService, site, webkit, dataDir);
  else if (testCase == QLatin1String("about")) runAboutLinksCase(*window, webkit);
  else if (testCase == QLatin1String("translate")) runTranslationPopupCase(*window, site, webkit);
  else if (testCase == QLatin1String("find")) runFindInPageCase(*window, site, webkit);
  else if (testCase == QLatin1String("shortcuts")) runShortcutsCase(*window, webkit);
  else if (testCase == QLatin1String("sidepanel")) runSidePanelCase(*window, site, webkit);
  else if (testCase == QLatin1String("screenshots")) runScreenshotsCase(*window, profileService, site, webkit);
  else CHECK(false && "unknown case");

  // Self-test of the failure path (a failed check after a case): opt-in.
  CHECK(!qEnvironmentVariableIsSet("DALINIRA_PARITY_FORCE_FAIL") && "forced failure (DALINIRA_PARITY_FORCE_FAIL)");
  g_failureShutdown = nullptr;  // the regular shutdown follows
  log(QStringLiteral("closing window"));
  window->close();
  delete window;
  dalinira::engine::EngineBackend::shutdown();
#if defined(DALINIRA_BUILD_WEBKIT)
  if (webkit) {
    // As main() does: retained (service worker) web processes end first.
    QElapsedTimer timer;
    timer.start();
    CHECK(dalinira::engine::WebKitBackend::waitForRetainedWebProcesses());
    log(QStringLiteral("WebKit web processes gone %1 ms after shutdown").arg(timer.elapsed()));
  }
#endif
  log(QStringLiteral("PASS"));
  return 0;
}

int runGui(const QString &testCase, int argc, char *argv[]) {
  QCoreApplication app(argc, argv);
  // QT_QPA_PLATFORM=wayland: Chromium on native Wayland, WebKit through
  // XWayland (its display plan), as DaliNira runs them.
  const QByteArray qpa = qgetenv("QT_QPA_PLATFORM").split(';').first().trimmed();
  if (qpa == "wayland" && qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) {
    std::cout << "[SKIP] not a Wayland session" << std::endl;
    return 77;
  }
  QTemporaryDir home;
  CHECK(home.isValid());
  std::cout << "=== ENGINE PARITY: " << testCase.toStdString() << " (QT_QPA_PLATFORM=" << qpa.constData()
            << ") ===" << std::endl;
  QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
  QString flags = environment.value(QStringLiteral("QTWEBENGINE_CHROMIUM_FLAGS"));
  if (flags.contains(QStringLiteral("--disable-gpu"))) {
    flags.remove(QStringLiteral("--disable-gpu"));
    environment.insert(QStringLiteral("QTWEBENGINE_CHROMIUM_FLAGS"), flags.simplified());
  }
  QStringList engines{QStringLiteral("chromium")};
#if defined(DALINIRA_BUILD_WEBKIT)
  engines << QStringLiteral("webkit");
#endif
  // DALINIRA_PARITY_ENGINE=chromium|webkit runs one engine's phase only.
  const QString only = qEnvironmentVariable("DALINIRA_PARITY_ENGINE");
  if (!only.isEmpty()) {
    CHECK(engines.contains(only));
    engines = {only};
  }
  const QString logDir = QCoreApplication::applicationDirPath() + QStringLiteral("/gui-test-logs");
  QDir().mkpath(logDir);
  QFile logFile(logDir + QStringLiteral("/engine-parity-%1%2.log")
                            .arg(testCase, qpa == "wayland" ? QStringLiteral("-wayland") : QString()));
  CHECK(logFile.open(QIODevice::WriteOnly | QIODevice::Truncate));

  QMap<QString, QMap<QString, QString>> results;  // engine -> name -> value
  QStringList order;
  static const QRegularExpression resultLine(QStringLiteral("^\\[RESULT\\] ([^=]+)=(.*)$"));
  static const QRegularExpression knownLine(QStringLiteral("^\\[KNOWN\\] ([^=]+)=(.*)$"));
  QMap<QString, QString> known;
  for (const QString &engine : engines) {
    // A fresh profile per engine: no state carries over between phases.
    environment.insert(QStringLiteral("XDG_CONFIG_HOME"), home.filePath(engine + QStringLiteral("/config")));
    environment.insert(QStringLiteral("XDG_DATA_HOME"), home.filePath(engine + QStringLiteral("/data")));
    environment.insert(QStringLiteral("XDG_CACHE_HOME"), home.filePath(engine + QStringLiteral("/cache")));
    QProcess child;
    child.setProcessEnvironment(environment);
    child.setProcessChannelMode(QProcess::MergedChannels);
    QByteArray output;
    QObject::connect(&child, &QProcess::readyRead, [&] {
      const QByteArray chunk = child.readAll();
      output += chunk;
      logFile.write(chunk);
      logFile.flush();
      std::cout << chunk.constData() << std::flush;
    });
    QEventLoop loop;
    QObject::connect(&child, &QProcess::finished, &loop, &QEventLoop::quit);
    QObject::connect(&child, &QProcess::errorOccurred, &loop, &QEventLoop::quit);
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    child.start(QCoreApplication::applicationFilePath(), {QStringLiteral("--phase"), testCase, engine});
    timeout.start(150000);
    loop.exec();
    if (child.state() != QProcess::NotRunning) {
      std::cout << "[FAIL] " << engine.toStdString() << " timed out" << std::endl;
      child.kill();
      child.waitForFinished(5000);
      return 1;
    }
    output += child.readAll();
    // A web process that lost DaliNira's network filter runs unfiltered.
    if (output.contains("network filter unavailable")) {
      std::cout << "[FAIL] " << engine.toStdString() << ": a web process lost the network filter" << std::endl;
      return 1;
    }
    if (output.contains("X Window System error") || output.contains("BadMatch")) {
      std::cout << "[FAIL] " << engine.toStdString() << ": X error in the log" << std::endl;
      return 1;
    }
    if (child.exitStatus() != QProcess::NormalExit || child.exitCode() != 0) {
      std::cout << "[FAIL] " << engine.toStdString() << " exit " << child.exitCode() << std::endl;
      return child.exitCode() == 77 ? 77 : 1;
    }
    for (const QByteArray &line : output.split('\n')) {
      const auto knownMatch = knownLine.match(QString::fromUtf8(line).trimmed());
      if (knownMatch.hasMatch()) known[knownMatch.captured(1)] = knownMatch.captured(2);
      const auto match = resultLine.match(QString::fromUtf8(line).trimmed());
      if (!match.hasMatch()) continue;
      results[engine][match.captured(1)] = match.captured(2);
      if (!order.contains(match.captured(1))) order << match.captured(1);
    }
  }

  bool same = true;
  std::cout << "=== PARITY (" << testCase.toStdString() << ") ===" << std::endl;
  for (const QString &name : order) {
    const QString reference = results[engines.first()].value(name, QStringLiteral("<missing>"));
    QString row = QStringLiteral("%1: %2=%3").arg(name, engines.first(), reference);
    bool rowSame = true;
    for (const QString &engine : engines.mid(1)) {
      const QString value = results[engine].value(name, QStringLiteral("<missing>"));
      row += QStringLiteral("  %1=%2").arg(engine, value);
      rowSame = rowSame && value == reference;
    }
    if (!rowSame && known.contains(name)) {
      std::cout << "  [KNOWN] " << row.toStdString() << "  (" << known.value(name).toStdString() << ")" << std::endl;
      logFile.write(("[KNOWN DIFF] " + row + "\n").toUtf8());
      continue;
    }
    std::cout << (rowSame ? "  [SAME] " : "  [DIFF] ") << row.toStdString() << std::endl;
    if (!rowSame) logFile.write(("[DIFF] " + row + "\n").toUtf8());
    same = same && rowSame;
  }
  logFile.write(same ? "=== PARITY OK ===\n" : "=== PARITY DIFFERENCES ===\n");
  if (!same) {
    std::cout << "[FAIL] the engines differ" << std::endl;
    return 1;
  }
  std::cout << "=== ENGINE PARITY " << testCase.toStdString() << " PASSED ===" << std::endl;
  return 0;
}

}  // namespace

int main(int argc, char *argv[]) {
  for (int i = 1; i < argc; ++i) {
    const QByteArray arg(argv[i]);
    if (arg == "--gui" && i + 1 < argc) return runGui(QString::fromLocal8Bit(argv[i + 1]), argc, argv);
    if (arg == "--phase" && i + 2 < argc)
      return runPhase(QString::fromLocal8Bit(argv[i + 1]), QString::fromLocal8Bit(argv[i + 2]), argc, argv);
  }
  std::cout << "usage: --gui popup|scripts|media|layout|privacy|network|fingerprint|audit|about|translate|find|shortcuts|sidepanel" << std::endl;
  return 2;
}
