#include "import_step.h"
#include "modern_check_box.h"
#include "modern_radio_button.h"
#include "onboarding_manager.h"
#include "core/browser_icons.h"
#include "i18n/i18n.h"

#include <QComboBox>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QPushButton>
#include <QRadioButton>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

namespace dalinira::onboarding {

namespace {

QIcon installedBrowserIcon(const DetectedBrowser &browser) {
  if (!browser.iconName.isEmpty()) {
    if (QFileInfo(browser.iconName).isAbsolute() && QFile::exists(browser.iconName)) {
      return QIcon(browser.iconName);
    }

    QIcon icon = QIcon::fromTheme(browser.iconName);
    if (!icon.isNull()) return icon;

    QStringList iconRoots = {
        QStringLiteral("/usr/share/icons"),
        QStringLiteral("/usr/local/share/icons"),
        QDir::homePath() + QStringLiteral("/.local/share/icons")};
    if (!browser.desktopFilePath.isEmpty()) {
      iconRoots.prepend(QFileInfo(browser.desktopFilePath).dir().absoluteFilePath(QStringLiteral("../icons")));
    }

    QStringList filters;
    const QString suffix = QFileInfo(browser.iconName).suffix().toLower();
    if (suffix != QLatin1String("svg") && suffix != QLatin1String("png") &&
        suffix != QLatin1String("xpm")) {
      filters << browser.iconName + QStringLiteral(".svg")
              << browser.iconName + QStringLiteral(".png")
              << browser.iconName + QStringLiteral(".xpm");
    } else {
      filters << browser.iconName;
    }
    for (const QString &root : iconRoots) {
      if (!QDir(root).exists()) continue;
      QDirIterator it(root, filters, QDir::Files, QDirIterator::Subdirectories);
      if (it.hasNext()) return QIcon(it.next());
    }
  }

  return QIcon::fromTheme(browser.id == QLatin1String("html_file")
                              ? QStringLiteral("text-html")
                              : QStringLiteral("internet-web-browser"));
}

class BrowserItemDelegate final : public QStyledItemDelegate {
 public:
  explicit BrowserItemDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}

  QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override {
    Q_UNUSED(option);
    Q_UNUSED(index);
    return QSize(360, 52);
  }

  void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    const bool isSelected = (option.state & QStyle::State_Selected);
    const bool isHovered = (option.state & QStyle::State_MouseOver);
    const QString id = index.data(Qt::UserRole).toString();
    const QString title = index.data(Qt::DisplayRole).toString();

    // Background
    const QRectF itemRect = QRectF(option.rect).adjusted(4, 2, -4, -2);
    if (isSelected) {
      painter->setPen(QPen(QColor(16, 185, 129, 200), 1.2));
      painter->setBrush(QColor(16, 185, 129, 45));
      painter->drawRoundedRect(itemRect, 8.0, 8.0);
    } else if (isHovered) {
      painter->setPen(QPen(QColor(255, 255, 255, 30), 1.0));
      painter->setBrush(QColor(255, 255, 255, 18));
      painter->drawRoundedRect(itemRect, 8.0, 8.0);
    }

    // Icon / Emblem (26x26 rounded box with distinctive icon)
    const QRectF iconRect(itemRect.left() + 10.0, itemRect.top() + (itemRect.height() - 26.0) / 2.0, 26.0, 26.0);
    drawBrowserIcon(painter, iconRect, id, qvariant_cast<QIcon>(index.data(Qt::DecorationRole)));

    // Text rendering
    const qreal textX = iconRect.right() + 12.0;
    const qreal textW = itemRect.right() - textX - 30.0;
    const QRectF titleRect(textX, itemRect.top() + 7.0, textW, 20.0);
    const QRectF subRect(textX, titleRect.bottom() - 2.0, textW, 16.0);

    QFont fTitle = option.font;
    fTitle.setPixelSize(13);
    fTitle.setWeight(QFont::DemiBold);
    painter->setFont(fTitle);
    painter->setPen(isSelected ? QColor(255, 255, 255) : QColor(241, 245, 249));
    painter->drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter, title);

    QString subText = index.data(Qt::UserRole + 1).toString();
    if (subText.isEmpty()) {
      subText = id == QLatin1String("html_file")
          ? QStringLiteral("HTML / Netscape Bookmarks")
          : title;
    }

