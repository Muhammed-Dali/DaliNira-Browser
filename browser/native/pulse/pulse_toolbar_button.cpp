#include "pulse_toolbar_button.h"
#include "big_listen_button.h"
#include "browser_icons.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <cmath>
#include "i18n/i18n.h"

namespace {

void loadCoverImage(QLabel *label, const QString &coverUrl, const QString &searchQuery, int size = 46, int radius = 6) {
  if (!label) return;
  label->clear();
  label->setText(QStringLiteral("🎵"));
  label->setStyleSheet(QStringLiteral("background:#1e293b;border-radius:%1px;font-size:%2px;color:#94a3b8;border:1px solid #334155;").arg(radius).arg(size / 3));

  auto applyPixmap = [label, size, radius](const QByteArray &data) -> bool {
    QPixmap pm;
    if (pm.loadFromData(data)) {
      pm = pm.scaled(size, size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
      QPixmap rounded(size, size);
      rounded.fill(Qt::transparent);
      QPainter p(&rounded);
      p.setRenderHint(QPainter::Antialiasing);
      QPainterPath path;
      path.addRoundedRect(0, 0, size, size, radius, radius);
      p.setClipPath(path);
      p.drawPixmap(0, 0, pm);
      label->clear();
      label->setPixmap(rounded);
      label->setStyleSheet(QStringLiteral("background:#0f172a;border-radius:%1px;border:1px solid #334155;").arg(radius));
      return true;
    }
    return false;
  };

  auto *nam = new QNetworkAccessManager(label);

  auto fallbackToItunes = [nam, applyPixmap, label, searchQuery]() {
    const QString q = searchQuery.trimmed();
    if (q.isEmpty()) return;
    const QUrl itunesUrl(QStringLiteral("https://itunes.apple.com/search?term=%1&entity=song&limit=1")
                             .arg(QString::fromUtf8(QUrl::toPercentEncoding(q))));
    QNetworkRequest req(itunesUrl);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"));

    QNetworkReply *reply = nam->get(req);
    QObject::connect(reply, &QNetworkReply::finished, label, [reply, nam, applyPixmap, label]() {
      reply->deleteLater();
      if (reply->error() == QNetworkReply::NoError) {
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        if (doc.isObject()) {
          const QJsonArray results = doc.object().value(QStringLiteral("results")).toArray();
          if (!results.isEmpty()) {
            QString artUrl = results.at(0).toObject().value(QStringLiteral("artworkUrl100")).toString();
            if (!artUrl.isEmpty()) {
              artUrl.replace(QStringLiteral("100x100bb"), QStringLiteral("600x600bb"));
              const QUrl directImgUrl(artUrl);
              QNetworkRequest imgReq(directImgUrl);
              imgReq.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
              imgReq.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"));
              QNetworkReply *imgReply = nam->get(imgReq);
              QObject::connect(imgReply, &QNetworkReply::finished, label, [imgReply, applyPixmap]() {
                imgReply->deleteLater();
                if (imgReply->error() == QNetworkReply::NoError) {
                  applyPixmap(imgReply->readAll());
                }
              });
            }
          }
        }
      }
    });
  };

  if (!coverUrl.trimmed().isEmpty()) {
    const QUrl directUrl(coverUrl.trimmed());
    QNetworkRequest req(directUrl);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36"));
    req.setRawHeader("Accept", "image/avif,image/webp,image/apng,image/svg+xml,image/*,*/*;q=0.8");

    QNetworkReply *reply = nam->get(req);
    QObject::connect(reply, &QNetworkReply::finished, label, [reply, applyPixmap, fallbackToItunes]() {
      reply->deleteLater();
      bool ok = false;
      if (reply->error() == QNetworkReply::NoError) {
        ok = applyPixmap(reply->readAll());
      }
      if (!ok) {
        fallbackToItunes();
      }
    });
  } else {
    fallbackToItunes();
  }
}

QIcon createYouTubeIcon(int size = 16) {
  QPixmap pix(size, size);
  pix.fill(Qt::transparent);
  QPainter p(&pix);
  p.setRenderHint(QPainter::Antialiasing, true);
  p.setBrush(QColor(255, 0, 0));
  p.setPen(Qt::NoPen);
  const qreal margin = size * 0.15;
  const qreal w = size - 2 * margin;
  const qreal h = size * 0.7;
  const qreal y = (size - h) / 2.0;
  p.drawRoundedRect(QRectF(margin, y, w, h), h * 0.28, h * 0.28);
  p.setBrush(Qt::white);
  QPolygonF tri;
  tri << QPointF(size * 0.42, size * 0.35)
      << QPointF(size * 0.42, size * 0.65)
      << QPointF(size * 0.65, size * 0.50);
  p.drawPolygon(tri);
  return QIcon(pix);
}

QIcon createYouTubeMusicIcon(int size = 16) {
  QPixmap pix(size, size);
  pix.fill(Qt::transparent);
  QPainter p(&pix);
  p.setRenderHint(QPainter::Antialiasing, true);
  p.setBrush(QColor(255, 0, 0));
  p.setPen(Qt::NoPen);
  p.drawEllipse(QRectF(1, 1, size - 2, size - 2));
  p.setBrush(Qt::NoBrush);
  p.setPen(QPen(Qt::white, std::max(1.0, size * 0.08)));
  p.drawEllipse(QRectF(size * 0.22, size * 0.22, size * 0.56, size * 0.56));
  p.setBrush(Qt::white);
  p.setPen(Qt::NoPen);
  QPolygonF tri;
  tri << QPointF(size * 0.43, size * 0.38)
      << QPointF(size * 0.43, size * 0.62)
      << QPointF(size * 0.64, size * 0.50);
  p.drawPolygon(tri);
  return QIcon(pix);
}

QString pulsePopupStyleSheet() {
  return QStringLiteral(R"CSS(
    QFrame#pulse-quick-popup {
      background: #161e27;
      border: 1px solid #303d4d;
      border-radius: 12px;
      color: #e4ebf5;
    }
    #pulse-title {
      color: #ffffff;
      font-size: 14px;
      font-weight: 700;
    }
    #pulse-status {
      color: #8da4b8;
      font-size: 12px;
      font-weight: 500;
    }
    #pulse-settings-btn {
      background: transparent;
      border: 0;
      color: #94a3b8;
      font-size: 15px;
      min-width: 26px;
      max-width: 26px;
      min-height: 26px;
      max-height: 26px;
      border-radius: 6px;
    }
    #pulse-settings-btn:hover {
      background: #253140;
      color: #ffffff;
    }
    QProgressBar#pulse-signal-bar {
      border: 0;
      border-radius: 2px;
      background: #0e141a;
      min-height: 4px;
      max-height: 4px;
      text-align: center;
    }
    QProgressBar#pulse-signal-bar::chunk {
      background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #0284c7, stop:1 #38bdf8);
      border-radius: 2px;
    }
    #pulse-empty-label {
      color: #64748b;
      font-size: 12px;
      padding: 12px 6px;
    }
    QFrame#pulse-result-card {
      background: #1d2633;
      border: 1px solid #2d3c4e;
      border-radius: 9px;
      padding: 6px 8px;
    }
    QFrame#pulse-result-card:hover {
      background: #243243;
      border-color: #3b5067;
    }
    #pulse-card-title {
      color: #f1f5f9;
      font-size: 13px;
      font-weight: 650;
    }
    #pulse-card-artist {
      color: #94a3b8;
      font-size: 12px;
      font-weight: 500;
    }
    #pulse-card-meta {
      color: #64748b;
      font-size: 11px;
    }
    #pulse-card-hint {
      color: #38bdf8;
      font-size: 11px;
      font-weight: 600;
    }
    QPushButton#pulse-open-full-btn {
      background: #1f2a38;
      color: #e2ecf6;
      border: 1px solid #334457;
      border-radius: 7px;
      padding: 6px 14px;
      font-size: 12px;
      font-weight: 600;
      min-height: 24px;
    }
    QPushButton#pulse-open-full-btn:hover {
      background: #283749;
      border-color: #49617d;
      color: #ffffff;
    }
  )CSS");
}

}  // namespace

