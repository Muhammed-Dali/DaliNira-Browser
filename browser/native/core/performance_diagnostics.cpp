#include "performance_diagnostics.h"

#include "audio/audio_device_manager.h"
#include "audio/web_audio_effects_controller.h"
#include "blocker/dalinira_blocker_service.h"
#include "desktop_tabs/tab_manager.h"
#include "desktop_tabs/tab_performance_manager.h"
#include "pulse/song_recognition_service.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QSet>
#include <QTextStream>

#if defined(Q_OS_WIN)
#include <windows.h>
#include <psapi.h>
#else
#include <unistd.h>
#endif

namespace {
struct ProcessMemorySample {
  qint64 rssKiB = -1;
  qint64 pssKiB = -1;
};

ProcessMemorySample measureProcess(qint64 pid) {
  ProcessMemorySample sample;
  if (pid <= 0) return sample;
#if defined(Q_OS_WIN)
  HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE,
                               static_cast<DWORD>(pid));
  if (!process) return sample;
  PROCESS_MEMORY_COUNTERS_EX counters{};
  if (GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters),
                           sizeof(counters))) {
    sample.rssKiB = static_cast<qint64>(counters.WorkingSetSize / 1024);
  }
  CloseHandle(process);
#else
  QFile rollup(QStringLiteral("/proc/%1/smaps_rollup").arg(pid));
  if (rollup.open(QIODevice::ReadOnly | QIODevice::Text)) {
    QTextStream input(&rollup);
    QString line;
    while (input.readLineInto(&line)) {
      const auto readKiB = [&line](QLatin1StringView label) -> qint64 {
        if (!line.startsWith(label)) return -1;
        const QStringList fields = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        bool ok = false;
        const qint64 value = fields.size() > 1 ? fields.at(1).toLongLong(&ok) : -1;
        return ok ? value : -1;
      };
      const qint64 rss = readKiB(QLatin1StringView("Rss:"));
      if (rss >= 0) sample.rssKiB = rss;
      const qint64 pss = readKiB(QLatin1StringView("Pss:"));
      if (pss >= 0) sample.pssKiB = pss;
    }
  }
  if (sample.rssKiB < 0) {
    QFile statm(QStringLiteral("/proc/%1/statm").arg(pid));
    if (statm.open(QIODevice::ReadOnly | QIODevice::Text)) {
      qint64 pages = 0;
      qint64 rssPages = 0;
      QTextStream input(&statm);
      input >> pages >> rssPages;
      const long pageSize = sysconf(_SC_PAGESIZE);
      if (rssPages > 0) sample.rssKiB = rssPages * (pageSize > 0 ? pageSize : 4096) / 1024;
    }
  }
#endif
  return sample;
}
}  // namespace

PerformanceDiagnostics::PerformanceDiagnostics(TabManager *tabManager,
                                               DaliNiraBlockerService *blocker,
                                               WebAudioEffectsController *audio,
                                               SongRecognitionService *songFinder,
                                               QObject *parent)
    : QObject(parent),
      tabManager_(tabManager),
      blocker_(blocker),
      audio_(audio),
      songFinder_(songFinder) {
  reportTimer_.setInterval(30 * 1000);
  connect(&reportTimer_, &QTimer::timeout, this, &PerformanceDiagnostics::reportNow);
}

void PerformanceDiagnostics::start() {
  if (reportTimer_.isActive()) return;
  reportTimer_.start();
  QTimer::singleShot(5000, this, &PerformanceDiagnostics::reportNow);
}

void PerformanceDiagnostics::reportNow() const {
  const ProcessMemorySample browserMemory = measureProcess(QCoreApplication::applicationPid());
  int webTabCount = 0;
  int frozenCount = 0;
  int discardedCount = 0;
  QSet<qint64> rendererPids;
  QStringList rendererMemory;
  quint64 deadlineChecks = 0;

  if (tabManager_ && tabManager_->performanceManager()) {
    auto *performance = tabManager_->performanceManager();
    const auto ids = performance->trackedWebTabs();
    webTabCount = ids.size();
    deadlineChecks = performance->deadlineCheckCount();
    for (const auto &id : ids) {
      const auto metadata = performance->metadata(id);
      if (metadata.lifecycleState == dalinira::TabLifecycleState::Frozen) ++frozenCount;
      if (metadata.lifecycleState == dalinira::TabLifecycleState::Discarded) ++discardedCount;
      const auto *record = tabManager_->record(id);
      if (record && record->lifecycle && record->lifecycle->rendererProcessId() > 0)
        rendererPids.insert(record->lifecycle->rendererProcessId());
    }
  }
  for (qint64 pid : rendererPids) {
    const ProcessMemorySample memory = measureProcess(pid);
    rendererMemory.append(QStringLiteral("%1:pss=%2,rss=%3")
                              .arg(pid).arg(memory.pssKiB).arg(memory.rssKiB));
  }

  const AudioDeviceManager *devices = songFinder_ ? songFinder_->deviceManager() : nullptr;
  qInfo().noquote()
      << QStringLiteral("[PERF] browser_pid=%1 browser_pss_kib=%2 browser_rss_kib=%3 "
                        "renderers=[%4] web_tabs=%5 frozen=%6 discarded=%7 "
                        "blocker_count=%8 blocker_avg_ms=%9 blocker_last_ms=%10 blocker_max_ms=%11 "
                        "audio_enabled_views=%12 audio_graph_views=%13 song_finder=%14 "
                        "pactl_monitoring=%15 pactl_polls=%16 pactl_processes=%17 lifecycle_checks=%18")
             .arg(QCoreApplication::applicationPid())
             .arg(browserMemory.pssKiB)
             .arg(browserMemory.rssKiB)
             .arg(rendererMemory.join(QLatin1Char(';')))
             .arg(webTabCount)
             .arg(frozenCount)
             .arg(discardedCount)
             .arg(blocker_ ? blocker_->evaluationCount() : 0)
             .arg(blocker_ ? blocker_->averageEvaluationTimeMs() : 0.0, 0, 'f', 4)
             .arg(blocker_ ? blocker_->lastEvaluationTimeMs() : 0.0, 0, 'f', 4)
             .arg(blocker_ ? blocker_->maxEvaluationTimeMs() : 0.0, 0, 'f', 4)
             .arg(audio_ ? audio_->audioEnabledWebViewCount() : 0)
             .arg(audio_ ? audio_->activeGraphViewCount() : 0)
             .arg(songFinder_ && songFinder_->isListening() ? QStringLiteral("active")
                                                             : QStringLiteral("idle"))
             .arg(devices && devices->isMonitoring() ? QStringLiteral("active")
                                                      : QStringLiteral("idle"))
             .arg(devices ? devices->pollCheckCount() : 0)
             .arg(devices ? devices->processLaunchCount() : 0)
             .arg(deadlineChecks);
}
