#include "download_toolbar_ui.h"

#include "core/browser_icons.h"
#include "download_desktop_actions.h"

#include <QApplication>
#include <QDesktopServices>
#include <QEnterEvent>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QProgressBar>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include "i18n/i18n.h"

namespace {
QString bytesLabel(qint64 bytes) {
  if (bytes < 0) return QStringLiteral("?");
  static const QStringList units{QStringLiteral("B"), QStringLiteral("KB"), QStringLiteral("MB"), QStringLiteral("GB"), QStringLiteral("TB")};
  double value = bytes;
  int unit = 0;
  while (value >= 1024.0 && unit + 1 < units.size()) { value /= 1024.0; ++unit; }
  return QStringLiteral("%1 %2").arg(QString::number(value, unit < 2 ? 'f' : 'f', unit < 2 ? 0 : 2), units.at(unit));
}

QString stateLabel(DownloadUiState state) {
  switch (state) {
    case DownloadUiState::Queued: return I18n::text(QStringLiteral("downloads.state_queued"));
    case DownloadUiState::Downloading: return I18n::text(QStringLiteral("downloads.state_downloading"));
    case DownloadUiState::Paused: return I18n::text(QStringLiteral("downloads.state_paused"));
    case DownloadUiState::Processing: return I18n::text(QStringLiteral("downloads.state_processing"));
    case DownloadUiState::Completed: return I18n::text(QStringLiteral("downloads.state_completed"));
    case DownloadUiState::Failed: return I18n::text(QStringLiteral("downloads.state_failed"));
    case DownloadUiState::Cancelled: return I18n::text(QStringLiteral("downloads.state_cancelled"));
  }
  return {};
}

QString progressLabel(const DownloadUiItem &item) {
  if (item.converting || item.state == DownloadUiState::Processing) {
    return !item.statusText.isEmpty() ? item.statusText : I18n::text(QStringLiteral("downloads.converting"));
  }
  QStringList parts;
  if (item.source == DownloadUiSource::Media
      && (item.state == DownloadUiState::Queued || item.state == DownloadUiState::Downloading))
    parts << QStringLiteral("%1%").arg(qRound(std::clamp(item.percent, 0.0, 100.0)));
  else if (item.totalBytes > 0 || item.percent > 0.0)
    parts << QStringLiteral("%1%").arg(qRound(std::clamp(item.percent, 0.0, 100.0)));
  if (item.source == DownloadUiSource::Media && item.state == DownloadUiState::Downloading
      && item.downloadedBytes <= 0)
    parts << I18n::text(QStringLiteral("media.waiting_for_stream"));
  if (item.totalBytes > 0) parts << QStringLiteral("%1 / %2").arg(bytesLabel(item.downloadedBytes), bytesLabel(item.totalBytes));
  else if (item.downloadedBytes > 0) parts << bytesLabel(item.downloadedBytes);
  if (item.bytesPerSecond > 0) parts << QStringLiteral("%1/s").arg(bytesLabel(item.bytesPerSecond));
  if (item.etaSeconds >= 0) parts << I18n::text(QStringLiteral("downloads.time_left")).arg(item.etaSeconds);
  return parts.isEmpty() ? stateLabel(item.state) : parts.join(QStringLiteral(" • "));
}
}  // namespace

DownloadToolbarButton::DownloadToolbarButton(QWidget *parent) : QToolButton(parent) {
  setIcon(BrowserIcons::icon(BrowserIcon::Download));
  setToolTip(I18n::text(QStringLiteral("newtab.downloads")));
}

void DownloadToolbarButton::setModelState(int activeCount, double progress, bool paused, bool error) {
  activeCount_ = std::max(0, activeCount);
  progress_ = std::clamp(progress, 0.0, 100.0);
  paused_ = paused;
  error_ = error;
  setToolTip(activeCount_ > 0 ? I18n::text(QStringLiteral("downloads.toolbar_active")).arg(activeCount_)
      : paused_ ? I18n::text(QStringLiteral("downloads.toolbar_paused")) : I18n::text(QStringLiteral("newtab.downloads")));
  update();
}

void DownloadToolbarButton::setPulse(qreal value) { pulse_ = value; update(); }

