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
#include <QPainter>
#include <QRegularExpression>
#include <QtMath>
#include <cmath>
#include <QPainterPath>
#include <QResizeEvent>
#include <QRegion>
#include <QPointer>
#include <QTimer>
#include <functional>


namespace {

// A CSS colour as getComputedStyle reports it: rgb()/rgba() or #hex.
QColor cssColor(const QString &text) {
  static const QRegularExpression rgb(QStringLiteral(R"(rgba?\(\s*([\d.]+)[ ,]+([\d.]+)[ ,]+([\d.]+)(?:[ ,/]+([\d.]+%?))?\s*\))"));
  const auto match = rgb.match(text.trimmed());
  if (match.hasMatch()) {
    QColor color(qRound(match.captured(1).toDouble()), qRound(match.captured(2).toDouble()),
                 qRound(match.captured(3).toDouble()));
    if (!match.captured(4).isEmpty()) {
      const QString alpha = match.captured(4);
      color.setAlphaF(alpha.endsWith(QLatin1Char('%')) ? alpha.chopped(1).toDouble() / 100.0 : alpha.toDouble());
    }
    return color;
  }
  return QColor(text.trimmed());
}

// Splits "a, b(c, d), e" at top-level commas.
QStringList topLevelParts(const QString &text) {
  QStringList parts;
  int depth = 0;
  int start = 0;
  for (int i = 0; i < text.size(); ++i) {
    if (text[i] == QLatin1Char('(')) ++depth;
    else if (text[i] == QLatin1Char(')')) --depth;
    else if (text[i] == QLatin1Char(',') && depth == 0) {
      parts << text.mid(start, i - start).trimmed();
      start = i + 1;
    }
  }
  parts << text.mid(start).trimmed();
  return parts;
}

// A CSS linear-gradient() laid out over `box` exactly as the page lays it out
// over its viewport (angle, gradient-line length, colour stops); outside the
// box it pads, so the panel continues the page's edge colour.
bool cssLinearGradient(const QString &css, const QRectF &box, QLinearGradient *gradient) {
  const int open = css.indexOf(QLatin1String("linear-gradient("));
  if (open < 0 || !css.endsWith(QLatin1Char(')'))) return false;
  QStringList parts = topLevelParts(css.mid(open + 16, css.size() - open - 17));
  qreal angle = 180.0;
  if (!parts.isEmpty()) {
    const QString first = parts.first();
    static const QRegularExpression deg(QStringLiteral(R"(^(-?[\d.]+)deg$)"));
    const auto match = deg.match(first);
    if (match.hasMatch()) {
      angle = match.captured(1).toDouble();
      parts.removeFirst();
    } else if (first.startsWith(QLatin1String("to "))) {
      const bool top = first.contains(QLatin1String("top")), bottom = first.contains(QLatin1String("bottom"));
      const bool left = first.contains(QLatin1String("left")), right = first.contains(QLatin1String("right"));
      angle = top ? (left ? 315 : right ? 45 : 0) : bottom ? (left ? 225 : right ? 135 : 180) : left ? 270 : 90;
      parts.removeFirst();
    }
  }
  if (parts.size() < 2) return false;
  QVector<QPair<qreal, QColor>> stops;
  static const QRegularExpression position(QStringLiteral(R"(\s([\d.]+)%$)"));
  for (const QString &part : std::as_const(parts)) {
    const auto match = position.match(part);
    const QColor color = cssColor(match.hasMatch() ? part.left(match.capturedStart()) : part);
    if (!color.isValid()) return false;
    stops.append({match.hasMatch() ? match.captured(1).toDouble() / 100.0 : -1.0, color});
  }
  if (stops.first().first < 0) stops.first().first = 0;
  if (stops.last().first < 0) stops.last().first = 1;
  for (int i = 1; i < stops.size() - 1; ++i) {  // unpositioned stops share the gap evenly
    if (stops[i].first >= 0) continue;
    int next = i;
    while (stops[next].first < 0) ++next;
    const qreal from = stops[i - 1].first, to = stops[next].first;
    for (int k = i; k < next; ++k) stops[k].first = from + (to - from) * (k - i + 1) / (next - i + 1);
  }
  const qreal radians = qDegreesToRadians(angle);
  const QPointF direction(std::sin(radians), -std::cos(radians));
  const qreal length = std::abs(box.width() * std::sin(radians)) + std::abs(box.height() * std::cos(radians));
  const QPointF centre = box.center();
  *gradient = QLinearGradient(centre - direction * length / 2, centre + direction * length / 2);
  gradient->setSpread(QGradient::PadSpread);
  for (const auto &stop : std::as_const(stops)) gradient->setColorAt(qBound(0.0, stop.first, 1.0), stop.second);
  return true;
}

}  // namespace

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
  std::function<void(int)> closeRow;
  std::function<void(bool)> dragActive;
  std::function<void(int, QPoint)> detach;
  std::function<void(BrowserSidePanel *, int, int)> transfer;

 protected:
  void mousePressEvent(QMouseEvent *event) override {
    // Middle-click closes a tab, as on the horizontal tab strip.
    if (event->button() == Qt::MiddleButton) {
      const int row = indexAt(event->position().toPoint()).row();
      if (row >= 0 && closeRow) closeRow(row);
      event->accept();
      return;
    }
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
      if (dragActive) dragActive(true);
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
    if (manualDragging_) {
      unsetCursor();
      if (dragActive) dragActive(false);
    }
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
  // Paint the style sheet background: a plain QWidget subclass does not, and
  // over a page (the revealed overlay) the panel was see-through.
  setAttribute(Qt::WA_StyledBackground, true);
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

  // 1. Top bar (Zen's sidebar top buttons): the pin toggle, then the
  // window's navigation controls while a side layout is active.
  topBar_ = new QWidget(this);
  topBar_->setObjectName(QStringLiteral("sidePanelTopBar"));
  topLayout_ = new QHBoxLayout(topBar_);
  topLayout_->setContentsMargins(0, 0, 0, 2);
  topLayout_->setSpacing(2);
  pinButton_ = new QToolButton(topBar_);
  pinButton_->setObjectName(QStringLiteral("sidePanelPinButton"));
  pinButton_->setCheckable(true);
  pinButton_->setFixedSize(30, 30);
  pinButton_->setIconSize(QSize(18, 18));
  pinButton_->setCursor(Qt::PointingHandCursor);
  connect(pinButton_, &QToolButton::clicked, this, [this] {
    pinButton_->setChecked(pinned_);  // the window decides; setPinned() follows
    emit pinToggleRequested();
  });
  topLayout_->addWidget(pinButton_);
  topLayout_->addStretch(1);
  rootLayout->addWidget(topBar_);

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
  tabList_->closeRow = [this](int row) { emit tabCloseRequested(row); };
  tabList_->dragActive = [this](bool active) { emit tabDragActiveChanged(active); };
  tabList_->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(tabList_, &QListWidget::customContextMenuRequested, this, [this](const QPoint &position) {
    const int row = tabList_->indexAt(position).row();
    if (row >= 0) emit tabContextMenuRequested(row, tabList_->viewport()->mapToGlobal(position));
  });
  tabList_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
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
  // Flat and compact, as Zen's sidebar: no boxed list, rounded rows, a soft
  // highlight for the active tab; sleeping tabs are muted.
  setStyleSheet((dark ? QStringLiteral(
      // A dark, near-black glass panel (Zen's sidebar feel); its own border.
      "#browserSidePanel { background-color:rgba(6, 8, 11, 0.93); border-left:1px solid rgba(255,255,255,0.07);"
      "  border-right:1px solid rgba(255,255,255,0.07); }"
      "#browserSidePanel[floatingCard=\"true\"] { border:1px solid rgba(255,255,255,0.10); border-radius:12px; }"
      "#sidePanelTopBar QToolButton { color:#e8eef5; background:transparent; border:none; border-radius:8px; padding:0;"
      "  min-width:30px; max-width:30px; min-height:30px; max-height:30px; font-size:16px; font-weight:bold; }"
      "#sidePanelTopBar QToolButton:hover { background:rgba(255,255,255,0.10); }"
      "#sidePanelTopBar QToolButton:checked { background:rgba(100,181,246,0.22); }"
      "#sidePanelTopBar QToolButton:disabled { color:#5b5b66; }"
      "#sidePanelTopBar QToolButton:focus { border:1px solid #62a9c8; }"
      "#sidePanelTabTitle { color:#dfe7ef; font-weight:500; }"
      "#sidePanelTabTitle[sleeping=\"true\"] { color:#7d8b99; font-style:italic; }"
      "#sidePanelTabsHeader { color:#8899aa; font-size:11px; letter-spacing:0.5px; }"
      "#sidePanelNewTabButton, #sidePanelTabClose {"
      "  background:transparent; color:#8899aa; border:none; border-radius:6px; font-size:13px; }"
      "#sidePanelNewTabButton:hover, #sidePanelTabClose:hover { background:rgba(255,255,255,0.10); color:#ffffff; }"
      "#sidePanelNewTabButton:focus, #sidePanelTabClose:focus { border:1px solid #62a9c8; }"
      "#sidePanelTabList { background:transparent; border:none; color:#cdd9e5; outline:none; padding:0; }"
      "#sidePanelTabList:focus { border:1px solid #2f4a63; border-radius:8px; }"
      "#sidePanelTabList::item { height:32px; padding:0 4px; border-radius:8px; margin:1px 2px; }"
      "#sidePanelTabList::item:hover { background:rgba(255,255,255,0.06); }"
      "#sidePanelTabList::item:selected { background:rgba(100,181,246,0.20); }"
      "QToolButton#sidePanelBookmarksBtn, QToolButton#sidePanelHistoryBtn, QToolButton#sidePanelSettingsBtn {"
      "  background:transparent; border:none; border-radius:6px; }"
      "QToolButton#sidePanelBookmarksBtn:hover, QToolButton#sidePanelHistoryBtn:hover,"
      "QToolButton#sidePanelSettingsBtn:hover { background:rgba(255,255,255,0.10); }") : QStringLiteral(
      "#browserSidePanel { background:#f3f6fa; border-left:1px solid #ccd5df; border-right:1px solid #ccd5df; }"
      "#browserSidePanel[floatingCard=\"true\"] { border:1px solid #c7d1dc; border-radius:12px; }"
      "#sidePanelTopBar QToolButton { color:#263442; background:transparent; border:none; border-radius:8px; padding:0;"
      "  min-width:30px; max-width:30px; min-height:30px; max-height:30px; font-size:16px; font-weight:bold; }"
      "#sidePanelTopBar QToolButton:hover { background:rgba(25,70,110,0.10); }"
      "#sidePanelTopBar QToolButton:checked { background:#d9eaff; }"
      "#sidePanelTopBar QToolButton:focus { border:1px solid #1873d1; }"
      "#sidePanelTabTitle { color:#1f2b38; font-weight:500; }"
      "#sidePanelTabTitle[sleeping=\"true\"] { color:#7a8796; font-style:italic; }"
      "#sidePanelTabsHeader { color:#637487; font-size:11px; }"
      "#sidePanelNewTabButton, #sidePanelTabClose { background:transparent; color:#637487; border:none; border-radius:6px; }"
      "#sidePanelNewTabButton:hover, #sidePanelTabClose:hover { background:#dce5ef; color:#17212b; }"
      "#sidePanelNewTabButton:focus, #sidePanelTabClose:focus { border:1px solid #1873d1; }"
      "#sidePanelTabList { background:transparent; border:none; outline:none; padding:0; }"
      "#sidePanelTabList:focus { border:1px solid #9cc4ea; border-radius:8px; }"
      "#sidePanelTabList::item { height:32px; padding:0 4px; border-radius:8px; margin:1px 2px; }"
      "#sidePanelTabList::item:hover { background:rgba(0,0,0,0.05); }"
      "#sidePanelTabList::item:selected { background:#d9eaff; }"
      "QToolButton#sidePanelBookmarksBtn, QToolButton#sidePanelHistoryBtn, QToolButton#sidePanelSettingsBtn {"
      "  background:transparent; border:none; border-radius:6px; }"
      "QToolButton#sidePanelBookmarksBtn:hover, QToolButton#sidePanelHistoryBtn:hover,"
      "QToolButton#sidePanelSettingsBtn:hover { background:#dce5ef; }")));
  updatePinButton();
}

