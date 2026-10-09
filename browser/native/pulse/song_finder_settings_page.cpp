#include "song_finder_settings_page.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>
#include "i18n/i18n.h"

namespace {
QString settingsPageStyleSheet() {
  return QStringLiteral(R"CSS(
    #song-finder-settings-page {
      background: #121820;
      color: #e8eef6;
    }
    #sfs-card {
      background: #1a222c;
      border: 1px solid #2e3b49;
      border-radius: 12px;
      padding: 24px;
    }
    #sfs-head-title {
      color: #f3f7fc;
      font-size: 22px;
      font-weight: 650;
      margin-bottom: 12px;
    }
    #sfs-field-label {
      color: #e7edf5;
      font-size: 14px;
      font-weight: 550;
      margin-top: 8px;
    }
    #sfs-field-hint {
      color: #91a1b2;
      font-size: 12px;
      margin-top: 2px;
      margin-bottom: 10px;
    }
    #sfs-divider {
      color: #2a3542;
      background: #2a3542;
      border: 0;
      max-height: 1px;
      margin: 14px 0;
    }
    #sfs-note {
      color: #7a8c9e;
      font-size: 12px;
      margin-top: 14px;
      margin-bottom: 14px;
    }
    QComboBox, QSpinBox {
      min-height: 32px;
      background: #111820;
      color: #e6edf5;
      border: 1px solid #3a4958;
      border-radius: 7px;
      padding: 0 10px;
    }
    QComboBox:focus, QSpinBox:focus {
      border: 2px solid #58a6c7;
    }
    QComboBox QAbstractItemView {
      background: #202a34;
      color: #e6edf5;
      selection-background-color: #324b60;
      border: 1px solid #46596b;
    }
    QCheckBox {
      color: #e7edf5;
      font-size: 13px;
      spacing: 8px;
      margin: 4px 0;
    }
    QCheckBox::indicator {
      width: 18px;
      height: 18px;
    }
    QPushButton {
      min-height: 32px;
      background: #25384a;
      color: #edf5fc;
      border: 1px solid #40576b;
      border-radius: 7px;
      padding: 2px 16px;
      font-size: 13px;
      font-weight: 550;
    }
    QPushButton:hover {
      background: #2d475d;
      border-color: #54728a;
    }
    QPushButton#sfs-save-btn {
      background: #0284c7;
      border-color: #38bdf8;
      color: #ffffff;
      font-weight: 650;
    }
    QPushButton#sfs-save-btn:hover {
      background: #0369a1;
    }
  )CSS");
}
}  // namespace

SongFinderSettingsPage::SongFinderSettingsPage(SongFinderSettings *settings, QWidget *parent)
    : QWidget(parent), settings_(settings ? settings : new SongFinderSettings(this)) {
  setObjectName(QStringLiteral("song-finder-settings-page"));
  setStyleSheet(settingsPageStyleSheet());
  setupUi();
  loadFormValues();

  connect(settings_, &SongFinderSettings::settingsChanged, this, &SongFinderSettingsPage::loadFormValues);
}