    QFont fSub = option.font;
    fSub.setPixelSize(11);
    fSub.setWeight(QFont::Normal);
    painter->setFont(fSub);
    painter->setPen(isSelected ? QColor(52, 211, 153) : QColor(148, 163, 184));
    painter->drawText(subRect, Qt::AlignLeft | Qt::AlignVCenter, subText);

    // If selected, draw check mark on the right
    if (isSelected) {
      QPainterPath checkPath;
      const qreal cx = itemRect.right() - 20.0;
      const qreal cy = itemRect.top() + itemRect.height() / 2.0;
      checkPath.moveTo(cx - 5.0, cy - 1.0);
      checkPath.lineTo(cx - 2.0, cy + 3.0);
      checkPath.lineTo(cx + 4.0, cy - 4.0);
      painter->setPen(QPen(QColor(16, 185, 129), 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
      painter->drawPath(checkPath);
    }

    painter->restore();
  }

 public:
  static void drawBrowserIcon(QPainter *painter, const QRectF &r, const QString &id,
                              const QIcon &installedIcon = QIcon()) {
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    if (!installedIcon.isNull()) {
      installedIcon.paint(painter, r.toAlignedRect(), Qt::AlignCenter, QIcon::Normal, QIcon::Off);
      painter->restore();
      return;
    }

    const QIcon fallbackIcon = QIcon::fromTheme(
        id == QLatin1String("html_file") ? QStringLiteral("text-html")
                                          : QStringLiteral("internet-web-browser"));
    if (!fallbackIcon.isNull()) {
      fallbackIcon.paint(painter, r.toAlignedRect(), Qt::AlignCenter, QIcon::Normal, QIcon::Off);
    } else {
      // Neutral placeholder only; never imitate a browser's protected logo.
      painter->setPen(QPen(QColor(148, 163, 184), 1.0));
      painter->setBrush(QColor(51, 65, 85, 230));
      painter->drawRoundedRect(r, 6, 6);
    }

    painter->restore();
  }
};

class ModernBrowserComboBox final : public QComboBox {
 public:
  explicit ModernBrowserComboBox(QWidget *parent = nullptr) : QComboBox(parent) {
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_Hover, true);
    setFocusPolicy(Qt::StrongFocus);
  }

  QSize sizeHint() const override {
    return QSize(360, 44);
  }

  QSize minimumSizeHint() const override {
    return QSize(340, 44);
  }

 protected:
  void enterEvent(QEnterEvent *event) override {
    isHovered_ = true;
    update();
    QComboBox::enterEvent(event);
  }

  void leaveEvent(QEvent *event) override {
    isHovered_ = false;
    update();
    QComboBox::leaveEvent(event);
  }

  void paintEvent(QPaintEvent *event) override {
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const QRectF r = rect().adjusted(1, 1, -1, -1);
    const bool isOpen = (view() && view()->isVisible());
    const bool isHover = isHovered_ || hasFocus();

    // Background pill container
    const QColor borderColor = (isOpen || isHover) ? QColor(16, 185, 129) : QColor(255, 255, 255, 45);
    const QColor bgColor = isOpen ? QColor(15, 23, 42, 250)
                                 : (isHover ? QColor(30, 41, 59, 245) : QColor(24, 32, 47, 230));

    p.setPen(QPen(borderColor, isOpen ? 1.8 : 1.4));
    p.setBrush(bgColor);
    p.drawRoundedRect(r, 10.0, 10.0);

    // Selected browser emblem (24x24)
    const QString id = currentData().toString();
    const QRectF iconRect(r.left() + 14.0, r.center().y() - 12.0, 24.0, 24.0);
    BrowserItemDelegate::drawBrowserIcon(&p, iconRect, id, itemIcon(currentIndex()));

    // Selected browser text
    const QString title = currentText();
    QFont f = font();
    f.setPixelSize(13);
    f.setWeight(QFont::DemiBold);
    p.setFont(f);
    p.setPen(QColor(255, 255, 255));
    const QRectF textRect(iconRect.right() + 12.0, r.top(), r.width() - iconRect.right() - 48.0, r.height());
    p.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, title);

