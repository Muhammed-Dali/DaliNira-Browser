#include "tab_hover_card.h"
#include "browser_icons.h"
#include "i18n/i18n.h"

#include <QGuiApplication>
#include <QHBoxLayout>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QVBoxLayout>

namespace dalinira {

QString formatMemorySize(qint64 bytes) {
  if (bytes < 1024 * 1024) return I18n::text(QStringLiteral("units.kilobytes")).arg(qMax<qint64>(0, bytes) / 1024);
  const double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
  if (mb >= 1024.0) return I18n::text(QStringLiteral("units.gigabytes")).arg(mb / 1024.0, 0, 'f', 1);
  return I18n::text(QStringLiteral("units.megabytes")).arg(qRound(mb));
}

TabResourceText describeTabResources(const TabResourceSnapshot &snapshot) {
  TabResourceText text;
  switch (snapshot.activity) {
    case TabActivity::Active:
      text.activity = I18n::text(QStringLiteral("tab.hover.state_active"));
      break;
    case TabActivity::Background:
      text.activity = I18n::text(QStringLiteral("tab.hover.state_background"));
      break;
    case TabActivity::LightSleep:
      text.activity = I18n::text(QStringLiteral("tab.hover.state_light_sleep"));
      break;
    case TabActivity::Sleeping:
      text.activity = I18n::text(QStringLiteral("tab.hover.state_sleeping"));
      break;
  }

  const QString memoryLabel = I18n::text(QStringLiteral("tab.hover.memory"));
  switch (snapshot.memory) {
    case TabMemoryAttribution::Dedicated:
      text.memory = memoryLabel.arg(formatMemorySize(snapshot.tabMemoryBytes));
      break;
    case TabMemoryAttribution::Shared:
      // The process total belongs to all of its tabs; it is never shown as
      // this tab's memory.
      text.memory = memoryLabel.arg(
          I18n::text(QStringLiteral("tab.hover.memory_not_separable")));
      text.sharedMemory = I18n::text(QStringLiteral("tab.hover.shared_process_memory"))
                              .arg(formatMemorySize(snapshot.processMemoryBytes))
                              .arg(snapshot.tabsInProcess);
      break;
    case TabMemoryAttribution::Released:
      // The page's renderer / web process memory was released; the browser
      // keeps only its session data (history, title, icon).
      text.memory = I18n::text(QStringLiteral("tab.hover.memory_released"));
      if (snapshot.memoryBeforeSleepBytes > 0) {
        text.memoryBeforeSleep =
            I18n::text(QStringLiteral("tab.hover.memory_before_sleep"))
                .arg(formatMemorySize(snapshot.memoryBeforeSleepBytes));
      }
      break;
    case TabMemoryAttribution::Unavailable:
      text.memory = memoryLabel.arg(I18n::text(QStringLiteral("tab.hover.memory_measuring")));
      break;
  }
  if (snapshot.cpuPercent.has_value()) {
    text.cpu = I18n::text(QStringLiteral("tab.hover.cpu"))
                   .arg(qRound(*snapshot.cpuPercent));
  }
  if (snapshot.processId > 0) {
    text.diagnostics = QStringLiteral("pid %1 · PSS %2 · private %3 · %4 tab(s) in process")
                           .arg(snapshot.processId)
                           .arg(formatMemorySize(snapshot.processMemoryBytes))
                           .arg(formatMemorySize(snapshot.processPrivateBytes))
                           .arg(snapshot.tabsInProcess);
  }
  return text;
}

}  // namespace dalinira

