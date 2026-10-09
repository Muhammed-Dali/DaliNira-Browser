#include <QCache>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QImageReader>
#include <QDateTime>
#include "new_tab_shortcuts.h"
#include <QSettings>
#include "new_tab_scheme.h"
#include "new_tab_asset_resolver.h"

#include <QBuffer>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QUrlQuery>
#include <QWebEngineUrlRequestJob>
#include <QWebEngineUrlScheme>
#include <QWebEngineUrlSchemeHandler>
#include <QJsonArray>
#include <QJsonObject>
#include <QIcon>
#include <QPointer>
#include <QWebEngineProfile>
#include <QHash>
#include <QUuid>

#include <algorithm>

#include <QCryptographicHash>
#include <QPainter>
#include <QPainterPath>
#include <QDir>
#include "core/browser_icons.h"
#include "core/browser_profile_data_provider.h"
#include "core/security_utils.h"
#include "new_tab_html.h"
#include "i18n/i18n.h"

namespace {
// LocalScheme documents can have an opaque ("null") initiator. A per-handler
// capability authenticates their image requests without trusting opaque origins.
QHash<const dalinira::core::IBrowserProfileDataProvider *, QString> faviconCapabilities;
QHash<const dalinira::core::IBrowserProfileDataProvider *, int> faviconProfileRefs;
QHash<const dalinira::core::IBrowserProfileDataProvider *, QHash<QString, QIcon>> sharedFavicons;
QHash<const dalinira::core::IBrowserProfileDataProvider *, quint64> faviconRevisions;

QString faviconKey(const QUrl &page) {
  const QUrl safe = BrowserSecurity::sanitizeUrlForPersistence(page).adjusted(QUrl::RemoveFragment);
  return safe.toString(QUrl::FullyEncoded);
}

QString faviconOriginKey(const QUrl &page) {
  const QUrl safe = BrowserSecurity::sanitizeUrlForPersistence(page);
  if (!safe.isValid() || safe.host().isEmpty()) return {};
  QUrl root = safe;
  root.setPath(QLatin1String("/"));
  root.setQuery(QString());
  root.setFragment(QString());
  root.setUserInfo(QString());
  return root.toString(QUrl::FullyEncoded);
}

QString faviconHostKey(const QUrl &page) {
  const QUrl safe = BrowserSecurity::sanitizeUrlForPersistence(page);
  return safe.host().toLower();
}

QString faviconStrippedHostKey(const QUrl &page) {
  QString host = faviconHostKey(page);
  if (host.startsWith(QLatin1String("www."))) host = host.mid(4);
  return host;
}
}

QString newTabFaviconCapability(const dalinira::core::IBrowserProfileDataProvider *profileData) {
  if (!profileData) return {};
  auto it = faviconCapabilities.find(profileData);
  if (it != faviconCapabilities.end()) return it.value();
  const QString cap = QUuid::createUuid().toString(QUuid::WithoutBraces);
  faviconCapabilities.insert(profileData, cap);
  return cap;
}

void retainNewTabFaviconProfile(const dalinira::core::IBrowserProfileDataProvider *profileData) {
  if (!profileData) return;
  ++faviconProfileRefs[profileData];
  newTabFaviconCapability(profileData);
}

void releaseNewTabFaviconProfile(const dalinira::core::IBrowserProfileDataProvider *profileData) {
  if (!profileData || !faviconProfileRefs.contains(profileData)) return;
  if (--faviconProfileRefs[profileData] > 0) return;
  faviconProfileRefs.remove(profileData);
  faviconCapabilities.remove(profileData);
  sharedFavicons.remove(profileData);
  faviconRevisions.remove(profileData);
}

void cacheNewTabFavicon(const dalinira::core::IBrowserProfileDataProvider *profileData,
                        const QUrl &page, const QIcon &icon) {
  if (!profileData || icon.isNull()) return;
  const QString exact = faviconKey(page);
  const QString origin = faviconOriginKey(page);
  const QString host = faviconHostKey(page);
  const QString stripped = faviconStrippedHostKey(page);
  if (exact.isEmpty() && origin.isEmpty() && host.isEmpty()) return;

  auto &cache = sharedFavicons[profileData];
  if (!exact.isEmpty()) cache.insert(exact, icon);
  if (!origin.isEmpty()) cache.insert(origin, icon);
  if (!host.isEmpty()) cache.insert(host, icon);
  if (!stripped.isEmpty() && stripped != host) cache.insert(stripped, icon);

  const QString profileDir = profileData->profilePath();
  if (!profileDir.isEmpty()) {
    const QString faviconsDir = QDir(profileDir).filePath(QStringLiteral("newtab_favicons"));
    QDir().mkpath(faviconsDir);
    const QString keyForFile = !stripped.isEmpty() ? stripped : host;
    if (!keyForFile.isEmpty()) {
      const QString hash = QString::fromUtf8(
          QCryptographicHash::hash(keyForFile.toUtf8(), QCryptographicHash::Sha256).toHex());
      const QString filePath = QDir(faviconsDir).filePath(hash + QLatin1String(".png"));
      const QPixmap pix = icon.pixmap(64, 64);
      if (!pix.isNull()) {
        pix.save(filePath, "PNG");
      }
    }
  }

  ++faviconRevisions[profileData];
}