    // Filled popup indicator (down when closed, up when open).  A filled
    // triangle stays legible at this compact size; the previous shallow
    // chevron could look like a minus sign on scaled displays.
    const qreal arrowX = r.right() - 20.0;
    const qreal arrowY = r.center().y();
    QPolygonF arrow;
    if (isOpen) {
      arrow << QPointF(arrowX - 5.0, arrowY + 3.0)
            << QPointF(arrowX, arrowY - 3.0)
            << QPointF(arrowX + 5.0, arrowY + 3.0);
    } else {
      arrow << QPointF(arrowX - 5.0, arrowY - 3.0)
            << QPointF(arrowX, arrowY + 3.0)
            << QPointF(arrowX + 5.0, arrowY - 3.0);
    }
    p.setPen(Qt::NoPen);
    p.setBrush((isOpen || isHover) ? QColor(16, 185, 129) : QColor(203, 213, 225));
    p.drawPolygon(arrow);
  }

 private:
  bool isHovered_ = false;
};

class ClickableCardFilter : public QObject {
 public:
  explicit ClickableCardFilter(std::function<void()> onClick, QObject *parent = nullptr)
      : QObject(parent), onClick_(std::move(onClick)) {}

 protected:
  bool eventFilter(QObject *obj, QEvent *event) override {
    if (event->type() == QEvent::MouseButtonPress) {
      if (onClick_) onClick_();
    }
    return QObject::eventFilter(obj, event);
  }

 private:
  std::function<void()> onClick_;
};

}  // namespace

ImportStep::ImportStep(IBrowserDataReceiver *dataReceiver, QWidget *parent)
    : OnboardingStep(parent), dataReceiver_(dataReceiver) {
  setupUi();
}

QString ImportStep::stepTitle() const {
  return dalinira::i18n::I18n::text(QStringLiteral("onboarding.import_title"));
}

QString ImportStep::stepSubtitle() const {
  return dalinira::i18n::I18n::text(QStringLiteral("onboarding.import_subtitle"));
}

QString ImportStep::nextButtonText() const {
  return dalinira::i18n::I18n::text(QStringLiteral("onboarding.next"));
}

QString ImportStep::backButtonText() const {
  return dalinira::i18n::I18n::text(QStringLiteral("onboarding.back"));
}

QString ImportStep::skipButtonText() const {
  return dalinira::i18n::I18n::text(QStringLiteral("onboarding.skip"));
}

void ImportStep::populateBrowsers() {
  detectedBrowsers_ = BrowserDataImporter::detectBrowsers();
  browserSelector_->clear();

  const QString savedSource = OnboardingManager::instance().importSource();
  int selectIndex = 0;

  for (int i = 0; i < detectedBrowsers_.size(); ++i) {
    const auto &browser = detectedBrowsers_[i];
    browserSelector_->addItem(installedBrowserIcon(browser), browser.displayName, browser.id);
    const int itemIndex = browserSelector_->count() - 1;
    const QString profileDescription = browser.browserType == QLatin1String("html_file")
        ? QStringLiteral("HTML / Netscape Bookmarks")
        : (browser.profileName.isEmpty() ? browser.displayName
                                         : browser.profileName);
    browserSelector_->setItemData(itemIndex, profileDescription, Qt::UserRole + 1);
    if (!savedSource.isEmpty() && browser.id == savedSource) {
      selectIndex = i;
    }
  }

  if (browserSelector_->count() > 0) {
    browserSelector_->setCurrentIndex(selectIndex);
  }
}

