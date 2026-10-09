#include "translate_service.h"

#include "providers/none_provider.h"
#include "providers/libretranslate_provider.h"
#include "providers/deepl_provider.h"
#include "providers/google_cloud_provider.h"
#include "providers/google_gtx_provider.h"
#include "i18n/i18n.h"
#include "i18n/language_manager.h"

namespace {
QString normalizeLangCode(QString lang) {
  lang = lang.trimmed().toLower();
  const int hyphen = lang.indexOf(QLatin1Char('-'));
  if (hyphen > 0) lang = lang.left(hyphen);
  const int underscore = lang.indexOf(QLatin1Char('_'));
  if (underscore > 0) lang = lang.left(underscore);
  return lang.trimmed();
}
}  // namespace

TranslateService::TranslateService(QObject *parent, QNetworkAccessManager *network, CredentialVaultManager *vaultManager)
    : QObject(parent), network_(network), ownsNetwork_(network == nullptr),
      providerId_(QStringLiteral("none")), secretStore_(vaultManager, this) {
  if (!network_) {
    network_ = new QNetworkAccessManager(this);
  }

  connect(&secretStore_, &TranslationSecretStore::secretChanged, this, [this](const QString &) {
    instantiateProvider();
  });
  connect(&secretStore_, &TranslationSecretStore::vaultLockStateChanged, this, [this](bool) {
    instantiateProvider();
  });

  instantiateProvider();
}

void TranslateService::setVaultManager(CredentialVaultManager *vaultManager) {
  secretStore_.setVaultManager(vaultManager);
  instantiateProvider();
}

void TranslateService::setEnabled(bool enabled) {
  enabled_ = enabled;
}

void TranslateService::setDefaultTargetLanguage(const QString &lang) {
  if (!lang.trimmed().isEmpty()) defaultTargetLanguage_ = lang.trimmed();
}

void TranslateService::setAutoTranslateLanguages(const QStringList &langs) {
  autoTranslateLanguages_ = langs;
}

void TranslateService::addAutoTranslateLanguage(const QString &lang) {
  const QString clean = lang.trimmed();
  if (!clean.isEmpty() && !autoTranslateLanguages_.contains(clean)) {
    autoTranslateLanguages_.append(clean);
  }
}

void TranslateService::removeAutoTranslateLanguage(const QString &lang) {
  autoTranslateLanguages_.removeAll(lang.trimmed());
}

void TranslateService::setNeverTranslateLanguages(const QStringList &langs) {
  neverTranslateLanguages_ = langs;
}

void TranslateService::addNeverTranslateLanguage(const QString &lang) {
  const QString clean = lang.trimmed();
  if (!clean.isEmpty() && !neverTranslateLanguages_.contains(clean)) {
    neverTranslateLanguages_.append(clean);
  }
}

void TranslateService::removeNeverTranslateLanguage(const QString &lang) {
  neverTranslateLanguages_.removeAll(lang.trimmed());
}

bool TranslateService::shouldAutoTranslate(const QString &sourceLang) const {
  if (!enabled_) return false;
  const QString norm = normalizeLangCode(sourceLang);
  for (const QString &lang : autoTranslateLanguages_) {
    if (normalizeLangCode(lang) == norm) return true;
  }
  return false;
}

bool TranslateService::shouldNeverTranslate(const QString &sourceLang) const {
  if (!enabled_) return true;
  const QString norm = normalizeLangCode(sourceLang);
  for (const QString &lang : neverTranslateLanguages_) {
    if (normalizeLangCode(lang) == norm) return true;
  }
  return false;
}

void TranslateService::setProvider(const QString &id) {
  const QString cleanId = id.trimmed().toLower();
  if (providerId_ != cleanId) {
    providerId_ = cleanId;
    instantiateProvider();
    emit providerChanged(providerId_);
  }
}

ITranslationProvider *TranslateService::currentProvider() const {
  return activeProvider_.get();
}

