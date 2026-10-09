#pragma once

#include <QWidget>
#include <QTimer>
#include <QColor>
#include <vector>

namespace dalinira::onboarding {

class AuroraWaveBackground final : public QWidget {
  Q_OBJECT

 public:
  explicit AuroraWaveBackground(QWidget *parent = nullptr);
  ~AuroraWaveBackground() override = default;

  void startAnimation();
  void stopAnimation();

 protected:
  void paintEvent(QPaintEvent *event) override;
  void showEvent(QShowEvent *event) override;
  void hideEvent(QHideEvent *event) override;

 private:
  void updateAnimation();

  QTimer *animTimer_ = nullptr;
  float phase_ = 0.0f;
};

}  // namespace dalinira::onboarding
