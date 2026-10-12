#include <iostream>
#include <QUrl>

#include "core/sign_in_compatibility.h"

using namespace dalinira::sign_in_compatibility;

namespace {
int failures = 0;

void check(bool condition, const char *name) {
  std::cout << (condition ? "  PASS " : "  FAIL ") << name << "\n";
  if (!condition) ++failures;
}
}  // namespace

int main() {
  const QUrl signIn(QStringLiteral("https://accounts.google.com/v3/signin/identifier"));
  check(chromiumUserAgentOverride(signIn).isEmpty(), "no override before the profile records its User-Agent");

  const QByteArray engine =
      "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) QtWebEngine/6.11.2 "
      "Chrome/140.0.0.0 Safari/537.36";
  setChromiumEngineUserAgent(engine);
  check(chromiumUserAgentOverride(signIn) == engine, "account pages get the engine's own User-Agent");
  check(chromiumUserAgentOverride(QUrl(QStringLiteral("https://ACCOUNTS.google.com/"))) == engine,
        "the host comparison ignores case");

  check(chromiumUserAgentOverride(QUrl(QStringLiteral("https://www.google.com/search?q=x"))).isEmpty(),
        "other Google hosts keep the profile User-Agent");
  check(chromiumUserAgentOverride(QUrl(QStringLiteral("https://mail.google.com/"))).isEmpty(),
        "Gmail keeps the profile User-Agent");
  check(chromiumUserAgentOverride(QUrl(QStringLiteral("https://accounts.google.com.example.org/"))).isEmpty(),
        "a look-alike host keeps the profile User-Agent");
  check(chromiumUserAgentOverride(QUrl(QStringLiteral("http://accounts.google.com/"))).isEmpty(),
        "plain HTTP keeps the profile User-Agent");
  check(chromiumUserAgentOverride(QUrl()).isEmpty(), "an invalid URL keeps the profile User-Agent");

  if (failures) {
    std::cout << failures << " check(s) failed\n";
    return 1;
  }
  std::cout << "sign-in compatibility: all checks passed\n";
  return 0;
}