void SongFinderSettingsPage::setupUi() {
  auto *rootLayout = new QVBoxLayout(this);
  rootLayout->setContentsMargins(0, 0, 0, 0);
  rootLayout->setSpacing(0);

  auto *scrollArea = new QScrollArea(this);
  scrollArea->setWidgetResizable(true);
  scrollArea->setFrameShape(QFrame::NoFrame);
  scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

  auto *scrollContent = new QWidget(scrollArea);
  auto *outerLayout = new QHBoxLayout(scrollContent);
  outerLayout->setContentsMargins(24, 24, 24, 32);
  outerLayout->addStretch(1);

  auto *centerColumn = new QWidget(scrollContent);
  centerColumn->setMaximumWidth(760);
  centerColumn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  auto *cardLayout = new QVBoxLayout(centerColumn);
  cardLayout->setContentsMargins(0, 0, 0, 0);
  cardLayout->setSpacing(16);

  auto *card = new QFrame(centerColumn);
  card->setObjectName(QStringLiteral("sfs-card"));
  auto *bodyLayout = new QVBoxLayout(card);
  bodyLayout->setContentsMargins(24, 24, 24, 24);
  bodyLayout->setSpacing(8);

  auto *headTitle = new QLabel(I18n::text(QStringLiteral("pulse.settings_tab_title")), card);
  headTitle->setObjectName(QStringLiteral("sfs-head-title"));
  bodyLayout->addWidget(headTitle);

  // Field: Bulunan şarkıyı aç
  auto *platformLabel = new QLabel(I18n::text(QStringLiteral("pulse.open_found_song")), card);
  platformLabel->setObjectName(QStringLiteral("sfs-field-label"));
  bodyLayout->addWidget(platformLabel);

  platformCombo_ = new QComboBox(card);
  platformCombo_->addItem(QStringLiteral("YouTube"), QStringLiteral("youtube"));
  platformCombo_->addItem(QStringLiteral("YouTube Music"), QStringLiteral("ytmusic"));
  bodyLayout->addWidget(platformCombo_);

  auto *platformHint = new QLabel(I18n::text(QStringLiteral("pulse.settings.platform_desc")), card);
  platformHint->setObjectName(QStringLiteral("sfs-field-hint"));
  platformHint->setWordWrap(true);
  bodyLayout->addWidget(platformHint);

  auto *div1 = new QFrame(card);
  div1->setObjectName(QStringLiteral("sfs-divider"));
  div1->setFrameShape(QFrame::HLine);
  bodyLayout->addWidget(div1);

  // Field: Tanıma hassasiyeti
  auto *sensitivityLabel = new QLabel(I18n::text(QStringLiteral("pulse.sensitivity")), card);
  sensitivityLabel->setObjectName(QStringLiteral("sfs-field-label"));
  bodyLayout->addWidget(sensitivityLabel);

  sensitivityCombo_ = new QComboBox(card);
  sensitivityCombo_->addItem(I18n::text(QStringLiteral("pulse.sensitivity_normal")), QStringLiteral("normal"));
  sensitivityCombo_->addItem(I18n::text(QStringLiteral("pulse.sensitivity_background")), QStringLiteral("background"));
  sensitivityCombo_->addItem(I18n::text(QStringLiteral("pulse.sensitivity_max")), QStringLiteral("max"));
  sensitivityCombo_->addItem(I18n::text(QStringLiteral("common.custom")), QStringLiteral("custom"));
  connect(sensitivityCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &SongFinderSettingsPage::onSensitivityModeChanged);
  bodyLayout->addWidget(sensitivityCombo_);

  auto *sensitivityHint = new QLabel(I18n::text(QStringLiteral("pulse.settings.sensitivity_desc")), card);
  sensitivityHint->setObjectName(QStringLiteral("sfs-field-hint"));
  bodyLayout->addWidget(sensitivityHint);

  // Row with numeric inputs: İstek aralığı & Arabellek boyutu
  auto *numRow = new QWidget(card);
  auto *numLayout = new QHBoxLayout(numRow);
  numLayout->setContentsMargins(0, 0, 0, 0);
  numLayout->setSpacing(16);

  auto *col1 = new QWidget(numRow);
  auto *col1Layout = new QVBoxLayout(col1);
  col1Layout->setContentsMargins(0, 0, 0, 0);
  col1Layout->setSpacing(4);
  auto *intervalLabel = new QLabel(I18n::text(QStringLiteral("pulse.request_interval")), col1);
  intervalLabel->setObjectName(QStringLiteral("sfs-field-label"));
  col1Layout->addWidget(intervalLabel);
  requestIntervalSpin_ = new QSpinBox(col1);
  requestIntervalSpin_->setRange(1, 120);
  requestIntervalSpin_->setSuffix(QStringLiteral(" sn"));
  connect(requestIntervalSpin_, QOverload<int>::of(&QSpinBox::valueChanged), this, &SongFinderSettingsPage::onIntervalOrBufferChanged);
  col1Layout->addWidget(requestIntervalSpin_);
  auto *intervalHint = new QLabel(I18n::text(QStringLiteral("pulse.settings.interval_desc")), col1);
  intervalHint->setObjectName(QStringLiteral("sfs-field-hint"));
  col1Layout->addWidget(intervalHint);
  numLayout->addWidget(col1);

  auto *col2 = new QWidget(numRow);
  auto *col2Layout = new QVBoxLayout(col2);
  col2Layout->setContentsMargins(0, 0, 0, 0);
  col2Layout->setSpacing(4);
  auto *bufferLabel = new QLabel(I18n::text(QStringLiteral("pulse.buffer_size")), col2);
  bufferLabel->setObjectName(QStringLiteral("sfs-field-label"));
  col2Layout->addWidget(bufferLabel);
  bufferSizeSpin_ = new QSpinBox(col2);
  bufferSizeSpin_->setRange(4, 30);
  bufferSizeSpin_->setSuffix(QStringLiteral(" sn"));
  connect(bufferSizeSpin_, QOverload<int>::of(&QSpinBox::valueChanged), this, &SongFinderSettingsPage::onIntervalOrBufferChanged);
  col2Layout->addWidget(bufferSizeSpin_);
  auto *bufferHint = new QLabel(I18n::text(QStringLiteral("pulse.settings.buffer_desc")), col2);
  bufferHint->setObjectName(QStringLiteral("sfs-field-hint"));
  col2Layout->addWidget(bufferHint);
  numLayout->addWidget(col2);

  bodyLayout->addWidget(numRow);

  auto *div2 = new QFrame(card);
  div2->setObjectName(QStringLiteral("sfs-divider"));
  div2->setFrameShape(QFrame::HLine);
  bodyLayout->addWidget(div2);

  // Checkboxes
  noDuplicatesCheck_ = new QCheckBox(I18n::text(QStringLiteral("pulse.skip_duplicates")), card);
  bodyLayout->addWidget(noDuplicatesCheck_);

  webFallbackCheck_ = new QCheckBox(I18n::text(QStringLiteral("pulse.settings.fallback_desc")), card);
  bodyLayout->addWidget(webFallbackCheck_);

  autoStopCheck_ = new QCheckBox(I18n::text(QStringLiteral("pulse.stop_on_found")), card);
  bodyLayout->addWidget(autoStopCheck_);

  autoOpenCheck_ = new QCheckBox(I18n::text(QStringLiteral("pulse.settings.auto_search_desc")), card);
  bodyLayout->addWidget(autoOpenCheck_);

  rememberDeviceCheck_ = new QCheckBox(I18n::text(QStringLiteral("pulse.remember_source")), card);
  bodyLayout->addWidget(rememberDeviceCheck_);

  auto *noteLabel = new QLabel(
      I18n::text(QStringLiteral("pulse.settings.footer")),
      card);
  noteLabel->setObjectName(QStringLiteral("sfs-note"));
  noteLabel->setWordWrap(true);
  bodyLayout->addWidget(noteLabel);

  // Bottom Action Buttons
  auto *actionsRow = new QWidget(card);
  auto *actionsLayout = new QHBoxLayout(actionsRow);
  actionsLayout->setContentsMargins(0, 8, 0, 0);
  actionsLayout->setSpacing(10);
  actionsLayout->addStretch(1);

  cancelBtn_ = new QPushButton(I18n::text(QStringLiteral("common.cancel_action")), actionsRow);
  cancelBtn_->setCursor(Qt::PointingHandCursor);
  connect(cancelBtn_, &QPushButton::clicked, this, &SongFinderSettingsPage::onCancelClicked);
  actionsLayout->addWidget(cancelBtn_);

  defaultsBtn_ = new QPushButton(I18n::text(QStringLiteral("common.default")), actionsRow);
  defaultsBtn_->setCursor(Qt::PointingHandCursor);
  connect(defaultsBtn_, &QPushButton::clicked, this, &SongFinderSettingsPage::onDefaultsClicked);
  actionsLayout->addWidget(defaultsBtn_);

  saveBtn_ = new QPushButton(I18n::text(QStringLiteral("common.save")), actionsRow);
  saveBtn_->setObjectName(QStringLiteral("sfs-save-btn"));
  saveBtn_->setCursor(Qt::PointingHandCursor);
  connect(saveBtn_, &QPushButton::clicked, this, &SongFinderSettingsPage::onSaveClicked);
  actionsLayout->addWidget(saveBtn_);

  bodyLayout->addWidget(actionsRow);

  cardLayout->addWidget(card);
  outerLayout->addWidget(centerColumn);
  outerLayout->addStretch(1);

  scrollArea->setWidget(scrollContent);
  rootLayout->addWidget(scrollArea, 1);
}

