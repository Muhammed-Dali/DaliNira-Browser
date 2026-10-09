#pragma once

#include <QDateTime>
#include <QString>
#include <QUrl>
#include <algorithm>
#include <cmath>

namespace dalinira::core::frequent_sites {

// Minimum visit count required before an automatic site is eligible to appear
// as a frequent-site card on the New Tab page.
// Accidental single visits are never promoted.
inline constexpr int kMinimumVisitCountForEligibility = 2;

// Canonical origin/site key helper for deduplication
inline QString frequentSiteKey(const QUrl &url) {
  QString host = url.host().toLower();
  if (host.startsWith(QStringLiteral("www."))) host.remove(0, 4);
  if (host.isEmpty()) return {};
  const int port = url.port();
  if (port > 0 && port != 80 && port != 443) host += QStringLiteral(":%1").arg(port);
  return host;
}

// Check whether a URL is an internal, auth callback, or non-navigable resource
// that should never be recorded or promoted to Frequent Sites.
inline bool isExcludedUrl(const QUrl &url) {
  if (!url.isValid()) return true;
  const QString scheme = url.scheme().toLower();
  if (scheme != QLatin1String("http") && scheme != QLatin1String("https")) return true;

  const QString host = url.host().toLower();
  if (host.isEmpty()) return true;

  // Exclude internal keywords and hostnames
  if (host == QLatin1String("newtab") || host == QLatin1String("settings")
      || host == QLatin1String("dalinira") || host == QLatin1String("devtools")) {
    return true;
  }

  // Exclude auth/SSO callbacks and transient login handshakes
  const QString path = url.path().toLower();
  if (path.contains(QLatin1String("/oauth/callback"))
      || path.contains(QLatin1String("/auth/callback"))
      || path.contains(QLatin1String("/signin-oidc"))
      || path.contains(QLatin1String("/login/callback"))
      || path.endsWith(QLatin1String("/callback"))
      || path.endsWith(QLatin1String("callback.html"))
      || path.endsWith(QLatin1String("/logout"))) {
    return true;
  }

  // Exclude transient download fragments
  if (path.endsWith(QLatin1String(".crdownload")) || path.endsWith(QLatin1String(".part"))) {
    return true;
  }

  return false;
}

// Compute the frecency score combining:
// 1. Visit frequency with rapid-reload damping
// 2. Direct intent bonus (typedCount)
// 3. Recency decay based on days elapsed since the last visit
inline double calculateScore(int visitCount, int typedCount, const QDateTime &lastVisitedAt,
                             int rapidReloads = 0,
                             const QDateTime &now = QDateTime::currentDateTimeUtc()) {
  if (visitCount <= 0) return 0.0;

  // 1. Damped visits: rapid reloads in quick succession get fractional weight (0.1)
  const double effectiveVisits = std::max(0.0, static_cast<double>(visitCount - rapidReloads))
                               + (std::max(0, rapidReloads) * 0.1);

  // 2. Direct user intent: explicitly typed / suggestion-selected navigations carry higher weight
  const double typedBonus = std::max(0, typedCount) * 1.5;

  const double baseScore = effectiveVisits + typedBonus;

  // 3. Recency multiplier: gradual decay so stale sites yield to actively used ones
  double recencyMultiplier = 1.0;
  if (lastVisitedAt.isValid() && lastVisitedAt <= now) {
    const qint64 secsAgo = lastVisitedAt.secsTo(now);
    const double daysAgo = static_cast<double>(secsAgo) / 86400.0;
    if (daysAgo < 1.0) {
      recencyMultiplier = 1.0;
    } else if (daysAgo < 3.0) {
      recencyMultiplier = 0.85;
    } else if (daysAgo < 7.0) {
      recencyMultiplier = 0.70;
    } else if (daysAgo < 14.0) {
      recencyMultiplier = 0.50;
    } else if (daysAgo < 30.0) {
      recencyMultiplier = 0.30;
    } else {
      recencyMultiplier = 0.15;
    }
  }

  return baseScore * recencyMultiplier;
}

// Determine if a site qualifies to be displayed as an automatic New Tab card.
inline bool isEligibleForNewTab(int visitCount, int typedCount, const QDateTime &lastVisitedAt,
                               const QUrl &url,
                               const QDateTime &now = QDateTime::currentDateTimeUtc()) {
  if (isExcludedUrl(url)) return false;
  if (visitCount < kMinimumVisitCountForEligibility) return false;
  const double score = calculateScore(visitCount, typedCount, lastVisitedAt, 0, now);
  return score >= 1.0;
}

}  // namespace dalinira::core::frequent_sites
