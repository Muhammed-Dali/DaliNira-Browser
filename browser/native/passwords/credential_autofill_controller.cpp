#include "credential_autofill_controller.h"
#include "credential_vault_manager.h"
#include "vault_unlock_dialog.h"

#include <libpsl.h>

#include <QApplication>
#include <QComboBox>
#include <QFutureWatcher>
#include <QFile>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLayout>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QUuid>
#include <QWebEnginePage>
#include <QWebEngineView>
#include <QtConcurrent>
#include "i18n/i18n.h"

static QString registrableDomain(const QString &host) {
  QString h = host.trimmed().toLower();
  if (h.isEmpty()) return {};
  if (h.endsWith(QLatin1Char('.'))) h.chop(1);
  const QByteArray ace = QUrl::toAce(h);
  const char *registrable = psl_registrable_domain(psl_builtin(), ace.constData());
  return registrable ? QString::fromUtf8(registrable).toLower() : QString();
}

static bool isExplicitLoginFailureUrl(const QUrl &url) {
  const QString path = url.path().toLower();
  const QString query = url.query().toLower();
  return query.contains(QStringLiteral("error=")) ||
         query.contains(QStringLiteral("error_code=")) ||
         query.contains(QStringLiteral("login_error")) ||
         query.contains(QStringLiteral("login_failed")) ||
         query.contains(QStringLiteral("failed=1")) ||
         query.contains(QStringLiteral("failed=true")) ||
         query.contains(QStringLiteral("incorrect_password")) ||
         query.contains(QStringLiteral("invalid_credentials")) ||
         path.contains(QStringLiteral("/auth_failure")) ||
         path.contains(QStringLiteral("/login_failure")) ||
         path.endsWith(QStringLiteral("/error"));
}

bool CredentialAutofillController::isSameSiteOrOrigin(const QString &origin1, const QString &origin2) {
  if (origin1.isEmpty() || origin2.isEmpty()) return false;
  if (origin1 == origin2) return true;
  const QUrl u1(origin1);
  const QUrl u2(origin2);
  if (!u1.isValid() || !u2.isValid()) return false;
  if (u1.scheme().compare(u2.scheme(), Qt::CaseInsensitive) != 0) return false;
  if (u1.port() != u2.port()) return false;
  const QString h1 = u1.host().toLower();
  const QString h2 = u2.host().toLower();
  if (h1 == h2) return true;
  const QString d1 = registrableDomain(h1);
  const QString d2 = registrableDomain(h2);
  return (!d1.isEmpty() && d1 == d2);
}

bool CredentialAutofillController::isLoginOrErrorUrl(const QUrl &url) {
  if (!url.isValid()) return false;
  const QString path = url.path().toLower();

  const bool isLoginPath = (path == QStringLiteral("/login") ||
                            path == QStringLiteral("/login/") ||
                            path == QStringLiteral("/login.php") ||
                            path == QStringLiteral("/signin") ||
                            path == QStringLiteral("/signin/") ||
                            path == QStringLiteral("/session/new") ||
                            path == QStringLiteral("/sessions/new") ||
                            path.endsWith(QStringLiteral("/login")) ||
                            path.endsWith(QStringLiteral("/signin")) ||
                            path.endsWith(QStringLiteral("/login.php")) ||
                            path.contains(QStringLiteral("/login/")) ||
                            path.contains(QStringLiteral("/signin/")));

  const bool isIntermediateAuthPath =
      path.contains(QStringLiteral("/checkpoint")) ||
      path.contains(QStringLiteral("/challenge")) ||
      path.contains(QStringLiteral("/two-factor")) ||
      path.contains(QStringLiteral("/2fa")) ||
      path.contains(QStringLiteral("/verify")) ||
      path.contains(QStringLiteral("/oauth/")) ||
      path.contains(QStringLiteral("/auth/continue")) ||
      path.contains(QStringLiteral("/auth/callback"));

  return isLoginPath || isExplicitLoginFailureUrl(url) || isIntermediateAuthPath;
}

CredentialAutofillController::CredentialAutofillController(CredentialVaultManager *vaultManager,
                                                           QWidget *dialogParent,
                                                           QObject *parent)
    : QObject(parent), vaultManager_(vaultManager), dialogParent_(dialogParent) {
  confirmationTimer_.setInterval(kConfirmationPollIntervalMs);
  confirmationTimer_.setSingleShot(false);
  connect(&confirmationTimer_, &QTimer::timeout, this, &CredentialAutofillController::pollLoginAttempts);
  if (vaultManager_) {
    connect(vaultManager_, &CredentialVaultManager::lockStateChanged, this, [this](bool locked) {
      if (locked) onVaultLocked();
      refreshAutofillForViews();
    });
    connect(vaultManager_, &CredentialVaultManager::changed, this, &CredentialAutofillController::refreshAutofillForViews);
  }
}

CredentialAutofillController::~CredentialAutofillController() {
  clearAllSensitiveData();
}

QString CredentialAutofillController::stageKey(QWebEngineView *view, const QString &origin) const {
  return QString::number(reinterpret_cast<quintptr>(view)) + QLatin1Char(':') + origin;
}

QString CredentialAutofillController::candidateKey(QWebEngineView *view, const QString &origin, const QString &username) const {
  return stageKey(view, origin) + QLatin1Char(':') + username;
}

QString CredentialAutofillController::activeTokenForView(QWebEngineView *view, const QString &origin) const {
  return credentialFillTokens_.value(stageKey(view, origin));
}

QString CredentialAutofillController::grantFillToken(QWebEngineView *view, const QString &origin) {
  if (!view || origin.isEmpty()) return {};
  const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
  credentialFillTokens_.insert(stageKey(view, origin), token);
  return token;
}

bool CredentialAutofillController::isCandidatePending(QWebEngineView *view, const QString &origin, const QString &username) const {
  return pendingCandidates_.contains(candidateKey(view, origin, username));
}

VaultUnlockDialog *CredentialAutofillController::activeUnlockDialog() const {
  return activeUnlockDialog_.data();
}

void CredentialAutofillController::pruneExpiredData() {
  if (isPruning_) return;
  isPruning_ = true;
  const QDateTime now = QDateTime::currentDateTimeUtc();
  for (auto it = stagedUsernames_.begin(); it != stagedUsernames_.end();) {
    if (it->expiresAt <= now) {
      it = stagedUsernames_.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = pendingCandidates_.begin(); it != pendingCandidates_.end();) {
    const bool isActivelyPrompted = (it.key() == activeBubbleCandidateKey_ &&
                                     activeSaveBubble_ != nullptr);
    if (!isActivelyPrompted && it->expiresAt <= now) {
      it->password.fill(QChar());
      it = pendingCandidates_.erase(it);
    } else {
      ++it;
    }
  }
  QStringList expiredAttempts;
  for (auto it = loginAttempts_.cbegin(); it != loginAttempts_.cend(); ++it) {
    if (it->submittedAt.addSecs(kLoginAttemptTimeoutSecs) <= now) expiredAttempts.append(it.key());
  }
  for (const QString &key : expiredAttempts) {
    cancelPendingSaveFlow(key, QStringLiteral("timeout"));
  }
  stopConfirmationTimerIfIdle();
  isPruning_ = false;
}

void CredentialAutofillController::clearAllSensitiveData() {
  for (auto &candidate : pendingCandidates_) {
    candidate.password.fill(QChar());
  }
  pendingCandidates_.clear();
  loginAttempts_.clear();
  pendingSaveFlows_.clear();
  confirmationTimer_.stop();
  stagedUsernames_.clear();
  credentialFillTokens_.clear();
  activePrompts_.clear();
  if (activeUnlockDialog_) {
    activeUnlockDialog_->reject();
  }
  clearPendingFill();
  dismissSaveBubble();
}

bool CredentialAutofillController::isFillButtonActiveForView(QWebEngineView *view, const QString &origin) const {
  return !activeTokenForView(view, origin).isEmpty();
}

bool CredentialAutofillController::isViewTracked(QWebEngineView *view) const {
  if (!view) return false;
  for (const auto &p : activeViews_) {
    if (p.data() == view) return true;
  }
  return false;
}

void CredentialAutofillController::trackView(QWebEngineView *view) {
  if (!view) return;
  for (const auto &p : activeViews_) {
    if (p.data() == view) return;
  }
  activeViews_.append(view);
  connect(view, &QObject::destroyed, this, [this, view] {
    activeViews_.removeAll(view);
    onViewClosed(view);
  });
}

void CredentialAutofillController::removeFillButton(QWebEngineView *view) {
  if (!view) return;
  const QString prefix = QString::number(reinterpret_cast<quintptr>(view)) + QLatin1Char(':');
  for (auto it = credentialFillTokens_.begin(); it != credentialFillTokens_.end();) {
    if (it.key().startsWith(prefix)) {
      it = credentialFillTokens_.erase(it);
    } else {
      ++it;
    }
  }
  if (isViewTracked(view) && view->page()) {
    static const QString kRemoveScript = QStringLiteral(R"JS((() => {
      try {
        if (typeof window.__daliniraRemoveFillButton === 'function') {
          window.__daliniraRemoveFillButton();
        }
        const btn = document.querySelector('[data-dalinira-autofill-btn="true"]');
        if (btn) btn.remove();
        window.__daliniraFillButtonInstalled = false;
        window.__daliniraFillToken = null;
      } catch (_) {}
    })())JS");
    view->page()->runJavaScript(kRemoveScript, QWebEngineScript::ApplicationWorld);
  }
}

void CredentialAutofillController::refreshAutofillForViews() {
  for (int i = activeViews_.size() - 1; i >= 0; --i) {
    QWebEngineView *view = activeViews_.at(i).data();
    if (!view) {
      activeViews_.removeAt(i);
      continue;
    }
    const QString origin = CredentialVault::canonicalHttpsOrigin(view->url());
    if (origin.isEmpty() || !vaultManager_ || !vaultManager_->hasMatchingCredential(view->url())) {
      removeFillButton(view);
    } else {
      installFillButton(view);
    }
  }
}

void CredentialAutofillController::onVaultLocked() {
  for (auto &candidate : pendingCandidates_) {
    candidate.password.fill(QChar());
  }
  pendingCandidates_.clear();
  loginAttempts_.clear();
  pendingSaveFlows_.clear();
  confirmationTimer_.stop();
  stagedUsernames_.clear();
  activePrompts_.clear();
  if (activeUnlockDialog_) {
    activeUnlockDialog_->reject();
  }
  clearPendingFill();
  dismissSaveBubble();
}

