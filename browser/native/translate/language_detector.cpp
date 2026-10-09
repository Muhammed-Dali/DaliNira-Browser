#include "language_detector.h"

#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include "i18n/language_manager.h"

namespace {

bool isValidLanguageCode(const QString &code) {
  static const QRegularExpression regex(QStringLiteral("^[a-z]{2,3}$"));
  return regex.match(code).hasMatch() && code != QLatin1String("und") && code != QLatin1String("zxx")
         && code != QLatin1String("mul") && code != QLatin1String("mis");
}

int countMatches(const QStringList &words, const QSet<QString> &dictionary) {
  int count = 0;
  for (const QString &word : words) {
    if (dictionary.contains(word)) ++count;
  }
  return count;
}

QString detectFromSample(const QString &sample) {
  const QString clean = sample.trimmed();
  if (clean.isEmpty()) return {};

  // Script-based detection
  int cyrillicCount = 0;
  int arabicCount = 0;
  int cjkCount = 0;
  int hiraganaKatakanaCount = 0;
  int hangulCount = 0;
  int greekCount = 0;

  for (const QChar &ch : clean) {
    const ushort u = ch.unicode();
    if (u >= 0x0400 && u <= 0x04FF) ++cyrillicCount;
    else if (u >= 0x0600 && u <= 0x06FF) ++arabicCount;
    else if ((u >= 0x3040 && u <= 0x309F) || (u >= 0x30A0 && u <= 0x30FF)) ++hiraganaKatakanaCount;
    else if (u >= 0x4E00 && u <= 0x9FFF) ++cjkCount;
    else if (u >= 0xAC00 && u <= 0xD7AF) ++hangulCount;
    else if (u >= 0x0370 && u <= 0x03FF) ++greekCount;
  }

  const int threshold = std::max(4, static_cast<int>(clean.size() / 15));
  if (hiraganaKatakanaCount >= 3) return QStringLiteral("ja");
  if (hangulCount >= 3) return QStringLiteral("ko");
  if (cjkCount >= threshold) return QStringLiteral("zh");
  if (arabicCount >= threshold) return QStringLiteral("ar");
  if (cyrillicCount >= threshold) return QStringLiteral("ru");
  if (greekCount >= threshold) return QStringLiteral("el");

  // Latin stopword dictionaries
  static const QSet<QString> enWords{
      QStringLiteral("the"), QStringLiteral("and"), QStringLiteral("is"), QStringLiteral("of"),
      QStringLiteral("to"), QStringLiteral("in"), QStringLiteral("that"), QStringLiteral("it"),
      QStringLiteral("with"), QStringLiteral("as"), QStringLiteral("for"), QStringLiteral("was"),
      QStringLiteral("on"), QStringLiteral("are"), QStringLiteral("by"), QStringLiteral("this"),
      QStringLiteral("from"), QStringLiteral("at"), QStringLiteral("be"), QStringLiteral("have"),
      QStringLiteral("an"), QStringLiteral("which"), QStringLiteral("you"), QStringLiteral("we")};

  static const QSet<QString> trWords{
      QStringLiteral("ve"), QStringLiteral("bir"), QStringLiteral("bu"), QStringLiteral("da"),
      QStringLiteral("de"), QStringLiteral("için"), QStringLiteral("ile"), QStringLiteral("ne"),
      QStringLiteral("var"), QStringLiteral("çok"), QStringLiteral("olarak"), QStringLiteral("gibi"),
      QStringLiteral("kadar"), QStringLiteral("olan"), QStringLiteral("daha"), QStringLiteral("sonra"),
      QStringLiteral("en"), QStringLiteral("tarafından"), QStringLiteral("ise"), QStringLiteral("ancak")};

  static const QSet<QString> deWords{
      QStringLiteral("der"), QStringLiteral("die"), QStringLiteral("das"), QStringLiteral("und"),
      QStringLiteral("in"), QStringLiteral("den"), QStringLiteral("von"), QStringLiteral("zu"),
      QStringLiteral("mit"), QStringLiteral("ist"), QStringLiteral("des"), QStringLiteral("nicht"),
      QStringLiteral("eine"), QStringLiteral("einer"), QStringLiteral("sich"), QStringLiteral("auch"),
      QStringLiteral("auf"), QStringLiteral("für"), QStringLiteral("an"), QStringLiteral("dem")};

  static const QSet<QString> frWords{
      QStringLiteral("le"), QStringLiteral("la"), QStringLiteral("les"), QStringLiteral("de"),
      QStringLiteral("et"), QStringLiteral("un"), QStringLiteral("une"), QStringLiteral("dans"),
      QStringLiteral("pour"), QStringLiteral("qui"), QStringLiteral("sur"), QStringLiteral("que"),
      QStringLiteral("est"), QStringLiteral("des"), QStringLiteral("avec"), QStringLiteral("ce"),
      QStringLiteral("en"), QStringLiteral("du"), QStringLiteral("par"), QStringLiteral("au")};

  static const QSet<QString> esWords{
      QStringLiteral("el"), QStringLiteral("la"), QStringLiteral("los"), QStringLiteral("las"),
      QStringLiteral("de"), QStringLiteral("en"), QStringLiteral("y"), QStringLiteral("un"),
      QStringLiteral("una"), QStringLiteral("por"), QStringLiteral("que"), QStringLiteral("con"),
      QStringLiteral("para"), QStringLiteral("es"), QStringLiteral("al"), QStringLiteral("del"),
      QStringLiteral("se"), QStringLiteral("lo"), QStringLiteral("como"), QStringLiteral("más")};

  static const QSet<QString> itWords{
      QStringLiteral("il"), QStringLiteral("lo"), QStringLiteral("la"), QStringLiteral("i"),
      QStringLiteral("gli"), QStringLiteral("le"), QStringLiteral("di"), QStringLiteral("e"),
      QStringLiteral("che"), QStringLiteral("in"), QStringLiteral("un"), QStringLiteral("una"),
      QStringLiteral("per"), QStringLiteral("non"), QStringLiteral("con"), QStringLiteral("sono"),
      QStringLiteral("del"), QStringLiteral("della"), QStringLiteral("ed"), QStringLiteral("si")};

  static const QSet<QString> ptWords{
      QStringLiteral("o"), QStringLiteral("a"), QStringLiteral("os"), QStringLiteral("as"),
      QStringLiteral("de"), QStringLiteral("em"), QStringLiteral("e"), QStringLiteral("um"),
      QStringLiteral("uma"), QStringLiteral("para"), QStringLiteral("com"), QStringLiteral("não"),
      QStringLiteral("que"), QStringLiteral("do"), QStringLiteral("da"), QStringLiteral("por")};

  static const QSet<QString> nlWords{
      QStringLiteral("de"), QStringLiteral("het"), QStringLiteral("een"), QStringLiteral("en"),
      QStringLiteral("van"), QStringLiteral("in"), QStringLiteral("op"), QStringLiteral("te"),
      QStringLiteral("is"), QStringLiteral("met"), QStringLiteral("voor"), QStringLiteral("zijn")};

  // Turkish specific characters test
  static const QRegularExpression trSpecial(QStringLiteral("[çğıöşüÇĞİÖŞÜ]"));
  if (trSpecial.match(clean).hasMatch()) {
    const QString lower = clean.toLower();
    const QStringList tokens = lower.split(QRegularExpression(QStringLiteral("[\\s\\p{P}\\d]+")), Qt::SkipEmptyParts);
    // ç/ö/ü also occur in French/German. Only the genuinely distinctive
    // Turkish letters are decisive; otherwise require multiple stopwords.
    static const QRegularExpression trDistinctive(QStringLiteral("[ğışĞİŞ]"));
    if (trDistinctive.match(clean).hasMatch() || countMatches(tokens, trWords) >= 2) {
      return QStringLiteral("tr");
    }
  }

  const QString lower = clean.toLower();
  const QStringList tokens = lower.split(QRegularExpression(QStringLiteral("[\\s\\p{P}\\d]+")), Qt::SkipEmptyParts);
  if (tokens.isEmpty()) return {};

  struct Score {
    QString lang;
    int matches;
  };

  QList<Score> scores = {
      {QStringLiteral("en"), countMatches(tokens, enWords)},
      {QStringLiteral("tr"), countMatches(tokens, trWords)},
      {QStringLiteral("de"), countMatches(tokens, deWords)},
      {QStringLiteral("fr"), countMatches(tokens, frWords)},
      {QStringLiteral("es"), countMatches(tokens, esWords)},
      {QStringLiteral("it"), countMatches(tokens, itWords)},
      {QStringLiteral("pt"), countMatches(tokens, ptWords)},
      {QStringLiteral("nl"), countMatches(tokens, nlWords)}
  };

  std::sort(scores.begin(), scores.end(), [](const Score &a, const Score &b) {
    return a.matches > b.matches;
  });

  if (scores.first().matches >= 2 || (tokens.size() <= 4 && scores.first().matches >= 1)) {
    return scores.first().lang;
  }

  return {};
}

}  // namespace

