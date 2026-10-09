#include "dalinira_blocker_page.h"
#include "glow_toggle_switch.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QVBoxLayout>
#include <QtMath>

#include "dalinira_blocker_service.h"
#include "i18n/i18n.h"

namespace {
QFrame *createRankedMetricRow(int rank, const QString &name, quint64 count,
                              const QString &unit, const QString &accent,
                              QWidget *parent) {
  auto *row = new QFrame(parent);
  row->setObjectName(QStringLiteral("ranked-metric-row"));
  row->setFixedHeight(46);
  row->setStyleSheet(QStringLiteral(
      "QFrame#ranked-metric-row { background: #121923; border: 1px solid #2a3747; border-radius: 9px; }"
      "QFrame#ranked-metric-row:hover { background: #17212d; border-color: %1; }").arg(accent));
  auto *layout = new QHBoxLayout(row);
  layout->setContentsMargins(10, 6, 10, 6);
  layout->setSpacing(10);

  auto *rankLabel = new QLabel(QString::number(rank), row);
  rankLabel->setFixedSize(28, 28);
  rankLabel->setAlignment(Qt::AlignCenter);
  rankLabel->setStyleSheet(QStringLiteral(
      "background: %1; color: #071117; border: 0; border-radius: 14px; font-size: 12px; font-weight: 800;")
      .arg(accent));

  auto *nameLabel = new QLabel(name, row);
  nameLabel->setStyleSheet(QStringLiteral(
      "color: #e4ebf5; border: 0; font-size: 13px; font-weight: 600;"));
  nameLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

  auto *countLabel = new QLabel(QStringLiteral("%1 %2").arg(count).arg(unit), row);
  countLabel->setAlignment(Qt::AlignCenter);
  countLabel->setMinimumWidth(96);
  countLabel->setStyleSheet(QStringLiteral(
      "background: #0d141c; color: %1; border: 1px solid %1; border-radius: 12px; "
      "padding: 4px 10px; font-size: 12px; font-weight: 700;").arg(accent));

  layout->addWidget(rankLabel);
  layout->addWidget(nameLabel, 1);
  layout->addWidget(countLabel);
  return row;
}
}  // namespace

// ---------------- ModeKnobWidget Implementation ----------------

ModeKnobWidget::ModeKnobWidget(int level, int value, const QString &label, QWidget *parent)
    : QWidget(parent), level_(level), value_(value), label_(label) {
  setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
  setAttribute(Qt::WA_Hover, true);
}

void ModeKnobWidget::setValue(int val) {
  value_ = qBound(0, val, 100);
  update();
}

void ModeKnobWidget::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing, true);

  const int w = width();
  const int h = height();
  const qreal cx = w / 2.0;
  const qreal cy = 52.0;
  const qreal radius = 42.0;
  const qreal innerRadius = radius - 9.0;
  const qreal arcRadius = radius - 5.0;

  QColor accent;
  QColor glowColor;
  if (level_ == 1) {
    accent = QColor(0x53, 0xd2, 0xff);
    glowColor = QColor(0x53, 0xd2, 0xff, 120);
  } else if (level_ == 3) {
    accent = QColor(0x5c, 0xf2, 0xc4);
    glowColor = QColor(0x5c, 0xf2, 0xc4, 120);
  } else {
    accent = QColor(0x86, 0xa7, 0xff);
    glowColor = QColor(0x86, 0xa7, 0xff, 120);
  }

  // Outer circle background
  p.setPen(Qt::NoPen);
  p.setBrush(QColor(0x2a, 0x2a, 0x2a));
  p.drawEllipse(QPointF(cx, cy), radius, radius);

  // Inner circle background
  p.setBrush(QColor(0x1a, 0x1d, 0x22));
  p.drawEllipse(QPointF(cx, cy), innerRadius, innerRadius);

  // Segment arc
  const int segments = 50;
  const int activeSegments = qRound((value_ / 100.0) * segments);
  const qreal startDeg = 135.0;
  const qreal spanDeg = 270.0;
  const qreal segDeg = spanDeg / segments;

  QPen pen;
  pen.setWidthF(5.0);
  pen.setCapStyle(Qt::RoundCap);

  for (int i = 0; i < segments; ++i) {
    const qreal angle = startDeg + (i * segDeg);
    const qreal rad = qDegreesToRadians(angle);
    const qreal x = cx + arcRadius * qCos(rad);
    const qreal y = cy + arcRadius * qSin(rad);

    if (i < activeSegments) {
      pen.setColor(accent);
      p.setPen(pen);
      p.drawPoint(QPointF(x, y));
    } else {
      pen.setColor(QColor(0x35, 0x3d, 0x47));
      p.setPen(pen);
      p.drawPoint(QPointF(x, y));
    }
  }

  // Active Dot Glow & Indicator
  if (activeSegments > 0) {
    const qreal dotAngle = startDeg + ((activeSegments - 1) * segDeg);
    const qreal dotRad = qDegreesToRadians(dotAngle);
    const qreal dotX = cx + arcRadius * qCos(dotRad);
    const qreal dotY = cy + arcRadius * qSin(dotRad);

    QRadialGradient glow(QPointF(dotX, dotY), 12.0);
    glow.setColorAt(0, glowColor);
    glow.setColorAt(1, Qt::transparent);
    p.setPen(Qt::NoPen);
    p.setBrush(glow);
    p.drawEllipse(QPointF(dotX, dotY), 12.0, 12.0);

    p.setBrush(accent);
    p.drawEllipse(QPointF(dotX, dotY), 4.0, 4.0);
  }

  // Center axle
  p.setPen(Qt::NoPen);
  p.setBrush(QColor(0x4a, 0x55, 0x68));
  p.drawEllipse(QPointF(cx, cy), 5.0, 5.0);

  // Value text
  p.setPen(QColor(0xf0, 0xf4, 0xf8));
  QFont font = p.font();
  font.setPixelSize(14);
  font.setBold(true);
  p.setFont(font);
  p.drawText(QRectF(0, h - 36, w, 18), Qt::AlignCenter, QStringLiteral("%1%").arg(value_));

  // Label text
  p.setPen(QColor(0x9a, 0xa6, 0xb8));
  font.setPixelSize(10);
  font.setBold(false);
  p.setFont(font);
  p.drawText(QRectF(0, h - 18, w, 16), Qt::AlignCenter, label_.toUpper());
}

// ---------------- DaliNiraBlockerPage Implementation ----------------

DaliNiraBlockerPage::DaliNiraBlockerPage(DaliNiraBlockerService *service, QWidget *parent)
    : QWidget(parent), service_(service) {
  setObjectName(QStringLiteral("adblock-page"));
  setStyleSheet(QStringLiteral(
      "QWidget#adblock-page { background: #12161c; color: #e4ebf5; font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, Ubuntu, sans-serif; }"
      "QListWidget#adblock-sidebar { background: #181f28; border: 0; border-right: 1px solid #283442; color: #b8c4d4; font-size: 13px; font-weight: 500; outline: 0; padding: 12px 6px; min-width: 170px; max-width: 190px; }"
      "QListWidget#adblock-sidebar::item { min-height: 38px; padding-left: 14px; border-radius: 8px; margin-bottom: 4px; }"
      "QListWidget#adblock-sidebar::item:hover { background: #222b38; color: #ffffff; }"
      "QListWidget#adblock-sidebar::item:selected { background: #2f3d4f; color: #4ec9ff; font-weight: 600; }"
      "QGroupBox { font-size: 14px; font-weight: 600; color: #e4ebf5; border: 1px solid #283442; border-radius: 10px; margin-top: 14px; padding-top: 18px; background: #181f28; }"
      "QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top left; left: 14px; padding: 0 6px; background: transparent; }"
      "QPushButton { background: #253140; color: #e4ebf5; border: 1px solid #364659; border-radius: 6px; padding: 6px 14px; font-size: 12px; font-weight: 500; min-height: 18px; }"
      "QPushButton:hover { background: #324357; border-color: #4b617a; color: #ffffff; }"
      "QPushButton:pressed { background: #1c2633; }"
      "QPushButton:disabled { background: #1a222c; color: #5a6878; border-color: #242f3d; }"
      "QPushButton#primary-btn { background: #2160c4; border-color: #2c75e8; color: #ffffff; }"
      "QPushButton#primary-btn:hover { background: #2872e4; }"
      "QLineEdit, QPlainTextEdit, QTextEdit { background: #10141a; border: 1px solid #2b394a; border-radius: 6px; color: #e4ebf5; padding: 7px 10px; font-size: 13px; selection-background-color: #2c75e8; }"
      "QLineEdit:focus, QPlainTextEdit:focus { border-color: #4ec9ff; }"
      "QCheckBox { color: #d0dbe8; font-size: 13px; spacing: 8px; }"
      "QCheckBox::indicator { width: 18px; height: 18px; border: 1px solid #3b4c61; border-radius: 4px; background: #10141a; }"
      "QCheckBox::indicator:checked { background: #2160c4; border-color: #4ec9ff; }"
      "QComboBox { background: #181f28; border: 1px solid #2b394a; border-radius: 6px; color: #e4ebf5; padding: 5px 10px; min-height: 20px; font-size: 12px; }"
      "QComboBox QAbstractItemView { background: #181f28; border: 1px solid #2b394a; color: #e4ebf5; selection-background-color: #2f3d4f; }"
      "QScrollArea { border: 0; background: transparent; }"
      "QScrollBar:vertical { background: #12161c; width: 10px; margin: 0; }"
      "QScrollBar::handle:vertical { background: #2c3a4d; border-radius: 5px; min-height: 20px; }"
      "QScrollBar::handle:vertical:hover { background: #3d4f66; }"
  ));

  auto *mainLayout = new QHBoxLayout(this);
  mainLayout->setContentsMargins(0, 0, 0, 0);
  mainLayout->setSpacing(0);

  sidebar_ = new QListWidget(this);
  sidebar_->setObjectName(QStringLiteral("adblock-sidebar"));

  auto addTabItem = [this](Tab tab, const QString &text) {
    sidebar_->addItem(text);
    tabIndices_[tab] = sidebar_->count() - 1;
  };

  addTabItem(Tab::Settings, I18n::text(QStringLiteral("menu.settings")));
  addTabItem(Tab::Rulesets, I18n::text(QStringLiteral("blocker.tab_filter_lists")));
  addTabItem(Tab::CustomFilters, I18n::text(QStringLiteral("blocker.tab_custom_filters")));
  addTabItem(Tab::Sites, I18n::text(QStringLiteral("blocker.tab_sites")));
  addTabItem(Tab::Statistics, I18n::text(QStringLiteral("blocker.tab_statistics")));
  addTabItem(Tab::Logger, I18n::text(QStringLiteral("blocker.tab_logger")));
  addTabItem(Tab::Develop, I18n::text(QStringLiteral("blocker.tab_develop")));
  addTabItem(Tab::About, I18n::text(QStringLiteral("common.about")));

  mainLayout->addWidget(sidebar_);

  stack_ = new QStackedWidget(this);
  mainLayout->addWidget(stack_, 1);

  createSettingsTab();
  createRulesetsTab();
  createCustomFiltersTab();
  createSitesTab();
  createStatisticsTab();
  createLoggerTab();
  createDevelopTab();
  createAboutTab();

  connect(sidebar_, &QListWidget::currentRowChanged, this, [this](int index) {
    stack_->setCurrentIndex(index);
    if (index == tabIndices_[Tab::Statistics]) refreshStatisticsTab();
    else if (index == tabIndices_[Tab::Logger]) refreshLoggerTab();
    else if (index == tabIndices_[Tab::Develop]) refreshDevelopTab();
    else if (index == tabIndices_[Tab::Rulesets]) refreshRulesetTab();
    else if (index == tabIndices_[Tab::Sites]) refreshSitesTab();
    else if (index == tabIndices_[Tab::CustomFilters]) refreshCustomFiltersTab();
  });
  sidebar_->setCurrentRow(0);

  // Only live telemetry needs polling. Rebuilding every tab's widget tree every
  // 1.5 seconds caused avoidable allocations and visible stalls while editing.
  connect(&refreshTimer_, &QTimer::timeout, this, [this]() {
    const int index = stack_->currentIndex();
    if (index == tabIndices_[Tab::Statistics]) refreshStatisticsTab();
    else if (index == tabIndices_[Tab::Logger]) refreshLoggerTab();
    else if (index == tabIndices_[Tab::Develop]) refreshDevelopTab();
  });
  refreshTimer_.setInterval(1500);

  if (service_) {
    connect(service_->settings(), &DaliNiraBlockerSettings::settingsChanged, this, &DaliNiraBlockerPage::updateModeUi);
  }
  // Provider, source and generated-format rows combine translated templates
  // with third-party metadata. Rebuild just this catalog on a live language
  // switch because the generic widget retranslator cannot reverse-map a
  // composed string back to its catalog key.
  connect(&dalinira::i18n::LanguageManager::instance(),
          &dalinira::i18n::LanguageManager::languageChanged,
          this, [this] { refreshRulesetTab(); });

  updateModeUi();
}