void CredentialAutofillController::onViewClosed(QWebEngineView *view) {
  if (!view) return;
  const QString prefix = QString::number(reinterpret_cast<quintptr>(view)) + QLatin1Char(':');
  const auto flowKeys = pendingSaveFlows_.keys();
  for (const QString &key : flowKeys) {
    if (key.startsWith(prefix)) cancelPendingSaveFlow(key, QStringLiteral("tab_close"));
  }
  if (activeSaveBubble_ && (activeBubbleCandidateKey_.startsWith(prefix) || activeBubbleView_ == view)) {
    dismissSaveBubble();
  }

  const bool tracked = isViewTracked(view);
  if (tracked) {
    activeViews_.removeAll(view);
    removeFillButton(view);
  } else {
    for (auto it = credentialFillTokens_.begin(); it != credentialFillTokens_.end();) {
      if (it.key().startsWith(prefix)) it = credentialFillTokens_.erase(it); else ++it;
    }
  }
  if (pendingFill_.view == view) {
    clearPendingFill();
    if (activeUnlockDialog_) {
      activeUnlockDialog_->reject();
    }
  }
  navigationGenerations_.remove(view);
  for (auto it = stagedUsernames_.begin(); it != stagedUsernames_.end();) {
    if (it.key().startsWith(prefix)) it = stagedUsernames_.erase(it); else ++it;
  }
  for (auto it = pendingCandidates_.begin(); it != pendingCandidates_.end();) {
    if (it.key().startsWith(prefix)) {
      it->password.fill(QChar());
      it = pendingCandidates_.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = loginAttempts_.begin(); it != loginAttempts_.end();) {
    if (it.key().startsWith(prefix)) {
      it = loginAttempts_.erase(it);
    } else {
      ++it;
    }
  }
  stopConfirmationTimerIfIdle();
}

void CredentialAutofillController::onUrlChanged(QWebEngineView *view, const QUrl &url) {
  if (!view) return;
  navigationGenerations_[view] = navigationGenerations_.value(view) + 1;
  const QString origin = CredentialVault::canonicalHttpsOrigin(url);
  const QString prefix = QString::number(reinterpret_cast<quintptr>(view)) + QLatin1Char(':');
  const auto flowKeys = pendingSaveFlows_.keys();
  for (const QString &flowKey : flowKeys) {
    const auto flowIt = pendingSaveFlows_.constFind(flowKey);
    if (flowIt != pendingSaveFlows_.cend() && flowKey.startsWith(prefix) &&
        !isSameSiteOrOrigin(flowIt->origin, origin)) {
      cancelPendingSaveFlow(flowKey, QStringLiteral("unrelated_navigation"));
    }
  }
  // Any navigation (including same-origin SPA changes) invalidates a pending
  // secret release.  A fresh user gesture must start a fresh authorization.
  if (pendingFill_.view == view) {
    clearPendingFill();
    if (activeUnlockDialog_) {
      activeUnlockDialog_->reject();
    }
  }

  if (activeSaveBubble_ && activeBubbleView_ == view) {
    if (activeSaveBubble_->mode() == CredentialSaveMode::Verifying) {
      promptCandidate(view, activeBubbleCandidateKey_);
    }
  }

  if (isViewTracked(view) && (origin.isEmpty() || !vaultManager_ || !vaultManager_->hasMatchingCredential(url))) {
    removeFillButton(view);
  }

  // Record navigation as evidence, but preserve the attempt across same-site
  // redirects and reloads.  A same-URL load is deliberately not sufficient by
  // itself; the bounded DOM recheck confirms that flow.
  if (!origin.isEmpty()) {
    if (LoginAttemptState *attempt = findActiveSubmittedAttempt(view, origin)) {
      attempt->navigationObserved = true;
      attempt->lastObservedUrl = url;
      if (isExplicitLoginFailureUrl(url)) {
        attempt->errorStateObserved = true;
      }
      evaluateLoginAttempt(view, origin);
    }
  }

  // STEP 2: STALE STATE CLEANUP (ONLY FOR UNRELATED SITES)
  // Remove fill token for this view if origin changed
  const QString key = stageKey(view, origin);
  for (auto it = credentialFillTokens_.begin(); it != credentialFillTokens_.end();) {
    if (it.key().startsWith(prefix) && it.key() != key) {
      it = credentialFillTokens_.erase(it);
    } else {
      ++it;
    }
  }
  // Clear candidates only if they belong to a completely different site (not same-site)
  // and are not actively presented to the user for a save decision.
  for (auto it = pendingCandidates_.begin(); it != pendingCandidates_.end();) {
    const bool isActivelyPrompted = (it.key() == activeBubbleCandidateKey_ && activeSaveBubble_ != nullptr);
    if (!isActivelyPrompted && it.key().startsWith(prefix) && !isSameSiteOrOrigin(it->origin, origin)) {
      it->password.fill(QChar());
      it = pendingCandidates_.erase(it);
    } else {
      ++it;
    }
  }
  // Clear any attempts that do not match the new origin (not same-site)
  for (auto it = loginAttempts_.begin(); it != loginAttempts_.end();) {
    if (it.key().startsWith(prefix) && !isSameSiteOrOrigin(it->origin, origin)) {
      it = loginAttempts_.erase(it);
    } else {
      ++it;
    }
  }
  stopConfirmationTimerIfIdle();
  for (auto it = stagedUsernames_.begin(); it != stagedUsernames_.end();) {
    if (it.key().startsWith(prefix) && !isSameSiteOrOrigin(it.key().mid(prefix.length()), origin)) {
      it = stagedUsernames_.erase(it);
    } else {
      ++it;
    }
  }
}

void CredentialAutofillController::onPageLoadFinished(QWebEngineView *view, bool success) {
  if (!view || !success) return;
  trackView(view);
  pruneExpiredData();

  if (activeSaveBubble_ && activeBubbleView_ == view) {
    if (activeSaveBubble_->mode() == CredentialSaveMode::Verifying) {
      promptCandidate(view, activeBubbleCandidateKey_);
    }
  }

  const QString origin = CredentialVault::canonicalHttpsOrigin(view->url());
  if (origin.isEmpty()) {
    removeFillButton(view);
    return;
  }

  if (LoginAttemptState *attempt = findActiveSubmittedAttempt(view, origin)) {
    attempt->pageLoadObserved = true;
    attempt->lastObservedUrl = view->url();
    if (isExplicitLoginFailureUrl(view->url())) {
      attempt->errorStateObserved = true;
    }
    evaluateLoginAttempt(view, origin);
  }

  if (!vaultManager_ || !vaultManager_->hasMatchingCredential(view->url())) {
    removeFillButton(view);
    return;
  }

  installFillButton(view);
}

void CredentialAutofillController::installFillButton(QWebEngineView *view) {
  if (!view) return;
  trackView(view);
  const QString origin = CredentialVault::canonicalHttpsOrigin(view->url());
  if (origin.isEmpty() || !vaultManager_ || !vaultManager_->hasMatchingCredential(view->url())) {
    removeFillButton(view);
    return;
  }

  const QString token = grantFillToken(view, origin);
  if (token.isEmpty()) return;

  // Injected into ApplicationWorld so the host page's scripts cannot tamper with or inspect the closure
  if (view->page()) {
    view->page()->runJavaScript(fillButtonScript(token), QWebEngineScript::ApplicationWorld);
  }
}

QWebEngineScript CredentialAutofillController::candidateCaptureScript() {
  QWebEngineScript script;
  script.setName(QStringLiteral("dalinira-credential-candidate-capture"));
  script.setWorldId(QWebEngineScript::ApplicationWorld);
  script.setInjectionPoint(QWebEngineScript::DocumentCreation);
  script.setRunsOnSubFrames(false);
  QFile webChannelSource(QStringLiteral(":/qtwebchannel/qwebchannel.js"));
  if (!webChannelSource.open(QIODevice::ReadOnly)) return script;
  script.setSourceCode(QString::fromUtf8(webChannelSource.readAll()) + QStringLiteral(R"JS(
  (() => {
    if (window.top !== window || window.__daliniraCredentialCaptureInstalled) return;
    if (typeof QWebChannel !== 'function' || !window.qt || !qt.webChannelTransport) return;
    try {
      Object.defineProperty(window, '__daliniraCredentialCaptureInstalled', { value: true });
    } catch (_) {}

    const bridgeQueue = [];
    let nativeBridge = null;
    window.__daliniraCredentialSend = (type, payload) => {
      if (nativeBridge) nativeBridge.send(type, payload);
      else if (bridgeQueue.length < 16) bridgeQueue.push([type, payload]);
    };
    new QWebChannel(qt.webChannelTransport, channel => {
      nativeBridge = channel.objects.daliniraCredentials || null;
      if (!nativeBridge) return;
      for (const [type, payload] of bridgeQueue.splice(0)) nativeBridge.send(type, payload);
    });

    const visible = el => {
      if (!el || !el.isConnected) return false;
      const r = el.getBoundingClientRect();
      if (!r.width || !r.height) return false;
      const s = getComputedStyle(el);
      return el.type !== 'hidden' && !el.disabled && !el.readOnly &&
             s.display !== 'none' && s.visibility !== 'hidden' && Number(s.opacity || 1) > 0;
    };

    const findUsername = (root, pwd) => {
      const candidates = Array.from((root || document).querySelectorAll('input')).filter(
        e => e !== pwd && visible(e) && !e.disabled && !e.readOnly && /^(text|email|tel)$/i.test(e.type || 'text')
      );
      // Priority scoring: 1. autocomplete username/email, 2. name/id hint, 3. closest preceding input
      let best = null;
      let bestScore = -1;
      for (const input of candidates) {
        const val = String(input.value || '').trim();
        if (!val || val.length > 320) continue;
        let score = 0;
        const autoC = (input.autocomplete || '').toLowerCase();
        const hint = (input.name + ' ' + input.id + ' ' + (input.placeholder || '')).toLowerCase();
        if (autoC.includes('username') || autoC.includes('email')) score += 100;
        if (/user|email|login|account|identifier|eposta|telefon/.test(hint)) score += 50;
        if (score > bestScore) {
          bestScore = score;
          best = input;
        }
      }
      if (!best && candidates.length > 0) {
        // Fallback to the closest preceding visible text/email field
        const filled = candidates.filter(e => String(e.value || '').trim().length > 0);
        if (filled.length > 0) best = filled[filled.length - 1];
      }
      return best;
    };

    let lastCandidateAt = 0;
    let lastUsername = '';
    let loginSubmitted = false;
    let loginSubmittedAt = 0;
    let attemptNonce = '';
    let confirmationTimer = null;

    const isEyeOrToggle = el => {
      if (!el) return false;
      const target = el.closest ? el.closest('button, [role="button"], a, span, div') : el;
      if (!target) return false;
      const combined = `${target.className || ''} ${target.id || ''} ${target.getAttribute('aria-label') || ''} ${target.title || ''} ${target.getAttribute('data-testid') || ''}`.toLowerCase();
      if (/eye|reveal|show[-_]?pass|hide[-_]?pass|toggle[-_]?pass|password[-_]?toggle|visibility|g[oö]ster|gizle/.test(combined)) {
        return true;
      }
      const text = (target.textContent || '').trim().toLowerCase();
      if (/^(show|hide|göster|gizle|parolayı göster|şifreyi göster)$/.test(text)) {
        return true;
      }
      return false;
    };

    const isPasswordChangeForm = formOrScope => {
      if (!formOrScope || !formOrScope.querySelectorAll) return false;
      const pwds = Array.from(formOrScope.querySelectorAll(
        'input[type="password"], input[autocomplete="current-password"], input[autocomplete="new-password"]'
      )).filter(visible);
      if (pwds.some(p => (p.autocomplete || '').toLowerCase().includes('new-password'))) return true;

      // A page may contain separate desktop/mobile login forms at the same time.
      // Only multiple visible password controls in the same local form/container
      // indicate a password-change flow; counting the whole document caused real
      // login clicks on responsive sites to be discarded.
      const groups = new Map();
      for (const pwd of pwds) {
        const container = pwd.form || pwd.closest('form, [role="form"], [role="dialog"], fieldset') || pwd.parentElement;
        groups.set(container, (groups.get(container) || 0) + 1);
      }
      return Array.from(groups.values()).some(count => count >= 2);
    };

    const isSubmitControl = el => {
      if (!el || !el.closest) return false;
      const control = el.closest('button, input[type="submit"], input[type="button"], [role="button"]');
      if (!control) return false;
      if (isEyeOrToggle(control)) return false;

      const text = `${control.className || ''} ${control.id || ''} ${control.textContent || ''} ${control.value || ''} ${control.getAttribute('aria-label') || ''}`.toLowerCase();
      if (/eye|reveal|show|hide|cancel|vazge[çc]|reset|temizle|help|yard[ıi]m|close|kapat/.test(text)) {
        return false;
      }

      if (control instanceof HTMLInputElement) {
        return control.type === 'submit' || /log[-_]?in|sign[-_]?in|giri[şs]|submit|devam|next|continue/.test(text);
      }
      if (control instanceof HTMLButtonElement) {
        if (control.type === 'button' && !/log[-_]?in|sign[-_]?in|giri[şs]|submit|devam|next|continue/.test(text)) {
          return false;
        }
        return true;
      }
      return /log[-_]?in|sign[-_]?in|giri[şs]|submit|enter|devam|next|continue/.test(text);
    };

    const stageUsername = value => {
      const username = String(value || '').trim();
      if (!username || username.length > 320 || username === lastUsername) return;
      lastUsername = username;
      try {
        window.__daliniraCredentialSend('stage', { origin: location.origin, username });
      } catch (_) {}
    };

    const capture = (scope, isSubmit = false) => {
      try {
        const now = Date.now();
        if (!isSubmit && now - lastCandidateAt < 800) return;
        const root = (scope && typeof scope.querySelectorAll === 'function') ? scope : document;
        const pwdFields = Array.from(root.querySelectorAll(
          'input[type="password"], input[autocomplete="current-password"]'
        )).filter(e => visible(e) && e.value);
        if (!pwdFields.length) return;
        const pwd = pwdFields[0];
        const searchScope = pwd.form || pwd.closest('form, [role="form"], [role="dialog"], fieldset') || root;
        if (isPasswordChangeForm(searchScope)) return;

        const usernameField = findUsername(searchScope, pwd);
        const username = String(usernameField?.value || lastUsername || '').trim();
        const password = String(pwd.value || '');
        if (!password || password.length > 4096) return;
        if (username) stageUsername(username);
        lastCandidateAt = now;
        window.__daliniraCredentialSend('candidate', {
          origin: location.origin,
          username: username.slice(0, 320),
          password: password.slice(0, 4096),
          submitted: isSubmit,
          nonce: attemptNonce
        });
      } catch (_) {}
    };

    const triggerSubmitIntent = (scope) => {
      if (!scope) return;
      if (isPasswordChangeForm(scope)) return;
      const now = Date.now();
      loginSubmitted = true;
      loginSubmittedAt = now;
      attemptNonce = Math.random().toString(36).slice(2) + Date.now().toString(36);
      capture(scope, true);
      try {
        window.__daliniraCredentialSend('submit', {
          origin: location.origin,
          nonce: attemptNonce
        });
      } catch (_) {}
      clearInterval(confirmationTimer);
      confirmationTimer = setInterval(observeSuccess, 400);
    };

    const successHint = () => {
      if (!loginSubmitted || Date.now() - loginSubmittedAt > 12000) return;
      try {
        window.__daliniraCredentialSend('success', {
          origin: location.origin,
          loginFormVisible: false,
          passwordFieldVisible: false,
          errorStateObserved: false
        });
      } catch (_) {}
      loginSubmitted = false;
      clearInterval(confirmationTimer);
      confirmationTimer = null;
    };

    let observeSuccessTimer = null;
    function observeSuccess() {
      if (!loginSubmitted) return;
      if (observeSuccessTimer) return;
      observeSuccessTimer = setTimeout(() => {
        observeSuccessTimer = null;
        if (!loginSubmitted) return;
        if (Date.now() - loginSubmittedAt > 12000) {
          loginSubmitted = false;
          clearInterval(confirmationTimer);
          confirmationTimer = null;
          return;
        }
        const pwdFields = Array.from(document.querySelectorAll('input[type="password"]'));
        if (pwdFields.some(visible)) return;

        const allInputs = Array.from(document.querySelectorAll('input'));
        const loginInputStillVisible = allInputs.some(inp => visible(inp) && /password|şifre|parola/i.test(inp.name + ' ' + inp.id + ' ' + inp.placeholder));
        if (loginInputStillVisible) return;

        successHint();
      }, 300);
    }

    document.addEventListener('submit', event => {
      if (!event.isTrusted) return;
      if (isPasswordChangeForm(event.target)) return;
      triggerSubmitIntent(event.target);
    }, true);

    document.addEventListener('input', event => {
      if (!event.isTrusted) return;
      const input = event.target;
      if (!(input instanceof HTMLInputElement) || input.type === 'password') return;
      const hint = `${input.type || ''} ${input.autocomplete || ''} ${input.name || ''} ${input.id || ''}`.toLowerCase();
      if (/email|user|login|identifier|eposta|account/.test(hint)) stageUsername(input.value);
    }, true);

    document.addEventListener('change', event => {
      if (!event.isTrusted) return;
      const input = event.target;
      if (input instanceof HTMLInputElement && input.type === 'password' && input.value) {
        capture(input.form || input.closest('form, [role="dialog"]') || document, false);
      }
    }, true);

    document.addEventListener('click', event => {
      if (!event.isTrusted) return;
      const path = typeof event.composedPath === 'function' ? event.composedPath() : [event.target];
      const target = path.find(node => node && node.nodeType === Node.ELEMENT_NODE &&
        (isEyeOrToggle(node) || isSubmitControl(node))) || event.target;
      if (isEyeOrToggle(target)) return;
      if (isSubmitControl(target)) {
        const scope = target.form || target.closest('form, [role="form"], [role="dialog"], fieldset') || document;
        triggerSubmitIntent(scope);
      }
    }, true);

    document.addEventListener('keydown', event => {
      if (!event.isTrusted || event.key !== 'Enter') return;
      const target = event.target;
      if (target instanceof HTMLInputElement && (target.type === 'password' || target.type === 'email' || target.type === 'text')) {
        const scope = target.form || target.closest('form, [role="dialog"]') || document;
        triggerSubmitIntent(scope);
      }
    }, true);

    new MutationObserver(observeSuccess).observe(document, {
      childList: true, subtree: true, attributes: true, attributeFilter: ['style', 'class', 'hidden']
    });
    window.addEventListener('popstate', observeSuccess, true);
    for (const name of ['pushState', 'replaceState']) {
      const original = history[name];
      if (typeof original !== 'function') continue;
      history[name] = function(...args) {
        const res = original.apply(this, args);
        observeSuccess();
        return res;
      };
    }
  })())JS"));
  return script;
}

QString CredentialAutofillController::webKitCandidateCaptureSource() {
  const QString chromiumSource = candidateCaptureScript().sourceCode();
  const qsizetype body = chromiumSource.indexOf(QStringLiteral("    const visible = el =>"));
  if (body < 0) return {};
  return QStringLiteral(R"JS((() => {
    if (window.top !== window || window.__daliniraCredentialCaptureInstalled) return;
    const handler = window.webkit && window.webkit.messageHandlers &&
                    window.webkit.messageHandlers.daliniraCredentials;
    if (!handler) return;
    try { Object.defineProperty(window, '__daliniraCredentialCaptureInstalled', { value: true }); }
    catch (_) { return; }
    window.__daliniraCredentialSend = (type, payload) => {
      try { handler.postMessage({ type, payload }); } catch (_) {}
    };
)JS") + chromiumSource.mid(body);
}

namespace {
// JSON-encoded string literal safe to splice into injected JavaScript.
QString jsStringLiteral(const QString &value) {
  QString json = QString::fromUtf8(QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact));
  json = json.mid(1, json.size() - 2);
  json.replace(QLatin1String("</"), QLatin1String("<\\/"));
  return json;
}
}  // namespace

QString CredentialAutofillController::fillButtonScript(const QString &token) {
  const QString payload = QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("token"), token}}).toJson(QJsonDocument::Compact));
  return QStringLiteral(R"JS((() => {
    if (window.top !== window) return;

    const payload = %1;
    window.__daliniraFillToken = payload.token;

    if (window.__daliniraFillButtonInstalled) {
      return;
    }
    window.__daliniraFillButtonInstalled = true;

    let managedButton = null;
    let currentTarget = null;
    let isInteracting = false;
    let hideTimer = null;

    window.__daliniraRemoveFillButton = () => {
      clearTimeout(hideTimer);
      if (managedButton) {
        managedButton.remove();
        managedButton = null;
      }
      currentTarget = null;
      window.__daliniraFillButtonInstalled = false;
      window.__daliniraFillToken = null;
    };

    const visible = el => {
      if (!el || !el.isConnected) return false;
      const r = el.getBoundingClientRect();
      if (!r.width || !r.height) return false;
      const s = getComputedStyle(el);
      return el.type !== 'hidden' && !el.disabled && !el.readOnly &&
             s.display !== 'none' && s.visibility !== 'hidden' && Number(s.opacity || 1) > 0;
    };

    const localScope = input => input.form ||
      input.closest('form, [role="form"], [role="dialog"], fieldset, .login-container') || document;
    const isPasswordField = input => input instanceof HTMLInputElement &&
      (input.type === 'password' || (input.autocomplete || '').toLowerCase() === 'current-password');
    const isLoginIdentifier = input => {
      if (!(input instanceof HTMLInputElement) || !visible(input) ||
          !/^(text|email|tel)$/i.test(input.type || 'text')) return false;
      const autocomplete = (input.autocomplete || '').toLowerCase();
      const hint = `${input.name || ''} ${input.id || ''} ${input.placeholder || ''} ${input.getAttribute('aria-label') || ''}`.toLowerCase();
      const stronglyIdentified = autocomplete.includes('username') || autocomplete.includes('email') ||
        /(^|[^a-z])(user(name)?|email|e[- ]?posta|login|account|identifier|telefon|phone)([^a-z]|$)/.test(hint);
      if (!stronglyIdentified) return false;
      return Array.from(localScope(input).querySelectorAll(
        'input[type="password"], input[autocomplete="current-password"]'
      )).some(visible);
    };
    const isCredentialField = input => visible(input) && (isPasswordField(input) || isLoginIdentifier(input));

    // Responsive offset calculation: Avoid overlapping with existing site eye icons or buttons inside the field
    const computeRightOffset = (field) => {
      const pRect = field.getBoundingClientRect();
      const rightEdge = pRect.right;
      let detectedOffset = 0;
      const parent = field.parentElement;
      if (parent) {
        for (const child of parent.children) {
          if (child !== field && child.nodeType === 1) {
            const cRect = child.getBoundingClientRect();
            // Check if sibling button or icon sits at the right end of the password input
            if (cRect.width > 0 && cRect.height > 0 && cRect.right > rightEdge - 50 && cRect.left < rightEdge) {
              const overlap = (rightEdge - cRect.left) + 4;
              if (overlap > detectedOffset) detectedOffset = overlap;
            }
          }
        }
      }
      const pr = parseInt(getComputedStyle(field).paddingRight, 10) || 0;
      if (pr >= 32 && detectedOffset === 0) detectedOffset = pr;
      return Math.max(6, detectedOffset + 4);
    };

    const hideButton = () => {
      if (managedButton) {
        managedButton.style.display = 'none';
      }
      currentTarget = null;
    };

    const updatePosition = () => {
      if (!managedButton || !currentTarget || !visible(currentTarget) || managedButton.style.display === 'none') {
        return;
      }
      const r = currentTarget.getBoundingClientRect();
      if (r.width < 40 || r.height < 10) return;
      const offset = computeRightOffset(currentTarget);
      const left = Math.max(r.left + 10, r.right - offset - 26);
      const top = r.top + (r.height - 26) / 2;
      managedButton.style.left = `${left}px`;
      managedButton.style.top = `${top}px`;
    };

    const showButtonForField = (field) => {
      if (!window.__daliniraFillButtonInstalled || !isCredentialField(field)) return;
      currentTarget = field;
      if (!managedButton) {
        managedButton = document.createElement('button');
        managedButton.type = 'button';
        managedButton.textContent = '🔑';
        managedButton.title = %2;
        managedButton.setAttribute('aria-label', managedButton.title);
        managedButton.setAttribute('data-dalinira-autofill-btn', 'true');
        Object.assign(managedButton.style, {
          position: 'fixed',
          zIndex: '2147483647',
          pointerEvents: 'auto',
          userSelect: 'none',
          width: '26px',
          height: '26px',
          padding: '0',
          border: '1px solid #334a66',
          borderRadius: '6px',
          background: '#172235',
          color: '#f0c040',
          cursor: 'pointer',
          fontSize: '14px',
          lineHeight: '24px',
          textAlign: 'center',
          boxShadow: '0 2px 6px rgba(0,0,0,0.35)',
          transition: 'transform 0.08s ease, background 0.15s ease',
          display: 'none'
        });

        managedButton.addEventListener('mouseenter', () => {
          isInteracting = true;
          managedButton.style.background = '#22344d';
          managedButton.style.borderColor = '#4f729e';
        });
        managedButton.addEventListener('mouseleave', () => {
          isInteracting = false;
          managedButton.style.background = '#172235';
          managedButton.style.borderColor = '#334a66';
        });

        // USER GESTURE ENFORCEMENT:
        // Reject synthetic clicks, dispatchEvent, or untrusted events!
        let lastTriggerAt = 0;
        const triggerAutofill = (event) => {
          isInteracting = true;
          clearTimeout(hideTimer);
          event.preventDefault();
          event.stopImmediatePropagation();
          if (!event.isTrusted) return;
          if (typeof navigator.userActivation !== 'undefined' && navigator.userActivation.isActive === false) return;
          const now = Date.now();
          if (now - lastTriggerAt < 400) return;
          lastTriggerAt = now;
          try {
            const activeTok = window.__daliniraFillToken || payload.token;
            if (typeof window.__daliniraCredentialSend !== 'function') return;
            window.__daliniraCredentialSend('fill', {
              origin: location.origin,
              token: activeTok
            });
          } catch (_) {}
        };

        managedButton.addEventListener('pointerdown', triggerAutofill, true);
        managedButton.addEventListener('click', triggerAutofill, true);

        document.documentElement.appendChild(managedButton);
      }

      managedButton.style.display = 'block';
      updatePosition();
    };

    const scanAndAttach = () => {
      if (!window.__daliniraFillButtonInstalled) return;
      // In SPA or dynamic DOM changes, expose the same action on a reliably
      // associated login identifier or its password field.
      const active = document.activeElement;
      if (active instanceof HTMLInputElement && isCredentialField(active)) {
        showButtonForField(active);
      } else if (currentTarget) {
        if (!currentTarget.isConnected || !visible(currentTarget)) {
          hideButton();
        } else if (!isInteracting && document.activeElement !== currentTarget) {
          hideButton();
        }
      }
    };

    document.addEventListener('focusin', event => {
      if (!window.__daliniraFillButtonInstalled) return;
      clearTimeout(hideTimer);
      const target = event.target;
      if (target instanceof HTMLInputElement && isCredentialField(target)) {
        showButtonForField(target);
      } else if (target !== managedButton) {
        if (!isInteracting) {
          hideButton();
        }
      }
    }, true);

    document.addEventListener('focusout', event => {
      if (!window.__daliniraFillButtonInstalled) return;
      if (event.target === currentTarget) {
        clearTimeout(hideTimer);
        hideTimer = setTimeout(() => {
          if (!isInteracting && document.activeElement !== currentTarget) {
            hideButton();
          }
        }, 220);
      }
    }, true);

    document.addEventListener('pointerdown', event => {
      if (!window.__daliniraFillButtonInstalled) return;
      const target = event.target;
      if (target === managedButton || managedButton?.contains(target)) {
        isInteracting = true;
        clearTimeout(hideTimer);
        return;
      }
      if (target instanceof HTMLInputElement && isCredentialField(target)) {
        clearTimeout(hideTimer);
        showButtonForField(target);
        return;
      }
      // Clicked outside button and outside password input
      if (currentTarget && !isInteracting) {
        hideButton();
      }
    }, true);

    window.addEventListener('scroll', () => {
      if (!window.__daliniraFillButtonInstalled) return;
      updatePosition();
    }, { passive: true, capture: true });

    window.addEventListener('resize', () => {
      if (!window.__daliniraFillButtonInstalled) return;
      updatePosition();
    }, { passive: true });

    new MutationObserver(() => {
      if (!window.__daliniraFillButtonInstalled) return;
      scanAndAttach();
    }).observe(document.documentElement || document.body, {
      childList: true, subtree: true, attributes: true,
      attributeFilter: ['style', 'class', 'hidden', 'type', 'disabled', 'autocomplete', 'name', 'id']
    });

    // On initial injection, do not auto-show unless a password field is already active
    scanAndAttach();
  })())JS").arg(payload, jsStringLiteral(I18n::text(QStringLiteral("passwords.autofill_button_title"))));
}