void BrowserSidePanel::refreshAppearance() { updateStyles(); }

void BrowserSidePanel::retranslateUi() {
  setAccessibleName(I18n::text(QStringLiteral("onboarding.side_panel_title")));
  tabList_->setAccessibleName(I18n::text(QStringLiteral("onboarding.side_panel_tabs")));
  newTabButton_->setAccessibleName(I18n::text(QStringLiteral("menu.new_tab")));
  tabsHeader_->setText(I18n::text(QStringLiteral("onboarding.side_panel_tabs")));
  newTabButton_->setToolTip(I18n::text(QStringLiteral("menu.new_tab")));
  bookmarksButton_->setToolTip(I18n::text(QStringLiteral("menu.bookmarks")));
  historyButton_->setToolTip(I18n::text(QStringLiteral("menu.history")));
  settingsButton_->setToolTip(I18n::text(QStringLiteral("menu.settings")));
  for (QToolButton *button : {bookmarksButton_, historyButton_, settingsButton_})
    button->setAccessibleName(button->toolTip());
  updatePinButton();
}

void BrowserSidePanel::setChromeControls(const QList<QWidget *> &leading, const QList<QWidget *> &trailing) {
  for (QWidget *control : std::as_const(chromeControls_)) topLayout_->removeWidget(control);
  chromeControls_.clear();
  // [pin] [leading...] <stretch> [trailing...]
  int index = topLayout_->indexOf(pinButton_) + 1;
  for (QWidget *control : leading) {
    topLayout_->insertWidget(index++, control);
    chromeControls_.append(control);
  }
  for (QWidget *control : trailing) {
    topLayout_->addWidget(control);
    chromeControls_.append(control);
  }
  for (QWidget *control : std::as_const(chromeControls_)) control->show();
}

