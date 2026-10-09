#include "tab_group_popup.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QScreen>
#include <QVBoxLayout>
#include "i18n/i18n.h"

namespace dalinira::desktop_tabs {

namespace {

QIcon createNewTabInGroupIcon(const QColor &color = QColor("#c4c7c5")) {
  QPixmap pm(20, 20);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing, true);
  QPen pen(color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
  p.setPen(pen);
  p.setBrush(Qt::NoBrush);
  p.drawRoundedRect(QRectF(3.5, 3.5, 13.0, 13.0), 2, 2);
  p.drawLine(QPointF(10.0, 7.0), QPointF(10.0, 13.0));
  p.drawLine(QPointF(7.0, 10.0), QPointF(13.0, 10.0));
  return QIcon(pm);
}

QIcon createMoveWindowIcon(const QColor &color = QColor("#c4c7c5")) {
  QPixmap pm(20, 20);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing, true);
  QPen pen(color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
  p.setPen(pen);
  p.setBrush(Qt::NoBrush);
  p.drawRoundedRect(QRectF(2.5, 3.5, 10.5, 13.0), 2, 2);
  p.drawLine(QPointF(7.5, 10.0), QPointF(17.5, 10.0));
  p.drawLine(QPointF(10.5, 7.5), QPointF(7.5, 10.0));
  p.drawLine(QPointF(10.5, 12.5), QPointF(7.5, 10.0));
  return QIcon(pm);
}

QIcon createCloseGroupIcon(const QColor &color = QColor("#c4c7c5")) {
  QPixmap pm(20, 20);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing, true);
  QPen pen(color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
  p.setPen(pen);
  p.setBrush(Qt::NoBrush);
  p.drawRoundedRect(QRectF(3.5, 3.5, 13.0, 13.0), 2, 2);
  p.drawLine(QPointF(7.5, 7.5), QPointF(12.5, 12.5));
  p.drawLine(QPointF(12.5, 7.5), QPointF(7.5, 12.5));
  return QIcon(pm);
}

QIcon createUngroupIcon(const QColor &color = QColor("#c4c7c5")) {
  QPixmap pm(20, 20);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing, true);
  QPen pen(color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
  p.setPen(pen);
  p.setBrush(Qt::NoBrush);
  p.drawLine(QPointF(4.0, 10.5), QPointF(4.0, 16.0));
  p.drawLine(QPointF(4.0, 16.0), QPointF(9.5, 16.0));
  p.drawLine(QPointF(7.0, 13.0), QPointF(15.5, 4.5));
  p.drawLine(QPointF(11.0, 4.5), QPointF(15.5, 4.5));
  p.drawLine(QPointF(15.5, 4.5), QPointF(15.5, 9.0));
  return QIcon(pm);
}

QIcon createDeleteGroupIcon(const QColor &color = QColor("#c4c7c5")) {
  QPixmap pm(20, 20);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing, true);
  QPen pen(color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
  p.setPen(pen);
  p.setBrush(Qt::NoBrush);
  p.drawLine(QPointF(4.0, 6.0), QPointF(16.0, 6.0));
  p.drawLine(QPointF(8.0, 4.0), QPointF(12.0, 4.0));
  p.drawLine(QPointF(5.5, 6.0), QPointF(6.5, 16.5));
  p.drawLine(QPointF(6.5, 16.5), QPointF(13.5, 16.5));
  p.drawLine(QPointF(13.5, 16.5), QPointF(14.5, 6.0));
  p.drawLine(QPointF(8.5, 8.5), QPointF(8.5, 14.0));
  p.drawLine(QPointF(11.5, 8.5), QPointF(11.5, 14.0));
  return QIcon(pm);
}

QPixmap renderColorSwatch(const QColor &color, bool selected) {
  QPixmap pm(22, 22);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing, true);

  if (selected) {
    p.setPen(QPen(QColor(255, 255, 255, 240), 1.8));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QRectF(1.5, 1.5, 19.0, 19.0));

    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x28, 0x29, 0x2a));
    p.drawEllipse(QRectF(3.0, 3.0, 16.0, 16.0));

    p.setBrush(color);
    p.drawEllipse(QRectF(4.5, 4.5, 13.0, 13.0));
  } else {
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawEllipse(QRectF(2.5, 2.5, 17.0, 17.0));
  }
  return pm;
}