// ============================================================================
// PulseResultCard Implementation
// ============================================================================

PulseResultCard::PulseResultCard(const SongResult &result,
                                 SongFinderSettings::OpenPlatform platform,
                                 QWidget *parent)
    : QFrame(parent), result_(result) {
  setObjectName(QStringLiteral("pulse-result-card"));
  setCursor(Qt::PointingHandCursor);
  setAttribute(Qt::WA_Hover, true);

  auto *mainLayout = new QHBoxLayout(this);
  mainLayout->setContentsMargins(8, 7, 8, 7);
  mainLayout->setSpacing(10);

  // Cover Image
  auto *coverLabel = new QLabel(this);
  coverLabel->setFixedSize(46, 46);
  coverLabel->setStyleSheet(QStringLiteral("background: #10161d; border-radius: 6px; border: 1px solid #2b3949;"));
  coverLabel->setAlignment(Qt::AlignCenter);
  loadCoverImage(coverLabel, result.coverUrl, result.searchQuery(), 46, 6);
  mainLayout->addWidget(coverLabel);

  // Text Column
  auto *textCol = new QWidget(this);
  auto *textLayout = new QVBoxLayout(textCol);
  textLayout->setContentsMargins(0, 0, 0, 0);
  textLayout->setSpacing(1);

  auto *titleLabel = new QLabel(result.title.isEmpty() ? I18n::text(QStringLiteral("pulse.unknown_track")) : result.title, textCol);
  titleLabel->setObjectName(QStringLiteral("pulse-card-title"));
  textLayout->addWidget(titleLabel);

  auto *artistLabel = new QLabel(result.artist.isEmpty() ? I18n::text(QStringLiteral("pulse.unknown_artist")) : result.artist, textCol);
  artistLabel->setObjectName(QStringLiteral("pulse-card-artist"));
  textLayout->addWidget(artistLabel);

  auto *metaRow = new QHBoxLayout();
  metaRow->setContentsMargins(0, 2, 0, 0);
  metaRow->setSpacing(6);

  QString metaStr = QStringLiteral("%1 • %2").arg(result.timestamp.toString(QStringLiteral("hh:mm:ss")), result.sourceDisplayName());
  if (!result.album.isEmpty()) {
    metaStr += QStringLiteral(" • %1").arg(result.album);
  }
  if (!result.genre.isEmpty()) {
    metaStr += QStringLiteral(" • %1").arg(result.genre);
  }
  auto *metaLabel = new QLabel(metaStr, textCol);
  metaLabel->setObjectName(QStringLiteral("pulse-card-meta"));
  metaRow->addWidget(metaLabel);
  metaRow->addStretch(1);

  const bool isYtMusic = (platform == SongFinderSettings::OpenPlatform::YouTubeMusic);
  auto *hintIcon = new QLabel(textCol);
  hintIcon->setPixmap(isYtMusic ? createYouTubeMusicIcon(14).pixmap(14, 14) : createYouTubeIcon(14).pixmap(14, 14));
  metaRow->addWidget(hintIcon);

  const QString hintText = isYtMusic ? QStringLiteral("YouTube Music ↗") : QStringLiteral("YouTube ↗");
  auto *hintLabel = new QLabel(hintText, textCol);
  hintLabel->setObjectName(QStringLiteral("pulse-card-hint"));
  metaRow->addWidget(hintLabel);

  textLayout->addLayout(metaRow);
  mainLayout->addWidget(textCol, 1);
}

