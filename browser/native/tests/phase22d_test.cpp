#ifdef NDEBUG
#undef NDEBUG
#endif
#include <QApplication>
#include <QCheckBox>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPushButton>
#include <QTemporaryDir>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QTimer>
#include <QWebEnginePage>
#include <QWebEngineView>
#include <QWebEngineProfile>
#include <QWebEngineScriptCollection>
#include <QNetworkReply>
#include <QNetworkAccessManager>
#include <QJsonDocument>
#include <QJsonArray>
#include <QPointer>
#include <QSharedPointer>
#include <cassert>
#include <cstring>
#include <iostream>
#include "dalinira_blocker_service.h"
#include "dalinira_blocker_shield_button.h"
#include "search_suggestion_service.h"
#include "newtab/new_tab_suggest_command.h"
#include "glow_toggle_switch.h"

static void wait(int ms) { QEventLoop loop; QTimer::singleShot(ms,&loop,&QEventLoop::quit); loop.exec(); }
struct JavaScriptCallState { QVariant result; bool completed = false; };
static QVariant js(QWebEnginePage &page, const QString &script, quint32 world = 0, int timeoutMs = 5000) {
  auto state=QSharedPointer<JavaScriptCallState>::create();QEventLoop loop;const QPointer<QEventLoop> guardedLoop(&loop);
  page.runJavaScript(script,world,[state,guardedLoop](const QVariant &v){state->result=v;state->completed=true;if(guardedLoop)guardedLoop->quit();});
  if(!state->completed){QTimer::singleShot(qMax(0,timeoutMs),&loop,&QEventLoop::quit);loop.exec();}return state->result;
}
static bool waitForJs(QWebEnginePage &page, const QString &condition, quint32 world = 0, int timeoutMs = 5000) {
  QElapsedTimer timer;timer.start();do{const int remaining=timeoutMs-static_cast<int>(timer.elapsed());if(remaining<=0)break;if(js(page,condition,world,qMin(1000,remaining)).toBool())return true;const int waitMs=qMin(50,timeoutMs-static_cast<int>(timer.elapsed()));if(waitMs>0)wait(waitMs);}while(timer.elapsed()<timeoutMs);return false;
}
static void click(QWidget *widget, QPoint point) {
  const auto global = widget->mapToGlobal(point);
  QMouseEvent down(QEvent::MouseButtonPress,point,global,Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
  QMouseEvent up(QEvent::MouseButtonRelease,point,global,Qt::LeftButton,Qt::NoButton,Qt::NoModifier);
  QApplication::sendEvent(widget,&down);QApplication::sendEvent(widget,&up);
}
#include "suggestion_test_transport.h"
static void suggestions() {
  Network network;SearchSuggestionService service(nullptr,&network);QObject owner;
  int calls=0;QStringList results;
  auto done=[&](const QStringList &list){++calls;results=list;};
  service.request(&owner,"hav","Google",false,done);wait(240);assert(network.requests==0);
  service.setEnabled(true);
  service.request(&owner,"ha","Google",false,done);wait(70);
  service.request(&owner,"hav","Google",false,done);wait(160);assert(network.requests==0);
  wait(100);assert(network.requests==1);assert(results==QStringList({"hava","hava durumu"}));
  assert(network.last.url().host()=="suggestqueries.google.com");
  assert(network.last.attribute(QNetworkRequest::CookieLoadControlAttribute).toInt()==QNetworkRequest::Manual);
  assert(network.last.attribute(QNetworkRequest::CookieSaveControlAttribute).toInt()==QNetworkRequest::Manual);
  assert(network.last.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt()==QNetworkRequest::ManualRedirectPolicy);
  assert(!network.last.hasRawHeader("Cookie")&&!network.last.hasRawHeader("Authorization"));
  for(const QString &engine : {"Google","DuckDuckGo","Startpage"}) assert(SearchSuggestionService::endpoint(engine,"hav").scheme()=="https");
  assert(SearchSuggestionService::endpoint("Startpage","hav").toString()=="https://www.startpage.com/osuggestions?q=hav");
  assert(!SearchSuggestionService::endpoint("Mojeek","hav").isValid());
  assert(SearchSuggestionService::endpoint("Brave Search","hav").host()=="suggestqueries.google.com");
  assert(SearchSuggestionService::endpoint("Bing","hav").host()=="suggestqueries.google.com");
  assert(!SearchSuggestionService::endpoint("https://127.0.0.1/","hav").isValid());
  assert(SearchSuggestionService::parseResponse("not json").isEmpty());
  assert(SearchSuggestionService::parseResponse(QByteArray(65537,'a')).isEmpty());
  assert(SearchSuggestionService::parseResponse(R"([{"phrase":"one"},{"phrase":"one"},{"phrase":"two"}])").size()==2);
  for(const QString &query : {"","javascript:foo","data:text/html,hi","file:///tmp/a","qrc:/secret","user:password@example.com"}) assert(!SearchSuggestionService::safeQuery(query));
  const int before=network.requests;
  service.request(&owner,"private","Google",true,done);wait(230);assert(network.requests==before);
  service.request(&owner,"cancel","DuckDuckGo",false,done);service.setEnabled(false);wait(230);assert(network.requests==before);
  service.setEnabled(true);network.delay=500;network.body=R"(["old",["old result"]])";
  service.request(&owner,"old","Google",false,done);wait(230);
  network.delay=20;network.body=R"(["latest",["latest result"]])";
  service.request(&owner,"latest","DuckDuckGo",false,done);wait(300);assert(results==QStringList{"latest result"});
  wait(350);assert(results==QStringList{"latest result"});
  network.body=QByteArray(65537,'x');service.request(&owner,"large","Google",false,done);wait(270);assert(results.isEmpty());
  network.delay=10000;service.request(&owner,"timeout","Google",false,done);wait(4900);assert(results.isEmpty());
  // Empty input and anything the omnibox would open as an address never leave the browser.
  network.delay=30;int sent=network.requests;
  for(const QString &query : {"","   ","github.com","www.example.org/path","https://example.com","http://x.y",
                              "localhost:3000","127.0.0.1","8.8.8.8","[::1]","sub.example.co.uk"}){
    assert(!SearchSuggestionService::endpoint("Google",query).isValid());
    calls=0;service.request(&owner,query,"Google",false,done);wait(240);
    assert(network.requests==sent);assert(calls==1&&results.isEmpty());
  }
  for(const QString &query : {"github","hava durumu","c++ tutorial","ışık","3.5 kg"}) assert(SearchSuggestionService::endpoint("Google",query).isValid());
  // Turkish input is UTF-8 percent-encoded; a fast typing burst sends one request for the final text.
  network.body=R"(["çiğ",["çiğköfte tarifi","çiğ süt","ÇİĞ"]])";
  service.request(&owner,QStringLiteral("ç"),"Google",false,done);wait(50);
  service.request(&owner,QStringLiteral("çi"),"Google",false,done);wait(50);
  service.request(&owner,QStringLiteral("çiğ"),"Google",false,done);wait(300);
  assert(network.requests==sent+1);
  assert(network.last.url().toString(QUrl::FullyEncoded).endsWith(QStringLiteral("&q=%C3%A7i%C4%9F")));
  assert(results==QStringList({QStringLiteral("çiğköfte tarifi"),QStringLiteral("çiğ süt"),QStringLiteral("ÇİĞ")}));
  // A malformed reply yields no suggestions and leaves the service usable.
  network.body="<html>error</html>";service.request(&owner,"broken","Google",false,done);wait(300);assert(results.isEmpty());
  network.body=R"(["ok",["ok google"]])";service.request(&owner,"ok","Google",false,done);wait(300);
  assert(results==QStringList{"ok google"});
  // Both engines' New Tab transports share one parser for dalinira://suggest.
  using Op=NewTabSuggestCommand::Op;
  auto command=NewTabSuggestCommand::parse(QUrl(QStringLiteral("dalinira://suggest?op=query&cap=x&id=7&q=%C3%A7i%C4%9Fk%C3%B6fte%20tarifi")));
  assert(command.op==Op::Query&&command.id==7&&command.text==QStringLiteral("çiğköfte tarifi"));
  assert(NewTabSuggestCommand::parse(QUrl("dalinira://suggest?op=query&id=-1&q=a")).op==Op::Invalid);
  assert(NewTabSuggestCommand::parse(QUrl("dalinira://suggest?op=query&id=x&q=a")).op==Op::Invalid);
  assert(NewTabSuggestCommand::parse(QUrl("dalinira://suggest?op=query&id=1&q="+QString(257,'a'))).op==Op::Invalid);
  assert(NewTabSuggestCommand::parse(QUrl("dalinira://navigate?op=query&id=1&q=a")).op==Op::Invalid);
  command=NewTabSuggestCommand::parse(QUrl("dalinira://suggest?op=consent&enabled=true"));assert(command.op==Op::Consent&&command.enabled);
  command=NewTabSuggestCommand::parse(QUrl("dalinira://suggest?op=consent&enabled=false"));assert(command.op==Op::Consent&&!command.enabled);
  assert(NewTabSuggestCommand::parse(QUrl("dalinira://suggest?op=consent&enabled=1")).op==Op::Invalid);
  command=NewTabSuggestCommand::parse(QUrl("dalinira://suggest?op=activate&q=%20a%20%20b%20"));assert(command.op==Op::Activate&&command.text=="a b");
  assert(NewTabSuggestCommand::parse(QUrl("dalinira://suggest?op=activate&q=%20")).op==Op::Invalid);
  command=NewTabSuggestCommand::parse(QUrl("dalinira://suggest?op=delete-history&id=3&q=old&input=ol"));
  assert(command.op==Op::DeleteHistory&&command.id==3&&command.text=="old"&&command.input=="ol");
  std::cout<<"Suggestion consent, providers, debounce, cancellation, limits, parsing and timeout passed\n";
}
static void shields() {
  QTemporaryDir directory;DaliNiraBlockerService service(directory.path());
  service.settings()->setAutoReloadOnModeChange(false);
  DaliNiraBlockerQuickPopup popup(&service);popup.updateForHost("youtube.com",12);popup.show();wait(30);
  const QString snapshotPath = qEnvironmentVariable("DALINIRA_PHASE22D_SNAPSHOT");
  if (!snapshotPath.isEmpty()) {
    assert(popup.grab().save(snapshotPath));
  }
  int writes=0;QObject::connect(service.settings(),&DaliNiraBlockerSettings::sitePoliciesChanged,[&]{++writes;});
  for(auto *toggle:popup.findChildren<QCheckBox *>()) {
    // Keep the master on while exercising each subordinate row.
    for(const QPoint point : {QPoint(3,toggle->height()/2),QPoint(toggle->width()-5,toggle->height()/2),QPoint(toggle->width()/2,toggle->height()/2)}) {
      for(int i=0;i<10;++i){const bool old=toggle->isChecked();const int before=writes;click(toggle,point);assert(toggle->isChecked()!=old);assert(writes==before+1);}
    }
    const bool old=toggle->isChecked();
    QKeyEvent down(QEvent::KeyPress,Qt::Key_Space,Qt::NoModifier),up(QEvent::KeyRelease,Qt::Key_Space,Qt::NoModifier);
    QApplication::sendEvent(toggle,&down);QApplication::sendEvent(toggle,&up);assert(toggle->isChecked()!=old);
    QKeyEvent enter(QEvent::KeyPress,Qt::Key_Return,Qt::NoModifier);QApplication::sendEvent(toggle,&enter);assert(toggle->isChecked()==old);
  }
  SitePolicy other;other.whitelisted=true;service.settings()->setSitePolicy("facebook.com",other);
  for(int i=0;i<5;++i){popup.hide();popup.show();popup.updateForHost("facebook.com",0);assert(!popup.findChild<QCheckBox *>("adblock-master-toggle")->isChecked());popup.updateForHost("youtube.com",0);}
  popup.findChild<QToolButton *>("adblock-site-reset")->click();
  assert(!service.settings()->sitePolicies().contains("youtube.com"));assert(service.settings()->sitePolicy("facebook.com").whitelisted);
  assert(service.settings()->protectionEnabled());
  assert(DaliNiraBlockerSettings::normalizeSiteHost("HTTPS://WWW.YouTube.COM.:443/")=="youtube.com");
  assert(DaliNiraBlockerSettings::normalizeSiteHost("facebook.com@youtube.com").isEmpty());
  assert(DaliNiraBlockerSettings::normalizeSiteHost("youtube.com/path").isEmpty());
  assert(DaliNiraBlockerSettings::normalizeSiteHost("[::1]:443")=="::1");
  service.settings()->setMode(DaliNiraBlockerMode::Aggressive);service.settings()->resetToDefaults();
  assert(service.settings()->mode()==DaliNiraBlockerDefaults::Mode&&service.settings()->sitePolicies().isEmpty());
  assert(!DaliNiraBlockerEngine::validateCustomFilterLine(QString(9000,'a')).isEmpty());
  service.evaluateRequest(QUrl("https://user:password@www.google.com/search?q=private-query&token=secret#fragment"),0,QUrl("https://www.google.com/"),1);
  assert(service.recentLogs(1).first().requestUrl==QStringLiteral("https://www.google.com/search"));
  assert(!DaliNiraBlockerSettings::findSitePolicy("127.0.0.1",{}).has_value());
  assert(DaliNiraBlockerSettings::normalizeSiteHost("::1")==QStringLiteral("::1"));
  assert(DaliNiraBlockerSettings::normalizeSiteHost("co.uk").isEmpty());
  assert(DaliNiraBlockerSettings::normalizeSiteHost(QString::fromUtf8("BÜCHER.de"))==QStringLiteral("xn--bcher-kva.de"));
  DaliNiraBlockerEngine securityEngine;
  securityEngine.addCustomFilterLines({QStringLiteral("||notgoogle.com^"),QStringLiteral("||asset.github.io^$third-party")});
  assert(securityEngine.evaluate(QUrl("https://notgoogle.com/ad.js"),DaliNiraBlockerResourceType::Script,"notgoogle.com",DaliNiraBlockerMode::Ideal,SitePolicy{}).action==DaliNiraBlockerAction::Block);
  assert(securityEngine.evaluate(QUrl("https://asset.github.io/ad.js"),DaliNiraBlockerResourceType::Script,"other.github.io",DaliNiraBlockerMode::Ideal,SitePolicy{}).action==DaliNiraBlockerAction::Block);
  std::cout<<"Shields mouse/keyboard, repeated clicks, reopen, host switching, resets and normalization passed\n";
}
class FixturePage final : public QWebEnginePage {
 public: using QWebEnginePage::QWebEnginePage;
 protected: void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel,const QString &message,int,const QString &) override { std::cerr << "fixture JS: " << message.toStdString() << std::endl; }
};
static void cosmetics(const QString &host) {
  QTemporaryDir directory;DaliNiraBlockerService service(directory.path());
  QWebEngineProfile profile;FixturePage page(&profile);QWebEngineView view;view.setPage(&page);view.resize(800,600);view.show();
  const bool youtube=host=="youtube.com";
  const QString html=youtube ? QStringLiteral("<html><body><ytd-rich-item-renderer id='normal'>normal video</ytd-rich-item-renderer><ytd-reel-item-renderer id='short'>Short</ytd-reel-item-renderer><ytd-video-renderer id='search'>Search result</ytd-video-renderer><ytd-rich-item-renderer id='ad'><ytd-ad-slot-renderer></ytd-ad-slot-renderer></ytd-rich-item-renderer><ytd-rich-item-renderer id='recycled'><span>Normal</span></ytd-rich-item-renderer></body></html>") : QStringLiteral("<html><body><div role='feed'><div role='article' id='normal'>friend post Sponsored discussion</div><div role='article' id='ad' data-ad-preview='message'>Sponsorlu</div><div role='article' id='recycled'>normal</div></div><div id='messenger'>Messenger</div></body></html>");
  page.scripts().insert(service.createCosmeticScriptForHost(host));
  QEventLoop loop;QObject::connect(&page,&QWebEnginePage::loadFinished,&loop,&QEventLoop::quit);
  page.setHtml(html,QUrl("https://"+host+"/"));QTimer::singleShot(8000,&loop,&QEventLoop::quit);loop.exec();wait(350);
  const QString adHidden=QStringLiteral("(()=>{const p=document.getElementById('ad');const c=p&&p.querySelector('ytd-ad-slot-renderer,[data-ad-preview],[data-ad-comet-preview]');return !!p&&(getComputedStyle(p).display==='none'||(!!c&&getComputedStyle(c).display==='none'));})()");
  const QString recycledHidden=QStringLiteral("(()=>{const p=document.getElementById('recycled');const c=p&&p.querySelector('ytd-ad-slot-renderer,[data-ad-preview],[data-ad-comet-preview]');return !!p&&(getComputedStyle(p).display==='none'||(!!c&&getComputedStyle(c).display==='none'));})()");
  assert(waitForJs(page,adHidden));
  assert(js(page,"getComputedStyle(document.getElementById('normal')).display!=='none'").toBool());
  if(youtube){assert(js(page,"getComputedStyle(document.getElementById('short')).display!=='none'&&getComputedStyle(document.getElementById('search')).display!=='none'").toBool());}
  const QString marker=youtube?"<ytd-ad-slot-renderer></ytd-ad-slot-renderer>":"<div data-ad-preview='message'>Sponsored</div>";
  js(page,QString("document.getElementById('recycled').innerHTML=%1[0]").arg(QString::fromUtf8(QJsonDocument(QJsonArray{marker}).toJson(QJsonDocument::Compact))));wait(250);
  assert(waitForJs(page,recycledHidden));
  js(page,"document.getElementById('recycled').textContent='now a normal post';document.getElementById('ad').style.display='block';document.getElementById('dalinira-adblock-cosmetic').remove();window.dispatchEvent(new Event('yt-navigate-finish'));window.dispatchEvent(new Event('popstate'));");wait(300);
  assert(waitForJs(page,"getComputedStyle(document.getElementById('recycled')).display!=='none'"));
  assert(waitForJs(page,adHidden));
  js(page,service.createCosmeticScriptForHost(host).sourceCode(),QWebEngineScript::ApplicationWorld);wait(250);
  assert(js(page,"document.querySelectorAll('#dalinira-adblock-cosmetic').length").toInt()==1);
  assert(!js(page,"!!window.__daliniraCosmeticRuntime").toBool());
  assert(js(page,"!!window.__daliniraCosmeticRuntime.observer",QWebEngineScript::ApplicationWorld).toBool());
  service.settings()->setProtectionEnabled(false);assert(service.createCosmeticScriptForHost(host).sourceCode().isEmpty());
  page.scripts().clear();page.setHtml(html);wait(350);assert(waitForJs(page,"!"+adHidden));
  std::cout<<host.toStdString()<<" dynamic/recycled DOM, style recovery, route lifecycle, isolation and OFF passed\n";
}

