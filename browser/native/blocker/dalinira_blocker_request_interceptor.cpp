#include "dalinira_blocker_request_interceptor.h"

#include <QBuffer>
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>
#include <QWebEngineNewWindowRequest>
#include <QWebEnginePage>
#include <QWebEngineUrlRequestJob>
#include <QWebEngineUrlScheme>
#include <atomic>
#include "dalinira_blocker_redirect_resources.h"
#include "dalinira_blocker_service.h"
#include "new_tab_html.h"

namespace {
std::atomic<bool> g_resourceSchemeRegistered{false};
}  // namespace

QByteArray DaliNiraBlockerRequestInterceptor::resourceScheme() { return QByteArrayLiteral("dalinira-blocker-resource"); }

bool DaliNiraBlockerRequestInterceptor::resourceSchemeRegistered() { return g_resourceSchemeRegistered.load(); }

void DaliNiraBlockerRequestInterceptor::registerResourceScheme() {
  if (g_resourceSchemeRegistered.exchange(true)) return;
  QWebEngineUrlScheme scheme(resourceScheme());
  scheme.setSyntax(QWebEngineUrlScheme::Syntax::Path);
  // Loadable by web content (it replaces their subresources), readable by
  // fetch/XHR; not local, so it grants no access to anything else.
  QWebEngineUrlScheme::Flags flags = QWebEngineUrlScheme::SecureScheme | QWebEngineUrlScheme::CorsEnabled;
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
  flags |= QWebEngineUrlScheme::FetchApiAllowed;
#endif
  scheme.setFlags(flags);
  QWebEngineUrlScheme::registerScheme(scheme);
}

DaliNiraBlockerResourceSchemeHandler::DaliNiraBlockerResourceSchemeHandler(
    const DaliNiraBlockerRedirectResources *resources, const QString &token, QObject *parent)
    : QWebEngineUrlSchemeHandler(parent), resources_(resources), token_(token) {}

void DaliNiraBlockerResourceSchemeHandler::requestStarted(QWebEngineUrlRequestJob *job) {
  // "<token>/<name>": exactly one separator, a name the registry packages.
  const QString path = job->requestUrl().path();
  const int slash = path.indexOf(QLatin1Char('/'));
  const QString token = slash > 0 ? path.left(slash) : QString();
  const QString name = slash > 0 ? path.mid(slash + 1) : QString();
  QString mimeType;
  bool ok = false;
  QByteArray bytes;
  if (resources_ && !token_.isEmpty() && token == token_ && !name.contains(QLatin1Char('/')) &&
      resources_->names().contains(name))
    bytes = resources_->content(name, &mimeType, &ok);
  if (!ok) {
    job->fail(QWebEngineUrlRequestJob::UrlNotFound);
    return;
  }
  auto *buffer = new QBuffer(job);
  buffer->setData(bytes);
  buffer->open(QIODevice::ReadOnly);
  job->reply(mimeType.toUtf8(), buffer);
}

DaliNiraBlockerRequestInterceptor::DaliNiraBlockerRequestInterceptor(DaliNiraBlockerService *service, QObject *parent)
    : QWebEngineUrlRequestInterceptor(parent), service_(service) {}

void DaliNiraBlockerRequestInterceptor::interceptRequest(QWebEngineUrlRequestInfo &info) {
  QUrl requestUrl = info.requestUrl();
  if (!requestUrl.isValid()) return;

  const QString scheme = requestUrl.scheme().toLower();
  if (scheme == QLatin1String("dalinira")) {
    if (requestUrl.host() == QLatin1String("bypass-strictblock")) {
      if (!isAuthorizedStrictBlockBypass(requestUrl, info.initiator())) {
        info.block(true);
        return;
      }
      const QString domain = QUrlQuery(requestUrl).queryItemValue(QStringLiteral("domain"), QUrl::FullyDecoded);
      if (service_) {
        service_->allowTemporaryStrictBypass(domain, 15);
      }
    }
    return;
  }

  if (scheme != QLatin1String("http") && scheme != QLatin1String("https")) return;
  // Global Privacy Control: the user does not consent to sale or sharing of
  // their data (both engines send it on every web request).
  info.setHttpHeader(QByteArrayLiteral("Sec-GPC"), QByteArrayLiteral("1"));
  if (!service_) return;

  QUrl firstPartyUrl = info.firstPartyUrl();
  if (!firstPartyUrl.isValid() || firstPartyUrl.host().isEmpty()) {
    firstPartyUrl = info.initiator();
  }
  // Same decision (tracking-parameter stripping, rules, site policy, counters)
  // the WebKit engine applies through its web process extension.
  const NetworkRequestVerdict verdict = service_->decideNetworkRequest(
      requestUrl, resourceTypeFromWebEngine(static_cast<int>(info.resourceType())), firstPartyUrl, 0,
      QString::fromLatin1(info.requestMethod()).toLower());
  switch (verdict.kind) {
    case NetworkVerdictKind::Block:
      if (qEnvironmentVariableIntValue("DALINIRA_FEATURE_DIAGNOSTICS") == 1) {
        qInfo().noquote() << "[BLOCKER] request blocked";
      }
      info.block(true);
      break;
    case NetworkVerdictKind::Redirect:
      // A packaged substitute goes to the resource scheme (see the header);
      // everything else (stripped or transformed URLs) is a web URL.
      if (!verdict.decision.redirectResource.isEmpty() && resourceSchemeRegistered() &&
          !service_->resourceSchemeToken().isEmpty()) {
        info.redirect(QUrl(QString::fromLatin1(resourceScheme()) + QLatin1Char(':') + service_->resourceSchemeToken() +
                           QLatin1Char('/') + verdict.decision.redirectResource));
      } else {
        info.redirect(verdict.redirectUrl);
      }
      break;
    case NetworkVerdictKind::Allow:
      break;
  }
}

DaliNiraBlockerPopupGuard::DaliNiraBlockerPopupGuard(QWebEnginePage *page, ServiceProvider provider)
    : QObject(page), provider_(std::move(provider)) {
  connect(page, &QWebEnginePage::newWindowRequested, this, [this, page](QWebEngineNewWindowRequest &request) {
    blockNext_ = false;
    DaliNiraBlockerService *service = provider_ ? provider_() : nullptr;
    if (!service) return;
    // The blocker counts per view, like the request interceptor's tabs.
    const quint64 tabId = reinterpret_cast<quintptr>(page->parent());
    blockNext_ = service->decidePopup(request.requestedUrl(), page->url(), tabId).action ==
                 DaliNiraBlockerAction::Block;
  });
}

bool DaliNiraBlockerPopupGuard::consumeBlockedWindow() {
  const bool blocked = blockNext_;
  blockNext_ = false;
  return blocked;
}
