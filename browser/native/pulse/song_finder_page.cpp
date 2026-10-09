#include "song_finder_page.h"

#include "big_listen_button.h"
#include "browser_icons.h"

#include <QComboBox>
#include <QDesktopServices>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include "i18n/i18n.h"

namespace {
void loadCoverImage(QLabel *label, const QString &coverUrl, const QString &searchQuery, int size = 58, int radius = 10) {
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
QIcon createPulseMenuIcon(int /*size*/ = 32) {
  QIcon icon;
  for (const int s : {24, 32, 48}) {
    QPixmap pm(s, s);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);

    // Cyan/blue outer circle ring
    QPen ringPen(QColor(QStringLiteral("#38bdf8")), std::max(1.5, s * 0.07));
    p.setPen(ringPen);
    p.setBrush(QColor(15, 23, 42, 210));
    const qreal m = s * 0.08;
    p.drawEllipse(QRectF(m, m, s - 2 * m, s - 2 * m));

    // 3 vertical lines (|||)
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(QStringLiteral("#38bdf8")));
    const qreal barW = std::max(2.0, s * 0.085);
    const qreal barH = s * 0.44;
    const qreal spacing = s * 0.16;
    const qreal cx = s * 0.50;
    const qreal cy = s * 0.50;

    for (int i = -1; i <= 1; ++i) {
      const qreal bx = cx + (i * spacing) - (barW * 0.5);
      const qreal by = cy - (barH * 0.5);
      p.drawRoundedRect(QRectF(bx, by, barW, barH), barW * 0.5, barW * 0.5);
    }

    icon.addPixmap(pm);
  }
  return icon;
}

QString songFinderStyleSheet() {
  return QStringLiteral(R"CSS(
    #song-finder-page {
      background-color: #0d1117;
      color: #e6edf3;
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
    }
    #sf-header {
      background: #161b22;
      border-bottom: 1px solid #30363d;
    }
    #sf-title {
      color: #f0f6fc;
      font-size: 16px;
      font-weight: 700;
      letter-spacing: 0.2px;
    }
    #sf-status {
      color: #7d8590;
      font-size: 12px;
      font-weight: 500;
    }
    #sf-pulse-menu-btn {
      width: 32px;
      height: 32px;
      background: transparent;
      border: none;
      border-radius: 16px;
      padding: 0px;
    }
    #sf-pulse-menu-btn:hover {
      background: rgba(56, 189, 248, 0.18);
    }
    #sf-signal-label {
      color: #8b949e;
      font-size: 11px;
      font-weight: 600;
      text-transform: uppercase;
      letter-spacing: 0.5px;
    }
    #sf-signal-bar {
      min-height: 6px;
      max-height: 6px;
      background: #21262d;
      border: none;
      border-radius: 3px;
    }
    #sf-signal-bar::chunk {
      background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #0284c7, stop:1 #38bdf8);
      border-radius: 3px;
    }
    #sf-state-title {
      color: #f0f6fc;
      font-size: 20px;
      font-weight: 650;
      padding: 0px;
      margin: 0px;
    }
    #sf-state-sub {
      color: #8da4b8;
      font-size: 13px;
      padding: 0px;
      margin: 0px;
    }
    #sf-active-card {
      background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 #1a2736, stop:1 #131c26);
      border: 1.5px solid #0284c7;
      border-radius: 14px;
      padding: 12px 14px;
      margin-bottom: 4px;
    }
    #sf-active-badge {
      color: #38bdf8;
      font-size: 11px;
      font-weight: 700;
      text-transform: uppercase;
      letter-spacing: 0.5px;
    }
    #sf-active-title {
      color: #ffffff;
      font-size: 16px;
      font-weight: 700;
    }
    #sf-active-artist {
      color: #94a3b8;
      font-size: 13px;
      font-weight: 550;
    }
    #sf-active-meta {
      color: #64748b;
      font-size: 11px;
    }
    #sf-list-toggle-btn {
      min-height: 38px;
      background: #141c26;
      color: #94a3b8;
      border: 1px solid #283749;
      border-radius: 10px;
      padding: 0 16px;
      font-size: 13px;
      font-weight: 600;
      text-align: left;
    }
    #sf-list-toggle-btn:hover {
      background: #1c2838;
      border-color: #38bdf8;
      color: #f1f5f9;
    }
    #sf-results-card {
      background: #171e27;
      border: 1px solid #283543;
      border-radius: 12px;
    }
    #sf-empty-label {
      color: #64748b;
      font-size: 14px;
      padding: 24px;
    }
    #sf-result-item {
      background: #1e2835;
      border: 1px solid #314254;
      border-radius: 10px;
      padding: 10px 14px;
    }
    #sf-result-item:hover {
      background: #243242;
      border-color: #3e536a;
    }
    #sf-result-title {
      color: #f1f5f9;
      font-size: 14px;
      font-weight: 650;
    }
    #sf-result-artist {
      color: #94a3b8;
      font-size: 13px;
    }
    #sf-result-meta {
      color: #64748b;
      font-size: 11px;
    }
    #sf-platform-btn {
      min-height: 28px;
      background: #25384a;
      color: #edf5fc;
      border: 1px solid #40576b;
      border-radius: 6px;
      padding: 2px 10px;
      font-size: 12px;
      font-weight: 550;
    }
    #sf-platform-btn:hover {
      background: #2d475d;
      border-color: #54728a;
    }
    #sf-delete-card-btn {
      width: 28px;
      height: 28px;
      min-width: 28px;
      min-height: 28px;
      background: #223242;
      color: #94a3b8;
      border: 1px solid #3c5064;
      border-radius: 6px;
      padding: 0px;
    }
    #sf-delete-card-btn:hover {
      background: rgba(239, 68, 68, 0.22);
      border-color: #ef4444;
      color: #fca5a5;
    }
    #sf-results-header {
      border-bottom: 1px solid #233140;
      padding: 0 2px 8px 2px;
      margin-bottom: 2px;
    }
    #sf-results-title {
      color: #94a3b8;
      font-size: 13px;
      font-weight: 700;
    }
    #sf-clear-all-btn {
      background: transparent;
      color: #94a3b8;
      border: 1px solid transparent;
      border-radius: 6px;
      padding: 3px 8px;
      font-size: 11px;
      font-weight: 500;
    }
    #sf-clear-all-btn:hover {
      background: rgba(239, 68, 68, 0.15);
      border-color: rgba(239, 68, 68, 0.35);
      color: #f87171;
    }
    #sf-controls-card {
      background: #171e27;
      border: 1px solid #283543;
      border-radius: 12px;
    }
    #sf-device-label {
      color: #94a3b8;
      font-size: 12px;
      font-weight: 600;
    }
    #sf-device-combo {
      min-height: 32px;
      background: #10161d;
      color: #e6edf5;
      border: 1px solid #3a4958;
      border-radius: 7px;
      padding: 0 10px;
    }
    #sf-device-combo:focus {
      border-color: #38bdf8;
    }
    #sf-refresh-btn {
      min-height: 32px;
      background: #1f2937;
      color: #cbd5e1;
      border: 1px solid #374759;
      border-radius: 7px;
      padding: 0 14px;
      font-size: 12px;
      font-weight: 550;
    }
    #sf-refresh-btn:hover {
      background: #283747;
      border-color: #526579;
    }
    #sf-hint-label {
      color: #64748b;
      font-size: 11px;
    }
  )CSS");
}
}  // namespace

