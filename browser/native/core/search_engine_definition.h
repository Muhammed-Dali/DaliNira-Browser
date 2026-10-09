#pragma once

#include <array>

#include <QString>

namespace dalinira::core {

struct SearchEngineDefinition {
  const char *id;
  const char *iconAsset;
  const char *searchUrl;
  const char *suggestUrl;
  const char *queryParam;
};

const std::array<SearchEngineDefinition, 4> &searchEngineDefinitions();
const SearchEngineDefinition &searchEngineDefinition(const QString &engineName);
QString searchEngineIconAsset(const QString &engineName);
QString searchEngineResourcePath(const QString &engineName);
QString searchEnginePlaceholderText(const QString &engineName);

}  // namespace dalinira::core
