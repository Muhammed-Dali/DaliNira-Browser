#include "vault_unlock_dialog.h"
#include "credential_vault_manager.h"
#include "core/browser_icons.h"

#include <QApplication>
#include <QFutureWatcher>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QShowEvent>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtConcurrent>
#include "i18n/i18n.h"

namespace {

QIcon passwordVisibilityIcon(bool visible) {
  QPixmap pixmap(20, 20);
  pixmap.fill(Qt::transparent);
  QPainter painter(&pixmap);
  painter.setRenderHint(QPainter::Antialiasing);
  QPen pen(QColor(QStringLiteral("#c7d2df")));
  pen.setWidthF(1.7);
  painter.setPen(pen);
  painter.setBrush(Qt::NoBrush);
  painter.drawEllipse(QRectF(3, 6, 14, 8));
  painter.setBrush(QColor(QStringLiteral("#c7d2df")));
  painter.drawEllipse(QRectF(8, 8, 4, 4));
  if (!visible) {
    QPen slash(QColor(QStringLiteral("#c7d2df")));
    slash.setWidthF(2.0);
    painter.setPen(slash);
    painter.drawLine(QPointF(3.5, 3.5), QPointF(16.5, 16.5));
  }
  return QIcon(pixmap);
}

}  // namespace

VaultUnlockDialog::VaultUnlockDialog(CredentialVaultManager *vaultManager,
                                     const QString &canonicalOrigin,
                                     QWidget *parent,
                                     Purpose purpose,
                                     const QString &vaultId)
    : QDialog(parent, Qt::Dialog | Qt::FramelessWindowHint),
      vaultManager_(vaultManager),
      canonicalOrigin_(canonicalOrigin),
      vaultId_(vaultId),
      purpose_(purpose) {
  setAttribute(Qt::WA_TranslucentBackground, true);
  setAttribute(Qt::WA_DeleteOnClose, false);
  setModal(true);

  initializeUi();
  setCanonicalOrigin(canonicalOrigin_);
  checkExistingCooldown();
}

VaultUnlockDialog::~VaultUnlockDialog() {
  if (passwordInput_) {
    passwordInput_->clear();
  }
}

