#include "audio_effects_page.h"

#include "glow_toggle_switch.h"
#include "web_audio_effects_controller.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDial>
#include <QFocusEvent>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QMouseEvent>
#include <QPushButton>
#include <QPainter>
#include <QProgressBar>
#include <QScrollArea>
#include <QShowEvent>
#include <QHideEvent>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStyle>
#include <QSlider>
#include <QTimer>
#include <QWheelEvent>
#include <QVBoxLayout>
#include <QtMath>

#include <QEasingCurve>
#include <QPointer>
#include <QVariantAnimation>

#include <algorithm>
#include <array>
#include <cmath>
#include "i18n/i18n.h"
#include "i18n/widget_i18n.h"
#include "i18n/language_manager.h"

namespace {
// `name` is either a catalog key ("audio.*") or an untranslated DSP proper
// name such as "De-esser".
struct ModuleDefinition { const char *id; const char *name; const char *symbol; };

QString displayName(const char *nameOrKey) {
  const QString value = QString::fromUtf8(nameOrKey);
  return value.startsWith(QLatin1String("audio.")) ? I18n::text(value) : value;
}

// Controller status text in the active UI language (it may have been
// produced before a live language switch).
QString localizedDetail(const WebAudioEffectsController::Status &status) {
  if (status.detailKey.isEmpty()) return I18n::retranslated(status.detail);
  QString text = I18n::text(status.detailKey);
  for (const QString &arg : status.detailArgs) text = text.arg(arg);
  return text;
}

// Argument for a live-retranslated composed string (widget_i18n.h bind*).
QString nameArg(const char *nameOrKey) {
  const QString value = QString::fromUtf8(nameOrKey);
  return value.startsWith(QLatin1String("audio.")) ? QLatin1Char('@') + value : value;
}
constexpr std::array<ModuleDefinition, 22> kModules = {{
    {"output", "audio.module_output", "♫"}, {"eq32", "audio.module_eq32", "≋"}, {"reverb", "audio.reverb_heading", "↝"},
    {"compressor", "audio.module_compressor", "⌁"}, {"limiter", "audio.limiter", "┫"}, {"bassboost", "audio.module_bass_booster", "◖"},
    {"autogain", "audio.auto_gain", "↕"}, {"truepeak", "True Peak Limiter + Meter", "▥"}, {"peq", "audio.module_peq", "⌁"},
    {"dynamiceq", "Dynamic EQ", "⌁"}, {"exciter", "audio.module_exciter", "✦"}, {"deesser", "De-esser", "S"},
    {"noisegate", "audio.module_noise_gate", "⌁"}, {"stereowidener", "Stereo Widener v2", "↔"}, {"echo", "audio.module_echo", "↩"},
    {"softecho", "audio.module_soft_echo", "⌁"}, {"convreverb", "audio.module_conv_reverb", "▣"}, {"crossfeed", "audio.module_crossfeed", "◉"},
    {"surround", "Surround (5.1/7.1)", "▥"}, {"bassmono", "Bass Mono", "◍"}, {"tapesat", "Tape Saturation", "▤"}, {"bitdither", "Bit-depth / Dither", "▦"}
}};

QFrame *card(QWidget *parent) {
  auto *frame = new QFrame(parent);
  frame->setObjectName(QStringLiteral("audio-effects-card"));
  frame->setFrameShape(QFrame::StyledPanel);
  return frame;
}

QLabel *label(const QString &text, QWidget *parent, const QString &object = {}) {
  auto *result = new QLabel(text, parent);
  if (!object.isEmpty()) result->setObjectName(object);
  result->setWordWrap(true);
  return result;
}

QString frequencyLabel(int frequency) {
  if (frequency < 1000) return QString::number(frequency);
  const double kilohertz = frequency / 1000.0;
  return QStringLiteral("%1k").arg(kilohertz, 0, 'f', frequency % 1000 == 0 ? 0 : 1);
}

QString compactDecimal(double value, int maximumDecimals = 3) {
  QString text = QString::number(value, 'f', maximumDecimals);
  while (text.endsWith(QLatin1Char('0'))) text.chop(1);
  if (text.endsWith(QLatin1Char('.'))) text.chop(1);
  return text;
}

class FrequencyLabel final : public QLabel {
 public:
  explicit FrequencyLabel(const QString &text, QWidget *parent) : QLabel(text, parent) {
    setObjectName(QStringLiteral("audio-effects-eq-frequency"));
    setFixedSize(42, 30);
  }

 protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setPen(QColor(QStringLiteral("#9caab4")));
    QFont font = painter.font();
    font.setPointSize(8);
    painter.setFont(font);
    painter.translate(width() / 2.0, height() / 2.0 + 3.0);
    painter.rotate(-43.0);
    painter.drawText(QRect(-24, -9, 48, 18), Qt::AlignCenter, text());
  }
};

class EffectDial final : public QDial {
 public:
  enum class Style { Tone, Reverb };

  explicit EffectDial(Style style = Style::Tone, QWidget *parent = nullptr) : QDial(parent), style_(style) {
    setNotchesVisible(false);
    setWrapping(false);
    setTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    colorTimer_.setInterval(16);
    connect(this, &QDial::valueChanged, this, [this] { updateTargetColor(); });
    connect(&colorTimer_, &QTimer::timeout, this, [this] {
      const auto step = [](int from, int to) { return from + qRound((to - from) * 0.16); };
      currentCenterColor_.setRgb(step(currentCenterColor_.red(), targetCenterColor_.red()),
                                 step(currentCenterColor_.green(), targetCenterColor_.green()),
                                 step(currentCenterColor_.blue(), targetCenterColor_.blue()));
      update();
      if (qAbs(currentCenterColor_.red() - targetCenterColor_.red()) <= 1
          && qAbs(currentCenterColor_.green() - targetCenterColor_.green()) <= 1
          && qAbs(currentCenterColor_.blue() - targetCenterColor_.blue()) <= 1) {
        currentCenterColor_ = targetCenterColor_;
        colorTimer_.stop();
      }
    });
    updateTargetColor();
  }

  void refreshColor() {
    updateTargetColor();
    update();
  }

 protected:
  void mousePressEvent(QMouseEvent *event) override {
    if (event->button() != Qt::LeftButton) { event->ignore(); return; }
    dragStartPosition_ = event->position();
    dragStartValue_ = value();
    dragging_ = true;
    setFocus(Qt::MouseFocusReason);
    grabMouse();
    event->accept();
  }

  void mouseMoveEvent(QMouseEvent *event) override {
    if (!dragging_) { event->ignore(); return; }
    // Relative vertical dragging avoids QDial's angle-boundary jumps near the top point.
    const double range = std::max(1, maximum() - minimum());
    const int next = qRound(dragStartValue_ + (dragStartPosition_.y() - event->position().y()) * range / 240.0);
    setValue(qBound(minimum(), next, maximum()));
    event->accept();
  }

  void mouseReleaseEvent(QMouseEvent *event) override {
    if (event->button() == Qt::LeftButton && dragging_) {
      dragging_ = false;
      releaseMouse();
      event->accept();
      return;
    }
    event->ignore();
  }

  void wheelEvent(QWheelEvent *event) override {
    const int steps = event->angleDelta().y() != 0 ? event->angleDelta().y() / 120
                                                    : event->pixelDelta().y() / 20;
    if (steps != 0) setValue(qBound(minimum(), value() + steps * singleStep(), maximum()));
    // Do not propagate this event to the scroll area while a dial is under the cursor.
    event->accept();
  }

  void focusOutEvent(QFocusEvent *event) override {
    if (dragging_) {
      dragging_ = false;
      releaseMouse();
    }
    QDial::focusOutEvent(event);
  }

  void paintEvent(QPaintEvent *) override {
    const qreal inset = 8.0;
    const QRectF dialRect = rect().adjusted(inset, inset, -inset, -inset);
    const QPointF centre = dialRect.center();
    const qreal radius = dialRect.width() / 2.0;
    const qreal ratio = maximum() == minimum() ? 0.5
        : qBound(0.0, (value() - minimum()) / static_cast<double>(maximum() - minimum()), 1.0);
    constexpr qreal startDegrees = 225.0;
    constexpr qreal arcDegrees = -270.0;
    const qreal angle = startDegrees + arcDegrees * ratio;

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    if (style_ == Style::Reverb) {
      painter.setPen(Qt::NoPen);
      painter.setBrush(QColor(QStringLiteral("#262626")));
      painter.drawEllipse(dialRect);
    }
    QPen basePen(QColor(QStringLiteral("#374045")), 6.0, Qt::SolidLine, Qt::RoundCap);
    painter.setPen(basePen);
    painter.drawArc(dialRect, qRound(startDegrees * 16.0), qRound(arcDegrees * 16.0));
    QPen activePen(QColor(QStringLiteral("#29bdf1")), 6.0, Qt::SolidLine, Qt::RoundCap);
    painter.setPen(activePen);
    painter.drawArc(dialRect, qRound(startDegrees * 16.0), qRound(arcDegrees * ratio * 16.0));

    const qreal radians = qDegreesToRadians(angle);
    const QPointF knob(centre.x() + std::cos(radians) * radius, centre.y() - std::sin(radians) * radius);
    if (style_ == Style::Reverb) {
      painter.setPen(QPen(QColor(QStringLiteral("#57d8ff")), 1.4));
      painter.setBrush(QColor(QStringLiteral("#08202a")));
      painter.drawEllipse(knob, 7.0, 7.0);
      painter.setPen(Qt::NoPen);
      painter.setBrush(QColor(QStringLiteral("#696d70")));
      painter.drawEllipse(centre, 6.0, 6.0);
    } else {
      painter.setPen(QPen(QColor(QStringLiteral("#9af8b8")), 1.2));
      painter.setBrush(QColor(QStringLiteral("#159d58")));
      painter.drawEllipse(knob, 7.0, 7.0);
      painter.setPen(QPen(QColor(QStringLiteral("#05110a")), 1.0));
      painter.setBrush(QColor(QStringLiteral("#0b1711")));
      painter.drawEllipse(centre, 6.6, 6.6);
      painter.setPen(Qt::NoPen);
      painter.setBrush(currentCenterColor_);
      painter.drawEllipse(centre, 4.2, 4.2);
    }
  }

 private:
  static QColor blend(const QColor &from, const QColor &to, double amount) {
    return QColor(qRound(from.red() + (to.red() - from.red()) * amount),
                  qRound(from.green() + (to.green() - from.green()) * amount),
                  qRound(from.blue() + (to.blue() - from.blue()) * amount));
  }

  void updateTargetColor() {
    if (style_ == Style::Reverb) {
      colorTimer_.stop();
      return;
    }
    const double neutral = (minimum() + maximum()) / 2.0;
    const double range = std::max(1.0, std::max(neutral - minimum(), maximum() - neutral));
    const double intensity = qBound(0.0, std::abs(value() - neutral) / range, 1.0);
    const QColor green(QStringLiteral("#35d07f"));
    const QColor yellow(QStringLiteral("#f1d34f"));
    const QColor red(QStringLiteral("#f05b52"));
    targetCenterColor_ = intensity <= 0.5 ? blend(green, yellow, intensity * 2.0)
                                          : blend(yellow, red, (intensity - 0.5) * 2.0);
    if (!colorTimer_.isActive()) colorTimer_.start();
  }

  QColor currentCenterColor_ = QColor(QStringLiteral("#35d07f"));
  QColor targetCenterColor_ = currentCenterColor_;
  QTimer colorTimer_;
  QPointF dragStartPosition_;
  int dragStartValue_ = 0;
  bool dragging_ = false;
  Style style_ = Style::Tone;
};
}  // namespace

