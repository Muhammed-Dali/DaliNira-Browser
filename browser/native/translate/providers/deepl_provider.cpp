#include "deepl_provider.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include "i18n/i18n.h"

namespace {
constexpr int kDefaultTimeoutMs = 12000;
const QString kDeepLFreeEndpoint = QStringLiteral("https://api-free.deepl.com/v2/translate");
const QString kDeepLProEndpoint = QStringLiteral("https://api.deepl.com/v2/translate");
}

DeepLProvider::DeepLProvider(QNetworkAccessManager *nam, const QString &apiKey, bool isPro)
    : nam_(nam), apiKey_(apiKey.trimmed()), isPro_(isPro) {}

bool DeepLProvider::isConfigured() const {
  return !apiKey_.isEmpty();
}

QUrl DeepLProvider::endpointUrl() const {
  // Auto-detect pro if key ends with :fx (free key) or based on isPro_ flag
  if (apiKey_.endsWith(QLatin1String(":fx"), Qt::CaseInsensitive)) {
    return QUrl(kDeepLFreeEndpoint);
  }
  return isPro_ ? QUrl(kDeepLProEndpoint) : QUrl(kDeepLFreeEndpoint);
}

void DeepLProvider::translateBatch(
    const QStringList &texts,
    const QString &sourceLang,
    const QString &targetLang,
    ProviderCallback callback
) {
  if (!isConfigured()) {
    if (callback) {
      callback({false, {}, TranslationError::NotConfigured, I18n::text(QStringLiteral("translate.deepl_key_missing"))});
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

  QJsonObject payload;
  payload.insert(QStringLiteral("text"), QJsonArray::fromStringList(texts));
  if (!sourceLang.isEmpty() && sourceLang.compare(QLatin1String("auto"), Qt::CaseInsensitive) != 0) {
    QString srcUpper = sourceLang.trimmed().toUpper();
    const int dash = srcUpper.indexOf(QLatin1Char('-'));
    if (dash > 0) srcUpper = srcUpper.left(dash);
    payload.insert(QStringLiteral("source_lang"), srcUpper);
  }
  QString targetMapped = (targetLang.isEmpty() ? QStringLiteral("TR") : targetLang.trimmed().toUpper());
  if (targetMapped == QLatin1String("EN")) targetMapped = QStringLiteral("EN-US");
  else if (targetMapped == QLatin1String("PT")) targetMapped = QStringLiteral("PT-PT");
  else if (targetMapped.startsWith(QLatin1String("ZH"))) targetMapped = QStringLiteral("ZH");
  payload.insert(QStringLiteral("target_lang"), targetMapped);

  QNetworkRequest request(endpointUrl());
  request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
  request.setRawHeader("Authorization", QStringLiteral("DeepL-Auth-Key %1").arg(apiKey_).toUtf8());
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
        errMsg = I18n::text(QStringLiteral("translate.deepl_timeout"));
      } else if (httpCode == 403 || httpCode == 401) {
        errType = TranslationError::AuthenticationFailed;
        errMsg = I18n::text(QStringLiteral("translate.deepl_key_invalid"));
      } else if (httpCode == 456) {
        errType = TranslationError::RateLimited;
        errMsg = I18n::text(QStringLiteral("translate.deepl_quota"));
      } else if (httpCode == 429) {
        errType = TranslationError::RateLimited;
        errMsg = I18n::text(QStringLiteral("translate.deepl_rate_limit"));
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
        callback({false, {}, TranslationError::InvalidResponse, I18n::text(QStringLiteral("translate.deepl_invalid_json"))});
      }
      return;
    }

    const QJsonObject rootObj = doc.object();
    if (!rootObj.contains(QStringLiteral("translations"))) {
      if (callback) {
        callback({false, {}, TranslationError::InvalidResponse, I18n::text(QStringLiteral("translate.deepl_no_translation"))});
      }
      return;
    }

    const QJsonArray translations = rootObj.value(QStringLiteral("translations")).toArray();
    QStringList translatedList;
    translatedList.reserve(translations.size());

    for (const QJsonValue &val : translations) {
      if (val.isObject()) {
        translatedList.append(val.toObject().value(QStringLiteral("text")).toString());
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