void DaliNiraBlockerPage::showEvent(QShowEvent *) {
  refreshAll();
  refreshTimer_.start();
}

void DaliNiraBlockerPage::hideEvent(QHideEvent *) {
  refreshTimer_.stop();
}

void DaliNiraBlockerPage::setActiveTab(Tab tab) {
  if (tabIndices_.contains(tab)) {
    sidebar_->setCurrentRow(tabIndices_[tab]);
  }
}

void DaliNiraBlockerPage::setActiveHost(const QString &host) {
  activeHost_ = host.trimmed().toLower();
  if (siteHostInput_ && !activeHost_.isEmpty()) {
    siteHostInput_->setText(activeHost_);
  }
  if (aboutActiveHostLabel_) {
    aboutActiveHostLabel_->setText(activeHost_.isEmpty() ? I18n::text(QStringLiteral("blocker.no_active_web_tab")) : activeHost_);
  }
  refreshSitesTab();
}

void DaliNiraBlockerPage::refreshAll() {
  updateModeUi();
  refreshRulesetTab();
  refreshCustomFiltersTab();
  refreshSitesTab();
  refreshStatisticsTab();
  refreshLoggerTab();
  refreshDevelopTab();
}

// ---------------- 1. Settings Tab ----------------

void DaliNiraBlockerPage::createSettingsTab() {
  auto *scroll = new QScrollArea(this);
  scroll->setWidgetResizable(true);
  auto *container = new QWidget(scroll);
  auto *layout = new QVBoxLayout(container);
  layout->setContentsMargins(28, 24, 28, 24);
  layout->setSpacing(20);

  // Header & Mode Pill
  auto *heroGroup = new QGroupBox(I18n::text(QStringLiteral("blocker.default_mode")), container);
  auto *heroLayout = new QVBoxLayout(heroGroup);
  heroLayout->setContentsMargins(18, 18, 18, 18);
  heroLayout->setSpacing(14);

  auto *heroTop = new QHBoxLayout;
  modeDesc_ = new QLabel(I18n::text(QStringLiteral("blocker.default_mode_desc")), heroGroup);
  modeDesc_->setWordWrap(true);
  modeDesc_->setStyleSheet(QStringLiteral("color: #9aa7b8; font-size: 13px;"));
  heroTop->addWidget(modeDesc_, 1);

  modePill_ = new QLabel(I18n::text(QStringLiteral("blocker.mode_optimal")), heroGroup);
  modePill_->setStyleSheet(QStringLiteral("background: #1f3b5c; color: #53d2ff; font-weight: 700; font-size: 12px; border-radius: 12px; padding: 4px 14px; border: 1px solid #2e598a;"));
  heroTop->addWidget(modePill_, 0, Qt::AlignTop);
  heroLayout->addLayout(heroTop);

  // 3 Mode Cards
  auto *cardsLayout = new QHBoxLayout;
  cardsLayout->setSpacing(14);

  auto createModeCard = [this](int level, int value, const QString &title, const QString &knobLabel, const QString &desc, QWidget **outCard) {
    auto *card = new QFrame;
    card->setObjectName(QStringLiteral("mode-card"));
    card->setCursor(Qt::PointingHandCursor);
    card->setStyleSheet(QStringLiteral(
        "QFrame#mode-card { background: #121720; border: 1.5px solid #283545; border-radius: 12px; padding: 12px; }"
        "QFrame#mode-card:hover { background: #161e2a; border-color: #4a617d; }"
    ));
    auto *cLayout = new QVBoxLayout(card);
    cLayout->setContentsMargins(8, 8, 8, 8);
    cLayout->setSpacing(8);

    auto *headerLayout = new QHBoxLayout;
    auto *radioDot = new QLabel(QStringLiteral("○"), card);
    radioDot->setObjectName(QStringLiteral("radio-dot"));
    radioDot->setStyleSheet(QStringLiteral("color: #72849a; font-size: 16px; font-weight: bold;"));
    auto *titleLabel = new QLabel(title, card);
    titleLabel->setStyleSheet(QStringLiteral("color: #ffffff; font-size: 15px; font-weight: 700;"));
    headerLayout->addWidget(radioDot);
    headerLayout->addWidget(titleLabel);
    headerLayout->addStretch(1);
    cLayout->addLayout(headerLayout);

    auto *knob = new ModeKnobWidget(level, value, knobLabel, card);
    cLayout->addWidget(knob, 0, Qt::AlignCenter);

    auto *descLabel = new QLabel(desc, card);
    descLabel->setWordWrap(true);
    descLabel->setStyleSheet(QStringLiteral("color: #8c9bae; font-size: 12px; min-height: 48px;"));
    cLayout->addWidget(descLabel);

    card->installEventFilter(this);
    radioDot->installEventFilter(this);
    titleLabel->installEventFilter(this);
    knob->installEventFilter(this);
    descLabel->installEventFilter(this);
    *outCard = card;
    return card;
  };

  cardsLayout->addWidget(createModeCard(
      1, 35, I18n::text(QStringLiteral("blocker.mode_basic")), I18n::text(QStringLiteral("blocker.mode_light")),
      I18n::text(QStringLiteral("blocker.mode_basic_desc")),
      &basicCard_));

  cardsLayout->addWidget(createModeCard(
      2, 65, I18n::text(QStringLiteral("blocker.mode_optimal")), I18n::text(QStringLiteral("common.balanced")),
      I18n::text(QStringLiteral("blocker.mode_optimal_desc")),
      &idealCard_));

  cardsLayout->addWidget(createModeCard(
      3, 95, I18n::text(QStringLiteral("blocker.mode_complete")), I18n::text(QStringLiteral("blocker.mode_strong")),
      I18n::text(QStringLiteral("blocker.mode_complete_desc")),
      &aggressiveCard_));

  heroLayout->addLayout(cardsLayout);
  layout->addWidget(heroGroup);

  // Click handling for mode cards
  basicCard_->setMouseTracking(true);
  idealCard_->setMouseTracking(true);
  aggressiveCard_->setMouseTracking(true);

  // Behavior Section
  auto *behaviorGroup = new QGroupBox(I18n::text(QStringLiteral("blocker.behavior")), container);
  auto *bLayout = new QVBoxLayout(behaviorGroup);
  bLayout->setContentsMargins(18, 18, 18, 18);
  bLayout->setSpacing(12);

  autoReloadCheck_ = new GlowToggleSwitch(I18n::text(QStringLiteral("blocker.auto_reload_on_mode_change")), behaviorGroup);
  showCountCheck_ = new GlowToggleSwitch(I18n::text(QStringLiteral("blocker.show_count_on_icon")), behaviorGroup);
  strictBlockCheck_ = new GlowToggleSwitch(I18n::text(QStringLiteral("blocker.strict_block")), behaviorGroup);
  popupBlockCheck_ = new GlowToggleSwitch(I18n::text(QStringLiteral("blocker.popup_block")), behaviorGroup);
  developerModeCheck_ = new GlowToggleSwitch(I18n::text(QStringLiteral("blocker.developer_mode")), behaviorGroup);

  bLayout->addWidget(autoReloadCheck_);
  bLayout->addWidget(showCountCheck_);
  bLayout->addWidget(strictBlockCheck_);
  bLayout->addWidget(popupBlockCheck_);
  bLayout->addWidget(developerModeCheck_);
  layout->addWidget(behaviorGroup);

  connect(autoReloadCheck_, &QCheckBox::toggled, this, [this](bool checked) {
    if (service_) service_->settings()->setAutoReloadOnModeChange(checked);
  });
  connect(showCountCheck_, &QCheckBox::toggled, this, [this](bool checked) {
    if (service_) service_->settings()->setShowBlockedCountOnToolbar(checked);
  });
  connect(strictBlockCheck_, &QCheckBox::toggled, this, [this](bool checked) {
    if (service_) service_->settings()->setStrictBlock(checked);
  });
  connect(popupBlockCheck_, &QCheckBox::toggled, this, [this](bool checked) {
    if (service_) service_->settings()->setPopupBlock(checked);
  });
  connect(developerModeCheck_, &QCheckBox::toggled, this, [this](bool checked) {
    if (service_) service_->settings()->setDeveloperMode(checked);
  });

  // Backup & Restore Section
  auto *backupGroup = new QGroupBox(I18n::text(QStringLiteral("blocker.backup_group")), container);
  auto *bkLayout = new QVBoxLayout(backupGroup);
  bkLayout->setContentsMargins(18, 18, 18, 18);
  bkLayout->setSpacing(12);

  auto *bkDesc = new QLabel(I18n::text(QStringLiteral("blocker.backup_desc")), backupGroup);
  bkDesc->setStyleSheet(QStringLiteral("color: #9aa7b8; font-size: 13px;"));
  bkLayout->addWidget(bkDesc);

  auto *bkBtns = new QHBoxLayout;
  auto *backupBtn = new QPushButton(I18n::text(QStringLiteral("blocker.backup_button")), backupGroup);
  auto *restoreBtn = new QPushButton(I18n::text(QStringLiteral("blocker.restore_button")), backupGroup);
  auto *resetBtn = new QPushButton(I18n::text(QStringLiteral("blocker.reset_defaults_button")), backupGroup);

  bkBtns->addWidget(backupBtn);
  bkBtns->addWidget(restoreBtn);
  bkBtns->addWidget(resetBtn);
  bkBtns->addStretch(1);
  bkLayout->addLayout(bkBtns);
  layout->addWidget(backupGroup);

  connect(backupBtn, &QPushButton::clicked, this, [this]() {
    if (!service_) return;
    const QString path = QFileDialog::getSaveFileName(this, I18n::text(QStringLiteral("blocker.backup_title")), QStringLiteral("deliblock-settings.json"), I18n::text(QStringLiteral("blocker.json_filter")));
    if (path.isEmpty()) return;
    QSaveFile file(path);
    const QByteArray bytes = QJsonDocument(service_->settings()->exportBackupJson()).toJson(QJsonDocument::Indented);
    if (file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit()) {
      QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
      QMessageBox::information(this, I18n::text(QStringLiteral("blocker.backup_success_title")), I18n::text(QStringLiteral("blocker.backup_success")));
    }
  });

  connect(restoreBtn, &QPushButton::clicked, this, [this]() {
    if (!service_) return;
    const QString path = QFileDialog::getOpenFileName(this, I18n::text(QStringLiteral("blocker.restore_title")), QString(), I18n::text(QStringLiteral("blocker.json_filter")));
    if (path.isEmpty()) return;
    QFile file(path);
    if (file.open(QIODevice::ReadOnly) && file.size() <= 8 * 1024 * 1024) {
      QJsonParseError err;
      const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &err);
      if (err.error == QJsonParseError::NoError && doc.isObject()) {
        if (service_->settings()->importBackupJson(doc.object())) {
          updateModeUi();
          QMessageBox::information(this, I18n::text(QStringLiteral("blocker.restore_success_title")), I18n::text(QStringLiteral("blocker.restore_success")));
          return;
        }
      }
      QMessageBox::warning(this, I18n::text(QStringLiteral("common.error")), I18n::text(QStringLiteral("blocker.invalid_backup")));
    }
  });

  connect(resetBtn, &QPushButton::clicked, this, [this]() {
    if (!service_) return;
    if (QMessageBox::question(this, I18n::text(QStringLiteral("blocker.reset_confirm_title")), I18n::text(QStringLiteral("blocker.reset_confirm"))) == QMessageBox::Yes) {
      service_->settings()->resetToDefaults();
      refreshAll();
      QMessageBox::information(this, I18n::text(QStringLiteral("blocker.reset_done_title")), I18n::text(QStringLiteral("blocker.reset_done")));
    }
  });

  layout->addStretch(1);
  scroll->setWidget(container);
  stack_->addWidget(scroll);
}

