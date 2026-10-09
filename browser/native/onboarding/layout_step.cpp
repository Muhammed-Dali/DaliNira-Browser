#include "layout_step.h"
#include "modern_radio_button.h"
#include "onboarding_manager.h"
#include "i18n/i18n.h"
#include "i18n/language_manager.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QRadioButton>
#include <QFrame>
#include <QMouseEvent>
#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>
#include <QStyle>
#include <QButtonGroup>
#include <QApplication>

using dalinira::i18n::I18n;

namespace dalinira::onboarding {

ClickableLayoutCard::ClickableLayoutCard(QWidget *parent) : QFrame(parent) {
  setCursor(Qt::PointingHandCursor);
}

void ClickableLayoutCard::mousePressEvent(QMouseEvent *event) {
  if (event->button() == Qt::LeftButton && onClicked) {
    onClicked();
    event->accept();
    return;
  }
  QFrame::mousePressEvent(event);
}

class ClickableRowOption : public QFrame {
 public:
  explicit ClickableRowOption(QWidget *parent = nullptr) : QFrame(parent) {
    setCursor(Qt::PointingHandCursor);
    setStyleSheet(QStringLiteral("background: transparent; border: none;"));
  }
  std::function<void()> onClicked;
 protected:
  void mousePressEvent(QMouseEvent *event) override {
    if (event->button() == Qt::LeftButton && onClicked) {
      onClicked();
      event->accept();
      return;
    }
    QFrame::mousePressEvent(event);
  }
};

class LayoutPreviewWidget : public QWidget {
 public:
  explicit LayoutPreviewWidget(QWidget *parent = nullptr) : QWidget(parent) {
    setFixedHeight(380);
    setMaximumWidth(860);
    setMinimumWidth(820);
    setObjectName(QStringLiteral("layoutPreviewWidget"));
    setMouseTracking(true);

    auto *rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    // Window frame
    windowFrame_ = new QFrame(this);
    windowFrame_->setObjectName(QStringLiteral("miniWindowFrame"));
    auto *frameLayout = new QVBoxLayout(windowFrame_);
    frameLayout->setContentsMargins(0, 0, 0, 0);
    frameLayout->setSpacing(0);

    // 1. Miniature Titlebar / Tabbar
    auto *titlebar = new QWidget(windowFrame_);
    titlebar->setObjectName(QStringLiteral("miniTitlebar"));
    titlebar->setFixedHeight(32);
    auto *titleLayout = new QHBoxLayout(titlebar);
    titleLayout->setContentsMargins(12, 4, 12, 4);
    titleLayout->setSpacing(8);

    // 3 miniature window dots
    const auto makeDot = [titlebar](const QString &color) {
      auto *dot = new QFrame(titlebar);
      dot->setFixedSize(11, 11);
      dot->setStyleSheet(QStringLiteral("background: %1; border-radius: 5.5px;").arg(color));
      return dot;
    };
    titleLayout->addWidget(makeDot(QStringLiteral("#ef4444")));
    titleLayout->addWidget(makeDot(QStringLiteral("#f59e0b")));
    titleLayout->addWidget(makeDot(QStringLiteral("#10b981")));
    titleLayout->addSpacing(10);

    // Horizontal Tabs Container (ONLY visible in Classic mode!)
    miniHorizontalTabsContainer_ = new QWidget(titlebar);
    miniHorizontalTabsContainer_->setObjectName(QStringLiteral("miniHorizontalTabsContainer"));
    auto *htLayout = new QHBoxLayout(miniHorizontalTabsContainer_);
    htLayout->setContentsMargins(0, 0, 0, 0);
    htLayout->setSpacing(8);

    auto *miniTab1 = new QFrame(miniHorizontalTabsContainer_);
    miniTab1->setObjectName(QStringLiteral("miniTabActive"));
    miniTab1->setFixedHeight(24);
    miniTab1->setMinimumWidth(180);
    auto *tab1Layout = new QHBoxLayout(miniTab1);
    tab1Layout->setContentsMargins(8, 0, 8, 0);
    tab1Layout->setSpacing(6);
    auto *tab1Icon = new QFrame(miniTab1);
    tab1Icon->setObjectName(QStringLiteral("miniTabActiveDot"));
    tab1Icon->setFixedSize(8, 8);
    tab1Title_ = new QLabel(I18n::text(QStringLiteral("onboarding.preview_title_classic")), miniTab1);
    tab1Title_->setObjectName(QStringLiteral("miniTabActiveTitle"));
    auto *tab1Close = new QLabel(QStringLiteral("×"), miniTab1);
    tab1Close->setObjectName(QStringLiteral("miniTabActiveClose"));
    tab1Layout->addWidget(tab1Icon);
    tab1Layout->addWidget(tab1Title_, 1);
    tab1Layout->addWidget(tab1Close);
    htLayout->addWidget(miniTab1);

    auto *miniTab2 = new QFrame(miniHorizontalTabsContainer_);
    miniTab2->setObjectName(QStringLiteral("miniTabInactive"));
    miniTab2->setFixedHeight(24);
    miniTab2->setMinimumWidth(110);
    auto *tab2Layout = new QHBoxLayout(miniTab2);
    tab2Layout->setContentsMargins(8, 0, 8, 0);
    auto *tab2Title = new QLabel(QStringLiteral("GitHub"), miniTab2);
    tab2Title->setObjectName(QStringLiteral("miniTabInactiveTitle"));
    tab2Layout->addWidget(tab2Title);
    htLayout->addWidget(miniTab2);

    auto *newTabBtn = new QLabel(QStringLiteral("+"), miniHorizontalTabsContainer_);
    newTabBtn->setObjectName(QStringLiteral("miniNewTabBtn"));
    htLayout->addWidget(newTabBtn);

    titleLayout->addWidget(miniHorizontalTabsContainer_);

    // Titlebar label for non-classic modes (when horizontal tabs are moved to sides)
    miniTitlebarLabel_ = new QLabel(QStringLiteral("DaliNira Browser"), titlebar);
    miniTitlebarLabel_->setObjectName(QStringLiteral("miniTitlebarLabel"));
    miniTitlebarLabel_->hide();
    titleLayout->addWidget(miniTitlebarLabel_);

    titleLayout->addStretch(1);

    liveBadge_ = new QLabel(I18n::text(QStringLiteral("onboarding.preview_badge_classic")), titlebar);
    liveBadge_->setObjectName(QStringLiteral("miniLiveBadge"));
    titleLayout->addWidget(liveBadge_);

    frameLayout->addWidget(titlebar);

    // 2. Miniature Navigation Bar
    auto *navBar = new QWidget(windowFrame_);
    navBar->setObjectName(QStringLiteral("miniNavBar"));
    navBar->setFixedHeight(32);
    auto *navLayout = new QHBoxLayout(navBar);
    navLayout->setContentsMargins(14, 3, 14, 3);
    navLayout->setSpacing(10);

    const auto makeNavBtn = [navBar](const QString &symbol) {
      auto *btn = new QLabel(symbol, navBar);
      btn->setObjectName(QStringLiteral("miniNavBtn"));
      btn->setFixedSize(16, 16);
      btn->setAlignment(Qt::AlignCenter);
      return btn;
    };
    navLayout->addWidget(makeNavBtn(QStringLiteral("‹")));
    navLayout->addWidget(makeNavBtn(QStringLiteral("›")));
    navLayout->addWidget(makeNavBtn(QStringLiteral("↻")));

    auto *miniOmnibox = new QFrame(navBar);
    miniOmnibox->setObjectName(QStringLiteral("miniOmnibox"));
    miniOmnibox->setFixedHeight(22);
    auto *omniLayout = new QHBoxLayout(miniOmnibox);
    omniLayout->setContentsMargins(10, 0, 10, 0);
    omniLayout->setSpacing(8);
    auto *lockIcon = new QLabel(QStringLiteral("🔒"), miniOmnibox);
    auto *urlText = new QLabel(QStringLiteral("https://dalinira.app"), miniOmnibox);
    urlText->setObjectName(QStringLiteral("miniOmniboxUrl"));
    auto *shieldIcon = new QLabel(QStringLiteral("🛡️"), miniOmnibox);
    omniLayout->addWidget(lockIcon);
    omniLayout->addWidget(urlText, 1);
    omniLayout->addWidget(shieldIcon);
    navLayout->addWidget(miniOmnibox, 1);

    auto *eqIcon = new QLabel(QStringLiteral("🎚️"), navBar);
    navLayout->addWidget(eqIcon);

    frameLayout->addWidget(navBar);

    // 3. Miniature Body Container (380 - 32 - 32 = 316px tall)
    bodyContainer_ = new QWidget(windowFrame_);
    bodyContainer_->setObjectName(QStringLiteral("miniBodyContainer"));
    bodyContainer_->setMouseTracking(true);
    auto *bodyLayout = new QHBoxLayout(bodyContainer_);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(0);

    const auto makeSideTab = [](QWidget *parent, const QString &text, bool active, QLabel **outLabel = nullptr) {
      auto *item = new QFrame(parent);
      item->setObjectName(active ? QStringLiteral("miniSideTabActive") : QStringLiteral("miniSideTabInactive"));
      item->setFixedHeight(25);
      auto *l = new QHBoxLayout(item);
      l->setContentsMargins(8, 0, 8, 0);
      l->setSpacing(8);
      auto *dot = new QFrame(item);
      dot->setObjectName(active ? QStringLiteral("miniSideTabActiveDot") : QStringLiteral("miniSideTabInactiveDot"));
      dot->setFixedSize(6, 6);
      auto *lbl = new QLabel(text, item);
      lbl->setObjectName(active ? QStringLiteral("miniSideTabActiveText") : QStringLiteral("miniSideTabInactiveText"));
      if (outLabel) *outLabel = lbl;
      l->addWidget(dot);
      l->addWidget(lbl, 1);
      return item;
    };

    // Miniature Left Panel (Docked)
    miniLeftPanel_ = new QWidget(bodyContainer_);
    miniLeftPanel_->setObjectName(QStringLiteral("miniSidePanelLeft"));
    miniLeftPanel_->setFixedWidth(150);
    auto *leftLayout = new QVBoxLayout(miniLeftPanel_);
    leftLayout->setContentsMargins(8, 8, 8, 8);
    leftLayout->setSpacing(5);
    leftHeader_ = new QLabel(I18n::text(QStringLiteral("onboarding.preview_side_header_left")), miniLeftPanel_);
    leftHeader_->setObjectName(QStringLiteral("miniSideHeader"));
    leftLayout->addWidget(leftHeader_);
    leftLayout->addWidget(makeSideTab(miniLeftPanel_, I18n::text(QStringLiteral("onboarding.preview_tab_home")), true, &leftTabHome_));
    leftLayout->addWidget(makeSideTab(miniLeftPanel_, I18n::text(QStringLiteral("onboarding.preview_tab_docs")), false, &leftTabDocs_));
    leftLayout->addWidget(makeSideTab(miniLeftPanel_, I18n::text(QStringLiteral("onboarding.preview_tab_music")), false, &leftTabMusic_));
    leftLayout->addWidget(makeSideTab(miniLeftPanel_, I18n::text(QStringLiteral("onboarding.preview_tab_downloads")), false, &leftTabDownloads_));
    leftLayout->addWidget(makeSideTab(miniLeftPanel_, I18n::text(QStringLiteral("onboarding.preview_tab_bookmarks")), false, &leftTabBookmarks_));

    auto *newTabSideLeft = new QFrame(miniLeftPanel_);
    newTabSideLeft->setObjectName(QStringLiteral("miniNewTabSide"));
    newTabSideLeft->setFixedHeight(22);
    auto *ntlL = new QHBoxLayout(newTabSideLeft);
    ntlL->setContentsMargins(8, 0, 8, 0);
    ntlL->setSpacing(6);
    auto *plusIconL = new QLabel(QStringLiteral("+"), newTabSideLeft);
    plusIconL->setObjectName(QStringLiteral("miniNewTabSidePlus"));
    leftNewTabLabel_ = new QLabel(I18n::text(QStringLiteral("onboarding.preview_new_tab")), newTabSideLeft);
    leftNewTabLabel_->setObjectName(QStringLiteral("miniNewTabSideText"));
    ntlL->addWidget(plusIconL);
    ntlL->addWidget(leftNewTabLabel_, 1);
    leftLayout->addWidget(newTabSideLeft);

    leftLayout->addStretch(1);
    bodyLayout->addWidget(miniLeftPanel_);

    // Miniature Web Content
    miniContent_ = new QWidget(bodyContainer_);
    miniContent_->setObjectName(QStringLiteral("miniWebContent"));
    auto *contentLayout = new QVBoxLayout(miniContent_);
    contentLayout->setContentsMargins(16, 12, 16, 12);
    contentLayout->setSpacing(10);

    auto *heroBanner = new QFrame(miniContent_);
    heroBanner->setObjectName(QStringLiteral("miniHeroBanner"));
    heroBanner->setFixedHeight(54);
    auto *heroLayout = new QVBoxLayout(heroBanner);
    heroLayout->setContentsMargins(14, 6, 14, 6);
    heroLayout->setSpacing(2);
    heroText_ = new QLabel(I18n::text(QStringLiteral("onboarding.preview_hero_title")), heroBanner);
    heroText_->setObjectName(QStringLiteral("miniHeroText"));
    heroSub_ = new QLabel(I18n::text(QStringLiteral("onboarding.preview_hero_sub")), heroBanner);
    heroSub_->setObjectName(QStringLiteral("miniHeroSub"));
    heroLayout->addWidget(heroText_);
    heroLayout->addWidget(heroSub_);
    contentLayout->addWidget(heroBanner);

    // 3 Tiles
    auto *tilesRow = new QHBoxLayout();
    tilesRow->setSpacing(10);
    const auto makeTile = [](QWidget *parent, const QString &text, const QString &sub, const QString &accent, QLabel **outTitle = nullptr, QLabel **outSub = nullptr) {
      auto *tile = new QFrame(parent);
      tile->setObjectName(QStringLiteral("miniTile"));
      tile->setFixedHeight(46);
      auto *tl = new QVBoxLayout(tile);
      tl->setContentsMargins(10, 5, 10, 5);
      tl->setSpacing(2);
      auto *tLabel = new QLabel(text, tile);
      tLabel->setObjectName(QStringLiteral("miniTileTitle"));
      if (outTitle) *outTitle = tLabel;
      auto *sLabel = new QLabel(sub, tile);
      sLabel->setObjectName(QStringLiteral("miniTileSub"));
      if (outSub) *outSub = sLabel;
      auto *bar = new QFrame(tile);
      bar->setFixedHeight(3);
      bar->setStyleSheet(QStringLiteral("background: %1; border-radius: 1.5px;").arg(accent));
      tl->addWidget(tLabel);
      tl->addWidget(sLabel);
      tl->addWidget(bar);
      return tile;
    };
    tilesRow->addWidget(makeTile(miniContent_, I18n::text(QStringLiteral("onboarding.preview_tile_engine_title")), I18n::text(QStringLiteral("onboarding.preview_tile_engine_sub")), QStringLiteral("#3b82f6"), &tile1Title_, &tile1Sub_), 1);
    tilesRow->addWidget(makeTile(miniContent_, I18n::text(QStringLiteral("onboarding.preview_tile_eq_title")), I18n::text(QStringLiteral("onboarding.preview_tile_eq_sub")), QStringLiteral("#10b981"), &tile2Title_, &tile2Sub_), 1);
    tilesRow->addWidget(makeTile(miniContent_, I18n::text(QStringLiteral("onboarding.preview_tile_shield_title")), I18n::text(QStringLiteral("onboarding.preview_tile_shield_sub")), QStringLiteral("#8b5cf6"), &tile3Title_, &tile3Sub_), 1);
    contentLayout->addLayout(tilesRow);

    // Search bar simulation
    auto *searchMock = new QFrame(miniContent_);
    searchMock->setObjectName(QStringLiteral("miniSearchMock"));
    searchMock->setFixedHeight(30);
    auto *searchLayout = new QHBoxLayout(searchMock);
    searchLayout->setContentsMargins(10, 0, 10, 0);
    searchLabel_ = new QLabel(I18n::text(QStringLiteral("onboarding.preview_search_mock")), searchMock);
    searchLabel_->setObjectName(QStringLiteral("miniSearchText"));
    searchLayout->addWidget(searchLabel_);
    contentLayout->addWidget(searchMock);

    // 2 Simulated Web Feature Cards
    auto *webCardsRow = new QHBoxLayout();
    webCardsRow->setSpacing(10);
    const auto makeWebCard = [](QWidget *parent, const QString &tag, const QString &title, const QString &desc, const QString &tagCol,
                                QLabel **outBadge = nullptr, QLabel **outTitle = nullptr, QLabel **outDesc = nullptr) {
      auto *card = new QFrame(parent);
      card->setObjectName(QStringLiteral("miniWebCard"));
      card->setFixedHeight(52);
      auto *cl = new QVBoxLayout(card);
      cl->setContentsMargins(10, 6, 10, 6);
      cl->setSpacing(2);
      auto *topH = new QHBoxLayout();
      topH->setSpacing(6);
      auto *badge = new QLabel(tag, card);
      badge->setStyleSheet(QStringLiteral("background: %1; color: #ffffff; font-size: 7.5px; font-weight: 700; border-radius: 3px; padding: 1px 4px;").arg(tagCol));
      if (outBadge) *outBadge = badge;
      auto *tit = new QLabel(title, card);
      tit->setObjectName(QStringLiteral("miniWebCardTitle"));
      if (outTitle) *outTitle = tit;
      topH->addWidget(badge);
      topH->addWidget(tit, 1);
      cl->addLayout(topH);
      auto *d = new QLabel(desc, card);
      d->setObjectName(QStringLiteral("miniWebCardDesc"));
      if (outDesc) *outDesc = d;
      cl->addWidget(d);
      return card;
    };
    webCardsRow->addWidget(makeWebCard(miniContent_, I18n::text(QStringLiteral("onboarding.preview_card_ram_badge")), I18n::text(QStringLiteral("onboarding.preview_card_ram_title")), I18n::text(QStringLiteral("onboarding.preview_card_ram_desc")), QStringLiteral("#0284c7"), &webCard1Badge_, &webCard1Title_, &webCard1Desc_), 1);
    webCardsRow->addWidget(makeWebCard(miniContent_, I18n::text(QStringLiteral("onboarding.preview_card_dsp_badge")), I18n::text(QStringLiteral("onboarding.preview_card_dsp_title")), I18n::text(QStringLiteral("onboarding.preview_card_dsp_desc")), QStringLiteral("#059669"), &webCard2Badge_, &webCard2Title_, &webCard2Desc_), 1);
    contentLayout->addLayout(webCardsRow);

    contentLayout->addStretch(1);
    bodyLayout->addWidget(miniContent_, 1);

    // Miniature Right Panel (Docked)
    miniRightPanel_ = new QWidget(bodyContainer_);
    miniRightPanel_->setObjectName(QStringLiteral("miniSidePanelRight"));
    miniRightPanel_->setFixedWidth(150);
    auto *rightLayout = new QVBoxLayout(miniRightPanel_);
    rightLayout->setContentsMargins(8, 8, 8, 8);
    rightLayout->setSpacing(5);
    rightHeader_ = new QLabel(I18n::text(QStringLiteral("onboarding.preview_side_header_right")), miniRightPanel_);
    rightHeader_->setObjectName(QStringLiteral("miniSideHeader"));
    rightLayout->addWidget(rightHeader_);
    rightLayout->addWidget(makeSideTab(miniRightPanel_, I18n::text(QStringLiteral("onboarding.preview_tab_home")), true, &rightTabHome_));
    rightLayout->addWidget(makeSideTab(miniRightPanel_, I18n::text(QStringLiteral("onboarding.preview_tab_docs")), false, &rightTabDocs_));
    rightLayout->addWidget(makeSideTab(miniRightPanel_, I18n::text(QStringLiteral("onboarding.preview_tab_music")), false, &rightTabMusic_));
    rightLayout->addWidget(makeSideTab(miniRightPanel_, I18n::text(QStringLiteral("onboarding.preview_tab_downloads")), false, &rightTabDownloads_));
    rightLayout->addWidget(makeSideTab(miniRightPanel_, I18n::text(QStringLiteral("onboarding.preview_tab_bookmarks")), false, &rightTabBookmarks_));

    auto *newTabSideRight = new QFrame(miniRightPanel_);
    newTabSideRight->setObjectName(QStringLiteral("miniNewTabSide"));
    newTabSideRight->setFixedHeight(22);
    auto *ntlR = new QHBoxLayout(newTabSideRight);
    ntlR->setContentsMargins(8, 0, 8, 0);
    ntlR->setSpacing(6);
    auto *plusIconR = new QLabel(QStringLiteral("+"), newTabSideRight);
    plusIconR->setObjectName(QStringLiteral("miniNewTabSidePlus"));
    rightNewTabLabel_ = new QLabel(I18n::text(QStringLiteral("onboarding.preview_new_tab")), newTabSideRight);
    rightNewTabLabel_->setObjectName(QStringLiteral("miniNewTabSideText"));
    ntlR->addWidget(plusIconR);
    ntlR->addWidget(rightNewTabLabel_, 1);
    rightLayout->addWidget(newTabSideRight);

    rightLayout->addStretch(1);
    bodyLayout->addWidget(miniRightPanel_);

    // 4. Miniature Floating Panel (Zen Browser-style floating overlay)
    miniFloatingPanel_ = new QWidget(bodyContainer_);
    miniFloatingPanel_->setObjectName(QStringLiteral("miniFloatingPanel"));
    miniFloatingPanel_->setFixedWidth(175);
    auto *flLayout = new QVBoxLayout(miniFloatingPanel_);
    flLayout->setContentsMargins(10, 10, 10, 10);
    flLayout->setSpacing(6);

    flBadge_ = new QLabel(I18n::text(QStringLiteral("onboarding.preview_floating_badge")), miniFloatingPanel_);
    flBadge_->setObjectName(QStringLiteral("miniFloatingBadge"));
    flLayout->addWidget(flBadge_);

    flSub_ = new QLabel(I18n::text(QStringLiteral("onboarding.preview_floating_sub")), miniFloatingPanel_);
    flSub_->setObjectName(QStringLiteral("miniFloatingSub"));
    flLayout->addWidget(flSub_);

    flLayout->addWidget(makeSideTab(miniFloatingPanel_, I18n::text(QStringLiteral("onboarding.preview_tab_home")), true, &flTabHome_));
    flLayout->addWidget(makeSideTab(miniFloatingPanel_, I18n::text(QStringLiteral("onboarding.preview_tab_docs")), false, &flTabDocs_));
    flLayout->addWidget(makeSideTab(miniFloatingPanel_, I18n::text(QStringLiteral("onboarding.preview_tab_music")), false, &flTabMusic_));
    flLayout->addWidget(makeSideTab(miniFloatingPanel_, I18n::text(QStringLiteral("onboarding.preview_tab_downloads")), false, &flTabDownloads_));
    flLayout->addWidget(makeSideTab(miniFloatingPanel_, I18n::text(QStringLiteral("onboarding.preview_tab_bookmarks")), false, &flTabBookmarks_));

    auto *newTabFloating = new QFrame(miniFloatingPanel_);
    newTabFloating->setObjectName(QStringLiteral("miniNewTabSide"));
    newTabFloating->setFixedHeight(22);
    auto *ntfLayout = new QHBoxLayout(newTabFloating);
    ntfLayout->setContentsMargins(8, 0, 8, 0);
    ntfLayout->setSpacing(6);
    auto *plusIconF = new QLabel(QStringLiteral("+"), newTabFloating);
    plusIconF->setObjectName(QStringLiteral("miniNewTabSidePlus"));
    flNewTabLabel_ = new QLabel(I18n::text(QStringLiteral("onboarding.preview_new_tab")), newTabFloating);
    flNewTabLabel_->setObjectName(QStringLiteral("miniNewTabSideText"));
    ntfLayout->addWidget(plusIconF);
    ntfLayout->addWidget(flNewTabLabel_, 1);
    flLayout->addWidget(newTabFloating);

    flHint_ = new QLabel(I18n::text(QStringLiteral("onboarding.preview_floating_hint")), miniFloatingPanel_);
    flHint_->setObjectName(QStringLiteral("miniFloatingHint"));
    flLayout->addWidget(flHint_);

    flLayout->addStretch(1);
    miniFloatingPanel_->hide();

    frameLayout->addWidget(bodyContainer_, 1);
    rootLayout->addWidget(windowFrame_);

    leftOpacity_ = new QGraphicsOpacityEffect(miniLeftPanel_);
    miniLeftPanel_->setGraphicsEffect(leftOpacity_);

    rightOpacity_ = new QGraphicsOpacityEffect(miniRightPanel_);
    miniRightPanel_->setGraphicsEffect(rightOpacity_);

    updateTheme(dalinira::layout::isDarkAppearance(dalinira::layout::currentAppearanceTheme()));
    setLayoutMode(dalinira::layout::BrowserLayoutMode::Classic, false);
    retranslateUi();
  }

