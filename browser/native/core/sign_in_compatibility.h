#pragma once

#include <QByteArray>
#include <QUrl>

namespace dalinira::sign_in_compatibility {

// DaliNira removes Qt WebEngine's "QtWebEngine/x.y" token from the Chromium
// User-Agent (browser_profile_service.cpp). Google's sign-in then sees a
// browser that names itself Chrome but is not Google Chrome and refuses it
// with "This browser or app may not be secure". With the engine's own
// User-Agent, token included, it accepts the request. Measured on 2026-10-13
// at the identifier step (Qt WebEngine 6.11, Chromium 140): the stripped
// User-Agent and a Firefox User-Agent were rejected, the engine's own passed.
// Only the account host gets it back; every other site keeps the stripped one.

// The engine's User-Agent before DaliNira changes it. Set once while the
// profile is created, before its request interceptor is installed.
inline QByteArray &engineUserAgentStorage() {
  static QByteArray userAgent;
  return userAgent;
}

inline void setChromiumEngineUserAgent(const QByteArray &userAgent) { engineUserAgentStorage() = userAgent; }

// The User-Agent to send for this request on the Chromium engine, or an empty
// value to keep the profile's.
inline QByteArray chromiumUserAgentOverride(const QUrl &requestUrl) {
  if (requestUrl.scheme() != QLatin1String("https")) return {};
  if (requestUrl.host().compare(QLatin1String("accounts.google.com"), Qt::CaseInsensitive) != 0) return {};
  return engineUserAgentStorage();
}

}  // namespace dalinira::sign_in_compatibility
