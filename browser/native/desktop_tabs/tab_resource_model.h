#pragma once

// One browser-level model of a tab's lifecycle and resource use, shared by
// every engine. The hover card (and anything else showing tab resources)
// reads TabResourceSnapshot; engines only supply, through the shared
// TabLifecycleAdapter, which OS process renders a tab's page
// (TabLifecycleAdapter::rendererProcessId): Chromium's renderer pid, or the
// WebKit web process registered by DaliNira's web process extension.
//
// Memory attribution. Neither engine can split one content process's memory
// between the pages it hosts, so a tab gets a memory figure only when that is
// honest:
//  * Dedicated: the tab is the only open tab in its content process. The
//    process's PSS (proportional set size: private pages plus a fair share of
//    pages shared with other processes) is the tab's memory. Exact.
//  * Shared: several open tabs share the process. There is no per-tab value;
//    the process total is shown as shared, with the number of tabs.
//  * Released: the tab fully sleeps; its content process resources were
//    released (only browser-side session data remains). The last dedicated
//    figure before sleeping is kept so the saving is visible.
//  A light-sleeping (frozen) tab keeps its content process and is measured
//  like an awake one: freezing mainly stops activity, not memory.
//  * Unavailable: no content process is known (starting, or not reported).
// CPU is reported only for Dedicated tabs (the renderer's CPU time is then the
// tab's). GPU work is not attributed: Chromium does it in a GPU process shared
// by all tabs, and WebKit splits it between web and UI processes.

#include <QHash>
#include <QString>
#include <QUuid>
#include <QVector>
#include <functional>
#include <optional>

#include "engine/tab_lifecycle_state.h"

class TabManager;

namespace dalinira {

// The user-facing tab state, the same for every engine.
enum class TabActivity {
  Active,      // selected in its window
  Background,  // open but not selected, running (engines throttle hidden pages)
  LightSleep,  // frozen: paused in place, memory kept, wakes without reload
  Sleeping,    // discarded (full sleep): content process resources released
};

enum class TabMemoryAttribution { Unavailable, Dedicated, Shared, Released };

struct ProcessUsage {
  bool valid = false;
  qint64 pssBytes = 0;
  qint64 privateBytes = 0;  // Private_Clean + Private_Dirty (developer diagnostics)
  qint64 cpuTicks = 0;      // utime + stime, in clock ticks
};

// What the model needs to know about one tab, independent of its engine.
struct TabResourceInput {
  QUuid id;
  TabLifecycleState lifecycle = TabLifecycleState::Active;
  bool visible = false;
  bool audible = false;
  qint64 processId = 0;
};

struct TabResourceSnapshot {
  TabActivity activity = TabActivity::Active;
  bool audible = false;
  TabMemoryAttribution memory = TabMemoryAttribution::Unavailable;
  // Dedicated: the process PSS; Released: 0; otherwise unknown (-1).
  qint64 tabMemoryBytes = -1;
  bool tabMemoryExact = false;
  // Released: the tab's last dedicated figure before it slept (-1 if none).
  qint64 memoryBeforeSleepBytes = -1;
  // The content process, and all of its memory (every tab in it).
  qint64 processId = 0;
  qint64 processMemoryBytes = -1;
  qint64 processPrivateBytes = -1;
  int tabsInProcess = 0;
  // Percent of one CPU core; Dedicated tabs only, once two samples exist.
  std::optional<double> cpuPercent;
};

class TabResourceModel {
 public:
  using ProcessReader = std::function<ProcessUsage(qint64 pid)>;
  using Clock = std::function<qint64()>;  // milliseconds, monotonic

  explicit TabResourceModel(ProcessReader reader = &TabResourceModel::readProcessUsage, Clock clock = {});

  // The snapshot of one tab, given every open tab (for process sharing).
  // Reads only that tab's content process.
  TabResourceSnapshot snapshot(const QUuid &id, const QVector<TabResourceInput> &tabs);

  // Records the tab's current memory as its "before sleep" figure; called
  // just before the memory saver discards it (TabManager wires this).
  void noteAboutToSleep(const QUuid &id, const QVector<TabResourceInput> &tabs);

  // Every web tab of the application, from the shared tab registry and memory
  // saver; the same path for Chromium and WebKit tabs.
  static QVector<TabResourceInput> inputs(const TabManager &manager);
  // /proc/<pid>/smaps_rollup and /proc/<pid>/stat.
  static ProcessUsage readProcessUsage(qint64 pid);
  static TabActivity activityFor(TabLifecycleState lifecycle, bool visible);

 private:
  struct CpuSample {
    qint64 ticks = 0;
    qint64 atMs = 0;
  };
  ProcessReader reader_;
  Clock clock_;
  QHash<qint64, CpuSample> cpuSamples_;
  QHash<QUuid, qint64> lastDedicatedBytes_;
};

}  // namespace dalinira
