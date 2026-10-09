#include "welcome_step.h"
#include "core/browser_icons.h"
#include "i18n/i18n.h"
#include "i18n/language_manager.h"

#include <algorithm>

#include <QComboBox>
#include <QResizeEvent>
#include <QFile>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSvgRenderer>
#include <QTimer>
#include <QVBoxLayout>

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
                1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
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
  } else if (relativeName == QLatin1String("tune.svg")) {
    p.drawLine(QPointF(size * 0.2, size * 0.35), QPointF(size * 0.8, size * 0.35));
    p.drawLine(QPointF(size * 0.2, size * 0.65), QPointF(size * 0.8, size * 0.65));
    p.setBrush(tint);
    p.drawEllipse(QPointF(size * 0.4, size * 0.35), size * 0.1, size * 0.1);
    p.drawEllipse(QPointF(size * 0.65, size * 0.65), size * 0.1, size * 0.1);
  } else if (relativeName == QLatin1String("music.svg")) {
    p.setBrush(tint);
    p.drawEllipse(QPointF(size * 0.35, size * 0.75), size * 0.14, size * 0.12);
    p.drawEllipse(QPointF(size * 0.72, size * 0.65), size * 0.14, size * 0.12);
    p.drawLine(QPointF(size * 0.45, size * 0.75), QPointF(size * 0.45, size * 0.25));
    p.drawLine(QPointF(size * 0.82, size * 0.65), QPointF(size * 0.82, size * 0.15));
    p.drawLine(QPointF(size * 0.45, size * 0.25), QPointF(size * 0.82, size * 0.15));
  } else if (relativeName == QLatin1String("download.svg")) {
    p.drawLine(QPointF(size * 0.5, size * 0.15), QPointF(size * 0.5, size * 0.65));
    p.drawLine(QPointF(size * 0.3, size * 0.45), QPointF(size * 0.5, size * 0.65));
    p.drawLine(QPointF(size * 0.7, size * 0.45), QPointF(size * 0.5, size * 0.65));
    p.drawLine(QPointF(size * 0.2, size * 0.82), QPointF(size * 0.8, size * 0.82));
  }
  return pm;
}

}  // namespace

WelcomeStep::WelcomeStep(QWidget *parent) : OnboardingStep(parent) {
  setupUi();
  connect(&dalinira::i18n::LanguageManager::instance(), &dalinira::i18n::LanguageManager::languageChanged,
          this, &WelcomeStep::retranslateUi);
}

QString WelcomeStep::stepTitle() const {
  return dalinira::i18n::I18n::text(QStringLiteral("onboarding.welcome_title"));
}

QString WelcomeStep::stepSubtitle() const {
  return dalinira::i18n::I18n::text(QStringLiteral("onboarding.welcome_subtitle"));
}

QString WelcomeStep::nextButtonText() const {
  return dalinira::i18n::I18n::text(QStringLiteral("onboarding.get_started"));
}

QString WelcomeStep::skipButtonText() const {
  return dalinira::i18n::I18n::text(QStringLiteral("onboarding.skip"));
}