class PulseMenuPopup final : public QFrame {
 public:
  explicit PulseMenuPopup(QWidget *parent = nullptr)
      : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint) {
    setObjectName(QStringLiteral("sf-menu-popup"));
    setAttribute(Qt::WA_DeleteOnClose, false);
    setFixedWidth(220);
    setStyleSheet(QStringLiteral(
      "QFrame#sf-menu-popup {"
      "  background: #141b24;"
      "  border: 1px solid #2d3c4e;"
      "  border-radius: 10px;"
      "  padding: 6px;"
      "}"
      "QPushButton.sf-popup-btn {"
      "  background: transparent;"
      "  color: #e2ecf6;"
      "  border: none;"
      "  border-radius: 6px;"
      "  padding: 8px 12px;"
      "  font-size: 13px;"
      "  font-weight: 550;"
      "  text-align: left;"
      "}"
      "QPushButton.sf-popup-btn:hover {"
      "  background: #223040;"
      "  color: #ffffff;"
      "}"
      "QLabel#sf-theme-label {"
      "  color: #8da4b8;"
      "  font-size: 11px;"
      "  font-weight: 600;"
      "  padding: 6px 12px 2px 12px;"
      "}"
      "QComboBox#sf-theme-combo {"
      "  background: #0d1217;"
      "  color: #e2ecf6;"
      "  border: 1px solid #2b3949;"
      "  border-radius: 6px;"
      "  padding: 4px 8px;"
      "  font-size: 12px;"
      "  margin: 0 8px 4px 8px;"
      "}"
    ));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(2);

    prefBtn_ = new QPushButton(I18n::text(QStringLiteral("pulse.preferences")), this);
    prefBtn_->setProperty("class", "sf-popup-btn");
    prefBtn_->setCursor(Qt::PointingHandCursor);
    layout->addWidget(prefBtn_);

    aboutBtn_ = new QPushButton(I18n::text(QStringLiteral("common.about")), this);
    aboutBtn_->setProperty("class", "sf-popup-btn");
    aboutBtn_->setCursor(Qt::PointingHandCursor);
    layout->addWidget(aboutBtn_);

    auto *sep = new QFrame(this);
    sep->setFrameShape(QFrame::HLine);
    sep->setStyleSheet(QStringLiteral("background: #233140; min-height: 1px; max-height: 1px; margin: 4px 6px;"));
    layout->addWidget(sep);

    auto *themeLabel = new QLabel(I18n::text(QStringLiteral("pulse.theme")), this);
    themeLabel->setObjectName(QStringLiteral("sf-theme-label"));
    layout->addWidget(themeLabel);

    themeCombo_ = new QComboBox(this);
    themeCombo_->setObjectName(QStringLiteral("sf-theme-combo"));
    themeCombo_->addItem(I18n::text(QStringLiteral("pulse.theme_app")));
    themeCombo_->addItem(I18n::text(QStringLiteral("pulse.theme_dark")));
    themeCombo_->addItem(I18n::text(QStringLiteral("pulse.theme_light")));
    layout->addWidget(themeCombo_);
  }

  QPushButton *prefBtn() const { return prefBtn_; }
  QPushButton *aboutBtn() const { return aboutBtn_; }
  QComboBox *themeCombo() const { return themeCombo_; }

 private:
  QPushButton *prefBtn_ = nullptr;
  QPushButton *aboutBtn_ = nullptr;
  QComboBox *themeCombo_ = nullptr;
};

