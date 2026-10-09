#include "password_manager_page.h"
#include "credential_vault_manager.h"

#include <QClipboard>
#include <QAbstractButton>
#include <QCheckBox>
#include <QComboBox>
#include <QAction>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFileDialog>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMenu>
#include <QPushButton>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStyle>
#include <QStyleOptionButton>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QFutureWatcher>
#include <QPointer>
#include <QWebEngineProfile>
#include <QtConcurrentRun>

#include <algorithm>
#include <functional>
#include <QMouseEvent>
#include "i18n/i18n.h"

namespace {
class ConsentCard final : public QFrame {
 public:
  using QFrame::QFrame;

  QSize sizeHint() const override {
    if (layout()) {
      return layout()->sizeHint();
    }
    return QFrame::sizeHint();
  }

  QSize minimumSizeHint() const override {
    if (layout()) {
      return layout()->sizeHint();
    }
    return QFrame::minimumSizeHint();
  }
};

class UnlockCard final : public QFrame {
 public:
  using QFrame::QFrame;

  QSize sizeHint() const override {
    const QSize content = layout() ? layout()->sizeHint() : QFrame::sizeHint();
    return QSize(480, content.height());
  }

  QSize minimumSizeHint() const override {
    const QSize content = layout() ? layout()->minimumSize() : QFrame::minimumSizeHint();
    return QSize(0, content.height());
  }
};

class PasswordToolbar final : public QWidget {
 public:
  using QWidget::QWidget;

  void setControls(QWidget *vault, QWidget *search, QWidget *add, QWidget *lock, QWidget *options) {
    vault_ = vault;
    search_ = search;
    add_ = add;
    lock_ = lock;
    options_ = options;
    grid_ = new QGridLayout(this);
    grid_->setContentsMargins(0, 0, 0, 0);
    grid_->setHorizontalSpacing(8);
    grid_->setVerticalSpacing(8);
    rebuild(width() < 560);
  }

 protected:
  void resizeEvent(QResizeEvent *event) override {
    QWidget::resizeEvent(event);
    rebuild(event->size().width() < 560);
  }

 private:
  void rebuild(bool compact) {
    if (!grid_ || (laidOut_ && compact_ == compact)) return;
    compact_ = compact;
    laidOut_ = true;
    for (QWidget *widget : {vault_, search_, add_, lock_, options_}) grid_->removeWidget(widget);
    for (int column = 0; column < 5; ++column) grid_->setColumnStretch(column, 0);
    if (compact) {
      grid_->addWidget(vault_, 0, 0);
      grid_->addWidget(search_, 0, 1, 1, 4);
      grid_->addWidget(add_, 1, 1);
      grid_->addWidget(lock_, 1, 2);
      grid_->addWidget(options_, 1, 3);
      grid_->setColumnStretch(0, 1);
    } else {
      grid_->addWidget(vault_, 0, 0);
      grid_->addWidget(search_, 0, 1);
      grid_->addWidget(add_, 0, 2);
      grid_->addWidget(lock_, 0, 3);
      grid_->addWidget(options_, 0, 4);
      grid_->setColumnStretch(1, 1);
    }
    updateGeometry();
  }

  QGridLayout *grid_ = nullptr;
  QWidget *vault_ = nullptr;
  QWidget *search_ = nullptr;
  QWidget *add_ = nullptr;
  QWidget *lock_ = nullptr;
  QWidget *options_ = nullptr;
  bool compact_ = false;
  bool laidOut_ = false;
};

class ConsentLabel final : public QLabel {
 public:
  explicit ConsentLabel(const QString &text, std::function<void()> onClick, QWidget *parent = nullptr)
      : QLabel(text, parent), onClick_(std::move(onClick)) {
    setObjectName(QStringLiteral("vault-consent-label"));
    setWordWrap(true);
    setTextInteractionFlags(Qt::NoTextInteraction);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    setStyleSheet(QStringLiteral("QLabel#vault-consent-label { color:#cbd5e1; font-size:13px; line-height:1.4; } QLabel#vault-consent-label:disabled { color:#65717c; }"));
  }

 protected:
  void mousePressEvent(QMouseEvent *event) override {
    if (event->button() == Qt::LeftButton && isEnabled() && onClick_) {
      onClick_();
      event->accept();
      return;
    }
    QLabel::mousePressEvent(event);
  }

 private:
  std::function<void()> onClick_;
};

class ConsentCheckBox final : public QCheckBox {
 public:
  explicit ConsentCheckBox(const QString &text, QWidget *parent = nullptr)
      : QCheckBox(text, parent) {
    setFixedSize(18, 20);
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  }

  void setCompanionLabel(ConsentLabel *label) {
    companionLabel_ = label;
    if (companionLabel_) {
      companionLabel_->setEnabled(isEnabled());
      companionLabel_->setToolTip(toolTip());
      companionLabel_->setCursor(isEnabled() ? Qt::PointingHandCursor : Qt::ArrowCursor);
    }
  }

  ConsentLabel *companionLabel() const { return companionLabel_; }

  QSize sizeHint() const override { return QSize(18, 20); }
  QSize minimumSizeHint() const override { return QSize(18, 20); }

 protected:
  void changeEvent(QEvent *event) override {
    QCheckBox::changeEvent(event);
    if (event->type() == QEvent::EnabledChange) {
      if (companionLabel_) {
        companionLabel_->setEnabled(isEnabled());
        companionLabel_->setCursor(isEnabled() ? Qt::PointingHandCursor : Qt::ArrowCursor);
      }
      update();
    } else if (event->type() == QEvent::ToolTipChange) {
      if (companionLabel_) {
        companionLabel_->setToolTip(toolTip());
      }
    }
  }

  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    // 18x18 indicator placed at (0, 1) to align with the first line of companion label text
    const QRect indicator(0, 1, 18, 18);
    const bool enabled = isEnabled();
    const bool checked = isChecked();

    QColor borderColor;
    QColor bgColor;

    if (checked) {
      borderColor = enabled ? QColor(QStringLiteral("#16b8e7")) : QColor(QStringLiteral("#0e6782"));
      bgColor = enabled ? QColor(QStringLiteral("#16b8e7")) : QColor(QStringLiteral("#0e6782"));
    } else {
      borderColor = enabled ? QColor(QStringLiteral("#64748b")) : QColor(QStringLiteral("#334155"));
      bgColor = enabled ? QColor(QStringLiteral("#080d15")) : QColor(QStringLiteral("#0a0f18"));
    }

    painter.setPen(QPen(borderColor, 1.2));
    painter.setBrush(bgColor);
    painter.drawRoundedRect(indicator.adjusted(0, 0, -1, -1), 4, 4);

    if (checked) {
      QPen tick(enabled ? QColor(QStringLiteral("#062632")) : QColor(QStringLiteral("#041820")));
      tick.setWidthF(2.1);
      tick.setCapStyle(Qt::RoundCap);
      tick.setJoinStyle(Qt::RoundJoin);
      painter.setPen(tick);
      painter.setBrush(Qt::NoBrush);

      QPainterPath path;
      path.moveTo(indicator.left() + 3.8, indicator.center().y());
      path.lineTo(indicator.left() + 6.8, indicator.bottom() - 4.2);
      path.lineTo(indicator.right() - 3.8, indicator.top() + 4.2);
      painter.drawPath(path);
    }
  }

 private:
  ConsentLabel *companionLabel_ = nullptr;
};

QIcon passwordVisibilityIcon(bool visible) {
  QPixmap pixmap(20, 20); pixmap.fill(Qt::transparent);
  QPainter painter(&pixmap); painter.setRenderHint(QPainter::Antialiasing);
  QPen pen(QColor(QStringLiteral("#c7d2df"))); pen.setWidthF(1.7); painter.setPen(pen); painter.setBrush(Qt::NoBrush);
  painter.drawEllipse(QRectF(3, 6, 14, 8)); painter.setBrush(QColor(QStringLiteral("#c7d2df"))); painter.drawEllipse(QRectF(8, 8, 4, 4));
  if (!visible) { QPen slash(QColor(QStringLiteral("#c7d2df"))); slash.setWidthF(2.0); painter.setPen(slash); painter.drawLine(QPointF(3.5, 3.5), QPointF(16.5, 16.5)); }
  return QIcon(pixmap);
}

