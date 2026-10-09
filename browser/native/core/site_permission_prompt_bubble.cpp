#include "site_permission_prompt_bubble.h"

#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QPainter>
#include <QStyleOption>
#include "i18n/i18n.h"

namespace {
QString formatOriginDisplay(const QUrl &url) {
  if (!url.isValid()) return I18n::text(QStringLiteral("permission.unknown_site"));
  QString host = url.host().trimmed();
  if (host.isEmpty()) return url.toDisplayString();
  const int port = url.port();
  if (port > 0 && port != 80 && port != 443) {
    host += QStringLiteral(":%1").arg(port);
  }
  return host;
}

QString canonicalOriginFromUrl(const QUrl &url) {
  if (!url.isValid()) return {};
  const QString scheme = url.scheme().toLower();
  const QString host = url.host().toLower();
  if (host.isEmpty()) return {};
  const int port = url.port();
  if (port > 0 && port != 80 && port != 443) {
    return QStringLiteral("%1://%2:%3").arg(scheme, host).arg(port);
  }
  return QStringLiteral("%1://%2").arg(scheme, host);
}
}  // namespace

SitePermissionPromptBubble::SitePermissionPromptBubble(QWidget *parent)
    : QFrame(parent, Qt::FramelessWindowHint) {
  initializeUi();
}