SongFinderPage::SongFinderPage(SongRecognitionService *service, QWidget *parent)
    : QWidget(parent), service_(service) {
  setObjectName(QStringLiteral("song-finder-page"));
  setStyleSheet(songFinderStyleSheet());
  setupUi();

  if (service_) {
    service_->beginDeviceUiUse();
    connect(service_, &SongRecognitionService::stateChanged, this, &SongFinderPage::onServiceStateChanged);
    connect(service_, &SongRecognitionService::volumeChanged, this, &SongFinderPage::onServiceVolumeChanged);
    connect(service_, &SongRecognitionService::songFound, this, &SongFinderPage::onServiceSongFound);
    connect(service_, &SongRecognitionService::devicesUpdated, this, &SongFinderPage::onServiceDevicesUpdated);
    connect(service_, &SongRecognitionService::autoOpenRequested, this, &SongFinderPage::onAutoOpenRequested);
    connect(service_, &SongRecognitionService::activeResultChanged, this, [this](const SongResult &res, bool hasActive) {
      if (hasActive) {
        showActiveResult(res);
      } else {
        hideActiveResult();
      }
    });

    // Initial population from history
    const auto history = service_->foundHistory();
    for (int i = history.size() - 1; i >= 0; --i) {
      addResultCard(history.at(i));
    }
    updateHistoryListHeader();

    if (service_->hasActiveResult() && service_->activeResult().isValid()) {
      showActiveResult(service_->activeResult());
    }

    currentRoute_ = service_->currentRoute();
    updateDeviceList(service_->cachedDevices(), currentRoute_);
    onServiceStateChanged(service_->state(), service_->stateMessage());
  }
}

SongFinderPage::~SongFinderPage() {
  if (service_ && service_->isListening()) {
    service_->stopListening();
  }
  if (service_) service_->endDeviceUiUse();
}

