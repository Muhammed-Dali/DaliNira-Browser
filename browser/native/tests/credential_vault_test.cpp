#include "credential_vault.h"
#include "credential_vault_manager.h"
#include "device_keyring.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QMap>

#include <cassert>
#include <cstdio>
#include <iostream>

namespace {
class MockKeyringProvider : public DeviceKeyring::Provider {
 public:
  bool available = true;
  bool storeSucceeds = true;
  bool loadSucceeds = true;
  QMap<QString, QByteArray> storage;
  int clearCalls = 0;
  int storeCalls = 0;
  int loadCalls = 0;

  bool isAvailable() const override { return available; }
  bool storeSecret(const QString &vaultKeyId, const QByteArray &secret) override {
    if (!available || !storeSucceeds) return false;
    ++storeCalls;
    storage[vaultKeyId] = secret;
    return true;
  }
  bool loadSecret(const QString &vaultKeyId, QByteArray *secret) override {
    if (!available || !loadSucceeds || !storage.contains(vaultKeyId)) return false;
    ++loadCalls;
    *secret = storage.value(vaultKeyId);
    return true;
  }
  bool clearSecret(const QString &vaultKeyId) override {
    if (!available) return false;
    ++clearCalls;
    storage.remove(vaultKeyId);
    return true;
  }
};
}

