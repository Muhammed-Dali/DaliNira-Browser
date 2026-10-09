#include "search_engine_definition.h"

#include "i18n/i18n.h"

namespace dalinira::core {

const std::array<SearchEngineDefinition, 4> &searchEngineDefinitions() {
  static const std::array<SearchEngineDefinition, 4> definitions{{
      {"Google", "google.ico", "https://www.google.com/search", "https://suggestqueries.google.com/complete/search?client=firefox&oe=utf-8", "q"},
      {"DuckDuckGo", "duckduckgo.ico", "https://duckduckgo.com/", "https://duckduckgo.com/ac/?type=list", "q"},
      {"Startpage", "startpage.ico", "https://www.startpage.com/sp/search", "https://www.startpage.com/osuggestions", "query"},
      {"Mojeek", "mojeek.ico", "https://www.mojeek.com/search", "", "q"},
  }};
  return definitions;
}

const SearchEngineDefinition &searchEngineDefinition(const QString &engineName) {
  const QString lower = engineName.trimmed().toLower();
  const auto &definitions = searchEngineDefinitions();
  if (lower.contains(QLatin1String("duckduckgo")) || lower.contains(QLatin1String("duck")))
    return definitions[1];
  if (lower.contains(QLatin1String("startpage")))
    return definitions[2];
  if (lower.contains(QLatin1String("mojeek")))
    return definitions[3];
  if (lower.contains(QLatin1String("google")) || lower.contains(QLatin1String("brave")) || lower.contains(QLatin1String("bing")))
    return definitions[0];
  return definitions[0];
}

QString searchEngineIconAsset(const QString &engineName) {
  return QString::fromLatin1(searchEngineDefinition(engineName).iconAsset);
}

QString searchEngineResourcePath(const QString &engineName) {
  return QStringLiteral(":/search-engines/") + searchEngineIconAsset(engineName);
}

QString searchEnginePlaceholderText(const QString &engineName) {
  // Built-in engines (and legacy aliases that resolve to one) show the engine
  // they actually use; a custom engine shows its own name.
  const QString trimmed = engineName.trimmed();
  const QString lower = trimmed.toLower();
  static const char *const kBuiltInHints[] = {"google", "duck", "startpage", "mojeek", "brave", "bing"};
  bool builtIn = trimmed.isEmpty();
  for (const char *hint : kBuiltInHints) builtIn = builtIn || lower.contains(QLatin1String(hint));
  if (!builtIn) return I18n::text(QStringLiteral("newtab.search_placeholder")).arg(trimmed);
  // Per-engine keys keep natural grammar (e.g. Turkish vowel harmony:
  // "Google'da", "Startpage'de", "Mojeek'te").
  const QString id = QString::fromLatin1(searchEngineDefinition(engineName).id).toLower();
  return I18n::text(QStringLiteral("newtab.search_placeholder_") + id);
}

}  // namespace dalinira::core