void BrowserSidePanel::setPinned(bool pinned) {
  pinned_ = pinned;
  updatePinButton();
}

void BrowserSidePanel::setPanelOnRight(bool right) {
  if (onRight_ == right) return;
  onRight_ = right;
  updatePinButton();
}

// A sidebar glyph, as Zen's toggle: a window with its side column, filled
// while the panel is pinned open.
void BrowserSidePanel::updatePinButton() {
  if (!pinButton_) return;
  const bool dark = isDarkAppearance(currentAppearanceTheme());
  const QColor ink = dark ? QColor(0xe8, 0xee, 0xf5) : QColor(0x26, 0x34, 0x42);
  QIcon icon;
  for (const int size : {18, 36}) {
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    const qreal unit = size / 18.0;
    const QRectF frame(2 * unit, 3.5 * unit, 14 * unit, 11 * unit);
    const qreal column = onRight_ ? frame.right() - 5 * unit : frame.left() + 5 * unit;
    if (pinned_) {
      const QRectF side = onRight_ ? QRectF(column, frame.top(), frame.right() - column, frame.height())
                                   : QRectF(frame.left(), frame.top(), column - frame.left(), frame.height());
      QPainterPath clip;
      clip.addRoundedRect(frame, 2.5 * unit, 2.5 * unit);
      painter.setClipPath(clip);
      painter.fillRect(side, ink);
      painter.setClipping(false);
    }
    painter.setPen(QPen(ink, 1.5 * unit));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(frame, 2.5 * unit, 2.5 * unit);
    painter.drawLine(QPointF(column, frame.top()), QPointF(column, frame.bottom()));
    painter.end();
    icon.addPixmap(pixmap);
  }
  pinButton_->setIcon(icon);
  pinButton_->setChecked(pinned_);
  const QString text = I18n::text(pinned_ ? QStringLiteral("sidepanel.unpin") : QStringLiteral("sidepanel.pin"));
  pinButton_->setToolTip(text);
  pinButton_->setAccessibleName(text);
}

