#pragma once

#include "translation_provider.h"
#include "i18n/i18n.h"

class NoneProvider final : public ITranslationProvider {
 public:
  NoneProvider() = default;
  ~NoneProvider() override = default;

  QString id() const override { return QStringLiteral("none"); }
  QString displayName() const override { return I18n::text(QStringLiteral("translate.provider_not_configured")); }
  bool requiresApiKey() const override { return false; }
  bool isConfigured() const override { return false; }

  void translateBatch(
      const QStringList &texts,
      const QString &sourceLang,
      const QString &targetLang,
      ProviderCallback callback
  ) override {
    Q_UNUSED(texts);
    Q_UNUSED(sourceLang);
    Q_UNUSED(targetLang);
    if (callback) {
      callback({false, {}, TranslationError::NotConfigured, I18n::text(QStringLiteral("translate.provider_not_configured_error"))});
    }
  }
};
