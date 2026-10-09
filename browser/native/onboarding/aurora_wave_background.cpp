#include "aurora_wave_background.h"
#include "onboarding_manager.h"

#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QLinearGradient>
#include <cmath>

namespace dalinira::onboarding {

AuroraWaveBackground::AuroraWaveBackground(QWidget *parent)
    : QWidget(parent) {
  setAttribute(Qt::WA_OpaquePaintEvent, false);
  setAttribute(Qt::WA_NoSystemBackground, true);

  animTimer_ = new QTimer(this);
  animTimer_->setInterval(28); // ~35 FPS, silky smooth and very light on CPU
  connect(animTimer_, &QTimer::timeout, this, &AuroraWaveBackground::updateAnimation);
}

void AuroraWaveBackground::startAnimation() {
  if (OnboardingManager::animationsEnabled()) {
    if (!animTimer_->isActive()) {
      animTimer_->start();
    }
  }
}

void AuroraWaveBackground::stopAnimation() {
  animTimer_->stop();
}

void AuroraWaveBackground::showEvent(QShowEvent *event) {
  QWidget::showEvent(event);
  startAnimation();
}

void AuroraWaveBackground::hideEvent(QHideEvent *event) {
  QWidget::hideEvent(event);
  stopAnimation();
}

void AuroraWaveBackground::updateAnimation() {
  phase_ += 0.022f;
  if (phase_ > 628.3185f) { // 100 * 2pi
    phase_ -= 628.3185f;
  }
  update();
}

void AuroraWaveBackground::paintEvent(QPaintEvent * /*event*/) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

  const int w = width();
  const int h = height();
  if (w <= 0 || h <= 0) return;

  const float p = phase_;
  const float maxDim = static_cast<float>(std::max(w, h));

  // 1. Deep Velvet Midnight Canvas with Subtle Emerald / Navy Tint
  QLinearGradient baseGrad(0, 0, w, h);
  baseGrad.setColorAt(0.0, QColor(8, 16, 26));
  baseGrad.setColorAt(0.4, QColor(10, 24, 28));
  baseGrad.setColorAt(0.7, QColor(12, 18, 34));
  baseGrad.setColorAt(1.0, QColor(6, 12, 20));
  painter.fillRect(rect(), baseGrad);

  // 2. Dynamic Harmonic Aurora Mesh Orbs (Rainbow Pastel Glows)
  struct AuroraOrb {
    float x;
    float y;
    float radius;
    QColor centerColor;
  };

  const AuroraOrb orbs[5] = {
    // Orb 1: Zen Sage / Emerald / Mint Green (Iconic Zen look)
    {
      w * (0.35f + 0.18f * std::sin(p * 0.70f)),
      h * (0.32f + 0.16f * std::cos(p * 0.85f)),
      maxDim * (0.55f + 0.04f * std::sin(p * 0.50f)),
      QColor(16, 185, 129, 140)
    },
    // Orb 2: Ocean Turquoise / Cyan / Sky Blue
    {
      w * (0.68f + 0.18f * std::cos(p * 0.60f + 0.8f)),
      h * (0.34f + 0.16f * std::sin(p * 0.75f + 0.4f)),
      maxDim * (0.58f + 0.05f * std::cos(p * 0.45f)),
      QColor(6, 182, 212, 130)
    },
    // Orb 3: Dreamy Lavender / Royal Violet / Purple
    {
      w * (0.54f + 0.20f * std::sin(p * 0.50f + 2.0f)),
      h * (0.66f + 0.18f * std::cos(p * 0.65f + 1.2f)),
      maxDim * (0.54f + 0.04f * std::sin(p * 0.60f)),
      QColor(139, 92, 246, 120)
    },
    // Orb 4: Sunset Rose / Coral / Soft Magenta
    {
      w * (0.22f + 0.16f * std::cos(p * 0.68f + 2.5f)),
      h * (0.64f + 0.18f * std::sin(p * 0.55f + 1.8f)),
      maxDim * (0.50f + 0.04f * std::cos(p * 0.50f)),
      QColor(244, 63, 94, 110)
    },
    // Orb 5: Golden Amber / Sunbeam Warmth
    {
      w * (0.76f + 0.14f * std::sin(p * 0.80f + 3.8f)),
      h * (0.76f + 0.14f * std::cos(p * 0.45f + 3.0f)),
      maxDim * (0.46f + 0.04f * std::sin(p * 0.70f)),
      QColor(245, 158, 11, 85)
    }
  };