void DownloadToolbarButton::acknowledge(bool animationsEnabled) {
  if (pulseAnimation_) {
    pulseAnimation_->stop();
    pulseAnimation_->deleteLater();
    pulseAnimation_ = nullptr;
  }
  if (!animationsEnabled) { pulse_ = 0.0; update(); return; }
  const bool completionRing = activeCount_ == 0;
  if (completionRing) progress_ = 100.0;
  auto *animation = new QPropertyAnimation(this, "pulse", this);
  pulseAnimation_ = animation;
  animation->setDuration(280);
  animation->setStartValue(0.0);
  animation->setKeyValueAt(0.45, 1.0);
  animation->setEndValue(0.0);
  connect(animation, &QPropertyAnimation::finished, this, [this, animation, completionRing] {
    if (completionRing) progress_ = 0.0;
    animation->deleteLater();
    update();
  });
  animation->start();
}

void DownloadToolbarButton::paintEvent(QPaintEvent *event) {
  QToolButton::paintEvent(event);
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  const QRectF ring = QRectF(rect()).adjusted(2.5, 2.5, -2.5, -2.5);
  if (activeCount_ > 0 || paused_ || error_ || pulse_ > 0.0) {
    QPen base(QColor(85, 105, 122, 150), 2.2 + pulse_ * 1.2, Qt::SolidLine, Qt::RoundCap);
    painter.setPen(base);
    painter.drawArc(ring, 0, 360 * 16);
    QColor accent = error_ ? QColor(245, 110, 110) : paused_ ? QColor(240, 184, 88) : QColor(79, 195, 247);
    accent.setAlphaF(std::min(1.0, 0.82 + pulse_ * 0.18));
    painter.setPen(QPen(accent, 2.4 + pulse_ * 1.4, Qt::SolidLine, Qt::RoundCap));
    const double shownProgress = error_ && activeCount_ == 0 ? 100.0 : progress_;
    if (shownProgress > 0.0) {
      painter.drawArc(ring, 90 * 16, -qRound(360.0 * 16.0 * shownProgress / 100.0));
    } else if (activeCount_ > 0 && !paused_ && !error_) {
      painter.drawArc(ring, 90 * 16, -qRound(360.0 * 16.0 * 0.20));
    }
  }
  if (activeCount_ > 1) {
    const QString badge = activeCount_ > 9 ? QStringLiteral("9+") : QString::number(activeCount_);
    QRect badgeRect(width() - 15, 1, 14, 12);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(61, 142, 211));
    painter.drawRoundedRect(badgeRect, 6, 6);
    QFont font = painter.font(); font.setPixelSize(8); font.setBold(true); painter.setFont(font);
    painter.setPen(Qt::white);
    painter.drawText(badgeRect, Qt::AlignCenter, badge);
  }
}

