#include "credential_vault_manager.h"

#include <QDir>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUuid>

#include <algorithm>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include "i18n/i18n.h"

namespace {
constexpr int kMaxVaults = 32;
constexpr int kMaxVaultNameLength = 64;
constexpr int kBackupIterations = 600000;
constexpr int kBackupKeyBytes = 32;
constexpr int kBackupNonceBytes = 12;
constexpr int kBackupTagBytes = 16;
constexpr qint64 kMaxBackupBytes = 64 * 1024 * 1024;
constexpr qint64 kSensitiveAuthorizationMs = 60 * 1000;
QString normalizedName(const QString &name) { return name.simplified().left(kMaxVaultNameLength); }
QByteArray encoded(const QByteArray &value) { return value.toBase64(); }
QByteArray decoded(const QJsonValue &value) { return QByteArray::fromBase64(value.toString().toLatin1()); }
bool randomBytes(QByteArray *out, int size) { out->resize(size); return RAND_bytes(reinterpret_cast<unsigned char *>(out->data()), size) == 1; }
bool backupKey(const QString &password, const QByteArray &salt, QByteArray *key) { const QByteArray value = password.toUtf8(); key->resize(kBackupKeyBytes); return PKCS5_PBKDF2_HMAC(value.constData(), value.size(), reinterpret_cast<const unsigned char *>(salt.constData()), salt.size(), kBackupIterations, EVP_sha256(), kBackupKeyBytes, reinterpret_cast<unsigned char *>(key->data())) == 1; }
void wipe(QByteArray *value) { if (!value) return; std::fill(value->begin(), value->end(), '\0'); value->clear(); }
bool backupCipher(bool encrypt, const QByteArray &key, const QByteArray &nonce, const QByteArray &input, QByteArray *output, QByteArray *tag) {
  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new(); if (!ctx) return false; int size = 0, finalSize = 0; bool ok = false; output->resize(input.size() + kBackupTagBytes);
  do { if (EVP_CipherInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr, encrypt ? 1 : 0) != 1) break; if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, nonce.size(), nullptr) != 1) break; if (EVP_CipherInit_ex(ctx, nullptr, nullptr, reinterpret_cast<const unsigned char *>(key.constData()), reinterpret_cast<const unsigned char *>(nonce.constData()), -1) != 1) break; if (EVP_CipherUpdate(ctx, reinterpret_cast<unsigned char *>(output->data()), &size, reinterpret_cast<const unsigned char *>(input.constData()), input.size()) != 1) break; if (!encrypt && (tag->size() != kBackupTagBytes || EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, kBackupTagBytes, tag->data()) != 1)) break; if (EVP_CipherFinal_ex(ctx, reinterpret_cast<unsigned char *>(output->data()) + size, &finalSize) != 1) break; output->resize(size + finalSize); if (encrypt) { tag->resize(kBackupTagBytes); if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, kBackupTagBytes, tag->data()) != 1) break; } ok = true; } while (false);
  EVP_CIPHER_CTX_free(ctx); return ok;
}
bool validEnvelope(const QByteArray &bytes) { const QJsonObject root = QJsonDocument::fromJson(bytes).object(); const int ver = root.value(QStringLiteral("schemaVersion")).toInt(); return (ver == 2 || ver == 3 || ver == 4) && root.value(QStringLiteral("kdf")).toString() == QLatin1String("PBKDF2-HMAC-SHA256") && root.value(QStringLiteral("records")).isArray(); }
}

CredentialVaultManager::CredentialVaultManager(const QString &dataDirectory, QObject *parent)
    : QObject(parent), dataDirectory_(dataDirectory), indexPath_(dataDirectory + QStringLiteral("/credential-vault/vault-index.json")) {
  loadIndex();
}