void PulseResultCard::mousePressEvent(QMouseEvent *event) {
  if (event->button() == Qt::LeftButton) {
    isPressed_ = true;
  }
  QFrame::mousePressEvent(event);
}

void PulseResultCard::mouseReleaseEvent(QMouseEvent *event) {
  if (isPressed_ && event->button() == Qt::LeftButton && rect().contains(event->pos())) {
    isPressed_ = false;
    emit clicked(result_);
  }
  isPressed_ = false;
  QFrame::mouseReleaseEvent(event);
}

void PulseResultCard::enterEvent(QEnterEvent *event) {
  Q_UNUSED(event);
  isHovered_ = true;
  update();
}

void PulseResultCard::leaveEvent(QEvent *event) {
  Q_UNUSED(event);
  isHovered_ = false;
  isPressed_ = false;
  update();
}

// ============================================================================
// PulseQuickPopup Implementation
// ============================================================================

PulseQuickPopup::PulseQuickPopup(SongRecognitionService *service,
                                 SongFinderSettings *settings,
                                 QWidget *parent)
    : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint),
      service_(service),
      settings_(settings) {
  setObjectName(QStringLiteral("pulse-quick-popup"));
  setAttribute(Qt::WA_DeleteOnClose, false);
  setFixedWidth(340);
  setStyleSheet(pulsePopupStyleSheet());
  setupUi();

  if (service_) {
    connect(service_, &SongRecognitionService::stateChanged, this, &PulseQuickPopup::onServiceStateChanged);
    connect(service_, &SongRecognitionService::volumeChanged, this, &PulseQuickPopup::onServiceVolumeChanged);
    connect(service_, &SongRecognitionService::songFound, this, &PulseQuickPopup::onServiceSongFound);
    connect(service_, &SongRecognitionService::activeResultChanged, this, [this](const SongResult &, bool) {
      updateResultsList();
    });
  }
}