void VaultUnlockDialog::initializeUi() {
  setObjectName(QStringLiteral("vault-unlock-dialog"));
  setFixedWidth(400);

  auto *rootLayout = new QVBoxLayout(this);
  rootLayout->setContentsMargins(12, 12, 12, 12);

  card_ = new QFrame(this);
  card_->setObjectName(QStringLiteral("vault-unlock-card"));
  card_->setAttribute(Qt::WA_StyledBackground, true);

  auto *shadow = new QGraphicsDropShadowEffect(this);
  shadow->setBlurRadius(24);
  shadow->setOffset(0, 8);
  shadow->setColor(QColor(0, 0, 0, 180));
  card_->setGraphicsEffect(shadow);

  card_->setStyleSheet(QStringLiteral(
      "QFrame#vault-unlock-card {"
      "  background-color: #181f28;"
      "  border: 1px solid #2e3b49;"
      "  border-radius: 12px;"
      "}"
      "QLabel#vault-unlock-title {"
      "  font-size: 14px;"
      "  font-weight: 700;"
      "  color: #f3f7fc;"
      "}"
      "QLabel#vault-unlock-origin {"
      "  font-size: 11px;"
      "  font-weight: 600;"
      "  color: #60a5fa;"
      "  background-color: #101926;"
      "  border: 1px solid #1e3a5f;"
      "  border-radius: 6px;"
      "  padding: 3px 8px;"
      "}"
      "QLabel#vault-unlock-desc {"
      "  font-size: 12px;"
      "  color: #92a1b3;"
      "  line-height: 1.4;"
      "}"
      "QLabel#vault-unlock-field-label {"
      "  font-size: 12px;"
      "  font-weight: 600;"
      "  color: #e4ebf5;"
      "}"
      "QLineEdit#vault-unlock-password {"
      "  background-color: #12171f;"
      "  border: 1px solid #2e3b49;"
      "  border-radius: 8px;"
      "  color: #f3f7fc;"
      "  font-size: 13px;"
      "  padding: 8px 34px 8px 10px;"
      "}"
      "QLineEdit#vault-unlock-password:focus {"
      "  border-color: #3b82f6;"
      "}"
      "QLineEdit#vault-unlock-password:disabled {"
      "  background-color: #1a222d;"
      "  color: #64748b;"
      "  border-color: #232d39;"
      "}"
      "QLabel#vault-unlock-error {"
      "  color: #f87171;"
      "  font-size: 11px;"
      "  font-weight: 500;"
      "}"
      "QPushButton#vault-unlock-cancel {"
      "  background-color: #222d3b;"
      "  border: 1px solid #2e3b49;"
      "  border-radius: 8px;"
      "  color: #c7d2df;"
      "  font-size: 12px;"
      "  font-weight: 500;"
      "  padding: 7px 16px;"
      "}"
      "QPushButton#vault-unlock-cancel:hover {"
      "  background-color: #2c3a4d;"
      "  color: #f3f7fc;"
      "}"
      "QPushButton#vault-unlock-submit {"
      "  background-color: #2563eb;"
      "  border: none;"
      "  border-radius: 8px;"
      "  color: #ffffff;"
      "  font-size: 12px;"
      "  font-weight: 600;"
      "  padding: 7px 18px;"
      "}"
      "QPushButton#vault-unlock-submit:hover {"
      "  background-color: #3b82f6;"
      "}"
      "QPushButton#vault-unlock-submit:pressed {"
      "  background-color: #1d4ed8;"
      "}"
      "QPushButton#vault-unlock-submit:disabled {"
      "  background-color: #1e3a5f;"
      "  color: #92a1b3;"
      "}"
      "QToolButton#vault-unlock-close {"
      "  background: transparent;"
      "  border: none;"
      "  color: #92a1b3;"
      "  font-size: 14px;"
      "  padding: 4px;"
      "  border-radius: 6px;"
      "}"
      "QToolButton#vault-unlock-close:hover {"
      "  background-color: #222d3b;"
      "  color: #f3f7fc;"
      "}"));

  auto *cardLayout = new QVBoxLayout(card_);
  cardLayout->setContentsMargins(18, 16, 18, 16);
  cardLayout->setSpacing(12);

  // Header row
  auto *headerLayout = new QHBoxLayout();
  headerLayout->setSpacing(10);

  auto *iconLabel = new QLabel(card_);
  iconLabel->setFixedSize(22, 22);
  const QPixmap iconPix = BrowserIcons::icon(BrowserIcon::Password).pixmap(20, 20);
  if (!iconPix.isNull()) {
    iconLabel->setPixmap(iconPix);
  } else {
    iconLabel->setText(QStringLiteral("🔒"));
    iconLabel->setAlignment(Qt::AlignCenter);
  }
  headerLayout->addWidget(iconLabel);

  titleLabel_ = new QLabel(
      purpose_ == Purpose::ReauthenticateForAutofill
          ? I18n::text(QStringLiteral("passwords.verify_title"))
          : I18n::text(QStringLiteral("passwords.unlock_title")),
      card_);
  titleLabel_->setObjectName(QStringLiteral("vault-unlock-title"));
  headerLayout->addWidget(titleLabel_, 1);

  closeButton_ = new QToolButton(card_);
  closeButton_->setObjectName(QStringLiteral("vault-unlock-close"));
  closeButton_->setText(QStringLiteral("✕"));
  closeButton_->setCursor(Qt::PointingHandCursor);
  closeButton_->setToolTip(I18n::text(QStringLiteral("common.close")));
  connect(closeButton_, &QToolButton::clicked, this, &VaultUnlockDialog::reject);
  headerLayout->addWidget(closeButton_);

  cardLayout->addLayout(headerLayout);

  // Origin badge (optional)
  originBadge_ = new QLabel(card_);
  originBadge_->setObjectName(QStringLiteral("vault-unlock-origin"));
  originBadge_->setVisible(false);
  cardLayout->addWidget(originBadge_);

  // Description
  descLabel_ = new QLabel(
      purpose_ == Purpose::ReauthenticateForAutofill
          ? I18n::text(QStringLiteral("passwords.verify_desc"))
          : I18n::text(QStringLiteral("passwords.unlock_desc")),
      card_);
  descLabel_->setObjectName(QStringLiteral("vault-unlock-desc"));
  descLabel_->setWordWrap(true);
  cardLayout->addWidget(descLabel_);

  cardLayout->addSpacing(4);

  // Field label
  auto *fieldLabel = new QLabel(I18n::text(QStringLiteral("passwords.master_password")), card_);
  fieldLabel->setObjectName(QStringLiteral("vault-unlock-field-label"));
  cardLayout->addWidget(fieldLabel);

  // Password Input with show/hide eye action
  passwordInput_ = new QLineEdit(card_);
  passwordInput_->setObjectName(QStringLiteral("vault-unlock-password"));
  passwordInput_->setEchoMode(QLineEdit::Password);
  passwordInput_->setMaxLength(256);
  passwordInput_->setPlaceholderText(I18n::text(QStringLiteral("passwords.enter_master")));
  passwordInput_->setAccessibleName(I18n::text(QStringLiteral("passwords.master_password")));

  auto *toggleEye = passwordInput_->addAction(passwordVisibilityIcon(false), QLineEdit::TrailingPosition);
  toggleEyeAction_ = toggleEye;
  toggleEye->setToolTip(I18n::text(QStringLiteral("passwords.show_password")));
  connect(toggleEye, &QAction::triggered, this, [this, toggleEye] {
    const bool makeVisible = passwordInput_->echoMode() != QLineEdit::Normal;
    passwordInput_->setEchoMode(makeVisible ? QLineEdit::Normal : QLineEdit::Password);
    toggleEye->setIcon(passwordVisibilityIcon(makeVisible));
    toggleEye->setToolTip(makeVisible ? I18n::text(QStringLiteral("passwords.hide_password")) : I18n::text(QStringLiteral("passwords.show_password")));
    if (makeVisible) {
      QTimer::singleShot(15000, passwordInput_, [this, toggleEye] {
        if (passwordInput_->echoMode() == QLineEdit::Normal) {
          passwordInput_->setEchoMode(QLineEdit::Password);
          toggleEye->setIcon(passwordVisibilityIcon(false));
          toggleEye->setToolTip(I18n::text(QStringLiteral("passwords.show_password")));
        }
      });
    }
  });

  connect(passwordInput_, &QLineEdit::textEdited, this, &VaultUnlockDialog::clearError);
  connect(passwordInput_, &QLineEdit::returnPressed, this, &VaultUnlockDialog::attemptUnlock);
  cardLayout->addWidget(passwordInput_);

  // Error label
  errorLabel_ = new QLabel(card_);
  errorLabel_->setObjectName(QStringLiteral("vault-unlock-error"));
  errorLabel_->setWordWrap(true);
  errorLabel_->setVisible(false);
  cardLayout->addWidget(errorLabel_);

  cardLayout->addSpacing(4);

  // Buttons row
  auto *btnLayout = new QHBoxLayout();
  btnLayout->setSpacing(8);

  cancelButton_ = new QPushButton(I18n::text(QStringLiteral("common.cancel")), card_);
  cancelButton_->setObjectName(QStringLiteral("vault-unlock-cancel"));
  cancelButton_->setCursor(Qt::PointingHandCursor);
  connect(cancelButton_, &QPushButton::clicked, this, &VaultUnlockDialog::reject);
  btnLayout->addWidget(cancelButton_);

  btnLayout->addStretch(1);

  unlockButton_ = new QPushButton(
      purpose_ == Purpose::ReauthenticateForAutofill ? I18n::text(QStringLiteral("passwords.verify_btn"))
                                                      : I18n::text(QStringLiteral("passwords.unlock_btn")),
      card_);
  unlockButton_->setObjectName(QStringLiteral("vault-unlock-submit"));
  unlockButton_->setCursor(Qt::PointingHandCursor);
  connect(unlockButton_, &QPushButton::clicked, this, &VaultUnlockDialog::attemptUnlock);
  btnLayout->addWidget(unlockButton_);

  cardLayout->addLayout(btnLayout);
  rootLayout->addWidget(card_);

  setTabOrder(passwordInput_, unlockButton_);
  setTabOrder(unlockButton_, cancelButton_);
}

