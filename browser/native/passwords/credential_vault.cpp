#include "credential_vault.h"
#include "device_keyring.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSettings>
#include <QMutexLocker>
#include <QTimer>
#include <QUuid>

#include <openssl/evp.h>
#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

namespace {
constexpr int kPbkdf2Iterations = 600000;
constexpr int kKeyBytes = 32;
constexpr int kNonceBytes = 12;
constexpr int kTagBytes = 16;
constexpr qint64 kMaxVaultBytes = 16 * 1024 * 1024;
constexpr int kMaxRecords = 1000;
constexpr int kCurrentSchemaVersion = 4;

QByteArray b64(const QByteArray &value) { return value.toBase64(); }
QByteArray fromB64(const QJsonValue &value) { return QByteArray::fromBase64(value.toString().toLatin1()); }
bool randomBytes(QByteArray *out, int size) { out->resize(size); return RAND_bytes(reinterpret_cast<unsigned char *>(out->data()), size) == 1; }

bool deriveKey(const QString &password, const QByteArray &salt, const QByteArray &deviceSecret, QByteArray *key) {
  const QByteArray utf8 = password.toUtf8();
  QByteArray pbkdf2Key;
  pbkdf2Key.resize(kKeyBytes);
  if (PKCS5_PBKDF2_HMAC(utf8.constData(), utf8.size(),
                        reinterpret_cast<const unsigned char *>(salt.constData()), salt.size(),
                        kPbkdf2Iterations, EVP_sha256(), kKeyBytes,
                        reinterpret_cast<unsigned char *>(pbkdf2Key.data())) != 1) {
    if (pbkdf2Key.size() > 0) std::fill(pbkdf2Key.begin(), pbkdf2Key.end(), '\0');
    return false;
  }
  if (deviceSecret.isEmpty()) {
    *key = pbkdf2Key;
    return true;
  }
  key->resize(kKeyBytes);
  unsigned int outLen = 0;
  unsigned char *res = HMAC(EVP_sha256(),
                            deviceSecret.constData(), deviceSecret.size(),
                            reinterpret_cast<const unsigned char *>(pbkdf2Key.constData()), pbkdf2Key.size(),
                            reinterpret_cast<unsigned char *>(key->data()), &outLen);
  std::fill(pbkdf2Key.begin(), pbkdf2Key.end(), '\0');
  return (res != nullptr && outLen == kKeyBytes);
}

bool aesGcm(bool encrypt, const QByteArray &key, const QByteArray &nonce, const QByteArray &input, const QByteArray &aad,
            QByteArray *output, QByteArray *tag) {
  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new(); if (!ctx) return false;
  int size = 0, finalSize = 0; bool ok = false; output->resize(input.size() + kTagBytes);
  do {
    if (EVP_CipherInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr, encrypt ? 1 : 0) != 1) break;
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, nonce.size(), nullptr) != 1) break;
    if (EVP_CipherInit_ex(ctx, nullptr, nullptr, reinterpret_cast<const unsigned char *>(key.constData()), reinterpret_cast<const unsigned char *>(nonce.constData()), -1) != 1) break;
    if (!aad.isEmpty() && EVP_CipherUpdate(ctx, nullptr, &size, reinterpret_cast<const unsigned char *>(aad.constData()), aad.size()) != 1) break;
    if (EVP_CipherUpdate(ctx, reinterpret_cast<unsigned char *>(output->data()), &size, reinterpret_cast<const unsigned char *>(input.constData()), input.size()) != 1) break;
    if (!encrypt && (tag->size() != kTagBytes || EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, kTagBytes, tag->data()) != 1)) break;
    if (EVP_CipherFinal_ex(ctx, reinterpret_cast<unsigned char *>(output->data()) + size, &finalSize) != 1) break;
    output->resize(size + finalSize);
    if (encrypt) { tag->resize(kTagBytes); if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, kTagBytes, tag->data()) != 1) break; }
    ok = true;
  } while (false);
  EVP_CIPHER_CTX_free(ctx); return ok;
}
QString recordAad(const QString &id) { return QStringLiteral("dalinira-vault-record:") + id; }
}

CredentialVault::CredentialVault(const QString &dataDirectory, QObject *parent, const QString &storageId)
    : QObject(parent),
      directory_(storageId.isEmpty() ? dataDirectory + QStringLiteral("/credential-vault") : dataDirectory + QStringLiteral("/credential-vault/vaults/") + storageId),
      storageId_(storageId),
      path_(directory_ + QStringLiteral("/vault-v2.json")),
      securityStatePath_(directory_ + QStringLiteral("/security-state.json")),
      originIndexPath_(directory_ + QStringLiteral("/origin-index.json")) {
  const int configured = QSettings().value(QStringLiteral("browser/passwords/autoLockMs"), autoLockTimeoutMs_).toInt();
  if (configured == 60000 || configured == 5 * 60000 || configured == 15 * 60000 || configured == 30 * 60000) autoLockTimeoutMs_ = configured;
  idleTimer_ = new QTimer(this); idleTimer_->setInterval(15000);
  connect(idleTimer_, &QTimer::timeout, this, [this] { if (!isLocked() && QDateTime::currentMSecsSinceEpoch() - lastActivityMs_ >= autoLockTimeoutMs_) lock(); });
  idleTimer_->start();
  loadSecurityState();
  loadOriginIndex();
}

CredentialVault::~CredentialVault() { lock(); }

QString CredentialVault::vaultKeyringId() const {
  const QString saltHex = salt_.isEmpty() ? QStringLiteral("nosalt") : QString::fromLatin1(salt_.toHex());
  if (storageId_.isEmpty()) {
    return QStringLiteral("primary-") + saltHex;
  }
  return storageId_ + QLatin1Char('-') + saltHex;
}

