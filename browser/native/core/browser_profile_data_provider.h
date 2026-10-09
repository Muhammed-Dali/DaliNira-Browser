#pragma once

#include <QDateTime>
#include <QList>
#include <QString>
#include <QtGlobal>
#include <QUrl>

#include "frequent_site_ranking.h"

#ifndef BROWSER_HISTORY_ENTRY_DEFINED
#define BROWSER_HISTORY_ENTRY_DEFINED
struct BrowserHistoryEntry {
  QString title;
  QUrl url;
  QDateTime visitedAt;
};

struct BrowserFrequentSite {
  QString title;
  QUrl url;
  QUrl iconLookupUrl;
  int visitCount = 0;
  int typedCount = 0;
  QDateTime lastVisitedAt;

  double score(const QDateTime &now = QDateTime::currentDateTimeUtc()) const {
    return dalinira::core::frequent_sites::calculateScore(visitCount, typedCount, lastVisitedAt, 0, now);
  }

  bool isEligibleForNewTab(const QDateTime &now = QDateTime::currentDateTimeUtc()) const {
    return dalinira::core::frequent_sites::isEligibleForNewTab(visitCount, typedCount, lastVisitedAt, url, now);
  }
};
#endif

namespace dalinira::core {

class IBrowserProfileDataProvider {
 public:
  virtual ~IBrowserProfileDataProvider() = default;
  virtual QList<BrowserHistoryEntry> recentHistory() const = 0;
  virtual QList<BrowserFrequentSite> frequentSites(int limit = 6) const = 0;
  virtual QList<BrowserFrequentSite> frequentSites(int limit, const QString &engine) const {
    Q_UNUSED(engine);
    return frequentSites(limit);
  }
  virtual QList<QUrl> bookmarks() const = 0;
  virtual QList<QUrl> quickAccess(const QString &engine = QString{}) const {
    Q_UNUSED(engine);
    return bookmarks();
  }
  virtual bool addQuickAccess(const QUrl &url, const QString &title = QString{}, const QString &engine = QString{}) {
    Q_UNUSED(url); Q_UNUSED(title); Q_UNUSED(engine); return false;
  }
  virtual bool removeQuickAccess(const QUrl &url, const QString &engine = QString{}) {
    Q_UNUSED(url); Q_UNUSED(engine); return false;
  }
  virtual bool isQuickAccess(const QUrl &url, const QString &engine = QString{}) const {
    Q_UNUSED(url); Q_UNUSED(engine); return false;
  }
  virtual QString searchEngine() const { return QStringLiteral("DuckDuckGo"); }
  virtual quint64 totalBlockedCount() const { return 0; }
  virtual quint64 sessionBlockedCount() const { return 0; }
  virtual int recentDownloadCount() const { return 0; }
  virtual QString profilePath() const { return QString(); }
  virtual QStringList hiddenFrequentSites() const { return {}; }
  virtual QStringList hiddenFrequentSites(const QString &engine) const {
    Q_UNUSED(engine);
    return hiddenFrequentSites();
  }
  virtual bool hideFrequentSite(const QUrl &url) { Q_UNUSED(url); return false; }
  virtual bool hideFrequentSite(const QUrl &url, const QString &engine) {
    Q_UNUSED(engine);
    return hideFrequentSite(url);
  }
  virtual bool restoreHiddenFrequentSites() { return false; }
  virtual bool restoreHiddenFrequentSites(const QString &engine) {
    Q_UNUSED(engine);
    return restoreHiddenFrequentSites();
  }
};

}  // namespace dalinira::core