QLineEdit *passwordField(QWidget *parent) {
  auto *field = new QLineEdit(parent); field->setEchoMode(QLineEdit::Password); field->setMaxLength(256);
  auto *toggle = field->addAction(passwordVisibilityIcon(false), QLineEdit::TrailingPosition);
  toggle->setToolTip(I18n::text(QStringLiteral("passwords.show_password")));
  QObject::connect(toggle, &QAction::triggered, field, [field, toggle] {
    const bool makeVisible = field->echoMode() != QLineEdit::Normal;
    field->setEchoMode(makeVisible ? QLineEdit::Normal : QLineEdit::Password);
    toggle->setIcon(passwordVisibilityIcon(makeVisible));
    toggle->setToolTip(makeVisible ? I18n::text(QStringLiteral("passwords.hide_password")) : I18n::text(QStringLiteral("passwords.show_password")));
    if (makeVisible) QTimer::singleShot(15000, field, [field, toggle] {
      if (field->echoMode() != QLineEdit::Normal) return;
      field->setEchoMode(QLineEdit::Password); toggle->setIcon(passwordVisibilityIcon(false)); toggle->setToolTip(I18n::text(QStringLiteral("passwords.show_password")));
    });
  });
  return field;
}
QString generatedPassword(int length, bool upper, bool lower, bool digits, bool symbols) {
  QString alphabet;
  if (upper) alphabet += QStringLiteral("ABCDEFGHJKLMNPQRSTUVWXYZ");
  if (lower) alphabet += QStringLiteral("abcdefghijkmnopqrstuvwxyz");
  if (digits) alphabet += QStringLiteral("23456789");
  if (symbols) alphabet += QStringLiteral("!@#$%^&*_-+=");
  if (alphabet.isEmpty()) return {};
  QString result; result.reserve(length);
  auto *rng = QRandomGenerator::system();
  for (int index = 0; index < length; ++index) result += alphabet.at(rng->bounded(alphabet.size()));
  return result;
}

QIcon genericSiteIcon() {
  QPixmap pixmap(32, 32); pixmap.fill(Qt::transparent);
  QPainter painter(&pixmap); painter.setRenderHint(QPainter::Antialiasing);
  painter.setPen(Qt::NoPen); painter.setBrush(QColor(QStringLiteral("#4d8fc8"))); painter.drawEllipse(QRectF(2, 2, 28, 28));
  QFont font = painter.font(); font.setBold(true); font.setPixelSize(10); painter.setFont(font);
  painter.setPen(Qt::white); painter.drawText(pixmap.rect(), Qt::AlignCenter, QStringLiteral("WW"));
  return QIcon(pixmap);
}

QIcon recordActionIcon(const QString &action) {
  QPixmap pixmap(18, 18); pixmap.fill(Qt::transparent);
  QPainter painter(&pixmap); painter.setRenderHint(QPainter::Antialiasing);
  QPen pen(QColor(QStringLiteral("#d6e1ec"))); pen.setWidthF(1.6); painter.setPen(pen); painter.setBrush(Qt::NoBrush);
  if (action == QLatin1String("edit")) {
    painter.drawLine(QPointF(4, 13.5), QPointF(13.5, 4)); painter.drawLine(QPointF(4, 13.5), QPointF(7.2, 12.7));
    painter.drawLine(QPointF(12.2, 3.8), QPointF(14.2, 5.8));
  } else if (action == QLatin1String("copy")) {
    painter.drawRoundedRect(QRectF(6, 3, 8, 10), 1.2, 1.2); painter.drawRoundedRect(QRectF(3, 6, 8, 9), 1.2, 1.2);
  } else {
    painter.drawLine(QPointF(4, 5), QPointF(14, 5)); painter.drawLine(QPointF(7, 3), QPointF(11, 3)); painter.drawRoundedRect(QRectF(5.5, 5.5, 7, 9), 1.0, 1.0);
    painter.drawLine(QPointF(8, 7.5), QPointF(8, 12.5)); painter.drawLine(QPointF(11, 7.5), QPointF(11, 12.5));
  }
  return QIcon(pixmap);
}

QIcon themedIcon(const QStringList &names, const QIcon &fallback = {}) {
  for (const QString &name : names) {
    const QIcon icon = QIcon::fromTheme(name);
    if (!icon.isNull()) return icon;
  }
  return fallback;
}
}