void DaliNiraBlockerPage::updateModeUi() {
  if (!service_) return;
  const DaliNiraBlockerMode mode = service_->settings()->mode();

  auto setCardActive = [](QWidget *card, bool active, const QString &borderCol) {
    if (!card) return;
    auto *dot = card->findChild<QLabel *>(QStringLiteral("radio-dot"));
    if (dot) {
      dot->setText(active ? QStringLiteral("●") : QStringLiteral("○"));
      dot->setStyleSheet(active ? QStringLiteral("color: %1; font-size: 16px; font-weight: bold;").arg(borderCol)
                                : QStringLiteral("color: #72849a; font-size: 16px; font-weight: bold;"));
    }
    card->setStyleSheet(QStringLiteral(
        "QFrame#mode-card { background: %1; border: 2px solid %2; border-radius: 12px; padding: 12px; }"
    ).arg(active ? QStringLiteral("#182333") : QStringLiteral("#121720"),
          active ? borderCol : QStringLiteral("#283545")));
  };

  setCardActive(basicCard_, mode == DaliNiraBlockerMode::Basic, QStringLiteral("#53d2ff"));
  setCardActive(idealCard_, mode == DaliNiraBlockerMode::Ideal, QStringLiteral("#86a7ff"));
  setCardActive(aggressiveCard_, mode == DaliNiraBlockerMode::Aggressive, QStringLiteral("#5cf2c4"));

  if (modePill_) {
    if (mode == DaliNiraBlockerMode::Basic) {
      modePill_->setText(I18n::text(QStringLiteral("blocker.mode_basic")));
      modePill_->setStyleSheet(QStringLiteral("background: #143542; color: #53d2ff; font-weight: 700; font-size: 12px; border-radius: 12px; padding: 4px 14px; border: 1px solid #20576e;"));
    } else if (mode == DaliNiraBlockerMode::Aggressive) {
      modePill_->setText(I18n::text(QStringLiteral("blocker.mode_complete")));
      modePill_->setStyleSheet(QStringLiteral("background: #11382e; color: #5cf2c4; font-weight: 700; font-size: 12px; border-radius: 12px; padding: 4px 14px; border: 1px solid #1c614f;"));
    } else {
      modePill_->setText(I18n::text(QStringLiteral("blocker.mode_optimal")));
      modePill_->setStyleSheet(QStringLiteral("background: #1f3b5c; color: #86a7ff; font-weight: 700; font-size: 12px; border-radius: 12px; padding: 4px 14px; border: 1px solid #2e598a;"));
    }
  }

  if (autoReloadCheck_) autoReloadCheck_->setChecked(service_->settings()->autoReloadOnModeChange());
  if (showCountCheck_) showCountCheck_->setChecked(service_->settings()->showBlockedCountOnToolbar());
  if (strictBlockCheck_) strictBlockCheck_->setChecked(service_->settings()->strictBlock());
  if (popupBlockCheck_) popupBlockCheck_->setChecked(service_->settings()->popupBlock());
  if (developerModeCheck_) developerModeCheck_->setChecked(service_->settings()->developerMode());
}

// ---------------- 2. Rulesets Tab ----------------