QIcon cachedNewTabFavicon(const dalinira::core::IBrowserProfileDataProvider *profileData,
                          const QUrl &page) {
  if (!profileData) return {};
  const auto profileIt = sharedFavicons.constFind(profileData);
  if (profileIt != sharedFavicons.cend()) {
    const auto &map = profileIt.value();
    const QString exact = faviconKey(page);
    if (!exact.isEmpty()) {
      auto it = map.constFind(exact);
      if (it != map.cend()) return it.value();
    }
    const QString origin = faviconOriginKey(page);
    if (!origin.isEmpty()) {
      auto it = map.constFind(origin);
      if (it != map.cend()) return it.value();
    }
    const QString host = faviconHostKey(page);
    if (!host.isEmpty()) {
      auto it = map.constFind(host);
      if (it != map.cend()) return it.value();
    }
    const QString stripped = faviconStrippedHostKey(page);
    if (!stripped.isEmpty()) {
      auto it = map.constFind(stripped);
      if (it != map.cend()) return it.value();
    }
  }

  // Check persistent disk cache
  const QString profileDir = profileData->profilePath();
  if (!profileDir.isEmpty()) {
    const QString faviconsDir = QDir(profileDir).filePath(QStringLiteral("newtab_favicons"));
    const QString host = faviconHostKey(page);
    const QString stripped = faviconStrippedHostKey(page);
    const QStringList keysToCheck = { stripped, host };
    for (const QString &key : keysToCheck) {
      if (key.isEmpty()) continue;
      const QString hash = QString::fromUtf8(
          QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256).toHex());
      const QString filePath = QDir(faviconsDir).filePath(hash + QLatin1String(".png"));
      if (QFile::exists(filePath)) {
        const QPixmap pix(filePath);
        if (!pix.isNull()) {
          const QIcon icon(pix);
          auto &cache = sharedFavicons[profileData];
          cache.insert(key, icon);
          const QString exact = faviconKey(page);
          if (!exact.isEmpty()) cache.insert(exact, icon);
          return icon;
        }
      }
    }
  }

  return {};
}