  void resizeEvent(QResizeEvent *event) override {
    QWidget::resizeEvent(event);
    if (currentMode_ == dalinira::layout::BrowserLayoutMode::FloatingSidebar) {
      updateFloatingPanelGeometry(false);
    }
  }

  void mouseMoveEvent(QMouseEvent *event) override {
    QWidget::mouseMoveEvent(event);
    if (currentMode_ == dalinira::layout::BrowserLayoutMode::FloatingSidebar && miniFloatingPanel_ && bodyContainer_) {
      const QPoint posInBody = bodyContainer_->mapFrom(this, event->pos());
      if (bodyContainer_->rect().contains(posInBody)) {
        if (!floatingPanelShown_ && posInBody.x() <= 24) {
          floatingPanelShown_ = true;
          updateFloatingPanelGeometry(true);
        }
      }
    }
  }

  void setLayoutMode(dalinira::layout::BrowserLayoutMode mode, bool animated) {
    currentMode_ = mode;
    updateModeText();

    if (mode == dalinira::layout::BrowserLayoutMode::Classic) {
      // Classic: Top horizontal tabs are shown, all side panels are hidden
      miniHorizontalTabsContainer_->show();
      miniTitlebarLabel_->hide();

      if (!animated) {
        miniLeftPanel_->hide();
        miniRightPanel_->hide();
        updateFloatingPanelGeometry(false);
      } else {
        auto *fadeLeft = new QPropertyAnimation(leftOpacity_, "opacity", this);
        fadeLeft->setDuration(160);
        fadeLeft->setStartValue(leftOpacity_->opacity());
        fadeLeft->setEndValue(0.0);
        connect(fadeLeft, &QPropertyAnimation::finished, miniLeftPanel_, &QWidget::hide);
        fadeLeft->start(QAbstractAnimation::DeleteWhenStopped);

        auto *fadeRight = new QPropertyAnimation(rightOpacity_, "opacity", this);
        fadeRight->setDuration(160);
        fadeRight->setStartValue(rightOpacity_->opacity());
        fadeRight->setEndValue(0.0);
        connect(fadeRight, &QPropertyAnimation::finished, miniRightPanel_, &QWidget::hide);
        fadeRight->start(QAbstractAnimation::DeleteWhenStopped);

        updateFloatingPanelGeometry(true);
      }
    } else if (mode == dalinira::layout::BrowserLayoutMode::VerticalTabsLeft) {
      // Sol Panel: Top horizontal tabs are HIDDEN, left panel is shown
      miniHorizontalTabsContainer_->hide();
      miniTitlebarLabel_->show();

      miniRightPanel_->hide();
      updateFloatingPanelGeometry(false);
      miniLeftPanel_->show();

      if (!animated) {
        leftOpacity_->setOpacity(1.0);
      } else {
        auto *fadeLeft = new QPropertyAnimation(leftOpacity_, "opacity", this);
        fadeLeft->setDuration(200);
        fadeLeft->setStartValue(0.0);
        fadeLeft->setEndValue(1.0);
        fadeLeft->start(QAbstractAnimation::DeleteWhenStopped);
      }
    } else if (mode == dalinira::layout::BrowserLayoutMode::VerticalTabsRight) {
      // Sağ Panel: Top horizontal tabs are HIDDEN, right panel is shown
      miniHorizontalTabsContainer_->hide();
      miniTitlebarLabel_->show();

      miniLeftPanel_->hide();
      updateFloatingPanelGeometry(false);
      miniRightPanel_->show();

      if (!animated) {
        rightOpacity_->setOpacity(1.0);
      } else {
        auto *fadeRight = new QPropertyAnimation(rightOpacity_, "opacity", this);
        fadeRight->setDuration(200);
        fadeRight->setStartValue(0.0);
        fadeRight->setEndValue(1.0);
        fadeRight->start(QAbstractAnimation::DeleteWhenStopped);
      }
    } else if (mode == dalinira::layout::BrowserLayoutMode::FloatingSidebar) {
      // Yüzen Panel: Top horizontal tabs are HIDDEN, docked side panels are HIDDEN,
      // and the floating panel smoothly slides over the full-width web content!
      miniHorizontalTabsContainer_->hide();
      miniTitlebarLabel_->show();

      miniLeftPanel_->hide();
      miniRightPanel_->hide();
      floatingPanelShown_ = true;
      updateFloatingPanelGeometry(animated);
    }
  }