void ImportStep::setupUi() {
  auto *outerLayout = new QVBoxLayout(this);
  outerLayout->setContentsMargins(24, 24, 24, 24);
  outerLayout->setAlignment(Qt::AlignCenter);

  auto *mainCard = new QFrame(this);
  mainCard->setObjectName(QStringLiteral("import-card-container"));
  mainCard->setMaximumWidth(800);
  mainCard->setStyleSheet(QStringLiteral(
      "#import-card-container {"
      "  background: rgba(15, 23, 42, 0.82);"
      "  border: 1px solid rgba(255, 255, 255, 0.18);"
      "  border-radius: 24px;"
      "}"));

  auto *cardLayout = new QVBoxLayout(mainCard);
  cardLayout->setContentsMargins(36, 16, 36, 16);
  cardLayout->setSpacing(8);

  // Icon emblem
  auto *iconContainer = new QFrame(mainCard);
  iconContainer->setFixedSize(56, 56);
  iconContainer->setStyleSheet(QStringLiteral(
      "background: qradialgradient(cx:0.5, cy:0.5, radius:0.5, fx:0.5, fy:0.5, stop:0 rgba(59, 130, 246, 0.3), stop:1 rgba(14, 165, 233, 0.1));"
      "border: 1px solid rgba(147, 197, 253, 0.4);"
      "border-radius: 16px;"));
  auto *iconLayout = new QVBoxLayout(iconContainer);
  iconLayout->setContentsMargins(0, 0, 0, 0);
  iconLayout->setAlignment(Qt::AlignCenter);

  auto *iconLabel = new QLabel(iconContainer);
  iconLabel->setPixmap(BrowserIcons::icon(BrowserIcon::Download).pixmap(32, 32));
  iconLabel->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
  iconLayout->addWidget(iconLabel);
  cardLayout->addWidget(iconContainer, 0, Qt::AlignHCenter);

  // Title
  titleLabel_ = new QLabel(stepTitle(), mainCard);
  titleLabel_->setObjectName(QStringLiteral("import-title"));
  titleLabel_->setStyleSheet(QStringLiteral(
      "font-size: 26px; font-weight: 800; color: #ffffff; background: transparent; border: none;"));
  titleLabel_->setAlignment(Qt::AlignCenter);
  cardLayout->addWidget(titleLabel_);

  // Subtitle
  subtitleLabel_ = new QLabel(stepSubtitle(), mainCard);
  subtitleLabel_->setObjectName(QStringLiteral("import-subtitle"));
  subtitleLabel_->setStyleSheet(QStringLiteral(
      "font-size: 14px; font-weight: 500; color: #f1f5f9; background: transparent; border: none;"));
  subtitleLabel_->setAlignment(Qt::AlignCenter);
  subtitleLabel_->setWordWrap(true);
  subtitleLabel_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  subtitleLabel_->setMinimumHeight(36);
  cardLayout->addWidget(subtitleLabel_);

  cardLayout->addSpacing(4);

  // Choice 1: Import Card
  importCard_ = new QFrame(mainCard);
  importCard_->setObjectName(QStringLiteral("choice-card-import"));
  importCard_->setCursor(Qt::PointingHandCursor);
  importCard_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  auto *importCardLayout = new QVBoxLayout(importCard_);
  importCardLayout->setContentsMargins(20, 12, 20, 12);
  importCardLayout->setSpacing(8);

  auto *importHeaderLayout = new QHBoxLayout();
  importHeaderLayout->setSpacing(14);
  importHeaderLayout->setAlignment(Qt::AlignVCenter);

  importRadio_ = new ModernRadioButton(importCard_);
  importRadio_->setObjectName(QStringLiteral("import-radio"));
  importHeaderLayout->addWidget(importRadio_, 0, Qt::AlignVCenter);

  auto *importTextLayout = new QVBoxLayout();
  importTextLayout->setSpacing(4);
  importTitle_ = new QLabel(dalinira::i18n::I18n::text(QStringLiteral("onboarding.import_from_browser")), importCard_);
  importTitle_->setObjectName(QStringLiteral("import-choice-title"));
  importTitle_->setStyleSheet(QStringLiteral("font-size: 15px; font-weight: 700; color: #ffffff; background: transparent; border: none; padding: 0px;"));
  importTitle_->setWordWrap(true);
  importTitle_->setMinimumHeight(20);
  importTitle_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

  importDesc_ = new QLabel(dalinira::i18n::I18n::text(QStringLiteral("onboarding.import_from_browser_desc")), importCard_);
  importDesc_->setObjectName(QStringLiteral("import-choice-desc"));
  importDesc_->setStyleSheet(QStringLiteral("font-size: 13px; font-weight: 400; color: #cbd5e1; background: transparent; border: none; padding: 0px;"));
  importDesc_->setWordWrap(true);
  importDesc_->setMinimumHeight(34);
  importDesc_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

  importTextLayout->addWidget(importTitle_);
  importTextLayout->addWidget(importDesc_);
  importHeaderLayout->addLayout(importTextLayout, 1);
  importCardLayout->addLayout(importHeaderLayout);

  // Expandable options in Import Card
  importOptionsContainer_ = new QWidget(importCard_);
  auto *optionsLayout = new QVBoxLayout(importOptionsContainer_);
  optionsLayout->setContentsMargins(36, 6, 16, 4);
  optionsLayout->setSpacing(8);

  // Browser dropdown row
  auto *sourceRow = new QHBoxLayout();
  sourceRow->setSpacing(14);
  sourceLabel_ = new QLabel(dalinira::i18n::I18n::text(QStringLiteral("onboarding.import_source_label")), importOptionsContainer_);
  sourceLabel_->setStyleSheet(QStringLiteral("font-size: 14px; font-weight: 600; color: #e2e8f0; background: transparent; border: none; padding: 0px;"));
  sourceLabel_->setMinimumHeight(24);
  sourceLabel_->setAlignment(Qt::AlignVCenter);

  browserSelector_ = new ModernBrowserComboBox(importOptionsContainer_);
  browserSelector_->setObjectName(QStringLiteral("import-browser-selector"));
  browserSelector_->setMinimumWidth(320);
  browserSelector_->setFixedHeight(46);
  browserSelector_->setMaxVisibleItems(8);

  auto *listView = new QListView(browserSelector_);
  listView->setItemDelegate(new BrowserItemDelegate(listView));
  listView->setSpacing(2);
  listView->setStyleSheet(QStringLiteral(
      "QListView {"
      "  background-color: #0f172a;"
      "  border: 1.5px solid rgba(16, 185, 129, 0.45);"
      "  border-radius: 12px;"
      "  padding: 6px;"
      "  outline: none;"
      "}"));
  browserSelector_->setView(listView);

  browserSelector_->setStyleSheet(QStringLiteral(
      "#import-browser-selector {"
      "  color: #ffffff;"
      "  font-size: 14px;"
      "  font-weight: 600;"
      "  border: none;"
      "  background: transparent;"
      "  padding: 0;"
      "}"));

  sourceRow->addWidget(sourceLabel_);
  sourceRow->addWidget(browserSelector_);
  sourceRow->addStretch();
  optionsLayout->addLayout(sourceRow);

  // HTML File row (if HTML file is chosen)
  htmlFileRow_ = new QWidget(importOptionsContainer_);
  auto *htmlLayout = new QHBoxLayout(htmlFileRow_);
  htmlLayout->setContentsMargins(0, 0, 0, 0);
  htmlLayout->setSpacing(8);
  htmlPathEdit_ = new QLineEdit(htmlFileRow_);
  htmlPathEdit_->setObjectName(QStringLiteral("import-html-path-edit"));
  htmlPathEdit_->setReadOnly(true);
  htmlPathEdit_->setPlaceholderText(QStringLiteral("bookmarks.html"));
  htmlPathEdit_->setStyleSheet(QStringLiteral(
      "background: #182030; color: #f1f3f4; border: 1px solid rgba(255, 255, 255, 0.12); border-radius: 8px; padding: 8px 12px; font-size: 13px;"));
  browseBtn_ = new QPushButton(dalinira::i18n::I18n::text(QStringLiteral("onboarding.import_html_browse")), htmlFileRow_);
  browseBtn_->setCursor(Qt::PointingHandCursor);
  browseBtn_->setStyleSheet(QStringLiteral(
      "background: rgba(16, 185, 129, 0.15); color: #34d399; font-weight: 600; border: 1px solid rgba(16, 185, 129, 0.35); border-radius: 8px; padding: 8px 16px; font-size: 13px;"));
  htmlLayout->addWidget(htmlPathEdit_, 1);
  htmlLayout->addWidget(browseBtn_);
  optionsLayout->addWidget(htmlFileRow_);
  htmlFileRow_->hide();

  // Modern Checkboxes for data types
  auto *checksLayout = new QHBoxLayout();
  checksLayout->setSpacing(28);
  checksLayout->setContentsMargins(0, 4, 0, 0);

  bookmarksCheck_ = new ModernCheckBox(dalinira::i18n::I18n::text(QStringLiteral("onboarding.import_bookmarks")), importOptionsContainer_);
  bookmarksCheck_->setObjectName(QStringLiteral("import-check-bookmarks"));
  bookmarksCheck_->setChecked(true);

  historyCheck_ = new ModernCheckBox(dalinira::i18n::I18n::text(QStringLiteral("onboarding.import_history")), importOptionsContainer_);
  historyCheck_->setObjectName(QStringLiteral("import-check-history"));
  historyCheck_->setChecked(true);

  checksLayout->addWidget(bookmarksCheck_);
  checksLayout->addWidget(historyCheck_);
  checksLayout->addStretch();
  optionsLayout->addLayout(checksLayout);

  // Status feedback label
  statusLabel_ = new QLabel(importOptionsContainer_);
  statusLabel_->setObjectName(QStringLiteral("import-status-label"));
  statusLabel_->setStyleSheet(QStringLiteral("color: #34d399; font-size: 13px; font-weight: 600; padding-top: 4px; background: transparent; border: none;"));
  optionsLayout->addWidget(statusLabel_);

  importCardLayout->addWidget(importOptionsContainer_);
  cardLayout->addWidget(importCard_);

  // Choice 2: Start Fresh Card
  freshCard_ = new QFrame(mainCard);
  freshCard_->setObjectName(QStringLiteral("choice-card-fresh"));
  freshCard_->setCursor(Qt::PointingHandCursor);
  auto *freshCardLayout = new QHBoxLayout(freshCard_);
  freshCardLayout->setContentsMargins(20, 12, 20, 12);
  freshCardLayout->setSpacing(14);
  freshCardLayout->setAlignment(Qt::AlignVCenter);

  freshRadio_ = new ModernRadioButton(freshCard_);
  freshRadio_->setObjectName(QStringLiteral("fresh-radio"));
  freshCardLayout->addWidget(freshRadio_, 0, Qt::AlignVCenter);

  auto *freshTextLayout = new QVBoxLayout();
  freshTextLayout->setSpacing(4);
  freshTitle_ = new QLabel(dalinira::i18n::I18n::text(QStringLiteral("onboarding.start_fresh")), freshCard_);
  freshTitle_->setObjectName(QStringLiteral("fresh-choice-title"));
  freshTitle_->setStyleSheet(QStringLiteral("font-size: 15px; font-weight: 700; color: #ffffff; background: transparent; border: none; padding: 0px;"));
  freshTitle_->setWordWrap(true);
  freshTitle_->setMinimumHeight(20);
  freshTitle_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

  freshDesc_ = new QLabel(dalinira::i18n::I18n::text(QStringLiteral("onboarding.start_fresh_desc")), freshCard_);
  freshDesc_->setObjectName(QStringLiteral("fresh-choice-desc"));
  freshDesc_->setStyleSheet(QStringLiteral("font-size: 13px; font-weight: 400; color: #cbd5e1; background: transparent; border: none; padding: 0px;"));
  freshDesc_->setWordWrap(true);
  freshDesc_->setMinimumHeight(34);
  freshDesc_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

  freshTextLayout->addWidget(freshTitle_);
  freshTextLayout->addWidget(freshDesc_);
  freshCardLayout->addLayout(freshTextLayout, 1);
  cardLayout->addWidget(freshCard_);

  outerLayout->addWidget(mainCard, 0, Qt::AlignCenter);

  // Populate detected browsers
  populateBrowsers();

  // Event filters for card click
  importCard_->installEventFilter(new ClickableCardFilter([this] { selectChoice(true); }, importCard_));
  freshCard_->installEventFilter(new ClickableCardFilter([this] { selectChoice(false); }, freshCard_));

  // toggled, not clicked: keyboard and assistive technologies check a radio
  // without a click. selectChoice() is idempotent.
  connect(importRadio_, &QRadioButton::toggled, this, [this](bool checked) { if (checked) selectChoice(true); });
  connect(freshRadio_, &QRadioButton::toggled, this, [this](bool checked) { if (checked) selectChoice(false); });

  // Browser combo box changes
  connect(browserSelector_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int idx) {
    if (idx >= 0 && idx < detectedBrowsers_.size()) {
      const auto &browser = detectedBrowsers_[idx];
      OnboardingManager::instance().setImportSource(browser.id);
      updateSourceOptionsVisibility();
    }
  });

  // Browse file button
  connect(browseBtn_, &QPushButton::clicked, this, [this] {
    const QString filePath = QFileDialog::getOpenFileName(
        this,
        dalinira::i18n::I18n::text(QStringLiteral("onboarding.import_html_browse")),
        QDir::homePath(),
        QStringLiteral("HTML Bookmarks (*.html *.htm);;All Files (*)"));
    if (!filePath.isEmpty()) {
      htmlPathEdit_->setText(filePath);
      OnboardingManager::instance().setImportHtmlPath(filePath);
      statusLabel_->setText(dalinira::i18n::I18n::text(QStringLiteral("onboarding.import_status_ready")));
    }
  });

  // Checkboxes persist state immediately
  connect(bookmarksCheck_, &QCheckBox::toggled, this, [](bool checked) {
    OnboardingManager::instance().setImportBookmarksEnabled(checked);
  });
  connect(historyCheck_, &QCheckBox::toggled, this, [](bool checked) {
    OnboardingManager::instance().setImportHistoryEnabled(checked);
  });
  // Passwords and security credentials deliberately belong to DaliNira's
  // separate password manager and are never imported during onboarding.
  OnboardingManager::instance().setImportPasswordsEnabled(false);

  // Initial selection
  const bool isImport = (OnboardingManager::instance().importChoice() == OnboardingManager::ImportChoice::Import);
  selectChoice(isImport);
  updateSourceOptionsVisibility();
}

