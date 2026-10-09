#include "google_cloud_provider.h"

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
const QString kGoogleCloudEndpoint = QStringLiteral("https://translation.googleapis.com/language/translate/v2");
}

GoogleCloudProvider::GoogleCloudProvider(QNetworkAccessManager *nam, const QString &apiKey)
    : nam_(nam), apiKey_(apiKey.trimmed()) {}

bool GoogleCloudProvider::isConfigured() const {
  return !apiKey_.isEmpty();
}

void GoogleCloudProvider::translateBatch(
    const QStringList &texts,
    const QString &sourceLang,
    const QString &targetLang,
    ProviderCallback callback
) {
  if (!isConfigured()) {
    if (callback) {
      callback({false, {}, TranslationError::NotConfigured, I18n::text(QStringLiteral("translate.gcp_key_missing"))});
    }
    return;
  }

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

  QUrl url(kGoogleCloudEndpoint);
  QUrlQuery query;
  query.addQueryItem(QStringLiteral("key"), apiKey_);
  url.setQuery(query);

  QJsonObject payload;
  payload.insert(QStringLiteral("q"), QJsonArray::fromStringList(texts));
  if (!sourceLang.isEmpty() && sourceLang.compare(QLatin1String("auto"), Qt::CaseInsensitive) != 0) {
    QString srcNorm = sourceLang.trimmed();
    if (srcNorm.toLower() == QLatin1String("en-us") || srcNorm.toLower() == QLatin1String("en-gb")) {
      srcNorm = QStringLiteral("en");
    } else {
      const int dash = srcNorm.indexOf(QLatin1Char('-'));
      if (dash > 0 && !srcNorm.toLower().startsWith(QLatin1String("zh"))) srcNorm = srcNorm.left(dash);
    }
    payload.insert(QStringLiteral("source"), srcNorm);
  }
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
  }
  payload.insert(QStringLiteral("target"), effectiveTarget);
  payload.insert(QStringLiteral("format"), QStringLiteral("text"));

  QNetworkRequest request(url);
  request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

  QNetworkReply *reply = nam_->post(request, QJsonDocument(payload).toJson(QJsonDocument::Compact));

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
      QString errMsg = reply->errorString();

      if (reply->error() == QNetworkReply::OperationCanceledError || reply->error() == QNetworkReply::TimeoutError) {
        errType = TranslationError::Timeout;
        errMsg = I18n::text(QStringLiteral("translate.gcp_timeout"));
      } else if (httpCode == 400 || httpCode == 403) {
        errType = TranslationError::AuthenticationFailed;
        errMsg = I18n::text(QStringLiteral("translate.gcp_key_invalid"));
      } else if (httpCode == 429) {
        errType = TranslationError::RateLimited;
        errMsg = I18n::text(QStringLiteral("translate.gcp_quota"));
      }

      if (callback) {
        callback({false, {}, errType, errMsg});
      }
      return;
    }

    const QByteArray responseData = reply->readAll();
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(responseData, &parseError);

    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
      if (callback) {
        callback({false, {}, TranslationError::InvalidResponse, I18n::text(QStringLiteral("translate.gcp_invalid_json"))});
      }
      return;
    }

    const QJsonObject rootObj = doc.object();
    const QJsonObject dataObj = rootObj.value(QStringLiteral("data")).toObject();
    const QJsonArray translations = dataObj.value(QStringLiteral("translations")).toArray();

    QStringList translatedList;
    translatedList.reserve(translations.size());

    for (const QJsonValue &val : translations) {
      if (val.isObject()) {
        translatedList.append(val.toObject().value(QStringLiteral("translatedText")).toString());
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