QString LanguageDetector::normalizeLanguageCode(QString lang) {
  lang = lang.trimmed().toLower();
  const int hyphen = lang.indexOf(QLatin1Char('-'));
  if (hyphen > 0) lang = lang.left(hyphen);
  const int underscore = lang.indexOf(QLatin1Char('_'));
  if (underscore > 0) lang = lang.left(underscore);
  return lang.trimmed();
}

QString LanguageDetector::detectLanguage(const QString &htmlLang, const QString &contentLanguage, const QString &textSample) {
  const QString sampleLang = detectFromSample(textSample);
  if (!hasReliableTextSample(textSample)) return {};
  // Substantial visible text is a stronger signal than frequently stale or
  // templated metadata. Metadata remains the fallback for shorter documents.
  if (hasReliableTextSample(textSample) && !sampleLang.isEmpty()) return sampleLang;

  const QString normalizedHtml = normalizeLanguageCode(htmlLang);
  if (isValidLanguageCode(normalizedHtml)) return normalizedHtml;

  const QString normalizedContent = normalizeLanguageCode(contentLanguage);
  if (isValidLanguageCode(normalizedContent)) return normalizedContent;

  if (!sampleLang.isEmpty()) return sampleLang;

  return {};
}

QString LanguageDetector::detectTextLanguage(const QString &textSample) {
  return detectFromSample(textSample);
}

