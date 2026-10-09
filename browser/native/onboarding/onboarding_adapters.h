#pragma once

#include "browser_data_importer.h"
#include "search_privacy_step.h"
#include "core/browser_profile_service.h"
#include "blocker/dalinira_blocker_service.h"

#include <QObject>
#include <QUrl>
#include <QString>

namespace dalinira::onboarding {

class ProfileDataReceiverAdapter final : public QObject, public IBrowserDataReceiver {
 public:
  explicit ProfileDataReceiverAdapter(BrowserProfileService *svc, QObject *parent = nullptr)
      : QObject(parent), svc_(svc) {}

  bool addBookmark(const QUrl &url, const QString &title, const QString &folder) override {
    return svc_ ? svc_->addBookmark(url, title, folder) : false;
  }

  void createBookmarkFolder(const QString &folder) override {
    if (svc_) svc_->createBookmarkFolder(folder);
  }

  void recordHistory(const QUrl &url, const QString &title, bool isReload = false) override {
    if (svc_) svc_->recordHistory(url, title, isReload);
  }

  void recordImportedHistory(const QUrl &url, const QString &title,
                             const QDateTime &visitedAt, int visitCount,
                             int typedCount) override {
    if (!svc_) return;
    svc_->importHistoryEntries({{url, title, visitedAt, visitCount, typedCount}});
  }

  void importHistoryEntries(const QList<ImportedHistoryEntry> &entries) override {
    if (!svc_) return;
    QList<BrowserProfileService::HistoryImportEntry> converted;
    converted.reserve(entries.size());
    for (const auto &entry : entries) {
      converted.append({entry.url, entry.title, entry.visitedAt,
                        entry.visitCount, entry.typedCount});
    }
    svc_->importHistoryEntries(converted);
  }

 private:
  BrowserProfileService *svc_ = nullptr;
};

class ProfileSearchPrivacyAdapter final : public QObject, public ISearchPrivacyPreferences {
 public:
  explicit ProfileSearchPrivacyAdapter(BrowserProfileService *service, QObject *parent = nullptr)
      : QObject(parent), service_(service) {}

  QString searchEngine() const override {
    return service_ ? service_->searchEngine() : QStringLiteral("DuckDuckGo");
  }

  void setSearchEngine(const QString &engine) override {
    if (service_) service_->setSearchEngine(engine);
  }

  bool blockerEnabled() const override {
    return service_ && service_->blockerService()
        ? service_->blockerService()->settings()->protectionEnabled() : true;
  }

  void setBlockerEnabled(bool enabled) override {
    if (service_ && service_->blockerService())
      service_->blockerService()->settings()->setProtectionEnabled(enabled);
  }

 private:
  BrowserProfileService *service_ = nullptr;
};

}  // namespace dalinira::onboarding
