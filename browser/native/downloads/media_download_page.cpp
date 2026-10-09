#include "media_download_page.h"

#include "browser_icons.h"
#include "browser_profile_service.h"
#include "download_desktop_actions.h"
#include "general_download_manager.h"
#include "local_media_routing.h"
#include "security_utils.h"

#include <QAction>
#include <QAbstractButton>
#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QMenu>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProgressBar>
#include <QProcess>
#include <QPushButton>
#include <QPixmap>
#include <QResizeEvent>
#include <QSaveFile>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStyle>
#include <QStyleHints>
#include <QSet>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include "i18n/i18n.h"

namespace {

QWidget *card(QWidget *parent, const QString &title, QVBoxLayout **content = nullptr,
              QWidget *headerAction = nullptr) {
  auto *widget = new QFrame(parent);
  widget->setObjectName(QStringLiteral("media-download-card"));
  auto *layout = new QVBoxLayout(widget);
  layout->setContentsMargins(16, 14, 16, 16);
  layout->setSpacing(10);
  auto *headingRow = new QHBoxLayout;
  headingRow->setSpacing(8);
  auto *heading = new QLabel(title, widget);
  heading->setObjectName(QStringLiteral("media-download-heading"));
  headingRow->addWidget(heading);
  headingRow->addStretch();
  if (headerAction) headingRow->addWidget(headerAction);
  layout->addLayout(headingRow);
  if (content) *content = layout;
  return widget;
}

bool terminalState(MediaDownloadState state) {
  return state == MediaDownloadState::Completed || state == MediaDownloadState::Failed
      || state == MediaDownloadState::Cancelled;
}

QString kindText(MediaDownloadKind kind) {
  switch (kind) {
    case MediaDownloadKind::Video: return I18n::text(QStringLiteral("media.kind_video"));
    case MediaDownloadKind::AudioOriginal: return I18n::text(QStringLiteral("media.kind_audio_original"));
    case MediaDownloadKind::AudioConvert: return I18n::text(QStringLiteral("media.kind_audio_converted"));
    case MediaDownloadKind::PlaylistThumbnails: return I18n::text(QStringLiteral("media.kind_playlist_thumbnails"));
    case MediaDownloadKind::PlaylistLinks: return I18n::text(QStringLiteral("media.kind_playlist_links"));
  }
  return {};
}

QString stateProperty(MediaDownloadState state) {
  switch (state) {
    case MediaDownloadState::Queued: return QStringLiteral("queued");
    case MediaDownloadState::Downloading: return QStringLiteral("downloading");
    case MediaDownloadState::Processing: return QStringLiteral("processing");
    case MediaDownloadState::Completed: return QStringLiteral("completed");
    case MediaDownloadState::Failed: return QStringLiteral("failed");
    case MediaDownloadState::Cancelled: return QStringLiteral("cancelled");
  }
  return {};
}

QString friendlySource(QString source) {
  const QString lowered = source.toLower();
  if (lowered.contains(QStringLiteral("youtube"))) return QStringLiteral("YouTube");
  if (lowered.contains(QStringLiteral("tiktok"))) return QStringLiteral("TikTok");
  if (lowered.contains(QStringLiteral("facebook"))) return QStringLiteral("Facebook");
  if (lowered.contains(QStringLiteral("instagram"))) return QStringLiteral("Instagram");
  source.replace(QLatin1Char('_'), QLatin1Char(' '));
  return source.trimmed();
}

// Engine/service messages are catalog values; classify them by key so the
// mapping works in every UI language (and across a live language switch).
QString friendlyEngineStatus(const QString &message) {
  static const QSet<QString> preparing{
      QStringLiteral("media.engine_preparing"), QStringLiteral("media.checking_ytdlp_update"),
      QStringLiteral("media.downloading_ytdlp")};
  static const QSet<QString> verifying{
      QStringLiteral("media.verifying_integrity"), QStringLiteral("media.verifying_ytdlp"),
      QStringLiteral("media.verifying_js_runtime"), QStringLiteral("media.engine_verifying")};
  static const QSet<QString> ready{
      QStringLiteral("media.ytdlp_ready"), QStringLiteral("media.ytdlp_up_to_date"),
      QStringLiteral("media.engine_ready")};
  static const QSet<QString> updateFailed{
      QStringLiteral("media.update_failed_using_current"),
      QStringLiteral("media.ytdlp_update_failed_current"), QStringLiteral("media.engine_update_failed")};
  const QString key = dalinira::i18n::LanguageManager::instance().keyForText(message);
  if (preparing.contains(key)) return I18n::text(QStringLiteral("media.engine_preparing"));
  if (verifying.contains(key)) return I18n::text(QStringLiteral("media.engine_verifying"));
  if (ready.contains(key)) return I18n::text(QStringLiteral("media.engine_ready"));
  if (updateFailed.contains(key)) return I18n::text(QStringLiteral("media.engine_update_failed"));
  return message;
}

bool isMediaNotFoundMessage(const QString &message) {
  if (I18n::isTranslationOf(QStringLiteral("media.no_supported_media"), message)) return true;
  const QString lowered = message.toLower();
  return lowered.contains(QStringLiteral("unsupported url"));
}

bool isDirectGeneralFileUrl(const QUrl &url) {
  if (!url.isValid()) return false;
  const QString scheme = url.scheme().toLower();
  if (scheme != QLatin1String("http") && scheme != QLatin1String("https")) return false;

  static const QSet<QString> generalExtensions{
    QStringLiteral("iso"), QStringLiteral("img"), QStringLiteral("vhd"), QStringLiteral("dmg"),
    QStringLiteral("zip"), QStringLiteral("rar"), QStringLiteral("7z"), QStringLiteral("tar"),
    QStringLiteral("gz"), QStringLiteral("tgz"), QStringLiteral("bz2"), QStringLiteral("xz"),
    QStringLiteral("exe"), QStringLiteral("msi"), QStringLiteral("apk"), QStringLiteral("deb"),
    QStringLiteral("rpm"), QStringLiteral("appimage"), QStringLiteral("pkg"), QStringLiteral("bin"),
    QStringLiteral("pdf"), QStringLiteral("doc"), QStringLiteral("docx"), QStringLiteral("xls"),
    QStringLiteral("xlsx"), QStringLiteral("ppt"), QStringLiteral("pptx"), QStringLiteral("epub"),
    QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("gif"),
    QStringLiteral("webp"), QStringLiteral("svg"), QStringLiteral("ico"), QStringLiteral("bmp"),
    QStringLiteral("tiff")
  };

  const QString path = url.path().toLower();
  const int lastDot = path.lastIndexOf(QLatin1Char('.'));
  if (lastDot >= 0 && lastDot < path.length() - 1) {
    const QString ext = path.mid(lastDot + 1);
    if (generalExtensions.contains(ext)) return true;
  }

  const QString query = url.query().toLower();
  if (!query.isEmpty()) {
    for (const QString &ext : generalExtensions) {
      if (query.contains(QLatin1Char('.') + ext)) return true;
    }
  }

  return false;
}

QString friendlyFailure(const QString &message) {
  const QString lowered = message.toLower();
  if (lowered.contains(QStringLiteral("yt-dlp")) || lowered.contains(QStringLiteral("executable")))
    return I18n::text(QStringLiteral("media.engine_prepare_failed"));
  if (lowered.contains(QStringLiteral("unsupported url"))
      || I18n::isTranslationOf(QStringLiteral("media.no_supported_media"), message))
    return I18n::text(QStringLiteral("media.no_supported_media"));
  return message;
}

QString durationText(qint64 seconds) {
  if (seconds <= 0) return I18n::text(QStringLiteral("media.duration_unknown"));
  const qint64 hours = seconds / 3600;
  const qint64 minutes = (seconds % 3600) / 60;
  const qint64 remainingSeconds = seconds % 60;
  if (hours > 0)
    return QStringLiteral("%1:%2:%3").arg(hours).arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(remainingSeconds, 2, 10, QLatin1Char('0'));
  return QStringLiteral("%1:%2").arg(minutes).arg(remainingSeconds, 2, 10, QLatin1Char('0'));
}

QString chooseDirectorySafely(QWidget *parent, const QString &initialDir, const QString &title, const QString &acceptLabel = QString{}) {
  QString startDir = initialDir.trimmed();
  if (startDir.isEmpty() || !QDir(startDir).exists()) {
    startDir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
  }
  startDir = QDir::cleanPath(startDir);
  if (!startDir.endsWith(QLatin1Char('/'))) {
    startDir += QLatin1Char('/');
  }

  QFileDialog dialog(parent, title);
  dialog.setFileMode(QFileDialog::Directory);
  dialog.setOption(QFileDialog::ShowDirsOnly, true);
  dialog.setDirectory(startDir);
  dialog.selectFile(QString{});
  if (!acceptLabel.isEmpty()) {
    dialog.setLabelText(QFileDialog::Accept, acceptLabel);
  }

  if (dialog.exec() != QDialog::Accepted) {
    return {};
  }

  const QStringList selected = dialog.selectedFiles();
  if (!selected.isEmpty() && !selected.first().isEmpty()) {
    const QString candidate = QDir::cleanPath(selected.first());
    if (QFileInfo(candidate).isDir()) {
      return candidate;
    }
    const QString parentDir = QFileInfo(candidate).absolutePath();
    if (QFileInfo(parentDir).isDir()) {
      return parentDir;
    }
  }

  const QString currentDir = dialog.directory().absolutePath();
  if (QFileInfo(currentDir).isDir()) {
    return QDir::cleanPath(currentDir);
  }

  return {};
}

QPushButton *actionButton(BrowserIcon icon, const QString &text, const QString &tooltip,
                          QWidget *parent, const char *role = nullptr) {
  auto *button = new QPushButton(BrowserIcons::icon(icon), text, parent);
  button->setToolTip(tooltip);
  button->setAccessibleName(tooltip);
  button->setMinimumHeight(36);
  if (role) button->setProperty(role, true);
  return button;
}

QWidget *emptyState(const QIcon &icon, const QString &title, const QString &description, QWidget *parent) {
  auto *container = new QWidget(parent);
  container->setObjectName(QStringLiteral("media-empty-state"));
  auto *layout = new QHBoxLayout(container);
  layout->setContentsMargins(10, 8, 10, 8);
  layout->setSpacing(10);
  auto *iconLabel = new QLabel(container);
  iconLabel->setObjectName(QStringLiteral("media-empty-icon"));
  iconLabel->setPixmap(icon.pixmap(22, 22));
  iconLabel->setFixedSize(28, 28);
  iconLabel->setAlignment(Qt::AlignCenter);
  auto *titleLabel = new QLabel(title, container);
  titleLabel->setObjectName(QStringLiteral("media-empty-title"));
  auto *descriptionLabel = new QLabel(description, container);
  descriptionLabel->setObjectName(QStringLiteral("media-empty-description"));
  descriptionLabel->setWordWrap(true);
  auto *textLayout = new QVBoxLayout;
  textLayout->setContentsMargins(0, 0, 0, 0);
  textLayout->setSpacing(2);
  textLayout->addWidget(titleLabel);
  textLayout->addWidget(descriptionLabel);
  layout->addWidget(iconLabel);
  layout->addLayout(textLayout, 1);
  container->setAccessibleName(title + QStringLiteral(". ") + description);
  container->setMinimumHeight(52);
  return container;
}

}  // namespace

MediaDownloadPage::MediaDownloadPage(MediaDownloadService *service,
                                     BrowserProfileService *profileService,
                                     QWidget *parent)
    : QWidget(parent), service_(service), profileService_(profileService) {
  generalDownloads_ = profileService_ ? profileService_->downloadManager() : nullptr;
  setObjectName(QStringLiteral("media-download-page"));
  setAccessibleName(I18n::text(QStringLiteral("media.page_accessible")));
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
  setProperty("lightTheme", QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Light);
  connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this,
          [this](Qt::ColorScheme scheme) {
    setProperty("lightTheme", scheme == Qt::ColorScheme::Light);
    const QString sheet = styleSheet();
    setStyleSheet(QString{});
    setStyleSheet(sheet);
    update();
  });
#else
  setProperty("lightTheme", false);
