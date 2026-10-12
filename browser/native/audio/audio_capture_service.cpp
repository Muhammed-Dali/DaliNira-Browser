#include "audio_capture_service.h"
#include "core/application_identity.h"

#include "security_utils.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include "i18n/i18n.h"

AudioCaptureService::AudioCaptureService(QObject *parent)
    : QObject(parent), systemBuffer_(16000, 12), micBuffer_(16000, 12) {
#if defined(Q_OS_WIN)
  const QString executableName = QStringLiteral("ffmpeg.exe");
#else
  const QString executableName = QStringLiteral("ffmpeg");
#endif
  const QString appDir = dalinira::application_identity::applicationDirectory();
  ffmpegPath_ = BrowserSecurity::resolveTrustedExecutable(
      executableName, {QDir(appDir).filePath(executableName), QDir(appDir).filePath(QStringLiteral("bin/") + executableName)});
  levelTimer_ = new QTimer(this);
  levelTimer_->setInterval(50);  // ~20 FPS for responsive real-time VU meter
  connect(levelTimer_, &QTimer::timeout, this, &AudioCaptureService::onLevelTimerTimeout);
}

AudioCaptureService::~AudioCaptureService() {
  stop();
}

bool AudioCaptureService::isRunning() const {
  return (systemProcess_ && systemProcess_->state() != QProcess::NotRunning) ||
         (micProcess_ && micProcess_->state() != QProcess::NotRunning);
}

QString AudioCaptureService::activeSourceName() const {
  if (!isAutoMode_) {
    return currentDeviceId_;
  }
  if (activeSourceType_ == ActiveSourceType::SystemAudio) {
    return I18n::text(QStringLiteral("audio.system_audio_label"));
  }
  if (activeSourceType_ == ActiveSourceType::Microphone) {
    return I18n::text(QStringLiteral("settings.site.microphone"));
  }
  return I18n::text(QStringLiteral("common.automatic"));
}

SlidingPcmBuffer &AudioCaptureService::activeBuffer() {
  if (isAutoMode_ && activeSourceType_ == ActiveSourceType::Microphone) {
    return micBuffer_;
  }
  return systemBuffer_;
}

const SlidingPcmBuffer &AudioCaptureService::activeBuffer() const {
  if (isAutoMode_ && activeSourceType_ == ActiveSourceType::Microphone) {
    return micBuffer_;
  }
  return systemBuffer_;
}

QProcess *AudioCaptureService::spawnFfmpegProcess(const QString &deviceId) {
  if (deviceId.isEmpty() || ffmpegPath_.isEmpty()) {
    if (ffmpegPath_.isEmpty()) emit errorOccurred(I18n::text(QStringLiteral("pulse.ffmpeg_not_found")));
    return nullptr;
  }

  auto *proc = new QProcess(this);
  proc->setProcessChannelMode(QProcess::SeparateChannels);

  const QStringList args = {
      QStringLiteral("-hide_banner"),
      QStringLiteral("-loglevel"), QStringLiteral("warning"),
      QStringLiteral("-f"), QStringLiteral("pulse"),
      QStringLiteral("-i"), deviceId,
      QStringLiteral("-ac"), QStringLiteral("1"),
      QStringLiteral("-ar"), QStringLiteral("16000"),
      QStringLiteral("-f"), QStringLiteral("f32le"),
      QStringLiteral("pipe:1")};

  proc->start(ffmpegPath_, args);
  if (!proc->waitForStarted(2000)) {
    const QString err = proc->errorString();
    proc->deleteLater();
    emit errorOccurred(I18n::text(QStringLiteral("pulse.capture_start_failed_detail")).arg(deviceId, err));
    return nullptr;
  }
  return proc;
}

void AudioCaptureService::stopProcess(QProcess *&proc) {
  if (!proc) return;
  disconnect(proc, nullptr, this, nullptr);
  if (proc->state() != QProcess::NotRunning) {
    proc->terminate();
    if (!proc->waitForFinished(400)) {
      proc->kill();
      proc->waitForFinished(200);
    }
  }
  proc->deleteLater();
  proc = nullptr;
}

bool AudioCaptureService::start(const QString &deviceId, int bufferSizeSecs) {
  if (deviceId.isEmpty() || deviceId == QStringLiteral("auto") || deviceId.startsWith(QStringLiteral("auto:"))) {
    // If called with empty or auto without resolved IDs, we will wait for startAuto call from manager
    return false;
  }

  stop();
  isAutoMode_ = false;
  bufferSizeSecs_ = bufferSizeSecs;
  currentDeviceId_ = deviceId;
  activeSourceType_ = ActiveSourceType::Manual;

  systemBuffer_.resizeCapacity(bufferSizeSecs);
  systemBuffer_.clear();

  systemProcess_ = spawnFfmpegProcess(deviceId);
  if (!systemProcess_) {
    return false;
  }

  connect(systemProcess_, &QProcess::readyReadStandardOutput, this, &AudioCaptureService::onSystemReadyRead);
  levelTimer_->start();
  emit started(deviceId);
  return true;
}