void VaultUnlockDialog::setCanonicalOrigin(const QString &origin) {
  canonicalOrigin_ = origin;
  if (!originBadge_) return;

  if (canonicalOrigin_.isEmpty()) {
    originBadge_->setVisible(false);
  } else {
    // Extract host for clean badge display (e.g. www.facebook.com)
    QUrl url(canonicalOrigin_);
    const QString displayHost = url.isValid() && !url.host().isEmpty() ? url.host() : canonicalOrigin_;
    originBadge_->setText(I18n::text(QStringLiteral("passwords.saved_login_badge")).arg(displayHost));
    originBadge_->setVisible(true);
  }
}

void VaultUnlockDialog::setError(const QString &errorMessage) {
  if (!errorLabel_) return;
  errorLabel_->setText(errorMessage);
  errorLabel_->setVisible(!errorMessage.isEmpty());
}

void VaultUnlockDialog::clearError() {
  if (errorLabel_ && errorLabel_->isVisible()) {
    errorLabel_->clear();
    errorLabel_->setVisible(false);
  }
}

QString VaultUnlockDialog::formatCooldownTime(int totalSeconds) {
  if (totalSeconds < 0) totalSeconds = 0;
  const int minutes = totalSeconds / 60;
  const int seconds = totalSeconds % 60;
  return QStringLiteral("%1:%2")
      .arg(minutes, 2, 10, QLatin1Char('0'))
      .arg(seconds, 2, 10, QLatin1Char('0'));
}