QIcon platformPresetFavicon(const QUrl &url) {
  const QString host = url.host().toLower();
  if (host.isEmpty()) return {};

  if (host.endsWith(QLatin1String("duckduckgo.com"))) {
    return BrowserIcons::searchEngineIcon(QStringLiteral("DuckDuckGo"));
  }
  if (host.endsWith(QLatin1String("startpage.com"))) {
    return BrowserIcons::searchEngineIcon(QStringLiteral("Startpage"));
  }
  if (host.endsWith(QLatin1String("mojeek.com"))) {
    return BrowserIcons::searchEngineIcon(QStringLiteral("Mojeek"));
  }
  if (host.endsWith(QLatin1String("google.com")) || host.contains(QLatin1String(".google."))) {
    const QIcon icon = BrowserIcons::searchEngineIcon(QStringLiteral("Google"));
    if (!icon.isNull()) return icon;
  }
  if (host.endsWith(QLatin1String("youtube.com"))) {
    return BrowserIcons::youtubeIcon();
  }
  if (host.endsWith(QLatin1String("github.com"))) {
    QPixmap pixmap(64, 64);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(QStringLiteral("#24292f")));
    p.drawEllipse(QRectF(4, 4, 56, 56));
    p.setBrush(Qt::white);
    QPainterPath path;
    path.addEllipse(QRectF(17, 18, 30, 26));
    QPolygonF leftEar;
    leftEar << QPointF(19, 23) << QPointF(22, 13) << QPointF(27, 19);
    path.addPolygon(leftEar);
    QPolygonF rightEar;
    rightEar << QPointF(45, 23) << QPointF(42, 13) << QPointF(37, 19);
    path.addPolygon(rightEar);
    path.addRoundedRect(QRectF(22, 38, 20, 16), 4, 4);
    p.drawPath(path);
    return QIcon(pixmap);
  }
  if (host.endsWith(QLatin1String("gitlab.com"))) {
    QPixmap pixmap(64, 64);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(QStringLiteral("#fc6d26")));
    painter.drawRoundedRect(QRectF(4, 4, 56, 56), 12, 12);
    painter.setPen(Qt::white);
    QFont font = painter.font();
    font.setBold(true);
    font.setPixelSize(26);
    painter.setFont(font);
    painter.drawText(pixmap.rect(), Qt::AlignCenter, QStringLiteral("GL"));
    return QIcon(pixmap);
  }
  if (host.endsWith(QLatin1String("openai.com")) || host.endsWith(QLatin1String("chatgpt.com"))) {
    QPixmap pixmap(64, 64);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(QStringLiteral("#10a37f")));
    painter.drawEllipse(QRectF(4, 4, 56, 56));
    painter.setPen(Qt::white);
    QFont font = painter.font();
    font.setBold(true);
    font.setPixelSize(26);
    painter.setFont(font);
    painter.drawText(pixmap.rect(), Qt::AlignCenter, QStringLiteral("AI"));
    return QIcon(pixmap);
  }
  if (host.endsWith(QLatin1String("facebook.com"))) {
    QPixmap pixmap(64, 64);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(QStringLiteral("#1877f2")));
    painter.drawEllipse(QRectF(4, 4, 56, 56));
    painter.setPen(Qt::white);
    QFont font = painter.font();
    font.setBold(true);
    font.setPixelSize(40);
    painter.setFont(font);
    painter.drawText(QRectF(8, 0, 52, 64), Qt::AlignCenter, QStringLiteral("f"));
    return QIcon(pixmap);
  }
  if (host.endsWith(QLatin1String("instagram.com"))) {
    QPixmap pixmap(64, 64);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    QLinearGradient grad(0, 64, 64, 0);
    grad.setColorAt(0.0, QColor(QStringLiteral("#f09433")));
    grad.setColorAt(0.3, QColor(QStringLiteral("#e6683c")));
    grad.setColorAt(0.6, QColor(QStringLiteral("#dc2743")));
    grad.setColorAt(1.0, QColor(QStringLiteral("#bc1888")));
    painter.setPen(Qt::NoPen);
    painter.setBrush(grad);
    painter.drawRoundedRect(QRectF(4, 4, 56, 56), 14, 14);
    painter.setPen(QPen(Qt::white, 4.0));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(13, 13, 38, 38), 10, 10);
    painter.drawEllipse(QRectF(22, 22, 20, 20));
    painter.setBrush(Qt::white);
    painter.drawEllipse(QRectF(41, 17, 5, 5));
    return QIcon(pixmap);
  }
  if (host.endsWith(QLatin1String("wikipedia.org"))) {
    QPixmap pixmap(64, 64);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(QStringLiteral("#eef0f2")));
    painter.drawEllipse(QRectF(4, 4, 56, 56));
    painter.setPen(QColor(QStringLiteral("#202122")));
    QFont font = painter.font();
    font.setBold(true);
    font.setPixelSize(34);
    painter.setFont(font);
    painter.drawText(pixmap.rect(), Qt::AlignCenter, QStringLiteral("W"));
    return QIcon(pixmap);
  }
  if (host.endsWith(QLatin1String("ebay.com")) || host.contains(QLatin1String(".ebay."))) {
    QPixmap pixmap(64, 64);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(QStringLiteral("#1e2530")));
    painter.drawRoundedRect(QRectF(4, 4, 56, 56), 14, 14);
    QFont font = painter.font();
    font.setBold(true);
    font.setPixelSize(22);
    painter.setFont(font);
    const int y = 40;
    painter.setPen(QColor(QStringLiteral("#e53238")));
    painter.drawText(8, y, QStringLiteral("e"));
    painter.setPen(QColor(QStringLiteral("#0064d2")));
    painter.drawText(21, y, QStringLiteral("b"));
    painter.setPen(QColor(QStringLiteral("#f5af02")));
    painter.drawText(34, y, QStringLiteral("a"));
    painter.setPen(QColor(QStringLiteral("#86b817")));
    painter.drawText(46, y, QStringLiteral("y"));
    return QIcon(pixmap);
  }
  if (host.endsWith(QLatin1String("amazon.com")) || host.contains(QLatin1String(".amazon."))) {
    QPixmap pixmap(64, 64);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(QStringLiteral("#232f3e")));
    painter.drawRoundedRect(QRectF(4, 4, 56, 56), 14, 14);
    painter.setPen(Qt::white);
    QFont font = painter.font();
    font.setBold(true);
    font.setPixelSize(32);
    painter.setFont(font);
    painter.drawText(QRectF(4, 8, 56, 36), Qt::AlignCenter, QStringLiteral("a"));
    painter.setPen(QPen(QColor(QStringLiteral("#ff9900")), 3.5, Qt::SolidLine, Qt::RoundCap));
    QPainterPath smile;
    smile.moveTo(18, 44);
    smile.quadTo(32, 52, 46, 44);
    painter.drawPath(smile);
    return QIcon(pixmap);
  }
  if (host.endsWith(QLatin1String("twitter.com")) || host == QLatin1String("x.com") || host.endsWith(QLatin1String(".x.com"))) {
    QPixmap pixmap(64, 64);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(QStringLiteral("#111111")));
    painter.drawRoundedRect(QRectF(4, 4, 56, 56), 14, 14);
    painter.setPen(Qt::white);
    QFont font = painter.font();
    font.setBold(true);
    font.setPixelSize(30);
    painter.setFont(font);
    painter.drawText(pixmap.rect(), Qt::AlignCenter, QStringLiteral("𝕏"));
    return QIcon(pixmap);
  }
  if (host.endsWith(QLatin1String("reddit.com"))) {
    QPixmap pixmap(64, 64);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(QStringLiteral("#ff4500")));
    painter.drawEllipse(QRectF(4, 4, 56, 56));
    painter.setBrush(Qt::white);
    painter.drawEllipse(QRectF(14, 20, 36, 26));
    painter.setBrush(QColor(QStringLiteral("#ff4500")));
    painter.drawEllipse(QRectF(22, 28, 6, 6));
    painter.drawEllipse(QRectF(36, 28, 6, 6));
    return QIcon(pixmap);
  }
  if (host.endsWith(QLatin1String("threads.net"))) {
    QPixmap pixmap(64, 64);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(QStringLiteral("#101010")));
    painter.drawEllipse(QRectF(4, 4, 56, 56));
    painter.setPen(Qt::white);
    QFont font = painter.font();
    font.setBold(true);
    font.setPixelSize(36);
    painter.setFont(font);
    painter.drawText(pixmap.rect(), Qt::AlignCenter, QStringLiteral("@"));
    return QIcon(pixmap);
  }

  return {};
}

