#include "ready_step.h"

#include "core/browser_icons.h"
#include "core/search_engine_definition.h"
#include "i18n/i18n.h"
#include "i18n/language_manager.h"
#include "onboarding_manager.h"
#include "search_privacy_step.h"

#include <QDateTime>
#include <QFile>
#include <QGraphicsOpacityEffect>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QSettings>
#include <QSvgRenderer>
#include <QVBoxLayout>

using dalinira::i18n::I18n;

namespace dalinira::onboarding {

namespace {

QPixmap loadSvgPixmap(const QString &relativeName, int size, const QColor &tint) {
  // The icons are compiled into the application's resources.
  const QStringList candidates = {QStringLiteral(":/browser-icons/") + relativeName};
  for (const auto &path : candidates) {
    if (QFile::exists(path)) {
      QSvgRenderer renderer(path);
      if (renderer.isValid()) {
        QPixmap pm(size, size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        renderer.render(&p, QRectF(0, 0, size, size));
        if (tint.isValid()) {
          p.setCompositionMode(QPainter::CompositionMode_SourceIn);
          p.fillRect(pm.rect(), tint);
        }
        return pm;
      }
    }
  }

  // Pure QPainter vector fallback ensuring icons never fail
  QPixmap pm(size, size);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing);
  p.setPen(QPen(tint.isValid() ? tint : QColor(QStringLiteral("#38bdf8")),
                2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  p.setBrush(Qt::NoBrush);

  if (relativeName == QLatin1String("privacy.svg")) {
    QPainterPath shield;
    shield.moveTo(size * 0.5, size * 0.12);
    shield.lineTo(size * 0.85, size * 0.25);
    shield.lineTo(size * 0.85, size * 0.55);
    shield.quadTo(size * 0.85, size * 0.85, size * 0.5, size * 0.95);
    shield.quadTo(size * 0.15, size * 0.85, size * 0.15, size * 0.55);
    shield.lineTo(size * 0.15, size * 0.25);
    shield.closeSubpath();
    p.drawPath(shield);
    QPainterPath check;
    check.moveTo(size * 0.35, size * 0.52);
    check.lineTo(size * 0.46, size * 0.63);
    check.lineTo(size * 0.68, size * 0.40);
    p.drawPath(check);
  } else if (relativeName == QLatin1String("window.svg") || relativeName == QLatin1String("appearance.svg")) {
    p.drawRoundedRect(QRectF(size * 0.15, size * 0.18, size * 0.70, size * 0.64), 3, 3);
    p.drawLine(QPointF(size * 0.15, size * 0.36), QPointF(size * 0.85, size * 0.36));
    p.drawLine(QPointF(size * 0.45, size * 0.36), QPointF(size * 0.45, size * 0.82));
  } else if (relativeName == QLatin1String("search.svg")) {
    p.drawEllipse(QPointF(size * 0.42, size * 0.42), size * 0.26, size * 0.26);
    p.drawLine(QPointF(size * 0.62, size * 0.62), QPointF(size * 0.85, size * 0.85));
  } else if (relativeName == QLatin1String("chromium.svg") || relativeName == QLatin1String("performance.svg")) {
    p.drawEllipse(QPointF(size * 0.5, size * 0.5), size * 0.35, size * 0.35);
    p.drawEllipse(QPointF(size * 0.5, size * 0.5), size * 0.15, size * 0.15);
  } else if (relativeName == QLatin1String("webkit.svg")) {
    p.drawEllipse(QPointF(size * 0.5, size * 0.5), size * 0.36, size * 0.36);
    p.drawLine(QPointF(size * 0.5, size * 0.15), QPointF(size * 0.5, size * 0.85));
    p.drawLine(QPointF(size * 0.15, size * 0.5), QPointF(size * 0.85, size * 0.5));
  }
  return pm;
}

QWidget *summaryCard(const QString &title, QLabel **headingOut, QLabel **valueOut, QLabel **iconOut,
                     const QString &badgeBg, const QString &badgeBorder, QWidget *parent) {
  auto *card = new QWidget(parent);
  card->setObjectName(QStringLiteral("readySummaryCard"));
  auto *layout = new QHBoxLayout(card);
  layout->setContentsMargins(18, 14, 18, 14);
  layout->setSpacing(14);
  layout->setAlignment(Qt::AlignVCenter);

  // Icon Badge Container (48x48)
  auto *badge = new QFrame(card);
  badge->setObjectName(QStringLiteral("readySummaryBadge"));
  badge->setFixedSize(48, 48);
  badge->setStyleSheet(QStringLiteral(
      "background: %1; border: 1px solid %2; border-radius: 12px;").arg(badgeBg, badgeBorder));

  auto *badgeLayout = new QVBoxLayout(badge);
  badgeLayout->setContentsMargins(0, 0, 0, 0);
  badgeLayout->setAlignment(Qt::AlignCenter);

  *iconOut = new QLabel(badge);
  (*iconOut)->setObjectName(QStringLiteral("readySummaryIcon"));
  (*iconOut)->setFixedSize(28, 28);
  (*iconOut)->setAlignment(Qt::AlignCenter);
  (*iconOut)->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
  badgeLayout->addWidget(*iconOut, 0, Qt::AlignCenter);

  layout->addWidget(badge, 0, Qt::AlignVCenter);

  // Text container (Heading + Value)
  auto *textLayout = new QVBoxLayout();
  textLayout->setContentsMargins(0, 0, 0, 0);
  textLayout->setSpacing(4);
  textLayout->setAlignment(Qt::AlignVCenter);

  *headingOut = new QLabel(title, card);
  (*headingOut)->setObjectName(QStringLiteral("readySummaryHeading"));
  (*headingOut)->setAlignment(Qt::AlignLeading | Qt::AlignVCenter);

  *valueOut = new QLabel(card);
  (*valueOut)->setObjectName(QStringLiteral("readySummaryValue"));
  (*valueOut)->setWordWrap(true);
  (*valueOut)->setAlignment(Qt::AlignLeading | Qt::AlignVCenter);

  textLayout->addWidget(*headingOut);
  textLayout->addWidget(*valueOut);

  layout->addLayout(textLayout, 1);
  return card;
}

QString layoutName(const QString &layout) {
  if (layout == QLatin1String("left_panel"))
    return I18n::text(QStringLiteral("onboarding.layout_left_name"));
  if (layout == QLatin1String("right_panel"))
    return I18n::text(QStringLiteral("onboarding.layout_right_name"));
  if (layout == QLatin1String("floating") || layout == QLatin1String("floating_panel"))
    return I18n::text(QStringLiteral("onboarding.layout_floating_name"));
  return I18n::text(QStringLiteral("onboarding.layout_classic_name"));
}

QString themeName(const QString &theme) {
  if (theme == QLatin1String("light"))
    return I18n::text(QStringLiteral("settings.appearance.theme_light"));
  if (theme == QLatin1String("dark"))
    return I18n::text(QStringLiteral("settings.appearance.theme_dark"));
  return I18n::text(QStringLiteral("settings.appearance.theme_system"));
}

}  // namespace

ReadyStep::ReadyStep(ISearchPrivacyPreferences *searchPrivacyPreferences,
                     dalinira::core::IDefaultBrowserIntegration *defaultBrowserIntegration,
                     QWidget *parent)
    : OnboardingStep(parent),
      searchPrivacyPreferences_(searchPrivacyPreferences),
      defaultBrowserIntegration_(defaultBrowserIntegration) {
  auto *root = new QVBoxLayout(this);
  root->setContentsMargins(64, 30, 64, 30);
  root->setSpacing(18);

  auto *brand = new QLabel(QStringLiteral("DaliNira"), this);
  brand->setObjectName(QStringLiteral("readyBrand"));
  brand->setAlignment(Qt::AlignCenter);
  root->addWidget(brand);

  titleLabel_ = new QLabel(stepTitle(), this);
  titleLabel_->setObjectName(QStringLiteral("readyTitle"));
  titleLabel_->setAlignment(Qt::AlignCenter);
  root->addWidget(titleLabel_);

  subtitleLabel_ = new QLabel(stepSubtitle(), this);
  subtitleLabel_->setObjectName(QStringLiteral("readySubtitle"));
  subtitleLabel_->setAlignment(Qt::AlignCenter);
  subtitleLabel_->setWordWrap(true);
  root->addWidget(subtitleLabel_);

  summaryGrid_ = new QWidget(this);
  auto *grid = new QGridLayout(summaryGrid_);
  grid->setContentsMargins(0, 4, 0, 4);
  grid->setSpacing(12);

  grid->addWidget(summaryCard(I18n::text(QStringLiteral("onboarding.ready.engine")),
                              &engineHeading_, &engineValue_, &engineIconLabel_,
                              QStringLiteral("rgba(56, 189, 248, 0.16)"),
                              QStringLiteral("rgba(56, 189, 248, 0.38)"),
                              summaryGrid_), 0, 0);
  grid->addWidget(summaryCard(I18n::text(QStringLiteral("onboarding.ready.appearance")),
                              &layoutThemeHeading_, &layoutThemeValue_, &layoutThemeIconLabel_,
                              QStringLiteral("rgba(168, 85, 247, 0.16)"),
                              QStringLiteral("rgba(168, 85, 247, 0.38)"),
                              summaryGrid_), 0, 1);
  grid->addWidget(summaryCard(I18n::text(QStringLiteral("onboarding.ready.search")),
                              &searchHeading_, &searchValue_, &searchIconLabel_,
                              QStringLiteral("rgba(245, 158, 11, 0.16)"),
                              QStringLiteral("rgba(245, 158, 11, 0.38)"),
                              summaryGrid_), 1, 0);
  grid->addWidget(summaryCard(I18n::text(QStringLiteral("onboarding.ready.privacy")),
                              &privacyHeading_, &privacyValue_, &privacyIconLabel_,
                              QStringLiteral("rgba(16, 185, 129, 0.16)"),
                              QStringLiteral("rgba(16, 185, 129, 0.38)"),
                              summaryGrid_), 1, 1);
  root->addWidget(summaryGrid_);

  statusLabel_ = new QLabel(this);
  statusLabel_->setObjectName(QStringLiteral("readyStatus"));
  statusLabel_->setAlignment(Qt::AlignCenter);
  statusLabel_->hide();
  root->addWidget(statusLabel_);

  auto *actions = new QHBoxLayout;
  actions->addStretch(1);
  makeDefaultButton_ = new QPushButton(
      I18n::text(QStringLiteral("onboarding.ready.make_default")), this);
  makeDefaultButton_->setObjectName(QStringLiteral("onboarding-make-default"));
  notNowButton_ = new QPushButton(
      I18n::text(QStringLiteral("onboarding.ready.not_now")), this);
  notNowButton_->setObjectName(QStringLiteral("onboarding-default-not-now"));
  actions->addWidget(makeDefaultButton_);
  actions->addWidget(notNowButton_);
  actions->addStretch(1);
  root->addLayout(actions);
  root->addStretch(1);

  connect(makeDefaultButton_, &QPushButton::clicked, this, &ReadyStep::chooseMakeDefault);
  connect(notNowButton_, &QPushButton::clicked, this, &ReadyStep::chooseNotNow);
  connect(&dalinira::i18n::LanguageManager::instance(), &dalinira::i18n::LanguageManager::languageChanged,
          this, &ReadyStep::retranslateUi);

  setStyleSheet(QStringLiteral(
      "#readyBrand { color:#93c5fd; font-size:18px; font-weight:800; letter-spacing:2px; }"
      "#readyTitle { color:#ffffff; font-size:32px; font-weight:800; }"
      "#readySubtitle { color:#f1f5f9; font-size:15px; font-weight:500; }"
      "#readySummaryCard { background:rgba(255, 255, 255, 0.05); border:1px solid rgba(255, 255, 255, 0.14); border-radius:14px; }"
      "#readySummaryCard:hover { background:rgba(255, 255, 255, 0.08); border-color:rgba(255, 255, 255, 0.24); }"
      "#readySummaryHeading { color:#94a3b8; font-size:11.5px; font-weight:650; }"
      "#readySummaryValue { color:#ffffff; font-size:16px; font-weight:700; }"
      "#readyStatus { color:#93c5fd; font-size:13px; }"
      "#onboarding-make-default { background:qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #2563eb, stop:1 #06b6d4); color:#ffffff; border:0; border-radius:10px; padding:10px 22px; font-weight:700; }"
      "#onboarding-default-not-now { background:rgba(255, 255, 255, 0.08); color:#f1f5f9; border:1px solid rgba(255, 255, 255, 0.15); border-radius:10px; padding:10px 22px; font-weight:600; }"));
  refreshSummary();
}

QString ReadyStep::stepTitle() const {
  return I18n::text(QStringLiteral("onboarding.ready.title"));
}

QString ReadyStep::stepSubtitle() const {
  return I18n::text(QStringLiteral("onboarding.ready.subtitle"));
}

void ReadyStep::retranslateUi() {
  if (titleLabel_) titleLabel_->setText(stepTitle());
  if (subtitleLabel_) subtitleLabel_->setText(stepSubtitle());
  if (engineHeading_) engineHeading_->setText(I18n::text(QStringLiteral("onboarding.ready.engine")));
  if (layoutThemeHeading_) layoutThemeHeading_->setText(I18n::text(QStringLiteral("onboarding.ready.appearance")));
  if (searchHeading_) searchHeading_->setText(I18n::text(QStringLiteral("onboarding.ready.search")));
  if (privacyHeading_) privacyHeading_->setText(I18n::text(QStringLiteral("onboarding.ready.privacy")));
  if (makeDefaultButton_) makeDefaultButton_->setText(I18n::text(QStringLiteral("onboarding.ready.make_default")));
  if (notNowButton_) notNowButton_->setText(I18n::text(QStringLiteral("onboarding.ready.not_now")));
  refreshSummary();
}

void ReadyStep::onStepEntering() {
  finishing_ = false;
  makeDefaultButton_->setEnabled(true);
  notNowButton_->setEnabled(true);
  statusLabel_->hide();
  retranslateUi();
  if (!OnboardingManager::animationsEnabled()) return;
  auto *effect = new QGraphicsOpacityEffect(summaryGrid_);
  summaryGrid_->setGraphicsEffect(effect);
  auto *animation = new QPropertyAnimation(effect, "opacity", summaryGrid_);
  animation->setDuration(220);
  animation->setStartValue(0.25);
  animation->setEndValue(1.0);
  connect(animation, &QPropertyAnimation::finished, summaryGrid_, [this, animation] {
    summaryGrid_->setGraphicsEffect(nullptr);
    animation->deleteLater();
  });
  animation->start();
}

void ReadyStep::refreshSummary() {
  const auto &manager = OnboardingManager::instance();
  const bool isWebKit = (manager.selectedEngine() == QLatin1String("webkit"));
  engineSummary_ = isWebKit ? QStringLiteral("WebKit") : QStringLiteral("Chromium");
  layoutThemeSummary_ = QStringLiteral("%1 · %2")
      .arg(layoutName(manager.selectedLayout()), themeName(manager.selectedTheme()));
  searchSummary_ = searchPrivacyPreferences_
      ? searchPrivacyPreferences_->searchEngine() : QStringLiteral("DuckDuckGo");
  privacySummary_ = I18n::text(searchPrivacyPreferences_ && !searchPrivacyPreferences_->blockerEnabled()
      ? QStringLiteral("onboarding.search_privacy.blocker_off")
      : QStringLiteral("onboarding.search_privacy.blocker_on"));

  if (engineValue_) engineValue_->setText(engineSummary_);
  if (layoutThemeValue_) layoutThemeValue_->setText(layoutThemeSummary_);
  if (searchValue_) searchValue_->setText(searchSummary_);
  if (privacyValue_) privacyValue_->setText(privacySummary_);

  // Update Icons
  if (engineIconLabel_) {
    QPixmap p;
    if (isWebKit) {
      p = loadSvgPixmap(QStringLiteral("webkit.svg"), 28, QColor());
      if (p.isNull()) p = BrowserIcons::webkitIcon().pixmap(28, 28);
      if (p.isNull()) p = loadSvgPixmap(QStringLiteral("performance.svg"), 28, QColor(QStringLiteral("#38bdf8")));
    } else {
      p = loadSvgPixmap(QStringLiteral("chromium.svg"), 28, QColor());
      if (p.isNull()) p = BrowserIcons::chromiumIcon().pixmap(28, 28);
      if (p.isNull()) p = loadSvgPixmap(QStringLiteral("performance.svg"), 28, QColor(QStringLiteral("#38bdf8")));
    }
    engineIconLabel_->setPixmap(p);
  }

  if (layoutThemeIconLabel_) {
    QPixmap p = loadSvgPixmap(QStringLiteral("window.svg"), 28, QColor(QStringLiteral("#c084fc")));
    if (p.isNull()) p = loadSvgPixmap(QStringLiteral("appearance.svg"), 28, QColor(QStringLiteral("#c084fc")));
    if (p.isNull()) p = BrowserIcons::icon(BrowserIcon::Appearance).pixmap(28, 28);
    layoutThemeIconLabel_->setPixmap(p);
  }

  if (searchIconLabel_) {
    const QString resPath = dalinira::core::searchEngineResourcePath(searchSummary_);
    QPixmap p;
    if (QFile::exists(resPath)) {
      QIcon engIcon(resPath);
      if (!engIcon.isNull()) p = engIcon.pixmap(28, 28);
    }
    if (p.isNull()) p = loadSvgPixmap(QStringLiteral("search.svg"), 28, QColor(QStringLiteral("#fbbf24")));
    searchIconLabel_->setPixmap(p);
  }

  if (privacyIconLabel_) {
    QPixmap p = loadSvgPixmap(QStringLiteral("privacy.svg"), 28, QColor(QStringLiteral("#34d399")));
    if (p.isNull()) p = BrowserIcons::icon(BrowserIcon::Privacy).pixmap(28, 28);
    privacyIconLabel_->setPixmap(p);
  }
}

void ReadyStep::chooseMakeDefault() {
  if (finishing_) return;
  finishing_ = true;
  makeDefaultButton_->setEnabled(false);
  notNowButton_->setEnabled(false);
  statusLabel_->setText(I18n::text(QStringLiteral("onboarding.ready.default_working")));
  statusLabel_->show();
  if (!defaultBrowserIntegration_) {
    finishWithResult(dalinira::core::DefaultBrowserResult::Unsupported);
    return;
  }
  QPointer<ReadyStep> guard(this);
  defaultBrowserIntegration_->requestDefaultBrowser([guard](dalinira::core::DefaultBrowserResult result) {
    if (guard) guard->finishWithResult(result);
  });
}

void ReadyStep::chooseNotNow() {
  if (finishing_) return;
  finishing_ = true;
  QSettings().setValue(QString::fromLatin1(dalinira::core::kDefaultBrowserLastPromptUtcKey),
                       QDateTime::currentDateTimeUtc());
  emit stepCompleted();
}

void ReadyStep::finishWithResult(dalinira::core::DefaultBrowserResult result) {
  if (result == dalinira::core::DefaultBrowserResult::Success
      || result == dalinira::core::DefaultBrowserResult::AlreadyDefault) {
    QSettings settings;
    settings.remove(QString::fromLatin1(dalinira::core::kDefaultBrowserPromptDisabledKey));
    settings.remove(QString::fromLatin1(dalinira::core::kDefaultBrowserLastPromptUtcKey));
    statusLabel_->setText(I18n::text(QStringLiteral("onboarding.ready.default_done")));
  } else if (result == dalinira::core::DefaultBrowserResult::Unsupported) {
    QSettings().setValue(QString::fromLatin1(dalinira::core::kDefaultBrowserLastPromptUtcKey),
                         QDateTime::currentDateTimeUtc());
    statusLabel_->setText(I18n::text(QStringLiteral("onboarding.ready.default_unsupported")));
  } else {
    QSettings().setValue(QString::fromLatin1(dalinira::core::kDefaultBrowserLastPromptUtcKey),
                         QDateTime::currentDateTimeUtc());
    statusLabel_->setText(I18n::text(QStringLiteral("onboarding.ready.default_failed")));
  }
  emit defaultBrowserFinished(result);
  emit stepCompleted();
}

}  // namespace dalinira::onboarding
