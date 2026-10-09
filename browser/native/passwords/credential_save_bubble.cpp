#include "credential_save_bubble.h"
#include "../core/browser_icons.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStyleOption>
#include "i18n/i18n.h"

CredentialSaveBubble::CredentialSaveBubble(QWidget *parent) : QFrame(parent) {
  setAttribute(Qt::WA_DeleteOnClose, false);
  setWindowFlags(Qt::SubWindow | Qt::FramelessWindowHint);
  setObjectName(QStringLiteral("credentialSaveBubble"));
  initializeUi();
  qApp->installEventFilter(this);
}

CredentialSaveBubble::~CredentialSaveBubble() {
  if (qApp) {
    qApp->removeEventFilter(this);
  }
}

void CredentialSaveBubble::initializeUi() {
  setFixedWidth(360);
  setStyleSheet(QStringLiteral(
      "QFrame#credentialSaveBubble {"
      "  background-color: #101722;"
      "  border: 1px solid #29364a;"
      "  border-radius: 14px;"
      "}"));

  auto *rootLayout = new QVBoxLayout(this);
  rootLayout->setContentsMargins(16, 14, 16, 14);
  rootLayout->setSpacing(10);

  // Top header row: Icon + Title + Close Button
  auto *headerLayout = new QHBoxLayout();
  headerLayout->setContentsMargins(0, 0, 0, 0);
  headerLayout->setSpacing(8);

  iconLabel_ = new QLabel(this);
  iconLabel_->setFixedSize(22, 22);
  iconLabel_->setPixmap(BrowserIcons::icon(BrowserIcon::Password).pixmap(20, 20));
  iconLabel_->setStyleSheet(QStringLiteral("border: none; background: transparent;"));
  headerLayout->addWidget(iconLabel_);

  titleLabel_ = new QLabel(this);
  titleLabel_->setStyleSheet(QStringLiteral("font-size: 13px; font-weight: 700; color: #eef4fb; border: none; background: transparent;"));
  titleLabel_->setWordWrap(true);
  headerLayout->addWidget(titleLabel_, 1);

  closeBtn_ = new QToolButton(this);
  closeBtn_->setText(QStringLiteral("✕"));
  closeBtn_->setCursor(Qt::PointingHandCursor);
  closeBtn_->setToolTip(I18n::text(QStringLiteral("common.close")));
  closeBtn_->setStyleSheet(QStringLiteral(
      "QToolButton {"
      "  color: #94a3b8;"
      "  border: none;"
      "  background: transparent;"
      "  font-size: 13px;"
      "  padding: 2px 4px;"
      "  border-radius: 4px;"
      "}"
      "QToolButton:hover {"
      "  color: #f1f5f9;"
      "  background-color: #1e293b;"
      "}"
      "QToolButton:pressed {"
      "  color: #ffffff;"
      "  background-color: #0f172a;"
      "  padding-top: 3px;"
      "  padding-bottom: 1px;"
      "}"));
  connect(closeBtn_, &QToolButton::clicked, this, &CredentialSaveBubble::clickClose);
  headerLayout->addWidget(closeBtn_);

  rootLayout->addLayout(headerLayout);

  // Details box: Origin & Username
  auto *detailsBox = new QFrame(this);
  detailsBox->setStyleSheet(QStringLiteral(
      "QFrame {"
      "  background-color: #0b111a;"
      "  border: 1px solid #1e293b;"
      "  border-radius: 8px;"
      "  padding: 4px;"
      "}"));
  auto *detailsLayout = new QVBoxLayout(detailsBox);
  detailsLayout->setContentsMargins(10, 8, 10, 8);
  detailsLayout->setSpacing(4);

  originLabel_ = new QLabel(detailsBox);
  originLabel_->setStyleSheet(QStringLiteral("font-size: 12px; font-weight: 600; color: #38bdf8; border: none; background: transparent;"));
  originLabel_->setTextInteractionFlags(Qt::NoTextInteraction);
  detailsLayout->addWidget(originLabel_);

  userLabel_ = new QLabel(detailsBox);
  userLabel_->setStyleSheet(QStringLiteral("font-size: 12px; color: #cbd5e1; border: none; background: transparent;"));
  userLabel_->setTextInteractionFlags(Qt::NoTextInteraction);
  detailsLayout->addWidget(userLabel_);

  rootLayout->addWidget(detailsBox);

  // Description label (used in Verifying and Cancelled modes)
  descriptionLabel_ = new QLabel(this);
  descriptionLabel_->setStyleSheet(QStringLiteral("font-size: 12px; color: #94a3b8; border: none; background: transparent; line-height: 1.4;"));
  descriptionLabel_->setWordWrap(true);
  descriptionLabel_->hide();
  rootLayout->addWidget(descriptionLabel_);

  // Bottom action buttons row
  auto *btnLayout = new QHBoxLayout();
  btnLayout->setContentsMargins(0, 4, 0, 0);
  btnLayout->setSpacing(8);

  secondaryBtn_ = new QPushButton(I18n::text(QStringLiteral("passwords.not_now")), this);
  secondaryBtn_->setObjectName(QStringLiteral("credential-save-secondary"));
  secondaryBtn_->setCursor(Qt::PointingHandCursor);
  secondaryBtn_->setStyleSheet(QStringLiteral(
      "QPushButton {"
      "  background-color: #1e293b;"
      "  color: #cbd5e1;"
      "  border: 1px solid #334155;"
      "  border-radius: 6px;"
      "  padding: 6px 12px;"
      "  font-size: 12px;"
      "  font-weight: 500;"
      "}"
      "QPushButton:hover {"
      "  background-color: #3d4d63;"
      "  color: #f8fafc;"
      "  border-color: #64748b;"
      "}"
      "QPushButton:pressed {"
      "  background-color: #172033;"
      "  border-color: #7dd3fc;"
      "  padding-top: 7px;"
      "  padding-bottom: 5px;"
      "}"));
  connect(secondaryBtn_, &QPushButton::clicked, this, &CredentialSaveBubble::clickSecondary);
  btnLayout->addWidget(secondaryBtn_);

  primaryBtn_ = new QPushButton(this);
  primaryBtn_->setObjectName(QStringLiteral("credential-save-primary"));
  primaryBtn_->setCursor(Qt::PointingHandCursor);
  primaryBtn_->setStyleSheet(QStringLiteral(
      "QPushButton {"
      "  background-color: #16b8e7;"
      "  color: #031525;"
      "  border: 1px solid transparent;"
      "  border-radius: 6px;"
      "  padding: 6px 14px;"
      "  font-size: 12px;"
      "  font-weight: 700;"
      "}"
      "QPushButton:hover {"
      "  background-color: #38cbf6;"
      "  border-color: #a5efff;"
      "}"
      "QPushButton:pressed {"
      "  background-color: #0891b2;"
      "  border-color: #67e8f9;"
      "  padding-top: 7px;"
      "  padding-bottom: 5px;"
      "}"));
  connect(primaryBtn_, &QPushButton::clicked, this, &CredentialSaveBubble::clickPrimary);
  btnLayout->addWidget(primaryBtn_, 1);

  rootLayout->addLayout(btnLayout);
}