QIcon platformIconForBookmark(const QUrl &url, const dalinira::core::IBrowserProfileDataProvider *profileData, bool allowGenericFallback) {
  if (profileData) {
    const QIcon cached = cachedNewTabFavicon(profileData, url);
    if (!cached.isNull()) return cached;
  }
  const QIcon preset = platformPresetFavicon(url);
  if (!preset.isNull()) return preset;
  if (!allowGenericFallback) return {};

  const QString host = url.host().toLower();
  QString cleanHost = host;
  if (cleanHost.startsWith(QLatin1String("www."))) cleanHost = cleanHost.mid(4);
  QPixmap pixmap(24, 24);
  pixmap.fill(Qt::transparent);
  QPainter painter(&pixmap);
  painter.setRenderHint(QPainter::Antialiasing);
  QString initial = cleanHost.section(QLatin1Char('.'), 0, 0).left(1).toUpper();
  if (initial.isEmpty()) initial = QStringLiteral("★");
  const QColor bg = QColor::fromHsv(int(qHash(cleanHost) % 360), 140, 180);
  painter.setPen(Qt::NoPen);
  painter.setBrush(bg);
  painter.drawRoundedRect(QRectF(1, 1, 22, 22), 4, 4);
  painter.setPen(Qt::white);
  QFont font = painter.font();
  font.setBold(true);
  font.setPixelSize(12);
  painter.setFont(font);
  painter.drawText(pixmap.rect(), Qt::AlignCenter, initial);
  return QIcon(pixmap);
}

QIcon platformIconForBookmark(const QUrl &url, bool allowGenericFallback) {
  return platformIconForBookmark(url, nullptr, allowGenericFallback);
}

bool authorizeNewTabFaviconUrl(const dalinira::core::IBrowserProfileDataProvider *profileData,
                               const QUrl &requestUrl, QUrl *pageOut) {
  const QUrl page = BrowserSecurity::sanitizeUrlForPersistence(
      QUrl(QUrlQuery(requestUrl).queryItemValue(QStringLiteral("page"), QUrl::FullyDecoded)));
  const bool allowed = profileData && requestUrl.host() == QLatin1String("newtab") &&
      requestUrl.path() == QLatin1String("/favicon") && requestUrl.toString().size() <= 8192 &&
      QUrlQuery(requestUrl).queryItemValue(QStringLiteral("cap")) == faviconCapabilities.value(profileData) &&
      page.isValid() && !page.host().isEmpty() &&
      (page.scheme() == QLatin1String("http") || page.scheme() == QLatin1String("https"));
  if (allowed && pageOut) *pageOut = page;
  return allowed;
}

QString newTabFaviconUrl(const dalinira::core::IBrowserProfileDataProvider *profileData, const QUrl &page) {
  const QUrl safe = BrowserSecurity::sanitizeUrlForPersistence(page);
  if (!safe.isValid() || safe.host().isEmpty() ||
      (safe.scheme() != QLatin1String("http") && safe.scheme() != QLatin1String("https"))) return {};
  const QString cap = newTabFaviconCapability(profileData);
  if (cap.isEmpty()) return {};
  QUrl domainUrl = safe;
  domainUrl.setPath(QLatin1String("/"));
  domainUrl.setQuery(QString());
  domainUrl.setFragment(QString());
  domainUrl.setUserInfo(QString());
  QUrl icon(QStringLiteral("dalinira://newtab/favicon"));
  QUrlQuery query;
  query.addQueryItem(QStringLiteral("page"), domainUrl.toString(QUrl::FullyEncoded));
  query.addQueryItem(QStringLiteral("cap"), cap);
  query.addQueryItem(QStringLiteral("v"), QString::number(faviconRevisions.value(profileData)));
  icon.setQuery(query);
  return icon.toString(QUrl::FullyEncoded);
}

