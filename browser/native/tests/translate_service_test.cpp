#include "i18n/i18n.h"
#include <cassert>
#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrlQuery>
#include <QTimer>
#include <QTemporaryDir>
#include <QSettings>
#include <iostream>

#include "language_detector.h"
#include "translate_service.h"
#include "translation_secret_store.h"
#include "translate_engine_script.h"
#include "passwords/credential_vault_manager.h"

namespace {

class FakeTranslateNetworkAccessManager final : public QNetworkAccessManager {
 public:
  enum class Mode {
    Success,
    HttpError,
    MalformedJson,
    Timeout,
    AuthError
  };

  explicit FakeTranslateNetworkAccessManager(Mode mode, QObject *parent = nullptr)
      : QNetworkAccessManager(parent), mode_(mode) {}

  void setMode(Mode mode) { mode_ = mode; }
  int requestCount() const { return requestCount_; }
  QNetworkRequest lastRequest() const { return lastRequest_; }
  QByteArray lastRequestBody() const { return lastRequestBody_; }

 protected:
  QNetworkReply *createRequest(Operation op, const QNetworkRequest &request, QIODevice *outgoingData) override {
    Q_UNUSED(op);
    ++requestCount_;
    lastRequest_ = request;
    lastRequestBody_.clear();
    if (outgoingData) lastRequestBody_ = outgoingData->readAll();

    return new FakeTranslateReply(mode_, lastRequest_, lastRequestBody_, this);
  }

 private:
  class FakeTranslateReply final : public QNetworkReply {
   public:
    FakeTranslateReply(Mode mode, const QNetworkRequest &req, const QByteArray &reqBody, QObject *parent = nullptr)
        : QNetworkReply(parent), mode_(mode), req_(req), reqBody_(reqBody) {
      setOpenMode(QIODevice::ReadOnly);
      QTimer::singleShot(5, this, &FakeTranslateReply::simulateResponse);
    }

    void abort() override {
      setError(QNetworkReply::OperationCanceledError, QStringLiteral("Operation canceled"));
      emit errorOccurred(QNetworkReply::OperationCanceledError);
      setFinished(true);
      emit finished();
    }

    qint64 readData(char *data, qint64 maxlen) override {
      const qint64 bytesToRead = qMin(maxlen, static_cast<qint64>(buffer_.size() - offset_));
      if (bytesToRead <= 0) return 0;
      memcpy(data, buffer_.constData() + offset_, static_cast<size_t>(bytesToRead));
      offset_ += bytesToRead;
      return bytesToRead;
    }

    qint64 bytesAvailable() const override {
      return buffer_.size() - offset_ + QIODevice::bytesAvailable();
    }

   private:
    void simulateResponse() {
      if (mode_ == Mode::Timeout) {
        return;
      }

      if (mode_ == Mode::HttpError) {
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 500);
        setError(QNetworkReply::InternalServerError, QStringLiteral("HTTP 500 Internal Server Error"));
        buffer_ = "{\"error\":\"Internal translation engine failure\"}";
        setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        setFinished(true);
        emit errorOccurred(QNetworkReply::InternalServerError);
        emit finished();
        return;
      }

      if (mode_ == Mode::AuthError) {
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 403);
        setError(QNetworkReply::AuthenticationRequiredError, QStringLiteral("HTTP 403 Forbidden"));
        buffer_ = "{\"error\":\"Invalid API key or unauthorized\"}";
        setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        setFinished(true);
        emit errorOccurred(QNetworkReply::AuthenticationRequiredError);
        emit finished();
        return;
      }

      if (mode_ == Mode::MalformedJson) {
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 200);
        buffer_ = "<html><body>502 Bad Gateway</body></html>";
        setHeader(QNetworkRequest::ContentTypeHeader, "text/html");
        setFinished(true);
        emit errorOccurred(QNetworkReply::UnknownNetworkError);
        emit finished();
        return;
      }

      // Mode::Success
      setAttribute(QNetworkRequest::HttpStatusCodeAttribute, 200);
      setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

      const QUrl url = req_.url();

