#include "audio_device_manager.h"
#include "core/application_identity.h"

#include "security_utils.h"

#include <QCoreApplication>
#include <QDir>
#include <QHash>
#include <QProcess>
#include <QRegularExpression>
#include <QTimer>
#include <algorithm>

AudioDeviceManager::AudioDeviceManager(QObject *parent) : QObject(parent) {
#if defined(Q_OS_WIN)
  const QString executableName = QStringLiteral("pactl.exe");
#else
  const QString executableName = QStringLiteral("pactl");
#endif
  const QString appDir = dalinira::application_identity::applicationDirectory();
  pactlPath_ = BrowserSecurity::resolveTrustedExecutable(
      executableName, {QDir(appDir).filePath(executableName), QDir(appDir).filePath(QStringLiteral("bin/") + executableName)});
  pollTimer_ = new QTimer(this);
  pollTimer_->setInterval(1500);  // Check route changes every 1.5s
  connect(pollTimer_, &QTimer::timeout, this, &AudioDeviceManager::checkDeviceChanges);
}

AudioDeviceManager::~AudioDeviceManager() {
  if (pollTimer_) {
    pollTimer_->stop();
  }
}

QString AudioDeviceManager::executeCommand(const QStringList &arguments, int timeoutMs) const {
  if (pactlPath_.isEmpty()) return {};
  QProcess process;
  ++processLaunchCount_;
  process.start(pactlPath_, arguments);
  if (!process.waitForFinished(timeoutMs)) {
    process.kill();
    process.waitForFinished(500);
    return {};
  }
  if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
    return {};
  }
  return QString::fromUtf8(process.readAllStandardOutput()).trimmed();
}

QPair<QString, QString> AudioDeviceManager::queryDefaultPulseRoute() const {
  const QString info = executeCommand({QStringLiteral("info")});
  QString sink;
  QString source;
  const QStringList lines = info.split(QRegularExpression(QStringLiteral("[\r\n]+")), Qt::SkipEmptyParts);
  for (const QString &line : lines) {
    if (line.startsWith(QStringLiteral("Default Source:"), Qt::CaseInsensitive)) {
      source = line.section(QLatin1Char(':'), 1).trimmed();
    } else if (line.startsWith(QStringLiteral("Default Sink:"), Qt::CaseInsensitive)) {
      sink = line.section(QLatin1Char(':'), 1).trimmed();
    }
  }
  return {sink, source};
}

void AudioDeviceManager::startMonitoring() {
  if (pollTimer_ && !pollTimer_->isActive()) pollTimer_->start();
}

void AudioDeviceManager::stopMonitoring() {
  if (pollTimer_) pollTimer_->stop();
}

bool AudioDeviceManager::isMonitoring() const {
  return pollTimer_ && pollTimer_->isActive();
}

struct SourceDetail {
  QString description;
  QString driver;
  QString cardBusId;
  QString cardName;
  QString monitorOfSink;
  bool isMonitor = false;
  int channels = 2;
};