QWidget *WelcomeStep::createDualEngineCard(const QString &title, const QString &description,
                                          FeatureCardWidgets &outWidgets) {
  auto *card = new QFrame(this);
  card->setObjectName(QStringLiteral("onboarding-feature-card"));
  card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  card->setMinimumHeight(154);
  outWidgets.card = card;
  card->setStyleSheet(QStringLiteral(
      "#onboarding-feature-card {"
      "  background: rgba(255, 255, 255, 0.055);"
      "  border: 1px solid rgba(255, 255, 255, 0.14);"
      "  border-radius: 14px;"
      "}"
      "#onboarding-feature-card:hover {"
      "  background: rgba(59, 130, 246, 0.12);"
      "  border: 1px solid rgba(147, 197, 253, 0.45);"
      "}"));

  auto *layout = new QVBoxLayout(card);
  layout->setContentsMargins(14, 14, 14, 14);
  layout->setSpacing(8);

  // Dual engine badge: WebKit logo + separator + Chromium logo
  auto *badge = new QFrame(card);
  badge->setFixedHeight(32);
  badge->setStyleSheet(QStringLiteral(
      "background: rgba(37, 99, 235, 0.15);"
      "border: 1px solid rgba(59, 130, 246, 0.35);"
      "border-radius: 8px;"));
  auto *badgeLayout = new QHBoxLayout(badge);
  badgeLayout->setContentsMargins(6, 2, 6, 2);
  badgeLayout->setSpacing(6);
  badgeLayout->setAlignment(Qt::AlignCenter);

  auto *wkIconLabel = new QLabel(badge);
  wkIconLabel->setPixmap(BrowserIcons::webkitIcon().pixmap(18, 18));
  wkIconLabel->setStyleSheet(QStringLiteral("background: transparent; border: none;"));

  auto *plusLabel = new QLabel(QStringLiteral("+"), badge);
  plusLabel->setStyleSheet(QStringLiteral("color: #60a5fa; font-size: 11px; font-weight: bold; background: transparent; border: none;"));

  auto *crIconLabel = new QLabel(badge);
  crIconLabel->setPixmap(BrowserIcons::chromiumIcon().pixmap(18, 18));
  crIconLabel->setStyleSheet(QStringLiteral("background: transparent; border: none;"));

  badgeLayout->addWidget(wkIconLabel);
  badgeLayout->addWidget(plusLabel);
  badgeLayout->addWidget(crIconLabel);

  layout->addWidget(badge, 0, Qt::AlignLeft);

  outWidgets.titleLabel = new QLabel(title, card);
  outWidgets.titleLabel->setStyleSheet(QStringLiteral(
      "font-size: 13.5px; font-weight: 700; color: #ffffff; background: transparent; border: none;"));
  outWidgets.titleLabel->setWordWrap(true);
  outWidgets.titleLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
  layout->addWidget(outWidgets.titleLabel);

  outWidgets.descLabel = new QLabel(description, card);
  outWidgets.descLabel->setStyleSheet(QStringLiteral(
      "font-size: 11.5px; font-weight: 400; color: #cbd5e1; background: transparent; border: none;"));
  outWidgets.descLabel->setWordWrap(true);
  outWidgets.descLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  layout->addWidget(outWidgets.descLabel);

  layout->addStretch(1);
  return card;
}

QWidget *WelcomeStep::createFeatureCard(const QString &title, const QString &description,
                                       const QString &svgName, const QColor &accentColor,
                                       FeatureCardWidgets &outWidgets) {
  auto *card = new QFrame(this);
  card->setObjectName(QStringLiteral("onboarding-feature-card"));
  card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  card->setMinimumHeight(154);
  outWidgets.card = card;
  card->setStyleSheet(QStringLiteral(
      "#onboarding-feature-card {"
      "  background: rgba(255, 255, 255, 0.055);"
      "  border: 1px solid rgba(255, 255, 255, 0.14);"
      "  border-radius: 14px;"
      "}"
      "#onboarding-feature-card:hover {"
      "  background: rgba(59, 130, 246, 0.12);"
      "  border: 1px solid rgba(147, 197, 253, 0.45);"
      "}"));

  auto *layout = new QVBoxLayout(card);
  layout->setContentsMargins(14, 14, 14, 14);
  layout->setSpacing(8);

  // Icon badge
  auto *badge = new QFrame(card);
  badge->setFixedSize(32, 32);
  badge->setStyleSheet(QStringLiteral(
      "background: rgba(%1, %2, %3, 0.16);"
      "border: 1px solid rgba(%1, %2, %3, 0.40);"
      "border-radius: 8px;")
      .arg(accentColor.red())
      .arg(accentColor.green())
      .arg(accentColor.blue()));
  auto *badgeLayout = new QVBoxLayout(badge);
  badgeLayout->setContentsMargins(0, 0, 0, 0);
  badgeLayout->setAlignment(Qt::AlignCenter);

  auto *iconLabel = new QLabel(badge);
  iconLabel->setPixmap(loadSvgPixmap(svgName, 18, accentColor));
  iconLabel->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
  badgeLayout->addWidget(iconLabel, 0, Qt::AlignCenter);

  layout->addWidget(badge, 0, Qt::AlignLeft);

  outWidgets.titleLabel = new QLabel(title, card);
  outWidgets.titleLabel->setStyleSheet(QStringLiteral(
      "font-size: 13.5px; font-weight: 700; color: #ffffff; background: transparent; border: none;"));
  outWidgets.titleLabel->setWordWrap(true);
  outWidgets.titleLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
  layout->addWidget(outWidgets.titleLabel);

  outWidgets.descLabel = new QLabel(description, card);
  outWidgets.descLabel->setStyleSheet(QStringLiteral(
      "font-size: 11.5px; font-weight: 400; color: #cbd5e1; background: transparent; border: none;"));
  outWidgets.descLabel->setWordWrap(true);
  outWidgets.descLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  layout->addWidget(outWidgets.descLabel);

  layout->addStretch(1);
  return card;
}