bool CredentialVaultManager::setError(const QString &error) const { lastError_ = error; return false; }
CredentialVaultManager::Entry *CredentialVaultManager::entry(const QString &id) { for (Entry &candidate : entries_) if (candidate.id == id) return &candidate; return nullptr; }
const CredentialVaultManager::Entry *CredentialVaultManager::entry(const QString &id) const { for (const Entry &candidate : entries_) if (candidate.id == id) return &candidate; return nullptr; }
CredentialVaultManager::Entry *CredentialVaultManager::activeEntry() { return entry(activeId_); }
const CredentialVaultManager::Entry *CredentialVaultManager::activeEntry() const { return entry(activeId_); }
QString CredentialVaultManager::namespacedId(const QString &vaultId, const QString &recordId) { return vaultId + QLatin1Char(':') + recordId; }
bool CredentialVaultManager::splitId(const QString &id, QString *vaultId, QString *recordId) { const int colon = id.indexOf(QLatin1Char(':')); if (colon <= 0 || colon == id.size() - 1) return false; if (vaultId) *vaultId = id.left(colon); if (recordId) *recordId = id.mid(colon + 1); return true; }

void CredentialVaultManager::attach(Entry *candidate) {
  if (!candidate || !candidate->vault) return;
  connect(candidate->vault, &CredentialVault::changed, this, [this] { emit changed(); });
  const QString vaultId = candidate->id;
  connect(candidate->vault, &CredentialVault::lockStateChanged, this, [this, vaultId](bool locked) {
    if (locked) clearSensitiveAuthorization(vaultId);
    emit lockStateChanged(isLocked());
    emit changed();
  });
}

bool CredentialVaultManager::loadIndex() {
  entries_.clear(); activeId_.clear();
  const auto add = [this](const QString &id, const QString &name, bool legacy) {
    if (id.isEmpty() || name.isEmpty() || entries_.size() >= kMaxVaults) return false;
    auto *vault = new CredentialVault(dataDirectory_, this, legacy ? QString{} : id);
    if (timeProvider_) vault->setTimeProviderForTesting(timeProvider_);
    entries_.append({id, name, vault, legacy}); attach(&entries_.last()); return true;
  };
  QFile file(indexPath_);
  if (file.exists()) {
    if (!file.open(QIODevice::ReadOnly)) return setError(QStringLiteral("vault-index-read-failed"));
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    if (root.value(QStringLiteral("schemaVersion")).toInt() != 1) return setError(QStringLiteral("invalid-vault-index"));
    for (const QJsonValue &value : root.value(QStringLiteral("vaults")).toArray()) {
      const QJsonObject object = value.toObject(); const QString id = object.value(QStringLiteral("id")).toString(); const QString name = normalizedName(object.value(QStringLiteral("name")).toString()); const bool legacy = object.value(QStringLiteral("legacyStorage")).toBool(false);
      if (!id.contains(QRegularExpression(QStringLiteral("^[a-f0-9-]{36}$"))) || name.isEmpty() || !add(id, name, legacy)) return setError(QStringLiteral("invalid-vault-index"));
    }
    activeId_ = root.value(QStringLiteral("activeVaultId")).toString();
    if (!activeEntry() && !entries_.isEmpty()) activeId_ = entries_.front().id;
    return true;
  }
  // Non-destructive migration: the pre-multi-vault file becomes Kişisel.
  if (QFileInfo::exists(dataDirectory_ + QStringLiteral("/credential-vault/vault-v2.json"))) {
    const QString legacyId = QStringLiteral("00000000-0000-0000-0000-000000000001");
    if (!add(legacyId, I18n::text(QStringLiteral("passwords.default_vault_name")), true)) return false;
    activeId_ = legacyId;
    return persistIndex();
  }
  return true;
}

bool CredentialVaultManager::persistIndex() {
  const QString directory = QFileInfo(indexPath_).absolutePath(); QDir().mkpath(directory); QFile::setPermissions(directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
  QJsonArray vaults;
  for (const Entry &candidate : entries_) vaults.append(QJsonObject{{QStringLiteral("id"), candidate.id}, {QStringLiteral("name"), candidate.name}, {QStringLiteral("legacyStorage"), candidate.legacyStorage}});
  QSaveFile file(indexPath_); const QByteArray bytes = QJsonDocument(QJsonObject{{QStringLiteral("schemaVersion"), 1}, {QStringLiteral("activeVaultId"), activeId_}, {QStringLiteral("vaults"), vaults}}).toJson(QJsonDocument::Compact);
  if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) || file.write(bytes) != bytes.size() || !file.commit()) return setError(QStringLiteral("vault-index-write-failed"));
  QFile::setPermissions(indexPath_, QFileDevice::ReadOwner | QFileDevice::WriteOwner); return true;
}

