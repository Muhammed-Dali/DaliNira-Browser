#pragma once

#include <QJsonArray>
#include <QIcon>
#include <QObject>
#include <QUrl>
#include <functional>

class QWebEngineUrlSchemeHandler;
class QWebEngineProfile;

namespace dalinira::core {
class IBrowserProfileDataProvider;
}

void registerDaliNiraUrlSchemes();
void retainNewTabFaviconProfile(const dalinira::core::IBrowserProfileDataProvider *profileData);
void releaseNewTabFaviconProfile(const dalinira::core::IBrowserProfileDataProvider *profileData);
void cacheNewTabFavicon(const dalinira::core::IBrowserProfileDataProvider *profileData,
                        const QUrl &page, const QIcon &icon);
QIcon cachedNewTabFavicon(const dalinira::core::IBrowserProfileDataProvider *profileData,
                          const QUrl &page);
QIcon platformPresetFavicon(const QUrl &url);
QIcon platformIconForBookmark(const QUrl &url, bool allowGenericFallback = true);
QIcon platformIconForBookmark(const QUrl &url, const dalinira::core::IBrowserProfileDataProvider *profileData, bool allowGenericFallback = true);
bool authorizeNewTabFaviconUrl(const dalinira::core::IBrowserProfileDataProvider *profileData,
                               const QUrl &requestUrl, QUrl *pageOut);
QString newTabFaviconUrl(const dalinira::core::IBrowserProfileDataProvider *profileData, const QUrl &page);
void requestNewTabShortcutFavicon(const dalinira::core::IBrowserProfileDataProvider *profileData,
                                 const QUrl &page, QObject *owner, std::function<void()> ready);
QJsonArray collectNewTabFrequentSites(const dalinira::core::IBrowserProfileDataProvider *profileData,
                                      int limit = 6,
                                      const QString &engine = QString{});
QJsonArray collectNewTabBookmarks(const dalinira::core::IBrowserProfileDataProvider *profileData,
                                  int limit = 6,
                                  const QString &engine = QString{});
QJsonArray collectNewTabQuickAccess(const dalinira::core::IBrowserProfileDataProvider *profileData,
                                    int limit = 6,
                                    const QString &engine = QString{});
QWebEngineUrlSchemeHandler *createNewTabSchemeHandler(const QString &assetsDirectory, const QString &managedBackgroundPath,
                                                      const QString &managedThumbnailPath,
                                                      dalinira::core::IBrowserProfileDataProvider *profileData = nullptr,
                                                      QObject *parent = nullptr,
                                                      QWebEngineProfile *webProfile = nullptr);
