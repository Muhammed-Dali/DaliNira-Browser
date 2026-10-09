#include <QApplication>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>
#include <QUrl>
#include <QUrlQuery>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QEventLoop>
#include <QTimer>
#include <QImage>
#include <QIcon>
#include <QBuffer>

#include <cassert>
#include <iostream>

#include "core/browser_profile_data_provider.h"
#include "core/browser_profile_service.h"
#include "core/navigation_candidate.h"
#include "core/suggestion_remove_delegate.h"
#include "newtab/new_tab_suggest_command.h"
#include "newtab/new_tab_shortcuts.h"
#include "engine/engine_backend.h"
#include <QListView>
#include <QStandardItemModel>
#include <QStyleFactory>
#include <QCompleter>
#include <QLineEdit>
#include "core/search_engine_definition.h"
#include "newtab/new_tab_html.h"
#include "newtab/new_tab_background_store.h"
#include "newtab/new_tab_scheme.h"
#include "blocker/dalinira_blocker_service.h"
#include "blocker/dalinira_blocker_settings.h"
#include "blocker/dalinira_blocker_types.h"

namespace {

class EvidencePage : public QWebEnginePage {
 public:
  using QWebEnginePage::QWebEnginePage;
 protected:
  void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel, const QString &message, int, const QString &) override {
    std::cerr << "JS: " << message.toStdString() << std::endl;
  }
};

class MockProfileDataProvider final : public dalinira::core::IBrowserProfileDataProvider {
 public:
  QList<BrowserHistoryEntry> history;
  QList<BrowserFrequentSite> frequent;
  QList<QUrl> bookmarkUrls;

  QList<BrowserHistoryEntry> recentHistory() const override { return history; }
  QList<BrowserFrequentSite> frequentSites(int limit) const override {
    return limit > 0 ? frequent.mid(0, limit) : QList<BrowserFrequentSite>{};
  }
  QList<QUrl> bookmarks() const override { return bookmarkUrls; }
};

QJsonObject storedHistoryEntry(const QString &directory) {
  QSettings settings(directory + QStringLiteral("/browser-preferences.ini"), QSettings::IniFormat);
  settings.sync();
  const QJsonDocument document = QJsonDocument::fromJson(
      settings.value(QStringLiteral("history/entries")).toByteArray());
  assert(document.isArray() && !document.array().isEmpty());
  return document.array().first().toObject();
}