QPushButton *createActionRow(QWidget *parent, const QIcon &icon, const QString &text,
                             const QString &shortcut = QString(), bool isDelete = false) {
  auto *btn = new QPushButton(parent);
  btn->setCursor(Qt::PointingHandCursor);
  btn->setStyleSheet(QStringLiteral(
      "QPushButton {"
      "  border: none;"
      "  background: transparent;"
      "  border-radius: 6px;"
      "  padding: 6px 8px;"
      "}"
      "QPushButton:hover {"
      "  background-color: %1;"
      "}"
      "QPushButton:pressed {"
      "  background-color: rgba(255, 255, 255, 0.12);"
      "}"
  ).arg(isDelete ? QStringLiteral("rgba(217, 48, 37, 0.20)") : QStringLiteral("rgba(255, 255, 255, 0.08)")));

  auto *layout = new QHBoxLayout(btn);
  layout->setContentsMargins(4, 0, 4, 0);
  layout->setSpacing(10);

  auto *iconLabel = new QLabel(btn);
  iconLabel->setPixmap(icon.pixmap(18, 18));
  iconLabel->setFixedSize(18, 18);
  layout->addWidget(iconLabel, 0, Qt::AlignVCenter);

  auto *titleLabel = new QLabel(text, btn);
  titleLabel->setStyleSheet(QStringLiteral(
      "color: %1; font-size: 13px; font-weight: 500;"
  ).arg(isDelete ? QStringLiteral("#f28b82") : QStringLiteral("#f1f3f4")));
  layout->addWidget(titleLabel, 1, Qt::AlignVCenter);

  if (!shortcut.isEmpty()) {
    auto *shortcutLabel = new QLabel(shortcut, btn);
    shortcutLabel->setStyleSheet(QStringLiteral("color: #9aa0a6; font-size: 11px; font-weight: 500;"));
    layout->addWidget(shortcutLabel, 0, Qt::AlignVCenter);
  }

  return btn;
}

}  // namespace

TabGroupPopup::TabGroupPopup(TabGroupModel *model, QWidget *parent)
    : QWidget(parent, Qt::Popup | Qt::FramelessWindowHint), model_(model) {
  setAttribute(Qt::WA_DeleteOnClose, false);
  setupUi();
}