      if (url.toString().contains(QLatin1String("deepl.com"))) {
        const QJsonDocument reqDoc = QJsonDocument::fromJson(reqBody_);
        const QJsonArray textArray = reqDoc.object().value(QStringLiteral("text")).toArray();
        QJsonArray translations;
        for (const QJsonValue &v : textArray) {
          const QString src = v.toString();
          QJsonObject item;
          if (src == QLatin1String("Hello world")) item.insert(QStringLiteral("text"), QStringLiteral("Merhaba dünya"));
          else if (src == QLatin1String("Hello")) item.insert(QStringLiteral("text"), QStringLiteral("Merhaba"));
          else item.insert(QStringLiteral("text"), QStringLiteral("DeepL: ") + src);
          translations.append(item);
        }
        QJsonObject resp;
        resp.insert(QStringLiteral("translations"), translations);
        buffer_ = QJsonDocument(resp).toJson(QJsonDocument::Compact);
      } else if (url.toString().contains(QLatin1String("language/translate/v2"))) {
        const QJsonDocument reqDoc = QJsonDocument::fromJson(reqBody_);
        const QJsonArray qArray = reqDoc.object().value(QStringLiteral("q")).toArray();
        QJsonArray translations;
        for (const QJsonValue &v : qArray) {
          const QString src = v.toString();
          QJsonObject item;
          if (src == QLatin1String("Hello world")) item.insert(QStringLiteral("translatedText"), QStringLiteral("Merhaba dünya"));
          else if (src == QLatin1String("Hello")) item.insert(QStringLiteral("translatedText"), QStringLiteral("Merhaba"));
          else item.insert(QStringLiteral("translatedText"), QStringLiteral("GCP: ") + src);
          translations.append(item);
        }
        QJsonObject dataObj;
        dataObj.insert(QStringLiteral("translations"), translations);
        QJsonObject resp;
        resp.insert(QStringLiteral("data"), dataObj);
        buffer_ = QJsonDocument(resp).toJson(QJsonDocument::Compact);
      } else if (reqBody_.startsWith("q=")) {
        QUrlQuery q(QString::fromUtf8(reqBody_));
        const QString qStr = q.queryItemValue(QStringLiteral("q"), QUrl::FullyDecoded);
        const QString tl = QUrlQuery(req_.url()).queryItemValue(QStringLiteral("tl"), QUrl::FullyDecoded);
        const QStringList parts = qStr.split(QStringLiteral("___DALINIRA_SPLIT___"));
        QJsonArray segments;
        for (int i = 0; i < parts.size(); ++i) {
          const QString trimmed = parts.at(i).trimmed();
          QString translated;
          if (tl == QLatin1String("ar")) {
            if (trimmed == QLatin1String("Hello world")) translated = QString::fromUtf8("مرحبا بالعالم");
            else if (trimmed == QLatin1String("Search repository")) translated = QString::fromUtf8("ابحث في المستودع");
            else translated = QString::fromUtf8("ترجمة: ") + trimmed;
          } else {
            if (trimmed == QLatin1String("Hello world")) translated = QStringLiteral("Merhaba dünya");
            else if (trimmed == QLatin1String("This is a test page.")) translated = QStringLiteral("Bu bir test sayfasıdır.");
            else if (trimmed == QLatin1String("New dynamic content")) translated = QStringLiteral("Yeni dinamik içerik");
            else if (trimmed == QLatin1String("Search repository")) translated = QStringLiteral("Depoda ara");
            else if (trimmed == QLatin1String("Hello")) translated = QStringLiteral("Merhaba");
            else translated = QStringLiteral("Çeviri: ") + trimmed;
          }

          QJsonArray seg;
          if (i < parts.size() - 1) {
            if (tl == QLatin1String("ar")) {
              seg.append(translated + QString::fromUtf8("\n__أردالي_سبليت___\n"));
            } else {
              seg.append(translated + QStringLiteral("\n___DALINIRA_SPLIT___\n"));
            }
          } else {
            seg.append(translated);
          }
          seg.append(trimmed);
          segments.append(seg);
        }
        QJsonArray outer;
        outer.append(segments);
        buffer_ = QJsonDocument(outer).toJson(QJsonDocument::Compact);
      } else {
        const QJsonDocument reqDoc = QJsonDocument::fromJson(reqBody_);
        const QJsonObject reqObj = reqDoc.object();
        const QJsonArray qArray = reqObj.value(QStringLiteral("q")).toArray();

        QJsonArray translatedArray;
        for (const QJsonValue &v : qArray) {
          const QString src = v.toString();
          if (src == QLatin1String("Hello world")) {
            translatedArray.append(QStringLiteral("Merhaba dünya"));
          } else if (src == QLatin1String("This is a test page.")) {
            translatedArray.append(QStringLiteral("Bu bir test sayfasıdır."));
          } else if (src == QLatin1String("New dynamic content")) {
            translatedArray.append(QStringLiteral("Yeni dinamik içerik"));
          } else if (src == QLatin1String("Search repository")) {
            translatedArray.append(QStringLiteral("Depoda ara"));
          } else if (src == QLatin1String("Hello")) {
            translatedArray.append(QStringLiteral("Merhaba"));
          } else {
            translatedArray.append(QStringLiteral("Libre: ") + src);
          }
        }

        QJsonObject respObj;
        respObj.insert(QStringLiteral("translatedText"), translatedArray);
        buffer_ = QJsonDocument(respObj).toJson(QJsonDocument::Compact);
      }

      setFinished(true);
      emit finished();
    }

    Mode mode_;
    QNetworkRequest req_;
    QByteArray reqBody_;
    QByteArray buffer_;
    int offset_ = 0;
  };

  Mode mode_ = Mode::Success;
  int requestCount_ = 0;
  QNetworkRequest lastRequest_;
  QByteArray lastRequestBody_;
};

