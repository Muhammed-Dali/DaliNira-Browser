#include "browser_side_panel.h"
#include "browser_layout.h"
#include "core/browser_icons.h"
#include "i18n/i18n.h"
#include "i18n/language_manager.h"

using dalinira::i18n::I18n;

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QToolButton>
#include <QStyle>
#include <QApplication>
#include <QMouseEvent>
#include <QPointer>
#include <QTimer>
#include <functional>

namespace dalinira::layout {

class SidePanelTabList final : public QListWidget {
 public:
  explicit SidePanelTabList(BrowserSidePanel *owner) : QListWidget(owner), owner_(owner) {
    // QDrag starts a nested native drag loop. That loop changes the X11 focus
    // while WebKitGTK is embedded through XEmbed and can make the WebKit plug
    // handle a stale focus window. Keep this drag entirely in Qt mouse events;
    // the actual tab/window transfer happens only after mouse release.
    setAcceptDrops(false);
    setDragEnabled(false);
    setDropIndicatorShown(false);
    setDragDropMode(QAbstractItemView::NoDragDrop);
  }

  std::function<void(int, int)> reorder;
  std::function<void(int, QPoint)> detach;
  std::function<void(BrowserSidePanel *, int, int)> transfer;

 protected:
  void mousePressEvent(QMouseEvent *event) override {
    if (event->button() == Qt::LeftButton) {
      pressedRow_ = indexAt(event->position().toPoint()).row();
      pressGlobal_ = event->globalPosition().toPoint();
    }
    QListWidget::mousePressEvent(event);
  }

  void mouseMoveEvent(QMouseEvent *event) override {
    if (pressedRow_ < 0 || !(event->buttons() & Qt::LeftButton)) {
      cancelManualDrag();
      QListWidget::mouseMoveEvent(event);
      return;
    }

    const QPoint globalPosition = event->globalPosition().toPoint();
    if (!manualDragging_) {
      if ((globalPosition - pressGlobal_).manhattanLength() <
          QApplication::startDragDistance()) {
        return;
      }
      manualDragging_ = true;
      setCursor(Qt::ClosedHandCursor);
      showDragPreview();
    }

    if (dragPreview_)
      dragPreview_->move(globalPosition + QPoint(14, 14));
    event->accept();
  }

  void mouseReleaseEvent(QMouseEvent *event) override {
    if (event->button() != Qt::LeftButton || !manualDragging_) {
      cancelManualDrag();
      QListWidget::mouseReleaseEvent(event);
      return;
    }

    const int sourceRow = pressedRow_;
    const QPoint globalPosition = event->globalPosition().toPoint();
    SidePanelTabList *targetList = listAt(globalPosition);
    const QPointer<BrowserSidePanel> sourceOwner(owner_);
    const auto reorderCallback = reorder;
    const auto detachCallback = detach;
    const auto transferCallback = targetList ? targetList->transfer
                                             : std::function<void(BrowserSidePanel *, int, int)>();
    int targetRow = -1;
    if (targetList) {
      const QModelIndex targetIndex = targetList->indexAt(
          targetList->viewport()->mapFromGlobal(globalPosition));
      targetRow = targetIndex.isValid() ? targetIndex.row() : targetList->count();
    }

    cancelManualDrag();
    event->accept();

    // Run mutations after the release event has completely unwound. In
    // particular this prevents reparenting an XEmbed WebKit surface from an
    // active mouse/native-event callback.
    if (targetList == this) {
      const int boundedTarget = qBound(0, targetRow, qMax(0, count() - 1));
      if (sourceRow != boundedTarget && reorderCallback) {
        QTimer::singleShot(0, sourceOwner, [sourceOwner, reorderCallback, sourceRow, boundedTarget] {
          if (sourceOwner) reorderCallback(sourceRow, boundedTarget);
        });
      }
      return;
    }
    if (targetList && transferCallback) {
      const int boundedTarget = qBound(0, targetRow, targetList->count());
      QTimer::singleShot(0, targetList,
                         [sourceOwner, transferCallback, sourceRow, boundedTarget] {
        if (sourceOwner) transferCallback(sourceOwner, sourceRow, boundedTarget);
      });
      return;
    }

    const QRect safeRect(viewport()->mapToGlobal(QPoint(0, 0)) - QPoint(24, 24),
                         viewport()->size() + QSize(48, 48));
    if (!safeRect.contains(globalPosition) && detachCallback) {
      QTimer::singleShot(0, sourceOwner,
                         [sourceOwner, detachCallback, sourceRow, globalPosition] {
        if (sourceOwner) detachCallback(sourceRow, globalPosition);
      });
    }
  }

