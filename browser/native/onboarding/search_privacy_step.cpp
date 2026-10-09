#include "search_privacy_step.h"

#include "core/search_engine_definition.h"
#include "i18n/i18n.h"
#include "onboarding_manager.h"

#include <QButtonGroup>
#include <QGraphicsOpacityEffect>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMouseEvent>
#include <QPropertyAnimation>
#include <QRadioButton>
#include <QStyle>
#include <QVBoxLayout>

using dalinira::i18n::I18n;

namespace dalinira::onboarding {

SearchPrivacyCard::SearchPrivacyCard(QWidget *parent) : QFrame(parent) {
  setCursor(Qt::PointingHandCursor);
  setFrameShape(QFrame::StyledPanel);
  setObjectName(QStringLiteral("searchPrivacyCard"));
}

void SearchPrivacyCard::mousePressEvent(QMouseEvent *event) {
  if (event->button() == Qt::LeftButton && clicked) {
    clicked();
    event->accept();
    return;
  }
  QFrame::mousePressEvent(event);
}

SearchPrivacyStep::SearchPrivacyStep(ISearchPrivacyPreferences *preferences,
                                     QWidget *parent)
    : OnboardingStep(parent), preferences_(preferences) {
  auto *root = new QVBoxLayout(this);
  root->setContentsMargins(48, 24, 48, 24);
  root->setSpacing(18);

  auto *searchHeading = new QLabel(I18n::text(QStringLiteral("onboarding.search_privacy.search_heading")), this);
  searchHeading->setObjectName(QStringLiteral("searchPrivacyHeading"));
  root->addWidget(searchHeading);

  auto *searchGridWidget = new QWidget(this);
  auto *searchGrid = new QGridLayout(searchGridWidget);
  searchGrid->setContentsMargins(0, 0, 0, 0);
  searchGrid->setHorizontalSpacing(14);
  searchGrid->setVerticalSpacing(12);
  int index = 0;
  for (const auto &definition : dalinira::core::searchEngineDefinitions()) {
    const QString engine = QString::fromLatin1(definition.id);
    searchGrid->addWidget(createSearchCard(engine, searchGridWidget), index / 2, index % 2);
    ++index;
  }
  root->addWidget(searchGridWidget);

  auto *blockerHeading = new QLabel(I18n::text(QStringLiteral("onboarding.search_privacy.blocker_heading")), this);
  blockerHeading->setObjectName(QStringLiteral("searchPrivacyHeading"));
  root->addWidget(blockerHeading);

  auto *blockerRow = new QWidget(this);
  auto *blockerLayout = new QHBoxLayout(blockerRow);
  blockerLayout->setContentsMargins(0, 0, 0, 0);
  blockerLayout->setSpacing(14);
  blockerOnCard_ = createBlockerCard(true, blockerRow);
  blockerOffCard_ = createBlockerCard(false, blockerRow);
  blockerLayout->addWidget(blockerOnCard_);
  blockerLayout->addWidget(blockerOffCard_);
  root->addWidget(blockerRow);
  root->addStretch(1);

  setStyleSheet(QStringLiteral(
      "#searchPrivacyHeading { color:#ffffff; font-size:16px; font-weight:700; }"
      "#searchPrivacyCard { background:rgba(255, 255, 255, 0.05); border:1px solid rgba(255, 255, 255, 0.14); border-radius:12px; }"
      "#searchPrivacyCard:hover { background:rgba(255, 255, 255, 0.09); border-color:rgba(147, 197, 253, 0.4); }"
      "#searchPrivacyCard[selected=\"true\"] { background:rgba(59, 130, 246, 0.16); border:2px solid #60a5fa; }"
      "#searchPrivacyTitle { color:#ffffff; font-size:14px; font-weight:700; }"
      "#searchPrivacyDescription { color:#e2e8f0; font-size:12.5px; }"
      "#searchPrivacyBadge { background:rgba(59, 130, 246, 0.2); color:#93c5fd; border-radius:8px; padding:2px 7px; font-size:10px; font-weight:650; border:1px solid rgba(147, 197, 253, 0.3); }"));
  onStepEntering();
}

QString SearchPrivacyStep::stepTitle() const {
  return I18n::text(QStringLiteral("onboarding.search_privacy.title"));
}

QString SearchPrivacyStep::stepSubtitle() const {
  return I18n::text(QStringLiteral("onboarding.search_privacy.subtitle"));
}

QString SearchPrivacyStep::nextButtonText() const {
  return I18n::text(QStringLiteral("onboarding.next"));
}

SearchPrivacyCard *SearchPrivacyStep::createSearchCard(const QString &engine, QWidget *parent) {
  auto *card = new SearchPrivacyCard(parent);
  card->setProperty("engine", engine);
  auto *layout = new QHBoxLayout(card);
  layout->setContentsMargins(16, 13, 16, 13);
  layout->setSpacing(12);
  auto *radio = new QRadioButton(card);
  radio->setFocusPolicy(Qt::NoFocus);
  layout->addWidget(radio);
  auto *icon = new QLabel(card);
  icon->setFixedSize(30, 30);
  icon->setPixmap(QIcon(dalinira::core::searchEngineResourcePath(engine)).pixmap(28, 28));
  layout->addWidget(icon);
  auto *text = new QWidget(card);
  auto *textLayout = new QVBoxLayout(text);
  textLayout->setContentsMargins(0, 0, 0, 0);
  textLayout->setSpacing(3);
  auto *title = new QLabel(engine, text);
  title->setObjectName(QStringLiteral("searchPrivacyTitle"));
  auto *description = new QLabel(
      I18n::text(QStringLiteral("onboarding.search_privacy.provider_") + engine.toLower()), text);
  description->setObjectName(QStringLiteral("searchPrivacyDescription"));
  description->setWordWrap(true);
  textLayout->addWidget(title);
  textLayout->addWidget(description);
  layout->addWidget(text, 1);
  card->clicked = [this, engine] { selectSearchEngine(engine); };
  // toggled, not clicked: keyboard and assistive technologies check a radio
  // without a click.
  connect(radio, &QRadioButton::toggled, this, [this, engine](bool checked) {
    if (checked && selectedSearchEngine_ != engine) selectSearchEngine(engine);
  });
  searchCards_.insert(engine, card);
  searchRadios_.insert(engine, radio);
  return card;
}

SearchPrivacyCard *SearchPrivacyStep::createBlockerCard(bool enabled, QWidget *parent) {
  auto *card = new SearchPrivacyCard(parent);
  auto *layout = new QHBoxLayout(card);
  layout->setContentsMargins(16, 13, 16, 13);
  layout->setSpacing(12);
  auto *radio = new QRadioButton(card);
  radio->setFocusPolicy(Qt::NoFocus);
  layout->addWidget(radio);
  auto *text = new QWidget(card);
  auto *textLayout = new QVBoxLayout(text);
  textLayout->setContentsMargins(0, 0, 0, 0);
  textLayout->setSpacing(3);
  auto *titleRow = new QWidget(text);
  auto *titleLayout = new QHBoxLayout(titleRow);
  titleLayout->setContentsMargins(0, 0, 0, 0);
  auto *title = new QLabel(I18n::text(enabled
      ? QStringLiteral("onboarding.search_privacy.blocker_on")
      : QStringLiteral("onboarding.search_privacy.blocker_off")), titleRow);
  title->setObjectName(QStringLiteral("searchPrivacyTitle"));
  titleLayout->addWidget(title);
  if (enabled) {
    auto *badge = new QLabel(I18n::text(QStringLiteral("common.recommended")), titleRow);
    badge->setObjectName(QStringLiteral("searchPrivacyBadge"));
    titleLayout->addWidget(badge);
  }
  titleLayout->addStretch(1);
  auto *description = new QLabel(I18n::text(enabled
      ? QStringLiteral("onboarding.search_privacy.blocker_on_desc")
      : QStringLiteral("onboarding.search_privacy.blocker_off_desc")), text);
  description->setObjectName(QStringLiteral("searchPrivacyDescription"));
  description->setWordWrap(true);
  textLayout->addWidget(titleRow);
  textLayout->addWidget(description);
  layout->addWidget(text, 1);
  card->clicked = [this, enabled] { setBlockerEnabled(enabled); };
  connect(radio, &QRadioButton::toggled, this, [this, enabled](bool checked) {
    if (checked && blockerEnabled_ != enabled) setBlockerEnabled(enabled);
  });
  if (enabled) blockerOnRadio_ = radio;
  else blockerOffRadio_ = radio;
  return card;
}

void SearchPrivacyStep::onStepEntering() {
  if (preferences_) {
    selectedSearchEngine_ = preferences_->searchEngine();
    blockerEnabled_ = preferences_->blockerEnabled();
  }
  if (!searchCards_.contains(selectedSearchEngine_)) selectedSearchEngine_ = QStringLiteral("DuckDuckGo");
  updateSelection(false);
}

void SearchPrivacyStep::onStepLeaving() {
  if (!preferences_) return;
  preferences_->setSearchEngine(selectedSearchEngine_);
  preferences_->setBlockerEnabled(blockerEnabled_);
}

void SearchPrivacyStep::selectSearchEngine(const QString &engine, bool animated) {
  if (!searchCards_.contains(engine)) return;
  selectedSearchEngine_ = engine;
  if (preferences_) preferences_->setSearchEngine(engine);
  updateSelection(animated);
  pulse(searchCards_.value(engine), animated);
}

void SearchPrivacyStep::setBlockerEnabled(bool enabled, bool animated) {
  blockerEnabled_ = enabled;
  if (preferences_) preferences_->setBlockerEnabled(enabled);
  updateSelection(animated);
  pulse(enabled ? blockerOnCard_ : blockerOffCard_, animated);
}

void SearchPrivacyStep::updateSelection(bool) {
  for (auto it = searchCards_.begin(); it != searchCards_.end(); ++it) {
    const bool selected = it.key() == selectedSearchEngine_;
    it.value()->setProperty("selected", selected);
    searchRadios_.value(it.key())->setChecked(selected);
    it.value()->style()->unpolish(it.value());
    it.value()->style()->polish(it.value());
  }
  blockerOnCard_->setProperty("selected", blockerEnabled_);
  blockerOffCard_->setProperty("selected", !blockerEnabled_);
  blockerOnRadio_->setChecked(blockerEnabled_);
  blockerOffRadio_->setChecked(!blockerEnabled_);
  for (auto *card : {blockerOnCard_, blockerOffCard_}) {
    card->style()->unpolish(card);
    card->style()->polish(card);
  }
}

void SearchPrivacyStep::pulse(QWidget *widget, bool animated) {
  if (!widget || !animated || !OnboardingManager::animationsEnabled()) return;
  auto *effect = new QGraphicsOpacityEffect(widget);
  widget->setGraphicsEffect(effect);
  auto *animation = new QPropertyAnimation(effect, "opacity", widget);
  animation->setDuration(150);
  animation->setStartValue(0.72);
  animation->setEndValue(1.0);
  connect(animation, &QPropertyAnimation::finished, widget, [widget, animation] {
    widget->setGraphicsEffect(nullptr);
    animation->deleteLater();
  });
  animation->start();
}

}  // namespace dalinira::onboarding