void SongFinderPage::setupUi() {
  auto *rootLayout = new QVBoxLayout(this);
  rootLayout->setContentsMargins(0, 0, 0, 0);
  rootLayout->setSpacing(0);

  // Top Header Bar
  auto *header = new QWidget(this);
  header->setObjectName(QStringLiteral("sf-header"));
  auto *headerLayout = new QHBoxLayout(header);
  headerLayout->setContentsMargins(24, 14, 24, 14);
  headerLayout->setSpacing(14);

  auto *titleLabel = new QLabel(QStringLiteral("DaliNira Pulse"), header);
  titleLabel->setObjectName(QStringLiteral("sf-title"));
  headerLayout->addWidget(titleLabel);
  headerLayout->addStretch(1);

  statusLabel_ = new QLabel(I18n::text(QStringLiteral("common.ready")), header);
  statusLabel_->setObjectName(QStringLiteral("sf-status"));
  headerLayout->addWidget(statusLabel_);

  menuBtn_ = new QPushButton(header);
  menuBtn_->setObjectName(QStringLiteral("sf-pulse-menu-btn"));
  menuBtn_->setIcon(createPulseMenuIcon(32));
  menuBtn_->setIconSize(QSize(30, 30));
  menuBtn_->setFixedSize(34, 34);
  menuBtn_->setCursor(Qt::PointingHandCursor);
  menuBtn_->setToolTip(I18n::text(QStringLiteral("common.options")));
  connect(menuBtn_, &QPushButton::clicked, this, &SongFinderPage::toggleMenuPopup);

  headerLayout->addWidget(menuBtn_);
  rootLayout->addWidget(header);

  // Scrollable Body
  auto *scrollArea = new QScrollArea(this);
  scrollArea->setWidgetResizable(true);
  scrollArea->setFrameShape(QFrame::NoFrame);
  scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

  auto *bodyWidget = new QWidget(scrollArea);
  auto *bodyOuterLayout = new QHBoxLayout(bodyWidget);
  bodyOuterLayout->setContentsMargins(20, 16, 20, 20);
  bodyOuterLayout->addStretch(1);

  auto *centerColumn = new QWidget(bodyWidget);
  centerColumn->setMaximumWidth(780);
  centerColumn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  auto *bodyLayout = new QVBoxLayout(centerColumn);
  bodyLayout->setContentsMargins(0, 0, 0, 0);
  bodyLayout->setSpacing(14);

  // Signal Level Indicator
  auto *signalContainer = new QWidget(centerColumn);
  auto *signalLayout = new QVBoxLayout(signalContainer);
  signalLayout->setContentsMargins(0, 0, 0, 0);
  signalLayout->setSpacing(4);

  auto *signalHeader = new QHBoxLayout();
  signalTitle_ = new QLabel(I18n::text(QStringLiteral("pulse.source_signal")), signalContainer);
  signalTitle_->setObjectName(QStringLiteral("sf-signal-label"));
  signalHeader->addWidget(signalTitle_);
  signalHeader->addStretch(1);
  signalLayout->addLayout(signalHeader);

  signalBar_ = new QProgressBar(signalContainer);
  signalBar_->setObjectName(QStringLiteral("sf-signal-bar"));
  signalBar_->setRange(0, 100);
  signalBar_->setValue(0);
  signalBar_->setTextVisible(false);
  signalLayout->addWidget(signalBar_);
  bodyLayout->addWidget(signalContainer);

  // Big Listen Button Stage
  auto *stageWidget = new QWidget(centerColumn);
  auto *stageLayout = new QVBoxLayout(stageWidget);
  stageLayout->setContentsMargins(0, 10, 0, 10);
  stageLayout->setAlignment(Qt::AlignCenter);

  listenBtn_ = new BigListenButton(210, stageWidget);
  connect(listenBtn_, &QAbstractButton::clicked, this, &SongFinderPage::onBigListenButtonClicked);
  stageLayout->addWidget(listenBtn_, 0, Qt::AlignCenter);

  auto *statusContainer = new QWidget(stageWidget);
  statusContainer->setMinimumHeight(64);
  auto *statusLayout = new QVBoxLayout(statusContainer);
  statusLayout->setContentsMargins(0, 8, 0, 4);
  statusLayout->setSpacing(4);
  statusLayout->setAlignment(Qt::AlignCenter);

  stateTitle_ = new QLabel(I18n::text(QStringLiteral("pulse.ready_to_listen")), statusContainer);
  stateTitle_->setObjectName(QStringLiteral("sf-state-title"));
  stateTitle_->setAlignment(Qt::AlignCenter);
  stateTitle_->setMinimumHeight(32);
  statusLayout->addWidget(stateTitle_, 0, Qt::AlignCenter);

  stateSub_ = new QLabel(I18n::text(QStringLiteral("pulse.press_to_start")), statusContainer);
  stateSub_->setObjectName(QStringLiteral("sf-state-sub"));
  stateSub_->setAlignment(Qt::AlignCenter);
  stateSub_->setMinimumHeight(24);
  statusLayout->addWidget(stateSub_, 0, Qt::AlignCenter);

  stageLayout->addWidget(statusContainer, 0, Qt::AlignCenter);
  bodyLayout->addWidget(stageWidget);

  // Active Hero Result Card (Son Bulunan Şarkı)
  activeCard_ = new QFrame(centerColumn);
  activeCard_->setObjectName(QStringLiteral("sf-active-card"));
  auto *activeLayout = new QHBoxLayout(activeCard_);
  activeLayout->setContentsMargins(14, 12, 14, 12);
  activeLayout->setSpacing(14);

  activeCoverLabel_ = new QLabel(activeCard_);
  activeCoverLabel_->setFixedSize(58, 58);
  activeCoverLabel_->setStyleSheet(QStringLiteral("background:#0f172a;border-radius:10px;border:1px solid #334155;"));
  activeCoverLabel_->setAlignment(Qt::AlignCenter);
  activeLayout->addWidget(activeCoverLabel_);

  auto *activeTextLayout = new QVBoxLayout();
  activeTextLayout->setContentsMargins(0, 0, 0, 0);
  activeTextLayout->setSpacing(2);

  auto *badgeLabel = new QLabel(I18n::text(QStringLiteral("pulse.last_found_song")), activeCard_);
  badgeLabel->setObjectName(QStringLiteral("sf-active-badge"));
  activeTextLayout->addWidget(badgeLabel);

  activeTitleLabel_ = new QLabel(activeCard_);
  activeTitleLabel_->setObjectName(QStringLiteral("sf-active-title"));
  activeTextLayout->addWidget(activeTitleLabel_);

  activeArtistLabel_ = new QLabel(activeCard_);
  activeArtistLabel_->setObjectName(QStringLiteral("sf-active-artist"));
  activeTextLayout->addWidget(activeArtistLabel_);

  activeMetaLabel_ = new QLabel(activeCard_);
  activeMetaLabel_->setObjectName(QStringLiteral("sf-active-meta"));
  activeTextLayout->addWidget(activeMetaLabel_);

  activeLayout->addLayout(activeTextLayout, 1);

  auto *activeBtnLayout = new QHBoxLayout();
  activeBtnLayout->setSpacing(8);

  activeYtBtn_ = new QPushButton(QStringLiteral(" YouTube"), activeCard_);
  activeYtBtn_->setObjectName(QStringLiteral("sf-platform-btn"));
  activeYtBtn_->setIcon(BrowserIcons::youtubeIcon());
  activeYtBtn_->setIconSize(QSize(16, 16));
  activeYtBtn_->setCursor(Qt::PointingHandCursor);
  connect(activeYtBtn_, &QPushButton::clicked, this, [this]() {
    const QUrl url = buildSearchUrl(SongFinderSettings::OpenPlatform::YouTube, currentActiveResult_.searchQuery());
    emit openUrlRequested(url);
  });
  activeBtnLayout->addWidget(activeYtBtn_);

  activeYtmBtn_ = new QPushButton(QStringLiteral(" YouTube Music"), activeCard_);
  activeYtmBtn_->setObjectName(QStringLiteral("sf-platform-btn"));
  activeYtmBtn_->setIcon(BrowserIcons::youtubeMusicIcon());
  activeYtmBtn_->setIconSize(QSize(16, 16));
  activeYtmBtn_->setCursor(Qt::PointingHandCursor);
  connect(activeYtmBtn_, &QPushButton::clicked, this, [this]() {
    const QUrl url = buildSearchUrl(SongFinderSettings::OpenPlatform::YouTubeMusic, currentActiveResult_.searchQuery());
    emit openUrlRequested(url);
  });
  activeBtnLayout->addWidget(activeYtmBtn_);

  activeDismissBtn_ = new QPushButton(activeCard_);
  activeDismissBtn_->setObjectName(QStringLiteral("sf-delete-card-btn"));
  activeDismissBtn_->setIcon(BrowserIcons::icon(BrowserIcon::Trash));
  activeDismissBtn_->setIconSize(QSize(15, 15));
  activeDismissBtn_->setToolTip(I18n::text(QStringLiteral("pulse.dismiss_card")));
  activeDismissBtn_->setCursor(Qt::PointingHandCursor);
  connect(activeDismissBtn_, &QPushButton::clicked, this, [this]() {
    if (hasActiveResult_) {
      addResultCard(currentActiveResult_);
    }
    if (service_) {
      service_->dismissActiveResult();
    }
    hideActiveResult();
    updateHistoryListHeader();
  });
  activeBtnLayout->addWidget(activeDismissBtn_);

  activeLayout->addLayout(activeBtnLayout);
  activeCard_->hide();
  bodyLayout->addWidget(activeCard_);

  // History List Toggle Button
  listToggleBtn_ = new QPushButton(centerColumn);
  listToggleBtn_->setObjectName(QStringLiteral("sf-list-toggle-btn"));
  listToggleBtn_->setCursor(Qt::PointingHandCursor);
  listToggleBtn_->setText(I18n::text(QStringLiteral("pulse.results_list_toggle_empty")));
  connect(listToggleBtn_, &QPushButton::clicked, this, [this]() {
    setListExpanded(!isListExpanded_);
  });
  bodyLayout->addWidget(listToggleBtn_);

  // Found Results History List Container Card
  resultsContainer_ = new QFrame(centerColumn);
  resultsContainer_->setObjectName(QStringLiteral("sf-results-card"));
  resultsLayout_ = new QVBoxLayout(resultsContainer_);
  resultsLayout_->setContentsMargins(14, 14, 14, 14);
  resultsLayout_->setSpacing(10);

  resultsHeaderWidget_ = new QWidget(resultsContainer_);
  resultsHeaderWidget_->setObjectName(QStringLiteral("sf-results-header"));
  auto *resHeaderLayout = new QHBoxLayout(resultsHeaderWidget_);
  resHeaderLayout->setContentsMargins(2, 0, 2, 4);
  resHeaderLayout->setSpacing(8);

  resultsTitleLabel_ = new QLabel(I18n::text(QStringLiteral("pulse.found_results")), resultsHeaderWidget_);
  resultsTitleLabel_->setObjectName(QStringLiteral("sf-results-title"));
  resHeaderLayout->addWidget(resultsTitleLabel_);
  resHeaderLayout->addStretch(1);

  clearAllBtn_ = new QPushButton(I18n::text(QStringLiteral("pulse.clear_all")), resultsHeaderWidget_);
  clearAllBtn_->setObjectName(QStringLiteral("sf-clear-all-btn"));
  clearAllBtn_->setIcon(BrowserIcons::icon(BrowserIcon::Trash));
  clearAllBtn_->setIconSize(QSize(13, 13));
  clearAllBtn_->setCursor(Qt::PointingHandCursor);
  connect(clearAllBtn_, &QPushButton::clicked, this, [this]() {
    if (service_) {
      service_->clearHistory();
    }
    const auto items = resultsContainer_->findChildren<QWidget *>(QStringLiteral("sf-result-item"));
    for (auto *item : items) {
      item->deleteLater();
    }
    hideActiveResult();
    QTimer::singleShot(30, this, [this]() {
      updateHistoryListHeader();
    });
  });
  resHeaderLayout->addWidget(clearAllBtn_);
  resultsHeaderWidget_->hide();
  resultsLayout_->addWidget(resultsHeaderWidget_);

  emptyResultsLabel_ = new QLabel(I18n::text(QStringLiteral("pulse.no_songs")), resultsContainer_);
  emptyResultsLabel_->setObjectName(QStringLiteral("sf-empty-label"));
  emptyResultsLabel_->setAlignment(Qt::AlignCenter);
  resultsLayout_->addWidget(emptyResultsLabel_);

  resultsContainer_->hide();
  bodyLayout->addWidget(resultsContainer_);

  // Bottom Audio Controls Card
  auto *controlsCard = new QFrame(centerColumn);
  controlsCard->setObjectName(QStringLiteral("sf-controls-card"));
  auto *controlsLayout = new QVBoxLayout(controlsCard);
  controlsLayout->setContentsMargins(16, 14, 16, 14);
  controlsLayout->setSpacing(10);

  auto *deviceField = new QWidget(controlsCard);
  auto *deviceLayout = new QHBoxLayout(deviceField);
  deviceLayout->setContentsMargins(0, 0, 0, 0);
  deviceLayout->setSpacing(10);

  auto *deviceLabel = new QLabel(I18n::text(QStringLiteral("pulse.audio_source")), deviceField);
  deviceLabel->setObjectName(QStringLiteral("sf-device-label"));
  deviceLayout->addWidget(deviceLabel);

  deviceCombo_ = new QComboBox(deviceField);
  deviceCombo_->setObjectName(QStringLiteral("sf-device-combo"));
  deviceCombo_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  connect(deviceCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &SongFinderPage::onDeviceSelectionChanged);
  deviceLayout->addWidget(deviceCombo_, 1);

  refreshBtn_ = new QPushButton(I18n::text(QStringLiteral("pulse.refresh_devices")), deviceField);
  refreshBtn_->setObjectName(QStringLiteral("sf-refresh-btn"));
  refreshBtn_->setCursor(Qt::PointingHandCursor);
  connect(refreshBtn_, &QPushButton::clicked, this, &SongFinderPage::onRefreshDevicesClicked);
  deviceLayout->addWidget(refreshBtn_);

  controlsLayout->addWidget(deviceField);

  hintLabel_ = new QLabel(controlsCard);
  hintLabel_->setObjectName(QStringLiteral("sf-hint-label"));
  hintLabel_->setText(I18n::text(QStringLiteral("pulse.volume_hint")));
  controlsLayout->addWidget(hintLabel_);

  bodyLayout->addWidget(controlsCard);

  bodyOuterLayout->addWidget(centerColumn);
  bodyOuterLayout->addStretch(1);

  scrollArea->setWidget(bodyWidget);
  rootLayout->addWidget(scrollArea, 1);
}