 private:
  static SidePanelTabList *listAt(const QPoint &globalPosition) {
    QWidget *candidate = QApplication::widgetAt(globalPosition);
    while (candidate) {
      if (auto *list = dynamic_cast<SidePanelTabList *>(candidate)) return list;
      candidate = candidate->parentWidget();
    }
    return nullptr;
  }

  void showDragPreview() {
    QListWidgetItem *draggedItem =
        pressedRow_ >= 0 && pressedRow_ < count() ? item(pressedRow_) : nullptr;
    QWidget *row = draggedItem ? itemWidget(draggedItem) : nullptr;
    if (!row) return;
    auto *preview = new QLabel(nullptr, Qt::ToolTip | Qt::FramelessWindowHint |
                                           Qt::BypassWindowManagerHint);
    preview->setAttribute(Qt::WA_ShowWithoutActivating);
    preview->setAttribute(Qt::WA_TransparentForMouseEvents);
    preview->setPixmap(row->grab());
    preview->setFixedSize(row->size());
    preview->setWindowOpacity(0.90);
    preview->move(QCursor::pos() + QPoint(14, 14));
    preview->show();
    dragPreview_ = preview;
  }

  void cancelManualDrag() {
    if (manualDragging_) unsetCursor();
    manualDragging_ = false;
    pressedRow_ = -1;
    if (dragPreview_) dragPreview_->deleteLater();
    dragPreview_.clear();
  }

