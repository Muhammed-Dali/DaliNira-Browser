#include "tab_strip_widget.h"
#include "tab_animation_metrics.h"
#include "tab_appearance.h"
#include "tab_drag_controller.h"
#include "window_drag_geometry.h"
#include "core/engine_ui_profile.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QFontMetrics>
#include <QDateTime>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QRegion>
#include <QResizeEvent>
#include <QSettings>
#include <QScreen>
#include <QWindow>
#include <QWheelEvent>
#include <algorithm>

namespace dalinira::desktop_tabs {

TabStripWidget::TabStripWidget(QWidget *parent)
    : QWidget(parent) {
  setAttribute(Qt::WA_Hover, true);
  setMouseTracking(true);
  // Native Wayland cross-toplevel dragging is delivered as QDrag events.
  // The controller's application event filter consumes only DaliNira's private
  // MIME type, so unrelated URL/file drops remain unaffected.
  setAcceptDrops(true);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  setFixedHeight(layoutModel_.metrics().tabHeight);

  loadSettings();

  connect(&animator_, &TabStripAnimator::animationProgressed, this, &TabStripWidget::onAnimationTick);
  connect(&animator_, &TabStripAnimator::closingTransitionFinished, this, [this](quint64 trId) {
    closingGhosts_.remove(trId);
    update();
  });
  hoverAnimationTimer_.setInterval(16);
  hoverAnimationTimer_.setTimerType(Qt::PreciseTimer);
  connect(&hoverAnimationTimer_, &QTimer::timeout, this,
          &TabStripWidget::onHoverAnimationTick);
  hoverTimer_.setSingleShot(true);
  connect(&hoverTimer_, &QTimer::timeout, this, &TabStripWidget::onHoverTimeout);

  // Release mouse capture when the global drag session ends so the widget
  // does not permanently hold the mouse after a cancel or complete.
  connect(&TabDragController::instance(), &TabDragController::dragFinished,
          this, [this]() {
            if (mouseGrabber() == this) releaseMouse();
          });
}

TabStripWidget::~TabStripWidget() {
  hoverTimer_.stop();
  hoverAnimationTimer_.stop();
}

void TabStripWidget::loadSettings() {
  const QString styleValue = dalinira::ui_profile::getVisualValue(
      QStringLiteral("browser/tabStyle"),
      tabStylePreferenceValue(TabStyle::DaliNiraConnected)).toString();
  setTabStyle(tabStyleFromPreference(styleValue));
}

void TabStripWidget::setTabStyle(TabStyle style) {
  const bool changed = tabStyle_ != style;
  tabStyle_ = style;
  layoutModel_.setMetrics(tabAppearance(tabStyle_).layout);
  setFixedHeight(layoutModel_.metrics().tabHeight);
  recomputeLayout(false);
  if (changed) updateGeometry();
  update();
}

void TabStripWidget::setCurrentIndex(int index) {
  if (index < 0 || index >= items_.size()) {
    if (items_.isEmpty()) {
      activeIndex_ = -1;
      update();
    }
    return;
  }
  if (activeIndex_ != index) {
    activeIndex_ = index;
    update();
    emit currentChanged(activeIndex_);
  }
}

int TabStripWidget::addTab(uint64_t tabId, const QString &title, const QIcon &icon, bool pinned) {
  const int index = items_.size();
  insertTab(index, tabId, title, icon, pinned);
  return index;
}

int TabStripWidget::addTab(const QString &title) {
  return addTab(0, title, QIcon(), false);
}

int TabStripWidget::addTab(const QIcon &icon, const QString &title) {
  return addTab(0, title, icon, false);
}

void TabStripWidget::insertTab(int index, uint64_t tabId, const QString &title, const QIcon &icon, bool pinned) {
  const int safeIndex = std::clamp(index, 0, static_cast<int>(items_.size()));
  TabModelItem item;
  item.tabId = tabId;
  item.title = title;
  item.icon = icon;
  item.isPinned = pinned;
  items_.insert(safeIndex, item);

  if (hoveredIndex_ >= safeIndex) ++hoveredIndex_;

  if (activeIndex_ == -1) {
    activeIndex_ = safeIndex;
  } else if (safeIndex <= activeIndex_) {
    activeIndex_++;
  }

  recomputeLayout(true, TabAnimationMetrics::tabOpenDurationMs);
}

int TabStripWidget::insertTab(int index, const QString &title) {
  const int safeIndex = std::clamp(index, 0, static_cast<int>(items_.size()));
  insertTab(safeIndex, 0, title, QIcon(), false);
  return safeIndex;
}

int TabStripWidget::insertTab(int index, const QIcon &icon, const QString &title) {
  const int safeIndex = std::clamp(index, 0, static_cast<int>(items_.size()));
  insertTab(safeIndex, 0, title, icon, false);
  return safeIndex;
}

void TabStripWidget::removeTab(int index) {
  if (index < 0 || index >= items_.size()) return;

  const auto item = items_[index];
  const QRect currentR = tabRect(index);

  hoverOpacities_.remove(item.tabId);
  items_.removeAt(index);

  if (hoveredIndex_ == index) {
    hoveredIndex_ = -1;
  } else if (hoveredIndex_ > index) {
    --hoveredIndex_;
  }

  if (draggedIndex_ == index) {
    draggedIndex_ = -1;
    insertionSlot_ = -1;
    draggedTopLeft_.reset();
  } else if (draggedIndex_ > index) {
    --draggedIndex_;
  }

  if (items_.isEmpty()) {
    activeIndex_ = -1;
  } else if (activeIndex_ == index) {
    activeIndex_ = std::min(index, static_cast<int>(items_.size() - 1));
    emit currentChanged(activeIndex_);
  } else if (activeIndex_ > index) {
    activeIndex_--;
  }

  if (currentR.isValid() && !currentR.isEmpty() && TabAnimationMetrics::animationsEnabled()) {
    const quint64 trId = animator_.startClosingTransition(
        currentR, TabAnimationMetrics::tabCloseDurationMs);
    if (trId > 0) {
      closingGhosts_.insert(trId, ClosingGhostTab{trId, item.title, item.icon, item.isPinned, item.tabId});
    }
  }

  recomputeLayout(true, TabAnimationMetrics::tabCloseDurationMs);
}

void TabStripWidget::moveTab(int fromIndex, int toIndex) {
  if (fromIndex < 0 || fromIndex >= items_.size() || toIndex < 0 || toIndex >= items_.size() || fromIndex == toIndex) {
    return;
  }

  const auto item = items_.takeAt(fromIndex);
  items_.insert(toIndex, item);

  if (draggedIndex_ == fromIndex) {
    draggedIndex_ = toIndex;
    insertionSlot_ = toIndex;
  } else if (fromIndex < draggedIndex_ && toIndex >= draggedIndex_) {
    --draggedIndex_;
  } else if (fromIndex > draggedIndex_ && toIndex <= draggedIndex_) {
    ++draggedIndex_;
  }

  if (activeIndex_ == fromIndex) {
    activeIndex_ = toIndex;
  } else if (fromIndex < activeIndex_ && toIndex >= activeIndex_) {
    activeIndex_--;
  } else if (fromIndex > activeIndex_ && toIndex <= activeIndex_) {
    activeIndex_++;
  }

  if (hoveredIndex_ == fromIndex) {
    hoveredIndex_ = toIndex;
  } else if (fromIndex < hoveredIndex_ && toIndex >= hoveredIndex_) {
    --hoveredIndex_;
  } else if (fromIndex > hoveredIndex_ && toIndex <= hoveredIndex_) {
    ++hoveredIndex_;
  }

  // During commit, preserve the directly-following dragged visual until
  // controllerFinishDrag() starts its settle animation at the new index.
  recomputeLayout(draggedIndex_ < 0, 160);
  emit tabMoved(fromIndex, toIndex);
}

void TabStripWidget::setTabTitle(int index, const QString &title) {
  if (index >= 0 && index < items_.size()) {
    items_[index].title = title;
    update();
  }
}

void TabStripWidget::setTabIcon(int index, const QIcon &icon) {
  if (index >= 0 && index < items_.size()) {
    items_[index].icon = icon;
    update();
  }
}

void TabStripWidget::setTabPinned(int index, bool pinned) {
  if (index >= 0 && index < items_.size() && items_[index].isPinned != pinned) {
    items_[index].isPinned = pinned;
    recomputeLayout(true, 180);
  }
}

void TabStripWidget::setTabAudible(int index, bool audible) {
  if (index >= 0 && index < items_.size() && items_[index].isAudible != audible) {
    items_[index].isAudible = audible;
    update();
  }
}

void TabStripWidget::setTabSleeping(int index, bool sleeping) {
  if (index >= 0 && index < items_.size() && items_[index].isSleeping != sleeping) {
    items_[index].isSleeping = sleeping;
    update();
  }
}

bool TabStripWidget::isTabSleeping(int index) const {
  return index >= 0 && index < items_.size() && items_[index].isSleeping;
}

void TabStripWidget::setTabLoading(int index, bool loading) {
  if (index >= 0 && index < items_.size() && items_[index].isLoading != loading) {
    items_[index].isLoading = loading;
    update();
  }
}

void TabStripWidget::setTabData(int index, const QVariant &data) {
  if (index >= 0 && index < items_.size()) {
    items_[index].userData = data;
  }
}

QVariant TabStripWidget::tabData(int index) const {
  if (index >= 0 && index < items_.size()) return items_[index].userData;
  return {};
}

int TabStripWidget::tabAt(const QPoint &pos) const {
  const auto geoms = currentGeometries();
  return layoutModel_.hitTestTabIndex(geoms, pos);
}

void TabStripWidget::cancelHover() {
  hoverTimer_.stop();
  setHoveredIndex(-1);
  hoveredCloseIndex_ = -1;
  cardActive_ = false;
  emit tabHoverLeave();
  update();
}

QString TabStripWidget::tabText(int index) const {
  if (index >= 0 && index < items_.size()) return items_[index].title;
  return {};
}

QIcon TabStripWidget::tabIcon(int index) const {
  if (index >= 0 && index < items_.size()) return items_[index].icon;
  return {};
}

bool TabStripWidget::isTabPinned(int index) const {
  if (index >= 0 && index < items_.size()) return items_[index].isPinned;
  return false;
}

bool TabStripWidget::isTabAudible(int index) const {
  if (index >= 0 && index < items_.size()) return items_[index].isAudible;
  return false;
}

bool TabStripWidget::isTabLoading(int index) const {
  if (index >= 0 && index < items_.size()) return items_[index].isLoading;
  return false;
}

uint64_t TabStripWidget::tabId(int index) const {
  if (index >= 0 && index < items_.size()) return items_[index].tabId;
  return 0;
}

QRect TabStripWidget::tabRect(int index) const {
  const auto geoms = currentGeometries();
  if (index >= 0 && index < geoms.size()) {
    return geoms[index].visualRect;
  }
  return {};
}

QRect TabStripWidget::detachBoundary() const {
  return layoutModel_.computeDetachBoundary(size());
}

QRect TabStripWidget::attachBoundary() const {
  return layoutModel_.computeAttachBoundary(size());
}

int TabStripWidget::contentWidth() const {
  return layoutModel_.computePreferredTotalWidth(items_);
}

int TabStripWidget::visualTabsRight() const {
  const auto geoms = currentGeometries();
  int right = 0;
  for (const auto &g : geoms) {
    if (g.isDragged && geoms.size() > 1) continue;
    right = std::max(right, g.visualRect.right());
  }
  for (const auto &c : animator_.closingVisuals()) {
    if (!c.rect.isEmpty() && c.rect.width() > 0) {
      right = std::max(right, c.rect.right());
    }
  }
  return right;
}

QSize TabStripWidget::sizeHint() const {
  return QSize(contentWidth(), layoutModel_.metrics().tabHeight);
}

QSize TabStripWidget::minimumSizeHint() const {
  return QSize(120, layoutModel_.metrics().tabHeight);
}

void TabStripWidget::setTabVisualSuppressed(int index, bool suppressed) {
  const int next = suppressed ? index : -1;
  if (suppressedVisualTab_ != next) {
    suppressedVisualTab_ = next;
    update();
  }
}

void TabStripWidget::controllerBeginDrag(
    int index,
    const QPoint &globalCursor,
    const QPoint &pressOffsetInTab,
    std::optional<int> initialTabX) {
  if (index < 0 || index >= items_.size()) return;

  unsetCursor();
  draggedIndex_ = index;
  insertionSlot_ = index;
  dragOffsetInTab_ = pressOffsetInTab;

  if (initialTabX.has_value()) {
    draggedTopLeft_ = QPoint(initialTabX.value(), 0);
  } else {
    const QPoint localCursor = mapFromGlobal(globalCursor);
    draggedTopLeft_ = QPoint(localCursor.x() - dragOffsetInTab_.x(), 0);
  }

  recomputeLayout(false);
}

void TabStripWidget::controllerUpdateDrag(const QPoint &globalCursor, int insertionSlot) {
  if (draggedIndex_ < 0) return;

  insertionSlot_ = std::clamp(
      insertionSlot, 0, static_cast<int>(items_.size()) - 1);
  const QPoint localCursor = mapFromGlobal(globalCursor);
  draggedTopLeft_ = QPoint(localCursor.x() - dragOffsetInTab_.x(), 0);

  recomputeLayout(true, TabAnimationMetrics::tabSlotDurationMs);
}

void TabStripWidget::controllerFinishDrag(int finalIndex, bool cancelled) {
  unsetCursor();
  hoveredCloseIndex_ = -1;
  hoveredNewTabButton_ = false;
  hoveredIndex_ = -1;

  const int settleIndex = draggedIndex_ >= 0 ? draggedIndex_ : finalIndex;
  const QRect dropRect = draggedIndex_ >= 0 ? tabRect(draggedIndex_) : QRect{};

  draggedIndex_ = -1;
  insertionSlot_ = -1;
  draggedTopLeft_ = std::nullopt;
  pressedIndex_ = -1;

  const auto targetGeoms = layoutModel_.computeLayout(
      width(), items_, activeIndex_, -1, -1, std::nullopt, font());
  animator_.resetSlots(targetGeoms);
  if (!cancelled && settleIndex >= 0 && settleIndex < targetGeoms.size() &&
      dropRect.isValid()) {
    animator_.startSettle(
        settleIndex, dropRect, targetGeoms[settleIndex].logicalRect, TabAnimationMetrics::tabSettleDurationMs);
  }
  update();
}

void TabStripWidget::recomputeLayout(bool animate, int durationMs) {
  const auto geoms = layoutModel_.computeLayout(
      width(), items_, activeIndex_, draggedIndex_, insertionSlot_, draggedTopLeft_, font());

  if (animate) {
    animator_.setTargetGeometries(geoms, durationMs);
  } else {
    animator_.resetSlots(geoms);
  }
  if (draggedIndex_ >= 0 && draggedIndex_ < geoms.size()) {
    animator_.setImmediateVisualRect(draggedIndex_, geoms[draggedIndex_].visualRect);
  }
  update();
}

QVector<TabGeometry> TabStripWidget::currentGeometries() const {
  auto geoms = layoutModel_.computeLayout(
      width(), items_, activeIndex_, draggedIndex_, insertionSlot_, draggedTopLeft_, font());

  for (int i = 0; i < geoms.size(); ++i) {
    const QRect visualRect = geoms[i].isDragged && draggedTopLeft_.has_value()
        ? geoms[i].visualRect
        : animator_.currentVisualRect(i, geoms[i].logicalRect);
    layoutModel_.applyVisualRect(geoms[i], visualRect, font());
    geoms[i].visualOpacity = animator_.currentOpacity(i);
    geoms[i].hoverOpacity = hoverOpacity(geoms[i].tabId);
    geoms[i].isHovered = geoms[i].hoverOpacity > 0.001;
  }
  return geoms;
}

void TabStripWidget::onAnimationTick() {
  update();
}

qreal TabStripWidget::hoverOpacity(uint64_t tabId) const {
  return hoverOpacities_.value(tabId, 0.0);
}

void TabStripWidget::setHoveredIndex(int index) {
  const int safeIndex = (index >= 0 && index < items_.size()) ? index : -1;
  if (hoveredIndex_ == safeIndex) return;

  const int oldIndex = hoveredIndex_;
  hoveredIndex_ = safeIndex;
  hoverAnimationClock_.restart();
  if (!hoverAnimationTimer_.isActive()) hoverAnimationTimer_.start();

  QRegion dirty;
  if (oldIndex >= 0) dirty += tabRect(oldIndex).adjusted(-12, -2, 12, 2);
  if (hoveredIndex_ >= 0) dirty += tabRect(hoveredIndex_).adjusted(-12, -2, 12, 2);
  if (!dirty.isEmpty()) update(dirty);
}

void TabStripWidget::onHoverAnimationTick() {
  const qint64 elapsed = hoverAnimationClock_.isValid()
      ? std::max<qint64>(1, hoverAnimationClock_.restart())
      : 16;
  const TabAppearance &appearance = tabAppearance(tabStyle_);
  const auto geoms = currentGeometries();
  QRegion dirty;
  bool stillAnimating = false;

  for (int i = 0; i < items_.size(); ++i) {
    const uint64_t id = items_[i].tabId;
    const qreal target = i == hoveredIndex_
        ? appearance.hoverMaximumOpacity : 0.0;
    const qreal current = hoverOpacities_.value(id, 0.0);
    const qreal duration = std::max(1, appearance.hoverDurationMs);
    const qreal step = appearance.hoverMaximumOpacity *
                       std::clamp(qreal(elapsed) / duration, 0.0, 1.0);
    const qreal next = target > current
        ? std::min(target, current + step)
        : std::max(target, current - step);

    if (next <= 0.001 && target == 0.0) {
      hoverOpacities_.remove(id);
    } else {
      hoverOpacities_.insert(id, next);
    }
    if (!qFuzzyCompare(next + 1.0, target + 1.0)) stillAnimating = true;
    if (!qFuzzyCompare(next + 1.0, current + 1.0) && i < geoms.size()) {
      dirty += geoms[i].visualRect.adjusted(-12, -2, 12, 2);
    }
  }

  if (!dirty.isEmpty()) update(dirty);
  if (!stillAnimating) hoverAnimationTimer_.stop();
}

void TabStripWidget::onHoverTimeout() {
  if (hoveredIndex_ >= 0 && hoveredIndex_ < items_.size()) {
    cardActive_ = true;
    const QRect globalRect(mapToGlobal(tabRect(hoveredIndex_).topLeft()), tabRect(hoveredIndex_).size());
    emit tabHovered(hoveredIndex_, QCursor::pos(), globalRect);
  }
}

void TabStripWidget::setGroupModel(TabGroupModel *model) {
  if (groupModel_ == model) return;
  if (groupModel_) {
    disconnect(groupModel_, nullptr, this, nullptr);
  }
  groupModel_ = model;
  if (groupModel_) {
    connect(groupModel_, &TabGroupModel::groupAdded, this, [this] { update(); });
    connect(groupModel_, &TabGroupModel::groupUpdated, this, [this] { update(); });
    connect(groupModel_, &TabGroupModel::groupRemoved, this, [this] { update(); });
    connect(groupModel_, &TabGroupModel::tabGroupAssigned, this, [this] { update(); });
    connect(groupModel_, &TabGroupModel::tabGroupRemoved, this, [this] { update(); });
  }
  update();
}

QRect TabStripWidget::groupChipRect(const QUuid &groupId) const {
  return groupChipRects_.value(groupId);
}

void TabStripWidget::updateGroupChipRects(const QVector<TabGeometry> &geoms) const {
  groupChipRects_.clear();
  if (!groupModel_) return;

  struct GroupSpan {
    int firstIdx = -1;
    int lastIdx = -1;
  };
  QMap<QUuid, GroupSpan> spans;

  for (int i = 0; i < geoms.size(); ++i) {
    const uint64_t tid = geoms[i].tabId;
    const auto optGid = groupModel_->groupIdForTab(tid);
    if (!optGid.has_value() || optGid->isNull()) continue;
    const QUuid gid = *optGid;

    if (!spans.contains(gid)) {
      spans.insert(gid, GroupSpan{i, i});
    } else {
      spans[gid].lastIdx = i;
    }
  }

  QFont chipFont = font();
  chipFont.setPixelSize(10);
  chipFont.setBold(true);
  QFontMetrics fm(chipFont);

  for (auto it = spans.begin(); it != spans.end(); ++it) {
    const QUuid gid = it.key();
    const auto optGroup = groupModel_->group(gid);
    if (!optGroup.has_value()) continue;

    const int firstIdx = it.value().firstIdx;
    if (firstIdx < 0 || firstIdx >= geoms.size()) continue;

    const QRect firstRect = geoms[firstIdx].visualRect;
    if (firstRect.isEmpty()) continue;

    int chipW = 16;
    if (!optGroup->name.isEmpty()) {
      chipW = std::clamp(fm.horizontalAdvance(optGroup->name) + 14, 24, 110);
    }
    const int chipH = 18;
    const int chipY = firstRect.top() + (firstRect.height() - chipH) / 2;
    const int chipX = firstRect.left() + 4;

    groupChipRects_.insert(gid, QRect(chipX, chipY, chipW, chipH));
  }
}

void TabStripWidget::paintEvent(QPaintEvent *) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
  painter.setRenderHint(QPainter::TextAntialiasing, true);

