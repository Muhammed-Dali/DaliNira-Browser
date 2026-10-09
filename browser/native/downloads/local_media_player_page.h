#pragma once

#include <QByteArray>
#include <QRect>
#include <QWidget>

#include <memory>

#include "local_media_routing.h"

class QLabel;
class QNetworkAccessManager;
class QNetworkReply;
class QProcess;
class QPushButton;
class QWebEngineProfile;
class QWebEngineView;
class QTemporaryDir;
class WebAudioEffectsController;

class LocalMediaPlayerPage final : public QWidget {
  Q_OBJECT
 public:
  static QString playerHtml(const LocalMediaOpenRequest &request,
                            const QUrl &mediaUrl,
                            bool includeHeader = false,
                            const QString &resolvedArtworkUrl = QString{},
                            bool customControls = false);
  static QString createOrUpdateWebKitPlayerHtml(const LocalMediaOpenRequest &request,
                                                const QString &ffmpegPath = QString{});

  LocalMediaPlayerPage(const LocalMediaOpenRequest &request,
                       QWebEngineProfile *profile,
                       WebAudioEffectsController *audioEffects,
                       const QString &ffmpegPath,
                       QWidget *parent = nullptr);
  ~LocalMediaPlayerPage() override;

  void loadMedia(const LocalMediaOpenRequest &request);
  QString mediaPath() const { return mediaPath_; }
  LocalMediaPlayerKind playerKind() const { return playerKind_; }
  QWebEngineView *webView() const { return view_; }

 signals:
  void audioEffectsRequested();

 private:
  void requestArtwork(const LocalMediaOpenRequest &request);
  void requestRemoteArtwork(const QString &thumbnailUrl, quint64 generation);
  bool setArtworkData(const QByteArray &bytes, quint64 generation);
  void updateArtworkInDocument();

  QString mediaPath_;
  QString ffmpegPath_;
  LocalMediaPlayerKind playerKind_ = LocalMediaPlayerKind::External;
  QWebEngineView *view_ = nullptr;
  QLabel *fileLabel_ = nullptr;
  QPushButton *effectsButton_ = nullptr;
  WebAudioEffectsController *audioEffects_ = nullptr;
  QProcess *artworkProcess_ = nullptr;
  QNetworkAccessManager *artworkNetwork_ = nullptr;
  QNetworkReply *artworkReply_ = nullptr;
  QByteArray artworkData_;
  quint64 artworkGeneration_ = 0;
  Qt::WindowStates windowStateBeforeFullScreen_ = Qt::WindowNoState;
  QRect windowGeometryBeforeFullScreen_;
  bool mediaFullScreen_ = false;
  std::unique_ptr<QTemporaryDir> privateArtworkDirectory_;
};
