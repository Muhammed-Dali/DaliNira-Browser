#pragma once

#include <QString>

class LanguageDetector final {
 public:
  static QString detectLanguage(const QString &htmlLang, const QString &contentLanguage, const QString &textSample);
  static QString detectTextLanguage(const QString &textSample);
  static bool hasReliableTextSample(const QString &textSample);
  static QString normalizeLanguageCode(QString lang);
  static QString languageDisplayName(const QString &langCode);
  static bool isTranslatable(const QString &detectedLang, const QString &targetLang = QStringLiteral("tr"));
};