#endif

  auto *outer = new QVBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  auto *scroll = new QScrollArea(this);
  scroll->setObjectName(QStringLiteral("media-download-scroll"));
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  auto *content = new QWidget(scroll);
  content->setObjectName(QStringLiteral("media-download-content"));
  content->setMinimumWidth(320);
  viewportLayout_ = new QHBoxLayout(content);
  viewportLayout_->setContentsMargins(18, 20, 18, 28);
  viewportLayout_->setSpacing(0);
  pageColumn_ = new QWidget(content);
  pageColumn_->setObjectName(QStringLiteral("media-download-column"));
  pageColumn_->setMaximumWidth(1160);
  pageColumn_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  auto *layout = new QVBoxLayout(pageColumn_);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(12);
  viewportLayout_->addWidget(pageColumn_);
  viewportLayout_->setAlignment(pageColumn_, Qt::AlignHCenter | Qt::AlignTop);

  auto *hero = new QWidget(pageColumn_);
  hero->setObjectName(QStringLiteral("media-download-hero"));
  auto *heroLayout = new QVBoxLayout(hero);
  heroLayout->setContentsMargins(2, 0, 2, 4);
  heroLayout->setSpacing(4);
  auto *heroTitle = new QLabel(I18n::text(QStringLiteral("newtab.downloads")), hero);
  heroTitle->setObjectName(QStringLiteral("media-download-title"));
  auto *heroDescription = new QLabel(
      I18n::text(QStringLiteral("media.page_subtitle")), hero);
  heroDescription->setObjectName(QStringLiteral("media-download-subtitle"));
  heroDescription->setWordWrap(true);
  heroLayout->addWidget(heroTitle);
  heroLayout->addWidget(heroDescription);
  layout->addWidget(hero);

  QVBoxLayout *sourceLayout = nullptr;
  QWidget *sourceCard = card(pageColumn_, I18n::text(QStringLiteral("media.from_link_card")), &sourceLayout);
  sourceCard->setObjectName(QStringLiteral("media-source-card"));
  auto *sourceHint = new QLabel(
      I18n::text(QStringLiteral("media.paste_link_hint")), sourceCard);
  sourceHint->setObjectName(QStringLiteral("media-source-hint"));
  sourceHint->setWordWrap(true);
  sourceLayout->addWidget(sourceHint);
  sourceGrid_ = new QGridLayout;
  sourceGrid_->setContentsMargins(0, 0, 0, 0);
  sourceGrid_->setHorizontalSpacing(8);
  sourceGrid_->setVerticalSpacing(8);
  urlInput_ = new QLineEdit(sourceCard);
  urlInput_->setObjectName(QStringLiteral("media-url-input"));
  urlInput_->setPlaceholderText(I18n::text(QStringLiteral("media.paste_link_placeholder")));
  urlInput_->setAccessibleName(I18n::text(QStringLiteral("media.download_link")));
  urlInput_->setAccessibleDescription(I18n::text(QStringLiteral("media.download_link_desc")));
  auto *paste = actionButton(BrowserIcon::Clipboard, I18n::text(QStringLiteral("bookmark.paste")),
                             I18n::text(QStringLiteral("media.paste_and_analyze")), sourceCard);
  paste->setObjectName(QStringLiteral("media-paste-button"));
  paste->setProperty("tertiary", true);
  analyzeButton_ = actionButton(BrowserIcon::Search, I18n::text(QStringLiteral("media.analyze")),
                                I18n::text(QStringLiteral("media.analyze_desc")), sourceCard, "primary");
  analyzeButton_->setObjectName(QStringLiteral("media-analyze-button"));
  cancelAnalysisButton_ = actionButton(BrowserIcon::Close, I18n::text(QStringLiteral("common.cancel")),
                                       I18n::text(QStringLiteral("media.cancel_analysis")), sourceCard, "danger");
  cancelAnalysisButton_->setObjectName(QStringLiteral("media-cancel-analysis-button"));
  cancelAnalysisButton_->hide();
  sourceGrid_->addWidget(urlInput_, 0, 0);
  sourceGrid_->addWidget(paste, 0, 1);
  sourceGrid_->addWidget(analyzeButton_, 0, 2);
  sourceGrid_->addWidget(cancelAnalysisButton_, 0, 3);
  sourceGrid_->setColumnStretch(0, 1);
  sourceLayout->addLayout(sourceGrid_);
  auto *statusRow = new QHBoxLayout;
  statusRow->setSpacing(10);
  analysisProgress_ = new QProgressBar(sourceCard);
  analysisProgress_->setObjectName(QStringLiteral("media-analysis-progress"));
  analysisProgress_->setTextVisible(false);
  analysisProgress_->setRange(0, 0);
  analysisProgress_->setFixedSize(120, 6);
  analysisProgress_->hide();
  statusLabel_ = new QLabel(sourceCard);
  statusLabel_->setObjectName(QStringLiteral("media-status-label"));
  statusLabel_->setWordWrap(true);
  statusLabel_->setText(service_ && service_->ytDlpAvailable()
      ? I18n::text(QStringLiteral("media.ready_to_download"))
      : I18n::text(QStringLiteral("media.engine_first_use")));
  statusRow->addWidget(analysisProgress_, 0, Qt::AlignVCenter);
  statusRow->addWidget(statusLabel_, 1);
  sourceLayout->addLayout(statusRow);
  layout->addWidget(sourceCard);

  QVBoxLayout *analysisLayout = nullptr;
  analysisCard_ = card(pageColumn_, I18n::text(QStringLiteral("media.options_card")), &analysisLayout);
  analysisCard_->setObjectName(QStringLiteral("media-analysis-card"));
  summaryGrid_ = new QGridLayout;
  summaryGrid_->setContentsMargins(0, 0, 0, 0);
  summaryGrid_->setHorizontalSpacing(14);
  summaryGrid_->setVerticalSpacing(9);
  thumbnailLabel_ = new QLabel(analysisCard_);
  thumbnailLabel_->setFixedSize(160, 90);
  thumbnailLabel_->setAlignment(Qt::AlignCenter);
  thumbnailLabel_->setPixmap(BrowserIcons::icon(BrowserIcon::Video).pixmap(40, 40));
  thumbnailLabel_->setAccessibleName(I18n::text(QStringLiteral("media.preview")));
  thumbnailLabel_->setObjectName(QStringLiteral("media-thumbnail"));
  summaryText_ = new QWidget(analysisCard_);
  auto *summaryText = new QVBoxLayout(summaryText_);
  summaryText->setContentsMargins(0, 1, 0, 0);
  summaryText->setSpacing(5);
  titleLabel_ = new QLabel(analysisCard_);
  titleLabel_->setObjectName(QStringLiteral("media-title"));
  titleLabel_->setWordWrap(true);
  titleLabel_->setText(I18n::text(QStringLiteral("media.analyzing")));
  detailsLabel_ = new QLabel(analysisCard_);
  detailsLabel_->setObjectName(QStringLiteral("media-details"));
  detailsLabel_->setWordWrap(true);
  detailsLabel_->setText(I18n::text(QStringLiteral("media.analyzing_desc")));
  summaryText->addWidget(titleLabel_);
  summaryText->addWidget(detailsLabel_);
  summaryText->addStretch();
  summaryGrid_->addWidget(thumbnailLabel_, 0, 0);
  summaryGrid_->addWidget(summaryText_, 0, 1);
  summaryGrid_->setColumnStretch(1, 1);
  analysisLayout->addLayout(summaryGrid_);

  analysisOptions_ = new QWidget(analysisCard_);
  analysisOptions_->setObjectName(QStringLiteral("media-analysis-options"));
  auto *optionsLayout = new QVBoxLayout(analysisOptions_);
  optionsLayout->setContentsMargins(0, 3, 0, 0);
  optionsLayout->setSpacing(10);
  auto *typeLabel = new QLabel(I18n::text(QStringLiteral("media.what_to_download")), analysisOptions_);
  typeLabel->setObjectName(QStringLiteral("media-option-label"));
  optionsLayout->addWidget(typeLabel);
  auto *typeRow = new QHBoxLayout;
  typeRow->setContentsMargins(0, 0, 0, 0);
  typeRow->setSpacing(8);
  videoModeButton_ = actionButton(BrowserIcon::Video, I18n::text(QStringLiteral("media.kind_video")),
                                  I18n::text(QStringLiteral("media.download_video_desc")), analysisOptions_);
  audioModeButton_ = actionButton(BrowserIcon::Music, I18n::text(QStringLiteral("settings.site.sound")),
                                  I18n::text(QStringLiteral("media.download_audio_only")), analysisOptions_);
  for (QPushButton *button : {videoModeButton_, audioModeButton_}) {
    button->setCheckable(true);
    button->setProperty("segment", true);
    button->setMinimumSize(112, 34);
    button->setMaximumWidth(150);
    typeRow->addWidget(button, 1);
  }
  videoModeButton_->setObjectName(QStringLiteral("media-video-mode"));
  audioModeButton_->setObjectName(QStringLiteral("media-audio-mode"));
  auto *typeGroup = new QButtonGroup(analysisOptions_);
  typeGroup->setExclusive(true);
  typeGroup->addButton(videoModeButton_);
  typeGroup->addButton(audioModeButton_);
  optionsLayout->addLayout(typeRow);

  modeBox_ = new QComboBox(analysisOptions_);
  modeBox_->setObjectName(QStringLiteral("media-internal-mode-model"));
  modeBox_->addItem(I18n::text(QStringLiteral("media.kind_video")), QStringLiteral("video"));
  modeBox_->addItem(I18n::text(QStringLiteral("media.kind_audio_original")), QStringLiteral("original"));
  for (const QString &format : {QStringLiteral("mp3"), QStringLiteral("m4a"), QStringLiteral("opus"),
                                QStringLiteral("wav"), QStringLiteral("flac"), QStringLiteral("alac")})
    modeBox_->addItem(I18n::text(QStringLiteral("media.audio_only_format")).arg(format.toUpper()), format);
  modeBox_->addItem(I18n::text(QStringLiteral("media.download_playlist_covers")), QStringLiteral("playlist-thumbnails"));
  modeBox_->addItem(I18n::text(QStringLiteral("media.save_playlist_links")), QStringLiteral("playlist-links"));
  modeBox_->hide();

  auto *audioFormatPanel = new QWidget(analysisOptions_);
  audioFormatPanel->setObjectName(QStringLiteral("audio-format-panel"));
  audioFormatChoicesLayout_ = new QGridLayout(audioFormatPanel);
  audioFormatChoicesLayout_->setContentsMargins(0, 0, 0, 0);
  audioFormatChoicesLayout_->setHorizontalSpacing(7);
  audioFormatChoicesLayout_->setVerticalSpacing(7);
  const QList<QPair<QString, QString>> primaryAudioFormats{
      {QStringLiteral("original"), I18n::text(QStringLiteral("media.original_fastest"))},
      {QStringLiteral("mp3"), I18n::text(QStringLiteral("media.format_mp3"))},
      {QStringLiteral("m4a"), I18n::text(QStringLiteral("media.format_m4a"))},
      {QStringLiteral("opus"), I18n::text(QStringLiteral("media.format_opus"))}};
  auto *audioFormatGroup = new QButtonGroup(audioFormatPanel);
  audioFormatGroup->setExclusive(true);
  int audioColumn = 0;
  for (const auto &[mode, label] : primaryAudioFormats) {
    auto *button = new QPushButton(label, audioFormatPanel);
    button->setObjectName(QStringLiteral("media-audio-format-%1").arg(mode));
    button->setProperty("audioMode", mode);
    button->setProperty("formatChip", true);
    button->setCheckable(true);
    button->setMinimumHeight(38);
    button->setAccessibleName(I18n::text(QStringLiteral("media.audio_format_option")).arg(label.section(QLatin1Char('\n'), 0, 0)));
    if (mode != QLatin1String("original") && !service_->ffmpegAvailable()) {
      button->setEnabled(false);
      button->setToolTip(I18n::text(QStringLiteral("media.converter_required")));
      button->setAccessibleDescription(I18n::text(QStringLiteral("media.converter_unavailable_disabled")));
    }
    audioFormatGroup->addButton(button);
    audioFormatChoicesLayout_->addWidget(button, 0, audioColumn++);
    connect(button, &QPushButton::clicked, this, [this, mode] { setMode(mode); });
  }
  moreAudioFormats_ = new QComboBox(audioFormatPanel);
  moreAudioFormats_->setObjectName(QStringLiteral("media-more-audio-formats"));
  moreAudioFormats_->setAccessibleName(I18n::text(QStringLiteral("media.more_audio_formats")));
  moreAudioFormats_->addItem(I18n::text(QStringLiteral("media.more_formats")), QString{});
  moreAudioFormats_->addItem(I18n::text(QStringLiteral("media.format_wav")), QStringLiteral("wav"));
  moreAudioFormats_->addItem(I18n::text(QStringLiteral("media.format_flac")), QStringLiteral("flac"));
  moreAudioFormats_->addItem(I18n::text(QStringLiteral("media.format_alac")), QStringLiteral("alac"));
  moreAudioFormats_->setEnabled(service_->ffmpegAvailable());
  if (!service_->ffmpegAvailable())
    moreAudioFormats_->setToolTip(I18n::text(QStringLiteral("media.converter_required_extra")));
  audioFormatChoicesLayout_->addWidget(moreAudioFormats_, 1, 0, 1, 4);
  audioFormatPanel->hide();
  optionsLayout->addWidget(audioFormatPanel);

  formatHeading_ = new QLabel(I18n::text(QStringLiteral("media.video_quality")), analysisOptions_);
  formatHeading_->setObjectName(QStringLiteral("media-option-label"));
  optionsLayout->addWidget(formatHeading_);
  formatChoices_ = new QWidget(analysisOptions_);
  formatChoices_->setObjectName(QStringLiteral("media-format-choices"));
  formatChoicesLayout_ = new QGridLayout(formatChoices_);
  formatChoicesLayout_->setContentsMargins(0, 0, 0, 0);
  formatChoicesLayout_->setHorizontalSpacing(8);
  formatChoicesLayout_->setVerticalSpacing(8);
  optionsLayout->addWidget(formatChoices_);

  advancedToggle_ = new QPushButton(I18n::text(QStringLiteral("media.advanced_options")), analysisOptions_);
  advancedToggle_->setObjectName(QStringLiteral("media-advanced-toggle"));
  advancedToggle_->setCheckable(true);
  advancedToggle_->setToolTip(I18n::text(QStringLiteral("media.advanced_options_desc")));
  advancedToggle_->setAccessibleName(I18n::text(QStringLiteral("media.show_advanced_options")));
  advancedToggle_->setProperty("tertiary", true);
  optionsLayout->addWidget(advancedToggle_, 0, Qt::AlignLeft);

  advancedOptions_ = new QWidget(analysisOptions_);
  advancedOptions_->setObjectName(QStringLiteral("media-advanced-options"));
  advancedLayout_ = new QGridLayout(advancedOptions_);
  advancedLayout_->setContentsMargins(12, 10, 12, 11);
  advancedLayout_->setHorizontalSpacing(9);
  advancedLayout_->setVerticalSpacing(8);
  detailsFormatLabel_ = new QLabel(I18n::text(QStringLiteral("media.codec_details")), advancedOptions_);
  formatBox_ = new QComboBox(advancedOptions_);
  formatBox_->setObjectName(QStringLiteral("media-format-details"));
  formatBox_->setAccessibleName(I18n::text(QStringLiteral("media.detailed_format")));
  advancedLayout_->addWidget(detailsFormatLabel_, 0, 0);
  advancedLayout_->addWidget(formatBox_, 0, 1, 1, 3);
  subtitlesBox_ = new QCheckBox(I18n::text(QStringLiteral("media.download_subtitles")), advancedOptions_);
  subtitlesBox_->setObjectName(QStringLiteral("media-subtitles-option"));
  subtitlesBox_->setAccessibleName(I18n::text(QStringLiteral("media.download_available_subtitles")));
  advancedLayout_->addWidget(subtitlesBox_, 1, 0, 1, 2);
  sectionLabel_ = new QLabel(I18n::text(QStringLiteral("media.time_range")), advancedOptions_);
  sectionStartBox_ = new QSpinBox(advancedOptions_);
  sectionStartBox_->setRange(0, 604800);
  sectionStartBox_->setSpecialValueText(I18n::text(QStringLiteral("media.from_start")));
  sectionStartBox_->setPrefix(I18n::text(QStringLiteral("media.start_prefix")));
  sectionStartBox_->setAccessibleName(I18n::text(QStringLiteral("media.section_start")));
  sectionEndBox_ = new QSpinBox(advancedOptions_);
  sectionEndBox_->setRange(0, 604800);
  sectionEndBox_->setSpecialValueText(I18n::text(QStringLiteral("media.to_end")));
  sectionEndBox_->setPrefix(I18n::text(QStringLiteral("media.end_prefix")));
  sectionEndBox_->setAccessibleName(I18n::text(QStringLiteral("media.section_end")));
  advancedLayout_->addWidget(sectionLabel_, 2, 0);
  advancedLayout_->addWidget(sectionStartBox_, 2, 1);
  advancedLayout_->addWidget(sectionEndBox_, 2, 2, 1, 2);
  playlistBox_ = new QCheckBox(I18n::text(QStringLiteral("media.download_whole_playlist")), advancedOptions_);
  playlistBox_->setObjectName(QStringLiteral("media-playlist-option"));
  playlistBox_->setToolTip(I18n::text(QStringLiteral("media.download_whole_playlist_desc")));
  playlistBox_->setAccessibleName(I18n::text(QStringLiteral("media.download_whole_playlist_accessible")));
  playlistStartBox_ = new QSpinBox(advancedOptions_);
  playlistStartBox_->setRange(1, 99999);
  playlistStartBox_->setValue(1);
  playlistStartBox_->setPrefix(I18n::text(QStringLiteral("media.first_item_prefix")));
  playlistEndBox_ = new QSpinBox(advancedOptions_);
  playlistEndBox_->setRange(0, 99999);
  playlistEndBox_->setSpecialValueText(I18n::text(QStringLiteral("media.last_item_all")));
  playlistEndBox_->setPrefix(I18n::text(QStringLiteral("media.last_item_prefix")));
  playlistStartBox_->hide();
  playlistEndBox_->hide();
  advancedLayout_->addWidget(playlistBox_, 3, 0);
  advancedLayout_->addWidget(playlistStartBox_, 3, 1);
  advancedLayout_->addWidget(playlistEndBox_, 3, 2, 1, 2);
  playlistActionLabel_ = new QLabel(I18n::text(QStringLiteral("media.playlist_tool")), advancedOptions_);
  playlistActionBox_ = new QComboBox(advancedOptions_);
  playlistActionBox_->setObjectName(QStringLiteral("media-playlist-action"));
  playlistActionBox_->setAccessibleName(I18n::text(QStringLiteral("media.playlist_action")));
  playlistActionBox_->addItem(I18n::text(QStringLiteral("media.download_media")), QString{});
  playlistActionBox_->addItem(I18n::text(QStringLiteral("media.download_cover_images")), QStringLiteral("playlist-thumbnails"));
  playlistActionBox_->addItem(I18n::text(QStringLiteral("media.save_links_to_text")), QStringLiteral("playlist-links"));
  advancedLayout_->addWidget(playlistActionLabel_, 4, 0);
  advancedLayout_->addWidget(playlistActionBox_, 4, 1, 1, 3);
  advancedOptions_->hide();
  optionsLayout->addWidget(advancedOptions_);

  auto *targetPanel = new QFrame(analysisOptions_);
  targetPanel->setObjectName(QStringLiteral("media-target-panel"));
  auto *targetLayout = new QHBoxLayout(targetPanel);
  targetLayout->setContentsMargins(11, 8, 10, 8);
  auto *targetIcon = new QLabel(targetPanel);
  targetIcon->setPixmap(BrowserIcons::icon(BrowserIcon::Folder).pixmap(24, 24));
  targetIcon->setAccessibleName(I18n::text(QStringLiteral("media.target_folder")));
  auto *targetText = new QVBoxLayout;
  targetText->setSpacing(1);
  targetNameLabel_ = new QLabel(targetPanel);
  targetNameLabel_->setObjectName(QStringLiteral("media-target-name"));
  targetPathLabel_ = new QLabel(targetPanel);
  targetPathLabel_->setObjectName(QStringLiteral("media-target-path"));
  targetPathLabel_->setTextInteractionFlags(Qt::TextSelectableByKeyboard | Qt::TextSelectableByMouse);
  targetPathLabel_->setMinimumWidth(0);
  targetPathLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  targetText->addWidget(targetNameLabel_);
  targetText->addWidget(targetPathLabel_);
  auto *changeTarget = new QPushButton(I18n::text(QStringLiteral("common.change")), targetPanel);
  changeTarget->setObjectName(QStringLiteral("media-change-target"));
  changeTarget->setToolTip(I18n::text(QStringLiteral("media.change_default_folder")));
  changeTarget->setAccessibleName(I18n::text(QStringLiteral("settings.downloads.change_folder")));
  changeTarget->setProperty("tertiary", true);
  targetLayout->addWidget(targetIcon);
  targetLayout->addLayout(targetText, 1);
  targetLayout->addWidget(changeTarget);
  optionsLayout->addWidget(targetPanel);

  auto *downloadRow = new QHBoxLayout;
  auto *selectionText = new QVBoxLayout;
  auto *selectionLabel = new QLabel(I18n::text(QStringLiteral("media.your_selection")), analysisOptions_);
  selectionLabel->setObjectName(QStringLiteral("media-selection-label"));
  selectionSummary_ = new QLabel(analysisOptions_);
  selectionSummary_->setObjectName(QStringLiteral("media-selection-summary"));
  selectionText->addWidget(selectionLabel);
  selectionText->addWidget(selectionSummary_);
  downloadButton_ = actionButton(BrowserIcon::Download, I18n::text(QStringLiteral("downloads.download_btn")),
                                 I18n::text(QStringLiteral("media.start_selected_download")), analysisOptions_, "primary");
  downloadButton_->setObjectName(QStringLiteral("media-primary-download"));
  downloadButton_->setMinimumSize(124, 40);
  downloadRow->addLayout(selectionText, 1);
  downloadRow->addWidget(downloadButton_);
  optionsLayout->addLayout(downloadRow);
  analysisLayout->addWidget(analysisOptions_);
  analysisCard_->hide();
  layout->addWidget(analysisCard_);

  QVBoxLayout *activeLayout = nullptr;
  QWidget *activeCard = card(pageColumn_, I18n::text(QStringLiteral("media.active_downloads_card")), &activeLayout);
  activeCard->setObjectName(QStringLiteral("media-active-downloads-card"));
  auto *activeContainer = new QWidget(activeCard);
  activeContainer->setObjectName(QStringLiteral("media-active-jobs"));
  activeJobsLayout_ = new QVBoxLayout(activeContainer);
  activeJobsLayout_->setContentsMargins(0, 0, 0, 0);
  activeJobsLayout_->setSpacing(9);
  activeLayout->addWidget(activeContainer);
  layout->addWidget(activeCard);

  exportButton_ = new QToolButton(pageColumn_);
  exportButton_->setObjectName(QStringLiteral("media-export-menu-button"));
  exportButton_->setIcon(BrowserIcons::icon(BrowserIcon::More));
  exportButton_->setText(I18n::text(QStringLiteral("common.more")));
  exportButton_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  exportButton_->setPopupMode(QToolButton::InstantPopup);
  exportButton_->setToolTip(I18n::text(QStringLiteral("media.history_actions")));
  exportButton_->setAccessibleName(I18n::text(QStringLiteral("media.history_more_actions")));
  auto *exportMenu = new QMenu(exportButton_);
  exportMenu->setObjectName(QStringLiteral("media-export-menu"));
  QAction *exportCsv = exportMenu->addAction(BrowserIcons::icon(BrowserIcon::Save), I18n::text(QStringLiteral("media.save_as_csv")));
  QAction *exportJson = exportMenu->addAction(BrowserIcons::icon(BrowserIcon::Save), I18n::text(QStringLiteral("media.save_as_json")));
  exportCsv->setToolTip(I18n::text(QStringLiteral("media.export_history_table")));
  exportJson->setToolTip(I18n::text(QStringLiteral("media.export_history_structured")));
  exportButton_->setMenu(exportMenu);
  QVBoxLayout *historyLayout = nullptr;
  QWidget *historyCard = card(pageColumn_, I18n::text(QStringLiteral("media.completed_history_card")), &historyLayout, exportButton_);
  historyCard->setObjectName(QStringLiteral("media-history-card"));
  auto *historyContainer = new QWidget(historyCard);
  historyContainer->setObjectName(QStringLiteral("media-history-jobs"));
  historyJobsLayout_ = new QVBoxLayout(historyContainer);
  historyJobsLayout_->setContentsMargins(0, 0, 0, 0);
  historyJobsLayout_->setSpacing(9);
  historyLayout->addWidget(historyContainer);
  layout->addWidget(historyCard);

  QVBoxLayout *browserLayout = nullptr;
  browserDownloadsCard_ = card(pageColumn_, I18n::text(QStringLiteral("media.file_downloads_card")), &browserLayout);
  browserDownloadsCard_->setObjectName(QStringLiteral("browser-downloads-card"));
  auto *browserHint = new QLabel(I18n::text(QStringLiteral("media.file_downloads_desc")), browserDownloadsCard_);
  browserHint->setObjectName(QStringLiteral("media-section-hint"));
  browserLayout->addWidget(browserHint);
  auto *browserContainer = new QWidget(browserDownloadsCard_);
  browserContainer->setObjectName(QStringLiteral("browser-downloads-list"));
  browserDownloadsLayout_ = new QVBoxLayout(browserContainer);
  browserDownloadsLayout_->setContentsMargins(0, 0, 0, 0);
  browserDownloadsLayout_->setSpacing(8);
  browserLayout->addWidget(browserContainer);
  layout->addWidget(browserDownloadsCard_);
  layout->addStretch();
  scroll->setWidget(content);
  outer->addWidget(scroll);

  setStyleSheet(QStringLiteral(R"CSS(
    #media-download-page, #media-download-content, #media-download-column,
    #media-download-scroll { background:#10151c; color:#edf2f7; border:0; }
    #media-download-title { color:#f4f7fb; font-size:24px; font-weight:750; }
    #media-download-subtitle, #media-source-hint, #media-section-hint, #media-details,
    #media-target-path, #media-status-label { color:#9eabba; font-size:13px; }
    QFrame#media-source-card, QFrame#media-analysis-card { background:#181f28; border:1px solid #283441; border-radius:12px; }
    QFrame#media-active-downloads-card, QFrame#media-history-card,
    QFrame#browser-downloads-card { background:#151c24; border:0; border-radius:12px; }
    #media-download-heading { color:#8ab4f8; font-size:11px; font-weight:750; letter-spacing:1px; }
    #media-title { color:#f2f6fa; font-size:17px; font-weight:720; }
    #media-thumbnail { background:#0f151c; border:0; border-radius:10px; color:#778697; }
    #media-option-label, #media-target-name, #media-selection-summary { color:#edf3f8; font-size:13px; font-weight:650; }
    #media-selection-label { color:#8290a0; font-size:11px; font-weight:650; }
    QLineEdit, QComboBox, QSpinBox { background:#10171f; color:#edf3f8; border:1px solid #344454; border-radius:9px; padding:6px 10px; min-height:22px; }
    QLineEdit:hover, QComboBox:hover, QSpinBox:hover { border-color:#465a6d; }
    QLineEdit:focus, QComboBox:focus, QSpinBox:focus { border-color:#70b4f4; }
    QComboBox QAbstractItemView { background:#1b232d; color:#edf3f8; selection-background-color:#30445a; }
    QPushButton, QToolButton { background:#242e39; color:#e9f0f7; border:1px solid #354352; border-radius:9px; padding:6px 11px; }
    QPushButton:hover, QToolButton:hover { background:#2d3a47; border-color:#4a5d70; }
    QPushButton:focus, QToolButton:focus { border:1px solid #70b4f4; }
    QPushButton:pressed, QToolButton:pressed { background:#1d2731; }
    QPushButton[primary="true"] { background:#2476c7; color:#ffffff; border-color:#2476c7; font-size:13px; font-weight:700; }
    QPushButton[primary="true"]:hover { background:#2e86d9; }
    QPushButton[primary="true"]:pressed { background:#1d65ad; }
    QPushButton[secondary="true"] { background:#263c52; border-color:#3c6489; }
    QPushButton[tertiary="true"] { background:transparent; border-color:#384655; }
    QPushButton[danger="true"] { background:#38262a; color:#ffb3ba; border-color:#624047; }
    QPushButton:disabled, QToolButton:disabled { color:#677481; background:#1a222b; border-color:#29333e; }
    QPushButton[segment="true"] { background:#111820; min-height:28px; font-size:13px; font-weight:650; }
    QPushButton[segment="true"]:checked { background:#23496c; color:#e7f3ff; border-color:#5da8ef; }
    QPushButton[formatChip="true"], QPushButton[qualityChip="true"] { background:#111820; text-align:center; min-width:0; min-height:26px; }
    QPushButton[formatChip="true"]:checked, QPushButton[qualityChip="true"]:checked { background:#23496c; color:#e7f3ff; border-color:#5da8ef; font-weight:650; }
    #media-advanced-options { background:#121922; border:0; border-radius:10px; }
    QFrame#media-target-panel { background:#121a23; border:0; border-radius:10px; }
    QFrame#media-job-card, QFrame#browser-download-row { background:#111820; border:0; border-radius:10px; }
    QFrame#general-download-card { background:#111820; border:0; border-radius:10px; }
    QFrame#media-job-card[state="completed"] { border-color:#285444; }
    QFrame#media-job-card[state="failed"] { border-color:#704048; }
    #media-job-title, #general-download-title, #browser-download-title { color:#edf3f8; font-size:14px; font-weight:680; }
    #media-job-meta, #general-download-meta, #media-job-progress-text,
    #general-download-progress-text, #browser-download-meta { color:#91a0b0; font-size:12px; }
    #media-job-status { color:#a9cfff; font-size:12px; font-weight:650; }
    #general-download-status { color:#a9cfff; font-size:12px; font-weight:650; }
    #media-job-error { color:#ffabb4; font-size:12px; }
    #media-empty-state { background:transparent; }
    #media-empty-title { color:#c9d4de; font-size:13px; font-weight:650; }
    #media-empty-description { color:#8795a4; font-size:12px; }
    QProgressBar#media-analysis-progress, QProgressBar#media-job-progress { background:#2b3947; border:0; border-radius:3px; min-height:6px; max-height:7px; }
    QProgressBar#media-analysis-progress::chunk { background:#68b8f5; border-radius:3px; }
    QProgressBar#media-job-progress::chunk { background:#4e9fe8; border-radius:3px; }
    QCheckBox { color:#dce5ee; spacing:8px; }
    QMenu { background:#1b232d; color:#edf3f8; border:1px solid #384756; border-radius:9px; padding:6px; }
    QMenu::item { min-height:26px; padding:5px 28px; border-radius:6px; }
    QMenu::item:selected { background:#2b3a48; }
    #media-download-page[lightTheme="true"], #media-download-page[lightTheme="true"] #media-download-content,
    #media-download-page[lightTheme="true"] #media-download-scroll,
    #media-download-page[lightTheme="true"] #media-download-column { background:#f4f7fa; color:#18222d; }
    #media-download-page[lightTheme="true"] #media-download-title,
    #media-download-page[lightTheme="true"] #media-title,
    #media-download-page[lightTheme="true"] #media-option-label,
    #media-download-page[lightTheme="true"] #media-target-name,
    #media-download-page[lightTheme="true"] #media-selection-summary { color:#17212c; }
    #media-download-page[lightTheme="true"] QFrame#media-source-card,
    #media-download-page[lightTheme="true"] QFrame#media-analysis-card { background:#ffffff; border-color:#d9e1e8; }
    #media-download-page[lightTheme="true"] QFrame#media-active-downloads-card,
    #media-download-page[lightTheme="true"] QFrame#media-history-card,
    #media-download-page[lightTheme="true"] QFrame#browser-downloads-card { background:#ffffff; border:0; }
    #media-download-page[lightTheme="true"] #media-download-subtitle,
    #media-download-page[lightTheme="true"] #media-source-hint,
    #media-download-page[lightTheme="true"] #media-section-hint,
    #media-download-page[lightTheme="true"] #media-details,
    #media-download-page[lightTheme="true"] #media-target-path,
    #media-download-page[lightTheme="true"] #media-status-label { color:#5d6b79; }
    #media-download-page[lightTheme="true"] #media-empty-title,
    #media-download-page[lightTheme="true"] #media-job-title,
    #media-download-page[lightTheme="true"] #general-download-title,
    #media-download-page[lightTheme="true"] #browser-download-title { color:#17212c; }
    #media-download-page[lightTheme="true"] #media-empty-description,
    #media-download-page[lightTheme="true"] #media-job-meta,
    #media-download-page[lightTheme="true"] #media-job-progress-text,
    #media-download-page[lightTheme="true"] #general-download-meta,
    #media-download-page[lightTheme="true"] #general-download-progress-text,
    #media-download-page[lightTheme="true"] #browser-download-meta { color:#5d6b79; }
    #media-download-page[lightTheme="true"] #media-download-heading { color:#1764a8; }
    #media-download-page[lightTheme="true"] #media-job-status { color:#205f95; }
    #media-download-page[lightTheme="true"] #general-download-status { color:#205f95; }
    #media-download-page[lightTheme="true"] #media-job-error { color:#a92f3a; }
    #media-download-page[lightTheme="true"] QLineEdit,
    #media-download-page[lightTheme="true"] QComboBox,
    #media-download-page[lightTheme="true"] QSpinBox,
    #media-download-page[lightTheme="true"] QPushButton[segment="true"],
    #media-download-page[lightTheme="true"] QPushButton[formatChip="true"],
    #media-download-page[lightTheme="true"] QPushButton[qualityChip="true"],
    #media-download-page[lightTheme="true"] QFrame#media-job-card,
    #media-download-page[lightTheme="true"] QFrame#general-download-card,
    #media-download-page[lightTheme="true"] QFrame#browser-download-row,
    #media-download-page[lightTheme="true"] QFrame#media-target-panel,
    #media-download-page[lightTheme="true"] #media-advanced-options { background:#f7f9fb; color:#17212c; border-color:#d5dee6; }
    #media-download-page[lightTheme="true"] QPushButton,
    #media-download-page[lightTheme="true"] QToolButton { background:#eef2f6; color:#17212c; border-color:#cbd5df; }
    #media-download-page[lightTheme="true"] QPushButton:hover,
    #media-download-page[lightTheme="true"] QToolButton:hover { background:#e4eaf0; border-color:#aebbc8; }
    #media-download-page[lightTheme="true"] QPushButton[primary="true"] { background:#176fbd; color:#ffffff; border-color:#176fbd; }
    #media-download-page[lightTheme="true"] QPushButton[primary="true"]:hover { background:#0e61aa; }
    #media-download-page[lightTheme="true"] QPushButton[secondary="true"] { background:#e4eef8; color:#153f66; border-color:#9dbbd6; }
    #media-download-page[lightTheme="true"] QPushButton[tertiary="true"] { background:transparent; color:#28445d; border-color:#c3ced8; }
    #media-download-page[lightTheme="true"] QPushButton[danger="true"] { background:#fff0f1; color:#9b2633; border-color:#e3aab0; }
    #media-download-page[lightTheme="true"] QPushButton[segment="true"]:checked,
    #media-download-page[lightTheme="true"] QPushButton[formatChip="true"]:checked,
    #media-download-page[lightTheme="true"] QPushButton[qualityChip="true"]:checked { background:#dceeff; color:#123f69; border-color:#3988ca; }
    #media-download-page[lightTheme="true"] QCheckBox { color:#263746; }
    #media-download-page[lightTheme="true"] QMenu { background:#ffffff; color:#17212c; border-color:#cbd5df; }
    #media-download-page[lightTheme="true"] QMenu::item:selected { background:#e4eef8; }
  )CSS"));

  thumbnailNetwork_ = new QNetworkAccessManager(this);
  thumbnailNetwork_->setCache(nullptr);
  connect(paste, &QPushButton::clicked, this, [this] {
    urlInput_->setText(QGuiApplication::clipboard()->text().trimmed());
    analyzeInput();
  });
  connect(analyzeButton_, &QPushButton::clicked, this, &MediaDownloadPage::analyzeInput);
  connect(urlInput_, &QLineEdit::returnPressed, this, &MediaDownloadPage::analyzeInput);
  connect(cancelAnalysisButton_, &QPushButton::clicked, service_, &MediaDownloadService::cancelAnalysis);
  connect(videoModeButton_, &QPushButton::clicked, this, [this] {
    playlistActionBox_->setCurrentIndex(0);
    setMode(QStringLiteral("video"));
  });
  connect(audioModeButton_, &QPushButton::clicked, this, [this] {
    playlistActionBox_->setCurrentIndex(0);
    const QString current = modeBox_->currentData().toString();
    setMode(current == QLatin1String("video") || current.startsWith(QStringLiteral("playlist-"))
        ? QStringLiteral("original") : current);
  });
  connect(moreAudioFormats_, qOverload<int>(&QComboBox::activated), this, [this](int index) {
    const QString mode = moreAudioFormats_->itemData(index).toString();
    if (!mode.isEmpty()) setMode(mode);
  });
  connect(formatBox_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
    if (!formatButtonGroup_) { updateSelectionSummary(); return; }
    const int formatIndex = formatBox_->currentData().toInt();
    if (QAbstractButton *exact = formatButtonGroup_->button(formatIndex)) exact->setChecked(true);
    else if (formatIndex >= 0) {
      const QVector<MediaFormatOption> &formats = modeBox_->currentData() == QLatin1String("video")
          ? analysis_.videoFormats : analysis_.audioFormats;
      if (formatIndex < formats.size()) {
        const int quality = formats.at(formatIndex).height;
        for (QAbstractButton *button : formatButtonGroup_->buttons())
          button->setChecked(button->property("qualityKey").toInt() == quality);
      }
    }
    updateSelectionSummary();
  });
  connect(advancedToggle_, &QPushButton::toggled, advancedOptions_, &QWidget::setVisible);
  connect(advancedToggle_, &QPushButton::toggled, this, [this](bool expanded) {
    advancedToggle_->setText(expanded ? I18n::text(QStringLiteral("media.hide_advanced_options"))
                                      : I18n::text(QStringLiteral("media.advanced_options")));
    advancedToggle_->setAccessibleName(expanded ? I18n::text(QStringLiteral("media.hide_advanced_download_options"))
                                                : I18n::text(QStringLiteral("media.show_advanced_options")));
  });
  connect(playlistBox_, &QCheckBox::toggled, playlistStartBox_, &QWidget::setVisible);
  connect(playlistBox_, &QCheckBox::toggled, playlistEndBox_, &QWidget::setVisible);
  connect(playlistActionBox_, qOverload<int>(&QComboBox::activated), this, [this](int index) {
    const QString specialMode = playlistActionBox_->itemData(index).toString();
    if (!specialMode.isEmpty()) { playlistBox_->setChecked(true); setMode(specialMode); }
    else setMode(audioModeButton_->isChecked() ? QStringLiteral("original") : QStringLiteral("video"));
  });
  connect(changeTarget, &QPushButton::clicked, this, [this] {
    QString current = profileService_ ? profileService_->configuredDownloadDirectory()
                                      : service_->defaultDownloadDirectory();
    if (current.isEmpty()) current = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    const QString selected = chooseDirectorySafely(this, current,
                                                  I18n::text(QStringLiteral("settings.downloads.choose_folder")),
                                                  I18n::text(QStringLiteral("settings.bookmarks.choose_folder_title")));
    if (selected.isEmpty()) return;
    if (profileService_) profileService_->setDownloadDirectory(selected);
    else service_->setDefaultDownloadDirectory(selected);
    refreshTargetDirectory();
  });
  connect(downloadButton_, &QPushButton::clicked, this, &MediaDownloadPage::startSelectedDownload);
  connect(exportCsv, &QAction::triggered, this, [this] { exportHistory(true); });
  connect(exportJson, &QAction::triggered, this, [this] { exportHistory(false); });
  connect(service_, &MediaDownloadService::analysisStarted, this, [this](const QUrl &) {
    setAnalysisLoading(true, I18n::text(QStringLiteral("media.analyzing")));
  });
  connect(service_, &MediaDownloadService::enginePreparationStatus, this,
          [this](const QString &message, int percent) {
    const QString friendlyMessage = friendlyEngineStatus(message);
    const QString display = percent > 0 && percent < 100
        ? QStringLiteral("%1 (%2%)").arg(friendlyMessage).arg(percent) : friendlyMessage;
    const bool preparing = service_->analysisRunning();
    statusLabel_->setText(display);
    analysisProgress_->setVisible(preparing);
    analyzeButton_->setEnabled(!preparing);
    cancelAnalysisButton_->setVisible(preparing);
    if (preparing) setAnalysisLoading(true, display);
  });
  connect(service_, &MediaDownloadService::analysisReady, this, [this](const MediaAnalysisResult &result) {
    setAnalysisLoading(false);
    applyAnalysis(result);
  });
  connect(service_, &MediaDownloadService::analysisFailed, this, [this](const QString &message) {
    setAnalysisLoading(false);
    if (analysis_.title.isEmpty()) analysisCard_->hide();

    const bool mediaNotFound = isMediaNotFoundMessage(message);
    const QUrl currentUrl = sourceUrl();

    if (mediaNotFound) {
      if (handleGeneralDownloadIfDirectFile(currentUrl)) {
        return;
      }
      if (hasActiveOrCompletedGeneralDownload(currentUrl)) {
        statusLabel_->setText(service_ && service_->ytDlpAvailable()
            ? I18n::text(QStringLiteral("media.ready_to_download"))
            : I18n::text(QStringLiteral("media.engine_ready")));
        return;
      }
      statusLabel_->setText(I18n::text(QStringLiteral("media.no_supported_media")));
    } else {
      statusLabel_->setText(friendlyFailure(message));
    }
  });
  connect(service_, &MediaDownloadService::analysisCancelled, this, [this] {
    setAnalysisLoading(false);
    statusLabel_->setText(I18n::text(QStringLiteral("media.analysis_cancelled")));
    if (analysis_.title.isEmpty()) analysisCard_->hide();
  });
  connect(service_, &MediaDownloadService::jobsChanged, this, &MediaDownloadPage::refreshJobs);
  if (generalDownloads_)
    connect(generalDownloads_, &GeneralDownloadManager::jobsChanged, this, &MediaDownloadPage::refreshJobs);
  if (profileService_)
    connect(profileService_, &BrowserProfileService::downloadsChanged, this, &MediaDownloadPage::refreshBrowserDownloads);
  refreshTargetDirectory();
  refreshJobs();
  refreshBrowserDownloads();
  updateResponsiveLayout();
}

void MediaDownloadPage::resizeEvent(QResizeEvent *event) {
  QWidget::resizeEvent(event);
  updateResponsiveLayout();
}

void MediaDownloadPage::updateResponsiveLayout() {
  if (!sourceGrid_ || !summaryGrid_ || !advancedLayout_) return;
  if (viewportLayout_) {
    const int horizontalMargin = std::max(14, (width() - 1160) / 2);
    viewportLayout_->setContentsMargins(horizontalMargin, 20, horizontalMargin, 28);
    if (pageColumn_)
      pageColumn_->setMinimumWidth(std::min(1160, std::max(0, width() - (horizontalMargin * 2))));
  }
  const bool narrow = width() < 640;
  const bool stackedSummary = width() < 560;
  auto *pasteButton = findChild<QPushButton *>(QStringLiteral("media-paste-button"));

  for (QWidget *widget : {static_cast<QWidget *>(urlInput_), static_cast<QWidget *>(pasteButton),
                          static_cast<QWidget *>(analyzeButton_),
                          static_cast<QWidget *>(cancelAnalysisButton_)})
    if (widget) sourceGrid_->removeWidget(widget);
  if (narrow) {
    sourceGrid_->addWidget(urlInput_, 0, 0, 1, 4);
    sourceGrid_->addWidget(pasteButton, 1, 0);
    sourceGrid_->addWidget(analyzeButton_, 1, 1);
    sourceGrid_->addWidget(cancelAnalysisButton_, 1, 2);
    sourceGrid_->setColumnStretch(0, 0);
    sourceGrid_->setColumnStretch(1, 0);
    sourceGrid_->setColumnStretch(2, 0);
    sourceGrid_->setColumnStretch(3, 1);
  } else {
    sourceGrid_->addWidget(urlInput_, 0, 0);
    sourceGrid_->addWidget(pasteButton, 0, 1);
    sourceGrid_->addWidget(analyzeButton_, 0, 2);
    sourceGrid_->addWidget(cancelAnalysisButton_, 0, 3);
    sourceGrid_->setColumnStretch(0, 1);
    sourceGrid_->setColumnStretch(1, 0);
    sourceGrid_->setColumnStretch(2, 0);
    sourceGrid_->setColumnStretch(3, 0);
  }

  summaryGrid_->removeWidget(thumbnailLabel_);
  summaryGrid_->removeWidget(summaryText_);
  if (stackedSummary) {
    summaryGrid_->addWidget(thumbnailLabel_, 0, 0, Qt::AlignLeft);
    summaryGrid_->addWidget(summaryText_, 1, 0);
    summaryGrid_->setColumnStretch(1, 0);
  } else {
    summaryGrid_->addWidget(thumbnailLabel_, 0, 0);
    summaryGrid_->addWidget(summaryText_, 0, 1);
    summaryGrid_->setColumnStretch(1, 1);
  }

  const QList<QWidget *> advancedWidgets{
      detailsFormatLabel_, formatBox_, subtitlesBox_, sectionLabel_, sectionStartBox_, sectionEndBox_,
      playlistBox_, playlistStartBox_, playlistEndBox_, playlistActionLabel_, playlistActionBox_};
  for (QWidget *widget : advancedWidgets) advancedLayout_->removeWidget(widget);
  if (narrow) {
    advancedLayout_->addWidget(detailsFormatLabel_, 0, 0, 1, 2);
    advancedLayout_->addWidget(formatBox_, 1, 0, 1, 2);
    advancedLayout_->addWidget(subtitlesBox_, 2, 0, 1, 2);
    advancedLayout_->addWidget(sectionLabel_, 3, 0, 1, 2);
    advancedLayout_->addWidget(sectionStartBox_, 4, 0);
    advancedLayout_->addWidget(sectionEndBox_, 4, 1);
    advancedLayout_->addWidget(playlistBox_, 5, 0, 1, 2);
    advancedLayout_->addWidget(playlistStartBox_, 6, 0);
    advancedLayout_->addWidget(playlistEndBox_, 6, 1);
    advancedLayout_->addWidget(playlistActionLabel_, 7, 0, 1, 2);
    advancedLayout_->addWidget(playlistActionBox_, 8, 0, 1, 2);
  } else {
    advancedLayout_->addWidget(detailsFormatLabel_, 0, 0);
    advancedLayout_->addWidget(formatBox_, 0, 1, 1, 3);
    advancedLayout_->addWidget(subtitlesBox_, 1, 0, 1, 2);
    advancedLayout_->addWidget(sectionLabel_, 2, 0);
    advancedLayout_->addWidget(sectionStartBox_, 2, 1);
    advancedLayout_->addWidget(sectionEndBox_, 2, 2, 1, 2);
    advancedLayout_->addWidget(playlistBox_, 3, 0);
    advancedLayout_->addWidget(playlistStartBox_, 3, 1);
    advancedLayout_->addWidget(playlistEndBox_, 3, 2, 1, 2);
    advancedLayout_->addWidget(playlistActionLabel_, 4, 0);
    advancedLayout_->addWidget(playlistActionBox_, 4, 1, 1, 3);
  }

  reflowChoiceGrids(width() < 560 ? 2 : width() < 900 ? 3 : 6);
}

void MediaDownloadPage::reflowChoiceGrids(int columns) {
  if (formatButtonGroup_ && formatChoicesLayout_) {
    QList<QAbstractButton *> buttons = formatButtonGroup_->buttons();
    std::sort(buttons.begin(), buttons.end(), [this](QAbstractButton *left, QAbstractButton *right) {
      return formatButtonGroup_->id(left) < formatButtonGroup_->id(right);
    });
    for (QAbstractButton *button : buttons) formatChoicesLayout_->removeWidget(button);
    for (int index = 0; index < buttons.size(); ++index)
      formatChoicesLayout_->addWidget(buttons.at(index), index / columns, index % columns);
  }

  if (!audioFormatChoicesLayout_) return;
  const QStringList modes{QStringLiteral("original"), QStringLiteral("mp3"),
                          QStringLiteral("m4a"), QStringLiteral("opus")};
  const int audioColumns = columns >= 3 ? 4 : 2;
  QList<QPushButton *> audioButtons;
  for (const QString &mode : modes)
    if (auto *button = findChild<QPushButton *>(QStringLiteral("media-audio-format-%1").arg(mode)))
      audioButtons.append(button);
  for (QPushButton *button : audioButtons) audioFormatChoicesLayout_->removeWidget(button);
  audioFormatChoicesLayout_->removeWidget(moreAudioFormats_);
  for (int index = 0; index < audioButtons.size(); ++index)
    audioFormatChoicesLayout_->addWidget(audioButtons.at(index), index / audioColumns, index % audioColumns);
  const int comboRow = (audioButtons.size() + audioColumns - 1) / audioColumns;
  audioFormatChoicesLayout_->addWidget(moreAudioFormats_, comboRow, 0, 1, audioColumns);
}

void MediaDownloadPage::setSourceUrl(const QUrl &url, bool analyzeImmediately) {
  urlInput_->setText(url.toString(QUrl::FullyEncoded));
  if (analyzeImmediately) analyzeInput();
}

QUrl MediaDownloadPage::sourceUrl() const {
  return QUrl::fromUserInput(urlInput_ ? urlInput_->text().trimmed() : QString{});
}

void MediaDownloadPage::analyzeInput() {
  const QUrl url = QUrl::fromUserInput(urlInput_->text().trimmed());
  QString reason;
  if (!MediaDownloadService::isSupportedMediaUrl(url, &reason)) {
    statusLabel_->setText(reason);
    urlInput_->setFocus();
    return;
  }
  statusLabel_->setText(I18n::text(QStringLiteral("media.analysis_starting")));
  const bool adultProtectionEnabled = profileService_
      && profileService_->isAdultContentProtectionEnabled();
  if (!service_->analyze(url, adultProtectionEnabled) && service_->analysisRunning())
    statusLabel_->setText(I18n::text(QStringLiteral("media.analysis_already_running")));
}

void MediaDownloadPage::setAnalysisLoading(bool loading, const QString &message) {
  analyzeButton_->setEnabled(!loading);
  cancelAnalysisButton_->setVisible(loading);
  analysisProgress_->setVisible(loading);
  if (loading) {
    analysisCard_->show();
    analysisOptions_->hide();
    titleLabel_->setText(message.isEmpty() ? I18n::text(QStringLiteral("media.analyzing")) : message);
    detailsLabel_->setText(I18n::text(QStringLiteral("media.analyzing_desc")));
    thumbnailLabel_->setPixmap(BrowserIcons::icon(BrowserIcon::Video).pixmap(40, 40));
    statusLabel_->setText(message.isEmpty() ? I18n::text(QStringLiteral("media.analyzing")) : message);
  }
}

void MediaDownloadPage::applyAnalysis(const MediaAnalysisResult &result) {
  analysis_ = result;
  titleLabel_->setText(result.title);
  titleLabel_->setToolTip(result.title);
  const QString duration = durationText(result.durationSeconds);
  detailsLabel_->setText(I18n::text(QStringLiteral("media.analysis_summary"))
      .arg(friendlySource(result.source), duration)
      .arg(result.videoFormats.size()).arg(result.audioFormats.size()));
  statusLabel_->setText(I18n::text(QStringLiteral("media.analysis_complete")));
  analysisOptions_->show();
  analysisCard_->show();
  setMode(result.videoFormats.isEmpty() ? QStringLiteral("original") : QStringLiteral("video"));
  loadThumbnail(QUrl(result.thumbnailUrl));
}

void MediaDownloadPage::setMode(const QString &mode) {
  const int index = modeBox_->findData(mode);
  if (index < 0) return;
  modeBox_->setCurrentIndex(index);
  const bool auxiliary = mode.startsWith(QStringLiteral("playlist-"));
  const bool audio = mode != QLatin1String("video") && !auxiliary;
  if (!auxiliary) { videoModeButton_->setChecked(!audio); audioModeButton_->setChecked(audio); }
  bool primaryAudioMode = false;
  for (QPushButton *button : findChildren<QPushButton *>()) {
    const QString buttonMode = button->property("audioMode").toString();
    if (buttonMode.isEmpty()) continue;
    const bool checked = buttonMode == mode;
    button->setChecked(checked);
    primaryAudioMode = primaryAudioMode || checked;
  }
  const QSignalBlocker blocker(moreAudioFormats_);
  moreAudioFormats_->setCurrentIndex(audio && !primaryAudioMode
      ? std::max(0, moreAudioFormats_->findData(mode)) : 0);
  refreshFormatChoices();
}

void MediaDownloadPage::refreshFormatChoices() {
  clearLayout(formatChoicesLayout_);
  delete formatButtonGroup_;
  formatButtonGroup_ = new QButtonGroup(this);
  formatButtonGroup_->setExclusive(true);
  const QString mode = modeBox_->currentData().toString();
  const bool auxiliaryPlaylist = mode.startsWith(QStringLiteral("playlist-"));
  const bool video = mode == QLatin1String("video");
  const bool audio = !video && !auxiliaryPlaylist;
  if (QWidget *panel = findChild<QWidget *>(QStringLiteral("audio-format-panel"))) panel->setVisible(audio);
  playlistBox_->setEnabled(!auxiliaryPlaylist);
  if (auxiliaryPlaylist) playlistBox_->setChecked(true);
  subtitlesBox_->setVisible(video);
  sectionStartBox_->setEnabled(!auxiliaryPlaylist);
  sectionEndBox_->setEnabled(!auxiliaryPlaylist);
  formatHeading_->setText(auxiliaryPlaylist ? I18n::text(QStringLiteral("media.playlist_operation"))
      : video ? I18n::text(QStringLiteral("media.video_quality")) : I18n::text(QStringLiteral("media.source_audio_quality")));
  const QSignalBlocker blocker(formatBox_);
  formatBox_->clear();
  const QVector<MediaFormatOption> &formats = video ? analysis_.videoFormats : analysis_.audioFormats;
  const bool conversionUnavailable = audio && mode != QLatin1String("original") && !service_->ffmpegAvailable();
  const auto formatUsable = [this, video, conversionUnavailable](const MediaFormatOption &format) {
    if (conversionUnavailable) return false;
    return !video || format.hasAudio || service_->ffmpegAvailable();
  };
  for (int index = 0; index < formats.size(); ++index)
    if (formatUsable(formats.at(index))) formatBox_->addItem(formats.at(index).label, index);
  if (auxiliaryPlaylist) {
    auto *description = new QLabel(mode == QLatin1String("playlist-thumbnails")
        ? I18n::text(QStringLiteral("media.playlist_covers_notice"))
        : I18n::text(QStringLiteral("media.playlist_links_notice")), formatChoices_);
    description->setObjectName(QStringLiteral("media-section-hint"));
    description->setWordWrap(true);
    formatChoicesLayout_->addWidget(description, 0, 0, 1, 3);
    description->show();
    formatBox_->setEnabled(false);
    downloadButton_->setEnabled(true);
    updateSelectionSummary();
    return;
  }
  QSet<int> seenQuality;
  int visibleIndex = 0;
  for (int index = 0; index < formats.size(); ++index) {
    const MediaFormatOption &format = formats.at(index);
    if (!formatUsable(format)) continue;
    const int qualityKey = video ? format.height : index;
    if (video && seenQuality.contains(qualityKey)) continue;
    seenQuality.insert(qualityKey);
    QString title;
    QString subtitle;
    if (video) {
      title = format.height > 0 ? QStringLiteral("%1p").arg(format.height) : I18n::text(QStringLiteral("media.kind_video"));
      if (format.height >= 1080) subtitle = QStringLiteral("Full HD");
      else if (format.height >= 720) subtitle = QStringLiteral("HD");
      else subtitle = format.extension.toUpper();
    } else {
      title = format.audioBitrate > 0 ? QStringLiteral("%1 kbps").arg(format.audioBitrate)
                                     : I18n::text(QStringLiteral("media.best_source"));
      subtitle = format.extension.toUpper();
    }
    auto *button = new QPushButton(title + QStringLiteral(" · ") + subtitle, formatChoices_);
    button->setObjectName(QStringLiteral("media-format-choice-%1").arg(index));
    button->setProperty("qualityChip", true);
    button->setProperty("qualityKey", qualityKey);
    button->setCheckable(true);
    button->setMinimumHeight(36);
    button->setToolTip(format.label);
    button->setAccessibleName(I18n::text(QStringLiteral("media.quality_option")).arg(title));
    button->setAccessibleDescription(format.label);
    formatButtonGroup_->addButton(button, index);
    formatChoicesLayout_->addWidget(button, visibleIndex / 6, visibleIndex % 6);
    button->show();
    if (visibleIndex == 0) button->setChecked(true);
    ++visibleIndex;
  }
  connect(formatButtonGroup_, &QButtonGroup::idClicked, this, [this](int formatIndex) {
    const int comboIndex = formatBox_->findData(formatIndex);
    if (comboIndex >= 0) formatBox_->setCurrentIndex(comboIndex);
    updateSelectionSummary();
  });
  formatBox_->setEnabled(visibleIndex > 0);
  downloadButton_->setEnabled(visibleIndex > 0);
  if (visibleIndex > 0) formatBox_->setCurrentIndex(0);
  if (visibleIndex == 0) {
    auto *missing = new QLabel(conversionUnavailable
        ? I18n::text(QStringLiteral("media.audio_converter_unavailable"))
        : I18n::text(QStringLiteral("media.no_formats")), formatChoices_);
    missing->setObjectName(QStringLiteral("media-section-hint"));
    formatChoicesLayout_->addWidget(missing, 0, 0, 1, 3);
    missing->show();
  }
  reflowChoiceGrids(width() < 560 ? 2 : width() < 900 ? 3 : 6);
  updateSelectionSummary();
}

void MediaDownloadPage::updateSelectionSummary() {
  const QString mode = modeBox_->currentData().toString();
  if (mode == QLatin1String("playlist-thumbnails")) { selectionSummary_->setText(I18n::text(QStringLiteral("media.kind_playlist_thumbnails"))); return; }
  if (mode == QLatin1String("playlist-links")) { selectionSummary_->setText(I18n::text(QStringLiteral("media.playlist_links_txt"))); return; }
  const bool video = mode == QLatin1String("video");
  const QVector<MediaFormatOption> &formats = video ? analysis_.videoFormats : analysis_.audioFormats;
  const int selected = formatBox_->currentIndex() >= 0 ? formatBox_->currentData().toInt() : -1;
  if (selected < 0 || selected >= formats.size()) { selectionSummary_->setText(I18n::text(QStringLiteral("media.choose_format"))); return; }
  const MediaFormatOption &format = formats.at(selected);
  if (video) selectionSummary_->setText(QStringLiteral("%1 • %2")
      .arg(format.height > 0 ? QStringLiteral("%1p").arg(format.height) : I18n::text(QStringLiteral("media.kind_video")), format.extension.toUpper()));
  else if (mode == QLatin1String("original"))
    selectionSummary_->setText(I18n::text(QStringLiteral("media.original_no_reencode")).arg(format.extension.toUpper()));
  else selectionSummary_->setText(I18n::text(QStringLiteral("media.best_quality")).arg(mode.toUpper()));
}

void MediaDownloadPage::refreshTargetDirectory() {
  QString directory = profileService_ ? profileService_->configuredDownloadDirectory() : service_->defaultDownloadDirectory();
  if (directory.isEmpty()) directory = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
  const bool asks = profileService_ && profileService_->asksDownloadLocation();
  const QString folderName = QFileInfo(directory).fileName().isEmpty() ? I18n::text(QStringLiteral("menu.downloads")) : QFileInfo(directory).fileName();
  targetNameLabel_->setText(asks ? I18n::text(QStringLiteral("media.ask_location_each")) : folderName);
  targetPathLabel_->setText(directory);
  targetPathLabel_->setToolTip(directory);
  targetPathLabel_->setAccessibleName(I18n::text(QStringLiteral("media.target_folder_value")).arg(directory));
}

void MediaDownloadPage::startSelectedDownload() {
  const QString mode = modeBox_->currentData().toString();
  const bool auxiliaryPlaylist = mode.startsWith(QStringLiteral("playlist-"));
  const QVector<MediaFormatOption> &formats = mode == QLatin1String("video") ? analysis_.videoFormats : analysis_.audioFormats;
  const int index = formatBox_->currentIndex() >= 0 ? formatBox_->currentData().toInt() : -1;
  if (!auxiliaryPlaylist && (index < 0 || index >= formats.size())) return;
  if (!auxiliaryPlaylist && sectionEndBox_->value() > 0 && sectionEndBox_->value() <= sectionStartBox_->value()) {
    statusLabel_->setText(I18n::text(QStringLiteral("media.section_end_after_start")));
    return;
  }
  QString directory = profileService_ ? profileService_->configuredDownloadDirectory() : service_->defaultDownloadDirectory();
  if (directory.isEmpty()) directory = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
  if (profileService_ && profileService_->asksDownloadLocation()) {
    directory = chooseDirectorySafely(this, directory,
                                      I18n::text(QStringLiteral("media.choose_media_folder")),
                                      I18n::text(QStringLiteral("downloads.download_btn")));
    if (directory.isEmpty()) return;
  }
  MediaDownloadRequest request;
  request.url = analysis_.url;
  request.adultProtectionContextUrl = analysis_.adultProtectionContextUrl;
  request.adultProtectionUrls = analysis_.adultProtectionUrls;
  request.adultProtectionUrlsComplete = analysis_.adultProtectionUrlsComplete;
  request.adultContentProtectionEnabled = profileService_
      && profileService_->isAdultContentProtectionEnabled();
  request.title = analysis_.title;
  request.source = analysis_.source;
  request.thumbnailUrl = analysis_.thumbnailUrl;
  request.targetDirectory = directory;
  if (!auxiliaryPlaylist) {
    const MediaFormatOption &format = formats.at(index);
    request.formatId = format.id;
    request.formatExtension = format.extension;
    request.formatHeight = format.height;
    request.formatHasAudio = format.hasAudio;
    request.estimatedBytes = format.estimatedBytes;
  }
  request.subtitles = subtitlesBox_->isChecked();
  request.sectionStartSeconds = sectionStartBox_->value();
  request.sectionEndSeconds = sectionEndBox_->value();
  request.playlist = playlistBox_->isChecked();
  request.playlistStart = playlistStartBox_->value();
  request.playlistEnd = playlistEndBox_->value();
  if (mode == QLatin1String("playlist-thumbnails")) request.kind = MediaDownloadKind::PlaylistThumbnails;
  else if (mode == QLatin1String("playlist-links")) request.kind = MediaDownloadKind::PlaylistLinks;
  else if (mode == QLatin1String("video")) request.kind = MediaDownloadKind::Video;
  else if (mode == QLatin1String("original")) request.kind = MediaDownloadKind::AudioOriginal;
  else { request.kind = MediaDownloadKind::AudioConvert; request.audioFormat = mode; }
  QString error;
  const QUuid id = service_->enqueue(request, &error);
  if (id.isNull()) {
    statusLabel_->setText(error.isEmpty()
        ? I18n::text(QStringLiteral("media.download_start_failed"))
        : error);
    return;
  }
  statusLabel_->setText(I18n::text(QStringLiteral("media.download_queued")));
}

QWidget *MediaDownloadPage::createJobCard(const MediaDownloadJob &job, QWidget *parent) {
  auto *jobCard = new QFrame(parent);
  jobCard->setObjectName(QStringLiteral("media-job-card"));
  jobCard->setProperty("state", stateProperty(job.state));
  jobCard->setProperty("jobId", job.id.toString(QUuid::WithoutBraces));
  jobCard->setAccessibleName(QStringLiteral("%1, %2").arg(job.title, job.statusText));
  auto *layout = new QVBoxLayout(jobCard);
  layout->setContentsMargins(14, 13, 14, 13);
  layout->setSpacing(9);
  auto *header = new QHBoxLayout;
  auto *icon = new QLabel(jobCard);
  const BrowserIcon mediaIcon = job.kind == MediaDownloadKind::Video ? BrowserIcon::Video
      : (job.kind == MediaDownloadKind::PlaylistLinks || job.kind == MediaDownloadKind::PlaylistThumbnails)
          ? BrowserIcon::Cards : BrowserIcon::Music;
  icon->setPixmap(BrowserIcons::icon(mediaIcon).pixmap(30, 30));
  icon->setFixedSize(38, 38);
  icon->setAlignment(Qt::AlignCenter);
  auto *text = new QVBoxLayout;
  text->setSpacing(2);
  auto *title = new QLabel(job.title, jobCard);
  title->setObjectName(QStringLiteral("media-job-title"));
  title->setWordWrap(true);
  title->setMaximumHeight(44);
  title->setToolTip(job.title);
  auto *meta = new QLabel((job.playlist ? I18n::text(QStringLiteral("media.playlist_kind")).arg(kindText(job.kind)) : kindText(job.kind)), jobCard);
  meta->setObjectName(QStringLiteral("media-job-meta"));
  text->addWidget(title);
  text->addWidget(meta);
  auto *status = new QLabel(job.statusText, jobCard);
  status->setObjectName(QStringLiteral("media-job-status"));
  header->addWidget(icon);
  header->addLayout(text, 1);
  header->addWidget(status, 0, Qt::AlignTop);
  layout->addLayout(header);
  if (!terminalState(job.state)) {
    auto *progress = new QProgressBar(jobCard);
    progress->setObjectName(QStringLiteral("media-job-progress"));
    progress->setTextVisible(false);
    if (job.state == MediaDownloadState::Processing) progress->setRange(0, 0);
    else { progress->setRange(0, 100); progress->setValue(qRound(job.percent)); }
    progress->setAccessibleName(I18n::text(QStringLiteral("media.download_progress_percent")).arg(qRound(job.percent)));
    layout->addWidget(progress);
    QStringList progressParts;
    if (job.state == MediaDownloadState::Queued || job.state == MediaDownloadState::Downloading)
      progressParts << QStringLiteral("%1%").arg(qRound(job.percent));
    if (job.state == MediaDownloadState::Downloading && job.downloadedBytes <= 0)
      progressParts << I18n::text(QStringLiteral("media.waiting_for_stream"));
    if (job.downloadedBytes > 0) progressParts << (job.totalBytes > 0
        ? QStringLiteral("%1 / %2").arg(formatBytes(job.downloadedBytes), formatBytes(job.totalBytes))
        : formatBytes(job.downloadedBytes));
    else if (job.totalBytes > 0) progressParts << I18n::text(QStringLiteral("media.total_size")).arg(formatBytes(job.totalBytes));
    if (job.bytesPerSecond > 0) progressParts << QStringLiteral("%1/s").arg(formatBytes(job.bytesPerSecond));
    if (job.etaSeconds >= 0) progressParts << I18n::text(QStringLiteral("downloads.time_left")).arg(job.etaSeconds);
    auto *progressText = new QLabel(progressParts.isEmpty() ? I18n::text(QStringLiteral("common.processing")) : progressParts.join(QStringLiteral(" • ")), jobCard);
    progressText->setObjectName(QStringLiteral("media-job-progress-text"));
    layout->addWidget(progressText);
  } else if (job.state == MediaDownloadState::Failed && !job.errorText.isEmpty()) {
    auto *error = new QLabel(friendlyFailure(job.errorText), jobCard);
    error->setObjectName(QStringLiteral("media-job-error"));
    error->setWordWrap(true);
    layout->addWidget(error);
  }
  auto *actions = new QHBoxLayout;
  actions->setSpacing(7);
  actions->addStretch();
  if (!terminalState(job.state)) {
    auto *cancel = actionButton(BrowserIcon::Close, I18n::text(QStringLiteral("common.cancel")), I18n::text(QStringLiteral("media.cancel_this_download")), jobCard, "danger");
    connect(cancel, &QPushButton::clicked, this, [this, id = job.id] { service_->cancel(id); });
    actions->addWidget(cancel);
  } else if (job.state == MediaDownloadState::Completed) {
    auto *open = actionButton(BrowserIcon::Play, I18n::text(QStringLiteral("menu.open")), I18n::text(QStringLiteral("media.open_downloaded_file")), jobCard);
    open->setEnabled(QFileInfo::exists(job.outputPath));
    connect(open, &QPushButton::clicked, this, [this, job] { openMediaDownload(job); });
    auto *folder = actionButton(BrowserIcon::Folder, I18n::text(QStringLiteral("common.show_in_folder")), I18n::text(QStringLiteral("media.open_downloaded_folder")), jobCard);
    const QString folderPath = job.outputPath.isEmpty() ? job.targetDirectory : QFileInfo(job.outputPath).absolutePath();
    folder->setEnabled(QFileInfo(folderPath).isDir());
    connect(folder, &QPushButton::clicked, this, [this, job] {
      if (!DownloadDesktopActions::revealInFolder(job.outputPath, job.targetDirectory)) {
        statusLabel_->setText(I18n::text(QStringLiteral("media.file_not_found")));
      }
    });
    auto *more = new QToolButton(jobCard);
    more->setIcon(BrowserIcons::icon(BrowserIcon::More));
    more->setText(I18n::text(QStringLiteral("common.more")));
    more->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    more->setPopupMode(QToolButton::InstantPopup);
    more->setToolTip(I18n::text(QStringLiteral("media.completed_more_actions")));
    more->setAccessibleName(I18n::text(QStringLiteral("media.completed_more_actions")));
    auto *menu = new QMenu(more);
    QAction *systemOpen = menu->addAction(BrowserIcons::icon(BrowserIcon::Play),
                                           I18n::text(QStringLiteral("common.open_with_system_app")));
    connect(systemOpen, &QAction::triggered, this,
            [this, path = job.outputPath] { openWithSystemApplication(path); });
    menu->addSeparator();
    QAction *remove = menu->addAction(BrowserIcons::icon(BrowserIcon::Trash), I18n::text(QStringLiteral("common.remove_from_list")));
    connect(remove, &QAction::triggered, this, [this, id = job.id] { service_->remove(id); });
    more->setMenu(menu);
    actions->addWidget(open);
    actions->addWidget(folder);
    actions->addWidget(more);
  } else {
    auto *retry = actionButton(BrowserIcon::Reset, I18n::text(QStringLiteral("common.try_again")), I18n::text(QStringLiteral("media.restart_download")), jobCard, "secondary");
    auto *remove = actionButton(BrowserIcon::Trash, I18n::text(QStringLiteral("common.remove_from_list")), I18n::text(QStringLiteral("media.remove_record")), jobCard, "danger");
    connect(retry, &QPushButton::clicked, this, [this, id = job.id] {
      QString error;
      const bool adultProtectionEnabled = profileService_
          && profileService_->isAdultContentProtectionEnabled();
      if (service_->retry(id, adultProtectionEnabled, &error).isNull() && !error.isEmpty())
        statusLabel_->setText(error);
    });
    connect(remove, &QPushButton::clicked, this, [this, id = job.id] { service_->remove(id); });
    actions->addWidget(retry);
    actions->addWidget(remove);
  }
  layout->addLayout(actions);
  return jobCard;
}

QWidget *MediaDownloadPage::createGeneralJobCard(const GeneralDownloadJob &job, QWidget *parent) {
  auto *card = new QFrame(parent);
  card->setObjectName(QStringLiteral("general-download-card"));
  card->setAccessibleName(QStringLiteral("%1, %2").arg(job.fileName, job.statusText));
  auto *layout = new QVBoxLayout(card);
  layout->setContentsMargins(14, 11, 14, 11);
  layout->setSpacing(7);

  auto *header = new QHBoxLayout;
  auto *icon = new QLabel(card);
  icon->setPixmap(BrowserIcons::icon(BrowserIcon::Download).pixmap(26, 26));
  icon->setFixedSize(34, 34);
  icon->setAlignment(Qt::AlignCenter);
  auto *text = new QVBoxLayout;
  text->setSpacing(2);
  auto *title = new QLabel(job.fileName, card);
  title->setObjectName(QStringLiteral("general-download-title"));
  title->setToolTip(job.targetPath);
  auto *meta = new QLabel(I18n::text(QStringLiteral("media.file_connections")).arg(job.connections), card);
  meta->setObjectName(QStringLiteral("general-download-meta"));
  auto *status = new QLabel(job.statusText, card);
  status->setObjectName(QStringLiteral("general-download-status"));
  text->addWidget(title);
  text->addWidget(meta);
  header->addWidget(icon);
  header->addLayout(text, 1);
  header->addWidget(status, 0, Qt::AlignTop);
  layout->addLayout(header);

  const bool active = job.state == GeneralDownloadState::Queued
      || job.state == GeneralDownloadState::Probing
      || job.state == GeneralDownloadState::Downloading
      || job.state == GeneralDownloadState::Paused;
  if (active) {
    auto *progress = new QProgressBar(card);
    progress->setObjectName(QStringLiteral("media-job-progress"));
    progress->setTextVisible(false);
    if (job.totalBytes <= 0 && job.state != GeneralDownloadState::Paused) progress->setRange(0, 0);
    else if (job.totalBytes <= 0) { progress->setRange(0, 1); progress->setValue(0); }
    else {
      progress->setRange(0, 1000);
      progress->setValue(static_cast<int>(std::clamp<qint64>(
          job.downloadedBytes * 1000 / std::max<qint64>(1, job.totalBytes), 0, 1000)));
    }
    layout->addWidget(progress);
    QStringList parts;
    if (job.totalBytes > 0)
      parts << QStringLiteral("%1 / %2").arg(formatBytes(job.downloadedBytes), formatBytes(job.totalBytes));
    else if (job.downloadedBytes > 0) parts << formatBytes(job.downloadedBytes);
    if (job.bytesPerSecond > 0) parts << QStringLiteral("%1/s").arg(formatBytes(job.bytesPerSecond));
    if (job.etaSeconds >= 0) parts << I18n::text(QStringLiteral("downloads.time_left")).arg(job.etaSeconds);
    auto *progressText = new QLabel(parts.isEmpty() ? I18n::text(QStringLiteral("common.preparing"))
                                                    : parts.join(QStringLiteral(" • ")), card);
    progressText->setObjectName(QStringLiteral("general-download-progress-text"));
    layout->addWidget(progressText);
  } else if (job.state == GeneralDownloadState::Failed && !job.errorText.isEmpty()) {
    auto *error = new QLabel(job.errorText, card);
    error->setObjectName(QStringLiteral("media-job-error"));
    error->setWordWrap(true);
    layout->addWidget(error);
  }

  auto *actions = new QHBoxLayout;
  actions->setSpacing(7);
  actions->addStretch();
  if (job.state == GeneralDownloadState::Downloading || job.state == GeneralDownloadState::Probing) {
    auto *pause = new QPushButton(I18n::text(QStringLiteral("common.pause")), card);
    pause->setAccessibleName(I18n::text(QStringLiteral("media.pause_download")));
    auto *cancelButton = actionButton(BrowserIcon::Close, I18n::text(QStringLiteral("common.cancel")),
                                      I18n::text(QStringLiteral("media.cancel_download")), card, "danger");
    connect(pause, &QPushButton::clicked, this, [this, id = job.id] { generalDownloads_->pause(id); });
    connect(cancelButton, &QPushButton::clicked, this, [this, id = job.id] { generalDownloads_->cancel(id); });
    actions->addWidget(pause);
    actions->addWidget(cancelButton);
  } else if (job.state == GeneralDownloadState::Paused) {
    auto *resumeButton = actionButton(BrowserIcon::Reset, I18n::text(QStringLiteral("common.resume")),
                                      I18n::text(QStringLiteral("media.resume_download")), card, "secondary");
    auto *cancelButton = actionButton(BrowserIcon::Close, I18n::text(QStringLiteral("common.cancel")),
                                      I18n::text(QStringLiteral("media.cancel_download")), card, "danger");
    connect(resumeButton, &QPushButton::clicked, this, [this, id = job.id] { generalDownloads_->resume(id); });
    connect(cancelButton, &QPushButton::clicked, this, [this, id = job.id] { generalDownloads_->cancel(id); });
    actions->addWidget(resumeButton);
    actions->addWidget(cancelButton);
  } else if (job.state == GeneralDownloadState::Completed) {
    auto *open = actionButton(BrowserIcon::Play, I18n::text(QStringLiteral("menu.open")), I18n::text(QStringLiteral("common.open_file")), card);
    auto *folder = actionButton(BrowserIcon::Folder, I18n::text(QStringLiteral("common.show_in_folder")),
                                I18n::text(QStringLiteral("media.open_file_folder")), card);
    open->setEnabled(QFileInfo::exists(job.targetPath));
    const QString folderPath = QFileInfo(job.targetPath).absolutePath();
    folder->setEnabled(QFileInfo(folderPath).isDir());
    connect(open, &QPushButton::clicked, this, [this, job] { openGeneralDownload(job); });
    connect(folder, &QPushButton::clicked, this, [this, job] {
      const QString fallback = QFileInfo(job.targetPath).absolutePath();
      if (!DownloadDesktopActions::revealInFolder(job.targetPath, fallback)) {
        statusLabel_->setText(I18n::text(QStringLiteral("media.file_not_found")));
      }
    });
    actions->addWidget(open);
    actions->addWidget(folder);
  } else {
    auto *retryButton = actionButton(BrowserIcon::Reset, I18n::text(QStringLiteral("common.try_again")),
                                     I18n::text(QStringLiteral("media.redownload_file")), card, "secondary");
    connect(retryButton, &QPushButton::clicked, this, [this, id = job.id] { generalDownloads_->retry(id); });
    actions->addWidget(retryButton);
  }
  if (!active) {
    auto *removeButton = actionButton(BrowserIcon::Trash, I18n::text(QStringLiteral("common.remove_from_list")),
                                      I18n::text(QStringLiteral("media.remove_download_record")), card, "tertiary");
    connect(removeButton, &QPushButton::clicked, this, [this, id = job.id] { generalDownloads_->remove(id); });
    actions->addWidget(removeButton);
  }
  layout->addLayout(actions);
  return card;
}

void MediaDownloadPage::refreshJobs() {
  clearLayout(activeJobsLayout_);
  clearLayout(historyJobsLayout_);
  int activeCount = 0;
  int historyCount = 0;
  int mediaHistoryCount = 0;
  for (const MediaDownloadJob &job : service_->jobs()) {
    if (terminalState(job.state)) { historyJobsLayout_->addWidget(createJobCard(job, historyJobsLayout_->parentWidget())); ++historyCount; ++mediaHistoryCount; }
    else { activeJobsLayout_->addWidget(createJobCard(job, activeJobsLayout_->parentWidget())); ++activeCount; }
  }
  if (generalDownloads_) {
    for (const GeneralDownloadJob &job : generalDownloads_->jobs()) {
      const bool finished = job.state == GeneralDownloadState::Completed
          || job.state == GeneralDownloadState::Failed
          || job.state == GeneralDownloadState::Cancelled;
      if (finished) {
        historyJobsLayout_->addWidget(createGeneralJobCard(job, historyJobsLayout_->parentWidget()));
        ++historyCount;
      } else {
        activeJobsLayout_->addWidget(createGeneralJobCard(job, activeJobsLayout_->parentWidget()));
        ++activeCount;
      }
    }
  }
  if (!activeCount) {
    activeEmptyLabel_ = emptyState(BrowserIcons::icon(BrowserIcon::Download), I18n::text(QStringLiteral("media.no_active_downloads")),
        I18n::text(QStringLiteral("media.no_active_downloads_desc")), activeJobsLayout_->parentWidget());
    activeJobsLayout_->addWidget(activeEmptyLabel_);
  }
  if (!historyCount) {
    historyEmptyLabel_ = emptyState(BrowserIcons::icon(BrowserIcon::History), I18n::text(QStringLiteral("settings.downloads.empty")),
        I18n::text(QStringLiteral("media.empty_history_desc")),
        historyJobsLayout_->parentWidget());
    historyJobsLayout_->addWidget(historyEmptyLabel_);
  }
  exportButton_->setEnabled(mediaHistoryCount > 0);

  if (isMediaNotFoundMessage(statusLabel_->text())) {
    const QUrl currentUrl = sourceUrl();
    if (hasActiveOrCompletedGeneralDownload(currentUrl)) {
      statusLabel_->setText(service_ && service_->ytDlpAvailable()
          ? I18n::text(QStringLiteral("media.ready_to_download"))
          : I18n::text(QStringLiteral("media.engine_ready")));
    }
  }
}

bool MediaDownloadPage::hasActiveOrCompletedGeneralDownload(const QUrl &url) const {
  if (generalDownloads_) {
    const QVector<GeneralDownloadJob> jobs = generalDownloads_->jobs();
    const QString host = url.isValid() ? url.host().toLower() : QString{};
    const QString urlStr = url.isValid() ? url.toString(QUrl::FullyEncoded) : QString{};

    for (const GeneralDownloadJob &job : jobs) {
      const bool isActive = (job.state == GeneralDownloadState::Downloading
          || job.state == GeneralDownloadState::Probing
          || job.state == GeneralDownloadState::Queued
          || job.state == GeneralDownloadState::Paused);
      const bool isCompleted = (job.state == GeneralDownloadState::Completed);

      if (!isActive && !isCompleted) continue;

      if (!urlStr.isEmpty()) {
        if (job.url == url || job.url.toString(QUrl::FullyEncoded) == urlStr)
          return true;
        const auto req = generalDownloads_->request(job.id);
        if (!req.referrer.isEmpty()) {
          const QUrl ref = QUrl::fromEncoded(req.referrer);
          if (ref == url || ref.toString(QUrl::FullyEncoded) == urlStr)
            return true;
          if (!host.isEmpty() && ref.host().compare(host, Qt::CaseInsensitive) == 0)
            return true;
        }
        if (!host.isEmpty() && job.url.host().compare(host, Qt::CaseInsensitive) == 0)
          return true;
      }

      if (isActive) return true;
    }
  }

  if (profileService_) {
    for (const BrowserDownloadEntry &entry : profileService_->nativeDownloads()) {
      if (I18n::isTranslationOf(QStringLiteral("downloads.state_downloading"), entry.state)) return true;
    }
  }

  return false;
}

bool MediaDownloadPage::handleGeneralDownloadIfDirectFile(const QUrl &url) {
  if (!generalDownloads_ || !isDirectGeneralFileUrl(url)) return false;
  GeneralDownloadRequest req;
  req.url = url;
  QString fileName = QFileInfo(url.path()).fileName();
  if (fileName.isEmpty()) fileName = QStringLiteral("download");
  req.suggestedFileName = fileName;
  QString directory = profileService_ ? profileService_->configuredDownloadDirectory()
                                      : (service_ ? service_->defaultDownloadDirectory() : QString{});
  if (directory.isEmpty()) directory = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
  req.targetDirectory = directory;
  req.connectionCount = 4;
  req.allowParallel = true;
  const QUuid id = generalDownloads_->enqueue(req);
  if (!id.isNull()) {
    statusLabel_->setText(I18n::text(QStringLiteral("media.file_download_started")));
    return true;
  }
  return false;
}

void MediaDownloadPage::refreshBrowserDownloads() {
  clearLayout(browserDownloadsLayout_);
  const QList<BrowserDownloadEntry> downloads = profileService_ ? profileService_->nativeDownloads() : QList<BrowserDownloadEntry>{};
  if (browserDownloadsCard_) browserDownloadsCard_->setVisible(!downloads.isEmpty());
  for (const BrowserDownloadEntry &entry : downloads) {
    auto *row = new QFrame(browserDownloadsLayout_->parentWidget());
    row->setObjectName(QStringLiteral("browser-download-row"));
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(13, 11, 13, 11);
    auto *icon = new QLabel(row);
    icon->setPixmap(BrowserIcons::icon(BrowserIcon::Download).pixmap(24, 24));
    auto *text = new QVBoxLayout;
    auto *title = new QLabel(entry.fileName, row);
    title->setObjectName(QStringLiteral("browser-download-title"));
    title->setToolTip(entry.fileName);
    auto *meta = new QLabel(QStringLiteral("%1 • %2").arg(entry.state, entry.path), row);
    meta->setObjectName(QStringLiteral("browser-download-meta"));
    meta->setToolTip(entry.path);
    meta->setTextInteractionFlags(Qt::TextSelectableByKeyboard | Qt::TextSelectableByMouse);
    text->addWidget(title);
    text->addWidget(meta);
    rowLayout->addWidget(icon);
    rowLayout->addLayout(text, 1);
    browserDownloadsLayout_->addWidget(row);
  }
}

void MediaDownloadPage::loadThumbnail(const QUrl &url) {
  if (thumbnailReply_) { thumbnailReply_->abort(); thumbnailReply_->deleteLater(); thumbnailReply_ = nullptr; }
  thumbnailLabel_->setPixmap(BrowserIcons::icon(BrowserIcon::Video).pixmap(40, 40));
  if (!url.isValid() || (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https"))) return;
  QNetworkRequest request(url);
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
  request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
  request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
  thumbnailReply_ = thumbnailNetwork_->get(request);
  connect(thumbnailReply_, &QNetworkReply::readyRead, this, [this] {
    if (thumbnailReply_ && thumbnailReply_->bytesAvailable() > 5 * 1024 * 1024) thumbnailReply_->abort();
  });
  connect(thumbnailReply_, &QNetworkReply::finished, this, [this] {
    QNetworkReply *reply = thumbnailReply_;
    thumbnailReply_ = nullptr;
    if (!reply) return;
    const QByteArray bytes = reply->error() == QNetworkReply::NoError && reply->bytesAvailable() <= 5 * 1024 * 1024 ? reply->readAll() : QByteArray{};
    QPixmap image;
    if (!bytes.isEmpty() && image.loadFromData(bytes))
      thumbnailLabel_->setPixmap(image.scaled(thumbnailLabel_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    reply->deleteLater();
  });
}

void MediaDownloadPage::openMediaDownload(const MediaDownloadJob &job) {
  if (job.outputPath.isEmpty() || !QFileInfo::exists(job.outputPath)) {
    statusLabel_->setText(I18n::text(QStringLiteral("media.file_not_found")));
    return;
  }
  if (!LocalMediaRouting::isSafeDownloadedFile(job.outputPath, job.targetDirectory)) {
    statusLabel_->setText(I18n::text(QStringLiteral("media.file_not_in_safe_folder")));
    return;
  }
  const LocalMediaPlayerKind kind = LocalMediaRouting::classify(job.outputPath, job.mimeType, &job.kind);
  if (kind == LocalMediaPlayerKind::External) {
    if (!DownloadDesktopActions::openFile(job.outputPath, job.targetDirectory)) {
      statusLabel_->setText(I18n::text(QStringLiteral("media.file_missing_or_unreadable")));
    }
    return;
  }
  LocalMediaOpenRequest request;
  request.path = QFileInfo(job.outputPath).canonicalFilePath();
  request.allowedRoot = QFileInfo(job.targetDirectory).canonicalFilePath();
  request.title = job.title;
  request.mimeType = LocalMediaRouting::detectedMimeType(job.outputPath, job.mimeType);
  request.thumbnailUrl = job.thumbnailUrl;
  request.playerKind = kind;
  emit internalMediaOpenRequested(request);
}

void MediaDownloadPage::openGeneralDownload(const GeneralDownloadJob &job) {
  const QString root = QFileInfo(job.targetPath).absolutePath();
  if (job.targetPath.isEmpty() || !QFileInfo::exists(job.targetPath)) {
    statusLabel_->setText(I18n::text(QStringLiteral("media.file_not_found")));
    return;
  }
  if (!LocalMediaRouting::isSafeDownloadedFile(job.targetPath, root)) {
    statusLabel_->setText(I18n::text(QStringLiteral("media.file_missing_or_unreadable")));
    return;
  }
  const LocalMediaPlayerKind kind = LocalMediaRouting::classify(job.targetPath, job.mimeType);
  if (kind == LocalMediaPlayerKind::External) {
    if (!DownloadDesktopActions::openFile(job.targetPath, root)) {
      statusLabel_->setText(I18n::text(QStringLiteral("media.file_missing_or_unreadable")));
    }
    return;
  }
  LocalMediaOpenRequest request;
  request.path = QFileInfo(job.targetPath).canonicalFilePath();
  request.allowedRoot = QFileInfo(root).canonicalFilePath();
  request.title = job.fileName;
  request.mimeType = LocalMediaRouting::detectedMimeType(job.targetPath, job.mimeType);
  request.playerKind = kind;
  emit internalMediaOpenRequested(request);
}

void MediaDownloadPage::openWithSystemApplication(const QString &path) {
  const QFileInfo file(path);
  if (!file.isFile()) {
    statusLabel_->setText(I18n::text(QStringLiteral("media.file_not_found")));
    return;
  }

#if defined(Q_OS_LINUX)
  // Always ask here. The user may have associated media files with DaliNira,
  // in which case QDesktopServices would route straight back into this
  // browser instead of opening an external player.
  const QString portalClient = QStringLiteral("/usr/bin/gdbus");
  if (QFileInfo(portalClient).isExecutable()) {
    auto *process = new QProcess(this);
    process->setProgram(portalClient);
    process->setArguments({QStringLiteral("call"), QStringLiteral("--session"),
        QStringLiteral("--dest"), QStringLiteral("org.freedesktop.portal.Desktop"),
        QStringLiteral("--object-path"), QStringLiteral("/org/freedesktop/portal/desktop"),
        QStringLiteral("--method"), QStringLiteral("org.freedesktop.portal.OpenURI.OpenURI"),
        QString{}, QUrl::fromLocalFile(file.absoluteFilePath()).toString(QUrl::FullyEncoded),
        QStringLiteral("{'ask': <true>}")});
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, process](int exitCode, QProcess::ExitStatus exitStatus) {
      if (exitStatus != QProcess::NormalExit || exitCode != 0)
        statusLabel_->setText(I18n::text(QStringLiteral("media.app_chooser_failed")));
      process->deleteLater();
    });
    connect(process, &QProcess::errorOccurred, this,
            [this, process](QProcess::ProcessError error) {
      if (error == QProcess::FailedToStart) {
        statusLabel_->setText(I18n::text(QStringLiteral("media.app_chooser_failed")));
        process->deleteLater();
      }
    });
    process->start();
    return;
  }
#endif

  if (!QDesktopServices::openUrl(QUrl::fromLocalFile(file.absoluteFilePath())))
    statusLabel_->setText(I18n::text(QStringLiteral("media.open_with_system_failed")));
}

void MediaDownloadPage::clearLayout(QLayout *layout) {
  if (!layout) return;
  while (QLayoutItem *item = layout->takeAt(0)) {
    if (QWidget *widget = item->widget()) delete widget;
    if (QLayout *child = item->layout()) clearLayout(child);
    delete item;
  }
}

QString MediaDownloadPage::formatBytes(qint64 bytes) {
  if (bytes <= 0) return QStringLiteral("—");
  if (bytes >= 1024 * 1024 * 1024LL) return I18n::text(QStringLiteral("units.gigabytes")).arg(bytes / 1024.0 / 1024.0 / 1024.0, 0, 'f', 2);
  if (bytes >= 1024 * 1024) return I18n::text(QStringLiteral("units.megabytes")).arg(bytes / 1024.0 / 1024.0, 0, 'f', 1);
  return I18n::text(QStringLiteral("units.kilobytes")).arg(bytes / 1024.0, 0, 'f', 0);
}

void MediaDownloadPage::exportHistory(bool csv) {
  const QString extension = csv ? QStringLiteral("csv") : QStringLiteral("json");
  const QString selected = QFileDialog::getSaveFileName(this, I18n::text(QStringLiteral("media.export_history_title")),
      QDir::home().filePath(QStringLiteral("dalinira-media-downloads.%1").arg(extension)),
      csv ? QStringLiteral("CSV (*.csv)") : QStringLiteral("JSON (*.json)"));
  if (selected.isEmpty()) return;
  QByteArray bytes;
  if (csv) {
    const auto escaped = [](QString value) { value.replace(QLatin1Char('"'), QStringLiteral("\"\"")); return QLatin1Char('"') + value + QLatin1Char('"'); };
    QString output = QStringLiteral("title,url,status,output,created_at\n");
    for (const MediaDownloadJob &job : service_->jobs()) {
      if (!terminalState(job.state)) continue;
      output += QStringList{escaped(job.title), escaped(BrowserSecurity::sanitizeUrlForPersistence(job.url).toString(QUrl::FullyEncoded)),
          escaped(job.statusText), escaped(job.outputPath), escaped(job.createdAt.toString(Qt::ISODate))}.join(QLatin1Char(',')) + QLatin1Char('\n');
    }
    bytes = output.toUtf8();
  } else {
    QJsonArray entries;
    for (const MediaDownloadJob &job : service_->jobs()) {
      if (!terminalState(job.state)) continue;
      entries.append(QJsonObject{{QStringLiteral("title"), job.title},
          {QStringLiteral("url"), BrowserSecurity::sanitizeUrlForPersistence(job.url).toString(QUrl::FullyEncoded)},
          {QStringLiteral("status"), job.statusText}, {QStringLiteral("output"), job.outputPath},
          {QStringLiteral("createdAt"), job.createdAt.toString(Qt::ISODate)}});
    }
    bytes = QJsonDocument(entries).toJson(QJsonDocument::Indented);
  }
  QSaveFile file(selected);
  if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)
      || file.write(bytes) != bytes.size() || !file.commit()) {
    statusLabel_->setText(I18n::text(QStringLiteral("media.export_history_failed")));
    return;
  }
  QFile::setPermissions(selected, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  statusLabel_->setText(I18n::text(QStringLiteral("media.export_history_done")));
}