TabHoverCard::TabHoverCard(QWidget *parent) : QFrame(parent) {
  setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint | Qt::WindowTransparentForInput);
  setAttribute(Qt::WA_ShowWithoutActivating, true);
  setAttribute(Qt::WA_TransparentForMouseEvents, true);
  setLayoutDirection(Qt::LeftToRight);

  setStyleSheet(QStringLiteral(
      "TabHoverCard {"
      "  background-color: #1c242e;"
      "  border: 1px solid #364554;"
      "  border-radius: 10px;"
      "}"
      "QLabel#hover-title {"
      "  color: #e8eef5;"
      "  font-size: 13px;"
      "  font-weight: bold;"
      "}"
      "QLabel#hover-domain {"
      "  color: #9aa0a6;"
      "  font-size: 11px;"
      "}"
      "QFrame#hover-sep {"
      "  background-color: #2c3846;"
      "  max-height: 1px;"
      "  min-height: 1px;"
      "}"
  ));

  auto *mainLayout = new QVBoxLayout(this);
  mainLayout->setContentsMargins(12, 10, 12, 10);
  mainLayout->setSpacing(5);

  auto *topLayout = new QHBoxLayout;
  topLayout->setContentsMargins(0, 0, 0, 0);
  topLayout->setSpacing(8);

  iconLabel_ = new QLabel(this);
  iconLabel_->setFixedSize(16, 16);
  iconLabel_->hide();

  auto *textLayout = new QVBoxLayout;
  textLayout->setContentsMargins(0, 0, 0, 0);
  textLayout->setSpacing(2);

  titleLabel_ = new QLabel(this);
  titleLabel_->setObjectName(QStringLiteral("hover-title"));
  titleLabel_->setWordWrap(true);
  titleLabel_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

  domainLabel_ = new QLabel(this);
  domainLabel_->setObjectName(QStringLiteral("hover-domain"));

  textLayout->addWidget(titleLabel_);
  textLayout->addWidget(domainLabel_);

  topLayout->addWidget(iconLabel_, 0, Qt::AlignTop);
  topLayout->addLayout(textLayout, 1);
  mainLayout->addLayout(topLayout);

  separator_ = new QFrame(this);
  separator_->setObjectName(QStringLiteral("hover-sep"));
  mainLayout->addWidget(separator_);

  audioTextLabel_ = addRow(BrowserIcons::icon(BrowserIcon::Audio), QStringLiteral("hover-audio"));
  audioTextLabel_->setText(I18n::text(QStringLiteral("tab.hover.audio")));
  lifecycleTextLabel_ = addRow(BrowserIcons::icon(BrowserIcon::Performance), QStringLiteral("hover-lifecycle"));
  memoryTextLabel_ = addRow(BrowserIcons::icon(BrowserIcon::Memory), QStringLiteral("hover-memory"));
  sharedMemoryTextLabel_ = addRow(QIcon(), QStringLiteral("hover-shared-memory"));
  sharedMemoryTextLabel_->setStyleSheet(
      QStringLiteral("color: #9fb8cc; font-size: 11px; font-weight: 600;"));
  memoryBeforeSleepTextLabel_ = addRow(QIcon(), QStringLiteral("hover-memory-before-sleep"));
  memoryBeforeSleepTextLabel_->setStyleSheet(
      QStringLiteral("color: #9aa8b5; font-size: 11px;"));
  cpuTextLabel_ = addRow(BrowserIcons::icon(BrowserIcon::Performance), QStringLiteral("hover-cpu"));
  diagnosticsLabel_ = addRow(QIcon(), QStringLiteral("hover-diagnostics"));
  diagnosticsLabel_->setStyleSheet(QStringLiteral("color: #7d8894; font-size: 10px;"));
  // Developer diagnostics (process id, PSS, private memory) are opt-in.
  showDiagnostics_ = qEnvironmentVariableIntValue("DALINIRA_TAB_DIAGNOSTICS") == 1;

  setFixedWidth(320);

  pollTimer_.setInterval(1500);
  connect(&pollTimer_, &QTimer::timeout, this, [this]() {
    if (!isVisible()) {
      pollTimer_.stop();
      return;
    }
    refreshCardInfo();
  });
  connect(&dalinira::i18n::LanguageManager::instance(),
          &dalinira::i18n::LanguageManager::languageChanged, this,
          [this] {
            audioTextLabel_->setText(I18n::text(QStringLiteral("tab.hover.audio")));
            if (isVisible()) refreshCardInfo();
          });
}

