# DaliNira Password Manager Security Notes

Status: remediation validation in progress. This describes the current Qt implementation, not a release certification.

## Storage and cryptography

`CredentialVaultManager` owns one or more `CredentialVault` instances shared by Chromium and WebKit. Vaults are compact local JSON envelopes under the application-data `credential-vault` directory; they are not SQLite, settings entries, or plaintext credential files.

Current schema 4 uses a random 256-bit data key per vault; AES-256-GCM for data-key wrapping and independent record encryption; random 96-bit nonces and 128-bit tags; and PBKDF2-HMAC-SHA256 with 600,000 iterations, a random 128-bit salt, and a 256-bit result. Record ids are authenticated as AAD. An HMAC-SHA256 collection tag covers record identity, order, timestamps, ciphertext, nonce, and tag. The origin index uses a per-vault data-key HMAC, not plaintext origins or unkeyed hashes.

Schema 2/3 vaults remain readable and migrate idempotently after a successful unlock. The migration authenticates existing data and updates metadata/data-key wrapping without needless record re-encryption. Strong rollback prevention would require an external OS-backed monotonic state and is not claimed.

The data key and cached device secret are cleared on lock. Qt implicit sharing means perfect process-memory zeroization cannot be guaranteed, but native pending candidates and temporary byte arrays are cleared where practical.

## Linux device binding

When Secret Service/libsecret is available, a random 256-bit per-vault device secret is stored in the OS credential store. The PBKDF2 result and device secret are combined using HMAC-SHA256. Canonical keyring identifiers include the destination vault id and authenticated vault salt.

Import validates envelope metadata before storing the device secret. A compatibility path accepts an old `<vault-id>-nosalt` entry only when it cryptographically unwraps the existing data key; it stores and verifies the canonical entry before removing the legacy one.

If Secret Service is unavailable, a new vault remains AES-GCM encrypted but uses master-password-only protection. The UI states this explicitly. Existing fallback vaults stay accessible and can use the existing safe upgrade after a later unlock. Windows DPAPI and macOS Keychain support are not implemented or claimed.

## Backup and recovery

Exports are AES-256-GCM archives protected by a separate backup password through PBKDF2-HMAC-SHA256 (600,000 iterations). Export verifies every included device-bound vault, including locked secondary vaults, and fails instead of omitting required device material. Import authenticates the archive, validates each envelope, derives the canonical keyring id from its actual salt, and rolls back an incomplete destination where practical.

Unlock validates the wrapped key, collection tag, and every record. A structurally valid but cryptographically invalid primary does not suppress recovery: `.bak` is used only if its schema, wrapped key, collection, and records all authenticate. If neither copy authenticates, the vault fails closed. A backup can still be an older valid snapshot.

## Chromium and WebKit integration

Both engines use the same native vault and storage. Chromium uses a narrow `QWebChannel` registered only in `QWebEngineScript::ApplicationWorld`; credentials no longer travel through console messages. WebKitGTK registers a narrow script-message handler only in DaliNira's isolated content world and injects capture only into the top frame.

Both paths accept canonical HTTPS only, compare the claimed origin to the native top-level URL, clear pending state on navigation/destruction, require trusted login-related capture events, and require explicit user action for fill. Fill reauthentication is bound to the selected record's owning vault. Scheme, host, and non-default port must match exactly; parent/subdomain, HTTP/HTTPS, custom-port, and iframe boundaries fail closed.

Web pages receive neither credential lists nor a general vault API. Like any browser autofill, page script can observe a value after the user authorizes insertion into that page's DOM.

## Native UI and local protections

Editing/revealing, copying, and encrypted export require vault-specific reauthentication. Its authorization window is one minute and clears on vault lock or process teardown; autofill retains per-operation reauthentication. Clipboard clearing after 30 seconds occurs only if the clipboard still contains DaliNira's value.

Unix vault directories are restricted to 0700 and vault, backup, security-state, origin-index, vault-index, and encrypted-export files to 0600 while atomic `QSaveFile` writes are retained.

## Remaining threat-model limits

- Malware running as the user may observe secrets while unlocked or after fill/copy.
- PBKDF2 raises offline-guessing cost but cannot compensate for a weak password.
- A valid older snapshot cannot be identified without an external monotonic trust anchor.
- Perfect memory zeroization and an “unhackable” implementation are not claimed.

## Validation notes

Targeted tests cover keyring migration, device-bound backup/import/restart, multi-vault ownership, collection and record tampering, backup recovery, keyed origin-index isolation, permissions, and vault-scoped authorization. The Chromium autofill suite checks trusted events, origin/frame/navigation rules, and removal of console transport; on this machine it reaches the known QtWebEngine sandbox-host teardown SIGTRAP after the preceding assertions pass. Real Chromium and WebKit GUI results must be reported separately using isolated synthetic profiles.