bool AudioCaptureService::startAuto(const QString &systemMonitorId, const QString &microphoneId, int bufferSizeSecs) {
  stop();
  isAutoMode_ = true;
  bufferSizeSecs_ = bufferSizeSecs;
  currentDeviceId_ = QStringLiteral("auto");
  systemMonitorId_ = systemMonitorId;
  microphoneId_ = microphoneId;
  activeSourceType_ = ActiveSourceType::SystemAudio;
  holdSystemUntilMs_ = 0;
  holdMicUntilMs_ = 0;

  systemBuffer_.resizeCapacity(bufferSizeSecs);
  systemBuffer_.clear();
  micBuffer_.resizeCapacity(bufferSizeSecs);
  micBuffer_.clear();

  if (!systemMonitorId.isEmpty()) {
    systemProcess_ = spawnFfmpegProcess(systemMonitorId);
    if (systemProcess_) {
      connect(systemProcess_, &QProcess::readyReadStandardOutput, this, &AudioCaptureService::onSystemReadyRead);
    }
  }

  if (!microphoneId.isEmpty()) {
    micProcess_ = spawnFfmpegProcess(microphoneId);
    if (micProcess_) {
      connect(micProcess_, &QProcess::readyReadStandardOutput, this, &AudioCaptureService::onMicReadyRead);
    }
  }

  if (!systemProcess_ && !micProcess_) {
    emit errorOccurred(I18n::text(QStringLiteral("pulse.no_source_started")));
    return false;
  }

  levelTimer_->start();
  emit started(QStringLiteral("auto"));
  return true;
}

void AudioCaptureService::updateAutoRoute(const QString &systemMonitorId, const QString &microphoneId) {
  if (!isAutoMode_ || !isRunning()) return;

  if (systemMonitorId_ != systemMonitorId) {
    systemMonitorId_ = systemMonitorId;
    stopProcess(systemProcess_);
    systemBuffer_.clear();
    if (!systemMonitorId.isEmpty()) {
      systemProcess_ = spawnFfmpegProcess(systemMonitorId);
      if (systemProcess_) {
        connect(systemProcess_, &QProcess::readyReadStandardOutput, this, &AudioCaptureService::onSystemReadyRead);
      }
    }
  }

  if (microphoneId_ != microphoneId) {
    microphoneId_ = microphoneId;
    stopProcess(micProcess_);
    micBuffer_.clear();
    if (!microphoneId.isEmpty()) {
      micProcess_ = spawnFfmpegProcess(microphoneId);
      if (micProcess_) {
        connect(micProcess_, &QProcess::readyReadStandardOutput, this, &AudioCaptureService::onMicReadyRead);
      }
    }
  }
}

void AudioCaptureService::stop() {
  levelTimer_->stop();
  stopProcess(systemProcess_);
  stopProcess(micProcess_);

  currentDeviceId_.clear();
  systemMonitorId_.clear();
  microphoneId_.clear();
  isAutoMode_ = false;

  emit volumeChanged(0.0, 0.0, ActiveSourceType::SystemAudio, I18n::text(QStringLiteral("common.ready")));
  emit stopped();
}

void AudioCaptureService::onSystemReadyRead() {
  if (!systemProcess_) return;
  const QByteArray data = systemProcess_->readAllStandardOutput();
  if (!data.isEmpty()) {
    systemBuffer_.pushBytes(data);
  }
}

void AudioCaptureService::onMicReadyRead() {
  if (!micProcess_) return;
  const QByteArray data = micProcess_->readAllStandardOutput();
  if (!data.isEmpty()) {
    micBuffer_.pushBytes(data);
  }
}

void AudioCaptureService::onLevelTimerTimeout() {
  if (!isRunning()) return;

  const double sysLevel = systemBuffer_.getLevelPercent(640, 2.2);
  const double sysFill = systemBuffer_.fillPercent();

  if (!isAutoMode_) {
    activeSourceType_ = ActiveSourceType::Manual;
    emit volumeChanged(sysLevel, sysFill, ActiveSourceType::Manual, currentDeviceId_);
    return;
  }

  const double micLevel = micBuffer_.getLevelPercent(640, 2.4);
  const double micFill = micBuffer_.fillPercent();
  const qint64 now = QDateTime::currentMSecsSinceEpoch();

  // Smart Decision Engine: System Audio Priority with Hysteresis
  if (sysLevel >= 2.0 || systemBuffer_.hasSignal(0.0015f)) {
    activeSourceType_ = ActiveSourceType::SystemAudio;
    holdSystemUntilMs_ = now + 1200;  // 1.2s hold duration
  } else if (now > holdSystemUntilMs_ && (micLevel >= 3.0 || micBuffer_.hasSignal(0.002f))) {
    activeSourceType_ = ActiveSourceType::Microphone;
  } else if (now > holdSystemUntilMs_) {
    activeSourceType_ = ActiveSourceType::SystemAudio;
  }

  const double activeLevel = (activeSourceType_ == ActiveSourceType::SystemAudio) ? sysLevel : micLevel;
  const double activeFill = (activeSourceType_ == ActiveSourceType::SystemAudio) ? sysFill : micFill;

  emit volumeChanged(activeLevel, activeFill, activeSourceType_, activeSourceName());
}