void ImportStep::updateSourceOptionsVisibility() {
  const int index = browserSelector_ ? browserSelector_->currentIndex() : -1;
  if (index < 0 || index >= detectedBrowsers_.size()) return;
  const auto &browser = detectedBrowsers_.at(index);
  const bool isHtmlFile = browser.id == QLatin1String("html_file");
  htmlFileRow_->setVisible(isHtmlFile);
  bookmarksCheck_->setEnabled(browser.hasBookmarks || isHtmlFile);
  historyCheck_->setEnabled(browser.hasHistory);
}

void ImportStep::selectChoice(bool isImport) {
  importRadio_->setChecked(isImport);
  freshRadio_->setChecked(!isImport);
  importOptionsContainer_->setVisible(isImport);

  OnboardingManager::instance().setImportChoice(
      isImport ? OnboardingManager::ImportChoice::Import : OnboardingManager::ImportChoice::Fresh);

  refreshCardStyles();

  if (layout()) {
    layout()->activate();
  }
}

void ImportStep::refreshCardStyles() {
  const bool isImport = importRadio_->isChecked();

  const QString activeImportCardStyle = QStringLiteral(
      "#choice-card-import {"
      "  background: rgba(16, 185, 129, 0.08);"
      "  border: 2px solid #10b981;"
      "  border-radius: 16px;"
      "}");

  const QString inactiveImportCardStyle = QStringLiteral(
      "#choice-card-import {"
      "  background: rgba(255, 255, 255, 0.045);"
      "  border: 1px solid rgba(255, 255, 255, 0.14);"
      "  border-radius: 16px;"
      "}"
      "#choice-card-import:hover {"
      "  background: rgba(255, 255, 255, 0.075);"
      "  border: 1px solid rgba(255, 255, 255, 0.28);"
      "}");

  const QString activeFreshCardStyle = QStringLiteral(
      "#choice-card-fresh {"
      "  background: rgba(16, 185, 129, 0.08);"
      "  border: 2px solid #10b981;"
      "  border-radius: 16px;"
      "}");

  const QString inactiveFreshCardStyle = QStringLiteral(
      "#choice-card-fresh {"
      "  background: rgba(255, 255, 255, 0.045);"
      "  border: 1px solid rgba(255, 255, 255, 0.14);"
      "  border-radius: 16px;"
      "}"
      "#choice-card-fresh:hover {"
      "  background: rgba(255, 255, 255, 0.075);"
      "  border: 1px solid rgba(255, 255, 255, 0.28);"
      "}");

  importCard_->setStyleSheet(isImport ? activeImportCardStyle : inactiveImportCardStyle);
  freshCard_->setStyleSheet(!isImport ? activeFreshCardStyle : inactiveFreshCardStyle);

  importRadio_->update();
  freshRadio_->update();
}