void testLanguageDetection() {
  std::cout << "[Test] Running testLanguageDetection..." << std::endl;

  const QString enSample = QStringLiteral("The quick brown fox jumps over the lazy dog and that was for you.");
  const QString trSample = QStringLiteral("Bu web sayfası Türkçe olarak hazırlanmıştır ve herkes için çok güzeldir.");
  const QString frSample = QStringLiteral("Cette page est écrite en français et contient des informations pour les personnes qui visitent le site.");
  const QString arSample = QString::fromUtf8("هذه صفحة عربية تحتوي على معلومات مفيدة لجميع الزوار والمستخدمين في الموقع");

  assert(LanguageDetector::detectLanguage(QStringLiteral("en-US"), QString(), enSample) == QStringLiteral("en"));
  assert(LanguageDetector::detectLanguage(QStringLiteral("tr-TR"), QString(), trSample) == QStringLiteral("tr"));
  assert(LanguageDetector::detectLanguage(QStringLiteral("FR"), QString(), frSample) == QStringLiteral("fr"));
  assert(LanguageDetector::detectLanguage(QStringLiteral("ar"), QString(), arSample) == QStringLiteral("ar"));
  // Visible page text wins over incorrect metadata.
  assert(LanguageDetector::detectLanguage(QStringLiteral("tr"), QString(), enSample) == QStringLiteral("en"));
  // Tiny/empty documents are deliberately not offered unreliable translation.
  assert(LanguageDetector::detectLanguage(QStringLiteral("en"), QString(), QStringLiteral("Hello")) .isEmpty());

  assert(LanguageDetector::detectLanguage(QString(), QString(), enSample) == QStringLiteral("en"));
  assert(LanguageDetector::detectLanguage(QString(), QString(), trSample) == QStringLiteral("tr"));
  assert(LanguageDetector::detectLanguage(QString(), QString(), frSample) == QStringLiteral("fr"));
  assert(LanguageDetector::detectLanguage(QString(), QString(), arSample) == QStringLiteral("ar"));

  assert(LanguageDetector::languageDisplayName(QStringLiteral("en")) == QStringLiteral("İngilizce"));
  assert(LanguageDetector::languageDisplayName(QStringLiteral("tr")) == QStringLiteral("Türkçe"));
  assert(LanguageDetector::isTranslatable(QStringLiteral("en"), QStringLiteral("tr")) == true);
  assert(LanguageDetector::isTranslatable(QStringLiteral("tr"), QStringLiteral("tr")) == false);

  std::cout << "[Test] testLanguageDetection passed!" << std::endl;
}

void testDomEngineScriptFiltering() {
  std::cout << "[Test] Running testDomEngineScriptFiltering..." << std::endl;

  const QString script = translateEngineScript();
  assert(!script.isEmpty());

  assert(script.contains(QStringLiteral("input[type=\"password\"]")));
  assert(script.contains(QStringLiteral("contenteditable")));
  assert(script.contains(QStringLiteral("translate")));
  assert(script.contains(QStringLiteral("notranslate")));
  assert(script.contains(QStringLiteral("SCRIPT")));
  assert(script.contains(QStringLiteral("STYLE")));
  assert(script.contains(QStringLiteral("TEXTAREA")));
  assert(script.contains(QStringLiteral("WeakMap")));
  assert(script.contains(QStringLiteral("restoreOriginal")));

  std::cout << "[Test] testDomEngineScriptFiltering passed!" << std::endl;
}

void testProviderFactoryAndNoneDefault() {
  std::cout << "[Test] Running testProviderFactoryAndNoneDefault..." << std::endl;

  auto *fakeNam = new FakeTranslateNetworkAccessManager(FakeTranslateNetworkAccessManager::Mode::Success);
  TranslateService service(nullptr, fakeNam);

  assert(service.providerId() == QStringLiteral("none"));
  assert(service.currentProvider() != nullptr);
  assert(service.currentProvider()->id() == QStringLiteral("none"));
  assert(service.currentProvider()->isConfigured() == false);

  bool callbackCalled = false;
  bool success = false;
  QStringList translated;
  QString err;
  service.translateBatch({QStringLiteral("Hello world")}, QStringLiteral("en"), QStringLiteral("tr"),
                         [&](bool s, const QStringList &result, const QString &e) {
                           callbackCalled = true;
                           success = s;
                           translated = result;
                           err = e;
                         });

  QEventLoop loop;
  QTimer::singleShot(1000, &loop, &QEventLoop::quit);
  QObject::connect(fakeNam, &QNetworkAccessManager::finished, &loop, &QEventLoop::quit);
  loop.exec();
  assert(callbackCalled == true);
  assert(success == true);
  assert(err.isEmpty());
  assert(translated == QStringList{QStringLiteral("Merhaba dünya")});
  assert(fakeNam->requestCount() == 1);

  std::cout << "[Test] testProviderFactoryAndNoneDefault passed!" << std::endl;
}