AudioEffectsPage::AudioEffectsPage(WebAudioEffectsController *controller, QWidget *parent)
    : QWidget(parent), controller_(controller) {
  setObjectName(QStringLiteral("audio-effects-page"));
  auto *root = new QVBoxLayout(this);
  root->setContentsMargins(0, 0, 0, 0);
  root->setSpacing(0);

  auto *top = new QFrame(this);
  top->setObjectName(QStringLiteral("audio-effects-topbar"));
  auto *topLayout = new QHBoxLayout(top);
  topLayout->setContentsMargins(18, 10, 18, 10);
  auto *icon = label(QStringLiteral("♫"), top, QStringLiteral("audio-effects-top-icon"));
  icon->setAlignment(Qt::AlignCenter);
  icon->setFixedSize(30, 30);
  globalToggle_ = new GlowToggleSwitch(top);
  globalToggle_->setObjectName(QStringLiteral("audio-effects-global-toggle"));
  globalToggle_->setChecked(controller_ && controller_->enabled());
  statusLabel_ = label({}, top, QStringLiteral("audio-effects-runtime-status"));
  statusLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  topLayout->addWidget(icon);
  topLayout->addWidget(globalToggle_);
  topLayout->addStretch();
  topLayout->addWidget(statusLabel_);
  root->addWidget(top);

  auto *body = new QFrame(this);
  auto *bodyLayout = new QHBoxLayout(body);
  bodyLayout->setContentsMargins(0, 0, 0, 0);
  bodyLayout->setSpacing(0);
  navigation_ = new QListWidget(body);
  navigation_->setObjectName(QStringLiteral("audio-effects-navigation"));
  navigation_->setIconSize(QSize(20, 20));
  navigation_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  for (const ModuleDefinition &module : kModules) {
    auto *item = new QListWidgetItem(QIcon(QStringLiteral(":/side-widget-icons/sound-effects.svg")), QString());
    dalinira::i18n::bindItemText(item, QStringLiteral("=%1   %2"),
                                 {QString::fromUtf8(module.symbol), nameArg(module.name)});
    item->setToolTip(displayName(module.name));
    navigation_->addItem(item);
  }
  pages_ = new QStackedWidget(body);
  pages_->setObjectName(QStringLiteral("audio-effects-module-pages"));
  createOutputPage();
  createEqualizerPage();
  createReverbPage();
  createCompressorPage();
  createLimiterPage();
  createBassEnhancerPage();
  createAutoGainPage();
  createPlaceholderPages();
  bodyLayout->addWidget(navigation_);
  bodyLayout->addWidget(pages_, 1);
  root->addWidget(body, 1);

  connect(navigation_, &QListWidget::currentRowChanged, pages_, &QStackedWidget::setCurrentIndex);
  connect(navigation_, &QListWidget::currentRowChanged, this, [this] {
    if (controller_) {
      controller_->setPanelVisible(isVisible(), currentSubpanelId());
    }
    updateCompressorMeterPolling();
    updateLimiterMeterPolling();
    updateAutoGainStatusPolling();
  });
  connect(globalToggle_, &QCheckBox::toggled, controller_, &WebAudioEffectsController::setEnabled);
  controllerSyncTimer_.setSingleShot(true);
  controllerSyncTimer_.setInterval(90);
  connect(&controllerSyncTimer_, &QTimer::timeout, this, &AudioEffectsPage::updateFromController);
  if (controller_) {
    // A drag can produce dozens of state values per second.  The originating
    // control already shows the current value, so repaint the full 32-band
    // page as one short, coalesced update instead of on every audio command.
    connect(controller_, &WebAudioEffectsController::stateChanged, this, &AudioEffectsPage::scheduleControllerSync);
    connect(controller_, &WebAudioEffectsController::statusChanged, this, &AudioEffectsPage::scheduleControllerSync);
    connect(controller_, &WebAudioEffectsController::compressorGainReductionChanged, this,
            [this](double reductionDb, bool available) {
      if (!compressorGainReductionMeter_ || !compressorGainReductionValue_) return;
      const double safeReduction = available ? qBound(-24.0, reductionDb, 0.0) : 0.0;
      compressorGainReductionMeter_->setValue(qRound(std::abs(safeReduction) * 10.0));
      compressorGainReductionValue_->setText(QStringLiteral("%1 dB").arg(safeReduction, 0, 'f', 1));
    });
    connect(controller_, &WebAudioEffectsController::limiterReductionChanged, this,
            [this](double reductionDb, bool available) {
      if (!limiterReductionMeter_ || !limiterReductionValue_) return;
      const double safeReduction = available ? qBound(-20.0, reductionDb, 0.0) : 0.0;
      limiterReductionMeter_->setValue(qRound(std::abs(safeReduction) * 10.0));
      limiterReductionValue_->setText(QStringLiteral("%1 dB").arg(safeReduction, 0, 'f', 1));
    });
  }
  compressorMeterTimer_.setInterval(160);
  connect(&compressorMeterTimer_, &QTimer::timeout, this, [this] {
    if (controller_) controller_->requestCompressorGainReduction();
  });
  limiterMeterTimer_.setInterval(160);
  connect(&limiterMeterTimer_, &QTimer::timeout, this, [this] {
    if (controller_) controller_->requestLimiterReduction();
  });
  // Only poll while the active Auto Gain page is visible. This is a low-rate
  // runtime health check, not audio analysis: RMS samples and gain adaptation
  // remain entirely inside the browser graph.
  autoGainStatusTimer_.setInterval(1000);
  connect(&autoGainStatusTimer_, &QTimer::timeout, this, [this] {
    if (controller_) controller_->applyToAllWebViews();
  });
  navigation_->setCurrentRow(0);
  updateFromController();

  setStyleSheet(QStringLiteral(R"CSS(
    QWidget#audio-effects-page { background: #050708; color: #e9edf2; }
    QFrame#audio-effects-topbar { background: #0b1014; border-bottom: 1px solid #15333c; }
    QLabel#audio-effects-top-icon { color: #70e5ff; border: 1px solid #17637a; border-radius: 8px; background: #0e2028; font-size: 19px; }
    QLabel#audio-effects-runtime-status { color: #aebfca; font-size: 11px; }
    QListWidget#audio-effects-navigation { background: #0a0c0e; border: 0; border-right: 1px solid #20272c; min-width: 205px; max-width: 205px; padding: 8px; outline: 0; }
    QListWidget#audio-effects-navigation::item { color: #e3e8ed; min-height: 36px; margin: 2px 0; padding: 5px 8px; border: 1px solid #20272c; border-radius: 8px; font-weight: 600; }
    QListWidget#audio-effects-navigation::item:hover { background: #111c22; border-color: #287c93; }
    QListWidget#audio-effects-navigation::item:selected { background: #092530; border-color: #27c8ef; color: #edfbff; }
    QScrollArea#audio-effects-content-scroll { border: 0; background: #050708; }
    QWidget#audio-effects-reverb-content, QWidget#audio-effects-reverb-controls,
    QWidget#audio-effects-compressor-content, QWidget#audio-effects-compressor-controls,
    QWidget#audio-effects-limiter-content, QWidget#audio-effects-limiter-controls,
    QWidget#audio-effects-bass-enhancer-content, QWidget#audio-effects-bass-enhancer-controls,
    QWidget#audio-effects-auto-gain-content, QWidget#audio-effects-auto-gain-controls { background: #050708; }
    QFrame#audio-effects-card { background: #0e151a; border: 1px solid #1b5362; border-radius: 12px; }
    QLabel#audio-effects-title { font-size: 24px; font-weight: 700; color: #f3f5f7; }
    QLabel#audio-effects-subtitle, QLabel#audio-effects-detail { color: #9caab4; font-size: 12px; }
    QFrame#audio-effects-reverb-header { background: #050708; border-bottom: 1px solid #121d22; min-height: 106px; }
    QFrame#audio-effects-reverb-icon { background: #07181e; border: 1px solid #1385a6; border-radius: 18px; }
    QLabel#audio-effects-reverb-icon-glyph { color: #dffaff; font-size: 27px; font-weight: 700; }
    QLabel#audio-effects-reverb-title { font-size: 24px; font-weight: 700; color: #f4f7fa; }
    QLabel#audio-effects-reverb-subtitle { color: #aab7c1; font-size: 12px; }

    QDial#audio-effects-reverb-dial { background: #272727; border: 5px solid #333333; border-radius: 58px; }
    QLabel#audio-effects-reverb-value { color: #f3f6f8; font-size: 14px; font-weight: 700; }
    QLabel#audio-effects-reverb-label { color: #9ba7af; font-size: 11px; }
    QFrame#audio-effects-reverb-presets { background: #0b0e10; border: 1px solid #12181c; border-radius: 12px; }
    QLabel#audio-effects-reverb-presets-title { color: #d8e0e5; font-size: 13px; font-weight: 700; }
    QPushButton#audio-effects-reverb-preset { color: #d8e1ec; background: #202838; border: 1px solid #4b5970; border-radius: 8px; padding: 8px 13px; font-weight: 600; }
    QPushButton#audio-effects-reverb-preset:hover { background: #27354a; border-color: #6f9db4; }
    QPushButton#audio-effects-reverb-preset[active="true"] { color: #ecfbff; background: #0d5367; border-color: #32cbed; }
    QFrame#audio-effects-compressor-header { background: #050708; border-bottom: 1px solid #121d22; min-height: 106px; }
    QFrame#audio-effects-compressor-icon { background: #07181e; border: 1px solid #1385a6; border-radius: 18px; }
    QLabel#audio-effects-compressor-icon-glyph { color: #dffaff; font-size: 25px; font-weight: 700; }
    QLabel#audio-effects-compressor-title { font-size: 24px; font-weight: 700; color: #f4f7fa; }
    QLabel#audio-effects-compressor-subtitle { color: #aab7c1; font-size: 12px; }
    QFrame#audio-effects-compressor-presets, QFrame#audio-effects-compressor-meter-card { background: #0b0e10; border: 1px solid #12181c; border-radius: 12px; }
    QLabel#audio-effects-compressor-presets-title, QLabel#audio-effects-compressor-meter-title { color: #d8e0e5; font-size: 13px; font-weight: 700; }
    QPushButton#audio-effects-compressor-preset { color: #d8e1ec; background: #202838; border: 1px solid #4b5970; border-radius: 8px; padding: 8px 13px; font-weight: 600; }
    QPushButton#audio-effects-compressor-preset:hover { background: #27354a; border-color: #6f9db4; }
    QPushButton#audio-effects-compressor-preset[active="true"] { color: #ecfbff; background: #0d5367; border-color: #32cbed; }
    QDial#audio-effects-compressor-dial { background: #272727; border: 5px solid #333333; border-radius: 56px; }
    QLabel#audio-effects-compressor-value { color: #f3f6f8; font-size: 14px; font-weight: 700; }
    QLabel#audio-effects-compressor-label { color: #9ba7af; font-size: 11px; }
    QLabel#audio-effects-compressor-meter-value { color: #64dcff; font-size: 13px; font-weight: 700; }
    QProgressBar#audio-effects-compressor-meter { border: 1px solid #26343b; border-radius: 7px; background: #12181c; min-height: 14px; max-height: 14px; }
    QProgressBar#audio-effects-compressor-meter::chunk { border-radius: 6px; background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #1ec9ef, stop:0.55 #f2d544, stop:1 #e84c45); }
    QLabel#audio-effects-compressor-meter-marker { color: #89969f; font-size: 10px; }
    QFrame#audio-effects-limiter-header { background: #050708; border-bottom: 1px solid #121d22; min-height: 106px; }
    QFrame#audio-effects-limiter-icon { background: #07181e; border: 1px solid #1385a6; border-radius: 18px; }
    QLabel#audio-effects-limiter-icon-glyph { color: #dffaff; font-size: 25px; font-weight: 700; }
    QLabel#audio-effects-limiter-title { font-size: 24px; font-weight: 700; color: #f4f7fa; }
    QLabel#audio-effects-limiter-subtitle { color: #aab7c1; font-size: 12px; }
    QFrame#audio-effects-limiter-presets, QFrame#audio-effects-limiter-meter-card { background: #0b0e10; border: 1px solid #12181c; border-radius: 12px; }
    QLabel#audio-effects-limiter-presets-title, QLabel#audio-effects-limiter-meter-title { color: #d8e0e5; font-size: 13px; font-weight: 700; }
    QPushButton#audio-effects-limiter-preset { color: #d8e1ec; background: #202838; border: 1px solid #4b5970; border-radius: 8px; padding: 8px 13px; font-weight: 600; }
    QPushButton#audio-effects-limiter-preset:hover { background: #27354a; border-color: #6f9db4; }
    QPushButton#audio-effects-limiter-preset[active="true"] { color: #ecfbff; background: #0d5367; border-color: #32cbed; }
    QDial#audio-effects-limiter-dial { background: #272727; border: 5px solid #333333; border-radius: 56px; }
    QLabel#audio-effects-limiter-value { color: #f3f6f8; font-size: 14px; font-weight: 700; }
    QLabel#audio-effects-limiter-label { color: #9ba7af; font-size: 11px; }
    QLabel#audio-effects-limiter-meter-value { color: #64dcff; font-size: 13px; font-weight: 700; }
    QProgressBar#audio-effects-limiter-meter { border: 1px solid #26343b; border-radius: 7px; background: #12181c; min-height: 14px; max-height: 14px; }
    QProgressBar#audio-effects-limiter-meter::chunk { border-radius: 6px; background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #1ec9ef, stop:0.55 #f2d544, stop:1 #e84c45); }
    QLabel#audio-effects-limiter-meter-marker { color: #89969f; font-size: 10px; }
    QFrame#audio-effects-bass-enhancer-header { background: #050708; border-bottom: 1px solid #121d22; min-height: 106px; }
    QFrame#audio-effects-bass-enhancer-icon { background: #07181e; border: 1px solid #1385a6; border-radius: 18px; }
    QLabel#audio-effects-bass-enhancer-icon-glyph { color: #dffaff; font-size: 25px; font-weight: 700; }
    QLabel#audio-effects-bass-enhancer-title { font-size: 24px; font-weight: 700; color: #f4f7fa; }
    QLabel#audio-effects-bass-enhancer-subtitle { color: #aab7c1; font-size: 12px; }
    QDial#audio-effects-bass-enhancer-dial { background: #272727; border: 5px solid #333333; border-radius: 58px; }
    QLabel#audio-effects-bass-enhancer-value { color: #f3f6f8; font-size: 14px; font-weight: 700; }
    QLabel#audio-effects-bass-enhancer-label { color: #9ba7af; font-size: 11px; }
    QPushButton#audio-effects-bass-enhancer-deep { color: #d8e1ec; background: #111b20; border: 1px solid #245b69; border-radius: 8px; padding: 8px 16px; font-weight: 650; }
    QPushButton#audio-effects-bass-enhancer-deep:hover { background: #15303a; border-color: #39bada; }
    QPushButton#audio-effects-bass-enhancer-deep[active="true"] { color: #ecfbff; background: #0d5367; border-color: #32cbed; }
    QFrame#audio-effects-auto-gain-header { background: #050708; border-bottom: 1px solid #121d22; min-height: 106px; }
    QFrame#audio-effects-auto-gain-icon { background: #07181e; border: 1px solid #1385a6; border-radius: 18px; }
    QLabel#audio-effects-auto-gain-icon-glyph { color: #dffaff; font-size: 24px; font-weight: 700; }
    QLabel#audio-effects-auto-gain-title { font-size: 24px; font-weight: 700; color: #f4f7fa; }
    QLabel#audio-effects-auto-gain-subtitle { color: #aab7c1; font-size: 12px; }
    QDial#audio-effects-auto-gain-dial { background: #272727; border: 5px solid #333333; border-radius: 64px; }
    QLabel#audio-effects-auto-gain-value { color: #f3f6f8; font-size: 15px; font-weight: 700; }
    QLabel#audio-effects-auto-gain-label { color: #9ba7af; font-size: 11px; }
    QFrame#audio-effects-auto-gain-presets { background: #0b0e10; border: 1px solid #12181c; border-radius: 12px; }
    QLabel#audio-effects-auto-gain-presets-title { color: #d8e0e5; font-size: 13px; font-weight: 700; }
    QPushButton#audio-effects-auto-gain-preset { color: #d8e1ec; background: #202838; border: 1px solid #4b5970; border-radius: 8px; padding: 9px 16px; font-weight: 600; }
    QPushButton#audio-effects-auto-gain-preset:hover { background: #27354a; border-color: #6f9db4; }
    QPushButton#audio-effects-auto-gain-preset[active="true"] { color: #ecfbff; background: #0d5367; border-color: #32cbed; }
    QLabel#audio-effects-card-title { color: #dcfaff; font-size: 14px; font-weight: 700; }
    QPushButton#audio-effects-reset { color: #fff; background: #d9423b; border: 0; border-radius: 8px; padding: 8px 18px; font-weight: 700; }
    QPushButton#audio-effects-reset:hover { background: #ee544c; }
    QDial#audio-effects-preamp { background: #101619; border: 5px solid #253238; border-radius: 64px; }
    QLabel#audio-effects-preamp-value { color: #f4f8fb; font-size: 17px; font-weight: 700; }
    QFrame#audio-effects-eq-analyser { background: #030506; border: 1px solid #1d2529; border-radius: 7px; min-height: 94px; }
    QFrame#audio-effects-eq-band-panel { background: #0b0f12; border: 1px solid #283338; border-radius: 7px; }
    QLabel#audio-effects-eq-value { color: #c9d7df; font-size: 10px; }
    QLabel#audio-effects-eq-frequency { color: #9caab4; font-size: 10px; }
    QSlider#audio-effects-eq-slider::groove:vertical { background: #174d63; border-radius: 4px; width: 8px; }
    QSlider#audio-effects-eq-slider::sub-page:vertical { background: #174d63; border-radius: 4px; }
    QSlider#audio-effects-eq-slider::add-page:vertical { background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #1ba9d3, stop:1 #74e4ff); border-radius: 4px; }
    QSlider#audio-effects-eq-slider::handle:vertical { background: #0f2933; border: 2px solid #27d2ff; border-radius: 8px; height: 12px; width: 12px; margin: 0 -4px; }
    QPushButton#audio-effects-module-reset { color: #ecf3f6; background: #0b0f12; border: 1px solid #20272c; border-radius: 7px; padding: 8px 14px; font-weight: 700; }
    QPushButton#audio-effects-module-reset:hover { background: #111c22; border-color: #287c93; }
    QComboBox#audio-effects-acoustic-space { color: #e4edf1; background: #14191c; border: 1px solid #293439; border-radius: 5px; padding: 7px 10px; }
    QComboBox#audio-effects-acoustic-space::drop-down { border: 0; width: 24px; }
    QComboBox#audio-effects-acoustic-space QAbstractItemView { color: #e4edf1; background: #11171b; border: 1px solid #28566a; selection-background-color: #123540; }
    QSlider#audio-effects-balance::groove:horizontal { height: 8px; background: #202b31; border-radius: 4px; }
    QSlider#audio-effects-balance::handle:horizontal { width: 16px; margin: -5px 0; border-radius: 8px; background: #f4f7f8; border: 2px solid #18bfe8; }
    QPushButton#audio-effects-balance-quick { color: #dceff5; background: #10191d; border: 1px solid #1c6275; border-radius: 7px; padding: 7px 10px; font-weight: 700; }
    QPushButton#audio-effects-balance-quick:hover { background: #10303a; }
    QLabel#audio-effects-placeholder { color: #aebac4; font-size: 15px; }
  )CSS"));
  // Live language switch: static strings and bound composed strings are
  // refreshed by retranslateWidgetTree; controller-driven text is recomputed.
  connect(&dalinira::i18n::LanguageManager::instance(), &dalinira::i18n::LanguageManager::languageChanged,
          this, [this] {
            dalinira::i18n::retranslateWidgetTree(this);
            for (int row = 0; row < navigation_->count() && row < static_cast<int>(kModules.size()); ++row)
              navigation_->item(row)->setToolTip(displayName(kModules[row].name));
            updateFromController();
          });
}

void AudioEffectsPage::scheduleControllerSync() {
  if (!controllerSyncTimer_.isActive()) controllerSyncTimer_.start();
}

QString AudioEffectsPage::currentSubpanelId() const {
  if (!navigation_) return QStringLiteral("output");
  const int row = navigation_->currentRow();
  if (row >= 0 && row < static_cast<int>(kModules.size())) {
    return QString::fromLatin1(kModules[row].id);
  }
  return QStringLiteral("output");
}

void AudioEffectsPage::showEvent(QShowEvent *event) {
  QWidget::showEvent(event);
  if (controller_) {
    controller_->setPanelVisible(true, currentSubpanelId());
  }
  updateCompressorMeterPolling();
  updateLimiterMeterPolling();
  updateAutoGainStatusPolling();
}

void AudioEffectsPage::hideEvent(QHideEvent *event) {
  if (controller_) {
    controller_->setPanelVisible(false);
  }
  compressorMeterTimer_.stop();
  limiterMeterTimer_.stop();
  autoGainStatusTimer_.stop();
  QWidget::hideEvent(event);
}

void AudioEffectsPage::updateLimiterMeterPolling() {
  const bool shouldPoll = isVisible() && navigation_ && navigation_->currentRow() == 4
      && controller_ && controller_->enabled() && controller_->limiterEnabled();
  if (shouldPoll) {
    if (!limiterMeterTimer_.isActive()) limiterMeterTimer_.start();
    controller_->requestLimiterReduction();
    return;
  }
  limiterMeterTimer_.stop();
  if (limiterReductionMeter_) limiterReductionMeter_->setValue(0);
  if (limiterReductionValue_) limiterReductionValue_->setText(QStringLiteral("0.0 dB"));
}

void AudioEffectsPage::updateCompressorMeterPolling() {
  const bool shouldPoll = isVisible() && navigation_ && navigation_->currentRow() == 3
      && controller_ && controller_->enabled() && controller_->compressorEnabled();
  if (shouldPoll) {
    if (!compressorMeterTimer_.isActive()) compressorMeterTimer_.start();
    controller_->requestCompressorGainReduction();
    return;
  }
  compressorMeterTimer_.stop();
  if (compressorGainReductionMeter_) compressorGainReductionMeter_->setValue(0);
  if (compressorGainReductionValue_) compressorGainReductionValue_->setText(QStringLiteral("0.0 dB"));
}

void AudioEffectsPage::updateAutoGainStatusPolling() {
  const bool shouldPoll = isVisible() && navigation_ && navigation_->currentRow() == 6
      && controller_ && controller_->enabled() && controller_->autoGainEnabled();
  if (shouldPoll) {
    if (!autoGainStatusTimer_.isActive()) {
      autoGainStatusTimer_.start();
      controller_->applyToAllWebViews();
    }
    return;
  }
  autoGainStatusTimer_.stop();
}

void AudioEffectsPage::createEqualizerPage() {
  auto *scroll = new QScrollArea(pages_);
  scroll->setObjectName(QStringLiteral("audio-effects-content-scroll"));
  scroll->setWidgetResizable(true);
  auto *content = new QWidget(scroll);
  auto *layout = new QVBoxLayout(content);
  layout->setContentsMargins(28, 25, 28, 28);
  layout->setSpacing(18);

  auto *header = new QHBoxLayout;
  auto *heading = new QVBoxLayout;
  heading->addWidget(label(I18n::text(QStringLiteral("audio.eq32_heading")), content, QStringLiteral("audio-effects-title")));
  heading->addWidget(label(I18n::text(QStringLiteral("audio.eq32_desc")), content,
                           QStringLiteral("audio-effects-subtitle")));
  auto *reset = new QPushButton(I18n::text(QStringLiteral("common.reset")), content);
  reset->setObjectName(QStringLiteral("audio-effects-reset"));
  header->addLayout(heading, 1);
  auto *presetBrowser = new QPushButton(I18n::text(QStringLiteral("audio.presets")), content);
  presetBrowser->setObjectName(QStringLiteral("audio-effects-reset"));
  connect(presetBrowser, &QPushButton::clicked, this, &AudioEffectsPage::eqPresetBrowserRequested);
  header->addWidget(presetBrowser, 0, Qt::AlignTop);
  header->addWidget(reset, 0, Qt::AlignTop);
  layout->addLayout(header);

  auto *bandsCard = card(content);
  auto *bandsLayout = new QVBoxLayout(bandsCard);
  bandsLayout->setContentsMargins(18, 18, 18, 18);
  auto *analyser = new QFrame(bandsCard);
  analyser->setObjectName(QStringLiteral("audio-effects-eq-analyser"));
  analyser->setFixedHeight(96);
  bandsLayout->addWidget(analyser);
  auto *bandPanel = new QFrame(bandsCard);
  bandPanel->setObjectName(QStringLiteral("audio-effects-eq-band-panel"));
  auto *panelLayout = new QVBoxLayout(bandPanel);
  panelLayout->setContentsMargins(12, 12, 12, 7);
  panelLayout->setSpacing(3);
  auto *bandsScroll = new QScrollArea(bandPanel);
  bandsScroll->setObjectName(QStringLiteral("audio-effects-eq-scroll"));
  bandsScroll->setWidgetResizable(true);
  bandsScroll->setFrameShape(QFrame::NoFrame);
  bandsScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  bandsScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  auto *bandsWidget = new QWidget(bandsScroll);
  auto *bandsGrid = new QHBoxLayout(bandsWidget);
  bandsGrid->setContentsMargins(0, 0, 0, 0);
  bandsGrid->setSpacing(0);
  const QVector<int> frequencies = WebAudioEffectsController::equalizerFrequencies();
  bandsWidget->setMinimumWidth(frequencies.size() * 43);
  for (int index = 0; index < frequencies.size(); ++index) {
    auto *band = new QWidget(bandsWidget);
    band->setMinimumWidth(40);
    auto *bandLayout = new QVBoxLayout(band);
    bandLayout->setContentsMargins(2, 4, 2, 0);
    bandLayout->setSpacing(2);
    auto *value = label({}, band, QStringLiteral("audio-effects-eq-value"));
    value->setAlignment(Qt::AlignCenter);
    auto *slider = new QSlider(Qt::Vertical, band);
    slider->setObjectName(QStringLiteral("audio-effects-eq-slider"));
    // The UI must match the controller's safe ±12 dB range; a wider slider
    // would be clamped by the controller and visually jump back on the next update.
    slider->setRange(-120, 120);
    slider->setSingleStep(5);
    slider->setPageStep(10);
    slider->setTickPosition(QSlider::NoTicks);
    slider->setMinimumHeight(166);
    dalinira::i18n::bindAccessibleName(slider, QStringLiteral("audio.eq_band"), {QString::number(frequencies[index])});
    auto *frequency = new FrequencyLabel(frequencyLabel(frequencies[index]), band);
    bandLayout->addWidget(value);
    bandLayout->addWidget(slider, 1, Qt::AlignHCenter);
    bandLayout->addWidget(frequency);
    bandsGrid->addWidget(band, 1);
    equalizerSliders_.push_back(slider);
    equalizerValues_.push_back(value);
    connect(slider, &QSlider::valueChanged, this, [this, index](int value) {
      if (controller_) controller_->setEqualizerBand(index, static_cast<double>(value) / 10.0);
    });
  }
  bandsScroll->setWidget(bandsWidget);
  panelLayout->addWidget(bandsScroll);
  bandsLayout->addWidget(bandPanel);
  layout->addWidget(bandsCard);

  auto *moduleRow = new QHBoxLayout;
  auto *toneCard = card(content);
  auto *toneLayout = new QVBoxLayout(toneCard);
  toneLayout->setContentsMargins(20, 16, 20, 16);
  toneLayout->setSpacing(10);
  auto *moduleTitle = label(I18n::text(QStringLiteral("audio.dalinira_module")), toneCard, QStringLiteral("audio-effects-card-title"));
  moduleTitle->setAlignment(Qt::AlignCenter);
  toneLayout->addWidget(moduleTitle);
  auto *dialRow = new QHBoxLayout;
  const struct { const char *title; const char *hint; QDial **dial; QLabel **value; } tones[] = {
      {"audio.tone_bass", "audio.tone_bass_desc", &bassDial_, &bassValue_},
      {"audio.tone_mid", "Peaking • 1.2 kHz", &midDial_, &midValue_},
      {"audio.tone_treble", "High shelf • 10 kHz", &trebleDial_, &trebleValue_},
      {"Stereo Expander", "Stereo Expander", &stereoExpanderDial_, &stereoExpanderValue_}
  };
  for (int index = 0; index < 4; ++index) {
    auto *column = new QVBoxLayout;
    auto *title = label(displayName(tones[index].title), toneCard, QStringLiteral("audio-effects-card-title"));
    title->setAlignment(Qt::AlignCenter);
    auto *dial = new EffectDial(EffectDial::Style::Tone, toneCard);
    dial->setObjectName(QStringLiteral("audio-effects-preamp"));
    // Tone controls use the same ±12 dB domain as the Web Audio controller.
    // A wider UI range would be clamped by the controller and visibly snap back.
    dial->setRange(index == 3 ? 0 : -120, index == 3 ? 200 : 120);
    dial->setSingleStep(index == 3 ? 1 : 5);
    dial->setPageStep(10);
    dial->refreshColor();
    dial->setNotchesVisible(true);
    dial->setFixedSize(112, 112);
    dial->setAccessibleName(displayName(tones[index].title));
    auto *value = label({}, toneCard, QStringLiteral("audio-effects-preamp-value"));
    value->setAlignment(Qt::AlignCenter);
    auto *hint = label(displayName(tones[index].hint), toneCard, QStringLiteral("audio-effects-detail"));
    hint->setAlignment(Qt::AlignCenter);
    *tones[index].dial = dial;
    *tones[index].value = value;
    column->addWidget(title);
    column->addWidget(dial, 0, Qt::AlignHCenter);
    column->addWidget(value);
    column->addWidget(hint);
    dialRow->addLayout(column, 1);
  }
  toneLayout->addLayout(dialRow);
  auto *acousticRow = new QHBoxLayout;
  acousticRow->addWidget(label(I18n::text(QStringLiteral("audio.acoustic_space")), toneCard, QStringLiteral("audio-effects-detail")));
  acousticSpaceSelect_ = new QComboBox(toneCard);
  acousticSpaceSelect_->setObjectName(QStringLiteral("audio-effects-acoustic-space"));
  acousticSpaceSelect_->addItem(I18n::text(QStringLiteral("common.off")), QStringLiteral("off"));
  acousticSpaceSelect_->addItem(I18n::text(QStringLiteral("audio.space_small_room")), QStringLiteral("small"));
  acousticSpaceSelect_->addItem(I18n::text(QStringLiteral("audio.space_medium_room")), QStringLiteral("medium"));
  acousticSpaceSelect_->addItem(I18n::text(QStringLiteral("audio.space_large_room")), QStringLiteral("large"));
  acousticSpaceSelect_->addItem(I18n::text(QStringLiteral("audio.space_hall")), QStringLiteral("hall"));
  acousticRow->addWidget(acousticSpaceSelect_, 1);
  toneLayout->addLayout(acousticRow);
  auto *moduleReset = new QPushButton(I18n::text(QStringLiteral("audio.reset_module")), toneCard);
  moduleReset->setObjectName(QStringLiteral("audio-effects-module-reset"));
  toneLayout->addWidget(moduleReset);
  moduleRow->addWidget(toneCard, 1);

  auto *balanceCard = card(content);
  balanceCard->setMinimumWidth(240);
  auto *balanceLayout = new QVBoxLayout(balanceCard);
  balanceLayout->setContentsMargins(18, 16, 18, 16);
  auto *balanceTitle = label(I18n::text(QStringLiteral("audio.balance_title")), balanceCard, QStringLiteral("audio-effects-card-title"));
  balanceTitle->setAlignment(Qt::AlignCenter);
  balanceValue_ = label({}, balanceCard, QStringLiteral("audio-effects-card-title"));
  balanceValue_->setAlignment(Qt::AlignCenter);
  balanceSlider_ = new QSlider(Qt::Horizontal, balanceCard);
  balanceSlider_->setObjectName(QStringLiteral("audio-effects-balance"));
  balanceSlider_->setRange(-100, 100);
  balanceSlider_->setSingleStep(1);
  balanceSlider_->setPageStep(10);
  balanceSlider_->setAccessibleName(I18n::text(QStringLiteral("audio.balance_accessible")));
  auto *quick = new QHBoxLayout;
  for (const int value : {-10, 0, 10}) {
    auto *button = new QPushButton(value > 0 ? QStringLiteral("+%1").arg(value) : QString::number(value), balanceCard);
    button->setObjectName(QStringLiteral("audio-effects-balance-quick"));
    quick->addWidget(button);
    connect(button, &QPushButton::clicked, this, [this, value] { if (controller_) controller_->setBalance(value); });
  }
  balanceLayout->addWidget(balanceTitle);
  balanceLayout->addWidget(balanceValue_);
  balanceLayout->addWidget(balanceSlider_);
  auto *balanceLabels = new QHBoxLayout;
  balanceLabels->addWidget(label(I18n::text(QStringLiteral("audio.left")), balanceCard, QStringLiteral("audio-effects-detail")));
  auto *centre = label(I18n::text(QStringLiteral("audio.center")), balanceCard, QStringLiteral("audio-effects-detail"));
  centre->setAlignment(Qt::AlignCenter);
  balanceLabels->addWidget(centre, 1);
  auto *right = label(I18n::text(QStringLiteral("audio.right")), balanceCard, QStringLiteral("audio-effects-detail"));
  right->setAlignment(Qt::AlignRight);
  balanceLabels->addWidget(right);
  balanceLayout->addLayout(balanceLabels);
  balanceLayout->addLayout(quick);
  balanceLayout->addStretch();
  moduleRow->addWidget(balanceCard);
  layout->addLayout(moduleRow);
  layout->addStretch();
  scroll->setWidget(content);
  pages_->addWidget(scroll);

  connect(reset, &QPushButton::clicked, controller_, &WebAudioEffectsController::resetEqualizer);
  connect(bassDial_, &QDial::valueChanged, this, [this](int value) { if (controller_) controller_->setBassDb(value / 10.0); });
  connect(midDial_, &QDial::valueChanged, this, [this](int value) { if (controller_) controller_->setMidDb(value / 10.0); });
  connect(trebleDial_, &QDial::valueChanged, this, [this](int value) { if (controller_) controller_->setTrebleDb(value / 10.0); });
  connect(stereoExpanderDial_, &QDial::valueChanged, this, [this](int value) { if (controller_) controller_->setStereoExpanderPercent(value); });
  connect(balanceSlider_, &QSlider::valueChanged, this, [this](int value) { if (controller_) controller_->setBalance(value); });
  connect(acousticSpaceSelect_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int index) {
    if (controller_ && index >= 0) controller_->setAcousticSpace(acousticSpaceSelect_->itemData(index).toString());
  });
  connect(moduleReset, &QPushButton::clicked, controller_, &WebAudioEffectsController::resetEqualizerModule);
}

