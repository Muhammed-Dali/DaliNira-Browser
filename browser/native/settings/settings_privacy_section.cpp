#include "settings_page.h"
#include "settings_ui_helpers.h"
#include "browser_profile_service.h"
#include "dalinira_blocker_service.h"
#include "dalinira_blocker_settings.h"
#include "glow_toggle_switch.h"
#include "i18n/i18n.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyle>
#include <QTreeWidget>
#include <QVBoxLayout>

using namespace dalinira::settings_ui;

class DynamicStackedWidget final : public QStackedWidget {
 public:
  explicit DynamicStackedWidget(QWidget *parent = nullptr) : QStackedWidget(parent) {
    connect(this, &QStackedWidget::currentChanged, this, [this](int) {
      updateGeometry();
      adjustSize();
    });
  }
  QSize sizeHint() const override {
    if (currentWidget()) return currentWidget()->sizeHint();
    return QStackedWidget::sizeHint();
  }
  QSize minimumSizeHint() const override {
    if (currentWidget()) return currentWidget()->minimumSizeHint();
    return QStackedWidget::minimumSizeHint();
  }
};

class PrivacyDetailSubpage final : public QWidget {
 public:
  PrivacyDetailSubpage(BrowserProfileService *profileService, std::function<void()> onBack, QWidget *parent = nullptr)
      : QWidget(parent), profileService_(profileService), onBack_(std::move(onBack)) {
    setObjectName(QStringLiteral("settings-privacy-subpage"));
    auto *outer = new QHBoxLayout(this);
    outer->setContentsMargins(22, 20, 22, 32);
    outer->addStretch(1);

    auto *column = new QWidget(this);
    column->setMaximumWidth(kContentMaxWidth);
    column->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto *layout = new QVBoxLayout(column);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(16);

    // 1. Header: Back button + Title + Help button + Search Filter
    auto *header = new QWidget(column);
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(0, 0, 0, 4);
    headerLayout->setSpacing(12);

    auto *backBtn = new QPushButton(header);
    backBtn->setObjectName(QStringLiteral("settings-subpage-back-btn"));
    backBtn->setFixedSize(36, 36);
    backBtn->setCursor(Qt::PointingHandCursor);
    backBtn->setIcon(BrowserIcons::icon(BrowserIcon::ArrowLeft));
    backBtn->setIconSize(QSize(18, 18));
    backBtn->setToolTip(I18n::text(QStringLiteral("common.back")));
    backBtn->setAccessibleName(I18n::text(QStringLiteral("common.back")));
    connect(backBtn, &QPushButton::clicked, this, [this] {
      if (onBack_) onBack_();
    });
    headerLayout->addWidget(backBtn, 0, Qt::AlignVCenter);

    titleLabel_ = new QLabel(header);
    titleLabel_->setObjectName(QStringLiteral("settings-subpage-title"));
    titleLabel_->setStyleSheet(QStringLiteral("font-size: 22px; font-weight: 700; color: #f3f7fc;"));
    headerLayout->addWidget(titleLabel_, 0, Qt::AlignVCenter);

    helpBtn_ = new QPushButton(header);
    helpBtn_->setObjectName(QStringLiteral("settings-subpage-help-btn"));
    helpBtn_->setFixedSize(32, 32);
    helpBtn_->setCursor(Qt::PointingHandCursor);
    helpBtn_->setIcon(BrowserIcons::icon(BrowserIcon::Help));
    helpBtn_->setIconSize(QSize(18, 18));
    helpBtn_->setToolTip(I18n::text(QStringLiteral("settings.site.third_party_cookies_about")));
    helpBtn_->setVisible(false);
    connect(helpBtn_, &QPushButton::clicked, this, [this] {
      QMessageBox::information(this, I18n::text(QStringLiteral("settings.site.third_party_cookies_title")),
                               I18n::text(QStringLiteral("settings.site.third_party_cookies_help")));
    });
    headerLayout->addWidget(helpBtn_, 0, Qt::AlignVCenter);

    headerLayout->addStretch(1);

    filterEdit_ = new QLineEdit(header);
    filterEdit_->setObjectName(QStringLiteral("settings-subpage-filter"));
    filterEdit_->setPlaceholderText(I18n::text(QStringLiteral("settings.site.filter_sites_placeholder")));
    filterEdit_->setFixedWidth(240);
    filterEdit_->setClearButtonEnabled(true);
    filterEdit_->addAction(BrowserIcons::icon(BrowserIcon::Search), QLineEdit::LeadingPosition);
    connect(filterEdit_, &QLineEdit::textChanged, this, &PrivacyDetailSubpage::filterSites);
    headerLayout->addWidget(filterEdit_, 0, Qt::AlignVCenter);

    layout->addWidget(header);

    // 2. Microphone device selector (only shown when configured for microphone)
    deviceContainer_ = new QWidget(column);
    auto *devLayout = new QHBoxLayout(deviceContainer_);
    devLayout->setContentsMargins(0, 0, 0, 2);
    devLayout->setSpacing(12);
    auto *devLabel = new QLabel(I18n::text(QStringLiteral("settings.site.microphone_device_label")), deviceContainer_);
    devLabel->setStyleSheet(QStringLiteral("color: #edf5fc; font-size: 13px; font-weight: 600;"));
    devLayout->addWidget(devLabel);
    deviceCombo_ = new QComboBox(deviceContainer_);
    deviceCombo_->setMinimumWidth(320);
    deviceCombo_->addItem(I18n::text(QStringLiteral("settings.site.microphone_device_system_default")), QStringLiteral("default"));
    deviceCombo_->addItem(I18n::text(QStringLiteral("settings.site.microphone_device_analog")), QStringLiteral("analog"));
    deviceCombo_->addItem(I18n::text(QStringLiteral("settings.site.microphone_device_external")), QStringLiteral("external"));
    devLayout->addWidget(deviceCombo_);
    devLayout->addStretch(1);
    deviceContainer_->setVisible(false);
    layout->addWidget(deviceContainer_);

    connect(deviceCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
      if (idx >= 0 && profileService_ && hasDeviceSelector_) {
        profileService_->setPreferredAudioInputDevice(deviceCombo_->itemText(idx));
      }
    });

    // 3. Intro description
    introLabel_ = new QLabel(column);
    introLabel_->setObjectName(QStringLiteral("settings-subpage-intro"));
    introLabel_->setStyleSheet(QStringLiteral("color: #98a8b9; font-size: 13px; line-height: 1.4; margin-bottom: 4px;"));
    introLabel_->setWordWrap(true);
    layout->addWidget(introLabel_);

    // 4. Varsayılan davranış Card
    defaultCard_ = makeCard(column, I18n::text(QStringLiteral("settings.site.default_behavior_card")));

    auto *defaultIntro = new QLabel(I18n::text(QStringLiteral("settings.site.default_behavior_intro")), defaultCard_);
    defaultIntro->setStyleSheet(QStringLiteral("color: #7f91a3; font-size: 12px; padding: 0 18px 8px;"));
    cardLayout(defaultCard_)->addWidget(defaultIntro);

    defaultBtnGroup_ = new QButtonGroup(defaultCard_);

    // Option 1: Allow / Ask
    allowFrame_ = new ClickableFrame(defaultCard_);
    allowFrame_->setObjectName(QStringLiteral("settings-row"));
    allowFrame_->setCursor(Qt::PointingHandCursor);
    auto *allowLayout = new QVBoxLayout(allowFrame_);
    allowLayout->setContentsMargins(18, 12, 18, 12);
    allowLayout->setSpacing(8);

    auto *allowTopRow = new QWidget(allowFrame_);
    auto *allowTopLayout = new QHBoxLayout(allowTopRow);
    allowTopLayout->setContentsMargins(0, 0, 0, 0);
    allowTopLayout->setSpacing(12);

    radioAllow_ = new QRadioButton(allowTopRow);
    radioAllow_->setCursor(Qt::PointingHandCursor);
    defaultBtnGroup_->addButton(radioAllow_, 0);
    allowTopLayout->addWidget(radioAllow_, 0, Qt::AlignVCenter);

    allowIconLabel_ = new QLabel(allowTopRow);
    allowIconLabel_->setFixedSize(20, 20);
    allowTopLayout->addWidget(allowIconLabel_, 0, Qt::AlignVCenter);

    allowTitleLabel_ = new QLabel(allowTopRow);
    allowTitleLabel_->setObjectName(QStringLiteral("settings-row-title"));
    allowTopLayout->addWidget(allowTitleLabel_, 1, Qt::AlignVCenter);

    allowLayout->addWidget(allowTopRow);

    // Sub-radios for display mode (only for location and notifications)
    displayModeContainer_ = new QWidget(allowFrame_);
    auto *dmLayout = new QVBoxLayout(displayModeContainer_);
    dmLayout->setContentsMargins(36, 4, 0, 4);
    dmLayout->setSpacing(8);

    auto *dmHeading = new QLabel(I18n::text(QStringLiteral("settings.site.request_display_heading")), displayModeContainer_);
    dmHeading->setStyleSheet(QStringLiteral("color: #d1dde8; font-size: 13px; font-weight: 600; margin-top: 2px;"));
    dmLayout->addWidget(dmHeading);

    dmGroup_ = new QButtonGroup(displayModeContainer_);

    radioCollapseAll_ = new QRadioButton(I18n::text(QStringLiteral("settings.site.request_collapse_all")), displayModeContainer_);
    radioCollapseAll_->setCursor(Qt::PointingHandCursor);
    dmGroup_->addButton(radioCollapseAll_, 0);
    dmLayout->addWidget(radioCollapseAll_);

    radioQuiet_ = new QRadioButton(I18n::text(QStringLiteral("settings.site.request_collapse_unwanted")), displayModeContainer_);
    radioQuiet_->setCursor(Qt::PointingHandCursor);
    dmGroup_->addButton(radioQuiet_, 1);
    dmLayout->addWidget(radioQuiet_);

    radioExpandAll_ = new QRadioButton(I18n::text(QStringLiteral("settings.site.request_expand_all")), displayModeContainer_);
    radioExpandAll_->setCursor(Qt::PointingHandCursor);
    dmGroup_->addButton(radioExpandAll_, 2);
    dmLayout->addWidget(radioExpandAll_);

    allowLayout->addWidget(displayModeContainer_);

    allowFrame_->clicked = [this] {
      radioAllow_->setChecked(true);
      onDefaultPolicyChanged(0);
    };
    addRow(defaultCard_, allowFrame_);

    // Option 2 (Middle): For siteData ("Tüm pencereleri kapattığınızda verileri sil")
    middleFrame_ = new ClickableFrame(defaultCard_);
    middleFrame_->setObjectName(QStringLiteral("settings-row"));
    middleFrame_->setCursor(Qt::PointingHandCursor);
    auto *middleLayout = new QHBoxLayout(middleFrame_);
    middleLayout->setContentsMargins(18, 12, 18, 12);
    middleLayout->setSpacing(12);

    radioMiddle_ = new QRadioButton(middleFrame_);
    radioMiddle_->setCursor(Qt::PointingHandCursor);
    defaultBtnGroup_->addButton(radioMiddle_, 2);
    middleLayout->addWidget(radioMiddle_, 0, Qt::AlignVCenter);

    middleIconLabel_ = new QLabel(middleFrame_);
    middleIconLabel_->setFixedSize(20, 20);
    middleLayout->addWidget(middleIconLabel_, 0, Qt::AlignVCenter);