void TabGroupPopup::setupUi() {
  setFixedWidth(290);
  setStyleSheet(QStringLiteral(
      "QWidget#groupPopupRoot {"
      "  background-color: #28292a;"
      "  border: 1px solid rgba(255, 255, 255, 0.12);"
      "  border-radius: 10px;"
      "}"
      "QLineEdit#groupNameEdit {"
      "  background-color: #1f1f1f;"
      "  color: #ffffff;"
      "  border: 2px solid #a8c7fa;"
      "  border-radius: 6px;"
      "  padding: 6px 10px;"
      "  font-size: 13px;"
      "}"
      "QLineEdit#groupNameEdit:focus {"
      "  border: 2px solid #a8c7fa;"
      "}"
      "QFrame.divider {"
      "  background-color: #3c4043;"
      "  max-height: 1px;"
      "  min-height: 1px;"
      "}"
  ));
  setObjectName(QStringLiteral("groupPopupRoot"));

  auto *mainLayout = new QVBoxLayout(this);
  mainLayout->setContentsMargins(12, 12, 12, 12);
  mainLayout->setSpacing(8);

  // 1. Group Name Input
  nameEdit_ = new QLineEdit(this);
  nameEdit_->setObjectName(QStringLiteral("groupNameEdit"));
  nameEdit_->setPlaceholderText(I18n::text(QStringLiteral("tabgroup.name_placeholder")));
  mainLayout->addWidget(nameEdit_);

  connect(nameEdit_, &QLineEdit::textChanged, this, [this](const QString &text) {
    if (!model_ || currentGroupId_.isNull()) return;
    auto optGroup = model_->group(currentGroupId_);
    if (optGroup.has_value()) {
      auto group = *optGroup;
      group.name = text.trimmed();
      model_->addOrUpdateGroup(group);
    }
  });

  // 2. Color Palette Swatches (9 colors)
  colorPaletteRow_ = new QWidget(this);
  auto *paletteLayout = new QHBoxLayout(colorPaletteRow_);
  paletteLayout->setContentsMargins(0, 2, 0, 4);
  paletteLayout->setSpacing(5);

  const auto &palette = tabGroupColorPalette();
  for (const QColor &c : palette) {
    auto *btn = new QPushButton(colorPaletteRow_);
    btn->setFixedSize(22, 22);
    btn->setCursor(Qt::PointingHandCursor);
    btn->setStyleSheet(QStringLiteral("border: none; background: transparent; padding: 0px;"));
    btn->setIconSize(QSize(22, 22));
    btn->setIcon(QIcon(renderColorSwatch(c, false)));
    btn->setProperty("colorHex", c.name());

    connect(btn, &QPushButton::clicked, this, [this, c] {
      if (!model_ || currentGroupId_.isNull()) return;
      auto optGroup = model_->group(currentGroupId_);
      if (optGroup.has_value()) {
        auto group = *optGroup;
        group.color = c;
        model_->addOrUpdateGroup(group);
        updateColorSelection(c);
      }
    });

    paletteLayout->addWidget(btn);
    colorButtons_.append(btn);
  }
  mainLayout->addWidget(colorPaletteRow_);

  // Separator
  auto *sep1 = new QFrame(this);
  sep1->setProperty("class", QStringLiteral("divider"));
  mainLayout->addWidget(sep1);

  // 3. Action: Grupta yeni sekme (Alt+Shift+C)
  auto *newTabBtn = createActionRow(this, createNewTabInGroupIcon(),
                                    I18n::text(QStringLiteral("tabgroup.new_tab_in_group")),
                                    QStringLiteral("Alt+Shift+C"));
  connect(newTabBtn, &QPushButton::clicked, this, [this] {
    const QUuid id = currentGroupId_;
    close();
    emit newTabInGroupRequested(id);
  });
  mainLayout->addWidget(newTabBtn);

  // 4. Action: Grubu yeni pencereye taşı
  auto *moveWindowBtn = createActionRow(this, createMoveWindowIcon(),
                                        I18n::text(QStringLiteral("tabgroup.move_to_new_window")));
  connect(moveWindowBtn, &QPushButton::clicked, this, [this] {
    const QUuid id = currentGroupId_;
    close();
    emit moveGroupToNewWindowRequested(id);
  });
  mainLayout->addWidget(moveWindowBtn);

  // 5. Action: Grubu kapat (Alt+Shift+W)
  auto *closeGroupBtn = createActionRow(this, createCloseGroupIcon(),
                                        I18n::text(QStringLiteral("tabgroup.close_group")),
                                        QStringLiteral("Alt+Shift+W"));
  connect(closeGroupBtn, &QPushButton::clicked, this, [this] {
    const QUuid id = currentGroupId_;
    close();
    emit closeGroupRequested(id);
  });
  mainLayout->addWidget(closeGroupBtn);

  // Separator
  auto *sep2 = new QFrame(this);
  sep2->setProperty("class", QStringLiteral("divider"));
  mainLayout->addWidget(sep2);

  // 6. Action: Grubu çöz
  auto *ungroupBtn = createActionRow(this, createUngroupIcon(),
                                     I18n::text(QStringLiteral("tabgroup.ungroup")));
  connect(ungroupBtn, &QPushButton::clicked, this, [this] {
    const QUuid id = currentGroupId_;
    close();
    emit ungroupRequested(id);
  });
  mainLayout->addWidget(ungroupBtn);

  // 7. Action: Grubu sil (opens confirmation dialog)
  auto *deleteGroupBtn = createActionRow(this, createDeleteGroupIcon(),
                                         I18n::text(QStringLiteral("tabgroup.delete_group")),
                                         QString(), true);
  connect(deleteGroupBtn, &QPushButton::clicked, this, [this] {
    showDeleteConfirmation();
  });
  mainLayout->addWidget(deleteGroupBtn);

  // Separator
  auto *sep3 = new QFrame(this);
  sep3->setProperty("class", QStringLiteral("divider"));
  mainLayout->addWidget(sep3);

  // 8. Footer notice (matching Chrome reference)
  auto *footerLabel = new QLabel(
      I18n::text(QStringLiteral("tabgroup.sync_notice")),
      this);
  footerLabel->setWordWrap(true);
  footerLabel->setStyleSheet(QStringLiteral("color: #9aa0a6; font-size: 11px; line-height: 14px; padding: 2px 4px;"));
  mainLayout->addWidget(footerLabel);
}

void TabGroupPopup::updateColorSelection(const QColor &color) {
  const QString currentHex = color.name().toLower();
  for (auto *btn : colorButtons_) {
    const QString btnHex = btn->property("colorHex").toString().toLower();
    const bool isSelected = (btnHex == currentHex);
    btn->setIcon(QIcon(renderColorSwatch(QColor(btnHex), isSelected)));
  }
}