void AudioEffectsPage::createOutputPage() {
  auto *scroll = new QScrollArea(pages_);
  scroll->setObjectName(QStringLiteral("audio-effects-content-scroll"));
  scroll->setWidgetResizable(true);
  auto *content = new QWidget(scroll);
  auto *layout = new QVBoxLayout(content);
  layout->setContentsMargins(28, 25, 28, 28);
  layout->setSpacing(18);

  auto *header = new QHBoxLayout;
  auto *heading = new QVBoxLayout;
  auto *title = label(I18n::text(QStringLiteral("audio.output_heading")), content, QStringLiteral("audio-effects-title"));
  auto *subtitle = label(I18n::text(QStringLiteral("audio.output_desc")), content, QStringLiteral("audio-effects-subtitle"));
  heading->addWidget(title);
  heading->addWidget(subtitle);
  auto *reset = new QPushButton(I18n::text(QStringLiteral("common.reset")), content);
  reset->setObjectName(QStringLiteral("audio-effects-reset"));
  header->addLayout(heading, 1);
  header->addWidget(reset, 0, Qt::AlignTop);
  layout->addLayout(header);

  auto *runtimeCard = card(content);
  auto *runtimeLayout = new QVBoxLayout(runtimeCard);
  runtimeLayout->setContentsMargins(16, 14, 16, 14);
  runtimeLayout->addWidget(label(I18n::text(QStringLiteral("audio.output_path")), runtimeCard, QStringLiteral("audio-effects-card-title")));
  auto *runtimeDetail = label({}, runtimeCard, QStringLiteral("audio-effects-detail"));
  runtimeDetail->setObjectName(QStringLiteral("audio-effects-output-detail"));
  runtimeLayout->addWidget(runtimeDetail);
  layout->addWidget(runtimeCard);

  auto *gainCard = card(content);
  auto *gainLayout = new QVBoxLayout(gainCard);
  gainLayout->setContentsMargins(20, 18, 20, 18);
  auto *gainTitle = label(I18n::text(QStringLiteral("audio.master_gain")), gainCard, QStringLiteral("audio-effects-card-title"));
  gainTitle->setAlignment(Qt::AlignCenter);
  preampDial_ = new QDial(gainCard);
  preampDial_->setObjectName(QStringLiteral("audio-effects-preamp"));
  preampDial_->setRange(-240, 240);
  preampDial_->setSingleStep(1);
  preampDial_->setPageStep(10);
  preampDial_->setNotchesVisible(true);
  preampDial_->setFixedSize(132, 132);
  preampValue_ = label({}, gainCard, QStringLiteral("audio-effects-preamp-value"));
  preampValue_->setAlignment(Qt::AlignCenter);
  auto *gainHint = label(I18n::text(QStringLiteral("audio.master_gain_desc")), gainCard, QStringLiteral("audio-effects-detail"));
  gainHint->setAlignment(Qt::AlignCenter);
  gainLayout->addWidget(gainTitle);
  gainLayout->addWidget(preampDial_, 0, Qt::AlignHCenter);
  gainLayout->addWidget(preampValue_);
  gainLayout->addWidget(gainHint);
  layout->addWidget(gainCard);
  layout->addStretch();
  scroll->setWidget(content);
  pages_->addWidget(scroll);

  connect(reset, &QPushButton::clicked, controller_, &WebAudioEffectsController::resetOutput);
  connect(preampDial_, &QDial::valueChanged, this, [this](int value) {
    if (controller_) controller_->setPreampDb(static_cast<double>(value) / 10.0);
  });
}