    auto *middleTextCol = new QWidget(middleFrame_);
    auto *middleTextLayout = new QVBoxLayout(middleTextCol);
    middleTextLayout->setContentsMargins(0, 0, 0, 0);
    middleTextLayout->setSpacing(3);

    middleTitleLabel_ = new QLabel(middleTextCol);
    middleTitleLabel_->setObjectName(QStringLiteral("settings-row-title"));
    middleTextLayout->addWidget(middleTitleLabel_);

    middleSubLabel_ = new QLabel(middleTextCol);
    middleSubLabel_->setObjectName(QStringLiteral("settings-row-description"));
    middleTextLayout->addWidget(middleSubLabel_);

    middleLayout->addWidget(middleTextCol, 1, Qt::AlignVCenter);

    middleFrame_->clicked = [this] {
      radioMiddle_->setChecked(true);
      onDefaultPolicyChanged(2);
    };
    addRow(defaultCard_, middleFrame_);
    middleFrame_->setVisible(false);

    // Option 3: Deny / Block / Alternative
    denyFrame_ = new ClickableFrame(defaultCard_);
    denyFrame_->setObjectName(QStringLiteral("settings-row"));
    denyFrame_->setCursor(Qt::PointingHandCursor);
    auto *denyLayout = new QHBoxLayout(denyFrame_);
    denyLayout->setContentsMargins(18, 12, 18, 12);
    denyLayout->setSpacing(12);

    radioDeny_ = new QRadioButton(denyFrame_);
    radioDeny_->setCursor(Qt::PointingHandCursor);
    defaultBtnGroup_->addButton(radioDeny_, 1);
    denyLayout->addWidget(radioDeny_, 0, Qt::AlignVCenter);

    denyIconLabel_ = new QLabel(denyFrame_);
    denyIconLabel_->setFixedSize(20, 20);
    denyLayout->addWidget(denyIconLabel_, 0, Qt::AlignVCenter);

    auto *denyTextCol = new QWidget(denyFrame_);
    auto *denyTextLayout = new QVBoxLayout(denyTextCol);
    denyTextLayout->setContentsMargins(0, 0, 0, 0);
    denyTextLayout->setSpacing(3);

    denyTitleLabel_ = new QLabel(denyTextCol);
    denyTitleLabel_->setObjectName(QStringLiteral("settings-row-title"));
    denyTextLayout->addWidget(denyTitleLabel_);

    denySubLabel_ = new QLabel(denyTextCol);
    denySubLabel_->setObjectName(QStringLiteral("settings-row-description"));
    denyTextLayout->addWidget(denySubLabel_);

    denyLayout->addWidget(denyTextCol, 1, Qt::AlignVCenter);

    denyFrame_->clicked = [this] {
      radioDeny_->setChecked(true);
      onDefaultPolicyChanged(1);
    };
    addRow(defaultCard_, denyFrame_);

    layout->addWidget(defaultCard_);

    connect(defaultBtnGroup_, &QButtonGroup::idClicked, this, [this](int id) {
      onDefaultPolicyChanged(id);
    });

    connect(dmGroup_, &QButtonGroup::idClicked, this, [this](int id) {
      if (radioDeny_->isChecked()) return;
      QString mode = QStringLiteral("quiet");
      QString policy = QStringLiteral("quiet");
      if (id == 0) {
        mode = QStringLiteral("collapse_all");
        policy = QStringLiteral("quiet");
      } else if (id == 2) {
        mode = QStringLiteral("expand_all");
        policy = QStringLiteral("prompt");
      }
      profileService_->setRequestDisplayMode(permissionKey_, mode);
      profileService_->setPermissionDefaultPolicy(permissionKey_, policy);
    });

    // Zoom Card (only for zoomLevels)
    zoomCard_ = makeCard(column, I18n::text(QStringLiteral("settings.site.default_zoom_card")));
    auto *zoomRowWidget = new QWidget(zoomCard_);
    auto *zoomRowLayout = new QHBoxLayout(zoomRowWidget);
    zoomRowLayout->setContentsMargins(18, 12, 18, 12);
    zoomRowLayout->setSpacing(12);
    auto *zoomLbl = new QLabel(I18n::text(QStringLiteral("settings.site.default_zoom_label")), zoomRowWidget);
    zoomLbl->setStyleSheet(QStringLiteral("color: #e7edf5; font-size: 14px; font-weight: 500;"));
    zoomRowLayout->addWidget(zoomLbl, 1);
    zoomCombo_ = new QComboBox(zoomRowWidget);
    zoomCombo_->setMinimumWidth(160);
    zoomCombo_->addItems({QStringLiteral("%25"), QStringLiteral("%33"), QStringLiteral("%50"),
                          QStringLiteral("%67"), QStringLiteral("%75"), QStringLiteral("%80"),
                          QStringLiteral("%90"), I18n::text(QStringLiteral("settings.site.zoom_100_default")),
                          QStringLiteral("%110"), QStringLiteral("%125"), QStringLiteral("%150"),
                          QStringLiteral("%175"), QStringLiteral("%200"), QStringLiteral("%250"),
                          QStringLiteral("%300"), QStringLiteral("%400"), QStringLiteral("%500")});
    zoomRowLayout->addWidget(zoomCombo_);
    addRow(zoomCard_, zoomRowWidget);
    zoomCard_->setVisible(false);
    layout->addWidget(zoomCard_);

