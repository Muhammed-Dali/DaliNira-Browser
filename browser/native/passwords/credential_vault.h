#pragma once

#include <QDateTime>
#include <QObject>
#include <QSet>
#include <QString>
#include <QUrl>
#include <QVector>
#include <QRecursiveMutex>

#include <chrono>
#include <functional>

class QTimer;

struct CredentialMetadata {
  QString id;
  QString origin;
  QString username;
  QDateTime createdAt;
  QDateTime updatedAt;
  // Compatibility-only field. Favicon bytes are resolved from the browser's
  // local favicon database and are never persisted in the credential vault.
  QString iconPngBase64;
  QString vaultId;
  QString vaultName;
};

struct CredentialSecret {
  QString origin;
  QString username;
  QString password;
  QString iconPngBase64;

  CredentialSecret() = default;
  CredentialSecret(const QString &o, const QString &u, const QString &p, const QString &icon = {})
      : origin(o), username(u), password(p), iconPngBase64(icon) {}
  CredentialSecret(const CredentialSecret &) = default;
  CredentialSecret(CredentialSecret &&) noexcept = default;
  CredentialSecret &operator=(const CredentialSecret &) = default;
  CredentialSecret &operator=(CredentialSecret &&) noexcept = default;
  ~CredentialSecret() {
    wipe();
  }

  void wipe() {
    password.fill(QChar());
    password.clear();
    username.fill(QChar());
    username.clear();
    origin.fill(QChar());
    origin.clear();
    iconPngBase64.clear();
  }
};

// Native-only encrypted credential store.  No web renderer is given a QObject,
// QWebChannel, or a callable API for this class.
class CredentialVault final : public QObject {
  Q_OBJECT
 public:
  explicit CredentialVault(const QString &dataDirectory, QObject *parent = nullptr, const QString &storageId = {});
  ~CredentialVault() override;

  static QString canonicalHttpsOrigin(const QUrl &url);
  static bool isStrongMasterPassword(const QString &password);
  static int cooldownForAttempt(int attempts);

  bool exists() const;
  bool isLocked() const;
  int autoLockTimeoutMs() const;
  bool setAutoLockTimeoutMs(int timeoutMs);
  QString lastError() const;
  bool create(const QString &masterPassword);
  bool unlock(const QString &masterPassword);
  void lock();
  bool save(const CredentialSecret &secret, bool *updated = nullptr);
  bool update(const QString &id, const CredentialSecret &secret);
  bool changeMasterPassword(const QString &currentPassword, const QString &nextPassword);
  bool remove(const QString &id);
  bool reset();
  QVector<CredentialMetadata> list() const;
  bool reveal(const QString &id, CredentialSecret *secret) const;
  QVector<CredentialMetadata> forOrigin(const QUrl &url) const;
  bool hasOrigin(const QString &origin) const;

  int schemaVersion() const;
  bool isDeviceBound() const;
  QString deviceBinding() const;
  QString storageId() const;
  QByteArray deviceSecret() const;
  bool importDeviceSecret(const QByteArray &secret);
  bool exportDeviceSecret(QByteArray *secret);
  QString canonicalKeyringId();

  bool isUnlockRateLimited() const;
  int remainingUnlockCooldownSeconds() const;
  int failedUnlockAttempts() const;
  void resetFailedUnlockAttempts();
  void setTimeProviderForTesting(std::function<qint64()> provider);

 signals:
  void lockStateChanged(bool locked);
  void changed();

 private:
  struct Record { QString id; qint64 createdAt = 0; qint64 updatedAt = 0; QByteArray encrypted; QByteArray nonce; QByteArray tag; };
  bool loadEnvelope();
  bool loadEnvelopeFromPath(const QString &candidate);
  bool persist();
  QByteArray collectionAuthenticator() const;
  bool validateDecryptedCollection() const;
  bool decryptRecord(const Record &record, CredentialSecret *secret) const;
  bool encryptRecord(Record *record, const CredentialSecret &secret);
  bool setError(const QString &error) const;
  void touch() const;
  void secureClear(QByteArray *value) const;

  void loadSecurityState();
  void persistSecurityState();
  void clearPersistedSecurityState();
  void recordFailedUnlockAttempt();
  qint64 nowEpochMs() const;

  QString originToken(const QString &canonicalOrigin) const;
  void loadOriginIndex();
  void persistOriginIndex();
  void clearPersistedOriginIndex();

  QString vaultKeyringId() const;
  QString legacyNosaltKeyringId() const;
  bool obtainDeviceSecret(QByteArray *outSecret) const;
  bool recoverLegacyDeviceSecret(const QString &masterPassword, QByteArray *outSecret);

  QString directory_;
  QString storageId_;
  QString path_;
  QString securityStatePath_;
  QString originIndexPath_;
  mutable QString lastError_;
  int schemaVersion_ = 3;
  QString deviceBinding_ = QStringLiteral("none");
  mutable QByteArray cachedDeviceSecret_;
  QByteArray salt_;
  QByteArray wrappedKey_;
  QByteArray wrappedNonce_;
  QByteArray wrappedTag_;
  QByteArray collectionTag_;
  mutable QByteArray dataKey_;
  QVector<Record> records_;
  mutable QSet<QString> storedOriginHashes_;
  mutable int failedUnlocks_ = 0;
  mutable qint64 cooldownExpiryEpochMs_ = 0;
  mutable std::chrono::steady_clock::time_point cooldownExpirySteady_{};
  mutable bool hasSteadyExpiry_ = false;
  std::function<qint64()> timeProvider_;
  mutable qint64 lastActivityMs_ = 0;
  mutable QRecursiveMutex mutex_;
  int autoLockTimeoutMs_ = 5 * 60 * 1000;
  QTimer *idleTimer_ = nullptr;
};