  const auto geoms = currentGeometries();
  const int total = geoms.size();
  const TabAppearance &appearance = tabAppearance(tabStyle_);

  updateGroupChipRects(geoms);

  // Separators are a fallback only. Like Chromium, they fade as either
  // adjacent tab gains a hover/selected surface.
  for (int i = 0; i + 1 < total; ++i) {
    const TabGeometry &left = geoms[i];
    const TabGeometry &right = geoms[i + 1];
    if (left.isActive || right.isActive || left.isDragged || right.isDragged)
      continue;
    const qreal opacity = 1.0 - std::max(left.hoverOpacity, right.hoverOpacity);
    if (opacity <= 0.01) continue;
    QColor separatorColor = appearance.separator;
    separatorColor.setAlphaF(separatorColor.alphaF() * opacity);
    painter.save();
    painter.setPen(QPen(separatorColor, 1.0));
    const qreal x = left.visualRect.right() + 0.5;
    painter.drawLine(QPointF(x, left.visualRect.top() +
                                appearance.separatorTopInset),
                     QPointF(x, left.visualRect.bottom() -
                                appearance.separatorBottomInset));
    painter.restore();
  }

  // Helper lambda to draw a single tab
  auto drawTab = [&](const TabGeometry &g, bool isDragged) {
    if (g.tabIndex == suppressedVisualTab_) return;
    const QRect r = g.visualRect;
    if (r.isEmpty() || r.width() <= 2) return;

    painter.save();
    if (g.visualOpacity < 0.999) {
      painter.setOpacity(painter.opacity() * std::clamp(g.visualOpacity, 0.0, 1.0));
    }

    // 1. Draw dragged tab drop shadow
    if (isDragged) {
      painter.save();
      painter.setPen(Qt::NoPen);
      painter.setBrush(QColor(0, 0, 0, 110));
      painter.translate(0, 3);
      if (tabStyle_ == TabStyle::DaliNiraConnected) {
        const QRectF surface = tabSurfaceRect(r, tabStyle_, true);
        QPainterPath body;
        body.addRoundedRect(surface, appearance.hoverCornerRadius,
                            appearance.hoverCornerRadius);
        painter.drawPath(body);
      } else {
        painter.drawPath(tabSurfacePath(r, height(), tabStyle_, true));
      }
      painter.restore();
    }

    // 2. Tab Background & Shape (drawn unclipped so Chrome/Brave shoulder curves render fully)
    if (!g.isActive && !isDragged) {
      if (g.hoverOpacity > 0.001) {
        QColor hoverFill = appearance.hoverFill;
        hoverFill.setAlphaF(hoverFill.alphaF() * g.hoverOpacity);
        QColor hoverOutline = appearance.outline;
        hoverOutline.setAlphaF(hoverOutline.alphaF() * g.hoverOpacity);
        painter.save();
        painter.setPen(hoverOutline.alpha() > 0
                           ? QPen(hoverOutline, 1.0)
                           : QPen(Qt::NoPen));
        painter.setBrush(hoverFill);
        painter.drawPath(tabSurfacePath(r, height(), tabStyle_, false));
        painter.restore();
      }
    } else {
      painter.save();
      QPen outlinePen(appearance.outline,
                      tabStyle_ == TabStyle::DaliNiraConnected ? 2.4 : 1.0);
      outlinePen.setCapStyle(Qt::RoundCap);
      outlinePen.setJoinStyle(Qt::RoundJoin);
      painter.setPen(appearance.outline.alpha() > 0 ? outlinePen : QPen(Qt::NoPen));
      painter.setBrush(appearance.activeFill);
      if (tabStyle_ == TabStyle::DaliNiraConnected) {
        const QRectF surface = tabSurfaceRect(r, tabStyle_, true);
        QPainterPath body;
        body.addRoundedRect(surface, appearance.hoverCornerRadius,
                            appearance.hoverCornerRadius);
        painter.fillPath(body, appearance.activeFill);
        painter.strokePath(body, outlinePen);

        // Connector smoothly transitions from the active capsule's accent outline
        // through the bend into the exact theme separator color at the lower surface
        const qreal right = surface.right();
        const qreal centerY = surface.center().y();
        const qreal stripBottom = qreal(height());
        const qreal connectorX = right + 8.2;

        QPainterPath connector;
        connector.moveTo(right, centerY - 1.0);
        connector.lineTo(right, centerY);
        connector.lineTo(right + 5.2, centerY);
        connector.cubicTo(right + 6.8, centerY,
                          connectorX, centerY + 0.8,
                          connectorX, centerY + 2.8);
        connector.lineTo(connectorX, stripBottom);

        QLinearGradient connectorGrad(right, centerY, connectorX, centerY + 3.6);
        connectorGrad.setColorAt(0.0, appearance.outline);
        connectorGrad.setColorAt(0.25, appearance.outline);
        connectorGrad.setColorAt(0.9, appearance.separator);
        connectorGrad.setColorAt(1.0, appearance.separator);

        QPen connectorPen(QBrush(connectorGrad), 2.4);
        connectorPen.setCapStyle(Qt::RoundCap);
        connectorPen.setJoinStyle(Qt::RoundJoin);
        painter.strokePath(connector, connectorPen);
      } else {
        painter.drawPath(tabSurfacePath(r, height(), tabStyle_, true));
      }
      painter.restore();

      if (appearance.paintsSignatureAccent) {
        const QRectF surface = tabSurfaceRect(r, tabStyle_, true);
        const qreal leftX = surface.left() + appearance.topCornerRadius + 3;
        const qreal rightX = surface.right() - appearance.topCornerRadius - 3;
        if (rightX > leftX) {
          QLinearGradient accentGrad(leftX, surface.top(), rightX, surface.top());
          accentGrad.setColorAt(0.0, QColor(79, 195, 247, 0));
          accentGrad.setColorAt(0.22, QColor(79, 195, 247, 185));
          accentGrad.setColorAt(0.5, QColor(138, 180, 248, 235));
          accentGrad.setColorAt(0.78, QColor(79, 195, 247, 185));
          accentGrad.setColorAt(1.0, QColor(79, 195, 247, 0));
          QPen accentPen(QBrush(accentGrad), 1.8);
          accentPen.setCapStyle(Qt::RoundCap);
          painter.save();
          painter.setPen(accentPen);
          painter.drawLine(QPointF(leftX, surface.top() + 0.8),
                           QPointF(rightX, surface.top() + 0.8));
          painter.restore();
        }
      }
    }

    // Clip interior tab contents so text and icons stay strictly within the tab body during motion
    painter.save();
    painter.setClipRect(r);

    // 2b. Tab Group Accent Lines
    const auto optGid = groupModel_ ? groupModel_->groupIdForTab(g.tabId) : std::nullopt;
    const bool hasGroup = optGid.has_value() && !optGid->isNull();
    const auto optGroup = hasGroup ? groupModel_->group(*optGid) : std::nullopt;
    const bool isFirstOfGroup = hasGroup && groupChipRects_.contains(*optGid) &&
                                (g.tabIndex == 0 || (groupModel_->groupIdForTab(geoms[g.tabIndex - 1].tabId) != optGid));

    if (hasGroup && optGroup.has_value()) {
      const QColor gc = optGroup->color;
      painter.fillRect(QRect(r.left() + 2, r.top(), r.width() - 4, 3), gc);
      painter.fillRect(QRect(r.left() + 2, r.bottom() - 1, r.width() - 4, 2), gc);
    }

    QRect favRect = g.faviconRect;
    QRect txtRect = g.textRect;
    if (isFirstOfGroup) {
      const int shift = groupChipRects_.value(*optGid).width() + 6;
      favRect.translate(shift, 0);
      txtRect.setLeft(txtRect.left() + shift);
    }

    // 3. Favicon
    if (!favRect.isEmpty() && !g.icon.isNull()) {
      const qreal dpr = devicePixelRatioF();
      const QPixmap favicon = g.icon.pixmap(
          favRect.size(), dpr, QIcon::Normal,
          g.isActive ? QIcon::On : QIcon::Off);
      if (g.isSleeping && !g.isActive) {
        painter.save();
        painter.setOpacity(0.45);
        painter.drawPixmap(favRect, favicon);
        painter.restore();
        // Moon badge at the favicon's lower right corner.
        const QRectF badge(favRect.right() - 6.5, favRect.bottom() - 6.5, 8.0, 8.0);
        painter.save();
        painter.setRenderHint(QPainter::Antialiasing, true);
        QPainterPath moon;
        moon.addEllipse(badge);
        QPainterPath bite;
        bite.addEllipse(badge.translated(2.6, -1.6));
        painter.fillPath(moon.subtracted(bite), QColor(0xc9, 0xd6, 0xe8));
        painter.restore();
      } else {
        painter.drawPixmap(favRect, favicon);
      }
    }

    // 4. Audio Indicator
    if (!g.audioIconRect.isEmpty() && g.isAudible) {
      painter.save();
      painter.setPen(Qt::NoPen);
      painter.setBrush(QColor(0x8a, 0xb4, 0xf8));
      painter.drawEllipse(g.audioIconRect.center(), 3, 3);
      painter.restore();
    }

    // 5. Title Text
    if (!txtRect.isEmpty() && !g.title.isEmpty() && txtRect.width() > 0) {
      QColor textColor = appearance.inactiveText;
      if (g.isSleeping && !g.isActive) textColor.setAlphaF(textColor.alphaF() * 0.6);
      if (g.isActive) {
        textColor = appearance.activeText;
      } else if (g.hoverOpacity > 0.0) {
        const qreal amount = std::clamp(g.hoverOpacity, 0.0, 1.0);
        textColor.setRedF(std::lerp(textColor.redF(),
                                   appearance.hoverText.redF(), amount));
        textColor.setGreenF(std::lerp(textColor.greenF(),
                                     appearance.hoverText.greenF(), amount));
        textColor.setBlueF(std::lerp(textColor.blueF(),
                                    appearance.hoverText.blueF(), amount));
      }

      QFont f = font();
      f.setWeight(g.isActive ? QFont::Medium : QFont::Normal);
      painter.setFont(f);
      painter.setPen(textColor);

      const QString elidedText = fontMetrics().elidedText(g.title, Qt::ElideRight, txtRect.width());
      painter.drawText(txtRect, Qt::AlignVCenter | Qt::AlignLeft, elidedText);
    }

    // 6. Close Button
    if (!g.closeButtonRect.isEmpty() && !g.isPinned && !g.isClosing) {
      const bool isCloseHovered = (hoveredCloseIndex_ == g.tabIndex);
      if (isCloseHovered) {
        painter.save();
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0x50, 0x54, 0x5c, 160));
        painter.drawRoundedRect(g.closeButtonRect, 4, 4);
        painter.restore();
      }