DownloadPopup::DownloadPopup(DownloadUiModel *model, QWidget *parent)
    : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint), model_(model) {
  setObjectName(QStringLiteral("download-popup"));
  setAttribute(Qt::WA_DeleteOnClose, false);
  setFixedWidth(390);
  setMaximumHeight(520);
  setStyleSheet(QStringLiteral(
      "#download-popup{background:#18222d;border:1px solid #3a4a5b;border-radius:12px;}"
      "QLabel{color:#e8eef5;} QLabel[muted=\"true\"]{color:#91a0b0;font-size:11px;}"
      "QProgressBar{background:#2b3947;border:0;border-radius:3px;min-height:6px;max-height:6px;}"
      "QProgressBar::chunk{background:#4fc3f7;border-radius:3px;}"
      "QPushButton{color:#dfe8f0;background:#263545;border:1px solid #3b4e61;border-radius:7px;padding:5px 9px;}"
      "QPushButton:hover{background:#31465b;border-color:#5a7895;}"
      "QScrollArea{border:0;background:transparent;} QWidget#download-popup-content{background:transparent;}"));
  auto *root = new QVBoxLayout(this);
  root->setContentsMargins(12, 10, 12, 10);
  root->setSpacing(8);
  auto *header = new QHBoxLayout;
  auto *title = new QLabel(I18n::text(QStringLiteral("newtab.downloads")), this);
  QFont titleFont = title->font(); titleFont.setBold(true); titleFont.setPointSize(titleFont.pointSize() + 1); title->setFont(titleFont);
  auto *details = new QPushButton(I18n::text(QStringLiteral("downloads.open_all")), this);
  connect(details, &QPushButton::clicked, this, [this] { hide(); emit openDownloadsRequested(); });
  header->addWidget(title); header->addStretch(); header->addWidget(details);
  root->addLayout(header);

  suggestedMediaCard_ = new QFrame(this);
  suggestedMediaCard_->setObjectName(QStringLiteral("suggested-media-card"));
  suggestedMediaCard_->setStyleSheet(QStringLiteral(
      "#suggested-media-card{background:#1c2d3d;border:1px solid #2d4860;border-radius:8px;}"
      "QLabel{color:#e8eef5;} QPushButton{background:#1976d2;color:#ffffff;border:0;border-radius:6px;padding:4px 10px;font-weight:bold;}"
      "QPushButton:hover{background:#2196f3;}"));
  auto *suggestedLayout = new QHBoxLayout(suggestedMediaCard_);
  suggestedLayout->setContentsMargins(8, 6, 8, 6);
  suggestedLayout->setSpacing(8);
  auto *suggestedIcon = new QLabel(suggestedMediaCard_);
  suggestedIcon->setPixmap(BrowserIcons::icon(BrowserIcon::Video).pixmap(20, 20));
  auto *suggestedInfo = new QVBoxLayout;
  suggestedInfo->setSpacing(1);
  auto *suggestedHeader = new QLabel(I18n::text(QStringLiteral("downloads.page_media")), suggestedMediaCard_);
  QFont shFont = suggestedHeader->font(); shFont.setBold(true); shFont.setPointSize(std::max(8, shFont.pointSize() - 1));
  suggestedHeader->setFont(shFont);
  auto *suggestedDetail = new QLabel(suggestedMediaCard_);
  suggestedDetail->setObjectName(QStringLiteral("suggested-detail-label"));
  suggestedDetail->setProperty("muted", true);
  suggestedInfo->addWidget(suggestedHeader);
  suggestedInfo->addWidget(suggestedDetail);
  auto *suggestedBtn = new QPushButton(I18n::text(QStringLiteral("downloads.download_btn")), suggestedMediaCard_);
  connect(suggestedBtn, &QPushButton::clicked, this, [this] {
    const QUrl url = suggestedMediaUrl_;
    hide();
    emit openMediaDownloadRequested(url);
  });
  suggestedLayout->addWidget(suggestedIcon);
  suggestedLayout->addLayout(suggestedInfo, 1);
  suggestedLayout->addWidget(suggestedBtn);
  suggestedMediaCard_->hide();
  root->addWidget(suggestedMediaCard_);

  auto *scroll = new QScrollArea(this);
  scroll->setWidgetResizable(true);
  auto *content = new QWidget(scroll);
  content->setObjectName(QStringLiteral("download-popup-content"));
  itemsLayout_ = new QVBoxLayout(content);
  itemsLayout_->setContentsMargins(0, 0, 0, 0);
  itemsLayout_->setSpacing(7);
  emptyLabel_ = new QLabel(I18n::text(QStringLiteral("downloads.empty")), content);
  emptyLabel_->setProperty("muted", true);
  itemsLayout_->addWidget(emptyLabel_);
  itemsLayout_->addStretch();
  scroll->setWidget(content);
  root->addWidget(scroll);
  autoCloseTimer_ = new QTimer(this);
  autoCloseTimer_->setSingleShot(true);
  connect(autoCloseTimer_, &QTimer::timeout, this, &QWidget::hide);
  if (qApp) {
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
      if (!isVisible()) return;
      if (now && now != this && isAncestorOf(now)) autoCloseTimer_->stop();
      else if (transient_ && (!now || !isAncestorOf(now))) scheduleAutoClose();
    });
  }
  if (model_) connect(model_, &DownloadUiModel::changed, this, [this] { if (isVisible()) refresh(); });
}

void DownloadPopup::setSuggestedMedia(const QUrl &url, const QString &title) {
  suggestedMediaUrl_ = url;
  suggestedMediaTitle_ = title;
  if (suggestedMediaCard_) {
    const bool showCard = suggestedMediaUrl_.isValid() && !suggestedMediaUrl_.isEmpty();
    suggestedMediaCard_->setVisible(showCard);
    if (showCard) {
      if (auto *detail = suggestedMediaCard_->findChild<QLabel *>(QStringLiteral("suggested-detail-label"))) {
        const QString txt = !suggestedMediaTitle_.isEmpty() ? suggestedMediaTitle_ : suggestedMediaUrl_.host();
        detail->setText(QFontMetrics(detail->font()).elidedText(txt, Qt::ElideMiddle, 260));
      }
    }
  }
  if (emptyLabel_) {
    emptyLabel_->setVisible((model_ ? model_->items().isEmpty() : true) && (!suggestedMediaCard_ || !suggestedMediaCard_->isVisible()));
  }
}