QLabel *TabHoverCard::addRow(const QIcon &icon, const QString &objectName) {
  auto *row = new QWidget(this);
  auto *layout = new QHBoxLayout(row);
  layout->setContentsMargins(0, 1, 0, 1);
  layout->setSpacing(6);
  auto *iconLabel = new QLabel(row);
  iconLabel->setFixedSize(14, 14);
  if (!icon.isNull()) iconLabel->setPixmap(icon.pixmap(14, 14));
  auto *text = new QLabel(row);
  text->setObjectName(objectName);
  text->setStyleSheet(QStringLiteral("color: #c4c7c5; font-size: 11px;"));
  text->setWordWrap(true);
  text->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  layout->addWidget(iconLabel, 0, Qt::AlignLeft | Qt::AlignVCenter);
  // A horizontal alignment flag makes Qt keep the label at its narrow
  // sizeHint even with stretch=1. Let it fill the row so height-for-width and
  // wrapping use the card's actual content width.
  layout->addWidget(text, 1, Qt::AlignVCenter);
  static_cast<QVBoxLayout *>(this->layout())->addWidget(row);
  row->hide();
  return text;
}

QRect TabHoverCard::boundedCardGeometry(const QRect &globalTabRect, const QSize &cardSize,
                                        const QRect &availableGeometry) {
  constexpr int kMargin = 8;
  constexpr int kGap = 4;
  const int maxWidth = qMax(1, availableGeometry.width() - 2 * kMargin);
  const int maxHeight = qMax(1, availableGeometry.height() - 2 * kMargin);
  const QSize boundedSize(qMin(cardSize.width(), maxWidth), qMin(cardSize.height(), maxHeight));

  int x = globalTabRect.left() + kMargin;
  int y = globalTabRect.bottom() + 1 + kGap;
  const int minX = availableGeometry.left() + kMargin;
  const int maxX = availableGeometry.right() - kMargin - boundedSize.width() + 1;
  x = qBound(minX, x, qMax(minX, maxX));

  const int minY = availableGeometry.top() + kMargin;
  const int maxY = availableGeometry.bottom() - kMargin - boundedSize.height() + 1;
  if (y > maxY) y = globalTabRect.top() - kGap - boundedSize.height();
  y = qBound(minY, y, qMax(minY, maxY));
  return QRect(QPoint(x, y), boundedSize);
}