void ImportStep::onStepEntering() {
  // Safe resume: restore exact choice & option settings
  const bool isImport = (OnboardingManager::instance().importChoice() == OnboardingManager::ImportChoice::Import);
  selectChoice(isImport);

  bookmarksCheck_->setChecked(OnboardingManager::instance().importBookmarksEnabled());
  historyCheck_->setChecked(OnboardingManager::instance().importHistoryEnabled());
  OnboardingManager::instance().setImportPasswordsEnabled(false);

  const QString savedPath = OnboardingManager::instance().importHtmlPath();
  if (!savedPath.isEmpty()) {
    htmlPathEdit_->setText(savedPath);
  }

  const QString savedSource = OnboardingManager::instance().importSource();
  for (int i = 0; i < detectedBrowsers_.size(); ++i) {
    if (detectedBrowsers_[i].id == savedSource) {
      browserSelector_->setCurrentIndex(i);
      break;
    }
  }
  updateSourceOptionsVisibility();

  statusLabel_->clear();
}

void ImportStep::onStepLeaving() {
  // Execute import if user selected import mode
  if (OnboardingManager::instance().importChoice() == OnboardingManager::ImportChoice::Import) {
    executeImport();
  }
}

int ImportStep::executeImport() {
  if (OnboardingManager::instance().importChoice() != OnboardingManager::ImportChoice::Import) {
    return 0;
  }

  int totalImported = 0;
  int bookmarkCount = 0;
  int historyCount = 0;

  const int idx = browserSelector_->currentIndex();
  if (idx < 0 || idx >= detectedBrowsers_.size()) {
    return 0;
  }
  const auto &browser = detectedBrowsers_[idx];

  // 1. Bookmarks
  if (bookmarksCheck_->isChecked()) {
    QStringList targetPaths = browser.bookmarksPaths;
    if (targetPaths.isEmpty() && !browser.bookmarksPath.isEmpty()) {
      targetPaths.append(browser.bookmarksPath);
    }
    if (browser.id == QLatin1String("html_file")) {
      targetPaths = {htmlPathEdit_->text()};
    }
    if (!targetPaths.isEmpty()) {
      bookmarkCount = BrowserDataImporter::importBookmarks(targetPaths, dataReceiver_);
      totalImported += bookmarkCount;
    }
  }

  // 2. History
  if (historyCheck_->isChecked() && browser.hasHistory && !browser.historyPath.isEmpty()) {
    historyCount = BrowserDataImporter::importHistoryFromDatabase(browser.historyPath, browser.browserType, dataReceiver_);
    totalImported += historyCount;
  }

  if (statusLabel_) {
    if (totalImported > 0) {
      statusLabel_->setText(dalinira::i18n::I18n::text(QStringLiteral("onboarding.import_summary"))
                                .arg(bookmarkCount)
                                .arg(historyCount));
    } else {
      statusLabel_->setText(dalinira::i18n::I18n::text(QStringLiteral("onboarding.import_status_done")));
    }
  }

  return totalImported;
}