bool LanguageDetector::hasReliableTextSample(const QString &textSample) {
  const QStringList words = textSample.trimmed().split(
      QRegularExpression(QStringLiteral("[\\s\\p{P}\\d]+")), Qt::SkipEmptyParts);
  return words.size() >= 8 && textSample.trimmed().size() >= 48;
}

QString LanguageDetector::languageDisplayName(const QString &langCode) {
  // Language names follow the application language (catalog keys
  // language.name.<code>[_<region>]); a regional name wins when catalogued.
  auto &languages = dalinira::i18n::LanguageManager::instance();
  QString regional = langCode.trimmed().toLower();
  regional.replace(QLatin1Char('-'), QLatin1Char('_'));
  const QStringList candidates{regional, normalizeLanguageCode(langCode)};
  for (const QString &code : candidates) {
    if (code.isEmpty()) continue;
    const QString key = QStringLiteral("language.name.") + code;
    const QString name = languages.translate(key);
    if (name != key) return name;
  }
  const QString normalized = normalizeLanguageCode(langCode);
  return normalized.toUpper();
}

bool LanguageDetector::isTranslatable(const QString &detectedLang, const QString &targetLang) {
  const QString source = normalizeLanguageCode(detectedLang);
  const QString target = normalizeLanguageCode(targetLang);
  return !source.isEmpty() && source != target && isValidLanguageCode(source);
}