void SongFinderPage::setListExpanded(bool expanded) {
  isListExpanded_ = expanded;
  if (resultsContainer_) {
    resultsContainer_->setVisible(expanded);
  }
  updateHistoryListHeader();
}

void SongFinderPage::toggleMenuPopup() {
  if (!menuPopup_) {
    menuPopup_ = new PulseMenuPopup(this);
    connect(menuPopup_->prefBtn(), &QPushButton::clicked, this, [this]() {
      menuPopup_->hide();
      emit openPreferencesRequested();
    });
    connect(menuPopup_->aboutBtn(), &QPushButton::clicked, this, [this]() {
      menuPopup_->hide();
      showAboutDialog();
    });
  }

  if (menuPopup_->isVisible()) {
    menuPopup_->hide();
    return;
  }

  const QPoint globalPos = menuBtn_->mapToGlobal(QPoint(0, menuBtn_->height() + 4));
  const int popupX = globalPos.x() + menuBtn_->width() - menuPopup_->width();
  menuPopup_->move(popupX, globalPos.y());
  menuPopup_->show();
}

void SongFinderPage::updateHistoryListHeader() {
  int count = 0;
  if (resultsContainer_) {
    const auto items = resultsContainer_->findChildren<QWidget *>(QStringLiteral("sf-result-item"));
    count = items.size();
    if (count == 0) {
      if (emptyResultsLabel_) emptyResultsLabel_->show();
      if (resultsHeaderWidget_) resultsHeaderWidget_->hide();
    } else {
      if (emptyResultsLabel_) emptyResultsLabel_->hide();
      if (resultsHeaderWidget_) resultsHeaderWidget_->show();
      if (resultsTitleLabel_) resultsTitleLabel_->setText(I18n::text(QStringLiteral("pulse.found_results_count")).arg(count));
    }
  }

  if (listToggleBtn_) {
    listToggleBtn_->setText(I18n::text(QStringLiteral("pulse.results_list_toggle"))
                                .arg(count)
                                .arg(isListExpanded_ ? QStringLiteral("▴") : QStringLiteral("▾")));
  }
}

