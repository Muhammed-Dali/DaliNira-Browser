#pragma once

#include <QJsonArray>
#include <QString>
#include <QtGlobal>
#include <QUrl>

QString searchEnginePlaceholder(const QString &engine);

QString newTabHtml(const QString &defaultEngine,
                   const QJsonArray &frequentSites = {},
                   const QJsonArray &bookmarks = {},
                   quint64 totalBlockedCount = 0,
                   int recentDownloadCount = 0,
                   quint64 sessionBlockedCount = 0,
                   bool showDownloadsCard = false,
                   bool showBlockedCard = false,
                   const QString &blockedCounterMode = QStringLiteral("session"),
                   const QString &managedBackgroundCapability = QString{});
QString incognitoNewTabHtml(const QString &defaultEngine);
QString newTabTopSitesUpdateScript(const QJsonArray &frequentSites,
                                   const QJsonArray &bookmarks);
QString newTabProtectionStatsUpdateScript(quint64 totalBlockedCount,
                                          int recentDownloadCount = 0,
                                          qint64 sessionBlockedCount = -1);
QString newTabCardSettingsUpdateScript(bool showDownloadsCard,
                                       bool showBlockedCard,
                                       const QString &blockedCounterMode);
QString newTabBackgroundStateUpdateScript(bool available, quint64 revision);
QString newTabBackgroundResultScript(bool ok, const QString &message,
                                     bool available, quint64 revision,
                                     bool selectCustom);
QString strictBlockWarningHtml(const QString &domain, const QString &targetUrl);
QUrl validatedStrictBlockTarget(const QString &domain, const QString &targetUrl);
bool isAuthorizedStrictBlockBypass(const QUrl &requestUrl, const QUrl &initiator);
QString adultBlockedWarningHtml();
