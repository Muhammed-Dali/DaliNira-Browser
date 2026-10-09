#include "browser_profile_service.h"
#include "settings_page.h"
#include "tab_manager.h"
#include "tab_performance_manager.h"
#include "system_memory_pressure_monitor.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QWebEngineProfile>
#include <cassert>

using namespace dalinira;

int main(int argc, char **argv) {
  QApplication app(argc, argv);

  QTemporaryDir settingsRoot;
  assert(settingsRoot.isValid());
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsRoot.path());
  app.setOrganizationName(QStringLiteral("DaliNiraTest"));
  app.setApplicationName(QStringLiteral("PerformanceSettingsTest"));

  QSettings settings;

  // Reset to clean test state
  settings.setValue(QStringLiteral("performance/policyMode"), QStringLiteral("balanced"));
  settings.setValue(QStringLiteral("performance/discardEnabled"), true);
  settings.remove(QStringLiteral("performance/siteAllowlist"));

  QTemporaryDir tempDir;
  assert(tempDir.isValid());
  BrowserProfileService profileService(tempDir.path(), nullptr, &app);

  TabManager tabManager;
  auto *perfManager = tabManager.performanceManager();
  assert(perfManager != nullptr);

  SettingsPage::Hooks hooks;
  hooks.searchEngine = [] { return QStringLiteral("Google"); };
  hooks.setSearchEngine = [](const QString &) {};
  hooks.syncNewTabs = [] {};
  hooks.refreshBookmarks = [] {};
  hooks.performanceManager = [&perfManager] { return perfManager; };

  SettingsPage page(&profileService, std::move(hooks));
  page.resize(960, 700);
  page.show();
  page.setCategory(SettingsPage::Category::Performance);
  QApplication::processEvents();

  // --------------------------------------------------------------------------
  // Test 1: Verify Initial Performance Category UI Components
  // --------------------------------------------------------------------------
  auto *modeContainer = page.findChild<QWidget *>(QStringLiteral("settings-mode-container"));
  assert(modeContainer != nullptr);

  auto modeCards = modeContainer->findChildren<QFrame *>(QStringLiteral("settings-mode-card"));
  assert(modeCards.size() == 3);

  auto radioButtons = modeContainer->findChildren<QRadioButton *>();
  assert(radioButtons.size() == 3);

  // Cards: Off (0), Balanced (1, default), Aggressive (2) — one policy for both engines.
  assert(!radioButtons[0]->isChecked());
  assert(radioButtons[1]->isChecked());
  assert(!radioButtons[2]->isChecked());
  assert(perfManager->policyMode() == PerformancePolicyMode::Balanced);

  // --------------------------------------------------------------------------
  // Test 2: Mode Switching (Aggressive, Off, Balanced)
  // --------------------------------------------------------------------------
  radioButtons[2]->click();
  QApplication::processEvents();
  assert(!radioButtons[0]->isChecked());
  assert(!radioButtons[1]->isChecked());
  assert(radioButtons[2]->isChecked());
  assert(perfManager->policyMode() == PerformancePolicyMode::MemorySaver);
  assert(settings.value(QStringLiteral("performance/policyMode")).toString() == QStringLiteral("memory_saver"));

  radioButtons[0]->click();
  QApplication::processEvents();
  assert(radioButtons[0]->isChecked());
  assert(perfManager->policyMode() == PerformancePolicyMode::Off);
  assert(!perfManager->automaticLifecycleEnabled());
  assert(settings.value(QStringLiteral("performance/policyMode")).toString() == QStringLiteral("off"));

  radioButtons[1]->click();
  QApplication::processEvents();
  assert(radioButtons[1]->isChecked());
  assert(perfManager->policyMode() == PerformancePolicyMode::Balanced);
  assert(perfManager->automaticLifecycleEnabled());
  assert(settings.value(QStringLiteral("performance/policyMode")).toString() == QStringLiteral("balanced"));

  // --------------------------------------------------------------------------
  // Test 3: Inactivity timeout and media-tab option
  // --------------------------------------------------------------------------
  auto *sleepAfter = page.findChild<QComboBox *>(QStringLiteral("settings-sleep-after-combo"));
  assert(sleepAfter != nullptr);
  assert(sleepAfter->currentData().toInt() == 0 && "mode default");
  assert(perfManager->effectiveSleepDelayMs() == TabPerformanceManager::modeDefaultSleepDelayMs(PerformancePolicyMode::Balanced));
  sleepAfter->setCurrentIndex(sleepAfter->findData(5 * 60));
  QApplication::processEvents();
  assert(perfManager->sleepAfterMs() == 5 * 60 * 1000);
  assert(perfManager->effectiveSleepDelayMs() == 5 * 60 * 1000);
  assert(settings.value(QStringLiteral("performance/sleepAfterSeconds")).toInt() == 5 * 60);

  auto *mediaToggle = page.findChild<QCheckBox *>(QStringLiteral("settings-sleep-media-toggle"));
  assert(mediaToggle != nullptr);
  assert(!mediaToggle->isChecked() && !perfManager->sleepMediaTabs());
  mediaToggle->click();
  QApplication::processEvents();
  assert(perfManager->sleepMediaTabs());
  assert(settings.value(QStringLiteral("performance/sleepMediaTabs")).toBool());
  mediaToggle->click();
  QApplication::processEvents();
  assert(!perfManager->sleepMediaTabs());
  assert(page.findChild<QLabel *>(QStringLiteral("settings-sleeping-count-label")) != nullptr);

  // --------------------------------------------------------------------------
  // Test 4: Site Exceptions Allowlist Editor
  // --------------------------------------------------------------------------
  auto *siteInput = page.findChild<QLineEdit *>(QStringLiteral("settings-allowlist-input"));
  auto *addBtn = page.findChild<QPushButton *>(QStringLiteral("settings-allowlist-add"));
  auto *siteList = page.findChild<QListWidget *>(QStringLiteral("settings-allowlist-list"));
  auto *statusMsg = page.findChild<QLabel *>(QStringLiteral("settings-allowlist-status"));

  assert(siteInput != nullptr);
  assert(addBtn != nullptr);
  assert(siteList != nullptr);
  assert(statusMsg != nullptr);

  // Initial list is empty (shows 1 placeholder item)
  assert(perfManager->siteAllowlist().isEmpty());

  // 4a. Reject invalid scheme: javascript:
  siteInput->setText(QStringLiteral("javascript:alert(1)"));
  addBtn->click();
  QApplication::processEvents();
  assert(!statusMsg->isHidden() && !statusMsg->text().isEmpty());
  assert(perfManager->siteAllowlist().isEmpty());

  // 4b. Reject invalid scheme: file:
  siteInput->setText(QStringLiteral("file:///etc/passwd"));
  addBtn->click();
  QApplication::processEvents();
  assert(!statusMsg->isHidden() && !statusMsg->text().isEmpty());
  assert(perfManager->siteAllowlist().isEmpty());

  // 4c. Reject invalid credentials URL: user@site.com
  siteInput->setText(QStringLiteral("user@site.com"));
  addBtn->click();
  QApplication::processEvents();
  assert(!statusMsg->isHidden() && !statusMsg->text().isEmpty());
  assert(perfManager->siteAllowlist().isEmpty());

  // 4d. Add valid site: https://youtube.com/watch?v=123 -> normalizes to youtube.com
  siteInput->setText(QStringLiteral("https://youtube.com/watch?v=123"));
  addBtn->click();
  QApplication::processEvents();
  assert(statusMsg->isHidden());
  assert(siteInput->text().isEmpty());
  assert(perfManager->siteAllowlist().contains(QStringLiteral("youtube.com")));
  assert(settings.value(QStringLiteral("performance/siteAllowlist")).toStringList().contains(QStringLiteral("youtube.com")));

  // 4e. Reject duplicate addition
  siteInput->setText(QStringLiteral("https://www.youtube.com"));
  addBtn->click();
  QApplication::processEvents();
  assert(!statusMsg->isHidden() && !statusMsg->text().isEmpty());
  assert(perfManager->siteAllowlist().size() == 1);

  // 4f. Add second site: github.com
  siteInput->setText(QStringLiteral("github.com"));
  addBtn->click();
  QApplication::processEvents();
  assert(statusMsg->isHidden());
  assert(perfManager->siteAllowlist().size() == 2);
  assert(perfManager->siteAllowlist().contains(QStringLiteral("github.com")));

  // 4g. Remove youtube.com via [Kaldır] button
  bool removed = false;
  for (int r = 0; r < siteList->count(); ++r) {
    auto *item = siteList->item(r);
    if (item && item->text() == QStringLiteral("youtube.com")) {
      auto *w = siteList->itemWidget(item);
      assert(w != nullptr);
      auto *removeBtn = w->findChild<QPushButton *>();
      assert(removeBtn != nullptr);
      removeBtn->click();
      QApplication::processEvents();
      removed = true;
      break;
    }
  }
  assert(removed);
  assert(!perfManager->siteAllowlist().contains(QStringLiteral("youtube.com")));
  assert(perfManager->siteAllowlist().contains(QStringLiteral("github.com")));
  assert(perfManager->siteAllowlist().size() == 1);

  // --------------------------------------------------------------------------
  // Test 5: Memory Pressure Dynamic Status
  // --------------------------------------------------------------------------
  auto *statusLabel = page.findChild<QLabel *>(QStringLiteral("settings-memory-status-label"));
  assert(statusLabel != nullptr);

  // Normal
  perfManager->memoryPressureMonitor()->setSimulatedPressureLevel(MemoryPressureLevel::Normal);
  QApplication::processEvents();
  assert(statusLabel->text() == QStringLiteral("Bellek kullanımı normal"));

  // Moderate
  perfManager->memoryPressureMonitor()->setSimulatedPressureLevel(MemoryPressureLevel::Moderate);
  QApplication::processEvents();
  assert(statusLabel->text() == QStringLiteral("Bellek kullanımı yüksek"));

  // Critical
  perfManager->memoryPressureMonitor()->setSimulatedPressureLevel(MemoryPressureLevel::Critical);
  QApplication::processEvents();
  assert(statusLabel->text() == QStringLiteral("Bellek kullanımı çok yüksek"));

  // --------------------------------------------------------------------------
  // Test 6: Reset Performance Preferences
  // --------------------------------------------------------------------------
  page.setCategory(SettingsPage::Category::Reset);
  QApplication::processEvents();

  auto *resetPerfCard = page.findChild<QWidget *>(QStringLiteral("settings-section"));
  assert(resetPerfCard != nullptr);

  // Find the Reset Performance button
  QPushButton *resetPerfBtn = nullptr;
  for (auto *btn : page.findChildren<QPushButton *>()) {
    if (btn->text().contains(QStringLiteral("Performans ayarlarını sıfırla"))) {
      resetPerfBtn = btn;
      break;
    }
  }
  assert(resetPerfBtn != nullptr);

  // Change settings before reset
  perfManager->setPolicyMode(PerformancePolicyMode::MemorySaver);
  perfManager->setDiscardEnabled(false);
  perfManager->setSleepMediaTabs(true);
  assert(perfManager->policyMode() == PerformancePolicyMode::MemorySaver);
  assert(!perfManager->isDiscardEnabled());

  // Click reset
  resetPerfBtn->click();
  QApplication::processEvents();

  assert(perfManager->policyMode() == PerformancePolicyMode::Balanced);
  assert(perfManager->isDiscardEnabled());
  assert(perfManager->siteAllowlist().isEmpty());
  assert(perfManager->sleepAfterMs() == 0);
  assert(!perfManager->sleepMediaTabs());

  // --------------------------------------------------------------------------
  // Test 7: Zero Forbidden Technical Terms in UI
  // --------------------------------------------------------------------------
  page.setCategory(SettingsPage::Category::Performance);
  QApplication::processEvents();

  const QStringList forbiddenTerms = {
    QStringLiteral("Frozen"),
    QStringLiteral("Discarded"),
    QStringLiteral("LifecycleState"),
    QStringLiteral("recommendedState"),
    QStringLiteral("renderer process"),
    QStringLiteral("memory pressure"),
    QStringLiteral("QWebEnginePage"),
    QStringLiteral("RSS"),
    QStringLiteral("PSS")
  };

  for (const auto *lbl : page.findChildren<QLabel *>()) {
    for (const auto &term : forbiddenTerms) {
      assert(!lbl->text().contains(term, Qt::CaseInsensitive));
    }
  }

  return 0;
}