void AudioEffectsPage::createReverbPage() {
  auto *scroll = new QScrollArea(pages_);
  scroll->setObjectName(QStringLiteral("audio-effects-content-scroll"));
  scroll->setWidgetResizable(true);
  auto *content = new QWidget(scroll);
  content->setObjectName(QStringLiteral("audio-effects-reverb-content"));
  auto *layout = new QVBoxLayout(content);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  auto *header = new QFrame(content);
  header->setObjectName(QStringLiteral("audio-effects-reverb-header"));
  auto *headerLayout = new QHBoxLayout(header);
  headerLayout->setContentsMargins(20, 15, 20, 15);
  headerLayout->setSpacing(10);
  auto *icon = new QFrame(header);
  icon->setObjectName(QStringLiteral("audio-effects-reverb-icon"));
  icon->setFixedSize(54, 54);
  auto *iconLayout = new QVBoxLayout(icon);
  iconLayout->setContentsMargins(0, 0, 0, 0);
  auto *glyph = label(QStringLiteral("↝"), icon, QStringLiteral("audio-effects-reverb-icon-glyph"));
  glyph->setAlignment(Qt::AlignCenter);
  iconLayout->addWidget(glyph);
  auto *heading = new QVBoxLayout;
  heading->setSpacing(4);
  heading->addWidget(label(I18n::text(QStringLiteral("audio.reverb_heading")), header, QStringLiteral("audio-effects-reverb-title")));
  heading->addWidget(label(I18n::text(QStringLiteral("audio.reverb_desc")), header,
                           QStringLiteral("audio-effects-reverb-subtitle")));
  reverbToggle_ = new GlowToggleSwitch(I18n::text(QStringLiteral("common.enable")), header);
  reverbToggle_->setObjectName(QStringLiteral("audio-effects-reverb-toggle"));
  reverbToggle_->setAccessibleName(I18n::text(QStringLiteral("audio.reverb_enable")));
  moduleToggles_.insert(QStringLiteral("reverb"), reverbToggle_);
  headerLayout->addWidget(icon);
  headerLayout->addLayout(heading, 1);
  headerLayout->addWidget(reverbToggle_, 0, Qt::AlignTop | Qt::AlignRight);
  layout->addWidget(header);

  auto *controls = new QWidget(content);
  controls->setObjectName(QStringLiteral("audio-effects-reverb-controls"));
  auto *controlsLayout = new QVBoxLayout(controls);
  controlsLayout->setContentsMargins(20, 28, 20, 24);
  controlsLayout->setSpacing(0);
  auto *knobRow = new QHBoxLayout;
  knobRow->setContentsMargins(10, 0, 10, 0);
  knobRow->setSpacing(18);
  const auto addDial = [controls, knobRow](const QString &caption, int minimum, int maximum, int step,
                                                  QDial **dialTarget, QLabel **valueTarget) {
    auto *column = new QVBoxLayout;
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(5);
    auto *dial = new EffectDial(EffectDial::Style::Reverb, controls);
    dial->setObjectName(QStringLiteral("audio-effects-reverb-dial"));
    dial->setRange(minimum, maximum);
    dial->setSingleStep(step);
    dial->setPageStep(std::max(step, (maximum - minimum) / 24));
    dial->setFixedSize(116, 116);
    dial->setAccessibleName(caption);
    dial->refreshColor();
    auto *value = label({}, controls, QStringLiteral("audio-effects-reverb-value"));
    value->setAlignment(Qt::AlignCenter);
    auto *title = label(caption, controls, QStringLiteral("audio-effects-reverb-label"));
    title->setAlignment(Qt::AlignCenter);
    column->addWidget(dial, 0, Qt::AlignHCenter);
    column->addWidget(value);
    column->addWidget(title);
    knobRow->addLayout(column, 1);
    *dialTarget = dial;
    *valueTarget = value;
  };
  addDial(I18n::text(QStringLiteral("audio.param_room_size")), 0, 30000, 10, &reverbRoomSizeDial_, &reverbRoomSizeValue_);
  addDial(I18n::text(QStringLiteral("audio.param_damping")), 0, 1000, 1, &reverbDampingDial_, &reverbDampingValue_);
  addDial(I18n::text(QStringLiteral("audio.param_wet_dry")), -960, 0, 1, &reverbWetDryDial_, &reverbWetDryValue_);
  addDial(I18n::text(QStringLiteral("audio.param_hf_ratio")), 1, 999, 1, &reverbHfRatioDial_, &reverbHfRatioValue_);
  addDial(I18n::text(QStringLiteral("audio.param_input_gain")), -960, 120, 1, &reverbInputGainDial_, &reverbInputGainValue_);
  controlsLayout->addLayout(knobRow);
  controlsLayout->addSpacing(24);

  auto *presets = new QFrame(controls);
  presets->setObjectName(QStringLiteral("audio-effects-reverb-presets"));
  auto *presetsLayout = new QVBoxLayout(presets);
  presetsLayout->setContentsMargins(17, 14, 17, 16);
  presetsLayout->setSpacing(12);
  presetsLayout->addWidget(label(I18n::text(QStringLiteral("audio.presets_heading")), presets,
                                 QStringLiteral("audio-effects-reverb-presets-title")));
  auto *presetButtons = new QHBoxLayout;
  presetButtons->setContentsMargins(0, 0, 0, 0);
  presetButtons->setSpacing(8);
  const struct { const char *id; const char *title; const char *symbol; } presetDefinitions[] = {
      {"smallRoom", "audio.space_small_room", "⌂"}, {"largeRoom", "audio.space_large_room", "▤"}, {"concertHall", "audio.preset_concert_hall", "▥"},
      {"cathedral", "audio.preset_cathedral", "♜"}, {"studioPlate", "Studio Plate", "▣"}, {"arena", "audio.preset_arena", "▥"},
      {"vocalRoom", "Vocal Room", "♩"}, {"ambientWash", "Ambient Wash", "⌁"}, {"slapback", "Slapback", "✦"},
      {"dreamVox", "Dream Vox", "✧"},
  };
  for (const auto &preset : presetDefinitions) {
    const QString id = QString::fromLatin1(preset.id);
    auto *button = new QPushButton(presets);
    dalinira::i18n::bindText(button, QStringLiteral("=%1  %2"), {QString::fromUtf8(preset.symbol), nameArg(preset.title)});
    button->setObjectName(QStringLiteral("audio-effects-reverb-preset"));
    dalinira::i18n::bindAccessibleName(button, QStringLiteral("audio.reverb_preset"), {nameArg(preset.title)});
    button->setProperty("presetId", id);
    button->setProperty("active", false);
    presetButtons->addWidget(button);
    connect(button, &QPushButton::clicked, this, [this, id] {
      if (controller_) controller_->applyReverbPreset(id);
    });
  }
  presetsLayout->addLayout(presetButtons);
  controlsLayout->addWidget(presets);
  auto *reset = new QPushButton(I18n::text(QStringLiteral("common.reset")), controls);
  reset->setObjectName(QStringLiteral("audio-effects-reset"));
  reset->setAccessibleName(I18n::text(QStringLiteral("audio.reverb_reset")));
  controlsLayout->addWidget(reset, 0, Qt::AlignLeft);
  controlsLayout->addStretch();
  layout->addWidget(controls, 1);
  scroll->setWidget(content);
  pages_->addWidget(scroll);

  connect(reverbToggle_, &QCheckBox::toggled, this, [this](bool checked) {
    if (controller_) controller_->setReverbEnabled(checked);
  });
  connect(reverbRoomSizeDial_, &QDial::valueChanged, this, [this](int value) {
    if (controller_) controller_->setReverbRoomSizeMs(value / 10.0);
  });
  connect(reverbDampingDial_, &QDial::valueChanged, this, [this](int value) {
    if (controller_) controller_->setReverbDamping(value / 1000.0);
  });
  connect(reverbWetDryDial_, &QDial::valueChanged, this, [this](int value) {
    if (controller_) controller_->setReverbWetDryDb(value / 10.0);
  });
  connect(reverbHfRatioDial_, &QDial::valueChanged, this, [this](int value) {
    if (controller_) controller_->setReverbHfRatio(value / 1000.0);
  });
  connect(reverbInputGainDial_, &QDial::valueChanged, this, [this](int value) {
    if (controller_) controller_->setReverbInputGainDb(value / 10.0);
  });
  connect(reset, &QPushButton::clicked, this, [this] {
    if (controller_) controller_->resetReverb();
  });
}

