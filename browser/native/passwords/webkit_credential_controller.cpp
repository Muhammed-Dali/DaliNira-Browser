#include "webkit_credential_controller.h"

#if defined(DALINIRA_BUILD_WEBKIT)

#include "credential_autofill_controller.h"
#include "credential_vault_manager.h"
#include "vault_unlock_dialog.h"
#include "engine/webkit/webkit_engine_view.h"
#include "i18n/i18n.h"

#include <QInputDialog>
#include <QMessageBox>
#include <QScopeGuard>
#include <QUuid>

using dalinira::engine::WebKitEngineView;

WebKitCredentialController::WebKitCredentialController(CredentialVaultManager *vault,
                                                       QWidget *dialogParent, QObject *parent)
    : QObject(parent), vault_(vault), dialogParent_(dialogParent) {}

void WebKitCredentialController::registerView(WebKitEngineView *view) {
  if (!view || generations_.contains(view)) return;
  generations_.insert(view, 0);
  view->setCredentialCaptureScript(CredentialAutofillController::webKitCandidateCaptureSource());
  connect(view, &WebKitEngineView::credentialMessage, this,
          [this, view](const QString &type, const QVariantMap &payload) { handleMessage(view, type, payload); });
  connect(view, &WebKitEngineView::urlChanged, this, [this, view](const QUrl &) {
    generations_[view] = generations_.value(view) + 1;
    clearView(view);
  });
  connect(view, &WebKitEngineView::loadFinished, this,
          [this, view](bool ok) { if (ok) refreshFillButton(view); });
  connect(view, &QObject::destroyed, this, [this, view] {
    clearView(view); generations_.remove(view);
  });
}

void WebKitCredentialController::unregisterView(WebKitEngineView *view) {
  if (!view || !generations_.contains(view)) return;
  disconnect(view, nullptr, this, nullptr);
  clearView(view);
  generations_.remove(view);
}

void WebKitCredentialController::clearView(WebKitEngineView *view) {
  auto it = candidates_.find(view);
  if (it != candidates_.end()) { it->password.fill(QChar()); candidates_.erase(it); }
  fillTokens_.remove(view);
}

void WebKitCredentialController::refreshFillButton(WebKitEngineView *view) {
  if (!view || !vault_) return;
  const QString origin = CredentialVaultManager::canonicalHttpsOrigin(view->url());
  if (origin.isEmpty() || !vault_->hasMatchingCredential(view->url())) return;
  const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
  fillTokens_.insert(view, token);
  view->runJavaScript(CredentialAutofillController::fillButtonScript(token));
}

void WebKitCredentialController::handleMessage(WebKitEngineView *view, const QString &type,
                                               const QVariantMap &payload) {
  if (!view || !vault_) return;
  const QString actualOrigin = CredentialVaultManager::canonicalHttpsOrigin(view->url());
  if (actualOrigin.isEmpty() || payload.value(QStringLiteral("origin")).toString() != actualOrigin) return;
  if (type == QLatin1String("candidate")) {
    const QString username = payload.value(QStringLiteral("username")).toString().trimmed();
    const QString password = payload.value(QStringLiteral("password")).toString();
    if (username.isEmpty() || username.size() > 320 || password.isEmpty() || password.size() > 4096 ||
        !payload.value(QStringLiteral("submitted")).toBool()) return;
    clearView(view);
    candidates_.insert(view, {actualOrigin, username, password,
                              QDateTime::currentDateTimeUtc().addSecs(120), generations_.value(view)});
    return;
  }
  if (type == QLatin1String("success")) { promptSave(view); return; }
  if (type == QLatin1String("fill")) fill(view, payload.value(QStringLiteral("token")).toString());
}

