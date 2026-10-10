#include "tab_search_popup.h"

#include <QApplication>
#include <QEvent>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QScrollBar>

#include "browser_window.h"
#include "browser_icons.h"
#include "browser_profile_service.h"
#include "tab_window_registry.h"
#include "i18n/i18n.h"

namespace dalinira::desktop_tabs {

namespace {

QString formatRelativeTime(const QDateTime &dt) {
  if (!dt.isValid()) return QString();
  const qint64 secs = dt.secsTo(QDateTime::currentDateTimeUtc());
  if (secs < 60) return I18n::text(QStringLiteral("time.just_now"));
  const qint64 mins = secs / 60;
  if (mins < 60) return I18n::text(QStringLiteral("time.minutes_ago")).arg(mins);
  const qint64 hours = mins / 60;
  if (hours < 24) return I18n::text(QStringLiteral("time.hours_ago")).arg(hours);
  const qint64 days = hours / 24;
  return I18n::text(QStringLiteral("time.days_ago")).arg(days);
}

QString displaySubtitleForUrl(const QUrl &url, const QDateTime &time = {}) {
  QString host = url.host();
  if (host.isEmpty()) {
    if (url.scheme() == QLatin1String("dalinira")) {
      host = QStringLiteral("dalinira://") + url.host();
      if (host == QLatin1String("dalinira://")) host = url.toString();
    } else {
      host = url.toDisplayString();
    }
  }
  if (time.isValid()) {
    const QString rel = formatRelativeTime(time);
    if (!rel.isEmpty()) {
      return QStringLiteral("%1 • %2").arg(host, rel);
    }
  }
  return host;
}

}  // namespace

TabSearchPopup::TabSearchPopup(BrowserWindow *parentWindow)
    : QWidget(nullptr, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint),
      parentWindow_(parentWindow) {
  setAttribute(Qt::WA_DeleteOnClose, false);
  setAttribute(Qt::WA_TranslucentBackground, true);
  setFixedWidth(360);
  setMaximumHeight(520);
  setupUi();
}

void TabSearchPopup::setupUi() {
  auto *rootLayout = new QVBoxLayout(this);
  rootLayout->setContentsMargins(8, 8, 8, 8);

  auto *card = new QWidget(this);
  card->setObjectName(QStringLiteral("tabSearchCard"));
  card->setStyleSheet(QStringLiteral(
      "#tabSearchCard {"
      "  background-color: #26252b;"
      "  border: 1px solid #3c3b45;"
      "  border-radius: 12px;"
      "}"
  ));

  auto *cardLayout = new QVBoxLayout(card);
  cardLayout->setContentsMargins(0, 0, 0, 0);
  cardLayout->setSpacing(0);

  // 1. Search Bar at Top
  auto *searchContainer = new QWidget(card);
  searchContainer->setObjectName(QStringLiteral("searchContainer"));
  searchContainer->setStyleSheet(QStringLiteral(
      "#searchContainer {"
      "  background: transparent;"
      "  border-bottom: 1px solid #36353f;"
      "}"
  ));
  auto *searchLayout = new QHBoxLayout(searchContainer);
  searchLayout->setContentsMargins(12, 10, 14, 10);
  searchLayout->setSpacing(10);

  auto *searchIcon = new QLabel(searchContainer);
  searchIcon->setPixmap(BrowserIcons::icon(BrowserIcon::Search).pixmap(18, 18));
  searchIcon->setFixedSize(18, 18);
  searchLayout->addWidget(searchIcon);

  searchEdit_ = new QLineEdit(searchContainer);
  searchEdit_->setObjectName(QStringLiteral("tabSearchInput"));
  searchEdit_->setPlaceholderText(I18n::text(QStringLiteral("tabsearch.placeholder")));
  searchEdit_->setStyleSheet(QStringLiteral(
      "#tabSearchInput {"
      "  background: transparent;"
      "  border: none;"
      "  color: #f1f3f4;"
      "  font-size: 13px;"
      "  font-weight: 500;"
      "  padding: 0px;"
      "}"
  ));
  searchLayout->addWidget(searchEdit_, 1);

  shortcutLabel_ = new QLabel(searchContainer);
  shortcutLabel_->setObjectName(QStringLiteral("tabSearchShortcutHint"));
  shortcutLabel_->hide();
  shortcutLabel_->setStyleSheet(QStringLiteral(
      "color: #9aa0a6; font-size: 11px; font-weight: 500; padding: 2px 6px; background: rgba(255, 255, 255, 0.06); border-radius: 4px;"
  ));
  searchLayout->addWidget(shortcutLabel_);

  cardLayout->addWidget(searchContainer);

  // 2. Scroll Area
  scrollArea_ = new QScrollArea(card);
  scrollArea_->setObjectName(QStringLiteral("tabSearchScrollArea"));
  scrollArea_->setWidgetResizable(true);
  scrollArea_->setFrameShape(QFrame::NoFrame);
  scrollArea_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  scrollArea_->setStyleSheet(QStringLiteral(
      "#tabSearchScrollArea {"
      "  background: transparent;"
      "  border: none;"
      "}"
      "QScrollBar:vertical {"
      "  border: none;"
      "  background: transparent;"
      "  width: 6px;"
      "  margin: 2px 0 2px 0;"
      "}"
      "QScrollBar::handle:vertical {"
      "  background: #50525b;"
      "  min-height: 24px;"
      "  border-radius: 3px;"
      "}"
      "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {"
      "  height: 0px;"
      "}"
  ));

  contentContainer_ = new QWidget(scrollArea_);
  contentContainer_->setObjectName(QStringLiteral("tabSearchContent"));
  contentContainer_->setStyleSheet(QStringLiteral("#tabSearchContent { background: transparent; }"));

  contentLayout_ = new QVBoxLayout(contentContainer_);
  contentLayout_->setContentsMargins(6, 6, 6, 8);
  contentLayout_->setSpacing(2);

  scrollArea_->setWidget(contentContainer_);
  cardLayout->addWidget(scrollArea_, 1);

  rootLayout->addWidget(card);

  connect(searchEdit_, &QLineEdit::textChanged, this, &TabSearchPopup::onSearchTextChanged);
  searchEdit_->installEventFilter(this);
}

void TabSearchPopup::setShortcutHint(const QString &text) {
  shortcutLabel_->setText(text);
  shortcutLabel_->setVisible(!text.isEmpty());
}

void TabSearchPopup::showBelow(QWidget *anchorWidget) {
  populateData();
  searchEdit_->clear();
  searchEdit_->setFocus();

  if (anchorWidget) {
    const QPoint globalPos = anchorWidget->mapToGlobal(QPoint(0, anchorWidget->height() + 4));
    move(globalPos);
  }
  show();
}

void TabSearchPopup::populateData() {
  items_.clear();
  selectedIndex_ = -1;

  // Clear existing items in contentLayout_
  QLayoutItem *child = nullptr;
  while ((child = contentLayout_->takeAt(0)) != nullptr) {
    if (child->widget()) {
      child->widget()->deleteLater();
    }
    delete child;
  }

  // 1. Collect Open Tabs across windows via TabWindowRegistry
  std::vector<TabSearchItem> openItems;
  const auto regWindows = TabWindowRegistry::instance().registeredWindows();
  for (const auto &rw : regWindows) {
    auto *bw = qobject_cast<BrowserWindow *>(rw.window.data());
    if (!bw) continue;
    for (int i = 0; i < bw->tabCount(); ++i) {
      const auto &info = bw->tabInfo(i);
      TabSearchItem item;
      item.type = TabSearchItem::Type::OpenTab;
      item.window = bw;
      item.tabIndex = i;
      item.tabId = info.id;
      item.title = info.title.trimmed().isEmpty() ? I18n::text(QStringLiteral("newtab.title")) : info.title.trimmed();
      item.url = info.url;
      item.icon = bw->tabIconForRecord(info);
      const auto optGroup = bw->groupForTab(info.id);
      if (optGroup.has_value()) {
        item.groupId = optGroup->id;
        item.groupName = optGroup->name;
        item.groupColor = optGroup->color;
      }
      openItems.push_back(std::move(item));
    }
  }

  // 2. Collect Recently Closed Tabs from Profile Service
  std::vector<TabSearchItem> closedItems;
  if (parentWindow_ && parentWindow_->services().profileService) {
    const auto &closed = parentWindow_->services().profileService->closedTabs();
    for (int i = 0; i < closed.size(); ++i) {
      const auto &c = closed[i];
      TabSearchItem item;
      item.type = TabSearchItem::Type::ClosedTab;
      item.window = parentWindow_;
      item.closedIndex = i;
      item.title = c.title.trimmed().isEmpty() ? c.url.toDisplayString() : c.title.trimmed();
      item.url = c.url;
      item.time = c.closedAt;
      item.icon = BrowserIcons::searchEngineIcon(c.title);
      if (item.icon.isNull() || item.icon.pixmap(16, 16).isNull()) {
        item.icon = BrowserIcons::icon(BrowserIcon::History);
      }
      closedItems.push_back(std::move(item));
    }
  }

  // Build Section: Açık Sekmeler
  if (!openItems.empty()) {
    openSectionHeader_ = new QLabel(I18n::text(QStringLiteral("tabsearch.open_tabs")), contentContainer_);
    openSectionHeader_->setStyleSheet(QStringLiteral(
        "color: #9aa0a6; font-size: 11px; font-weight: 600; padding: 6px 10px 4px 10px; text-transform: uppercase;"
    ));
    contentLayout_->addWidget(openSectionHeader_);

    for (auto &it : openItems) {
      auto *row = new QWidget(contentContainer_);
      row->setObjectName(QStringLiteral("tabSearchRow"));
      row->setCursor(Qt::PointingHandCursor);
      row->setFixedHeight(50);
      row->setStyleSheet(QStringLiteral(
          "#tabSearchRow {"
          "  background: transparent;"
          "  border-radius: 8px;"
          "}"
          "#tabSearchRow:hover {"
          "  background-color: rgba(255, 255, 255, 0.08);"
          "}"
      ));

      auto *rowLayout = new QHBoxLayout(row);
      rowLayout->setContentsMargins(10, 4, 8, 4);
      rowLayout->setSpacing(10);

      // Favicon
      auto *iconLabel = new QLabel(row);
      QPixmap pm = it.icon.pixmap(20, 20);
      if (pm.isNull()) pm = BrowserIcons::icon(BrowserIcon::NewTab).pixmap(20, 20);
      iconLabel->setPixmap(pm);
      iconLabel->setFixedSize(20, 20);
      rowLayout->addWidget(iconLabel);

      // Text column
      auto *textCol = new QWidget(row);
      textCol->setStyleSheet(QStringLiteral("background: transparent;"));
      auto *textLayout = new QVBoxLayout(textCol);
      textLayout->setContentsMargins(0, 0, 0, 0);
      textLayout->setSpacing(1);

      auto *titleRow = new QWidget(textCol);
      titleRow->setStyleSheet(QStringLiteral("background: transparent;"));
      auto *titleLayout = new QHBoxLayout(titleRow);
      titleLayout->setContentsMargins(0, 0, 0, 0);
      titleLayout->setSpacing(5);

      if (it.groupId.has_value() && !it.groupId->isNull()) {
        auto *groupBadge = new QLabel(titleRow);
        groupBadge->setFixedHeight(14);
        const QString gName = it.groupName.isEmpty() ? QStringLiteral("●") : it.groupName;
        groupBadge->setText(QStringLiteral(" %1 ").arg(gName));
        const QString bgHex = it.groupColor.isValid() ? it.groupColor.name() : QStringLiteral("#5f6368");
        groupBadge->setStyleSheet(QStringLiteral(
            "background-color: %1; color: white; border-radius: 3px; font-size: 10px; font-weight: bold; padding: 0 2px;"
        ).arg(bgHex));
        titleLayout->addWidget(groupBadge, 0, Qt::AlignVCenter);
      }

      auto *titleLabel = new QLabel(it.title, titleRow);
      titleLabel->setStyleSheet(QStringLiteral("color: #f1f3f4; font-size: 13px; font-weight: 500;"));
      titleLabel->setText(fontMetrics().elidedText(it.title, Qt::ElideRight, 210));
      titleLayout->addWidget(titleLabel, 1, Qt::AlignVCenter);

      auto *subLabel = new QLabel(displaySubtitleForUrl(it.url), textCol);
      subLabel->setStyleSheet(QStringLiteral("color: #9aa0a6; font-size: 11px;"));
      subLabel->setText(fontMetrics().elidedText(subLabel->text(), Qt::ElideRight, 230));

      textLayout->addWidget(titleRow);
      textLayout->addWidget(subLabel);
      rowLayout->addWidget(textCol, 1);

      // Close Button
      auto *closeBtn = new QToolButton(row);
      closeBtn->setText(QStringLiteral("✕"));
      closeBtn->setToolTip(I18n::text(QStringLiteral("tabsearch.close_tab")));
      closeBtn->setFixedSize(22, 22);
      closeBtn->setStyleSheet(QStringLiteral(
          "QToolButton {"
          "  color: #9aa0a6;"
          "  background: transparent;"
          "  border: none;"
          "  border-radius: 4px;"
          "  font-size: 11px;"
          "  font-weight: bold;"
          "}"
          "QToolButton:hover {"
          "  background: rgba(255, 255, 255, 0.15);"
          "  color: #ffffff;"
          "}"
      ));

      const QPointer<BrowserWindow> targetWin = it.window;
      const int targetIdx = it.tabIndex;
      connect(closeBtn, &QToolButton::clicked, this, [this, targetWin, targetIdx] {
        if (targetWin) {
          targetWin->closeTab(targetIdx);
          populateData();
        }
      });
      rowLayout->addWidget(closeBtn);

      it.rowWidget = row;
      row->installEventFilter(this);
      contentLayout_->addWidget(row);

      items_.push_back(it);
    }
  }

  // Build Section: Son Kapatılan
  if (!closedItems.empty()) {
    closedSectionHeader_ = new QLabel(I18n::text(QStringLiteral("tabsearch.recently_closed")), contentContainer_);
    closedSectionHeader_->setStyleSheet(QStringLiteral(
        "color: #9aa0a6; font-size: 11px; font-weight: 600; padding: 10px 10px 4px 10px; text-transform: uppercase;"
    ));
    contentLayout_->addWidget(closedSectionHeader_);

    for (auto &it : closedItems) {
      auto *row = new QWidget(contentContainer_);
      row->setObjectName(QStringLiteral("tabSearchRow"));
      row->setCursor(Qt::PointingHandCursor);
      row->setFixedHeight(50);
      row->setStyleSheet(QStringLiteral(
          "#tabSearchRow {"
          "  background: transparent;"
          "  border-radius: 8px;"
          "}"
          "#tabSearchRow:hover {"
          "  background-color: rgba(255, 255, 255, 0.08);"
          "}"
      ));

      auto *rowLayout = new QHBoxLayout(row);
      rowLayout->setContentsMargins(10, 4, 10, 4);
      rowLayout->setSpacing(10);

      // Favicon
      auto *iconLabel = new QLabel(row);
      QPixmap pm = it.icon.pixmap(20, 20);
      if (pm.isNull()) pm = BrowserIcons::icon(BrowserIcon::History).pixmap(20, 20);
      iconLabel->setPixmap(pm);
      iconLabel->setFixedSize(20, 20);
      rowLayout->addWidget(iconLabel);

      // Text column
      auto *textCol = new QWidget(row);
      textCol->setStyleSheet(QStringLiteral("background: transparent;"));
      auto *textLayout = new QVBoxLayout(textCol);
      textLayout->setContentsMargins(0, 0, 0, 0);
      textLayout->setSpacing(1);

      auto *titleLabel = new QLabel(it.title, textCol);
      titleLabel->setStyleSheet(QStringLiteral("color: #f1f3f4; font-size: 13px; font-weight: 500;"));
      titleLabel->setText(fontMetrics().elidedText(it.title, Qt::ElideRight, 250));

      auto *subLabel = new QLabel(displaySubtitleForUrl(it.url, it.time), textCol);
      subLabel->setStyleSheet(QStringLiteral("color: #9aa0a6; font-size: 11px;"));
      subLabel->setText(fontMetrics().elidedText(subLabel->text(), Qt::ElideRight, 250));

      textLayout->addWidget(titleLabel);
      textLayout->addWidget(subLabel);
      rowLayout->addWidget(textCol, 1);

      it.rowWidget = row;
      row->installEventFilter(this);
      contentLayout_->addWidget(row);

      items_.push_back(it);
    }
  }

  contentLayout_->addStretch();

  if (!items_.empty()) {
    selectRow(0);
  }
}

void TabSearchPopup::onSearchTextChanged(const QString &text) {
  filterRows(text.trimmed());
}

void TabSearchPopup::filterRows(const QString &query) {
  bool anyOpenVisible = false;
  bool anyClosedVisible = false;
  int firstVisible = -1;

  for (size_t i = 0; i < items_.size(); ++i) {
    auto &it = items_[i];
    if (!it.rowWidget) continue;

    bool match = true;
    if (!query.isEmpty()) {
      match = it.title.contains(query, Qt::CaseInsensitive)
          || it.url.toDisplayString().contains(query, Qt::CaseInsensitive)
          || it.url.host().contains(query, Qt::CaseInsensitive)
          || (!it.groupName.isEmpty() && it.groupName.contains(query, Qt::CaseInsensitive));
    }

    it.rowWidget->setVisible(match);
    if (match) {
      if (firstVisible < 0) firstVisible = static_cast<int>(i);
      if (it.type == TabSearchItem::Type::OpenTab) anyOpenVisible = true;
      else anyClosedVisible = true;
    }
  }

  if (openSectionHeader_) openSectionHeader_->setVisible(anyOpenVisible);
  if (closedSectionHeader_) closedSectionHeader_->setVisible(anyClosedVisible);

  selectRow(firstVisible);
}

void TabSearchPopup::selectRow(int index) {
  if (selectedIndex_ >= 0 && selectedIndex_ < static_cast<int>(items_.size())) {
    if (auto *w = items_[selectedIndex_].rowWidget) {
      w->setStyleSheet(QStringLiteral(
          "#tabSearchRow {"
          "  background: transparent;"
          "  border-radius: 8px;"
          "}"
          "#tabSearchRow:hover {"
          "  background-color: rgba(255, 255, 255, 0.08);"
          "}"
      ));
    }
  }

  selectedIndex_ = index;

  if (selectedIndex_ >= 0 && selectedIndex_ < static_cast<int>(items_.size())) {
    if (auto *w = items_[selectedIndex_].rowWidget) {
      w->setStyleSheet(QStringLiteral(
          "#tabSearchRow {"
          "  background-color: rgba(255, 255, 255, 0.12);"
          "  border-radius: 8px;"
          "}"
          "#tabSearchRow:hover {"
          "  background-color: rgba(255, 255, 255, 0.15);"
          "}"
      ));
      scrollArea_->ensureWidgetVisible(w);
    }
  }
}

void TabSearchPopup::activateSelectedRow() {
  if (selectedIndex_ < 0 || selectedIndex_ >= static_cast<int>(items_.size())) return;
  const auto &it = items_[selectedIndex_];

  if (it.type == TabSearchItem::Type::OpenTab) {
    if (it.window) {
      it.window->activateWindow();
      it.window->raise();
      it.window->switchTab(it.tabIndex);
    }
    close();
  } else if (it.type == TabSearchItem::Type::ClosedTab) {
    if (parentWindow_) {
      parentWindow_->addNewTab(it.url);
      if (parentWindow_->services().profileService) {
        parentWindow_->services().profileService->takeClosedTab(it.closedIndex);
      }
    }
    close();
  }
}

bool TabSearchPopup::eventFilter(QObject *watched, QEvent *event) {
  if (event->type() == QEvent::MouseButtonPress) {
    auto *mouseEvent = static_cast<QMouseEvent *>(event);
    if (mouseEvent->button() == Qt::LeftButton) {
      for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].rowWidget == watched) {
          selectRow(static_cast<int>(i));
          activateSelectedRow();
          return true;
        }
      }
    }
  }
  return QWidget::eventFilter(watched, event);
}

void TabSearchPopup::keyPressEvent(QKeyEvent *event) {
  switch (event->key()) {
    case Qt::Key_Escape:
      close();
      event->accept();
      return;
    case Qt::Key_Down: {
      int next = selectedIndex_ + 1;
      while (next < static_cast<int>(items_.size()) && (!items_[next].rowWidget || !items_[next].rowWidget->isVisible())) {
        ++next;
      }
      if (next < static_cast<int>(items_.size())) {
        selectRow(next);
      }
      event->accept();
      return;
    }
    case Qt::Key_Up: {
      int prev = selectedIndex_ - 1;
      while (prev >= 0 && (!items_[prev].rowWidget || !items_[prev].rowWidget->isVisible())) {
        --prev;
      }
      if (prev >= 0) {
        selectRow(prev);
      }
      event->accept();
      return;
    }
    case Qt::Key_Return:
    case Qt::Key_Enter:
      activateSelectedRow();
      event->accept();
      return;
    default:
      QWidget::keyPressEvent(event);
      break;
  }
}

}  // namespace dalinira::desktop_tabs