QString CredentialVault::legacyNosaltKeyringId() const {
  return storageId_.isEmpty() ? QStringLiteral("primary-nosalt")
                              : storageId_ + QStringLiteral("-nosalt");
}

bool CredentialVault::obtainDeviceSecret(QByteArray *outSecret) const {
  if (!outSecret) return false;
  if (!cachedDeviceSecret_.isEmpty()) {
    *outSecret = cachedDeviceSecret_;
    return true;
  }
  if (!DeviceKeyring::isAvailable()) {
    return false;
  }
  return DeviceKeyring::loadSecret(vaultKeyringId(), outSecret);
}

int CredentialVault::schemaVersion() const {
  QMutexLocker locker(&mutex_);
  return schemaVersion_;
}

bool CredentialVault::isDeviceBound() const {
  QMutexLocker locker(&mutex_);
  return (schemaVersion_ >= 3 && deviceBinding_ == QLatin1String("secret-service"));
}

QString CredentialVault::deviceBinding() const {
  QMutexLocker locker(&mutex_);
  return deviceBinding_;
}

QString CredentialVault::storageId() const {
  return storageId_;
}

QByteArray CredentialVault::deviceSecret() const {
  QMutexLocker locker(&mutex_);
  QByteArray secret;
  obtainDeviceSecret(&secret);
  return secret;
}

QString CredentialVault::canonicalKeyringId() {
  QMutexLocker locker(&mutex_);
  if (salt_.isEmpty() && exists() && !loadEnvelope()) return {};
  return salt_.isEmpty() ? QString{} : vaultKeyringId();
}

bool CredentialVault::exportDeviceSecret(QByteArray *secret) {
  QMutexLocker locker(&mutex_);
  if (!secret) return setError(QStringLiteral("invalid-device-secret-output"));
  secureClear(secret);
  if (salt_.isEmpty() && exists() && !loadEnvelope()) return false;
  if (schemaVersion_ < 3 || deviceBinding_ != QLatin1String("secret-service")) return true;
  if (!obtainDeviceSecret(secret) || secret->size() != kKeyBytes) {
    secureClear(secret);
    return setError(QStringLiteral("keyring-unavailable"));
  }
  return true;
}

bool CredentialVault::importDeviceSecret(const QByteArray &secret) {
  QMutexLocker locker(&mutex_);
  if (secret.size() != kKeyBytes || !exists()) return false;
  if (salt_.isEmpty() && !loadEnvelope()) return false;
  if (salt_.size() != 16 || schemaVersion_ < 3 ||
      deviceBinding_ != QLatin1String("secret-service") || !DeviceKeyring::isAvailable()) return false;
  const QString keyId = vaultKeyringId();
  if (keyId.endsWith(QLatin1String("-nosalt")) || !DeviceKeyring::storeSecret(keyId, secret)) return false;
  QByteArray verified;
  const bool stored = DeviceKeyring::loadSecret(keyId, &verified) && verified.size() == secret.size() &&
      CRYPTO_memcmp(verified.constData(), secret.constData(), size_t(secret.size())) == 0;
  secureClear(&verified);
  if (!stored) {
    DeviceKeyring::clearSecret(keyId);
    return false;
  }
  cachedDeviceSecret_ = secret;
  return true;
}

bool CredentialVault::recoverLegacyDeviceSecret(const QString &masterPassword, QByteArray *outSecret) {
  if (!outSecret || salt_.size() != 16 || storageId_.isEmpty() || !DeviceKeyring::isAvailable()) return false;
  QByteArray legacySecret;
  if (!DeviceKeyring::loadSecret(legacyNosaltKeyringId(), &legacySecret) || legacySecret.size() != kKeyBytes) {
    secureClear(&legacySecret);
    return false;
  }
  QByteArray wrapKey, verifiedKey, tag = wrappedTag_;
  const bool authentic = deriveKey(masterPassword, salt_, legacySecret, &wrapKey) &&
      aesGcm(false, wrapKey, wrappedNonce_, wrappedKey_, QByteArrayLiteral("dalinira-vault-key-v3"),
             &verifiedKey, &tag) && verifiedKey.size() == kKeyBytes;
  secureClear(&wrapKey);
  secureClear(&verifiedKey);
  if (!authentic || !DeviceKeyring::storeSecret(vaultKeyringId(), legacySecret)) {
    secureClear(&legacySecret);
    return false;
  }
  QByteArray canonical;
  const bool verified = DeviceKeyring::loadSecret(vaultKeyringId(), &canonical) &&
      canonical.size() == legacySecret.size() &&
      CRYPTO_memcmp(canonical.constData(), legacySecret.constData(), size_t(legacySecret.size())) == 0;
  secureClear(&canonical);
  if (!verified) {
    DeviceKeyring::clearSecret(vaultKeyringId());
    secureClear(&legacySecret);
    return false;
  }
  DeviceKeyring::clearSecret(legacyNosaltKeyringId());
  *outSecret = legacySecret;
  secureClear(&legacySecret);
  return true;
}

int CredentialVault::cooldownForAttempt(int attempts) {
  if (attempts <= 4) return 0;
  switch (attempts) {
    case 5: return 30;
    case 6: return 60;
    case 7: return 120;
    case 8: return 300;
    case 9: return 900;
    default: return 1800;
  }
}

qint64 CredentialVault::nowEpochMs() const {
  if (timeProvider_) {
    return timeProvider_();
  }
  return QDateTime::currentMSecsSinceEpoch();
}