      painter.save();
      QPen closePen(isCloseHovered ? QColor(QStringLiteral("#ffffff"))
                                   : QColor(QStringLiteral("#aeb4bc")),
                    1.6, Qt::SolidLine, Qt::RoundCap);
      painter.setPen(closePen);
      const QPointF center = g.closeButtonRect.center();
      constexpr qreal halfExtent = 4.0;
      painter.drawLine(center + QPointF(-halfExtent, -halfExtent),
                       center + QPointF(halfExtent, halfExtent));
      painter.drawLine(center + QPointF(halfExtent, -halfExtent),
                       center + QPointF(-halfExtent, halfExtent));
      painter.restore();
    }

    painter.restore(); // interior content clip
    painter.restore(); // outer tab save
  };

  // Draw inactive unpinned tabs
  for (const auto &g : geoms) {
    if (!g.isActive && !g.isDragged) {
      drawTab(g, false);
    }
  }

  // Draw closing ghost tabs
  const auto closingVisuals = animator_.closingVisuals();
  for (const auto &cv : closingVisuals) {
    if (cv.opacity <= 0.001 || cv.rect.width() <= 2) continue;
    const auto ghostIt = closingGhosts_.constFind(cv.transitionId);
    TabGeometry ghostGeom;
    ghostGeom.tabIndex = -1;
    ghostGeom.tabId = ghostIt != closingGhosts_.constEnd() ? ghostIt->tabId : 0;
    ghostGeom.title = ghostIt != closingGhosts_.constEnd() ? ghostIt->title : QString();
    ghostGeom.icon = ghostIt != closingGhosts_.constEnd() ? ghostIt->icon : QIcon();
    ghostGeom.isPinned = ghostIt != closingGhosts_.constEnd() ? ghostIt->isPinned : false;
    ghostGeom.isActive = false;
    ghostGeom.isDragged = false;
    ghostGeom.isClosing = true;
    ghostGeom.visualRect = cv.rect;
    ghostGeom.visualOpacity = cv.opacity;
    layoutModel_.applyVisualRect(ghostGeom, cv.rect, font());
    ghostGeom.closeButtonRect = {};
    drawTab(ghostGeom, false);
  }

  // Draw active tab (if not dragged)
  if (activeIndex_ >= 0 && activeIndex_ < total && draggedIndex_ != activeIndex_) {
    drawTab(geoms[activeIndex_], false);
  }

  // Draw dragged tab on top of everything
  if (draggedIndex_ >= 0 && draggedIndex_ < total) {
    drawTab(geoms[draggedIndex_], true);
  }

  // Draw Group Chip Pills on top of tabs
  if (groupModel_) {
    QFont chipFont = font();
    chipFont.setPixelSize(10);
    chipFont.setBold(true);
    QFontMetrics fm(chipFont);

    for (auto it = groupChipRects_.constBegin(); it != groupChipRects_.constEnd(); ++it) {
      const auto optGroup = groupModel_->group(it.key());
      if (!optGroup.has_value()) continue;
      const QRect chipR = it.value();
      painter.save();
      painter.setPen(Qt::NoPen);
      painter.setBrush(optGroup->color);
      painter.drawRoundedRect(chipR, 4, 4);

      if (!optGroup->name.isEmpty()) {
        painter.setFont(chipFont);
        painter.setPen(Qt::white);
        const QString elided = fm.elidedText(optGroup->name, Qt::ElideRight, chipR.width() - 4);
        painter.drawText(chipR, Qt::AlignCenter, elided);
      } else {
        // Unnamed: draw subtle inner white marker
        painter.setBrush(QColor(255, 255, 255, 220));
        painter.drawEllipse(chipR.center(), 3, 3);
      }
      painter.restore();
    }
  }

  // Draw New Tab (+) Button
  const QRect newTabRect = layoutModel_.computeNewTabButtonRect(visualTabsRight(), size());
  if (!newTabRect.isEmpty()) {
    if (hoveredNewTabButton_) {
      painter.save();
      painter.setPen(Qt::NoPen);
      painter.setBrush(QColor(255, 255, 255, 28));
      const QRectF hoverRect = QRectF(newTabRect).adjusted(2, 2, -2, -2);
      painter.drawRoundedRect(hoverRect, hoverRect.height() / 2.0,
                              hoverRect.height() / 2.0);
      painter.restore();
    }
    painter.save();
    painter.setPen(QPen(hoveredNewTabButton_
                            ? QColor(QStringLiteral("#ffffff"))
                            : QColor(QStringLiteral("#c5cad0")),
                        1.8, Qt::SolidLine, Qt::RoundCap));
    const QPointF center = newTabRect.center();
    constexpr qreal plusHalfExtent = 5.0;
    painter.drawLine(center + QPointF(-plusHalfExtent, 0),
                     center + QPointF(plusHalfExtent, 0));
    painter.drawLine(center + QPointF(0, -plusHalfExtent),
                     center + QPointF(0, plusHalfExtent));
    painter.restore();
  }
}