PasswordManagerPage::PasswordManagerPage(CredentialVaultManager *vault,
                                         QWebEngineProfile *webProfile,
                                         QWidget *parent)
    : QWidget(parent), vault_(vault), webProfile_(webProfile) {
  if (webProfile_) {
    faviconLookup_ = [profile = QPointer<QWebEngineProfile>(webProfile_)](
                         const QUrl &url, FaviconResultCallback callback) {
      if (!profile) return;
      profile->requestIconForPageURL(
          url, 32,
          [callback = std::move(callback)](const QIcon &icon, const QUrl &, const QUrl &pageUrl) {
            callback(icon, pageUrl);
          });
    };
  }
  setObjectName(QStringLiteral("password-manager-page"));
  setStyleSheet(QStringLiteral(R"(
    #password-manager-page { background:#090d15; color:#eef4ff; }
    #password-manager-page QLabel { color:#cbd5e1; }
    #password-manager-page QLineEdit, #password-manager-page QComboBox { background:#0b111b; color:#ffffff; border:1px solid #334155; border-radius:10px; min-height:38px; padding:0 12px; }
    #password-manager-page QLineEdit:focus, #password-manager-page QComboBox:focus { border:1px solid #38bdf8; }
    #password-manager-page QComboBox:hover, #password-manager-page QLineEdit:hover { border-color:#4b6078; }
    #password-manager-page QComboBox::drop-down { border:0; width:28px; }
    #password-manager-page QPushButton { border:0; border-radius:10px; background:#16b8e7; color:#00131b; font-weight:700; min-height:38px; padding:0 14px; }
    #password-manager-page QPushButton:hover { background:#41c8ef; border:1px solid #a5efff; }
    #password-manager-page QPushButton:pressed { background:#0891b2; border:2px solid #67e8f9; padding-top:2px; }
    #password-manager-page QPushButton:disabled { background:#16b8e7; color:#00131b; opacity:.45; }
    #password-manager-page QPushButton[ghost="true"] { background:#243044; color:#e5edf8; }
    #password-manager-page QPushButton[ghost="true"]:hover { background:#33435b; }
    #password-manager-page QPushButton[ghost="true"]:pressed { background:#182235; border:2px solid #7dd3fc; }
    #password-manager-page QPushButton[danger="true"] { background:#7f1d1d; color:#ffffff; }
    #password-manager-page QPushButton[danger="true"]:hover { background:#9f2929; }
    #password-manager-page QPushButton[danger="true"]:pressed { background:#601313; border:2px solid #f87171; }
    #password-manager-page QCheckBox { color:#cbd5e1; spacing:8px; }
    #password-manager-page QCheckBox:disabled { color:#65717c; }
    #password-manager-page QCheckBox::indicator { width:17px; height:17px; border:1px solid #64748b; border-radius:4px; background:#080d15; }
    #password-manager-page QCheckBox::indicator:checked { background:#16b8e7; border-color:#16b8e7; }
    #password-manager-page QListWidget { background:#0d141f; border:1px solid #263448; border-radius:12px; outline:0; padding:4px; color:#94a3b8; }
    #password-manager-page QListWidget::item { border-radius:9px; border-bottom:1px solid #1c2939; }
    #password-manager-page QListWidget::item:hover { background:#131e2c; }
    #password-manager-page QToolButton { color:#7dd3fc; background:transparent; border:0; font-weight:600; text-align:left; padding:3px 0; }
    #password-manager-page QToolButton[toolbarButton="true"] { color:#e8f2fb; background:#202c3e; border:1px solid #304158; border-radius:10px; min-height:38px; padding:0 11px; }
    #password-manager-page QToolButton[toolbarButton="true"]:hover { background:#2a3a50; border-color:#52708f; }
    #password-manager-page QToolButton[toolbarButton="true"]:pressed { background:#172235; border-color:#38bdf8; }
    #password-manager-page QToolButton[vaultSwitcher="true"] { color:#ffffff; background:#0b111b; border:1px solid #334155; border-radius:10px; min-height:38px; padding:0 12px; text-align:left; }
    #password-manager-page QToolButton[vaultSwitcher="true"]:hover { background:#101a28; border-color:#4b6078; }
    #password-manager-page QToolButton[vaultSwitcher="true"]:pressed { background:#09101a; border-color:#38bdf8; }
    #password-manager-page QToolButton[vaultSwitcher="true"]::menu-indicator { subcontrol-origin:padding; subcontrol-position:right center; right:10px; }
    #password-manager-page QToolButton[iconButton="true"] { background:transparent; border:1px solid transparent; border-radius:9px; min-width:34px; max-width:34px; min-height:34px; max-height:34px; padding:0; }
    #password-manager-page QToolButton[iconButton="true"]:hover { background:#223047; border-color:#3b506b; }
    #password-manager-page QToolButton[iconButton="true"]:pressed { background:#152033; border-color:#38bdf8; }
    #password-manager-page QToolButton[iconButton="true"][danger="true"] { color:#fca5a5; }
    #password-manager-page QToolButton[iconButton="true"][danger="true"]:hover { background:#4b1d24; border-color:#8f2e39; }
    #password-manager-page QFrame#vault-unlock-card { background:#101722; border:1px solid #29384c; border-radius:16px; }
    #password-manager-page QMenu { background:#111a27; color:#e5edf7; border:1px solid #334155; border-radius:10px; padding:6px; }
    #password-manager-page QMenu::item { min-height:30px; padding:4px 28px 4px 10px; border-radius:6px; }
    #password-manager-page QMenu::item:selected { background:#24344a; }
    #password-manager-page QMenu::item[danger="true"] { color:#fca5a5; }
    #password-manager-page QMenu::separator { height:1px; background:#334155; margin:6px 8px; }
  )"));
  auto *layout = new QVBoxLayout(this); layout->setContentsMargins(24, 24, 24, 24); layout->setSpacing(12);
  auto *kicker = new QLabel(I18n::text(QStringLiteral("passwords.secure_vault_kicker")), this); kicker->setStyleSheet("color:#aebdcd;font-size:12px;font-weight:600;"); layout->addWidget(kicker);
  auto *title = new QLabel(I18n::text(QStringLiteral("toolbar.passwords")), this); title->setStyleSheet("font-size:24px;font-weight:700;color:#eaf2ff;"); layout->addWidget(title);
  status_ = new QLabel(this); status_->setWordWrap(true); layout->addWidget(status_);
  connect(vault_, &CredentialVaultManager::lockStateChanged, this, &PasswordManagerPage::refresh);
  connect(vault_, &CredentialVaultManager::changed, this, &PasswordManagerPage::refresh);
  refresh();
}

void PasswordManagerPage::refresh() {
  while (auto *item = layout()->takeAt(3)) { if (item->widget()) item->widget()->deleteLater(); delete item; }
  if (webProfile_ && webProfile_->isOffTheRecord()) {
    showPrivateBrowsingNotice();
    return;
  }
  if (!vault_->exists()) {
    if (!QSettings().value(QStringLiteral("browser/passwords/experimentalConsentAccepted"), false).toBool()) showConsent(); else showSetup();
  } else if (vault_->isLocked()) showUnlock(); else showRecords();
  for (QAbstractButton *button : findChildren<QAbstractButton *>()) {
    button->setCursor(Qt::PointingHandCursor);
  }
}

void PasswordManagerPage::showPrivateBrowsingNotice() {
  status_->setText(I18n::text(QStringLiteral("passwords.incognito_mode")));
  auto *card = new QFrame(this);
  card->setObjectName(QStringLiteral("vault-private-notice-card"));
  card->setMinimumWidth(560);
  card->setMaximumWidth(680);
  card->setStyleSheet(QStringLiteral("QFrame#vault-private-notice-card{background:#181f2a;border:1px solid #2d3748;border-radius:16px;}"));
  auto *content = new QVBoxLayout(card);
  content->setContentsMargins(28, 24, 28, 24);
  content->setSpacing(12);

  auto *badge = new QLabel(I18n::text(QStringLiteral("passwords.private_window_badge")), card);
  badge->setStyleSheet(QStringLiteral("color:#38bdf8;font-weight:700;border:1px solid #0284c7;border-radius:10px;padding:3px 9px;"));
  content->addWidget(badge, 0, Qt::AlignLeft);

  auto *heading = new QLabel(I18n::text(QStringLiteral("passwords.unavailable_in_private")), card);
  heading->setStyleSheet(QStringLiteral("font-size:20px;font-weight:700;color:#eef4fb;"));
  content->addWidget(heading);

  auto *body = new QLabel(I18n::text(QStringLiteral("passwords.private_notice")), card);
  body->setWordWrap(true);
  body->setStyleSheet(QStringLiteral("color:#94a3b8;font-size:13px;line-height:1.5;"));
  content->addWidget(body);

  static_cast<QVBoxLayout *>(layout())->addWidget(card);
  layout()->addItem(new QSpacerItem(1, 1, QSizePolicy::Minimum, QSizePolicy::Expanding));
}

void PasswordManagerPage::setFaviconLookupForTesting(FaviconLookup lookup) {
  faviconLookup_ = std::move(lookup);
  refresh();
}

void PasswordManagerPage::requestCachedFavicon(QLabel *label, const QString &origin) {
  if (!label) return;
  label->setPixmap(genericSiteIcon().pixmap(32, 32));
  label->setProperty("faviconSource", QStringLiteral("generic"));
  if (!faviconLookup_) return;

  const QUrl pageUrl(origin);
  if (CredentialVault::canonicalHttpsOrigin(pageUrl) != origin) return;
  QPointer<QLabel> guardedLabel(label);
  faviconLookup_(
      pageUrl,
      [guardedLabel, origin](const QIcon &icon, const QUrl &iconPageUrl) {
        if (!guardedLabel || icon.isNull()) return;
        if (!iconPageUrl.isEmpty() && CredentialVault::canonicalHttpsOrigin(iconPageUrl) != origin) return;
        guardedLabel->setPixmap(icon.pixmap(32, 32));
        guardedLabel->setProperty("faviconSource", QStringLiteral("browser-cache"));
      });
}
void PasswordManagerPage::showConsent() {
  status_->setText(I18n::text(QStringLiteral("passwords.secure_vault_kicker")));
  auto *card = new ConsentCard(this);
  card->setObjectName(QStringLiteral("vault-consent-card"));
  card->setMinimumWidth(660);
  card->setMaximumWidth(720);
  card->setMinimumHeight(560);
  card->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Minimum);
  card->setStyleSheet(QStringLiteral("QFrame#vault-consent-card{background:#241b0d;border:1px solid #a16207;border-radius:18px;}"));

  auto *content = new QVBoxLayout(card);
  content->setContentsMargins(28, 24, 28, 24);
  content->setSpacing(0);

  auto *badge = new QLabel(I18n::text(QStringLiteral("passwords.experimental_badge")), card);
  badge->setStyleSheet(QStringLiteral("color:#ffd54a;font-weight:700;border:1px solid #d69a00;border-radius:10px;padding:3px 9px;"));
  content->addWidget(badge, 0, Qt::AlignLeft);
  content->addSpacing(10);

  auto *heading = new QLabel(I18n::text(QStringLiteral("passwords.enable_title")), card);
  heading->setStyleSheet(QStringLiteral("font-size:21px;font-weight:700;color:#eef4fb;"));
  content->addWidget(heading);
  content->addSpacing(8);

  auto *body = new QLabel(I18n::text(QStringLiteral("passwords.enable_intro")), card);
  body->setWordWrap(true);
  content->addWidget(body);
  content->addSpacing(12);

  auto *noticeToggle = new QToolButton(card);
  noticeToggle->setObjectName(QStringLiteral("vault-notice-toggle"));
  noticeToggle->setText(I18n::text(QStringLiteral("passwords.notice_scroll_hint")));
  noticeToggle->setCheckable(true);
  noticeToggle->setChecked(true);
  noticeToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  noticeToggle->setArrowType(Qt::DownArrow);
  content->addWidget(noticeToggle, 0, Qt::AlignLeft);
  content->addSpacing(8);

  auto *noticeScroll = new QScrollArea(card);
  noticeScroll->setObjectName(QStringLiteral("vault-notice-scroll"));
  noticeScroll->setWidgetResizable(true);
  noticeScroll->setFixedHeight(200);
  noticeScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  noticeScroll->setStyleSheet(QStringLiteral("QScrollArea{background:#17202b;border:1px solid #34506b;border-radius:10px;} QScrollBar:vertical{width:12px;background:#101722;border-radius:5px;} QScrollBar::handle:vertical{min-height:28px;background:#4a6580;border-radius:5px;}"));
  auto *notice = new QLabel(I18n::text(QStringLiteral("passwords.notice_body")), noticeScroll);
  notice->setObjectName(QStringLiteral("vault-notice"));
  notice->setWordWrap(true);
  notice->setAlignment(Qt::AlignTop | Qt::AlignLeft);
  notice->setStyleSheet(QStringLiteral("border:0;padding:12px;color:#e5d5b5;"));
  noticeScroll->setWidget(notice);
  content->addWidget(noticeScroll);

  connect(noticeToggle, &QToolButton::toggled, card, [noticeToggle, noticeScroll](bool shown) {
    noticeToggle->setArrowType(shown ? Qt::DownArrow : Qt::RightArrow);
    noticeScroll->setVisible(shown);
  });

  // Rule 1: Politika scroll alanı ile checkbox bölümü arasına 14 px dikey boşluk
  content->addSpacing(14);

  // Checkbox container
  auto *checkboxSection = new QWidget(card);
  checkboxSection->setObjectName(QStringLiteral("vault-consent-checkbox-section"));
  auto *checkboxLayout = new QVBoxLayout(checkboxSection);
  checkboxLayout->setContentsMargins(0, 0, 0, 0);
  checkboxLayout->setSpacing(10); // Rule 6: İki checkbox satırı arasında 10 px dikey boşluk

  // Satır 1: Rule 2: Ayrı horizontal container
  auto *row1 = new QWidget(checkboxSection);
  row1->setObjectName(QStringLiteral("vault-consent-row-notice"));
  auto *rowLayout1 = new QHBoxLayout(row1);
  rowLayout1->setContentsMargins(0, 0, 0, 0);
  rowLayout1->setSpacing(10); // Rule 3: Checkbox ile label arasında 10 px spacing

  auto *noticeCheck = new ConsentCheckBox(I18n::text(QStringLiteral("passwords.notice_ack")), row1);
  noticeCheck->setObjectName(QStringLiteral("vault-notice-acknowledgement"));
  noticeCheck->setEnabled(false);
  noticeCheck->setToolTip(I18n::text(QStringLiteral("passwords.notice_scroll_first")));

  auto *noticeLabel = new ConsentLabel(
      I18n::text(QStringLiteral("passwords.notice_ack")),
      [noticeCheck] { if (noticeCheck->isEnabled()) noticeCheck->toggle(); },
      row1);
  noticeCheck->setCompanionLabel(noticeLabel);

  rowLayout1->addWidget(noticeCheck, 0, Qt::AlignTop); // Rule 4: İlk satıra hizalı
  rowLayout1->addWidget(noticeLabel, 1, Qt::AlignTop); // Rule 5: Word wrap aktif, genişleyen layout
  checkboxLayout->addWidget(row1);

  // Satır 2: Rule 2: Ayrı horizontal container
  auto *row2 = new QWidget(checkboxSection);
  row2->setObjectName(QStringLiteral("vault-consent-row-activation"));
  auto *rowLayout2 = new QHBoxLayout(row2);
  rowLayout2->setContentsMargins(0, 0, 0, 0);
  rowLayout2->setSpacing(10); // Rule 3: Checkbox ile label arasında 10 px spacing

  auto *activationCheck = new ConsentCheckBox(
      I18n::text(QStringLiteral("passwords.consent")),
      row2);
  activationCheck->setObjectName(QStringLiteral("vault-activation-acknowledgement"));
  activationCheck->setEnabled(false);
  activationCheck->setToolTip(I18n::text(QStringLiteral("passwords.notice_scroll_first")));

  auto *activationLabel = new ConsentLabel(
      I18n::text(QStringLiteral("passwords.consent")),
      [activationCheck] { if (activationCheck->isEnabled()) activationCheck->toggle(); },
      row2);
  activationCheck->setCompanionLabel(activationLabel);

  rowLayout2->addWidget(activationCheck, 0, Qt::AlignTop); // Rule 4: İlk satıra hizalı
  rowLayout2->addWidget(activationLabel, 1, Qt::AlignTop); // Rule 5: Word wrap aktif, genişleyen layout
  checkboxLayout->addWidget(row2);

  content->addWidget(checkboxSection);

  // Rule 7: Checkbox bölümü ile buton grubu arasında 16 px boşluk
  content->addSpacing(16);

  auto *buttons = new QHBoxLayout;
  buttons->setContentsMargins(0, 0, 0, 0);
  buttons->setSpacing(12);
  auto *enable = new QPushButton(I18n::text(QStringLiteral("passwords.enable_experimental")), card);
  enable->setObjectName(QStringLiteral("vault-enable-experimental"));
  enable->setEnabled(false);
  auto *later = new QPushButton(I18n::text(QStringLiteral("passwords.not_now")), card);
  later->setProperty("ghost", true);
  buttons->addWidget(enable);
  buttons->addWidget(later);
  buttons->addStretch();
  content->addLayout(buttons);

  const auto updateEnable = [noticeCheck, activationCheck, noticeScroll, enable] {
    enable->setEnabled(noticeCheck->isChecked() && activationCheck->isChecked() && noticeScroll->property("readToEnd").toBool());
  };
  connect(noticeCheck, &QCheckBox::toggled, card, updateEnable);
  connect(activationCheck, &QCheckBox::toggled, card, updateEnable);
  connect(noticeScroll->verticalScrollBar(), &QScrollBar::valueChanged, card, [noticeScroll, noticeCheck, activationCheck, updateEnable](int position) {
    if (position >= noticeScroll->verticalScrollBar()->maximum() && !noticeScroll->property("readToEnd").toBool()) {
      noticeScroll->setProperty("readToEnd", true);
      noticeCheck->setEnabled(true);
      activationCheck->setEnabled(true);
      noticeCheck->setToolTip({});
      activationCheck->setToolTip({});
      updateEnable();
    }
  });
  connect(enable, &QPushButton::clicked, this, [this] {
    QSettings().setValue(QStringLiteral("browser/passwords/experimentalConsentAccepted"), true);
    statusMessage_ = I18n::text(QStringLiteral("passwords.experimental_enabled"));
    refresh();
  });
  connect(later, &QPushButton::clicked, this, [this] {
    status_->setText(I18n::text(QStringLiteral("passwords.not_enabled")));
  });
  static_cast<QVBoxLayout *>(layout())->addWidget(card, 0, Qt::AlignHCenter);
  layout()->addItem(new QSpacerItem(1, 1, QSizePolicy::Minimum, QSizePolicy::Expanding));
}
void PasswordManagerPage::showSetup() {
  status_->setText(statusMessage_.isEmpty() ? I18n::text(QStringLiteral("passwords.experimental_enabled")) : statusMessage_);
  auto *card = new QFrame(this); card->setObjectName(QStringLiteral("vault-setup-card")); card->setStyleSheet(QStringLiteral("QFrame#vault-setup-card{background:#101722;border:1px solid #29364a;border-radius:18px;}")); card->setMaximumWidth(520); auto *form = new QVBoxLayout(card); form->setContentsMargins(22, 22, 22, 22); form->setSpacing(10);
  auto *heading = new QLabel(I18n::text(QStringLiteral("passwords.create_vault_title")), card); heading->setStyleSheet(QStringLiteral("font-size:21px;font-weight:700;color:#eef4fb;")); form->addWidget(heading); form->addWidget(new QLabel(I18n::text(QStringLiteral("passwords.create_vault_intro")), card));
  form->addWidget(new QLabel(I18n::text(QStringLiteral("passwords.vault_name")), card)); auto *vaultName = new QLineEdit(card); vaultName->setObjectName(QStringLiteral("vault-name")); vaultName->setMaxLength(64); vaultName->setText(I18n::text(QStringLiteral("passwords.default_vault_name"))); form->addWidget(vaultName);
  form->addWidget(new QLabel(I18n::text(QStringLiteral("passwords.master_password_label")), card)); master_ = passwordField(card); master_->setObjectName(QStringLiteral("vault-master-password")); master_->setPlaceholderText(I18n::text(QStringLiteral("passwords.master_password_label"))); form->addWidget(master_);
  auto *rules = new QGridLayout; rules->setHorizontalSpacing(24); rules->setVerticalSpacing(7); const QStringList ruleTexts{I18n::text(QStringLiteral("passwords.rule_min_length")), I18n::text(QStringLiteral("passwords.rule_lowercase")), I18n::text(QStringLiteral("passwords.rule_uppercase")), I18n::text(QStringLiteral("passwords.rule_digit")), I18n::text(QStringLiteral("passwords.rule_symbol"))}; const QStringList ruleNames{QStringLiteral("length"), QStringLiteral("lower"), QStringLiteral("upper"), QStringLiteral("number"), QStringLiteral("symbol")}; QVector<QLabel *> ruleLabels; for (int i = 0; i < ruleTexts.size(); ++i) { auto *label = new QLabel(QStringLiteral("●  %1").arg(ruleTexts.at(i)), card); label->setObjectName(QStringLiteral("vault-rule-%1").arg(ruleNames.at(i))); label->setStyleSheet(QStringLiteral("color:#f87171;font-size:12px;")); ruleLabels.append(label); rules->addWidget(label, i / 2, i % 2); } form->addLayout(rules);
  form->addWidget(new QLabel(I18n::text(QStringLiteral("passwords.confirm_password")), card)); confirm_ = passwordField(card); confirm_->setObjectName(QStringLiteral("vault-confirm-password")); confirm_->setPlaceholderText(I18n::text(QStringLiteral("passwords.confirm_password"))); form->addWidget(confirm_); auto *match = new QLabel(I18n::text(QStringLiteral("passwords.passwords_mismatch")), card); match->setObjectName(QStringLiteral("vault-password-match")); match->setStyleSheet(QStringLiteral("color:#ff7676;")); form->addWidget(match);
  auto *create = new QPushButton(I18n::text(QStringLiteral("passwords.create_vault")), card); create->setObjectName(QStringLiteral("vault-create")); create->setEnabled(false); form->addWidget(create, 0, Qt::AlignLeft);
  const auto validate = [this, ruleLabels, match, create] { const QString value = master_->text(); const QVector<bool> checks{value.size() >= 12, value.contains(QRegularExpression(QStringLiteral("[a-z]"))), value.contains(QRegularExpression(QStringLiteral("[A-Z]"))), value.contains(QRegularExpression(QStringLiteral("[0-9]"))), value.contains(QRegularExpression(QStringLiteral("[^A-Za-z0-9\\s]")))}; for (int i = 0; i < checks.size(); ++i) ruleLabels.at(i)->setStyleSheet(checks.at(i) ? QStringLiteral("color:#55e69a;") : QStringLiteral("color:#ff7676;")); const bool same = !value.isEmpty() && value == confirm_->text(); match->setText(same ? I18n::text(QStringLiteral("passwords.passwords_match")) : I18n::text(QStringLiteral("passwords.passwords_mismatch"))); match->setStyleSheet(same ? QStringLiteral("color:#55e69a;") : QStringLiteral("color:#ff7676;")); create->setEnabled(std::all_of(checks.cbegin(), checks.cend(), [](bool ok) { return ok; }) && same); };
  connect(master_, &QLineEdit::textChanged, card, validate); connect(confirm_, &QLineEdit::textChanged, card, validate);
  connect(create, &QPushButton::clicked, this, [this, create, vaultName] { const QString p = master_->text(); const QString name = vaultName->text(); master_->clear(); confirm_->clear(); create->setEnabled(false); status_->setText(I18n::text(QStringLiteral("passwords.creating_vault"))); auto *watcher = new QFutureWatcher<bool>(this); connect(watcher, &QFutureWatcher<bool>::finished, this, [this, create, watcher] { const bool ok = watcher->result(); watcher->deleteLater(); create->setEnabled(true); if (ok) { statusMessage_ = I18n::text(QStringLiteral("passwords.vault_created")); refresh(); } else status_->setText(I18n::text(QStringLiteral("passwords.vault_create_failed")).arg(vault_->lastError())); }); watcher->setFuture(QtConcurrent::run([vault = vault_, name, p] { return vault->createVault(name, p); })); });
  static_cast<QVBoxLayout *>(layout())->addWidget(card, 0, Qt::AlignHCenter); layout()->addItem(new QSpacerItem(1, 1, QSizePolicy::Minimum, QSizePolicy::Expanding));
}
void PasswordManagerPage::showUnlock() {
  status_->clear();
  auto *card = new UnlockCard(this);
  card->setObjectName(QStringLiteral("vault-unlock-card"));
  card->setMaximumWidth(500);
  card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  auto *form = new QVBoxLayout(card);
  form->setContentsMargins(28, 26, 28, 28);
  form->setSpacing(9);

  auto *heading = new QLabel(I18n::text(QStringLiteral("passwords.vault_locked_title")), card);
  heading->setStyleSheet(QStringLiteral("font-size:20px;font-weight:700;color:#f1f6fc;"));
  auto *description = new QLabel(I18n::text(QStringLiteral("passwords.vault_locked_desc")), card);
  description->setStyleSheet(QStringLiteral("color:#8fa1b5;font-size:13px;"));
  form->addWidget(heading);
  form->addWidget(description);
  form->addSpacing(8);

  auto *vaultLabel = new QLabel(I18n::text(QStringLiteral("passwords.vault")), card);
  vaultLabel->setStyleSheet(QStringLiteral("color:#b9c6d5;font-size:12px;font-weight:600;"));
  auto *vaultPicker = new QComboBox(card);
  vaultPicker->setObjectName(QStringLiteral("vault-picker"));
  for (const VaultMetadata &item : vault_->vaults()) vaultPicker->addItem(item.name, item.id);
  vaultPicker->setCurrentIndex(vaultPicker->findData(vault_->activeVaultId()));
  vaultPicker->hide();
  auto *vaultSwitcher = new QToolButton(card);
  vaultSwitcher->setObjectName(QStringLiteral("vault-switcher"));
  vaultSwitcher->setProperty("vaultSwitcher", true);
  vaultSwitcher->setText(vaultPicker->currentText());
  vaultSwitcher->setToolButtonStyle(Qt::ToolButtonTextOnly);
  vaultSwitcher->setPopupMode(QToolButton::InstantPopup);
  vaultSwitcher->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  auto *vaultMenu = new QMenu(vaultSwitcher);
  for (int index = 0; index < vaultPicker->count(); ++index) {
    auto *vaultAction = vaultMenu->addAction(vaultPicker->itemText(index));
    vaultAction->setCheckable(true);
    vaultAction->setChecked(index == vaultPicker->currentIndex());
    connect(vaultAction, &QAction::triggered, vaultPicker, [vaultPicker, vaultSwitcher, index] {
      vaultPicker->setCurrentIndex(index);
      vaultSwitcher->setText(vaultPicker->itemText(index));
    });
  }
  vaultSwitcher->setMenu(vaultMenu);
  form->addWidget(vaultLabel);
  form->addWidget(vaultSwitcher);
  form->addSpacing(5);

  auto *passwordLabel = new QLabel(I18n::text(QStringLiteral("passwords.master_password")), card);
  passwordLabel->setStyleSheet(QStringLiteral("color:#b9c6d5;font-size:12px;font-weight:600;"));
  master_ = passwordField(card);
  master_->setObjectName(QStringLiteral("vault-master-password"));
  master_->setPlaceholderText(I18n::text(QStringLiteral("passwords.your_master_password")));
  auto *unlock = new QPushButton(I18n::text(QStringLiteral("passwords.open_vault")), card);
  unlock->setObjectName(QStringLiteral("vault-unlock"));
  form->addWidget(passwordLabel);
  form->addWidget(master_);
  form->addSpacing(9);
  form->addWidget(unlock);

  layout()->addItem(new QSpacerItem(1, 8, QSizePolicy::Minimum, QSizePolicy::Expanding));
  static_cast<QVBoxLayout *>(layout())->addWidget(card, 0, Qt::AlignHCenter);
  layout()->addItem(new QSpacerItem(1, 8, QSizePolicy::Minimum, QSizePolicy::Expanding));
  connect(vaultPicker, &QComboBox::currentIndexChanged, this, [this, vaultPicker](int) { vault_->setActiveVault(vaultPicker->currentData().toString()); });
  connect(unlock, &QPushButton::clicked, this, [this, unlock] { const QString p = master_->text(); master_->clear(); unlock->setEnabled(false); status_->setText(I18n::text(QStringLiteral("passwords.verifying_master"))); auto *watcher = new QFutureWatcher<bool>(this); connect(watcher, &QFutureWatcher<bool>::finished, this, [this, unlock, watcher] { const bool ok = watcher->result(); watcher->deleteLater(); unlock->setEnabled(true); if (!ok) status_->setText(I18n::text(QStringLiteral("passwords.vault_open_failed")).arg(vault_->lastError())); }); watcher->setFuture(QtConcurrent::run([vault = vault_, p] { return vault->unlock(p); })); }); connect(master_, &QLineEdit::returnPressed, unlock, &QPushButton::click);
}
void PasswordManagerPage::showRecords() {
  status_->setText(statusMessage_.isEmpty() ? I18n::text(QStringLiteral("passwords.vault_open_masked")) : statusMessage_);
  // Keep every manager control in one owned widget.  A vault-created signal can
  // refresh this page while creation completes; a single subtree prevents a
  // partially laid out toolbar or footer during that first paint.
  auto *manager = new QWidget(this);
  manager->setObjectName(QStringLiteral("vault-manager"));
  auto *managerLayout = new QVBoxLayout(manager);
  managerLayout->setContentsMargins(0, 0, 0, 0);
  managerLayout->setSpacing(12);
  auto *protectionStatus = new QLabel(
      I18n::text(vault_->isActiveVaultDeviceBound()
                     ? QStringLiteral("passwords.protection_os_store")
                     : QStringLiteral("passwords.protection_master_only")),
      manager);
  protectionStatus->setObjectName(QStringLiteral("vault-protection-status"));
  protectionStatus->setWordWrap(true);
  protectionStatus->setStyleSheet(QStringLiteral("color:#9fb0c3;font-size:12px;"));
  managerLayout->addWidget(protectionStatus);
  auto *toolbar = new PasswordToolbar(manager);
  toolbar->setObjectName(QStringLiteral("vault-toolbar"));
  auto *vaultPicker = new QComboBox(manager); vaultPicker->setObjectName(QStringLiteral("vault-picker"));
  for (const VaultMetadata &item : vault_->vaults()) vaultPicker->addItem((item.locked ? I18n::text(QStringLiteral("passwords.vault_label_locked")) : I18n::text(QStringLiteral("passwords.vault_label_open"))).arg(item.name), item.id);
  vaultPicker->setCurrentIndex(vaultPicker->findData(vault_->activeVaultId()));
  vaultPicker->setMinimumWidth(110);
  vaultPicker->setMaximumWidth(180);
  search_ = new QLineEdit(manager); search_->setObjectName(QStringLiteral("vault-search")); search_->setPlaceholderText(I18n::text(QStringLiteral("passwords.search_placeholder")));
  search_->setMinimumWidth(60);
  search_->addAction(themedIcon({QStringLiteral("edit-find-symbolic"), QStringLiteral("edit-find")}, style()->standardIcon(QStyle::SP_FileDialogContentsView)), QLineEdit::LeadingPosition);
  auto *add = new QPushButton(themedIcon({QStringLiteral("list-add-symbolic"), QStringLiteral("list-add")}, style()->standardIcon(QStyle::SP_FileDialogNewFolder)), I18n::text(QStringLiteral("common.add")), manager); add->setObjectName(QStringLiteral("vault-add-credential"));
  add->setToolTip(I18n::text(QStringLiteral("passwords.add_credential")));
  auto *lock = new QPushButton(themedIcon({QStringLiteral("system-lock-screen-symbolic"), QStringLiteral("changes-prevent-symbolic"), QStringLiteral("system-lock-screen")}, style()->standardIcon(QStyle::SP_DialogCloseButton)), I18n::text(QStringLiteral("passwords.lock")), manager); lock->setObjectName(QStringLiteral("vault-lock")); lock->setProperty("ghost", true); lock->setToolTip(I18n::text(QStringLiteral("passwords.lock_vault")));
  auto *options = new QToolButton(manager);
  options->setObjectName(QStringLiteral("vault-options"));
  options->setProperty("toolbarButton", true);
  options->setIcon(themedIcon({QStringLiteral("view-more-symbolic"), QStringLiteral("open-menu-symbolic"), QStringLiteral("open-menu")}, style()->standardIcon(QStyle::SP_TitleBarMenuButton)));
  options->setToolTip(I18n::text(QStringLiteral("passwords.vault_options")));
  options->setPopupMode(QToolButton::InstantPopup);

  // These controls retain the existing object names and signal wiring while
  // their visible entry points live in the compact options menu.
  auto *toggleUsernames = new QPushButton(passwordVisibilityIcon(usernamesVisible_), {}, manager); toggleUsernames->setObjectName(QStringLiteral("vault-toggle-usernames")); toggleUsernames->hide();
  auto *newVault = new QPushButton(I18n::text(QStringLiteral("passwords.new_vault")), manager); newVault->setObjectName(QStringLiteral("vault-create-new")); newVault->setProperty("ghost", true);
  auto *changeMaster = new QPushButton(I18n::text(QStringLiteral("passwords.change_master")), manager);
  auto *autoLock = new QComboBox(manager); autoLock->setObjectName(QStringLiteral("vault-auto-lock")); autoLock->setToolTip(I18n::text(QStringLiteral("passwords.auto_lock_timeout")));
  autoLock->addItem(I18n::text(QStringLiteral("time.minutes_short")).arg(1), 60000); autoLock->addItem(I18n::text(QStringLiteral("time.minutes_short")).arg(5), 5 * 60000); autoLock->addItem(I18n::text(QStringLiteral("time.minutes_short")).arg(15), 15 * 60000); autoLock->addItem(I18n::text(QStringLiteral("time.minutes_short")).arg(30), 30 * 60000); autoLock->setCurrentIndex(autoLock->findData(vault_->autoLockTimeoutMs()));
  toolbar->setControls(vaultPicker, search_, add, lock, options);
  managerLayout->addWidget(toolbar);
  records_ = new QListWidget(manager); records_->setMinimumHeight(64); records_->setMaximumHeight(520); records_->setSelectionMode(QAbstractItemView::NoSelection); records_->setFocusPolicy(Qt::NoFocus); records_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel); managerLayout->addWidget(records_);
  const auto populate = [this] {
    if (!records_) return;
    const QString q = search_->text().trimmed(); records_->clear();
    for (const CredentialMetadata &meta : vault_->list()) {
      if (!q.isEmpty() && !meta.origin.contains(q, Qt::CaseInsensitive) && !meta.username.contains(q, Qt::CaseInsensitive)) continue;
      auto *item = new QListWidgetItem(records_); item->setData(Qt::UserRole, meta.id); item->setToolTip(I18n::text(QStringLiteral("passwords.record_updated")).arg(meta.origin, meta.updatedAt.toLocalTime().toString(Qt::ISODate)));
      auto *card = new QWidget(records_); auto *cardLayout = new QHBoxLayout(card); cardLayout->setContentsMargins(12, 8, 8, 8); cardLayout->setSpacing(12);
      auto *platform = new QLabel(card); platform->setObjectName(QStringLiteral("credential-site-icon")); platform->setFixedSize(32, 32); requestCachedFavicon(platform, meta.origin); cardLayout->addWidget(platform);
      auto *details = new QWidget(card); auto *detailsLayout = new QVBoxLayout(details); detailsLayout->setContentsMargins(0, 0, 0, 0); detailsLayout->setSpacing(2);
      QString host = QUrl(meta.origin).host();
      if (host.startsWith(QLatin1String("www."), Qt::CaseInsensitive)) host.remove(0, 4);
      auto *origin = new QLabel(host.isEmpty() ? meta.origin : host, details); origin->setStyleSheet(QStringLiteral("font-weight:600;color:#eef5fb;"));
      const QString displayedUsername = usernamesVisible_ ? meta.username : QString(std::max(8, int(meta.username.size())), QChar(0x2022));
      auto *username = new QLabel(displayedUsername, details); username->setStyleSheet(QStringLiteral("color:#9fb0c3;font-size:12px;")); detailsLayout->addWidget(origin); detailsLayout->addWidget(username); cardLayout->addWidget(details, 1);
      auto *edit = new QToolButton(card); edit->setIcon(themedIcon({QStringLiteral("document-edit-symbolic"), QStringLiteral("document-edit")}, recordActionIcon(QStringLiteral("edit")))); edit->setToolTip(I18n::text(QStringLiteral("passwords.edit_credential"))); edit->setProperty("iconButton", true);
      auto *copy = new QToolButton(card); copy->setIcon(themedIcon({QStringLiteral("edit-copy-symbolic"), QStringLiteral("edit-copy")}, recordActionIcon(QStringLiteral("copy")))); copy->setToolTip(I18n::text(QStringLiteral("passwords.copy_password"))); copy->setProperty("iconButton", true);
      auto *remove = new QToolButton(card); remove->setIcon(themedIcon({QStringLiteral("edit-delete-symbolic"), QStringLiteral("user-trash-symbolic"), QStringLiteral("edit-delete")}, recordActionIcon(QStringLiteral("delete")))); remove->setToolTip(I18n::text(QStringLiteral("passwords.delete_credential"))); remove->setProperty("iconButton", true); remove->setProperty("danger", true);
      cardLayout->addWidget(edit); cardLayout->addWidget(copy); cardLayout->addWidget(remove);
      item->setSizeHint(QSize(0, 58)); records_->setItemWidget(item, card);
      connect(edit, &QToolButton::clicked, this, [this, id = meta.id] { addCredential(id); });
      connect(copy, &QToolButton::clicked, this, [this, id = meta.id] { copyPassword(id); });
      connect(remove, &QToolButton::clicked, this, [this, id = meta.id] { if (QMessageBox::question(this, I18n::text(QStringLiteral("toolbar.passwords")), I18n::text(QStringLiteral("passwords.delete_credential_confirm")), QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes && !vault_->remove(id)) QMessageBox::warning(this, I18n::text(QStringLiteral("toolbar.passwords")), vault_->lastError()); });
    }
    if (!records_->count()) { auto *empty = new QListWidgetItem(I18n::text(QStringLiteral("passwords.no_credentials")), records_); empty->setFlags(Qt::NoItemFlags); }
    records_->setFixedHeight(std::min(520, std::max(64, records_->count() * 58 + 10)));
  };
  auto *backup = new QPushButton(I18n::text(QStringLiteral("passwords.backup")), manager); backup->setObjectName(QStringLiteral("vault-export-backup")); backup->setProperty("ghost", true); auto *restore = new QPushButton(I18n::text(QStringLiteral("passwords.import_backup")), manager); restore->setObjectName(QStringLiteral("vault-import-backup")); restore->setProperty("ghost", true); auto *disable = new QPushButton(I18n::text(QStringLiteral("passwords.disable_feature")), manager); disable->setObjectName(QStringLiteral("vault-disable")); disable->setProperty("ghost", true); auto *reset = new QPushButton(I18n::text(QStringLiteral("passwords.reset_vault")), manager); reset->setObjectName(QStringLiteral("vault-reset")); reset->setProperty("danger", true); auto *deleteVault = new QPushButton(I18n::text(QStringLiteral("passwords.delete_vault")), manager); deleteVault->setObjectName(QStringLiteral("vault-delete")); deleteVault->setProperty("danger", true); changeMaster->setObjectName(QStringLiteral("vault-change-master")); changeMaster->setProperty("ghost", true);
  const QList<QWidget *> hiddenControls{toggleUsernames, newVault, changeMaster, autoLock, backup, restore, disable, reset, deleteVault};
  for (QWidget *control : hiddenControls) control->hide();

  auto *menu = new QMenu(options);
  auto *newVaultAction = menu->addAction(themedIcon({QStringLiteral("folder-new-symbolic"), QStringLiteral("folder-new")}), I18n::text(QStringLiteral("passwords.new_vault")));
  auto *toggleUsernamesAction = menu->addAction(passwordVisibilityIcon(usernamesVisible_), usernamesVisible_ ? I18n::text(QStringLiteral("passwords.mask_usernames")) : I18n::text(QStringLiteral("passwords.show_usernames")));
  menu->addSeparator();
  auto *backupAction = menu->addAction(themedIcon({QStringLiteral("document-save-symbolic"), QStringLiteral("document-save")}), I18n::text(QStringLiteral("passwords.backup")));
  auto *restoreAction = menu->addAction(themedIcon({QStringLiteral("document-open-symbolic"), QStringLiteral("document-open")}), I18n::text(QStringLiteral("passwords.import_backup")));
  auto *changeMasterAction = menu->addAction(themedIcon({QStringLiteral("dialog-password-symbolic"), QStringLiteral("dialog-password")}), I18n::text(QStringLiteral("passwords.change_master")));
  auto *autoLockMenu = menu->addMenu(themedIcon({QStringLiteral("preferences-system-time-symbolic"), QStringLiteral("preferences-system-time")}), I18n::text(QStringLiteral("passwords.auto_lock")));
  for (int index = 0; index < autoLock->count(); ++index) {
    auto *duration = autoLockMenu->addAction(autoLock->itemText(index));
    duration->setCheckable(true);
    duration->setChecked(index == autoLock->currentIndex());
    connect(duration, &QAction::triggered, autoLock, [autoLock, autoLockMenu, index] {
      for (QAction *entry : autoLockMenu->actions()) entry->setChecked(false);
      autoLock->setCurrentIndex(index);
      if (QAction *entry = autoLockMenu->actions().value(index)) entry->setChecked(true);
    });
  }
  auto *disableAction = menu->addAction(I18n::text(QStringLiteral("passwords.disable_feature")));
  menu->addSeparator();
  auto *resetAction = menu->addAction(themedIcon({QStringLiteral("edit-clear-all-symbolic"), QStringLiteral("edit-clear-all")}), I18n::text(QStringLiteral("passwords.reset_vault")));
  auto *deleteAction = menu->addAction(themedIcon({QStringLiteral("edit-delete-symbolic"), QStringLiteral("user-trash-symbolic"), QStringLiteral("edit-delete")}), I18n::text(QStringLiteral("passwords.delete_vault")));
  resetAction->setProperty("danger", true);
  deleteAction->setProperty("danger", true);
  options->setMenu(menu);

  connect(newVaultAction, &QAction::triggered, newVault, &QPushButton::click);
  connect(toggleUsernamesAction, &QAction::triggered, toggleUsernames, &QPushButton::click);
  connect(backupAction, &QAction::triggered, backup, &QPushButton::click);
  connect(restoreAction, &QAction::triggered, restore, &QPushButton::click);
  connect(changeMasterAction, &QAction::triggered, changeMaster, &QPushButton::click);
  connect(disableAction, &QAction::triggered, disable, &QPushButton::click);
  connect(resetAction, &QAction::triggered, reset, &QPushButton::click);
  connect(deleteAction, &QAction::triggered, deleteVault, &QPushButton::click);

  static_cast<QVBoxLayout *>(layout())->addWidget(manager);
  layout()->addItem(new QSpacerItem(1, 1, QSizePolicy::Minimum, QSizePolicy::Expanding));
  populate(); connect(vaultPicker, &QComboBox::currentIndexChanged, this, [this, vaultPicker](int) { vault_->setActiveVault(vaultPicker->currentData().toString()); }); connect(search_, &QLineEdit::textChanged, this, populate); connect(toggleUsernames, &QPushButton::clicked, this, [this, toggleUsernames, toggleUsernamesAction, populate] { usernamesVisible_ = !usernamesVisible_; const QIcon icon = passwordVisibilityIcon(usernamesVisible_); toggleUsernames->setIcon(icon); toggleUsernames->setToolTip(usernamesVisible_ ? I18n::text(QStringLiteral("passwords.mask_usernames")) : I18n::text(QStringLiteral("passwords.show_usernames"))); toggleUsernamesAction->setIcon(icon); toggleUsernamesAction->setText(usernamesVisible_ ? I18n::text(QStringLiteral("passwords.mask_usernames")) : I18n::text(QStringLiteral("passwords.show_usernames"))); populate(); }); connect(add, &QPushButton::clicked, this, [this] { addCredential(); }); connect(newVault, &QPushButton::clicked, this, &PasswordManagerPage::createVault); connect(lock, &QPushButton::clicked, vault_, &CredentialVaultManager::lock);
  connect(autoLock, &QComboBox::currentIndexChanged, this, [this, autoLock](int) { vault_->setAutoLockTimeoutMs(autoLock->currentData().toInt()); });
  connect(changeMaster, &QPushButton::clicked, this, [this, changeMaster] { bool ok = false; const QString current = QInputDialog::getText(this, I18n::text(QStringLiteral("passwords.change_master")), I18n::text(QStringLiteral("passwords.current_master")), QLineEdit::Password, {}, &ok); if (!ok || current.isEmpty()) return; const QString next = QInputDialog::getText(this, I18n::text(QStringLiteral("passwords.change_master")), I18n::text(QStringLiteral("passwords.new_master")), QLineEdit::Password, {}, &ok); if (!ok || next.isEmpty()) return; changeMaster->setEnabled(false); status_->setText(I18n::text(QStringLiteral("passwords.changing_master"))); auto *watcher = new QFutureWatcher<bool>(this); connect(watcher, &QFutureWatcher<bool>::finished, this, [this, changeMaster, watcher] { const bool changed = watcher->result(); watcher->deleteLater(); changeMaster->setEnabled(true); status_->setText(changed ? I18n::text(QStringLiteral("passwords.master_changed")) : I18n::text(QStringLiteral("passwords.master_change_failed")).arg(vault_->lastError())); }); watcher->setFuture(QtConcurrent::run([vault = vault_, current, next] { return vault->changeMasterPassword(current, next); })); });
  connect(disable, &QPushButton::clicked, this, [this] { if (QMessageBox::question(this, I18n::text(QStringLiteral("toolbar.passwords")), I18n::text(QStringLiteral("passwords.disable_feature_confirm")), QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes) { vault_->lock(); QSettings().setValue(QStringLiteral("browser/passwords/experimentalConsentAccepted"), false); refresh(); } });
  connect(backup, &QPushButton::clicked, this, &PasswordManagerPage::exportBackup); connect(restore, &QPushButton::clicked, this, &PasswordManagerPage::importBackup);
  connect(reset, &QPushButton::clicked, this, [this] { if (QMessageBox::warning(this, I18n::text(QStringLiteral("passwords.reset_vault")), I18n::text(QStringLiteral("passwords.reset_vault_confirm")), QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes && !vault_->clearActiveVault()) QMessageBox::warning(this, I18n::text(QStringLiteral("toolbar.passwords")), vault_->lastError()); });
  connect(deleteVault, &QPushButton::clicked, this, [this] { const bool lastVault = vault_->vaults().size() == 1; const QString warning = lastVault ? I18n::text(QStringLiteral("passwords.delete_last_vault_confirm")) : I18n::text(QStringLiteral("passwords.delete_vault_confirm")); if (QMessageBox::warning(this, I18n::text(QStringLiteral("passwords.delete_vault")), warning, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes) { if (!vault_->deleteVault(vault_->activeVaultId())) QMessageBox::warning(this, I18n::text(QStringLiteral("toolbar.passwords")), vault_->lastError()); else if (lastVault) { QSettings().setValue(QStringLiteral("browser/passwords/experimentalConsentAccepted"), false); statusMessage_ = I18n::text(QStringLiteral("passwords.vault_deleted_reaccept")); refresh(); } } });
}
void PasswordManagerPage::createVault() {
  bool ok = false;
  const QString name = QInputDialog::getText(this, I18n::text(QStringLiteral("passwords.new_vault")), I18n::text(QStringLiteral("passwords.vault_name")), QLineEdit::Normal, {}, &ok).trimmed();
  if (!ok || name.isEmpty()) return;
  const QString master = QInputDialog::getText(this, I18n::text(QStringLiteral("passwords.new_vault")), I18n::text(QStringLiteral("passwords.master_password")), QLineEdit::Password, {}, &ok);
  if (!ok || master.isEmpty()) return;
  const QString confirmation = QInputDialog::getText(this, I18n::text(QStringLiteral("passwords.new_vault")), I18n::text(QStringLiteral("passwords.confirm_master")), QLineEdit::Password, {}, &ok);
  if (!ok || master != confirmation || !CredentialVaultManager::isStrongMasterPassword(master)) { QMessageBox::warning(this, I18n::text(QStringLiteral("passwords.new_vault")), I18n::text(QStringLiteral("passwords.master_rules_failed"))); return; }
  if (!vault_->createVault(name, master)) QMessageBox::warning(this, I18n::text(QStringLiteral("passwords.new_vault")), vault_->lastError());
}
void PasswordManagerPage::exportBackup() {
  for (const VaultMetadata &metadata : vault_->vaults()) {
    if (!ensureSensitiveAuthorization(metadata.id)) return;
  }
  QString path = QFileDialog::getSaveFileName(this, I18n::text(QStringLiteral("passwords.backup_vaults_title")), QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + QStringLiteral("/dalinira-kasalar.dalinira-vault-backup"), I18n::text(QStringLiteral("passwords.backup_filter")));
  if (path.isEmpty()) return;
  if (!path.endsWith(QStringLiteral(".dalinira-vault-backup"), Qt::CaseInsensitive)) path += QStringLiteral(".dalinira-vault-backup");
  bool ok = false; const QString password = QInputDialog::getText(this, I18n::text(QStringLiteral("passwords.backup_password")), I18n::text(QStringLiteral("passwords.backup_password_prompt")), QLineEdit::Password, {}, &ok); if (!ok || password.isEmpty()) return;
  const QString confirmation = QInputDialog::getText(this, I18n::text(QStringLiteral("passwords.backup_password")), I18n::text(QStringLiteral("passwords.confirm_backup_password")), QLineEdit::Password, {}, &ok); if (!ok || password != confirmation || !CredentialVaultManager::isStrongMasterPassword(password)) { QMessageBox::warning(this, I18n::text(QStringLiteral("passwords.backup")), I18n::text(QStringLiteral("passwords.backup_password_rules_failed"))); return; }
  if (!vault_->exportBackup(path, password)) QMessageBox::warning(this, I18n::text(QStringLiteral("passwords.backup")), vault_->lastError()); else status_->setText(I18n::text(QStringLiteral("passwords.backup_created")).arg(path));
}
void PasswordManagerPage::importBackup() {
  const QString path = QFileDialog::getOpenFileName(this, I18n::text(QStringLiteral("passwords.import_backup_title")), QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation), I18n::text(QStringLiteral("passwords.backup_filter"))); if (path.isEmpty()) return;
  bool ok = false; const QString password = QInputDialog::getText(this, I18n::text(QStringLiteral("passwords.import_backup")), I18n::text(QStringLiteral("passwords.backup_password")), QLineEdit::Password, {}, &ok); if (!ok || password.isEmpty()) return;
  QStringList imported; if (!vault_->importBackup(path, password, &imported)) QMessageBox::warning(this, I18n::text(QStringLiteral("passwords.import_backup")), vault_->lastError()); else statusMessage_ = I18n::text(QStringLiteral("passwords.imported_vaults")).arg(imported.join(QStringLiteral(", ")));
}
void PasswordManagerPage::addCredential(const QString &id) {
  if (vault_->isLocked()) return;
  if (!id.isEmpty() && !ensureSensitiveAuthorization(vault_->owningVaultId(id))) return;
  QDialog dialog(this); dialog.setWindowTitle(id.isEmpty() ? I18n::text(QStringLiteral("passwords.add_credential")) : I18n::text(QStringLiteral("passwords.edit_credential"))); auto *form = new QFormLayout(&dialog); auto *origin = new QLineEdit(&dialog); auto *username = new QLineEdit(&dialog); auto *password = passwordField(&dialog); if (!id.isEmpty()) { CredentialSecret value; if (!vault_->reveal(id, &value)) return; origin->setText(value.origin); origin->setReadOnly(true); username->setText(value.username); password->setText(value.password); QTimer::singleShot(15000, password, &QLineEdit::clear); }
  auto *generator = new QWidget(&dialog); auto *generatorLayout = new QHBoxLayout(generator); generatorLayout->setContentsMargins(0, 0, 0, 0); auto *length = new QSpinBox(generator); length->setRange(8, 128); length->setValue(20); auto *upper = new QCheckBox(QStringLiteral("A-Z"), generator); auto *lower = new QCheckBox(QStringLiteral("a-z"), generator); auto *digits = new QCheckBox(QStringLiteral("0-9"), generator); auto *symbols = new QCheckBox(I18n::text(QStringLiteral("passwords.symbols")), generator); upper->setChecked(true); lower->setChecked(true); digits->setChecked(true); symbols->setChecked(true); auto *generate = new QPushButton(I18n::text(QStringLiteral("passwords.generate")), generator); generatorLayout->addWidget(length); generatorLayout->addWidget(upper); generatorLayout->addWidget(lower); generatorLayout->addWidget(digits); generatorLayout->addWidget(symbols); generatorLayout->addWidget(generate);
  form->addRow(I18n::text(QStringLiteral("passwords.https_origin")), origin); form->addRow(I18n::text(QStringLiteral("passwords.username")), username); form->addRow(I18n::text(QStringLiteral("passwords.password")), password); form->addRow(I18n::text(QStringLiteral("passwords.secure_generator")), generator); auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog); form->addRow(buttons); connect(generate, &QPushButton::clicked, &dialog, [password, length, upper, lower, digits, symbols] { password->setText(generatedPassword(length->value(), upper->isChecked(), lower->isChecked(), digits->isChecked(), symbols->isChecked())); }); connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject); if (dialog.exec() != QDialog::Accepted) return; bool update = false; const CredentialSecret candidate{origin->text(), username->text(), password->text(), {}}; const bool saved = id.isEmpty() ? vault_->save(candidate, &update) : vault_->update(id, candidate); password->clear(); if (!saved) QMessageBox::warning(this, I18n::text(QStringLiteral("toolbar.passwords")), I18n::text(QStringLiteral("passwords.save_failed")).arg(vault_->lastError()));
}
void PasswordManagerPage::copyPassword(const QString &id) {
  if (!ensureSensitiveAuthorization(vault_->owningVaultId(id))) return;
  CredentialSecret secret;
  if (!vault_->reveal(id, &secret)) return;
  QClipboard *clipboard = QGuiApplication::clipboard();
  clipboard->setText(secret.password);
  const QString copiedValue = secret.password;
  secret.wipe();
  QTimer::singleShot(30000, qApp, [copiedValue] {
    QClipboard *cb = QGuiApplication::clipboard();
    if (cb && cb->text() == copiedValue) {
      cb->clear();
    }
  });
}

bool PasswordManagerPage::ensureSensitiveAuthorization(const QString &vaultId) {
  if (vaultId.isEmpty()) return false;
  if (vault_->hasSensitiveAuthorization(vaultId)) return true;
  QString vaultName;
  for (const VaultMetadata &metadata : vault_->vaults()) {
    if (metadata.id == vaultId) { vaultName = metadata.name; break; }
  }
  bool ok = false;
  QString master = QInputDialog::getText(
      this, I18n::text(QStringLiteral("passwords.reauthenticate_title")),
      I18n::text(QStringLiteral("passwords.reauthenticate_prompt")).arg(vaultName),
      QLineEdit::Password, {}, &ok);
  if (!ok || master.isEmpty()) { master.fill(QChar()); return false; }
  const bool authorized = vault_->authorizeSensitiveAction(vaultId, master);
  master.fill(QChar());
  if (!authorized) {
    QMessageBox::warning(this, I18n::text(QStringLiteral("passwords.reauthenticate_title")),
                         I18n::text(QStringLiteral("passwords.reauthenticate_failed")));
  }
  return authorized;
}