void CredentialSaveBubble::setupPrompt(CredentialSaveMode mode, const QString &origin, const QString &username) {
  mode_ = mode;
  origin_ = origin;
  username_ = username;

  originLabel_->setText(origin_);
  userLabel_->setText(username_.isEmpty() ? QStringLiteral("—") : username_);

  switch (mode_) {
    case CredentialSaveMode::Verifying:
      titleLabel_->setText(I18n::text(QStringLiteral("passwords.save_prompt_new")));
      descriptionLabel_->setText(I18n::text(QStringLiteral("passwords.verifying")));
      descriptionLabel_->show();
      primaryBtn_->setText(I18n::text(QStringLiteral("common.save")));
      primaryBtn_->setEnabled(false);
      secondaryBtn_->setText(I18n::text(QStringLiteral("passwords.not_now")));
      break;

    case CredentialSaveMode::ReadyNew:
      titleLabel_->setText(I18n::text(QStringLiteral("passwords.save_prompt_new")));
      descriptionLabel_->hide();
      primaryBtn_->setText(I18n::text(QStringLiteral("common.save")));
      primaryBtn_->setEnabled(true);
      secondaryBtn_->setText(I18n::text(QStringLiteral("passwords.not_now")));
      break;

    case CredentialSaveMode::ReadyUpdate:
      titleLabel_->setText(I18n::text(QStringLiteral("passwords.save_prompt_update")));
      descriptionLabel_->hide();
      primaryBtn_->setText(I18n::text(QStringLiteral("passwords.update")));
      primaryBtn_->setEnabled(true);
      secondaryBtn_->setText(I18n::text(QStringLiteral("passwords.not_now")));
      break;

    case CredentialSaveMode::Cancelled:
      descriptionLabel_->setText(I18n::text(QStringLiteral("passwords.failed")));
      descriptionLabel_->show();
      primaryBtn_->setText(I18n::text(QStringLiteral("common.save")));
      primaryBtn_->setEnabled(false);
      secondaryBtn_->setText(I18n::text(QStringLiteral("common.close")));
      break;
  }

  adjustSize();
}

void CredentialSaveBubble::clickPrimary() {
  if (!primaryBtn_ || !primaryBtn_->isEnabled()) return;
  emit saveAccepted();
}

void CredentialSaveBubble::clickSecondary() {
  emit saveRejected();
  hide();
}

void CredentialSaveBubble::clickClose() {
  emit saveRejected();
  hide();
}

bool CredentialSaveBubble::eventFilter(QObject *watched, QEvent *event) {
  Q_UNUSED(watched);
  if (!isVisible()) return false;
  if (event->type() == QEvent::KeyPress) {
    const auto *key = static_cast<QKeyEvent *>(event);
    if (key->key() == Qt::Key_Escape) {
      clickClose();
      return true;
    }
  }
  return false;
}

void CredentialSaveBubble::keyPressEvent(QKeyEvent *event) {
  if (event->key() == Qt::Key_Escape) {
    clickClose();
    event->accept();
    return;
  }
  QFrame::keyPressEvent(event);
}

void CredentialSaveBubble::paintEvent(QPaintEvent *event) {
  QFrame::paintEvent(event);
}