void TabStripWidget::enterEvent(QEnterEvent *event) {
  if (!TabDragController::instance().isActive()) {
    unsetCursor();
  }
  QWidget::enterEvent(event);
}

void TabStripWidget::mousePressEvent(QMouseEvent *event) {
  if (event->button() == Qt::LeftButton) {
    pendingWindowMovePress_.reset();
    manualWindowMoveOffset_.reset();
    cancelHover();
    const QPoint localPos = event->position().toPoint();
    pressGlobalPos_ = event->globalPosition().toPoint();
    pressLocalPos_ = localPos;

    // Check Group Chip click
    for (auto it = groupChipRects_.constBegin(); it != groupChipRects_.constEnd(); ++it) {
      if (it.value().contains(localPos)) {
        pressedIndex_ = -1;
        const QPoint globalBelow = mapToGlobal(QPoint(it.value().left(), it.value().bottom()));
        emit groupChipClicked(it.key(), globalBelow);
        event->accept();
        return;
      }
    }

    // Check New Tab Button click
    const QRect newTabRect = layoutModel_.computeNewTabButtonRect(visualTabsRight(), size());
    if (newTabRect.contains(localPos)) {
      pressedIndex_ = -1;
      emit newTabRequested();
      event->accept();
      return;
    }

    const auto geoms = currentGeometries();
    const int tabIdx = layoutModel_.hitTestTabIndex(geoms, localPos);

    if (tabIdx >= 0 && tabIdx < geoms.size()) {
      // Check if close button was clicked
      if (layoutModel_.hitTestCloseButton(geoms[tabIdx], localPos)) {
        pressedIndex_ = -1;
        emit tabCloseRequested(tabIdx);
        event->accept();
        return;
      }

      // Tab clicked
      pressedIndex_ = tabIdx;
      setCurrentIndex(tabIdx);
      dragOffsetInTab_ = localPos - geoms[tabIdx].visualRect.topLeft();
      event->accept();
      return;
    }

    // Empty strip clicked -> initiate window move (Chrome titlebar drag)
    pressedIndex_ = -1;
    if (window() && window()->windowHandle()) {
      // A click is not a drag. Wait for real pointer movement before asking
      // the window system to move this window, preserving clicks and double-clicks.
      pendingWindowMovePress_ = pressGlobalPos_;
      event->accept();
      return;
    }
  }

  QWidget::mousePressEvent(event);
}