void AudioEffectsPage::createCompressorPage() {
  auto *scroll = new QScrollArea(pages_);
  scroll->setObjectName(QStringLiteral("audio-effects-content-scroll"));
  scroll->setWidgetResizable(true);
  auto *content = new QWidget(scroll);
  content->setObjectName(QStringLiteral("audio-effects-compressor-content"));
  auto *layout = new QVBoxLayout(content);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  auto *header = new QFrame(content);
  header->setObjectName(QStringLiteral("audio-effects-compressor-header"));
  auto *headerLayout = new QHBoxLayout(header);
  headerLayout->setContentsMargins(20, 15, 20, 15);
  headerLayout->setSpacing(10);
  auto *icon = new QFrame(header);
  icon->setObjectName(QStringLiteral("audio-effects-compressor-icon"));
  icon->setFixedSize(54, 54);
  auto *iconLayout = new QVBoxLayout(icon);
  iconLayout->setContentsMargins(0, 0, 0, 0);
  auto *glyph = label(QStringLiteral("⌁"), icon, QStringLiteral("audio-effects-compressor-icon-glyph"));
  glyph->setAlignment(Qt::AlignCenter);
  iconLayout->addWidget(glyph);
  auto *heading = new QVBoxLayout;
  heading->setSpacing(4);
  heading->addWidget(label(I18n::text(QStringLiteral("audio.module_compressor")), header, QStringLiteral("audio-effects-compressor-title")));
  heading->addWidget(label(I18n::text(QStringLiteral("audio.compressor_desc")), header,
                           QStringLiteral("audio-effects-compressor-subtitle")));
  compressorToggle_ = new GlowToggleSwitch(I18n::text(QStringLiteral("common.enable")), header);
  compressorToggle_->setObjectName(QStringLiteral("audio-effects-compressor-toggle"));
  compressorToggle_->setAccessibleName(I18n::text(QStringLiteral("audio.compressor_enable")));
  moduleToggles_.insert(QStringLiteral("compressor"), compressorToggle_);
  headerLayout->addWidget(icon);
  headerLayout->addLayout(heading, 1);
  headerLayout->addWidget(compressorToggle_, 0, Qt::AlignTop | Qt::AlignRight);
  layout->addWidget(header);

  auto *controls = new QWidget(content);
  controls->setObjectName(QStringLiteral("audio-effects-compressor-controls"));
  auto *controlsLayout = new QVBoxLayout(controls);
  controlsLayout->setContentsMargins(20, 20, 20, 24);
  controlsLayout->setSpacing(18);

  auto *presets = new QFrame(controls);
  presets->setObjectName(QStringLiteral("audio-effects-compressor-presets"));
  auto *presetsLayout = new QVBoxLayout(presets);
  presetsLayout->setContentsMargins(17, 13, 17, 15);
  presetsLayout->setSpacing(10);
  presetsLayout->addWidget(label(I18n::text(QStringLiteral("audio.presets_heading")), presets,
                                 QStringLiteral("audio-effects-compressor-presets-title")));
  auto *presetButtons = new QHBoxLayout;
  presetButtons->setContentsMargins(0, 0, 0, 0);
  presetButtons->setSpacing(8);
  const struct { const char *id; const char *title; const char *symbol; } presetDefinitions[] = {
      {"gentle", "audio.preset_gentle", "⌁"}, {"vocal", "audio.preset_vocal", "♪"}, {"night", "audio.preset_night", "☾"},
      {"punch", "audio.preset_punch", "✦"}, {"broadcast", "audio.preset_broadcast", "◉"},
  };
  for (const auto &preset : presetDefinitions) {
    const QString id = QString::fromLatin1(preset.id);
    auto *button = new QPushButton(presets);
    dalinira::i18n::bindText(button, QStringLiteral("=%1  %2"), {QString::fromUtf8(preset.symbol), nameArg(preset.title)});
    button->setObjectName(QStringLiteral("audio-effects-compressor-preset"));
    dalinira::i18n::bindAccessibleName(button, QStringLiteral("audio.compressor_preset"), {nameArg(preset.title)});
    button->setProperty("presetId", id);
    button->setProperty("active", false);
    presetButtons->addWidget(button);
    connect(button, &QPushButton::clicked, this, [this, id] {
      if (controller_) controller_->applyCompressorPreset(id);
    });
  }
  presetsLayout->addLayout(presetButtons);
  controlsLayout->addWidget(presets);

  auto *knobRow = new QHBoxLayout;
  knobRow->setContentsMargins(10, 0, 10, 0);
  knobRow->setSpacing(12);
  const auto addDial = [controls, knobRow](const QString &caption, int minimum, int maximum, int step,
                                            QDial **dialTarget, QLabel **valueTarget) {
    auto *column = new QVBoxLayout;
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(5);
    auto *dial = new EffectDial(EffectDial::Style::Reverb, controls);
    dial->setObjectName(QStringLiteral("audio-effects-compressor-dial"));
    dial->setRange(minimum, maximum);
    dial->setSingleStep(step);
    dial->setPageStep(std::max(step, (maximum - minimum) / 24));
    dial->setFixedSize(112, 112);
    dial->setAccessibleName(caption);
    dial->refreshColor();
    auto *value = label({}, controls, QStringLiteral("audio-effects-compressor-value"));
    value->setAlignment(Qt::AlignCenter);
    auto *title = label(caption, controls, QStringLiteral("audio-effects-compressor-label"));
    title->setAlignment(Qt::AlignCenter);
    column->addWidget(dial, 0, Qt::AlignHCenter);
    column->addWidget(value);
    column->addWidget(title);
    knobRow->addLayout(column, 1);
    *dialTarget = dial;
    *valueTarget = value;
  };
  addDial(I18n::text(QStringLiteral("audio.param_threshold")), -600, 0, 10, &compressorThresholdDial_, &compressorThresholdValue_);
  addDial(I18n::text(QStringLiteral("audio.param_ratio")), 10, 200, 10, &compressorRatioDial_, &compressorRatioValue_);
  addDial(I18n::text(QStringLiteral("audio.param_attack")), 1, 1000, 10, &compressorAttackDial_, &compressorAttackValue_);
  addDial(I18n::text(QStringLiteral("audio.param_release")), 100, 10000, 10, &compressorReleaseDial_, &compressorReleaseValue_);
  addDial(I18n::text(QStringLiteral("audio.param_makeup_gain")), -120, 240, 10, &compressorMakeupDial_, &compressorMakeupValue_);
  addDial(I18n::text(QStringLiteral("audio.param_knee")), 0, 100, 10, &compressorKneeDial_, &compressorKneeValue_);
  controlsLayout->addLayout(knobRow);

  auto *meterCard = new QFrame(controls);
  meterCard->setObjectName(QStringLiteral("audio-effects-compressor-meter-card"));
  auto *meterLayout = new QVBoxLayout(meterCard);
  meterLayout->setContentsMargins(17, 12, 17, 13);
  meterLayout->setSpacing(7);
  auto *meterHeading = new QHBoxLayout;
  meterHeading->addWidget(label(I18n::text(QStringLiteral("audio.gain_reduction")), meterCard,
                                QStringLiteral("audio-effects-compressor-meter-title")));
  meterHeading->addStretch();
  compressorGainReductionValue_ = label(QStringLiteral("0.0 dB"), meterCard,
                                         QStringLiteral("audio-effects-compressor-meter-value"));
  meterHeading->addWidget(compressorGainReductionValue_);
  meterLayout->addLayout(meterHeading);
  compressorGainReductionMeter_ = new QProgressBar(meterCard);
  compressorGainReductionMeter_->setObjectName(QStringLiteral("audio-effects-compressor-meter"));
  compressorGainReductionMeter_->setAccessibleName(I18n::text(QStringLiteral("audio.compressor_gain_reduction")));
  compressorGainReductionMeter_->setRange(0, 240);
  compressorGainReductionMeter_->setValue(0);
  compressorGainReductionMeter_->setTextVisible(false);
  meterLayout->addWidget(compressorGainReductionMeter_);
  auto *markers = new QHBoxLayout;
  markers->setContentsMargins(0, 0, 0, 0);
  const QStringList markerTexts{QStringLiteral("0 dB"), QStringLiteral("-6 dB"), QStringLiteral("-12 dB"),
                                QStringLiteral("-18 dB"), QStringLiteral("-24 dB")};
  for (const QString &text : markerTexts) {
    auto *marker = label(text, meterCard, QStringLiteral("audio-effects-compressor-meter-marker"));
    if (text == markerTexts.front()) marker->setAlignment(Qt::AlignLeft);
    else if (text == markerTexts.back()) marker->setAlignment(Qt::AlignRight);
    else marker->setAlignment(Qt::AlignCenter);
    markers->addWidget(marker, 1);
  }
  meterLayout->addLayout(markers);
  controlsLayout->addWidget(meterCard);

  auto *reset = new QPushButton(I18n::text(QStringLiteral("common.reset")), controls);
  reset->setObjectName(QStringLiteral("audio-effects-reset"));
  reset->setAccessibleName(I18n::text(QStringLiteral("audio.compressor_reset")));
  controlsLayout->addWidget(reset, 0, Qt::AlignLeft);
  controlsLayout->addStretch();
  layout->addWidget(controls, 1);
  scroll->setWidget(content);
  pages_->addWidget(scroll);

  connect(compressorToggle_, &QCheckBox::toggled, this, [this](bool checked) {
    if (controller_) controller_->setCompressorEnabled(checked);
    updateCompressorMeterPolling();
  });
  connect(compressorThresholdDial_, &QDial::valueChanged, this, [this](int value) {
    if (controller_) controller_->setCompressorThresholdDb(value / 10.0);
  });
  connect(compressorRatioDial_, &QDial::valueChanged, this, [this](int value) {
    if (controller_) controller_->setCompressorRatio(value / 10.0);
  });
  connect(compressorAttackDial_, &QDial::valueChanged, this, [this](int value) {
    if (controller_) controller_->setCompressorAttackMs(value / 10.0);
  });
  connect(compressorReleaseDial_, &QDial::valueChanged, this, [this](int value) {
    if (controller_) controller_->setCompressorReleaseMs(value / 10.0);
  });
  connect(compressorMakeupDial_, &QDial::valueChanged, this, [this](int value) {
    if (controller_) controller_->setCompressorMakeupDb(value / 10.0);
  });
  connect(compressorKneeDial_, &QDial::valueChanged, this, [this](int value) {
    if (controller_) controller_->setCompressorKneeDb(value / 10.0);
  });
  connect(reset, &QPushButton::clicked, this, [this] {
    if (controller_) controller_->resetCompressor();
  });
}