void SongFinderSettingsPage::loadFormValues() {
  if (updatingForm_ || !settings_) return;
  updatingForm_ = true;

  const int platformIdx = (settings_->openPlatform() == SongFinderSettings::OpenPlatform::YouTubeMusic) ? 1 : 0;
  platformCombo_->setCurrentIndex(platformIdx);

  int sensIdx = 1;
  switch (settings_->sensitivityMode()) {
    case SongFinderSettings::SensitivityMode::Normal: sensIdx = 0; break;
    case SongFinderSettings::SensitivityMode::Background: sensIdx = 1; break;
    case SongFinderSettings::SensitivityMode::MaxAccuracy: sensIdx = 2; break;
    case SongFinderSettings::SensitivityMode::Custom: sensIdx = 3; break;
  }
  sensitivityCombo_->setCurrentIndex(sensIdx);

  requestIntervalSpin_->setValue(settings_->requestIntervalSecs());
  bufferSizeSpin_->setValue(settings_->bufferSizeSecs());

  noDuplicatesCheck_->setChecked(settings_->noDuplicates());
  webFallbackCheck_->setChecked(settings_->webMetadataFallback());
  autoStopCheck_->setChecked(settings_->autoStopOnResult());
  autoOpenCheck_->setChecked(settings_->autoOpenOnResult());
  rememberDeviceCheck_->setChecked(settings_->rememberAudioDevice());

  updatingForm_ = false;
}