QVector<AudioDeviceInfo> AudioDeviceManager::listDevices() {
  const auto defaults = queryDefaultPulseRoute();
  const QString defaultSink = defaults.first;
  const QString defaultSource = defaults.second;
  lastDefaultSink_ = defaultSink;
  lastDefaultSource_ = defaultSource;
  const QString defaultSinkMonitor = defaultSink.isEmpty() ? QString() : (defaultSink + QStringLiteral(".monitor"));

  QHash<QString, SourceDetail> detailedDescriptions;
  const QString fullList = executeCommand({QStringLiteral("list"), QStringLiteral("sources")});
  if (!fullList.isEmpty()) {
    const QStringList blocks = fullList.split(QRegularExpression(QStringLiteral("(?=Source #)")), Qt::SkipEmptyParts);
    for (const QString &block : blocks) {
      const QRegularExpression nameRe(QStringLiteral("^\\s*Name:\\s*(.+)$"), QRegularExpression::MultilineOption);
      const auto nameMatch = nameRe.match(block);
      if (!nameMatch.hasMatch()) continue;
      const QString name = nameMatch.captured(1).trimmed();

      const QRegularExpression descRe(QStringLiteral("^\\s*Description:\\s*(.+)$"), QRegularExpression::MultilineOption);
      const auto descMatch = descRe.match(block);
      const QString description = descMatch.hasMatch() ? descMatch.captured(1).trimmed() : QString();

      const QRegularExpression driverRe(QStringLiteral("^\\s*Driver:\\s*(.+)$"), QRegularExpression::MultilineOption);
      const auto driverMatch = driverRe.match(block);
      const QString driver = driverMatch.hasMatch() ? driverMatch.captured(1).trimmed() : QString();

      const QRegularExpression monitorRe(QStringLiteral("^\\s*Monitor of Sink:\\s*(.+)$"), QRegularExpression::MultilineOption);
      const auto monitorMatch = monitorRe.match(block);
      const QString monitorOf = monitorMatch.hasMatch() ? monitorMatch.captured(1).trimmed() : QString();
      const bool isMonitor = (!monitorOf.isEmpty() && monitorOf.compare(QStringLiteral("n/a"), Qt::CaseInsensitive) != 0);

      const QRegularExpression busIdRe(QStringLiteral("device\\.bus-id\\s*=\\s*\"([^\"]+)\""));
      const auto busIdMatch = busIdRe.match(block);
      const QString cardBusId = busIdMatch.hasMatch() ? busIdMatch.captured(1).trimmed() : QString();

      const QRegularExpression cardNameRe(QStringLiteral("(?:alsa\\.card_name|device\\.nick|device\\.description)\\s*=\\s*\"([^\"]+)\""));
      const auto cardNameMatch = cardNameRe.match(block);
      const QString cardName = cardNameMatch.hasMatch() ? cardNameMatch.captured(1).trimmed() : QString();

      const QRegularExpression chanRe(QStringLiteral("Sample Specification:[^\\n]*\\s(\\d+)ch"));
      const auto chanMatch = chanRe.match(block);
      const int channels = chanMatch.hasMatch() ? chanMatch.captured(1).toInt() : (isMonitor ? 2 : 1);

      detailedDescriptions.insert(name, {description, driver, cardBusId, cardName, monitorOf, isMonitor, channels});
    }
  }

  const QString shortList = executeCommand({QStringLiteral("list"), QStringLiteral("short"), QStringLiteral("sources")});
  QVector<AudioDeviceInfo> devices;

  if (!shortList.isEmpty()) {
    const QStringList lines = shortList.split(QRegularExpression(QStringLiteral("[\r\n]+")), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
      const QStringList parts = line.split(QLatin1Char('\t'));
      if (parts.size() < 2) continue;
      const QString id = parts.at(1).trimmed();
      if (id.isEmpty()) continue;

      const SourceDetail detail = detailedDescriptions.value(id);
      const QString driver = detail.driver.isEmpty() ? (parts.size() >= 3 ? parts.at(2).trimmed() : QString()) : detail.driver;
      const QString description = detail.description.isEmpty() ? id : detail.description;
      const bool isMonitor = detail.isMonitor || id.endsWith(QStringLiteral(".monitor"), Qt::CaseInsensitive) || description.contains(QStringLiteral("monitor"), Qt::CaseInsensitive);

      AudioDeviceInfo info;
      info.id = id;
      info.label = description;
      info.driver = driver;
      info.cardBusId = detail.cardBusId;
      info.cardName = detail.cardName.isEmpty() ? description : detail.cardName;
      info.monitorOfSink = detail.monitorOfSink;
      info.type = isMonitor ? AudioDeviceType::Monitor : AudioDeviceType::Microphone;
      info.isDefault = (id == defaultSource);
      info.isDefaultMonitor = (!defaultSinkMonitor.isEmpty() && id == defaultSinkMonitor);
      info.isMonitor = isMonitor;
      info.channelCount = detail.channels;
      devices.append(info);
    }
  }

  // Fallback if pactl short failed but detailed list had entries
  if (devices.isEmpty() && !detailedDescriptions.isEmpty()) {
    for (auto it = detailedDescriptions.constBegin(); it != detailedDescriptions.constEnd(); ++it) {
      const QString &id = it.key();
      const SourceDetail &detail = it.value();
      const bool isMonitor = detail.isMonitor || id.endsWith(QStringLiteral(".monitor"), Qt::CaseInsensitive) || detail.description.contains(QStringLiteral("monitor"), Qt::CaseInsensitive);

      AudioDeviceInfo info;
      info.id = id;
      info.label = detail.description.isEmpty() ? id : detail.description;
      info.driver = detail.driver;
      info.cardBusId = detail.cardBusId;
      info.cardName = detail.cardName.isEmpty() ? detail.description : detail.cardName;
      info.monitorOfSink = detail.monitorOfSink;
      info.type = isMonitor ? AudioDeviceType::Monitor : AudioDeviceType::Microphone;
      info.isDefault = (id == defaultSource);
      info.isDefaultMonitor = (!defaultSinkMonitor.isEmpty() && id == defaultSinkMonitor);
      info.isMonitor = isMonitor;
      info.channelCount = detail.channels;
      devices.append(info);
    }
  }

  // Sort: default monitor first, then other monitors, then default source, then other mics
  std::sort(devices.begin(), devices.end(), [](const AudioDeviceInfo &a, const AudioDeviceInfo &b) {
    if (a.isDefaultMonitor != b.isDefaultMonitor) return a.isDefaultMonitor > b.isDefaultMonitor;
    if (a.isMonitor != b.isMonitor) return a.isMonitor > b.isMonitor;
    if (a.isDefault != b.isDefault) return a.isDefault > b.isDefault;
    return a.label.localeAwareCompare(b.label) < 0;
  });

  return devices;
}