void CredentialVault::loadSecurityState() {
  QFile file(securityStatePath_);
  if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
    return;
  }
  const QByteArray data = file.readAll();
  const QJsonDocument doc = QJsonDocument::fromJson(data);
  if (!doc.isObject()) {
    return;
  }
  const QJsonObject root = doc.object();
  const int attempts = root.value(QStringLiteral("failedAttempts")).toInt();
  const qint64 expiry = root.value(QStringLiteral("cooldownExpiryEpochMs")).toVariant().toLongLong();

  failedUnlocks_ = std::clamp(attempts, 0, 10000);
  const qint64 now = nowEpochMs();
  if (expiry > now) {
    const qint64 remainingMs = expiry - now;
    const qint64 clampedSecs = std::clamp<qint64>((remainingMs + 999) / 1000, 1, 1800);
    cooldownExpiryEpochMs_ = now + (clampedSecs * 1000LL);
    cooldownExpirySteady_ = std::chrono::steady_clock::now() + std::chrono::seconds(clampedSecs);
    hasSteadyExpiry_ = true;
  } else {
    cooldownExpiryEpochMs_ = 0;
    hasSteadyExpiry_ = false;
  }
}

void CredentialVault::persistSecurityState() {
  QDir().mkpath(directory_);
  QJsonObject root;
  root.insert(QStringLiteral("version"), 1);
  root.insert(QStringLiteral("failedAttempts"), failedUnlocks_);
  root.insert(QStringLiteral("cooldownExpiryEpochMs"), cooldownExpiryEpochMs_);
  root.insert(QStringLiteral("updatedAt"), nowEpochMs());

  QSaveFile file(securityStatePath_);
  if (file.open(QIODevice::WriteOnly)) {
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    if (file.commit()) {
      QFile::setPermissions(securityStatePath_, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }
  }
}

void CredentialVault::clearPersistedSecurityState() {
  if (QFile::exists(securityStatePath_)) {
    QFile::remove(securityStatePath_);
  }
}

QString CredentialVault::originToken(const QString &canonicalOrigin) const {
  if (dataKey_.size() != kKeyBytes || canonicalOrigin.isEmpty()) return {};
  const QByteArray message = QByteArrayLiteral("dalinira-origin-index-v2:") + canonicalOrigin.toUtf8();
  QByteArray digest(kKeyBytes, Qt::Uninitialized);
  unsigned int length = 0;
  if (!HMAC(EVP_sha256(), dataKey_.constData(), dataKey_.size(),
            reinterpret_cast<const unsigned char *>(message.constData()), message.size(),
            reinterpret_cast<unsigned char *>(digest.data()), &length) || length != kKeyBytes) return {};
  return QString::fromLatin1(digest.toHex());
}

void CredentialVault::loadOriginIndex() {
  QFile file(originIndexPath_);
  if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
    return;
  }
  const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
  if (root.value(QStringLiteral("version")).toInt() != 2) return;
  const QJsonArray arr = root.value(QStringLiteral("originTokens")).toArray();
  storedOriginHashes_.clear();
  for (const QJsonValue &v : arr) {
    const QString h = v.toString().trimmed();
    if (!h.isEmpty()) {
      storedOriginHashes_.insert(h);
    }
  }
}

void CredentialVault::persistOriginIndex() {
  QDir().mkpath(directory_);
  QJsonArray arr;
  for (const QString &h : storedOriginHashes_) {
    arr.append(h);
  }
  QJsonObject root;
  root.insert(QStringLiteral("version"), 2);
  root.insert(QStringLiteral("originTokens"), arr);

  QSaveFile file(originIndexPath_);
  if (file.open(QIODevice::WriteOnly)) {
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    if (file.commit()) {
      QFile::setPermissions(originIndexPath_, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }
  }
}

void CredentialVault::clearPersistedOriginIndex() {
  if (QFile::exists(originIndexPath_)) {
    QFile::remove(originIndexPath_);
  }
}

void CredentialVault::recordFailedUnlockAttempt() {
  failedUnlocks_++;
  const int cooldownSecs = cooldownForAttempt(failedUnlocks_);
  if (cooldownSecs > 0) {
    const qint64 now = nowEpochMs();
    cooldownExpiryEpochMs_ = now + (static_cast<qint64>(cooldownSecs) * 1000LL);
    cooldownExpirySteady_ = std::chrono::steady_clock::now() + std::chrono::seconds(cooldownSecs);
    hasSteadyExpiry_ = true;
  } else {
    cooldownExpiryEpochMs_ = 0;
    hasSteadyExpiry_ = false;
  }
  persistSecurityState();
}

void CredentialVault::resetFailedUnlockAttempts() {
  failedUnlocks_ = 0;
  cooldownExpiryEpochMs_ = 0;
  hasSteadyExpiry_ = false;
  clearPersistedSecurityState();
}

bool CredentialVault::isUnlockRateLimited() const {
  return remainingUnlockCooldownSeconds() > 0;
}

int CredentialVault::remainingUnlockCooldownSeconds() const {
  QMutexLocker locker(&mutex_);
  if (cooldownExpiryEpochMs_ <= 0) {
    return 0;
  }
  if (timeProvider_) {
    const qint64 now = timeProvider_();
    if (now >= cooldownExpiryEpochMs_) {
      return 0;
    }
    const qint64 diffMs = cooldownExpiryEpochMs_ - now;
    return static_cast<int>(std::clamp<qint64>((diffMs + 999) / 1000, 1, 1800));
  }
  if (hasSteadyExpiry_) {
    const auto nowSteady = std::chrono::steady_clock::now();
    if (nowSteady >= cooldownExpirySteady_) {
      return 0;
    }
    const auto remSteady = std::chrono::duration_cast<std::chrono::seconds>(cooldownExpirySteady_ - nowSteady).count();
    return static_cast<int>(std::clamp<qint64>(remSteady, 1, 1800));
  }
  const qint64 nowEpoch = QDateTime::currentMSecsSinceEpoch();
  if (nowEpoch >= cooldownExpiryEpochMs_) {
    return 0;
  }
  const qint64 diffMs = cooldownExpiryEpochMs_ - nowEpoch;
  return static_cast<int>(std::clamp<qint64>((diffMs + 999) / 1000, 1, 1800));
}

int CredentialVault::failedUnlockAttempts() const {
  QMutexLocker locker(&mutex_);
  return failedUnlocks_;
}

void CredentialVault::setTimeProviderForTesting(std::function<qint64()> provider) {
  QMutexLocker locker(&mutex_);
  timeProvider_ = std::move(provider);
  loadSecurityState();
}

QString CredentialVault::canonicalHttpsOrigin(const QUrl &url) {
  if (!url.isValid() || url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) != 0 ||
      url.host().isEmpty() || !url.userName().isEmpty() || !url.password().isEmpty()) {
    return {};
  }
  QString rawHost = url.host().trimmed().toLower();
  while (rawHost.endsWith(QLatin1Char('.'))) {
    rawHost.chop(1);
  }
  if (rawHost.isEmpty()) return {};

  const QByteArray ace = QUrl::toAce(rawHost);
  if (ace.isEmpty()) return {};

  QString canonical = QStringLiteral("https://") + QString::fromLatin1(ace);
  if (url.port() > 0 && url.port() != 443) {
    canonical += QLatin1Char(':') + QString::number(url.port());
  }
  return canonical;
}

