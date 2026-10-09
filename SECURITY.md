# Security Policy

## Supported Versions

DaliNira Browser actively maintains and patches the latest stable release line.

| Version | Supported          | Notes |
| ------- | ------------------ | ----- |
| 8.0.x   | :white_check_mark: | Current stable branch |
| < 8.0.0 | :x:                | Unsupported; please update to the latest release |

## Security Architecture Overview

DaliNira Browser is engineered with a privacy- and security-first model:

- **Local-Only Encrypted Vault (Schema v3):** Master password key derivation uses PBKDF2-HMAC-SHA256 (600,000 iterations). Records are encrypted with AES-256-GCM (12-byte random IVs, 16-byte authentication tags). No passwords or keys are ever sent to external cloud servers.
- **Hardware-Bound Key Derivation (`DeviceKeyring`):** Vault wrap keys are bound to the local machine via the Linux FreeDesktop Secret Service (`org.freedesktop.secrets` / `libsecret-1`).
- **Autofill Script Isolation:** Credential autofill and login detection scripts execute exclusively in isolated `QWebEngineScript::ApplicationWorld`, preventing web page scripts from tampering with or intercepting autofill routines.
- **Single-Use Fill Tokens:** Autofill requests require single-use, origin-bound random tokens validated on the native C++ side.
- **Sensitive Memory Protection:** Passwords and keys in RAM are explicitly zero-wiped upon destruction or rejection (`CredentialSecret::wipe()`).
- **Private Browsing Isolation:** Private windows use isolated ephemeral directories that are strictly separated from persistent profile vaults and history.

## Reporting a Vulnerability

If you discover a security vulnerability in DaliNira Browser, please **do not report it in a public issue, pull request, or public discussion**.

### How to Report Privately

1. Use GitHub's private vulnerability reporting portal:
   **[Submit a Security Advisory](https://github.com/Muhammed-Dali/DaliNira-Browser/security/advisories/new)**
2. Provide a clear, detailed description including:
   - Affected DaliNira Browser version and Linux distribution.
   - Attack vector and reproduction steps or proof-of-concept.
   - Potential impact of the vulnerability.
   - Any proposed mitigations or patches.

### Critical Privacy Precaution

When submitting a security report, **never disclose actual personal credentials**:
- **Do not include real passwords, master passwords, or vault files.**
- **Do not include real auth tokens, session cookies, or personal browsing history.**
- Use synthetic dummy accounts (e.g. `user@example.com` / `SecretPassword#123`) in all reproduction scripts.

### Response Timeline

- **Acknowledgment:** Within 48 hours.
- **Triage & Status Update:** Within 7 business days.
- **Fix & Disclosure:** Coordinated disclosure after a patched release is published.