  void updateModeText() {
    if (!liveBadge_) return;
    if (currentMode_ == dalinira::layout::BrowserLayoutMode::Classic) {
      liveBadge_->setText(I18n::text(QStringLiteral("onboarding.preview_badge_classic")));
      if (miniTitlebarLabel_) {
        miniTitlebarLabel_->setText(I18n::text(QStringLiteral("onboarding.preview_title_classic")));
      }
    } else if (currentMode_ == dalinira::layout::BrowserLayoutMode::VerticalTabsLeft) {
      liveBadge_->setText(I18n::text(QStringLiteral("onboarding.preview_badge_left")));
      if (miniTitlebarLabel_) {
        miniTitlebarLabel_->setText(I18n::text(QStringLiteral("onboarding.preview_title_left")));
      }
    } else if (currentMode_ == dalinira::layout::BrowserLayoutMode::VerticalTabsRight) {
      liveBadge_->setText(I18n::text(QStringLiteral("onboarding.preview_badge_right")));
      if (miniTitlebarLabel_) {
        miniTitlebarLabel_->setText(I18n::text(QStringLiteral("onboarding.preview_title_right")));
      }
    } else if (currentMode_ == dalinira::layout::BrowserLayoutMode::FloatingSidebar) {
      liveBadge_->setText(I18n::text(QStringLiteral("onboarding.preview_badge_floating")));
      if (miniTitlebarLabel_) {
        miniTitlebarLabel_->setText(I18n::text(QStringLiteral("onboarding.preview_title_floating")));
      }
    }
  }