void VaultUnlockDialog::startCooldown(int seconds) {
  remainingCooldown_ = seconds;
  if (!cooldownTimer_) {
    cooldownTimer_ = new QTimer(this);
    cooldownTimer_->setInterval(1000);
    connect(cooldownTimer_, &QTimer::timeout, this, [this] {
      if (!vaultManager_) return;
      const int rem = vaultId_.isEmpty() ? vaultManager_->remainingUnlockCooldownSeconds()
                                         : vaultManager_->remainingVaultUnlockCooldownSeconds(vaultId_);
      if (rem <= 0) {
        stopCooldown();
      } else {
        remainingCooldown_ = rem;
        updateCooldownUi(rem);
      }
    });
  }

  if (passwordInput_) {
    passwordInput_->clear();
    passwordInput_->setEnabled(false);
  }
  if (toggleEyeAction_) {
    toggleEyeAction_->setEnabled(false);
  }
  if (unlockButton_) {
    unlockButton_->setEnabled(false);
    unlockButton_->setText(purpose_ == Purpose::ReauthenticateForAutofill
                               ? I18n::text(QStringLiteral("passwords.verify_btn"))
                               : I18n::text(QStringLiteral("passwords.unlock_btn")));
  }
  if (cancelButton_) {
    cancelButton_->setEnabled(true);
  }
  if (closeButton_) {
    closeButton_->setEnabled(true);
  }

  updateCooldownUi(remainingCooldown_);
  if (!cooldownTimer_->isActive()) {
    cooldownTimer_->start();
  }
}

void VaultUnlockDialog::stopCooldown() {
  remainingCooldown_ = 0;
  if (cooldownTimer_ && cooldownTimer_->isActive()) {
    cooldownTimer_->stop();
  }
  if (passwordInput_) {
    passwordInput_->setEnabled(true);
    passwordInput_->setAccessibleDescription(QString());
    passwordInput_->setFocus();
  }
  if (toggleEyeAction_) {
    toggleEyeAction_->setEnabled(true);
  }
  if (unlockButton_) {
    unlockButton_->setEnabled(true);
    unlockButton_->setText(purpose_ == Purpose::ReauthenticateForAutofill
                               ? I18n::text(QStringLiteral("passwords.verify_btn"))
                               : I18n::text(QStringLiteral("passwords.unlock_btn")));
  }
  clearError();
}