void TranslateService::setLibreTranslateEndpoint(const QUrl &url) {
  libreTranslateEndpoint_ = url;
  if (providerId_ == QLatin1String("libretranslate")) {
    instantiateProvider();
  }
}

void TranslateService::setDeepLIsPro(bool isPro) {
  deepLIsPro_ = isPro;
  if (providerId_ == QLatin1String("deepl")) {
    instantiateProvider();
  }
}

bool TranslateService::saveApiKey(const QString &providerId, const QString &apiKey) {
  const bool ok = secretStore_.saveSecret(providerId, apiKey);
  instantiateProvider();
  return ok;
}

QString TranslateService::loadApiKey(const QString &providerId) const {
  return secretStore_.loadSecret(providerId);
}

bool TranslateService::removeApiKey(const QString &providerId) {
  const bool ok = secretStore_.removeSecret(providerId);
  instantiateProvider();
  return ok;
}

void TranslateService::instantiateProvider() {
  if (providerId_ == QLatin1String("libretranslate")) {
    const QString key = secretStore_.loadSecret(QStringLiteral("libretranslate"));
    activeProvider_ = std::make_unique<LibreTranslateProvider>(network_, libreTranslateEndpoint_, key);
  } else if (providerId_ == QLatin1String("deepl")) {
    const QString key = secretStore_.loadSecret(QStringLiteral("deepl"));
    activeProvider_ = std::make_unique<DeepLProvider>(network_, key, deepLIsPro_);
  } else if (providerId_ == QLatin1String("google_cloud")) {
    const QString key = secretStore_.loadSecret(QStringLiteral("google_cloud"));
    activeProvider_ = std::make_unique<GoogleCloudProvider>(network_, key);
  } else if (providerId_ == QLatin1String("google_gtx")) {
    activeProvider_ = std::make_unique<GoogleGtxProvider>(network_);
  } else {
    activeProvider_ = std::make_unique<NoneProvider>();
  }
}

void TranslateService::clearCache() {
  cache_.clear();
}

void TranslateService::loadPreferences(QSettings &prefs) {
  enabled_ = prefs.value(QStringLiteral("translation/enabled"), true).toBool();
  // Until the user picks one, pages are translated into the interface language.
  QString interfaceLanguage = dalinira::i18n::LanguageManager::instance().activeLanguageCode();
  if (interfaceLanguage.isEmpty()) interfaceLanguage = QStringLiteral("en");
  defaultTargetLanguage_ = prefs.value(QStringLiteral("translation/targetLanguage"), interfaceLanguage).toString();
  providerId_ = prefs.value(QStringLiteral("translation/provider"), QStringLiteral("google_gtx")).toString();
  if (providerId_ == QLatin1String("none")) {
    providerId_ = QStringLiteral("google_gtx");
  }

  autoTranslateLanguages_ = prefs.value(QStringLiteral("translation/autoTranslateLanguages")).toStringList();
  if (prefs.contains(QStringLiteral("translation/neverTranslateLanguages"))) {
    neverTranslateLanguages_ = prefs.value(QStringLiteral("translation/neverTranslateLanguages")).toStringList();
  } else {
    neverTranslateLanguages_ = {QStringLiteral("tr")};
  }

  libreTranslateEndpoint_ = QUrl(prefs.value(QStringLiteral("translation/libretranslateEndpoint")).toString());
  deepLIsPro_ = prefs.value(QStringLiteral("translation/deeplPlan"), QStringLiteral("free")).toString() == QLatin1String("pro");

  // Migrate legacy XOR/Base64 secrets to Credential Vault (if any exist)
  secretStore_.migrateLegacySecrets(prefs);

  instantiateProvider();
}