void SongFinderSettingsPage::onSensitivityModeChanged(int index) {
  if (updatingForm_) return;
  updatingForm_ = true;
  if (index == 0) {
    requestIntervalSpin_->setValue(8);
    bufferSizeSpin_->setValue(10);
  } else if (index == 1) {
    requestIntervalSpin_->setValue(6);
    bufferSizeSpin_->setValue(12);
  } else if (index == 2) {
    requestIntervalSpin_->setValue(6);
    bufferSizeSpin_->setValue(16);
  }
  updatingForm_ = false;
}

void SongFinderSettingsPage::onIntervalOrBufferChanged() {
  if (updatingForm_) return;
  const int interval = requestIntervalSpin_->value();
  const int buffer = bufferSizeSpin_->value();

  updatingForm_ = true;
  if (interval == 8 && buffer == 10) {
    sensitivityCombo_->setCurrentIndex(0);
  } else if (interval == 6 && buffer == 12) {
    sensitivityCombo_->setCurrentIndex(1);
  } else if (interval == 6 && buffer == 16) {
    sensitivityCombo_->setCurrentIndex(2);
  } else {
    sensitivityCombo_->setCurrentIndex(3);  // Custom
  }
  updatingForm_ = false;
}

void SongFinderSettingsPage::onCancelClicked() {
  loadFormValues();
  emit closeTabRequested();
}

void SongFinderSettingsPage::onDefaultsClicked() {
  updatingForm_ = true;
  platformCombo_->setCurrentIndex(0);
  sensitivityCombo_->setCurrentIndex(1);
  requestIntervalSpin_->setValue(6);
  bufferSizeSpin_->setValue(12);
  noDuplicatesCheck_->setChecked(true);
  webFallbackCheck_->setChecked(true);
  autoStopCheck_->setChecked(true);
  autoOpenCheck_->setChecked(false);
  rememberDeviceCheck_->setChecked(true);
  updatingForm_ = false;
}

void SongFinderSettingsPage::onSaveClicked() {
  if (!settings_) return;

  settings_->setOpenPlatform(platformCombo_->currentIndex() == 1
                                ? SongFinderSettings::OpenPlatform::YouTubeMusic
                                : SongFinderSettings::OpenPlatform::YouTube);

  SongFinderSettings::SensitivityMode mode = SongFinderSettings::SensitivityMode::Custom;
  const int sIdx = sensitivityCombo_->currentIndex();
  if (sIdx == 0) mode = SongFinderSettings::SensitivityMode::Normal;
  else if (sIdx == 1) mode = SongFinderSettings::SensitivityMode::Background;
  else if (sIdx == 2) mode = SongFinderSettings::SensitivityMode::MaxAccuracy;

  settings_->setSensitivityMode(mode);
  settings_->setRequestIntervalSecs(requestIntervalSpin_->value());
  settings_->setBufferSizeSecs(bufferSizeSpin_->value());

  settings_->setNoDuplicates(noDuplicatesCheck_->isChecked());
  settings_->setWebMetadataFallback(webFallbackCheck_->isChecked());
  settings_->setAutoStopOnResult(autoStopCheck_->isChecked());
  settings_->setAutoOpenOnResult(autoOpenCheck_->isChecked());
  settings_->setRememberAudioDevice(rememberDeviceCheck_->isChecked());

  settings_->save();
  emit closeTabRequested();
}
