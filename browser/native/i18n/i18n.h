#pragma once

#include "language_manager.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

namespace dalinira::i18n {

class I18n {
 public:
  // Every user-visible string is looked up by a stable semantic key. The
  // catalogs are audited for exact key parity (scripts/i18n_audit.py), so a
  // missing key falls back to English, never to hardcoded Turkish.
  static inline QString text(const QString &key) {
    return LanguageManager::instance().translate(key);
  }

  static inline QString text(const QString &key, const QVariantMap &replacements) {
    return LanguageManager::instance().translate(key, replacements);
  }

  static inline QString tr(const QString &key) {
    return LanguageManager::instance().translate(key);
  }

  // True when `value` is the translation of `key` in any supported language.
  // Use this instead of comparing against I18n::text() for values that may
  // have been produced before a live language switch (e.g. tab titles).
  static inline bool isTranslationOf(const QString &key, const QString &value,
                                     Qt::CaseSensitivity cs = Qt::CaseSensitive) {
    const QStringList values = LanguageManager::instance().translationsOf(key);
    for (const QString &candidate : values) {
      if (candidate.compare(value, cs) == 0) return true;
    }
    return false;
  }

  // Re-expresses a previously translated catalog value in the active
  // language; other text (user data, composed strings) is returned unchanged.
  static inline QString retranslated(const QString &value) {
    const QString key = LanguageManager::instance().keyForText(value);
    return key.isEmpty() ? value : text(key);
  }

  // Internal HTML pages carry `{{i18n:key}}` (HTML text/attribute context),
  // `{{i18n-js:key}}` (JavaScript string literal context) and
  // `{{i18n-html:key}}` (trusted catalog markup, keys ending in `_html`)
  // tokens; `{{i18n-lang}}` is the active language code. Both engines render
  // the same localized document.
  static inline QString localizeHtml(QString html) {
    html.replace(QLatin1String("{{i18n-lang}}"), activeLanguageCode().toHtmlEscaped());
    static const QRegularExpression token(
        QStringLiteral(R"(\{\{i18n(-js|-html)?:([A-Za-z0-9_.]+)\}\})"));
    QString out;
    out.reserve(html.size());
    qsizetype last = 0;
    auto it = token.globalMatch(html);
    while (it.hasNext()) {
      const QRegularExpressionMatch match = it.next();
      out.append(QStringView(html).mid(last, match.capturedStart() - last));
      const QString value = text(match.captured(2));
      if (match.captured(1).isEmpty()) {
        out.append(value.toHtmlEscaped());
      } else if (match.captured(1) == QLatin1String("-html")) {
        out.append(value);
      } else {
        QString json = QString::fromUtf8(
            QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact));
        json = json.mid(1, json.size() - 2);
        json.replace(QLatin1String("</"), QLatin1String("<\\/"));
        out.append(json);
      }
      last = match.capturedEnd();
    }
    out.append(QStringView(html).mid(last));
    return out;
  }

  static inline bool isRtl() {
    return LanguageManager::instance().isRtl();
  }

  static inline QString activeLanguageCode() {
    return LanguageManager::instance().activeLanguageCode();
  }
};

}  // namespace dalinira::i18n

namespace dalinira {
using I18n = dalinira::i18n::I18n;
}

using I18n = dalinira::i18n::I18n;