void BrowserSidePanel::setNewTabBackdrop(const NewTabBackdrop &backdrop) {
  if (backdrop_ == backdrop) return;
  backdrop_ = backdrop;
  backdropCacheKey_.clear();
  update();
}

// Docked beside the New Tab page: the page's own background (gradient, image
// or colour, and its dim layer), continued from the page edge, softened, under
// a tinted glass - semi-transparent in look, readable, and the same on both
// engines (the page is never composited through the panel).
void BrowserSidePanel::paintEvent(QPaintEvent *event) {
  if (!backdrop_.valid || !pageArea_ || property("floatingCard").toBool()) {
    QWidget::paintEvent(event);
    return;
  }
  const QRect page(mapFromGlobal(pageArea_->mapToGlobal(QPoint(0, 0))), pageArea_->size());
  const bool dark = isDarkAppearance(currentAppearanceTheme());
  const QString key = QStringLiteral("%1x%2|%3,%4,%5x%6|%7").arg(width()).arg(height()).arg(page.x()).arg(page.y())
                          .arg(page.width()).arg(page.height()).arg(dark);
  if (key != backdropCacheKey_ || backdropCache_.isNull()) {
    QImage image(size().expandedTo(QSize(1, 1)), QImage::Format_ARGB32_Premultiplied);
    image.fill(backdrop_.baseColor.isValid() ? backdrop_.baseColor : QColor(0x07, 0x11, 0x1f));
    QPainter painter(&image);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    QLinearGradient gradient;
    if (cssLinearGradient(backdrop_.cssImage, QRectF(page), &gradient)) {
      painter.fillRect(image.rect(), gradient);
    } else if (!backdrop_.imagePath.isEmpty()) {
      const QImage source(backdrop_.imagePath);
      if (!source.isNull()) {
        // background-size: cover, centred in the page area; the panel shows
        // the same picture continued past the page edge.
        const auto cover = [&source](const QRect &box) {
          const qreal scale = std::max(box.width() / qreal(source.width()), box.height() / qreal(source.height()));
          const QSizeF scaled(source.width() * scale, source.height() * scale);
          return QRectF(QPointF(box.center()) - QPointF(scaled.width() / 2, scaled.height() / 2), scaled);
        };
        painter.drawImage(cover(page.united(rect())), source);
        painter.drawImage(cover(page), source);
      }
    }
    if (backdrop_.overlay.isValid() && backdrop_.overlay.alpha() > 0) painter.fillRect(image.rect(), backdrop_.overlay);
    painter.end();
    // A soft blur (down and up again): cheap, once per size or background.
    const QSize small(std::max(1, image.width() / 16), std::max(1, image.height() / 16));
    QImage soft = image.scaled(small, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                      .scaled(image.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QPainter tint(&soft);
    tint.fillRect(soft.rect(), dark ? QColor(6, 8, 11, 150) : QColor(243, 246, 250, 185));
    tint.end();
    backdropCache_ = QPixmap::fromImage(soft);
    backdropCacheKey_ = key;
  }
  QPainter painter(this);
  painter.drawPixmap(0, 0, backdropCache_);
  painter.setPen(dark ? QColor(255, 255, 255, 22) : QColor(0, 0, 0, 30));
  const int edge = onRight_ ? 0 : width() - 1;
  painter.drawLine(edge, 0, edge, height());
}

bool BrowserSidePanel::event(QEvent *event) {
  // The layout controller marks the floating overlay card ("floatingCard").
  if (event->type() == QEvent::DynamicPropertyChange &&
      static_cast<QDynamicPropertyChangeEvent *>(event)->propertyName() == "floatingCard") {
    style()->unpolish(this);
    style()->polish(this);
    updateCardShape();
  }
  return QWidget::event(event);
}

void BrowserSidePanel::resizeEvent(QResizeEvent *event) {
  QWidget::resizeEvent(event);
  updateCardShape();
}

// Over a WebKit page the panel is a native window, which cannot be
// translucent: its rounded corners need a window shape.
void BrowserSidePanel::updateCardShape() {
  if (property("floatingCard").toBool() && internalWinId()) {
    QPainterPath path;
    path.addRoundedRect(QRectF(rect()), 12, 12);
    setMask(QRegion(path.toFillPolygon().toPolygon()));
  } else if (!mask().isEmpty()) {
    clearMask();
  }
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
    item->setSizeHint(QSize(0, 32));
    item->setToolTip(title);
    // Screen readers get the tab title, with its sleeping state.
    item->setData(Qt::AccessibleTextRole,
                  tabs[i].sleeping ? QStringLiteral("%1 (%2)").arg(title, I18n::text(QStringLiteral("sidepanel.tab_sleeping")))
                                   : title);
    auto *row = new QWidget(tabList_);
    row->setObjectName(QStringLiteral("sidePanelTabRow"));
    row->setProperty("sleeping", tabs[i].sleeping);
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(7, 2, 3, 2);
    rowLayout->setSpacing(7);
    auto *icon = new QLabel(row);
    icon->setFixedSize(18, 18);
    icon->setPixmap(tabs[i].icon.pixmap(16, 16, tabs[i].sleeping ? QIcon::Disabled : QIcon::Normal));
    rowLayout->addWidget(icon);
    auto *label = new QLabel(title, row);
    label->setObjectName(QStringLiteral("sidePanelTabTitle"));
    label->setTextInteractionFlags(Qt::NoTextInteraction);
    label->setProperty("sleeping", tabs[i].sleeping);
    if (tabs[i].sleeping) label->setToolTip(I18n::text(QStringLiteral("sidepanel.tab_sleeping")));
    rowLayout->addWidget(label, 1);
    auto *close = new QToolButton(row);
    close->setObjectName(QStringLiteral("sidePanelTabClose"));
    close->setText(QString::fromUtf8("×"));
    close->setToolTip(I18n::text(QStringLiteral("common.close")));
    close->setAccessibleName(I18n::text(QStringLiteral("sidepanel.close_tab")).arg(title));
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
    tabList_->scrollToItem(tabList_->item(activeIndex));
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
    tabList_->scrollToItem(tabList_->item(activeIndex));  // the active tab stays in view
  }
  updating_ = false;
}

} // namespace dalinira::layout