void testCredentialVaultIntegrationAndRoundtrip() {
  std::cout << "[Test] Running testCredentialVaultIntegrationAndRoundtrip..." << std::endl;

  QTemporaryDir tempVaultDir;
  assert(tempVaultDir.isValid());
  const QString masterPassword = QStringLiteral("MasterSecret#2026");

  CredentialVaultManager vaultManager(tempVaultDir.path());
  assert(!vaultManager.exists());
  assert(vaultManager.create(masterPassword));
  assert(!vaultManager.isLocked());

  auto *fakeNam = new FakeTranslateNetworkAccessManager(FakeTranslateNetworkAccessManager::Mode::Success);
  TranslateService service(nullptr, fakeNam, &vaultManager);

  // 1. Save and roundtrip LibreTranslate key
  assert(service.saveApiKey(QStringLiteral("libretranslate"), QStringLiteral("LT_TEST_SECRET_123")));
  assert(service.loadApiKey(QStringLiteral("libretranslate")) == QStringLiteral("LT_TEST_SECRET_123"));

  // 2. Save and roundtrip DeepL key
  assert(service.saveApiKey(QStringLiteral("deepl"), QStringLiteral("DEEPL_SECRET_456:fx")));
  assert(service.loadApiKey(QStringLiteral("deepl")) == QStringLiteral("DEEPL_SECRET_456:fx"));

  // 3. Save and roundtrip Google Cloud key
  assert(service.saveApiKey(QStringLiteral("google_cloud"), QStringLiteral("GCP_SECRET_789")));
  assert(service.loadApiKey(QStringLiteral("google_cloud")) == QStringLiteral("GCP_SECRET_789"));

  // 4. Test actual DeepL provider call with key retrieved from vault
  service.setProvider(QStringLiteral("deepl"));
  assert(service.currentProvider()->isConfigured() == true);

  QEventLoop loop;
  bool success = false;
  QStringList results;
  service.translateBatch({QStringLiteral("Hello world")}, QStringLiteral("en"), QStringLiteral("tr"),
                         [&](bool s, const QStringList &r, const QString &) {
                           success = s;
                           results = r;
                           loop.quit();
                         });
  loop.exec();

  assert(success == true);
  assert(results.size() == 1);
  assert(results.first() == QStringLiteral("Merhaba dünya"));
  assert(fakeNam->lastRequest().rawHeader("Authorization") == "DeepL-Auth-Key DEEPL_SECRET_456:fx");

  std::cout << "[Test] testCredentialVaultIntegrationAndRoundtrip passed!" << std::endl;
}

void testVaultLockedBehavior() {
  std::cout << "[Test] Running testVaultLockedBehavior..." << std::endl;

  QTemporaryDir tempVaultDir;
  assert(tempVaultDir.isValid());
  const QString masterPassword = QStringLiteral("MasterSecret#2026");

  CredentialVaultManager vaultManager(tempVaultDir.path());
  assert(vaultManager.create(masterPassword));

  auto *fakeNam = new FakeTranslateNetworkAccessManager(FakeTranslateNetworkAccessManager::Mode::Success);
  TranslateService service(nullptr, fakeNam, &vaultManager);

  service.setProvider(QStringLiteral("deepl"));
  assert(service.saveApiKey(QStringLiteral("deepl"), QStringLiteral("my-deepl-key:fx")));

  // Lock the vault
  vaultManager.lock();
  assert(vaultManager.isLocked());

  // API key retrieval must be refused when vault is locked
  assert(service.loadApiKey(QStringLiteral("deepl")).isEmpty());

  // Translation request must fail gracefully and not attempt any network request
  bool callbackCalled = false;
  bool success = true;
  QString errMsg;
  service.translateBatch({QStringLiteral("Hello world")}, QStringLiteral("en"), QStringLiteral("tr"),
                         [&](bool s, const QStringList &, const QString &e) {
                           callbackCalled = true;
                           success = s;
                           errMsg = e;
                         });

  assert(callbackCalled == true);
  assert(success == false);
  assert(errMsg.contains(QStringLiteral("güvenli kasanın kilidini açın")));
  assert(fakeNam->requestCount() == 0); // No network requests when vault is locked

  // Unlock the vault again
  assert(vaultManager.unlock(masterPassword));
  assert(!vaultManager.isLocked());
  assert(service.loadApiKey(QStringLiteral("deepl")) == QStringLiteral("my-deepl-key:fx"));

  // After unlocking, translation works immediately without restarting
  QEventLoop loop;
  service.translateBatch({QStringLiteral("Hello world")}, QStringLiteral("en"), QStringLiteral("tr"),
                         [&](bool s, const QStringList &, const QString &) {
                           success = s;
                           loop.quit();
                         });
  loop.exec();
  assert(success == true);
  assert(fakeNam->requestCount() == 1);

  std::cout << "[Test] testVaultLockedBehavior passed!" << std::endl;
}

void testSecretDelete() {
  std::cout << "[Test] Running testSecretDelete..." << std::endl;

  QTemporaryDir tempVaultDir;
  assert(tempVaultDir.isValid());
  const QString masterPassword = QStringLiteral("MasterSecret#2026");

  CredentialVaultManager vaultManager(tempVaultDir.path());
  assert(vaultManager.create(masterPassword));

  TranslateService service(nullptr, nullptr, &vaultManager);
  assert(service.saveApiKey(QStringLiteral("deepl"), QStringLiteral("secret-to-remove")));
  assert(service.loadApiKey(QStringLiteral("deepl")) == QStringLiteral("secret-to-remove"));

  // Remove secret
  assert(service.removeApiKey(QStringLiteral("deepl")));
  assert(service.loadApiKey(QStringLiteral("deepl")).isEmpty());

  std::cout << "[Test] testSecretDelete passed!" << std::endl;
}