void DaliNiraBlockerPage::createRulesetsTab() {
  auto *scroll = new QScrollArea(this);
  scroll->setWidgetResizable(true);
  auto *container = new QWidget(scroll);
  auto *layout = new QVBoxLayout(container);
  layout->setContentsMargins(28, 24, 28, 24);
  layout->setSpacing(24);

  // Summary Row
  auto *summaryBox = new QFrame(container);
  summaryBox->setStyleSheet(QStringLiteral("background: #181f28; border: 1px solid #283442; border-radius: 10px; padding: 12px;"));
  auto *sLayout = new QHBoxLayout(summaryBox);
  sLayout->setSpacing(16);

  auto addStatTile = [sLayout, summaryBox](const QString &title, QLabel **outVal) {
    auto *tile = new QVBoxLayout;
    auto *tLabel = new QLabel(title, summaryBox);
    tLabel->setStyleSheet(QStringLiteral("color: #8c9ba8; font-size: 11px; font-weight: 600; text-transform: uppercase;"));
    auto *vLabel = new QLabel(QStringLiteral("0"), summaryBox);
    vLabel->setStyleSheet(QStringLiteral("color: #4ec9ff; font-size: 20px; font-weight: 700;"));
    tile->addWidget(tLabel);
    tile->addWidget(vLabel);
    sLayout->addLayout(tile);
    *outVal = vLabel;
  };

  addStatTile(I18n::text(QStringLiteral("blocker.stat_this_session")), &rulesetSessionCount_);
  addStatTile(I18n::text(QStringLiteral("blocker.stat_total")), &rulesetTotalCount_);
  addStatTile(I18n::text(QStringLiteral("blocker.stat_rulesets")), &rulesetCount_);
  addStatTile(I18n::text(QStringLiteral("blocker.stat_domain_rules")), &rulesetDomainRuleCount_);
  addStatTile(I18n::text(QStringLiteral("blocker.stat_dnr_rules")), &rulesetDnrRuleCount_);

  layout->addWidget(summaryBox);

  // Breakdown Section
  auto *breakdownGroup = new QFrame(container);
  breakdownGroup->setObjectName(QStringLiteral("ruleset-breakdown-group"));
  breakdownGroup->setStyleSheet(QStringLiteral(
      "QFrame#ruleset-breakdown-group { background: #181f28; border: 1px solid #283442; border-radius: 10px; }"));
  auto *breakdownSectionLayout = new QVBoxLayout(breakdownGroup);
  breakdownSectionLayout->setContentsMargins(16, 14, 16, 16);
  breakdownSectionLayout->setSpacing(12);
  auto *breakdownTitle = new QLabel(I18n::text(QStringLiteral("blocker.top_filter_lists")), breakdownGroup);
  breakdownTitle->setStyleSheet(QStringLiteral(
      "color: #e4ebf5; border: 0; font-size: 14px; font-weight: 700;"));
  breakdownSectionLayout->addWidget(breakdownTitle);
  rulesetBreakdownLayout_ = new QVBoxLayout;
  rulesetBreakdownLayout_->setContentsMargins(0, 0, 0, 0);
  rulesetBreakdownLayout_->setSpacing(12);
  breakdownSectionLayout->addLayout(rulesetBreakdownLayout_);
  layout->addWidget(breakdownGroup);

  // Catalog Section
  auto *catalogGroup = new QGroupBox(
      I18n::text(QStringLiteral("blocker.third_party_filter_lists")), container);
  catalogGroup->setObjectName(QStringLiteral("ruleset-catalog-group"));
  auto *cMainLayout = new QVBoxLayout(catalogGroup);
  cMainLayout->setContentsMargins(16, 28, 16, 16);
  cMainLayout->setSpacing(12);

  auto *provenanceNotice = new QLabel(
      I18n::text(QStringLiteral("blocker.third_party_notice")), catalogGroup);
  provenanceNotice->setObjectName(QStringLiteral("ruleset-native-engine-notice"));
  provenanceNotice->setWordWrap(true);
  provenanceNotice->setStyleSheet(QStringLiteral(
      "background: #10243a; border: 1px solid #24517b; border-radius: 7px; "
      "color: #b9ddff; font-size: 12px; padding: 10px;"));
  cMainLayout->addWidget(provenanceNotice);

  auto *actionRow = new QHBoxLayout;
  auto *updateBtn = new QPushButton(I18n::text(QStringLiteral("blocker.update_filters")), catalogGroup);
  updateBtn->setObjectName(QStringLiteral("ruleset-update-filters-button"));
  updateBtn->setToolTip(I18n::text(QStringLiteral("blocker.update_filters_desc")));
  rulesetUpdateStatus_ = new QLabel(I18n::text(QStringLiteral("blocker.local_filters_ready")), catalogGroup);
  rulesetUpdateStatus_->setStyleSheet(QStringLiteral("color: #72849a; font-size: 12px;"));
  rulesetUpdateProgress_ = new QProgressBar(catalogGroup);
  rulesetUpdateProgress_->setRange(0, 1);
  rulesetUpdateProgress_->setValue(0);
  rulesetUpdateProgress_->setTextVisible(true);
  rulesetUpdateProgress_->setFixedWidth(140);
  rulesetUpdateProgress_->hide();

  actionRow->addWidget(updateBtn);
  actionRow->addWidget(rulesetUpdateStatus_);
  actionRow->addWidget(rulesetUpdateProgress_);
  actionRow->addStretch(1);
  cMainLayout->addLayout(actionRow);

  auto *updateHint = new QLabel(
      I18n::text(QStringLiteral("blocker.update_notice")),
      catalogGroup);
  updateHint->setWordWrap(true);
  updateHint->setStyleSheet(QStringLiteral("color: #8c9ba8; font-size: 12px;"));
  cMainLayout->addWidget(updateHint);

  autoUpdateListsCheck_ = new GlowToggleSwitch(I18n::text(QStringLiteral("blocker.auto_update_lists")), catalogGroup);
  autoUpdateListsCheck_->setObjectName(QStringLiteral("ruleset-auto-update"));
  autoUpdateListsCheck_->setToolTip(I18n::text(QStringLiteral("blocker.auto_update_lists_desc")));
  cMainLayout->addWidget(autoUpdateListsCheck_);
  connect(autoUpdateListsCheck_, &QCheckBox::toggled, this, [this](bool checked) {
    if (service_) service_->settings()->setAutoUpdateRulesets(checked);
  });

  connect(updateBtn, &QPushButton::clicked, this, [this]() {
    if (service_) service_->updateFiltersAsync();
  });

  if (service_) {
    connect(service_, &DaliNiraBlockerService::filterUpdateStarted, this, [this, updateBtn]() {
      updateBtn->setEnabled(false);
      if (rulesetUpdateStatus_) rulesetUpdateStatus_->setText(I18n::text(QStringLiteral("blocker.updating")));
      if (rulesetUpdateProgress_) {
        rulesetUpdateProgress_->setRange(0, 3);
        rulesetUpdateProgress_->setValue(0);
        rulesetUpdateProgress_->show();
      }
    });
    connect(service_, &DaliNiraBlockerService::filterUpdateProgress, this,
            [this](int completed, int total, const QString &stage) {
              if (rulesetUpdateStatus_) {
                const QString label = stage == QLatin1String("download") ? I18n::text(QStringLiteral("blocker.stage_download"))
                    : stage == QLatin1String("parse") ? I18n::text(QStringLiteral("blocker.stage_parse"))
                    : stage == QLatin1String("cosmetic") ? I18n::text(QStringLiteral("blocker.stage_cosmetic"))
                    : stage == QLatin1String("swap") ? I18n::text(QStringLiteral("blocker.stage_swap"))
                    : I18n::text(QStringLiteral("blocker.stage_preparing"));
                rulesetUpdateStatus_->setText(QStringLiteral("%1… %2/%3").arg(label).arg(completed).arg(total));
              }
              if (rulesetUpdateProgress_) {
                rulesetUpdateProgress_->setRange(0, qMax(1, total));
                rulesetUpdateProgress_->setValue(completed);
              }
            });
    connect(service_, &DaliNiraBlockerService::filterUpdateFinished, this, [this, updateBtn](bool, const QString &msg) {
      rulesetUpdateStatus_->setText(msg + QStringLiteral(" (") + QDateTime::currentDateTime().toString(QStringLiteral("hh:mm")) + QStringLiteral(")"));
      refreshRulesetTab();
      updateBtn->setEnabled(true);
      if (rulesetUpdateProgress_) rulesetUpdateProgress_->hide();
    });
  }

  rulesetCatalogLayout_ = new QVBoxLayout;
  rulesetCatalogLayout_->setSpacing(8);
  cMainLayout->addLayout(rulesetCatalogLayout_);

  layout->addWidget(catalogGroup);
  layout->addStretch(1);
  scroll->setWidget(container);
  stack_->addWidget(scroll);
}

void DaliNiraBlockerPage::refreshRulesetTab() {
  if (!service_) return;

  if (rulesetSessionCount_) rulesetSessionCount_->setText(QString::number(service_->sessionBlockedCount()));
  if (rulesetTotalCount_) rulesetTotalCount_->setText(QString::number(service_->totalBlockedCount()));
  if (rulesetCount_) rulesetCount_->setText(QString::number(service_->listManager()->availableLists().size()));
  if (autoUpdateListsCheck_) {
    const QSignalBlocker blocker(autoUpdateListsCheck_);
    autoUpdateListsCheck_->setChecked(service_->settings()->autoUpdateRulesets());
  }
  if (rulesetDomainRuleCount_) rulesetDomainRuleCount_->setText(QString::number(service_->filterEngine()->ruleCount()));
  if (rulesetDnrRuleCount_) rulesetDnrRuleCount_->setText(QString::number(service_->filterEngine()->ruleCount()));

  // Top Ruleset Breakdown
  if (rulesetBreakdownLayout_) {
    while (rulesetBreakdownLayout_->count() > 0) {
      auto *item = rulesetBreakdownLayout_->takeAt(0);
      if (item->widget()) delete item->widget();
      delete item;
    }
    const auto top = service_->topMatchedRulesets(6);
    if (top.isEmpty()) {
      auto *empty = new QLabel(I18n::text(QStringLiteral("blocker.waiting_ruleset_matches")), this);
      empty->setStyleSheet(QStringLiteral("color: #72849a; font-style: italic;"));
      rulesetBreakdownLayout_->addWidget(empty);
    } else {
      int rank = 1;
      for (const auto &p : top) {
        rulesetBreakdownLayout_->addWidget(createRankedMetricRow(
            rank++, p.first, p.second, I18n::text(QStringLiteral("blocker.matches_unit")),
            QStringLiteral("#4ec9ff"), rulesetBreakdownLayout_->parentWidget()));
      }
    }
  }

  // Ruleset Catalog
  if (rulesetCatalogLayout_) {
    while (rulesetCatalogLayout_->count() > 0) {
      auto *item = rulesetCatalogLayout_->takeAt(0);
      if (item->widget()) delete item->widget();
      delete item;
    }
    const auto lists = service_->listManager()->availableLists();
    const QStringList enabled = service_->settings()->enabledRulesetIds();
    const bool selectionConfigured = service_->settings()->rulesetSelectionConfigured();

    for (const auto &list : lists) {
      auto *row = new QFrame(this);
      row->setStyleSheet(QStringLiteral("background: #121720; border: 1px solid #24303f; border-radius: 8px; padding: 10px;"));
      auto *rLayout = new QHBoxLayout(row);
      rLayout->setContentsMargins(8, 4, 8, 4);

      auto *chk = new GlowToggleSwitch(row);
      chk->setChecked(selectionConfigured ? enabled.contains(list.id) : list.enabled);
      chk->setToolTip(I18n::text(QStringLiteral("blocker.include_list_in_plan")));
      rLayout->addWidget(chk);

      auto *infoLayout = new QVBoxLayout;
      auto *nameLbl = new QLabel(list.name, row);
      nameLbl->setObjectName(QStringLiteral("ruleset-name-") + list.id);
      nameLbl->setStyleSheet(QStringLiteral("font-size: 13px; font-weight: 600; color: #e4ebf5;"));
      auto *metaLbl = new QLabel(I18n::text(QStringLiteral("blocker.list_meta"))
                                     .arg(list.group, QString::number(list.ruleCount), list.version,
                                          list.lastUpdated.toString(QStringLiteral("dd.MM.yyyy"))),
                                 row);
      metaLbl->setStyleSheet(QStringLiteral("font-size: 11px; color: #8c9ba8;"));
      infoLayout->addWidget(nameLbl);
      infoLayout->addWidget(metaLbl);

      if (!list.provider.isEmpty()) {
        auto *providerLbl = new QLabel(
            I18n::text(QStringLiteral("blocker.list_provider")).arg(list.provider), row);
        providerLbl->setObjectName(QStringLiteral("ruleset-provider-") + list.id);
        providerLbl->setStyleSheet(QStringLiteral("font-size: 11px; color: #9fb2c8;"));
        infoLayout->addWidget(providerLbl);
      }

      auto *provenanceRow = new QHBoxLayout;
      provenanceRow->setContentsMargins(0, 0, 0, 0);
      provenanceRow->setSpacing(10);
      if (!list.sourceUrl.isEmpty()) {
        auto *sourceLbl = new QLabel(row);
        sourceLbl->setObjectName(QStringLiteral("ruleset-source-") + list.id);
        sourceLbl->setText(QStringLiteral("<a href=\"%1\">%2</a>")
                               .arg(list.sourceUrl.toHtmlEscaped(),
                                    I18n::text(QStringLiteral("blocker.list_project_source"))
                                        .arg(list.source).toHtmlEscaped()));
        sourceLbl->setOpenExternalLinks(true);
        sourceLbl->setTextInteractionFlags(Qt::TextBrowserInteraction);
        sourceLbl->setStyleSheet(QStringLiteral("font-size: 11px; color: #66b7ff;"));
        provenanceRow->addWidget(sourceLbl);
      }
      if (!list.formatOrigin.isEmpty()) {
        auto *formatLbl = new QLabel(
            I18n::text(QStringLiteral("blocker.list_compiled_format")).arg(list.formatOrigin), row);
        formatLbl->setObjectName(QStringLiteral("ruleset-format-") + list.id);
        formatLbl->setStyleSheet(QStringLiteral("font-size: 11px; color: #8c9ba8;"));
        provenanceRow->addWidget(formatLbl);
      }
      provenanceRow->addStretch(1);
      infoLayout->addLayout(provenanceRow);

      QString updateText;
      const QString checked = list.lastUpdateCheck.toString(QStringLiteral("dd.MM.yyyy hh:mm"));
      if (list.updateUrl.isEmpty())
        updateText = I18n::text(QStringLiteral("blocker.list_update_bundled_only"));
      else if (list.lastUpdateResult == QLatin1String("failed"))
        updateText = I18n::text(list.activeSource == QLatin1String("bundled")
                                    ? QStringLiteral("blocker.list_update_failed_bundled")
                                    : QStringLiteral("blocker.list_update_failed_previous")).arg(checked);
      else if (list.activeSource != QLatin1String("bundled"))
        updateText = I18n::text(QStringLiteral("blocker.list_update_remote"))
                         .arg(list.lastUpdated.toString(QStringLiteral("dd.MM.yyyy")), checked);
      else
        updateText = I18n::text(QStringLiteral("blocker.list_update_pending"));
      auto *updateLbl = new QLabel(updateText, row);
      updateLbl->setObjectName(QStringLiteral("ruleset-update-") + list.id);
      updateLbl->setWordWrap(true);
      updateLbl->setStyleSheet(list.lastUpdateResult == QLatin1String("failed")
                                   ? QStringLiteral("font-size: 11px; color: #f2b35c;")
                                   : QStringLiteral("font-size: 11px; color: #8c9ba8;"));
      infoLayout->addWidget(updateLbl);
      rLayout->addLayout(infoLayout, 1);

      connect(chk, &QCheckBox::toggled, this, [this, list](bool checked) {
        if (!service_) return;
        QStringList cur = service_->settings()->enabledRulesetIds();
        if (!service_->settings()->rulesetSelectionConfigured()) {
          for (const auto &l : service_->listManager()->availableLists()) {
            if (l.enabled) cur.append(l.id);
          }
        }
        if (checked && !cur.contains(list.id)) cur.append(list.id);
        else if (!checked) cur.removeAll(list.id);
        service_->settings()->setEnabledRulesetIds(cur);
      });

      rulesetCatalogLayout_->addWidget(row);
    }
  }
}