void TabStripWidget::mouseMoveEvent(QMouseEvent *event) {
  const QPoint localPos = event->position().toPoint();
  const QPoint globalPos = event->globalPosition().toPoint();

  if (manualWindowMoveOffset_) {
    if (event->buttons() & Qt::LeftButton) {
      window()->move(globalPos - *manualWindowMoveOffset_);
      event->accept();
      return;
    }
    manualWindowMoveOffset_.reset();
    if (mouseGrabber() == this) releaseMouse();
  }

  if (pendingWindowMovePress_) {
    if (!(event->buttons() & Qt::LeftButton)) {
      pendingWindowMovePress_.reset();
    } else if ((globalPos - *pendingWindowMovePress_).manhattanLength() >= QApplication::startDragDistance()) {
      const QPoint pressed = *pendingWindowMovePress_;
      pendingWindowMovePress_.reset();
      QWidget *top = window();
      // The window manager moves the window (X11 _NET_WM_MOVERESIZE, Wayland
      // xdg_toplevel.move), so it owns edge snapping, tiling, maximize at the
      // top edge, restoring a dragged maximized window and moves between
      // monitors, as with any other window.
      if (top->windowHandle() && top->windowHandle()->startSystemMove()) {
        event->accept();
        return;
      }
      // Fallback for a window manager without a native move: follow the
      // pointer ourselves, restoring a maximized window under the grabbed point.
      if (top->isMaximized()) {
        const QRect maximized = top->geometry();
        const QRect normal = top->normalGeometry();
        QSize restored = top->property("daliniraRestoredSize").toSize();
        if (restored.width() < 600 || restored.height() < 400) restored = normal.size();
        QScreen *screen = QGuiApplication::screenAt(globalPos);
        if (!screen) screen = top->screen();
        const QSize available = screen ? screen->availableGeometry().size() : QSize(1200, 800);
        if (restored.width() < 600 || restored.height() < 400)
          restored = QSize(qMin(1200, available.width()), qMin(800, available.height()));
        restored = restored.boundedTo(available);
        const QRect target = restoredWindowGeometryForDrag(maximized, restored, pressed, globalPos);
        manualWindowMoveOffset_ = globalPos - target.topLeft();
        top->showNormal();
        top->setGeometry(target);
      } else {
        manualWindowMoveOffset_ = globalPos - top->pos();
      }
      grabMouse();
      event->accept();
      return;
    }
  }

  if (TabDragController::instance().isActive()) {
    TabDragController::instance().handleMouseMove(globalPos);
    event->accept();
    return;
  }

  // Clear pressedIndex_ if left mouse button is not actively held down.
  // Synthetic moves or layout adjustments during tab animations must NEVER trigger drag.
  if (!(event->buttons() & Qt::LeftButton)) {
    pressedIndex_ = -1;
  }

  // If left button pressed, check for drag threshold.
  // Two independent thresholds (matching Chromium behaviour):
  //   horizontalStartDragThreshold — minimum Manhattan distance to begin InStrip drag
  //   verticalDetachThreshold      — vertical overshoot to detach into new window
  // Only horizontal movement initiates DraggingInStrip; reaching the vertical
  // boundary from within the strip then promotes to Detaching.
  if (pressedIndex_ >= 0 && (event->buttons() & Qt::LeftButton)) {
    const QPoint delta = globalPos - pressGlobalPos_;
    const int manhattanDist = delta.manhattanLength();
    if (manhattanDist >= layoutModel_.metrics().horizontalStartDragThreshold && draggedIndex_ < 0) {
      const auto geoms = currentGeometries();
      if (pressedIndex_ < geoms.size()) {
        cancelHover();
        const QSize tabSize = geoms[pressedIndex_].visualRect.size();
        emit dragInitiated(pressedIndex_, pressGlobalPos_, dragOffsetInTab_, tabSize);
        if (TabDragController::instance().isActive()) {
          TabDragController::instance().handleMouseMove(globalPos);
        }
        event->accept();
        return;
      }
    }
  }

  // Handle Hover state
  const auto geoms = currentGeometries();
  const int hitTab = layoutModel_.hitTestTabIndex(geoms, localPos);

  int newHoveredClose = -1;
  if (hitTab >= 0 && hitTab < geoms.size()) {
    if (layoutModel_.hitTestCloseButton(geoms[hitTab], localPos)) {
      newHoveredClose = hitTab;
    }
  }

  bool isOverChip = false;
  for (const auto &rect : groupChipRects_) {
    if (rect.contains(localPos)) {
      isOverChip = true;
      break;
    }
  }
  if (isOverChip) {
    setCursor(Qt::PointingHandCursor);
  } else {
    unsetCursor();
  }

  if (hoveredCloseIndex_ != newHoveredClose) {
    hoveredCloseIndex_ = newHoveredClose;
    update();
  }

  // Check New Tab button hover
  const QRect newTabRect = layoutModel_.computeNewTabButtonRect(visualTabsRight(), size());
  const bool newTabHover = newTabRect.contains(localPos);
  if (hoveredNewTabButton_ != newTabHover) {
    hoveredNewTabButton_ = newTabHover;
    update();
  }

  if (hitTab != hoveredIndex_) {
    setHoveredIndex(hitTab);
    if (hoveredIndex_ >= 0) {
      if (cardActive_) {
        onHoverTimeout();
      } else {
        hoverTimer_.start(400);
      }
    } else {
      hoverTimer_.stop();
      cardActive_ = false;
      emit tabHoverLeave();
    }
  }

  QWidget::mouseMoveEvent(event);
  event->accept();
}

