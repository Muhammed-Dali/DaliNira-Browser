#pragma once

#include <QDateTime>
#include <QHash>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>
#include <QVector>
#include <QWebEngineScript>

#include "credential_save_bubble.h"

class QWebEngineView;
class QWebEnginePage;
class QWidget;
class CredentialVaultManager;
class VaultUnlockDialog;

enum class CredentialSaveFlowState {
  None,
  Verifying,
  ReadyNew,
  ReadyUpdate
};

class CredentialAutofillController final : public QObject {
  Q_OBJECT
 public:
  explicit CredentialAutofillController(CredentialVaultManager *vaultManager,
                                        QWidget *dialogParent = nullptr,
                                        QObject *parent = nullptr);
  ~CredentialAutofillController() override;

  // Script factories (isolated ApplicationWorld)
  static QWebEngineScript candidateCaptureScript();
  static QString webKitCandidateCaptureSource();
  static QString fillButtonScript(const QString &token);
  static QString domFillScript(const QString &origin, const QString &username, const QString &password,
                               const QUrl &expectedUrl = {}, const QString &documentToken = {});
  static QString loginStateQueryScript();

  // Tab and navigation lifecycle
  void onPageLoadFinished(QWebEngineView *view, bool success);
  void onUrlChanged(QWebEngineView *view, const QUrl &url);
  void onViewClosed(QWebEngineView *view);
  void onVaultLocked();
  void clearAllSensitiveData();

  // Console bridge router
  // Returns true if the message was handled by credential autofill.
  bool handleBridgeMessage(QWebEnginePage *page, const QString &message);
  bool handleBridgePayload(QWebEnginePage *page, const QString &type, const QVariantMap &payload);

  // User-initiated fill actions
  void triggerFillForView(QWebEngineView *view);
  void unlockVaultForFill(QWebEngineView *view);

  // Key / Token query helpers (useful for tests)
  QString stageKey(QWebEngineView *view, const QString &origin) const;
  QString candidateKey(QWebEngineView *view, const QString &origin, const QString &username) const;
  QString grantFillToken(QWebEngineView *view, const QString &origin);
  QString activeTokenForView(QWebEngineView *view, const QString &origin) const;
  bool isCandidatePending(QWebEngineView *view, const QString &origin, const QString &username) const;
  bool isFillButtonActiveForView(QWebEngineView *view, const QString &origin) const;
  void installFillButton(QWebEngineView *view);
  void removeFillButton(QWebEngineView *view);
  void refreshAutofillForViews();

  // Origin and login page helpers
  static bool isSameSiteOrOrigin(const QString &origin1, const QString &origin2);
  static bool isLoginOrErrorUrl(const QUrl &url);

  // Active unlock dialog accessor
  VaultUnlockDialog *activeUnlockDialog() const;

  // Save prompt and bubble management
  CredentialSaveBubble *activeSaveBubble() const;
  QWebEngineView *activeBubbleView() const;
  void dismissSaveBubble();
  void promptCandidate(QWebEngineView *view, const QString &candidateKey);

  // Explicit action triggers for testing and programmatic interaction
  void triggerSaveCandidate(const QString &candidateKey);
  void triggerUpdateCandidate(const QString &candidateKey);
  void triggerRejectCandidate(const QString &candidateKey);

  // Query candidate details for tests
  int pendingCandidateCount() const;
  bool hasPendingCandidate(const QString &candidateKey) const;
  bool hasPendingCredentialSaveFlow(QWebEngineView *view, const QString &origin) const;
  CredentialSaveFlowState credentialSaveFlowState(QWebEngineView *view, const QString &origin) const;
  QString lastSaveFlowEndReason() const;

  // Login attempt state queries and controls
  bool hasActiveSubmittedLoginAttempt(QWebEngineView *view, const QString &origin) const;
  void recordLoginAttemptSubmit(QWebEngineView *view, const QString &origin, const QString &nonce = QString(), const QString &username = QString());
  void clearLoginAttempt(QWebEngineView *view, const QString &origin);

 signals:
  void openPasswordManagerRequested();
  void saveBubbleShown(const QString &origin, const QString &username, CredentialSaveMode mode);
  void saveBubbleDismissed();
  void credentialSaved(const QString &origin, const QString &username);
  void credentialUpdated(const QString &origin, const QString &username);
  void reauthenticationRequested(const QString &origin);
  void credentialFillDispatched(const QString &origin, const QString &username);
  void saveFlowStateChanged(const QString &origin, const QString &username, CredentialSaveFlowState state);
  void saveFlowEnded(const QString &reasonCode);

 private:
  struct StagedUsername {
    QString username;
    QDateTime expiresAt;
  };

  struct PendingCandidate {
    QString origin;
    QString username;
    QString password;
    QUrl sourceUrl;
    QDateTime expiresAt;
    bool isUpdate = false;
    QString updateRecordId;
    QPointer<QWebEngineView> view;
  };