void DownloadPopup::showAnchored(QWidget *anchor, bool transient) {
  anchor_ = anchor;
  transient_ = transient;
  if (suggestedMediaCard_) {
    const bool showCard = suggestedMediaUrl_.isValid() && !suggestedMediaUrl_.isEmpty();
    suggestedMediaCard_->setVisible(showCard);
    if (showCard) {
      if (auto *detail = suggestedMediaCard_->findChild<QLabel *>(QStringLiteral("suggested-detail-label"))) {
        const QString txt = !suggestedMediaTitle_.isEmpty() ? suggestedMediaTitle_ : suggestedMediaUrl_.host();
        detail->setText(QFontMetrics(detail->font()).elidedText(txt, Qt::ElideMiddle, 260));
      }
    }
  }
  refresh();
  adjustSize();
  reposition(anchor);
  show();
  raise();
  if (transient_) scheduleAutoClose(); else autoCloseTimer_->stop();
}

void DownloadPopup::reposition(QWidget *anchor) {
  if (!anchor || (!isVisible() && anchor_ != anchor)) return;
  const QPoint below = anchor->mapToGlobal(QPoint(anchor->width(), anchor->height() + 6));
  QScreen *screen = QGuiApplication::screenAt(below);
  if (!screen) screen = anchor->screen();
  if (!screen) screen = QGuiApplication::primaryScreen();
  const QRect available = screen ? screen->availableGeometry() : QRect(0, 0, 800, 600);
  const int x = std::clamp(below.x() - width(), available.left() + 6, available.right() - width() - 6);
  int y = below.y();
  if (y + height() > available.bottom() - 6) y = anchor->mapToGlobal(QPoint(0, -height() - 6)).y();
  move(x, std::max(available.top() + 6, y));
}

void DownloadPopup::enterEvent(QEnterEvent *event) { autoCloseTimer_->stop(); QFrame::enterEvent(event); }
void DownloadPopup::leaveEvent(QEvent *event) { if (transient_) scheduleAutoClose(); QFrame::leaveEvent(event); }
void DownloadPopup::scheduleAutoClose() { autoCloseTimer_->start(4200); }