void TranslateService::savePreferences(QSettings &prefs) {
  // Only persist non-sensitive settings in QSettings
  prefs.setValue(QStringLiteral("translation/enabled"), enabled_);
  prefs.setValue(QStringLiteral("translation/targetLanguage"), defaultTargetLanguage_);
  prefs.setValue(QStringLiteral("translation/provider"), providerId_);
  prefs.setValue(QStringLiteral("translation/autoTranslateLanguages"), autoTranslateLanguages_);
  prefs.setValue(QStringLiteral("translation/neverTranslateLanguages"), neverTranslateLanguages_);

  if (libreTranslateEndpoint_.isValid()) {
    prefs.setValue(QStringLiteral("translation/libretranslateEndpoint"), libreTranslateEndpoint_.toString());
  } else {
    prefs.remove(QStringLiteral("translation/libretranslateEndpoint"));
  }

  prefs.setValue(QStringLiteral("translation/deeplPlan"), deepLIsPro_ ? QStringLiteral("pro") : QStringLiteral("free"));

  // Ensure no legacy secret keys remain in QSettings
  prefs.remove(QStringLiteral("translation/sec_lt"));
  prefs.remove(QStringLiteral("translation/sec_deepl"));
  prefs.remove(QStringLiteral("translation/sec_gcp"));
}

void TranslateService::testConnection(const QString &testProviderId, std::function<void(bool success, const QString &message)> callback) {
  std::unique_ptr<ITranslationProvider> testProvider;

  if (testProviderId == QLatin1String("libretranslate")) {
    const QString key = secretStore_.loadSecret(QStringLiteral("libretranslate"));
    testProvider = std::make_unique<LibreTranslateProvider>(network_, libreTranslateEndpoint_, key);
  } else if (testProviderId == QLatin1String("deepl")) {
    const QString key = secretStore_.loadSecret(QStringLiteral("deepl"));
    testProvider = std::make_unique<DeepLProvider>(network_, key, deepLIsPro_);
  } else if (testProviderId == QLatin1String("google_cloud")) {
    const QString key = secretStore_.loadSecret(QStringLiteral("google_cloud"));
    testProvider = std::make_unique<GoogleCloudProvider>(network_, key);
  } else if (testProviderId == QLatin1String("google_gtx")) {
    testProvider = std::make_unique<GoogleGtxProvider>(network_);
  } else {
    testProvider = std::make_unique<NoneProvider>();
  }

  if (testProvider->requiresApiKey() && !testProvider->isConfigured()) {
    if (secretStore_.isVaultLocked()) {
      if (callback) callback(false, I18n::text(QStringLiteral("translate.unlock_vault_warning")));
      return;
    }
    if (callback) callback(false, I18n::text(QStringLiteral("translate.api_key_missing_warning")));
    return;
  }

  testProvider->translateBatch({QStringLiteral("Hello")}, QStringLiteral("en"), defaultTargetLanguage_, [callback](const TranslationResult &res) {
    if (!callback) return;
    if (res.success && !res.translatedTexts.isEmpty()) {
      callback(true, I18n::text(QStringLiteral("translate.connection_ok")).arg(res.translatedTexts.first()));
    } else {
      QString msg = res.errorMessage;
      switch (res.errorType) {
        case TranslationError::NotConfigured:
          msg = I18n::text(QStringLiteral("translate.provider_incomplete"));
          break;
        case TranslationError::AuthenticationFailed:
          msg = I18n::text(QStringLiteral("translate.api_key_invalid"));
          break;
        case TranslationError::RateLimited:
          msg = I18n::text(QStringLiteral("translate.quota_exceeded"));
          break;
        case TranslationError::Timeout:
          msg = I18n::text(QStringLiteral("translate.server_timeout"));
          break;
        case TranslationError::NetworkError:
          msg = I18n::text(QStringLiteral("translate.server_unreachable"));
          break;
        default:
          if (msg.isEmpty()) msg = I18n::text(QStringLiteral("translate.test_failed"));
          break;
      }
      callback(false, QStringLiteral("⚠ %1").arg(msg));
    }
  });
}

