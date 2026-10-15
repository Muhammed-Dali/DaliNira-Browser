#include "newtab/gecko_new_tab_controller.h"
#include "browser_window.h"
#include "core/browser_profile_service.h"
#include "engine/gecko/gecko_engine_view.h"
#include "engine/gecko/gecko_session.h"
#include "i18n/i18n.h"
#include "newtab/new_tab_asset_resolver.h"
#include "newtab/new_tab_html.h"
#include "newtab/new_tab_shortcuts.h"
#include <QApplication>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QSettings>
#include <QTimer>
namespace {
const QString prefix = QStringLiteral("ui_gecko/browser/geckoNewTab/");
bool web(const QUrl &u) {
  return u.isValid() && !u.host().isEmpty() && u.userName().isEmpty() &&
         u.password().isEmpty() &&
         (u.scheme() == QLatin1String("http") ||
          u.scheme() == QLatin1String("https"));
}
QString visualPath(const QString &key) {
  if (key == QLatin1String("backgroundSource"))
    return QStringLiteral("ui_gecko/browser/backgroundSource");
  if (key == QLatin1String("backgroundVisible"))
    return QStringLiteral("ui_gecko/browser/backgroundVisible");
  if (key == QLatin1String("shortcuts"))
    return QStringLiteral("ui_gecko/browser/showFrequentSites");
  return prefix + key;
}
QJsonObject defaults() {
  return {{"theme", "plain"},
          {"backgroundSource", "builtin"},
          {"backgroundVisible", false},
          {"searchVisible", true},
          {"clock", true},
          {"date", true},
          {"shortcuts", true},
          {"dim", 0},
          {"frequentPanelOpacity", 72},
          {"frequentIconOpacity", 82},
          {"clockStyle", "wall"},
          {"clockPosition", "auto"},
          {"clockFormat", "auto"}};
}
bool validVisual(const QString &key, const QJsonValue &value) {
  const auto base = defaults().value(key);
  if (base.isBool())
    return value.isBool();
  if (base.isDouble())
    return value.isDouble() && value.toDouble() == value.toInt(-1) &&
           value.toInt() >= 0 &&
           value.toInt() <= (key == QLatin1String("dim") ? 80 : 100);
  const QHash<QString, QStringList> choices{
      {"theme", {"plain", "flow"}},
      {"backgroundSource",
       {"builtin", "solid-indigo", "solid-magenta", "solid-teal", "solid-ember",
        "gradient-violet", "gradient-aurora", "gradient-ocean",
        "gradient-sunset"}},
      {"clockStyle",
       {"classic", "digital", "flip", "minimal", "modern-analog", "neon",
        "wall"}},
      {"clockPosition",
       {"auto", "center", "top-left", "top-right", "bottom-left",
        "bottom-right"}},
      {"clockFormat", {"auto", "12", "24"}}};
  return value.isString() && choices.value(key).contains(value.toString());
}
bool exactAction(const QJsonObject &action, const QStringList &keys) {
  if (action.size() != keys.size())
    return false;
  for (auto it = action.begin(); it != action.end(); ++it)
    if (!keys.contains(it.key()))
      return false;
  return true;
}
QString itemId(const QUrl &u) {
  return QString::fromLatin1(
      QCryptographicHash::hash(u.toEncoded(), QCryptographicHash::Sha256)
          .toHex()
          .left(32));
}
} // namespace
GeckoNewTabController::GeckoNewTabController(
    dalinira::engine::GeckoEngineView *view, BrowserWindow *window,
    BrowserProfileService *profile)
    : QObject(view), view_(view), window_(window), profile_(profile) {
  connect(view, &dalinira::engine::GeckoEngineView::newTabRequested, this,
          [this] { refresh(true); });
  connect(view, &dalinira::engine::GeckoEngineView::internalAction, this,
          [this](const QJsonObject &event) { action(event); });
  auto *timer = new QTimer(this);
  timer->setInterval(500);
  connect(timer, &QTimer::timeout, this, [this] { refresh(); });
  timer->start();
}
bool GeckoNewTabController::active() const {
  return view_ && window_ && view_->isEngineActive() && view_->isVisible() &&
         window_->isVisible() && !window_->isMinimized() &&
         view_->window() == window_ &&
         (window_->isActiveWindow() || QApplication::activeWindow() == window_);
}
void GeckoNewTabController::refresh(bool document) {
  if (!view_)
    return;
  QJsonArray providers, configuration;
  QString configured = profile_ ? profile_->configuredSearchEngineId()
                                : QStringLiteral("Google");
  for (const auto &id : {"Google", "DuckDuckGo", "Startpage", "Mojeek"})
    providers.append(QJsonObject{{"id", id}, {"label", id}});
  if (profile_)
    for (const auto &p : profile_->customSearchEngines()) {
      configuration.append(
          QJsonObject{{"id", p.name}, {"template", p.urlTemplate}});
      // The legacy setter aliases these names even for a custom provider.
      if (QStringList{QStringLiteral("bing"), QStringLiteral("brave"),
                      QStringLiteral("brave search")}
              .contains(p.name.toLower()))
        continue;
      providers.append(QJsonObject{{"id", p.name}, {"label", p.name}});
    }
  QJsonObject visual = defaults();
  QSettings settings;
  for (auto i = visual.begin(); i != visual.end(); ++i) {
    const QVariant value =
        settings.value(visualPath(i.key()), i.value().toVariant());
    i.value() = QJsonValue::fromVariant(value);
  }
  if (visual.value("backgroundSource") == QLatin1String("custom"))
    visual.insert("backgroundSource", QStringLiteral("builtin"));
  QJsonArray items;
  QHash<QString, QUrl> mapping;
  const auto add = [&](const QString &name, const QUrl &url) {
    if (items.size() >= 10 || !web(url))
      return;
    const QString id = itemId(url);
    if (mapping.contains(id))
      return;
    mapping.insert(id, url);
    items.append(QJsonObject{{"id", id},
                             {"name", name.left(180)},
                             {"url", url.toString(QUrl::FullyEncoded)}});
  };
  const QString source =
      settings
          .value(QStringLiteral("ui_gecko/browser/topSitesSource"),
                 QStringLiteral("frequent"))
          .toString();
  if (source == QLatin1String("custom"))
    for (const auto &item :
         QJsonDocument::fromJson(
             settings.value(QStringLiteral("ui_gecko/browser/customShortcuts"))
                 .toByteArray())
             .array()) {
      const auto o = item.toObject();
      add(o.value("name").toString(), QUrl(o.value("url").toString()));
    }
  else if (profile_ && source == QLatin1String("bookmarks")) {
    for (const auto &url : profile_->bookmarks())
      add(url.host(), url);
  } else if (profile_)
    for (const auto &item :
         profile_->frequentSites(10, QStringLiteral("gecko")))
      add(item.title.isEmpty() ? item.url.host() : item.title, item.url);
  bool supported = false;
  for (const auto &p : providers)
    if (p.toObject().value("id") == configured)
      supported = true;
  QJsonObject next{
      {"providerId", configured},
      {"providerLabel", configured},
      {"placeholder",
       supported ? I18n::text(QStringLiteral("newtab.search_placeholder"))
                       .arg(configured)
                 : configured + QStringLiteral(" (unsupported provider)")},
      {"providers", providers},
      {"items", items},
      {"source", source},
      {"visual", visual}};
  QJsonObject old = state_;
  old.remove("revision");
  const QByteArray signature =
      QJsonDocument(configuration).toJson(QJsonDocument::Compact);
  const bool changed = old != next || signature != providerConfiguration_;
  providerConfiguration_ = signature;
  if (changed || state_.isEmpty()) {
    next.insert("revision", static_cast<qint64>(++revision_));
    state_ = next;
    items_ = mapping;
  }
  if (document) {
    QString markup = newTabHtml(configured, {}, {}, 0, 0, 0, false, false,
                                QStringLiteral("session"), {},
                                NewTabTransport::GeckoDocument);
    const QString assets =
        resolveNewTabAssetsDirectory(QCoreApplication::applicationDirPath());
    // Fixed resolver names only: there is no page-selected path or network
    // fallback.
    for (const QString &name :
         {QStringLiteral("dalinira-flow-blue.png"),
          QStringLiteral("dalinira-browser.png"), QStringLiteral("google.ico"),
          QStringLiteral("duckduckgo.ico"), QStringLiteral("startpage.ico"),
          QStringLiteral("mojeek.ico"), QStringLiteral("icons/settings.svg"),
          QStringLiteral("icons/close.svg"),
          QStringLiteral("icons/appearance.svg"),
          QStringLiteral("icons/search.svg"), QStringLiteral("icons/grid.svg"),
          QStringLiteral("icons/clock.svg"),
          QStringLiteral("icons/cards.svg")}) {
      NewTabStaticAsset asset;
      if (!newTabStaticAsset(QLatin1Char('/') + name, &asset))
        continue;
      const QByteArray bytes = readNewTabStaticAsset(asset, assets);
      if (bytes.size() > 4 * 1024 * 1024)
        continue;
      const QString data = QStringLiteral("data:%1;base64,%2")
                               .arg(QString::fromLatin1(asset.mimeType),
                                    QString::fromLatin1(bytes.toBase64()));
      markup.replace(name, data);
    }
    view_->prepareNewTab(markup.toUtf8(), state_);
  } else if (changed)
    view_->updateNewTabState(state_);
}
void GeckoNewTabController::action(const QJsonObject &event) {
  if (!active() || !profile_)
    return;
  refresh();
  if (event.value("revision").toInteger() != static_cast<qint64>(revision_))
    return;
  const QJsonObject a = event.value("action").toObject();
  const QString kind = a.value("kind").toString();
  QStringList keys{"kind"};
  if (kind == QLatin1String("search"))
    keys << "query";
  else if (kind == QLatin1String("speed-dial") ||
           kind == QLatin1String("shortcut-remove"))
    keys << "itemId";
  else if (kind == QLatin1String("visual"))
    keys << "key" << "value";
  else if (kind == QLatin1String("provider") || kind == QLatin1String("source"))
    keys << "value";
  else if (kind == QLatin1String("shortcut-add"))
    keys << "name" << "url";
  else if (kind == QLatin1String("shortcut-edit"))
    keys << "itemId" << "name" << "url";
  else
    return;
  if (!exactAction(a, keys))
    return;
  for (const auto &key : keys) {
    const auto value = a.value(key);
    if (kind == QLatin1String("visual") && key == QLatin1String("value"))
      continue;
    if (!value.isString())
      return;
    const auto text = value.toString();
    for (qsizetype i = 0; i < text.size(); ++i) {
      const auto c = text.at(i);
      if (c.unicode() < 32 || c.unicode() == 127)
        return;
      if (c.isHighSurrogate()) {
        if (++i >= text.size() || !text.at(i).isLowSurrogate())
          return;
      } else if (c.isLowSurrogate())
        return;
    }
  }
  QJsonObject effect;
  std::function<void()> apply;
  if (kind == QLatin1String("search")) {
    const QString provider = state_.value("providerId").toString();
    if (profile_->configuredSearchEngineId() != provider)
      return;
    bool known = false;
    for (const auto &p : state_.value("providers").toArray())
      if (p.toObject().value("id") == provider)
        known = true;
    const QString query = a.value("query").toString().trimmed();
    if (!known || query.isEmpty() || query.toUtf8().size() > 4096)
      return;
    const QUrl url = profile_->searchUrlForEngine(provider, query);
    if (!web(url))
      return;
    effect = {{"kind", "navigate"}, {"url", url.toString(QUrl::FullyEncoded)}};
  } else if (kind == QLatin1String("speed-dial")) {
    const QUrl url = items_.value(a.value("itemId").toString());
    if (!web(url))
      return;
    effect = {{"kind", "navigate"}, {"url", url.toString(QUrl::FullyEncoded)}};
  } else if (kind == QLatin1String("provider")) {
    const QString value = a.value("value").toString();
    bool known = false;
    for (const auto &p : state_.value("providers").toArray())
      if (p.toObject().value("id") == value)
        known = true;
    if (!known)
      return;
    effect = {{"kind", "preference"}};
    apply = [this, value] {
      if (profile_)
        profile_->setSearchEngine(value);
    };
  } else if (kind == QLatin1String("source")) {
    const QString value = a.value("value").toString();
    if (!QStringList{QStringLiteral("frequent"), QStringLiteral("bookmarks"),
                     QStringLiteral("custom")}
             .contains(value))
      return;
    effect = {{"kind", "preference"}};
    apply = [value] {
      QSettings s;
      s.setValue(QStringLiteral("ui_gecko/browser/topSitesSource"), value);
    };
  } else if (kind.startsWith(QLatin1String("shortcut-"))) {
    if (state_.value("source") != QLatin1String("custom"))
      return;
    QSettings settings;
    const auto stored =
        QJsonDocument::fromJson(
            settings.value(QStringLiteral("ui_gecko/browser/customShortcuts"))
                .toByteArray())
            .array();
    QJsonArray entries;
    const QString name = a.value("name").toString().trimmed();
    const QUrl target =
        dalinira::newtab::shortcuts::validatedUrl(a.value("url").toString());
    const QUrl original = items_.value(a.value("itemId").toString());
    bool found = false;
    if (kind != QLatin1String("shortcut-remove") &&
        (name.isEmpty() || name.size() > 180 || !web(target) ||
         a.value("url").toString().toUtf8().size() > 2048))
      return;
    if (kind != QLatin1String("shortcut-add") && !web(original))
      return;
    for (const auto &v : stored) {
      const auto o = v.toObject();
      const QUrl u =
          dalinira::newtab::shortcuts::validatedUrl(o.value("url").toString());
      if (!web(u))
        continue;
      if (kind != QLatin1String("shortcut-add") && u == original) {
        found = true;
        if (kind == QLatin1String("shortcut-edit"))
          entries.append(QJsonObject{
              {"name", name}, {"url", target.toString(QUrl::FullyEncoded)}});
        continue;
      }
      if (kind != QLatin1String("shortcut-remove") && u == target)
        return;
      entries.append(o);
    }
    if (kind == QLatin1String("shortcut-add")) {
      if (entries.size() >= 10)
        return;
      entries.append(QJsonObject{{"name", name},
                                 {"url", target.toString(QUrl::FullyEncoded)}});
    } else if (!found)
      return;
    const QByteArray serialized =
        QJsonDocument(entries).toJson(QJsonDocument::Compact);
    effect = {{"kind", "preference"}};
    apply = [serialized] {
      QSettings s;
      s.setValue(QStringLiteral("ui_gecko/browser/customShortcuts"),
                 serialized);
    };
  } else if (kind == QLatin1String("visual")) {
    const QString key = a.value("key").toString();
    if (!defaults().contains(key) || !validVisual(key, a.value("value")))
      return;
    const QJsonValue value = a.value("value");
    effect = {{"kind", "preference"}};
    apply = [key, value] {
      QSettings s;
      s.setValue(visualPath(key), value.toVariant());
      if (key == QLatin1String("backgroundVisible"))
        s.setValue(QStringLiteral("ui_gecko/browser/backgroundPreferenceSet"),
                   true);
      if (key == QLatin1String("backgroundSource")) {
        s.setValue(visualPath(QStringLiteral("theme")), QStringLiteral("flow"));
        s.setValue(visualPath(QStringLiteral("backgroundVisible")), true);
        s.setValue(QStringLiteral("ui_gecko/browser/backgroundPreferenceSet"),
                   true);
      }
    };
  } else
    return;
  const quint64 revision = revision_;
  QPointer<GeckoNewTabController> guard(this);
  view_->commitNewTabAction(event, effect, [guard, revision, apply] {
    if (!guard)
      return;
    guard->refresh();
    if (guard && guard->active() && guard->revision_ == revision && apply) {
      apply();
      if (guard)
        guard->refresh();
    }
  });
}