// ---------------- 3. Custom Filters Tab ----------------

void DaliNiraBlockerPage::createCustomFiltersTab() {
  auto *scroll = new QScrollArea(this);
  scroll->setWidgetResizable(true);
  auto *container = new QWidget(scroll);
  auto *layout = new QVBoxLayout(container);
  layout->setContentsMargins(28, 24, 28, 24);
  layout->setSpacing(18);

  auto *editorGroup = new QGroupBox(I18n::text(QStringLiteral("blocker.tab_custom_filters")), container);
  auto *eLayout = new QVBoxLayout(editorGroup);
  eLayout->setContentsMargins(16, 16, 16, 16);
  eLayout->setSpacing(10);

  auto *hint = new QLabel(I18n::text(QStringLiteral("blocker.rule_format_hint")), editorGroup);
  hint->setStyleSheet(QStringLiteral("color: #8c9ba8; font-size: 12px;"));
  eLayout->addWidget(hint);

  userFilterEditor_ = new QPlainTextEdit(editorGroup);
  userFilterEditor_->setPlaceholderText(I18n::text(QStringLiteral("blocker.rule_example_placeholder")));
  userFilterEditor_->setMinimumHeight(110);
  eLayout->addWidget(userFilterEditor_);

  auto *btnRow = new QHBoxLayout;
  auto *addBtn = new QPushButton(I18n::text(QStringLiteral("blocker.add_filter")), editorGroup);
  addBtn->setObjectName(QStringLiteral("primary-btn"));
  auto *importBtn = new QPushButton(I18n::text(QStringLiteral("common.import_ellipsis")), editorGroup);
  auto *exportBtn = new QPushButton(I18n::text(QStringLiteral("common.export_ellipsis")), editorGroup);
  userFilterValidation_ = new QLabel(editorGroup);
  userFilterValidation_->setStyleSheet(QStringLiteral("color: #5cf2c4; font-size: 12px;"));

  btnRow->addWidget(addBtn);
  btnRow->addWidget(importBtn);
  btnRow->addWidget(exportBtn);
  btnRow->addWidget(userFilterValidation_);
  btnRow->addStretch(1);
  eLayout->addLayout(btnRow);

  connect(addBtn, &QPushButton::clicked, this, [this]() {
    if (!service_) return;
    const QString text = userFilterEditor_->toPlainText().trimmed();
    if (text.isEmpty()) return;
    const QStringList lines = text.split(QRegularExpression(QStringLiteral("[\r\n]+")), Qt::SkipEmptyParts);
    QStringList current = service_->settings()->customFilters();
    int added = 0;
    QStringList errors;
    for (const QString &line : lines) {
      const QString trimmed = line.trimmed();
      const QString error = DaliNiraBlockerEngine::validateCustomFilterLine(
          trimmed, service_ ? service_->listManager()->redirectResources() : nullptr);
      if (!error.isEmpty()) {
        errors.append(QStringLiteral("%1: %2").arg(trimmed, error));
        continue;
      }
      if (!trimmed.isEmpty() && !current.contains(trimmed)) {
        current.append(trimmed);
        added++;
      }
    }
    service_->settings()->setCustomFilters(current);
    userFilterEditor_->clear();
    if (errors.isEmpty()) {
      userFilterValidation_->setStyleSheet(QStringLiteral("color: #5cf2c4; font-size: 12px;"));
      userFilterValidation_->setText(I18n::text(QStringLiteral("blocker.rules_added")).arg(added));
    } else {
      userFilterValidation_->setStyleSheet(QStringLiteral("color: #ff8e99; font-size: 12px;"));
      userFilterValidation_->setText(I18n::text(QStringLiteral("blocker.rules_added_rejected"))
          .arg(added).arg(errors.size()).arg(errors.first()));
    }
    refreshCustomFiltersTab();
  });

  connect(importBtn, &QPushButton::clicked, this, [this]() {
    if (!service_) return;
    const QString path = QFileDialog::getOpenFileName(this, I18n::text(QStringLiteral("blocker.import_filter_list_title")), QString(), I18n::text(QStringLiteral("blocker.text_or_all_filter")));
    if (path.isEmpty()) return;
    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
      const QString content = QString::fromUtf8(file.readAll());
      const QStringList lines = content.split(QRegularExpression(QStringLiteral("[\r\n]+")), Qt::SkipEmptyParts);
      QStringList current = service_->settings()->customFilters();
      QStringList errors;
      for (const QString &l : lines) {
        const QString tr = l.trimmed();
        const QString error = DaliNiraBlockerEngine::validateCustomFilterLine(
            tr, service_ ? service_->listManager()->redirectResources() : nullptr);
        if (!error.isEmpty()) {
          errors.append(QStringLiteral("%1: %2").arg(tr, error));
          continue;
        }
        // Preserve comments: they are part of the user's list even though the
        // compiler intentionally ignores them.
        if (!tr.isEmpty() && !current.contains(tr)) {
          current.append(tr);
        }
      }
      service_->settings()->setCustomFilters(current);
      if (!errors.isEmpty()) {
        userFilterValidation_->setStyleSheet(QStringLiteral("color: #ff8e99; font-size: 12px;"));
        userFilterValidation_->setText(I18n::text(QStringLiteral("blocker.invalid_lines_skipped"))
            .arg(errors.size()).arg(errors.first()));
      }
      refreshCustomFiltersTab();
    }
  });

  connect(exportBtn, &QPushButton::clicked, this, [this]() {
    if (!service_) return;
    const QString path = QFileDialog::getSaveFileName(this, I18n::text(QStringLiteral("blocker.export_custom_filters_title")), QStringLiteral("dalinira-user-filters.txt"), I18n::text(QStringLiteral("blocker.text_filter")));
    if (path.isEmpty()) return;
    QSaveFile file(path);
    const QByteArray bytes = service_->settings()->customFilters().join(QStringLiteral("\n")).toUtf8();
    if (file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit()) {
      QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
      QMessageBox::information(this, I18n::text(QStringLiteral("blocker.exported_title")), I18n::text(QStringLiteral("blocker.custom_filters_saved")));
    }
  });

  layout->addWidget(editorGroup);

  // User Filter List
  auto *listGroup = new QGroupBox(I18n::text(QStringLiteral("blocker.user_filter_list")), container);
  userFilterListLayout_ = new QVBoxLayout(listGroup);
  userFilterListLayout_->setContentsMargins(16, 16, 16, 16);
  userFilterListLayout_->setSpacing(8);
  layout->addWidget(listGroup);

  layout->addStretch(1);
  scroll->setWidget(container);
  stack_->addWidget(scroll);
}

void DaliNiraBlockerPage::refreshCustomFiltersTab() {
  if (!service_ || !userFilterListLayout_) return;

  while (userFilterListLayout_->count() > 0) {
    auto *item = userFilterListLayout_->takeAt(0);
    if (item->widget()) delete item->widget();
    delete item;
  }

  const QStringList filters = service_->settings()->customFilters();
  if (filters.isEmpty()) {
    auto *empty = new QLabel(I18n::text(QStringLiteral("blocker.no_user_filters")), this);
    empty->setStyleSheet(QStringLiteral("color: #72849a; font-style: italic;"));
    userFilterListLayout_->addWidget(empty);
    return;
  }

  for (const QString &f : filters) {
    auto *row = new QFrame(this);
    row->setStyleSheet(QStringLiteral("background: #121720; border: 1px solid #24303f; border-radius: 8px; padding: 8px 12px;"));
    auto *rLayout = new QHBoxLayout(row);
    rLayout->setContentsMargins(6, 4, 6, 4);

    auto *textLbl = new QLabel(f, row);
    textLbl->setStyleSheet(QStringLiteral("font-family: monospace; font-size: 13px; color: #e4ebf5;"));
    rLayout->addWidget(textLbl, 1);

    auto *delBtn = new QPushButton(I18n::text(QStringLiteral("common.delete")), row);
    delBtn->setStyleSheet(QStringLiteral("background: #3b2226; border-color: #5c2b33; color: #ff8e99;"));
    rLayout->addWidget(delBtn);

    connect(delBtn, &QPushButton::clicked, this, [this, f]() {
      if (!service_) return;
      QStringList current = service_->settings()->customFilters();
      current.removeAll(f);
      service_->settings()->setCustomFilters(current);
      refreshCustomFiltersTab();
    });

    userFilterListLayout_->addWidget(row);
  }
}