void testNoPlaintextOrObfuscatedSecretsInQSettings() {
  std::cout << "[Test] Running testNoPlaintextOrObfuscatedSecretsInQSettings..." << std::endl;

  QTemporaryDir tempVaultDir;
  assert(tempVaultDir.isValid());
  const QString masterPassword = QStringLiteral("MasterSecret#2026");

  CredentialVaultManager vaultManager(tempVaultDir.path());
  assert(vaultManager.create(masterPassword));

  QTemporaryDir tempPrefsDir;
  assert(tempPrefsDir.isValid());
  const QString iniPath = tempPrefsDir.filePath(QStringLiteral("prefs.ini"));

  {
    QSettings prefs(iniPath, QSettings::IniFormat);
    TranslateService service(nullptr, nullptr, &vaultManager);
    service.setEnabled(true);
    service.setDefaultTargetLanguage(QStringLiteral("tr"));
    service.setProvider(QStringLiteral("deepl"));
    service.setLibreTranslateEndpoint(QUrl(QStringLiteral("https://lt.local/translate")));
    service.setDeepLIsPro(true);
    service.saveApiKey(QStringLiteral("deepl"), QStringLiteral("REAL_SUPER_SECRET_KEY_12345"));
    service.savePreferences(prefs);
  }

  // Inspect raw INI file bytes on disk
  QFile iniFile(iniPath);
  assert(iniFile.open(QIODevice::ReadOnly));
  const QByteArray iniBytes = iniFile.readAll();

  // MUST NOT contain the real key
  assert(!iniBytes.contains("REAL_SUPER_SECRET_KEY_12345"));
  // MUST NOT contain active obfuscated keys
  assert(!iniBytes.contains("translation/sec_deepl"));
  assert(!iniBytes.contains("translation/sec_lt"));
  assert(!iniBytes.contains("translation/sec_gcp"));

  // Non-secret preferences must be stored correctly
  QSettings verifyPrefs(iniPath, QSettings::IniFormat);
  assert(verifyPrefs.value(QStringLiteral("translation/provider")).toString() == QStringLiteral("deepl"));
  assert(verifyPrefs.value(QStringLiteral("translation/deeplPlan")).toString() == QStringLiteral("pro"));
  assert(verifyPrefs.value(QStringLiteral("translation/libretranslateEndpoint")).toString() == QStringLiteral("https://lt.local/translate"));

  std::cout << "[Test] testNoPlaintextOrObfuscatedSecretsInQSettings passed!" << std::endl;
}

void testLegacyMigrationToVault() {
  std::cout << "[Test] Running testLegacyMigrationToVault..." << std::endl;

  QTemporaryDir tempVaultDir;
  assert(tempVaultDir.isValid());
  const QString masterPassword = QStringLiteral("MasterSecret#2026");

  CredentialVaultManager vaultManager(tempVaultDir.path());
  assert(vaultManager.create(masterPassword));

  QTemporaryDir tempPrefsDir;
  assert(tempPrefsDir.isValid());
  const QString iniPath = tempPrefsDir.filePath(QStringLiteral("legacy-prefs.ini"));

  // Simulate an old V2.1 preference file with Base64/XOR obfuscated keys
  {
    QSettings prefs(iniPath, QSettings::IniFormat);
    // Legacy obfuscation of "MIGRATED_DEEPL_KEY_999" with XOR mask 0x5A
    const QString clearKey = QStringLiteral("MIGRATED_DEEPL_KEY_999");
    const QByteArray raw = clearKey.toUtf8();
    QByteArray obfuscated;
    constexpr quint8 kMask = 0x5A;
    for (int i = 0; i < raw.size(); ++i) {
      obfuscated.append(static_cast<char>(raw.at(i) ^ (kMask + (i % 7))));
    }
    prefs.setValue(QStringLiteral("translation/sec_deepl"), obfuscated.toBase64());
    prefs.setValue(QStringLiteral("translation/provider"), QStringLiteral("deepl"));
  }

  // Load preferences with TranslateService connected to unlocked vault
  {
    QSettings prefs(iniPath, QSettings::IniFormat);
    TranslateService service(nullptr, nullptr, &vaultManager);
    service.loadPreferences(prefs);

    // Key must now be decrypted and stored in the secure Credential Vault
    assert(service.loadApiKey(QStringLiteral("deepl")) == QStringLiteral("MIGRATED_DEEPL_KEY_999"));

    // Legacy key must have been cleanly purged from QSettings
    assert(!prefs.contains(QStringLiteral("translation/sec_deepl")));
  }

  std::cout << "[Test] testLegacyMigrationToVault passed!" << std::endl;
}