void requestNewTabShortcutFavicon(const dalinira::core::IBrowserProfileDataProvider *profileData,
                                  const QUrl &page, QObject *owner, std::function<void()> ready) {
  if (!owner || !profileData || dalinira::newtab::shortcuts::validatedUrl(page.toString()).isEmpty()) return;
  bool configured = false;
  for (const auto &value : dalinira::newtab::shortcuts::load())
    if (QUrl(value.toObject().value(QStringLiteral("url")).toString()) == page) configured = true;
  if (!configured) return;
  if (!cachedNewTabFavicon(profileData, page).isNull()) return;
  const QIcon preset = platformPresetFavicon(page);
  if (!preset.isNull()) { cacheNewTabFavicon(profileData, page, preset); return; }
  QUrl target = page;target.setPath(QStringLiteral("/favicon.ico"));target.setQuery(QString());target.setFragment(QString());
  auto attempts = owner->property("dalinira-shortcut-icon-attempts").toMap();
  const QString key = target.toString(QUrl::FullyEncoded);
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  if (attempts.contains(key) && now - attempts.value(key).toLongLong() < 300000) return;
  attempts.insert(key, now);owner->setProperty("dalinira-shortcut-icon-attempts", attempts);
  // Fetch only the configured site's favicon, without credentials, cookies,
  // redirects or a third-party favicon service. Keep page remote-access rules intact.
  auto *network = new QNetworkAccessManager(owner);
  QNetworkRequest request(target);
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
  request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
  request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
  request.setTransferTimeout(5000);
  auto *reply = network->get(request);
  constexpr qint64 maxBytes = 1024 * 1024;
  QObject::connect(reply, &QNetworkReply::readyRead, reply, [reply] {
    if (reply->bytesAvailable() > maxBytes) reply->abort();
  });
  QObject::connect(reply, &QNetworkReply::finished, owner, [reply,network,profileData,page,ready=std::move(ready)] {
    if (reply->error() == QNetworkReply::NoError && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200
        && reply->bytesAvailable() <= maxBytes) {
      QByteArray bytes = reply->readAll();QBuffer buffer(&bytes);buffer.open(QIODevice::ReadOnly);
      QImageReader reader(&buffer);const QSize size = reader.size();
      if (size.isValid() && size.width() <= 1024 && size.height() <= 1024) {
        const QImage image = reader.read();
        if (!image.isNull()) { cacheNewTabFavicon(profileData,page,QIcon(QPixmap::fromImage(image)));ready(); }
      }
    }
    network->deleteLater();
  });
}

QJsonArray collectNewTabFrequentSites(const dalinira::core::IBrowserProfileDataProvider *profileData,
                                      int limit,
                                      const QString &engine) {
  QJsonArray result;
  const int cappedLimit = std::clamp(limit, 0, 6);
  if (!profileData || cappedLimit == 0) return result;

  const QStringList hiddenList = profileData->hiddenFrequentSites(engine);
  QSet<QString> hiddenOrigins;
  for (const QString &item : hiddenList) {
    const QString trimmed = item.trimmed().toLower();
    if (trimmed.isEmpty()) continue;
    hiddenOrigins.insert(trimmed);
    const QString key = dalinira::core::frequent_sites::frequentSiteKey(QUrl::fromUserInput(trimmed));
    if (!key.isEmpty()) hiddenOrigins.insert(key);
  }

  QSet<QString> seenOrigins;

  for (const BrowserFrequentSite &site : profileData->frequentSites(100, engine)) {
    if (!site.isEligibleForNewTab()) {
      continue;
    }

    const QUrl url = BrowserSecurity::sanitizeUrlForPersistence(site.url).adjusted(QUrl::RemoveFragment);
    const QString scheme = url.scheme().toLower();
    if (!url.isValid() || url.host().isEmpty()
        || (scheme != QLatin1String("http") && scheme != QLatin1String("https"))) {
      continue;
    }

    const QString originKey = dalinira::core::frequent_sites::frequentSiteKey(url);
    if (originKey.isEmpty() || seenOrigins.contains(originKey)
        || hiddenOrigins.contains(originKey) || hiddenOrigins.contains(url.host().toLower())) {
      continue;
    }
    seenOrigins.insert(originKey);

    QString title = site.title.trimmed().left(180);
    if (title.isEmpty() || I18n::isTranslationOf(QStringLiteral("newtab.title"), title)) title = url.host();
    QUrl lookup = BrowserSecurity::sanitizeUrlForPersistence(site.iconLookupUrl);
    if (!lookup.isValid() || lookup.host() != url.host()
        || (lookup.scheme() != QLatin1String("http") && lookup.scheme() != QLatin1String("https"))) lookup = url;
    result.append(QJsonObject{
        {QStringLiteral("icon"), newTabFaviconUrl(profileData, lookup)},
        {QStringLiteral("url"), url.toString(QUrl::FullyEncoded)},
        {QStringLiteral("name"), title},
        {QStringLiteral("title"), title},
        {QStringLiteral("visitCount"), std::max(1, site.visitCount)}});
    if (result.size() >= cappedLimit) break;
  }
  return result;
}

