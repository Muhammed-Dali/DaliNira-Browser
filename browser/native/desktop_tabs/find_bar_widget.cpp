#include "find_bar_widget.h"

#include <QHBoxLayout>
#include <QIcon>
#include <QPainterPath>
#include <QPalette>
#include <QRegion>
#include <QStyle>
#include "core/browser_icons.h"
#include "i18n/i18n.h"
#include "i18n/language_manager.h"

namespace dalinira::desktop_tabs {

FindBarWidget::FindBarWidget(QWidget *parent)
    : QWidget(parent) {
  setupUi();
  applyStyle();
  connect(&dalinira::i18n::LanguageManager::instance(),
          &dalinira::i18n::LanguageManager::languageChanged,
          this, &FindBarWidget::retranslateUi);
}

void FindBarWidget::setupUi() {
  setObjectName(QStringLiteral("daliniraFindBar"));
  setAttribute(Qt::WA_StyledBackground, true);

  auto *layout = new QHBoxLayout(this);
  layout->setContentsMargins(10, 7, 7, 7);
  layout->setSpacing(2);

  searchEdit_ = new QLineEdit(this);
  searchEdit_->setObjectName(QStringLiteral("find-search-field"));
  searchEdit_->setClearButtonEnabled(true);
  searchEdit_->addAction(BrowserIcons::icon(BrowserIcon::Search), QLineEdit::LeadingPosition);
  searchEdit_->installEventFilter(this);
  connect(searchEdit_, &QLineEdit::textChanged, this, &FindBarWidget::onTextChanged);
  layout->addWidget(searchEdit_, 1);

  matchCountLabel_ = new QLabel(this);
  matchCountLabel_->setObjectName(QStringLiteral("matchCountLabel"));
  matchCountLabel_->setMinimumWidth(48);
  matchCountLabel_->setAlignment(Qt::AlignCenter | Qt::AlignVCenter);
  layout->addSpacing(4);
  layout->addWidget(matchCountLabel_);

  divider_ = new QFrame(this);
  divider_->setObjectName(QStringLiteral("find-divider"));
  divider_->setFixedSize(1, 22);
  layout->addSpacing(4);
  layout->addWidget(divider_, 0, Qt::AlignVCenter);
  layout->addSpacing(4);

  caseBtn_ = new QToolButton(this);
  caseBtn_->setObjectName(QStringLiteral("find-case-button"));
  caseBtn_->setText(QStringLiteral("Aa"));  // the conventional match-case glyph
  caseBtn_->setCheckable(true);
  connect(caseBtn_, &QToolButton::toggled, this, &FindBarWidget::onCaseToggled);
  layout->addWidget(caseBtn_);

  prevBtn_ = new QToolButton(this);
  prevBtn_->setObjectName(QStringLiteral("find-previous-button"));
  connect(prevBtn_, &QToolButton::clicked, this, &FindBarWidget::onPrevClicked);
  layout->addWidget(prevBtn_);

  nextBtn_ = new QToolButton(this);
  nextBtn_->setObjectName(QStringLiteral("find-next-button"));
  connect(nextBtn_, &QToolButton::clicked, this, &FindBarWidget::onNextClicked);
  layout->addWidget(nextBtn_);

  closeBtn_ = new QToolButton(this);
  closeBtn_->setObjectName(QStringLiteral("find-close-button"));
  connect(closeBtn_, &QToolButton::clicked, this, &FindBarWidget::closeRequested);
  layout->addWidget(closeBtn_);

  for (QToolButton *button : {caseBtn_, prevBtn_, nextBtn_, closeBtn_}) {
    button->setAutoRaise(true);
    button->setFixedSize(30, 30);
    button->setIconSize(QSize(18, 18));
    button->setFocusPolicy(Qt::TabFocus);
  }

  setFixedHeight(46);
  setMinimumWidth(220);
  retranslateUi();
  updateNavigationState();
}

void FindBarWidget::applyStyle(bool force) {
  const bool dark = palette().color(QPalette::Window).lightness() < 128;
  // setStyleSheet() repolishes and sends PaletteChange back to changeEvent():
  // restyle only when the theme actually flipped, never from inside a restyle.
  if (applyingStyle_ || (!force && styled_ && dark == styledDark_)) return;
  applyingStyle_ = true;
  styled_ = true;
  styledDark_ = dark;
  const QString background = dark ? QStringLiteral("#20252d") : QStringLiteral("#ffffff");
  const QString border = dark ? QStringLiteral("#3b4654") : QStringLiteral("#cbd5e1");
  const QString fieldBackground = dark ? QStringLiteral("#151a21") : QStringLiteral("#f6f8fb");
  const QString fieldBorder = dark ? QStringLiteral("#465465") : QStringLiteral("#b8c5d3");
  const QString text = dark ? QStringLiteral("#edf3fa") : QStringLiteral("#17212b");
  const QString secondary = dark ? QStringLiteral("#aeb9c7") : QStringLiteral("#526274");
  const QString hover = dark ? QStringLiteral("#303945") : QStringLiteral("#e8eef5");
  const QString pressed = dark ? QStringLiteral("#3a4654") : QStringLiteral("#dbe5ef");
  const QString accent = dark ? QStringLiteral("#8ab4f8") : QStringLiteral("#1873d1");
  const QString disabled = dark ? QStringLiteral("#5b6676") : QStringLiteral("#a7b2bf");

  // Theme-tinted icons: the shared toolbar set is drawn for the dark chrome.
  prevBtn_->setIcon(BrowserIcons::tintedIcon(BrowserIcon::ChevronUp, QColor(secondary), QColor(text), QColor(disabled)));
  nextBtn_->setIcon(BrowserIcons::tintedIcon(BrowserIcon::ChevronDown, QColor(secondary), QColor(text), QColor(disabled)));
  closeBtn_->setIcon(BrowserIcons::tintedIcon(BrowserIcon::Close, QColor(secondary), QColor(text), QColor(disabled)));

  setStyleSheet(QStringLiteral(
      "#daliniraFindBar {"
      "  background-color: %1;"
      "  border: 1px solid %2;"
      "  border-radius: 12px;"
      "}"
      "#find-search-field {"
      "  background-color: %3;"
      "  color: %4;"
      "  border: 1px solid %5;"
      "  border-radius: 8px;"
      "  padding: 4px 8px;"
      "  font-size: 13px;"
      "  min-height: 20px;"
      "}"
      "#find-search-field:focus {"
      "  border: 2px solid %6;"
      "  padding: 3px 7px;"
      "}"
      "#matchCountLabel {"
      "  color: %7;"
      "  font-size: 12px;"
      "  font-variant-numeric: tabular-nums;"
      "  padding: 0 2px;"
      "}"
      "#matchCountLabel[noMatches=\"true\"] {"
      "  color: #ef4444;"
      "}"
      "#daliniraFindBar QToolButton {"
      "  background-color: transparent;"
      "  color: %7;"
      "  border: 1px solid transparent;"
      "  border-radius: 7px;"
      "  padding: 5px;"
      "  font-size: 12px;"
      "}"
      "#daliniraFindBar QToolButton:hover {"
      "  background-color: %8;"
      "  color: %4;"
      "}"
      "#daliniraFindBar QToolButton:pressed {"
      "  background-color: %9;"
      "}"
      "#daliniraFindBar QToolButton:focus {"
      "  border: 1px solid %6;"
      "}"
      "#daliniraFindBar QToolButton:checked {"
      "  background-color: %6;"
      "  color: %1;"
      "}"
      "#daliniraFindBar QToolButton:disabled {"
      "  color: %10;"
      "}"
      "#find-divider {"
      "  background-color: %2;"
      "  border: none;"
      "}"
  ).arg(background, border, fieldBackground, text, fieldBorder, accent,
        secondary, hover, pressed).arg(disabled));
  applyingStyle_ = false;
}

void FindBarWidget::retranslateUi() {
  searchEdit_->setPlaceholderText(I18n::text(QStringLiteral("find.placeholder")));
  caseBtn_->setToolTip(I18n::text(QStringLiteral("find.match_case")));
  prevBtn_->setToolTip(I18n::text(QStringLiteral("find.previous")));
  nextBtn_->setToolTip(I18n::text(QStringLiteral("find.next")));
  closeBtn_->setToolTip(I18n::text(QStringLiteral("find.close")));
}

void FindBarWidget::updateNavigationState() {
  const bool hasText = !findText().isEmpty();
  prevBtn_->setEnabled(hasText);
  nextBtn_->setEnabled(hasText);
}

void FindBarWidget::updateNativeMask() {
  // Over a WebKit page the bar is a native X window, which is rectangular:
  // without a shape its corners would show the window background instead of
  // the page. Qt paints alien widgets' rounded corners itself.
  if (!internalWinId()) {
    if (!mask().isEmpty()) clearMask();
    return;
  }
  QPainterPath path;
  path.addRoundedRect(QRectF(rect()), 12, 12);
  setMask(QRegion(path.toFillPolygon().toPolygon()));
}

void FindBarWidget::setFindText(const QString &text) {
  if (searchEdit_) {
    searchEdit_->setText(text);
  }
}

QString FindBarWidget::findText() const {
  return searchEdit_ ? searchEdit_->text() : QString();
}

bool FindBarWidget::isCaseSensitive() const {
  return caseBtn_ && caseBtn_->isChecked();
}

void FindBarWidget::setMatchCount(int current, int total) {
  if (!matchCountLabel_) return;
  if (total <= 0) {
    if (findText().isEmpty()) {
      matchCountLabel_->clear();
      matchCountLabel_->setProperty("noMatches", false);
    } else {
      matchCountLabel_->setText(I18n::text(QStringLiteral("find.no_matches")));
      matchCountLabel_->setProperty("noMatches", true);
    }
  } else {
    matchCountLabel_->setText(QStringLiteral("%1/%2").arg(current).arg(total));
    matchCountLabel_->setProperty("noMatches", false);
  }
  matchCountLabel_->style()->unpolish(matchCountLabel_);
  matchCountLabel_->style()->polish(matchCountLabel_);
}

void FindBarWidget::clearMatchCount() {
  if (matchCountLabel_) {
    matchCountLabel_->clear();
    matchCountLabel_->setProperty("noMatches", false);
    matchCountLabel_->style()->unpolish(matchCountLabel_);
    matchCountLabel_->style()->polish(matchCountLabel_);
  }
}

void FindBarWidget::focusAndSelectAll() {
  if (searchEdit_) {
    searchEdit_->setFocus();
    searchEdit_->selectAll();
  }
}

void FindBarWidget::refreshAppearance() {
  applyStyle();
}

QSize FindBarWidget::sizeHint() const {
  return QSize(440, 46);
}

void FindBarWidget::onTextChanged(const QString &text) {
  updateNavigationState();
  if (text.isEmpty()) {
    clearMatchCount();
    emit clearFindRequested();
  } else {
    emit findRequested(text, true, isCaseSensitive());
  }
}

void FindBarWidget::onNextClicked() {
  const QString text = findText();
  if (!text.isEmpty()) {
    emit findRequested(text, true, isCaseSensitive());
  }
}

void FindBarWidget::onPrevClicked() {
  const QString text = findText();
  if (!text.isEmpty()) {
    emit findRequested(text, false, isCaseSensitive());
  }
}

void FindBarWidget::onCaseToggled() {
  const QString text = findText();
  if (!text.isEmpty()) {
    emit findRequested(text, true, isCaseSensitive());
  }
}

bool FindBarWidget::eventFilter(QObject *watched, QEvent *event) {
  if (watched == searchEdit_ && event->type() == QEvent::KeyPress) {
    auto *keyEvent = static_cast<QKeyEvent *>(event);
    if (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) {
      const bool forward = !(keyEvent->modifiers() & Qt::ShiftModifier);
      const QString text = findText();
      if (!text.isEmpty()) {
        emit findRequested(text, forward, isCaseSensitive());
      }
      return true;
    }
    if (keyEvent->key() == Qt::Key_F3 ||
        (keyEvent->key() == Qt::Key_G && (keyEvent->modifiers() & Qt::ControlModifier))) {
      const bool forward = !(keyEvent->modifiers() & Qt::ShiftModifier);
      const QString text = findText();
      if (!text.isEmpty()) {
        emit findRequested(text, forward, isCaseSensitive());
      }
      return true;
    }
    if (keyEvent->key() == Qt::Key_Escape) {
      emit closeRequested();
      return true;
    }
  }
  return QWidget::eventFilter(watched, event);
}

void FindBarWidget::keyPressEvent(QKeyEvent *event) {
  if (event->key() == Qt::Key_Escape) {
    emit closeRequested();
    event->accept();
    return;
  }
  QWidget::keyPressEvent(event);
}

void FindBarWidget::resizeEvent(QResizeEvent *event) {
  QWidget::resizeEvent(event);
  updateNativeMask();
}

void FindBarWidget::showEvent(QShowEvent *event) {
  QWidget::showEvent(event);
  updateNativeMask();
}

void FindBarWidget::changeEvent(QEvent *event) {
  QWidget::changeEvent(event);
  if (event->type() == QEvent::PaletteChange)
    applyStyle(false);
}

}  // namespace dalinira::desktop_tabs