void SongFinderPage::showActiveResult(const SongResult &result) {
  currentActiveResult_ = result;
  hasActiveResult_ = true;
  activeTitleLabel_->setText(result.title.isEmpty() ? I18n::text(QStringLiteral("pulse.unknown_title")) : result.title);
  activeArtistLabel_->setText(result.artist.isEmpty() ? I18n::text(QStringLiteral("pulse.unknown_artist")) : result.artist);

  QString metaStr = result.timestamp.toString(QStringLiteral("dd.MM.yyyy hh:mm"));
  if (!result.album.isEmpty()) {
    metaStr += QStringLiteral(" • ") + result.album;
  }
  if (!result.genre.isEmpty()) {
    metaStr += QStringLiteral(" • ") + result.genre;
  }
  metaStr += QStringLiteral(" • ") + result.sourceDisplayName();
  activeMetaLabel_->setText(metaStr);

  loadCoverImage(activeCoverLabel_, result.coverUrl, result.searchQuery(), 58, 10);
  activeCard_->show();
}

void SongFinderPage::hideActiveResult() {
  hasActiveResult_ = false;
  if (activeCard_) {
    activeCard_->hide();
  }
}

void SongFinderPage::updateDeviceList(const QVector<AudioDeviceInfo> &devices, const AutoRouteInfo &route) {
  currentRoute_ = route;
  updatingDevices_ = true;

  deviceCombo_->clear();

  // 1. Smart Auto-Route Option
  QString autoSummary = I18n::text(QStringLiteral("pulse.auto_source_summary"));
  deviceCombo_->addItem(autoSummary, QStringLiteral("auto"));

  // 2. Individual hardware devices
  for (const auto &dev : devices) {
    QString label = dev.label;
    if (dev.isMonitor) {
      label = QStringLiteral("🔊 ") + label;
    } else {
      label = QStringLiteral("🎤 ") + label;
    }
    if (dev.isDefault) {
      label += I18n::text(QStringLiteral("pulse.default_suffix"));
    }
    deviceCombo_->addItem(label, dev.id);
  }

  // Restore current selection
  if (service_) {
    const QString activeId = service_->currentDeviceId();
    if (activeId.isEmpty() || activeId == QStringLiteral("auto")) {
      deviceCombo_->setCurrentIndex(0);
    } else {
      const int idx = deviceCombo_->findData(activeId);
      if (idx >= 0) {
        deviceCombo_->setCurrentIndex(idx);
      }
    }
  }

  if (route.hasSystemMonitor || route.hasMicrophone) {
    hintLabel_->setText(route.summaryDescription());
  } else {
    hintLabel_->setText(I18n::text(QStringLiteral("pulse.devices_ready")));
  }

  updatingDevices_ = false;
}