void ImportStep::retranslateUi() {
  if (titleLabel_) titleLabel_->setText(stepTitle());
  if (subtitleLabel_) subtitleLabel_->setText(stepSubtitle());
  if (importTitle_) importTitle_->setText(dalinira::i18n::I18n::text(QStringLiteral("onboarding.import_from_browser")));
  if (importDesc_) importDesc_->setText(dalinira::i18n::I18n::text(QStringLiteral("onboarding.import_from_browser_desc")));
  if (freshTitle_) freshTitle_->setText(dalinira::i18n::I18n::text(QStringLiteral("onboarding.start_fresh")));
  if (freshDesc_) freshDesc_->setText(dalinira::i18n::I18n::text(QStringLiteral("onboarding.start_fresh_desc")));
  if (sourceLabel_) sourceLabel_->setText(dalinira::i18n::I18n::text(QStringLiteral("onboarding.import_source_label")));
  if (browseBtn_) browseBtn_->setText(dalinira::i18n::I18n::text(QStringLiteral("onboarding.import_html_browse")));
  if (bookmarksCheck_) bookmarksCheck_->setText(dalinira::i18n::I18n::text(QStringLiteral("onboarding.import_bookmarks")));
  if (historyCheck_) historyCheck_->setText(dalinira::i18n::I18n::text(QStringLiteral("onboarding.import_history")));
}

}  // namespace dalinira::onboarding