// ---------------- 4. Sites Tab ----------------

void DaliNiraBlockerPage::createSitesTab() {
  auto *scroll = new QScrollArea(this);
  scroll->setWidgetResizable(true);
  auto *container = new QWidget(scroll);
  auto *layout = new QVBoxLayout(container);
  layout->setContentsMargins(28, 24, 28, 24);
  layout->setSpacing(18);

  auto *siteEditGroup = new QGroupBox(I18n::text(QStringLiteral("blocker.site_protection_allowlist")), container);
  auto *sLayout = new QVBoxLayout(siteEditGroup);
  sLayout->setContentsMargins(16, 16, 16, 16);
  sLayout->setSpacing(12);

  siteActiveStatus_ = new QLabel(siteEditGroup);
  siteActiveStatus_->setStyleSheet(QStringLiteral("color: #5cf2c4; font-size: 13px; font-weight: 600;"));
  sLayout->addWidget(siteActiveStatus_);

  auto *inputRow = new QHBoxLayout;
  siteHostInput_ = new QLineEdit(siteEditGroup);
  siteHostInput_->setPlaceholderText(QStringLiteral("example.com"));
  siteHostInput_->setMinimumWidth(220);

  siteWhitelistCheck_ = new GlowToggleSwitch(I18n::text(QStringLiteral("blocker.allowlist_toggle")), siteEditGroup);
  siteAdsCheck_ = new GlowToggleSwitch(I18n::text(QStringLiteral("blocker.ad_blocking")), siteEditGroup);
  siteAdsCheck_->setChecked(true);
  siteTrackersCheck_ = new GlowToggleSwitch(I18n::text(QStringLiteral("blocker.tracker_protection")), siteEditGroup);
  siteTrackersCheck_->setChecked(true);

  auto *saveBtn = new QPushButton(I18n::text(QStringLiteral("common.save")), siteEditGroup);
  saveBtn->setObjectName(QStringLiteral("primary-btn"));

  inputRow->addWidget(siteHostInput_);
  inputRow->addWidget(siteWhitelistCheck_);
  inputRow->addWidget(siteAdsCheck_);
  inputRow->addWidget(siteTrackersCheck_);
  inputRow->addWidget(saveBtn);
  sLayout->addLayout(inputRow);

  connect(siteWhitelistCheck_, &QCheckBox::toggled, this, [this](bool checked) {
    siteAdsCheck_->setEnabled(!checked);
    siteTrackersCheck_->setEnabled(!checked);
  });

  connect(saveBtn, &QPushButton::clicked, this, [this]() {
    if (!service_) return;
    const QString host = siteHostInput_->text().trimmed().toLower();
    if (host.isEmpty()) return;
    SitePolicy p;
    p.whitelisted = siteWhitelistCheck_->isChecked();
    p.adBlocking = siteAdsCheck_->isChecked();
    p.trackerProtection = siteTrackersCheck_->isChecked();
    service_->settings()->setSitePolicy(host, p);
    refreshSitesTab();
  });

  layout->addWidget(siteEditGroup);

  // Site Policies List
  auto *listGroup = new QGroupBox(I18n::text(QStringLiteral("blocker.site_rules")), container);
  sitePolicyListLayout_ = new QVBoxLayout(listGroup);
  sitePolicyListLayout_->setContentsMargins(16, 16, 16, 16);
  sitePolicyListLayout_->setSpacing(8);
  layout->addWidget(listGroup);

  layout->addStretch(1);
  scroll->setWidget(container);
  stack_->addWidget(scroll);
}

void DaliNiraBlockerPage::refreshSitesTab() {
  if (!service_) return;

  if (siteActiveStatus_) {
    if (!activeHost_.isEmpty()) {
      const SitePolicy p = service_->settings()->sitePolicy(activeHost_);
      if (p.whitelisted) {
        siteActiveStatus_->setText(I18n::text(QStringLiteral("blocker.active_site_allowlisted")).arg(activeHost_));
        siteActiveStatus_->setStyleSheet(QStringLiteral("color: #ffb86c; font-size: 13px; font-weight: 600;"));
      } else {
        siteActiveStatus_->setText(I18n::text(QStringLiteral("blocker.active_site_protected")).arg(activeHost_));
        siteActiveStatus_->setStyleSheet(QStringLiteral("color: #5cf2c4; font-size: 13px; font-weight: 600;"));
      }
    } else {
      siteActiveStatus_->setText(I18n::text(QStringLiteral("blocker.no_active_web_tab")));
      siteActiveStatus_->setStyleSheet(QStringLiteral("color: #8c9ba8; font-size: 13px;"));
    }
  }

  if (sitePolicyListLayout_) {
    while (sitePolicyListLayout_->count() > 0) {
      auto *item = sitePolicyListLayout_->takeAt(0);
      if (item->widget()) delete item->widget();
      delete item;
    }

    const auto policies = service_->settings()->sitePolicies();
    if (policies.isEmpty()) {
      auto *empty = new QLabel(I18n::text(QStringLiteral("blocker.no_site_rules")), this);
      empty->setStyleSheet(QStringLiteral("color: #72849a; font-style: italic;"));
      sitePolicyListLayout_->addWidget(empty);
      return;
    }

    for (auto it = policies.constBegin(); it != policies.constEnd(); ++it) {
      const QString host = it.key();
      const SitePolicy p = it.value();

      auto *row = new QFrame(this);
      row->setStyleSheet(QStringLiteral("background: #121720; border: 1px solid #24303f; border-radius: 8px; padding: 8px 12px;"));
      auto *rLayout = new QHBoxLayout(row);
      rLayout->setContentsMargins(6, 4, 6, 4);

      auto *hostLbl = new QLabel(host, row);
      hostLbl->setStyleSheet(QStringLiteral("font-size: 13px; font-weight: 600; color: #e4ebf5; min-width: 160px;"));
      rLayout->addWidget(hostLbl);

      QString statusStr = p.whitelisted ? I18n::text(QStringLiteral("blocker.allowlisted")) : I18n::text(QStringLiteral("blocker.site_status"))
                                                                                .arg(p.adBlocking ? I18n::text(QStringLiteral("common.on")) : I18n::text(QStringLiteral("common.off")),
                                                                                     p.trackerProtection ? I18n::text(QStringLiteral("common.on")) : I18n::text(QStringLiteral("common.off")));
      auto *statusLbl = new QLabel(statusStr, row);
      statusLbl->setStyleSheet(p.whitelisted ? QStringLiteral("color: #ffb86c; font-size: 12px;") : QStringLiteral("color: #8c9ba8; font-size: 12px;"));
      rLayout->addWidget(statusLbl, 1);

      auto *toggleBtn = new QPushButton(p.whitelisted ? I18n::text(QStringLiteral("blocker.remove_from_allowlist")) : I18n::text(QStringLiteral("blocker.add_to_allowlist")), row);
      auto *pauseBtn = new QPushButton(I18n::text(QStringLiteral("blocker.pause_10_min")), row);
      auto *delBtn = new QPushButton(I18n::text(QStringLiteral("common.delete")), row);
      delBtn->setStyleSheet(QStringLiteral("background: #3b2226; border-color: #5c2b33; color: #ff8e99;"));

      rLayout->addWidget(toggleBtn);
      rLayout->addWidget(pauseBtn);
      rLayout->addWidget(delBtn);

      connect(toggleBtn, &QPushButton::clicked, this, [this, host, p]() {
        if (!service_) return;
        SitePolicy np = p;
        np.whitelisted = !np.whitelisted;
        service_->settings()->setSitePolicy(host, np);
        refreshSitesTab();
      });

      connect(pauseBtn, &QPushButton::clicked, this, [this, host, p]() {
        if (!service_) return;
        SitePolicy np = p;
        np.temporaryDisabledUntil = QDateTime::currentMSecsSinceEpoch() + (10 * 60 * 1000);
        service_->settings()->setSitePolicy(host, np);
        refreshSitesTab();
      });

      connect(delBtn, &QPushButton::clicked, this, [this, host]() {
        if (!service_) return;
        service_->settings()->removeSitePolicy(host);
        refreshSitesTab();
      });

      sitePolicyListLayout_->addWidget(row);
    }
  }
}

// ---------------- 5. Statistics Tab ----------------

void DaliNiraBlockerPage::createStatisticsTab() {
  auto *scroll = new QScrollArea(this);
  scroll->setWidgetResizable(true);
  auto *container = new QWidget(scroll);
  auto *layout = new QVBoxLayout(container);
  layout->setContentsMargins(28, 24, 28, 24);
  layout->setSpacing(18);

  // 9 Stats Tiles Grid
  auto *gridFrame = new QFrame(container);
  gridFrame->setStyleSheet(QStringLiteral("background: #181f28; border: 1px solid #283442; border-radius: 10px; padding: 14px;"));
  auto *grid = new QGridLayout(gridFrame);
  grid->setSpacing(14);

  auto addGridStat = [grid, gridFrame](int r, int c, const QString &title, QLabel **outVal) {
    auto *tile = new QVBoxLayout;
    auto *tLabel = new QLabel(title, gridFrame);
    tLabel->setStyleSheet(QStringLiteral("color: #8c9ba8; font-size: 11px; font-weight: 600; text-transform: uppercase;"));
    auto *vLabel = new QLabel(QStringLiteral("0"), gridFrame);
    vLabel->setStyleSheet(QStringLiteral("color: #4ec9ff; font-size: 18px; font-weight: 700;"));
    tile->addWidget(tLabel);
    tile->addWidget(vLabel);
    grid->addLayout(tile, r, c);
    *outVal = vLabel;
  };

  addGridStat(0, 0, I18n::text(QStringLiteral("common.today")), &statsToday_);
  addGridStat(0, 1, I18n::text(QStringLiteral("blocker.stat_this_week")), &statsWeek_);
  addGridStat(0, 2, I18n::text(QStringLiteral("blocker.stat_this_month")), &statsMonth_);
  addGridStat(1, 0, I18n::text(QStringLiteral("blocker.stat_total")), &statsTotal_);
  addGridStat(1, 1, I18n::text(QStringLiteral("blocker.stat_trackers")), &statsTrackers_);
  addGridStat(1, 2, I18n::text(QStringLiteral("blocker.stat_estimated_savings")), &statsSaved_);
  addGridStat(2, 0, I18n::text(QStringLiteral("blocker.stat_speedup")), &statsSpeed_);
  addGridStat(2, 1, I18n::text(QStringLiteral("blocker.stat_allowlisted_sites")), &statsWhitelistSites_);
  addGridStat(2, 2, I18n::text(QStringLiteral("blocker.stat_allowlist_allowed")), &statsWhitelistAllowed_);

  layout->addWidget(gridFrame);

  // Breakdown lists
  auto createRankingSection = [](const QString &title, QWidget *parent,
                                 QVBoxLayout **rows) {
    auto *section = new QFrame(parent);
    section->setObjectName(QStringLiteral("ranking-section"));
    section->setStyleSheet(QStringLiteral(
        "QFrame#ranking-section { background: #181f28; border: 1px solid #283442; border-radius: 10px; }"));
    auto *sectionLayout = new QVBoxLayout(section);
    sectionLayout->setContentsMargins(16, 14, 16, 16);
    sectionLayout->setSpacing(12);
    auto *titleLabel = new QLabel(title, section);
    titleLabel->setStyleSheet(QStringLiteral(
        "color: #e4ebf5; border: 0; font-size: 14px; font-weight: 700;"));
    sectionLayout->addWidget(titleLabel);
    *rows = new QVBoxLayout;
    (*rows)->setContentsMargins(0, 0, 0, 0);
    (*rows)->setSpacing(12);
    sectionLayout->addLayout(*rows);
    return section;
  };

  auto *topSitesGroup = createRankingSection(
      I18n::text(QStringLiteral("blocker.top_blocked_sites")), container, &topSitesLayout_);
  topSitesLayout_->setSpacing(12);
  layout->addWidget(topSitesGroup);

  auto *topListsGroup = createRankingSection(
      I18n::text(QStringLiteral("blocker.top_filter_lists")), container, &topListsLayout_);
  topListsLayout_->setSpacing(12);
  layout->addWidget(topListsGroup);

  layout->addStretch(1);
  scroll->setWidget(container);
  stack_->addWidget(scroll);
}