void testGoogleGtxExplicitSelection() {
  std::cout << "[Test] Running testGoogleGtxExplicitSelection..." << std::endl;

  auto *fakeNam = new FakeTranslateNetworkAccessManager(FakeTranslateNetworkAccessManager::Mode::Success);
  TranslateService service(nullptr, fakeNam);

  assert(service.providerId() == QStringLiteral("none"));

  service.setProvider(QStringLiteral("google_gtx"));
  assert(service.providerId() == QStringLiteral("google_gtx"));
  // The name is localized; it must be the catalog's "experimental / unofficial" label.
  assert(service.currentProvider()->displayName() == I18n::text(QStringLiteral("settings.languages.provider_gtx")));

  QEventLoop loop;
  bool success = false;
  QStringList results;
  service.translateBatch({QStringLiteral("Hello world")}, QStringLiteral("en"), QStringLiteral("tr"),
                         [&](bool s, const QStringList &r, const QString &) {
                           success = s;
                           results = r;
                           loop.quit();
                         });
  loop.exec();

  assert(success == true);
  assert(results.size() == 1);
  assert(results.first() == QStringLiteral("Merhaba dünya"));
  assert(fakeNam->requestCount() == 1);

  std::cout << "[Test] testGoogleGtxExplicitSelection passed!" << std::endl;
}

void testRuntimeProviderSwitching() {
  std::cout << "[Test] Running testRuntimeProviderSwitching..." << std::endl;

  QTemporaryDir tempVaultDir;
  assert(tempVaultDir.isValid());
  const QString masterPassword = QStringLiteral("MasterSecret#2026");

  CredentialVaultManager vaultManager(tempVaultDir.path());
  assert(vaultManager.create(masterPassword));

  auto *fakeNam = new FakeTranslateNetworkAccessManager(FakeTranslateNetworkAccessManager::Mode::Success);
  TranslateService service(nullptr, fakeNam, &vaultManager);

  // 1. DeepL
  service.setProvider(QStringLiteral("deepl"));
  service.saveApiKey(QStringLiteral("deepl"), QStringLiteral("key-deepl"));

  QEventLoop loop1;
  service.translateBatch({QStringLiteral("Hello")}, QStringLiteral("en"), QStringLiteral("tr"),
                         [&](bool, const QStringList &, const QString &) { loop1.quit(); });
  loop1.exec();
  assert(fakeNam->lastRequest().url().toString().contains(QLatin1String("deepl.com")));

  // 2. Google Cloud
  service.clearCache();
  service.setProvider(QStringLiteral("google_cloud"));
  service.saveApiKey(QStringLiteral("google_cloud"), QStringLiteral("key-gcp"));

  QEventLoop loop2;
  service.translateBatch({QStringLiteral("Hello")}, QStringLiteral("en"), QStringLiteral("tr"),
                         [&](bool, const QStringList &, const QString &) { loop2.quit(); });
  loop2.exec();
  assert(fakeNam->lastRequest().url().toString().contains(QLatin1String("language/translate/v2")));

  // 3. LibreTranslate
  service.clearCache();
  service.setProvider(QStringLiteral("libretranslate"));
  service.setLibreTranslateEndpoint(QUrl(QStringLiteral("https://lt.local/translate")));

  QEventLoop loop3;
  service.translateBatch({QStringLiteral("Hello")}, QStringLiteral("en"), QStringLiteral("tr"),
                         [&](bool, const QStringList &, const QString &) { loop3.quit(); });
  loop3.exec();
  assert(fakeNam->lastRequest().url().toString().contains(QLatin1String("lt.local")));

  std::cout << "[Test] testRuntimeProviderSwitching passed!" << std::endl;
}

void testTestConnectionFeature() {
  std::cout << "[Test] Running testTestConnectionFeature..." << std::endl;

  QTemporaryDir tempVaultDir;
  assert(tempVaultDir.isValid());
  const QString masterPassword = QStringLiteral("MasterSecret#2026");

  CredentialVaultManager vaultManager(tempVaultDir.path());
  assert(vaultManager.create(masterPassword));

  auto *fakeNam = new FakeTranslateNetworkAccessManager(FakeTranslateNetworkAccessManager::Mode::Success);
  TranslateService service(nullptr, fakeNam, &vaultManager);

  service.saveApiKey(QStringLiteral("deepl"), QStringLiteral("valid-key"));

  QEventLoop loop;
  bool testSuccess = false;
  QString testMessage;

  service.testConnection(QStringLiteral("deepl"), [&](bool s, const QString &msg) {
    testSuccess = s;
    testMessage = msg;
    loop.quit();
  });
  loop.exec();

  assert(testSuccess == true);
  assert(testMessage.contains(QStringLiteral("Bağlantı başarılı")));

  // Auth failure test
  fakeNam->setMode(FakeTranslateNetworkAccessManager::Mode::AuthError);
  QEventLoop loopAuth;
  bool authSuccess = true;
  QString authMessage;

  service.testConnection(QStringLiteral("deepl"), [&](bool s, const QString &msg) {
    authSuccess = s;
    authMessage = msg;
    loopAuth.quit();
  });
  loopAuth.exec();

  assert(authSuccess == false);
  assert(authMessage.contains(QStringLiteral("API anahtarı geçersiz")));

  std::cout << "[Test] testTestConnectionFeature passed!" << std::endl;
}