void AudioEffectsPage::createLimiterPage() {
  auto *scroll = new QScrollArea(pages_);
  scroll->setObjectName(QStringLiteral("audio-effects-content-scroll"));
  scroll->setWidgetResizable(true);
  auto *content = new QWidget(scroll);
  content->setObjectName(QStringLiteral("audio-effects-limiter-content"));
  auto *layout = new QVBoxLayout(content);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  auto *header = new QFrame(content);
  header->setObjectName(QStringLiteral("audio-effects-limiter-header"));
  auto *headerLayout = new QHBoxLayout(header);
  headerLayout->setContentsMargins(20, 15, 20, 15);
  headerLayout->setSpacing(10);
  auto *icon = new QFrame(header);
  icon->setObjectName(QStringLiteral("audio-effects-limiter-icon"));
  icon->setFixedSize(54, 54);
  auto *iconLayout = new QVBoxLayout(icon);
  iconLayout->setContentsMargins(0, 0, 0, 0);
  auto *glyph = label(QStringLiteral("┫"), icon, QStringLiteral("audio-effects-limiter-icon-glyph"));
  glyph->setAlignment(Qt::AlignCenter);
  iconLayout->addWidget(glyph);
  auto *heading = new QVBoxLayout;
  heading->setSpacing(4);
  heading->addWidget(label(I18n::text(QStringLiteral("audio.limiter")), header, QStringLiteral("audio-effects-limiter-title")));
  heading->addWidget(label(I18n::text(QStringLiteral("audio.limiter_desc")), header,
                           QStringLiteral("audio-effects-limiter-subtitle")));
  limiterToggle_ = new GlowToggleSwitch(I18n::text(QStringLiteral("common.enable")), header);
  limiterToggle_->setObjectName(QStringLiteral("audio-effects-limiter-toggle"));
  limiterToggle_->setAccessibleName(I18n::text(QStringLiteral("audio.limiter_enable")));
  moduleToggles_.insert(QStringLiteral("limiter"), limiterToggle_);
  headerLayout->addWidget(icon);
  headerLayout->addLayout(heading, 1);
  headerLayout->addWidget(limiterToggle_, 0, Qt::AlignTop | Qt::AlignRight);
  layout->addWidget(header);

  auto *controls = new QWidget(content);
  controls->setObjectName(QStringLiteral("audio-effects-limiter-controls"));
  auto *controlsLayout = new QVBoxLayout(controls);
  controlsLayout->setContentsMargins(20, 20, 20, 24);
  controlsLayout->setSpacing(18);

  auto *presets = new QFrame(controls);
  presets->setObjectName(QStringLiteral("audio-effects-limiter-presets"));
  auto *presetsLayout = new QVBoxLayout(presets);
  presetsLayout->setContentsMargins(17, 13, 17, 15);
  presetsLayout->setSpacing(10);
  presetsLayout->addWidget(label(I18n::text(QStringLiteral("audio.presets_heading")), presets,
                                 QStringLiteral("audio-effects-limiter-presets-title")));
  auto *presetButtons = new QHBoxLayout;
  presetButtons->setContentsMargins(0, 0, 0, 0);
  presetButtons->setSpacing(8);
  const struct { const char *id; const char *title; const char *symbol; } definitions[] = {
      {"transparent", "audio.preset_transparent", "◇"}, {"loud", "audio.preset_loud", "✦"}, {"streaming", "audio.preset_streaming", "◉"},
      {"night", "audio.preset_night", "☾"}, {"safe", "audio.preset_safe", "◎"},
  };
  for (const auto &preset : definitions) {
    const QString id = QString::fromLatin1(preset.id);
    auto *button = new QPushButton(presets);
    dalinira::i18n::bindText(button, QStringLiteral("=%1  %2"), {QString::fromUtf8(preset.symbol), nameArg(preset.title)});
    button->setObjectName(QStringLiteral("audio-effects-limiter-preset"));
    dalinira::i18n::bindAccessibleName(button, QStringLiteral("audio.limiter_preset"), {nameArg(preset.title)});
    button->setProperty("presetId", id);
    button->setProperty("active", false);
    presetButtons->addWidget(button);
    connect(button, &QPushButton::clicked, this, [this, id] {
      if (controller_) controller_->applyLimiterPreset(id);
    });
  }
  presetsLayout->addLayout(presetButtons);
  controlsLayout->addWidget(presets);

  auto *knobRow = new QHBoxLayout;
  knobRow->setContentsMargins(45, 0, 45, 0);
  knobRow->setSpacing(30);
  const auto addDial = [controls, knobRow](const QString &caption, int minimum, int maximum, int step,
                                            QDial **dialTarget, QLabel **valueTarget) {
    auto *column = new QVBoxLayout;
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(5);
    auto *dial = new EffectDial(EffectDial::Style::Reverb, controls);
    dial->setObjectName(QStringLiteral("audio-effects-limiter-dial"));
    dial->setRange(minimum, maximum);
    dial->setSingleStep(step);
    dial->setPageStep(std::max(step, (maximum - minimum) / 24));
    dial->setFixedSize(112, 112);
    dial->setAccessibleName(caption);
    dial->refreshColor();
    auto *value = label({}, controls, QStringLiteral("audio-effects-limiter-value"));
    value->setAlignment(Qt::AlignCenter);
    auto *title = label(caption, controls, QStringLiteral("audio-effects-limiter-label"));
    title->setAlignment(Qt::AlignCenter);
    column->addWidget(dial, 0, Qt::AlignHCenter);
    column->addWidget(value);
    column->addWidget(title);
    knobRow->addLayout(column, 1);
    *dialTarget = dial;
    *valueTarget = value;
  };
  addDial(I18n::text(QStringLiteral("audio.param_ceiling")), -120, 0, 1, &limiterCeilingDial_, &limiterCeilingValue_);
  addDial(I18n::text(QStringLiteral("audio.param_release")), 10, 500, 1, &limiterReleaseDial_, &limiterReleaseValue_);
  addDial(I18n::text(QStringLiteral("audio.param_lookahead")), 0, 20, 1, &limiterLookaheadDial_, &limiterLookaheadValue_);
  addDial(I18n::text(QStringLiteral("audio.param_gain")), -12, 12, 1, &limiterGainDial_, &limiterGainValue_);
  controlsLayout->addLayout(knobRow);

  auto *meterCard = new QFrame(controls);
  meterCard->setObjectName(QStringLiteral("audio-effects-limiter-meter-card"));
  auto *meterLayout = new QVBoxLayout(meterCard);
  meterLayout->setContentsMargins(17, 12, 17, 13);
  meterLayout->setSpacing(7);
  auto *meterHeading = new QHBoxLayout;
  meterHeading->addWidget(label(I18n::text(QStringLiteral("audio.limiting_amount")), meterCard,
                                QStringLiteral("audio-effects-limiter-meter-title")));
  meterHeading->addStretch();
  limiterReductionValue_ = label(QStringLiteral("0.0 dB"), meterCard,
                                  QStringLiteral("audio-effects-limiter-meter-value"));
  meterHeading->addWidget(limiterReductionValue_);
  meterLayout->addLayout(meterHeading);
  limiterReductionMeter_ = new QProgressBar(meterCard);
  limiterReductionMeter_->setObjectName(QStringLiteral("audio-effects-limiter-meter"));
  limiterReductionMeter_->setAccessibleName(I18n::text(QStringLiteral("audio.limiter_limiting_amount")));
  limiterReductionMeter_->setRange(0, 200);
  limiterReductionMeter_->setValue(0);
  limiterReductionMeter_->setTextVisible(false);
  meterLayout->addWidget(limiterReductionMeter_);
  auto *markers = new QHBoxLayout;
  markers->setContentsMargins(0, 0, 0, 0);
  const QStringList markerTexts{QStringLiteral("0 dB"), QStringLiteral("-5 dB"), QStringLiteral("-10 dB"),
                                QStringLiteral("-15 dB"), QStringLiteral("-20 dB")};
  for (const QString &text : markerTexts) {
    auto *marker = label(text, meterCard, QStringLiteral("audio-effects-limiter-meter-marker"));
    if (text == markerTexts.front()) marker->setAlignment(Qt::AlignLeft);
    else if (text == markerTexts.back()) marker->setAlignment(Qt::AlignRight);
    else marker->setAlignment(Qt::AlignCenter);
    markers->addWidget(marker, 1);
  }
  meterLayout->addLayout(markers);
  controlsLayout->addWidget(meterCard);

  auto *reset = new QPushButton(I18n::text(QStringLiteral("common.reset")), controls);
  reset->setObjectName(QStringLiteral("audio-effects-reset"));
  reset->setAccessibleName(I18n::text(QStringLiteral("audio.limiter_reset")));
  controlsLayout->addWidget(reset, 0, Qt::AlignLeft);
  controlsLayout->addStretch();
  layout->addWidget(controls, 1);
  scroll->setWidget(content);
  pages_->addWidget(scroll);

  connect(limiterToggle_, &QCheckBox::toggled, this, [this](bool checked) {
    if (controller_) controller_->setLimiterEnabled(checked);
    updateLimiterMeterPolling();
  });
  connect(limiterCeilingDial_, &QDial::valueChanged, this, [this](int value) {
    if (controller_) controller_->setLimiterCeilingDb(value / 10.0);
  });
  connect(limiterReleaseDial_, &QDial::valueChanged, this, [this](int value) {
    if (controller_) controller_->setLimiterReleaseMs(value);
  });
  connect(limiterLookaheadDial_, &QDial::valueChanged, this, [this](int value) {
    if (controller_) controller_->setLimiterLookaheadMs(value);
  });
  connect(limiterGainDial_, &QDial::valueChanged, this, [this](int value) {
    if (controller_) controller_->setLimiterGainDb(value);
  });
  connect(reset, &QPushButton::clicked, this, [this] {
    if (controller_) controller_->resetLimiter();
  });
}

