#include "engine_step.h"
#include "modern_radio_button.h"
#include "onboarding_manager.h"
#include "core/browser_icons.h"
#include "engine/engine_type.h"
#include "i18n/i18n.h"

#include <QEvent>
#include <QButtonGroup>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPropertyAnimation>
#include <QRadioButton>
#include <QVBoxLayout>

namespace dalinira::onboarding {

namespace {

class CardClickFilter : public QObject {
 public:
  explicit CardClickFilter(std::function<void()> callback, QObject *parent = nullptr)
      : QObject(parent), callback_(std::move(callback)) {}

 protected:
  bool eventFilter(QObject *watched, QEvent *event) override {
    if (event->type() == QEvent::MouseButtonPress) {
      if (callback_) callback_();
      return true;
    }
    return QObject::eventFilter(watched, event);
  }

 private:
  std::function<void()> callback_;
};

}  // namespace

EngineStep::EngineStep(QWidget *parent)
    : OnboardingStep(parent) {
  setupUi();
}

QString EngineStep::stepTitle() const {
  return dalinira::i18n::I18n::text(QStringLiteral("onboarding.engine_title"));
}

QString EngineStep::stepSubtitle() const {
  return dalinira::i18n::I18n::text(QStringLiteral("onboarding.engine_subtitle"));
}

QString EngineStep::nextButtonText() const {
  return dalinira::i18n::I18n::text(QStringLiteral("onboarding.next"));
}

QString EngineStep::backButtonText() const {
  return dalinira::i18n::I18n::text(QStringLiteral("onboarding.back"));
}

QString EngineStep::skipButtonText() const {
  return dalinira::i18n::I18n::text(QStringLiteral("onboarding.skip"));
}

void EngineStep::setupUi() {
  setObjectName(QStringLiteral("step-engine"));

  auto *mainLayout = new QVBoxLayout(this);
  mainLayout->setContentsMargins(32, 24, 32, 24);
  mainLayout->setAlignment(Qt::AlignCenter);

  // Central dark glass card
  auto *card = new QFrame(this);
  card->setObjectName(QStringLiteral("engine-main-card"));
  card->setMaximumWidth(700);
  card->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
  card->setStyleSheet(QStringLiteral(
      "#engine-main-card {"
      "  background: rgba(15, 23, 42, 0.82);"
      "  border: 1px solid rgba(255, 255, 255, 0.18);"
      "  border-radius: 24px;"
      "}"));

  auto *cardLayout = new QVBoxLayout(card);
  cardLayout->setContentsMargins(36, 30, 36, 30);
  cardLayout->setSpacing(16);

  // Title
  titleLabel_ = new QLabel(stepTitle(), card);
  titleLabel_->setObjectName(QStringLiteral("engine-step-title"));
  titleLabel_->setStyleSheet(QStringLiteral(
      "font-size: 28px; font-weight: 800; color: #ffffff; background: transparent; border: none;"));
  titleLabel_->setAlignment(Qt::AlignCenter);
  cardLayout->addWidget(titleLabel_);

  // Subtitle
  subtitleLabel_ = new QLabel(stepSubtitle(), card);
  subtitleLabel_->setObjectName(QStringLiteral("engine-step-subtitle"));
  subtitleLabel_->setStyleSheet(QStringLiteral(
      "font-size: 15px; font-weight: 500; color: #f1f5f9; background: transparent; border: none;"));
  subtitleLabel_->setAlignment(Qt::AlignCenter);
  subtitleLabel_->setWordWrap(true);
  subtitleLabel_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  subtitleLabel_->setMinimumHeight(44);
  cardLayout->addWidget(subtitleLabel_);

  cardLayout->addSpacing(8);

  // Horizontal cards row
  auto *cardsRow = new QHBoxLayout();
  cardsRow->setSpacing(18);

  // 1. Chromium Card
  chromiumCard_ = new QFrame(card);
  chromiumCard_->setObjectName(QStringLiteral("card-chromium"));
  chromiumCard_->setCursor(Qt::PointingHandCursor);
  chromiumCard_->setMinimumSize(250, 160);
  chromiumCard_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

  auto *chromiumLayout = new QVBoxLayout(chromiumCard_);
  chromiumLayout->setContentsMargins(20, 18, 20, 18);
  chromiumLayout->setSpacing(10);

  auto *chromiumTopRow = new QHBoxLayout();
  auto *chromiumIconLabel = new QLabel(chromiumCard_);
  chromiumIconLabel->setPixmap(BrowserIcons::chromiumIcon().pixmap(44, 44));
  chromiumIconLabel->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
  chromiumRadio_ = new ModernRadioButton(chromiumCard_);
  chromiumRadio_->setObjectName(QStringLiteral("chromium-radio"));
  chromiumTopRow->addWidget(chromiumIconLabel);
  chromiumTopRow->addStretch();
  chromiumTopRow->addWidget(chromiumRadio_);
  chromiumLayout->addLayout(chromiumTopRow);

  chromiumTitle_ = new QLabel(dalinira::i18n::I18n::text(QStringLiteral("onboarding.engine_chromium_name")), chromiumCard_);
  chromiumTitle_->setStyleSheet(QStringLiteral("font-size: 17px; font-weight: 700; color: #ffffff; background: transparent; border: none;"));
  chromiumLayout->addWidget(chromiumTitle_);

  chromiumBadge_ = new QLabel(dalinira::i18n::I18n::text(QStringLiteral("onboarding.engine_chromium_badge")), chromiumCard_);
  chromiumBadge_->setStyleSheet(QStringLiteral(
      "background: rgba(138, 180, 248, 0.12); color: #8ab4f8; font-size: 11px; font-weight: 600; border-radius: 6px; padding: 3px 8px; border: 1px solid rgba(138, 180, 248, 0.25);"));
  chromiumBadge_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
  chromiumLayout->addWidget(chromiumBadge_);

  cardsRow->addWidget(chromiumCard_);

  // 2. WebKit Card
  webkitCard_ = new QFrame(card);
  webkitCard_->setObjectName(QStringLiteral("card-webkit"));
  webkitCard_->setCursor(Qt::PointingHandCursor);
  webkitCard_->setMinimumSize(250, 160);
  webkitCard_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

  auto *webkitLayout = new QVBoxLayout(webkitCard_);
  webkitLayout->setContentsMargins(20, 18, 20, 18);
  webkitLayout->setSpacing(10);

  auto *webkitTopRow = new QHBoxLayout();
  auto *webkitIconLabel = new QLabel(webkitCard_);
  webkitIconLabel->setPixmap(BrowserIcons::webkitIcon().pixmap(44, 44));
  webkitIconLabel->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
  webkitRadio_ = new ModernRadioButton(webkitCard_);
  webkitRadio_->setObjectName(QStringLiteral("webkit-radio"));
  webkitTopRow->addWidget(webkitIconLabel);
  webkitTopRow->addStretch();
  webkitTopRow->addWidget(webkitRadio_);
  webkitLayout->addLayout(webkitTopRow);

  webkitTitle_ = new QLabel(dalinira::i18n::I18n::text(QStringLiteral("onboarding.engine_webkit_name")), webkitCard_);
  webkitTitle_->setStyleSheet(QStringLiteral("font-size: 17px; font-weight: 700; color: #ffffff; background: transparent; border: none;"));
  webkitLayout->addWidget(webkitTitle_);

  webkitBadge_ = new QLabel(dalinira::i18n::I18n::text(QStringLiteral("onboarding.engine_webkit_badge")), webkitCard_);
  webkitBadge_->setStyleSheet(QStringLiteral(
      "background: rgba(138, 180, 248, 0.12); color: #8ab4f8; font-size: 11px; font-weight: 600; border-radius: 6px; padding: 3px 8px; border: 1px solid rgba(138, 180, 248, 0.25);"));
  webkitBadge_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
  webkitLayout->addWidget(webkitBadge_);

  cardsRow->addWidget(webkitCard_);
  cardLayout->addLayout(cardsRow);

  // The radio buttons live under different card parents, so Qt's automatic
  // exclusivity cannot group them. Without an explicit group, clicking the
  // Chromium radio only changed its paint state while selectedEngine_ stayed
  // "webkit". Keep both the visual and persisted selection authoritative.
  auto *engineGroup = new QButtonGroup(this);
  engineGroup->setExclusive(true);
  engineGroup->addButton(chromiumRadio_);
  engineGroup->addButton(webkitRadio_);
  // toggled, not clicked: keyboard and assistive technologies (AT-SPI)
  // check a radio without a click. Re-selecting the current engine is a no-op.
  connect(chromiumRadio_, &QRadioButton::toggled, this, [this](bool checked) {
    if (checked && selectedEngine_ != QLatin1String("chromium")) selectEngine(QStringLiteral("chromium"));
  });
  connect(webkitRadio_, &QRadioButton::toggled, this, [this](bool checked) {
    if (checked && selectedEngine_ != QLatin1String("webkit")) selectEngine(QStringLiteral("webkit"));
  });

  // Click filters
  chromiumCard_->installEventFilter(new CardClickFilter([this] { selectEngine(QStringLiteral("chromium")); }, this));
  webkitCard_->installEventFilter(new CardClickFilter([this] { selectEngine(QStringLiteral("webkit")); }, this));

  cardLayout->addSpacing(6);

  // Explanation Banner below cards
  explanationBanner_ = new QFrame(card);
  explanationBanner_->setObjectName(QStringLiteral("engine-explanation-banner"));
  explanationBanner_->setStyleSheet(QStringLiteral(
      "#engine-explanation-banner {"
      "  background: rgba(30, 41, 59, 0.72);"
      "  border: 1px solid rgba(147, 197, 253, 0.35);"
      "  border-radius: 14px;"
      "}"));

  auto *explanationLayout = new QVBoxLayout(explanationBanner_);
  explanationLayout->setContentsMargins(18, 14, 18, 14);
  explanationLayout->setSpacing(6);

  explanationTitle_ = new QLabel(explanationBanner_);
  explanationTitle_->setStyleSheet(QStringLiteral("font-size: 14px; font-weight: 700; color: #93c5fd; background: transparent; border: none;"));
  explanationLayout->addWidget(explanationTitle_);

  explanationDesc_ = new QLabel(explanationBanner_);
  explanationDesc_->setStyleSheet(QStringLiteral("font-size: 13px; color: #f1f5f9; background: transparent; border: none;"));
  explanationDesc_->setWordWrap(true);
  explanationLayout->addWidget(explanationDesc_);

  bannerOpacityEffect_ = new QGraphicsOpacityEffect(explanationBanner_);
  explanationBanner_->setGraphicsEffect(bannerOpacityEffect_);

  cardLayout->addWidget(explanationBanner_);

  // Note footer
  noteLabel_ = new QLabel(dalinira::i18n::I18n::text(QStringLiteral("onboarding.engine_note")), card);
  noteLabel_->setStyleSheet(QStringLiteral("font-size: 12px; color: #cbd5e1; background: transparent; border: none;"));
  noteLabel_->setAlignment(Qt::AlignCenter);
  cardLayout->addWidget(noteLabel_, 0, Qt::AlignHCenter);

  mainLayout->addWidget(card, 0, Qt::AlignCenter);

  // Initial selection
  selectedEngine_ = OnboardingManager::instance().selectedEngine();
  if (selectedEngine_.isEmpty()) {
    selectedEngine_ = QStringLiteral("chromium");
  }
  selectEngine(selectedEngine_, false);
}

void EngineStep::selectEngine(const QString &engine, bool animate) {
  // Setup offers Chromium and WebKit; any other engine key means Chromium.
  const bool webkit = dalinira::engine::engineTypeFromKey(engine) == dalinira::engine::EngineType::WebKit;
  selectedEngine_ = dalinira::engine::engineKey(webkit ? dalinira::engine::EngineType::WebKit
                                                       : dalinira::engine::EngineType::Chromium);

  updateCardStyles();
  updateExplanation(animate);

  OnboardingManager::instance().setSelectedEngine(selectedEngine_);
}

void EngineStep::updateCardStyles() {
  const bool isChromium = (selectedEngine_ == QLatin1String("chromium"));

  chromiumRadio_->setChecked(isChromium);
  webkitRadio_->setChecked(!isChromium);

  const QString activeChromiumStyle = QStringLiteral(
      "#card-chromium {"
      "  background: rgba(16, 185, 129, 0.08);"
      "  border: 2px solid #10b981;"
      "  border-radius: 16px;"
      "}");

  const QString inactiveChromiumStyle = QStringLiteral(
      "#card-chromium {"
      "  background: rgba(255, 255, 255, 0.045);"
      "  border: 1px solid rgba(255, 255, 255, 0.14);"
      "  border-radius: 16px;"
      "}"
      "#card-chromium:hover {"
      "  background: rgba(255, 255, 255, 0.075);"
      "  border: 1px solid rgba(255, 255, 255, 0.28);"
      "}");

  const QString activeWebkitStyle = QStringLiteral(
      "#card-webkit {"
      "  background: rgba(16, 185, 129, 0.08);"
      "  border: 2px solid #10b981;"
      "  border-radius: 16px;"
      "}");

  const QString inactiveWebkitStyle = QStringLiteral(
      "#card-webkit {"
      "  background: rgba(255, 255, 255, 0.045);"
      "  border: 1px solid rgba(255, 255, 255, 0.14);"
      "  border-radius: 16px;"
      "}"
      "#card-webkit:hover {"
      "  background: rgba(255, 255, 255, 0.075);"
      "  border: 1px solid rgba(255, 255, 255, 0.28);"
      "}");

  chromiumCard_->setStyleSheet(isChromium ? activeChromiumStyle : inactiveChromiumStyle);
  webkitCard_->setStyleSheet(!isChromium ? activeWebkitStyle : inactiveWebkitStyle);

  chromiumRadio_->update();
  webkitRadio_->update();
}

void EngineStep::updateExplanation(bool animate) {
  const bool isChromium = (selectedEngine_ == QLatin1String("chromium"));

  if (isChromium) {
    explanationTitle_->setText(dalinira::i18n::I18n::text(QStringLiteral("onboarding.engine_chromium_name")));
    explanationDesc_->setText(dalinira::i18n::I18n::text(QStringLiteral("onboarding.engine_chromium_desc")));
  } else {
    explanationTitle_->setText(dalinira::i18n::I18n::text(QStringLiteral("onboarding.engine_webkit_name")));
    explanationDesc_->setText(dalinira::i18n::I18n::text(QStringLiteral("onboarding.engine_webkit_desc")));
  }

  if (animate && OnboardingManager::animationsEnabled()) {
    auto *anim = new QPropertyAnimation(bannerOpacityEffect_, "opacity", this);
    anim->setDuration(220);
    anim->setStartValue(0.3);
    anim->setEndValue(1.0);
    anim->setEasingCurve(QEasingCurve::OutCubic);
    anim->start(QAbstractAnimation::DeleteWhenStopped);
  } else {
    bannerOpacityEffect_->setOpacity(1.0);
  }
}

void EngineStep::onStepEntering() {
  const QString saved = OnboardingManager::instance().selectedEngine();
  selectEngine(saved.isEmpty() ? QStringLiteral("chromium") : saved, false);
}

void EngineStep::onStepLeaving() {
  OnboardingManager::instance().setSelectedEngine(selectedEngine_);
}

void EngineStep::retranslateUi() {
  if (titleLabel_) titleLabel_->setText(stepTitle());
  if (subtitleLabel_) subtitleLabel_->setText(stepSubtitle());
  if (chromiumTitle_) chromiumTitle_->setText(dalinira::i18n::I18n::text(QStringLiteral("onboarding.engine_chromium_name")));
  if (chromiumBadge_) chromiumBadge_->setText(dalinira::i18n::I18n::text(QStringLiteral("onboarding.engine_chromium_badge")));
  if (webkitTitle_) webkitTitle_->setText(dalinira::i18n::I18n::text(QStringLiteral("onboarding.engine_webkit_name")));
  if (webkitBadge_) webkitBadge_->setText(dalinira::i18n::I18n::text(QStringLiteral("onboarding.engine_webkit_badge")));
  if (noteLabel_) noteLabel_->setText(dalinira::i18n::I18n::text(QStringLiteral("onboarding.engine_note")));
  updateExplanation(false);
}

}  // namespace dalinira::onboarding