QString CredentialAutofillController::domFillScript(const QString &origin, const QString &username,
                                                     const QString &password, const QUrl &expectedUrl,
                                                     const QString &documentToken) {
  QJsonObject values{
      {QStringLiteral("origin"), origin},
      {QStringLiteral("username"), username},
      {QStringLiteral("password"), password}
  };
  if (!expectedUrl.isEmpty()) values.insert(QStringLiteral("url"), expectedUrl.toString(QUrl::FullyEncoded));
  if (!documentToken.isEmpty()) values.insert(QStringLiteral("documentToken"), documentToken);
  const QString json = QString::fromUtf8(QJsonDocument(values).toJson(QJsonDocument::Compact));
  return QStringLiteral(R"JS((() => {
    const v = %1;
    if (location.origin !== v.origin) return false;
    if (v.url && location.href !== v.url) return false;
    if (v.documentToken && window.__daliniraFillToken !== v.documentToken) return false;

    const visible = el => {
      if (!el || !el.isConnected) return false;
      const r = el.getBoundingClientRect();
      if (!r.width || !r.height) return false;
      const s = getComputedStyle(el);
      return el.type !== 'hidden' && !el.disabled && !el.readOnly &&
             s.display !== 'none' && s.visibility !== 'hidden' && Number(s.opacity || 1) > 0;
    };

    // Framework-safe (React, Vue, Svelte, Angular, Vanilla) input value setter
    const setInputValue = (input, val) => {
      input.focus();
      const proto = input instanceof HTMLInputElement ? HTMLInputElement.prototype : Object.getPrototypeOf(input);
      const desc = Object.getOwnPropertyDescriptor(proto, 'value') || Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value');
      if (desc && desc.set) {
        desc.set.call(input, val);
      } else {
        input.value = val;
      }
      input.dispatchEvent(new Event('input', { bubbles: true, cancelable: true }));
      input.dispatchEvent(new Event('change', { bubbles: true, cancelable: true }));
    };

    const pwdFields = Array.from(document.querySelectorAll('input[type="password"]')).filter(visible);
    if (!pwdFields.length) return false;
    const pwd = pwdFields[0];

    // Find username field: check pwd.form or closest container or preceding inputs
    const root = pwd.form || pwd.closest('form, [role="form"], [role="dialog"], fieldset, .login-container, body') || document;
    const userCandidates = Array.from(root.querySelectorAll('input')).filter(
      e => e !== pwd && visible(e) && !e.disabled && !e.readOnly && /^(text|email|tel)$/i.test(e.type || 'text')
    );

    let usernameInput = null;
    let bestScore = -1;
    for (const u of userCandidates) {
      let score = 0;
      const autoC = (u.autocomplete || '').toLowerCase();
      const hint = (u.name + ' ' + u.id + ' ' + (u.placeholder || '')).toLowerCase();
      if (autoC.includes('username') || autoC.includes('email')) score += 100;
      if (/user|email|login|account|identifier|eposta|telefon/.test(hint)) score += 50;
      if (score > bestScore) {
        bestScore = score;
        usernameInput = u;
      }
    }
    if (!usernameInput && userCandidates.length > 0) {
      usernameInput = userCandidates[userCandidates.length - 1];
    }

    if (usernameInput && v.username) {
      setInputValue(usernameInput, v.username);
    }
    setInputValue(pwd, v.password);
    pwd.focus();
    return true;
  })())JS").arg(json);
}