void TabHoverCard::updateContentGeometry() {
  constexpr int kCardWidth = 320;
  constexpr int kHorizontalMargins = 24;
  constexpr int kIconWidth = 14;
  constexpr int kRowSpacing = 6;
  constexpr int kRowVerticalMargins = 2;
  constexpr int kTitleIconAndSpacing = 24;
  constexpr int kMaxTitleLines = 2;
  constexpr int kWrapSafety = 8;  // frame/subpixel rounding must not create a third line

  setFixedWidth(kCardWidth);
  ensurePolished();

  const int titleWidth = kCardWidth - kHorizontalMargins -
                         (iconLabel_->isVisible() ? kTitleIconAndSpacing : 0) - kWrapSafety;
  const QFontMetrics titleMetrics(titleLabel_->font());
  const int maxTitleHeight = titleMetrics.lineSpacing() * kMaxTitleLines;
  QString displayTitle = fullTitle_;
  QRect titleBounds = titleMetrics.boundingRect(
      QRect(0, 0, titleWidth, 10000), Qt::TextWordWrap | Qt::AlignLeft, displayTitle);
  if (titleBounds.height() > maxTitleHeight) {
    int low = 0;
    int high = displayTitle.size();
    while (low < high) {
      const int mid = (low + high + 1) / 2;
      const QString candidate = displayTitle.left(mid).trimmed() + QChar(0x2026);
      const int height = titleMetrics.boundingRect(
          QRect(0, 0, titleWidth, 10000), Qt::TextWordWrap | Qt::AlignLeft, candidate).height();
      if (height <= maxTitleHeight) low = mid;
      else high = mid - 1;
    }
    displayTitle = displayTitle.left(low).trimmed() + QChar(0x2026);
    titleBounds = titleMetrics.boundingRect(
        QRect(0, 0, titleWidth, 10000), Qt::TextWordWrap | Qt::AlignLeft, displayTitle);
  }
  titleLabel_->setText(displayTitle);
  titleLabel_->setToolTip(displayTitle == fullTitle_ ? QString() : fullTitle_);
  titleLabel_->setFixedHeight(qMax(titleMetrics.lineSpacing(), titleBounds.height()));

  const QFontMetrics domainMetrics(domainLabel_->font());
  domainLabel_->setText(domainMetrics.elidedText(fullDomain_, Qt::ElideMiddle, titleWidth));
  domainLabel_->setToolTip(domainLabel_->text() == fullDomain_ ? QString() : fullDomain_);

  const int rowTextWidth = kCardWidth - kHorizontalMargins - kIconWidth - kRowSpacing;
  for (QLabel *label : {audioTextLabel_, lifecycleTextLabel_, memoryTextLabel_,
                        sharedMemoryTextLabel_, memoryBeforeSleepTextLabel_, cpuTextLabel_,
                        diagnosticsLabel_}) {
    QWidget *row = label->parentWidget();
    if (!row->isVisible()) continue;
    const QFontMetrics metrics(label->font());
    const int textHeight = metrics.boundingRect(
        QRect(0, 0, rowTextWidth, 10000), Qt::TextWordWrap | Qt::AlignLeft, label->text()).height();
    row->setFixedHeight(qMax(kIconWidth, textHeight) + kRowVerticalMargins);
  }

  layout()->invalidate();
  layout()->activate();
  setFixedHeight(layout()->sizeHint().height());
  updatePosition();
}

void TabHoverCard::updatePosition() {
  if (!globalTabRect_.isValid()) return;
  QScreen *screen = QGuiApplication::screenAt(globalTabRect_.center());
  if (!screen) screen = QGuiApplication::primaryScreen();
  if (!screen) return;

  QRect bounds = screen->availableGeometry();
  if (parentWidget() && parentWidget()->window()) {
    const QRect windowBounds = parentWidget()->window()->frameGeometry();
    const QRect intersection = bounds.intersected(windowBounds);
    if (intersection.width() >= width() + 16 && intersection.height() >= height() + 16) {
      bounds = intersection;
    }
  }
  setGeometry(boundedCardGeometry(globalTabRect_, size(), bounds));
}

QString TabHoverCard::extractDomain(const QUrl &url) {
  if (url.isEmpty()) return QStringLiteral("DaliNiraBrowser");

  if (url.scheme() == QLatin1String("dalinira")) {
    const QString host = url.host().toLower();
    const QString path = url.path().toLower();
    if (host == QLatin1String("newtab") || host == QLatin1String("dalinira-browser.local")) {
      return QStringLiteral("DaliNiraBrowser");
    }
    if (host == QLatin1String("settings") || path.contains(QLatin1String("settings"))) {
      return I18n::text(QStringLiteral("menu.settings"));
    }
    if (host == QLatin1String("audio-effects") || path.contains(QLatin1String("audio-effects"))) {
      return I18n::text(QStringLiteral("menu.audio_effects"));
    }
    if (host == QLatin1String("eq-presets") || path.contains(QLatin1String("eq-presets"))) {
      return I18n::text(QStringLiteral("menu.eq_presets"));
    }
    if (host == QLatin1String("passwords") || path.contains(QLatin1String("passwords"))) {
      return I18n::text(QStringLiteral("toolbar.passwords"));
    }
    if (host == QLatin1String("downloads") || path.contains(QLatin1String("downloads"))) {
      return I18n::text(QStringLiteral("newtab.downloads"));
    }
    if (host == QLatin1String("blocker") || path.contains(QLatin1String("blocker"))) {
      return I18n::text(QStringLiteral("shield.name"));
    }
    if (host == QLatin1String("listen") || host == QLatin1String("listen-settings")) {
      return QStringLiteral("DaliNira Pulse");
    }
    return QStringLiteral("DaliNira");
  }

  if (url.host() == QLatin1String("dalinira-browser.local")) {
    return QStringLiteral("DaliNiraBrowser");
  }

  if (!url.host().isEmpty()) {
    QString host = url.host().toLower();
    if (host.startsWith(QStringLiteral("www."))) {
      host.remove(0, 4);
    }
    return host;
  }

  return QStringLiteral("DaliNiraBrowser");
}