void SitePermissionPromptBubble::initializeUi() {
  setObjectName(QStringLiteral("site-permission-prompt-bubble"));
  setAttribute(Qt::WA_StyledBackground, true);
  setFixedWidth(380);

  setStyleSheet(QStringLiteral(
      "QFrame#site-permission-prompt-bubble {"
      "  background-color: #181f28;"
      "  border: 1px solid #2e3b49;"
      "  border-radius: 12px;"
      "  color: #e4ebf5;"
      "}"
      "QLabel#permission-origin-label {"
      "  font-size: 13px;"
      "  font-weight: 700;"
      "  color: #f3f7fc;"
      "}"
      "QLabel#permission-prompt-label {"
      "  font-size: 13px;"
      "  font-weight: 450;"
      "  color: #cbd7e4;"
      "  line-height: 1.4;"
      "  padding: 4px 0 10px 0;"
      "}"
      "QToolButton#permission-close-btn {"
      "  background: transparent;"
      "  border: none;"
      "  border-radius: 6px;"
      "  color: #92a1b3;"
      "  font-size: 16px;"
      "  font-weight: bold;"
      "  padding: 2px;"
      "  min-width: 26px;"
      "  max-width: 26px;"
      "  min-height: 26px;"
      "  max-height: 26px;"
      "}"
      "QToolButton#permission-close-btn:hover {"
      "  background-color: #273444;"
      "  color: #ffffff;"
      "}"
      "QPushButton {"
      "  font-size: 12px;"
      "  font-weight: 600;"
      "  min-height: 32px;"
      "  border-radius: 7px;"
      "  padding: 6px 14px;"
      "  outline: none;"
      "}"
      "QPushButton#permission-allow-visit-btn {"
      "  background-color: #25384a;"
      "  border: 1px solid #486681;"
      "  color: #edf5fc;"
      "}"
      "QPushButton#permission-allow-visit-btn:hover {"
      "  background-color: #2e465d;"
      "  border-color: #5d81a3;"
      "}"
      "QPushButton#permission-allow-visit-btn:focus {"
      "  border: 2px solid #58a6c7;"
      "}"
      "QPushButton#permission-always-allow-btn {"
      "  background-color: #1a232d;"
      "  border: 1px solid #334354;"
      "  color: #d1dce7;"
      "}"
      "QPushButton#permission-always-allow-btn:hover {"
      "  background-color: #23303e;"
      "  border-color: #44596f;"
      "}"
      "QPushButton#permission-always-allow-btn:focus {"
      "  border: 2px solid #58a6c7;"
      "}"
      "QPushButton#permission-block-btn {"
      "  background-color: #221d23;"
      "  border: 1px solid #4a2d34;"
      "  color: #f1b8c1;"
      "}"
      "QPushButton#permission-block-btn:hover {"
      "  background-color: #312128;"
      "  border-color: #633640;"
      "  color: #ffd0d8;"
      "}"
      "QPushButton#permission-block-btn:focus {"
      "  border: 2px solid #c7586e;"
      "}"
  ));

  auto *shadow = new QGraphicsDropShadowEffect(this);
  shadow->setBlurRadius(20);
  shadow->setColor(QColor(0, 0, 0, 190));
  shadow->setOffset(0, 8);
  setGraphicsEffect(shadow);

  auto *mainLayout = new QVBoxLayout(this);
  mainLayout->setContentsMargins(16, 14, 16, 14);
  mainLayout->setSpacing(8);

  // Top header row: [Icon] [Origin] [Stretch] [Close Btn]
  auto *headerLayout = new QHBoxLayout;
  headerLayout->setSpacing(10);
  headerLayout->setContentsMargins(0, 0, 0, 0);

  iconLabel_ = new QLabel(this);
  iconLabel_->setFixedSize(20, 20);
  iconLabel_->setAlignment(Qt::AlignCenter);
  headerLayout->addWidget(iconLabel_);

  originLabel_ = new QLabel(this);
  originLabel_->setObjectName(QStringLiteral("permission-origin-label"));
  originLabel_->setTextInteractionFlags(Qt::NoTextInteraction);
  headerLayout->addWidget(originLabel_, 1);

  closeBtn_ = new QToolButton(this);
  closeBtn_->setObjectName(QStringLiteral("permission-close-btn"));
  closeBtn_->setText(QStringLiteral("✕"));
  closeBtn_->setToolTip(I18n::text(QStringLiteral("permission.close_tooltip")));
  closeBtn_->setAccessibleName(I18n::text(QStringLiteral("permission.close_acc")));
  headerLayout->addWidget(closeBtn_, 0, Qt::AlignRight);

  mainLayout->addLayout(headerLayout);

  // Body prompt text
  promptTextLabel_ = new QLabel(this);
  promptTextLabel_->setObjectName(QStringLiteral("permission-prompt-label"));
  promptTextLabel_->setWordWrap(true);
  mainLayout->addWidget(promptTextLabel_);

  // Action buttons (Vertical stack)
  auto *buttonsLayout = new QVBoxLayout;
  buttonsLayout->setSpacing(7);

  allowThisVisitBtn_ = new QPushButton(I18n::text(QStringLiteral("permission.allow_this_visit")), this);
  allowThisVisitBtn_->setObjectName(QStringLiteral("permission-allow-visit-btn"));
  allowThisVisitBtn_->setAccessibleName(I18n::text(QStringLiteral("permission.allow_this_visit_acc")));
  allowThisVisitBtn_->setDefault(true);
  buttonsLayout->addWidget(allowThisVisitBtn_);

  alwaysAllowBtn_ = new QPushButton(I18n::text(QStringLiteral("permission.always_allow")), this);
  alwaysAllowBtn_->setObjectName(QStringLiteral("permission-always-allow-btn"));
  alwaysAllowBtn_->setAccessibleName(I18n::text(QStringLiteral("permission.always_allow_acc")));
  buttonsLayout->addWidget(alwaysAllowBtn_);

  blockBtn_ = new QPushButton(I18n::text(QStringLiteral("permission.block")), this);
  blockBtn_->setObjectName(QStringLiteral("permission-block-btn"));
  blockBtn_->setAccessibleName(I18n::text(QStringLiteral("permission.block_acc")));
  buttonsLayout->addWidget(blockBtn_);

  mainLayout->addLayout(buttonsLayout);

  // Connections
  connect(allowThisVisitBtn_, &QPushButton::clicked, this, [this] {
    emit choiceMade(SitePermissionChoice::AllowThisVisit);
    hide();
  });

  connect(alwaysAllowBtn_, &QPushButton::clicked, this, [this] {
    emit choiceMade(SitePermissionChoice::AlwaysAllow);
    hide();
  });

  connect(blockBtn_, &QPushButton::clicked, this, [this] {
    emit choiceMade(SitePermissionChoice::Block);
    hide();
  });

  connect(closeBtn_, &QToolButton::clicked, this, [this] {
    emit choiceMade(SitePermissionChoice::Dismissed);
    hide();
  });
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
void SitePermissionPromptBubble::setupPrompt(const QUrl &origin, QWebEnginePermission::PermissionType type) {
  origin_ = origin;
  canonicalOrigin_ = canonicalOriginFromUrl(origin);

  const QString displayHost = formatOriginDisplay(origin);
  originLabel_->setText(displayHost);
  originLabel_->setToolTip(canonicalOrigin_);

  BrowserIcon iconId = BrowserIcon::Privacy;
  QString featureDesc;

  switch (type) {
    case QWebEnginePermission::PermissionType::Geolocation:
      iconId = BrowserIcon::Location;
      featureDesc = I18n::text(QStringLiteral("permission.geolocation"));
      break;
    case QWebEnginePermission::PermissionType::MediaVideoCapture:
      iconId = BrowserIcon::Camera;
      featureDesc = I18n::text(QStringLiteral("permission.camera"));
      break;
    case QWebEnginePermission::PermissionType::MediaAudioCapture:
      iconId = BrowserIcon::Microphone;
      featureDesc = I18n::text(QStringLiteral("permission.microphone"));
      break;
    case QWebEnginePermission::PermissionType::MediaAudioVideoCapture:
      iconId = BrowserIcon::Camera;
      featureDesc = I18n::text(QStringLiteral("permission.camera_microphone"));
      break;
    case QWebEnginePermission::PermissionType::DesktopVideoCapture:
      iconId = BrowserIcon::Video;
      featureDesc = I18n::text(QStringLiteral("permission.screen"));
      break;
    case QWebEnginePermission::PermissionType::DesktopAudioVideoCapture:
      iconId = BrowserIcon::Video;
      featureDesc = I18n::text(QStringLiteral("permission.screen_audio"));
      break;
    case QWebEnginePermission::PermissionType::Notifications:
      iconId = BrowserIcon::Notification;
      featureDesc = I18n::text(QStringLiteral("permission.notifications"));
      break;
    case QWebEnginePermission::PermissionType::ClipboardReadWrite:
      iconId = BrowserIcon::Clipboard;
      featureDesc = I18n::text(QStringLiteral("permission.clipboard"));
      break;
    case QWebEnginePermission::PermissionType::LocalFontsAccess:
      iconId = BrowserIcon::Fonts;
      featureDesc = I18n::text(QStringLiteral("permission.fonts"));
      break;
    case QWebEnginePermission::PermissionType::MouseLock:
      iconId = BrowserIcon::Mouse;
      featureDesc = I18n::text(QStringLiteral("permission.mouselock"));
      break;
    default:
      iconId = BrowserIcon::Privacy;
      featureDesc = I18n::text(QStringLiteral("permission.generic"));
      break;
  }

  iconLabel_->setPixmap(BrowserIcons::icon(iconId).pixmap(QSize(18, 18)));
  promptTextLabel_->setText(formatPermissionPromptText(displayHost, featureDesc));

  allowThisVisitBtn_->setFocus();
  adjustSize();
}
#endif

void SitePermissionPromptBubble::setupPromptLegacy(const QUrl &origin, const QString &featureText, BrowserIcon icon) {
  origin_ = origin;
  canonicalOrigin_ = canonicalOriginFromUrl(origin);

  const QString displayHost = formatOriginDisplay(origin);
  originLabel_->setText(displayHost);
  originLabel_->setToolTip(canonicalOrigin_);

  iconLabel_->setPixmap(BrowserIcons::icon(icon).pixmap(QSize(18, 18)));
  promptTextLabel_->setText(formatPermissionPromptText(displayHost, I18n::text(QStringLiteral("permission.wants_feature")).arg(featureText)));

  allowThisVisitBtn_->setFocus();
  adjustSize();
}

QString SitePermissionPromptBubble::formatPermissionPromptText(const QString &displayHost, const QString &featureDescription) {
  return I18n::text(QStringLiteral("permission.prompt_title")).arg(displayHost, featureDescription);
}

void SitePermissionPromptBubble::keyPressEvent(QKeyEvent *event) {
  if (event->key() == Qt::Key_Escape) {
    event->accept();
    emit choiceMade(SitePermissionChoice::Dismissed);
    hide();
    return;
  }
  QFrame::keyPressEvent(event);
}

void SitePermissionPromptBubble::paintEvent(QPaintEvent *event) {
  QStyleOption opt;
  opt.initFrom(this);
  QPainter p(this);
  style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
  QFrame::paintEvent(event);
}