  BrowserSidePanel *owner_ = nullptr;
  int pressedRow_ = -1;
  bool manualDragging_ = false;
  QPoint pressGlobal_;
  QPointer<QLabel> dragPreview_;
};

BrowserSidePanel::BrowserSidePanel(QWidget *parent)
    : QWidget(parent) {
  setObjectName(QStringLiteral("browserSidePanel"));
  setMinimumWidth(180);
  setMaximumWidth(240);
  setupUi();
  updateStyles();
  retranslateUi();
}

void BrowserSidePanel::setupUi() {
  auto *rootLayout = new QVBoxLayout(this);
  rootLayout->setContentsMargins(6, 8, 6, 8);
  rootLayout->setSpacing(6);

  // 1. Header Row
  auto *headerWidget = new QWidget(this);
  auto *headerLayout = new QHBoxLayout(headerWidget);
  headerLayout->setContentsMargins(4, 0, 4, 0);
  headerLayout->setSpacing(4);

  headerTitle_ = new QLabel(headerWidget);
  headerTitle_->setObjectName(QStringLiteral("sidePanelHeaderTitle"));
  QFont titleFont = headerTitle_->font();
  titleFont.setBold(true);
  titleFont.setPointSize(10);
  headerTitle_->setFont(titleFont);
  headerLayout->addWidget(headerTitle_, 1);

  closeButton_ = new QToolButton(headerWidget);
  closeButton_->setObjectName(QStringLiteral("sidePanelCloseButton"));
  closeButton_->setText(QString::fromUtf8("✕"));
  closeButton_->setFixedSize(22, 22);
  closeButton_->setCursor(Qt::PointingHandCursor);
  connect(closeButton_, &QToolButton::clicked, this, &BrowserSidePanel::closePanelRequested);
  headerLayout->addWidget(closeButton_);

  rootLayout->addWidget(headerWidget);

  // 2. Tabs Section Header
  auto *tabsHeaderWidget = new QWidget(this);
  auto *tabsHeaderLayout = new QHBoxLayout(tabsHeaderWidget);
  tabsHeaderLayout->setContentsMargins(4, 4, 4, 0);
  tabsHeaderLayout->setSpacing(4);

  tabsHeader_ = new QLabel(tabsHeaderWidget);
  tabsHeader_->setObjectName(QStringLiteral("sidePanelTabsHeader"));
  tabsHeaderLayout->addWidget(tabsHeader_, 1);

  newTabButton_ = new QToolButton(tabsHeaderWidget);
  newTabButton_->setObjectName(QStringLiteral("sidePanelNewTabButton"));
  newTabButton_->setText(QString::fromUtf8("+"));
  newTabButton_->setFixedSize(22, 22);
  newTabButton_->setCursor(Qt::PointingHandCursor);
  connect(newTabButton_, &QToolButton::clicked, this, &BrowserSidePanel::newTabRequested);
  tabsHeaderLayout->addWidget(newTabButton_);

  rootLayout->addWidget(tabsHeaderWidget);

  // 3. Tab List
  tabList_ = new SidePanelTabList(this);
  tabList_->setObjectName(QStringLiteral("sidePanelTabList"));
  tabList_->setSelectionMode(QAbstractItemView::SingleSelection);
  tabList_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  tabList_->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  connect(tabList_, &QListWidget::currentRowChanged, this, [this](int row) {
    if (!updating_ && row >= 0) {
      emit tabSelected(row);
    }
  });
  tabList_->reorder = [this](int from, int to) { requestTabMove(from, to); };
  tabList_->detach = [this](int index, QPoint globalPosition) {
    requestTabDetach(index, globalPosition);
  };
  tabList_->transfer = [this](BrowserSidePanel *source, int sourceIndex, int targetIndex) {
    requestTabTransfer(source, sourceIndex, targetIndex);
  };
  rootLayout->addWidget(tabList_, 1);

  // 4. Quick Tools Navigation Bar
  auto *toolsWidget = new QWidget(this);
  auto *toolsLayout = new QHBoxLayout(toolsWidget);
  toolsLayout->setContentsMargins(2, 4, 2, 0);
  toolsLayout->setSpacing(4);

  const auto createToolBtn = [this, toolsWidget](const QIcon &icon, const QString &objName) {
    auto *btn = new QToolButton(toolsWidget);
    btn->setObjectName(objName);
    btn->setIcon(icon);
    btn->setIconSize(QSize(18, 18));
    btn->setFixedSize(28, 28);
    btn->setCursor(Qt::PointingHandCursor);
    return btn;
  };

  bookmarksButton_ = createToolBtn(BrowserIcons::icon(BrowserIcon::Bookmark), QStringLiteral("sidePanelBookmarksBtn"));
  connect(bookmarksButton_, &QToolButton::clicked, this, &BrowserSidePanel::bookmarksRequested);
  toolsLayout->addWidget(bookmarksButton_);

  historyButton_ = createToolBtn(BrowserIcons::icon(BrowserIcon::History), QStringLiteral("sidePanelHistoryBtn"));
  connect(historyButton_, &QToolButton::clicked, this, &BrowserSidePanel::historyRequested);
  toolsLayout->addWidget(historyButton_);

  downloadsButton_ = createToolBtn(BrowserIcons::icon(BrowserIcon::Download), QStringLiteral("sidePanelDownloadsBtn"));
  connect(downloadsButton_, &QToolButton::clicked, this, &BrowserSidePanel::downloadsRequested);
  toolsLayout->addWidget(downloadsButton_);

  passwordsButton_ = createToolBtn(BrowserIcons::icon(BrowserIcon::Password), QStringLiteral("sidePanelPasswordsBtn"));
  connect(passwordsButton_, &QToolButton::clicked, this, &BrowserSidePanel::passwordsRequested);
  toolsLayout->addWidget(passwordsButton_);

  toolsLayout->addStretch(1);

  settingsButton_ = createToolBtn(BrowserIcons::icon(BrowserIcon::Settings), QStringLiteral("sidePanelSettingsBtn"));
  connect(settingsButton_, &QToolButton::clicked, this, &BrowserSidePanel::settingsRequested);
  toolsLayout->addWidget(settingsButton_);

  rootLayout->addWidget(toolsWidget);
}

void BrowserSidePanel::updateStyles() {
  // The persisted appearance, as the window chrome uses: the panel's own
  // palette follows its style sheet and would keep the previous theme.
  const bool dark = isDarkAppearance(currentAppearanceTheme());
  setStyleSheet((dark ? QStringLiteral(
      "#browserSidePanel {"
      "  background-color: #121922;"
      "  border-left: 1px solid #233144;"
      "  border-right: 1px solid #233144;"
      "}"
      "#sidePanelHeaderTitle, #sidePanelTabTitle {"
      "  color: #e3ebf5;"
      "  font-weight: 600;"
      "}"
      "#sidePanelTabsHeader {"
      "  color: #8899aa;"
      "  font-size: 11px;"
      "  text-transform: uppercase;"
      "  letter-spacing: 0.5px;"
      "}"
      "#sidePanelCloseButton, #sidePanelNewTabButton, #sidePanelTabClose {"
      "  background: transparent;"
      "  color: #8899aa;"
      "  border: none;"
      "  border-radius: 4px;"
      "  font-size: 13px;"
      "}"
      "#sidePanelCloseButton:hover, #sidePanelNewTabButton:hover, #sidePanelTabClose:hover {"
      "  background: #233144;"
      "  color: #ffffff;"
      "}"
      "#sidePanelTabList {"
      "  background: #0e141c;"
      "  border: 1px solid #1c2736;"
      "  border-radius: 6px;"
      "  color: #cdd9e5;"
      "  outline: none;"
      "  padding: 2px;"
      "}"
      "#sidePanelTabList::item {"
      "  height: 28px;"
      "  padding: 0 6px;"
      "  border-radius: 4px;"
      "  margin: 1px 0;"
      "}"
      "#sidePanelTabList::item:hover {"
      "  background: #1c2736;"
      "  color: #ffffff;"
      "}"
      "#sidePanelTabList::item:selected {"
      "  background: #243b56;"
      "  color: #64b5f6;"
      "  border-left: 2px solid #64b5f6;"
      "}"
      "QToolButton#sidePanelBookmarksBtn, QToolButton#sidePanelHistoryBtn,"
      "QToolButton#sidePanelDownloadsBtn, QToolButton#sidePanelPasswordsBtn,"
      "QToolButton#sidePanelSettingsBtn {"
      "  background: transparent;"
      "  border: none;"
      "  border-radius: 4px;"
      "}"
      "QToolButton#sidePanelBookmarksBtn:hover, QToolButton#sidePanelHistoryBtn:hover,"
      "QToolButton#sidePanelDownloadsBtn:hover, QToolButton#sidePanelPasswordsBtn:hover,"
      "QToolButton#sidePanelSettingsBtn:hover {"
      "  background: #233144;"
      "}") : QStringLiteral(
      "#browserSidePanel { background:#f3f6fa; border:1px solid #ccd5df; }"
      "#sidePanelHeaderTitle, #sidePanelTabTitle { color:#1f2b38; font-weight:600; }"
      "#sidePanelTabsHeader { color:#637487; font-size:11px; }"
      "#sidePanelCloseButton, #sidePanelNewTabButton, #sidePanelTabClose {"
      " background:transparent; color:#637487; border:none; border-radius:4px; }"
      "#sidePanelCloseButton:hover, #sidePanelNewTabButton:hover, #sidePanelTabClose:hover {"
      " background:#dce5ef; color:#17212b; }"
      "#sidePanelTabList { background:#ffffff; border:1px solid #d5dde6; border-radius:6px; outline:none; }"
      "#sidePanelTabList::item { height:32px; border-radius:4px; margin:1px; }"
      "#sidePanelTabList::item:hover { background:#e8eef5; }"
      "#sidePanelTabList::item:selected { background:#d9eaff; border-left:2px solid #1873d1; }"
      "QToolButton#sidePanelBookmarksBtn, QToolButton#sidePanelHistoryBtn,"
      "QToolButton#sidePanelDownloadsBtn, QToolButton#sidePanelPasswordsBtn,"
      "QToolButton#sidePanelSettingsBtn { background:transparent; border:none; border-radius:4px; }"
      "QToolButton#sidePanelBookmarksBtn:hover, QToolButton#sidePanelHistoryBtn:hover,"
      "QToolButton#sidePanelDownloadsBtn:hover, QToolButton#sidePanelPasswordsBtn:hover,"
      "QToolButton#sidePanelSettingsBtn:hover { background:#dce5ef; }")));
}

void BrowserSidePanel::refreshAppearance() { updateStyles(); }

void BrowserSidePanel::retranslateUi() {
  headerTitle_->setText(I18n::text(QStringLiteral("onboarding.side_panel_title")));
  tabsHeader_->setText(I18n::text(QStringLiteral("onboarding.side_panel_tabs")));
  closeButton_->setToolTip(I18n::text(QStringLiteral("common.close")));
  newTabButton_->setToolTip(I18n::text(QStringLiteral("menu.new_tab")));
  bookmarksButton_->setToolTip(I18n::text(QStringLiteral("menu.bookmarks")));
  historyButton_->setToolTip(I18n::text(QStringLiteral("menu.history")));
  downloadsButton_->setToolTip(I18n::text(QStringLiteral("menu.downloads")));
  passwordsButton_->setToolTip(I18n::text(QStringLiteral("menu.passwords")));
  settingsButton_->setToolTip(I18n::text(QStringLiteral("menu.settings")));
}

void BrowserSidePanel::setTabs(const QStringList &tabTitles, int activeIndex) {
  QVector<SidePanelTabInfo> tabs;
  tabs.reserve(tabTitles.size());
  for (const QString &title : tabTitles) tabs.push_back({0, title, QIcon(), false});
  setTabs(tabs, activeIndex);
}

void BrowserSidePanel::setTabs(const QVector<SidePanelTabInfo> &tabs, int activeIndex) {
  updating_ = true;
  tabList_->clear();
  for (int i = 0; i < tabs.size(); ++i) {
    const QString title = tabs[i].title.isEmpty() ? I18n::text(QStringLiteral("menu.new_tab")) : tabs[i].title;
    // The item supplies selection geometry only. Icon and title are painted by
    // the row widget below; leaving them on the item paints both layers.
    auto *item = new QListWidgetItem(tabList_);
    item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(tabs[i].id));
    item->setData(Qt::UserRole + 1, title);
    item->setSizeHint(QSize(0, 34));
    item->setToolTip(title);
    auto *row = new QWidget(tabList_);
    row->setObjectName(QStringLiteral("sidePanelTabRow"));
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(7, 2, 3, 2);
    rowLayout->setSpacing(7);
    auto *icon = new QLabel(row);
    icon->setFixedSize(18, 18);
    icon->setPixmap(tabs[i].icon.pixmap(16, 16));
    rowLayout->addWidget(icon);
    auto *label = new QLabel(title, row);
    label->setObjectName(QStringLiteral("sidePanelTabTitle"));
    label->setTextInteractionFlags(Qt::NoTextInteraction);
    rowLayout->addWidget(label, 1);
    auto *close = new QToolButton(row);
    close->setObjectName(QStringLiteral("sidePanelTabClose"));
    close->setText(QString::fromUtf8("×"));
    close->setToolTip(I18n::text(QStringLiteral("common.close")));
    close->setFixedSize(22, 22);
    connect(close, &QToolButton::clicked, this, [this, item] {
      const int row = tabList_->row(item);
      if (row >= 0) emit tabCloseRequested(row);
    });
    rowLayout->addWidget(close);
    tabList_->setItemWidget(item, row);
  }
  activeIndex_ = activeIndex;
  if (activeIndex >= 0 && activeIndex < tabList_->count()) {
    tabList_->setCurrentRow(activeIndex);
  }
  updating_ = false;
}