bool CredentialVault::isStrongMasterPassword(const QString &p) {
  return p.size() >= 12 && p.size() <= 256 &&
         p.contains(QRegularExpression("[a-z]")) &&
         p.contains(QRegularExpression("[A-Z]")) &&
         p.contains(QRegularExpression("[0-9]")) &&
         p.contains(QRegularExpression("[^A-Za-z0-9\\s]"));
}

bool CredentialVault::exists() const { return QFileInfo::exists(path_); }

bool CredentialVault::hasOrigin(const QString &origin) const {
  QMutexLocker locker(&mutex_);
  if (!exists()) return false;
  const QString canonical = canonicalHttpsOrigin(QUrl(origin));
  if (canonical.isEmpty()) return false;
  if (!isLocked()) {
    for (const Record &r : records_) {
      CredentialSecret s;
      if (decryptRecord(r, &s) && s.origin == canonical) {
        return true;
      }
    }
    return false;
  }
  // The v2 index is keyed by the in-memory data key. While locked, report no
  // origin-specific fact; the manager may offer a generic unlock action and
  // performs the exact-origin lookup only after authentication.
  return !isLocked() && storedOriginHashes_.contains(originToken(canonical));
}

bool CredentialVault::isLocked() const { QMutexLocker locker(&mutex_); return dataKey_.isEmpty(); }
int CredentialVault::autoLockTimeoutMs() const { QMutexLocker locker(&mutex_); return autoLockTimeoutMs_; }

bool CredentialVault::setAutoLockTimeoutMs(int timeoutMs) {
  QMutexLocker locker(&mutex_);
  if (timeoutMs != 60000 && timeoutMs != 5 * 60000 && timeoutMs != 15 * 60000 && timeoutMs != 30 * 60000) return setError(QStringLiteral("invalid-auto-lock-timeout"));
  autoLockTimeoutMs_ = timeoutMs; QSettings().setValue(QStringLiteral("browser/passwords/autoLockMs"), timeoutMs); touch(); return true;
}

QString CredentialVault::lastError() const { QMutexLocker locker(&mutex_); return lastError_; }
bool CredentialVault::setError(const QString &error) const { QMutexLocker locker(&mutex_); lastError_ = error; return false; }
void CredentialVault::secureClear(QByteArray *value) const { if (!value) return; std::fill(value->begin(), value->end(), '\0'); value->clear(); }
void CredentialVault::touch() const { lastActivityMs_ = QDateTime::currentMSecsSinceEpoch(); }

bool CredentialVault::create(const QString &masterPassword) {
  QMutexLocker locker(&mutex_);
  if (exists()) return setError(QStringLiteral("vault-exists"));
  if (!isStrongMasterPassword(masterPassword)) return setError(QStringLiteral("weak-master-password"));

  if (!randomBytes(&salt_, 16)) return setError(QStringLiteral("crypto-failed"));

  QByteArray deviceSecret;
  if (DeviceKeyring::isAvailable()) {
    if (randomBytes(&deviceSecret, kKeyBytes) && DeviceKeyring::storeSecret(vaultKeyringId(), deviceSecret)) {
      deviceBinding_ = QStringLiteral("secret-service");
      cachedDeviceSecret_ = deviceSecret;
    } else {
      secureClear(&deviceSecret);
      deviceBinding_ = QStringLiteral("none");
      cachedDeviceSecret_.clear();
    }
  } else {
    deviceBinding_ = QStringLiteral("none");
    cachedDeviceSecret_.clear();
  }
  schemaVersion_ = kCurrentSchemaVersion;

  QByteArray wrapKey, rawKey;
  if (!deriveKey(masterPassword, salt_, cachedDeviceSecret_, &wrapKey) ||
      !randomBytes(&rawKey, kKeyBytes) ||
      !randomBytes(&wrappedNonce_, kNonceBytes)) {
    secureClear(&wrapKey);
    secureClear(&rawKey);
    secureClear(&deviceSecret);
    return setError(QStringLiteral("crypto-failed"));
  }

  const QByteArray aad = (schemaVersion_ >= 3 && deviceBinding_ == QLatin1String("secret-service"))
      ? QByteArrayLiteral("dalinira-vault-key-v3")
      : QByteArrayLiteral("dalinira-vault-key-v2");

  if (!aesGcm(true, wrapKey, wrappedNonce_, rawKey, aad, &wrappedKey_, &wrappedTag_)) {
    secureClear(&wrapKey);
    secureClear(&rawKey);
    secureClear(&deviceSecret);
    return setError(QStringLiteral("crypto-failed"));
  }
  secureClear(&wrapKey);
  secureClear(&deviceSecret);
  dataKey_ = rawKey;
  records_.clear();
  storedOriginHashes_.clear();
  touch();
  resetFailedUnlockAttempts();
  clearPersistedOriginIndex();
  if (!persist()) {
    lock();
    return false;
  }
  emit lockStateChanged(false);
  emit changed();
  return true;
}