void WelcomeStep::fitFeatureCards() {
  const FeatureCardWidgets *cards[] = {&cardDualEngine_, &cardPrivacy_, &cardAudio_, &cardPulse_, &cardDownload_};
  int needed = 154;
  for (const FeatureCardWidgets *card : cards) {
    if (!card->card || !card->titleLabel || !card->descLabel || !card->card->layout()) continue;
    const QMargins margins = card->card->layout()->contentsMargins();
    const int textWidth = card->card->width() - margins.left() - margins.right();
    if (textWidth <= 0) continue;
    const int spacing = card->card->layout()->spacing();
    // Badge, title and description, with the layout's spacing between them.
    const int height = margins.top() + margins.bottom() + 32 + 2 * spacing +
                       card->titleLabel->heightForWidth(textWidth) +
                       card->descLabel->heightForWidth(textWidth);
    needed = std::max(needed, height);
  }
  for (const FeatureCardWidgets *card : cards)
    if (card->card && card->card->minimumHeight() != needed) card->card->setMinimumHeight(needed);
}

void WelcomeStep::resizeEvent(QResizeEvent *event) {
  OnboardingStep::resizeEvent(event);
  // The layout resizes the cards after this event; measure their final width.
  QTimer::singleShot(0, this, [this] { fitFeatureCards(); });
}