QString CredentialAutofillController::loginStateQueryScript() {
  // This query intentionally inspects only DOM shape and visibility.  In
  // particular, it never reads an input's value; the submitted password stays
  // solely in the native pending candidate captured at submit time.
  return QStringLiteral(R"JS((() => {
    try {
      const visible = el => {
        if (!el || !el.isConnected) return false;
        const r = el.getBoundingClientRect();
        if (!r.width || !r.height) return false;
        const s = getComputedStyle(el);
        return s.display !== 'none' && s.visibility !== 'hidden' && Number(s.opacity || 1) > 0;
      };
      const passwordFields = Array.from(document.querySelectorAll('input[type="password"], input[autocomplete="current-password"]'));
      const passwordFieldVisible = passwordFields.some(visible);
      const loginForms = Array.from(document.querySelectorAll('form, [role="form"], [role="dialog"]'));
      const loginFormVisible = passwordFieldVisible || loginForms.some(form => {
        if (!visible(form)) return false;
        const hint = `${form.getAttribute('action') || ''} ${form.id || ''} ${form.className || ''} ${form.getAttribute('aria-label') || ''}`.toLowerCase();
        const hasPasswordControl = !!form.querySelector('input[type="password"], input[autocomplete="current-password"]');
        return hasPasswordControl || /log[-_ ]?in|sign[-_ ]?in|giri[şs]|oturum/.test(hint);
      });
      const errorNodes = Array.from(document.querySelectorAll(
        '[role="alert"], [aria-invalid="true"], [class*="error" i], [id*="error" i], [data-testid*="error" i]'
      )).slice(0, 80);
      const errorStateObserved = errorNodes.some(el => {
        if (!visible(el)) return false;
        const text = String(el.textContent || '').slice(0, 500).toLowerCase();
        return /invalid|incorrect|wrong|failed|failure|denied|hatal[ıi]|yanl[ıi][şs]|ge[çc]ersiz|ba[şs]ar[ıi]s[ıi]z/.test(text) ||
               el.getAttribute('aria-invalid') === 'true';
      });
      const authControls = Array.from(document.querySelectorAll(
        'a[href*="logout" i], a[href*="signout" i], form[action*="logout" i], [aria-label*="log out" i], [aria-label*="sign out" i], [data-testid*="logout" i]'
      ));
      return {
        ready: document.readyState !== 'loading',
        origin: location.origin,
        loginFormVisible,
        passwordFieldVisible,
        errorStateObserved,
        authenticatedStateObserved: authControls.some(visible)
      };
    } catch (_) {
      return { ready: false, origin: location.origin };
    }
  })())JS");
}

bool CredentialAutofillController::handleBridgeMessage(QWebEnginePage *page, const QString &message) {
  const int separator = message.indexOf(QLatin1Char(':'));
  if (separator <= 0) return false;
  const QString type = message.left(separator);
  const QJsonDocument document = QJsonDocument::fromJson(message.mid(separator + 1).toUtf8());
  return document.isObject() && handleBridgePayload(page, type, document.object().toVariantMap());
}

bool CredentialAutofillController::handleBridgePayload(QWebEnginePage *page, const QString &type,
                                                        const QVariantMap &payload) {
  if (!page || !vaultManager_) return false;
  static const QSet<QString> allowed{QStringLiteral("candidate"), QStringLiteral("submit"),
                                     QStringLiteral("stage"), QStringLiteral("success"),
                                     QStringLiteral("state"), QStringLiteral("fill")};
  if (!allowed.contains(type)) return false;
  if (!vaultManager_->exists()) return true;
  const QByteArray encoded = QJsonDocument::fromVariant(payload).toJson(QJsonDocument::Compact);
  if (encoded.isEmpty() || encoded.size() > 16 * 1024) return false;
  const QString json = QString::fromUtf8(encoded);
  if (type == QLatin1String("candidate")) handleCandidate(page, json);
  else if (type == QLatin1String("submit")) handleSubmit(page, json);
  else if (type == QLatin1String("stage")) handleStage(page, json);
  else if (type == QLatin1String("success")) handleSuccessHint(page, json);
  else if (type == QLatin1String("state")) handleLoginState(page, json);
  else if (type == QLatin1String("fill")) handleFillRequest(page, json);
  return true;
}

CredentialMessageBridge::CredentialMessageBridge(QWebEnginePage *page,
                                                   CredentialAutofillController *controller,
                                                   QObject *parent)
    : QObject(parent), page_(page), controller_(controller) {}

void CredentialMessageBridge::send(const QString &type, const QVariantMap &payload) {
  if (!page_ || !controller_) return;
  controller_->handleBridgePayload(page_, type, payload);
}

void CredentialAutofillController::handleSubmit(QWebEnginePage *page, const QString &payload) {
  auto *view = page ? qobject_cast<QWebEngineView *>(page->parent()) : nullptr;
  if (!view || !vaultManager_) return;

  const QJsonObject submitObj = QJsonDocument::fromJson(payload.toUtf8()).object();
  const QString origin = CredentialVault::canonicalHttpsOrigin(view->url());
  const QString claimedOrigin = submitObj.value(QStringLiteral("origin")).toString();
  const QString nonce = submitObj.value(QStringLiteral("nonce")).toString();
  const QString username = submitObj.value(QStringLiteral("username")).toString().trimmed();

  if (origin.isEmpty() || claimedOrigin != origin) return;

  if (activeSaveBubble_) {
    const auto it = pendingCandidates_.find(activeBubbleCandidateKey_);
    if (it != pendingCandidates_.end() && (it->origin != origin || (!username.isEmpty() && it->username != username))) {
      return;
    }
  }

  recordLoginAttemptSubmit(view, origin, nonce, username);
}

