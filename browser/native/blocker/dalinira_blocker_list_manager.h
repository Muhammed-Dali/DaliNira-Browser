#pragma once

#include <QDateTime>
#include <QHash>
#include <QCache>
#include <QJsonObject>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QPair>
#include <QString>
#include <atomic>
#include <memory>

#include "dalinira_blocker_filter_updates.h"
#include "dalinira_blocker_redirect_resources.h"
#include "dalinira_blocker_types.h"

struct FilterListInfo {
  QString id;
  QString name;
  QString group;
  QString description;
  bool enabled = true;
  int ruleCount = 0;
  QDateTime lastUpdated;
  QString localFilePath;
  QString downloadUrl;
  QString version;
  QString provider;
  QString projectUrl;
  QString source;
  QString sourceUrl;
  QString license;
  QString licenseUrl;
  QString formatOrigin;
  bool attributionReviewRequired = false;
  // Remote updates (DaliNiraBlockerFilterStore). updateUrl is empty for lists
  // that only ship with the application; activeSource is "bundled", "remote"
  // or "remote-previous" (the last known good download).
  QString updateUrl;
  QString activeSource = QStringLiteral("bundled");
  QDateTime lastUpdateCheck;
  QString lastUpdateResult;
  QString lastUpdateFailure;
};

class DaliNiraBlockerListManager final : public QObject {
  Q_OBJECT
 public:
  explicit DaliNiraBlockerListManager(const QString &dataDir, QObject *parent = nullptr);
  ~DaliNiraBlockerListManager() override;

  QList<FilterListInfo> availableLists() const;
  QStringList resolveRulesetIds(DaliNiraBlockerMode mode, const QStringList &enabledIds,
                                bool selectionConfigured) const;
  // Network rules of the active lists. Text sources (downloaded copies and
  // `customLines`) are compiled with every $badfilter key they contain, which
  // is also returned in `badfilterKeys` for compiling the custom lines.
  QList<FilterRule> loadRulesForModeAndSelection(DaliNiraBlockerMode mode, const QStringList &enabledIds,
                                                  bool selectionConfigured = false,
                                                  bool strictBlock = false,
                                                  const QStringList &customLines = {},
                                                  QSet<QString> *badfilterKeys = nullptr);
  QString loadCosmeticCssForSelection(const QStringList &enabledIds, bool selectionConfigured = false) const;
  QString loadSpecificCosmeticCssForHost(const QString &host, const QStringList &enabledIds,
                                         bool selectionConfigured = false) const;
  QJsonArray loadProceduralRulesForHost(const QString &host, const QStringList &enabledIds,
                                        bool selectionConfigured = false) const;
  // Returns generated assets as {world, source}.
  // `includeGeneric` false leaves out the generic cosmetic assets (a
  // $generichide exception applies to the page); scriptlets stay.
  QList<QPair<QString, QString>> loadScriptingSourcesForHost(const QString &host,
                                                             const QStringList &enabledIds,
                                                             bool selectionConfigured = false,
                                                             bool includeGeneric = true) const;

  void invalidateCaches();
  QString rulesetDir() const;
  // Downloaded copies of lists with a configured upstream source; null when
  // the profile has no data directory. Network rules of such a list come from
  // its validated copy when one exists, otherwise from the bundled ruleset.
  DaliNiraBlockerFilterStore *filterStore() const { return filterStore_.get(); }
  // The packaged redirect resources every rule source substitutes from.
  const DaliNiraBlockerRedirectResources *redirectResources() const { return redirectResources_.get(); }
  // Generated rules the last load left out because they need the response
  // (response-header conditions, modifyHeaders): neither engine exposes it.
  int responsePhaseRulesSkipped() const { return responsePhaseRulesSkipped_.load(); }

 signals:
 private:
  void initRulesetCatalog();
  QString findRulesetFilePath(const QString &fileName) const;
  QList<FilterRule> parseRulesetFile(const QString &filePath, const QString &rulesetId,
                                     int *responsePhaseSkipped = nullptr);
  QJsonObject cachedScriptingJson(const QString &path) const;

  QString dataDir_;
  mutable QMutex mutex_;
  QList<FilterListInfo> lists_;
  mutable QCache<QString, QString> scriptingSourceCache_{16 * 1024 * 1024};
  mutable QCache<QString, QJsonObject> scriptingJsonCache_{16 * 1024 * 1024};
  mutable QHash<QString, bool> scriptingApplicabilityCache_;
  std::unique_ptr<DaliNiraBlockerFilterStore> filterStore_;
  std::unique_ptr<DaliNiraBlockerRedirectResources> redirectResources_;
  std::atomic<int> responsePhaseRulesSkipped_{0};
};

using AdBlockFilterListManager = DaliNiraBlockerListManager;