bool CredentialVault::loadEnvelopeFromPath(const QString &candidate) {
    const QFileInfo info(candidate);
    if (!info.isFile() || info.isSymLink() || info.size() <= 0 || info.size() > kMaxVaultBytes) return false;
    QFile file(candidate);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    const int version = root.value(QStringLiteral("schemaVersion")).toInt();
    if (version != 2 && version != 3 && version != 4) {
      if (version > kCurrentSchemaVersion) {
        lastError_ = QStringLiteral("unsupported-vault-version");
      }
      return false;
    }
    if (root.value(QStringLiteral("kdf")).toString() != QLatin1String("PBKDF2-HMAC-SHA256") ||
        root.value(QStringLiteral("iterations")).toInt() != kPbkdf2Iterations) {
      return false;
    }
    const QByteArray salt = fromB64(root.value(QStringLiteral("salt")));
    const QByteArray wrapped = fromB64(root.value(QStringLiteral("wrappedKey")));
    const QByteArray nonce = fromB64(root.value(QStringLiteral("wrappedNonce")));
    const QByteArray tag = fromB64(root.value(QStringLiteral("wrappedTag")));
    const QJsonArray entries = root.value(QStringLiteral("records")).toArray();
    const QByteArray collectionTag = fromB64(root.value(QStringLiteral("collectionTag")));
    if (salt.size() != 16 || nonce.size() != kNonceBytes || tag.size() != kTagBytes || wrapped.isEmpty() ||
        entries.size() > kMaxRecords || (version >= 4 && collectionTag.size() != kKeyBytes)) return false;
    QVector<Record> parsed;
    QSet<QString> ids;
    for (const QJsonValue &value : entries) {
      const QJsonObject o = value.toObject();
      Record r{o.value(QStringLiteral("id")).toString(),
               o.value(QStringLiteral("createdAt")).toVariant().toLongLong(),
               o.value(QStringLiteral("updatedAt")).toVariant().toLongLong(),
               fromB64(o.value(QStringLiteral("ciphertext"))),
               fromB64(o.value(QStringLiteral("nonce"))),
               fromB64(o.value(QStringLiteral("tag")))};
      if (r.id.isEmpty() || ids.contains(r.id) || r.nonce.size() != kNonceBytes || r.tag.size() != kTagBytes || r.encrypted.isEmpty()) return false;
      ids.insert(r.id);
      parsed.append(std::move(r));
    }
    salt_ = salt;
    wrappedKey_ = wrapped;
    wrappedNonce_ = nonce;
    wrappedTag_ = tag;
    collectionTag_ = collectionTag;
    records_ = std::move(parsed);
    schemaVersion_ = version;
    deviceBinding_ = root.value(QStringLiteral("deviceBinding")).toString(QStringLiteral("none"));
    return true;
}

bool CredentialVault::loadEnvelope() {
  if (loadEnvelopeFromPath(path_)) return true;
  if (loadEnvelopeFromPath(path_ + QStringLiteral(".bak"))) {
    lastError_ = QStringLiteral("vault-recovered-from-backup");
    return true;
  }
  return setError(lastError_.isEmpty() ? QStringLiteral("invalid-vault") : lastError_);
}