void WelcomeStep::setupUi() {
  auto *outerLayout = new QVBoxLayout(this);
  outerLayout->setContentsMargins(20, 16, 20, 16);
  outerLayout->setAlignment(Qt::AlignCenter);

  auto *mainCard = new QFrame(this);
  mainCard->setObjectName(QStringLiteral("welcome-card"));
  mainCard->setMinimumWidth(920);
  mainCard->setMaximumWidth(1040);
  mainCard->setStyleSheet(QStringLiteral(
      "#welcome-card {"
      "  background: rgba(15, 23, 42, 0.88);"
      "  border: 1px solid rgba(255, 255, 255, 0.18);"
      "  border-radius: 22px;"
      "}"));

  auto *cardLayout = new QVBoxLayout(mainCard);
  cardLayout->setContentsMargins(32, 24, 32, 24);
  cardLayout->setSpacing(14);

  // App Logo Emblem with Ambient Glow
  auto *iconContainer = new QFrame(mainCard);
  iconContainer->setFixedSize(76, 76);
  iconContainer->setStyleSheet(QStringLiteral(
      "background: qradialgradient(cx:0.5, cy:0.5, radius:0.5, fx:0.5, fy:0.5, stop:0 rgba(59, 130, 246, 0.3), stop:1 rgba(14, 165, 233, 0.1));"
      "border: 1px solid rgba(147, 197, 253, 0.4);"
      "border-radius: 20px;"));
  auto *iconLayout = new QVBoxLayout(iconContainer);
  iconLayout->setContentsMargins(0, 0, 0, 0);
  iconLayout->setAlignment(Qt::AlignCenter);

  auto *iconLabel = new QLabel(iconContainer);
  const QIcon icon = BrowserIcons::appIcon();
  if (!icon.isNull()) {
    iconLabel->setPixmap(icon.pixmap(48, 48));
  }
  iconLabel->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
  iconLayout->addWidget(iconLabel);
  cardLayout->addWidget(iconContainer, 0, Qt::AlignHCenter);

  // Title
  titleLabel_ = new QLabel(stepTitle(), mainCard);
  titleLabel_->setObjectName(QStringLiteral("welcome-title"));
  titleLabel_->setStyleSheet(QStringLiteral(
      "font-size: 26px; font-weight: 800; color: #ffffff; background: transparent; border: none;"));
  titleLabel_->setAlignment(Qt::AlignCenter);
  cardLayout->addWidget(titleLabel_);

  // Subtitle
  subtitleLabel_ = new QLabel(stepSubtitle(), mainCard);
  subtitleLabel_->setObjectName(QStringLiteral("welcome-subtitle"));
  subtitleLabel_->setStyleSheet(QStringLiteral(
      "font-size: 14px; font-weight: 500; color: #f1f5f9; background: transparent; border: none;"));
  subtitleLabel_->setAlignment(Qt::AlignCenter);
  subtitleLabel_->setWordWrap(true);
  subtitleLabel_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  subtitleLabel_->setMinimumHeight(48);
  cardLayout->addWidget(subtitleLabel_);

  cardLayout->addSpacing(4);

  // 5 Feature Highlights (Dual Engine, Privacy/Adblock, Audio DSP, Pulse Song Recognition, Download Manager)
  auto *featuresLayout = new QHBoxLayout();
  featuresLayout->setSpacing(10);

  // 1. Dual Engine Power (WebKit + Chromium icons)
  featuresLayout->addWidget(createDualEngineCard(
      dalinira::i18n::I18n::text(QStringLiteral("onboarding.feature_dual_engine_title")),
      dalinira::i18n::I18n::text(QStringLiteral("onboarding.feature_dual_engine_desc")),
      cardDualEngine_), 1);

  // 2. Built-in Privacy & Adblock (Shield icon)
  featuresLayout->addWidget(createFeatureCard(
      dalinira::i18n::I18n::text(QStringLiteral("onboarding.feature_privacy_title")),
      dalinira::i18n::I18n::text(QStringLiteral("onboarding.feature_privacy_desc")),
      QStringLiteral("privacy.svg"), QColor(QStringLiteral("#10b981")),
      cardPrivacy_), 1);

  // 3. Audiophile Audio & DSP (Equalizer/Tune icon)
  featuresLayout->addWidget(createFeatureCard(
      dalinira::i18n::I18n::text(QStringLiteral("onboarding.feature_audio_title")),
      dalinira::i18n::I18n::text(QStringLiteral("onboarding.feature_audio_desc")),
      QStringLiteral("tune.svg"), QColor(QStringLiteral("#a855f7")),
      cardAudio_), 1);

  // 4. Pulse Song Recognition (Music icon)
  featuresLayout->addWidget(createFeatureCard(
      dalinira::i18n::I18n::text(QStringLiteral("onboarding.feature_pulse_title")),
      dalinira::i18n::I18n::text(QStringLiteral("onboarding.feature_pulse_desc")),
      QStringLiteral("music.svg"), QColor(QStringLiteral("#f43f5e")),
      cardPulse_), 1);

  // 5. Smart Download Manager (Download icon)
  featuresLayout->addWidget(createFeatureCard(
      dalinira::i18n::I18n::text(QStringLiteral("onboarding.feature_download_title")),
      dalinira::i18n::I18n::text(QStringLiteral("onboarding.feature_download_desc")),
      QStringLiteral("download.svg"), QColor(QStringLiteral("#06b6d4")),
      cardDownload_), 1);

  cardLayout->addLayout(featuresLayout);

  cardLayout->addSpacing(6);

  // Application Language Selection Bar
  auto *langContainer = new QFrame(mainCard);
  langContainer->setObjectName(QStringLiteral("welcome-lang-container"));
  langContainer->setStyleSheet(QStringLiteral(
      "#welcome-lang-container {"
      "  background: rgba(255, 255, 255, 0.045);"
      "  border: 1px solid rgba(255, 255, 255, 0.12);"
      "  border-radius: 12px;"
      "}"));
  auto *langLayout = new QHBoxLayout(langContainer);
  langLayout->setContentsMargins(16, 8, 16, 8);
  langLayout->setSpacing(12);

  auto *langIcon = new QLabel(QStringLiteral("🌐"), langContainer);
  langIcon->setStyleSheet(QStringLiteral("font-size: 15px; background: transparent; border: none;"));
  langLayout->addWidget(langIcon);

  langLabel_ = new QLabel(dalinira::i18n::I18n::text(QStringLiteral("settings.language.select_label")), langContainer);
  langLabel_->setStyleSheet(QStringLiteral(
      "font-size: 12.5px; font-weight: 600; color: #f1f5f9; background: transparent; border: none;"));
  langLayout->addWidget(langLabel_);

  langLayout->addStretch(1);

  langCombo_ = new QComboBox(langContainer);
  langCombo_->setObjectName(QStringLiteral("welcome-language-combo"));
  langCombo_->setCursor(Qt::PointingHandCursor);
  langCombo_->setStyleSheet(QStringLiteral(
      "#welcome-language-combo {"
      "  background: rgba(15, 23, 42, 0.95);"
      "  color: #ffffff;"
      "  border: 1px solid rgba(59, 130, 246, 0.45);"
      "  border-radius: 8px;"
      "  padding: 4px 12px;"
      "  font-size: 12.5px;"
      "  font-weight: 500;"
      "  min-width: 180px;"
      "}"
      "#welcome-language-combo:hover {"
      "  border: 1px solid #38bdf8;"
      "}"
      "#welcome-language-combo QAbstractItemView {"
      "  background: #0f172a;"
      "  color: #ffffff;"
      "  selection-background-color: #2563eb;"
      "  selection-color: #ffffff;"
      "  border: 1px solid rgba(255, 255, 255, 0.15);"
      "  border-radius: 8px;"
      "  padding: 4px;"
      "}"));

  langCombo_->addItem(dalinira::i18n::LanguageManager::instance().formatSystemLanguageLabel(), QStringLiteral("system"));
  for (const auto &info : dalinira::i18n::LanguageManager::instance().supportedLanguages()) {
    langCombo_->addItem(info.nativeName, info.code);
  }
  const QString currentPref = dalinira::i18n::LanguageManager::instance().languagePreference();
  int prefIdx = langCombo_->findData(currentPref);
  if (prefIdx >= 0) {
    langCombo_->setCurrentIndex(prefIdx);
  } else {
    langCombo_->setCurrentIndex(0);
  }

  connect(langCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int idx) {
    if (idx >= 0 && langCombo_ && !updatingLangCombo_) {
      const QString chosen = langCombo_->itemData(idx).toString();
      dalinira::i18n::LanguageManager::instance().setLanguagePreference(chosen);
    }
  });

  langLayout->addWidget(langCombo_);
  cardLayout->addWidget(langContainer);

  cardLayout->addSpacing(8);

  // Buttons Layout
  auto *buttonsLayout = new QVBoxLayout();
  buttonsLayout->setSpacing(10);
  buttonsLayout->setAlignment(Qt::AlignCenter);

  getStartedBtn_ = new QPushButton(nextButtonText(), mainCard);
  getStartedBtn_->setObjectName(QStringLiteral("onboarding-get-started-btn"));
  getStartedBtn_->setCursor(Qt::PointingHandCursor);
  getStartedBtn_->setMinimumSize(250, 46);
  getStartedBtn_->setStyleSheet(QStringLiteral(
      "#onboarding-get-started-btn {"
      "  background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #2563eb, stop:0.5 #3b82f6, stop:1 #06b6d4);"
      "  color: #ffffff;"
      "  font-size: 14.5px;"
      "  font-weight: 700;"
      "  border: none;"
      "  border-radius: 13px;"
      "  padding: 0 32px;"
      "}"
      "#onboarding-get-started-btn:hover {"
      "  background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #3b82f6, stop:0.5 #60a5fa, stop:1 #22d3ee);"
      "}"
      "#onboarding-get-started-btn:pressed {"
      "  background: #1d4ed8;"
      "}"));
  connect(getStartedBtn_, &QPushButton::clicked, this, [this] {
    emit stepCompleted();
  });
  buttonsLayout->addWidget(getStartedBtn_, 0, Qt::AlignHCenter);

  skipBtn_ = new QPushButton(skipButtonText(), mainCard);
  skipBtn_->setObjectName(QStringLiteral("onboarding-skip-btn"));
  skipBtn_->setCursor(Qt::PointingHandCursor);
  skipBtn_->setStyleSheet(QStringLiteral(
      "#onboarding-skip-btn {"
      "  background: transparent;"
      "  color: #94a3b8;"
      "  font-size: 12.5px;"
      "  font-weight: 500;"
      "  border: none;"
      "  padding: 6px 16px;"
      "}"
      "#onboarding-skip-btn:hover {"
      "  color: #ffffff;"
      "  text-decoration: underline;"
      "}"));
  connect(skipBtn_, &QPushButton::clicked, this, [this] {
    emit skipRequested();
  });
  buttonsLayout->addWidget(skipBtn_, 0, Qt::AlignHCenter);

  cardLayout->addLayout(buttonsLayout);

  outerLayout->addWidget(mainCard, 0, Qt::AlignCenter);
}