QJsonArray collectNewTabQuickAccess(const dalinira::core::IBrowserProfileDataProvider *profileData,
                                    int limit,
                                    const QString &engine) {
  QJsonArray result;
  const int cappedLimit = std::clamp(limit, 0, 6);
  if (!profileData || cappedLimit == 0) return result;

  for (const QUrl &quickItem : profileData->quickAccess(engine)) {
    const QUrl url = BrowserSecurity::sanitizeUrlForPersistence(quickItem).adjusted(QUrl::RemoveFragment);
    const QString scheme = url.scheme().toLower();
    if (!url.isValid() || url.host().isEmpty()
        || (scheme != QLatin1String("http") && scheme != QLatin1String("https"))) {
      continue;
    }

    result.append(QJsonObject{
        {QStringLiteral("icon"), newTabFaviconUrl(profileData, url)},
        {QStringLiteral("url"), url.toString(QUrl::FullyEncoded)},
        {QStringLiteral("name"), url.host()},
        {QStringLiteral("title"), url.host()}});
    if (result.size() >= cappedLimit) break;
  }
  return result;
}

QJsonArray collectNewTabBookmarks(const dalinira::core::IBrowserProfileDataProvider *profileData,
                                  int limit,
                                  const QString &engine) {
  return collectNewTabQuickAccess(profileData, limit, engine);
}

namespace {

class NewTabSchemeHandler final : public QWebEngineUrlSchemeHandler {
 public:
  NewTabSchemeHandler(const QString &assetsDirectory, const QString &managedBackgroundPath,
                      const QString &managedThumbnailPath,
                      dalinira::core::IBrowserProfileDataProvider *profileData, QObject *parent,
                      QWebEngineProfile *webProfile)
      : QWebEngineUrlSchemeHandler(parent), assetsDirectory_(assetsDirectory),
        managedBackgroundPath_(managedBackgroundPath), managedThumbnailPath_(managedThumbnailPath),
        profileData_(profileData), webProfile_(webProfile),
        managedBackgroundCapability_(QUuid::createUuid().toString(QUuid::WithoutBraces)) {
    retainNewTabFaviconProfile(profileData_);
    faviconCapability_ = faviconCapabilities.value(profileData_);
  }

  ~NewTabSchemeHandler() override {
    releaseNewTabFaviconProfile(profileData_);
  }