PulseQuickPopup::~PulseQuickPopup() {
  if (service_ && service_->isListening() && !handingOffToFullPage_) service_->stopListening();
  if (service_ && deviceUseHeld_) service_->endDeviceUiUse();
}

void PulseQuickPopup::setupUi() {
  auto *rootLayout = new QVBoxLayout(this);
  rootLayout->setContentsMargins(14, 12, 14, 12);
  rootLayout->setSpacing(8);

  // Top Header Row: Icon + Title + Settings Button
  auto *headerRow = new QHBoxLayout;
  headerRow->setContentsMargins(0, 0, 0, 0);
  headerRow->setSpacing(8);

  auto *pulseIcon = new QLabel(this);
  pulseIcon->setPixmap(QIcon(QStringLiteral(":/side-widget-icons/pulse.svg")).pixmap(18, 18));
  headerRow->addWidget(pulseIcon);

  titleLabel_ = new QLabel(QStringLiteral("DaliNira Pulse"), this);
  titleLabel_->setObjectName(QStringLiteral("pulse-title"));
  headerRow->addWidget(titleLabel_);

  headerRow->addStretch(1);

  settingsBtn_ = new QPushButton(this);
  settingsBtn_->setObjectName(QStringLiteral("pulse-settings-btn"));
  settingsBtn_->setIcon(BrowserIcons::icon(BrowserIcon::Settings));
  settingsBtn_->setIconSize(QSize(16, 16));
  settingsBtn_->setToolTip(I18n::text(QStringLiteral("pulse.settings_tab_title")));
  settingsBtn_->setCursor(Qt::PointingHandCursor);
  connect(settingsBtn_, &QPushButton::clicked, this, [this]() {
    emit openSettingsRequested();
    hide();
  });
  headerRow->addWidget(settingsBtn_);
  rootLayout->addLayout(headerRow);

  // Subtitle / Status Label
  statusLabel_ = new QLabel(I18n::text(QStringLiteral("pulse.ready_to_find")), this);
  statusLabel_->setObjectName(QStringLiteral("pulse-status"));
  statusLabel_->setAlignment(Qt::AlignCenter);
  statusLabel_->setFixedHeight(22);
  rootLayout->addWidget(statusLabel_);

  // Slim Signal Level Bar
  signalBar_ = new QProgressBar(this);
  signalBar_->setObjectName(QStringLiteral("pulse-signal-bar"));
  signalBar_->setRange(0, 100);
  signalBar_->setValue(0);
  signalBar_->setTextVisible(false);
  rootLayout->addWidget(signalBar_);

  // Center Stage: Big Pulse / Listen Button
  auto *stage = new QWidget(this);
  auto *stageLayout = new QVBoxLayout(stage);
  stageLayout->setContentsMargins(0, 4, 0, 4);
  stageLayout->setAlignment(Qt::AlignCenter);

  listenBtn_ = new BigListenButton(116, stage);
  connect(listenBtn_, &QAbstractButton::clicked, this, &PulseQuickPopup::onListenButtonClicked);
  stageLayout->addWidget(listenBtn_, 0, Qt::AlignCenter);
  rootLayout->addWidget(stage);

  // Results Section (Scrollable for up to 3-5 results)
  resultsScroll_ = new QScrollArea(this);
  resultsScroll_->setObjectName(QStringLiteral("pulse-results-scroll"));
  resultsScroll_->setWidgetResizable(true);
  resultsScroll_->setFrameShape(QFrame::NoFrame);
  resultsScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  resultsScroll_->setMaximumHeight(180);

  resultsContainer_ = new QWidget(resultsScroll_);
  resultsLayout_ = new QVBoxLayout(resultsContainer_);
  resultsLayout_->setContentsMargins(0, 0, 0, 0);
  resultsLayout_->setSpacing(6);

  emptyLabel_ = new QLabel(I18n::text(QStringLiteral("pulse.no_found_songs")), resultsContainer_);
  emptyLabel_->setObjectName(QStringLiteral("pulse-empty-label"));
  emptyLabel_->setAlignment(Qt::AlignCenter);
  resultsLayout_->addWidget(emptyLabel_);

  resultsScroll_->setWidget(resultsContainer_);
  rootLayout->addWidget(resultsScroll_);

  // Footer Row: "DaliNira Pulse'u aç" Button
  openFullBtn_ = new QPushButton(I18n::text(QStringLiteral("pulse.open_pulse")), this);
  openFullBtn_->setObjectName(QStringLiteral("pulse-open-full-btn"));
  openFullBtn_->setCursor(Qt::PointingHandCursor);
  openFullBtn_->setToolTip(I18n::text(QStringLiteral("pulse.open_full_page")));
  connect(openFullBtn_, &QPushButton::clicked, this, [this]() {
    handingOffToFullPage_ = true;
    emit openFullPageRequested();
    hide();
    handingOffToFullPage_ = false;
  });
  rootLayout->addWidget(openFullBtn_);
}