void TabHoverCard::refreshCardInfo() {
  const auto setRow = [](QLabel *label, const QString &text) {
    label->setText(text);
    label->parentWidget()->setVisible(!text.isEmpty());
  };
  if (isInternal_ || !snapshotProvider_) {
    audioTextLabel_->parentWidget()->hide();
    setRow(lifecycleTextLabel_, I18n::text(QStringLiteral("tab.hover.internal_state")));
    setRow(memoryTextLabel_, I18n::text(QStringLiteral("tab.hover.internal_page")));
    setRow(sharedMemoryTextLabel_, QString());
    setRow(memoryBeforeSleepTextLabel_, QString());
    setRow(cpuTextLabel_, QString());
    setRow(diagnosticsLabel_, QString());
    separator_->show();
    updateContentGeometry();
    return;
  }

  const dalinira::TabResourceSnapshot snapshot = snapshotProvider_();
  const dalinira::TabResourceText text = dalinira::describeTabResources(snapshot);
  audioTextLabel_->parentWidget()->setVisible(snapshot.audible && snapshot.activity != dalinira::TabActivity::Sleeping &&
                                              snapshot.activity != dalinira::TabActivity::LightSleep);
  setRow(lifecycleTextLabel_, text.activity);
  setRow(memoryTextLabel_, text.memory);
  setRow(sharedMemoryTextLabel_, text.sharedMemory);
  setRow(memoryBeforeSleepTextLabel_, text.memoryBeforeSleep);
  setRow(cpuTextLabel_, text.cpu);
  setRow(diagnosticsLabel_, showDiagnostics_ ? text.diagnostics : QString());
  separator_->show();
  updateContentGeometry();
}

void TabHoverCard::showForTab(const QString &title, const QUrl &url, const QIcon &icon, const QRect &globalTabRect,
                              SnapshotProvider snapshot, bool isInternal) {
  snapshotProvider_ = std::move(snapshot);
  isInternal_ = isInternal;
  globalTabRect_ = globalTabRect;

  fullTitle_ = title.isEmpty()
                   ? I18n::text(QStringLiteral("tab.new"))
                   : title;
  if (!icon.isNull()) {
    iconLabel_->setPixmap(icon.pixmap(16, 16));
    iconLabel_->show();
  } else {
    iconLabel_->hide();
  }
  fullDomain_ = extractDomain(url);

  refreshCardInfo();
  show();
  raise();
  if (!pollTimer_.isActive()) pollTimer_.start();
}

void TabHoverCard::hideCard() {
  pollTimer_.stop();
  snapshotProvider_ = nullptr;
  hide();
}

void TabHoverCard::hideEvent(QHideEvent *event) {
  pollTimer_.stop();
  snapshotProvider_ = nullptr;
  QFrame::hideEvent(event);
}

void TabHoverCard::paintEvent(QPaintEvent *event) {
  Q_UNUSED(event);
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);

  QPainterPath path;
  path.addRoundedRect(rect(), 10, 10);

  painter.fillPath(path, QColor(QStringLiteral("#1c242e")));
  QPen pen(QColor(QStringLiteral("#364554")));
  pen.setWidth(1);
  painter.setPen(pen);
  painter.drawPath(path);
}