void SongFinderPage::onBigListenButtonClicked() {
  if (!service_) return;
  if (service_->isListening()) {
    service_->stopListening();
  } else {
    const QString devId = deviceCombo_->currentData().toString();
    service_->startListening(devId);
  }
}

void SongFinderPage::onRefreshDevicesClicked() {
  if (!service_) return;
  service_->refreshDevices();
}

void SongFinderPage::onDeviceSelectionChanged(int index) {
  if (updatingDevices_ || !service_) return;
  const QString deviceId = deviceCombo_->itemData(index).toString();

  if (service_->settings() && service_->settings()->rememberAudioDevice()) {
    service_->settings()->setSavedDeviceId(deviceId);
    service_->settings()->save();
  }

  if (service_->isListening()) {
    service_->startListening(deviceId);
  }
}

void SongFinderPage::onServiceStateChanged(SongRecognitionService::State state, const QString &message) {
  Q_UNUSED(message);
  const bool listening = (state == SongRecognitionService::State::Listening ||
                          state == SongRecognitionService::State::Recognizing);
  listenBtn_->setListening(listening);

  switch (state) {
    case SongRecognitionService::State::Ready:
      statusLabel_->setText(I18n::text(QStringLiteral("common.ready")));
      stateTitle_->setText(I18n::text(QStringLiteral("pulse.ready_to_listen")));
      stateSub_->setText(I18n::text(QStringLiteral("pulse.press_to_start")));
      signalBar_->setValue(0);
      signalTitle_->setText(I18n::text(QStringLiteral("pulse.source_signal")));
      break;

    case SongRecognitionService::State::Listening: {
      statusLabel_->setText(I18n::text(QStringLiteral("pulse.listening_status")));
      stateTitle_->setText(I18n::text(QStringLiteral("pulse.listening_title")));
      stateSub_->setText(I18n::text(QStringLiteral("pulse.listening_desc")));
      break;
    }

    case SongRecognitionService::State::Recognizing:
      statusLabel_->setText(I18n::text(QStringLiteral("pulse.searching_song")));
      stateTitle_->setText(I18n::text(QStringLiteral("pulse.searching_song")));
      stateSub_->setText(I18n::text(QStringLiteral("pulse.scanning_fingerprint")));
      break;

    case SongRecognitionService::State::Found:
      statusLabel_->setText(I18n::text(QStringLiteral("pulse.song_found")));
      stateTitle_->setText(I18n::text(QStringLiteral("pulse.song_found")));
      stateSub_->setText(I18n::text(QStringLiteral("pulse.result_shown")));
      break;

    case SongRecognitionService::State::NotFound:
      statusLabel_->setText(I18n::text(QStringLiteral("pulse.song_not_found")));
      stateTitle_->setText(I18n::text(QStringLiteral("pulse.song_not_found")));
      stateSub_->setText(I18n::text(QStringLiteral("pulse.still_listening")));
      break;

    case SongRecognitionService::State::Error:
      statusLabel_->setText(I18n::text(QStringLiteral("common.error")));
      stateTitle_->setText(I18n::text(QStringLiteral("pulse.listening_error")));
      stateSub_->setText(message.isEmpty() ? I18n::text(QStringLiteral("pulse.source_unavailable")) : message);
      break;
  }
}

void SongFinderPage::onServiceVolumeChanged(double levelPercent, double bufferFillPercent, const QString &activeSourceName) {
  signalBar_->setValue(static_cast<int>(levelPercent));
  listenBtn_->setLevel(levelPercent);

  if (service_ && service_->isListening()) {
    if (!activeSourceName.isEmpty()) {
      signalTitle_->setText(I18n::text(QStringLiteral("pulse.source_signal_named")).arg(activeSourceName));
    } else {
      signalTitle_->setText(I18n::text(QStringLiteral("pulse.source_signal")));
    }

    if (bufferFillPercent > 0.0 && bufferFillPercent < 70.0) {
      stateSub_->setText(I18n::text(QStringLiteral("pulse.sample_filling")).arg(static_cast<int>(bufferFillPercent)));
    }
  }
}

void SongFinderPage::onServiceSongFound(const SongResult &result) {
  // Show active hero result card immediately, without dropping it into the history list prematurely
  showActiveResult(result);
}

void SongFinderPage::onServiceDevicesUpdated(const QVector<AudioDeviceInfo> &devices, const AutoRouteInfo &route) {
  updateDeviceList(devices, route);
}

void SongFinderPage::onAutoOpenRequested(const SongResult &result, SongFinderSettings::OpenPlatform platform) {
  const QUrl url = buildSearchUrl(platform, result.searchQuery());
  if (url.isValid()) {
    emit openUrlRequested(url);
  }
}

QUrl SongFinderPage::buildSearchUrl(SongFinderSettings::OpenPlatform platform, const QString &query) const {
  const QString encoded = QString::fromUtf8(QUrl::toPercentEncoding(query));
  if (platform == SongFinderSettings::OpenPlatform::YouTubeMusic) {
    return QUrl(QStringLiteral("https://music.youtube.com/search?q=%1").arg(encoded));
  }
  return QUrl(QStringLiteral("https://www.youtube.com/results?search_query=%1").arg(encoded));
}