  void retranslateUi() {
    updateModeText();

    if (tab1Title_) {
      tab1Title_->setText(I18n::text(QStringLiteral("onboarding.preview_title_classic")));
    }

    if (leftHeader_) leftHeader_->setText(I18n::text(QStringLiteral("onboarding.preview_side_header_left")));
    if (leftTabHome_) leftTabHome_->setText(I18n::text(QStringLiteral("onboarding.preview_tab_home")));
    if (leftTabDocs_) leftTabDocs_->setText(I18n::text(QStringLiteral("onboarding.preview_tab_docs")));
    if (leftTabMusic_) leftTabMusic_->setText(I18n::text(QStringLiteral("onboarding.preview_tab_music")));
    if (leftTabDownloads_) leftTabDownloads_->setText(I18n::text(QStringLiteral("onboarding.preview_tab_downloads")));
    if (leftTabBookmarks_) leftTabBookmarks_->setText(I18n::text(QStringLiteral("onboarding.preview_tab_bookmarks")));
    if (leftNewTabLabel_) leftNewTabLabel_->setText(I18n::text(QStringLiteral("onboarding.preview_new_tab")));

    if (rightHeader_) rightHeader_->setText(I18n::text(QStringLiteral("onboarding.preview_side_header_right")));
    if (rightTabHome_) rightTabHome_->setText(I18n::text(QStringLiteral("onboarding.preview_tab_home")));
    if (rightTabDocs_) rightTabDocs_->setText(I18n::text(QStringLiteral("onboarding.preview_tab_docs")));
    if (rightTabMusic_) rightTabMusic_->setText(I18n::text(QStringLiteral("onboarding.preview_tab_music")));
    if (rightTabDownloads_) rightTabDownloads_->setText(I18n::text(QStringLiteral("onboarding.preview_tab_downloads")));
    if (rightTabBookmarks_) rightTabBookmarks_->setText(I18n::text(QStringLiteral("onboarding.preview_tab_bookmarks")));
    if (rightNewTabLabel_) rightNewTabLabel_->setText(I18n::text(QStringLiteral("onboarding.preview_new_tab")));

    if (flBadge_) flBadge_->setText(I18n::text(QStringLiteral("onboarding.preview_floating_badge")));
    if (flSub_) flSub_->setText(I18n::text(QStringLiteral("onboarding.preview_floating_sub")));
    if (flTabHome_) flTabHome_->setText(I18n::text(QStringLiteral("onboarding.preview_tab_home")));
    if (flTabDocs_) flTabDocs_->setText(I18n::text(QStringLiteral("onboarding.preview_tab_docs")));
    if (flTabMusic_) flTabMusic_->setText(I18n::text(QStringLiteral("onboarding.preview_tab_music")));
    if (flTabDownloads_) flTabDownloads_->setText(I18n::text(QStringLiteral("onboarding.preview_tab_downloads")));
    if (flTabBookmarks_) flTabBookmarks_->setText(I18n::text(QStringLiteral("onboarding.preview_tab_bookmarks")));
    if (flNewTabLabel_) flNewTabLabel_->setText(I18n::text(QStringLiteral("onboarding.preview_new_tab")));
    if (flHint_) flHint_->setText(I18n::text(QStringLiteral("onboarding.preview_floating_hint")));

    if (heroText_) heroText_->setText(I18n::text(QStringLiteral("onboarding.preview_hero_title")));
    if (heroSub_) heroSub_->setText(I18n::text(QStringLiteral("onboarding.preview_hero_sub")));

    if (tile1Title_) tile1Title_->setText(I18n::text(QStringLiteral("onboarding.preview_tile_engine_title")));
    if (tile1Sub_) tile1Sub_->setText(I18n::text(QStringLiteral("onboarding.preview_tile_engine_sub")));
    if (tile2Title_) tile2Title_->setText(I18n::text(QStringLiteral("onboarding.preview_tile_eq_title")));
    if (tile2Sub_) tile2Sub_->setText(I18n::text(QStringLiteral("onboarding.preview_tile_eq_sub")));
    if (tile3Title_) tile3Title_->setText(I18n::text(QStringLiteral("onboarding.preview_tile_shield_title")));
    if (tile3Sub_) tile3Sub_->setText(I18n::text(QStringLiteral("onboarding.preview_tile_shield_sub")));

    if (searchLabel_) searchLabel_->setText(I18n::text(QStringLiteral("onboarding.preview_search_mock")));

    if (webCard1Badge_) webCard1Badge_->setText(I18n::text(QStringLiteral("onboarding.preview_card_ram_badge")));
    if (webCard1Title_) webCard1Title_->setText(I18n::text(QStringLiteral("onboarding.preview_card_ram_title")));
    if (webCard1Desc_) webCard1Desc_->setText(I18n::text(QStringLiteral("onboarding.preview_card_ram_desc")));

    if (webCard2Badge_) webCard2Badge_->setText(I18n::text(QStringLiteral("onboarding.preview_card_dsp_badge")));
    if (webCard2Title_) webCard2Title_->setText(I18n::text(QStringLiteral("onboarding.preview_card_dsp_title")));
    if (webCard2Desc_) webCard2Desc_->setText(I18n::text(QStringLiteral("onboarding.preview_card_dsp_desc")));
  }