void VaultUnlockDialog::updateCooldownUi(int seconds) {
  if (!errorLabel_) return;
  const QString formatted = formatCooldownTime(seconds);
  errorLabel_->setText(I18n::text(QStringLiteral("passwords.too_many_attempts")).arg(formatted));
  errorLabel_->setVisible(true);
  errorLabel_->setAccessibleDescription(I18n::text(QStringLiteral("passwords.retry_seconds_left")).arg(seconds));
  if (passwordInput_) {
    passwordInput_->setAccessibleDescription(I18n::text(QStringLiteral("passwords.retry_seconds_left")).arg(seconds));
  }
}

void VaultUnlockDialog::checkExistingCooldown() {
  if (!vaultManager_) return;
  const int remaining = vaultId_.isEmpty() ? vaultManager_->remainingUnlockCooldownSeconds()
                                           : vaultManager_->remainingVaultUnlockCooldownSeconds(vaultId_);
  if (remaining > 0) {
    startCooldown(remaining);
  } else if (isCooldownActive()) {
    stopCooldown();
  }
}

void VaultUnlockDialog::showEvent(QShowEvent *event) {
  QDialog::showEvent(event);
  checkExistingCooldown();
}

void VaultUnlockDialog::attemptUnlock() {
  if (isChecking_ || isCooldownActive() || !vaultManager_) return;

  const int preCooldown = vaultId_.isEmpty() ? vaultManager_->remainingUnlockCooldownSeconds()
                                             : vaultManager_->remainingVaultUnlockCooldownSeconds(vaultId_);
  if (preCooldown > 0) {
    startCooldown(preCooldown);
    return;
  }

  const QString masterPassword = passwordInput_->text();
  if (masterPassword.isEmpty()) {
    setError(I18n::text(QStringLiteral("passwords.enter_master_please")));
    passwordInput_->setFocus();
    return;
  }

  isChecking_ = true;
  clearError();
  unlockButton_->setEnabled(false);
  unlockButton_->setText(I18n::text(QStringLiteral("common.verifying")));
  cancelButton_->setEnabled(false);

  auto *watcher = new QFutureWatcher<bool>(this);
  connect(watcher, &QFutureWatcher<bool>::finished, this, [this, watcher] {
    const bool ok = watcher->result();
    watcher->deleteLater();
    isChecking_ = false;

    if (ok) {
      if (passwordInput_) passwordInput_->clear();
      emit unlocked();
      accept();
      return;
    }

    const int cooldown = vaultId_.isEmpty() ? vaultManager_->remainingUnlockCooldownSeconds()
                                            : vaultManager_->remainingVaultUnlockCooldownSeconds(vaultId_);
    if (cooldown > 0) {
      startCooldown(cooldown);
      return;
    }

    // Normal failure (attempts 1-4)
    unlockButton_->setEnabled(true);
    unlockButton_->setText(purpose_ == Purpose::ReauthenticateForAutofill
                               ? I18n::text(QStringLiteral("passwords.verify_btn"))
                               : I18n::text(QStringLiteral("passwords.unlock_btn")));
    cancelButton_->setEnabled(true);
    if (passwordInput_) {
      passwordInput_->clear();
      passwordInput_->setFocus();
    }
    setError(purpose_ == Purpose::ReauthenticateForAutofill
                 ? I18n::text(QStringLiteral("passwords.master_unverified"))
                 : I18n::text(QStringLiteral("passwords.master_wrong")));
  });

  watcher->setFuture(QtConcurrent::run([vaultManager = vaultManager_, masterPassword, vaultId = vaultId_] {
    return vaultId.isEmpty() ? vaultManager->unlock(masterPassword)
                             : vaultManager->unlockVault(vaultId, masterPassword);
  }));
}

void VaultUnlockDialog::keyPressEvent(QKeyEvent *event) {
  if (!event) {
    QDialog::keyPressEvent(event);
    return;
  }
  if (event->key() == Qt::Key_Escape) {
    if (!isChecking_) {
      reject();
    }
    event->accept();
    return;
  }
  if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
    if (!isChecking_ && !isCooldownActive()) {
      attemptUnlock();
    }
    event->accept();
    return;
  }
  QDialog::keyPressEvent(event);
}