void PulseQuickPopup::refreshState() {
  if (!service_) return;
  const bool listening = service_->isListening();
  listenBtn_->setListening(listening);
  onServiceStateChanged(service_->state(), service_->stateMessage());
  updateResultsList();
}

void PulseQuickPopup::showEvent(QShowEvent *event) {
  QFrame::showEvent(event);
  if (service_ && !deviceUseHeld_) {
    deviceUseHeld_ = true;
    service_->beginDeviceUiUse();
  }
  refreshState();
}

void PulseQuickPopup::hideEvent(QHideEvent *event) {
  if (service_ && service_->isListening() && !handingOffToFullPage_) {
    service_->stopListening();
  }
  if (service_ && deviceUseHeld_) {
    deviceUseHeld_ = false;
    service_->endDeviceUiUse();
  }
  QFrame::hideEvent(event);
}

void PulseQuickPopup::onListenButtonClicked() {
  if (!service_) return;
  if (service_->isListening()) {
    service_->stopListening();
  } else {
    service_->startListening();
  }
}

void PulseQuickPopup::onServiceStateChanged(SongRecognitionService::State state, const QString &message) {
  const bool listening = (state == SongRecognitionService::State::Listening ||
                          state == SongRecognitionService::State::Recognizing);
  listenBtn_->setListening(listening);

  switch (state) {
    case SongRecognitionService::State::Ready:
      statusLabel_->setText(I18n::text(QStringLiteral("pulse.ready_to_find")));
      signalBar_->setValue(0);
      break;

    case SongRecognitionService::State::Listening:
      statusLabel_->setText(I18n::text(QStringLiteral("pulse.listening_title")));
      break;

    case SongRecognitionService::State::Recognizing:
      statusLabel_->setText(I18n::text(QStringLiteral("pulse.searching_song")));
      break;

    case SongRecognitionService::State::Found:
      statusLabel_->setText(I18n::text(QStringLiteral("pulse.song_found")));
      updateResultsList();
      break;

    case SongRecognitionService::State::NotFound:
      statusLabel_->setText(I18n::text(QStringLiteral("pulse.song_not_found")));
      break;

    case SongRecognitionService::State::Error:
      statusLabel_->setText(message.isEmpty() ? I18n::text(QStringLiteral("pulse.capture_error")) : message);
      signalBar_->setValue(0);
      break;
  }
}