  void setTheme(dalinira::layout::AppearanceTheme theme) {
    currentTheme_ = theme;
    const bool dark = dalinira::layout::isDarkAppearance(theme);
    updateTheme(dark);
  }

  void updateTheme(bool dark) {
    setStyleSheet(getStyleSheet(dark));
    style()->unpolish(this);
    style()->polish(this);
    for (auto *child : findChildren<QWidget *>()) {
      child->style()->unpolish(child);
      child->style()->polish(child);
      child->update();
    }
    update();
  }

 private:
  void updateFloatingPanelGeometry(bool animated) {
    if (!miniFloatingPanel_ || !bodyContainer_) return;
    const int panelW = 175;
    const int panelH = qMax(120, bodyContainer_->height() - 16);
    const QRect shown(10, 8, panelW, panelH);
    const QRect hidden(-panelW - 10, 8, panelW, panelH);

    if (currentMode_ == dalinira::layout::BrowserLayoutMode::FloatingSidebar && floatingPanelShown_) {
      miniFloatingPanel_->show();
      miniFloatingPanel_->raise();
      if (!animated) {
        miniFloatingPanel_->setGeometry(shown);
      } else {
        auto *anim = new QPropertyAnimation(miniFloatingPanel_, "geometry", this);
        anim->setDuration(240);
        anim->setEasingCurve(QEasingCurve::OutCubic);
        anim->setStartValue(miniFloatingPanel_->geometry().isValid() && miniFloatingPanel_->geometry().x() < 0
                                ? miniFloatingPanel_->geometry()
                                : hidden);
        anim->setEndValue(shown);
        anim->start(QAbstractAnimation::DeleteWhenStopped);
      }
    } else {
      if (!animated) {
        miniFloatingPanel_->setGeometry(hidden);
        miniFloatingPanel_->hide();
      } else if (miniFloatingPanel_->isVisible()) {
        auto *anim = new QPropertyAnimation(miniFloatingPanel_, "geometry", this);
        anim->setDuration(180);
        anim->setEasingCurve(QEasingCurve::InCubic);
        anim->setStartValue(miniFloatingPanel_->geometry());
        anim->setEndValue(hidden);
        connect(anim, &QPropertyAnimation::finished, miniFloatingPanel_, &QWidget::hide);
        anim->start(QAbstractAnimation::DeleteWhenStopped);
      }
    }
  }