void DaliNiraBlockerPage::refreshStatisticsTab() {
  if (!service_) return;

  if (statsToday_) statsToday_->setText(QString::number(service_->todayBlockedCount()));
  if (statsWeek_) statsWeek_->setText(QString::number(service_->weekBlockedCount()));
  if (statsMonth_) statsMonth_->setText(QString::number(service_->monthBlockedCount()));
  if (statsTotal_) statsTotal_->setText(QString::number(service_->totalBlockedCount()));
  if (statsTrackers_) statsTrackers_->setText(QString::number(service_->totalTrackersBlockedCount()));

  if (statsSaved_) {
    const quint64 bytes = service_->estimatedBytesSaved();
    if (bytes < 1024 * 1024) statsSaved_->setText(I18n::text(QStringLiteral("units.kilobytes")).arg(bytes / 1024));
    else statsSaved_->setText(I18n::text(QStringLiteral("units.megabytes")).arg(QString::number(bytes / (1024.0 * 1024.0), 'f', 1)));
  }

  if (statsSpeed_) {
    const quint64 blocked = service_->totalBlockedCount();
    const quint64 allowed = service_->totalAllowedCount();
    const quint64 total = blocked + allowed;
    const int pct = total > 0 ? qBound(0, static_cast<int>((blocked * 100) / total), 95) : 0;
    statsSpeed_->setText(QStringLiteral("%1%").arg(pct));
  }

  if (statsWhitelistSites_) statsWhitelistSites_->setText(QString::number(service_->settings()->sitePolicies().size()));
  if (statsWhitelistAllowed_) statsWhitelistAllowed_->setText(QString::number(service_->whitelistAllowedCount()));

  // Top Sites
  if (topSitesLayout_) {
    while (topSitesLayout_->count() > 0) {
      auto *item = topSitesLayout_->takeAt(0);
      if (item->widget()) delete item->widget();
      delete item;
    }
    const auto top = service_->topBlockedHosts(6);
    if (top.isEmpty()) {
      auto *empty = new QLabel(I18n::text(QStringLiteral("blocker.no_blocked_sites")), this);
      empty->setStyleSheet(QStringLiteral("color: #72849a; font-style: italic;"));
      topSitesLayout_->addWidget(empty);
    } else {
      int rank = 1;
      for (const auto &p : top) {
        topSitesLayout_->addWidget(createRankedMetricRow(
            rank++, p.first, p.second, I18n::text(QStringLiteral("blocker.blocks_unit")),
            QStringLiteral("#5cf2c4"), topSitesLayout_->parentWidget()));
      }
    }
  }

  // Top Lists
  if (topListsLayout_) {
    while (topListsLayout_->count() > 0) {
      auto *item = topListsLayout_->takeAt(0);
      if (item->widget()) delete item->widget();
      delete item;
    }
    const auto top = service_->topMatchedRulesets(6);
    if (top.isEmpty()) {
      auto *empty = new QLabel(I18n::text(QStringLiteral("blocker.no_ruleset_matches")), this);
      empty->setStyleSheet(QStringLiteral("color: #72849a; font-style: italic;"));
      topListsLayout_->addWidget(empty);
    } else {
      int rank = 1;
      for (const auto &p : top) {
        topListsLayout_->addWidget(createRankedMetricRow(
            rank++, p.first, p.second, I18n::text(QStringLiteral("blocker.matches_unit")),
            QStringLiteral("#4ec9ff"), topListsLayout_->parentWidget()));
      }
    }
  }
}

// ---------------- 6. Logger Tab ----------------

void DaliNiraBlockerPage::createLoggerTab() {
  auto *container = new QWidget(this);
  auto *layout = new QVBoxLayout(container);
  layout->setContentsMargins(28, 24, 28, 24);
  layout->setSpacing(14);

  // Search & Filter Header
  auto *header = new QHBoxLayout;
  loggerSearch_ = new QLineEdit(container);
  loggerSearch_->setPlaceholderText(I18n::text(QStringLiteral("blocker.logger_search_placeholder")));
  loggerActionCombo_ = new QComboBox(container);
  loggerActionCombo_->addItem(I18n::text(QStringLiteral("common.all")), QStringLiteral("all"));
  loggerActionCombo_->addItem(I18n::text(QStringLiteral("blocker.blocked")), QStringLiteral("block"));
  loggerActionCombo_->addItem(I18n::text(QStringLiteral("blocker.allowed")), QStringLiteral("allow"));

  header->addWidget(loggerSearch_, 1);
  header->addWidget(loggerActionCombo_);
  layout->addLayout(header);

  // Table
  logTable_ = new QTableWidget(container);
  logTable_->setColumnCount(6);
  logTable_->setHorizontalHeaderLabels({I18n::text(QStringLiteral("blocker.log_col_time")), I18n::text(QStringLiteral("blocker.log_col_type")), I18n::text(QStringLiteral("blocker.log_col_action")),
                                        I18n::text(QStringLiteral("blocker.log_col_list")), I18n::text(QStringLiteral("blocker.log_col_rule")), QStringLiteral("URL")});
  logTable_->horizontalHeader()->setStretchLastSection(true);
  logTable_->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft);
  logTable_->verticalHeader()->setVisible(false);
  logTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
  logTable_->setStyleSheet(QStringLiteral(
      "QTableWidget { background: #121720; border: 1px solid #283442; gridline-color: #1e2733; color: #e4ebf5; font-size: 12px; }"
      "QHeaderView::section { background: #181f28; color: #8c9ba8; font-weight: 600; border: 0; border-bottom: 1px solid #283442; padding: 6px 8px; }"
      "QTableWidget::item { padding: 4px 8px; }"
      "QTableWidget::item:selected { background: #2f3d4f; }"
  ));
  layout->addWidget(logTable_, 1);

  connect(loggerSearch_, &QLineEdit::textChanged, this, &DaliNiraBlockerPage::refreshLoggerTab);
  connect(loggerActionCombo_, &QComboBox::currentIndexChanged, this, &DaliNiraBlockerPage::refreshLoggerTab);

  // Diagnostics Box
  auto *diagBox = new QFrame(container);
  diagBox->setStyleSheet(QStringLiteral("background: #181f28; border: 1px solid #283442; border-radius: 8px; padding: 10px;"));
  auto *dLayout = new QHBoxLayout(diagBox);

  auto addDiagTile = [dLayout, diagBox](const QString &title, QLabel **outVal) {
    auto *tile = new QVBoxLayout;
    auto *t = new QLabel(title, diagBox);
    t->setStyleSheet(QStringLiteral("color: #8c9ba8; font-size: 10px; text-transform: uppercase;"));
    auto *v = new QLabel(QStringLiteral("0"), diagBox);
    v->setStyleSheet(QStringLiteral("color: #4ec9ff; font-size: 14px; font-weight: bold;"));
    tile->addWidget(t);
    tile->addWidget(v);
    dLayout->addLayout(tile);
    *outVal = v;
  };

  addDiagTile(I18n::text(QStringLiteral("blocker.diag_loaded_filters")), &diagLoaded_);
  addDiagTile(I18n::text(QStringLiteral("blocker.diag_active_lists")), &diagActive_);
  addDiagTile(I18n::text(QStringLiteral("blocker.blocked")), &diagBlocked_);
  addDiagTile(I18n::text(QStringLiteral("blocker.allowed")), &diagAllowed_);
  addDiagTile(I18n::text(QStringLiteral("blocker.diag_processing_time")), &diagTime_);
  addDiagTile(I18n::text(QStringLiteral("blocker.diag_memory")), &diagMemory_);

  layout->addWidget(diagBox);
  stack_->addWidget(container);
}