void testV2DynamicEngineFeatures() {
  std::cout << "[Test] Running testV2DynamicEngineFeatures..." << std::endl;

  const QString script = translateEngineScript();
  assert(!script.isEmpty());

  assert(script.contains(QStringLiteral("MutationObserver")));
  assert(script.contains(QStringLiteral("childList")));
  assert(script.contains(QStringLiteral("subtree")));
  assert(script.contains(QStringLiteral("characterData")));
  assert(script.contains(QStringLiteral("isApplyingBatch")));
  assert(script.contains(QStringLiteral("isTranslating")));
  assert(script.contains(QStringLiteral("nodeMeta")));
  assert(script.contains(QStringLiteral("checkPendingMutations")));
  assert(script.contains(QStringLiteral("startObserving")));
  assert(script.contains(QStringLiteral("stopObserving")));
  assert(script.contains(QStringLiteral("history.pushState")));
  assert(script.contains(QStringLiteral("history.replaceState")));
  assert(script.contains(QStringLiteral("popstate")));
  assert(script.contains(QStringLiteral("shadowRoot")));
  assert(script.contains(QStringLiteral("contentDocument")));
  assert(script.contains(QStringLiteral("title")));
  assert(script.contains(QStringLiteral("placeholder")));
  assert(script.contains(QStringLiteral("aria-label")));

  std::cout << "[Test] testV2DynamicEngineFeatures passed!" << std::endl;
}

void testV2DynamicBatchingAndRerender() {
  std::cout << "[Test] Running testV2DynamicBatchingAndRerender..." << std::endl;

  auto *fakeNam = new FakeTranslateNetworkAccessManager(FakeTranslateNetworkAccessManager::Mode::Success);
  TranslateService service(nullptr, fakeNam);
  service.setProvider(QStringLiteral("google_gtx"));

  const QStringList initialBatch = {
      QStringLiteral("Hello world"),
      QStringLiteral("Search repository")
  };

  QEventLoop loop;
  bool initialSuccess = false;
  QStringList initialResults;

  service.translateBatch(initialBatch, QStringLiteral("en"), QStringLiteral("tr"),
                         [&](bool success, const QStringList &results, const QString &) {
                           initialSuccess = success;
                           initialResults = results;
                           loop.quit();
                         });
  loop.exec();

  assert(initialSuccess == true);
  assert(initialResults.size() == 2);
  assert(initialResults.at(0) == QStringLiteral("Merhaba dünya"));
  assert(initialResults.at(1) == QStringLiteral("Depoda ara"));
  assert(fakeNam->requestCount() == 1);

  const QStringList dynamicBatch = {
      QStringLiteral("Hello world"),
      QStringLiteral("New dynamic content")
  };

  QEventLoop dynamicLoop;
  bool dynamicSuccess = false;
  QStringList dynamicResults;

  service.translateBatch(dynamicBatch, QStringLiteral("en"), QStringLiteral("tr"),
                         [&](bool success, const QStringList &results, const QString &) {
                           dynamicSuccess = success;
                           dynamicResults = results;
                           dynamicLoop.quit();
                         });
  dynamicLoop.exec();

  assert(dynamicSuccess == true);
  assert(dynamicResults.size() == 2);
  assert(dynamicResults.at(0) == QStringLiteral("Merhaba dünya"));
  assert(dynamicResults.at(1) == QStringLiteral("Yeni dinamik içerik"));
  assert(fakeNam->requestCount() == 2);

  // Cached batch
  bool cachedSuccess = false;
  QStringList cachedResults;
  service.translateBatch(dynamicBatch, QStringLiteral("en"), QStringLiteral("tr"),
                         [&](bool success, const QStringList &results, const QString &) {
                           cachedSuccess = success;
                           cachedResults = results;
                         });

  assert(cachedSuccess == true);
  assert(cachedResults == dynamicResults);
  assert(fakeNam->requestCount() == 2);

  std::cout << "[Test] testV2DynamicBatchingAndRerender passed!" << std::endl;
}