void PulseQuickPopup::onServiceVolumeChanged(double levelPercent, double bufferFillPercent, const QString &activeSourceName) {
  Q_UNUSED(activeSourceName);
  signalBar_->setValue(static_cast<int>(levelPercent));
  listenBtn_->setLevel(levelPercent);

  if (service_ && service_->isListening()) {
    if (bufferFillPercent > 0.0 && bufferFillPercent < 70.0) {
      statusLabel_->setText(I18n::text(QStringLiteral("pulse.sample_filling")).arg(static_cast<int>(bufferFillPercent)));
    }
  }
}

void PulseQuickPopup::onServiceSongFound(const SongResult &result) {
  Q_UNUSED(result);
  statusLabel_->setText(I18n::text(QStringLiteral("pulse.song_found")));
  updateResultsList();
}

void PulseQuickPopup::updateResultsList() {
  if (!service_) return;

  // Clear existing result card widgets
  QLayoutItem *child = nullptr;
  while ((child = resultsLayout_->takeAt(0)) != nullptr) {
    if (child->widget()) {
      child->widget()->deleteLater();
    }
    delete child;
  }

  if (!service_->hasActiveResult() || !service_->activeResult().isValid()) {
    emptyLabel_ = new QLabel(I18n::text(QStringLiteral("pulse.no_found_songs")), resultsContainer_);
    emptyLabel_->setObjectName(QStringLiteral("pulse-empty-label"));
    emptyLabel_->setAlignment(Qt::AlignCenter);
    resultsLayout_->addWidget(emptyLabel_);
    return;
  }

  emptyLabel_ = nullptr;
  const SongFinderSettings::OpenPlatform platform = settings_ ? settings_->openPlatform() : SongFinderSettings::OpenPlatform::YouTube;

  const SongResult &res = service_->activeResult();
  auto *card = new PulseResultCard(res, platform, resultsContainer_);
  connect(card, &PulseResultCard::clicked, this, &PulseQuickPopup::onResultCardClicked);
  resultsLayout_->addWidget(card);
}

void PulseQuickPopup::onResultCardClicked(const SongResult &result) {
  const SongFinderSettings::OpenPlatform platform = settings_ ? settings_->openPlatform() : SongFinderSettings::OpenPlatform::YouTube;
  const QUrl url = buildSearchUrl(platform, result.searchQuery());
  if (url.isValid()) {
    emit openUrlRequested(url);
    hide();
  }
}

QUrl PulseQuickPopup::buildSearchUrl(SongFinderSettings::OpenPlatform platform, const QString &query) {
  const QString encoded = QString::fromUtf8(QUrl::toPercentEncoding(query.trimmed()));
  if (platform == SongFinderSettings::OpenPlatform::YouTubeMusic) {
    return QUrl(QStringLiteral("https://music.youtube.com/search?q=%1").arg(encoded));
  }
  return QUrl(QStringLiteral("https://www.youtube.com/results?search_query=%1").arg(encoded));
}

// ============================================================================
// PulseToolbarButton Implementation
// ============================================================================