  void requestStarted(QWebEngineUrlRequestJob *job) override {
    const QUrl url = job->requestUrl();
    if (url.host() == QLatin1String("bypass-strictblock")) {
      if (!isAuthorizedStrictBlockBypass(url, job->initiator())) {
        job->fail(QWebEngineUrlRequestJob::RequestDenied);
        return;
      }
      const QUrlQuery query(url);
      const QString domain = query.queryItemValue(QStringLiteral("domain"), QUrl::FullyDecoded);
      const QUrl target = validatedStrictBlockTarget(domain, query.queryItemValue(QStringLiteral("target"), QUrl::FullyDecoded));
      const QString redirectHtml = I18n::localizeHtml(QStringLiteral(
          "<!doctype html><html lang=\"{{i18n-lang}}\" dir=\"ltr\"><head><meta http-equiv=\"refresh\" content=\"0;url=%1\"></head>"
          "<body style=\"background:#0c1017;color:#fff;font-family:sans-serif;display:grid;place-items:center;height:100vh;\">"
          "<p>{{i18n:blockpage.temporary_allow_redirect}}</p></body></html>"
      )).arg(target.toString(QUrl::FullyEncoded).toHtmlEscaped());
      auto *buffer = new QBuffer(job);
      buffer->setData(redirectHtml.toUtf8());
      buffer->open(QIODevice::ReadOnly);
      job->reply("text/html; charset=utf-8", buffer);
      return;
    }
    if (url.host() == QLatin1String("navigate")) {
      job->fail(QWebEngineUrlRequestJob::RequestDenied);
      return;
    }
    if (url.host() == QLatin1String("blocked")) {
      auto *buffer = new QBuffer(job);
      buffer->setData(adultBlockedWarningHtml().toUtf8());
      buffer->open(QIODevice::ReadOnly);
      job->reply("text/html; charset=utf-8", buffer);
      return;
    }
    if (url.host() != QLatin1String("newtab") && url.host() != QLatin1String("incognito")) { job->fail(QWebEngineUrlRequestJob::UrlNotFound); return; }
    const QString requested = url.path();
    if (requested == QLatin1String("/favicon")) {
      QUrl page;
      if (!webProfile_ || !authorizeNewTabFaviconUrl(profileData_, url, &page)) {
        job->fail(QWebEngineUrlRequestJob::RequestDenied);
        return;
      }
      const QString cacheKey = page.toString(QUrl::FullyEncoded);
      const QIcon sharedIcon = cachedNewTabFavicon(profileData_, page);
      if (!sharedIcon.isNull()) {
        auto *buffer = new QBuffer(job);
        buffer->open(QIODevice::ReadWrite);
        if (sharedIcon.pixmap(64, 64).save(buffer, "PNG") && buffer->size() > 0) {
          buffer->seek(0);
          job->reply("image/png", buffer);
          return;
        }
        delete buffer;
      }
      if (const auto *cached = faviconCache_.object(cacheKey)) {
        if (cached->isEmpty()) { job->fail(QWebEngineUrlRequestJob::UrlNotFound); return; }
        auto *buffer = new QBuffer(job); buffer->setData(*cached); buffer->open(QIODevice::ReadOnly);
        job->reply("image/png", buffer); return;
      }
      if (pendingFavicons_ >= 16) { job->fail(QWebEngineUrlRequestJob::RequestFailed); return; }
      ++pendingFavicons_;
      const QPointer<NewTabSchemeHandler> handler(this);
      const QPointer<QWebEngineUrlRequestJob> guardedJob(job);
      QUrl rootPage = page;
      rootPage.setPath(QLatin1String("/"));
      rootPage.setQuery(QString());
      rootPage.setFragment(QString());
      rootPage.setUserInfo(QString());
      auto sendIcon = [guardedJob, handler, cacheKey, page](const QIcon &icon, const QUrl &iconUrl) -> bool {
        if (!handler || !guardedJob) return false;
        if (icon.isNull()) return false;
        if (!iconUrl.isEmpty()) {
          const QUrl safe = BrowserSecurity::sanitizeUrlForPersistence(iconUrl);
          if (!safe.isValid() || (safe.scheme() != QLatin1String("http") && safe.scheme() != QLatin1String("https"))) {
            return false;
          }
        }
        auto *buffer = new QBuffer(guardedJob);
        buffer->open(QIODevice::ReadWrite);
        if (!icon.pixmap(64, 64).save(buffer, "PNG") || buffer->size() == 0) {
          delete buffer;
          return false;
        }
        handler->faviconCache_.insert(cacheKey, new QByteArray(buffer->data()));
        cacheNewTabFavicon(handler->profileData_, page, icon);
        buffer->seek(0);
        guardedJob->reply("image/png", buffer);
        return true;
      };

      webProfile_->requestIconForPageURL(page, 64,
          [guardedJob, handler, cacheKey, rootPage, page, sendIcon](const QIcon &icon, const QUrl &iconUrl, const QUrl &) {
        if (!handler) return;
        if (sendIcon(icon, iconUrl)) {
          --handler->pendingFavicons_;
          return;
        }
        if (page != rootPage && handler->webProfile_) {
          handler->webProfile_->requestIconForPageURL(rootPage, 64,
              [guardedJob, handler, cacheKey, page, sendIcon](const QIcon &rootIcon, const QUrl &rootIconUrl, const QUrl &) {
            if (!handler) return;
            --handler->pendingFavicons_;
            if (!guardedJob) return;
            if (sendIcon(rootIcon, rootIconUrl)) return;
            const QIcon platform = platformPresetFavicon(page);
            if (!platform.isNull() && sendIcon(platform, QUrl())) return;
            guardedJob->fail(QWebEngineUrlRequestJob::UrlNotFound);
          });
          return;
        }
        --handler->pendingFavicons_;
        if (!guardedJob) return;
        const QIcon platform = platformPresetFavicon(page);
        if (!platform.isNull() && sendIcon(platform, QUrl())) return;
        guardedJob->fail(QWebEngineUrlRequestJob::UrlNotFound);
      });
      return;
    }
    if (requested.isEmpty() || requested == QLatin1String("/")) {
      const QUrlQuery query(url);
      if (query.hasQueryItem(QStringLiteral("strictblock"))) {
        const QString domain = query.queryItemValue(QStringLiteral("domain"), QUrl::FullyDecoded);
        const QString targetUrl = query.queryItemValue(QStringLiteral("url"), QUrl::FullyDecoded);
        if (!validatedStrictBlockTarget(domain, targetUrl).isValid()) {
          job->fail(QWebEngineUrlRequestJob::RequestDenied);
          return;
        }
        auto *buffer = new QBuffer(job);
        buffer->setData(strictBlockWarningHtml(domain, targetUrl).toUtf8());
        buffer->open(QIODevice::ReadOnly);
        job->reply("text/html; charset=utf-8", buffer);
        return;
      }
      QString engine = profileData_ ? profileData_->searchEngine() : QStringLiteral("DuckDuckGo");

      const bool isIncognitoSession = (webProfile_ && webProfile_->isOffTheRecord())
                                      || url.host() == QLatin1String("incognito")
                                      || query.hasQueryItem(QStringLiteral("incognito"));
      if (isIncognitoSession) {
        auto *buffer = new QBuffer(job);
        buffer->setData(incognitoNewTabHtml(engine).toUtf8());
        buffer->open(QIODevice::ReadOnly);
        job->reply("text/html; charset=utf-8", buffer);
        return;
      }

      const QJsonArray frequentSitesArray = collectNewTabFrequentSites(profileData_, 6, QStringLiteral("chromium"));
      const QJsonArray bookmarksArray = collectNewTabBookmarks(profileData_, 6, QStringLiteral("chromium"));
      QSettings settings;
      const bool legacyCards = settings.value(QStringLiteral("browser/cards"), false).toBool();
      const bool showDownloadsCard = settings.value(QStringLiteral("browser/showDownloadsCard"), legacyCards).toBool();
      const bool showBlockedCard = settings.value(QStringLiteral("browser/showBlockedCard"), legacyCards).toBool();
      const QString blockedCounterMode = settings.value(QStringLiteral("browser/blockedCounterMode"), QStringLiteral("session")).toString();

      auto *buffer = new QBuffer(job);
      buffer->setData(newTabHtml(engine, frequentSitesArray, bookmarksArray,
                                 profileData_ ? profileData_->totalBlockedCount() : 0,
                                 profileData_ ? profileData_->recentDownloadCount() : 0,
                                 profileData_ ? profileData_->sessionBlockedCount() : 0,
                                 showDownloadsCard,
                                 showBlockedCard,
                                 blockedCounterMode,
                                 managedBackgroundCapability_).toUtf8());
      buffer->open(QIODevice::ReadOnly);
      job->reply("text/html; charset=utf-8", buffer);
      return;
    }
    // Static assets come from the allow-list shared with the WebKit engine;
    // no path from an URL is ever joined directly into the filesystem path.
    NewTabStaticAsset staticAsset;
    if (newTabStaticAsset(requested, &staticAsset)) {
      const QByteArray data = readNewTabStaticAsset(staticAsset, assetsDirectory_);
      if (data.isEmpty()) { job->fail(QWebEngineUrlRequestJob::UrlNotFound); return; }
      auto *buffer = new QBuffer(job);
      buffer->setData(data);
      buffer->open(QIODevice::ReadOnly);
      job->reply(staticAsset.mimeType, buffer);
      return;
    }
    const bool managedImage = requested == QLatin1String("/managed-background");
    const bool managedThumbnail = requested == QLatin1String("/managed-background-thumbnail");
    if (!managedImage && !managedThumbnail) { job->fail(QWebEngineUrlRequestJob::UrlNotFound); return; }
    if (QUrlQuery(url).queryItemValue(QStringLiteral("cap")) != managedBackgroundCapability_) {
      job->fail(QWebEngineUrlRequestJob::RequestDenied);
      return;
    }
    QFile file(managedImage ? managedBackgroundPath_ : managedThumbnailPath_);
    if (!file.open(QIODevice::ReadOnly)) { job->fail(QWebEngineUrlRequestJob::UrlNotFound); return; }
    auto *buffer = new QBuffer(job);
    buffer->setData(file.readAll());
    buffer->open(QIODevice::ReadOnly);
    job->reply(managedImage ? QByteArrayLiteral("image/png") : QByteArrayLiteral("image/jpeg"), buffer);
  }