QVector<VaultMetadata> CredentialVaultManager::vaults() const { QVector<VaultMetadata> result; result.reserve(entries_.size()); for (const Entry &candidate : entries_) result.append({candidate.id, candidate.name, candidate.vault->isLocked()}); return result; }
QString CredentialVaultManager::activeVaultId() const { return activeId_; }
bool CredentialVaultManager::setActiveVault(const QString &id) { if (!entry(id)) return setError(QStringLiteral("vault-not-found")); activeId_ = id; if (!persistIndex()) return false; emit changed(); return true; }
bool CredentialVaultManager::exists() const {
  if (entries_.isEmpty()) return false;
  for (const Entry &candidate : entries_) {
    if (candidate.vault && candidate.vault->exists()) return true;
  }
  return false;
}
bool CredentialVaultManager::isLocked() const { const Entry *candidate = activeEntry(); return !candidate || candidate->vault->isLocked(); }
bool CredentialVaultManager::isVaultLocked(const QString &id) const { const Entry *candidate = entry(id); return !candidate || candidate->vault->isLocked(); }
bool CredentialVaultManager::isActiveVaultDeviceBound() const { const Entry *candidate = activeEntry(); return candidate && candidate->vault && candidate->vault->isDeviceBound(); }
bool CredentialVaultManager::authorizeSensitiveAction(const QString &vaultId, const QString &masterPassword) {
  if (vaultId.isEmpty() || masterPassword.isEmpty() || !unlockVault(vaultId, masterPassword)) {
    sensitiveAuthorizationExpiry_.remove(vaultId);
    return false;
  }
  const qint64 now = timeProvider_ ? timeProvider_() : QDateTime::currentMSecsSinceEpoch();
  sensitiveAuthorizationExpiry_.insert(vaultId, now + kSensitiveAuthorizationMs);
  return true;
}
bool CredentialVaultManager::hasSensitiveAuthorization(const QString &vaultId) const {
  const Entry *candidate = entry(vaultId);
  if (!candidate || !candidate->vault || candidate->vault->isLocked()) return false;
  const qint64 now = timeProvider_ ? timeProvider_() : QDateTime::currentMSecsSinceEpoch();
  return sensitiveAuthorizationExpiry_.value(vaultId, 0) > now;
}
void CredentialVaultManager::clearSensitiveAuthorization(const QString &vaultId) {
  if (vaultId.isEmpty()) sensitiveAuthorizationExpiry_.clear();
  else sensitiveAuthorizationExpiry_.remove(vaultId);
}
QString CredentialVaultManager::lastError() const { if (!lastError_.isEmpty()) return lastError_; const Entry *candidate = activeEntry(); return candidate ? candidate->vault->lastError() : QStringLiteral("vault-not-found"); }
int CredentialVaultManager::autoLockTimeoutMs() const { const Entry *candidate = activeEntry(); return candidate ? candidate->vault->autoLockTimeoutMs() : 5 * 60000; }
bool CredentialVaultManager::setAutoLockTimeoutMs(int timeoutMs) { Entry *candidate = activeEntry(); return candidate ? candidate->vault->setAutoLockTimeoutMs(timeoutMs) : setError(QStringLiteral("vault-not-found")); }

bool CredentialVaultManager::isUnlockRateLimited() const {
  const Entry *candidate = activeEntry();
  return candidate && candidate->vault ? candidate->vault->isUnlockRateLimited() : false;
}

bool CredentialVaultManager::isVaultUnlockRateLimited(const QString &vaultId) const {
  const Entry *candidate = entry(vaultId);
  return candidate && candidate->vault ? candidate->vault->isUnlockRateLimited() : false;
}

int CredentialVaultManager::remainingUnlockCooldownSeconds() const {
  const Entry *candidate = activeEntry();
  return candidate && candidate->vault ? candidate->vault->remainingUnlockCooldownSeconds() : 0;
}

int CredentialVaultManager::remainingVaultUnlockCooldownSeconds(const QString &vaultId) const {
  const Entry *candidate = entry(vaultId);
  return candidate && candidate->vault ? candidate->vault->remainingUnlockCooldownSeconds() : 0;
}