QString TranslateService::mapLanguageForProvider(const QString &providerId, const QString &lang, bool isTarget) {
  const QString clean = lang.trimmed();
  if (clean.isEmpty()) {
    return isTarget ? QStringLiteral("tr") : QStringLiteral("auto");
  }
  if (clean.compare(QLatin1String("auto"), Qt::CaseInsensitive) == 0) {
    return (providerId == QLatin1String("google_cloud") || providerId == QLatin1String("deepl")) ? QString() : QStringLiteral("auto");
  }

  const QString lower = clean.toLower();
  const QString pid = providerId.trimmed().toLower();

  if (pid == QLatin1String("deepl")) {
    if (isTarget) {
      if (lower == QLatin1String("en") || lower == QLatin1String("en-us")) return QStringLiteral("EN-US");
      if (lower == QLatin1String("en-gb")) return QStringLiteral("EN-GB");
      if (lower == QLatin1String("pt") || lower == QLatin1String("pt-pt")) return QStringLiteral("PT-PT");
      if (lower == QLatin1String("pt-br")) return QStringLiteral("PT-BR");
      if (lower.startsWith(QLatin1String("zh"))) return QStringLiteral("ZH");
      if (lower == QLatin1String("ar")) return QStringLiteral("AR");
      if (lower == QLatin1String("tr")) return QStringLiteral("TR");
      const int dash = lower.indexOf(QLatin1Char('-'));
      const QString base = (dash > 0) ? lower.left(dash) : lower;
      return base.toUpper();
    } else {
      if (lower.startsWith(QLatin1String("en"))) return QStringLiteral("EN");
      if (lower.startsWith(QLatin1String("pt"))) return QStringLiteral("PT");
      if (lower.startsWith(QLatin1String("zh"))) return QStringLiteral("ZH");
      const int dash = lower.indexOf(QLatin1Char('-'));
      const QString base = (dash > 0) ? lower.left(dash) : lower;
      return base.toUpper();
    }
  }

  if (pid == QLatin1String("google_cloud")) {
    if (!isTarget && (lower == QLatin1String("auto") || lower.isEmpty())) return QString();
    if (lower == QLatin1String("en-us") || lower == QLatin1String("en-gb")) return QStringLiteral("en");
    if (lower.startsWith(QLatin1String("zh-")) || lower == QLatin1String("zh")) return QStringLiteral("zh-CN");
    if (lower == QLatin1String("pt-br")) return QStringLiteral("pt");
    const int dash = lower.indexOf(QLatin1Char('-'));
    return (dash > 0) ? lower.left(dash) : lower;
  }

  if (pid == QLatin1String("libretranslate")) {
    if (lower == QLatin1String("en-us") || lower == QLatin1String("en-gb")) return QStringLiteral("en");
    if (lower.startsWith(QLatin1String("zh"))) return QStringLiteral("zh");
    if (lower.startsWith(QLatin1String("pt"))) return QStringLiteral("pt");
    const int dash = lower.indexOf(QLatin1Char('-'));
    return (dash > 0) ? lower.left(dash) : lower;
  }

  // Default / google_gtx
  if (isTarget) {
    if (lower == QLatin1String("en-us") || lower == QLatin1String("en-gb")) return QStringLiteral("en");
    if (lower.startsWith(QLatin1String("zh-")) || lower == QLatin1String("zh")) return QStringLiteral("zh-CN");
    if (lower == QLatin1String("pt-br")) return QStringLiteral("pt");
    const int dash = lower.indexOf(QLatin1Char('-'));
    return (dash > 0) ? lower.left(dash) : lower;
  } else {
    if (lower == QLatin1String("auto")) return QStringLiteral("auto");
    if (lower == QLatin1String("en-us") || lower == QLatin1String("en-gb")) return QStringLiteral("en");
    if (lower.startsWith(QLatin1String("zh-")) || lower == QLatin1String("zh")) return QStringLiteral("zh-CN");
    if (lower == QLatin1String("pt-br")) return QStringLiteral("pt");
    const int dash = lower.indexOf(QLatin1Char('-'));
    return (dash > 0) ? lower.left(dash) : lower;
  }
}