void SongFinderPage::addResultCard(const SongResult &result) {
  // Avoid duplicate widgets in the history list
  const auto existingItems = resultsContainer_->findChildren<QWidget *>(QStringLiteral("sf-result-item"));
  for (auto *w : existingItems) {
    if (w->property("dedupeKey").toString() == result.dedupeKey()) {
      return;
    }
  }

  auto *itemWidget = new QWidget(resultsContainer_);
  itemWidget->setObjectName(QStringLiteral("sf-result-item"));
  itemWidget->setProperty("dedupeKey", result.dedupeKey());
  auto *itemLayout = new QHBoxLayout(itemWidget);
  itemLayout->setContentsMargins(12, 10, 12, 10);
  itemLayout->setSpacing(14);

  // Cover Image
  auto *coverLabel = new QLabel(itemWidget);
  coverLabel->setFixedSize(54, 54);
  coverLabel->setStyleSheet(QStringLiteral("background:#0f172a;border-radius:8px;border:1px solid #334155;"));
  coverLabel->setAlignment(Qt::AlignCenter);
  loadCoverImage(coverLabel, result.coverUrl, result.searchQuery(), 54, 8);
  itemLayout->addWidget(coverLabel);

  // Text Metadata
  auto *textLayout = new QVBoxLayout();
  textLayout->setContentsMargins(0, 0, 0, 0);
  textLayout->setSpacing(2);

  auto *titleLabel = new QLabel(result.title.isEmpty() ? I18n::text(QStringLiteral("pulse.unknown_title")) : result.title, itemWidget);
  titleLabel->setObjectName(QStringLiteral("sf-result-title"));
  textLayout->addWidget(titleLabel);

  auto *artistLabel = new QLabel(result.artist.isEmpty() ? I18n::text(QStringLiteral("pulse.unknown_artist")) : result.artist, itemWidget);
  artistLabel->setObjectName(QStringLiteral("sf-result-artist"));
  textLayout->addWidget(artistLabel);

  QString metaStr = result.timestamp.toString(QStringLiteral("dd.MM.yyyy hh:mm"));
  if (!result.album.isEmpty()) {
    metaStr += QStringLiteral(" • ") + result.album;
  }
  if (!result.genre.isEmpty()) {
    metaStr += QStringLiteral(" • ") + result.genre;
  }
  metaStr += QStringLiteral(" • ") + result.sourceDisplayName();

  auto *metaLabel = new QLabel(metaStr, itemWidget);
  metaLabel->setObjectName(QStringLiteral("sf-result-meta"));
  textLayout->addWidget(metaLabel);

  itemLayout->addLayout(textLayout, 1);

  // Action Buttons & Delete Button
  auto *btnLayout = new QHBoxLayout();
  btnLayout->setSpacing(8);

  auto *ytBtn = new QPushButton(QStringLiteral(" YouTube"), itemWidget);
  ytBtn->setObjectName(QStringLiteral("sf-platform-btn"));
  ytBtn->setIcon(BrowserIcons::youtubeIcon());
  ytBtn->setIconSize(QSize(16, 16));
  ytBtn->setCursor(Qt::PointingHandCursor);
  connect(ytBtn, &QPushButton::clicked, this, [this, result]() {
    const QUrl url = buildSearchUrl(SongFinderSettings::OpenPlatform::YouTube, result.searchQuery());
    emit openUrlRequested(url);
  });
  btnLayout->addWidget(ytBtn);

  auto *ytmBtn = new QPushButton(QStringLiteral(" YouTube Music"), itemWidget);
  ytmBtn->setObjectName(QStringLiteral("sf-platform-btn"));
  ytmBtn->setIcon(BrowserIcons::youtubeMusicIcon());
  ytmBtn->setIconSize(QSize(16, 16));
  ytmBtn->setCursor(Qt::PointingHandCursor);
  connect(ytmBtn, &QPushButton::clicked, this, [this, result]() {
    const QUrl url = buildSearchUrl(SongFinderSettings::OpenPlatform::YouTubeMusic, result.searchQuery());
    emit openUrlRequested(url);
  });
  btnLayout->addWidget(ytmBtn);

  auto *deleteBtn = new QPushButton(itemWidget);
  deleteBtn->setObjectName(QStringLiteral("sf-delete-card-btn"));
  deleteBtn->setIcon(BrowserIcons::icon(BrowserIcon::Trash));
  deleteBtn->setIconSize(QSize(15, 15));
  deleteBtn->setToolTip(I18n::text(QStringLiteral("pulse.delete_result")));
  deleteBtn->setCursor(Qt::PointingHandCursor);
  connect(deleteBtn, &QPushButton::clicked, this, [this, itemWidget, result]() {
    itemWidget->deleteLater();
    if (service_) {
      service_->removeHistoryItem(result.dedupeKey());
    }
    QTimer::singleShot(30, this, [this]() {
      updateHistoryListHeader();
    });
  });
  btnLayout->addWidget(deleteBtn);

  itemLayout->addLayout(btnLayout);

  resultsLayout_->insertWidget(1, itemWidget);

  if (service_ && service_->settings() && service_->settings()->autoPruneHistory()) {
    const auto items = resultsContainer_->findChildren<QWidget *>(QStringLiteral("sf-result-item"));
    for (int i = 10; i < items.size(); ++i) {
      items.at(i)->deleteLater();
    }
  }
}

void SongFinderPage::showAboutDialog() {
  QMessageBox::about(this, I18n::text(QStringLiteral("pulse.about_title")),
                     I18n::text(QStringLiteral("pulse.about_html")));
}
