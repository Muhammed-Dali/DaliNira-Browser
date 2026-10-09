#include "desktop_tabs/tab_resource_model.h"

#include <QElapsedTimer>
#include <QFile>

#include <unistd.h>

#include "desktop_tabs/tab_lifecycle_adapter.h"
#include "desktop_tabs/tab_manager.h"
#include "desktop_tabs/tab_performance_manager.h"

namespace dalinira {

namespace {

qint64 monotonicMs() {
  static QElapsedTimer timer = [] {
    QElapsedTimer started;
    started.start();
    return started;
  }();
  return timer.elapsed();
}

// A CPU figure needs two samples at least this far apart.
constexpr qint64 kMinCpuIntervalMs = 500;

}  // namespace

TabResourceModel::TabResourceModel(ProcessReader reader, Clock clock)
    : reader_(std::move(reader)), clock_(clock ? std::move(clock) : Clock(&monotonicMs)) {}

TabActivity TabResourceModel::activityFor(TabLifecycleState lifecycle, bool visible) {
  if (lifecycle == TabLifecycleState::Discarded) return TabActivity::Sleeping;
  if (lifecycle == TabLifecycleState::Frozen) return TabActivity::LightSleep;
  return visible ? TabActivity::Active : TabActivity::Background;
}

TabResourceSnapshot TabResourceModel::snapshot(const QUuid &id, const QVector<TabResourceInput> &tabs) {
  TabResourceSnapshot result;
  const TabResourceInput *self = nullptr;
  for (const TabResourceInput &tab : tabs) {
    if (tab.id == id) self = &tab;
  }
  if (!self) return result;

  result.activity = activityFor(self->lifecycle, self->visible);
  result.audible = self->audible;

  if (result.activity == TabActivity::Sleeping) {
    // A sleeping tab has no content process of its own any more; whatever it
    // shared keeps running for the other tabs, not for this one.
    result.memory = TabMemoryAttribution::Released;
    result.tabMemoryBytes = 0;
    result.tabMemoryExact = true;
    result.memoryBeforeSleepBytes = lastDedicatedBytes_.value(id, -1);
    return result;
  }
  // Only a figure measured in the tab's current situation may be shown as
  // "before sleep"; an older one (e.g. from before it shared its process) is dropped.
  lastDedicatedBytes_.remove(id);
  if (self->processId <= 0) return result;  // Unavailable

  result.processId = self->processId;
  for (const TabResourceInput &tab : tabs) {
    if (tab.processId == self->processId && tab.lifecycle != TabLifecycleState::Discarded) ++result.tabsInProcess;
  }
  const ProcessUsage usage = reader_(self->processId);
  if (!usage.valid) return result;
  result.processMemoryBytes = usage.pssBytes;
  result.processPrivateBytes = usage.privateBytes;

  const qint64 now = clock_();
  const auto previous = cpuSamples_.constFind(self->processId);
  std::optional<double> cpu;
  if (previous != cpuSamples_.constEnd() && now - previous->atMs >= kMinCpuIntervalMs &&
      usage.cpuTicks >= previous->ticks) {
    cpu = 100.0 * static_cast<double>(usage.cpuTicks - previous->ticks) / sysconf(_SC_CLK_TCK) * 1000.0 /
          static_cast<double>(now - previous->atMs);
  }
  if (previous == cpuSamples_.constEnd() || now - previous->atMs >= kMinCpuIntervalMs)
    cpuSamples_.insert(self->processId, CpuSample{usage.cpuTicks, now});

  if (result.tabsInProcess <= 1) {
    result.memory = TabMemoryAttribution::Dedicated;
    result.tabMemoryBytes = usage.pssBytes;
    result.tabMemoryExact = true;
    result.cpuPercent = cpu;
    lastDedicatedBytes_.insert(id, usage.pssBytes);
  } else {
    result.memory = TabMemoryAttribution::Shared;
  }
  return result;
}

void TabResourceModel::noteAboutToSleep(const QUuid &id, const QVector<TabResourceInput> &tabs) {
  // A dedicated figure is recorded by snapshot() itself.
  snapshot(id, tabs);
}

QVector<TabResourceInput> TabResourceModel::inputs(const TabManager &manager) {
  QVector<TabResourceInput> result;
  const TabPerformanceManager *perf = manager.performanceManager();
  for (const TabManager::TabId &id : manager.allIds()) {
    const TabManager::TabRecord *record = manager.record(id);
    if (!record || record->kind != TabManager::TabKind::Web) continue;
    TabResourceInput input;
    input.id = id;
    input.visible = record->active;
    input.audible = record->recentlyAudible;
    if (perf && perf->hasMetadata(id)) {
      const TabPerformanceMetadata meta = perf->metadata(id);
      input.lifecycle = meta.lifecycleState;
      input.audible = input.audible || meta.recentlyAudible;
    } else if (record->lifecycle) {
      input.lifecycle = record->lifecycle->lifecycleState();
    }
    if (record->lifecycle) input.processId = record->lifecycle->rendererProcessId();
    result.append(input);
  }
  return result;
}

ProcessUsage TabResourceModel::readProcessUsage(qint64 pid) {
  ProcessUsage usage;
  if (pid <= 0) return usage;
  QFile rollup(QStringLiteral("/proc/%1/smaps_rollup").arg(pid));
  if (!rollup.open(QIODevice::ReadOnly)) return usage;
  bool havePss = false;
  for (const QByteArray &line : rollup.readAll().split('\n')) {
    const QList<QByteArray> fields = line.simplified().split(' ');
    if (fields.size() < 2) continue;
    const qint64 kb = fields.at(1).toLongLong();
    if (fields.at(0) == "Pss:") {
      usage.pssBytes = kb * 1024;
      havePss = true;
    } else if (fields.at(0) == "Private_Clean:" || fields.at(0) == "Private_Dirty:") {
      usage.privateBytes += kb * 1024;
    }
  }
  QFile stat(QStringLiteral("/proc/%1/stat").arg(pid));
  if (stat.open(QIODevice::ReadOnly)) {
    const QByteArray text = stat.readAll();
    const QList<QByteArray> fields = text.mid(text.lastIndexOf(')') + 2).split(' ');
    if (fields.size() > 12) usage.cpuTicks = fields.at(11).toLongLong() + fields.at(12).toLongLong();
  }
  usage.valid = havePss;
  return usage;
}

}  // namespace dalinira
