#include "big_listen_button.h"

#include <QPainterPath>
#include <QRadialGradient>
#include <cmath>
#include <numbers>
#include "i18n/i18n.h"

BigListenButton::BigListenButton(QWidget *parent) : BigListenButton(140, parent) {}

BigListenButton::BigListenButton(int diameter, QWidget *parent)
    : QAbstractButton(parent), diameter_(diameter) {
  setObjectName(QStringLiteral("big-listen-button"));
  setFixedSize(diameter_, diameter_);
  setCursor(Qt::PointingHandCursor);
  setFocusPolicy(Qt::StrongFocus);
  setAccessibleName(I18n::text(QStringLiteral("pulse.listen_button")));
  setToolTip(I18n::text(QStringLiteral("pulse.listen_toggle")));

  animTimer_ = new QTimer(this);
  animTimer_->setInterval(33);  // ~30 FPS
  connect(animTimer_, &QTimer::timeout, this, &BigListenButton::onAnimationTick);
}

BigListenButton::~BigListenButton() {
  animTimer_->stop();
}

void BigListenButton::setDiameter(int diameter) {
  diameter_ = diameter;
  setFixedSize(diameter_, diameter_);
  update();
}

void BigListenButton::setListening(bool listening) {
  if (isListening_ == listening) return;
  isListening_ = listening;
  if (isListening_) {
    animationPhase_ = 0.0;
    animTimer_->start();
    setToolTip(I18n::text(QStringLiteral("pulse.stop_listening")));
  } else {
    animTimer_->stop();
    levelPercent_ = 0.0;
    setToolTip(I18n::text(QStringLiteral("pulse.start_listening")));
    update();
  }
}

void BigListenButton::setLevel(double levelPercent) {
  levelPercent_ = std::clamp(levelPercent, 0.0, 100.0);
  if (isListening_) {
    update();
  }
}

void BigListenButton::onAnimationTick() {
  if (!isListening_) return;
  animationPhase_ += 0.08;
  if (animationPhase_ >= 2.0 * std::numbers::pi_v<double>) {
    animationPhase_ -= 2.0 * std::numbers::pi_v<double>;
  }
  update();
}

void BigListenButton::enterEvent(QEnterEvent *event) {
  Q_UNUSED(event);
  isHovered_ = true;
  update();
}

void BigListenButton::leaveEvent(QEvent *event) {
  Q_UNUSED(event);
  isHovered_ = false;
  update();
}