 private:
  QString assetsDirectory_;
  QString managedBackgroundPath_;
  QString managedThumbnailPath_;
  dalinira::core::IBrowserProfileDataProvider *profileData_ = nullptr;
  QPointer<QWebEngineProfile> webProfile_;
  QString faviconCapability_;
  QString managedBackgroundCapability_;
  QCache<QString, QByteArray> faviconCache_{64};
  int pendingFavicons_ = 0;
};

}  // namespace

void registerDaliNiraUrlSchemes() {
  QWebEngineUrlScheme scheme("dalinira");
  scheme.setSyntax(QWebEngineUrlScheme::Syntax::Host);
  // LocalAccessAllowed is required for relative packaged assets on this
  // scheme. Profile-wide LocalContentCanAccessFileUrls remains disabled, and
  // the handler itself serves only an explicit path allow-list.
  scheme.setFlags(QWebEngineUrlScheme::SecureScheme | QWebEngineUrlScheme::LocalScheme
      | QWebEngineUrlScheme::LocalAccessAllowed);
  QWebEngineUrlScheme::registerScheme(scheme);
}

QWebEngineUrlSchemeHandler *createNewTabSchemeHandler(const QString &assetsDirectory, const QString &managedBackgroundPath,
                                                      const QString &managedThumbnailPath,
                                                      dalinira::core::IBrowserProfileDataProvider *profileData,
                                                      QObject *parent, QWebEngineProfile *webProfile) {
  return new NewTabSchemeHandler(assetsDirectory, managedBackgroundPath, managedThumbnailPath, profileData, parent, webProfile);
}