void AudioEffectsPage::createBassEnhancerPage() {
  auto *scroll = new QScrollArea(pages_);
  scroll->setObjectName(QStringLiteral("audio-effects-content-scroll"));
  scroll->setWidgetResizable(true);
  auto *content = new QWidget(scroll);
  content->setObjectName(QStringLiteral("audio-effects-bass-enhancer-content"));
  content->setMinimumWidth(720);
  auto *layout = new QVBoxLayout(content);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  auto *header = new QFrame(content);
  header->setObjectName(QStringLiteral("audio-effects-bass-enhancer-header"));
  auto *headerLayout = new QHBoxLayout(header);
  headerLayout->setContentsMargins(20, 15, 20, 15);
  headerLayout->setSpacing(10);
  auto *icon = new QFrame(header);
  icon->setObjectName(QStringLiteral("audio-effects-bass-enhancer-icon"));
  icon->setFixedSize(54, 54);
  auto *iconLayout = new QVBoxLayout(icon);
  iconLayout->setContentsMargins(0, 0, 0, 0);
  auto *glyph = label(QStringLiteral("◖"), icon, QStringLiteral("audio-effects-bass-enhancer-icon-glyph"));
  glyph->setAlignment(Qt::AlignCenter);
  iconLayout->addWidget(glyph);
  auto *heading = new QVBoxLayout;
  heading->setSpacing(4);
  heading->addWidget(label(I18n::text(QStringLiteral("audio.module_bass_booster")), header,
                           QStringLiteral("audio-effects-bass-enhancer-title")));
  heading->addWidget(label(I18n::text(QStringLiteral("audio.bass_desc")), header,
                           QStringLiteral("audio-effects-bass-enhancer-subtitle")));
  bassEnhancerToggle_ = new GlowToggleSwitch(I18n::text(QStringLiteral("common.enable")), header);
  bassEnhancerToggle_->setObjectName(QStringLiteral("audio-effects-bass-enhancer-toggle"));
  bassEnhancerToggle_->setAccessibleName(I18n::text(QStringLiteral("audio.bass_enable")));
  moduleToggles_.insert(QStringLiteral("bassboost"), bassEnhancerToggle_);
  headerLayout->addWidget(icon);
  headerLayout->addLayout(heading, 1);
  headerLayout->addWidget(bassEnhancerToggle_, 0, Qt::AlignTop | Qt::AlignRight);
  layout->addWidget(header);

  auto *controls = new QWidget(content);
  controls->setObjectName(QStringLiteral("audio-effects-bass-enhancer-controls"));
  auto *controlsLayout = new QVBoxLayout(controls);
  controlsLayout->setContentsMargins(20, 28, 20, 24);
  controlsLayout->setSpacing(18);

  auto *knobRow = new QHBoxLayout;
  knobRow->setContentsMargins(22, 0, 22, 0);
  knobRow->setSpacing(24);
  const auto addDial = [controls, knobRow](const QString &caption, int minimum, int maximum, int step,
                                            QDial **dialTarget, QLabel **valueTarget) {
    auto *column = new QVBoxLayout;
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(5);
    auto *dial = new EffectDial(EffectDial::Style::Reverb, controls);
    dial->setObjectName(QStringLiteral("audio-effects-bass-enhancer-dial"));
    dial->setRange(minimum, maximum);
    dial->setSingleStep(step);
    dial->setPageStep(std::max(step, (maximum - minimum) / 24));
    dial->setFixedSize(112, 112);
    dial->setAccessibleName(caption);
    dial->refreshColor();
    auto *value = label({}, controls, QStringLiteral("audio-effects-bass-enhancer-value"));
    value->setAlignment(Qt::AlignCenter);
    auto *title = label(caption, controls, QStringLiteral("audio-effects-bass-enhancer-label"));
    title->setAlignment(Qt::AlignCenter);
    column->addWidget(dial, 0, Qt::AlignHCenter);
    column->addWidget(value);
    column->addWidget(title);
    knobRow->addLayout(column, 1);
    *dialTarget = dial;
    *valueTarget = value;
  };
  // Integer QDial storage is scaled by ten so the legacy 17.5 dB Deep value
  // remains exact while direct user movement follows the old 1-unit steps.
  addDial(I18n::text(QStringLiteral("audio.param_frequency")), 200, 1200, 10,
          &bassEnhancerFrequencyDial_, &bassEnhancerFrequencyValue_);
  addDial(I18n::text(QStringLiteral("audio.param_gain")), 0, 180, 10,
          &bassEnhancerGainDial_, &bassEnhancerGainValue_);
  addDial(I18n::text(QStringLiteral("audio.param_harmonics")), 0, 1000, 10,
          &bassEnhancerHarmonicsDial_, &bassEnhancerHarmonicsValue_);
  addDial(I18n::text(QStringLiteral("audio.param_width")), 5, 30, 1,
          &bassEnhancerWidthDial_, &bassEnhancerWidthValue_);
  addDial(I18n::text(QStringLiteral("audio.param_mix")), 0, 1000, 10,
          &bassEnhancerMixDial_, &bassEnhancerMixValue_);
  controlsLayout->addLayout(knobRow);

  auto *deep = new QPushButton(I18n::text(QStringLiteral("audio.bass_deep_button")), controls);
  deep->setObjectName(QStringLiteral("audio-effects-bass-enhancer-deep"));
  deep->setAccessibleName(I18n::text(QStringLiteral("audio.bass_deep")));
  deep->setProperty("active", false);
  controlsLayout->addWidget(deep, 0, Qt::AlignLeft);

  auto *reset = new QPushButton(I18n::text(QStringLiteral("common.reset")), controls);
  reset->setObjectName(QStringLiteral("audio-effects-reset"));
  reset->setAccessibleName(I18n::text(QStringLiteral("audio.bass_reset")));
  controlsLayout->addWidget(reset, 0, Qt::AlignLeft);
  controlsLayout->addStretch();
  layout->addWidget(controls, 1);
  scroll->setWidget(content);
  pages_->addWidget(scroll);

  connect(bassEnhancerToggle_, &QCheckBox::toggled, this, [this](bool checked) {
    if (controller_) controller_->setBassEnhancerEnabled(checked);
  });
  connect(bassEnhancerFrequencyDial_, &QDial::valueChanged, this, [this](int value) {
    if (controller_) controller_->setBassEnhancerFrequencyHz(value / 10.0);
  });
  connect(bassEnhancerGainDial_, &QDial::valueChanged, this, [this](int value) {
    if (controller_) controller_->setBassEnhancerGainDb(value / 10.0);
  });
  connect(bassEnhancerHarmonicsDial_, &QDial::valueChanged, this, [this](int value) {
    if (controller_) controller_->setBassEnhancerHarmonicsPercent(value / 10.0);
  });
  connect(bassEnhancerWidthDial_, &QDial::valueChanged, this, [this](int value) {
    if (controller_) controller_->setBassEnhancerWidth(value / 10.0);
  });
  connect(bassEnhancerMixDial_, &QDial::valueChanged, this, [this](int value) {
    if (controller_) controller_->setBassEnhancerMixPercent(value / 10.0);
  });
  connect(deep, &QPushButton::clicked, this, [this] {
    if (controller_) controller_->applyBassEnhancerDeep();
  });
  connect(reset, &QPushButton::clicked, this, [this] {
    if (controller_) controller_->resetBassEnhancer();
  });
}

void AudioEffectsPage::createAutoGainPage() {
  auto *scroll = new QScrollArea(pages_);
  scroll->setObjectName(QStringLiteral("audio-effects-content-scroll"));
  scroll->setWidgetResizable(true);
  auto *content = new QWidget(scroll);
  content->setObjectName(QStringLiteral("audio-effects-auto-gain-content"));
  content->setMinimumWidth(660);
  auto *layout = new QVBoxLayout(content);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);

  auto *header = new QFrame(content);
  header->setObjectName(QStringLiteral("audio-effects-auto-gain-header"));
  auto *headerLayout = new QHBoxLayout(header);
  headerLayout->setContentsMargins(20, 15, 20, 15);
  headerLayout->setSpacing(10);
  auto *icon = new QFrame(header);
  icon->setObjectName(QStringLiteral("audio-effects-auto-gain-icon"));
  icon->setFixedSize(54, 54);
  auto *iconLayout = new QVBoxLayout(icon);
  iconLayout->setContentsMargins(0, 0, 0, 0);
  auto *glyph = label(QStringLiteral("↕"), icon, QStringLiteral("audio-effects-auto-gain-icon-glyph"));
  glyph->setAlignment(Qt::AlignCenter);
  iconLayout->addWidget(glyph);
  auto *heading = new QVBoxLayout;
  heading->setSpacing(4);
  heading->addWidget(label(I18n::text(QStringLiteral("audio.auto_gain")), header,
                           QStringLiteral("audio-effects-auto-gain-title")));
  heading->addWidget(label(I18n::text(QStringLiteral("audio.auto_gain_desc")), header,
                           QStringLiteral("audio-effects-auto-gain-subtitle")));
  autoGainToggle_ = new GlowToggleSwitch(I18n::text(QStringLiteral("common.enable")), header);
  autoGainToggle_->setObjectName(QStringLiteral("audio-effects-auto-gain-toggle"));
  autoGainToggle_->setAccessibleName(I18n::text(QStringLiteral("audio.auto_gain_enable")));
  moduleToggles_.insert(QStringLiteral("autogain"), autoGainToggle_);
  headerLayout->addWidget(icon);
  headerLayout->addLayout(heading, 1);
  headerLayout->addWidget(autoGainToggle_, 0, Qt::AlignTop | Qt::AlignRight);
  layout->addWidget(header);

  auto *controls = new QWidget(content);
  controls->setObjectName(QStringLiteral("audio-effects-auto-gain-controls"));
  auto *controlsLayout = new QVBoxLayout(controls);
  controlsLayout->setContentsMargins(20, 28, 20, 24);
  controlsLayout->setSpacing(18);

  auto *knobRow = new QHBoxLayout;
  knobRow->setContentsMargins(70, 0, 70, 0);
  knobRow->setSpacing(90);
  const auto addDial = [controls, knobRow](const QString &caption, int minimum, int maximum,
                                           QDial **dialTarget, QLabel **valueTarget) {
    auto *column = new QVBoxLayout;
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(6);
    auto *dial = new EffectDial(EffectDial::Style::Reverb, controls);
    dial->setObjectName(QStringLiteral("audio-effects-auto-gain-dial"));
    dial->setRange(minimum, maximum);
    dial->setSingleStep(10);
    dial->setPageStep(20);
    dial->setFixedSize(124, 124);
    dial->setAccessibleName(caption);
    dial->refreshColor();
    auto *value = label({}, controls, QStringLiteral("audio-effects-auto-gain-value"));
    value->setAlignment(Qt::AlignCenter);
    auto *title = label(caption, controls, QStringLiteral("audio-effects-auto-gain-label"));
    title->setAlignment(Qt::AlignCenter);
    column->addWidget(dial, 0, Qt::AlignHCenter);
    column->addWidget(value);
    column->addWidget(title);
    knobRow->addLayout(column, 1);
    *dialTarget = dial;
    *valueTarget = value;
  };
  addDial(I18n::text(QStringLiteral("audio.param_target_level")), -240, 0, &autoGainTargetDial_, &autoGainTargetValue_);
  addDial(I18n::text(QStringLiteral("audio.param_max_gain")), 0, 240, &autoGainMaxGainDial_, &autoGainMaxGainValue_);
  controlsLayout->addLayout(knobRow);

  auto *presets = new QFrame(controls);
  presets->setObjectName(QStringLiteral("audio-effects-auto-gain-presets"));
  auto *presetLayout = new QVBoxLayout(presets);
  presetLayout->setContentsMargins(15, 13, 15, 15);
  presetLayout->setSpacing(10);
  presetLayout->addWidget(label(I18n::text(QStringLiteral("audio.auto_gain_quick_presets")), presets,
                                QStringLiteral("audio-effects-auto-gain-presets-title")));
  auto *presetRow = new QHBoxLayout;
  presetRow->setSpacing(10);
  const std::array<std::pair<const char *, const char *>, 4> presetDefinitions{{
      {"balanced", "audio.preset_balanced"}, {"night", "audio.preset_night_quiet"},
      {"loud", "audio.preset_high_energy"}, {"speech", "audio.preset_speech_clear"},
  }};
  for (const auto &[id, title] : presetDefinitions) {
    auto *button = new QPushButton(displayName(title), presets);  // catalog value: retranslated live
    button->setObjectName(QStringLiteral("audio-effects-auto-gain-preset"));
    button->setProperty("presetId", QString::fromLatin1(id));
    button->setProperty("active", false);
    presetRow->addWidget(button, 1);
    connect(button, &QPushButton::clicked, this, [this, id] {
      if (controller_) controller_->applyAutoGainPreset(QString::fromLatin1(id));
    });
  }
  presetLayout->addLayout(presetRow);
  controlsLayout->addWidget(presets);

  auto *reset = new QPushButton(I18n::text(QStringLiteral("common.reset")), controls);
  reset->setObjectName(QStringLiteral("audio-effects-reset"));
  reset->setAccessibleName(I18n::text(QStringLiteral("audio.auto_gain_reset")));
  controlsLayout->addWidget(reset, 0, Qt::AlignLeft);
  controlsLayout->addStretch();
  layout->addWidget(controls, 1);
  scroll->setWidget(content);
  pages_->addWidget(scroll);

  connect(autoGainToggle_, &QCheckBox::toggled, this, [this](bool checked) {
    if (controller_) controller_->setAutoGainEnabled(checked);
    updateAutoGainStatusPolling();
  });
  connect(autoGainTargetDial_, &QDial::valueChanged, this, [this](int value) {
    if (controller_) controller_->setAutoGainTargetDbfs(value / 10.0);
  });
  connect(autoGainMaxGainDial_, &QDial::valueChanged, this, [this](int value) {
    if (controller_) controller_->setAutoGainMaxGainDb(value / 10.0);
  });
  connect(reset, &QPushButton::clicked, this, [this] {
    if (controller_) controller_->resetAutoGain();
  });
}

void AudioEffectsPage::createPlaceholderPages() {
  for (int index = 7; index < static_cast<int>(kModules.size()); ++index) {
    auto *page = new QWidget(pages_);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(32, 28, 32, 28);
    auto *header = new QHBoxLayout;
    auto *title = label(displayName(kModules[index].name), page, QStringLiteral("audio-effects-title"));
    const QString moduleId = QString::fromLatin1(kModules[index].id);
    auto *toggle = new GlowToggleSwitch(I18n::text(QStringLiteral("common.enable")), page);
    toggle->setObjectName(QStringLiteral("audio-effects-module-toggle"));
    dalinira::i18n::bindAccessibleName(toggle, QStringLiteral("audio.enable_module"), {nameArg(kModules[index].name)});
    toggle->setProperty("moduleId", moduleId);
    moduleToggles_.insert(moduleId, toggle);
    auto *reset = new QPushButton(I18n::text(QStringLiteral("common.reset")), page);
    reset->setObjectName(QStringLiteral("audio-effects-reset"));
    header->addWidget(title);
    header->addStretch();
    header->addWidget(toggle, 0, Qt::AlignTop | Qt::AlignRight);
    layout->addLayout(header);
    layout->addWidget(reset, 0, Qt::AlignLeft);
    auto *placeholder = label(I18n::text(QStringLiteral("audio.module_coming_soon")), page,
                              QStringLiteral("audio-effects-placeholder"));
    layout->addWidget(placeholder);
    layout->addStretch();
    // This module has no implemented state in Aşama 1; its reset is intentionally scoped no-op.
    connect(reset, &QPushButton::clicked, page, [] {});
    connect(toggle, &QCheckBox::toggled, this, [this, moduleId](bool enabled) {
      if (controller_) controller_->setModuleEnabled(moduleId, enabled);
    });
    pages_->addWidget(page);
  }
}