PulseToolbarButton::PulseToolbarButton(SongRecognitionService *service,
                                       SongFinderSettings *settings,
                                       QWidget *parent)
    : QToolButton(parent), service_(service), settings_(settings) {
  setObjectName(QStringLiteral("pulse-toolbar-button"));
  setIcon(QIcon(QStringLiteral(":/side-widget-icons/pulse.svg")));
  setIconSize(QSize(20, 20));
  setFixedSize(30, 30);
  setCursor(Qt::PointingHandCursor);
  setToolTip(QStringLiteral("DaliNira Pulse"));
  setAccessibleName(QStringLiteral("DaliNira Pulse"));

  setStyleSheet(QStringLiteral(
      "QToolButton#pulse-toolbar-button { background: transparent; border: 0; border-radius: 15px; padding: 2px; }"
      "QToolButton#pulse-toolbar-button:hover { background: #383a3d; }"
  ));

  animTimer_ = new QTimer(this);
  animTimer_->setInterval(50);
  connect(animTimer_, &QTimer::timeout, this, &PulseToolbarButton::onAnimTick);

  if (service_) {
    connect(service_, &SongRecognitionService::stateChanged, this, &PulseToolbarButton::onServiceStateChanged);
  }

  connect(this, &QToolButton::clicked, this, &PulseToolbarButton::toggleQuickPopup);
}

void PulseToolbarButton::toggleQuickPopup() {
  const qint64 now = QDateTime::currentMSecsSinceEpoch();
  if (now - lastClosedMs_ < 220) {
    return;
  }
  if (popup_ && popup_->isVisible()) {
    popup_->hide();
    lastClosedMs_ = QDateTime::currentMSecsSinceEpoch();
    return;
  }
  showQuickPopup();
}

void PulseToolbarButton::showQuickPopup() {
  if (!popup_) {
    popup_ = new PulseQuickPopup(service_, settings_, window());
    connect(popup_, &PulseQuickPopup::openUrlRequested, this, &PulseToolbarButton::openUrlRequested);
    connect(popup_, &PulseQuickPopup::openFullPageRequested, this, &PulseToolbarButton::openFullPageRequested);
    connect(popup_, &PulseQuickPopup::openSettingsRequested, this, &PulseToolbarButton::openSettingsRequested);
  }
  popup_->refreshState();
  const QPoint pos = mapToGlobal(QPoint(width() - popup_->width(), height() + 4));
  popup_->move(pos);
  popup_->show();
  popup_->raise();
  popup_->activateWindow();
}

void PulseToolbarButton::onServiceStateChanged(SongRecognitionService::State state, const QString &message) {
  Q_UNUSED(message);
  const bool listening = (state == SongRecognitionService::State::Listening ||
                          state == SongRecognitionService::State::Recognizing);
  if (listening) {
    if (!animTimer_->isActive()) {
      animTimer_->start();
    }
    setToolTip(I18n::text(QStringLiteral("pulse.toolbar_listening")));
  } else {
    if (animTimer_->isActive()) {
      animTimer_->stop();
    }
    setToolTip(QStringLiteral("DaliNira Pulse"));
  }
  update();
}

void PulseToolbarButton::onAnimTick() {
  animPhase_ += 0.12;
  if (animPhase_ >= 2.0 * M_PI) {
    animPhase_ -= 2.0 * M_PI;
  }
  update();
}

void PulseToolbarButton::paintEvent(QPaintEvent *event) {
  QToolButton::paintEvent(event);

  if (service_ && service_->isListening()) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const qreal dotX = width() - 7.0;
    const qreal dotY = height() - 7.0;
    const double glow = (std::sin(animPhase_) + 1.0) / 2.0;

    // Glowing outer ring
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(56, 189, 248, static_cast<int>(50 + glow * 80)));
    p.drawEllipse(QPointF(dotX, dotY), 4.0 + glow * 1.5, 4.0 + glow * 1.5);

    // Inner bright cyan dot
    p.setBrush(QColor(56, 189, 248));
    p.drawEllipse(QPointF(dotX, dotY), 2.5, 2.5);
  }
}