  static QString getStyleSheet(bool dark) {
    if (dark) {
      return QStringLiteral(
          "#miniWindowFrame {"
          "  background: #0b111a;"
          "  border: 1px solid rgba(255, 255, 255, 0.18);"
          "  border-radius: 12px;"
          "}"
          "#miniWindowFrame QLabel { background: transparent; border: none; }"
          "#miniTitlebar {"
          "  background: #111827;"
          "  border-top-left-radius: 11px;"
          "  border-top-right-radius: 11px;"
          "  border-bottom: 1px solid rgba(255, 255, 255, 0.08);"
          "}"
          "#miniTabActive {"
          "  background: #1e293b;"
          "  border-radius: 6px;"
          "  border-bottom: 2px solid #38bdf8;"
          "}"
          "#miniTabActiveDot { background: #38bdf8; border-radius: 4px; }"
          "#miniTabActiveTitle { color: #ffffff; font-size: 11px; font-weight: 600; }"
          "#miniTabActiveClose { color: #94a3b8; font-size: 12px; }"
          "#miniTabInactive {"
          "  background: rgba(255, 255, 255, 0.04);"
          "  border-radius: 6px;"
          "}"
          "#miniTabInactiveTitle { color: #94a3b8; font-size: 10.5px; }"
          "#miniNewTabBtn { color: #64748b; font-size: 14px; font-weight: bold; padding-left: 4px; }"
          "#miniTitlebarLabel { color: #94a3b8; font-size: 11px; font-weight: 600; padding-left: 6px; }"
          "#miniLiveBadge {"
          "  background: rgba(16, 185, 129, 0.15);"
          "  color: #34d399;"
          "  font-size: 9.5px;"
          "  font-weight: 700;"
          "  border-radius: 5px;"
          "  padding: 2px 8px;"
          "  border: 1px solid rgba(16, 185, 129, 0.35);"
          "}"
          "#miniNavBar {"
          "  background: #0f172a;"
          "  border-bottom: 1px solid rgba(255, 255, 255, 0.08);"
          "}"
          "#miniNavBtn { color: #94a3b8; font-size: 12px; font-weight: bold; }"
          "#miniOmnibox {"
          "  background: rgba(255, 255, 255, 0.06);"
          "  border: 1px solid rgba(255, 255, 255, 0.12);"
          "  border-radius: 6px;"
          "}"
          "#miniOmniboxUrl { color: #e2e8f0; font-size: 10.5px; font-family: monospace; }"
          "#miniBodyContainer {"
          "  background: #090e17;"
          "  border-bottom-left-radius: 11px;"
          "  border-bottom-right-radius: 11px;"
          "}"
          "#miniSidePanelLeft {"
          "  background: #0f172a;"
          "  border-right: 1px solid rgba(255, 255, 255, 0.1);"
          "}"
          "#miniSidePanelRight {"
          "  background: #0f172a;"
          "  border-left: 1px solid rgba(255, 255, 255, 0.1);"
          "}"
          "#miniSideHeader { color: #60a5fa; font-size: 10px; font-weight: 700; margin-bottom: 2px; }"
          "#miniSideTabActive {"
          "  background: rgba(56, 189, 248, 0.22);"
          "  border-radius: 5px;"
          "  border: 1px solid rgba(56, 189, 248, 0.4);"
          "}"
          "#miniSideTabActiveDot { background: #38bdf8; border-radius: 3px; }"
          "#miniSideTabActiveText { color: #ffffff; font-size: 10px; font-weight: 600; }"
          "#miniSideTabInactive {"
          "  background: rgba(255, 255, 255, 0.04);"
          "  border-radius: 5px;"
          "  border: none;"
          "}"
          "#miniSideTabInactiveDot { background: #64748b; border-radius: 3px; }"
          "#miniSideTabInactiveText { color: #94a3b8; font-size: 10px; font-weight: 400; }"
          "#miniNewTabSide {"
          "  background: rgba(255, 255, 255, 0.03);"
          "  border: 1px dashed rgba(255, 255, 255, 0.15);"
          "  border-radius: 4px;"
          "}"
          "#miniNewTabSidePlus { color: #60a5fa; font-size: 12px; font-weight: bold; }"
          "#miniNewTabSideText { color: #94a3b8; font-size: 9px; }"
          "#miniWebContent { background: #090e17; }"
          "#miniHeroBanner {"
          "  background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 rgba(37, 99, 235, 0.28), stop:1 rgba(6, 182, 212, 0.20));"
          "  border: 1px solid rgba(147, 197, 253, 0.30);"
          "  border-radius: 8px;"
          "}"
          "#miniHeroText { color: #ffffff; font-size: 12px; font-weight: 700; }"
          "#miniHeroSub { color: #94a3b8; font-size: 9.5px; }"
          "#miniTile {"
          "  background: rgba(255, 255, 255, 0.05);"
          "  border: 1px solid rgba(255, 255, 255, 0.1);"
          "  border-radius: 6px;"
          "}"
          "#miniTileTitle { color: #f1f5f9; font-size: 10px; font-weight: 600; }"
          "#miniTileSub { color: #94a3b8; font-size: 8.5px; }"
          "#miniSearchMock {"
          "  background: rgba(255, 255, 255, 0.04);"
          "  border: 1px solid rgba(255, 255, 255, 0.08);"
          "  border-radius: 6px;"
          "}"
          "#miniSearchText { color: #64748b; font-size: 9.5px; }"
          "#miniWebCard {"
          "  background: rgba(255, 255, 255, 0.03);"
          "  border: 1px solid rgba(255, 255, 255, 0.07);"
          "  border-radius: 6px;"
          "}"
          "#miniWebCardTitle { color: #e2e8f0; font-size: 9.5px; font-weight: 600; }"
          "#miniWebCardDesc { color: #94a3b8; font-size: 8.5px; }"
          "#miniFloatingPanel {"
          "  background: rgba(11, 17, 28, 0.96);"
          "  border: 1.5px solid #38bdf8;"
          "  border-radius: 10px;"
          "}"
          "#miniFloatingBadge {"
          "  color: #38bdf8;"
          "  font-size: 9.5px;"
          "  font-weight: 700;"
          "  background: rgba(56, 189, 248, 0.18);"
          "  border-radius: 4px;"
          "  padding: 2px 6px;"
          "  border: 1px solid rgba(56, 189, 248, 0.4);"
          "}"
          "#miniFloatingSub { color: #94a3b8; font-size: 8.5px; }"
          "#miniFloatingHint { color: #38bdf8; font-size: 8px; font-style: italic; margin-top: 4px; }");
    } else {
      return QStringLiteral(
          "#miniWindowFrame {"
          "  background: #ffffff;"
          "  border: 1px solid #cbd5e1;"
          "  border-radius: 12px;"
          "}"
          "#miniWindowFrame QLabel { background: transparent; border: none; }"
          "#miniTitlebar {"
          "  background: #e2e8f0;"
          "  border-top-left-radius: 11px;"
          "  border-top-right-radius: 11px;"
          "  border-bottom: 1px solid #cbd5e1;"
          "}"
          "#miniTabActive {"
          "  background: #ffffff;"
          "  border-radius: 6px;"
          "  border-bottom: 2px solid #0284c7;"
          "}"
          "#miniTabActiveDot { background: #0284c7; border-radius: 4px; }"
          "#miniTabActiveTitle { color: #0f172a; font-size: 11px; font-weight: 600; }"
          "#miniTabActiveClose { color: #64748b; font-size: 12px; }"
          "#miniTabInactive {"
          "  background: rgba(0, 0, 0, 0.04);"
          "  border-radius: 6px;"
          "}"
          "#miniTabInactiveTitle { color: #64748b; font-size: 10.5px; }"
          "#miniNewTabBtn { color: #475569; font-size: 14px; font-weight: bold; padding-left: 4px; }"
          "#miniTitlebarLabel { color: #334155; font-size: 11px; font-weight: 600; padding-left: 6px; }"
          "#miniLiveBadge {"
          "  background: rgba(16, 185, 129, 0.15);"
          "  color: #059669;"
          "  font-size: 9.5px;"
          "  font-weight: 700;"
          "  border-radius: 5px;"
          "  padding: 2px 8px;"
          "  border: 1px solid rgba(16, 185, 129, 0.4);"
          "}"
          "#miniNavBar {"
          "  background: #f1f5f9;"
          "  border-bottom: 1px solid #e2e8f0;"
          "}"
          "#miniNavBtn { color: #475569; font-size: 12px; font-weight: bold; }"
          "#miniOmnibox {"
          "  background: #ffffff;"
          "  border: 1px solid #cbd5e1;"
          "  border-radius: 6px;"
          "}"
          "#miniOmniboxUrl { color: #0f172a; font-size: 10.5px; font-family: monospace; }"
          "#miniBodyContainer {"
          "  background: #f8fafc;"
          "  border-bottom-left-radius: 11px;"
          "  border-bottom-right-radius: 11px;"
          "}"
          "#miniSidePanelLeft {"
          "  background: #f1f5f9;"
          "  border-right: 1px solid #e2e8f0;"
          "}"
          "#miniSidePanelRight {"
          "  background: #f1f5f9;"
          "  border-left: 1px solid #e2e8f0;"
          "}"
          "#miniSideHeader { color: #0284c7; font-size: 10px; font-weight: 700; margin-bottom: 2px; }"
          "#miniSideTabActive {"
          "  background: rgba(2, 132, 199, 0.15);"
          "  border-radius: 5px;"
          "  border: 1px solid rgba(2, 132, 199, 0.4);"
          "}"
          "#miniSideTabActiveDot { background: #0284c7; border-radius: 3px; }"
          "#miniSideTabActiveText { color: #0369a1; font-size: 10px; font-weight: 600; }"
          "#miniSideTabInactive {"
          "  background: rgba(0, 0, 0, 0.03);"
          "  border-radius: 5px;"
          "  border: none;"
          "}"
          "#miniSideTabInactiveDot { background: #94a3b8; border-radius: 3px; }"
          "#miniSideTabInactiveText { color: #475569; font-size: 10px; font-weight: 400; }"
          "#miniNewTabSide {"
          "  background: rgba(0, 0, 0, 0.02);"
          "  border: 1px dashed #cbd5e1;"
          "  border-radius: 4px;"
          "}"
          "#miniNewTabSidePlus { color: #0284c7; font-size: 12px; font-weight: bold; }"
          "#miniNewTabSideText { color: #64748b; font-size: 9px; }"
          "#miniWebContent { background: #f8fafc; }"
          "#miniHeroBanner {"
          "  background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 rgba(37, 99, 235, 0.12), stop:1 rgba(6, 182, 212, 0.08));"
          "  border: 1px solid rgba(37, 99, 235, 0.25);"
          "  border-radius: 8px;"
          "}"
          "#miniHeroText { color: #0f172a; font-size: 12px; font-weight: 700; }"
          "#miniHeroSub { color: #475569; font-size: 9.5px; }"
          "#miniTile {"
          "  background: #ffffff;"
          "  border: 1px solid #e2e8f0;"
          "  border-radius: 6px;"
          "}"
          "#miniTileTitle { color: #0f172a; font-size: 10px; font-weight: 600; }"
          "#miniTileSub { color: #64748b; font-size: 8.5px; }"
          "#miniSearchMock {"
          "  background: #ffffff;"
          "  border: 1px solid #cbd5e1;"
          "  border-radius: 6px;"
          "}"
          "#miniSearchText { color: #64748b; font-size: 9.5px; }"
          "#miniWebCard {"
          "  background: #ffffff;"
          "  border: 1px solid #e2e8f0;"
          "  border-radius: 6px;"
          "}"
          "#miniWebCardTitle { color: #0f172a; font-size: 9.5px; font-weight: 600; }"
          "#miniWebCardDesc { color: #475569; font-size: 8.5px; }"
          "#miniFloatingPanel {"
          "  background: rgba(255, 255, 255, 0.97);"
          "  border: 1.5px solid #0284c7;"
          "  border-radius: 10px;"
          "}"
          "#miniFloatingBadge {"
          "  color: #0284c7;"
          "  font-size: 9.5px;"
          "  font-weight: 700;"
          "  background: rgba(2, 132, 199, 0.12);"
          "  border-radius: 4px;"
          "  padding: 2px 6px;"
          "  border: 1px solid rgba(2, 132, 199, 0.35);"
          "}"
          "#miniFloatingSub { color: #64748b; font-size: 8.5px; }"
          "#miniFloatingHint { color: #0284c7; font-size: 8px; font-style: italic; margin-top: 4px; }");
    }
  }

  QFrame *windowFrame_ = nullptr;
  QWidget *miniHorizontalTabsContainer_ = nullptr;
  QLabel *tab1Title_ = nullptr;
  QLabel *miniTitlebarLabel_ = nullptr;
  QLabel *liveBadge_ = nullptr;
  QWidget *bodyContainer_ = nullptr;
  QWidget *miniLeftPanel_ = nullptr;
  QWidget *miniContent_ = nullptr;
  QWidget *miniRightPanel_ = nullptr;
  QWidget *miniFloatingPanel_ = nullptr;
  QGraphicsOpacityEffect *leftOpacity_ = nullptr;
  QGraphicsOpacityEffect *rightOpacity_ = nullptr;

  QLabel *leftHeader_ = nullptr;
  QLabel *leftTabHome_ = nullptr;
  QLabel *leftTabDocs_ = nullptr;
  QLabel *leftTabMusic_ = nullptr;
  QLabel *leftTabDownloads_ = nullptr;
  QLabel *leftTabBookmarks_ = nullptr;
  QLabel *leftNewTabLabel_ = nullptr;

  QLabel *rightHeader_ = nullptr;
  QLabel *rightTabHome_ = nullptr;
  QLabel *rightTabDocs_ = nullptr;
  QLabel *rightTabMusic_ = nullptr;
  QLabel *rightTabDownloads_ = nullptr;
  QLabel *rightTabBookmarks_ = nullptr;
  QLabel *rightNewTabLabel_ = nullptr;

  QLabel *flBadge_ = nullptr;
  QLabel *flSub_ = nullptr;
  QLabel *flTabHome_ = nullptr;
  QLabel *flTabDocs_ = nullptr;
  QLabel *flTabMusic_ = nullptr;
  QLabel *flTabDownloads_ = nullptr;
  QLabel *flTabBookmarks_ = nullptr;
  QLabel *flNewTabLabel_ = nullptr;
  QLabel *flHint_ = nullptr;