void TranslateService::translateBatch(const QStringList &texts, const QString &sourceLang, const QString &targetLang, TranslationCallback callback) {
  if (!enabled_) {
    if (callback) callback(false, {}, I18n::text(QStringLiteral("translate.disabled_in_settings")));
    return;
  }

  if (texts.isEmpty()) {
    if (callback) callback(true, {}, QString());
    return;
  }

  // Check in-memory cache
  QStringList uncachedTexts;
  QList<int> uncachedIndices;
  QStringList results;
  results.reserve(texts.size());

  const QString effectiveSrc = sourceLang.isEmpty() ? QStringLiteral("auto") : sourceLang;
  const QString effectiveTarget = targetLang.isEmpty() ? defaultTargetLanguage_ : targetLang;

  for (int i = 0; i < texts.size(); ++i) {
    const QString &text = texts.at(i);
    const QString cacheKey = effectiveSrc + QLatin1Char(':') + effectiveTarget + QLatin1Char(':') + text;
    if (cache_.contains(cacheKey)) {
      results.append(cache_.value(cacheKey));
    } else {
      results.append(QString()); // placeholder
      uncachedTexts.append(text);
      uncachedIndices.append(i);
    }
  }

  if (uncachedTexts.isEmpty()) {
    if (callback) callback(true, results, QString());
    return;
  }

  if (!activeProvider_ || providerId_ == QLatin1String("none")) {
    instantiateProvider();
  }

  ITranslationProvider *providerToUse = activeProvider_.get();
  std::shared_ptr<GoogleGtxProvider> fallbackProvider;
  if (providerToUse && providerToUse->requiresApiKey() &&
      !providerToUse->isConfigured() && secretStore_.isVaultLocked()) {
    const QString error = I18n::text(QStringLiteral("translate.unlock_vault"));
    emit translationFailed(error);
    if (callback) callback(false, {}, error);
    return;
  }
  if (!providerToUse || (providerToUse->requiresApiKey() && !providerToUse->isConfigured()) ||
      providerId_ == QLatin1String("none")) {
    fallbackProvider = std::make_shared<GoogleGtxProvider>(network_);
    providerToUse = fallbackProvider.get();
  }

  const QString effectiveProviderId = (providerToUse == fallbackProvider.get()) ? QStringLiteral("google_gtx") : providerId_;
  const QString mappedSrc = mapLanguageForProvider(effectiveProviderId, effectiveSrc, false);
  const QString mappedTarget = mapLanguageForProvider(effectiveProviderId, effectiveTarget, true);

  providerToUse->translateBatch(uncachedTexts, mappedSrc, mappedTarget, [this, results, uncachedIndices, uncachedTexts, effectiveSrc, effectiveTarget, callback, fallbackProvider](const TranslationResult &res) mutable {
    if (!res.success || res.translatedTexts.size() != uncachedTexts.size()) {
      QString err = res.errorMessage;
      if (err.isEmpty()) {
        err = I18n::text(QStringLiteral("translate.service_error"));
      }
      emit translationFailed(err);
      if (callback) callback(false, {}, err);
      return;
    }

    for (int i = 0; i < uncachedIndices.size() && i < res.translatedTexts.size(); ++i) {
      const int targetIdx = uncachedIndices.at(i);
      const QString &translatedText = res.translatedTexts.at(i);
      results[targetIdx] = translatedText;
      const QString cacheKey = effectiveSrc + QLatin1Char(':') + effectiveTarget + QLatin1Char(':') + uncachedTexts.at(i);
      cache_.insert(cacheKey, translatedText);
    }

    if (callback) callback(true, results, QString());
  });
}
