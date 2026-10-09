#pragma once

#include <QPainter>
#include <QPaintEvent>
#include <QPen>
#include <QRadioButton>

namespace dalinira::onboarding {

/**
 * ModernRadioButton
 * Premium modern radio button with crisp antialiased rendering,
 * emerald-green active ring and inner dot, and frosted inactive ring.
 */
class ModernRadioButton : public QRadioButton {
 public:
  explicit ModernRadioButton(QWidget *parent = nullptr, int diameter = 20)
      : QRadioButton(parent), diameter_(diameter) {
    setCursor(Qt::PointingHandCursor);
    const int boxSz = diameter_ + 4;
    setFixedSize(boxSz, boxSz);
    setFocusPolicy(Qt::NoFocus);
    setStyleSheet(QStringLiteral("background: transparent; border: none; padding: 0px; margin: 0px;"));
  }

  void setDiameter(int diameter) {
    diameter_ = diameter;
    const int boxSz = diameter_ + 4;
    setFixedSize(boxSz, boxSz);
    update();
  }

  int diameter() const { return diameter_; }

 protected:
  void paintEvent(QPaintEvent *event) override {
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);

    const qreal sz = static_cast<qreal>(diameter_);
    const QRectF r((width() - sz) / 2.0, (height() - sz) / 2.0, sz, sz);

    if (isChecked()) {
      // Modern emerald-green active ring (#10b981)
      const qreal penW = diameter_ <= 15 ? 1.6 : 2.2;
      p.setPen(QPen(QColor(16, 185, 129), penW));
      p.setBrush(QColor(16, 185, 129, 36));
      p.drawEllipse(r);

      // Inner dot: vibrant modern emerald green (#10b981)
      const qreal dotSz = diameter_ <= 15 ? 5.5 : 9.5;
      const QRectF dotR((width() - dotSz) / 2.0, (height() - dotSz) / 2.0, dotSz, dotSz);
      p.setPen(Qt::NoPen);
      p.setBrush(QColor(16, 185, 129));
      p.drawEllipse(dotR);
    } else {
      // Inactive: sleek frosted glass ring
      const qreal penW = diameter_ <= 15 ? 1.3 : 1.8;
      p.setPen(QPen(QColor(255, 255, 255, 75), penW));
      p.setBrush(QColor(255, 255, 255, 12));
      p.drawEllipse(r);
    }
  }

 private:
  int diameter_{20};
};

}  // namespace dalinira::onboarding