void AudioEffectsPage::updateFromController() {
  if (!controller_) return;
  const WebAudioEffectsController::Status status = controller_->status();
  const QSignalBlocker toggleBlock(globalToggle_);
  const QSignalBlocker dialBlock(preampDial_);
  globalToggle_->setChecked(controller_->enabled());
  globalToggle_->setText(controller_->enabled()
      ? I18n::text(QStringLiteral("audio.effects_on"))
      : I18n::text(QStringLiteral("audio.effects_off")));
  for (auto toggle = moduleToggles_.cbegin(); toggle != moduleToggles_.cend(); ++toggle) {
    if (!toggle.value()) continue;
    const QSignalBlocker moduleBlock(toggle.value());
    toggle.value()->setChecked(controller_->moduleEnabled(toggle.key()));
  }
  preampDial_->setValue(qRound(controller_->preampDb() * 10.0));
  preampValue_->setText(QStringLiteral("%1 dB").arg(controller_->preampDb(), 0, 'f', 1));
  const QVector<double> bands = controller_->equalizerBands();
  for (int index = 0; index < equalizerSliders_.size() && index < bands.size(); ++index) {
    const QSignalBlocker sliderBlock(equalizerSliders_[index]);
    equalizerSliders_[index]->setValue(qRound(bands[index] * 10.0));
    equalizerValues_[index]->setText(QStringLiteral("%1 dB").arg(bands[index], 0, 'f', 1));
  }
  const struct { QDial *dial; QLabel *value; double db; } tones[] = {
      {bassDial_, bassValue_, controller_->bassDb()}, {midDial_, midValue_, controller_->midDb()}, {trebleDial_, trebleValue_, controller_->trebleDb()}
  };
  for (const auto &tone : tones) {
    if (!tone.dial || !tone.value) continue;
    const QSignalBlocker toneBlock(tone.dial);
    tone.dial->setValue(qRound(tone.db * 10.0));
    if (auto *effectDial = dynamic_cast<EffectDial *>(tone.dial)) effectDial->refreshColor();
    tone.value->setText(QStringLiteral("%1 dB").arg(tone.db, 0, 'f', 1));
  }
  if (stereoExpanderDial_ && stereoExpanderValue_) {
    const QSignalBlocker stereoBlock(stereoExpanderDial_);
    stereoExpanderDial_->setValue(qRound(controller_->stereoExpanderPercent()));
    if (auto *effectDial = dynamic_cast<EffectDial *>(stereoExpanderDial_)) effectDial->refreshColor();
    stereoExpanderValue_->setText(QStringLiteral("%1 %").arg(controller_->stereoExpanderPercent(), 0, 'f', 0));
  }
  if (acousticSpaceSelect_) {
    const QSignalBlocker acousticBlock(acousticSpaceSelect_);
    const int index = acousticSpaceSelect_->findData(controller_->acousticSpace());
    acousticSpaceSelect_->setCurrentIndex(index >= 0 ? index : 0);
  }
  if (balanceSlider_ && balanceValue_) {
    const QSignalBlocker balanceBlock(balanceSlider_);
    balanceSlider_->setValue(qRound(controller_->balance()));
    const double balance = controller_->balance();
    balanceValue_->setText(qFuzzyIsNull(balance) ? I18n::text(QStringLiteral("audio.balance_center"))
                                                : (balance < 0 ? I18n::text(QStringLiteral("audio.balance_left")).arg(qRound(balance))
                                                               : I18n::text(QStringLiteral("audio.balance_right")).arg(qRound(balance))));
  }
  const struct { QDial *dial; QLabel *value; double scaledValue; int scale; QString text; } reverbs[] = {
      {reverbRoomSizeDial_, reverbRoomSizeValue_, controller_->reverbRoomSizeMs(), 10,
       QStringLiteral("%1 ms").arg(controller_->reverbRoomSizeMs(), 0, 'f', 1)},
      {reverbDampingDial_, reverbDampingValue_, controller_->reverbDamping(), 1000,
       compactDecimal(controller_->reverbDamping())},
      {reverbWetDryDial_, reverbWetDryValue_, controller_->reverbWetDryDb(), 10,
       QStringLiteral("%1 dB").arg(controller_->reverbWetDryDb(), 0, 'f', 1)},
      {reverbHfRatioDial_, reverbHfRatioValue_, controller_->reverbHfRatio(), 1000,
       compactDecimal(controller_->reverbHfRatio())},
      {reverbInputGainDial_, reverbInputGainValue_, controller_->reverbInputGainDb(), 10,
       QStringLiteral("%1 dB").arg(controller_->reverbInputGainDb(), 0, 'f', 1)},
  };
  for (const auto &reverb : reverbs) {
    if (!reverb.dial || !reverb.value) continue;
    const QSignalBlocker dialBlock(reverb.dial);
    reverb.dial->setValue(qRound(reverb.scaledValue * reverb.scale));
    if (auto *effectDial = dynamic_cast<EffectDial *>(reverb.dial)) effectDial->refreshColor();
    reverb.value->setText(reverb.text);
  }
  const QString selectedReverbPreset = controller_->reverbPreset();
  for (QPushButton *button : findChildren<QPushButton *>(QStringLiteral("audio-effects-reverb-preset"))) {
    const bool active = button->property("presetId").toString() == selectedReverbPreset;
    if (button->property("active").toBool() == active) continue;
    button->setProperty("active", active);
    button->style()->unpolish(button);
    button->style()->polish(button);
    button->update();
  }
  const struct { QDial *dial; QLabel *value; double scaledValue; int scale; QString text; } compressors[] = {
      {compressorThresholdDial_, compressorThresholdValue_, controller_->compressorThresholdDb(), 10,
       QStringLiteral("%1 dB").arg(controller_->compressorThresholdDb(), 0, 'f', 1)},
      {compressorRatioDial_, compressorRatioValue_, controller_->compressorRatio(), 10,
       QStringLiteral("%1 :1").arg(controller_->compressorRatio(), 0, 'f', 1)},
      {compressorAttackDial_, compressorAttackValue_, controller_->compressorAttackMs(), 10,
       QStringLiteral("%1 ms").arg(controller_->compressorAttackMs(), 0, 'f', 1)},
      {compressorReleaseDial_, compressorReleaseValue_, controller_->compressorReleaseMs(), 10,
       QStringLiteral("%1 ms").arg(controller_->compressorReleaseMs(), 0, 'f', 1)},
      {compressorMakeupDial_, compressorMakeupValue_, controller_->compressorMakeupDb(), 10,
       QStringLiteral("%1 dB").arg(controller_->compressorMakeupDb(), 0, 'f', 1)},
      {compressorKneeDial_, compressorKneeValue_, controller_->compressorKneeDb(), 10,
       QStringLiteral("%1 dB").arg(controller_->compressorKneeDb(), 0, 'f', 1)},
  };
  for (const auto &compressor : compressors) {
    if (!compressor.dial || !compressor.value) continue;
    const QSignalBlocker dialBlock(compressor.dial);
    compressor.dial->setValue(qRound(compressor.scaledValue * compressor.scale));
    if (auto *effectDial = dynamic_cast<EffectDial *>(compressor.dial)) effectDial->refreshColor();
    compressor.value->setText(compressor.text);
  }
  const QString selectedCompressorPreset = controller_->compressorPreset();
  for (QPushButton *button : findChildren<QPushButton *>(QStringLiteral("audio-effects-compressor-preset"))) {
    const bool active = button->property("presetId").toString() == selectedCompressorPreset;
    if (button->property("active").toBool() == active) continue;
    button->setProperty("active", active);
    button->style()->unpolish(button);
    button->style()->polish(button);
    button->update();
  }
  const struct { QDial *dial; QLabel *value; double scaledValue; int scale; QString text; } limiters[] = {
      {limiterCeilingDial_, limiterCeilingValue_, controller_->limiterCeilingDb(), 10,
       QStringLiteral("%1 dB").arg(controller_->limiterCeilingDb(), 0, 'f', 1)},
      {limiterReleaseDial_, limiterReleaseValue_, controller_->limiterReleaseMs(), 1,
       QStringLiteral("%1 ms").arg(controller_->limiterReleaseMs(), 0, 'f', 1)},
      {limiterLookaheadDial_, limiterLookaheadValue_, controller_->limiterLookaheadMs(), 1,
       QStringLiteral("%1 ms").arg(controller_->limiterLookaheadMs(), 0, 'f', 1)},
      {limiterGainDial_, limiterGainValue_, controller_->limiterGainDb(), 1,
       QStringLiteral("%1 dB").arg(controller_->limiterGainDb(), 0, 'f', 1)},
  };
  for (const auto &limiter : limiters) {
    if (!limiter.dial || !limiter.value) continue;
    const QSignalBlocker dialBlock(limiter.dial);
    limiter.dial->setValue(qRound(limiter.scaledValue * limiter.scale));
    if (auto *effectDial = dynamic_cast<EffectDial *>(limiter.dial)) effectDial->refreshColor();
    limiter.value->setText(limiter.text);
  }
  const QString selectedLimiterPreset = controller_->limiterPreset();
  for (QPushButton *button : findChildren<QPushButton *>(QStringLiteral("audio-effects-limiter-preset"))) {
    const bool active = button->property("presetId").toString() == selectedLimiterPreset;
    if (button->property("active").toBool() == active) continue;
    button->setProperty("active", active);
    button->style()->unpolish(button);
    button->style()->polish(button);
    button->update();
  }
  const struct { QDial *dial; QLabel *value; double scaledValue; int scale; QString text; } bassEnhancers[] = {
      {bassEnhancerFrequencyDial_, bassEnhancerFrequencyValue_, controller_->bassEnhancerFrequencyHz(), 10,
       QStringLiteral("%1 Hz").arg(controller_->bassEnhancerFrequencyHz(), 0, 'f', 1)},
      {bassEnhancerGainDial_, bassEnhancerGainValue_, controller_->bassEnhancerGainDb(), 10,
       QStringLiteral("%1 dB").arg(controller_->bassEnhancerGainDb(), 0, 'f', 1)},
      {bassEnhancerHarmonicsDial_, bassEnhancerHarmonicsValue_, controller_->bassEnhancerHarmonicsPercent(), 10,
       QStringLiteral("%1 %").arg(controller_->bassEnhancerHarmonicsPercent(), 0, 'f', 1)},
      {bassEnhancerWidthDial_, bassEnhancerWidthValue_, controller_->bassEnhancerWidth(), 10,
       QString::number(controller_->bassEnhancerWidth(), 'f', 1)},
      {bassEnhancerMixDial_, bassEnhancerMixValue_, controller_->bassEnhancerMixPercent(), 10,
       QStringLiteral("%1 %").arg(controller_->bassEnhancerMixPercent(), 0, 'f', 1)},
  };
  for (const auto &bassEnhancer : bassEnhancers) {
    if (!bassEnhancer.dial || !bassEnhancer.value) continue;
    const QSignalBlocker dialBlock(bassEnhancer.dial);
    bassEnhancer.dial->setValue(qRound(bassEnhancer.scaledValue * bassEnhancer.scale));
    if (auto *effectDial = dynamic_cast<EffectDial *>(bassEnhancer.dial)) effectDial->refreshColor();
    bassEnhancer.value->setText(bassEnhancer.text);
  }
  if (auto *deep = findChild<QPushButton *>(QStringLiteral("audio-effects-bass-enhancer-deep"))) {
    const bool active = controller_->bassEnhancerDeep();
    if (deep->property("active").toBool() != active) {
      deep->setProperty("active", active);
      deep->style()->unpolish(deep);
      deep->style()->polish(deep);
      deep->update();
    }
  }
  if (autoGainTargetDial_ && autoGainTargetValue_) {
    const QSignalBlocker block(autoGainTargetDial_);
    autoGainTargetDial_->setValue(qRound(controller_->autoGainTargetDbfs() * 10.0));
    if (auto *effectDial = dynamic_cast<EffectDial *>(autoGainTargetDial_)) effectDial->refreshColor();
    autoGainTargetValue_->setText(QStringLiteral("%1 dBFS").arg(controller_->autoGainTargetDbfs(), 0, 'f', 1));
  }
  if (autoGainMaxGainDial_ && autoGainMaxGainValue_) {
    const QSignalBlocker block(autoGainMaxGainDial_);
    autoGainMaxGainDial_->setValue(qRound(controller_->autoGainMaxGainDb() * 10.0));
    if (auto *effectDial = dynamic_cast<EffectDial *>(autoGainMaxGainDial_)) effectDial->refreshColor();
    autoGainMaxGainValue_->setText(QStringLiteral("%1 dB").arg(controller_->autoGainMaxGainDb(), 0, 'f', 1));
  }
  const QString selectedAutoGainPreset = controller_->autoGainPreset();
  for (QPushButton *button : findChildren<QPushButton *>(QStringLiteral("audio-effects-auto-gain-preset"))) {
    const bool active = button->property("presetId").toString() == selectedAutoGainPreset;
    if (button->property("active").toBool() == active) continue;
    button->setProperty("active", active);
    button->style()->unpolish(button);
    button->style()->polish(button);
    button->update();
  }
  if (autoGainToggle_) {
    const bool hardAutoGainFailure = status.hardFailure;
    autoGainToggle_->setEnabled(!hardAutoGainFailure);
    autoGainToggle_->setText(hardAutoGainFailure ? I18n::text(QStringLiteral("common.unavailable")) : I18n::text(QStringLiteral("common.enable")));
    autoGainToggle_->setToolTip(hardAutoGainFailure ? localizedDetail(status) : QString());
  }
  QString rate;
  if (!status.sampleRates.isEmpty()) {
    QStringList labels;
    for (const int sampleRate : status.sampleRates) labels.push_back(QStringLiteral("%1 kHz").arg(sampleRate / 1000.0, 0, 'f', 1));
    rate = labels.join(QStringLiteral(", "));
  }
  statusLabel_->setText(status.engineAvailable
      ? I18n::text(QStringLiteral("audio.web_dsp_status")).arg(status.enabled ? I18n::text(QStringLiteral("common.on")) : I18n::text(QStringLiteral("audio.bypass")))
            .arg(status.attachedMediaCount).arg(rate.isEmpty() ? QString() : QStringLiteral(" • %1").arg(rate))
      : I18n::text(QStringLiteral("audio.web_dsp_preparing")));
  if (auto *detail = findChild<QLabel *>(QStringLiteral("audio-effects-output-detail"))) {
    detail->setText(localizedDetail(status) + (rate.isEmpty() ? QString() : I18n::text(QStringLiteral("audio.sample_rate_suffix")).arg(rate)));
  }
  updateCompressorMeterPolling();
  updateLimiterMeterPolling();
  updateAutoGainStatusPolling();
}