void CredentialAutofillController::handleStage(QWebEnginePage *page, const QString &payload) {
  auto *view = page ? qobject_cast<QWebEngineView *>(page->parent()) : nullptr;
  if (!view || !vaultManager_) return;

  const QJsonObject staged = QJsonDocument::fromJson(payload.toUtf8()).object();
  const QString origin = CredentialVault::canonicalHttpsOrigin(view->url());
  const QString claimedOrigin = staged.value(QStringLiteral("origin")).toString();
  const QString username = staged.value(QStringLiteral("username")).toString().trimmed().left(320);

  if (origin.isEmpty() || claimedOrigin != origin || username.isEmpty()) return;

  pruneExpiredData();
  stagedUsernames_.insert(stageKey(view, origin), {username, QDateTime::currentDateTimeUtc().addSecs(5 * 60)});
}

void CredentialAutofillController::handleCandidate(QWebEnginePage *page, const QString &payload) {
  auto *view = page ? qobject_cast<QWebEngineView *>(page->parent()) : nullptr;
  if (!view || !vaultManager_) return;

  const QJsonDocument doc = QJsonDocument::fromJson(payload.toUtf8());
  const QJsonObject candidateObj = doc.object();
  const QString origin = CredentialVault::canonicalHttpsOrigin(view->url());
  const QString claimedOrigin = candidateObj.value(QStringLiteral("origin")).toString();
  QString username = candidateObj.value(QStringLiteral("username")).toString().trimmed().left(320);
  QString password = candidateObj.value(QStringLiteral("password")).toString();

  if (origin.isEmpty() || claimedOrigin != origin || password.isEmpty() || password.size() > 4096) {
    password.fill(QChar());
    return;
  }

  pruneExpiredData();
  if (username.isEmpty()) {
    username = stagedUsernames_.value(stageKey(view, origin)).username;
  }
  if (username.isEmpty()) {
    password.fill(QChar());
    return;
  }

  const QString key = candidateKey(view, origin, username);
  if (activeSaveBubble_) {
    if (activeBubbleCandidateKey_ == key) {
      auto it = pendingCandidates_.find(key);
      if (it != pendingCandidates_.end()) {
        it->password = password;
        it->expiresAt = QDateTime::currentDateTimeUtc().addSecs(120);
      }
      password.fill(QChar());
      return;
    } else {
      password.fill(QChar());
      return;
    }
  }

  if (activePrompts_.contains(key) || hasSaveFlowForCandidate(key)) {
    password.fill(QChar());
    return;
  }

  const QDateTime expiresAt = QDateTime::currentDateTimeUtc().addSecs(120);
  pendingCandidates_.insert(key, {origin, username, password, view->url(), expiresAt, false, QString(), view});
  password.fill(QChar());

  if (candidateObj.value(QStringLiteral("submitted")).toBool(false)) {
    recordLoginAttemptSubmit(view, origin, candidateObj.value(QStringLiteral("nonce")).toString(), username);
  }

  QPointer<CredentialAutofillController> guardedThis(this);
  QPointer<QWebEngineView> guardedView(view);
  QTimer::singleShot(121000, this, [guardedThis, guardedView, key] {
    if (!guardedThis) return;
    const auto it = guardedThis->pendingCandidates_.find(key);
    const bool isActivelyPrompted = (guardedThis->activeBubbleCandidateKey_ == key &&
                                     guardedThis->activeSaveBubble_ != nullptr);
    if (it != guardedThis->pendingCandidates_.end() && !isActivelyPrompted &&
        it->expiresAt <= QDateTime::currentDateTimeUtc()) {
      it->password.fill(QChar());
      guardedThis->pendingCandidates_.erase(it);
    }
  });
}

void CredentialAutofillController::handleSuccessHint(QWebEnginePage *page, const QString &payload) {
  auto *view = page ? qobject_cast<QWebEngineView *>(page->parent()) : nullptr;
  if (!view) return;

  const QJsonObject hint = QJsonDocument::fromJson(payload.toUtf8()).object();
  const QString origin = CredentialVault::canonicalHttpsOrigin(view->url());
  const QString hintOrigin = hint.value(QStringLiteral("origin")).toString();
  if (origin.isEmpty() || (!isSameSiteOrOrigin(origin, hintOrigin) && origin != hintOrigin)) return;

  LoginAttemptState *attempt = findActiveSubmittedAttempt(view, origin);
  if (!attempt || !attempt->submitted || !attempt->successEligible) {
    return;
  }
  attempt->successHintObserved = true;
  const bool hasVerifiedState = hint.contains(QStringLiteral("loginFormVisible")) &&
                                hint.contains(QStringLiteral("passwordFieldVisible"));
  attempt->legacySuccessHint = !hasVerifiedState;
  if (hasVerifiedState) {
    attempt->uiStateObserved = true;
    attempt->loginFormStillVisible = hint.value(QStringLiteral("loginFormVisible")).toBool(true);
    attempt->passwordFieldStillVisible = hint.value(QStringLiteral("passwordFieldVisible")).toBool(true);
    attempt->errorStateObserved = attempt->errorStateObserved ||
                                  hint.value(QStringLiteral("errorStateObserved")).toBool(false);
    if (!attempt->loginFormStillVisible && !attempt->passwordFieldStillVisible && !attempt->errorStateObserved) {
      ++attempt->consecutiveSuccessStates;
    }
  }
  evaluateLoginAttempt(view, origin);
}

void CredentialAutofillController::handleLoginState(QWebEnginePage *page, const QString &payload) {
  auto *view = page ? qobject_cast<QWebEngineView *>(page->parent()) : nullptr;
  if (!view) return;
  const QJsonObject state = QJsonDocument::fromJson(payload.toUtf8()).object();
  const QString currentOrigin = CredentialVault::canonicalHttpsOrigin(view->url());
  const QString claimedOrigin = state.value(QStringLiteral("origin")).toString();
  if (currentOrigin.isEmpty() || !isSameSiteOrOrigin(currentOrigin, claimedOrigin)) return;

  LoginAttemptState *attempt = findActiveSubmittedAttempt(view, currentOrigin);
  if (!attempt) return;
  const QString attemptKey = stageKey(view, attempt->origin);
  QVariantMap values;
  values.insert(QStringLiteral("origin"), claimedOrigin);
  values.insert(QStringLiteral("loginFormVisible"), state.value(QStringLiteral("loginFormVisible")).toBool(true));
  values.insert(QStringLiteral("passwordFieldVisible"), state.value(QStringLiteral("passwordFieldVisible")).toBool(true));
  values.insert(QStringLiteral("errorStateObserved"), state.value(QStringLiteral("errorStateObserved")).toBool(false));
  values.insert(QStringLiteral("authenticatedStateObserved"), state.value(QStringLiteral("authenticatedStateObserved")).toBool(false));
  applyLoginUiState(attemptKey, attempt->generation, values);
}

void CredentialAutofillController::handleFillRequest(QWebEnginePage *page, const QString &payload) {
  auto *view = page ? qobject_cast<QWebEngineView *>(page->parent()) : nullptr;
  if (!view || !page || page != view->page()) return;

  const QJsonObject request = QJsonDocument::fromJson(payload.toUtf8()).object();
  const QString origin = CredentialVault::canonicalHttpsOrigin(view->url());
  const QString key = stageKey(view, origin);

  // Strict token validation
  const QString expectedToken = credentialFillTokens_.value(key);
  const QString receivedToken = request.value(QStringLiteral("token")).toString();
  const QString claimedOrigin = request.value(QStringLiteral("origin")).toString();

  if (origin.isEmpty() ||
      expectedToken.isEmpty() ||
      receivedToken.isEmpty() ||
      claimedOrigin != origin ||
      receivedToken != expectedToken) {
    return;
  }

  triggerFillForView(view);
}

void CredentialAutofillController::triggerFillForView(QWebEngineView *view) {
  if (!view || !view->page() || !vaultManager_) return;

  if (!vaultManager_->exists()) {
    // No-vault autofill is intentionally silent.  In particular, a web-page
    // action must never be able to summon vault-creation UI.
    return;
  }

  const QString origin = CredentialVault::canonicalHttpsOrigin(view->url());
  if (origin.isEmpty() || !vaultManager_->hasMatchingCredential(view->url())) return;
  if (activeUnlockDialog_ || pendingFill_.view) return;

  // Locked metadata deliberately does not expose account names. Authenticate
  // first; if more than one account is discovered, selection is followed by a
  // second, explicit release authorization.
  if (vaultManager_->isLocked()) {
    const QVector<VaultMetadata> vaultChoices = vaultManager_->vaultsForOrigin(view->url());
    if (vaultChoices.isEmpty()) return;
    int selectedVault = 0;
    if (vaultChoices.size() > 1) {
      QStringList labels;
      for (const VaultMetadata &vault : vaultChoices) labels << vault.name;
      bool ok = false;
      const QString label = QInputDialog::getItem(
          dialogParent_, I18n::text(QStringLiteral("passwords.choose_vault")),
          I18n::text(QStringLiteral("passwords.vault_with_login")), labels, 0, false, &ok);
      if (!ok) return;
      selectedVault = labels.indexOf(label);
      if (selectedVault < 0) return;
    }
    if (!vaultManager_->setActiveVault(vaultChoices.at(selectedVault).id)) return;
    unlockVaultForFill(view);
    return;
  }

  const QString recordId = selectCredentialForView(view);
  if (recordId.isEmpty()) return;
  beginFillReauthentication(view, recordId);
}

QString CredentialAutofillController::selectCredentialForView(QWebEngineView *view) {
  if (!view || !vaultManager_ || vaultManager_->isLocked()) return {};
  const QVector<CredentialMetadata> choices = vaultManager_->forOrigin(view->url());
  if (choices.isEmpty()) {
    // Check if other vaults have credentials for this origin
    const QVector<VaultMetadata> otherVaults = vaultManager_->vaultsForOrigin(view->url());
    if (otherVaults.isEmpty()) {
      return {};
    }
    QStringList labels;
    for (const VaultMetadata &v : otherVaults) {
      labels << (v.locked ? I18n::text(QStringLiteral("passwords.vault_label_locked"))
                         : I18n::text(QStringLiteral("passwords.vault_label_open"))).arg(v.name);
    }
    bool ok = false;
    const QString selection = QInputDialog::getItem(dialogParent_, I18n::text(QStringLiteral("passwords.choose_vault")),
                                                    I18n::text(QStringLiteral("passwords.vault_with_login")),
                                                    labels, 0, false, &ok);
    if (!ok) return {};
    const int index = labels.indexOf(selection);
    if (index >= 0 && vaultManager_->setActiveVault(otherVaults.at(index).id)) {
      if (otherVaults.at(index).locked) {
        unlockVaultForFill(view);
      } else {
        return selectCredentialForView(view);
      }
    }
    return {};
  }

  CredentialMetadata selected = choices.front();
  if (choices.size() > 1) {
    QStringList labels;
    for (const auto &c : choices) {
      labels << QStringLiteral("%1 (%2)").arg(c.username, c.vaultName);
    }
    bool ok = false;
    const QString label = QInputDialog::getItem(dialogParent_, I18n::text(QStringLiteral("passwords.choose_account")),
                                                I18n::text(QStringLiteral("passwords.account_to_use")), labels, 0, false, &ok);
    if (!ok) return {};
    const int selIdx = labels.indexOf(label);
    if (selIdx >= 0) selected = choices.at(selIdx);
  }

  return selected.id;
}

bool CredentialAutofillController::pendingFillContextIsCurrent() const {
  QWebEngineView *view = pendingFill_.view.data();
  if (!view || !pendingFill_.page || view->page() != pendingFill_.page.data()) return false;
  if (navigationGenerations_.value(view) != pendingFill_.navigationGeneration) return false;
  if (view->url() != pendingFill_.url) return false;
  const QString currentOrigin = CredentialVault::canonicalHttpsOrigin(view->url());
  if (currentOrigin.isEmpty() || currentOrigin != pendingFill_.origin) return false;
  if (!vaultManager_ || pendingFill_.vaultId.isEmpty() ||
      vaultManager_->isVaultLocked(pendingFill_.vaultId)) return false;
  if (!pendingFill_.recordId.isEmpty() &&
      vaultManager_->owningVaultId(pendingFill_.recordId) != pendingFill_.vaultId) return false;
  return !activeTokenForView(view, currentOrigin).isEmpty();
}

void CredentialAutofillController::clearPendingFill() {
  pendingFill_.view.clear();
  pendingFill_.page.clear();
  pendingFill_.origin.clear();
  pendingFill_.url.clear();
  pendingFill_.recordId.clear();
  pendingFill_.vaultId.clear();
  pendingFill_.navigationGeneration = 0;
}