void DaliNiraBlockerPage::refreshLoggerTab() {
  if (!service_ || !logTable_) return;

  const QString filterText = loggerSearch_->text().trimmed().toLower();
  const QString actionFilter = loggerActionCombo_->currentData().toString();
  const auto logs = service_->recentLogs(300);

  logTable_->setRowCount(0);
  int row = 0;

  for (const auto &entry : logs) {
    const QString actionStr = entry.action == DaliNiraBlockerAction::Block ? QStringLiteral("block") :
                              entry.action == DaliNiraBlockerAction::Redirect ? QStringLiteral("redirect") : QStringLiteral("allow");
    if (actionFilter == QLatin1String("block") && entry.action == DaliNiraBlockerAction::Allow) continue;
    if (actionFilter == QLatin1String("allow") && entry.action != DaliNiraBlockerAction::Allow) continue;

    if (!filterText.isEmpty()) {
      if (!entry.requestUrl.toLower().contains(filterText) &&
          !entry.siteHost.toLower().contains(filterText) &&
          !entry.rulesetId.toLower().contains(filterText)) {
        continue;
      }
    }

    logTable_->insertRow(row);
    logTable_->setItem(row, 0, new QTableWidgetItem(entry.timestamp.toString(QStringLiteral("hh:mm:ss"))));
    logTable_->setItem(row, 1, new QTableWidgetItem(entry.resourceTypeStr));

    // The final action, and for redirects and allows what kind they were.
    QString actionLabel;
    if (entry.action == DaliNiraBlockerAction::Block) actionLabel = I18n::text(QStringLiteral("blocker.log_block"));
    else if (entry.reason == QLatin1String("dnr-remove-param")) actionLabel = I18n::text(QStringLiteral("blocker.log_params_removed"));
    else if (entry.reason == QLatin1String("dnr-redirect-resource")) actionLabel = I18n::text(QStringLiteral("blocker.log_redirect_resource"));
    else if (entry.action == DaliNiraBlockerAction::Redirect) actionLabel = I18n::text(QStringLiteral("blocker.log_redirect"));
    else if (entry.reason == QLatin1String("dnr-allow") || entry.reason == QLatin1String("user-allow"))
      actionLabel = I18n::text(QStringLiteral("blocker.log_allow_exception"));
    else actionLabel = I18n::text(QStringLiteral("blocker.log_allow"));
    auto *actItem = new QTableWidgetItem(actionLabel);
    if (entry.action == DaliNiraBlockerAction::Block) actItem->setForeground(QColor(0xff, 0x6b, 0x6b));
    else if (entry.action == DaliNiraBlockerAction::Redirect) actItem->setForeground(QColor(0xff, 0xb8, 0x6c));
    else actItem->setForeground(QColor(0x5c, 0xf2, 0xc4));
    logTable_->setItem(row, 2, actItem);

    logTable_->setItem(row, 3, new QTableWidgetItem(entry.rulesetId.isEmpty() ? entry.reason : entry.rulesetId));
    logTable_->setItem(row, 4, new QTableWidgetItem(entry.ruleId > 0 ? QString::number(entry.ruleId) : QStringLiteral("-")));
    logTable_->setItem(row, 5, new QTableWidgetItem(entry.requestUrl));
    row++;
  }

  // Diagnostics
  if (diagLoaded_) diagLoaded_->setText(QString::number(service_->filterEngine()->ruleCount()));
  if (diagActive_) diagActive_->setText(QString::number(service_->listManager()->availableLists().size()));
  if (diagBlocked_) diagBlocked_->setText(QString::number(service_->sessionBlockedCount()));
  if (diagAllowed_) diagAllowed_->setText(QString::number(service_->totalAllowedCount()));
  if (diagTime_) {
    if (service_->evaluationCount() > 0) {
      diagTime_->setText(QStringLiteral("%1 ms").arg(QString::number(service_->averageEvaluationTimeMs(), 'f', 3)));
    } else {
      diagTime_->setText(QStringLiteral("—"));
    }
  }
  if (diagMemory_) {
    const quint64 memBytes = service_->estimatedMemoryBytes();
    diagMemory_->setText(I18n::text(QStringLiteral("blocker.diag_estimated_mb")).arg(QString::number(memBytes / (1024.0 * 1024.0), 'f', 2)));
  }
}

// ---------------- 7. Develop Tab ----------------

void DaliNiraBlockerPage::createDevelopTab() {
  auto *container = new QWidget(this);
  auto *layout = new QVBoxLayout(container);
  layout->setContentsMargins(28, 24, 28, 24);
  layout->setSpacing(14);

  auto *topBar = new QHBoxLayout;
  auto *vLabel = new QLabel(I18n::text(QStringLiteral("blocker.view_label")), container);
  developViewCombo_ = new QComboBox(container);
  developViewCombo_->addItem(I18n::text(QStringLiteral("blocker.view_mode_details")), QStringLiteral("modes"));
  developViewCombo_->addItem(I18n::text(QStringLiteral("blocker.view_dnr_summary")), QStringLiteral("dnr"));
  developViewCombo_->addItem(I18n::text(QStringLiteral("blocker.view_session_summary")), QStringLiteral("session"));

  auto *resetStatsBtn = new QPushButton(I18n::text(QStringLiteral("blocker.reset_counter")), container);

  topBar->addWidget(vLabel);
  topBar->addWidget(developViewCombo_);
  topBar->addStretch(1);
  topBar->addWidget(resetStatsBtn);
  layout->addLayout(topBar);

  developEditor_ = new QPlainTextEdit(container);
  developEditor_->setReadOnly(true);
  developEditor_->setStyleSheet(QStringLiteral("font-family: monospace; font-size: 12px; background: #0e1217; color: #a4b3c6;"));
  layout->addWidget(developEditor_, 1);

  connect(developViewCombo_, &QComboBox::currentIndexChanged, this, &DaliNiraBlockerPage::refreshDevelopTab);
  connect(resetStatsBtn, &QPushButton::clicked, this, [this]() {
    if (service_) {
      service_->resetAllStats();
      refreshAll();
    }
  });

  stack_->addWidget(container);
}

void DaliNiraBlockerPage::refreshDevelopTab() {
  if (!service_ || !developEditor_) return;
  const QString view = developViewCombo_->currentData().toString();

  QString text;
  if (view == QLatin1String("dnr")) {
    text = I18n::text(QStringLiteral("blocker.dnr_summary_text"))
               .arg(QString::number(service_->filterEngine()->ruleCount()),
                    QString::number(service_->filterEngine()->customRuleCount()),
                    QString::number(service_->listManager()->availableLists().size()));
    for (const auto &p : service_->topMatchedRulesets()) {
      text += I18n::text(QStringLiteral("blocker.dnr_summary_line")).arg(p.first, QString::number(p.second));
    }
  } else if (view == QLatin1String("session")) {
    text = I18n::text(QStringLiteral("blocker.session_summary_text"))
               .arg(QString::number(service_->sessionBlockedCount()),
                    QString::number(service_->totalBlockedCount()),
                    QString::number(service_->totalAllowedCount()),
                    QString::number(service_->totalTrackersBlockedCount()),
                    QString::number(service_->whitelistAllowedCount()));
    const auto logs = service_->recentLogs(10);
    for (const auto &l : logs) {
      if (l.action == DaliNiraBlockerAction::Block || l.action == DaliNiraBlockerAction::Redirect) {
        text += QStringLiteral("  [%1] %2 -> %3 (%4)\n")
                    .arg(l.timestamp.toString(QStringLiteral("hh:mm:ss")), l.siteHost, l.rulesetId, l.resourceTypeStr);
      }
    }
  } else {
    text = I18n::text(QStringLiteral("blocker.mode_summary_text"))
               .arg(modeToString(service_->settings()->mode()),
                    service_->settings()->strictBlock() ? I18n::text(QStringLiteral("common.on")) : I18n::text(QStringLiteral("common.off")),
                    service_->settings()->popupBlock() ? I18n::text(QStringLiteral("common.on")) : I18n::text(QStringLiteral("common.off")),
                    service_->settings()->developerMode() ? I18n::text(QStringLiteral("common.on")) : I18n::text(QStringLiteral("common.off")),
                    service_->settings()->autoReloadOnModeChange() ? I18n::text(QStringLiteral("common.on")) : I18n::text(QStringLiteral("common.off")),
                    service_->settings()->showBlockedCountOnToolbar() ? I18n::text(QStringLiteral("common.on")) : I18n::text(QStringLiteral("common.off")));
  }

  developEditor_->setPlainText(text);
}

// ---------------- 8. About Tab ----------------

void DaliNiraBlockerPage::createAboutTab() {
  auto *scroll = new QScrollArea(this);
  scroll->setWidgetResizable(true);
  auto *container = new QWidget(scroll);
  auto *layout = new QVBoxLayout(container);
  layout->setContentsMargins(28, 24, 28, 24);
  layout->setSpacing(18);

  auto *aboutGroup = new QGroupBox(I18n::text(QStringLiteral("blocker.about_title")), container);
  auto *aLayout = new QVBoxLayout(aboutGroup);
  aLayout->setContentsMargins(18, 18, 18, 18);
  aLayout->setSpacing(14);

  auto *body = new QLabel(
      I18n::text(QStringLiteral("blocker.about_body")),
      aboutGroup);
  body->setWordWrap(true);
  body->setStyleSheet(QStringLiteral("color: #b8c4d4; font-size: 13px; line-height: 1.5;"));
  aLayout->addWidget(body);

  auto *hostBox = new QFrame(aboutGroup);
  hostBox->setStyleSheet(QStringLiteral("background: #121720; border: 1px solid #283545; border-radius: 8px; padding: 12px;"));
  auto *hLayout = new QHBoxLayout(hostBox);
  auto *globe = new QLabel(QStringLiteral("🌐"), hostBox);
  globe->setStyleSheet(QStringLiteral("font-size: 20px;"));
  aboutActiveHostLabel_ = new QLabel(I18n::text(QStringLiteral("blocker.no_active_web_tab")), hostBox);
  aboutActiveHostLabel_->setStyleSheet(QStringLiteral("font-size: 14px; font-weight: 600; color: #4ec9ff;"));

  hLayout->addWidget(globe);
  hLayout->addWidget(aboutActiveHostLabel_, 1);
  aLayout->addWidget(hostBox);

  auto *verLbl = new QLabel(
      I18n::text(QStringLiteral("blocker.about_version"))
          .arg(QStringLiteral(DALINIRA_BROWSER_VERSION)),
      aboutGroup);
  verLbl->setStyleSheet(QStringLiteral("color: #72849a; font-size: 12px;"));
  aLayout->addWidget(verLbl);

  layout->addWidget(aboutGroup);
  layout->addStretch(1);
  scroll->setWidget(container);
  stack_->addWidget(scroll);
}

bool DaliNiraBlockerPage::eventFilter(QObject *watched, QEvent *event) {
  if (event->type() == QEvent::MouseButtonRelease) {
    auto isChildOf = [](QWidget *parent, QObject *obj) {
      if (!parent || !obj) return false;
      if (parent == obj) return true;
      auto *w = qobject_cast<QWidget *>(obj);
      return w && parent->isAncestorOf(w);
    };

    if (isChildOf(basicCard_, watched)) {
      if (service_) service_->settings()->setMode(DaliNiraBlockerMode::Basic);
      return true;
    }
    if (isChildOf(idealCard_, watched)) {
      if (service_) service_->settings()->setMode(DaliNiraBlockerMode::Ideal);
      return true;
    }
    if (isChildOf(aggressiveCard_, watched)) {
      if (service_) service_->settings()->setMode(DaliNiraBlockerMode::Aggressive);
      return true;
    }
  }
  return QWidget::eventFilter(watched, event);
}