    connect(zoomCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
      if (permissionKey_ == QLatin1String("zoomLevels")) {
        const QString text = zoomCombo_->itemText(idx);
        QSettings().setValue(QStringLiteral("appearance/defaultZoom"), text);
      }
    });

    // 5. Özelleştirilmiş Davranışlar Card (default for permissions & content)
    customCard_ = makeCard(column, I18n::text(QStringLiteral("settings.site.customized_behaviors_card")));

    auto *customIntro = new QLabel(I18n::text(QStringLiteral("settings.site.customized_behaviors_intro")), customCard_);
    customIntro->setStyleSheet(QStringLiteral("color: #7f91a3; font-size: 12px; padding: 0 18px 8px;"));
    cardLayout(customCard_)->addWidget(customIntro);

    // Denied group
    auto *deniedSection = new QWidget(customCard_);
    auto *deniedSecLayout = new QVBoxLayout(deniedSection);
    deniedSecLayout->setContentsMargins(0, 0, 0, 0);
    deniedSecLayout->setSpacing(0);

    auto *deniedHeader = new QWidget(deniedSection);
    auto *deniedHLayout = new QHBoxLayout(deniedHeader);
    deniedHLayout->setContentsMargins(18, 12, 18, 8);
    deniedHLayout->setSpacing(12);

    deniedTitleLabel_ = new QLabel(deniedHeader);
    deniedTitleLabel_->setStyleSheet(QStringLiteral("color: #e7edf5; font-size: 14px; font-weight: 600;"));
    deniedHLayout->addWidget(deniedTitleLabel_, 1);

    auto *addDeniedBtn = new QPushButton(I18n::text(QStringLiteral("common.add")), deniedHeader);
    addDeniedBtn->setObjectName(QStringLiteral("settings-add-btn"));
    connect(addDeniedBtn, &QPushButton::clicked, this, [this] { showAddSiteDialog(false); });
    deniedHLayout->addWidget(addDeniedBtn);
    deniedSecLayout->addWidget(deniedHeader);

    deniedListWidget_ = new QWidget(deniedSection);
    deniedListLayout_ = new QVBoxLayout(deniedListWidget_);
    deniedListLayout_->setContentsMargins(0, 0, 0, 0);
    deniedListLayout_->setSpacing(0);
    deniedSecLayout->addWidget(deniedListWidget_);

    addRow(customCard_, deniedSection);

    // Allowed group
    auto *allowedSection = new QWidget(customCard_);
    auto *allowedSecLayout = new QVBoxLayout(allowedSection);
    allowedSecLayout->setContentsMargins(0, 0, 0, 0);
    allowedSecLayout->setSpacing(0);

    auto *allowedHeader = new QWidget(allowedSection);
    auto *allowedHLayout = new QHBoxLayout(allowedHeader);
    allowedHLayout->setContentsMargins(18, 12, 18, 8);
    allowedHLayout->setSpacing(12);

    allowedTitleLabel_ = new QLabel(allowedHeader);
    allowedTitleLabel_->setStyleSheet(QStringLiteral("color: #e7edf5; font-size: 14px; font-weight: 600;"));
    allowedHLayout->addWidget(allowedTitleLabel_, 1);

    auto *addAllowedBtn = new QPushButton(I18n::text(QStringLiteral("common.add")), allowedHeader);
    addAllowedBtn->setObjectName(QStringLiteral("settings-add-btn"));
    connect(addAllowedBtn, &QPushButton::clicked, this, [this] { showAddSiteDialog(true); });
    allowedHLayout->addWidget(addAllowedBtn);
    allowedSecLayout->addWidget(allowedHeader);

    allowedListWidget_ = new QWidget(allowedSection);
    allowedListLayout_ = new QVBoxLayout(allowedListWidget_);
    allowedListLayout_->setContentsMargins(0, 0, 0, 0);
    allowedListLayout_->setSpacing(0);
    allowedSecLayout->addWidget(allowedListWidget_);

    addRow(customCard_, allowedSection);

    layout->addWidget(customCard_);

    // 6. Üçüncü taraf çerezleri Card (only shown for thirdPartyCookies)
    cookiesCard_ = makeCard(column, I18n::text(QStringLiteral("settings.site.customized_behaviors_card")));

    auto *cookiesSection = new QWidget(cookiesCard_);
    auto *cookiesSecLayout = new QVBoxLayout(cookiesSection);
    cookiesSecLayout->setContentsMargins(0, 0, 0, 0);
    cookiesSecLayout->setSpacing(0);

    auto *cookiesHeader = new QWidget(cookiesSection);
    auto *cookiesHLayout = new QHBoxLayout(cookiesHeader);
    cookiesHLayout->setContentsMargins(18, 12, 18, 8);
    cookiesHLayout->setSpacing(12);

    auto *cookiesTitleLabel = new QLabel(I18n::text(QStringLiteral("settings.site.third_party_cookies_allowed_sites")), cookiesHeader);
    cookiesTitleLabel->setStyleSheet(QStringLiteral("color: #e7edf5; font-size: 14px; font-weight: 600;"));
    cookiesHLayout->addWidget(cookiesTitleLabel, 1);

    auto *addCookiesBtn = new QPushButton(I18n::text(QStringLiteral("common.add")), cookiesHeader);
    addCookiesBtn->setObjectName(QStringLiteral("settings-add-btn"));
    connect(addCookiesBtn, &QPushButton::clicked, this, [this] { showAddSiteDialog(true); });
    cookiesHLayout->addWidget(addCookiesBtn);
    cookiesSecLayout->addWidget(cookiesHeader);

    auto *noticeBox = new QFrame(cookiesSection);
    noticeBox->setObjectName(QStringLiteral("settings-cookies-notice"));
    noticeBox->setStyleSheet(QStringLiteral(R"CSS(
      QFrame#settings-cookies-notice {
        background: rgba(45, 80, 115, 0.25);
        border: 1px solid rgba(88, 166, 255, 0.25);
        border-radius: 8px;
        margin: 4px 18px 12px 18px;
      }
    )CSS"));
    auto *noticeLayout = new QHBoxLayout(noticeBox);
    noticeLayout->setContentsMargins(14, 10, 14, 10);
    noticeLayout->setSpacing(10);
    auto *noticeIcon = new QLabel(noticeBox);
    noticeIcon->setPixmap(BrowserIcons::icon(BrowserIcon::Privacy).pixmap(18, 18));
    noticeIcon->setFixedSize(18, 18);
    noticeLayout->addWidget(noticeIcon, 0, Qt::AlignVCenter);
    auto *noticeLabel = new QLabel(I18n::text(QStringLiteral("settings.site.cookie_shields_notice")), noticeBox);
    noticeLabel->setStyleSheet(QStringLiteral("color: #9ac2ef; font-size: 13px; background: transparent; border: none;"));
    noticeLabel->setWordWrap(true);
    noticeLayout->addWidget(noticeLabel, 1, Qt::AlignVCenter);
    cookiesSecLayout->addWidget(noticeBox);

    cookiesListWidget_ = new QWidget(cookiesSection);
    cookiesListLayout_ = new QVBoxLayout(cookiesListWidget_);
    cookiesListLayout_->setContentsMargins(0, 0, 0, 0);
    cookiesListLayout_->setSpacing(0);
    cookiesSecLayout->addWidget(cookiesListWidget_);

    addRow(cookiesCard_, cookiesSection);
    cookiesCard_->setVisible(false);
    layout->addWidget(cookiesCard_);

    layout->addStretch(1);

    outer->addWidget(column, 2);
    outer->addStretch(1);
  }

  void configure(const QString &permissionKey) {
    permissionKey_ = permissionKey;
    if (filterEdit_) {
      const QSignalBlocker blocker(filterEdit_);
      filterEdit_->clear();
    }

    if (permissionKey == QLatin1String("geolocation")) {
      titleLabel_->setText(I18n::text(QStringLiteral("settings.site.location")));
      introLabel_->setText(I18n::text(QStringLiteral("settings.site.location_intro")));
      allowIconLabel_->setPixmap(BrowserIcons::icon(BrowserIcon::Location).pixmap(18, 18));
      denyIconLabel_->setPixmap(BrowserIcons::icon(BrowserIcon::LocationSlash).pixmap(18, 18));
      allowTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.location_allow")));
      denyTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.location_deny")));
      denySubLabel_->setText(I18n::text(QStringLiteral("settings.site.location_deny_desc")));
      deniedTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.location_denied_list")));
      allowedTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.location_allowed_list")));
      hasDeviceSelector_ = false;
      hasDisplayMode_ = true;
    } else if (permissionKey == QLatin1String("camera")) {
      titleLabel_->setText(I18n::text(QStringLiteral("settings.site.camera")));
      introLabel_->setText(I18n::text(QStringLiteral("settings.site.camera_intro")));
      allowIconLabel_->setPixmap(BrowserIcons::icon(BrowserIcon::Camera).pixmap(18, 18));
      denyIconLabel_->setPixmap(BrowserIcons::icon(BrowserIcon::CameraSlash).pixmap(18, 18));
      allowTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.camera_allow")));
      denyTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.camera_deny")));
      denySubLabel_->setText(I18n::text(QStringLiteral("settings.site.camera_deny_desc")));
      deniedTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.camera_denied_list")));
      allowedTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.camera_allowed_list")));
      hasDeviceSelector_ = false;
      hasDisplayMode_ = false;
    } else if (permissionKey == QLatin1String("microphone")) {
      titleLabel_->setText(I18n::text(QStringLiteral("settings.site.microphone")));
      introLabel_->setText(I18n::text(QStringLiteral("settings.site.microphone_intro")));
      allowIconLabel_->setPixmap(BrowserIcons::icon(BrowserIcon::Microphone).pixmap(18, 18));
      denyIconLabel_->setPixmap(BrowserIcons::icon(BrowserIcon::MicrophoneSlash).pixmap(18, 18));
      allowTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.microphone_allow")));
      denyTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.microphone_deny")));
      denySubLabel_->setText(I18n::text(QStringLiteral("settings.site.microphone_deny_desc")));
      deniedTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.microphone_denied_list")));
      allowedTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.microphone_allowed_list")));
      hasDeviceSelector_ = true;
      hasDisplayMode_ = false;
    } else if (permissionKey == QLatin1String("notifications")) {
      titleLabel_->setText(I18n::text(QStringLiteral("settings.site.notifications")));
      introLabel_->setText(I18n::text(QStringLiteral("settings.site.notifications_intro")));
      allowIconLabel_->setPixmap(BrowserIcons::icon(BrowserIcon::Notification).pixmap(18, 18));
      denyIconLabel_->setPixmap(BrowserIcons::icon(BrowserIcon::NotificationSlash).pixmap(18, 18));
      allowTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.notifications_allow")));
      denyTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.notifications_deny")));
      denySubLabel_->setText(I18n::text(QStringLiteral("settings.site.notifications_deny_desc")));
      deniedTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.notifications_denied_list")));
      allowedTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.notifications_allowed_list")));
      hasDeviceSelector_ = false;
      hasDisplayMode_ = true;
    } else if (permissionKey == QLatin1String("clipboard")) {
      titleLabel_->setText(I18n::text(QStringLiteral("settings.site.clipboard")));
      introLabel_->setText(I18n::text(QStringLiteral("settings.site.clipboard_intro")));
      allowIconLabel_->setPixmap(BrowserIcons::icon(BrowserIcon::Clipboard).pixmap(18, 18));
      denyIconLabel_->setPixmap(BrowserIcons::icon(BrowserIcon::Clipboard).pixmap(18, 18));
      allowTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.clipboard_allow")));
      denyTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.clipboard_deny")));
      denySubLabel_->setText(I18n::text(QStringLiteral("settings.site.clipboard_deny_desc")));
      deniedTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.clipboard_denied_list")));
      allowedTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.clipboard_allowed_list")));
      hasDeviceSelector_ = false;
      hasDisplayMode_ = false;
    } else if (permissionKey == QLatin1String("localFonts")) {
      titleLabel_->setText(I18n::text(QStringLiteral("settings.site.local_fonts")));
      introLabel_->setText(I18n::text(QStringLiteral("settings.site.local_fonts_intro")));
      allowIconLabel_->setPixmap(BrowserIcons::icon(BrowserIcon::Fonts).pixmap(18, 18));
      denyIconLabel_->setPixmap(BrowserIcons::icon(BrowserIcon::Fonts).pixmap(18, 18));
      allowTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.local_fonts_allow")));
      denyTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.local_fonts_deny")));
      denySubLabel_->setText(I18n::text(QStringLiteral("settings.site.local_fonts_deny_desc")));
      deniedTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.local_fonts_denied_list")));
      allowedTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.local_fonts_allowed_list")));
      hasDeviceSelector_ = false;
      hasDisplayMode_ = false;
    } else if (permissionKey == QLatin1String("mouseLock")) {
      titleLabel_->setText(I18n::text(QStringLiteral("settings.site.pointer_lock")));
      introLabel_->setText(I18n::text(QStringLiteral("settings.site.pointer_lock_intro")));
      allowIconLabel_->setPixmap(BrowserIcons::icon(BrowserIcon::Mouse).pixmap(18, 18));
      denyIconLabel_->setPixmap(BrowserIcons::icon(BrowserIcon::Mouse).pixmap(18, 18));
      allowTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.pointer_lock_allow")));
      denyTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.pointer_lock_deny")));
      denySubLabel_->setText(I18n::text(QStringLiteral("settings.site.pointer_lock_deny_desc")));
      deniedTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.pointer_lock_denied_list")));
      allowedTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.pointer_lock_allowed_list")));
      hasDeviceSelector_ = false;
      hasDisplayMode_ = false;
    } else if (permissionKey == QLatin1String("screenShare")) {
      titleLabel_->setText(I18n::text(QStringLiteral("settings.site.screen_sharing")));
      introLabel_->setText(I18n::text(QStringLiteral("settings.site.screen_sharing_intro")));
      allowIconLabel_->setPixmap(BrowserIcons::icon(BrowserIcon::Window).pixmap(18, 18));
      denyIconLabel_->setPixmap(BrowserIcons::icon(BrowserIcon::Window).pixmap(18, 18));
      allowTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.screen_sharing_allow")));
      denyTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.screen_sharing_deny")));
      denySubLabel_->setText(I18n::text(QStringLiteral("settings.site.screen_sharing_deny_desc")));
      deniedTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.screen_sharing_denied_list")));
      allowedTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.screen_sharing_allowed_list")));
      hasDeviceSelector_ = false;
      hasDisplayMode_ = false;
    } else if (permissionKey == QLatin1String("javascript")) {
      titleLabel_->setText(QStringLiteral("JavaScript"));
      introLabel_->setText(I18n::text(QStringLiteral("settings.site.javascript_intro")));
      allowIconLabel_->setPixmap(BrowserIcons::icon(BrowserIcon::Javascript).pixmap(18, 18));
      denyIconLabel_->setPixmap(BrowserIcons::icon(BrowserIcon::JavascriptSlash).pixmap(18, 18));
      allowTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.javascript_allow_recommended")));
      denyTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.javascript_deny")));
      denySubLabel_->setText(I18n::text(QStringLiteral("settings.site.javascript_deny_desc")));
      deniedTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.javascript_denied_list")));
      allowedTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.javascript_allowed_list")));
      hasDeviceSelector_ = false;
      hasDisplayMode_ = false;
    } else if (permissionKey == QLatin1String("images")) {
      titleLabel_->setText(I18n::text(QStringLiteral("settings.site.images")));
      introLabel_->setText(I18n::text(QStringLiteral("settings.site.images_intro")));
      allowIconLabel_->setPixmap(BrowserIcons::icon(BrowserIcon::Image).pixmap(18, 18));
      denyIconLabel_->setPixmap(BrowserIcons::icon(BrowserIcon::ImageSlash).pixmap(18, 18));
      allowTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.images_allow_recommended")));
      denyTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.images_deny")));
      denySubLabel_->setText(I18n::text(QStringLiteral("settings.site.images_deny_desc")));
      deniedTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.images_denied_list")));
      allowedTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.images_allowed_list")));
      hasDeviceSelector_ = false;
      hasDisplayMode_ = false;
    } else if (permissionKey == QLatin1String("popups")) {
      titleLabel_->setText(I18n::text(QStringLiteral("settings.site.popups")));
      introLabel_->setText(I18n::text(QStringLiteral("settings.site.popups_intro")));
      allowIconLabel_->setPixmap(BrowserIcons::icon(BrowserIcon::Popup).pixmap(18, 18));
      denyIconLabel_->setPixmap(BrowserIcons::icon(BrowserIcon::PopupSlash).pixmap(18, 18));
      allowTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.popups_allow_send")));
      denyTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.popups_deny_recommended")));
      denySubLabel_->setText(I18n::text(QStringLiteral("settings.site.popups_deny_desc")));
      deniedTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.popups_denied_list")));
      allowedTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.popups_allowed_list")));
      hasDeviceSelector_ = false;
      hasDisplayMode_ = false;
    } else if (permissionKey == QLatin1String("thirdPartyCookies")) {
      titleLabel_->setText(I18n::text(QStringLiteral("settings.site.third_party_cookies")));
      introLabel_->setText(I18n::text(QStringLiteral("settings.site.third_party_cookies_intro")));
      hasDeviceSelector_ = false;
      hasDisplayMode_ = false;
    } else if (permissionKey == QLatin1String("sound")) {
      titleLabel_->setText(I18n::text(QStringLiteral("settings.site.sound")));
      introLabel_->setText(I18n::text(QStringLiteral("settings.site.sound_intro")));
      allowIconLabel_->setPixmap(BrowserIcons::icon(BrowserIcon::Audio).pixmap(18, 18));
      denyIconLabel_->setPixmap(BrowserIcons::icon(BrowserIcon::Audio).pixmap(18, 18));
      allowTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.sound_allow_recommended")));
      denyTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.sound_deny")));
      denySubLabel_->setText(I18n::text(QStringLiteral("settings.site.sound_deny_desc")));
      deniedTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.sound_denied_list")));
      allowedTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.sound_allowed_list")));
      hasDeviceSelector_ = false;
      hasDisplayMode_ = false;
    } else if (permissionKey == QLatin1String("zoomLevels")) {
      titleLabel_->setText(I18n::text(QStringLiteral("settings.site.zoom_levels")));
      introLabel_->setText(I18n::text(QStringLiteral("settings.site.zoom_levels_intro")));
      deniedTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.zoom_custom_list")));
      allowedTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.zoom_standard_list")));
      hasDeviceSelector_ = false;
      hasDisplayMode_ = false;
    } else if (permissionKey == QLatin1String("pdf")) {
      titleLabel_->setText(I18n::text(QStringLiteral("settings.site.pdf_documents")));
      introLabel_->setText(I18n::text(QStringLiteral("settings.site.pdf_intro")));
      allowIconLabel_->setPixmap(BrowserIcons::icon(BrowserIcon::Pdf).pixmap(18, 18));
      denyIconLabel_->setPixmap(BrowserIcons::icon(BrowserIcon::Download).pixmap(18, 18));
      allowTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.pdf_open_in_browser")));
      denyTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.pdf_download")));
      denySubLabel_->setText(I18n::text(QStringLiteral("settings.site.pdf_download_desc")));
      hasDeviceSelector_ = false;
      hasDisplayMode_ = false;
    } else if (permissionKey == QLatin1String("protectedContent")) {
      titleLabel_->setText(I18n::text(QStringLiteral("settings.site.protected_content")));
      introLabel_->setText(I18n::text(QStringLiteral("settings.site.protected_content_intro")));
      allowIconLabel_->setPixmap(BrowserIcons::icon(BrowserIcon::ProtectedContent).pixmap(18, 18));
      denyIconLabel_->setPixmap(BrowserIcons::icon(BrowserIcon::ProtectedContent).pixmap(18, 18));
      allowTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.protected_content_allow_recommended")));
      denyTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.protected_content_deny")));
      denySubLabel_->setText(I18n::text(QStringLiteral("settings.site.protected_content_deny_desc")));
      deniedTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.protected_content_denied_list")));
      allowedTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.protected_content_allowed_list")));
      hasDeviceSelector_ = false;
      hasDisplayMode_ = false;
    } else if (permissionKey == QLatin1String("insecureContent")) {
      titleLabel_->setText(I18n::text(QStringLiteral("settings.site.insecure_content")));
      introLabel_->setText(I18n::text(QStringLiteral("settings.site.insecure_content_intro")));
      allowIconLabel_->setPixmap(BrowserIcons::icon(BrowserIcon::InsecureContent).pixmap(18, 18));
      denyIconLabel_->setPixmap(BrowserIcons::icon(BrowserIcon::InsecureContent).pixmap(18, 18));
      allowTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.insecure_content_block_recommended")));
      denyTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.insecure_content_allow_all")));
      denySubLabel_->setText(I18n::text(QStringLiteral("settings.site.insecure_content_allow_desc")));
      deniedTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.insecure_content_blocked_list")));
      allowedTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.insecure_content_allowed_list")));
      hasDeviceSelector_ = false;
      hasDisplayMode_ = false;
    } else if (permissionKey == QLatin1String("siteData")) {
      titleLabel_->setText(I18n::text(QStringLiteral("settings.site.on_device_data")));
      introLabel_->setText(I18n::text(QStringLiteral("settings.site.on_device_data_intro")));
      allowIconLabel_->setPixmap(BrowserIcons::icon(BrowserIcon::SiteData).pixmap(18, 18));
      middleIconLabel_->setPixmap(BrowserIcons::icon(BrowserIcon::SiteData).pixmap(18, 18));
      denyIconLabel_->setPixmap(BrowserIcons::icon(BrowserIcon::SiteData).pixmap(18, 18));
      allowTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.on_device_data_allow_recommended")));
      middleTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.on_device_data_clear_on_exit")));
      middleSubLabel_->setText(I18n::text(QStringLiteral("settings.site.on_device_data_clear_on_exit_desc")));
      denyTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.on_device_data_block_not_recommended")));
      denySubLabel_->setText(I18n::text(QStringLiteral("settings.site.on_device_data_block_desc")));
      deniedTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.on_device_data_denied_list")));
      allowedTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.on_device_data_allowed_list")));
      hasDeviceSelector_ = false;
      hasDisplayMode_ = false;
    } else if (permissionKey == QLatin1String("jsOptimize")) {
      titleLabel_->setText(I18n::text(QStringLiteral("settings.site.v8_optimizer")));
      introLabel_->setText(I18n::text(QStringLiteral("settings.site.v8_optimizer_intro")));
      allowIconLabel_->setPixmap(BrowserIcons::icon(BrowserIcon::JsOptimize).pixmap(18, 18));
      denyIconLabel_->setPixmap(BrowserIcons::icon(BrowserIcon::JsOptimize).pixmap(18, 18));
      allowTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.v8_optimizer_allow_recommended")));
      denyTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.v8_optimizer_deny")));
      denySubLabel_->setText(I18n::text(QStringLiteral("settings.site.v8_optimizer_deny_desc")));
      deniedTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.v8_optimizer_denied_list")));
      allowedTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.v8_optimizer_allowed_list")));
      hasDeviceSelector_ = false;
      hasDisplayMode_ = false;
    } else if (permissionKey == QLatin1String("autoFullscreen")) {
      titleLabel_->setText(I18n::text(QStringLiteral("settings.site.auto_fullscreen")));
      introLabel_->setText(I18n::text(QStringLiteral("settings.site.auto_fullscreen_intro")));
      allowIconLabel_->setPixmap(BrowserIcons::icon(BrowserIcon::Fullscreen).pixmap(18, 18));
      denyIconLabel_->setPixmap(BrowserIcons::icon(BrowserIcon::Fullscreen).pixmap(18, 18));
      allowTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.auto_fullscreen_block_recommended")));
      denyTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.auto_fullscreen_allow")));
      denySubLabel_->setText(I18n::text(QStringLiteral("settings.site.auto_fullscreen_allow_desc")));
      deniedTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.auto_fullscreen_denied_list")));
      allowedTitleLabel_->setText(I18n::text(QStringLiteral("settings.site.auto_fullscreen_allowed_list")));
      hasDeviceSelector_ = false;
      hasDisplayMode_ = false;
    }

    const bool isCookies = (permissionKey == QLatin1String("thirdPartyCookies"));
    const bool isZoom = (permissionKey == QLatin1String("zoomLevels"));
    const bool isPdf = (permissionKey == QLatin1String("pdf"));
    const bool isSiteData = (permissionKey == QLatin1String("siteData"));

    if (helpBtn_) helpBtn_->setVisible(isCookies);
    if (defaultCard_) defaultCard_->setVisible(!isCookies && !isZoom);
    if (middleFrame_) middleFrame_->setVisible(isSiteData);
    if (zoomCard_) zoomCard_->setVisible(isZoom);
    if (customCard_) customCard_->setVisible(!isCookies && !isPdf);
    if (cookiesCard_) cookiesCard_->setVisible(isCookies);
    if (deviceContainer_) deviceContainer_->setVisible(!isCookies && hasDeviceSelector_);
    if (displayModeContainer_) displayModeContainer_->setVisible(!isCookies && hasDisplayMode_);

    refresh();
  }

  void refresh() {
    if (!profileService_ || permissionKey_.isEmpty()) return;

    if (permissionKey_ == QLatin1String("thirdPartyCookies")) {
      populateSiteList(cookiesListLayout_, profileService_->allowedOrigins(permissionKey_), true);
      if (filterEdit_ && !filterEdit_->text().isEmpty()) {
        filterSites(filterEdit_->text());
      }
      return;
    }

    if (permissionKey_ == QLatin1String("zoomLevels")) {
      const QString saved = QSettings().value(QStringLiteral("appearance/defaultZoom"), I18n::text(QStringLiteral("settings.site.zoom_100_default"))).toString();
      const QSignalBlocker blocker(zoomCombo_);
      int idx = zoomCombo_->findText(saved);
      if (idx >= 0) zoomCombo_->setCurrentIndex(idx);
      populateSiteList(deniedListLayout_, profileService_->deniedOrigins(permissionKey_), false);
      populateSiteList(allowedListLayout_, profileService_->allowedOrigins(permissionKey_), true);
      if (filterEdit_ && !filterEdit_->text().isEmpty()) filterSites(filterEdit_->text());
      return;
    }

    if (permissionKey_ == QLatin1String("siteData")) {
      const QString pol = profileService_->siteDataPolicy();
      const QSignalBlocker blocker(defaultBtnGroup_);
      if (pol == QStringLiteral("delete_on_exit")) {
        radioMiddle_->setChecked(true);
      } else if (pol == QStringLiteral("block")) {
        radioDeny_->setChecked(true);
      } else {
        radioAllow_->setChecked(true);
      }
      populateSiteList(deniedListLayout_, profileService_->deniedOrigins(permissionKey_), false);
      populateSiteList(allowedListLayout_, profileService_->allowedOrigins(permissionKey_), true);
      if (filterEdit_ && !filterEdit_->text().isEmpty()) filterSites(filterEdit_->text());
      return;
    }

    bool isDeny = false;
    if (permissionKey_ == QLatin1String("javascript")) {
      isDeny = !profileService_->isJavascriptEnabled();
    } else if (permissionKey_ == QLatin1String("images")) {
      isDeny = !profileService_->isAutoLoadImagesEnabled();
    } else if (permissionKey_ == QLatin1String("popups")) {
      isDeny = !profileService_->arePopupsAllowed();
    } else if (permissionKey_ == QLatin1String("sound")) {
      isDeny = !profileService_->isSoundAllowed();
    } else if (permissionKey_ == QLatin1String("pdf")) {
      isDeny = !profileService_->openPdfInBrowser();
    } else if (permissionKey_ == QLatin1String("protectedContent")) {
      isDeny = !profileService_->isProtectedContentEnabled();
    } else if (permissionKey_ == QLatin1String("insecureContent")) {
      isDeny = (profileService_->insecureContentPolicy() == QStringLiteral("allow"));
    } else if (permissionKey_ == QLatin1String("jsOptimize")) {
      isDeny = !profileService_->isJsOptimizationEnabled();
    } else if (permissionKey_ == QLatin1String("autoFullscreen")) {
      isDeny = profileService_->isAutoFullscreenAllowed();
    } else {
      const QString policy = profileService_->permissionDefaultPolicy(permissionKey_);
      isDeny = (policy == QLatin1String("deny"));
    }

    {
      const QSignalBlocker blocker(defaultBtnGroup_);
      radioAllow_->setChecked(!isDeny);
      radioDeny_->setChecked(isDeny);
    }

    if (hasDisplayMode_) {
      displayModeContainer_->setVisible(!isDeny);
      const QString mode = profileService_->requestDisplayMode(permissionKey_);
      const QSignalBlocker blocker(dmGroup_);
      if (mode == QLatin1String("collapse_all")) {
        radioCollapseAll_->setChecked(true);
      } else if (mode == QLatin1String("expand_all")) {
        radioExpandAll_->setChecked(true);
      } else {
        radioQuiet_->setChecked(true);
      }
    }

    if (hasDeviceSelector_ && deviceCombo_) {
      const QString dev = profileService_->preferredAudioInputDevice();
      const QSignalBlocker blocker(deviceCombo_);
      int idx = deviceCombo_->findText(dev);
      if (idx >= 0) {
        deviceCombo_->setCurrentIndex(idx);
      } else if (!dev.isEmpty()) {
        deviceCombo_->addItem(dev);
        deviceCombo_->setCurrentIndex(deviceCombo_->count() - 1);
      } else {
        deviceCombo_->setCurrentIndex(0);
      }
    }

    populateSiteList(deniedListLayout_, profileService_->deniedOrigins(permissionKey_), false);
    populateSiteList(allowedListLayout_, profileService_->allowedOrigins(permissionKey_), true);

    if (filterEdit_ && !filterEdit_->text().isEmpty()) {
      filterSites(filterEdit_->text());
    }
  }

 private:
  void filterSites(const QString &query) {
    const QString term = query.trimmed().toCaseFolded();
    auto filterLayout = [term](QVBoxLayout *layout) {
      if (!layout) return;
      for (int i = 0; i < layout->count(); ++i) {
        auto *w = layout->itemAt(i)->widget();
        if (!w) continue;
        const QString origin = w->property("originText").toString();
        if (!origin.isEmpty()) {
          w->setVisible(term.isEmpty() || origin.toCaseFolded().contains(term));
        }
      }
    };
    filterLayout(deniedListLayout_);
    filterLayout(allowedListLayout_);
    filterLayout(cookiesListLayout_);
  }

  void showAddSiteDialog(bool allow) {
    QDialog dialog(this);
    dialog.setWindowTitle(I18n::text(QStringLiteral("settings.site.add_site_title")));
    dialog.setWindowFlags(dialog.windowFlags() & ~Qt::WindowContextHelpButtonHint);
    dialog.setMinimumWidth(440);
    dialog.setStyleSheet(QStringLiteral(R"CSS(
      QDialog { background: #151d26; color: #edf5fc; border: 1px solid #2e3b49; border-radius: 12px; }
      QLabel { color: #edf5fc; font-size: 13px; }
      QLineEdit { min-height: 34px; background: #111820; color: #e6edf5; border: 1px solid #3a4958; border-radius: 7px; padding: 0 10px; font-size: 13px; }
      QLineEdit:focus { border: 2px solid #58a6c7; padding: 0 9px; }
      QPushButton { min-height: 32px; background: #243546; color: #edf5fc; border: 1px solid #3c5164; border-radius: 7px; padding: 0 16px; font-weight: 550; }
      QPushButton:hover { background: #2d4358; border-color: #4b667e; }
      QPushButton#primary { background: #32759e; border-color: #4595c2; }
      QPushButton#primary:hover { background: #3c8bb9; }
    )CSS"));

    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setSpacing(14);

    const bool isCookies = (permissionKey_ == QLatin1String("thirdPartyCookies"));
    QString titleText;
    if (isCookies) {
      titleText = I18n::text(QStringLiteral("settings.site.add_third_party_cookie_site"));
    } else {
      titleText = allow ? I18n::text(QStringLiteral("settings.site.add_allowed_site")) : I18n::text(QStringLiteral("settings.site.add_blocked_site"));
    }

    auto *titleLbl = new QLabel(titleText, &dialog);
    titleLbl->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: 650; color: #f2f7fc;"));
    layout->addWidget(titleLbl);

    QString descText = isCookies
        ? I18n::text(QStringLiteral("settings.site.add_site_prompt_wildcard"))
        : I18n::text(QStringLiteral("settings.site.add_site_prompt"));

    auto *descLbl = new QLabel(descText, &dialog);
    descLbl->setStyleSheet(QStringLiteral("color: #8fa0b3; font-size: 13px;"));
    layout->addWidget(descLbl);

    auto *edit = new QLineEdit(&dialog);
    edit->setPlaceholderText(isCookies ? I18n::text(QStringLiteral("settings.site.cookie_pattern_placeholder")) : QStringLiteral("https://www.example.com"));
    layout->addWidget(edit);

    auto *btnLayout = new QHBoxLayout;
    btnLayout->addStretch();
    auto *cancelBtn = new QPushButton(I18n::text(QStringLiteral("common.cancel")), &dialog);
    auto *addBtn = new QPushButton(I18n::text(QStringLiteral("common.add")), &dialog);
    addBtn->setObjectName(QStringLiteral("primary"));
    btnLayout->addWidget(cancelBtn);
    btnLayout->addWidget(addBtn);
    layout->addLayout(btnLayout);

    connect(cancelBtn, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(addBtn, &QPushButton::clicked, &dialog, &QDialog::accept);

    if (dialog.exec() == QDialog::Accepted) {
      QString urlStr = edit->text().trimmed();
      if (!urlStr.isEmpty()) {
        if (urlStr.startsWith(QLatin1String("[*.]"))) {
          profileService_->addSitePermissionRule(permissionKey_, urlStr, allow);
          refresh();
        } else {
          if (!urlStr.startsWith(QLatin1String("http://")) && !urlStr.startsWith(QLatin1String("https://"))) {
            urlStr = QStringLiteral("https://") + urlStr;
          }
          QUrl url(urlStr);
          if (url.isValid() && !url.host().isEmpty()) {
            const QString origin = url.scheme() + QStringLiteral("://") + url.authority();
            profileService_->addSitePermissionRule(permissionKey_, origin, allow);
            refresh();
          }
        }
      }
    }
  }

  void populateSiteList(QVBoxLayout *layout, const QStringList &origins, bool isAllowed) {
    while (QLayoutItem *item = layout->takeAt(0)) {
      if (item->widget()) delete item->widget();
      delete item;
    }

    if (origins.isEmpty()) {
      auto *emptyLabel = new QLabel(I18n::text(QStringLiteral("settings.site.no_sites_added")), layout->parentWidget());
      emptyLabel->setStyleSheet(QStringLiteral("color: #728496; font-size: 13px; font-style: italic; padding: 10px 18px;"));
      layout->addWidget(emptyLabel);
      return;
    }

    for (const QString &origin : origins) {
      auto *row = new QWidget(layout->parentWidget());
      row->setObjectName(QStringLiteral("settings-site-row"));
      row->setProperty("originText", origin);
      auto *rowLayout = new QHBoxLayout(row);
      rowLayout->setContentsMargins(18, 8, 18, 8);
      rowLayout->setSpacing(12);

      auto *iconLbl = new QLabel(row);
      iconLbl->setPixmap(BrowserIcons::icon(BrowserIcon::Privacy).pixmap(16, 16));
      iconLbl->setFixedSize(16, 16);
      rowLayout->addWidget(iconLbl);

      auto *originLbl = new QLabel(origin, row);
      originLbl->setStyleSheet(QStringLiteral("color: #edf5fc; font-size: 13px; font-weight: 500;"));
      originLbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
      rowLayout->addWidget(originLbl, 1);

      auto *delBtn = new QPushButton(row);
      delBtn->setObjectName(QStringLiteral("settings-subpage-del-btn"));
      delBtn->setFixedSize(28, 28);
      delBtn->setCursor(Qt::PointingHandCursor);
      delBtn->setIcon(BrowserIcons::icon(BrowserIcon::Trash));
      delBtn->setIconSize(QSize(14, 14));
      delBtn->setToolTip(I18n::text(QStringLiteral("common.remove")));
      delBtn->setAccessibleName(I18n::text(QStringLiteral("common.remove_item")).arg(origin));
      connect(delBtn, &QPushButton::clicked, this, [this, origin] {
        profileService_->removeSitePermissionRule(permissionKey_, origin);
        refresh();
      });
      rowLayout->addWidget(delBtn);

      layout->addWidget(row);
    }
  }

  void onDefaultPolicyChanged(int optionId) {
    if (!profileService_ || permissionKey_.isEmpty()) return;

    if (permissionKey_ == QLatin1String("javascript")) {
      profileService_->setJavascriptEnabled(optionId == 0);
      return;
    }
    if (permissionKey_ == QLatin1String("images")) {
      profileService_->setAutoLoadImagesEnabled(optionId == 0);
      return;
    }
    if (permissionKey_ == QLatin1String("popups")) {
      profileService_->setPopupsAllowed(optionId == 0);
      return;
    }
    if (permissionKey_ == QLatin1String("sound")) {
      profileService_->setSoundAllowed(optionId == 0);
      return;
    }
    if (permissionKey_ == QLatin1String("pdf")) {
      profileService_->setOpenPdfInBrowser(optionId == 0);
      return;
    }
    if (permissionKey_ == QLatin1String("protectedContent")) {
      profileService_->setProtectedContentEnabled(optionId == 0);
      return;
    }
    if (permissionKey_ == QLatin1String("insecureContent")) {
      profileService_->setInsecureContentPolicy(optionId == 0 ? QStringLiteral("block") : QStringLiteral("allow"));
      return;
    }
    if (permissionKey_ == QLatin1String("siteData")) {
      if (optionId == 0) profileService_->setSiteDataPolicy(QStringLiteral("allow"));
      else if (optionId == 2) profileService_->setSiteDataPolicy(QStringLiteral("delete_on_exit"));
      else if (optionId == 1) profileService_->setSiteDataPolicy(QStringLiteral("block"));
      return;
    }
    if (permissionKey_ == QLatin1String("jsOptimize")) {
      profileService_->setJsOptimizationEnabled(optionId == 0);
      return;
    }
    if (permissionKey_ == QLatin1String("autoFullscreen")) {
      profileService_->setAutoFullscreenAllowed(optionId == 1);
      return;
    }

    const bool isDeny = (optionId == 1);
    if (isDeny) {
      if (hasDisplayMode_) displayModeContainer_->setVisible(false);
      profileService_->setPermissionDefaultPolicy(permissionKey_, QStringLiteral("deny"));
    } else {
      if (hasDisplayMode_) {
        displayModeContainer_->setVisible(true);
        const QString mode = profileService_->requestDisplayMode(permissionKey_);
        if (mode == QLatin1String("expand_all")) {
          profileService_->setPermissionDefaultPolicy(permissionKey_, QStringLiteral("prompt"));
        } else {
          profileService_->setPermissionDefaultPolicy(permissionKey_, QStringLiteral("quiet"));
        }
      } else {
        profileService_->setPermissionDefaultPolicy(permissionKey_, QStringLiteral("prompt"));
      }
    }
  }

  BrowserProfileService *profileService_ = nullptr;
  std::function<void()> onBack_;
  QString permissionKey_;

  QLabel *titleLabel_ = nullptr;
  QPushButton *helpBtn_ = nullptr;
  QLineEdit *filterEdit_ = nullptr;

  QLabel *introLabel_ = nullptr;
  QWidget *deviceContainer_ = nullptr;
  QComboBox *deviceCombo_ = nullptr;

  QFrame *defaultCard_ = nullptr;
  ClickableFrame *allowFrame_ = nullptr;
  QRadioButton *radioAllow_ = nullptr;
  QLabel *allowIconLabel_ = nullptr;
  QLabel *allowTitleLabel_ = nullptr;

  QWidget *displayModeContainer_ = nullptr;
  QButtonGroup *dmGroup_ = nullptr;
  QRadioButton *radioCollapseAll_ = nullptr;
  QRadioButton *radioQuiet_ = nullptr;
  QRadioButton *radioExpandAll_ = nullptr;

  ClickableFrame *middleFrame_ = nullptr;
  QRadioButton *radioMiddle_ = nullptr;
  QLabel *middleIconLabel_ = nullptr;
  QLabel *middleTitleLabel_ = nullptr;
  QLabel *middleSubLabel_ = nullptr;

  ClickableFrame *denyFrame_ = nullptr;
  QRadioButton *radioDeny_ = nullptr;
  QLabel *denyIconLabel_ = nullptr;
  QLabel *denyTitleLabel_ = nullptr;
  QLabel *denySubLabel_ = nullptr;

  QButtonGroup *defaultBtnGroup_ = nullptr;

  QFrame *zoomCard_ = nullptr;
  QComboBox *zoomCombo_ = nullptr;

  QFrame *customCard_ = nullptr;
  QLabel *deniedTitleLabel_ = nullptr;
  QLabel *allowedTitleLabel_ = nullptr;
  QWidget *deniedListWidget_ = nullptr;
  QWidget *allowedListWidget_ = nullptr;
  QVBoxLayout *deniedListLayout_ = nullptr;
  QVBoxLayout *allowedListLayout_ = nullptr;

  QFrame *cookiesCard_ = nullptr;
  QWidget *cookiesListWidget_ = nullptr;
  QVBoxLayout *cookiesListLayout_ = nullptr;

  bool hasDeviceSelector_ = false;
  bool hasDisplayMode_ = false;
};

QWidget *SettingsPage::createPrivacySection() {
  privacyStack_ = new DynamicStackedWidget;
  privacyStack_->setObjectName(QStringLiteral("settings-privacy-stack"));

  privacySubpage_ = new PrivacyDetailSubpage(profileService_, [this] {
    if (privacyStack_) {
      privacyStack_->setCurrentIndex(0);
      privacyStack_->updateGeometry();
      QWidget *p = privacyStack_->parentWidget();
      while (p) {
        if (auto *scroll = qobject_cast<QScrollArea *>(p)) {
          scroll->verticalScrollBar()->setValue(0);
          break;
        }
        p = p->parentWidget();
      }
    }
    if (updatePrivacySubtitles_) updatePrivacySubtitles_();
  }, privacyStack_);

  Section section = makeSection(I18n::text(QStringLiteral("settings.category.privacy")), I18n::text(QStringLiteral("settings.privacy.description")));

  // =========================================================================
  // 1. İZİNLER KARTI
  // =========================================================================
  auto *permissionsCard = makeCard(section.page, I18n::text(QStringLiteral("settings.privacy.permissions_card")));

  auto openSubpage = [this](const QString &key) {
    if (privacySubpage_ && privacyStack_) {
      privacySubpage_->configure(key);
      privacyStack_->setCurrentIndex(1);
      privacyStack_->updateGeometry();
      QWidget *p = privacyStack_->parentWidget();
      while (p) {
        if (auto *scroll = qobject_cast<QScrollArea *>(p)) {
          scroll->verticalScrollBar()->setValue(0);
          break;
        }
        p = p->parentWidget();
      }
    }
  };

  // Konum
  const auto getGeoSub = [this] {
    return profileService_->permissionDefaultPolicy(QStringLiteral("geolocation")) == QStringLiteral("deny")
        ? I18n::text(QStringLiteral("settings.site.location_deny"))
        : I18n::text(QStringLiteral("settings.site.location_allow"));
  };
  InteractiveSettingRowResult geoRow = makeInteractiveSettingRow(
      permissionsCard, BrowserIcon::Location, I18n::text(QStringLiteral("settings.site.location")), getGeoSub(),
      [openSubpage] { openSubpage(QStringLiteral("geolocation")); });
  addRow(permissionsCard, geoRow.frame);

  // Kamera
  const auto getCamSub = [this] {
    return profileService_->permissionDefaultPolicy(QStringLiteral("camera")) == QStringLiteral("deny")
        ? I18n::text(QStringLiteral("settings.site.camera_deny"))
        : I18n::text(QStringLiteral("settings.site.camera_allow"));
  };
  InteractiveSettingRowResult camRow = makeInteractiveSettingRow(
      permissionsCard, BrowserIcon::Camera, I18n::text(QStringLiteral("settings.site.camera")), getCamSub(),
      [openSubpage] { openSubpage(QStringLiteral("camera")); });
  addRow(permissionsCard, camRow.frame);

  // Mikrofon
  const auto getMicSub = [this] {
    return profileService_->permissionDefaultPolicy(QStringLiteral("microphone")) == QStringLiteral("deny")
        ? I18n::text(QStringLiteral("settings.site.microphone_deny"))
        : I18n::text(QStringLiteral("settings.site.microphone_allow"));
  };
  InteractiveSettingRowResult micRow = makeInteractiveSettingRow(
      permissionsCard, BrowserIcon::Microphone, I18n::text(QStringLiteral("settings.site.microphone")), getMicSub(),
      [openSubpage] { openSubpage(QStringLiteral("microphone")); });
  addRow(permissionsCard, micRow.frame);

  // Bildirimler
  const auto getNotifSub = [this] {
    const auto pol = profileService_->permissionDefaultPolicy(QStringLiteral("notifications"));
    if (pol == QStringLiteral("deny")) return I18n::text(QStringLiteral("settings.site.notifications_deny"));
    return I18n::text(QStringLiteral("settings.site.notifications_allow"));
  };
  InteractiveSettingRowResult notifRow = makeInteractiveSettingRow(
      permissionsCard, BrowserIcon::Notification, I18n::text(QStringLiteral("settings.site.notifications")), getNotifSub(),
      [openSubpage] { openSubpage(QStringLiteral("notifications")); });
  addRow(permissionsCard, notifRow.frame);

  // Ek izinler (Akordeon)
  auto extraPerms = makeCollapsibleSectionRow(permissionsCard, I18n::text(QStringLiteral("settings.privacy.additional_permissions")));
  addRow(permissionsCard, extraPerms.headerRow);

  const auto getClipSub = [this] {
    return profileService_->permissionDefaultPolicy(QStringLiteral("clipboard")) == QStringLiteral("deny")
        ? I18n::text(QStringLiteral("settings.site.clipboard_deny"))
        : I18n::text(QStringLiteral("settings.site.clipboard_allow"));
  };
  const auto getFontsSub = [this] {
    return profileService_->permissionDefaultPolicy(QStringLiteral("localFonts")) == QStringLiteral("deny")
        ? I18n::text(QStringLiteral("settings.site.local_fonts_deny"))
        : I18n::text(QStringLiteral("settings.site.local_fonts_allow"));
  };
  const auto getMouseSub = [this] {
    return profileService_->permissionDefaultPolicy(QStringLiteral("mouseLock")) == QStringLiteral("deny")
        ? I18n::text(QStringLiteral("settings.site.pointer_lock_deny"))
        : I18n::text(QStringLiteral("settings.site.pointer_lock_allow"));
  };
  const auto getScreenSub = [this] {
    return profileService_->permissionDefaultPolicy(QStringLiteral("screenShare")) == QStringLiteral("deny")
        ? I18n::text(QStringLiteral("settings.site.screen_sharing_deny"))
        : I18n::text(QStringLiteral("settings.site.screen_sharing_allow"));
  };

  // Ek izin 1: Pano
  InteractiveSettingRowResult clipRow = makeInteractiveSettingRow(
      extraPerms.childContainer, BrowserIcon::Clipboard, I18n::text(QStringLiteral("settings.site.clipboard")), getClipSub(),
      [openSubpage] { openSubpage(QStringLiteral("clipboard")); });
  extraPerms.childContainer->layout()->addWidget(clipRow.frame);

  // Ek izin 2: Yerel yazı tipleri
  InteractiveSettingRowResult fontsRow = makeInteractiveSettingRow(
      extraPerms.childContainer, BrowserIcon::Fonts, I18n::text(QStringLiteral("settings.site.local_fonts")), getFontsSub(),
      [openSubpage] { openSubpage(QStringLiteral("localFonts")); });
  extraPerms.childContainer->layout()->addWidget(fontsRow.frame);

  // Ek izin 3: Fare kilidi
  InteractiveSettingRowResult mouseRow = makeInteractiveSettingRow(
      extraPerms.childContainer, BrowserIcon::Mouse, I18n::text(QStringLiteral("settings.site.pointer_lock")), getMouseSub(),
      [openSubpage] { openSubpage(QStringLiteral("mouseLock")); });
  extraPerms.childContainer->layout()->addWidget(mouseRow.frame);

  // Ek izin 4: Ekran paylaşımı
  InteractiveSettingRowResult screenRow = makeInteractiveSettingRow(
      extraPerms.childContainer, BrowserIcon::Window, I18n::text(QStringLiteral("settings.site.screen_sharing")), getScreenSub(),
      [openSubpage] { openSubpage(QStringLiteral("screenShare")); });
  extraPerms.childContainer->layout()->addWidget(screenRow.frame);

  addRow(permissionsCard, extraPerms.childContainer);
  section.layout->addWidget(permissionsCard);

  // =========================================================================
  // 2. İÇERİK KARTI
  // =========================================================================
  auto *contentCard = makeCard(section.page, I18n::text(QStringLiteral("settings.privacy.content_card")));

  const auto getCookieSub = [this] {
    const auto pol = profileService_->cookiePolicy();
    if (pol == QStringLiteral("block_all")) return I18n::text(QStringLiteral("settings.privacy.cookies_block_all"));
    if (pol == QStringLiteral("allow_all")) return I18n::text(QStringLiteral("settings.privacy.cookies_allow_all"));
    return I18n::text(QStringLiteral("settings.privacy.cookies_block_third_party"));
  };
  const auto getJsSub = [this] {
    return profileService_->isJavascriptEnabled()
        ? I18n::text(QStringLiteral("settings.privacy.javascript_allowed"))
        : I18n::text(QStringLiteral("settings.site.javascript_deny"));
  };
  const auto getImgSub = [this] {
    return profileService_->isAutoLoadImagesEnabled()
        ? I18n::text(QStringLiteral("settings.privacy.images_allowed"))
        : I18n::text(QStringLiteral("settings.site.images_deny"));
  };
  const auto getPopupSub = [this] {
    return !profileService_->arePopupsAllowed()
        ? I18n::text(QStringLiteral("settings.privacy.popups_blocked"))
        : I18n::text(QStringLiteral("settings.privacy.popups_allowed"));
  };

  // 1. Üçüncü taraf çerezleri
  InteractiveSettingRowResult cookieRow = makeInteractiveSettingRow(
      contentCard, BrowserIcon::Cookie, I18n::text(QStringLiteral("settings.site.third_party_cookies")), getCookieSub(),
      [openSubpage] { openSubpage(QStringLiteral("thirdPartyCookies")); });
  addRow(contentCard, cookieRow.frame);

  // 2. JavaScript
  InteractiveSettingRowResult jsRow = makeInteractiveSettingRow(
      contentCard, BrowserIcon::Javascript, QStringLiteral("JavaScript"), getJsSub(),
      [openSubpage] { openSubpage(QStringLiteral("javascript")); });
  addRow(contentCard, jsRow.frame);

  // 3. Resimler
  InteractiveSettingRowResult imgRow = makeInteractiveSettingRow(
      contentCard, BrowserIcon::Image, I18n::text(QStringLiteral("settings.site.images")), getImgSub(),
      [openSubpage] { openSubpage(QStringLiteral("images")); });
  addRow(contentCard, imgRow.frame);

  // 4. Pop-up ve yönlendirmeler
  InteractiveSettingRowResult popupRow = makeInteractiveSettingRow(
      contentCard, BrowserIcon::Popup, I18n::text(QStringLiteral("settings.site.popups")), getPopupSub(),
      [openSubpage] { openSubpage(QStringLiteral("popups")); });
  addRow(contentCard, popupRow.frame);

  // Ek içerik ayarları (Akordeon)
  auto extraContent = makeCollapsibleSectionRow(contentCard, I18n::text(QStringLiteral("settings.privacy.additional_content_settings")));
  addRow(contentCard, extraContent.headerRow);

  // Subtitle getters for extra content
  const auto getSoundSub = [this] {
    return profileService_->isSoundAllowed()
        ? I18n::text(QStringLiteral("settings.privacy.sound_allowed"))
        : I18n::text(QStringLiteral("settings.site.sound_deny"));
  };
  const auto getPdfSub = [this] {
    return profileService_->openPdfInBrowser()
        ? I18n::text(QStringLiteral("settings.site.pdf_open_in_browser"))
        : I18n::text(QStringLiteral("settings.site.pdf_download"));
  };
  const auto getProtectedContentSub = [this] {
    return profileService_->isProtectedContentEnabled()
        ? I18n::text(QStringLiteral("settings.privacy.protected_content_allowed"))
        : I18n::text(QStringLiteral("settings.site.protected_content_deny"));
  };
  const auto getInsecureContentSub = [this] {
    return profileService_->insecureContentPolicy() == QStringLiteral("allow")
        ? I18n::text(QStringLiteral("settings.site.insecure_content_allow_all"))
        : I18n::text(QStringLiteral("settings.privacy.insecure_content_blocked"));
  };
  const auto getSiteDataSub = [this] {
    const QString pol = profileService_->siteDataPolicy();
    if (pol == QStringLiteral("delete_on_exit")) return I18n::text(QStringLiteral("settings.site.on_device_data_clear_on_exit"));
    if (pol == QStringLiteral("block")) return I18n::text(QStringLiteral("settings.privacy.on_device_data_blocked"));
    return I18n::text(QStringLiteral("settings.privacy.on_device_data_allowed"));
  };
  const auto getJsOptimizeSub = [this] {
    return profileService_->isJsOptimizationEnabled()
        ? I18n::text(QStringLiteral("settings.privacy.v8_optimizer_allowed"))
        : I18n::text(QStringLiteral("settings.site.v8_optimizer_deny"));
  };
  const auto getAutoFullscreenSub = [this] {
    return profileService_->isAutoFullscreenAllowed()
        ? I18n::text(QStringLiteral("settings.site.auto_fullscreen_allow"))
        : I18n::text(QStringLiteral("settings.privacy.auto_fullscreen_blocked"));
  };

  // 1. Ses
  InteractiveSettingRowResult soundRow = makeInteractiveSettingRow(
      extraContent.childContainer, BrowserIcon::Audio, I18n::text(QStringLiteral("settings.site.sound")), getSoundSub(),
      [openSubpage] { openSubpage(QStringLiteral("sound")); });
  extraContent.childContainer->layout()->addWidget(soundRow.frame);

  // 2. Yakınlaştırma seviyeleri
  InteractiveSettingRowResult zoomRow = makeInteractiveSettingRow(
      extraContent.childContainer, BrowserIcon::Zoom, I18n::text(QStringLiteral("settings.site.zoom_levels")),
      I18n::text(QStringLiteral("settings.privacy.zoom_levels_summary")),
      [openSubpage] { openSubpage(QStringLiteral("zoomLevels")); });
  extraContent.childContainer->layout()->addWidget(zoomRow.frame);

  // 3. PDF dokümanları
  InteractiveSettingRowResult pdfRow = makeInteractiveSettingRow(
      extraContent.childContainer, BrowserIcon::Pdf, I18n::text(QStringLiteral("settings.site.pdf_documents")), getPdfSub(),
      [openSubpage] { openSubpage(QStringLiteral("pdf")); });
  extraContent.childContainer->layout()->addWidget(pdfRow.frame);

  // 4. Korumalı içerik kimlikleri
  InteractiveSettingRowResult protectedContentRow = makeInteractiveSettingRow(
      extraContent.childContainer, BrowserIcon::ProtectedContent, I18n::text(QStringLiteral("settings.site.protected_content")), getProtectedContentSub(),
      [openSubpage] { openSubpage(QStringLiteral("protectedContent")); });
  extraContent.childContainer->layout()->addWidget(protectedContentRow.frame);

  // 5. Güvenli olmayan içerik
  InteractiveSettingRowResult insecureContentRow = makeInteractiveSettingRow(
      extraContent.childContainer, BrowserIcon::InsecureContent, I18n::text(QStringLiteral("settings.site.insecure_content")), getInsecureContentSub(),
      [openSubpage] { openSubpage(QStringLiteral("insecureContent")); });
  extraContent.childContainer->layout()->addWidget(insecureContentRow.frame);

  // 6. Cihaz üzerindeki site verileri
  InteractiveSettingRowResult siteDataRow = makeInteractiveSettingRow(
      extraContent.childContainer, BrowserIcon::SiteData, I18n::text(QStringLiteral("settings.site.on_device_data")), getSiteDataSub(),
      [openSubpage] { openSubpage(QStringLiteral("siteData")); });
  extraContent.childContainer->layout()->addWidget(siteDataRow.frame);

  // 7. JavaScript optimizasyonu ve güvenlik
  InteractiveSettingRowResult jsOptimizeRow = makeInteractiveSettingRow(
      extraContent.childContainer, BrowserIcon::JsOptimize, I18n::text(QStringLiteral("settings.site.v8_optimizer")), getJsOptimizeSub(),
      [openSubpage] { openSubpage(QStringLiteral("jsOptimize")); });
  extraContent.childContainer->layout()->addWidget(jsOptimizeRow.frame);

  // 8. Otomatik tam ekran
  InteractiveSettingRowResult autoFullscreenRow = makeInteractiveSettingRow(
      extraContent.childContainer, BrowserIcon::Fullscreen, I18n::text(QStringLiteral("settings.site.auto_fullscreen")), getAutoFullscreenSub(),
      [openSubpage] { openSubpage(QStringLiteral("autoFullscreen")); });
  extraContent.childContainer->layout()->addWidget(autoFullscreenRow.frame);

  addRow(contentCard, extraContent.childContainer);
  section.layout->addWidget(contentCard);

  updatePrivacySubtitles_ = [geoRow, getGeoSub, camRow, getCamSub, micRow, getMicSub, notifRow, getNotifSub,
                             clipRow, getClipSub, fontsRow, getFontsSub, mouseRow, getMouseSub, screenRow, getScreenSub,
                             cookieRow, getCookieSub, jsRow, getJsSub, imgRow, getImgSub, popupRow, getPopupSub,
                             soundRow, getSoundSub, pdfRow, getPdfSub, protectedContentRow, getProtectedContentSub,
                             insecureContentRow, getInsecureContentSub, siteDataRow, getSiteDataSub,
                             jsOptimizeRow, getJsOptimizeSub, autoFullscreenRow, getAutoFullscreenSub] {
    if (geoRow.subtitleLabel) geoRow.subtitleLabel->setText(getGeoSub());
    if (camRow.subtitleLabel) camRow.subtitleLabel->setText(getCamSub());
    if (micRow.subtitleLabel) micRow.subtitleLabel->setText(getMicSub());
    if (notifRow.subtitleLabel) notifRow.subtitleLabel->setText(getNotifSub());
    if (clipRow.subtitleLabel) clipRow.subtitleLabel->setText(getClipSub());
    if (fontsRow.subtitleLabel) fontsRow.subtitleLabel->setText(getFontsSub());
    if (mouseRow.subtitleLabel) mouseRow.subtitleLabel->setText(getMouseSub());
    if (screenRow.subtitleLabel) screenRow.subtitleLabel->setText(getScreenSub());
    if (cookieRow.subtitleLabel) cookieRow.subtitleLabel->setText(getCookieSub());
    if (jsRow.subtitleLabel) jsRow.subtitleLabel->setText(getJsSub());
    if (imgRow.subtitleLabel) imgRow.subtitleLabel->setText(getImgSub());
    if (popupRow.subtitleLabel) popupRow.subtitleLabel->setText(getPopupSub());
    if (soundRow.subtitleLabel) soundRow.subtitleLabel->setText(getSoundSub());
    if (pdfRow.subtitleLabel) pdfRow.subtitleLabel->setText(getPdfSub());
    if (protectedContentRow.subtitleLabel) protectedContentRow.subtitleLabel->setText(getProtectedContentSub());
    if (insecureContentRow.subtitleLabel) insecureContentRow.subtitleLabel->setText(getInsecureContentSub());
    if (siteDataRow.subtitleLabel) siteDataRow.subtitleLabel->setText(getSiteDataSub());
    if (jsOptimizeRow.subtitleLabel) jsOptimizeRow.subtitleLabel->setText(getJsOptimizeSub());
    if (autoFullscreenRow.subtitleLabel) autoFullscreenRow.subtitleLabel->setText(getAutoFullscreenSub());
  };

  // =========================================================================
  // 3. KULLANILMAYAN SİTELERİN İZİNLERİNİ OTOMATİK OLARAK KALDIR
  // =========================================================================
  auto *autoRevokeCard = makeCard(section.page);
  auto *autoRevokeSwitch = new GlowToggleSwitch(autoRevokeCard);
  autoRevokeSwitch->setChecked(profileService_->autoRevokeUnusedPermissions());
  autoRevokeSwitch->setAccessibleName(I18n::text(QStringLiteral("settings.privacy.auto_revoke")));
  addRow(autoRevokeCard, settingRow(
      autoRevokeCard,
      I18n::text(QStringLiteral("settings.privacy.auto_revoke")),
      I18n::text(QStringLiteral("settings.privacy.auto_revoke_desc")),
      autoRevokeSwitch));
  section.layout->addWidget(autoRevokeCard);
  connect(autoRevokeSwitch, &QCheckBox::toggled, this, [this](bool checked) {
    profileService_->setAutoRevokeUnusedPermissions(checked);
  });

  // =========================================================================
  // 4. TARAMA VERİLERİ VE İZLEME KORUMASI KARTI
  // =========================================================================
  auto *privacyCard = makeCard(section.page, I18n::text(QStringLiteral("settings.privacy.browsing_data_card")));
  auto *adultSwitch = new GlowToggleSwitch(privacyCard);
  adultSwitch->setChecked(profileService_->isAdultContentProtectionEnabled());
  adultSwitch->setAccessibleName(I18n::text(QStringLiteral("settings.privacy.adult_content_protection")));
  addRow(privacyCard, settingRow(
      privacyCard,
      I18n::text(QStringLiteral("settings.privacy.adult_content_protection")),
      I18n::text(QStringLiteral("settings.privacy.adult_content_protection_desc")),
      adultSwitch,
      BrowserIcon::Privacy,
      true));
  auto *strip = new QCheckBox(privacyCard); strip->setAccessibleName(I18n::text(QStringLiteral("settings.privacy.strip_tracking_params"))); strip->setChecked(profileService_->stripsTrackingParameters());
  addRow(privacyCard, settingRow(privacyCard, I18n::text(QStringLiteral("settings.privacy.strip_tracking_params")), I18n::text(QStringLiteral("settings.privacy.strip_tracking_params_desc")), strip, BrowserIcon::Privacy, true));
  auto *cache = new QPushButton(I18n::text(QStringLiteral("common.clear")), privacyCard); cache->setProperty("danger", true); cache->setAccessibleName(I18n::text(QStringLiteral("settings.privacy.clear_http_cache")));
  addRow(privacyCard, settingRow(privacyCard, I18n::text(QStringLiteral("settings.privacy.http_cache")), I18n::text(QStringLiteral("settings.privacy.http_cache_desc")), cache, BrowserIcon::Trash, true));
  auto *cookies = new QPushButton(I18n::text(QStringLiteral("common.clear")), privacyCard); cookies->setProperty("danger", true); cookies->setAccessibleName(I18n::text(QStringLiteral("settings.privacy.clear_cookies")));
  addRow(privacyCard, settingRow(privacyCard, I18n::text(QStringLiteral("settings.privacy.cookies_and_site_data")), I18n::text(QStringLiteral("settings.privacy.cookies_and_site_data_desc")), cookies));
  section.layout->addWidget(privacyCard);

  connect(adultSwitch, &QCheckBox::toggled, this, [this](bool enabled) { profileService_->setAdultContentProtectionEnabled(enabled); });
  connect(strip, &QCheckBox::toggled, this, [this](bool enabled) { profileService_->setStripsTrackingParameters(enabled); });
  connect(cache, &QPushButton::clicked, this, [this] { profileService_->clearHttpCache(); QMessageBox::information(this, I18n::text(QStringLiteral("settings.privacy.cache")), I18n::text(QStringLiteral("settings.privacy.http_cache_clear_requested"))); });
  connect(cookies, &QPushButton::clicked, this, [this] { if (QMessageBox::question(this, I18n::text(QStringLiteral("settings.privacy.clear_cookies")), I18n::text(QStringLiteral("settings.privacy.clear_cookies_confirm"))) == QMessageBox::Yes) profileService_->clearCookies(); });

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
  auto *sitePermsCard = makeCard(section.page, I18n::text(QStringLiteral("settings.privacy.saved_site_permissions_card")));
  auto *list = new QListWidget(sitePermsCard); list->setObjectName(QStringLiteral("settings-data-list")); list->setAccessibleName(I18n::text(QStringLiteral("settings.privacy.persistent_site_permissions"))); list->setMinimumHeight(180);
  auto *reset = new QPushButton(I18n::text(QStringLiteral("settings.privacy.reset_selected_permission")), sitePermsCard); reset->setProperty("danger", true);
  auto *listContainer = new QWidget(sitePermsCard); auto *listLayout = new QVBoxLayout(listContainer); listLayout->setContentsMargins(18, 12, 18, 14); listLayout->setSpacing(10); listLayout->addWidget(list); listLayout->addWidget(reset, 0, Qt::AlignLeft);
  addRow(sitePermsCard, listContainer);
  section.layout->addWidget(sitePermsCard);
  const auto refresh = [this, list] {
    list->clear();
    for (const QWebEnginePermission &permission : profileService_->sitePermissions()) {
      if (!permission.isValid()) continue;
      auto *item = new QListWidgetItem(
          BrowserIcons::icon(permissionIcon(permission.permissionType())),
          QStringLiteral("%1 — %2\n%3").arg(permission.origin().host(), permissionText(permission.permissionType()), permissionState(permission)),
          list);
      item->setData(Qt::UserRole, permission.origin());
      item->setData(Qt::UserRole + 1, static_cast<int>(permission.permissionType()));
      item->setSizeHint(QSize(0, 54));
    }
    if (!list->count()) {
      auto *item = new QListWidgetItem(I18n::text(QStringLiteral("settings.privacy.no_persistent_permissions")), list);
      item->setFlags(Qt::NoItemFlags);
    }
  };
  languageRefreshers_.push_back(refresh);
  refresh();
  connect(profileService_, &BrowserProfileService::permissionsPolicyChanged, this, refresh);
  connect(reset, &QPushButton::clicked, this, [this, list, refresh] {
    auto *item = list->currentItem();
    if (!item) return;
    if (profileService_->resetSitePermission(item->data(Qt::UserRole).toUrl(), static_cast<QWebEnginePermission::PermissionType>(item->data(Qt::UserRole + 1).toInt()))) {
      refresh();
    }
  });
#endif

  // =========================================================================
  // 5. GÜVENLİ DNS (DNS-OVER-HTTPS)
  // =========================================================================
  auto *dnsCard = makeCard(section.page, I18n::text(QStringLiteral("settings.privacy.secure_dns_card")));
  auto *dnsModeCombo = new QComboBox(dnsCard);
  dnsModeCombo->addItem(I18n::text(QStringLiteral("settings.privacy.dns_system_default")), QStringLiteral("system"));
  dnsModeCombo->addItem(I18n::text(QStringLiteral("settings.privacy.dns_automatic")), QStringLiteral("fallback"));
  dnsModeCombo->addItem(I18n::text(QStringLiteral("settings.privacy.dns_always_secure")), QStringLiteral("secure"));

  const QString curMode = profileService_->secureDnsMode();
  int dIdx = dnsModeCombo->findData(curMode);
  if (dIdx >= 0) dnsModeCombo->setCurrentIndex(dIdx);

  addRow(dnsCard, settingRow(
      dnsCard,
      I18n::text(QStringLiteral("settings.privacy.dns_mode")),
      I18n::text(QStringLiteral("settings.privacy.dns_mode_desc")),
      dnsModeCombo,
      BrowserIcon::Privacy));

  auto *dnsProviderCombo = new QComboBox(dnsCard);
  dnsProviderCombo->addItem(QStringLiteral("Cloudflare (1.1.1.1)"), QStringLiteral("https://cloudflare-dns.com/dns-query"));
  dnsProviderCombo->addItem(QStringLiteral("Google Public DNS (8.8.8.8)"), QStringLiteral("https://dns.google/dns-query"));
  dnsProviderCombo->addItem(QStringLiteral("Quad9 (9.9.9.9)"), QStringLiteral("https://dns.quad9.net/dns-query"));
  dnsProviderCombo->addItem(I18n::text(QStringLiteral("settings.privacy.dns_custom_provider")), QStringLiteral("custom"));

  const QString curTpl = profileService_->secureDnsTemplate();
  int pIdx = dnsProviderCombo->findData(curTpl);
  if (pIdx >= 0) {
    dnsProviderCombo->setCurrentIndex(pIdx);
  } else {
    dnsProviderCombo->setCurrentIndex(3);
  }

  auto *customDnsEdit = new QLineEdit(dnsCard);
  customDnsEdit->setPlaceholderText(QStringLiteral("https://example.com/dns-query"));
  customDnsEdit->setText(curTpl);
  customDnsEdit->setVisible(dnsProviderCombo->currentIndex() == 3);

  addRow(dnsCard, settingRow(
      dnsCard,
      I18n::text(QStringLiteral("settings.privacy.dns_provider")),
      I18n::text(QStringLiteral("settings.privacy.dns_provider_desc")),
      dnsProviderCombo,
      BrowserIcon::Privacy));

  addRow(dnsCard, settingRow(
      dnsCard,
      I18n::text(QStringLiteral("settings.privacy.dns_custom_template")),
      I18n::text(QStringLiteral("settings.privacy.dns_custom_template_desc")),
      customDnsEdit,
      BrowserIcon::Privacy));

  connect(dnsModeCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, dnsModeCombo] {
    profileService_->setSecureDnsMode(dnsModeCombo->currentData().toString());
  });

  connect(dnsProviderCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, dnsProviderCombo, customDnsEdit] {
    const QString data = dnsProviderCombo->currentData().toString();
    if (data == QLatin1String("custom")) {
      customDnsEdit->setVisible(true);
      profileService_->setSecureDnsTemplate(customDnsEdit->text().trimmed());
    } else {
      customDnsEdit->setVisible(false);
      customDnsEdit->setText(data);
      profileService_->setSecureDnsTemplate(data);
    }
  });

  connect(customDnsEdit, &QLineEdit::editingFinished, this, [this, customDnsEdit] {
    if (!customDnsEdit->text().trimmed().isEmpty()) {
      profileService_->setSecureDnsTemplate(customDnsEdit->text().trimmed());
    }
  });

  section.layout->addWidget(dnsCard);
  section.layout->addStretch();

  privacyStack_->addWidget(section.page);
  privacyStack_->addWidget(privacySubpage_);

  return privacyStack_;
}

QWidget *SettingsPage::createBlockerSection() {
  Section section = makeSection(QStringLiteral("DaliNira Blocker"), I18n::text(QStringLiteral("settings.blocker.description")));
  auto *blockerCard = makeCard(section.page, I18n::text(QStringLiteral("settings.blocker.card")));

  auto *openBtn = new QPushButton(I18n::text(QStringLiteral("settings.blocker.open_settings")), blockerCard);
  openBtn->setAccessibleName(I18n::text(QStringLiteral("settings.blocker.open_tab")));
  addRow(blockerCard, settingRow(blockerCard, I18n::text(QStringLiteral("settings.blocker.filtering_rules")),
                                 I18n::text(QStringLiteral("settings.blocker.filtering_rules_desc")),
                                 openBtn, BrowserIcon::Privacy, true));

  if (profileService_ && profileService_->blockerService()) {
    auto *blockerSvc = profileService_->blockerService();
    auto *protectionCheck = new QCheckBox(blockerCard);
    protectionCheck->setObjectName(QStringLiteral("settings-blocker-protection"));
    protectionCheck->setChecked(blockerSvc->settings()->protectionEnabled());
    protectionCheck->setAccessibleName(I18n::text(QStringLiteral("settings.blocker.protection")));
    addRow(blockerCard, settingRow(blockerCard,
                                   I18n::text(QStringLiteral("settings.blocker.protection")),
                                   I18n::text(QStringLiteral("settings.blocker.protection_desc")),
                                   protectionCheck, BrowserIcon::Privacy, true));

    auto *showCountCheck = new QCheckBox(blockerCard);
    showCountCheck->setChecked(blockerSvc->settings()->showBlockedCountOnToolbar());
    addRow(blockerCard, settingRow(blockerCard, I18n::text(QStringLiteral("settings.blocker.toolbar_counter")),
                                   I18n::text(QStringLiteral("settings.blocker.toolbar_counter_desc")),
                                   showCountCheck, BrowserIcon::Privacy));

    connect(protectionCheck, &QCheckBox::toggled, this, [blockerSvc](bool checked) {
      blockerSvc->settings()->setProtectionEnabled(checked);
    });
    connect(blockerSvc->settings(), &DaliNiraBlockerSettings::protectionEnabledChanged,
            protectionCheck, [protectionCheck](bool enabled) {
      const QSignalBlocker blocker(protectionCheck);
      protectionCheck->setChecked(enabled);
    });
    connect(showCountCheck, &QCheckBox::toggled, this, [blockerSvc](bool checked) {
      blockerSvc->settings()->setShowBlockedCountOnToolbar(checked);
    });
  }

  section.layout->addWidget(blockerCard);
  section.layout->addStretch();

  connect(openBtn, &QPushButton::clicked, this, [this] {
    emit navigateRequested(QUrl(QStringLiteral("dalinira://blocker")));
  });

  return section.page;
}