bool CredentialVault::unlock(const QString &masterPassword) {
  QMutexLocker locker(&mutex_);
  if (isUnlockRateLimited()) return setError(QStringLiteral("unlock-rate-limited"));
  if (!exists() || !loadEnvelope()) return false;

  QByteArray deviceSecret;
  if (schemaVersion_ >= 3 && deviceBinding_ == QLatin1String("secret-service")) {
    if (!obtainDeviceSecret(&deviceSecret)) {
      if (!recoverLegacyDeviceSecret(masterPassword, &deviceSecret)) {
        return setError(QStringLiteral("keyring-unavailable"));
      }
    }
  }

  QByteArray wrapKey, key;
  const QByteArray aad = (schemaVersion_ >= 3 && deviceBinding_ == QLatin1String("secret-service"))
      ? QByteArrayLiteral("dalinira-vault-key-v3")
      : QByteArrayLiteral("dalinira-vault-key-v2");

  const bool ok = deriveKey(masterPassword, salt_, deviceSecret, &wrapKey) &&
                  aesGcm(false, wrapKey, wrappedNonce_, wrappedKey_, aad, &key, &wrappedTag_) &&
                  key.size() == kKeyBytes;
  secureClear(&wrapKey);
  if (!ok) {
    secureClear(&key);
    secureClear(&deviceSecret);
    recordFailedUnlockAttempt();
    return setError(QStringLiteral("invalid-master-password"));
  }

  cachedDeviceSecret_ = deviceSecret;
  secureClear(&deviceSecret);
  secureClear(&dataKey_);
  dataKey_ = key;
  bool recoveredFromBackup = false;
  if (!validateDecryptedCollection()) {
    secureClear(&dataKey_);
    if (!loadEnvelopeFromPath(path_ + QStringLiteral(".bak"))) {
      recordFailedUnlockAttempt();
      return setError(QStringLiteral("corrupt-vault"));
    }
    QByteArray backupDeviceSecret;
    if (schemaVersion_ >= 3 && deviceBinding_ == QLatin1String("secret-service") &&
        !obtainDeviceSecret(&backupDeviceSecret)) {
      return setError(QStringLiteral("keyring-unavailable"));
    }
    QByteArray backupWrapKey, backupKey;
    const QByteArray backupAad = (schemaVersion_ >= 3 && deviceBinding_ == QLatin1String("secret-service"))
        ? QByteArrayLiteral("dalinira-vault-key-v3") : QByteArrayLiteral("dalinira-vault-key-v2");
    const bool backupOk = deriveKey(masterPassword, salt_, backupDeviceSecret, &backupWrapKey) &&
        aesGcm(false, backupWrapKey, wrappedNonce_, wrappedKey_, backupAad, &backupKey, &wrappedTag_) &&
        backupKey.size() == kKeyBytes;
    secureClear(&backupWrapKey);
    secureClear(&backupDeviceSecret);
    if (!backupOk) {
      secureClear(&backupKey);
      recordFailedUnlockAttempt();
      return setError(QStringLiteral("corrupt-vault"));
    }
    dataKey_ = backupKey;
    if (!validateDecryptedCollection()) {
      secureClear(&dataKey_);
      recordFailedUnlockAttempt();
      return setError(QStringLiteral("corrupt-vault"));
    }
    recoveredFromBackup = true;
  }
  resetFailedUnlockAttempts();
  touch();

  storedOriginHashes_.clear();
  for (const Record &r : records_) {
    CredentialSecret s;
    if (decryptRecord(r, &s)) storedOriginHashes_.insert(originToken(s.origin));
  }
  persistOriginIndex();

  // Backward-compatible migration:
  // If this was a legacy schemaVersion 2 vault, or schemaVersion 3 without device binding,
  // and the OS Secret Service is available, seamlessly migrate to schemaVersion 3 with device binding.
  if ((schemaVersion_ < 3 || deviceBinding_ != QLatin1String("secret-service")) && DeviceKeyring::isAvailable()) {
    QByteArray newDevSecret;
    if (randomBytes(&newDevSecret, kKeyBytes) && DeviceKeyring::storeSecret(vaultKeyringId(), newDevSecret)) {
      QByteArray newWrapKey, newNonce, newCipher, newTag;
      if (randomBytes(&newNonce, kNonceBytes) &&
          deriveKey(masterPassword, salt_, newDevSecret, &newWrapKey) &&
          aesGcm(true, newWrapKey, newNonce, dataKey_, QByteArrayLiteral("dalinira-vault-key-v3"), &newCipher, &newTag)) {
        wrappedNonce_ = newNonce;
        wrappedKey_ = newCipher;
        wrappedTag_ = newTag;
        schemaVersion_ = kCurrentSchemaVersion;
        deviceBinding_ = QStringLiteral("secret-service");
        cachedDeviceSecret_ = newDevSecret;
        persist();
      }
      secureClear(&newWrapKey);
      secureClear(&newDevSecret);
    }
  }

  if (schemaVersion_ < kCurrentSchemaVersion || recoveredFromBackup) {
    schemaVersion_ = kCurrentSchemaVersion;
    if (!persist()) {
      secureClear(&dataKey_);
      return false;
    }
  }

  emit lockStateChanged(false);
  return true;
}

void CredentialVault::lock() {
  QMutexLocker locker(&mutex_);
  const bool wasUnlocked = !dataKey_.isEmpty();
  secureClear(&dataKey_);
  secureClear(&cachedDeviceSecret_);
  lastActivityMs_ = 0;
  if (wasUnlocked) emit lockStateChanged(true);
}

bool CredentialVault::encryptRecord(Record *record, const CredentialSecret &secret) {
  QByteArray plain = QJsonDocument(QJsonObject{
      {"origin", secret.origin},
      {"username", secret.username},
      {"password", secret.password}
  }).toJson(QJsonDocument::Compact);
  if (!randomBytes(&record->nonce, kNonceBytes) ||
      !aesGcm(true, dataKey_, record->nonce, plain, recordAad(record->id).toUtf8(), &record->encrypted, &record->tag)) {
    secureClear(&plain);
    return false;
  }
  secureClear(&plain);
  return true;
}

bool CredentialVault::decryptRecord(const Record &record, CredentialSecret *secret) const {
  if (!secret || isLocked()) return false;
  QByteArray plain, tag = record.tag;
  if (!aesGcm(false, dataKey_, record.nonce, record.encrypted, recordAad(record.id).toUtf8(), &plain, &tag)) {
    secureClear(&plain);
    return false;
  }
  const QJsonObject o = QJsonDocument::fromJson(plain).object();
  secureClear(&plain);
  const QUrl origin(o.value(QStringLiteral("origin")).toString());
  const QString canonical = canonicalHttpsOrigin(origin);
  const QString username = o.value(QStringLiteral("username")).toString();
  const QString password = o.value(QStringLiteral("password")).toString();
  if (canonical.isEmpty() || username.isEmpty() || password.isEmpty() || password.size() > 4096) return false;
  *secret = {canonical, username, password, {}};
  return true;
}

QByteArray CredentialVault::collectionAuthenticator() const {
  if (dataKey_.size() != kKeyBytes) return {};
  QJsonArray records;
  for (const Record &r : records_) {
    records.append(QJsonObject{{"id", r.id}, {"createdAt", QString::number(r.createdAt)},
                               {"updatedAt", QString::number(r.updatedAt)},
                               {"ciphertext", QString::fromLatin1(b64(r.encrypted))},
                               {"nonce", QString::fromLatin1(b64(r.nonce))},
                               {"tag", QString::fromLatin1(b64(r.tag))}});
  }
  const QByteArray message = QByteArrayLiteral("dalinira-vault-collection-v1:") +
      QJsonDocument(records).toJson(QJsonDocument::Compact);
  QByteArray digest(kKeyBytes, Qt::Uninitialized);
  unsigned int length = 0;
  if (!HMAC(EVP_sha256(), dataKey_.constData(), dataKey_.size(),
            reinterpret_cast<const unsigned char *>(message.constData()), message.size(),
            reinterpret_cast<unsigned char *>(digest.data()), &length) || length != kKeyBytes) return {};
  return digest;
}