void TabStripWidget::mouseReleaseEvent(QMouseEvent *event) {
  pendingWindowMovePress_.reset();
  if (manualWindowMoveOffset_) {
    manualWindowMoveOffset_.reset();
    if (mouseGrabber() == this) releaseMouse();
    event->accept();
    return;
  }
  unsetCursor();
  if (event->button() == Qt::LeftButton) {
    if (TabDragController::instance().isActive()) {
      TabDragController::instance().handleMouseRelease(event->globalPosition().toPoint());
      pressedIndex_ = -1;
      event->accept();
      return;
    }
  }
  pressedIndex_ = -1;
  QWidget::mouseReleaseEvent(event);
}

void TabStripWidget::mouseDoubleClickEvent(QMouseEvent *event) {
  if (event->button() == Qt::LeftButton) {
    const QPoint localPos = event->position().toPoint();
    const auto geoms = currentGeometries();
    const int tabIdx = layoutModel_.hitTestTabIndex(geoms, localPos);

    if (tabIdx < 0) {
      // Double click on empty space -> maximize / restore window (Chrome titlebar behaviour)
      if (window()) {
        if (window()->isMaximized()) {
          window()->showNormal();
        } else {
          window()->showMaximized();
        }
      }
      event->accept();
      return;
    }
  }
  QWidget::mouseDoubleClickEvent(event);
}