void testSuggestionRemoval() {
  using namespace dalinira::engine;
  // Exercise storage without launching Chromium. Shared service = both engines.
  EngineBackend::setSelectedEngineType(EngineType::WebKit);
  QTemporaryDir directory;
  const QUrl url(QStringLiteral("https://history.example/item"));
  const QUrl other(QStringLiteral("https://history.example/other"));
  {
    BrowserProfileService service(directory.path(), nullptr);
    service.recordSearch(QStringLiteral("saved query"));
    service.recordHistory(url, QStringLiteral("Item"), true);
    service.recordHistory(other, QStringLiteral("Other"), true);
    assert(!service.removeSuggestion(QStringLiteral("remote"), QStringLiteral("saved query"), url));
    assert(!service.removeSuggestion(QStringLiteral("bookmark"), QStringLiteral("Item"), url));
    assert(service.removeSuggestion(QStringLiteral("search-history"), QStringLiteral("saved query"), QUrl{}));
    assert(service.removeSuggestion(QStringLiteral("history"), QStringLiteral("Item"), url));
    assert(service.recentSearches().isEmpty());
    for (const auto &entry : service.recentHistory()) assert(entry.url != url);
    const QUrl root(QStringLiteral("https://history.example/"));
    assert(service.removeSuggestion(QStringLiteral("frequent"), QStringLiteral("Site"), root));
    for (const auto &entry : service.frequentSites()) assert(entry.url != root);
  }
  {
    BrowserProfileService reopened(directory.path(), nullptr);
    assert(reopened.recentSearches().isEmpty());
    assert(reopened.recentHistory().size() == 1 && reopened.recentHistory().first().url == other);
    assert(!reopened.removeSuggestion(QStringLiteral("history"), {}, url));
  }
  {
    BrowserProfileService privateService(directory.path(), nullptr, nullptr, true);
    assert(!privateService.removeSuggestion(QStringLiteral("history"), {}, other));
  }
  auto command = NewTabSuggestCommand::parse(QUrl(QStringLiteral("dalinira://suggest?op=delete-suggestion&type=history&url=https%3A%2F%2Fhistory.example%2Fitem&id=1")));
  assert(command.op == NewTabSuggestCommand::Op::DeleteSuggestion && command.url == url);
  assert(NewTabSuggestCommand::parse(QUrl(QStringLiteral("dalinira://suggest?op=delete-suggestion&type=remote&q=test&id=1"))).op == NewTabSuggestCommand::Op::Invalid);
  assert(NewTabSuggestCommand::parse(QUrl(QStringLiteral("dalinira://suggest?op=delete-suggestion&type=history&url=javascript%3Aalert(1)&id=1"))).op == NewTabSuggestCommand::Op::Invalid);

  assert(SuggestionRemoveDelegate::usesHistoryIcon(QStringLiteral("history")));
  assert(SuggestionRemoveDelegate::usesHistoryIcon(QStringLiteral("search-history")));
  assert(SuggestionRemoveDelegate::usesHistoryIcon(QStringLiteral("frequent")));
  assert(!SuggestionRemoveDelegate::usesHistoryIcon(QStringLiteral("remote")));
  assert(!SuggestionRemoveDelegate::usesHistoryIcon(QStringLiteral("search")));

  QListView view;
  QStandardItemModel model;
  auto *history = new QStandardItem(QStringLiteral("History"));
  history->setData(true, SuggestionRemoveDelegate::RemovableRole);
  model.appendRow(history);
  model.appendRow(new QStandardItem(QStringLiteral("Provider")));
  view.setModel(&model);
  int removals = 0;
  int activations = 0;
  QObject::connect(&view, &QListView::clicked, [&] { ++activations; });
  auto *delegate = new SuggestionRemoveDelegate(&view, [&](const QModelIndex &index) {
    assert(index.row() == 0);
    ++removals;
    model.removeRow(index.row());
  });
  view.setItemDelegate(delegate);
  view.resize(300, 100);
  view.show();
  QApplication::processEvents();
  const auto click = [&](const QPoint &pos) {
    QMouseEvent press(QEvent::MouseButtonPress, pos, pos, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, pos, pos, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(view.viewport(), &press);
    QApplication::sendEvent(view.viewport(), &release);
  };
  click(SuggestionRemoveDelegate::removeRect(view.visualRect(model.index(1, 0))).center());
  assert(removals == 0 && activations == 1);
  const auto hover = [&](const QPoint &pos) {
    QMouseEvent move(QEvent::MouseMove, pos, pos, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(view.viewport(), &move);
  };
  hover(view.visualRect(model.index(0, 0)).center());
  assert(view.currentIndex() == model.index(0, 0));
  hover(view.visualRect(model.index(1, 0)).center());
  assert(view.currentIndex() == model.index(1, 0));
  hover(SuggestionRemoveDelegate::removeRect(view.visualRect(model.index(0, 0))).center());
  assert(view.currentIndex() == model.index(0, 0) && activations == 1);
  QEvent leave(QEvent::Leave);
  QApplication::sendEvent(view.viewport(), &leave);
  assert(!view.currentIndex().isValid() && view.selectionModel()->selectedIndexes().isEmpty());
  hover(view.visualRect(model.index(0, 0)).center());
  assert(view.currentIndex() == model.index(0, 0));

  // The selected panel must extend beneath the remove control, including
  // the trailing edge. Text elision must not shrink that panel.
  auto *style = QStyleFactory::create(QStringLiteral("Fusion"));
  assert(style);
  style->setParent(&view);
  view.setStyle(style);
  QStyleOptionViewItem option;
  option.initFrom(&view);
  option.widget = &view;
  option.rect = QRect(0, 0, 300, 30);
  option.state |= QStyle::State_Selected | QStyle::State_Active | QStyle::State_Enabled;
  option.palette.setColor(QPalette::Highlight, QColor(50, 100, 150));
  QImage painted(option.rect.size(), QImage::Format_ARGB32);
  painted.fill(Qt::black);
  QPainter painter(&painted);
  delegate->paint(&painter, option, model.index(0, 0));
  painter.end();
  const QRect remove = SuggestionRemoveDelegate::removeRect(option.rect);
  assert(option.rect.contains(remove));
  assert(remove.center().y() == option.rect.center().y());
  // Fusion shades the highlight slightly; compare the panel at the same y.
  const QColor panel = painted.pixelColor(100, option.rect.center().y());
  assert(panel != QColor(Qt::black));
  assert(painted.pixelColor(option.rect.right() - 2, option.rect.center().y()) == panel);
  assert(painted.pixelColor(remove.left() + 1, option.rect.center().y()) == panel);
  // Pressing X and releasing over the label cancels removal and activation.
  const QPoint cancelX = SuggestionRemoveDelegate::removeRect(view.visualRect(model.index(0, 0))).center();
  const QPoint label = view.visualRect(model.index(0, 0)).center();
  QMouseEvent cancelPress(QEvent::MouseButtonPress, cancelX, cancelX, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QMouseEvent cancelRelease(QEvent::MouseButtonRelease, label, label, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(view.viewport(), &cancelPress);
  QApplication::sendEvent(view.viewport(), &cancelRelease);
  assert(removals == 0 && activations == 1 && model.rowCount() == 2);
  click(SuggestionRemoveDelegate::removeRect(view.visualRect(model.index(0, 0))).center());
  assert(removals == 1 && activations == 1 && model.rowCount() == 1);
  // Use the actual QCompleter popup/proxy too: moving to the X must
  // preserve the typed input, and removing must not emit activation.
  QLineEdit input;
  QStandardItemModel completions;
  auto *saved = new QStandardItem(QStringLiteral("saved query"));
  saved->setData(true, SuggestionRemoveDelegate::RemovableRole);
  completions.appendRow(saved);
  auto *secondSaved = new QStandardItem(QStringLiteral("second saved query"));
  secondSaved->setData(true, SuggestionRemoveDelegate::RemovableRole);
  completions.appendRow(secondSaved);
  completions.appendRow(new QStandardItem(QStringLiteral("provider query")));
  QCompleter completer(&completions);
  completer.setCompletionMode(QCompleter::UnfilteredPopupCompletion);
  input.setCompleter(&completer);
  auto *popup = completer.popup();
  int completed = 0;
  int highlighted = 0;
  QObject::connect(&completer, qOverload<const QModelIndex &>(&QCompleter::activated), [&] { ++completed; });
  // Observe preview even when the offscreen platform changes input focus.
  QObject::connect(&completer, qOverload<const QString &>(&QCompleter::highlighted), [&](const QString &text) {
    ++highlighted;
    input.setText(text);
  });
  int popupRemovals = 0;
  popup->setItemDelegate(new SuggestionRemoveDelegate(popup, [&](const QModelIndex &index) {
    assert(index.data().toString() == (popupRemovals == 0
        ? QStringLiteral("saved query") : QStringLiteral("second saved query")));
    ++popupRemovals;
    // Match the browser's clear/repopulate/complete refresh after deletion.
    completions.clear();
    if (popupRemovals == 1) {
      auto *remaining = new QStandardItem(QStringLiteral("second saved query"));
      remaining->setData(true, SuggestionRemoveDelegate::RemovableRole);
      completions.appendRow(remaining);
    }
    completions.appendRow(new QStandardItem(QStringLiteral("provider query")));
    completer.complete();
  }));
  input.setText(QStringLiteral("typed"));
  input.show();
  input.setFocus();
  completer.complete();
  QApplication::processEvents();
  const auto savedIndex = popup->model()->index(0, 0);
  const QPoint x = SuggestionRemoveDelegate::removeRect(popup->visualRect(savedIndex)).center();
  const QPoint secondX = SuggestionRemoveDelegate::removeRect(popup->visualRect(popup->model()->index(1, 0))).center();
  QMouseEvent secondMove(QEvent::MouseMove, secondX, secondX, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(popup->viewport(), &secondMove);
  QMouseEvent move(QEvent::MouseMove, x, x, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(popup->viewport(), &move);
  assert(popup->currentIndex() == savedIndex && input.text() == QStringLiteral("typed") && highlighted == 0);
  assert(popup->viewport()->cursor().shape() == Qt::PointingHandCursor);
  QApplication::sendEvent(popup->viewport(), &leave);
  assert(!popup->currentIndex().isValid() && popup->selectionModel()->selectedIndexes().isEmpty());
  assert(popup->viewport()->cursor().shape() != Qt::PointingHandCursor);
  assert(input.text() == QStringLiteral("typed") && highlighted == 0);
  QApplication::sendEvent(popup->viewport(), &move);
  assert(popup->currentIndex() == savedIndex && popup->viewport()->cursor().shape() == Qt::PointingHandCursor);
  QMouseEvent press(QEvent::MouseButtonPress, x, x, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QMouseEvent release(QEvent::MouseButtonRelease, x, x, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(popup->viewport(), &press);
  QApplication::sendEvent(popup->viewport(), &release);
  assert(completions.rowCount() == 2 && popupRemovals == 1 && completed == 0 && highlighted == 0 && input.text() == QStringLiteral("typed"));
  QMouseEvent doubleClick(QEvent::MouseButtonDblClick, x, x, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(popup->viewport(), &doubleClick);
  QApplication::sendEvent(popup->viewport(), &release);
  assert(completions.rowCount() == 1 && popupRemovals == 2 && completed == 0 && highlighted == 0 && input.text() == QStringLiteral("typed"));
  QApplication::sendEvent(popup->viewport(), &move);
  assert(popup->viewport()->cursor().shape() != Qt::PointingHandCursor);
  // Keyboard selection still provides the normal completion preview.
  popup->selectionModel()->clearSelection();
  popup->setCurrentIndex(popup->model()->index(0, 0));
  assert(highlighted > 0 && input.text() == QStringLiteral("provider query"));
  QApplication::sendEvent(popup->viewport(), &press);
  QApplication::sendEvent(popup->viewport(), &release);
  assert(completed == 1 && popupRemovals == 2);
  EngineBackend::setSelectedEngineType(EngineType::Chromium);
}

void testHistoryTitleAndTypedEvidence() {
  QTemporaryDir directory;
  assert(directory.isValid());
  BrowserProfileService service(directory.path(), nullptr);
  const QUrl url(QStringLiteral("https://www.ebay.com/item/42#details"));

  service.recordHistory(url, QString{}, false);
  const auto initialHistory = service.recentHistory();
  const auto initialSites = service.frequentSites();
  assert(initialHistory.size() == 1);
  assert(initialSites.size() == 1);
  assert(initialHistory.first().title == QStringLiteral("www.ebay.com"));
  const QDateTime visitedAt = initialHistory.first().visitedAt;
  const QDateTime lastVisitedAt = initialSites.first().lastVisitedAt;
  const int visitCount = initialSites.first().visitCount;

  assert(service.updateHistoryTitle(url, QStringLiteral("  eBay — Electronics  ")));
  const auto updatedHistory = service.recentHistory();
  const auto updatedSites = service.frequentSites();
  assert(updatedHistory.first().title == QStringLiteral("eBay — Electronics"));
  assert(updatedSites.first().title == QStringLiteral("eBay — Electronics"));
  assert(updatedHistory.first().visitedAt == visitedAt);
  assert(updatedSites.first().lastVisitedAt == lastVisitedAt);
  assert(updatedSites.first().visitCount == visitCount);

  assert(!service.updateHistoryTitle(url, QString{}));
  assert(!service.updateHistoryTitle(url, QStringLiteral("Yeni Sekme")));
  assert(service.recentHistory().first().title == QStringLiteral("eBay — Electronics"));

  service.recordHistory(url, QString{}, true);
  assert(service.frequentSites().first().typedCount == 1);
  QJsonObject stored = storedHistoryEntry(directory.path());
  assert(stored.value(QStringLiteral("isTyped")).toBool());
  assert(stored.value(QStringLiteral("typedCount")).toInt() == 1);

  service.recordHistory(url, QString{}, true);
  assert(service.frequentSites().first().typedCount == 2);
  stored = storedHistoryEntry(directory.path());
  assert(stored.value(QStringLiteral("typedCount")).toInt() == 2);
}

void testBackwardCompatibleTypedCount() {
  QTemporaryDir directory;
  assert(directory.isValid());
  const QString preferencesPath = directory.path() + QStringLiteral("/browser-preferences.ini");
  {
    QSettings settings(preferencesPath, QSettings::IniFormat);
    const QJsonObject oldSite{
        {QStringLiteral("key"), QStringLiteral("example.com")},
        {QStringLiteral("url"), QStringLiteral("https://example.com/")},
        {QStringLiteral("iconLookupUrl"), QStringLiteral("https://example.com/page")},
        {QStringLiteral("title"), QStringLiteral("Example")},
        {QStringLiteral("visitCount"), 3},
        {QStringLiteral("lastVisitedAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};
    settings.setValue(QStringLiteral("history/frequentSites"),
                      QJsonDocument(QJsonArray{oldSite}).toJson(QJsonDocument::Compact));
    settings.sync();
  }

  BrowserProfileService service(directory.path(), nullptr);
  const auto sites = service.frequentSites();
  assert(sites.size() == 1);
  assert(sites.first().typedCount == 0);
}

void testSearchHistoryPersistenceAndClearing() {
  QTemporaryDir directory;
  assert(directory.isValid());
  BrowserProfileService service(directory.path(), nullptr);

  service.recordSearch(QStringLiteral("  arch   aur giriş  "));
  service.recordSearch(QStringLiteral("youtube müzik"));
  service.recordSearch(QStringLiteral("arch aur giriş"));
  const QStringList recent = service.recentSearches();
  assert(recent.size() == 2);
  assert(recent.first() == QStringLiteral("arch aur giriş"));
  assert(service.recentSearches(QStringLiteral("aur"), 5) == QStringList{QStringLiteral("arch aur giriş")});
  assert(service.removeSearchHistory(QStringLiteral("youtube müzik")));
  assert(!service.removeSearchHistory(QStringLiteral("bulunmayan sorgu")));
  assert(service.recentSearches() == QStringList{QStringLiteral("arch aur giriş")});
  {
    BrowserProfileService reloaded(directory.path(), nullptr);
    assert(reloaded.recentSearches() == QStringList{QStringLiteral("arch aur giriş")});
  }

  service.clearSearchHistory();
  assert(service.recentSearches().isEmpty());
  service.recordSearch(QStringLiteral("temizlenecek sorgu"));
  service.clearHistory();
  assert(service.recentSearches().isEmpty());

  BrowserProfileService privateService(directory.path() + QStringLiteral("/private"), nullptr, nullptr, true);
  privateService.recordSearch(QStringLiteral("gizli sorgu"));
  assert(privateService.recentSearches().isEmpty());
}

void testAntiPoisoningConfidence() {
  dalinira::core::NavigationCandidate passive;
  passive.hasExactTokenMatch = true;
  passive.isExactDomainMatch = true;
  passive.visitCount = 1;
  assert(dalinira::core::CandidateScoringConfig::calculateConfidence(passive) == 0.40);
  assert(dalinira::core::CandidateScoringConfig::calculateConfidence(passive)
         < dalinira::core::CandidateScoringConfig::kMinConfidenceThreshold);

  dalinira::core::NavigationCandidate typed = passive;
  typed.typedCount = 1;
  typed.hasTypedEvidence = true;
  assert(dalinira::core::CandidateScoringConfig::calculateConfidence(typed) == 0.85);
  assert(dalinira::core::CandidateScoringConfig::calculateConfidence(typed)
         >= dalinira::core::CandidateScoringConfig::kMinConfidenceThreshold);
}

void testNewTabDataAndScriptSafety() {
  MockProfileDataProvider profile;
  profile.frequent.append({QStringLiteral("Yeni Sekme"), QUrl(QStringLiteral("file:///tmp/nope")),
                           QUrl{}, 99, 0, QDateTime::currentDateTimeUtc()});
  profile.frequent.append({QStringLiteral("  Example  "),
                           QUrl(QStringLiteral("https://user:password@example.com/?token=secret#section")),
                           QUrl{}, 8, 2, QDateTime::currentDateTimeUtc()});
  for (int i = 0; i < 7; ++i) {
    profile.frequent.append({QStringLiteral("Site %1").arg(i),
                             QUrl(QStringLiteral("https://site%1.example/").arg(i)), QUrl{},
                             i + 1, 0, QDateTime::currentDateTimeUtc()});
  }
  profile.bookmarkUrls = {
      QUrl(QStringLiteral("javascript:alert(1)")),
      QUrl(QStringLiteral("https://user:password@bookmark.example/path#private")),
      QUrl(QStringLiteral("http://two.example/")), QUrl(QStringLiteral("https://three.example/")),
      QUrl(QStringLiteral("https://four.example/")), QUrl(QStringLiteral("https://five.example/")),
      QUrl(QStringLiteral("https://six.example/")), QUrl(QStringLiteral("https://seven.example/"))};

  const QJsonArray frequent = collectNewTabFrequentSites(&profile);
  const QJsonArray bookmarks = collectNewTabBookmarks(&profile);
  assert(frequent.size() == 6);
  assert(bookmarks.size() == 6);
  assert(frequent.first().toObject().value(QStringLiteral("name")).toString() == QStringLiteral("Example"));
  const QString frequentUrl = frequent.first().toObject().value(QStringLiteral("url")).toString();
  assert(!frequentUrl.contains(QStringLiteral("user")));
  assert(!frequentUrl.contains(QStringLiteral("password")));
  assert(!frequentUrl.contains(QStringLiteral("token")));
  const QUrl faviconUrl(frequent.first().toObject().value(QStringLiteral("icon")).toString());
  const QUrl faviconPage(QUrlQuery(faviconUrl).queryItemValue(QStringLiteral("page"), QUrl::FullyDecoded));
  assert(faviconPage.scheme() == QStringLiteral("https"));
  assert(faviconPage.host() == QStringLiteral("example.com"));
  assert(faviconPage.userName().isEmpty() && faviconPage.password().isEmpty());
  assert(!faviconPage.toString().contains(QStringLiteral("token")));
  const QString bookmarkUrl = bookmarks.first().toObject().value(QStringLiteral("url")).toString();
  assert(!bookmarkUrl.contains(QStringLiteral("user")));
  assert(!bookmarkUrl.contains(QStringLiteral("password")));
  assert(!bookmarkUrl.contains(QLatin1Char('#')));

  const QString maliciousTitle = QStringLiteral("attack</script><script>alert(1)</script>");
  const QJsonArray malicious{QJsonObject{{QStringLiteral("url"), QStringLiteral("https://safe.example/")},
                                         {QStringLiteral("name"), maliciousTitle},
                                         {QStringLiteral("title"), maliciousTitle}}};
  const QString html = newTabHtml(QStringLiteral("Google"), malicious, bookmarks, 37, 4);
  assert(html.contains(QStringLiteral("search-history")));
  assert(html.contains(QStringLiteral("suggestionCommand('activate'")));
  assert(html.contains(QStringLiteral("suggestion-remove")));
  assert(html.contains(QStringLiteral("suggestionCommandUrl('delete-suggestion'")));
  assert(html.contains(QStringLiteral("attack<\\/script><script>alert(1)<\\/script>")));
  assert(!html.contains(maliciousTitle));
  assert(html.contains(QStringLiteral("window.daliniraTopSiteSources")));
  assert(html.contains(QStringLiteral("id=\"protection-card-value\">37")));
  assert(html.contains(QStringLiteral("Engellenen öğeler")));
  assert(html.contains(QStringLiteral("id=\"downloads-card-value\">4")));
  assert(html.contains(QStringLiteral("İstatistik kartlarını göster")));
  assert(html.contains(QStringLiteral("const button=document.createElement('a')")));
  assert(html.contains(QStringLiteral("button.href=suggestionCommandUrl('activate'")));
  assert(html.contains(QStringLiteral("button.onpointerdown=event=>")));
  assert(html.contains(QStringLiteral("remove.onpointerdown=event=>")));
  assert(html.contains(QStringLiteral("query.addEventListener('input',scheduleSuggestions)")));
  assert(html.contains(QStringLiteral("suggestionRequestTimer=setTimeout(requestSuggestions,80)")));
  assert(html.contains(QStringLiteral("if(document.activeElement!==query&&!suggestionList.matches(':hover'))closeSuggestions()")));
  assert(html.contains(QStringLiteral("query.setSelectionRange(suggestionTypedValue.length,completion.length)")));
  assert(html.contains(QStringLiteral("id=\"dalinira-navigation-bridge\"")));
  assert(html.contains(QStringLiteral("bridge.src='dalinira://navigate?q='")));
  assert(html.contains(QStringLiteral("encodeURIComponent(window.navigationCapability)")));

  const QString updateScript = newTabTopSitesUpdateScript(frequent, bookmarks);
  assert(updateScript.contains(QStringLiteral("window.renderFrequentSites")));
  assert(updateScript.contains(QStringLiteral("https://example.com/")));
  assert(newTabProtectionStatsUpdateScript(91, 6).contains(QStringLiteral("daliniraSetProtectionStats(91,6)")));
}

}  // namespace

static void testPlaceholdersAndCachedFavicon() {
  const QString generatedNewTab = newTabHtml(
      QStringLiteral("DuckDuckGo"), {}, {}, 0, 0, 0, true, true,
      QStringLiteral("all_time"), QStringLiteral("asset-capability"));
  assert(generatedNewTab.contains(QStringLiteral("dalinira://newtab-background?op=")));
  assert(generatedNewTab.contains(QStringLiteral("const managedBackgroundCapability=\"asset-capability\"")));
  assert(generatedNewTab.contains(QStringLiteral("managed-background-thumbnail'+managedQuery")));
  assert(generatedNewTab.contains(QStringLiteral("dalinira://background-preference?source=")));
  assert(!generatedNewTab.contains(QStringLiteral("__BACKGROUND_VISIBLE_DEFAULT__")));
  assert(!generatedNewTab.contains(QStringLiteral("__BACKGROUND_PREF_SET_DEFAULT__")));
  assert(!generatedNewTab.contains(QStringLiteral("__BACKGROUND_SOURCE_DEFAULT__")));
  assert(!generatedNewTab.contains(QStringLiteral("window.daliniraNewTabCommand=")));
  assert(dalinira::core::searchEngineIconAsset(QStringLiteral("Google")) == QStringLiteral("google.ico"));
  assert(dalinira::core::searchEngineIconAsset(QStringLiteral("DuckDuckGo")) == QStringLiteral("duckduckgo.ico"));
  assert(dalinira::core::searchEngineIconAsset(QStringLiteral("Startpage")) == QStringLiteral("startpage.ico"));
  assert(dalinira::core::searchEngineIconAsset(QStringLiteral("Mojeek")) == QStringLiteral("mojeek.ico"));
  assert(dalinira::core::searchEngineIconAsset(QStringLiteral("Brave Search")) == QStringLiteral("google.ico"));
  assert(dalinira::core::searchEngineIconAsset(QStringLiteral("Bing")) == QStringLiteral("google.ico"));
  assert(dalinira::core::searchEngineResourcePath(QStringLiteral("Google")) == QStringLiteral(":/search-engines/google.ico"));
  assert(searchEnginePlaceholder(QStringLiteral("Google")) == QStringLiteral("Google'da arayın veya URL'yi yazın"));
  assert(searchEnginePlaceholder(QStringLiteral("DuckDuckGo")) == QStringLiteral("DuckDuckGo'da arayın veya URL'yi yazın"));
  assert(searchEnginePlaceholder(QStringLiteral("Startpage")) == QStringLiteral("Startpage'de arayın veya URL'yi yazın"));
  assert(searchEnginePlaceholder(QStringLiteral("Mojeek")) == QStringLiteral("Mojeek'te arayın veya URL'yi yazın"));
  assert(searchEnginePlaceholder(QStringLiteral("Brave Search")) == QStringLiteral("Google'da arayın veya URL'yi yazın"));
  assert(searchEnginePlaceholder(QStringLiteral("Bing")) == QStringLiteral("Google'da arayın veya URL'yi yazın"));
  QTemporaryDir dir;
  BrowserProfileService service(dir.path(), nullptr);
  assert(service.searchEngine() == QStringLiteral("DuckDuckGo"));
  service.setSearchEngine(QStringLiteral("Startpage"));
  assert(service.searchEngine() == QStringLiteral("Startpage"));
  service.setSearchEngine(QStringLiteral("Mojeek"));
  assert(service.searchEngine() == QStringLiteral("Mojeek"));
  service.setSearchEngine(QStringLiteral("Brave Search"));
  assert(service.searchEngine() == QStringLiteral("Google"));
  service.setSearchEngine(QStringLiteral("Bing"));
  assert(service.searchEngine() == QStringLiteral("Google"));
  service.setSearchEngine(QStringLiteral("DuckDuckGo"));
  QImage image(32, 32, QImage::Format_ARGB32);
  image.fill(Qt::green);
  const QString managedBackgroundFixture = dir.path() + QStringLiteral("/managed-background.png");
  assert(image.save(managedBackgroundFixture, "PNG"));
  assert(service.newTabBackgroundStore()->importImage(managedBackgroundFixture).ok());
  QByteArray png;
  QBuffer buffer(&png);
  buffer.open(QIODevice::WriteOnly);
  assert(image.save(&buffer, "PNG"));
  QTcpServer server;
  assert(server.listen(QHostAddress::LocalHost));
  int requests = 0;
  QObject::connect(&server, &QTcpServer::newConnection, &server, [&] {
    while (auto *socket = server.nextPendingConnection()) {
      QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
        const QByteArray request = socket->readAll();
        if (request.isEmpty()) return;
        ++requests;
        const bool icon = request.startsWith("GET /favicon.png ");
        const QByteArray body = icon ? png : QByteArray("<html><head><link rel='icon' href='/favicon.png'></head><body>Local fixture</body></html>");
        socket->write("HTTP/1.1 200 OK\r\nContent-Type: " + QByteArray(icon ? "image/png" : "text/html")
            + "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        socket->disconnectFromHost();
      });
      QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    }
  });
  EvidencePage page(service.profile());
  const QUrl url(QStringLiteral("http://127.0.0.1:%1/").arg(server.serverPort()));
  QEventLoop iconLoop;
  QObject::connect(&page, &QWebEnginePage::iconChanged, &iconLoop, &QEventLoop::quit);
  QTimer::singleShot(10000, &iconLoop, &QEventLoop::quit);
  page.load(url);
  iconLoop.exec();
  assert(!page.icon().isNull());
  QEventLoop cacheLoop;
  bool cacheHit = false;
  service.profile()->requestIconForPageURL(url, 64, [&](const QIcon &cached, const QUrl &, const QUrl &cachedPage) {
    cacheHit = !cached.isNull() && cachedPage == url;
    cacheLoop.quit();
  });
  QTimer::singleShot(5000, &cacheLoop, &QEventLoop::quit);
  cacheLoop.exec();
  assert(cacheHit);
  service.recordHistory(url, QStringLiteral("Fixture"));
  service.recordHistory(url, QStringLiteral("Fixture"));
  service.recordHistory(QUrl(QStringLiteral("https://uncached.invalid/")), QStringLiteral("Uncached"));
  service.recordHistory(QUrl(QStringLiteral("https://uncached.invalid/")), QStringLiteral("Uncached"));
  const int before = requests;
  QEventLoop loadLoop;
  QObject::connect(&page, &QWebEnginePage::loadFinished, &loadLoop, &QEventLoop::quit);
  QTimer::singleShot(10000, &loadLoop, &QEventLoop::quit);
  // A stale URL parameter must not override the profile's authoritative engine.
  page.load(QUrl(QStringLiteral("dalinira://newtab/?engine=Google")));
  loadLoop.exec();
  QEventLoop evalLoop;
  bool passed = false;
  page.runJavaScript(QStringLiteral("JSON.stringify({placeholder:document.querySelector('#query')?.placeholder,fallback:Array.from(document.querySelectorAll('.shortcut-icon')).some(x=>x.textContent==='U'&&!x.querySelector('img')),width:document.querySelector('.shortcut-icon img')?.naturalWidth,strip:document.querySelector('#shortcuts')?.classList.contains('top-sites-strip'),siteCards:document.querySelectorAll('#shortcut-list .shortcut').length,oldPanel:!!document.querySelector('#top-sites-title, #frequent-settings, .module'),gear:document.querySelector('#customize img')?.getAttribute('src'),backgroundHidden:document.body.classList.contains('background-hidden'),backgroundToggle:document.querySelector('#background-toggle')?.checked,colorBackgrounds:document.querySelectorAll('.color-background-card').length,uploadIcon:!!document.querySelector('#background-upload .upload-icon path'),clockToggle:document.querySelector('#clock-toggle')?.checked,clockCentered:document.querySelector('#clock-widget')?.classList.contains('position-center'),clockStyles:document.querySelectorAll('[data-clock-style]').length,clockPositions:document.querySelectorAll('[data-clock-position]').length})"),
      [&](const QVariant &value) {
        std::cerr << "New Tab evidence: " << value.toString().toStdString() << std::endl;
        const auto result = QJsonDocument::fromJson(value.toString().toUtf8()).object();
        passed = result.value(QStringLiteral("placeholder")).toString() == searchEnginePlaceholder(QStringLiteral("DuckDuckGo"))
            && result.value(QStringLiteral("fallback")).toBool()
            && result.value(QStringLiteral("width")).toInt() > 0
            && result.value(QStringLiteral("strip")).toBool()
            && result.value(QStringLiteral("siteCards")).toInt() > 0
            && !result.value(QStringLiteral("oldPanel")).toBool()
            && result.value(QStringLiteral("gear")).toString() == QStringLiteral("icons/settings.svg")
            && result.value(QStringLiteral("backgroundHidden")).toBool()
            && !result.value(QStringLiteral("backgroundToggle")).toBool()
            && result.value(QStringLiteral("colorBackgrounds")).toInt() == 8
            && result.value(QStringLiteral("uploadIcon")).toBool()
            && result.value(QStringLiteral("clockToggle")).toBool()
            && result.value(QStringLiteral("clockCentered")).toBool()
            && result.value(QStringLiteral("clockStyles")).toInt() == 7  // classic, digital, minimal, flip, wall, modern-analog, neon
            && result.value(QStringLiteral("clockPositions")).toInt() == 6;
        evalLoop.quit();
      });
  QTimer::singleShot(5000, &evalLoop, &QEventLoop::quit);
  evalLoop.exec();
  assert(passed);
  assert(requests == before); // New Tab reads only the existing favicon database.

  QEventLoop runtimeLoop;
  bool runtimePassed = false;
  page.runJavaScript(QStringLiteral(R"JS((()=>{try{
    const input=document.querySelector('#query');
    const clockWidget=document.querySelector('#clock-widget');
    const colorfulBackground=document.querySelector('[data-background="gradient-violet"]');
    colorfulBackground.click();
    if(!clockWidget.classList.contains('position-top-left'))return 'fail@1 '+clockWidget.className;
    document.querySelector('[data-clock-style="digital"]').click();
    document.querySelector('[data-clock-position="bottom-right"]').click();
    document.querySelector('#clock-toggle').click();
    if(!clockWidget.hidden)return 'fail@2';
    document.querySelector('#clock-toggle').click();
    // The user-facing card switches (the combined #cards-toggle is hidden).
    for(const id of ['#downloads-card-toggle','#blocked-card-toggle']){const t=document.querySelector(id);if(t.checked)t.click();}
    const saved=JSON.parse(localStorage.getItem('dalinira.newtab')||'{}');
    if(!saved.backgroundVisible||!saved.backgroundPreferenceSet||saved.backgroundSource!=='gradient-violet'||document.body.classList.contains('background-hidden')||saved.clockStyle!=='digital'||saved.clockPosition!=='bottom-right'||!saved.clock||saved.cards!==false||!document.querySelector('#cards').hidden||!clockWidget.classList.contains('style-digital')||!clockWidget.classList.contains('position-bottom-right'))return 'fail@3 '+JSON.stringify(saved);
    window.daliniraBackgroundResult(true,'',true,123,true);
    if(JSON.parse(localStorage.getItem('dalinira.newtab')).backgroundSource!=='custom'||!document.documentElement.style.getPropertyValue('--new-tab-background').includes('managed-background?v=123')||document.querySelector('#custom-background-card').hidden||document.querySelector('#background-remove').hidden)return 'fail@4';
    document.querySelector('#background-toggle').click();
    if(JSON.parse(localStorage.getItem('dalinira.newtab')).backgroundSource!=='custom'||!document.body.classList.contains('background-hidden'))return 'fail@5';
    // Away from the custom image (8.0 has no separate built-in card) and back.
    document.querySelector('[data-background="solid-teal"]').click();
    document.querySelector('[data-background="custom"]').click();
    if(JSON.parse(localStorage.getItem('dalinira.newtab')).backgroundSource!=='custom'||document.body.classList.contains('background-hidden'))return 'fail@6';
    window.daliniraSetSearchEngine('Google');
    if(input.placeholder!=="Google'da arayın veya URL'yi yazın" || !input.matches(':placeholder-shown'))return 'fail@7';
    input.value='keep this text';
    window.daliniraSetSearchEngine('DuckDuckGo');
    if(input.value!=='keep this text' || input.matches(':placeholder-shown'))return 'fail@8';
    input.value='';
    return input.matches(':placeholder-shown') && input.placeholder==="DuckDuckGo'da arayın veya URL'yi yazın";
  }catch(error){return 'exception: '+error}})())JS"), [&](const QVariant &value) {
    // true, or the label of the first failed step
    std::cerr << "New Tab runtime: " << value.toString().toStdString() << std::endl;
    runtimePassed = value.typeId() == QMetaType::Bool && value.toBool();
    runtimeLoop.quit();
  });
  QTimer::singleShot(5000, &runtimeLoop, &QEventLoop::quit);
  runtimeLoop.exec();
  assert(runtimePassed);

  QEventLoop backgroundImageLoop;
  QTimer backgroundPoll;
  backgroundPoll.setInterval(50);
  bool backgroundImageLoaded = false;
  QObject::connect(&backgroundPoll, &QTimer::timeout, &page, [&] {
    page.runJavaScript(QStringLiteral("document.querySelector('#custom-background-thumbnail')?.naturalWidth>0"),
        [&](const QVariant &value) {
          if (!value.toBool()) return;
          backgroundImageLoaded = true;
          backgroundImageLoop.quit();
        });
  });
  QTimer::singleShot(5000, &backgroundImageLoop, &QEventLoop::quit);
  backgroundPoll.start();
  backgroundImageLoop.exec();
  backgroundPoll.stop();
  assert(backgroundImageLoaded);

  QEventLoop focusLoop;
  bool focusPassed = false;
  page.runJavaScript(QStringLiteral(R"JS((()=>{
    const fail=step=>{console.log('focus-mode-failure:'+step);return false};
    const pageRoot=document.querySelector('#page');
    const input=document.querySelector('#query');
    const strip=document.querySelector('#shortcuts');
    const current=document.querySelector('#engine-current');
    const search=document.querySelector('#search');
    const brand=document.querySelector('.brand');
    pageRoot.style.width='760px'; // Standalone QWebEnginePage has no view width.
    search.style.transition=brand.style.transition='none';
    if(search.offsetWidth!==420||brand.offsetWidth!==36)return fail('compact-size:'+search.offsetWidth+','+brand.offsetWidth);
    if(search.getBoundingClientRect().top-pageRoot.getBoundingClientRect().top>60)return fail('compact-position');
    const gear=document.querySelector('#customize');
    gear.click();
    if(document.querySelector('#customization-overlay').hidden)return fail('gear-open');
    document.querySelector('#customization-close').click();
    if(!document.querySelector('#customization-overlay').hidden)return fail('gear-close');
    input.value='';
    input.focus();
    input.dispatchEvent(new FocusEvent('focus'));
    if(!pageRoot.classList.contains('search-focused'))return fail('focus-state');
    if(search.offsetWidth!==620||brand.offsetWidth!==112)return fail('expanded-size');
    if(!strip.matches('.top-sites-strip'))return fail('strip-class');
    input.blur();
    updateSearchMode();
    if(pageRoot.classList.contains('search-focused'))return fail('blur-exit');
    if(search.offsetWidth!==420||brand.offsetWidth!==36)return fail('compact-restore');
    input.dispatchEvent(new FocusEvent('focus'));
    document.dispatchEvent(new KeyboardEvent('keydown',{key:'Escape',bubbles:true}));
    if(pageRoot.classList.contains('search-focused'))return fail('escape-exit');
    input.dispatchEvent(new FocusEvent('focus'));
    const searchWidth=document.querySelector('#search').getBoundingClientRect().width;
    current.click();
    if(document.querySelector('#engine-menu').hidden||!pageRoot.classList.contains('search-focused'))return fail('picker-state');
    if(document.querySelector('#search').getBoundingClientRect().width!==searchWidth)return fail('picker-layout');
    if(document.querySelector('.engine-option[data-engine="Bing"]')||document.querySelector('.engine-option[data-engine="Brave Search"]'))return fail('removed-engines-present');
    if(!document.querySelector('.engine-option[data-engine="Startpage"]')||!document.querySelector('.engine-option[data-engine="Mojeek"]'))return fail('new-engines-missing');
    document.querySelector('.engine-option[data-engine="Startpage"]').click();
    if(document.querySelector('#engine-current-icon').getAttribute('src')!=='startpage.ico'||input.placeholder!=="Startpage'de arayın veya URL'yi yazın")return fail('startpage-select');
    document.querySelector('.engine-option[data-engine="Mojeek"]').click();
    if(document.querySelector('#engine-current-icon').getAttribute('src')!=='mojeek.ico'||input.placeholder!=="Mojeek'te arayın veya URL'yi yazın")return fail('mojeek-select');
    document.querySelector('.engine-option[data-engine="Google"]').click();
    return (pageRoot.classList.contains('search-focused')
      && document.querySelector('#engine-current-icon').getAttribute('src')==='google.ico'
      && input.placeholder==="Google'da arayın veya URL'yi yazın")||fail('picker-update');
  })())JS"), [&](const QVariant &value) { focusPassed = value.toBool(); focusLoop.quit(); });
  QTimer::singleShot(5000, &focusLoop, &QEventLoop::quit);
  focusLoop.exec();
  assert(focusPassed);

  const QUrl iconUrl(collectNewTabFrequentSites(&service).first().toObject().value(QStringLiteral("icon")).toString());
  for (bool unsafePage : {false, true}) {
    QUrl deniedUrl(iconUrl);
    QUrlQuery query(deniedUrl);
    if (unsafePage) {
      query.removeAllQueryItems(QStringLiteral("page"));
      query.addQueryItem(QStringLiteral("page"), QStringLiteral("file:///tmp/nope"));
    } else {
      query.removeAllQueryItems(QStringLiteral("cap"));
    }
    deniedUrl.setQuery(query);
    EvidencePage denied(service.profile());
    QEventLoop deniedLoop;
    bool rejected = false;
    QObject::connect(&denied, &QWebEnginePage::loadFinished, &deniedLoop, [&](bool ok) {
      rejected = !ok;
      deniedLoop.quit();
    });
    QTimer::singleShot(5000, &deniedLoop, &QEventLoop::quit);
    denied.load(deniedUrl);
    deniedLoop.exec();
    assert(rejected);
  }
}

void testPrivacyAndContentSettingsPolicies() {
  QTemporaryDir directory;
  assert(directory.isValid());

  BrowserProfileService service(directory.path(), nullptr);

  // 1. Permission Defaults
  assert(service.permissionDefaultPolicy(QStringLiteral("geolocation")) == QStringLiteral("prompt"));
  assert(service.permissionDefaultPolicy(QStringLiteral("camera")) == QStringLiteral("prompt"));
  assert(service.permissionDefaultPolicy(QStringLiteral("microphone")) == QStringLiteral("prompt"));
  assert(service.permissionDefaultPolicy(QStringLiteral("notifications")) == QStringLiteral("quiet"));

  service.setPermissionDefaultPolicy(QStringLiteral("geolocation"), QStringLiteral("deny"));
  assert(service.permissionDefaultPolicy(QStringLiteral("geolocation")) == QStringLiteral("deny"));

  service.setPermissionDefaultPolicy(QStringLiteral("camera"), QStringLiteral("deny"));
  assert(service.permissionDefaultPolicy(QStringLiteral("camera")) == QStringLiteral("deny"));

  service.setPermissionDefaultPolicy(QStringLiteral("notifications"), QStringLiteral("deny"));
  assert(service.permissionDefaultPolicy(QStringLiteral("notifications")) == QStringLiteral("deny"));

  // 2. Auto-revoke unused permissions
  assert(service.autoRevokeUnusedPermissions() == true);
  service.setAutoRevokeUnusedPermissions(false);
  assert(service.autoRevokeUnusedPermissions() == false);
  service.setAutoRevokeUnusedPermissions(true);
  assert(service.autoRevokeUnusedPermissions() == true);

  // 3. Content Settings: Cookies
  assert(service.cookiePolicy() == QStringLiteral("third_party"));
  service.setCookiePolicy(QStringLiteral("block_all"));
  assert(service.cookiePolicy() == QStringLiteral("block_all"));
  assert(service.profile()->persistentCookiesPolicy() == QWebEngineProfile::NoPersistentCookies);
  service.setCookiePolicy(QStringLiteral("allow_all"));
  assert(service.cookiePolicy() == QStringLiteral("allow_all"));
  assert(service.profile()->persistentCookiesPolicy() == QWebEngineProfile::ForcePersistentCookies);

  // 4. Content Settings: JavaScript
  assert(service.isJavascriptEnabled() == true);
  assert(service.profile()->settings()->testAttribute(QWebEngineSettings::JavascriptEnabled) == true);
  service.setJavascriptEnabled(false);
  assert(service.isJavascriptEnabled() == false);
  assert(service.profile()->settings()->testAttribute(QWebEngineSettings::JavascriptEnabled) == false);

  // 5. Content Settings: Images
  assert(service.isAutoLoadImagesEnabled() == true);
  assert(service.profile()->settings()->testAttribute(QWebEngineSettings::AutoLoadImages) == true);
  service.setAutoLoadImagesEnabled(false);
  assert(service.isAutoLoadImagesEnabled() == false);
  assert(service.profile()->settings()->testAttribute(QWebEngineSettings::AutoLoadImages) == false);

  // 6. Content Settings: Pop-ups
  assert(service.arePopupsAllowed() == false);
  assert(service.profile()->settings()->testAttribute(QWebEngineSettings::JavascriptCanOpenWindows) == false);
  service.setPopupsAllowed(true);
  assert(service.arePopupsAllowed() == true);
  assert(service.profile()->settings()->testAttribute(QWebEngineSettings::JavascriptCanOpenWindows) == true);

  // 7. Content Settings: Sound & PDF
  assert(service.isSoundAllowed() == true);
  service.setSoundAllowed(false);
  assert(service.isSoundAllowed() == false);

  assert(service.openPdfInBrowser() == true);
  assert(service.profile()->settings()->testAttribute(QWebEngineSettings::PdfViewerEnabled) == true);
  service.setOpenPdfInBrowser(false);
  assert(service.openPdfInBrowser() == false);
  assert(service.profile()->settings()->testAttribute(QWebEngineSettings::PdfViewerEnabled) == false);

  // 8. Persistence across profile reload
  {
    BrowserProfileService reloaded(directory.path(), nullptr);
    assert(reloaded.permissionDefaultPolicy(QStringLiteral("geolocation")) == QStringLiteral("deny"));
    assert(reloaded.permissionDefaultPolicy(QStringLiteral("camera")) == QStringLiteral("deny"));
    assert(reloaded.permissionDefaultPolicy(QStringLiteral("notifications")) == QStringLiteral("deny"));
    assert(reloaded.cookiePolicy() == QStringLiteral("allow_all"));
    assert(reloaded.isJavascriptEnabled() == false);
    assert(reloaded.isAutoLoadImagesEnabled() == false);
    assert(reloaded.arePopupsAllowed() == true);
    assert(reloaded.isSoundAllowed() == false);
    assert(reloaded.openPdfInBrowser() == false);
    assert(reloaded.autoRevokeUnusedPermissions() == true);
  }
}

static void testNewTabTemplateKeepsScriptPlaceholders() {
  // The engine <option>s are filled by name: the page script's own '%1'
  // placeholders (accessible names, tooltips) must reach the page intact.
  for (const QString &engine : {QStringLiteral("Google"), QStringLiteral("DuckDuckGo"),
                                QStringLiteral("Startpage"), QStringLiteral("Mojeek")}) {
    const QString html = newTabHtml(engine);
    assert(html.count(QStringLiteral(".replace('%1',")) == 3);
    assert(!html.contains(QStringLiteral(".replace('',")) && !html.contains(QStringLiteral(".replace(' selected',")));
    assert(html.contains(QStringLiteral("<option selected>%1</option>").arg(engine)));
    assert(html.count(QStringLiteral("<option selected>")) == 1);
    assert(!html.contains(QStringLiteral("__SELECTED_")));
  }
  std::cout << "[PASS] New Tab template keeps the page script's %1 placeholders" << std::endl;
}

static void testPhase2_2C_NewTabCardHidingOnSearchFocus() {
  const QString html = newTabHtml(QStringLiteral("Google"));
  assert(html.contains(QStringLiteral(".cards{position:relative;")));
  assert(html.contains(QStringLiteral("transition:opacity .2s ease,transform .2s ease,visibility 0s linear 0s")));
  assert(html.contains(QStringLiteral(".page.search-focused .cards{opacity:0;transform:translateY(-10px);visibility:hidden;pointer-events:none;")));
  assert(html.contains(QStringLiteral("transition:opacity .18s ease,transform .2s ease,visibility 0s linear .2s")));
  assert(html.contains(QStringLiteral("id=\"cards\"")));
  assert(html.contains(QStringLiteral("id=\"downloads-card-value\"")));
  assert(html.contains(QStringLiteral("id=\"protection-card-value\"")));
  assert(html.contains(QStringLiteral("İndirmeler")));
  assert(html.contains(QStringLiteral("Engellenen öğeler")));
  assert(!html.contains(QStringLiteral("İzleme parametresi koruması")));
  assert(html.contains(QStringLiteral("query.addEventListener('focus'")));
  assert(html.contains(QStringLiteral("classList.add('search-focused')")));
  assert(html.contains(QStringLiteral("query.addEventListener('blur'")));
}

static void testPhase2_2C_SitePolicyPersistenceAndExtendedFields() {
  QTemporaryDir dir;
  const QString iniPath = dir.path() + QStringLiteral("/test-adblock.ini");
  {
    DaliNiraBlockerSettings settings(iniPath);
    SitePolicy p;
    p.adBlocking = true;
    p.trackerProtection = false;
    p.whitelisted = false;
    p.perSiteMode = 2; // Aggressive
    p.blockScripts = true;
    p.blockFingerprinting = true;
    p.upgradeHttps = true;
    p.forgetOnClose = true;
    p.cookiePolicy = QStringLiteral("block_all");
    settings.setSitePolicy(QStringLiteral("youtube.com"), p);
  }
  {
    DaliNiraBlockerSettings reloaded(iniPath);
    const auto loaded = reloaded.sitePolicy(QStringLiteral("youtube.com"));
    assert(loaded.adBlocking == true);
    assert(loaded.trackerProtection == false);
    assert(loaded.whitelisted == false);
    assert(loaded.perSiteMode == 2);
    assert(loaded.blockScripts == true);
    assert(loaded.blockFingerprinting == true);
    assert(loaded.upgradeHttps == true);
    assert(loaded.forgetOnClose == true);
    assert(loaded.cookiePolicy == QStringLiteral("block_all"));
  }
}

static void testPhase2_2D_YouTubeCentralCosmeticLifecycle() {
  QTemporaryDir dir;
  DaliNiraBlockerService service(dir.path());
  
  const QString css = service.cosmeticCssForHost(QStringLiteral("www.youtube.com"));
  assert(!css.isEmpty());
  assert(css.contains(QStringLiteral("#masthead-ad")));
  assert(css.contains(QStringLiteral("ytd-in-feed-ad-layout-renderer")));
  assert(css.contains(QStringLiteral("ytd-rich-item-renderer:has(ytd-in-feed-ad-layout-renderer)")));
  assert(css.contains(QStringLiteral("ytd-rich-item-renderer:has(ytd-ad-slot-renderer)")));
  assert(!css.contains(QStringLiteral("{\"")));
  assert(!css.contains(QStringLiteral("ytd-rich-item-renderer { display: none")));

  const auto ytScripts = service.createScriptingScriptsForHost(QStringLiteral("www.youtube.com"));
  bool hasRuntime = false;
  for (const auto &s : ytScripts) {
    if (s.name() == QStringLiteral("dalinira-adblock-cosmetic")) {
      hasRuntime = true;
      assert(s.worldId() == QWebEngineScript::ApplicationWorld);
      assert(s.injectionPoint() == QWebEngineScript::DocumentCreation);
      const QString code = s.sourceCode();
      assert(code.contains(QStringLiteral("__daliniraCosmeticRuntime")));
      assert(code.contains(QStringLiteral("yt-navigate-finish")));
      assert(code.contains(QStringLiteral("yt-page-data-updated")));
      assert(code.contains(QStringLiteral("MutationObserver")));
      assert(code.contains(QStringLiteral("180")));
    }
  }
  assert(hasRuntime);

  const auto exampleScripts = service.createScriptingScriptsForHost(QStringLiteral("example.com"));
  for (const auto &s : exampleScripts) {
    assert(s.name() != QStringLiteral("dalinira-adblock-youtube-guardian"));
  }
}

static void testPhase2_2C_EvaluateRequestPolicyEnforcement() {
  QTemporaryDir dir;
  DaliNiraBlockerService service(dir.path());

  {
    SitePolicy p;
    p.blockScripts = true;
    p.whitelisted = false;
    service.settings()->setSitePolicy(QStringLiteral("script-test.com"), p);
    
    auto dec = service.evaluateRequest(QUrl(QStringLiteral("https://script-test.com/app.js")),
                                       static_cast<int>(DaliNiraBlockerResourceType::Script),
                                       QUrl(QStringLiteral("https://script-test.com/")), 10);
    assert(dec.action == DaliNiraBlockerAction::Block);

    p.whitelisted = true;
    service.settings()->setSitePolicy(QStringLiteral("script-test.com"), p);
    auto decAllowed = service.evaluateRequest(QUrl(QStringLiteral("https://script-test.com/app.js")),
                                             static_cast<int>(DaliNiraBlockerResourceType::Script),
                                             QUrl(QStringLiteral("https://script-test.com/")), 10);
    assert(decAllowed.action == DaliNiraBlockerAction::Allow);
  }

  {
    SitePolicy up;
    up.upgradeHttps = true;
    up.whitelisted = false;
    service.settings()->setSitePolicy(QStringLiteral("upgrade-test.com"), up);

    auto decUp = service.evaluateRequest(QUrl(QStringLiteral("http://upgrade-test.com/page.html")),
                                         static_cast<int>(DaliNiraBlockerResourceType::MainFrame),
                                         QUrl(QStringLiteral("http://upgrade-test.com/")), 20);
    assert(decUp.action == DaliNiraBlockerAction::Redirect);
    assert(decUp.redirectUrl == QStringLiteral("https://upgrade-test.com/page.html"));

    auto decLocal = service.evaluateRequest(QUrl(QStringLiteral("http://127.0.0.1:8080/test")),
                                           static_cast<int>(DaliNiraBlockerResourceType::MainFrame),
                                           QUrl(QStringLiteral("http://127.0.0.1:8080/")), 30);
    assert(decLocal.action != DaliNiraBlockerAction::Redirect);
  }
}

void testFrequentSitesAlgorithmAndEligibility() {
  QTemporaryDir directory;
  assert(directory.isValid());
  BrowserProfileService service(directory.path(), nullptr);

  // 1. First / accidental visit -> site does NOT appear in New Tab cards
  const QUrl siteA(QStringLiteral("https://custom-domain-alpha.com/home"));
  service.recordHistory(siteA, QStringLiteral("Alpha Home"), false);
  QJsonArray cardsAfterOneVisit = collectNewTabFrequentSites(&service);
  const auto containsHost = [](const QJsonArray &cards, const QString &host) {
    for (const QJsonValue &val : cards) {
      if (QUrl(val.toObject().value(QStringLiteral("url")).toString()).host() == host) return true;
    }
    return false;
  };
  assert(!containsHost(cardsAfterOneVisit, QStringLiteral("custom-domain-alpha.com"))
         && "First accidental visit must NOT appear in New Tab frequent cards");

  // 2. Second / repeated visit -> site becomes eligible and appears
  service.recordHistory(QUrl(QStringLiteral("https://custom-domain-alpha.com/features")),
                        QStringLiteral("Alpha Features"), false);
  QJsonArray cardsAfterTwoVisits = collectNewTabFrequentSites(&service);
  assert(containsHost(cardsAfterTwoVisits, QStringLiteral("custom-domain-alpha.com"))
         && "Second visit makes site eligible for New Tab frequent cards");

  // 3. Deduplication: multiple URLs from the same origin result in exactly 1 card
  service.recordHistory(QUrl(QStringLiteral("https://custom-domain-alpha.com/login")),
                        QStringLiteral("Sign in to Alpha"), false);
  service.recordHistory(QUrl(QStringLiteral("https://custom-domain-alpha.com/search?q=test")),
                        QStringLiteral("Alpha Search"), false);
  QJsonArray dedupedCards = collectNewTabFrequentSites(&service);
  int alphaCardCount = 0;
  for (const QJsonValue &val : dedupedCards) {
    if (QUrl(val.toObject().value(QStringLiteral("url")).toString()).host() == QStringLiteral("custom-domain-alpha.com")) {
      ++alphaCardCount;
    }
  }
  assert(alphaCardCount == 1 && "Multiple URLs from the same origin must deduplicate into 1 card");

  // 4. Rapid reloads vs steady usage: rapid reloads are damped
  const QDateTime now = QDateTime::currentDateTimeUtc();
  const double rapidScore = dalinira::core::frequent_sites::calculateScore(10, 0, now, 8, now);
  const double steadyScore = dalinira::core::frequent_sites::calculateScore(4, 0, now, 0, now);
  assert(steadyScore > rapidScore && "Steady visits must outrank a burst of rapid reloads");

  // 5. Recency decay: recent active site ranks above stale site
  const QDateTime fortyDaysAgo = now.addDays(-40);
  const double staleScore = dalinira::core::frequent_sites::calculateScore(10, 0, fortyDaysAgo, 0, now);
  const double recentScore = dalinira::core::frequent_sites::calculateScore(3, 0, now, 0, now);
  assert(recentScore > staleScore && "Actively visited recent site must rank above very old stale visits");

  // 6. Private/incognito browsing must never affect frequent sites ranking
  QTemporaryDir privateDir;
  assert(privateDir.isValid());
  BrowserProfileService privateService(privateDir.path(), nullptr, nullptr, true, nullptr);
  privateService.recordHistory(QUrl(QStringLiteral("https://incognito-secret.com/")),
                               QStringLiteral("Secret"), false);
  privateService.recordHistory(QUrl(QStringLiteral("https://incognito-secret.com/")),
                               QStringLiteral("Secret"), false);
  assert(privateService.frequentSites().isEmpty()
         && "Private browsing must not record frequent sites");
  assert(collectNewTabFrequentSites(&privateService).isEmpty()
         && "Private browsing must not create New Tab cards");

  // 7. Internal and auth callback URLs must never appear
  service.recordHistory(QUrl(QStringLiteral("dalinira://settings")), QStringLiteral("Settings"), false);
  service.recordHistory(QUrl(QStringLiteral("file:///tmp/malicious.html")), QStringLiteral("Local File"), false);
  service.recordHistory(QUrl(QStringLiteral("https://auth.company.com/oauth/callback?code=xyz")),
                        QStringLiteral("OAuth Handshake"), false);
  QJsonArray filteredCards = collectNewTabFrequentSites(&service);
  assert(!containsHost(filteredCards, QStringLiteral("auth.company.com")));
  for (const QJsonValue &val : filteredCards) {
    const QString cardUrl = val.toObject().value(QStringLiteral("url")).toString();
    assert(!cardUrl.startsWith(QStringLiteral("dalinira://")));
    assert(!cardUrl.startsWith(QStringLiteral("file://")));
    assert(!cardUrl.contains(QStringLiteral("callback")));
  }

  // 8. Manual Quick Access (bookmarks) appears immediately without threshold
  service.addBookmark(QUrl(QStringLiteral("https://instant-quick-access.org/")),
                      QStringLiteral("Instant Quick Access"));
  QJsonArray bookmarks = collectNewTabBookmarks(&service);
  assert(containsHost(bookmarks, QStringLiteral("instant-quick-access.org"))
         && "Manually added Quick Access cards appear immediately without visit threshold");
  assert(!containsHost(collectNewTabFrequentSites(&service), QStringLiteral("instant-quick-access.org"))
         && "Automatic Frequent Sites still enforce the eligibility threshold");

  // 9. Persistence across reload
  BrowserProfileService reloadedService(directory.path(), nullptr);
  QJsonArray reloadedCards = collectNewTabFrequentSites(&reloadedService);
  assert(containsHost(reloadedCards, QStringLiteral("custom-domain-alpha.com"))
         && "Frequent sites ranking must survive profile restart");

  // 10. Arbitrary non-hardcoded site generates proper favicon lookup URL
  const QString iconField = reloadedCards.first().toObject().value(QStringLiteral("icon")).toString();
  assert(iconField.startsWith(QStringLiteral("dalinira://newtab/favicon?"))
         && "Arbitrary sites use the shared favicon model");
  assert(iconField.contains(QStringLiteral("custom-domain-alpha.com"))
         && "Favicon lookup URL points to the arbitrary site's origin");
}

void testFrequentSitesPersistentSuppression() {
  QTemporaryDir directory;
  assert(directory.isValid());
  BrowserProfileService service(directory.path(), nullptr);

  const auto containsHost = [](const QJsonArray &cards, const QString &host) {
    for (const QJsonValue &val : cards) {
      if (QUrl(val.toObject().value(QStringLiteral("url")).toString()).host().toLower() == host.toLower()) {
        return true;
      }
    }
    return false;
  };

  // 1. Visit custom-site-1.com twice to make it eligible
  const QUrl site1Url(QStringLiteral("https://custom-site-1.com/"));
  service.recordHistory(site1Url, QStringLiteral("Site 1"), false);
  service.recordHistory(site1Url, QStringLiteral("Site 1"), false);

  // Visit custom-site-2.com twice to make it eligible
  const QUrl site2Url(QStringLiteral("https://custom-site-2.com/"));
  service.recordHistory(site2Url, QStringLiteral("Site 2"), false);
  service.recordHistory(site2Url, QStringLiteral("Site 2"), false);

  QJsonArray initialCards = collectNewTabFrequentSites(&service);
  assert(containsHost(initialCards, QStringLiteral("custom-site-1.com")) && "Site 1 must appear");
  assert(containsHost(initialCards, QStringLiteral("custom-site-2.com")) && "Site 2 must appear");
  const int initialHistoryCount = service.recentHistory().size();
  assert(initialHistoryCount >= 2);

  // 2. Remove frequent-site card: hide site 1
  assert(service.hideFrequentSite(site1Url) && "hideFrequentSite must succeed");
  assert(service.isFrequentSiteHidden(site1Url) && "Site 1 must report hidden");
  assert(service.isFrequentSiteHidden(QUrl(QStringLiteral("https://custom-site-1.com/features")))
         && "Subpages on same origin must report hidden");

  // 3. Site immediately disappears from New Tab cards
  QJsonArray cardsAfterHide = collectNewTabFrequentSites(&service);
  assert(!containsHost(cardsAfterHide, QStringLiteral("custom-site-1.com"))
         && "Hidden site must disappear from New Tab cards");
  assert(containsHost(cardsAfterHide, QStringLiteral("custom-site-2.com"))
         && "Unrelated domain must remain unaffected");

  // 4. Opening another New Tab: collectNewTabFrequentSites keeps it hidden
  QJsonArray secondNewTabCards = collectNewTabFrequentSites(&service);
  assert(!containsHost(secondNewTabCards, QStringLiteral("custom-site-1.com"))
         && "Opening another New Tab keeps site hidden");

  // 5. Normal history remains completely intact (not deleted)
  assert(service.recentHistory().size() == initialHistoryCount
         && "Hiding a card must NOT delete browsing history");

  // 6. Further visits do not automatically unsuppress it
  for (int i = 0; i < 10; ++i) {
    service.recordHistory(QUrl(QStringLiteral("https://custom-site-1.com/heavy-usage")),
                          QStringLiteral("Heavy usage"), false);
  }
  QJsonArray cardsAfterHeavyVisits = collectNewTabFrequentSites(&service);
  assert(!containsHost(cardsAfterHeavyVisits, QStringLiteral("custom-site-1.com"))
         && "Additional visits must NEVER automatically unsuppress a user-removed site");

  // 7. Same-origin subpages remain suppressed
  service.recordHistory(QUrl(QStringLiteral("https://custom-site-1.com/subpage/detail")),
                        QStringLiteral("Detail"), false);
  service.recordHistory(QUrl(QStringLiteral("https://custom-site-1.com/subpage/detail")),
                        QStringLiteral("Detail"), false);
  assert(!containsHost(collectNewTabFrequentSites(&service), QStringLiteral("custom-site-1.com"))
         && "Same-origin subpages remain suppressed");

  // 8. Profile reload / browser restart persistence
  {
    BrowserProfileService reloadedService(directory.path(), nullptr);
    assert(reloadedService.isFrequentSiteHidden(site1Url)
           && "Hidden state must survive profile service restart");
    assert(reloadedService.hiddenFrequentSites().contains(QStringLiteral("custom-site-1.com"))
           && "Canonical key must be in hiddenFrequentSites list");
    QJsonArray reloadedCards = collectNewTabFrequentSites(&reloadedService);
    assert(!containsHost(reloadedCards, QStringLiteral("custom-site-1.com"))
           && "Hidden site remains suppressed after restart");
    assert(containsHost(reloadedCards, QStringLiteral("custom-site-2.com"))
           && "Other site remains visible after restart");
  }

  // 9. Chromium and WebKit share identical backend pipeline:
  // Both engines consume collectNewTabFrequentSites(&service) and window.daliniraFrequentSiteConfig.hiddenSites
  {
    QStringList hiddenList = service.hiddenFrequentSites();
    assert(hiddenList.contains(QStringLiteral("custom-site-1.com"))
           && "Shared profile provides suppression list to both engines");
  }

  // 10. Manual Quick Access (bookmark) removal does NOT create frequent site suppression
  const QUrl manualUrl(QStringLiteral("https://manual-pin.org/"));
  service.addBookmark(manualUrl, QStringLiteral("Manual Pin"));
  assert(containsHost(collectNewTabBookmarks(&service), QStringLiteral("manual-pin.org")));
  assert(service.removeBookmark(manualUrl) && "removeBookmark succeeds");
  assert(!containsHost(collectNewTabBookmarks(&service), QStringLiteral("manual-pin.org"))
         && "Bookmark is removed");
  assert(!service.isFrequentSiteHidden(manualUrl)
         && "Removing manual bookmark must NOT add to hiddenFrequentSites");

  // 11. Private/incognito profile cannot corrupt normal preferences
  {
    BrowserProfileService privateService(directory.path(), nullptr, nullptr, true, nullptr);
    assert(!privateService.hideFrequentSite(site2Url)
           && "Private mode must reject hiding to prevent persistent modification");
    assert(!privateService.restoreHiddenFrequentSites()
           && "Private mode must reject restore");
  }

  // 12. Restore hidden frequent sites
  assert(service.restoreHiddenFrequentSites() && "restoreHiddenFrequentSites must succeed");
  assert(!service.isFrequentSiteHidden(site1Url) && "Site 1 must no longer be hidden");
  QJsonArray restoredCards = collectNewTabFrequentSites(&service);
  assert(containsHost(restoredCards, QStringLiteral("custom-site-1.com"))
         && "Restored site reappears in New Tab cards");
}

void testFrequentSitesEngineIsolationAndQuickAccess() {
  QTemporaryDir directory;
  assert(directory.isValid());

  const auto containsCardHost = [](const QJsonArray &cards, const QString &host) {
    for (const QJsonValue &v : cards) {
      if (QUrl(v.toObject().value(QStringLiteral("url")).toString()).host().contains(host, Qt::CaseInsensitive)) {
        return true;
      }
    }
    return false;
  };
  const auto containsFrequentHost = [](const QList<BrowserFrequentSite> &sites, const QString &host) {
    for (const BrowserFrequentSite &s : sites) {
      if (s.url.host().contains(host, Qt::CaseInsensitive)) return true;
    }
    return false;
  };

  // 1. Isolation: visits in Chromium do not affect WebKit and vice versa
  {
    BrowserProfileService service(directory.path(), nullptr, nullptr, false, nullptr);
    const QUrl amazonUrl(QStringLiteral("https://www.amazon.com/"));
    const QUrl instagramUrl(QStringLiteral("https://www.instagram.com/"));

    // Visit Amazon 3 times in Chromium
    for (int i = 0; i < 3; ++i) {
      service.recordHistory(amazonUrl, QStringLiteral("Amazon"), true, QStringLiteral("chromium"));
    }

    // Chromium has Amazon as a frequent site and card
    assert(containsFrequentHost(service.frequentSites(10, QStringLiteral("chromium")), QStringLiteral("amazon.com")));
    assert(containsCardHost(collectNewTabFrequentSites(&service, 6, QStringLiteral("chromium")), QStringLiteral("amazon.com")));

    // WebKit must NOT have Amazon
    assert(!containsFrequentHost(service.frequentSites(10, QStringLiteral("webkit")), QStringLiteral("amazon.com")));
    assert(!containsCardHost(collectNewTabFrequentSites(&service, 6, QStringLiteral("webkit")), QStringLiteral("amazon.com")));

    // Visit Instagram 3 times in WebKit
    for (int i = 0; i < 3; ++i) {
      service.recordHistory(instagramUrl, QStringLiteral("Instagram"), true, QStringLiteral("webkit"));
    }

    // WebKit has Instagram, Chromium does NOT
    assert(containsFrequentHost(service.frequentSites(10, QStringLiteral("webkit")), QStringLiteral("instagram.com")));
    assert(containsCardHost(collectNewTabFrequentSites(&service, 6, QStringLiteral("webkit")), QStringLiteral("instagram.com")));
    assert(!containsFrequentHost(service.frequentSites(10, QStringLiteral("chromium")), QStringLiteral("instagram.com")));
    assert(!containsCardHost(collectNewTabFrequentSites(&service, 6, QStringLiteral("chromium")), QStringLiteral("instagram.com")));

    // 2. Same site can independently become frequent in both engines
    const QUrl githubUrl(QStringLiteral("https://github.com/"));
    for (int i = 0; i < 3; ++i) {
      service.recordHistory(githubUrl, QStringLiteral("GitHub"), true, QStringLiteral("chromium"));
      service.recordHistory(githubUrl, QStringLiteral("GitHub"), true, QStringLiteral("webkit"));
    }
    assert(containsFrequentHost(service.frequentSites(10, QStringLiteral("chromium")), QStringLiteral("github.com")));
    assert(containsFrequentHost(service.frequentSites(10, QStringLiteral("webkit")), QStringLiteral("github.com")));
    assert(containsCardHost(collectNewTabFrequentSites(&service, 6, QStringLiteral("chromium")), QStringLiteral("github.com")));
    assert(containsCardHost(collectNewTabFrequentSites(&service, 6, QStringLiteral("webkit")), QStringLiteral("github.com")));

    // 3. Suppression isolation: hiding in Chromium does not hide in WebKit
    assert(service.hideFrequentSite(githubUrl, QStringLiteral("chromium")));
    assert(service.isFrequentSiteHidden(githubUrl, QStringLiteral("chromium")));
    assert(!service.isFrequentSiteHidden(githubUrl, QStringLiteral("webkit")));
    assert(!containsCardHost(collectNewTabFrequentSites(&service, 6, QStringLiteral("chromium")), QStringLiteral("github.com")));
    assert(containsCardHost(collectNewTabFrequentSites(&service, 6, QStringLiteral("webkit")), QStringLiteral("github.com")));

    // Restore Chromium suppression
    assert(service.restoreHiddenFrequentSites(QStringLiteral("chromium")));
    assert(!service.isFrequentSiteHidden(githubUrl, QStringLiteral("chromium")));
    assert(containsCardHost(collectNewTabFrequentSites(&service, 6, QStringLiteral("chromium")), QStringLiteral("github.com")));

    // 4. Manual Quick Access vs Shared Bookmarks
    const QUrl bookmarkUrl(QStringLiteral("https://shared-bookmark.org/"));
    service.addBookmark(bookmarkUrl, QStringLiteral("Shared Bookmark"));
    assert(service.isBookmarked(bookmarkUrl));

    // Chromium fallback to bookmarks when no specialized quickAccess is set
    assert(service.quickAccess(QStringLiteral("chromium")).contains(bookmarkUrl));

    // Add engine-specific quick access shortcut
    const QUrl chromQuick(QStringLiteral("https://chromium-only.org/"));
    assert(service.addQuickAccess(chromQuick, QStringLiteral("Chromium Quick"), QStringLiteral("chromium")));
    assert(service.quickAccess(QStringLiteral("chromium")).contains(chromQuick));
    assert(!service.quickAccess(QStringLiteral("webkit")).contains(chromQuick));
    // Bookmarks should NOT be polluted
    assert(!service.isBookmarked(chromQuick));

    const QUrl webkitQuick(QStringLiteral("https://webkit-only.org/"));
    assert(service.addQuickAccess(webkitQuick, QStringLiteral("WebKit Quick"), QStringLiteral("webkit")));
    assert(service.quickAccess(QStringLiteral("webkit")).contains(webkitQuick));
    assert(!service.quickAccess(QStringLiteral("chromium")).contains(webkitQuick));
    assert(!service.isBookmarked(webkitQuick));

    // Remove Quick Access from Chromium
    assert(service.removeQuickAccess(chromQuick, QStringLiteral("chromium")));
    assert(!service.quickAccess(QStringLiteral("chromium")).contains(chromQuick));
    // WebKit remains intact
    assert(service.quickAccess(QStringLiteral("webkit")).contains(webkitQuick));
    // Real bookmark remains intact
    assert(service.isBookmarked(bookmarkUrl));
  }

  // 5. Persistence across restart
  {
    BrowserProfileService reloaded(directory.path(), nullptr, nullptr, false, nullptr);
    assert(containsFrequentHost(reloaded.frequentSites(10, QStringLiteral("chromium")), QStringLiteral("amazon.com")));
    assert(!containsFrequentHost(reloaded.frequentSites(10, QStringLiteral("webkit")), QStringLiteral("amazon.com")));
    assert(containsFrequentHost(reloaded.frequentSites(10, QStringLiteral("webkit")), QStringLiteral("instagram.com")));
    assert(!containsFrequentHost(reloaded.frequentSites(10, QStringLiteral("chromium")), QStringLiteral("instagram.com")));

    assert(reloaded.quickAccess(QStringLiteral("webkit")).contains(QUrl(QStringLiteral("https://webkit-only.org/"))));
    assert(!reloaded.quickAccess(QStringLiteral("chromium")).contains(QUrl(QStringLiteral("https://webkit-only.org/"))));
  }

  // 6. Safe legacy migration: legacy shared data moves into chromium scope without corrupting webkit
  {
    QTemporaryDir legacyDir;
    assert(legacyDir.isValid());
    const QString iniPath = QDir(legacyDir.path()).filePath(QStringLiteral("browser-preferences.ini"));
    {
      QSettings legacySettings(iniPath, QSettings::IniFormat);
      QJsonArray legacySites{
        QJsonObject{
          {QStringLiteral("url"), QStringLiteral("https://legacy-site.com/")},
          {QStringLiteral("title"), QStringLiteral("Legacy Site")},
          {QStringLiteral("visitCount"), 10},
          {QStringLiteral("lastVisited"), QString::number(QDateTime::currentSecsSinceEpoch())},
          {QStringLiteral("score"), 100.0}
        }
      };
      legacySettings.setValue(QStringLiteral("history/frequentSites"),
                              QJsonDocument(legacySites).toJson(QJsonDocument::Compact));
      legacySettings.setValue(QStringLiteral("newtab/hiddenFrequentSites"),
                              QStringList{QStringLiteral("hidden-legacy.com")});
      legacySettings.sync();
    }

    BrowserProfileService migratedService(legacyDir.path(), nullptr, nullptr, false, nullptr);
    // Chromium gets migrated data
    assert(containsFrequentHost(migratedService.frequentSites(10, QStringLiteral("chromium")), QStringLiteral("legacy-site.com")));
    assert(migratedService.hiddenFrequentSites(QStringLiteral("chromium")).contains(QStringLiteral("hidden-legacy.com")));

    // WebKit starts clean and is NOT corrupted by legacy data
    assert(!containsFrequentHost(migratedService.frequentSites(10, QStringLiteral("webkit")), QStringLiteral("legacy-site.com")));
    assert(!migratedService.hiddenFrequentSites(QStringLiteral("webkit")).contains(QStringLiteral("hidden-legacy.com")));
  }

  // 7. Visual UI parity validation
  {
    const QString html = newTabHtml(QStringLiteral("Google"));
    assert(html.contains(QStringLiteral(".shortcut-wrap{position:relative;width:112px;min-width:112px;flex:0 0 112px}")));
    assert(html.contains(QStringLiteral(".shortcut{display:flex;flex-direction:column;align-items:center;justify-content:flex-start;box-sizing:border-box;text-decoration:none;font:inherit;width:112px;height:112px;border:0;border-radius:4px")));
    assert(html.contains(QStringLiteral(".shortcut-icon{display:flex;align-items:center;justify-content:center;width:48px;height:48px;min-width:48px;min-height:48px;margin:0 auto 16px;border-radius:50%")));
    assert(html.contains(QStringLiteral("window.renderFrequentSites=renderFrequentSites;")));
    assert(html.contains(QStringLiteral("window.applyFrequentConfig=applyFrequentConfig;")));
    assert(html.contains(QStringLiteral("window.render=render;")));
  }
}

void testCustomShortcuts(bool runtime) {
  using namespace dalinira::engine;
  namespace shortcuts = dalinira::newtab::shortcuts;
  QTemporaryDir directory;
  assert(directory.isValid());
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
  QCoreApplication::setOrganizationName(QStringLiteral("DaliNiraShortcutTest"));
  QCoreApplication::setApplicationName(QStringLiteral("Shortcuts"));
  for (const auto engine : {EngineType::Chromium, EngineType::WebKit}) {
    EngineBackend::setSelectedEngineType(engine);
    assert(shortcuts::load().isEmpty());
    assert(!shortcuts::add({}, QStringLiteral("https://example.com")));
    assert(!shortcuts::add(QStringLiteral("Bad"), QStringLiteral("javascript:alert(1)")));
    assert(!shortcuts::add(QStringLiteral("Bad"), QStringLiteral("file:///etc/passwd")));
    assert(!shortcuts::add(QStringLiteral("Bad"), QStringLiteral("https://user:secret@example.com")));
    assert(shortcuts::add(QStringLiteral("Example"), QStringLiteral("example.com")));
    assert(shortcuts::source() == QStringLiteral("custom"));
    assert(shortcuts::load().first().toObject().value(QStringLiteral("name")) == QStringLiteral("Example"));
    assert(!shortcuts::add(QStringLiteral("Duplicate"), QStringLiteral("https://example.com")));
    assert(shortcuts::edit(QStringLiteral("https://example.com"), QStringLiteral("Updated"), QStringLiteral("https://updated.example")));
    assert(!shortcuts::edit(QStringLiteral("https://missing.example"), QStringLiteral("Missing"), QStringLiteral("https://other.example")));
    assert(shortcuts::edit(QStringLiteral("https://updated.example"), QStringLiteral("Example"), QStringLiteral("https://example.com")));
    QSettings reopened;
    const auto persisted = QJsonDocument::fromJson(reopened.value(dalinira::ui_profile::scopedKey(QStringLiteral("browser/customShortcuts"))).toByteArray()).array();
    assert(persisted.size() == 1);
    assert(shortcuts::add(QStringLiteral("</script><script>window.shortcutInjected=true</script>"), QStringLiteral("https://safe.example")));
    assert(shortcuts::add(QStringLiteral("__BACKGROUND_SOURCE_DEFAULT__"), QStringLiteral("https://literal.example")));
    const QString html = newTabHtml(QStringLiteral("Google"));
    assert(html.contains(QStringLiteral("\"name\":\"__BACKGROUND_SOURCE_DEFAULT__\"")));
    assert(!html.contains(QStringLiteral("</script><script>window.shortcutInjected")));
    shortcuts::remove(QUrl(QStringLiteral("https://example.com")));
    assert(shortcuts::load().size() == 2);
    for (int i = 2; i < shortcuts::limit; ++i)
      assert(shortcuts::add(QStringLiteral("Site %1").arg(i), QStringLiteral("https://site%1.example").arg(i)));
    assert(!shortcuts::add(QStringLiteral("Overflow"), QStringLiteral("https://overflow.example")));
    dalinira::ui_profile::removeVisualValue(QStringLiteral("browser/customShortcuts"));
    dalinira::ui_profile::removeVisualValue(QStringLiteral("browser/topSitesSource"));
  }
  EngineBackend::setSelectedEngineType(EngineType::Chromium);
  assert(shortcuts::add(QStringLiteral("Chromium only"), QStringLiteral("https://chromium.example")));
  EngineBackend::setSelectedEngineType(EngineType::WebKit);
  assert(shortcuts::load().isEmpty());
  EngineBackend::setSelectedEngineType(EngineType::Chromium);
  dalinira::ui_profile::removeVisualValue(QStringLiteral("browser/customShortcuts"));
  dalinira::ui_profile::removeVisualValue(QStringLiteral("browser/topSitesSource"));
  if (!runtime) return;
  QImage fixtureImage(32, 32, QImage::Format_ARGB32);fixtureImage.fill(Qt::green);
  QByteArray fixturePng;QBuffer fixtureBuffer(&fixturePng);fixtureBuffer.open(QIODevice::WriteOnly);assert(fixtureImage.save(&fixtureBuffer, "PNG"));
  QTcpServer iconServer;assert(iconServer.listen(QHostAddress::LocalHost));
  int faviconRequests = 0;
  QObject::connect(&iconServer, &QTcpServer::newConnection, &iconServer, [&] {
    while (auto *socket = iconServer.nextPendingConnection()) {
      QObject::connect(socket, &QTcpSocket::readyRead, socket, [&,socket] {
        const QByteArray request = socket->readAll();if(request.isEmpty())return;
        assert(request.startsWith("GET /favicon.ico "));assert(!request.toLower().contains("referer:"));++faviconRequests;
        socket->write("HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nContent-Length: " + QByteArray::number(fixturePng.size()) + "\r\nConnection: close\r\n\r\n" + fixturePng);socket->disconnectFromHost();
      });
      QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    }
  });
  const QString fixtureUrl = QStringLiteral("http://127.0.0.1:%1/").arg(iconServer.serverPort());

  class ShortcutPage : public EvidencePage {
   public:
    using EvidencePage::EvidencePage;
    QList<QUrl> commands;
   protected:
    bool acceptNavigationRequest(const QUrl &url, NavigationType, bool) override {
      if (url.host() == QLatin1String("frequent-sites")) {
        commands.append(url);
        return false;
      }
      return true;
    }
  };
  BrowserProfileService iconProfile(directory.path()+QStringLiteral("/icons"), nullptr);
  ShortcutPage page(iconProfile.profile());
  QEventLoop loadLoop;
  bool loaded = false;
  QObject::connect(&page, &QWebEnginePage::loadFinished, &loadLoop, [&](bool ok) { loaded = ok; loadLoop.quit(); });
  QTimer::singleShot(10000, &loadLoop, &QEventLoop::quit);
  page.setHtml(newTabHtml(QStringLiteral("Google")), QUrl(QStringLiteral("dalinira://newtab/")));
  loadLoop.exec();
  assert(loaded);
  const auto evaluate = [&](const QString &script) {
    QEventLoop loop;
    bool result = false;
    page.runJavaScript(script, [&](const QVariant &value) { result = value.toBool(); loop.quit(); });
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    loop.exec();
    QApplication::processEvents();
    if (!result) std::cerr << "Failed shortcut script: " << script.toStdString() << std::endl;
    assert(result);
  };
  evaluate(QStringLiteral(R"JS((()=>{
    window.daliniraSuggestionBridge('shortcut-test-cap');
    const add=document.querySelector('#add-shortcut');
    if(!add||document.querySelector('#shortcuts').hidden||window.shortcutInjected)return false;
    add.click();
    if(document.querySelector('#shortcut-overlay').hidden||document.activeElement.id!=='shortcut-name')return false;
    document.querySelector('#shortcut-name').value='My example';
    document.querySelector('#shortcut-url').value='javascript:alert(1)';validateShortcut();
    if(!document.querySelector('#shortcut-save').disabled)return false;
    document.querySelector('#shortcut-url').value='%1';validateShortcut();
    if(document.querySelector('#shortcut-save').disabled)return false;
    document.querySelector('#shortcut-form').requestSubmit();
    return document.querySelector('#shortcut-overlay').hidden&&p.topSitesSource==='custom'&&p.customShortcuts.length===1&&document.querySelector('#shortcut-list a').textContent.includes('My example')&&document.querySelector('#shortcut-list a').href==='%1';
  })())JS").arg(fixtureUrl));
  assert(shortcuts::add(QStringLiteral("My example"),fixtureUrl));
  QEventLoop fetchLoop;bool fetched=false;
  requestNewTabShortcutFavicon(&iconProfile,QUrl(fixtureUrl),&page,[&]{fetched=true;fetchLoop.quit();});
  QTimer::singleShot(6000,&fetchLoop,&QEventLoop::quit);fetchLoop.exec();assert(fetched);
  const QString iconJson=QString::fromUtf8(QJsonDocument(QJsonArray{newTabFaviconUrl(&iconProfile,QUrl(fixtureUrl))}).toJson(QJsonDocument::Compact));
  evaluate(QStringLiteral("p.customShortcuts[0].icon=%1[0];renderFrequentSites();true").arg(iconJson));
  QEventLoop iconWait;QTimer::singleShot(300, &iconWait, &QEventLoop::quit);iconWait.exec();
  evaluate(QStringLiteral("document.querySelector('#shortcut-list .shortcut-icon img')?.naturalWidth>0"));
  assert(faviconRequests > 0);
  evaluate(QStringLiteral(R"JS((()=>{
    const tile=document.querySelector('#shortcut-list a'),badge=tile.querySelector('.shortcut-icon');
    if(getComputedStyle(badge).borderRadius!=='50%'||getComputedStyle(tile).backgroundColor!=='rgba(0, 0, 0, 0)')return false;
    document.querySelector('#shortcut-list .shortcut-more').click();
    if(document.querySelector('#shortcut-menu').hidden)return false;
    document.querySelector('#shortcut-menu-edit').click();
    if(document.querySelector('#shortcut-overlay').hidden||document.querySelector('#shortcut-name').value!=='My example')return false;
    document.querySelector('#shortcut-name').value='Edited example';validateShortcut();
    document.querySelector('#shortcut-form').requestSubmit();
    return p.customShortcuts.length===1&&document.querySelector('#shortcut-list a').textContent.includes('Edited example');
  })())JS"));
  assert(QUrlQuery(page.commands.last()).queryItemValue(QStringLiteral("op")) == QLatin1String("edit-shortcut"));
  assert(QUrlQuery(page.commands.last()).queryItemValue(QStringLiteral("original"), QUrl::FullyDecoded) == fixtureUrl);
  assert(!page.commands.isEmpty());
  auto command = QUrlQuery(page.commands.first());
  assert(command.queryItemValue(QStringLiteral("op")) == QLatin1String("add-shortcut"));
  assert(command.queryItemValue(QStringLiteral("name"), QUrl::FullyDecoded) == QLatin1String("My example"));
  assert(command.queryItemValue(QStringLiteral("cap")) == QLatin1String("shortcut-test-cap"));
  evaluate(QStringLiteral(R"JS((()=>{
    document.querySelector('#add-shortcut').click();
    document.querySelector('#shortcut-name').value='Duplicate';
    document.querySelector('#shortcut-url').value='%1';validateShortcut();
    if(!document.querySelector('#shortcut-save').disabled)return false;
    document.querySelector('#shortcut-cancel').click();
    if(p.customShortcuts.length!==1)return false;
    document.querySelector('#add-shortcut').click();
    document.querySelector('#shortcut-name').dispatchEvent(new KeyboardEvent('keydown',{key:'Escape',bubbles:true}));
    if(!document.querySelector('#shortcut-overlay').hidden)return false;
    window.daliniraCustomization.open('topsites');
    if(!document.querySelector('input[name="top-sites-source"][value="custom"]').checked)return false;
    document.querySelector('#customization-add-shortcut').click();
    if(!document.querySelector('#customization-overlay').hidden||document.querySelector('#shortcut-overlay').hidden)return false;
    closeShortcutDialog();
    document.querySelector('#shortcut-list .shortcut-more').click();document.querySelector('#shortcut-menu-remove').click();
    return p.customShortcuts.length===0&&!document.querySelector('#shortcut-list a')&&!!document.querySelector('#add-shortcut');
  })())JS").arg(fixtureUrl));
  assert(QUrlQuery(page.commands.last()).queryItemValue(QStringLiteral("op")) == QLatin1String("remove-shortcut"));
  evaluate(QStringLiteral(R"JS((()=>{
    window.daliniraFrequentSiteConfig={visible:false,topSitesSource:'custom',customShortcuts:[{name:'Restored',url:'https://restored.example/'}]};
    applyFrequentConfig();
    if(!document.querySelector('#shortcuts').hidden)return false;
    window.daliniraFrequentSiteConfig.visible=true;applyFrequentConfig();
    return !document.querySelector('#shortcuts').hidden&&document.querySelector('#shortcut-list a').textContent.includes('Restored');
  })())JS").arg(fixtureUrl));
  evaluate(QStringLiteral(R"JS((()=>{
    const source=document.querySelector('input[name="top-sites-source"][value="frequent"]');
    source.checked=true;source.dispatchEvent(new Event('change'));
    return p.topSitesSource==='frequent';
  })())JS").arg(fixtureUrl));
  assert(QUrlQuery(page.commands.last()).queryItemValue(QStringLiteral("op")) == QLatin1String("set-source"));
  assert(QUrlQuery(page.commands.last()).queryItemValue(QStringLiteral("name")) == QLatin1String("frequent"));
  evaluate(QStringLiteral(R"JS((()=>{
    const visible=document.querySelector('#shortcuts-toggle');visible.checked=false;visible.dispatchEvent(new Event('change'));
    return document.querySelector('#shortcuts').hidden;
  })())JS").arg(fixtureUrl));
  assert(QUrlQuery(page.commands.last()).queryItemValue(QStringLiteral("op")) == QLatin1String("set-visible"));
  assert(QUrlQuery(page.commands.last()).queryItemValue(QStringLiteral("name")) == QLatin1String("0"));
  std::cout << "Custom shortcut storage and UI tests passed" << std::endl;
}

