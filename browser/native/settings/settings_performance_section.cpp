#include "settings_page.h"
#include "settings_ui_helpers.h"
#include "tab_performance_manager.h"
#include "system_memory_pressure_monitor.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QAbstractItemView>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QStyle>
#include <vector>
#include "i18n/i18n.h"

using namespace dalinira::settings_ui;

QWidget *SettingsPage::createPerformanceSection() {
  Section section = makeSection(
      I18n::text(QStringLiteral("settings.category.performance")),
      I18n::text(QStringLiteral("settings.performance.description")));

  auto *perfManager = hooks_.performanceManager ? hooks_.performanceManager() : nullptr;

  // --------------------------------------------------------------------------
  // Card 1: SEKME UYKUSU — one policy for both engines (Chromium and WebKit)
  // --------------------------------------------------------------------------
  auto *modeCard = makeCard(section.page, I18n::text(QStringLiteral("settings.performance.tab_sleep_card")));

  auto *cardsContainer = new QWidget(modeCard);
  cardsContainer->setObjectName(QStringLiteral("settings-mode-container"));
  auto *cardsLayout = new QVBoxLayout(cardsContainer);
  cardsLayout->setContentsMargins(18, 14, 18, 14);
  cardsLayout->setSpacing(10);

  struct ModeCardInfo {
    dalinira::PerformancePolicyMode mode;
    QString title;
    QString badge;
    QString description;
  };

  const std::vector<ModeCardInfo> modeInfos = {
    { dalinira::PerformancePolicyMode::Off,
      I18n::text(QStringLiteral("common.off")),
      QString(),
      I18n::text(QStringLiteral("settings.performance.mode_off_desc")) },
    { dalinira::PerformancePolicyMode::Balanced,
      I18n::text(QStringLiteral("common.balanced")),
      I18n::text(QStringLiteral("common.recommended")),
      I18n::text(QStringLiteral("settings.performance.mode_balanced_desc")) },
    { dalinira::PerformancePolicyMode::MemorySaver,
      I18n::text(QStringLiteral("shield.aggressive")),
      I18n::text(QStringLiteral("settings.performance.badge_max_saving")),
      I18n::text(QStringLiteral("settings.performance.mode_aggressive_desc")) }
  };

  QVector<QFrame *> modeFrameWidgets;
  QVector<QRadioButton *> modeRadioButtons;

  QSettings preferences;
  const QString initialModeStr = preferences.value(QStringLiteral("performance/policyMode"), QStringLiteral("balanced")).toString().toLower();
  dalinira::PerformancePolicyMode currentMode = dalinira::PerformancePolicyMode::Balanced;
  if (perfManager) {
    currentMode = perfManager->policyMode();
  } else if (initialModeStr == QLatin1String("off")) {
    currentMode = dalinira::PerformancePolicyMode::Off;
  } else if (initialModeStr == QLatin1String("memory_saver")) {
    currentMode = dalinira::PerformancePolicyMode::MemorySaver;
  }
  // The legacy "maximum performance" mode is shown as Balanced (its closest option).
  if (currentMode == dalinira::PerformancePolicyMode::MaximumPerformance) currentMode = dalinira::PerformancePolicyMode::Balanced;

  auto *btnGroup = new QButtonGroup(cardsContainer);

  for (size_t i = 0; i < modeInfos.size(); ++i) {
    const auto &info = modeInfos[i];
    auto *frame = new QFrame(cardsContainer);
    frame->setObjectName(QStringLiteral("settings-mode-card"));
    frame->setProperty("selected", info.mode == currentMode);
    frame->setCursor(Qt::PointingHandCursor);

    auto *fLayout = new QHBoxLayout(frame);
    fLayout->setContentsMargins(16, 12, 16, 12);
    fLayout->setSpacing(12);

    auto *radio = new QRadioButton(frame);
    radio->setChecked(info.mode == currentMode);
    radio->setAccessibleName(info.title);
    radio->setAccessibleDescription(info.description);
    btnGroup->addButton(radio, static_cast<int>(i));
    modeRadioButtons.push_back(radio);

    auto *textBox = new QWidget(frame);
    auto *tLayout = new QVBoxLayout(textBox);
    tLayout->setContentsMargins(0, 0, 0, 0);
    tLayout->setSpacing(2);

    auto *titleRow = new QWidget(textBox);
    auto *trLayout = new QHBoxLayout(titleRow);
    trLayout->setContentsMargins(0, 0, 0, 0);
    trLayout->setSpacing(8);

    auto *titleLabel = new QLabel(info.title, titleRow);
    titleLabel->setObjectName(QStringLiteral("settings-mode-title"));
    trLayout->addWidget(titleLabel);

    if (!info.badge.isEmpty()) {
      auto *badgeLabel = new QLabel(info.badge, titleRow);
      badgeLabel->setObjectName(QStringLiteral("settings-mode-badge"));
      trLayout->addWidget(badgeLabel);
    }
    trLayout->addStretch(1);

    auto *descLabel = new QLabel(info.description, textBox);
    descLabel->setObjectName(QStringLiteral("settings-mode-desc"));
    descLabel->setWordWrap(true);

    tLayout->addWidget(titleRow);
    tLayout->addWidget(descLabel);

    fLayout->addWidget(radio, 0, Qt::AlignVCenter);
    fLayout->addWidget(textBox, 1, Qt::AlignVCenter);

    cardsLayout->addWidget(frame);
    modeFrameWidgets.push_back(frame);
  }

  auto updateModeSelection = [modeInfos, modeFrameWidgets, modeRadioButtons, perfManager](int index) {
    if (index < 0 || index >= static_cast<int>(modeInfos.size())) return;
    const auto selectedMode = modeInfos[index].mode;
    for (int i = 0; i < static_cast<int>(modeFrameWidgets.size()); ++i) {
      const bool isSelected = (i == index);
      modeFrameWidgets[i]->setProperty("selected", isSelected);
      modeFrameWidgets[i]->style()->unpolish(modeFrameWidgets[i]);
      modeFrameWidgets[i]->style()->polish(modeFrameWidgets[i]);
      if (modeRadioButtons[i]->isChecked() != isSelected) {
        modeRadioButtons[i]->setChecked(isSelected);
      }
    }
    if (perfManager) {
      perfManager->setPolicyMode(selectedMode);
    } else {
      QSettings s;
      switch (selectedMode) {
        case dalinira::PerformancePolicyMode::Off: s.setValue(QStringLiteral("performance/policyMode"), QStringLiteral("off")); break;
        case dalinira::PerformancePolicyMode::Balanced: s.setValue(QStringLiteral("performance/policyMode"), QStringLiteral("balanced")); break;
        case dalinira::PerformancePolicyMode::MemorySaver: s.setValue(QStringLiteral("performance/policyMode"), QStringLiteral("memory_saver")); break;
        case dalinira::PerformancePolicyMode::MaximumPerformance: s.setValue(QStringLiteral("performance/policyMode"), QStringLiteral("maximum_performance")); break;
      }
    }
  };

  connect(btnGroup, &QButtonGroup::idClicked, this, updateModeSelection);

  addRow(modeCard, cardsContainer);
  section.layout->addWidget(modeCard);

  // --------------------------------------------------------------------------
  // Card 2: UYKU ZAMANLAMASI (timeout and media)
  // --------------------------------------------------------------------------
  auto *timingCard = makeCard(section.page, I18n::text(QStringLiteral("settings.performance.timing_card")));
  auto *sleepAfterCombo = new QComboBox(timingCard);
  sleepAfterCombo->setObjectName(QStringLiteral("settings-sleep-after-combo"));
  sleepAfterCombo->setAccessibleName(I18n::text(QStringLiteral("settings.performance.sleep_after_accessible")));
  const QList<QPair<QString, int>> sleepChoices = {
      {I18n::text(QStringLiteral("settings.performance.sleep_after_mode_default")), 0},
      {I18n::text(QStringLiteral("time.seconds")).arg(30), 30},        {I18n::text(QStringLiteral("time.one_minute")), 60},
      {I18n::text(QStringLiteral("time.minutes")).arg(5), 5 * 60},     {I18n::text(QStringLiteral("time.minutes")).arg(15), 15 * 60},
      {I18n::text(QStringLiteral("time.minutes")).arg(30), 30 * 60},   {I18n::text(QStringLiteral("time.one_hour")), 60 * 60},
      {I18n::text(QStringLiteral("time.hours")).arg(2), 2 * 60 * 60}};
  const qlonglong currentSleepSeconds = perfManager
      ? perfManager->sleepAfterMs() / 1000
      : preferences.value(QStringLiteral("performance/sleepAfterSeconds"), 0).toLongLong();
  for (const auto &choice : sleepChoices) sleepAfterCombo->addItem(choice.first, choice.second);
  // The mode-default entry is long; sized to it, the combo squeezed the row's
  // title to one word per line. The popup still shows the full entries.
  sleepAfterCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  sleepAfterCombo->setMinimumContentsLength(18);
  sleepAfterCombo->view()->setMinimumWidth(sleepAfterCombo->view()->sizeHintForColumn(0) + 24);
  const int currentChoice = sleepAfterCombo->findData(static_cast<int>(currentSleepSeconds));
  sleepAfterCombo->setCurrentIndex(currentChoice >= 0 ? currentChoice : 0);
  connect(sleepAfterCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, [perfManager, sleepAfterCombo](int) {
    const qint64 seconds = sleepAfterCombo->currentData().toLongLong();
    if (perfManager) perfManager->setSleepAfterMs(seconds * 1000);
    else QSettings().setValue(QStringLiteral("performance/sleepAfterSeconds"), seconds);
  });
  addRow(timingCard, settingRow(
      timingCard,
      I18n::text(QStringLiteral("settings.performance.sleep_after")),
      I18n::text(QStringLiteral("settings.performance.sleep_after_desc")),
      sleepAfterCombo,
      BrowserIcon::Performance,
      false));

  auto *mediaToggle = new QCheckBox(timingCard);
  mediaToggle->setObjectName(QStringLiteral("settings-sleep-media-toggle"));
  mediaToggle->setAccessibleName(I18n::text(QStringLiteral("settings.performance.sleep_media_tabs")));
  mediaToggle->setChecked(perfManager ? perfManager->sleepMediaTabs()
                                      : preferences.value(QStringLiteral("performance/sleepMediaTabs"), false).toBool());
  connect(mediaToggle, &QCheckBox::toggled, this, [perfManager](bool checked) {
    if (perfManager) perfManager->setSleepMediaTabs(checked);
    else QSettings().setValue(QStringLiteral("performance/sleepMediaTabs"), checked);
  });
  addRow(timingCard, settingRow(
      timingCard,
      I18n::text(QStringLiteral("settings.performance.sleep_media_tabs")),
      I18n::text(QStringLiteral("settings.performance.sleep_media_tabs_desc")),
      mediaToggle,
      BrowserIcon::Audio,
      false));

  auto *sleepingLabel = new QLabel(timingCard);
  sleepingLabel->setObjectName(QStringLiteral("settings-sleeping-count-label"));
  auto updateSleepingCount = [sleepingLabel, perfManager] {
    const int count = perfManager ? perfManager->sleepingTabs().size() : 0;
    sleepingLabel->setText(count == 0 ? I18n::text(QStringLiteral("settings.performance.no_sleeping_tabs"))
                                      : I18n::text(QStringLiteral("settings.performance.sleeping_count")).arg(count));
  };
  updateSleepingCount();
  languageRefreshers_.push_back(updateSleepingCount);
  // Duration labels are composed ("%1 minutes"); relabel them after a switch.
  languageRefreshers_.push_back([sleepAfterCombo] {
    for (int i = 0; i < sleepAfterCombo->count(); ++i) {
      const int seconds = sleepAfterCombo->itemData(i).toInt();
      QString label;
      if (seconds == 0) label = I18n::text(QStringLiteral("settings.performance.sleep_after_mode_default"));
      else if (seconds < 60) label = I18n::text(QStringLiteral("time.seconds")).arg(seconds);
      else if (seconds == 60) label = I18n::text(QStringLiteral("time.one_minute"));
      else if (seconds < 3600) label = I18n::text(QStringLiteral("time.minutes")).arg(seconds / 60);
      else if (seconds == 3600) label = I18n::text(QStringLiteral("time.one_hour"));
      else label = I18n::text(QStringLiteral("time.hours")).arg(seconds / 3600);
      sleepAfterCombo->setItemText(i, label);
    }
  });
  if (perfManager) {
    connect(perfManager, &dalinira::TabPerformanceManager::tabLifecycleStateChanged, sleepingLabel,
            [updateSleepingCount](TabManager::TabId, dalinira::TabLifecycleState) { updateSleepingCount(); });
  }
  addRow(timingCard, settingRow(
      timingCard,
      I18n::text(QStringLiteral("settings.performance.sleeping_tabs")),
      I18n::text(QStringLiteral("settings.performance.never_sleep_rules")),
      sleepingLabel,
      BrowserIcon::Info,
      true));
  section.layout->addWidget(timingCard);

  // --------------------------------------------------------------------------
  // Card 3: SİTE İSTİSNALARI (Allowlist Manager)
  // --------------------------------------------------------------------------
  auto *allowlistCard = makeCard(section.page, I18n::text(QStringLiteral("settings.performance.never_sleep_card")));

  auto *allowlistContainer = new QWidget(allowlistCard);
  auto *alLayout = new QVBoxLayout(allowlistContainer);
  alLayout->setContentsMargins(18, 14, 18, 14);
  alLayout->setSpacing(12);

  auto *alDesc = new QLabel(
      I18n::text(QStringLiteral("settings.performance.never_sleep_desc")),
      allowlistContainer);
  alDesc->setObjectName(QStringLiteral("settings-row-description"));
  alDesc->setWordWrap(true);
  alLayout->addWidget(alDesc);

  auto *inputRow = new QWidget(allowlistContainer);
  auto *irLayout = new QHBoxLayout(inputRow);
  irLayout->setContentsMargins(0, 0, 0, 0);
  irLayout->setSpacing(10);

  auto *siteInput = new QLineEdit(inputRow);
  siteInput->setObjectName(QStringLiteral("settings-allowlist-input"));
  siteInput->setPlaceholderText(I18n::text(QStringLiteral("settings.performance.site_input_placeholder")));
  siteInput->setAccessibleName(I18n::text(QStringLiteral("settings.performance.site_input_accessible")));
  siteInput->setClearButtonEnabled(true);

  auto *addBtn = new QPushButton(I18n::text(QStringLiteral("common.add")), inputRow);
  addBtn->setObjectName(QStringLiteral("settings-allowlist-add"));
  addBtn->setAccessibleName(I18n::text(QStringLiteral("settings.performance.add_exception")));

  irLayout->addWidget(siteInput, 1);
  irLayout->addWidget(addBtn, 0);
  alLayout->addWidget(inputRow);

  auto *statusMsg = new QLabel(allowlistContainer);
  statusMsg->setObjectName(QStringLiteral("settings-allowlist-status"));
  statusMsg->setVisible(false);
  alLayout->addWidget(statusMsg);

  auto *siteListWidget = new QListWidget(allowlistContainer);
  siteListWidget->setObjectName(QStringLiteral("settings-allowlist-list"));
  siteListWidget->setAccessibleName(I18n::text(QStringLiteral("settings.performance.exceptions_list")));
  siteListWidget->setMinimumHeight(120);
  siteListWidget->setMaximumHeight(240);
  alLayout->addWidget(siteListWidget);

  auto refreshSiteList = [perfManager, siteListWidget]() {
    siteListWidget->clear();
    QStringList list;
    if (perfManager) {
      list = perfManager->siteAllowlist();
    } else {
      list = QSettings().value(QStringLiteral("performance/siteAllowlist")).toStringList();
    }
    for (const QString &domain : list) {
      if (domain.trimmed().isEmpty()) continue;
      auto *item = new QListWidgetItem(siteListWidget);
      auto *itemWidget = new QWidget;
      auto *iwLayout = new QHBoxLayout(itemWidget);
      iwLayout->setContentsMargins(8, 4, 8, 4);
      iwLayout->setSpacing(10);

      auto *domainLabel = new QLabel(domain, itemWidget);
      domainLabel->setObjectName(QStringLiteral("settings-row-title"));

      auto *removeBtn = new QPushButton(I18n::text(QStringLiteral("common.remove")), itemWidget);
      removeBtn->setProperty("danger", true);
      removeBtn->setAccessibleName(I18n::text(QStringLiteral("settings.performance.remove_exception")).arg(domain));
      removeBtn->setFixedSize(68, 28);

      QObject::connect(removeBtn, &QPushButton::clicked, itemWidget, [perfManager, domain, siteListWidget]() {
        QStringList current;
        if (perfManager) current = perfManager->siteAllowlist();
        else current = QSettings().value(QStringLiteral("performance/siteAllowlist")).toStringList();
        current.removeAll(domain);
        if (perfManager) perfManager->setSiteAllowlist(current);
        else QSettings().setValue(QStringLiteral("performance/siteAllowlist"), current);

        // Remove row from list
        for (int r = 0; r < siteListWidget->count(); ++r) {
          auto *it = siteListWidget->item(r);
          if (it && it->text() == domain) {
            delete siteListWidget->takeItem(r);
            break;
          }
        }
        if (siteListWidget->count() == 0) {
          auto *emptyItem = new QListWidgetItem(I18n::text(QStringLiteral("settings.performance.no_exceptions")), siteListWidget);
          emptyItem->setFlags(Qt::NoItemFlags);
        }
      });

      iwLayout->addWidget(domainLabel, 1);
      iwLayout->addWidget(removeBtn, 0);
      item->setSizeHint(QSize(0, 38));
      item->setText(domain);
      siteListWidget->setItemWidget(item, itemWidget);
    }
    if (siteListWidget->count() == 0) {
      auto *emptyItem = new QListWidgetItem(I18n::text(QStringLiteral("settings.performance.no_exceptions")), siteListWidget);
      emptyItem->setFlags(Qt::NoItemFlags);
    }
  };

  refreshSiteList();
  languageRefreshers_.push_back(refreshSiteList);

  auto handleAddSite = [siteInput, statusMsg, perfManager, refreshSiteList]() {
    const QString raw = siteInput->text().trimmed();
    statusMsg->setVisible(false);
    if (raw.isEmpty()) return;

    if (raw.contains(QLatin1String("javascript:"), Qt::CaseInsensitive) ||
        raw.contains(QLatin1String("file:"), Qt::CaseInsensitive) ||
        raw.contains(QLatin1String("data:"), Qt::CaseInsensitive) ||
        raw.contains(QLatin1Char('@'))) {
      statusMsg->setText(I18n::text(QStringLiteral("settings.performance.invalid_site")));
      statusMsg->setStyleSheet(QStringLiteral("color: #f28b82; font-size: 12px;"));
      statusMsg->setVisible(true);
      return;
    }

    const QString normalized = dalinira::TabPerformanceManager::normalizeSitePattern(raw);
    if (normalized.isEmpty() || !normalized.contains(QLatin1Char('.')) || normalized.endsWith(QLatin1Char('.'))) {
      statusMsg->setText(I18n::text(QStringLiteral("settings.performance.invalid_site")));
      statusMsg->setStyleSheet(QStringLiteral("color: #f28b82; font-size: 12px;"));
      statusMsg->setVisible(true);
      return;
    }

    QStringList current;
    if (perfManager) current = perfManager->siteAllowlist();
    else current = QSettings().value(QStringLiteral("performance/siteAllowlist")).toStringList();

    if (current.contains(normalized)) {
      statusMsg->setText(I18n::text(QStringLiteral("settings.performance.site_already_added")));
      statusMsg->setStyleSheet(QStringLiteral("color: #fdd663; font-size: 12px;"));
      statusMsg->setVisible(true);
      return;
    }

    current.append(normalized);
    if (perfManager) perfManager->setSiteAllowlist(current);
    else QSettings().setValue(QStringLiteral("performance/siteAllowlist"), current);

    siteInput->clear();
    statusMsg->setVisible(false);
    refreshSiteList();
  };

  connect(addBtn, &QPushButton::clicked, this, handleAddSite);
  connect(siteInput, &QLineEdit::returnPressed, this, handleAddSite);

  addRow(allowlistCard, allowlistContainer);
  section.layout->addWidget(allowlistCard);

  // --------------------------------------------------------------------------
  // Card 4: SİSTEM BELLEK DURUMU (Memory Status Indicator)
  // --------------------------------------------------------------------------
  auto *statusCard = makeCard(section.page, I18n::text(QStringLiteral("settings.performance.memory_status_card")));
  auto *statusLabel = new QLabel(statusCard);
  statusLabel->setObjectName(QStringLiteral("settings-memory-status-label"));

  auto updateMemoryStatus = [statusLabel, perfManager]() {
    dalinira::MemoryPressureLevel level = dalinira::MemoryPressureLevel::Normal;
    if (perfManager && perfManager->memoryPressureMonitor()) {
      level = perfManager->memoryPressureMonitor()->currentPressureLevel();
    }
    QString text;
    QString color;
    switch (level) {
      case dalinira::MemoryPressureLevel::Critical:
        text = I18n::text(QStringLiteral("settings.performance.memory_critical"));
        color = QStringLiteral("#f28b82");
        break;
      case dalinira::MemoryPressureLevel::Moderate:
        text = I18n::text(QStringLiteral("settings.performance.memory_high"));
        color = QStringLiteral("#fdd663");
        break;
      case dalinira::MemoryPressureLevel::Normal:
      default:
        text = I18n::text(QStringLiteral("settings.performance.memory_normal"));
        color = QStringLiteral("#81c995");
        break;
    }
    statusLabel->setText(text);
    statusLabel->setStyleSheet(QStringLiteral("color: %1; font-weight: 600;").arg(color));
  };

  updateMemoryStatus();

  if (perfManager && perfManager->memoryPressureMonitor()) {
    connect(perfManager->memoryPressureMonitor(), &dalinira::SystemMemoryPressureMonitor::pressureLevelChanged,
            statusLabel, [updateMemoryStatus](dalinira::MemoryPressureLevel) {
              updateMemoryStatus();
            });
  }

  addRow(statusCard, settingRow(
      statusCard,
      I18n::text(QStringLiteral("settings.performance.memory_status")),
      I18n::text(QStringLiteral("settings.performance.memory_status_desc")),
      statusLabel,
      BrowserIcon::Info,
      true));
  section.layout->addWidget(statusCard);

  section.layout->addStretch();
  return section.page;
}