AutoRouteInfo AudioDeviceManager::resolveAutoRoute(const QVector<AudioDeviceInfo> &devices) {
  AutoRouteInfo route;
  const QString defaultSink = lastDefaultSink_;
  const QString defaultSource = lastDefaultSource_;
  const QString defaultSinkMonitor = defaultSink.isEmpty() ? QString() : (defaultSink + QStringLiteral(".monitor"));

  // 1. Find matching System Audio Monitor for active sink
  const AudioDeviceInfo *matchedMonitor = nullptr;
  // The supplied snapshot is authoritative when it already identifies the
  // default monitor. Only consult the live pactl route as a fallback; it may
  // have changed after the snapshot was collected.
  for (const auto &dev : devices) {
    if (dev.isDefaultMonitor) {
      matchedMonitor = &dev;
      break;
    }
  }
  if (!matchedMonitor && !defaultSinkMonitor.isEmpty()) {
    for (const auto &dev : devices) {
      if (dev.isMonitor && (dev.id == defaultSinkMonitor || dev.monitorOfSink == defaultSink)) {
        matchedMonitor = &dev;
        break;
      }
    }
  }
  if (!matchedMonitor) {
    for (const auto &dev : devices) {
      if (dev.isMonitor) {
        matchedMonitor = &dev;
        break;
      }
    }
  }

  if (matchedMonitor) {
    route.systemMonitorId = matchedMonitor->id;
    route.systemMonitorLabel = matchedMonitor->label;
    route.hasSystemMonitor = true;
    route.cardName = matchedMonitor->cardName;
  }

  // 2. Find matching Microphone (paired by cardBusId or cardName with the active monitor / headset)
  const AudioDeviceInfo *matchedMic = nullptr;
  if (matchedMonitor && !matchedMonitor->cardBusId.isEmpty()) {
    for (const auto &dev : devices) {
      if (!dev.isMonitor && dev.cardBusId == matchedMonitor->cardBusId) {
        matchedMic = &dev;
        break;
      }
    }
  }
  if (!matchedMic && matchedMonitor && !matchedMonitor->cardName.isEmpty()) {
    for (const auto &dev : devices) {
      if (!dev.isMonitor && dev.cardName == matchedMonitor->cardName) {
        matchedMic = &dev;
        break;
      }
    }
  }
  if (!matchedMic && !defaultSource.isEmpty()) {
    for (const auto &dev : devices) {
      if (!dev.isMonitor && dev.id == defaultSource) {
        matchedMic = &dev;
        break;
      }
    }
  }
  if (!matchedMic) {
    for (const auto &dev : devices) {
      if (!dev.isMonitor) {
        matchedMic = &dev;
        break;
      }
    }
  }

  if (matchedMic) {
    route.microphoneId = matchedMic->id;
    route.microphoneLabel = matchedMic->label;
    route.hasMicrophone = true;
    if (route.cardName.isEmpty()) {
      route.cardName = matchedMic->cardName;
    }
  }

  route.activeSinkName = defaultSink;
  return route;
}

QString AudioDeviceManager::pickBestDeviceId(const QVector<AudioDeviceInfo> &devices, const QString &preferredId) const {
  const QString wanted = preferredId.trimmed();
  if (!wanted.isEmpty() && wanted != QStringLiteral("auto")) {
    for (const auto &dev : devices) {
      if (dev.id == wanted || dev.label == wanted) return dev.id;
    }
  }
  for (const auto &dev : devices) {
    if (dev.isDefaultMonitor) return dev.id;
  }
  for (const auto &dev : devices) {
    if (dev.isMonitor) return dev.id;
  }
  for (const auto &dev : devices) {
    if (dev.isDefault) return dev.id;
  }
  return devices.isEmpty() ? QString() : devices.first().id;
}

void AudioDeviceManager::checkDeviceChanges() {
  ++pollCheckCount_;
  const auto defaults = queryDefaultPulseRoute();
  const QString currentSink = defaults.first;
  const QString currentSource = defaults.second;

  if (currentSink != lastDefaultSink_ || currentSource != lastDefaultSource_) {
    lastDefaultSink_ = currentSink;
    lastDefaultSource_ = currentSource;

    const auto devs = listDevices();
    const auto route = resolveAutoRoute(devs);
    emit devicesChanged(devs, route);
  }
}