void WelcomeStep::retranslateUi() {
  if (titleLabel_) {
    titleLabel_->setText(stepTitle());
  }
  if (subtitleLabel_) {
    subtitleLabel_->setText(stepSubtitle());
  }
  if (cardDualEngine_.titleLabel) {
    cardDualEngine_.titleLabel->setText(dalinira::i18n::I18n::text(QStringLiteral("onboarding.feature_dual_engine_title")));
    cardDualEngine_.descLabel->setText(dalinira::i18n::I18n::text(QStringLiteral("onboarding.feature_dual_engine_desc")));
  }
  if (cardPrivacy_.titleLabel) {
    cardPrivacy_.titleLabel->setText(dalinira::i18n::I18n::text(QStringLiteral("onboarding.feature_privacy_title")));
    cardPrivacy_.descLabel->setText(dalinira::i18n::I18n::text(QStringLiteral("onboarding.feature_privacy_desc")));
  }
  if (cardAudio_.titleLabel) {
    cardAudio_.titleLabel->setText(dalinira::i18n::I18n::text(QStringLiteral("onboarding.feature_audio_title")));
    cardAudio_.descLabel->setText(dalinira::i18n::I18n::text(QStringLiteral("onboarding.feature_audio_desc")));
  }
  if (cardPulse_.titleLabel) {
    cardPulse_.titleLabel->setText(dalinira::i18n::I18n::text(QStringLiteral("onboarding.feature_pulse_title")));
    cardPulse_.descLabel->setText(dalinira::i18n::I18n::text(QStringLiteral("onboarding.feature_pulse_desc")));
  }
  if (cardDownload_.titleLabel) {
    cardDownload_.titleLabel->setText(dalinira::i18n::I18n::text(QStringLiteral("onboarding.feature_download_title")));
    cardDownload_.descLabel->setText(dalinira::i18n::I18n::text(QStringLiteral("onboarding.feature_download_desc")));
  }
  if (langLabel_) {
    langLabel_->setText(dalinira::i18n::I18n::text(QStringLiteral("settings.language.select_label")));
  }
  if (langCombo_) {
    updatingLangCombo_ = true;
    langCombo_->setItemText(0, dalinira::i18n::LanguageManager::instance().formatSystemLanguageLabel());
    updatingLangCombo_ = false;
  }
  if (getStartedBtn_) {
    getStartedBtn_->setText(nextButtonText());
  }
  if (skipBtn_) {
    skipBtn_->setText(skipButtonText());
  }
  // New wording, new heights once the labels have their new text laid out.
  QTimer::singleShot(0, this, [this] { fitFeatureCards(); });
}

}  // namespace dalinira::onboarding