void BigListenButton::paintEvent(QPaintEvent *event) {
  Q_UNUSED(event);
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);

  const qreal w = width();
  const qreal h = height();
  const QPointF center(w / 2.0, h / 2.0);
  const double scale = w / 140.0;

  // Center button radius (leaving room for continuous outer concentric rings)
  const qreal centerRadius = (w / 2.0) * 0.72;

  // Outer animated acoustic rings when listening
  if (isListening_) {
    const double sinVal = (std::sin(animationPhase_) + 1.0) / 2.0;  // 0.0 to 1.0
    const double levelFactor = std::min(1.0, levelPercent_ / 50.0);

    // 1. Soft ambient glow behind rings
    QRadialGradient ambientGlow(center, w / 2.0);
    ambientGlow.setColorAt(0.0, QColor(56, 189, 248, static_cast<int>(35 + sinVal * 20 + levelFactor * 30)));
    ambientGlow.setColorAt(0.65, QColor(30, 110, 220, static_cast<int>(15 + sinVal * 15)));
    ambientGlow.setColorAt(1.0, QColor(14, 75, 160, 0));
    painter.setPen(Qt::NoPen);
    painter.setBrush(ambientGlow);
    painter.drawEllipse(center, w / 2.0 - 1.0, w / 2.0 - 1.0);

    const qreal minRippleR = centerRadius + 4.0 * scale;
    const qreal maxRippleR = (w / 2.0) - 3.0 * scale;

    // 2. Inner Concentric Ring (İç Halka)
    const qreal r_inner = centerRadius + (6.0 + sinVal * 2.0 + levelFactor * 4.0) * scale;
    QPen penInner(QColor(126, 224, 255, static_cast<int>(160 + levelFactor * 70)), 1.5 * scale, Qt::SolidLine);
    painter.setPen(penInner);
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(center, r_inner, r_inner);

    // 3. Middle Continuous Expanding Concentric Ripple Ring (Orta Halka)
    const double prog1 = std::fmod(animationPhase_ / (2.0 * std::numbers::pi_v<double>), 1.0);
    const qreal r_rip1 = minRippleR + prog1 * (maxRippleR - minRippleR);
    const int alpha1 = static_cast<int>((1.0 - prog1) * (180.0 + levelFactor * 70.0));
    if (alpha1 > 0) {
      QPen ripPen1(QColor(90, 210, 255, alpha1), (1.4 + (1.0 - prog1) * 0.8) * scale, Qt::SolidLine);
      painter.setPen(ripPen1);
      painter.setBrush(Qt::NoBrush);
      painter.drawEllipse(center, r_rip1, r_rip1);
    }

    // 4. Outer Continuous Expanding Concentric Ripple Ring (Dış Halka - 0.5 phase offset)
    const double prog2 = std::fmod(prog1 + 0.5, 1.0);
    const qreal r_rip2 = minRippleR + prog2 * (maxRippleR - minRippleR);
    const int alpha2 = static_cast<int>((1.0 - prog2) * (150.0 + levelFactor * 80.0));
    if (alpha2 > 0) {
      QPen ripPen2(QColor(56, 189, 248, alpha2), (1.3 + (1.0 - prog2) * 0.7) * scale, Qt::SolidLine);
      painter.setPen(ripPen2);
      painter.setBrush(Qt::NoBrush);
      painter.drawEllipse(center, r_rip2, r_rip2);
    }

    // 5. Outer Subtle Boundary Ring (Dış İnce Halka)
    const qreal r_outer = (w / 2.0) - 2.5 * scale;
    QPen penOuter(QColor(56, 189, 248, static_cast<int>(50 + sinVal * 25 + levelFactor * 40)), 1.1 * scale, Qt::SolidLine);
    painter.setPen(penOuter);
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(center, r_outer, r_outer);
  } else {
    // Subtle faint idle ring
    const qreal r_idle = centerRadius + 5.0 * scale;
    QPen penIdle(QColor(56, 189, 248, isHovered_ ? 60 : 30), 1.0 * scale, Qt::SolidLine);
    painter.setPen(penIdle);
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(center, r_idle, r_idle);
  }

  // Base button circle gradient (Center button stays completely fixed)
  QRadialGradient grad(center, centerRadius);
  if (isListening_) {
    grad.setColorAt(0.0, QColor(64, 158, 255));
    grad.setColorAt(0.6, QColor(30, 110, 220));
    grad.setColorAt(1.0, QColor(14, 75, 160));
  } else if (isHovered_) {
    grad.setColorAt(0.0, QColor(48, 85, 118));
    grad.setColorAt(0.7, QColor(32, 60, 86));
    grad.setColorAt(1.0, QColor(22, 42, 62));
  } else {
    grad.setColorAt(0.0, QColor(38, 68, 96));
    grad.setColorAt(0.7, QColor(25, 48, 70));
    grad.setColorAt(1.0, QColor(18, 34, 50));
  }

  painter.setBrush(grad);
  if (isListening_) {
    painter.setPen(QPen(QColor(140, 220, 255), 2.2 * scale));
  } else if (isHovered_) {
    painter.setPen(QPen(QColor(100, 180, 230), 1.8 * scale));
  } else {
    painter.setPen(QPen(QColor(60, 120, 170), 1.4 * scale));
  }

  painter.drawEllipse(center, centerRadius, centerRadius);

  // Draw Equalizer / Soundwave Icon in Center
  painter.setPen(Qt::NoPen);
  const QColor barColor = isListening_ ? QColor(255, 255, 255) : (isHovered_ ? QColor(210, 240, 255) : QColor(170, 215, 245));
  painter.setBrush(barColor);

  // 4 vertical rounded rectangles (Equalizer bars)
  const double baseHeights[4] = {18.0 * scale, 32.0 * scale, 42.0 * scale, 24.0 * scale};
  const double barWidth = std::max(3.0, 6.0 * scale);
  const double barSpacing = std::max(3.0, 6.0 * scale);
  const double totalWidth = 4 * barWidth + 3 * barSpacing;
  const double startX = center.x() - totalWidth / 2.0;

  for (int i = 0; i < 4; ++i) {
    double h_bar = baseHeights[i];
    if (isListening_) {
      const double wave = std::sin(animationPhase_ * 1.5 + i * 1.2);
      const double levelMod = (levelPercent_ / 100.0) * (16.0 * scale);
      h_bar += wave * 8.0 * scale + levelMod;
      h_bar = std::clamp(h_bar, 6.0 * scale, 52.0 * scale);
    }
    const double x = startX + i * (barWidth + barSpacing);
    const double y = center.y() - h_bar / 2.0;
    painter.drawRoundedRect(QRectF(x, y, barWidth, h_bar), 2.5 * scale, 2.5 * scale);
  }
}