bool CredentialVault::validateDecryptedCollection() const {
  if (dataKey_.size() != kKeyBytes) return false;
  if (schemaVersion_ >= 4) {
    const QByteArray expected = collectionAuthenticator();
    if (expected.size() != kKeyBytes || collectionTag_.size() != kKeyBytes ||
        CRYPTO_memcmp(expected.constData(), collectionTag_.constData(), kKeyBytes) != 0) return false;
  }
  for (const Record &record : records_) {
    CredentialSecret secret;
    if (!decryptRecord(record, &secret)) return false;
  }
  return true;
}

bool CredentialVault::persist() {
  QDir().mkpath(directory_);
  QFile::setPermissions(directory_, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
  QJsonArray records;
  for (const Record &r : records_) {
    records.append(QJsonObject{
        {"id", r.id},
        {"createdAt", QString::number(r.createdAt)},
        {"updatedAt", QString::number(r.updatedAt)},
        {"ciphertext", QString::fromLatin1(b64(r.encrypted))},
        {"nonce", QString::fromLatin1(b64(r.nonce))},
        {"tag", QString::fromLatin1(b64(r.tag))}
    });
  }
  if (schemaVersion_ >= 4) {
    collectionTag_ = collectionAuthenticator();
    if (collectionTag_.size() != kKeyBytes) return setError(QStringLiteral("collection-integrity-failed"));
  }
  const QJsonObject root{
      {"schemaVersion", schemaVersion_},
      {"kdf", "PBKDF2-HMAC-SHA256"},
      {"iterations", kPbkdf2Iterations},
      {"deviceBinding", deviceBinding_},
      {"salt", QString::fromLatin1(b64(salt_))},
      {"wrappedKey", QString::fromLatin1(b64(wrappedKey_))},
      {"wrappedNonce", QString::fromLatin1(b64(wrappedNonce_))},
      {"wrappedTag", QString::fromLatin1(b64(wrappedTag_))},
      {"collectionTag", QString::fromLatin1(b64(collectionTag_))},
      {"records", records}
  };

  QSaveFile file(path_);
  const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Compact);
  if (!file.open(QIODevice::WriteOnly) ||
      !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
      file.write(bytes) != bytes.size() || !file.commit()) {
    return setError(QStringLiteral("write-failed"));
  }
  QFile::setPermissions(path_, QFileDevice::ReadOwner | QFileDevice::WriteOwner);

  // Successfully committed primary vault; safely update backup
  QFile::remove(path_ + QStringLiteral(".bak"));
  if (QFile::copy(path_, path_ + QStringLiteral(".bak"))) {
    QFile::setPermissions(path_ + QStringLiteral(".bak"), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  }
  return true;
}

bool CredentialVault::save(const CredentialSecret &input, bool *updated) {
  QMutexLocker locker(&mutex_);
  if (isLocked()) return setError(QStringLiteral("vault-locked"));
  const QString origin = canonicalHttpsOrigin(QUrl(input.origin));
  const QString username = input.username.trimmed();
  if (origin.isEmpty() || username.isEmpty() || input.password.isEmpty() || input.password.size() > 4096) return setError(QStringLiteral("invalid-credential"));
  Record *target = nullptr;
  for (Record &r : records_) {
    CredentialSecret old;
    if (!decryptRecord(r, &old)) return setError(QStringLiteral("corrupt-vault"));
    if (old.origin == origin && old.username == username) { target = &r; break; }
  }
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  const bool isUpdate = target != nullptr;
  if (!target) {
    if (records_.size() >= kMaxRecords) return setError(QStringLiteral("vault-full"));
    Record added;
    added.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    added.createdAt = now;
    added.updatedAt = now;
    records_.append(std::move(added));
    target = &records_.last();
  }
  target->updatedAt = now;
  if (!encryptRecord(target, {origin, username, input.password, {}}) || !persist()) return setError(QStringLiteral("write-failed"));
  storedOriginHashes_.insert(originToken(origin));
  persistOriginIndex();
  if (updated) *updated = isUpdate;
  touch();
  emit changed();
  return true;
}

bool CredentialVault::update(const QString &id, const CredentialSecret &input) {
  QMutexLocker locker(&mutex_);
  if (isLocked()) return setError(QStringLiteral("vault-locked"));
  const QString origin = canonicalHttpsOrigin(QUrl(input.origin));
  const QString username = input.username.trimmed();
  if (origin.isEmpty() || username.isEmpty() || input.password.isEmpty() || input.password.size() > 4096) return setError(QStringLiteral("invalid-credential"));
  Record *target = nullptr;
  for (Record &record : records_) {
    CredentialSecret current;
    if (!decryptRecord(record, &current)) return setError(QStringLiteral("corrupt-vault"));
    if (record.id == id) { target = &record; }
    if (record.id != id && current.origin == origin && current.username == username) return setError(QStringLiteral("duplicate-credential"));
  }
  if (!target) return setError(QStringLiteral("record-not-found"));
  target->updatedAt = QDateTime::currentMSecsSinceEpoch();
  if (!encryptRecord(target, {origin, username, input.password, {}}) || !persist()) return setError(QStringLiteral("write-failed"));
  storedOriginHashes_.clear();
  for (const Record &r : records_) {
    CredentialSecret s;
    if (decryptRecord(r, &s)) storedOriginHashes_.insert(originToken(s.origin));
  }
  persistOriginIndex();
  touch();
  emit changed();
  return true;
}

bool CredentialVault::changeMasterPassword(const QString &currentPassword, const QString &nextPassword) {
  QMutexLocker locker(&mutex_);
  if (isLocked()) return setError(QStringLiteral("vault-locked"));
  if (!isStrongMasterPassword(nextPassword)) return setError(QStringLiteral("weak-master-password"));

  QByteArray devSecret;
  if (schemaVersion_ >= 3 && deviceBinding_ == QLatin1String("secret-service")) {
    if (!obtainDeviceSecret(&devSecret)) {
      return setError(QStringLiteral("keyring-unavailable"));
    }
  }

  const QByteArray aad = (schemaVersion_ >= 3 && deviceBinding_ == QLatin1String("secret-service"))
      ? QByteArrayLiteral("dalinira-vault-key-v3")
      : QByteArrayLiteral("dalinira-vault-key-v2");

  QByteArray oldWrap, verifiedKey, newSalt, newWrap, newNonce, newCipher, newTag;
  const bool verified = deriveKey(currentPassword, salt_, devSecret, &oldWrap)
      && aesGcm(false, oldWrap, wrappedNonce_, wrappedKey_, aad, &verifiedKey, &wrappedTag_)
      && verifiedKey.size() == dataKey_.size()
      && CRYPTO_memcmp(verifiedKey.constData(), dataKey_.constData(), size_t(dataKey_.size())) == 0;
  secureClear(&oldWrap);
  secureClear(&verifiedKey);
  if (!verified || !randomBytes(&newSalt, 16) ||
      !deriveKey(nextPassword, newSalt, devSecret, &newWrap) ||
      !randomBytes(&newNonce, kNonceBytes) ||
      !aesGcm(true, newWrap, newNonce, dataKey_, aad, &newCipher, &newTag)) {
    secureClear(&devSecret);
    secureClear(&newSalt);
    secureClear(&newWrap);
    secureClear(&newNonce);
    secureClear(&newCipher);
    secureClear(&newTag);
    return setError(QStringLiteral("invalid-master-password"));
  }
  secureClear(&newWrap);

  if (schemaVersion_ >= 3 && deviceBinding_ == QLatin1String("secret-service") && !devSecret.isEmpty()) {
    const QString oldKeyringId = vaultKeyringId();
    salt_ = newSalt;
    DeviceKeyring::storeSecret(vaultKeyringId(), devSecret);
    if (oldKeyringId != vaultKeyringId()) {
      DeviceKeyring::clearSecret(oldKeyringId);
    }
  } else {
    salt_ = newSalt;
  }
  secureClear(&devSecret);

  wrappedNonce_ = newNonce;
  wrappedKey_ = newCipher;
  wrappedTag_ = newTag;
  if (!persist()) return false;
  touch();
  emit changed();
  return true;
}

QVector<CredentialMetadata> CredentialVault::list() const {
  QMutexLocker locker(&mutex_);
  QVector<CredentialMetadata> result;
  if (isLocked()) return result;
  for (const Record &r : records_) {
    CredentialSecret s;
    if (!decryptRecord(r, &s)) {
      setError(QStringLiteral("corrupt-vault"));
      return {};
    }
    result.append({r.id, s.origin, s.username, QDateTime::fromMSecsSinceEpoch(r.createdAt), QDateTime::fromMSecsSinceEpoch(r.updatedAt), s.iconPngBase64, {}, {}});
  }
  touch();
  return result;
}

bool CredentialVault::reveal(const QString &id, CredentialSecret *secret) const {
  QMutexLocker locker(&mutex_);
  if (isLocked()) return setError(QStringLiteral("vault-locked"));
  for (const Record &r : records_) {
    if (r.id == id) {
      if (!decryptRecord(r, secret)) return setError(QStringLiteral("corrupt-vault"));
      touch();
      return true;
    }
  }
  return setError(QStringLiteral("record-not-found"));
}

bool CredentialVault::remove(const QString &id) {
  QMutexLocker locker(&mutex_);
  if (isLocked()) return setError(QStringLiteral("vault-locked"));
  for (int i = 0; i < records_.size(); ++i) {
    if (records_[i].id == id) {
      records_.removeAt(i);
      storedOriginHashes_.clear();
      for (const Record &r : records_) {
        CredentialSecret s;
        if (decryptRecord(r, &s)) storedOriginHashes_.insert(originToken(s.origin));
      }
      persistOriginIndex();
      if (!persist()) return false;
      emit changed();
      return true;
    }
  }
  return setError(QStringLiteral("record-not-found"));
}

bool CredentialVault::reset() {
  QMutexLocker locker(&mutex_);
  lock();
  if (!salt_.isEmpty()) {
    DeviceKeyring::clearSecret(vaultKeyringId());
  }
  cachedDeviceSecret_.clear();
  records_.clear();
  storedOriginHashes_.clear();
  secureClear(&salt_);
  secureClear(&wrappedKey_);
  secureClear(&wrappedNonce_);
  secureClear(&wrappedTag_);
  resetFailedUnlockAttempts();
  clearPersistedOriginIndex();
  const bool primary = !QFile::exists(path_) || QFile::remove(path_);
  const bool backup = !QFile::exists(path_ + QStringLiteral(".bak")) || QFile::remove(path_ + QStringLiteral(".bak"));
  if (!primary || !backup) return setError(QStringLiteral("reset-failed"));
  emit changed();
  return true;
}

QVector<CredentialMetadata> CredentialVault::forOrigin(const QUrl &url) const {
  QMutexLocker locker(&mutex_);
  const QString origin = canonicalHttpsOrigin(url);
  QVector<CredentialMetadata> result;
  if (origin.isEmpty()) return result;
  for (const CredentialMetadata &meta : list()) {
    if (meta.origin == origin) result.append(meta);
  }
  return result;
}
