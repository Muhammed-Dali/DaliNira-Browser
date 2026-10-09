#pragma once

#include <QDialog>
#include <QPointer>

class QAction;
class QFrame;
class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;
class QToolButton;
class CredentialVaultManager;

class VaultUnlockDialog : public QDialog {
  Q_OBJECT

 public:
  enum class Purpose { Unlock, ReauthenticateForAutofill };

  explicit VaultUnlockDialog(CredentialVaultManager *vaultManager,
                             const QString &canonicalOrigin = {},
                             QWidget *parent = nullptr,
                             Purpose purpose = Purpose::Unlock,
                             const QString &vaultId = {});
  ~VaultUnlockDialog() override;

  QString canonicalOrigin() const { return canonicalOrigin_; }
  QString vaultId() const { return vaultId_; }
  Purpose purpose() const { return purpose_; }
  void setCanonicalOrigin(const QString &origin);

  QLineEdit *passwordInput() const { return passwordInput_; }
  QLabel *errorLabel() const { return errorLabel_; }
  QPushButton *unlockButton() const { return unlockButton_; }
  QPushButton *cancelButton() const { return cancelButton_; }

  void setError(const QString &errorMessage);
  void clearError();
  bool isChecking() const { return isChecking_; }
  bool isCooldownActive() const { return remainingCooldown_ > 0; }
  int remainingCooldown() const { return remainingCooldown_; }

  static QString formatCooldownTime(int totalSeconds);
  void checkExistingCooldown();

 public slots:
  void attemptUnlock();

 signals:
  void unlocked();

 protected:
  void keyPressEvent(QKeyEvent *event) override;
  void showEvent(QShowEvent *event) override;

 private:
  void initializeUi();
  void startCooldown(int seconds);
  void stopCooldown();
  void updateCooldownUi(int seconds);

  CredentialVaultManager *vaultManager_ = nullptr;
  QString canonicalOrigin_;
  QString vaultId_;
  Purpose purpose_ = Purpose::Unlock;
  bool isChecking_ = false;

  QFrame *card_ = nullptr;
  QLabel *titleLabel_ = nullptr;
  QLabel *originBadge_ = nullptr;
  QLabel *descLabel_ = nullptr;
  QLineEdit *passwordInput_ = nullptr;
  QLabel *errorLabel_ = nullptr;
  QPushButton *cancelButton_ = nullptr;
  QPushButton *unlockButton_ = nullptr;
  QToolButton *closeButton_ = nullptr;
  QAction *toggleEyeAction_ = nullptr;
  QTimer *cooldownTimer_ = nullptr;
  int remainingCooldown_ = 0;
};