void DownloadPopup::refresh() {
  const QVector<DownloadUiItem> all = model_ ? model_->items() : QVector<DownloadUiItem>{};
  const int limit = std::min(6, static_cast<int>(all.size()));
  QStringList structure;
  for (int index = 0; index < limit; ++index)
    structure << all.at(index).key + QLatin1Char(':') + QString::number(static_cast<int>(all.at(index).state));
  const QString nextSignature = structure.join(QLatin1Char('|'));
  if (nextSignature == structureSignature_) {
    const auto cards = findChildren<QFrame *>(QStringLiteral("download-popup-card"));
    for (QFrame *card : cards) {
      const QString key = card->property("downloadKey").toString();
      const auto found = std::find_if(all.cbegin(), all.cend(), [&key](const auto &item) { return item.key == key; });
      if (found == all.cend()) continue;
      if (auto *meta = card->findChild<QLabel *>(QStringLiteral("download-popup-meta"))) meta->setText(progressLabel(*found));
      if (auto *progress = card->findChild<QProgressBar *>(QStringLiteral("download-popup-progress"))) {
        if (found->converting || found->state == DownloadUiState::Processing) {
          progress->setRange(0, 0);
        } else if (found->source != DownloadUiSource::Media
                   && found->totalBytes <= 0 && found->percent <= 0.0) {
          progress->setRange(0, 0);
        } else {
          progress->setRange(0, 1000);
          progress->setValue(qRound(found->percent * 10.0));
        }
      }
    }
    return;
  }
  structureSignature_ = nextSignature;
  while (itemsLayout_->count() > 2) {
    QLayoutItem *layoutItem = itemsLayout_->takeAt(1);
    if (layoutItem->widget()) layoutItem->widget()->deleteLater();
    delete layoutItem;
  }
  emptyLabel_->setVisible(all.isEmpty() && (!suggestedMediaCard_ || !suggestedMediaCard_->isVisible()));
  for (int index = 0; index < limit; ++index) {
    const DownloadUiItem item = all.at(index);
    auto *card = new QFrame(this);
    card->setObjectName(QStringLiteral("download-popup-card"));
    card->setProperty("downloadKey", item.key);
    card->setStyleSheet(QStringLiteral("QFrame{background:#202d3a;border:1px solid #304253;border-radius:9px;} QLabel,QProgressBar,QPushButton{border:0;}"));
    auto *layout = new QVBoxLayout(card); layout->setContentsMargins(10, 8, 10, 8); layout->setSpacing(5);
    auto *name = new QLabel(item.title, card); name->setTextInteractionFlags(Qt::TextSelectableByMouse);
    name->setWordWrap(false); layout->addWidget(name);
    auto *source = new QLabel(item.subtitle, card); source->setProperty("muted", true); layout->addWidget(source);
    if (item.state == DownloadUiState::Downloading || item.state == DownloadUiState::Paused
        || item.state == DownloadUiState::Processing || item.state == DownloadUiState::Queued) {
      auto *progress = new QProgressBar(card); progress->setObjectName(QStringLiteral("download-popup-progress")); progress->setTextVisible(false);
      if (item.converting || item.state == DownloadUiState::Processing) progress->setRange(0, 0);
      else if (item.source != DownloadUiSource::Media
               && item.totalBytes <= 0 && item.percent <= 0.0) progress->setRange(0, 0);
      else { progress->setRange(0, 1000); progress->setValue(qRound(item.percent * 10.0)); }
      layout->addWidget(progress);
    }
    auto *meta = new QLabel(progressLabel(item), card); meta->setObjectName(QStringLiteral("download-popup-meta")); meta->setProperty("muted", true); layout->addWidget(meta);
    auto *actions = new QHBoxLayout; actions->setSpacing(5); actions->addStretch();
    if (item.state == DownloadUiState::Downloading || item.state == DownloadUiState::Queued) {
      if (item.source == DownloadUiSource::General) {
        auto *pause = new QPushButton(I18n::text(QStringLiteral("common.pause")), card);
        connect(pause, &QPushButton::clicked, model_, [model = model_, key = item.key] { model->pause(key); });
        actions->addWidget(pause);
      }
      auto *cancel = new QPushButton(I18n::text(QStringLiteral("common.cancel")), card);
      connect(cancel, &QPushButton::clicked, model_, [model = model_, key = item.key] { model->cancel(key); });
      actions->addWidget(cancel);
    } else if (item.state == DownloadUiState::Paused) {
      auto *resume = new QPushButton(I18n::text(QStringLiteral("common.resume")), card);
      connect(resume, &QPushButton::clicked, model_, [model = model_, key = item.key] { model->resume(key); });
      actions->addWidget(resume);
      auto *cancel = new QPushButton(I18n::text(QStringLiteral("common.cancel")), card);
      connect(cancel, &QPushButton::clicked, model_, [model = model_, key = item.key] { model->cancel(key); });
      actions->addWidget(cancel);
    } else if (item.state == DownloadUiState::Failed) {
      auto *retry = new QPushButton(I18n::text(QStringLiteral("common.retry")), card);
      connect(retry, &QPushButton::clicked, model_, [model = model_, key = item.key] { model->retry(key); });
      actions->addWidget(retry);
    } else if (item.state == DownloadUiState::Completed) {
      const QFileInfo file(item.localPath);
      if (file.isAbsolute() && file.exists() && file.isFile()) {
        auto *open = new QPushButton(I18n::text(QStringLiteral("common.open_file")), card);
        connect(open, &QPushButton::clicked, this, [this, item, path = file.absoluteFilePath()] {
          const LocalMediaPlayerKind kind = LocalMediaRouting::classify(path);
          if (kind != LocalMediaPlayerKind::External) {
            LocalMediaOpenRequest req;
            req.path = path;
            req.allowedRoot = QFileInfo(path).absolutePath();
            req.title = item.title;
            req.playerKind = kind;
            emit openLocalMediaRequested(req);
            hide();
          } else {
            DownloadDesktopActions::openFile(path);
          }
        });
        actions->addWidget(open);
        auto *reveal = new QPushButton(I18n::text(QStringLiteral("common.show_in_folder")), card);
        connect(reveal, &QPushButton::clicked, card, [path = file.absoluteFilePath()] {
          DownloadDesktopActions::revealInFolder(path, QFileInfo(path).absolutePath());
        });
        actions->addWidget(reveal);
      }
    }
    if (item.state == DownloadUiState::Completed || item.state == DownloadUiState::Failed
        || item.state == DownloadUiState::Cancelled) {
      auto *remove = new QPushButton(I18n::text(QStringLiteral("common.remove")), card);
      remove->setEnabled(item.source != DownloadUiSource::NativeFallback);
      connect(remove, &QPushButton::clicked, model_, [model = model_, key = item.key] { model->remove(key); });
      actions->addWidget(remove);
    }
    layout->addLayout(actions);
    itemsLayout_->insertWidget(itemsLayout_->count() - 1, card);
  }
}