void CredentialAutofillController::completePendingFill() {
  if (!pendingFillContextIsCurrent() || !vaultManager_) {
    clearPendingFill();
    return;
  }

  QPointer<QWebEngineView> view = pendingFill_.view;
  QString recordId = pendingFill_.recordId;
  if (recordId.isEmpty()) {
    const QVector<CredentialMetadata> choices = vaultManager_->forOrigin(view->url());
    if (choices.size() > 1) {
      // Account names only became available after unlocking.  Start the normal
      // selection -> reauthentication path so selection never authorizes fill.
      clearPendingFill();
      triggerFillForView(view);
      return;
    }
    if (choices.size() != 1) {
      clearPendingFill();
      return;
    }
    recordId = choices.front().id;
    if (vaultManager_->owningVaultId(recordId) != pendingFill_.vaultId) {
      clearPendingFill();
      return;
    }
  }

  if (!pendingFillContextIsCurrent()) {
    clearPendingFill();
    return;
  }

  CredentialSecret secret;
  if (!vaultManager_->reveal(recordId, &secret)) {
    clearPendingFill();
    return;
  }

  const QString expectedOrigin = pendingFill_.origin;
  if (!pendingFillContextIsCurrent() || secret.origin != expectedOrigin) {
    secret.password.fill(QChar());
    secret.username.fill(QChar());
    clearPendingFill();
    return;
  }

  const QString documentToken = activeTokenForView(view, expectedOrigin);
  const QString script = domFillScript(expectedOrigin, secret.username, secret.password,
                                       pendingFill_.url, documentToken);
  const QString filledUsername = secret.username;
  secret.password.fill(QChar());
  secret.username.fill(QChar());

  // Last native-side check, immediately before the renderer receives the
  // credential pair.
  if (!pendingFillContextIsCurrent() || !view || view->page() != pendingFill_.page.data()) {
    clearPendingFill();
    return;
  }
  clearPendingFill();

  view->page()->runJavaScript(script, QWebEngineScript::ApplicationWorld, [this](const QVariant &result) {
    if (!result.toBool() && dialogParent_) {
      QMessageBox::information(dialogParent_, I18n::text(QStringLiteral("toolbar.passwords")),
                               I18n::text(QStringLiteral("passwords.login_fields_not_found")));
    }
  });
  emit credentialFillDispatched(expectedOrigin, filledUsername);
}

void CredentialAutofillController::unlockVaultForFill(QWebEngineView *view) {
  beginFillReauthentication(view, QString());
}

void CredentialAutofillController::beginFillReauthentication(QWebEngineView *view,
                                                              const QString &recordId) {
  if (!view || !view->page() || !vaultManager_ || !vaultManager_->exists()) return;

  const QString canonicalOrigin = CredentialVault::canonicalHttpsOrigin(view->url());
  if (canonicalOrigin.isEmpty() || !vaultManager_->hasMatchingCredential(view->url())) return;

  // A second renderer event cannot replace or broaden an in-flight grant.
  if (activeUnlockDialog_) {
    activeUnlockDialog_->show();
    activeUnlockDialog_->raise();
    activeUnlockDialog_->activateWindow();
    return;
  }

  pendingFill_.view = view;
  pendingFill_.page = view->page();
  pendingFill_.origin = canonicalOrigin;
  pendingFill_.url = view->url();
  pendingFill_.recordId = recordId;
  pendingFill_.vaultId = recordId.isEmpty() ? vaultManager_->activeVaultId()
                                            : vaultManager_->owningVaultId(recordId);
  if (pendingFill_.vaultId.isEmpty()) {
    clearPendingFill();
    return;
  }
  pendingFill_.navigationGeneration = navigationGenerations_.value(view);

  QWidget *parent = dialogParent_ ? dialogParent_ : (view ? view->window() : nullptr);
  auto *dialog = new VaultUnlockDialog(vaultManager_, canonicalOrigin, parent,
                                       VaultUnlockDialog::Purpose::ReauthenticateForAutofill,
                                       pendingFill_.vaultId);
  activeUnlockDialog_ = dialog;
  emit reauthenticationRequested(canonicalOrigin);

  if (parent) {
    const QRect parentGeom = parent->geometry();
    dialog->move(parentGeom.center() - QPoint(dialog->width() / 2, dialog->height() / 2));
  }

  QPointer<CredentialAutofillController> guardedThis(this);

  connect(dialog, &VaultUnlockDialog::unlocked, this, [guardedThis, dialog] {
    if (!guardedThis) return;
    // A cancelled/stale async verifier must never authorize a newer request.
    if (guardedThis->activeUnlockDialog_ != dialog) return;
    guardedThis->activeUnlockDialog_.clear();
    guardedThis->completePendingFill();
  });

  connect(dialog, &QDialog::rejected, this, [guardedThis, dialog] {
    if (!guardedThis) return;
    if (guardedThis->activeUnlockDialog_ == dialog) {
      guardedThis->activeUnlockDialog_.clear();
    }
    guardedThis->clearPendingFill();
  });

  connect(dialog, &QDialog::finished, this, [guardedThis, dialog] {
    dialog->deleteLater();
    if (!guardedThis) return;
    if (guardedThis->activeUnlockDialog_ == dialog) {
      guardedThis->activeUnlockDialog_.clear();
      guardedThis->clearPendingFill();
    }
  });

  connect(dialog, &QObject::destroyed, this, [guardedThis, dialog] {
    if (!guardedThis) return;
    if (guardedThis->activeUnlockDialog_ == dialog) {
      guardedThis->activeUnlockDialog_.clear();
      guardedThis->clearPendingFill();
    }
  });

  dialog->show();
  dialog->raise();
  dialog->activateWindow();
}

void CredentialAutofillController::promptCandidate(QWebEngineView *view, const QString &candidateKey) {
  QWebEngineView *targetView = view ? view : activeBubbleView_.data();
  if (!targetView && !dialogParent_) {
    const auto candCheck = pendingCandidates_.constFind(candidateKey);
    if (candCheck != pendingCandidates_.cend()) targetView = candCheck->view.data();
  }
  const bool updatingVerifyingBubble = activeSaveBubble_ &&
                                        activeBubbleCandidateKey_ == candidateKey &&
                                        activeSaveBubble_->mode() == CredentialSaveMode::Verifying;
  if (activePrompts_.contains(candidateKey) && !updatingVerifyingBubble) return;
  pruneExpiredData();

  const auto it = pendingCandidates_.find(candidateKey);
  if (it == pendingCandidates_.end()) return;

  PendingCandidate &candidate = it.value();
  if (!vaultManager_ || !vaultManager_->exists()) {
    return;
  }

  // Deduplication: if active save bubble is already showing for this key, avoid duplicate
  if (activeSaveBubble_ && activeBubbleCandidateKey_ == candidateKey && !updatingVerifyingBubble) {
    return;
  }

  CredentialSaveMode mode = CredentialSaveMode::ReadyNew;

  if (!vaultManager_->isLocked()) {
    const QVector<CredentialMetadata> existingRecords = vaultManager_->forOrigin(QUrl(candidate.origin));
    const auto matchIt = std::find_if(existingRecords.cbegin(), existingRecords.cend(),
                                      [&candidate](const CredentialMetadata &item) {
                                        return item.username == candidate.username;
                                      });
    if (matchIt != existingRecords.cend()) {
      // Existing account found! Compare password:
      CredentialSecret existingSecret;
      if (vaultManager_->reveal(matchIt->id, &existingSecret)) {
        if (existingSecret.password == candidate.password) {
          // Password unchanged: do not show prompt
          existingSecret.password.fill(QChar());
          candidate.password.fill(QChar());
          pendingCandidates_.remove(candidateKey);
          activePrompts_.remove(candidateKey);
          removeSaveFlowForCandidate(candidateKey, QStringLiteral("success"));
          dismissSaveBubble();
          return;
        }
        existingSecret.password.fill(QChar());
      }
      mode = CredentialSaveMode::ReadyUpdate;
      candidate.isUpdate = true;
      candidate.updateRecordId = matchIt->id;
    }
  }

  if (updatingVerifyingBubble) {
    activeSaveBubble_->setupPrompt(mode, candidate.origin, candidate.username);
    for (auto flowIt = pendingSaveFlows_.begin(); flowIt != pendingSaveFlows_.end(); ++flowIt) {
      if (flowIt->candidateKey != candidateKey) continue;
      flowIt->state = (mode == CredentialSaveMode::ReadyUpdate)
                          ? CredentialSaveFlowState::ReadyUpdate
                          : CredentialSaveFlowState::ReadyNew;
      emit saveFlowStateChanged(flowIt->origin, flowIt->username, flowIt->state);
      break;
    }
    return;
  }

  dismissSaveBubble();

  activePrompts_.insert(candidateKey);
  activeBubbleCandidateKey_ = candidateKey;
  activeBubbleView_ = targetView;

  QWidget *parentWidget = dialogParent_ ? dialogParent_ : (targetView ? targetView->window() : nullptr);
  auto *bubble = new CredentialSaveBubble(parentWidget);
  activeSaveBubble_ = bubble;

  bubble->setupPrompt(mode, candidate.origin, candidate.username);

  // Connect user actions
  QPointer<CredentialAutofillController> guardedThis(this);
  const QString capturedKey = candidateKey;

  connect(bubble, &CredentialSaveBubble::saveAccepted, this, [guardedThis, capturedKey] {
    if (!guardedThis) return;
    const auto candIt = guardedThis->pendingCandidates_.find(capturedKey);
    if (candIt != guardedThis->pendingCandidates_.end() && candIt->isUpdate) {
      guardedThis->triggerUpdateCandidate(capturedKey);
    } else {
      guardedThis->triggerSaveCandidate(capturedKey);
    }
  });

  connect(bubble, &CredentialSaveBubble::saveRejected, this, [guardedThis, capturedKey] {
    if (!guardedThis) return;
    guardedThis->triggerRejectCandidate(capturedKey);
  });

  if (parentWidget) {
    const int x = std::max(10, (parentWidget->width() - bubble->width()) / 2);
    bubble->move(x, 70);
  }

  bubble->show();
  bubble->raise();
  emit saveBubbleShown(candidate.origin, candidate.username, mode);
  for (auto flowIt = pendingSaveFlows_.begin(); flowIt != pendingSaveFlows_.end(); ++flowIt) {
    if (flowIt->candidateKey != candidateKey) continue;
    flowIt->state = (mode == CredentialSaveMode::ReadyUpdate)
                        ? CredentialSaveFlowState::ReadyUpdate
                        : CredentialSaveFlowState::ReadyNew;
    emit saveFlowStateChanged(flowIt->origin, flowIt->username, flowIt->state);
    break;
  }
}

void CredentialAutofillController::triggerSaveCandidate(const QString &candidateKey) {
  const auto it = pendingCandidates_.find(candidateKey);
  if (it == pendingCandidates_.end() || !vaultManager_ || !vaultManager_->exists()) {
    dismissSaveBubble();
    return;
  }

  PendingCandidate candidate = it.value();
  QWebEngineView *view = candidate.view.data();

  if (vaultManager_->isLocked()) {
    unlockVaultForSave(view, candidateKey);
    return;
  }

  CredentialSecret saved;
  saved.origin = candidate.origin;
  saved.username = candidate.username;
  saved.password = candidate.password;

  bool updated = false;
  const bool ok = vaultManager_->save(saved, &updated);
  saved.password.fill(QChar());

  it->password.fill(QChar());
  pendingCandidates_.remove(candidateKey);
  activePrompts_.remove(candidateKey);
  removeSaveFlowForCandidate(candidateKey, ok ? QStringLiteral("success") : QStringLiteral("failure"));
  dismissSaveBubble();

  if (ok) {
    emit credentialSaved(candidate.origin, candidate.username);
    refreshAutofillForViews();
  } else if (dialogParent_) {
    QMessageBox::warning(dialogParent_, I18n::text(QStringLiteral("toolbar.passwords")),
                         I18n::text(QStringLiteral("passwords.login_save_failed")));
  }
}