int main(int argc, char *argv[]) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  qputenv("QTWEBENGINE_CHROMIUM_FLAGS", "--no-sandbox --disable-gpu");
  registerDaliNiraUrlSchemes();
  QApplication app(argc, argv);

  if (app.arguments().contains(QStringLiteral("--newtab-shortcuts")) || app.arguments().contains(QStringLiteral("--newtab-shortcuts-storage"))) {
    testCustomShortcuts(app.arguments().contains(QStringLiteral("--newtab-shortcuts")));
    return 0;
  }
  testSuggestionRemoval();
  if (app.arguments().contains(QStringLiteral("--suggestion-removal"))) return 0;
  std::cerr << "Running testHistoryTitleAndTypedEvidence" << std::endl;
  testHistoryTitleAndTypedEvidence();
  std::cerr << "Running testBackwardCompatibleTypedCount" << std::endl;
  testBackwardCompatibleTypedCount();
  std::cerr << "Running testSearchHistoryPersistenceAndClearing" << std::endl;
  testSearchHistoryPersistenceAndClearing();
  std::cerr << "Running testAntiPoisoningConfidence" << std::endl;
  testAntiPoisoningConfidence();
  std::cerr << "Running testNewTabDataAndScriptSafety" << std::endl;
  testNewTabDataAndScriptSafety();
  std::cerr << "Running testPlaceholdersAndCachedFavicon" << std::endl;
  testPlaceholdersAndCachedFavicon();
  std::cerr << "Running testPrivacyAndContentSettingsPolicies" << std::endl;
  testPrivacyAndContentSettingsPolicies();
  std::cerr << "Running testPhase2_2C_NewTabCardHidingOnSearchFocus" << std::endl;
  testNewTabTemplateKeepsScriptPlaceholders();
  testPhase2_2C_NewTabCardHidingOnSearchFocus();
  std::cerr << "Running testPhase2_2C_SitePolicyPersistenceAndExtendedFields" << std::endl;
  testPhase2_2C_SitePolicyPersistenceAndExtendedFields();
  std::cerr << "Running testPhase2_2D_YouTubeCentralCosmeticLifecycle" << std::endl;
  testPhase2_2D_YouTubeCentralCosmeticLifecycle();
  std::cerr << "Running testPhase2_2C_EvaluateRequestPolicyEnforcement" << std::endl;
  testPhase2_2C_EvaluateRequestPolicyEnforcement();
  std::cerr << "Running testFrequentSitesAlgorithmAndEligibility" << std::endl;
  testFrequentSitesAlgorithmAndEligibility();
  std::cerr << "Running testFrequentSitesPersistentSuppression" << std::endl;
  testFrequentSitesPersistentSuppression();
  std::cerr << "Running testFrequentSitesEngineIsolationAndQuickAccess" << std::endl;
  testFrequentSitesEngineIsolationAndQuickAccess();

  std::cout << "Smart Omnibox Phase 2.1 and Phase 2.2C tests passed" << std::endl;
  return 0;
}