void TabGroupPopup::showForGroup(const QUuid &groupId, const QPoint &globalPosBelowChip) {
  if (!model_ || groupId.isNull()) return;
  currentGroupId_ = groupId;

  const auto optGroup = model_->group(groupId);
  if (!optGroup.has_value()) return;

  const auto &group = *optGroup;
  nameEdit_->blockSignals(true);
  nameEdit_->setText(group.name);
  nameEdit_->blockSignals(false);

  updateColorSelection(group.color);

  adjustSize();
  const QRect screenGeom = screen() ? screen()->availableGeometry() : QRect(0, 0, 1920, 1080);
  int x = globalPosBelowChip.x();
  int y = globalPosBelowChip.y() + 4;

  if (x + width() > screenGeom.right()) {
    x = screenGeom.right() - width() - 8;
  }
  if (x < screenGeom.left()) {
    x = screenGeom.left() + 8;
  }
  if (y + height() > screenGeom.bottom()) {
    y = globalPosBelowChip.y() - height() - 4;
  }

  move(x, y);
  show();
  raise();
  nameEdit_->setFocus();
  nameEdit_->selectAll();
}

void TabGroupPopup::showDeleteConfirmation() {
  QDialog dialog(this, Qt::Dialog | Qt::FramelessWindowHint);
  dialog.setModal(true);
  dialog.setFixedWidth(340);
  dialog.setStyleSheet(QStringLiteral(
      "QDialog {"
      "  background-color: #28292a;"
      "  color: #fbfbfe;"
      "  border: 1px solid rgba(255, 255, 255, 0.16);"
      "  border-radius: 10px;"
      "}"
      "QLabel#title {"
      "  font-size: 15px;"
      "  font-weight: 600;"
      "  color: #ffffff;"
      "}"
      "QLabel#body {"
      "  font-size: 13px;"
      "  color: #b1b1b3;"
      "  line-height: 18px;"
      "}"
      "QPushButton {"
      "  padding: 6px 16px;"
      "  border-radius: 6px;"
      "  font-size: 13px;"
      "  font-weight: 500;"
      "}"
      "QPushButton#cancelBtn {"
      "  background-color: #3c4043;"
      "  color: #ffffff;"
      "  border: none;"
      "}"
      "QPushButton#cancelBtn:hover {"
      "  background-color: #4f5357;"
      "}"
      "QPushButton#deleteBtn {"
      "  background-color: #d93025;"
      "  color: #ffffff;"
      "  border: none;"
      "}"
      "QPushButton#deleteBtn:hover {"
      "  background-color: #ea4335;"
      "}"
  ));

  auto *layout = new QVBoxLayout(&dialog);
  layout->setContentsMargins(18, 18, 18, 18);
  layout->setSpacing(14);

  auto *titleLabel = new QLabel(I18n::text(QStringLiteral("tabgroup.close_and_delete_title")), &dialog);
  titleLabel->setObjectName(QStringLiteral("title"));
  layout->addWidget(titleLabel);

  auto *bodyLabel = new QLabel(
      I18n::text(QStringLiteral("tabgroup.close_and_delete_desc")),
      &dialog);
  bodyLabel->setObjectName(QStringLiteral("body"));
  bodyLabel->setWordWrap(true);
  layout->addWidget(bodyLabel);

  auto *btnBox = new QHBoxLayout();
  btnBox->addStretch();

  auto *cancelBtn = new QPushButton(I18n::text(QStringLiteral("common.cancel")), &dialog);
  cancelBtn->setObjectName(QStringLiteral("cancelBtn"));
  cancelBtn->setDefault(true);
  cancelBtn->setCursor(Qt::PointingHandCursor);
  btnBox->addWidget(cancelBtn);

  auto *confirmDeleteBtn = new QPushButton(I18n::text(QStringLiteral("tabgroup.delete_group")), &dialog);
  confirmDeleteBtn->setObjectName(QStringLiteral("deleteBtn"));
  confirmDeleteBtn->setCursor(Qt::PointingHandCursor);
  btnBox->addWidget(confirmDeleteBtn);

  layout->addLayout(btnBox);

  connect(cancelBtn, &QPushButton::clicked, &dialog, &QDialog::reject);
  connect(confirmDeleteBtn, &QPushButton::clicked, &dialog, &QDialog::accept);

  if (dialog.exec() == QDialog::Accepted) {
    const QUuid id = currentGroupId_;
    close();
    emit deleteGroupRequested(id);
  }
}

void TabGroupPopup::keyPressEvent(QKeyEvent *event) {
  if (event->key() == Qt::Key_Escape) {
    close();
    event->accept();
    return;
  }
  if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
    close();
    event->accept();
    return;
  }
  QWidget::keyPressEvent(event);
}

}  // namespace dalinira::desktop_tabs