void TabStripWidget::wheelEvent(QWheelEvent *event) {
  const int numDegrees = event->angleDelta().y() / 8;
  const int numSteps = numDegrees / 15;

  if (numSteps != 0 && count() > 1) {
    int next = activeIndex_ - numSteps;
    if (next < 0) next = count() - 1;
    if (next >= count()) next = 0;
    setCurrentIndex(next);
    event->accept();
    return;
  }
  QWidget::wheelEvent(event);
}

void TabStripWidget::leaveEvent(QEvent *event) {
  if (!TabDragController::instance().isActive()) {
    unsetCursor();
  }
  hoverTimer_.stop();
  setHoveredIndex(-1);
  hoveredCloseIndex_ = -1;
  hoveredNewTabButton_ = false;
  cardActive_ = false;
  emit tabHoverLeave();
  update();
  QWidget::leaveEvent(event);
}

void TabStripWidget::resizeEvent(QResizeEvent *event) {
  QWidget::resizeEvent(event);
  recomputeLayout(false);
}

void TabStripWidget::keyPressEvent(QKeyEvent *event) {
  if (event->key() == Qt::Key_Escape && draggedIndex_ >= 0) {
    controllerFinishDrag(draggedIndex_, true);
    event->accept();
    return;
  }
  QWidget::keyPressEvent(event);
}

void TabStripWidget::contextMenuEvent(QContextMenuEvent *event) {
  const auto geoms = currentGeometries();
  const int hitTab = layoutModel_.hitTestTabIndex(geoms, event->pos());
  if (hitTab >= 0) {
    emit tabContextMenuRequested(hitTab, event->globalPos());
    event->accept();
  }
}

}  // namespace dalinira::desktop_tabs
