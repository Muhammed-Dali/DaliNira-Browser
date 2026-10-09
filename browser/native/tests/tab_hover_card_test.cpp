// Tab hover card and the shared tab resource model it renders
// (desktop_tabs/tab_resource_model.h). The model is engine-neutral: these
// tests feed it the same inputs either engine produces (a content process id
// per tab plus the shared lifecycle state) and a fake /proc reader, so every
// attribution rule is checked without starting an engine.
#include "desktop_tabs/tab_hover_card.h"
#include "desktop_tabs/tab_resource_model.h"
#include "i18n/i18n.h"
#include "i18n/language_manager.h"

#include <cassert>
#include <iostream>

#include <QApplication>
#include <QHash>
#include <QIcon>
#include <QLabel>
#include <QFontMetrics>
#include <QRect>
#include <QSettings>
#include <QString>
#include <QUrl>
#include <QUuid>
#include <QVector>

#include <unistd.h>

using dalinira::ProcessUsage;
using dalinira::TabActivity;
using dalinira::TabLifecycleState;
using dalinira::TabMemoryAttribution;
using dalinira::TabResourceInput;
using dalinira::TabResourceModel;
using dalinira::TabResourceSnapshot;

namespace {

constexpr qint64 kMiB = 1024 * 1024;

// A fake /proc: pid -> usage, and a controllable clock.
struct FakeSystem {
  QHash<qint64, ProcessUsage> processes;
  qint64 nowMs = 0;
  TabResourceModel model() {
    return TabResourceModel([this](qint64 pid) { return processes.value(pid); }, [this] { return nowMs; });
  }
  void setProcess(qint64 pid, qint64 pssMiB, qint64 ticks = 0) {
    processes.insert(pid, ProcessUsage{true, pssMiB * kMiB, pssMiB * kMiB * 3 / 4, ticks});
  }
};

TabResourceInput tab(const QUuid &id, qint64 pid, TabLifecycleState state = TabLifecycleState::Active,
                     bool visible = false) {
  TabResourceInput input;
  input.id = id;
  input.processId = pid;
  input.lifecycle = state;
  input.visible = visible;
  return input;
}

// None of the user-facing lines may carry process-level jargon or mislabel
// memory as CPU/GPU.
void assertUserFacingWording(const dalinira::TabResourceText &text) {
  for (const QString &line : {text.activity, text.memory, text.sharedMemory, text.cpu}) {
    assert(!line.contains(QLatin1String("PSS")) && !line.contains(QLatin1String("Renderer")));
    assert(!line.contains(QLatin1String("GPU")));
  }
  assert(text.memory.startsWith(QStringLiteral("Bellek kullanımı")));
  assert(!text.memory.contains(QStringLiteral("İşlemci")));
}

void testDedicatedProcessIsExact() {
  std::cout << "[TEST] A tab alone in its content process gets that process's memory, exactly...\n";
  FakeSystem system;
  system.setProcess(100, 320);
  auto model = system.model();
  const QUuid a = QUuid::createUuid();
  const QUuid internal = QUuid::createUuid();
  const TabResourceSnapshot s = model.snapshot(a, {tab(a, 100, TabLifecycleState::Active, true), tab(internal, 0)});
  assert(s.activity == TabActivity::Active);
  assert(s.memory == TabMemoryAttribution::Dedicated && s.tabMemoryExact);
  assert(s.tabMemoryBytes == 320 * kMiB && s.processMemoryBytes == 320 * kMiB && s.tabsInProcess == 1);
  const auto text = dalinira::describeTabResources(s);
  assert(text.activity == QStringLiteral("Durum: Aktif"));
  assert(text.memory == QStringLiteral("Bellek kullanımı: 320 MB"));
  assert(text.sharedMemory.isEmpty());
  assertUserFacingWording(text);
  std::cout << "  -> PASS: \"" << text.memory.toStdString() << "\"\n";
}

void testSharedProcessIsNeverAttributedToOneTab() {
  std::cout << "[TEST] A shared content process is shown as shared, never as one tab's memory...\n";
  FakeSystem system;
  system.setProcess(200, 1434);  // 1.4 GB, the reported case
  auto model = system.model();
  const QUuid a = QUuid::createUuid();
  const QUuid b = QUuid::createUuid();
  const QVector<TabResourceInput> tabs{tab(a, 200, TabLifecycleState::Active, true), tab(b, 200)};
  const TabResourceSnapshot s = model.snapshot(a, tabs);
  assert(s.memory == TabMemoryAttribution::Shared && !s.tabMemoryExact);
  assert(s.tabMemoryBytes < 0 && "no per-tab value is invented, not even a division by tab count");
  assert(s.processMemoryBytes == 1434 * kMiB && s.tabsInProcess == 2);
  assert(!s.cpuPercent.has_value() && "process CPU is not shown as one tab's");
  const auto text = dalinira::describeTabResources(s);
  assert(!text.memory.contains(QStringLiteral("GB")) && !text.memory.contains(QStringLiteral("MB")));
  assert(text.sharedMemory == QStringLiteral("Paylaşılan işlem belleği: 1.4 GB (2 sekme)"));
  assertUserFacingWording(text);
  // The other tab of the process sees the same shared figure.
  const TabResourceSnapshot other = model.snapshot(b, tabs);
  assert(other.activity == TabActivity::Background && other.memory == TabMemoryAttribution::Shared);
  std::cout << "  -> PASS: \"" << text.memory.toStdString() << "\" / \"" << text.sharedMemory.toStdString() << "\"\n";
}

void testClosingOneOfTwoTabsMakesTheOtherDedicated() {
  std::cout << "[TEST] Closing one of two tabs sharing a process leaves the other with the whole process...\n";
  FakeSystem system;
  system.setProcess(300, 500);
  auto model = system.model();
  const QUuid a = QUuid::createUuid();
  const QUuid b = QUuid::createUuid();
  assert(model.snapshot(a, {tab(a, 300), tab(b, 300)}).memory == TabMemoryAttribution::Shared);
  system.setProcess(300, 410);  // b's page is gone from the process
  const TabResourceSnapshot s = model.snapshot(a, {tab(a, 300)});
  assert(s.memory == TabMemoryAttribution::Dedicated && s.tabMemoryBytes == 410 * kMiB);
  std::cout << "  -> PASS\n";
}

void testSleepReleasesAndWakeMeasuresAgain() {
  std::cout << "[TEST] Sleeping shows the released memory; waking measures again...\n";
  FakeSystem system;
  system.setProcess(400, 280);
  auto model = system.model();
  const QUuid a = QUuid::createUuid();
  const QUuid b = QUuid::createUuid();
  // Awake in the background, measured (as the memory saver does just before sleeping).
  model.noteAboutToSleep(a, {tab(a, 400), tab(b, 500, TabLifecycleState::Active, true)});
  // Asleep: no content process any more.
  system.processes.remove(400);
  const TabResourceSnapshot asleep = model.snapshot(a, {tab(a, 0, TabLifecycleState::Discarded)});
  assert(asleep.activity == TabActivity::Sleeping);
  assert(asleep.memory == TabMemoryAttribution::Released && asleep.tabMemoryBytes == 0 && asleep.tabMemoryExact);
  assert(asleep.memoryBeforeSleepBytes == 280 * kMiB);
  const auto text = dalinira::describeTabResources(asleep);
  assert(text.activity == QStringLiteral("Durum: Uyuyor (bellekten çıkarıldı)"));
  assert(text.memory == QStringLiteral("Bellek kullanımı: boşaltıldı"));
  assert(text.memoryBeforeSleep == QStringLiteral("Uyumadan önce: 280 MB"));
  assertUserFacingWording(text);
  // A sleeping tab that still reports a pid (shared renderer) is not counted in it.
  system.setProcess(500, 900);
  const TabResourceSnapshot sharer =
      model.snapshot(b, {tab(a, 500, TabLifecycleState::Discarded), tab(b, 500, TabLifecycleState::Active, true)});
  assert(sharer.memory == TabMemoryAttribution::Dedicated && sharer.tabsInProcess == 1);
  // Awake again in a new process: measured afresh.
  system.setProcess(401, 190);
  const TabResourceSnapshot awake = model.snapshot(a, {tab(a, 401, TabLifecycleState::Active, true)});
  assert(awake.activity == TabActivity::Active && awake.memory == TabMemoryAttribution::Dedicated &&
         awake.tabMemoryBytes == 190 * kMiB);
  // Exact while alone, then shared when it went to sleep: no "before" figure,
  // since the earlier one no longer described the tab.
  const QUuid c = QUuid::createUuid();
  const QUuid d = QUuid::createUuid();
  system.setProcess(700, 350);
  assert(model.snapshot(c, {tab(c, 700)}).memory == TabMemoryAttribution::Dedicated);
  model.noteAboutToSleep(c, {tab(c, 700), tab(d, 700, TabLifecycleState::Active, true)});
  const TabResourceSnapshot sleptShared = model.snapshot(c, {tab(c, 0, TabLifecycleState::Discarded)});
  assert(sleptShared.memoryBeforeSleepBytes < 0);
  assert(dalinira::describeTabResources(sleptShared).memory == QStringLiteral("Bellek kullanımı: boşaltıldı"));
  // Waking with no process reported yet: measuring, not a stale value.
  const TabResourceSnapshot starting = model.snapshot(a, {tab(a, 0, TabLifecycleState::Active, true)});
  assert(starting.memory == TabMemoryAttribution::Unavailable);
  assert(dalinira::describeTabResources(starting).memory == QStringLiteral("Bellek kullanımı: ölçülüyor…"));
  std::cout << "  -> PASS: \"" << text.memory.toStdString() << "\"\n";
}

void testLightSleepKeepsMemoryAndIsNamedSo() {
  std::cout << "[TEST] A light-sleeping (frozen) tab is shown as such and still measured...\n";
  FakeSystem system;
  system.setProcess(800, 240);
  auto model = system.model();
  const QUuid a = QUuid::createUuid();
  const TabResourceSnapshot s = model.snapshot(a, {tab(a, 800, TabLifecycleState::Frozen)});
  assert(s.activity == TabActivity::LightSleep);
  assert(s.memory == TabMemoryAttribution::Dedicated && s.tabMemoryBytes == 240 * kMiB && "freezing keeps the page in memory");
  const auto text = dalinira::describeTabResources(s);
  assert(text.activity == QStringLiteral("Durum: Hafif uyku (etkinlik kısıtlı)"));
  assert(text.memory == QStringLiteral("Bellek kullanımı: 240 MB"));
  assertUserFacingWording(text);
  // A frozen tab still shares its process with the other tabs in it.
  const QUuid b = QUuid::createUuid();
  assert(model.snapshot(a, {tab(a, 800, TabLifecycleState::Frozen), tab(b, 800)}).tabsInProcess == 2);
  std::cout << "  -> PASS: \"" << text.activity.toStdString() << "\" / \"" << text.memory.toStdString() << "\"\n";
}

void testCpuOnlyForDedicatedProcesses() {
  std::cout << "[TEST] CPU appears only for a dedicated process, after two samples...\n";
  FakeSystem system;
  auto model = system.model();
  const QUuid a = QUuid::createUuid();
  system.setProcess(600, 100, 1000);
  assert(!model.snapshot(a, {tab(a, 600)}).cpuPercent.has_value() && "one sample is not a rate");
  system.nowMs += 1000;
  const long ticksPerSecond = sysconf(_SC_CLK_TCK);
  system.setProcess(600, 100, 1000 + ticksPerSecond / 4);  // 0.25 s of CPU in 1 s
  const TabResourceSnapshot s = model.snapshot(a, {tab(a, 600)});
  assert(s.cpuPercent.has_value() && qAbs(*s.cpuPercent - 25.0) < 1.0);
  const auto text = dalinira::describeTabResources(s);
  assert(text.cpu == QStringLiteral("İşlemci kullanımı: %25"));
  assertUserFacingWording(text);
  std::cout << "  -> PASS: \"" << text.cpu.toStdString() << "\"\n";
}

void testWindowFlagsAndFlickerAttributes() {
  std::cout << "[TEST] Hover card window flags...\n";
  TabHoverCard card;
  assert(card.testAttribute(Qt::WA_ShowWithoutActivating));
  assert(card.testAttribute(Qt::WA_TransparentForMouseEvents));
  const Qt::WindowFlags flags = card.windowFlags();
  assert(flags & Qt::ToolTip);
  assert(flags & Qt::FramelessWindowHint);
  assert(flags & Qt::WindowTransparentForInput);
  std::cout << "  -> PASS\n";
}

void testDomainExtraction() {
  std::cout << "[TEST] Domain extraction...\n";
  assert(TabHoverCard::extractDomain(QUrl(QStringLiteral("dalinira://newtab"))) == QStringLiteral("DaliNiraBrowser"));
  assert(TabHoverCard::extractDomain(QUrl(QStringLiteral("dalinira://settings"))) == QStringLiteral("Ayarlar"));
  assert(TabHoverCard::extractDomain(QUrl(QStringLiteral("dalinira://passwords"))) == QStringLiteral("Şifre Yöneticisi"));
  assert(TabHoverCard::extractDomain(QUrl(QStringLiteral("dalinira://downloads"))) == QStringLiteral("İndirmeler"));
  assert(TabHoverCard::extractDomain(QUrl(QStringLiteral("https://www.google.com/search?q=test"))) == QStringLiteral("google.com"));
  assert(TabHoverCard::extractDomain(QUrl(QStringLiteral("https://github.com/openai"))) == QStringLiteral("github.com"));
  std::cout << "  -> PASS\n";
}

QString labelText(TabHoverCard &card, const char *name) {
  auto *label = card.findChild<QLabel *>(QLatin1String(name));
  assert(label);
  return label->parentWidget()->isVisible() ? label->text() : QString();
}

void assertVisibleTextFits(TabHoverCard &card) {
  for (QLabel *label : card.findChildren<QLabel *>()) {
    if (!label->isVisible() || label->text().isEmpty()) continue;
    const QRect labelRect(label->mapTo(&card, QPoint(0, 0)), label->size());
    assert(card.rect().contains(labelRect));
    if (!label->wordWrap()) continue;
    const int required = QFontMetrics(label->font())
                             .boundingRect(QRect(0, 0, label->width(), 10000),
                                           Qt::TextWordWrap | Qt::AlignLeft, label->text())
                             .height();
    if (label->height() < required) {
      std::cerr << "clipped label " << label->objectName().toStdString()
                << ": size=" << label->width() << "x" << label->height()
                << " required=" << required << " text=" << label->text().toStdString() << "\n";
    }
    assert(label->height() >= required && "a visible hover-card line is clipped");
  }
}

void testContentSizingTranslationsAndBounds() {
  std::cout << "[TEST] Content sizing, translations and edge placement...\n";
  auto &languages = dalinira::i18n::LanguageManager::instance();
  languages.initialize();
  const QString oldPreference = languages.languagePreference();
  TabHoverCard card;
  const QRect bounds(100, 50, 900, 600);

  const QRect first = TabHoverCard::boundedCardGeometry(
      QRect(100, 60, 120, 36), QSize(320, 180), bounds);
  const QRect last = TabHoverCard::boundedCardGeometry(
      QRect(940, 60, 60, 36), QSize(320, 180), bounds);
  const QRect bottom = TabHoverCard::boundedCardGeometry(
      QRect(500, 620, 120, 30), QSize(320, 180), bounds);
  assert(bounds.contains(first) && bounds.contains(last) && bounds.contains(bottom));
  assert(first.left() == bounds.left() + 8);
  assert(last.right() == bounds.right() - 8);
  assert(bottom.bottom() < QRect(500, 620, 120, 30).top());

  TabResourceSnapshot snapshot;
  snapshot.activity = TabActivity::LightSleep;
  snapshot.memory = TabMemoryAttribution::Shared;
  snapshot.processId = 84;
  snapshot.processMemoryBytes = 1434 * kMiB;
  snapshot.tabsInProcess = 12;
  const QString longTitle = QStringLiteral(
      "A deliberately very long tab title that must occupy no more than two clean lines even with translated resource information");

  for (const QString &language : {QStringLiteral("tr"), QStringLiteral("en"),
                                  QStringLiteral("ar"), QStringLiteral("fr")}) {
    languages.setLanguagePreference(language);
    card.showForTab(longTitle, QUrl(QStringLiteral("https://very-long-example-domain.test/path")),
                    QIcon(), QRect(300, 80, 180, 36), [&] { return snapshot; });
    QApplication::processEvents();
    assert(card.width() == 320);
    assert(card.layoutDirection() == Qt::LeftToRight);
    assert(!labelText(card, "hover-lifecycle").isEmpty());
    assert(!labelText(card, "hover-memory").isEmpty());
    assert(!labelText(card, "hover-shared-memory").isEmpty());
    assertVisibleTextFits(card);
  }

  snapshot = TabResourceSnapshot{};
  snapshot.memory = TabMemoryAttribution::Unavailable;
  card.refreshCardInfo();
  QApplication::processEvents();
  const int compactHeight = card.height();
  assertVisibleTextFits(card);

  snapshot.memory = TabMemoryAttribution::Dedicated;
  snapshot.tabMemoryBytes = 320 * kMiB;
  snapshot.cpuPercent = 37.0;
  card.refreshCardInfo();
  QApplication::processEvents();
  assert(card.height() > compactHeight);
  assert(!labelText(card, "hover-cpu").isEmpty());
  assertVisibleTextFits(card);

  snapshot.activity = TabActivity::Sleeping;
  snapshot.memory = TabMemoryAttribution::Released;
  snapshot.memoryBeforeSleepBytes = 473 * kMiB;
  snapshot.cpuPercent.reset();
  card.refreshCardInfo();
  QApplication::processEvents();
  assert(!labelText(card, "hover-memory-before-sleep").isEmpty());
  assert(labelText(card, "hover-diagnostics").isEmpty());
  assertVisibleTextFits(card);

  card.hideCard();
  languages.setLanguagePreference(oldPreference);
  std::cout << "  -> PASS\n";
}

void testCardRendersTheSharedModelAndFollowsIt() {
  std::cout << "[TEST] The card renders the snapshot and follows sleep and wake...\n";
  TabHoverCard card;
  const QRect tabRect(100, 100, 160, 36);

  card.showForTab(QStringLiteral("Ayarlar"), QUrl(QStringLiteral("dalinira://settings")), QIcon(), tabRect, {}, true);
  assert(labelText(card, "hover-lifecycle") == I18n::text(QStringLiteral("tab.hover.internal_state")));
  assert(labelText(card, "hover-memory") == I18n::text(QStringLiteral("tab.hover.internal_page")));

  TabResourceSnapshot current;
  current.activity = TabActivity::Background;
  current.memory = TabMemoryAttribution::Shared;
  current.processId = 42;
  current.processMemoryBytes = 1434 * kMiB;
  current.tabsInProcess = 2;
  card.showForTab(QStringLiteral("GitHub"), QUrl(QStringLiteral("https://github.com")), QIcon(), tabRect,
                  [&] { return current; });
  assert(card.isVisible());
  assert(labelText(card, "hover-lifecycle") == QStringLiteral("Durum: Arka planda"));
  assert(labelText(card, "hover-memory") == QStringLiteral("Bellek kullanımı: sekmeye ayrı ölçülemiyor"));
  assert(labelText(card, "hover-shared-memory") == QStringLiteral("Paylaşılan işlem belleği: 1.4 GB (2 sekme)"));
  assert(labelText(card, "hover-cpu").isEmpty());
  assert(labelText(card, "hover-diagnostics").isEmpty() && "developer diagnostics are opt-in");

  current = TabResourceSnapshot{};
  current.activity = TabActivity::Sleeping;
  current.memory = TabMemoryAttribution::Released;
  current.tabMemoryBytes = 0;
  current.memoryBeforeSleepBytes = 300 * kMiB;
  card.refreshCardInfo();  // what the poll timer does while the card is open
  assert(labelText(card, "hover-lifecycle") == QStringLiteral("Durum: Uyuyor (bellekten çıkarıldı)"));
  assert(labelText(card, "hover-memory") == QStringLiteral("Bellek kullanımı: boşaltıldı"));
  assert(labelText(card, "hover-memory-before-sleep") == QStringLiteral("Uyumadan önce: 300 MB"));
  assert(labelText(card, "hover-shared-memory").isEmpty());

  current = TabResourceSnapshot{};
  current.memory = TabMemoryAttribution::Dedicated;
  current.tabMemoryBytes = 250 * kMiB;
  current.cpuPercent = 3.4;
  card.refreshCardInfo();
  assert(labelText(card, "hover-lifecycle") == QStringLiteral("Durum: Aktif"));
  assert(labelText(card, "hover-memory") == QStringLiteral("Bellek kullanımı: 250 MB"));
  assert(labelText(card, "hover-cpu") == QStringLiteral("İşlemci kullanımı: %3"));

  card.hideCard();
  assert(!card.isVisible());
  card.refreshCardInfo();  // no provider any more: must not crash
  std::cout << "  -> PASS\n";
}

}  // namespace

int main(int argc, char *argv[]) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app(argc, argv);
  std::cout << "=== TAB HOVER CARD AND RESOURCE MODEL TESTS ===\n";
  testDedicatedProcessIsExact();
  testSharedProcessIsNeverAttributedToOneTab();
  testClosingOneOfTwoTabsMakesTheOtherDedicated();
  testSleepReleasesAndWakeMeasuresAgain();
  testLightSleepKeepsMemoryAndIsNamedSo();
  testCpuOnlyForDedicatedProcesses();
  testWindowFlagsAndFlickerAttributes();
  testDomainExtraction();
  testCardRendersTheSharedModelAndFollowsIt();
  testContentSizingTranslationsAndBounds();
  std::cout << "=== ALL TAB HOVER CARD AND RESOURCE MODEL TESTS PASSED ===\n";
  return 0;
}