void WebKitCredentialController::promptSave(WebKitEngineView *view) {
  auto it = candidates_.find(view);
  if (it == candidates_.end() || it->expiresAt <= QDateTime::currentDateTimeUtc() ||
      it->generation != generations_.value(view) ||
      CredentialVaultManager::canonicalHttpsOrigin(view->url()) != it->origin) { clearView(view); return; }
  Candidate candidate = *it;
  const auto wipeCandidate = qScopeGuard([&candidate] {
    candidate.password.fill(QChar());
    candidate.password.clear();
  });
  if (vault_->isLocked()) {
    VaultUnlockDialog unlock(vault_, candidate.origin, dialogParent_, VaultUnlockDialog::Purpose::Unlock,
                             vault_->activeVaultId());
    if (unlock.exec() != QDialog::Accepted || candidate.generation != generations_.value(view)) {
      clearView(view); return;
    }
  }
  QString existingId;
  for (const CredentialMetadata &record : vault_->forOrigin(view->url())) {
    if (record.vaultId == vault_->activeVaultId() && record.username == candidate.username) {
      existingId = record.id; break;
    }
  }
  const QString promptKey = existingId.isEmpty() ? QStringLiteral("passwords.save_prompt_new")
                                                  : QStringLiteral("passwords.save_prompt_update");
  if (QMessageBox::question(dialogParent_, I18n::text(QStringLiteral("toolbar.passwords")),
                            I18n::text(promptKey), QMessageBox::Yes | QMessageBox::No,
                            QMessageBox::No) == QMessageBox::Yes) {
    const CredentialSecret secret{candidate.origin, candidate.username, candidate.password, {}};
    if (existingId.isEmpty()) vault_->save(secret); else vault_->update(existingId, secret);
  }
  clearView(view);
  refreshFillButton(view);
}

void WebKitCredentialController::fill(WebKitEngineView *view, const QString &token) {
  if (!view || token.isEmpty() || fillTokens_.value(view) != token) return;
  const QString origin = CredentialVaultManager::canonicalHttpsOrigin(view->url());
  const quint64 generation = generations_.value(view);
  QVector<VaultMetadata> vaults = vault_->vaultsForOrigin(view->url());
  if (origin.isEmpty() || vaults.isEmpty()) return;
  int vaultIndex = 0;
  if (vaults.size() > 1) {
    QStringList names; for (const VaultMetadata &metadata : vaults) names.append(metadata.name);
    bool ok = false; const QString selected = QInputDialog::getItem(
        dialogParent_, I18n::text(QStringLiteral("passwords.choose_vault")),
        I18n::text(QStringLiteral("passwords.choose_vault")), names, 0, false, &ok);
    if (!ok) return;
    vaultIndex = names.indexOf(selected);
    if (vaultIndex < 0) return;
  }
  const QString vaultId = vaults.at(vaultIndex).id;
  VaultUnlockDialog unlock(vault_, origin, dialogParent_, VaultUnlockDialog::Purpose::ReauthenticateForAutofill,
                           vaultId);
  if (unlock.exec() != QDialog::Accepted || generation != generations_.value(view) ||
      fillTokens_.value(view) != token || CredentialVaultManager::canonicalHttpsOrigin(view->url()) != origin) return;
  QVector<CredentialMetadata> records;
  for (const CredentialMetadata &record : vault_->forOrigin(view->url()))
    if (record.vaultId == vaultId) records.append(record);
  if (records.isEmpty()) return;
  int recordIndex = 0;
  if (records.size() > 1) {
    QStringList names; for (const CredentialMetadata &record : records) names.append(record.username);
    bool ok = false; const QString selected = QInputDialog::getItem(
        dialogParent_, I18n::text(QStringLiteral("passwords.choose_account")),
        I18n::text(QStringLiteral("passwords.choose_account")), names, 0, false, &ok);
    if (!ok) return;
    recordIndex = names.indexOf(selected);
    if (recordIndex < 0) return;
  }
  CredentialSecret secret;
  if (!vault_->reveal(records.at(recordIndex).id, &secret)) return;
  view->runJavaScript(CredentialAutofillController::domFillScript(
      origin, secret.username, secret.password, view->url(), token));
  secret.wipe();
  fillTokens_.remove(view);
}

#else
WebKitCredentialController::WebKitCredentialController(CredentialVaultManager *, QWidget *, QObject *parent)
    : QObject(parent) {}
void WebKitCredentialController::registerView(dalinira::engine::WebKitEngineView *) {}
void WebKitCredentialController::unregisterView(dalinira::engine::WebKitEngineView *) {}
#endif
