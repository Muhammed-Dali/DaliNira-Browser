#include "google_gtx_provider.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrlQuery>
#include "i18n/i18n.h"

namespace {
constexpr int kDefaultTimeoutMs = 12000;
const QString kGoogleGtxEndpoint = QStringLiteral("https://translate.googleapis.com/translate_a/single");
const QString kSplitToken = QStringLiteral("___DALINIRA_SPLIT___");
}

GoogleGtxProvider::GoogleGtxProvider(QNetworkAccessManager *nam)
    : nam_(nam) {}

void GoogleGtxProvider::translateBatch(
    const QStringList &texts,
    const QString &sourceLang,
    const QString &targetLang,
    ProviderCallback callback
) {
  if (!nam_) {
    if (callback) {
      callback({false, {}, TranslationError::ProviderUnavailable, I18n::text(QStringLiteral("translate.network_manager_unavailable"))});
    }
    return;
  }

  if (texts.isEmpty()) {
    if (callback) callback({true, {}, TranslationError::None, QString()});
    return;
  }

  QString effectiveSrc = sourceLang.isEmpty() ? QStringLiteral("auto") : sourceLang.trimmed().toLower();
  QString effectiveTarget = targetLang.isEmpty() ? QStringLiteral("tr") : targetLang.trimmed();

  const QString targetLower = effectiveTarget.toLower();
  if (targetLower == QLatin1String("en-us") || targetLower == QLatin1String("en-gb")) {
    effectiveTarget = QStringLiteral("en");
  } else if (targetLower.startsWith(QLatin1String("zh-")) || targetLower == QLatin1String("zh")) {
    effectiveTarget = QStringLiteral("zh-CN");
  } else if (targetLower == QLatin1String("pt-br")) {
    effectiveTarget = QStringLiteral("pt");
  } else {
    const int dash = effectiveTarget.indexOf(QLatin1Char('-'));
    if (dash > 0) effectiveTarget = effectiveTarget.left(dash);
    const int under = effectiveTarget.indexOf(QLatin1Char('_'));
    if (under > 0) effectiveTarget = effectiveTarget.left(under);
  }

  QUrl url(kGoogleGtxEndpoint);
  QUrlQuery query;
  query.addQueryItem(QStringLiteral("client"), QStringLiteral("dict-chrome-ex"));
  query.addQueryItem(QStringLiteral("sl"), effectiveSrc);
  query.addQueryItem(QStringLiteral("tl"), effectiveTarget);
  query.addQueryItem(QStringLiteral("dt"), QStringLiteral("t"));
  url.setQuery(query);

  const QString joinedQuery = texts.join(QLatin1String("\n") + kSplitToken + QLatin1String("\n"));
  QUrlQuery postQuery;
  postQuery.addQueryItem(QStringLiteral("q"), joinedQuery);
  const QByteArray body = postQuery.toString(QUrl::FullyEncoded).toUtf8();

  QNetworkRequest request(url);
  request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded; charset=UTF-8"));
  request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"));
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

  QNetworkReply *reply = nam_->post(request, body);

  auto *timer = new QTimer(reply);
  timer->setSingleShot(true);
  timer->setInterval(kDefaultTimeoutMs);
  QObject::connect(timer, &QTimer::timeout, reply, [reply] {
    if (reply->isRunning()) reply->abort();
  });
  timer->start();

  const int expectedCount = texts.size();

  QObject::connect(reply, &QNetworkReply::finished, [reply, timer, expectedCount, callback]() {
    timer->stop();
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
      TranslationError errType = TranslationError::NetworkError;
      const int httpCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
      if (reply->error() == QNetworkReply::OperationCanceledError || reply->error() == QNetworkReply::TimeoutError) {
        errType = TranslationError::Timeout;
      } else if (httpCode == 429) {
        errType = TranslationError::RateLimited;
      }

      if (callback) {
        callback({false, {}, errType, reply->errorString()});
      }
      return;
    }

    const QByteArray responseData = reply->readAll();
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(responseData, &parseError);

    if (parseError.error != QJsonParseError::NoError || !doc.isArray() || doc.array().isEmpty()) {
      if (callback) {
        callback({false, {}, TranslationError::InvalidResponse, I18n::text(QStringLiteral("translate.gtx_parse_failed"))});
      }
      return;
    }

    QStringList translatedList;
    QString currentItem;
    const QJsonArray outer = doc.array();
    if (outer.at(0).isArray()) {
      const QJsonArray segments = outer.at(0).toArray();
      static const QStringList kKnownSplitTokens = {
          kSplitToken,
          QStringLiteral("__أردالي_سبليت___"),
          QStringLiteral("___أردالي_سبليت___"),
          QStringLiteral("__اردالي_سبليت___"),
          QStringLiteral("___اردالي_سبليت___")
      };

      for (const QJsonValue &segVal : segments) {
        if (!segVal.isArray() || segVal.toArray().isEmpty()) continue;
        const QJsonArray segArray = segVal.toArray();
        const QString trans = segArray.at(0).toString();
        const QString orig = segArray.size() > 1 ? segArray.at(1).toString() : QString();

        bool splitFound = false;
        if (orig.trimmed() == kSplitToken) {
          translatedList.append(currentItem.trimmed());
          currentItem.clear();
          splitFound = true;
        } else {
          for (const QString &token : kKnownSplitTokens) {
            if (trans.contains(token)) {
              const QStringList parts = trans.split(token);
              for (int pIdx = 0; pIdx < parts.size(); ++pIdx) {
                currentItem.append(parts.at(pIdx));
                if (pIdx < parts.size() - 1) {
                  translatedList.append(currentItem.trimmed());
                  currentItem.clear();
                }
              }
              splitFound = true;
              break;
            }
          }
        }

        if (!splitFound) {
          currentItem.append(trans);
        }
      }
      if (!currentItem.isEmpty() || translatedList.size() < expectedCount) {
        translatedList.append(currentItem.trimmed());
      }
    }

    if (translatedList.size() != expectedCount) {
      // Fallback check using raw concatenated text
      QString fullTranslatedText;
      if (outer.at(0).isArray()) {
        const QJsonArray segments = outer.at(0).toArray();
        for (const QJsonValue &segVal : segments) {
          if (segVal.isArray() && !segVal.toArray().isEmpty()) {
            fullTranslatedText.append(segVal.toArray().at(0).toString());
          }
        }
      }
      static const QStringList kFallbackSplitTokens = {
          kSplitToken,
          QStringLiteral("__أردالي_سبليت___"),
          QStringLiteral("___أردالي_سبليت___"),
          QStringLiteral("__اردالي_سبليت___"),
          QStringLiteral("___اردالي_سبليت___")
      };
      for (const QString &token : kFallbackSplitTokens) {
        const QStringList fallbackList = fullTranslatedText.split(token);
        if (fallbackList.size() == expectedCount) {
          translatedList.clear();
          for (const QString &item : fallbackList) {
            translatedList.append(item.trimmed());
          }
          break;
        }
      }
    }

    if (translatedList.size() != expectedCount) {
      if (callback) {
        callback({false, {}, TranslationError::InvalidResponse, I18n::text(QStringLiteral("translate.count_mismatch"))});
      }
      return;
    }

    if (callback) {
      callback({true, translatedList, TranslationError::None, QString()});
    }
  });
}