void CredentialAutofillController::triggerUpdateCandidate(const QString &candidateKey) {
  const auto it = pendingCandidates_.find(candidateKey);
  if (it == pendingCandidates_.end() || !vaultManager_ || !vaultManager_->exists()) {
    dismissSaveBubble();
    return;
  }

  PendingCandidate candidate = it.value();
  QWebEngineView *view = candidate.view.data();

  if (vaultManager_->isLocked()) {
    unlockVaultForSave(view, candidateKey);
    return;
  }

  CredentialSecret saved;
  saved.origin = candidate.origin;
  saved.username = candidate.username;
  saved.password = candidate.password;

  bool ok = false;
  if (!candidate.updateRecordId.isEmpty()) {
    ok = vaultManager_->update(candidate.updateRecordId, saved);
  } else {
    bool updated = false;
    ok = vaultManager_->save(saved, &updated);
  }
  saved.password.fill(QChar());

  it->password.fill(QChar());
  pendingCandidates_.remove(candidateKey);
  activePrompts_.remove(candidateKey);
  removeSaveFlowForCandidate(candidateKey, ok ? QStringLiteral("success") : QStringLiteral("failure"));
  dismissSaveBubble();

  if (ok) {
    emit credentialUpdated(candidate.origin, candidate.username);
    refreshAutofillForViews();
  } else if (dialogParent_) {
    QMessageBox::warning(dialogParent_, I18n::text(QStringLiteral("toolbar.passwords")),
                         I18n::text(QStringLiteral("passwords.login_update_failed")));
  }
}

void CredentialAutofillController::triggerRejectCandidate(const QString &candidateKey) {
  const auto it = pendingCandidates_.find(candidateKey);
  if (it != pendingCandidates_.end()) {
    it->password.fill(QChar());
    pendingCandidates_.erase(it);
  }
  activePrompts_.remove(candidateKey);
  removeSaveFlowForCandidate(candidateKey);
  dismissSaveBubble();
  emit saveBubbleDismissed();
}

void CredentialAutofillController::unlockVaultForSave(QWebEngineView *view, const QString &candidateKey) {
  if (!vaultManager_ || !vaultManager_->exists()) return;

  const auto it = pendingCandidates_.find(candidateKey);
  if (it == pendingCandidates_.end()) return;

  PendingCandidate candidate = it.value();
  const QString canonicalOrigin = candidate.origin;

  if (activeUnlockDialog_) {
    activeUnlockDialog_->setCanonicalOrigin(canonicalOrigin);
    activeUnlockDialog_->show();
    activeUnlockDialog_->raise();
    activeUnlockDialog_->activateWindow();
    return;
  }

  QWidget *parent = dialogParent_ ? dialogParent_ : (view ? view->window() : nullptr);
  auto *dialog = new VaultUnlockDialog(vaultManager_, canonicalOrigin, parent);
  activeUnlockDialog_ = dialog;

  if (parent) {
    const QRect parentGeom = parent->geometry();
    dialog->move(parentGeom.center() - QPoint(dialog->width() / 2, dialog->height() / 2));
  }

  dismissSaveBubble();

  QPointer<CredentialAutofillController> guardedThis(this);
  const QString key = candidateKey;

  connect(dialog, &VaultUnlockDialog::unlocked, this, [guardedThis, key, dialog] {
    if (!guardedThis) return;
    if (guardedThis->activeUnlockDialog_ == dialog) {
      guardedThis->activeUnlockDialog_.clear();
    }
    const auto candIt = guardedThis->pendingCandidates_.find(key);
    if (candIt != guardedThis->pendingCandidates_.end()) {
      PendingCandidate cand = candIt.value();
      CredentialSecret secret;
      secret.origin = cand.origin;
      secret.username = cand.username;
      secret.password = cand.password;

      const QVector<CredentialMetadata> existingRecords = guardedThis->vaultManager_->forOrigin(QUrl(cand.origin));
      const auto matchIt = std::find_if(existingRecords.cbegin(), existingRecords.cend(),
                                        [&cand](const CredentialMetadata &item) {
                                          return item.username == cand.username;
                                        });
      if (matchIt != existingRecords.cend()) {
        CredentialSecret existing;
        const bool revealed = guardedThis->vaultManager_->reveal(matchIt->id, &existing);
        const bool unchanged = revealed && existing.password == cand.password;
        existing.password.fill(QChar());
        if (!unchanged && guardedThis->vaultManager_->update(matchIt->id, secret)) {
          emit guardedThis->credentialUpdated(cand.origin, cand.username);
        }
      } else {
        bool updated = false;
        if (guardedThis->vaultManager_->save(secret, &updated)) {
          emit guardedThis->credentialSaved(cand.origin, cand.username);
        }
      }
      secret.password.fill(QChar());
      candIt->password.fill(QChar());
      guardedThis->pendingCandidates_.remove(key);
      guardedThis->activePrompts_.remove(key);
      guardedThis->removeSaveFlowForCandidate(key, QStringLiteral("success"));
      guardedThis->refreshAutofillForViews();
    }
  });

  connect(dialog, &QDialog::rejected, this, [guardedThis, key, dialog] {
    if (!guardedThis) return;
    if (guardedThis->activeUnlockDialog_ == dialog) {
      guardedThis->activeUnlockDialog_.clear();
    }
    const auto candIt = guardedThis->pendingCandidates_.find(key);
    if (candIt != guardedThis->pendingCandidates_.end()) {
      candIt->password.fill(QChar());
      guardedThis->pendingCandidates_.remove(key);
      guardedThis->activePrompts_.remove(key);
      guardedThis->removeSaveFlowForCandidate(key);
    }
  });

  connect(dialog, &QDialog::finished, this, [guardedThis, dialog] {
    dialog->deleteLater();
    if (!guardedThis) return;
    if (guardedThis->activeUnlockDialog_ == dialog) {
      guardedThis->activeUnlockDialog_.clear();
    }
  });

  dialog->show();
  dialog->raise();
  dialog->activateWindow();
}

CredentialSaveBubble *CredentialAutofillController::activeSaveBubble() const {
  return activeSaveBubble_.data();
}

QWebEngineView *CredentialAutofillController::activeBubbleView() const {
  return activeBubbleView_.data();
}

void CredentialAutofillController::dismissSaveBubble() {
  if (activeSaveBubble_) {
    activeSaveBubble_->hide();
    activeSaveBubble_->deleteLater();
    activeSaveBubble_.clear();
  }
  activeBubbleCandidateKey_.clear();
  activeBubbleView_.clear();
}



int CredentialAutofillController::pendingCandidateCount() const {
  return pendingCandidates_.size();
}

bool CredentialAutofillController::hasPendingCandidate(const QString &candidateKey) const {
  return pendingCandidates_.contains(candidateKey);
}

bool CredentialAutofillController::hasPendingCredentialSaveFlow(QWebEngineView *view,
                                                                  const QString &origin) const {
  return credentialSaveFlowState(view, origin) != CredentialSaveFlowState::None;
}

CredentialSaveFlowState CredentialAutofillController::credentialSaveFlowState(
    QWebEngineView *view, const QString &origin) const {
  if (!view || origin.isEmpty()) return CredentialSaveFlowState::None;
  const QString exactKey = stageKey(view, origin);
  const auto exact = pendingSaveFlows_.constFind(exactKey);
  if (exact != pendingSaveFlows_.cend()) return exact->state;
  const QString prefix = QString::number(reinterpret_cast<quintptr>(view)) + QLatin1Char(':');
  for (auto it = pendingSaveFlows_.cbegin(); it != pendingSaveFlows_.cend(); ++it) {
    if (it.key().startsWith(prefix) && isSameSiteOrOrigin(it->origin, origin)) return it->state;
  }
  return CredentialSaveFlowState::None;
}

QString CredentialAutofillController::lastSaveFlowEndReason() const {
  return lastSaveFlowEndReason_;
}

bool CredentialAutofillController::hasSaveFlowForCandidate(const QString &candidateKey) const {
  for (const auto &flow : pendingSaveFlows_) {
    if (flow.candidateKey == candidateKey) return true;
  }
  return false;
}

void CredentialAutofillController::createPendingSaveFlow(const QString &attemptKey,
                                                           const LoginAttemptState &attempt) {
  if (!attempt.view || attempt.candidateKey.isEmpty() ||
      !pendingCandidates_.contains(attempt.candidateKey)) {
    return;
  }
  PendingCredentialSaveFlow flow;
  flow.view = attempt.view;
  flow.origin = attempt.origin;
  flow.username = attempt.username;
  flow.candidateKey = attempt.candidateKey;
  flow.attemptNonce = attempt.attemptNonce;
  flow.createdAt = attempt.submittedAt;
  flow.state = CredentialSaveFlowState::Verifying;
  flow.generation = attempt.generation;
  pendingSaveFlows_.insert(attemptKey, flow);
  emit saveFlowStateChanged(flow.origin, flow.username, flow.state);

  QPointer<CredentialAutofillController> guardedThis(this);
  QTimer::singleShot(0, this, [guardedThis, attemptKey, generation = flow.generation] {
    if (guardedThis) guardedThis->showVerifyingBubble(attemptKey, generation);
  });
}

void CredentialAutofillController::showVerifyingBubble(const QString &attemptKey, quint64 generation) {
  const auto flowIt = pendingSaveFlows_.constFind(attemptKey);
  if (flowIt == pendingSaveFlows_.cend() || flowIt->generation != generation ||
      flowIt->state != CredentialSaveFlowState::Verifying || !flowIt->view) {
    return;
  }
  const auto candidateIt = pendingCandidates_.constFind(flowIt->candidateKey);
  if (candidateIt == pendingCandidates_.cend()) return;

  if (activeSaveBubble_) {
    if (activeBubbleCandidateKey_ == flowIt->candidateKey) return;
    return;
  }
  dismissSaveBubble();
  activePrompts_.insert(flowIt->candidateKey);
  activeBubbleCandidateKey_ = flowIt->candidateKey;
  activeBubbleView_ = flowIt->view;

  QWidget *parentWidget = dialogParent_ ? dialogParent_ : flowIt->view->window();
  auto *bubble = new CredentialSaveBubble(parentWidget);
  activeSaveBubble_ = bubble;
  bubble->setupPrompt(CredentialSaveMode::Verifying, candidateIt->origin, candidateIt->username);

  QPointer<CredentialAutofillController> guardedThis(this);
  const QString candidateKey = flowIt->candidateKey;
  connect(bubble, &CredentialSaveBubble::saveAccepted, this, [guardedThis, candidateKey] {
    if (!guardedThis) return;
    const auto it = guardedThis->pendingCandidates_.constFind(candidateKey);
    if (it != guardedThis->pendingCandidates_.cend() && it->isUpdate) {
      guardedThis->triggerUpdateCandidate(candidateKey);
    } else {
      guardedThis->triggerSaveCandidate(candidateKey);
    }
  });
  connect(bubble, &CredentialSaveBubble::saveRejected, this, [guardedThis, candidateKey] {
    if (guardedThis) guardedThis->triggerRejectCandidate(candidateKey);
  });

  if (parentWidget) {
    const int x = std::max(10, (parentWidget->width() - bubble->width()) / 2);
    bubble->move(x, 70);
  }
  bubble->show();
  bubble->raise();
  emit saveBubbleShown(candidateIt->origin, candidateIt->username, CredentialSaveMode::Verifying);
}

void CredentialAutofillController::cancelPendingSaveFlow(const QString &attemptKey,
                                                           const QString &reasonCode) {
  const auto flowIt = pendingSaveFlows_.find(attemptKey);
  if (flowIt == pendingSaveFlows_.end()) {
    loginAttempts_.remove(attemptKey);
    return;
  }
  const QString candidateKey = flowIt->candidateKey;
  QPointer<QWebEngineView> view = flowIt->view;
  const bool isBubbleActive = (activeBubbleCandidateKey_ == candidateKey && activeSaveBubble_ != nullptr);
  if (isBubbleActive && reasonCode != QLatin1String("tab_close")) {
    const bool wasVerifying = (activeSaveBubble_->mode() == CredentialSaveMode::Verifying);
    loginAttempts_.remove(attemptKey);
    pendingSaveFlows_.erase(flowIt);
    lastSaveFlowEndReason_ = reasonCode;
    emit saveFlowEnded(reasonCode);
    stopConfirmationTimerIfIdle();
    if (wasVerifying) {
      promptCandidate(view.data(), candidateKey);
    }
    return;
  }

  if (activeBubbleCandidateKey_ == candidateKey) dismissSaveBubble();
  activePrompts_.remove(candidateKey);
  const auto candidateIt = pendingCandidates_.find(candidateKey);
  if (candidateIt != pendingCandidates_.end()) {
    candidateIt->password.fill(QChar());
    pendingCandidates_.erase(candidateIt);
  }
  loginAttempts_.remove(attemptKey);
  pendingSaveFlows_.erase(flowIt);
  lastSaveFlowEndReason_ = reasonCode;
  emit saveFlowEnded(reasonCode);
  stopConfirmationTimerIfIdle();
}

