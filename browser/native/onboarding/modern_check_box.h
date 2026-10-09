#pragma once

#include <QCheckBox>
#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPen>

namespace dalinira::onboarding {

/**
 * ModernCheckBox
 * Premium modern check box with crisp antialiased rendering,
 * emerald-green active state, round-capped checkmark,
 * frosted inactive border, and spacious typography.
 */
class ModernCheckBox : public QCheckBox {
 public:
  explicit ModernCheckBox(const QString &text, QWidget *parent = nullptr)
      : QCheckBox(text, parent) {
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_Hover, true);
    setStyleSheet(QStringLiteral("background: transparent; border: none; padding: 0px; margin: 0px;"));
  }

  QSize sizeHint() const override {
    const int textW = fontMetrics().horizontalAdvance(text());
    // 22px indicator + 10px spacing + text width + 10px padding
    return QSize(22 + 10 + textW + 10, 36);
  }

  QSize minimumSizeHint() const override {
    return sizeHint();
  }

 protected:
  void enterEvent(QEnterEvent *event) override {
    isHovered_ = true;
    update();
    QCheckBox::enterEvent(event);
  }

  void leaveEvent(QEvent *event) override {
    isHovered_ = false;
    update();
    QCheckBox::leaveEvent(event);
  }

  void paintEvent(QPaintEvent *event) override {
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const qreal boxSz = 22.0;
    const qreal cornerRadius = 6.0;
    const qreal boxY = (height() - boxSz) / 2.0;
    const QRectF boxRect(2.0, boxY, boxSz, boxSz);

    const bool checked = isChecked();
    const bool enabled = isEnabled();

    if (checked) {
      // Checked state: vibrant emerald gradient / fill (#10b981)
      p.setPen(QPen(enabled ? QColor(16, 185, 129) : QColor(16, 185, 129, 130), 1.5));
      p.setBrush(enabled ? QColor(16, 185, 129) : QColor(16, 185, 129, 130));
      p.drawRoundedRect(boxRect, cornerRadius, cornerRadius);

      // White crisp antialiased checkmark
      QPainterPath checkPath;
      checkPath.moveTo(boxRect.x() + 5.5, boxRect.y() + 11.2);
      checkPath.lineTo(boxRect.x() + 9.2, boxRect.y() + 15.4);
      checkPath.lineTo(boxRect.x() + 16.5, boxRect.y() + 7.0);

      QPen checkPen(enabled ? QColor(255, 255, 255) : QColor(255, 255, 255, 180),
                    2.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
      p.strokePath(checkPath, checkPen);
    } else {
      // Unchecked state: frosted glass indicator with hover glow
      QColor borderColor;
      QColor fillColor;

      if (!enabled) {
        borderColor = QColor(255, 255, 255, 55);
        fillColor = QColor(255, 255, 255, 10);
      } else if (isHovered_) {
        borderColor = QColor(16, 185, 129, 220);
        fillColor = QColor(16, 185, 129, 30);
      } else {
        borderColor = QColor(255, 255, 255, 95);
        fillColor = QColor(255, 255, 255, 18);
      }

      p.setPen(QPen(borderColor, 1.6));
      p.setBrush(fillColor);
      p.drawRoundedRect(boxRect, cornerRadius, cornerRadius);
    }

    // Text rendering
    const qreal textLeft = boxRect.right() + 10.0;
    const QRectF textRect(textLeft, 0, width() - textLeft, height());

    QFont f = font();
    f.setPixelSize(13);
    f.setWeight(QFont::DemiBold);
    p.setFont(f);

    QColor textColor;
    if (!enabled) {
      textColor = QColor(148, 163, 184, 150);
    } else if (checked) {
      textColor = QColor(255, 255, 255);
    } else if (isHovered_) {
      textColor = QColor(241, 245, 249);
    } else {
      textColor = QColor(226, 232, 240);
    }

    p.setPen(textColor);
    p.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, text());
  }

 private:
  bool isHovered_ = false;
};

}  // namespace dalinira::onboarding