int main() {
  std::cout << "Starting DaliNira Credential Vault Security & Keyring Test Suite..." << std::endl;

  // 1. Basic Vault Operations & Ciphertext-at-Rest Security Invariants
  {
    std::cout << "[RUN] Basic vault operations & encryption at rest" << std::endl;
    QTemporaryDir root; assert(root.isValid());
    const QString master = QStringLiteral("SyntheticMaster#2026");
    CredentialVault vault(root.path());
    assert(vault.isLocked());
    assert(!vault.create(QStringLiteral("weak")));
    assert(vault.create(master));
    bool updated = false;
    const bool saved = vault.save({QStringLiteral("https://example.com"), QStringLiteral("test-user"), QStringLiteral("synthetic-secret-A"), QStringLiteral("aWNvbg==")}, &updated);
    if (!saved) { std::fprintf(stderr, "%s\n", qPrintable(vault.lastError())); return 2; }
    assert(!updated);
    assert(vault.forOrigin(QUrl(QStringLiteral("https://example.com/login"))).size() == 1);
    assert(vault.forOrigin(QUrl(QStringLiteral("https://evil-example.com"))).isEmpty());
    assert(vault.forOrigin(QUrl(QStringLiteral("http://example.com"))).isEmpty());

    QFile file(root.path() + QStringLiteral("/credential-vault/vault-v2.json")); assert(file.open(QIODevice::ReadOnly)); const QByteArray bytes = file.readAll();
    assert(!bytes.contains("synthetic-secret-A") && !bytes.contains("test-user") && !bytes.contains("https://example.com"));
    const QString firstNonce = QJsonDocument::fromJson(bytes).object().value(QStringLiteral("records")).toArray().at(0).toObject().value(QStringLiteral("nonce")).toString();
    const auto first = vault.list().front();
    assert(vault.save({QStringLiteral("https://example.com"), QStringLiteral("test-user"), QStringLiteral("synthetic-secret-B"), QString{}}, &updated) && updated);
    file.close(); assert(file.open(QIODevice::ReadOnly)); const QString secondNonce = QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("records")).toArray().at(0).toObject().value(QStringLiteral("nonce")).toString();
    assert(firstNonce != secondNonce);
    CredentialSecret revealed; assert(vault.reveal(first.id, &revealed) && revealed.password == QStringLiteral("synthetic-secret-B") && revealed.iconPngBase64.isEmpty());
    assert(vault.list().front().iconPngBase64.isEmpty());
    const QString changedMaster = QStringLiteral("ChangedSynthetic#2027");
    assert(vault.changeMasterPassword(master, changedMaster));
    vault.lock(); assert(vault.isLocked() && vault.list().isEmpty());
    assert(!vault.unlock(QStringLiteral("NotTheMaster#2026")));

    // A fresh process-equivalent instance is always locked and only the correct master can open it.
    CredentialVault restartedWrong(root.path()); assert(restartedWrong.isLocked());
    assert(!restartedWrong.unlock(master));
    CredentialVault restarted(root.path()); assert(restarted.unlock(changedMaster)); assert(restarted.list().size() == 1);
    assert(restarted.remove(restarted.list().front().id)); assert(restarted.list().isEmpty());
    const auto permissions = QFileInfo(root.path() + QStringLiteral("/credential-vault/vault-v2.json")).permissions();
    assert(!(permissions & (QFileDevice::ReadGroup | QFileDevice::ReadOther | QFileDevice::WriteGroup | QFileDevice::WriteOther)));
    assert(restarted.reset());
    assert(!restarted.exists() && restarted.isLocked());
    assert(!QFile::exists(root.path() + QStringLiteral("/credential-vault/vault-v2.json")));
    std::cout << "[PASS] Basic vault operations & encryption at rest" << std::endl;
  }

  // 2. OS Keyring / Device-Secret Binding (Schema v3)
  {
    std::cout << "[RUN] OS Keyring / Device-Secret Binding (Schema v3)" << std::endl;
    auto mockKeyring = std::make_shared<MockKeyringProvider>();
    DeviceKeyring::setCustomProviderForTesting(mockKeyring);

    QTemporaryDir keyringRoot; assert(keyringRoot.isValid());
    const QString master = QStringLiteral("DeviceBoundMaster#2026");
    CredentialVault vault(keyringRoot.path());
    assert(vault.create(master));
    assert(vault.schemaVersion() == 4);
    assert(vault.isDeviceBound());
    assert(vault.deviceBinding() == QStringLiteral("secret-service"));
    assert(mockKeyring->storeCalls > 0);
    assert(!mockKeyring->storage.isEmpty());
    const QString canonicalId = vault.canonicalKeyringId();
    assert(!canonicalId.isEmpty() && !canonicalId.endsWith(QStringLiteral("-nosalt")));
    assert(mockKeyring->storage.contains(canonicalId));

    // Save a secret
    assert(vault.save({QStringLiteral("https://secure.example.com"), QStringLiteral("device-user"), QStringLiteral("DeviceBoundSecret#999"), QString{}}));
    const QString recordId = vault.list().front().id;

    // Normal unlock with keyring available
    vault.lock();
    assert(vault.isLocked());
    assert(vault.unlock(master));
    CredentialSecret revealed;
    assert(vault.reveal(recordId, &revealed));
    assert(revealed.password == QStringLiteral("DeviceBoundSecret#999"));

    // Unlock fails if keyring is unavailable or secret removed
    vault.lock();
    mockKeyring->available = false;
    assert(!vault.unlock(master));
    assert(vault.lastError() == QStringLiteral("keyring-unavailable"));

    // Unlock succeeds once keyring becomes available again
    mockKeyring->available = true;
    assert(vault.unlock(master));

    // Reset clears keyring secret
    assert(vault.reset());
    assert(mockKeyring->clearCalls > 0);

    DeviceKeyring::resetCustomProviderForTesting();
    std::cout << "[PASS] OS Keyring / Device-Secret Binding (Schema v3)" << std::endl;
  }

  // 2b. Recover only a cryptographically valid legacy <vault-id>-nosalt secret.
  {
    std::cout << "[RUN] Legacy nosalt keyring recovery" << std::endl;
    auto mockKeyring = std::make_shared<MockKeyringProvider>();
    DeviceKeyring::setCustomProviderForTesting(mockKeyring);
    QTemporaryDir root; assert(root.isValid());
    const QString master = QStringLiteral("LegacyNosaltMaster#2026");
    const QString storageId = QStringLiteral("11111111-1111-1111-1111-111111111111");
    {
      CredentialVault vault(root.path(), nullptr, storageId);
      assert(vault.create(master));
      assert(vault.save({QStringLiteral("https://nosalt.example"), QStringLiteral("legacy-user"), QStringLiteral("legacy-value"), {}}));
      const QString canonical = vault.canonicalKeyringId();
      const QByteArray secret = mockKeyring->storage.take(canonical);
      assert(!secret.isEmpty());
      mockKeyring->storage.insert(storageId + QStringLiteral("-nosalt"), secret);
      vault.lock();
    }
    {
      CredentialVault recovered(root.path(), nullptr, storageId);
      assert(recovered.unlock(master));
      const QString canonical = recovered.canonicalKeyringId();
      assert(mockKeyring->storage.contains(canonical));
      assert(!mockKeyring->storage.contains(storageId + QStringLiteral("-nosalt")));
      recovered.lock();
    }
    {
      CredentialVault repeated(root.path(), nullptr, storageId);
      assert(repeated.unlock(master));
      assert(!mockKeyring->storage.contains(storageId + QStringLiteral("-nosalt")));
    }

    // An invalid compatibility secret must neither migrate nor be deleted.
    const QString badId = QStringLiteral("22222222-2222-2222-2222-222222222222");
    {
      CredentialVault bad(root.path(), nullptr, badId);
      assert(bad.create(master));
      const QString canonical = bad.canonicalKeyringId();
      mockKeyring->storage.remove(canonical);
      mockKeyring->storage.insert(badId + QStringLiteral("-nosalt"), QByteArray(32, 'x'));
      bad.lock();
    }
    {
      CredentialVault rejected(root.path(), nullptr, badId);
      assert(!rejected.unlock(master));
      assert(rejected.lastError() == QStringLiteral("keyring-unavailable"));
      assert(mockKeyring->storage.contains(badId + QStringLiteral("-nosalt")));
      assert(!mockKeyring->storage.contains(rejected.canonicalKeyringId()));
    }
    DeviceKeyring::resetCustomProviderForTesting();
    std::cout << "[PASS] Legacy nosalt keyring recovery" << std::endl;
  }

  // 3. Backward-Compatible Migration from Schema v2 to Schema v3
  {
    std::cout << "[RUN] Backward-Compatible Migration from Schema v2 to Schema v3" << std::endl;
    auto mockKeyring = std::make_shared<MockKeyringProvider>();

    QTemporaryDir legacyRoot; assert(legacyRoot.isValid());
    const QString master = QStringLiteral("LegacyMaster#2026");

    // Phase 1: Create a legacy vault without keyring binding
    mockKeyring->available = false;
    DeviceKeyring::setCustomProviderForTesting(mockKeyring);
    {
      CredentialVault legacy(legacyRoot.path());
      assert(legacy.create(master));
      assert(!legacy.isDeviceBound());
      assert(legacy.deviceBinding() == QStringLiteral("none"));
      assert(legacy.save({QStringLiteral("https://legacy.example.org"), QStringLiteral("legacy-user"), QStringLiteral("OldLegacySecret#123"), QString{}}));
      legacy.lock();
    }

    // Phase 2: Open with OS keyring now available -> auto-migration occurs upon unlock
    mockKeyring->available = true;
    {
      CredentialVault migrating(legacyRoot.path());
      assert(!migrating.isDeviceBound());
      assert(migrating.unlock(master));
      // Upon unlock, it should have seamlessly upgraded to Schema v3 with device binding
      assert(migrating.schemaVersion() == 4);
      assert(migrating.isDeviceBound());
      assert(migrating.deviceBinding() == QStringLiteral("secret-service"));
      assert(mockKeyring->storeCalls > 0);

      // Verify the credential is fully intact
      CredentialSecret secret;
      assert(migrating.reveal(migrating.list().front().id, &secret));
      assert(secret.password == QStringLiteral("OldLegacySecret#123"));
      migrating.lock();
    }

    // Phase 3: Verify the migrated vault now strictly requires the keyring
    {
      CredentialVault restarted(legacyRoot.path());
      mockKeyring->available = false;
      assert(!restarted.unlock(master));
      assert(restarted.lastError() == QStringLiteral("keyring-unavailable"));

      mockKeyring->available = true;
      assert(restarted.unlock(master));
      assert(restarted.isDeviceBound());
    }

    DeviceKeyring::resetCustomProviderForTesting();
    std::cout << "[PASS] Backward-Compatible Migration from Schema v2 to Schema v3" << std::endl;
  }

  // 4. Corrupt / Tampered Vault & Future Version Protection
  {
    std::cout << "[RUN] Tampered ciphertext, invalid tags, and unsupported versions" << std::endl;
    const QString master = QStringLiteral("TamperMaster#2026");

    // Tampered record ciphertext / tag
    QTemporaryDir corruptRoot; CredentialVault corrupt(corruptRoot.path()); assert(corrupt.create(master));
    assert(corrupt.save({QStringLiteral("https://example.org"), QStringLiteral("tamper-user"), QStringLiteral("synthetic-tamper-value"), QString{}}));
    const QString corruptId = corrupt.list().front().id;
    const QString corruptPath = corruptRoot.path() + QStringLiteral("/credential-vault/vault-v2.json");
    QFile corruptFile(corruptPath); assert(corruptFile.open(QIODevice::ReadOnly));
    QJsonObject tampered = QJsonDocument::fromJson(corruptFile.readAll()).object();
    corruptFile.close();
    QJsonArray records = tampered.value(QStringLiteral("records")).toArray();
    QJsonObject record = records.at(0).toObject();
    QString tag = record.value(QStringLiteral("tag")).toString();
    tag[0] = (tag[0] == QLatin1Char('A')) ? QLatin1Char('B') : QLatin1Char('A');
    record.insert(QStringLiteral("tag"), tag);
    records[0] = record;
    tampered.insert(QStringLiteral("records"), records);
    assert(corruptFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    corruptFile.write(QJsonDocument(tampered).toJson(QJsonDocument::Compact));
    corruptFile.close();
    corrupt.lock();
    CredentialVault corruptRestart(corruptRoot.path());
    assert(corruptRestart.unlock(master));
    CredentialSecret tamperedSecret;
    assert(corruptRestart.reveal(corruptId, &tamperedSecret));
    assert(tamperedSecret.username == QStringLiteral("tamper-user"));
    tamperedSecret.wipe();
    corruptRestart.lock();

    // If neither primary nor backup authenticates, unlock fails closed.
    for (const QString &candidate : {corruptPath, corruptPath + QStringLiteral(".bak")}) {
      QFile file(candidate); assert(file.open(QIODevice::ReadOnly));
      QJsonObject object = QJsonDocument::fromJson(file.readAll()).object(); file.close();
      QJsonArray candidateRecords = object.value(QStringLiteral("records")).toArray();
      QJsonObject candidateRecord = candidateRecords.at(0).toObject();
      QString candidateTag = candidateRecord.value(QStringLiteral("tag")).toString();
      candidateTag[0] = candidateTag[0] == QLatin1Char('A') ? QLatin1Char('B') : QLatin1Char('A');
      candidateRecord.insert(QStringLiteral("tag"), candidateTag);
      candidateRecords[0] = candidateRecord;
      object.insert(QStringLiteral("records"), candidateRecords);
      assert(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
      file.write(QJsonDocument(object).toJson(QJsonDocument::Compact)); file.close();
    }
    CredentialVault noRecovery(corruptRoot.path());
    assert(!noRecovery.unlock(master));
    assert(noRecovery.lastError() == QStringLiteral("corrupt-vault"));

    // Unsupported schema version protection (never overwrite unknown future vault versions)
    QTemporaryDir futureRoot;
    CredentialVault futureVault(futureRoot.path());
    assert(futureVault.create(master));
    futureVault.lock();
    const QString futurePath = futureRoot.path() + QStringLiteral("/credential-vault/vault-v2.json");
    QFile futureFile(futurePath); assert(futureFile.open(QIODevice::ReadOnly));
    QJsonObject futureObj = QJsonDocument::fromJson(futureFile.readAll()).object();
    futureFile.close();
    futureObj.insert(QStringLiteral("schemaVersion"), 99);
    assert(futureFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    futureFile.write(QJsonDocument(futureObj).toJson(QJsonDocument::Compact));
    futureFile.close();
    QFile::remove(futurePath + QStringLiteral(".bak"));

    CredentialVault futureCheck(futureRoot.path());
    assert(!futureCheck.unlock(master));
    assert(futureCheck.lastError() == QStringLiteral("unsupported-vault-version"));
    // File remains intact and uncorrupted
    assert(QFile::exists(futurePath));
    std::cout << "[PASS] Tampered ciphertext, invalid tags, and unsupported versions" << std::endl;
  }

  // 5. Origin / Domain Canonicalization Security Invariants
  {
    std::cout << "[RUN] Origin / Domain Canonicalization Security Invariants" << std::endl;
    // Standard HTTPS canonicalization
    assert(CredentialVault::canonicalHttpsOrigin(QUrl(QStringLiteral("https://example.com/login?token=abc#hash"))) == QStringLiteral("https://example.com"));
    assert(CredentialVault::canonicalHttpsOrigin(QUrl(QStringLiteral("https://EXAMPLE.COM/"))) == QStringLiteral("https://example.com"));
    assert(CredentialVault::canonicalHttpsOrigin(QUrl(QStringLiteral("https://example.com:8443/login"))) == QStringLiteral("https://example.com:8443"));
    assert(CredentialVault::canonicalHttpsOrigin(QUrl(QStringLiteral("https://example.com:443/login"))) == QStringLiteral("https://example.com"));

    // Trailing dot normalization
    assert(CredentialVault::canonicalHttpsOrigin(QUrl(QStringLiteral("https://example.com./login"))) == QStringLiteral("https://example.com"));

    // Punycode / IDN internationalized domain names
    const QString idnOrigin = CredentialVault::canonicalHttpsOrigin(QUrl(QStringLiteral("https://münchen.de/login")));
    assert(!idnOrigin.isEmpty() && idnOrigin.contains(QStringLiteral("xn--")));

    // Rejection of non-HTTPS schemes
    assert(CredentialVault::canonicalHttpsOrigin(QUrl(QStringLiteral("http://example.com"))).isEmpty());
    assert(CredentialVault::canonicalHttpsOrigin(QUrl(QStringLiteral("dalinira://passwords"))).isEmpty());
    assert(CredentialVault::canonicalHttpsOrigin(QUrl(QStringLiteral("file:///etc/passwd"))).isEmpty());

    // Rejection of credentials embedded in URL
    assert(CredentialVault::canonicalHttpsOrigin(QUrl(QStringLiteral("https://user:pass@example.com"))).isEmpty());

    // Hostile suffix protection
    const QString trusted = QStringLiteral("https://example.com");
    assert(CredentialVault::canonicalHttpsOrigin(QUrl(QStringLiteral("https://evil-example.com"))) != trusted);
    assert(CredentialVault::canonicalHttpsOrigin(QUrl(QStringLiteral("https://example.com.attacker.org"))) != trusted);
    std::cout << "[PASS] Origin / Domain Canonicalization Security Invariants" << std::endl;
  }

  // 6. Sensitive Memory Wipe Invariant
  {
    std::cout << "[RUN] Sensitive Memory Wipe" << std::endl;
    CredentialSecret secret{QStringLiteral("https://example.com"), QStringLiteral("user"), QStringLiteral("ClearMeSecret#123"), QStringLiteral("aWNvbg==")};
    secret.wipe();
    assert(secret.origin.isEmpty());
    assert(secret.username.isEmpty());
    assert(secret.password.isEmpty());
    assert(secret.iconPngBase64.isEmpty());
    std::cout << "[PASS] Sensitive Memory Wipe" << std::endl;
  }

  // 7. Multi-Vault Management, Portable Export & Import
  {
    std::cout << "[RUN] Multi-Vault Management, Portable Export & Import" << std::endl;
    const QString master = QStringLiteral("MultiMaster#2026");
    const QString changedMaster = QStringLiteral("MultiChangedMaster#2027");

    auto mockKeyring = std::make_shared<MockKeyringProvider>();
    DeviceKeyring::setCustomProviderForTesting(mockKeyring);
    QTemporaryDir multiRoot; CredentialVaultManager multi(multiRoot.path()); QString personalId, workId;
    assert(multi.createVault(QStringLiteral("Kişisel"), master, &personalId));
    assert(multi.saveToVault(personalId, {QStringLiteral("https://example.net"), QStringLiteral("personal-user"), QStringLiteral("personal-secret"), QString{}}));
    assert(multi.createVault(QStringLiteral("İş"), changedMaster, &workId));
    assert(multi.saveToVault(workId, {QStringLiteral("https://example.net"), QStringLiteral("work-user"), QStringLiteral("work-secret"), QString{}}));
    assert(multi.forOrigin(QUrl(QStringLiteral("https://example.net/login"))).size() == 2);
    const QVector<CredentialMetadata> ownedRecords = multi.forOrigin(QUrl(QStringLiteral("https://example.net/login")));
    for (const CredentialMetadata &record : ownedRecords) {
      assert(multi.owningVaultId(record.id) == record.vaultId);
    }
    assert(multi.setActiveVault(personalId));
    multi.lockVault(workId);
    assert(!multi.unlockVault(workId, master));
    assert(multi.unlockVault(workId, changedMaster));
    const QString backupPath = multiRoot.path() + QStringLiteral("/portable.dalinira-vault-backup");
    const QString backupPassword = QStringLiteral("BackupPassword#2026");
    multi.lockVault(personalId);
    assert(multi.exportBackup(backupPath, backupPassword));
    QFile backupFile(backupPath); assert(backupFile.open(QIODevice::ReadOnly)); const QByteArray backupBytes = backupFile.readAll(); assert(!backupBytes.contains("personal-secret") && !backupBytes.contains("work-secret"));
    QTemporaryDir restoredRoot;
    {
      CredentialVaultManager restored(restoredRoot.path()); QStringList imported;
      assert(!restored.importBackup(backupPath, QStringLiteral("WrongBackup#2026"), &imported));
      assert(restored.importBackup(backupPath, backupPassword, &imported) && imported.size() == 2 && restored.vaults().size() == 2);
    }
    // Genuine restart-equivalent reload: imported device secrets must be found
    // by their canonical salted ids rather than an in-memory cache.
    {
      CredentialVaultManager restartedRestore(restoredRoot.path());
      for (const VaultMetadata &restoredVault : restartedRestore.vaults())
        assert(restartedRestore.unlockVault(restoredVault.id, restoredVault.name == QStringLiteral("Kişisel") ? master : changedMaster));
      assert(restartedRestore.forOrigin(QUrl(QStringLiteral("https://example.net/login"))).size() == 2);
    }
    multi.lockVault(personalId);
    assert(multi.forOrigin(QUrl(QStringLiteral("https://example.net/login"))).size() == 1);
    assert(multi.unlockVault(personalId, master));
    assert(multi.forOrigin(QUrl(QStringLiteral("https://example.net/login"))).size() == 2);
    assert(multi.setActiveVault(workId) && multi.clearActiveVault() && multi.list().isEmpty());
    assert(multi.deleteVault(personalId) && multi.vaults().size() == 1);
    CredentialVaultManager multiRestart(multiRoot.path());
    assert(multiRestart.vaults().size() == 1 && multiRestart.activeVaultId() == workId);
    assert(multiRestart.unlockVault(workId, changedMaster) && multiRestart.list().isEmpty());
    DeviceKeyring::resetCustomProviderForTesting();
    std::cout << "[PASS] Multi-Vault Management, Portable Export & Import" << std::endl;
  }

  // 8. A device-bound backup fails closed when any required secret is missing.
  {
    std::cout << "[RUN] Device-bound backup fail-closed" << std::endl;
    auto mockKeyring = std::make_shared<MockKeyringProvider>();
    DeviceKeyring::setCustomProviderForTesting(mockKeyring);
    QTemporaryDir root; assert(root.isValid());
    CredentialVaultManager manager(root.path());
    QString id;
    assert(manager.createVault(QStringLiteral("Bound"), QStringLiteral("BoundMaster#2026"), &id));
    manager.lockVault(id);
    mockKeyring->storage.clear();
    const QString path = root.path() + QStringLiteral("/must-not-exist.dalinira-vault-backup");
    assert(!manager.exportBackup(path, QStringLiteral("BackupMaster#2026")));
    assert(manager.lastError() == QStringLiteral("backup-device-secret-unavailable"));
    assert(!QFile::exists(path));
    DeviceKeyring::resetCustomProviderForTesting();
    std::cout << "[PASS] Device-bound backup fail-closed" << std::endl;
  }

  // 9. Keyed origin-index isolation and vault-scoped sensitive authorization.
  {
    std::cout << "[RUN] Keyed origin index and sensitive authorization" << std::endl;
    auto unavailableKeyring = std::make_shared<MockKeyringProvider>();
    unavailableKeyring->available = false;
    DeviceKeyring::setCustomProviderForTesting(unavailableKeyring);
    QTemporaryDir firstRoot, secondRoot;
    CredentialVault first(firstRoot.path()), second(secondRoot.path());
    assert(first.create(QStringLiteral("FirstMaster#2026")));
    assert(second.create(QStringLiteral("SecondMaster#2026")));
    const CredentialSecret common{QStringLiteral("https://same-origin.example"),
                                  QStringLiteral("fixture-user"), QStringLiteral("fixture-secret")};
    assert(first.save(common));
    assert(second.save(common));
    const auto readTokens = [](const QString &root) {
      QFile file(root + QStringLiteral("/credential-vault/origin-index.json"));
      assert(file.open(QIODevice::ReadOnly));
      const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
      assert(object.value(QStringLiteral("version")).toInt() == 2);
      assert(!object.contains(QStringLiteral("originHashes")));
      return object.value(QStringLiteral("originTokens")).toArray();
    };
    const QJsonArray firstTokens = readTokens(firstRoot.path());
    const QJsonArray secondTokens = readTokens(secondRoot.path());
    assert(firstTokens.size() == 1 && secondTokens.size() == 1);
    assert(firstTokens.at(0).toString() != secondTokens.at(0).toString());
    assert(firstTokens.at(0).toString() != QStringLiteral("https://same-origin.example"));

    qint64 clock = 100000;
    QTemporaryDir managerRoot;
    CredentialVaultManager manager(managerRoot.path());
    manager.setTimeProviderForTesting([&clock] { return clock; });
    QString firstId, secondId;
    assert(manager.createVault(QStringLiteral("First"), QStringLiteral("FirstMaster#2026"), &firstId));
    assert(manager.createVault(QStringLiteral("Second"), QStringLiteral("SecondMaster#2026"), &secondId));
    assert(!manager.authorizeSensitiveAction(secondId, QStringLiteral("FirstMaster#2026")));
    assert(manager.authorizeSensitiveAction(secondId, QStringLiteral("SecondMaster#2026")));
    assert(manager.hasSensitiveAuthorization(secondId));
    assert(!manager.hasSensitiveAuthorization(firstId));
    clock += 60001;
    assert(!manager.hasSensitiveAuthorization(secondId));
    assert(manager.authorizeSensitiveAction(secondId, QStringLiteral("SecondMaster#2026")));
    manager.lockVault(secondId);
    assert(!manager.hasSensitiveAuthorization(secondId));
    DeviceKeyring::resetCustomProviderForTesting();
    std::cout << "[PASS] Keyed origin index and sensitive authorization" << std::endl;
  }

  std::cout << "All DaliNira Credential Vault Security & Keyring tests passed successfully!" << std::endl;
  return 0;
}