  struct LoginAttemptState {
    QPointer<QWebEngineView> view;
    QString origin;
    QString username;
    QString candidateKey;
    bool submitted = false;
    QDateTime submittedAt;
    QString attemptNonce;
    bool successEligible = false;
    QUrl submitUrl;
    QUrl lastObservedUrl;
    bool navigationObserved = false;
    bool pageLoadObserved = false;
    bool successHintObserved = false;
    bool legacySuccessHint = false;
    bool uiStateObserved = false;
    bool loginFormStillVisible = true;
    bool passwordFieldStillVisible = true;
    bool errorStateObserved = false;
    bool authenticatedStateObserved = false;
    bool confirmed = false;
    int consecutiveSuccessStates = 0;
    quint64 generation = 0;
  };

  struct PendingCredentialSaveFlow {
    QPointer<QWebEngineView> view;
    QString origin;
    QString username;
    QString candidateKey;
    QString attemptNonce;
    QDateTime createdAt;
    CredentialSaveFlowState state = CredentialSaveFlowState::Verifying;
    quint64 generation = 0;
  };

  struct PendingFillContext {
    QPointer<QWebEngineView> view;
    QPointer<QWebEnginePage> page;
    QString origin;
    QUrl url;
    QString recordId;
    QString vaultId;
    quint64 navigationGeneration = 0;
  };

  void pruneExpiredData();
  void trackView(QWebEngineView *view);
  bool isViewTracked(QWebEngineView *view) const;
  void handleCandidate(QWebEnginePage *page, const QString &payload);
  void handleSubmit(QWebEnginePage *page, const QString &payload);
  void handleStage(QWebEnginePage *page, const QString &payload);
  void handleSuccessHint(QWebEnginePage *page, const QString &payload);
  void handleLoginState(QWebEnginePage *page, const QString &payload);
  void handleFillRequest(QWebEnginePage *page, const QString &payload);
  QString selectCredentialForView(QWebEngineView *view);
  void beginFillReauthentication(QWebEngineView *view, const QString &recordId);
  void completePendingFill();
  bool pendingFillContextIsCurrent() const;
  void clearPendingFill();
  void unlockVaultForSave(QWebEngineView *view, const QString &candidateKey);
  LoginAttemptState* findActiveSubmittedAttempt(QWebEngineView *view, const QString &origin);
  QString candidateKeyForAttempt(QWebEngineView *view, const LoginAttemptState &attempt) const;
  void evaluateLoginAttempt(QWebEngineView *view, const QString &origin);
  void evaluateLoginAttemptKey(const QString &attemptKey);
  void pollLoginAttempts();
  void requestLoginUiState(const QString &attemptKey, const LoginAttemptState &attempt);
  void applyLoginUiState(const QString &attemptKey, quint64 generation, const QVariantMap &state);
  void stopConfirmationTimerIfIdle();
  void createPendingSaveFlow(const QString &attemptKey, const LoginAttemptState &attempt);
  void showVerifyingBubble(const QString &attemptKey, quint64 generation);
  void cancelPendingSaveFlow(const QString &attemptKey, const QString &reasonCode);
  void removeSaveFlowForCandidate(const QString &candidateKey, const QString &reasonCode = QString());
  bool hasSaveFlowForCandidate(const QString &candidateKey) const;

  CredentialVaultManager *vaultManager_ = nullptr;
  QWidget *dialogParent_ = nullptr;

  // Track active views for global autofill state refreshes
  QVector<QPointer<QWebEngineView>> activeViews_;

  // Tokens granted per view + origin for fill actions
  QMap<QString, QString> credentialFillTokens_;

  // Staged usernames for multi-step logins (5 min expiry)
  QMap<QString, StagedUsername> stagedUsernames_;

  // Candidates awaiting login success confirmation (120 sec expiry)
  QMap<QString, PendingCandidate> pendingCandidates_;

  // Login attempt states awaiting aggregated success confirmation.
  QMap<QString, LoginAttemptState> loginAttempts_;
  QMap<QString, PendingCredentialSaveFlow> pendingSaveFlows_;
  QTimer confirmationTimer_;
  quint64 nextAttemptGeneration_ = 0;
  static constexpr int kLoginAttemptTimeoutSecs = 12;
  static constexpr int kLoginGracePeriodMs = 1500;
  static constexpr int kConfirmationPollIntervalMs = 400;
  QString lastSaveFlowEndReason_;

  // Set of candidate keys currently being prompted to prevent duplicate dialogs
  QSet<QString> activePrompts_;

  // Modern VaultUnlockDialog reference and pending fill context
  QPointer<class VaultUnlockDialog> activeUnlockDialog_;
  PendingFillContext pendingFill_;
  QHash<QWebEngineView *, quint64> navigationGenerations_;

  // Native save bubble reference and active candidate key
  QPointer<CredentialSaveBubble> activeSaveBubble_;
  QPointer<QWebEngineView> activeBubbleView_;
  QString activeBubbleCandidateKey_;
  bool isPruning_ = false;
};

class CredentialMessageBridge final : public QObject {
  Q_OBJECT
 public:
  CredentialMessageBridge(QWebEnginePage *page, CredentialAutofillController *controller,
                          QObject *parent = nullptr);
 public slots:
  void send(const QString &type, const QVariantMap &payload);
 private:
  QPointer<QWebEnginePage> page_;
  QPointer<CredentialAutofillController> controller_;
};