static void testLanguageCodeMappingAcrossProviders() {
  // Google GTX: lowercase normalization, locale handling
  assert(TranslateService::mapLanguageForProvider(QStringLiteral("google_gtx"), QStringLiteral("ar"), true) == QStringLiteral("ar"));
  assert(TranslateService::mapLanguageForProvider(QStringLiteral("google_gtx"), QStringLiteral("AR"), true) == QStringLiteral("ar"));
  assert(TranslateService::mapLanguageForProvider(QStringLiteral("google_gtx"), QStringLiteral("en-US"), true) == QStringLiteral("en"));
  assert(TranslateService::mapLanguageForProvider(QStringLiteral("google_gtx"), QStringLiteral("en-GB"), true) == QStringLiteral("en"));
  assert(TranslateService::mapLanguageForProvider(QStringLiteral("google_gtx"), QStringLiteral("zh-CN"), true) == QStringLiteral("zh-CN"));
  assert(TranslateService::mapLanguageForProvider(QStringLiteral("google_gtx"), QStringLiteral("pt-BR"), true) == QStringLiteral("pt"));
  assert(TranslateService::mapLanguageForProvider(QStringLiteral("google_gtx"), QStringLiteral("tr"), true) == QStringLiteral("tr"));

  // DeepL: target language specificity & uppercase
  assert(TranslateService::mapLanguageForProvider(QStringLiteral("deepl"), QStringLiteral("ar"), true) == QStringLiteral("AR"));
  assert(TranslateService::mapLanguageForProvider(QStringLiteral("deepl"), QStringLiteral("en"), true) == QStringLiteral("EN-US"));
  assert(TranslateService::mapLanguageForProvider(QStringLiteral("deepl"), QStringLiteral("en-US"), true) == QStringLiteral("EN-US"));
  assert(TranslateService::mapLanguageForProvider(QStringLiteral("deepl"), QStringLiteral("en-GB"), true) == QStringLiteral("EN-GB"));
  assert(TranslateService::mapLanguageForProvider(QStringLiteral("deepl"), QStringLiteral("pt"), true) == QStringLiteral("PT-PT"));
  assert(TranslateService::mapLanguageForProvider(QStringLiteral("deepl"), QStringLiteral("pt-BR"), true) == QStringLiteral("PT-BR"));
  assert(TranslateService::mapLanguageForProvider(QStringLiteral("deepl"), QStringLiteral("zh-CN"), true) == QStringLiteral("ZH"));
  assert(TranslateService::mapLanguageForProvider(QStringLiteral("deepl"), QStringLiteral("zh"), true) == QStringLiteral("ZH"));
  assert(TranslateService::mapLanguageForProvider(QStringLiteral("deepl"), QStringLiteral("en-US"), false) == QStringLiteral("EN"));

  // Google Cloud
  assert(TranslateService::mapLanguageForProvider(QStringLiteral("google_cloud"), QStringLiteral("ar"), true) == QStringLiteral("ar"));
  assert(TranslateService::mapLanguageForProvider(QStringLiteral("google_cloud"), QStringLiteral("en-US"), true) == QStringLiteral("en"));
  assert(TranslateService::mapLanguageForProvider(QStringLiteral("google_cloud"), QStringLiteral("zh-CN"), true) == QStringLiteral("zh-CN"));

  // LibreTranslate
  assert(TranslateService::mapLanguageForProvider(QStringLiteral("libretranslate"), QStringLiteral("ar"), true) == QStringLiteral("ar"));
  assert(TranslateService::mapLanguageForProvider(QStringLiteral("libretranslate"), QStringLiteral("en-US"), true) == QStringLiteral("en"));
  assert(TranslateService::mapLanguageForProvider(QStringLiteral("libretranslate"), QStringLiteral("zh-CN"), true) == QStringLiteral("zh"));

  std::cout << "[Test] testLanguageCodeMappingAcrossProviders passed!" << std::endl;
}

static void testGoogleGtxArabicBatchTransliteration() {
  auto *fakeNam = new FakeTranslateNetworkAccessManager(FakeTranslateNetworkAccessManager::Mode::Success);
  TranslateService service(nullptr, fakeNam);
  service.setProvider(QStringLiteral("google_gtx"));

  const QStringList batch = {
      QStringLiteral("Hello world"),
      QStringLiteral("Search repository")
  };

  QEventLoop loop;
  bool successResult = false;
  QStringList resultsList;

  service.translateBatch(batch, QStringLiteral("en"), QStringLiteral("ar"),
                         [&](bool success, const QStringList &results, const QString &) {
                           successResult = success;
                           resultsList = results;
                           loop.quit();
                         });
  loop.exec();

  assert(successResult == true);
  assert(resultsList.size() == 2);
  assert(resultsList.at(0) == QString::fromUtf8("مرحبا بالعالم"));
  assert(resultsList.at(1) == QString::fromUtf8("ابحث في المستودع"));

  std::cout << "[Test] testGoogleGtxArabicBatchTransliteration passed!" << std::endl;
}

}  // namespace

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);

  testLanguageDetection();
  testDomEngineScriptFiltering();
  testProviderFactoryAndNoneDefault();
  testCredentialVaultIntegrationAndRoundtrip();
  testVaultLockedBehavior();
  testSecretDelete();
  testNoPlaintextOrObfuscatedSecretsInQSettings();
  testLegacyMigrationToVault();
  testGoogleGtxExplicitSelection();
  testRuntimeProviderSwitching();
  testTestConnectionFeature();
  testV2DynamicEngineFeatures();
  testV2DynamicBatchingAndRerender();
  testLanguageCodeMappingAcrossProviders();
  testGoogleGtxArabicBatchTransliteration();

  std::cout << "All DaliNira Translation Provider & Credential Vault Security Tests passed successfully!" << std::endl;
  return 0;
}