void CredentialAutofillController::removeSaveFlowForCandidate(const QString &candidateKey,
                                                                const QString &reasonCode) {
  const auto keys = pendingSaveFlows_.keys();
  for (const QString &key : keys) {
    const auto it = pendingSaveFlows_.constFind(key);
    if (it == pendingSaveFlows_.cend() || it->candidateKey != candidateKey) continue;
    loginAttempts_.remove(key);
    pendingSaveFlows_.remove(key);
  }
  if (!reasonCode.isEmpty()) {
    lastSaveFlowEndReason_ = reasonCode;
    emit saveFlowEnded(reasonCode);
  }
  stopConfirmationTimerIfIdle();
}

void CredentialAutofillController::stopConfirmationTimerIfIdle() {
  if (loginAttempts_.isEmpty()) confirmationTimer_.stop();
}

void CredentialAutofillController::evaluateLoginAttempt(QWebEngineView *view, const QString &origin) {
  LoginAttemptState *attempt = findActiveSubmittedAttempt(view, origin);
  if (!attempt) return;
  evaluateLoginAttemptKey(stageKey(view, attempt->origin));
}

void CredentialAutofillController::evaluateLoginAttemptKey(const QString &attemptKey) {
  auto it = loginAttempts_.find(attemptKey);
  if (it == loginAttempts_.end()) return;

  LoginAttemptState &attempt = it.value();
  const QDateTime now = QDateTime::currentDateTimeUtc();
  const qint64 elapsedMs = attempt.submittedAt.msecsTo(now);
  if (elapsedMs > kLoginAttemptTimeoutSecs * 1000LL) {
    cancelPendingSaveFlow(attemptKey, QStringLiteral("timeout"));
    return;
  }
  if (!attempt.submitted || !attempt.successEligible || attempt.confirmed) {
    cancelPendingSaveFlow(attemptKey, QStringLiteral("failure"));
    return;
  }

  // Explicit error evidence is terminal. A still-visible form/password alone is
  // not a failure: async login pages often keep those controls around briefly.
  if (attempt.errorStateObserved) {
    cancelPendingSaveFlow(attemptKey, QStringLiteral("failure"));
    return;
  }
  const bool reloadedLoginStillActive = elapsedMs >= kLoginGracePeriodMs &&
                                        attempt.pageLoadObserved &&
                                        attempt.uiStateObserved &&
                                        attempt.loginFormStillVisible &&
                                        attempt.passwordFieldStillVisible;
  if (reloadedLoginStillActive) {
    cancelPendingSaveFlow(attemptKey, QStringLiteral("failure"));
    return;
  }

  const QUrl observedUrl = attempt.lastObservedUrl.isValid() ? attempt.lastObservedUrl : attempt.submitUrl;
  const QUrl normalizedObserved = observedUrl.adjusted(QUrl::RemoveFragment);
  const QUrl normalizedSubmit = attempt.submitUrl.adjusted(QUrl::RemoveFragment);
  const bool changedToAuthenticatedPage =
      (attempt.navigationObserved || attempt.pageLoadObserved) &&
      normalizedObserved.isValid() && normalizedObserved != normalizedSubmit &&
      !isLoginOrErrorUrl(normalizedObserved);

  const bool domIsClear = attempt.uiStateObserved &&
                          !attempt.loginFormStillVisible &&
                          !attempt.passwordFieldStillVisible &&
                          !attempt.errorStateObserved;
  const bool stableDomSuccess = domIsClear && elapsedMs >= kLoginGracePeriodMs &&
                                (attempt.successHintObserved ||
                                 attempt.authenticatedStateObserved ||
                                 attempt.consecutiveSuccessStates >= 2);

  // Origin-only success hints are retained for compatibility with the original
  // native bridge tests.  The production capture script now always sends the
  // verified form/password/error fields and therefore follows stableDomSuccess.
  const bool confirmed = changedToAuthenticatedPage || stableDomSuccess || attempt.legacySuccessHint;
  if (!confirmed) return;

  const QString candidate = candidateKeyForAttempt(attempt.view.data(), attempt);
  QPointer<QWebEngineView> view = attempt.view;
  attempt.confirmed = true;
  attempt.successEligible = false;
  loginAttempts_.erase(it);
  stopConfirmationTimerIfIdle();
  if (view && !candidate.isEmpty() && pendingCandidates_.contains(candidate)) {
    promptCandidate(view, candidate);
  }
}

void CredentialAutofillController::applyLoginUiState(const QString &attemptKey,
                                                       quint64 generation,
                                                       const QVariantMap &state) {
  auto it = loginAttempts_.find(attemptKey);
  if (it == loginAttempts_.end() || it->generation != generation || !it->successEligible) return;
  if (state.contains(QStringLiteral("ready")) && !state.value(QStringLiteral("ready")).toBool()) return;

  const QString stateOrigin = state.value(QStringLiteral("origin")).toString();
  if (stateOrigin.isEmpty() || !isSameSiteOrOrigin(it->origin, stateOrigin)) return;

  it->uiStateObserved = true;
  it->loginFormStillVisible = state.value(QStringLiteral("loginFormVisible"), true).toBool();
  it->passwordFieldStillVisible = state.value(QStringLiteral("passwordFieldVisible"), true).toBool();
  it->errorStateObserved = it->errorStateObserved || state.value(QStringLiteral("errorStateObserved"), false).toBool();
  it->authenticatedStateObserved = it->authenticatedStateObserved ||
                                   state.value(QStringLiteral("authenticatedStateObserved"), false).toBool();
  if (!it->loginFormStillVisible && !it->passwordFieldStillVisible && !it->errorStateObserved) {
    ++it->consecutiveSuccessStates;
  } else {
    it->consecutiveSuccessStates = 0;
  }
  evaluateLoginAttemptKey(attemptKey);
}

void CredentialAutofillController::requestLoginUiState(const QString &attemptKey,
                                                        const LoginAttemptState &attempt) {
  QPointer<CredentialAutofillController> guardedThis(this);
  QPointer<QWebEngineView> guardedView = attempt.view;
  if (!guardedView || !guardedView->page()) return;
  const quint64 generation = attempt.generation;
  guardedView->page()->runJavaScript(
      loginStateQueryScript(), QWebEngineScript::ApplicationWorld,
      [guardedThis, guardedView, attemptKey, generation](const QVariant &result) {
        if (!guardedThis || !guardedView) return;
        guardedThis->applyLoginUiState(attemptKey, generation, result.toMap());
      });
}

void CredentialAutofillController::pollLoginAttempts() {
  pruneExpiredData();
  const auto keys = loginAttempts_.keys();
  for (const QString &key : keys) {
    auto it = loginAttempts_.find(key);
    if (it == loginAttempts_.end()) continue;
    evaluateLoginAttemptKey(key);
    it = loginAttempts_.find(key);
    if (it != loginAttempts_.end()) requestLoginUiState(key, it.value());
  }
  stopConfirmationTimerIfIdle();
}

bool CredentialAutofillController::hasActiveSubmittedLoginAttempt(QWebEngineView *view, const QString &origin) const {
  if (!view || origin.isEmpty()) return false;
  const QString prefix = QString::number(reinterpret_cast<quintptr>(view)) + QLatin1Char(':');
  const QDateTime now = QDateTime::currentDateTimeUtc();

  const QString key = stageKey(view, origin);
  const auto it = loginAttempts_.find(key);
  if (it != loginAttempts_.end() && it->submitted && it->successEligible) {
    if (it->submittedAt.addSecs(kLoginAttemptTimeoutSecs) >= now) {
      return true;
    }
  }

  for (auto it2 = loginAttempts_.cbegin(); it2 != loginAttempts_.cend(); ++it2) {
    if (it2.key().startsWith(prefix) && it2->submitted && it2->successEligible) {
      if (it2->submittedAt.addSecs(kLoginAttemptTimeoutSecs) >= now) {
        if (isSameSiteOrOrigin(it2->origin, origin)) {
          return true;
        }
      }
    }
  }
  return false;
}

CredentialAutofillController::LoginAttemptState *CredentialAutofillController::findActiveSubmittedAttempt(
    QWebEngineView *view, const QString &origin) {
  if (!view || origin.isEmpty()) return nullptr;
  const QString prefix = QString::number(reinterpret_cast<quintptr>(view)) + QLatin1Char(':');
  const QDateTime now = QDateTime::currentDateTimeUtc();

  // 1. Direct exact match
  const QString exactKey = stageKey(view, origin);
  auto it = loginAttempts_.find(exactKey);
  if (it != loginAttempts_.end() && it->submitted && it->successEligible) {
    if (it->submittedAt.addSecs(kLoginAttemptTimeoutSecs) >= now) {
      return &it.value();
    }
  }

  // 2. Same-site match for this view
  for (auto it2 = loginAttempts_.begin(); it2 != loginAttempts_.end(); ++it2) {
    if (it2.key().startsWith(prefix) && it2->submitted && it2->successEligible) {
      if (it2->submittedAt.addSecs(kLoginAttemptTimeoutSecs) >= now) {
        if (isSameSiteOrOrigin(it2->origin, origin)) {
          return &it2.value();
        }
      }
    }
  }
  return nullptr;
}

QString CredentialAutofillController::candidateKeyForAttempt(
    QWebEngineView *view, const LoginAttemptState &attempt) const {
  if (!attempt.candidateKey.isEmpty() && pendingCandidates_.contains(attempt.candidateKey)) {
    return attempt.candidateKey;
  }
  const QString prefix = QString::number(reinterpret_cast<quintptr>(view)) + QLatin1Char(':');
  QDateTime newestTime;
  QString bestKey;
  for (auto it = pendingCandidates_.cbegin(); it != pendingCandidates_.cend(); ++it) {
    if (it.key().startsWith(prefix) && isSameSiteOrOrigin(it->origin, attempt.origin) && it->expiresAt > newestTime) {
      newestTime = it->expiresAt;
      bestKey = it.key();
    }
  }
  return bestKey;
}

void CredentialAutofillController::recordLoginAttemptSubmit(QWebEngineView *view, const QString &origin, const QString &nonce, const QString &username) {
  if (!view || origin.isEmpty() || !vaultManager_ || !vaultManager_->exists()) return;
  const QString key = stageKey(view, origin);
  LoginAttemptState attempt;
  attempt.view = view;
  attempt.origin = origin;
  attempt.username = username;
  attempt.submitted = true;
  attempt.submittedAt = QDateTime::currentDateTimeUtc();
  attempt.attemptNonce = nonce;
  attempt.successEligible = true;
  attempt.submitUrl = view->url();
  attempt.lastObservedUrl = attempt.submitUrl;
  attempt.generation = ++nextAttemptGeneration_;

  // Associate newest candidate key if available (same site or origin)
  const QString prefix = QString::number(reinterpret_cast<quintptr>(view)) + QLatin1Char(':');
  QDateTime newestExpiry;
  for (auto it = pendingCandidates_.cbegin(); it != pendingCandidates_.cend(); ++it) {
    if (it.key().startsWith(prefix) && isSameSiteOrOrigin(it->origin, origin) && it->expiresAt > newestExpiry) {
      newestExpiry = it->expiresAt;
      attempt.candidateKey = it.key();
      if (attempt.username.isEmpty()) attempt.username = it->username;
    }
  }
  loginAttempts_.insert(key, attempt);
  createPendingSaveFlow(key, attempt);
  if (!confirmationTimer_.isActive()) confirmationTimer_.start();
}

void CredentialAutofillController::clearLoginAttempt(QWebEngineView *view, const QString &origin) {
  if (!view) return;
  const QString key = stageKey(view, origin);
  if (pendingSaveFlows_.contains(key)) {
    cancelPendingSaveFlow(key, QStringLiteral("failure"));
  } else {
    loginAttempts_.remove(key);
  }
  stopConfirmationTimerIfIdle();
}