  QLabel *heroText_ = nullptr;
  QLabel *heroSub_ = nullptr;
  QLabel *tile1Title_ = nullptr;
  QLabel *tile1Sub_ = nullptr;
  QLabel *tile2Title_ = nullptr;
  QLabel *tile2Sub_ = nullptr;
  QLabel *tile3Title_ = nullptr;
  QLabel *tile3Sub_ = nullptr;
  QLabel *searchLabel_ = nullptr;
  QLabel *webCard1Badge_ = nullptr;
  QLabel *webCard1Title_ = nullptr;
  QLabel *webCard1Desc_ = nullptr;
  QLabel *webCard2Badge_ = nullptr;
  QLabel *webCard2Title_ = nullptr;
  QLabel *webCard2Desc_ = nullptr;

  bool floatingPanelShown_{true};
  dalinira::layout::BrowserLayoutMode currentMode_{dalinira::layout::BrowserLayoutMode::Classic};
  dalinira::layout::AppearanceTheme currentTheme_{dalinira::layout::AppearanceTheme::System};
};

LayoutStep::LayoutStep(QWidget *parent)
    : OnboardingStep(parent) {
  setupUi();
  connect(&dalinira::i18n::LanguageManager::instance(), &dalinira::i18n::LanguageManager::languageChanged,
          this, &LayoutStep::retranslateUi);
}

QString LayoutStep::stepTitle() const {
  return I18n::text(QStringLiteral("onboarding.layout_title"));
}

QString LayoutStep::stepSubtitle() const {
  return I18n::text(QStringLiteral("onboarding.layout_subtitle"));
}

QString LayoutStep::nextButtonText() const {
  // Search and privacy and the summary still follow: a plain "Next".
  return OnboardingStep::nextButtonText();
}

QString LayoutStep::backButtonText() const {
  return I18n::text(QStringLiteral("onboarding.back"));
}

void LayoutStep::setupUi() {
  auto *rootLayout = new QVBoxLayout(this);
  rootLayout->setContentsMargins(24, 8, 24, 12);
  rootLayout->setSpacing(0);
  rootLayout->setAlignment(Qt::AlignHCenter | Qt::AlignTop);

  // 1. Live Miniature Layout Preview (Aşağıya doğru uzatıldı: 380px)
  previewWidget_ = new LayoutPreviewWidget(this);
  rootLayout->addWidget(previewWidget_, 0, Qt::AlignHCenter);

  rootLayout->addSpacing(22);

  // 2. Selection Cards Row (Alta indirildi: sabit 116px yükseklik ve aşağı konum)
  auto *cardsRow = new QWidget(this);
  cardsRow->setObjectName(QStringLiteral("layoutCardsRow"));
  cardsRow->setMaximumWidth(860);
  cardsRow->setFixedHeight(116);
  auto *cardsLayout = new QHBoxLayout(cardsRow);
  cardsLayout->setContentsMargins(0, 0, 0, 0);
  cardsLayout->setSpacing(12);

  const auto createLayoutCard = [cardsRow](const QString &objName,
                                           QRadioButton **outRadio,
                                           QLabel **outTitle,
                                           QLabel **outBadge,
                                           QLabel **outDesc) {
    auto *card = new ClickableLayoutCard(cardsRow);
    card->setObjectName(objName);
    card->setFrameShape(QFrame::StyledPanel);
    card->setFixedHeight(116);
    card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto *cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(12, 10, 12, 10);
    cardLayout->setSpacing(4);

    // Row 1: Modern Radio Dot (diameter 18) on left, Badge on right
    auto *topRow = new QHBoxLayout();
    topRow->setContentsMargins(0, 0, 0, 0);

    auto *radio = new ModernRadioButton(card, 18);
    *outRadio = radio;
    topRow->addWidget(radio);

    topRow->addStretch(1);

    auto *badge = new QLabel(card);
    badge->setObjectName(QStringLiteral("layoutCardBadge"));
    *outBadge = badge;
    topRow->addWidget(badge);

    cardLayout->addLayout(topRow);

    // Row 2: Title (Full width available)
    auto *title = new QLabel(card);
    title->setObjectName(QStringLiteral("layoutCardTitle"));
    title->setStyleSheet(QStringLiteral(
        "font-size: 14px; font-weight: 700; color: #ffffff; background: transparent; border: none;"));
    *outTitle = title;
    cardLayout->addWidget(title);

    // Row 3: Description
    auto *desc = new QLabel(card);
    desc->setObjectName(QStringLiteral("layoutCardDesc"));
    desc->setStyleSheet(QStringLiteral(
        "font-size: 11px; font-weight: 400; color: #cbd5e1; background: transparent; border: none;"));
    desc->setWordWrap(true);
    *outDesc = desc;
    cardLayout->addWidget(desc, 1);

    return card;
  };

  classicCard_ = createLayoutCard(QStringLiteral("classicLayoutCard"),
                                  &classicRadio_, &classicTitleLabel_,
                                  &classicBadgeLabel_, &classicDescLabel_);
  classicCard_->onClicked = [this] {
    setSelectedLayoutMode(dalinira::layout::BrowserLayoutMode::Classic);
  };
  cardsLayout->addWidget(classicCard_);

  leftCard_ = createLayoutCard(QStringLiteral("leftLayoutCard"),
                               &leftRadio_, &leftTitleLabel_,
                               &leftBadgeLabel_, &leftDescLabel_);
  leftCard_->onClicked = [this] {
    setSelectedLayoutMode(dalinira::layout::BrowserLayoutMode::LeftPanel);
  };
  cardsLayout->addWidget(leftCard_);

  rightCard_ = createLayoutCard(QStringLiteral("rightLayoutCard"),
                                &rightRadio_, &rightTitleLabel_,
                                &rightBadgeLabel_, &rightDescLabel_);
  rightCard_->onClicked = [this] {
    setSelectedLayoutMode(dalinira::layout::BrowserLayoutMode::VerticalTabsRight);
  };
  cardsLayout->addWidget(rightCard_);

  floatingCard_ = createLayoutCard(QStringLiteral("floatingLayoutCard"),
                                   &floatingRadio_, &floatingTitleLabel_,
                                   &floatingBadgeLabel_, &floatingDescLabel_);
  floatingCard_->onClicked = [this] {
    setSelectedLayoutMode(dalinira::layout::BrowserLayoutMode::FloatingSidebar);
  };
  cardsLayout->addWidget(floatingCard_);

  // The cards' radios select too (keyboard, assistive technologies); before,
  // only a mouse click on the card did.
  const std::pair<QRadioButton *, dalinira::layout::BrowserLayoutMode> layoutRadios[] = {
      {classicRadio_, dalinira::layout::BrowserLayoutMode::Classic},
      {leftRadio_, dalinira::layout::BrowserLayoutMode::LeftPanel},
      {rightRadio_, dalinira::layout::BrowserLayoutMode::VerticalTabsRight},
      {floatingRadio_, dalinira::layout::BrowserLayoutMode::FloatingSidebar}};
  for (const auto &[radio, mode] : layoutRadios) {
    connect(radio, &QRadioButton::toggled, this, [this, mode = mode](bool checked) {
      if (checked && selectedMode_ != mode) setSelectedLayoutMode(mode);
    });
  }

  rootLayout->addWidget(cardsRow, 0, Qt::AlignHCenter);

  rootLayout->addSpacing(14);

  // 3. Compact Slim Theme Selector Row (Alta getirildi ve noktalar küçültüldü)
  auto *themeRow = new QFrame(this);
  themeRow->setObjectName(QStringLiteral("appearanceThemeRow"));
  themeRow->setFixedHeight(32);
  themeRow->setStyleSheet(QStringLiteral(
      "#appearanceThemeRow {"
      "  background: rgba(15, 23, 42, 0.70);"
      "  border: 1px solid rgba(255, 255, 255, 0.12);"
      "  border-radius: 16px;"
      "}"));
  auto *themeLayout = new QHBoxLayout(themeRow);
  themeLayout->setContentsMargins(16, 2, 16, 2);
  themeLayout->setSpacing(18);

  auto *themeLabel = new QLabel(I18n::text(QStringLiteral("settings.appearance.theme")), themeRow);
  themeLabel->setObjectName(QStringLiteral("appearanceThemeLabel"));
  themeLabel->setStyleSheet(QStringLiteral("color: #94a3b8; font-size: 11.5px; font-weight: 600; background: transparent; border: none;"));
  themeLabel_ = themeLabel;
  themeLayout->addWidget(themeLabel);

  const auto makeThemeOption = [this, themeRow](const QString &text, QRadioButton **outRadio, QLabel **outLabel, int id) {
    auto *container = new ClickableRowOption(themeRow);
    auto *h = new QHBoxLayout(container);
    h->setContentsMargins(2, 0, 2, 0);
    h->setSpacing(6);

    auto *radio = new ModernRadioButton(container, 14);
    *outRadio = radio;
    h->addWidget(radio);

    auto *lbl = new QLabel(text, container);
    lbl->setStyleSheet(QStringLiteral("color: #e2e8f0; font-size: 11.5px; font-weight: 500; background: transparent; border: none;"));
    if (outLabel) *outLabel = lbl;
    h->addWidget(lbl);

    container->onClicked = [this, id] {
      setSelectedTheme(id == 1 ? dalinira::layout::AppearanceTheme::Light
                               : id == 2 ? dalinira::layout::AppearanceTheme::Dark
                                         : dalinira::layout::AppearanceTheme::System);
    };

    // toggled, not clicked: keyboard and assistive technologies check a radio
    // without a click.
    connect(radio, &QRadioButton::toggled, this, [this, id](bool checked) {
      const auto theme = id == 1 ? dalinira::layout::AppearanceTheme::Light
                       : id == 2 ? dalinira::layout::AppearanceTheme::Dark
                                 : dalinira::layout::AppearanceTheme::System;
      if (checked && selectedTheme_ != theme) setSelectedTheme(theme);
    });

    return container;
  };

  themeLayout->addWidget(makeThemeOption(I18n::text(QStringLiteral("settings.appearance.theme_system")), &systemThemeRadio_, &systemThemeLabel_, 0));
  themeLayout->addWidget(makeThemeOption(I18n::text(QStringLiteral("settings.appearance.theme_light")), &lightThemeRadio_, &lightThemeLabel_, 1));
  themeLayout->addWidget(makeThemeOption(I18n::text(QStringLiteral("settings.appearance.theme_dark")), &darkThemeRadio_, &darkThemeLabel_, 2));

  rootLayout->addWidget(themeRow, 0, Qt::AlignHCenter);

  setStyleSheet(QStringLiteral(
      "#classicLayoutCard, #leftLayoutCard, #rightLayoutCard, #floatingLayoutCard {"
      "  background: rgba(255, 255, 255, 0.05);"
      "  border: 1px solid rgba(255, 255, 255, 0.14);"
      "  border-radius: 12px;"
      "}"
      "#classicLayoutCard:hover, #leftLayoutCard:hover, #rightLayoutCard:hover, #floatingLayoutCard:hover {"
      "  background: rgba(255, 255, 255, 0.08);"
      "  border-color: rgba(255, 255, 255, 0.28);"
      "}"
      "#classicLayoutCard[selected=\"true\"], #leftLayoutCard[selected=\"true\"], #rightLayoutCard[selected=\"true\"], #floatingLayoutCard[selected=\"true\"] {"
      "  background: rgba(16, 185, 129, 0.12);"
      "  border: 2px solid #10b981;"
      "}"
      "#layoutCardTitle {"
      "  color: #ffffff;"
      "  font-weight: 700;"
      "  font-size: 14px;"
      "}"
      "#layoutCardBadge {"
      "  background: rgba(16, 185, 129, 0.2);"
      "  color: #34d399;"
      "  font-size: 9.5px;"
      "  font-weight: 600;"
      "  padding: 2px 6px;"
      "  border-radius: 4px;"
      "  border: 1px solid rgba(16, 185, 129, 0.35);"
      "}"
      "#layoutCardDesc {"
      "  color: #cbd5e1;"
      "  font-size: 11px;"
      "}"));

  retranslateUi();
}

void LayoutStep::retranslateUi() {
  classicTitleLabel_->setText(I18n::text(QStringLiteral("onboarding.layout_classic_name")));
  classicBadgeLabel_->setText(I18n::text(QStringLiteral("onboarding.layout_classic_badge")));
  classicDescLabel_->setText(I18n::text(QStringLiteral("onboarding.layout_classic_desc")));

  leftTitleLabel_->setText(I18n::text(QStringLiteral("onboarding.layout_left_name")));
  leftBadgeLabel_->setText(I18n::text(QStringLiteral("onboarding.layout_left_badge")));
  leftDescLabel_->setText(I18n::text(QStringLiteral("onboarding.layout_left_desc")));

  rightTitleLabel_->setText(I18n::text(QStringLiteral("onboarding.layout_right_name")));
  rightBadgeLabel_->setText(I18n::text(QStringLiteral("onboarding.layout_right_badge")));
  rightDescLabel_->setText(I18n::text(QStringLiteral("onboarding.layout_right_desc")));
  floatingTitleLabel_->setText(I18n::text(QStringLiteral("onboarding.layout_floating_name")));
  floatingBadgeLabel_->setText(I18n::text(QStringLiteral("onboarding.layout_floating_badge")));
  floatingDescLabel_->setText(I18n::text(QStringLiteral("onboarding.layout_floating_desc")));

  if (themeLabel_) {
    themeLabel_->setText(I18n::text(QStringLiteral("settings.appearance.theme")));
  }
  if (systemThemeLabel_) {
    systemThemeLabel_->setText(I18n::text(QStringLiteral("settings.appearance.theme_system")));
  }
  if (lightThemeLabel_) {
    lightThemeLabel_->setText(I18n::text(QStringLiteral("settings.appearance.theme_light")));
  }
  if (darkThemeLabel_) {
    darkThemeLabel_->setText(I18n::text(QStringLiteral("settings.appearance.theme_dark")));
  }

  if (previewWidget_) {
    previewWidget_->retranslateUi();
  }
}

void LayoutStep::onStepEntering() {
  const QString persisted = OnboardingManager::instance().selectedLayout();
  selectedMode_ = dalinira::layout::layoutModeFromString(persisted, dalinira::layout::BrowserLayoutMode::Classic);
  selectedTheme_ = dalinira::layout::appearanceThemeFromString(
      OnboardingManager::instance().selectedTheme());
  retranslateUi();
  updateSelectionVisuals(false);
}

void LayoutStep::onStepLeaving() {
  OnboardingManager::instance().setSelectedLayout(dalinira::layout::layoutModeToString(selectedMode_));
  dalinira::layout::setPersistedLayoutMode(selectedMode_);
  OnboardingManager::instance().setSelectedTheme(
      dalinira::layout::appearanceThemeToString(selectedTheme_));
  dalinira::layout::setPersistedAppearanceTheme(selectedTheme_);
}

void LayoutStep::setSelectedTheme(dalinira::layout::AppearanceTheme theme) {
  selectedTheme_ = theme;
  OnboardingManager::instance().setSelectedTheme(dalinira::layout::appearanceThemeToString(theme));
  dalinira::layout::setPersistedAppearanceTheme(theme);
  dalinira::layout::applyApplicationTheme(qobject_cast<QApplication *>(qApp), theme);
  updateSelectionVisuals(false);
}

void LayoutStep::setSelectedLayoutMode(dalinira::layout::BrowserLayoutMode mode) {
  selectedMode_ = mode;
  OnboardingManager::instance().setSelectedLayout(dalinira::layout::layoutModeToString(mode));
  dalinira::layout::setPersistedLayoutMode(mode);
  const bool anim = OnboardingManager::animationsEnabled();
  updateSelectionVisuals(anim);
}

void LayoutStep::updateSelectionVisuals(bool animated) {
  const bool isClassic = (selectedMode_ == dalinira::layout::BrowserLayoutMode::Classic);
  const bool isLeft = (selectedMode_ == dalinira::layout::BrowserLayoutMode::VerticalTabsLeft);
  const bool isRight = (selectedMode_ == dalinira::layout::BrowserLayoutMode::VerticalTabsRight);
  const bool isFloating = (selectedMode_ == dalinira::layout::BrowserLayoutMode::FloatingSidebar);

  classicCard_->setProperty("selected", isClassic);
  classicRadio_->setChecked(isClassic);
  classicCard_->style()->unpolish(classicCard_);
  classicCard_->style()->polish(classicCard_);

  leftCard_->setProperty("selected", isLeft);
  leftRadio_->setChecked(isLeft);
  leftCard_->style()->unpolish(leftCard_);
  leftCard_->style()->polish(leftCard_);

  rightCard_->setProperty("selected", isRight);
  rightRadio_->setChecked(isRight);
  rightCard_->style()->unpolish(rightCard_);
  rightCard_->style()->polish(rightCard_);

  floatingCard_->setProperty("selected", isFloating);
  floatingRadio_->setChecked(isFloating);
  floatingCard_->style()->unpolish(floatingCard_);
  floatingCard_->style()->polish(floatingCard_);

  classicRadio_->update();
  leftRadio_->update();
  rightRadio_->update();
  floatingRadio_->update();

  systemThemeRadio_->setChecked(selectedTheme_ == dalinira::layout::AppearanceTheme::System);
  lightThemeRadio_->setChecked(selectedTheme_ == dalinira::layout::AppearanceTheme::Light);
  darkThemeRadio_->setChecked(selectedTheme_ == dalinira::layout::AppearanceTheme::Dark);

  systemThemeRadio_->update();
  lightThemeRadio_->update();
  darkThemeRadio_->update();

  if (previewWidget_) {
    previewWidget_->setTheme(selectedTheme_);
    previewWidget_->setLayoutMode(selectedMode_, animated);
  }
}

} // namespace dalinira::onboarding