int BrowserSidePanel::tabCount() const { return tabList_ ? tabList_->count() : 0; }

QString BrowserSidePanel::tabTitle(int index) const {
  if (!tabList_ || index < 0 || index >= tabList_->count()) return {};
  return tabList_->item(index)->data(Qt::UserRole + 1).toString();
}

void BrowserSidePanel::requestTabMove(int from, int to) {
  if (from < 0 || from >= tabCount() || to < 0 || to >= tabCount() || from == to) return;
  emit tabMoved(from, to);
}

void BrowserSidePanel::requestTabDetach(int index, const QPoint &globalPosition) {
  if (index < 0 || index >= tabCount()) return;
  emit tabDetachRequested(index, globalPosition);
}

void BrowserSidePanel::requestTabTransfer(BrowserSidePanel *source, int sourceIndex, int targetIndex) {
  if (!source || source == this || sourceIndex < 0 || sourceIndex >= source->tabCount()) return;
  emit tabTransferRequested(source, sourceIndex, qBound(0, targetIndex, tabCount()));
}

void BrowserSidePanel::setActiveTab(int activeIndex) {
  if (activeIndex == activeIndex_) return;
  activeIndex_ = activeIndex;
  updating_ = true;
  if (activeIndex >= 0 && activeIndex < tabList_->count()) {
    tabList_->setCurrentRow(activeIndex);
  }
  updating_ = false;
}

} // namespace dalinira::layout