  // Render glowing orbs with soft radial falloff
  for (int i = 0; i < 5; ++i) {
    const auto &orb = orbs[i];
    QRadialGradient grad(QPointF(orb.x, orb.y), orb.radius, QPointF(orb.x, orb.y));
    grad.setColorAt(0.0, orb.centerColor);
    QColor mid = orb.centerColor;
    mid.setAlpha(orb.centerColor.alpha() / 2);
    grad.setColorAt(0.5, mid);
    QColor edge = orb.centerColor;
    edge.setAlpha(0);
    grad.setColorAt(1.0, edge);

    painter.setBrush(grad);
    painter.setPen(Qt::NoPen);
    painter.drawEllipse(QPointF(orb.x, orb.y), orb.radius, orb.radius);
  }

  // 3. Undulating Liquid Aurora Wave Ribbons ("dalgalı")
  // Harmonic waves sweeping smoothly across the canvas
  const auto drawWaveRibbon = [&](float baseYRatio, float amplitude1, float freq1, float speed1,
                                  float amplitude2, float freq2, float speed2,
                                  const QColor &startColor, const QColor &endColor) {
    QPainterPath path;
    path.moveTo(0, h);

    const int stepX = 16;
    for (int x = 0; x <= w + stepX; x += stepX) {
      const float xf = static_cast<float>(x);
      const float y = h * baseYRatio
                    + amplitude1 * std::sin(xf * freq1 + p * speed1)
                    + amplitude2 * std::cos(xf * freq2 - p * speed2);
      if (x == 0) {
        path.lineTo(xf, y);
      } else {
        path.lineTo(xf, y);
      }
    }
    path.lineTo(w, h);
    path.closeSubpath();

    QLinearGradient waveGrad(0, h * baseYRatio - amplitude1, w, h);
    waveGrad.setColorAt(0.0, startColor);
    waveGrad.setColorAt(1.0, endColor);

    painter.setBrush(waveGrad);
    painter.setPen(Qt::NoPen);
    painter.drawPath(path);
  };

  // Wave 1: Seafoam & Emerald Wave
  drawWaveRibbon(0.55f, 42.0f, 0.0035f, 1.1f, 24.0f, 0.0070f, 0.85f,
                 QColor(16, 185, 129, 80), QColor(52, 211, 153, 20));

  // Wave 2: Cyan & Sky Wave
  drawWaveRibbon(0.66f, 48.0f, 0.0030f, 0.95f, 22.0f, 0.0062f, 1.15f,
                 QColor(6, 182, 212, 70), QColor(14, 165, 233, 15));

  // Wave 3: Violet & Rose Wave
  drawWaveRibbon(0.78f, 38.0f, 0.0042f, 0.85f, 20.0f, 0.0085f, 1.05f,
                 QColor(139, 92, 246, 60), QColor(244, 63, 94, 10));

  // 4. Subtle Shimmer Aurora Curtain Lines (Upper Atmosphere)
  for (int c = 0; c < 2; ++c) {
    QPainterPath curtain;
    const float offset = c * 1.6f;
    curtain.moveTo(0, h * 0.25f);
    for (int x = 0; x <= w + 20; x += 20) {
      const float xf = static_cast<float>(x);
      const float y = h * (0.28f + 0.06f * c)
                    + 28.0f * std::sin(xf * 0.004f + p * 0.7f + offset)
                    + 16.0f * std::cos(xf * 0.009f - p * 0.5f);
      curtain.lineTo(xf, y);
    }
    QPen curtainPen(QColor(255, 255, 255, 16 + c * 8), 1.5);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(curtainPen);
    painter.drawPath(curtain);
  }

  // 5. Soft Vignette around border to frame content beautifully
  QRadialGradient vignette(QPointF(w * 0.5, h * 0.5), maxDim * 0.75, QPointF(w * 0.5, h * 0.5));
  vignette.setColorAt(0.0, QColor(0, 0, 0, 0));
  vignette.setColorAt(0.65, QColor(0, 0, 0, 15));
  vignette.setColorAt(1.0, QColor(0, 0, 0, 85));
  painter.setBrush(vignette);
  painter.setPen(Qt::NoPen);
  painter.fillRect(rect(), vignette);
}

}  // namespace dalinira::onboarding
