#include "tab_group_launcher_popup.h"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QVBoxLayout>
#include "core/keyboard_shortcut_service.h"
#include "i18n/i18n.h"

namespace dalinira::desktop_tabs {

namespace {
QIcon createTabGroupLauncherIcon(const QColor &color = QColor("#c4c7c5")) {
  QPixmap pm(20, 20);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing, true);

  QPen pen(color, 1.5);
  pen.setJoinStyle(Qt::RoundJoin);
  p.setPen(pen);
  p.setBrush(Qt::NoBrush);
  p.drawRoundedRect(QRectF(2.5, 4.5, 11.5, 10.0), 2, 2);

  p.setPen(QPen(color, 1.8, Qt::SolidLine, Qt::RoundCap));
  p.drawLine(QPointF(14.5, 10.5), QPointF(14.5, 15.5));
  p.drawLine(QPointF(12.0, 13.0), QPointF(17.0, 13.0));

  return QIcon(pm);
}
}  // namespace

TabGroupLauncherPopup::TabGroupLauncherPopup(QWidget *parent)
    : QWidget(parent, Qt::Popup | Qt::FramelessWindowHint) {
  setAttribute(Qt::WA_DeleteOnClose, false);
  setupUi();
}

void TabGroupLauncherPopup::setupUi() {
  setFixedWidth(280);
  setStyleSheet(QStringLiteral(
      "QWidget#launcherRoot {"
      "  background-color: #28292a;"
      "  border: 1px solid rgba(255, 255, 255, 0.12);"
      "  border-radius: 8px;"
      "}"
      "QPushButton#actionRow {"
      "  text-align: left;"
      "  border: none;"
      "  background: transparent;"
      "  border-radius: 6px;"
      "  padding: 8px 10px;"
      "}"
      "QPushButton#actionRow:hover {"
      "  background-color: rgba(255, 255, 255, 0.08);"
      "}"
      "QPushButton#actionRow:pressed {"
      "  background-color: rgba(255, 255, 255, 0.14);"
      "}"
  ));
  setObjectName(QStringLiteral("launcherRoot"));

  auto *mainLayout = new QVBoxLayout(this);
  mainLayout->setContentsMargins(4, 4, 4, 4);
  mainLayout->setSpacing(0);

  auto *rowBtn = new QPushButton(this);
  rowBtn->setObjectName(QStringLiteral("actionRow"));
  rowBtn->setCursor(Qt::PointingHandCursor);

  auto *rowLayout = new QHBoxLayout(rowBtn);
  rowLayout->setContentsMargins(4, 0, 4, 0);
  rowLayout->setSpacing(10);

  auto *iconLabel = new QLabel(rowBtn);
  iconLabel->setPixmap(createTabGroupLauncherIcon().pixmap(18, 18));
  iconLabel->setFixedSize(18, 18);
  rowLayout->addWidget(iconLabel, 0, Qt::AlignVCenter);

  auto *titleLabel = new QLabel(I18n::text(QStringLiteral("toolbar.new_tab_group")), rowBtn);
  titleLabel->setStyleSheet(QStringLiteral("color: #f1f3f4; font-size: 13px; font-weight: 500;"));
  rowLayout->addWidget(titleLabel, 1, Qt::AlignVCenter);

  auto *shortcutLabel = new QLabel(rowBtn);
  shortcutLabel->setStyleSheet(QStringLiteral("color: #9aa0a6; font-size: 11px; font-weight: 500;"));
  // The current "New tab group" shortcut, following changes made in Settings.
  const auto updateShortcut = [shortcutLabel] {
    const QString text = dalinira::KeyboardShortcutService::primaryText(QStringLiteral("tabs.group_new"));
    shortcutLabel->setText(text);
    shortcutLabel->setVisible(!text.isEmpty());
  };
  updateShortcut();
  connect(&dalinira::KeyboardShortcutService::instance(), &dalinira::KeyboardShortcutService::shortcutsChanged,
          shortcutLabel, updateShortcut);
  rowLayout->addWidget(shortcutLabel, 0, Qt::AlignVCenter);

  connect(rowBtn, &QPushButton::clicked, this, [this] {
    close();
    emit createGroupRequested();
  });

  mainLayout->addWidget(rowBtn);
}

void TabGroupLauncherPopup::showBelow(QWidget *anchorWidget) {
  if (!anchorWidget) return;

  adjustSize();
  const QPoint globalBelow = anchorWidget->mapToGlobal(QPoint(0, anchorWidget->height() + 4));

  const QRect screenGeom = screen() ? screen()->availableGeometry() : QRect(0, 0, 1920, 1080);
  int x = globalBelow.x();
  int y = globalBelow.y();

  if (x + width() > screenGeom.right()) {
    x = screenGeom.right() - width() - 8;
  }
  if (x < screenGeom.left()) {
    x = screenGeom.left() + 8;
  }
  if (y + height() > screenGeom.bottom()) {
    y = anchorWidget->mapToGlobal(QPoint(0, -height() - 4)).y();
  }

  move(x, y);
  show();
  raise();
}

void TabGroupLauncherPopup::keyPressEvent(QKeyEvent *event) {
  if (event->key() == Qt::Key_Escape) {
    close();
    event->accept();
    return;
  }
  QWidget::keyPressEvent(event);
}

}  // namespace dalinira::desktop_tabs
