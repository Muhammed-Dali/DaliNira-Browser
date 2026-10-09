#pragma once

#include "audio_capture_service.h"
#include "audio_device_manager.h"
#include "song_finder_settings.h"

#include <QDateTime>
#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QVector>
#include "i18n/i18n.h"

struct SongResult {
  enum class Source {
    Shazam,
    WebMetadata,
    Unknown
  };

  QString title;
  QString artist;
  QString album;
  QString genre;
  QString coverUrl;
  QString trackKey;
  Source source = Source::Shazam;
  QDateTime timestamp = QDateTime::currentDateTime();

  QString searchQuery() const {
    QString q = artist.trimmed();
    if (!q.isEmpty() && !title.trimmed().isEmpty()) q += QLatin1Char(' ');
    q += title.trimmed();
    return q.trimmed();
  }

  bool isValid() const {
    const QString t = title.trimmed();
    const QString a = artist.trimmed();
    if (t.isEmpty()) return false;
    // Reject generic or placeholder titles
    if (I18n::isTranslationOf(QStringLiteral("newtab.title"), t, Qt::CaseInsensitive) ||
        t.compare(QStringLiteral("New Tab"), Qt::CaseInsensitive) == 0 ||
        t.compare(QStringLiteral("YouTube"), Qt::CaseInsensitive) == 0 ||
        t.compare(QStringLiteral("Google"), Qt::CaseInsensitive) == 0 ||
        t.compare(QStringLiteral("DaliNiraBrowser"), Qt::CaseInsensitive) == 0 ||
        I18n::isTranslationOf(QStringLiteral("pulse.unknown_track"), t, Qt::CaseInsensitive)) {
      return false;
    }
    return !a.isEmpty() || !trackKey.isEmpty();
  }

  QString dedupeKey() const {
    if (!trackKey.isEmpty()) return trackKey;
    return QStringLiteral("%1|%2").arg(artist.trimmed().toLower(), title.trimmed().toLower());
  }

  QString sourceDisplayName() const {
    switch (source) {
      case Source::Shazam: return QStringLiteral("Shazam");
      case Source::WebMetadata: return I18n::text(QStringLiteral("pulse.source_web_media"));
      default: return I18n::text(QStringLiteral("pulse.source_unknown"));
    }
  }
};

class SongRecognitionService final : public QObject {
  Q_OBJECT

 public:
  enum class State {
    Ready,
    Listening,
    Recognizing,
    Found,
    NotFound,
    Error
  };

  explicit SongRecognitionService(SongFinderSettings *settings, QObject *parent = nullptr,
                                  bool historyPersistenceEnabled = true);
  ~SongRecognitionService() override;

  SongFinderSettings *settings() const { return settings_; }
  AudioCaptureService *captureService() const { return captureService_; }
  AudioDeviceManager *deviceManager() const { return deviceManager_; }

  State state() const { return state_; }
  QString stateMessage() const { return stateMessage_; }
  bool isListening() const;
  QString currentDeviceId() const;
  QVector<AudioDeviceInfo> cachedDevices() const { return cachedDevices_; }
  AutoRouteInfo currentRoute() const { return currentRoute_; }

  QVector<SongResult> foundHistory() const { return history_; }
  bool hasActiveResult() const { return hasActiveResult_; }
  SongResult activeResult() const { return activeResult_; }
  void dismissActiveResult();
  void clearActiveResult();
  void removeHistoryItem(const QString &dedupeKey);
  void clearHistory();
  bool isDuplicate(const SongResult &result) const;
  void rememberResult(const SongResult &result);

  bool startListening(const QString &requestedDeviceId = QString());
  void stopListening();
  QVector<AudioDeviceInfo> refreshDevices();
  void beginDeviceUiUse();
  void endDeviceUiUse();
  bool isDeviceMonitoringActive() const;
  int deviceUiConsumerCount() const { return deviceUiConsumerCount_; }

  uint64_t currentSessionId() const { return currentSessionId_; }

  void setWebContextMetadata(const QString &title, const QString &artist);
  void clearWebContextMetadata();

 signals:
  void stateChanged(State state, const QString &message);
  void songFound(const SongResult &result);
  void activeResultChanged(const SongResult &result, bool hasActive);
  void volumeChanged(double levelPercent, double bufferFillPercent, const QString &activeSourceName);
  void devicesUpdated(const QVector<AudioDeviceInfo> &devices, const AutoRouteInfo &route);
  void autoOpenRequested(const SongResult &result, SongFinderSettings::OpenPlatform platform);

 private slots:
  void onCaptureVolumeChanged(double levelPercent, double bufferFillPercent, AudioCaptureService::ActiveSourceType sourceType, const QString &sourceName);
  void onCaptureError(const QString &error);
  void onRecognitionTimerTimeout();
  void onDeviceManagerChanges(const QVector<AudioDeviceInfo> &devices, const AutoRouteInfo &route);

 private:
  void setState(State state, const QString &message = QString());
  void processBufferForRecognition();
  void sendShazamRequest(const QString &signatureUri, int sampleMs);
  void handleShazamResponse(const QByteArray &data, int statusCode);
  void loadHistory();
  void saveHistory();

  SongFinderSettings *settings_ = nullptr;
  AudioCaptureService *captureService_ = nullptr;
  AudioDeviceManager *deviceManager_ = nullptr;
  QNetworkAccessManager *networkManager_ = nullptr;
  QPointer<QNetworkReply> activeReply_;
  QTimer *recognitionTimer_ = nullptr;

  State state_ = State::Ready;
  QString stateMessage_;
  QVector<AudioDeviceInfo> cachedDevices_;
  AutoRouteInfo currentRoute_;
  QVector<SongResult> history_;
  QHash<QString, qint64> recentTrackKeys_;

  uint64_t currentSessionId_ = 0;
  qint64 sessionStartedAt_ = 0;
  bool isProcessing_ = false;
  qint64 backoffUntilMs_ = 0;
  int consecutiveNoSignal_ = 0;
  int consecutiveNoMatch_ = 0;

  QString webContextTitle_;
  QString webContextArtist_;
  qint64 webContextUpdatedAt_ = 0;

  bool hasActiveResult_ = false;
  SongResult activeResult_;
  int deviceUiConsumerCount_ = 0;
  bool historyPersistenceEnabled_ = true;
};