static void counter_invariants() {
  QTemporaryDir directory;
  DaliNiraBlockerService service(directory.path());

  const quint64 tabA = 1001;
  const quint64 tabB = 1002;
  service.updateTabUrl(tabA, QUrl("https://www.facebook.com/reel/1598051558482993"));
  service.updateTabUrl(tabB, QUrl("https://example.com/page"));

  // Initial counters must be 0
  assert(service.statsForTab(tabA).totalBlocked() == 0);
  assert(service.statsForTab(tabB).totalBlocked() == 0);

  // Invariant 1: Blocked network request => counter + 1
  service.reportBlockedEvent(tabA, DaliNiraBlockType::NetworkAd, 1, "https://an.facebook.com/ad");
  assert(service.statsForTab(tabA).blockedRequests == 1);
  assert(service.statsForTab(tabA).totalBlocked() == 1);

  // Invariant 2: Allowed request => counter does not change
  // (Calling evaluateRequest on non-blocked URL doesn't increment stats)
  service.evaluateRequest(QUrl("https://static.xx.fbcdn.net/rsrc.php/v3/y1.js"), 1, QUrl("https://www.facebook.com/"), tabA);
  assert(service.statsForTab(tabA).totalBlocked() == 1);

  // Invariant 4: Tab isolation: Tab A block => Tab B counter does not change
  assert(service.statsForTab(tabB).totalBlocked() == 0);

  // Invariant 11: Real cosmetic hit => counter + 1
  service.reportBlockedEvent(tabA, DaliNiraBlockType::Cosmetic, 1, "div[data-pagelet*='Reel']");
  assert(service.statsForTab(tabA).blockedCosmetics == 1);
  assert(service.statsForTab(tabA).totalBlocked() == 2);
  assert(service.statsForTab(tabB).totalBlocked() == 0);

  // Invariant 7: SPA / same-document navigation (Facebook Reel to another Reel) => counter is preserved
  service.updateTabUrl(tabA, QUrl("https://www.facebook.com/reel/9999999999"));
  assert(service.statsForTab(tabA).totalBlocked() == 2);

  // Host normalization: facebook.com <-> www.facebook.com navigation does not wipe counter
  service.updateTabUrl(tabA, QUrl("https://facebook.com/reel/9999999999"));
  assert(service.statsForTab(tabA).totalBlocked() == 2);

  // Invariant 6: True cross-site top-level navigation => counter resets
  service.updateTabUrl(tabA, QUrl("https://news.ycombinator.com/"));
  assert(service.statsForTab(tabA).totalBlocked() == 0);

  // Restore Facebook on tabA
  service.updateTabUrl(tabA, QUrl("https://www.facebook.com/"));
  assert(service.statsForTab(tabA).totalBlocked() == 0);

  // Invariant 8: Protection disabled => counter does NOT increase
  service.settings()->setProtectionEnabled(false);
  service.reportBlockedEvent(tabA, DaliNiraBlockType::NetworkAd, 1);
  service.reportBlockedEvent(tabA, DaliNiraBlockType::Cosmetic, 1);
  assert(service.statsForTab(tabA).totalBlocked() == 0);
  service.settings()->setProtectionEnabled(true);

  // Invariant 9: Ads blocking disabled for site => ad block events ignored
  SitePolicy policy;
  policy.adBlocking = false;
  policy.trackerProtection = true;
  service.settings()->setSitePolicy("facebook.com", policy);
  service.reportBlockedEvent(tabA, DaliNiraBlockType::NetworkAd, 1);
  service.reportBlockedEvent(tabA, DaliNiraBlockType::Cosmetic, 1);
  assert(service.statsForTab(tabA).totalBlocked() == 0);

  // Invariant 10: Tracker blocking still works if trackerProtection is true
  service.reportBlockedEvent(tabA, DaliNiraBlockType::Tracker, 1);
  assert(service.statsForTab(tabA).blockedTrackers == 1);
  assert(service.statsForTab(tabA).totalBlocked() == 1);

  // Tracker blocking disabled => tracker block event ignored
  policy.trackerProtection = false;
  service.settings()->setSitePolicy("facebook.com", policy);
  service.reportBlockedEvent(tabA, DaliNiraBlockType::Tracker, 1);
  assert(service.statsForTab(tabA).blockedTrackers == 1);
  assert(service.statsForTab(tabA).totalBlocked() == 1);

  // Lifecycle: Tab unregister clears stats and frees tab association
  service.unregisterTab(tabA);
  assert(service.statsForTab(tabA).totalBlocked() == 0);

  std::cout << "Counter invariants (network, cosmetic, tracker, tab isolation, SPA navigation, site policy, unregister) passed\n";
}

int main(int argc,char **argv){
  QApplication app(argc,argv);
  shields();
  suggestions();
  counter_invariants();
  cosmetics("youtube.com");
  cosmetics("facebook.com");
  return 0;
}