int CredentialVaultManager::failedUnlockAttempts() const {
  const Entry *candidate = activeEntry();
  return candidate && candidate->vault ? candidate->vault->failedUnlockAttempts() : 0;
}

void CredentialVaultManager::resetFailedUnlockAttempts() {
  Entry *candidate = activeEntry();
  if (candidate && candidate->vault) {
    candidate->vault->resetFailedUnlockAttempts();
  }
}

void CredentialVaultManager::setTimeProviderForTesting(std::function<qint64()> provider) {
  timeProvider_ = provider;
  for (Entry &candidate : entries_) {
    if (candidate.vault) {
      candidate.vault->setTimeProviderForTesting(provider);
    }
  }
}

bool CredentialVaultManager::create(const QString &masterPassword) { return createVault(I18n::text(QStringLiteral("passwords.default_vault_name")), masterPassword); }
bool CredentialVaultManager::createVault(const QString &rawName, const QString &masterPassword, QString *id) {
  const QString name = normalizedName(rawName); if (name.isEmpty() || entries_.size() >= kMaxVaults) return setError(QStringLiteral("invalid-vault-name"));
  for (const Entry &candidate : entries_) if (candidate.name.compare(name, Qt::CaseInsensitive) == 0) return setError(QStringLiteral("duplicate-vault-name"));
  const QString vaultId = QUuid::createUuid().toString(QUuid::WithoutBraces); auto *vault = new CredentialVault(dataDirectory_, this, vaultId);
  if (timeProvider_) vault->setTimeProviderForTesting(timeProvider_);
  if (!vault->create(masterPassword)) { lastError_ = vault->lastError(); vault->deleteLater(); return false; }
  entries_.append({vaultId, name, vault, false}); attach(&entries_.last()); activeId_ = vaultId;
  if (!persistIndex()) { vault->reset(); entries_.removeLast(); return false; }
  if (id) *id = vaultId;
  emit vaultCreated(vaultId);
  emit lockStateChanged(false); emit changed(); return true;
}
bool CredentialVaultManager::unlock(const QString &masterPassword) { return unlockVault(activeId_, masterPassword); }
bool CredentialVaultManager::unlockVault(const QString &id, const QString &masterPassword) { Entry *candidate = entry(id); if (!candidate) return setError(QStringLiteral("vault-not-found")); const bool ok = candidate->vault->unlock(masterPassword); if (!ok) lastError_ = candidate->vault->lastError(); return ok; }
QString CredentialVaultManager::owningVaultId(const QString &recordId) const { QString vaultId, localId; if (splitId(recordId, &vaultId, &localId) && entry(vaultId)) return vaultId; return entry(activeId_) ? activeId_ : QString{}; }
void CredentialVaultManager::lock() { clearSensitiveAuthorization(); for (Entry &candidate : entries_) candidate.vault->lock(); }
void CredentialVaultManager::lockVault(const QString &id) { clearSensitiveAuthorization(id); if (Entry *candidate = entry(id)) candidate->vault->lock(); }
bool CredentialVaultManager::deleteVault(const QString &id) { Entry *candidate = entry(id); if (!candidate) return setError(QStringLiteral("vault-not-found")); if (!candidate->vault->reset()) { lastError_ = candidate->vault->lastError(); return false; } clearSensitiveAuthorization(id); const int index = int(candidate - entries_.data()); candidate->vault->deleteLater(); entries_.removeAt(index); if (activeId_ == id) activeId_ = entries_.isEmpty() ? QString{} : entries_.front().id; if (!persistIndex()) return false; emit changed(); return true; }
bool CredentialVaultManager::clearActiveVault() { Entry *candidate = activeEntry(); if (!candidate || candidate->vault->isLocked()) return setError(QStringLiteral("vault-locked")); for (const CredentialMetadata &record : candidate->vault->list()) if (!candidate->vault->remove(record.id)) { lastError_ = candidate->vault->lastError(); return false; } return true; }
bool CredentialVaultManager::exportBackup(const QString &filePath, const QString &backupPassword) const {
  if (filePath.isEmpty() || !CredentialVault::isStrongMasterPassword(backupPassword)) return setError(QStringLiteral("weak-backup-password"));
  QJsonArray vaults;
  for (const Entry &candidate : entries_) {
    const QString source = candidate.legacyStorage ? dataDirectory_ + QStringLiteral("/credential-vault/vault-v2.json") : dataDirectory_ + QStringLiteral("/credential-vault/vaults/") + candidate.id + QStringLiteral("/vault-v2.json");
    QFile file(source); if (!file.open(QIODevice::ReadOnly)) return setError(QStringLiteral("backup-read-failed")); const QByteArray bytes = file.readAll();
    if (bytes.isEmpty() || bytes.size() > 16 * 1024 * 1024 || !validEnvelope(bytes)) return setError(QStringLiteral("invalid-vault"));
    QJsonObject vaultObj{{QStringLiteral("name"), candidate.name}, {QStringLiteral("vault"), QString::fromLatin1(encoded(bytes))}};
    if (candidate.vault) {
      QByteArray devSecret;
      if (!candidate.vault->exportDeviceSecret(&devSecret)) {
        wipe(&devSecret);
        return setError(QStringLiteral("backup-device-secret-unavailable"));
      }
      if (!devSecret.isEmpty()) vaultObj.insert(QStringLiteral("deviceSecret"), QString::fromLatin1(encoded(devSecret)));
      wipe(&devSecret);
    }
    vaults.append(vaultObj);
  }
  QByteArray plain = QJsonDocument(QJsonObject{{QStringLiteral("schemaVersion"), 1}, {QStringLiteral("vaults"), vaults}}).toJson(QJsonDocument::Compact), salt, nonce, key, ciphertext, tag;
  const bool ok = randomBytes(&salt, 16) && randomBytes(&nonce, kBackupNonceBytes) && backupKey(backupPassword, salt, &key) && backupCipher(true, key, nonce, plain, &ciphertext, &tag);
  wipe(&plain); wipe(&key); if (!ok) { wipe(&ciphertext); return setError(QStringLiteral("backup-crypto-failed")); }
  const QJsonObject archive{{QStringLiteral("format"), QStringLiteral("DaliNira encrypted vault backup")}, {QStringLiteral("schemaVersion"), 1}, {QStringLiteral("kdf"), QStringLiteral("PBKDF2-HMAC-SHA256")}, {QStringLiteral("iterations"), kBackupIterations}, {QStringLiteral("salt"), QString::fromLatin1(encoded(salt))}, {QStringLiteral("nonce"), QString::fromLatin1(encoded(nonce))}, {QStringLiteral("tag"), QString::fromLatin1(encoded(tag))}, {QStringLiteral("ciphertext"), QString::fromLatin1(encoded(ciphertext))}};
  QSaveFile target(filePath); const QByteArray output = QJsonDocument(archive).toJson(QJsonDocument::Compact); const bool written = target.open(QIODevice::WriteOnly) && target.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) && target.write(output) == output.size() && target.commit(); QFile::setPermissions(filePath, QFileDevice::ReadOwner | QFileDevice::WriteOwner); wipe(&ciphertext); wipe(&tag); return written ? true : setError(QStringLiteral("backup-write-failed"));
}
bool CredentialVaultManager::importBackup(const QString &filePath, const QString &backupPassword, QStringList *importedVaultNames) {
  if (importedVaultNames) importedVaultNames->clear();
  if (!CredentialVault::isStrongMasterPassword(backupPassword)) return setError(QStringLiteral("weak-backup-password"));
  QFile source(filePath); if (!source.open(QIODevice::ReadOnly) || source.size() <= 0 || source.size() > kMaxBackupBytes) return setError(QStringLiteral("backup-read-failed"));
  const QJsonObject archive = QJsonDocument::fromJson(source.readAll()).object(); if (archive.value(QStringLiteral("format")).toString() != QLatin1String("DaliNira encrypted vault backup") || archive.value(QStringLiteral("schemaVersion")).toInt() != 1 || archive.value(QStringLiteral("kdf")).toString() != QLatin1String("PBKDF2-HMAC-SHA256") || archive.value(QStringLiteral("iterations")).toInt() != kBackupIterations) return setError(QStringLiteral("invalid-backup"));
  QByteArray salt = decoded(archive.value(QStringLiteral("salt"))), nonce = decoded(archive.value(QStringLiteral("nonce"))), tag = decoded(archive.value(QStringLiteral("tag"))), ciphertext = decoded(archive.value(QStringLiteral("ciphertext"))), key, plain;
  const bool decrypted = salt.size() == 16 && nonce.size() == kBackupNonceBytes && tag.size() == kBackupTagBytes && !ciphertext.isEmpty() && ciphertext.size() <= kMaxBackupBytes && backupKey(backupPassword, salt, &key) && backupCipher(false, key, nonce, ciphertext, &plain, &tag); wipe(&key); wipe(&ciphertext); if (!decrypted) { wipe(&plain); return setError(QStringLiteral("invalid-backup-password-or-file")); }
  const QJsonObject payload = QJsonDocument::fromJson(plain).object(); wipe(&plain); const QJsonArray vaults = payload.value(QStringLiteral("vaults")).toArray(); if (payload.value(QStringLiteral("schemaVersion")).toInt() != 1 || vaults.isEmpty() || vaults.size() + entries_.size() > kMaxVaults) return setError(QStringLiteral("invalid-backup"));
  QVector<Entry> imported;
  for (const QJsonValue &value : vaults) {
    const QJsonObject object = value.toObject(); const QString name = normalizedName(object.value(QStringLiteral("name")).toString()); const QByteArray bytes = decoded(object.value(QStringLiteral("vault")));
    QByteArray devSecret = decoded(object.value(QStringLiteral("deviceSecret")));
    bool duplicate = name.isEmpty(); for (const Entry &candidate : entries_) duplicate = duplicate || candidate.name.compare(name, Qt::CaseInsensitive) == 0; for (const Entry &candidate : imported) duplicate = duplicate || candidate.name.compare(name, Qt::CaseInsensitive) == 0;
    if (duplicate || bytes.isEmpty() || bytes.size() > 16 * 1024 * 1024 || !validEnvelope(bytes)) continue;
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces); const QString path = dataDirectory_ + QStringLiteral("/credential-vault/vaults/") + id; QDir().mkpath(path); QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner); QSaveFile target(path + QStringLiteral("/vault-v2.json")); if (!target.open(QIODevice::WriteOnly) || !target.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) || target.write(bytes) != bytes.size() || !target.commit()) { for (const Entry &created : imported) QDir(dataDirectory_ + QStringLiteral("/credential-vault/vaults/") + created.id).removeRecursively(); return setError(QStringLiteral("backup-import-write-failed")); } QFile::setPermissions(path + QStringLiteral("/vault-v2.json"), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    auto *vault = new CredentialVault(dataDirectory_, this, id);
    const QJsonObject importedEnvelope = QJsonDocument::fromJson(bytes).object();
    const bool deviceBound = importedEnvelope.value(QStringLiteral("schemaVersion")).toInt() >= 3 &&
        importedEnvelope.value(QStringLiteral("deviceBinding")).toString() == QLatin1String("secret-service");
    if (deviceBound && (devSecret.size() != kBackupKeyBytes || !vault->importDeviceSecret(devSecret))) {
      wipe(&devSecret);
      vault->deleteLater();
      QDir(path).removeRecursively();
      for (const Entry &created : imported) {
        if (created.vault) created.vault->reset();
        QDir(dataDirectory_ + QStringLiteral("/credential-vault/vaults/") + created.id).removeRecursively();
      }
      return setError(QStringLiteral("backup-device-secret-unavailable"));
    }
    wipe(&devSecret);
    if (timeProvider_) vault->setTimeProviderForTesting(timeProvider_);
    imported.append({id, name, vault, false});
  }
  if (imported.isEmpty()) return setError(QStringLiteral("backup-no-new-vaults"));
  for (Entry &candidate : imported) { entries_.append(candidate); attach(&entries_.last()); if (importedVaultNames) importedVaultNames->append(candidate.name); }
  if (!activeEntry()) activeId_ = entries_.front().id;
  if (!persistIndex()) return false;
  emit changed(); return true;
}
bool CredentialVaultManager::save(const CredentialSecret &secret, bool *updated) { return saveToVault(activeId_, secret, updated); }
bool CredentialVaultManager::saveToVault(const QString &id, const CredentialSecret &secret, bool *updated) { Entry *candidate = entry(id); if (!candidate) return setError(QStringLiteral("vault-not-found")); const bool ok = candidate->vault->save(secret, updated); if (!ok) lastError_ = candidate->vault->lastError(); return ok; }
bool CredentialVaultManager::update(const QString &id, const CredentialSecret &secret) { QString vaultId, recordId; if (!splitId(id, &vaultId, &recordId)) { vaultId = activeId_; recordId = id; } Entry *candidate = entry(vaultId); if (!candidate) return setError(QStringLiteral("vault-not-found")); const bool ok = candidate->vault->update(recordId, secret); if (!ok) lastError_ = candidate->vault->lastError(); return ok; }
bool CredentialVaultManager::changeMasterPassword(const QString &currentPassword, const QString &nextPassword) { Entry *candidate = activeEntry(); if (!candidate) return setError(QStringLiteral("vault-not-found")); const bool ok = candidate->vault->changeMasterPassword(currentPassword, nextPassword); if (!ok) lastError_ = candidate->vault->lastError(); return ok; }
bool CredentialVaultManager::remove(const QString &id) { QString vaultId, recordId; if (!splitId(id, &vaultId, &recordId)) { vaultId = activeId_; recordId = id; } Entry *candidate = entry(vaultId); if (!candidate) return setError(QStringLiteral("vault-not-found")); const bool ok = candidate->vault->remove(recordId); if (!ok) lastError_ = candidate->vault->lastError(); return ok; }
bool CredentialVaultManager::reset() { return deleteVault(activeId_); }
QVector<CredentialMetadata> CredentialVaultManager::list() const { const Entry *candidate = activeEntry(); if (!candidate) return {}; QVector<CredentialMetadata> result = candidate->vault->list(); for (CredentialMetadata &record : result) { record.id = namespacedId(candidate->id, record.id); record.vaultId = candidate->id; record.vaultName = candidate->name; } return result; }
bool CredentialVaultManager::reveal(const QString &id, CredentialSecret *secret) const { QString vaultId, recordId; if (!splitId(id, &vaultId, &recordId)) { vaultId = activeId_; recordId = id; } const Entry *candidate = entry(vaultId); if (!candidate) return setError(QStringLiteral("vault-not-found")); const bool ok = candidate->vault->reveal(recordId, secret); if (!ok) lastError_ = candidate->vault->lastError(); return ok; }
QVector<CredentialMetadata> CredentialVaultManager::forOrigin(const QUrl &url) const { QVector<CredentialMetadata> result; const QString origin = canonicalHttpsOrigin(url); if (origin.isEmpty()) return result; for (const Entry &candidate : entries_) { if (candidate.vault->isLocked()) continue; for (CredentialMetadata record : candidate.vault->forOrigin(url)) { record.id = namespacedId(candidate.id, record.id); record.vaultId = candidate.id; record.vaultName = candidate.name; result.append(std::move(record)); } } return result; }
QVector<VaultMetadata> CredentialVaultManager::vaultsForOrigin(const QUrl &url) const {
  QVector<VaultMetadata> result;
  const QString origin = canonicalHttpsOrigin(url);
  if (origin.isEmpty()) return result;
  for (const Entry &candidate : entries_) {
    if (!candidate.vault || !candidate.vault->exists()) continue;
    if (candidate.vault->isLocked()) {
      result.append({candidate.id, candidate.name, true});
      continue;
    }
    if (candidate.vault->hasOrigin(origin)) {
      result.append({candidate.id, candidate.name, candidate.vault->isLocked()});
    }
  }
  return result;
}
bool CredentialVaultManager::hasMatchingCredential(const QUrl &url) const {
  if (!exists()) return false;
  const QString origin = canonicalHttpsOrigin(url);
  if (origin.isEmpty()) return false;
  for (const Entry &candidate : entries_) {
    if (candidate.vault && candidate.vault->exists() &&
        (candidate.vault->isLocked() || candidate.vault->hasOrigin(origin))) {
      return true;
    }
  }
  return false;
}
