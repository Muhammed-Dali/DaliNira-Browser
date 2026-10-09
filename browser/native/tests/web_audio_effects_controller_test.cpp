#include "audio_effects_page.h"
#include "eq_preset_page.h"
#include "web_audio_effects_controller.h"

#include <QApplication>
#include <QCheckBox>
#include <QDial>
#include <QElapsedTimer>
#include <QFile>
#include <QListWidget>
#include <QListView>
#include <QMouseEvent>
#include <QProgressBar>
#include <QPixmap>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QSlider>
#include <QTemporaryDir>
#include <QWebEnginePage>
#include <QWebEngineView>

#include <limits>


namespace {
QDial *dialWithName(AudioEffectsPage &page, const QString &name) {
  for (QDial *dial : page.findChildren<QDial *>()) {
    if (dial->accessibleName() == name) return dial;
  }
  return nullptr;
}

void dragToMaximum(QDial *dial) {
  const QPointF start(dial->width() / 2.0, dial->height() / 2.0);
  const QPointF top(start.x(), start.y() - 180.0);
  QMouseEvent press(QEvent::MouseButtonPress, start, start, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QMouseEvent move(QEvent::MouseMove, top, top, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
  QMouseEvent release(QEvent::MouseButtonRelease, top, top, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(dial, &press);
  QApplication::sendEvent(dial, &move);
  QApplication::sendEvent(dial, &release);
}

bool clickListIndex(QListView *view, const QModelIndex &index) {
  if (!view || !index.isValid()) return false;
  view->scrollTo(index);
  QApplication::processEvents();
  const QRect rect = view->visualRect(index);
  if (!rect.isValid()) return false;
  const QPointF point = rect.center();
  QMouseEvent press(QEvent::MouseButtonPress, point, point, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QMouseEvent release(QEvent::MouseButtonRelease, point, point, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(view->viewport(), &press);
  QApplication::sendEvent(view->viewport(), &release);
  return true;
}
}  // namespace

int main(int argc, char *argv[]) {
  QApplication app(argc, argv);
  QTemporaryDir directory;
  if (!directory.isValid()) return 1;
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, directory.path());
  QCoreApplication::setOrganizationName(QStringLiteral("DaliNiraTest"));
  QCoreApplication::setApplicationName(QStringLiteral("WebAudioEffectsPersistence"));

  {
    WebAudioEffectsController controller;
    const QVector<int> &frequencies = WebAudioEffectsController::equalizerFrequencies();
    if (controller.enabled() || controller.preampDb() != 0.0 || frequencies.size() != 32
        || frequencies.front() != 20 || frequencies[18] != 1300 || frequencies.back() != 22000
        || controller.reverbEnabled() || controller.reverbRoomSizeMs() != 1000.0 || controller.reverbDamping() != 0.5
        || controller.reverbWetDryDb() != -10.0 || controller.reverbHfRatio() != 0.7 || controller.reverbInputGainDb() != 0.0
        || controller.compressorEnabled() || controller.compressorThresholdDb() != -20.0 || controller.compressorRatio() != 4.0
        || controller.compressorAttackMs() != 10.0 || controller.compressorReleaseMs() != 100.0
        || controller.compressorMakeupDb() != 0.0 || controller.compressorKneeDb() != 3.0
        || !controller.compressorPreset().isEmpty() || controller.limiterEnabled()
        || controller.limiterCeilingDb() != -0.3 || controller.limiterReleaseMs() != 50.0
        || controller.limiterLookaheadMs() != 5.0 || controller.limiterGainDb() != 0.0
        || !controller.limiterPreset().isEmpty() || controller.bassEnhancerEnabled()
        || controller.bassEnhancerFrequencyHz() != 80.0 || controller.bassEnhancerGainDb() != 6.0
        || controller.bassEnhancerHarmonicsPercent() != 50.0 || controller.bassEnhancerWidth() != 1.5
        || controller.bassEnhancerMixPercent() != 50.0 || controller.bassEnhancerDeep()
        || controller.autoGainEnabled() || controller.autoGainTargetDbfs() != -14.0
        || controller.autoGainMaxGainDb() != 12.0 || controller.autoGainSpeed() != QStringLiteral("medium")
        || controller.autoGainPreset() != QStringLiteral("balanced")) return 1;
    const QString runtime = controller.injectionScript();
    if (!runtime.contains(QStringLiteral("options && options.mediaLatencyHint || 'playback'"))
        || !runtime.contains(QStringLiteral("protocol === 'file:'"))
        || !runtime.contains(QStringLiteral("!cfg.enabled || !root.isAudibleMedia(element)"))
        || !runtime.contains(QStringLiteral("root.scheduleIdleCheck"))
        || !runtime.contains(QStringLiteral("root.audioContext = ctx"))
        || !runtime.contains(QStringLiteral("document.addEventListener('canplay'"))) return 1;
    controller.setEnabled(false);
    controller.setPreampDb(7.3);
    controller.setEqualizerBand(0, -12.0);
    controller.setEqualizerBand(17, 6.5);
    controller.setEqualizerBand(31, 12.0);
    controller.setBassDb(4.5);
    controller.setMidDb(-2.0);
    controller.setTrebleDb(3.0);
    controller.setStereoExpanderPercent(135.0);
    controller.setBalance(-10.0);
    controller.setBassDb(12.0);
    controller.setMidDb(12.0);
    if (controller.bassDb() != 12.0 || controller.midDb() != 12.0 || controller.trebleDb() != 3.0
        || controller.stereoExpanderPercent() != 135.0) return 1;
    controller.setTrebleDb(12.0);
    controller.setStereoExpanderPercent(200.0);
    if (controller.bassDb() != 12.0 || controller.midDb() != 12.0 || controller.trebleDb() != 12.0
        || controller.stereoExpanderPercent() != 200.0) return 1;
  }
  {
    WebAudioEffectsController restored;
    if (restored.enabled() || qAbs(restored.preampDb() - 7.3) > 0.001
        || restored.equalizerBands().size() != 32 || restored.equalizerBand(0) != -12.0
        || restored.equalizerBand(17) != 6.5 || restored.equalizerBand(31) != 12.0
        || restored.bassDb() != 12.0 || restored.midDb() != 12.0 || restored.trebleDb() != 12.0
        || restored.stereoExpanderPercent() != 200.0 || restored.balance() != -10.0) return 1;
    restored.setAcousticSpace(QStringLiteral("hall"));
    if (restored.acousticSpace() != QStringLiteral("hall") || restored.bassDb() != 4.5 || restored.midDb() != 1.8
        || restored.trebleDb() != -2.5 || restored.stereoExpanderPercent() != 145.0) return 1;
    restored.resetEqualizerModule();
    if (restored.equalizerBand(17) != 6.5 || restored.preampDb() != 7.3 || restored.acousticSpace() != QStringLiteral("off")
        || restored.bassDb() != 0.0 || restored.midDb() != 0.0 || restored.trebleDb() != 0.0
        || restored.stereoExpanderPercent() != 100.0 || restored.balance() != 0.0) return 1;
    restored.resetEqualizer();
    if (restored.preampDb() != 7.3 || restored.enabled() || restored.bassDb() != 0.0 || restored.midDb() != 0.0
        || restored.trebleDb() != 0.0 || restored.stereoExpanderPercent() != 100.0 || restored.balance() != 0.0) return 1;
    for (const double band : restored.equalizerBands()) if (band != 0.0) return 1;
    restored.resetOutput();
    if (restored.enabled() || restored.preampDb() != 0.0) return 1;
  }
  {
    WebAudioEffectsController baseline;
    const bool enabled = baseline.enabled();
    const double preamp = baseline.preampDb();
    {
      WebAudioEffectsController privateController(nullptr, false);
      privateController.setEnabled(!enabled);
      privateController.setPreampDb(preamp == 5.5 ? -5.5 : 5.5);
    }
    WebAudioEffectsController restored;
    if (restored.enabled() != enabled || restored.preampDb() != preamp) return 1;
  }
  {
    QSettings settings;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    settings.setValue(QStringLiteral("audioEffects/web/output/preampDb"), nan);
    settings.setValue(QStringLiteral("audioEffects/web/equalizer/band0"), nan);
    settings.setValue(QStringLiteral("audioEffects/web/equalizer/bassDb"), nan);
    settings.setValue(QStringLiteral("audioEffects/web/equalizer/stereoExpanderPercent"), nan);
    settings.setValue(QStringLiteral("audioEffects/web/reverb/roomSizeMs"), nan);
    settings.sync();
    WebAudioEffectsController sanitized;
    if (!std::isfinite(sanitized.preampDb()) || sanitized.preampDb() != 0.0
        || !std::isfinite(sanitized.equalizerBand(0)) || sanitized.equalizerBand(0) != 0.0
        || !std::isfinite(sanitized.bassDb()) || sanitized.bassDb() != 0.0
        || !std::isfinite(sanitized.stereoExpanderPercent()) || sanitized.stereoExpanderPercent() != 100.0
        || !std::isfinite(sanitized.reverbRoomSizeMs()) || sanitized.reverbRoomSizeMs() != 1000.0) return 1;
  }
  {
    WebAudioEffectsController controller;
    controller.setAcousticSpace(QStringLiteral("medium"));
    controller.setBalance(8.0);
  }
  {
    WebAudioEffectsController restored;
    if (restored.acousticSpace() != QStringLiteral("medium") || restored.bassDb() != 2.5 || restored.midDb() != 1.0
        || restored.trebleDb() != -1.0 || restored.stereoExpanderPercent() != 120.0 || restored.balance() != 8.0) return 1;
  }
  {
    WebAudioEffectsController controller;
    controller.resetEqualizer();
    AudioEffectsPage page(&controller);
    QDial *bass = dialWithName(page, QStringLiteral("Bas"));
    QDial *mid = dialWithName(page, QStringLiteral("Mid"));
    QDial *treble = dialWithName(page, QStringLiteral("Tiz"));
    QDial *stereo = dialWithName(page, QStringLiteral("Stereo Expander"));
    if (!bass || !mid || !treble || !stereo) {
      qCritical() << "dial lookup failed" << bass << mid << treble << stereo;
      return 1;
    }
    dragToMaximum(bass);
    dragToMaximum(mid);
    dragToMaximum(treble);
    dragToMaximum(stereo);
    if (controller.bassDb() != 12.0 || controller.midDb() != 12.0 || controller.trebleDb() != 12.0
        || controller.stereoExpanderPercent() != 200.0 || bass->value() != 120 || mid->value() != 120
        || treble->value() != 120 || stereo->value() != 200) {
      qCritical() << "independent dial drag failed" << controller.bassDb() << controller.midDb() << controller.trebleDb()
                  << controller.stereoExpanderPercent() << bass->value() << mid->value() << treble->value() << stereo->value();
      return 1;
    }
    QVector<QSlider *> bands;
    for (QSlider *slider : page.findChildren<QSlider *>()) {
      if (slider->objectName() == QStringLiteral("audio-effects-eq-slider")) bands.push_back(slider);
    }
    if (bands.size() != 32 || bands[0]->minimum() != -120 || bands[0]->maximum() != 120) return 1;
    bands[0]->setValue(120);
    bands[1]->setValue(-120);
    bands[2]->setValue(90);
    bands[3]->setValue(-70);
    if (controller.equalizerBand(0) != 12.0 || controller.equalizerBand(1) != -12.0
        || controller.equalizerBand(2) != 9.0 || controller.equalizerBand(3) != -7.0
        || bands[0]->value() != 120 || bands[1]->value() != -120 || bands[2]->value() != 90 || bands[3]->value() != -70) return 1;
  }
  {
    // Module state has a separate persistence domain from the master DSP.  A
    // selected Reverb preset must not enable it, and reset must not disable it.
    WebAudioEffectsController controller;
    controller.setEnabled(true);
    controller.setReverbEnabled(false);
    controller.applyReverbPreset(QStringLiteral("cathedral"));
    if (controller.reverbEnabled() || controller.reverbPreset() != QStringLiteral("cathedral")
        || controller.reverbRoomSizeMs() != 3000.0 || controller.reverbDamping() != 0.2
        || controller.reverbWetDryDb() != -6.0 || controller.reverbHfRatio() != 0.9 || controller.reverbInputGainDb() != 0.0) return 1;
    controller.setReverbEnabled(true);
    controller.setReverbEnabled(false);
    if (controller.reverbEnabled() || controller.reverbPreset() != QStringLiteral("cathedral")
        || controller.reverbRoomSizeMs() != 3000.0) return 1;
    controller.setReverbEnabled(true);
    if (!controller.reverbEnabled() || controller.reverbPreset() != QStringLiteral("cathedral")
        || controller.reverbWetDryDb() != -6.0) return 1;
    controller.setEnabled(false);
    if (controller.enabled() || !controller.reverbEnabled()) return 1;
    controller.resetReverb();
    if (!controller.reverbEnabled() || controller.reverbPreset().size() != 0 || controller.reverbRoomSizeMs() != 1000.0
        || controller.reverbDamping() != 0.5 || controller.reverbWetDryDb() != -10.0 || controller.reverbHfRatio() != 0.7
        || controller.reverbInputGainDb() != 0.0) return 1;
    controller.applyReverbPreset(QStringLiteral("arena"));
    if (!controller.reverbEnabled() || controller.reverbPreset() != QStringLiteral("arena")
        || controller.reverbRoomSizeMs() != 2800.0 || controller.reverbDamping() != 0.28
        || controller.reverbWetDryDb() != -7.0 || controller.reverbHfRatio() != 0.84 || controller.reverbInputGainDb() != 1.0) return 1;
    controller.setModuleEnabled(QStringLiteral("compressor"), true);
    controller.setModuleEnabled(QStringLiteral("limiter"), false);
    if (!controller.moduleEnabled(QStringLiteral("compressor")) || controller.moduleEnabled(QStringLiteral("limiter"))) return 1;
  }
  {
    WebAudioEffectsController restored;
    if (restored.enabled() || !restored.reverbEnabled() || restored.reverbPreset() != QStringLiteral("arena")
        || restored.reverbRoomSizeMs() != 2800.0 || restored.reverbDamping() != 0.28
        || restored.reverbWetDryDb() != -7.0 || restored.reverbHfRatio() != 0.84 || restored.reverbInputGainDb() != 1.0
        || !restored.moduleEnabled(QStringLiteral("compressor")) || restored.moduleEnabled(QStringLiteral("limiter"))) return 1;
  }
  {
    // Exact legacy compressor presets, range validation, persistence and reset
    // isolation. Presets/reset never own either the module or master switch.
    struct Preset {
      const char *id;
      double threshold;
      double ratio;
      double attack;
      double release;
      double makeup;
      double knee;
    };
    const Preset presets[] = {
        {"gentle", -18.0, 2.2, 18.0, 180.0, 1.5, 5.0},
        {"vocal", -24.0, 3.2, 8.0, 120.0, 3.0, 4.0},
        {"night", -30.0, 4.8, 12.0, 260.0, 2.0, 6.0},
        {"punch", -16.0, 5.5, 4.0, 90.0, 2.5, 2.0},
        {"broadcast", -22.0, 6.0, 3.0, 160.0, 4.0, 3.0},
    };
    WebAudioEffectsController controller;
    controller.setEnabled(false);
    controller.setCompressorEnabled(false);
    for (const Preset &preset : presets) {
      controller.applyCompressorPreset(QLatin1String(preset.id));
      if (controller.compressorEnabled() || controller.compressorPreset() != QLatin1String(preset.id)
          || controller.compressorThresholdDb() != preset.threshold || controller.compressorRatio() != preset.ratio
          || controller.compressorAttackMs() != preset.attack || controller.compressorReleaseMs() != preset.release
          || controller.compressorMakeupDb() != preset.makeup || controller.compressorKneeDb() != preset.knee) return 1;
    }
    controller.setCompressorThresholdDb(-999.0);
    controller.setCompressorRatio(999.0);
    controller.setCompressorAttackMs(0.0);
    controller.setCompressorReleaseMs(9999.0);
    controller.setCompressorMakeupDb(-999.0);
    controller.setCompressorKneeDb(999.0);
    if (controller.compressorThresholdDb() != -60.0 || controller.compressorRatio() != 20.0
        || controller.compressorAttackMs() != 0.1 || controller.compressorReleaseMs() != 1000.0
        || controller.compressorMakeupDb() != -12.0 || controller.compressorKneeDb() != 10.0) return 1;
    controller.setCompressorThresholdDb(std::numeric_limits<double>::quiet_NaN());
    controller.setCompressorRatio(std::numeric_limits<double>::infinity());
    if (controller.compressorThresholdDb() != -20.0 || controller.compressorRatio() != 4.0) return 1;

    controller.setPreampDb(2.5);
    controller.setEqualizerBand(7, -3.5);
    controller.setReverbEnabled(true);
    controller.setReverbRoomSizeMs(1500.0);
    controller.setCompressorEnabled(true);
    controller.applyCompressorPreset(QStringLiteral("vocal"));
    controller.resetCompressor();
    if (!controller.compressorEnabled() || controller.enabled() || controller.compressorThresholdDb() != -20.0
        || controller.compressorRatio() != 4.0 || controller.compressorAttackMs() != 10.0
        || controller.compressorReleaseMs() != 100.0 || controller.compressorMakeupDb() != 0.0
        || controller.compressorKneeDb() != 3.0 || !controller.compressorPreset().isEmpty()
        || controller.preampDb() != 2.5 || controller.equalizerBand(7) != -3.5
        || !controller.reverbEnabled() || controller.reverbRoomSizeMs() != 1500.0) return 1;
    controller.applyCompressorPreset(QStringLiteral("broadcast"));
  }
  {
    WebAudioEffectsController restored;
    if (restored.enabled() || !restored.compressorEnabled() || restored.compressorPreset() != QStringLiteral("broadcast")
        || restored.compressorThresholdDb() != -22.0 || restored.compressorRatio() != 6.0
        || restored.compressorAttackMs() != 3.0 || restored.compressorReleaseMs() != 160.0
        || restored.compressorMakeupDb() != 4.0 || restored.compressorKneeDb() != 3.0
        || restored.preampDb() != 2.5 || restored.equalizerBand(7) != -3.5
        || !restored.reverbEnabled() || restored.reverbRoomSizeMs() != 1500.0) return 1;
  }
  {
    struct Preset { const char *id; double ceiling; double release; double lookahead; double gain; };
    const Preset presets[] = {
        {"transparent", -1.0, 180.0, 5.0, 0.0},
        {"loud", -0.5, 90.0, 7.0, 4.0},
        {"streaming", -1.0, 130.0, 6.0, 2.0},
        {"night", -2.0, 240.0, 8.0, -1.0},
        {"safe", -3.0, 180.0, 10.0, 0.0},
    };
    WebAudioEffectsController controller;
    controller.setLimiterEnabled(false);
    for (const Preset &preset : presets) {
      controller.applyLimiterPreset(QLatin1String(preset.id));
      if (controller.limiterEnabled() || controller.limiterPreset() != QLatin1String(preset.id)
          || controller.limiterCeilingDb() != preset.ceiling || controller.limiterReleaseMs() != preset.release
          || controller.limiterLookaheadMs() != preset.lookahead || controller.limiterGainDb() != preset.gain) return 1;
    }
    controller.setLimiterCeilingDb(-99.0);
    controller.setLimiterReleaseMs(9999.0);
    controller.setLimiterLookaheadMs(-1.0);
    controller.setLimiterGainDb(99.0);
    if (controller.limiterCeilingDb() != -12.0 || controller.limiterReleaseMs() != 500.0
        || controller.limiterLookaheadMs() != 0.0 || controller.limiterGainDb() != 12.0) return 1;
    controller.setLimiterCeilingDb(std::numeric_limits<double>::quiet_NaN());
    controller.setLimiterReleaseMs(std::numeric_limits<double>::infinity());
    if (controller.limiterCeilingDb() != -0.3 || controller.limiterReleaseMs() != 50.0) return 1;

    controller.setEnabled(false);
    controller.setPreampDb(3.5);
    controller.setEqualizerBand(9, -4.5);
    controller.setReverbEnabled(true);
    controller.setReverbRoomSizeMs(1700.0);
    controller.setCompressorEnabled(true);
    controller.applyCompressorPreset(QStringLiteral("vocal"));
    controller.setModuleEnabled(QStringLiteral("bassboost"), true);
    controller.setLimiterEnabled(true);
    controller.applyLimiterPreset(QStringLiteral("loud"));
    controller.resetLimiter();
    if (!controller.limiterEnabled() || controller.enabled() || controller.limiterCeilingDb() != -0.3
        || controller.limiterReleaseMs() != 50.0 || controller.limiterLookaheadMs() != 5.0
        || controller.limiterGainDb() != 0.0 || !controller.limiterPreset().isEmpty()
        || controller.preampDb() != 3.5 || controller.equalizerBand(9) != -4.5
        || !controller.reverbEnabled() || controller.reverbRoomSizeMs() != 1700.0
        || !controller.compressorEnabled() || controller.compressorPreset() != QStringLiteral("vocal")
        || !controller.moduleEnabled(QStringLiteral("bassboost"))) return 1;
    controller.applyLimiterPreset(QStringLiteral("streaming"));
  }
  {
    WebAudioEffectsController restored;
    if (restored.enabled() || !restored.limiterEnabled() || restored.limiterPreset() != QStringLiteral("streaming")
        || restored.limiterCeilingDb() != -1.0 || restored.limiterReleaseMs() != 130.0
        || restored.limiterLookaheadMs() != 6.0 || restored.limiterGainDb() != 2.0
        || restored.preampDb() != 3.5 || restored.equalizerBand(9) != -4.5
        || !restored.reverbEnabled() || restored.reverbRoomSizeMs() != 1700.0
        || !restored.compressorEnabled() || restored.compressorPreset() != QStringLiteral("vocal")
        || !restored.moduleEnabled(QStringLiteral("bassboost"))) return 1;
  }
  {
    // Bass Enhancer defaults/ranges come from the legacy scoped Web DALI UI.
    // Deep is the one legacy preset selection, not a separate native/BASS
    // algorithm, and it never owns the module switch in Stage 6.
    WebAudioEffectsController controller;
    controller.setBassEnhancerEnabled(false);
    controller.setBassEnhancerFrequencyHz(-99.0);
    controller.setBassEnhancerGainDb(99.0);
    controller.setBassEnhancerHarmonicsPercent(-1.0);
    controller.setBassEnhancerWidth(99.0);
    controller.setBassEnhancerMixPercent(999.0);
    if (controller.bassEnhancerFrequencyHz() != 20.0 || controller.bassEnhancerGainDb() != 18.0
        || controller.bassEnhancerHarmonicsPercent() != 0.0 || controller.bassEnhancerWidth() != 3.0
        || controller.bassEnhancerMixPercent() != 100.0 || controller.bassEnhancerDeep()) return 1;
    controller.setBassEnhancerFrequencyHz(std::numeric_limits<double>::quiet_NaN());
    controller.setBassEnhancerGainDb(std::numeric_limits<double>::infinity());
    controller.setBassEnhancerHarmonicsPercent(std::numeric_limits<double>::quiet_NaN());
    controller.setBassEnhancerWidth(-std::numeric_limits<double>::infinity());
    controller.setBassEnhancerMixPercent(std::numeric_limits<double>::quiet_NaN());
    if (controller.bassEnhancerFrequencyHz() != 80.0 || controller.bassEnhancerGainDb() != 6.0
        || controller.bassEnhancerHarmonicsPercent() != 50.0 || controller.bassEnhancerWidth() != 1.5
        || controller.bassEnhancerMixPercent() != 50.0) return 1;

    const bool globalBefore = controller.enabled();
    const double preampBefore = controller.preampDb();
    const double eqBefore = controller.equalizerBand(9);
    const bool reverbBefore = controller.reverbEnabled();
    const double reverbRoomBefore = controller.reverbRoomSizeMs();
    const bool compressorBefore = controller.compressorEnabled();
    const double compressorThresholdBefore = controller.compressorThresholdDb();
    const bool limiterBefore = controller.limiterEnabled();
    const double limiterCeilingBefore = controller.limiterCeilingDb();
    controller.applyBassEnhancerDeep();
    if (controller.bassEnhancerEnabled() || !controller.bassEnhancerDeep()
        || controller.bassEnhancerFrequencyHz() != 68.0 || controller.bassEnhancerGainDb() != 17.5
        || controller.bassEnhancerHarmonicsPercent() != 14.0 || controller.bassEnhancerWidth() != 1.2
        || controller.bassEnhancerMixPercent() != 82.0) return 1;
    controller.setBassEnhancerMixPercent(70.0);
    if (controller.bassEnhancerDeep() || controller.bassEnhancerEnabled()) return 1;
    controller.applyBassEnhancerDeep();
    controller.setBassEnhancerEnabled(true);
    controller.setBassEnhancerEnabled(false);
    if (controller.bassEnhancerEnabled() || !controller.bassEnhancerDeep()
        || controller.bassEnhancerGainDb() != 17.5 || controller.bassEnhancerMixPercent() != 82.0) return 1;
    controller.setBassEnhancerEnabled(true);
    controller.setEnabled(false);
    controller.resetBassEnhancer();
    if (!controller.bassEnhancerEnabled() || controller.enabled() || controller.bassEnhancerDeep()
        || controller.bassEnhancerFrequencyHz() != 80.0 || controller.bassEnhancerGainDb() != 6.0
        || controller.bassEnhancerHarmonicsPercent() != 50.0 || controller.bassEnhancerWidth() != 1.5
        || controller.bassEnhancerMixPercent() != 50.0
        || preampBefore != controller.preampDb() || eqBefore != controller.equalizerBand(9)
        || reverbBefore != controller.reverbEnabled() || reverbRoomBefore != controller.reverbRoomSizeMs()
        || compressorBefore != controller.compressorEnabled() || compressorThresholdBefore != controller.compressorThresholdDb()
        || limiterBefore != controller.limiterEnabled() || limiterCeilingBefore != controller.limiterCeilingDb()) return 1;
    Q_UNUSED(globalBefore);
    controller.applyBassEnhancerDeep();
  }
  {
    WebAudioEffectsController restored;
    if (restored.enabled() || !restored.bassEnhancerEnabled() || !restored.bassEnhancerDeep()
        || restored.bassEnhancerFrequencyHz() != 68.0 || restored.bassEnhancerGainDb() != 17.5
        || restored.bassEnhancerHarmonicsPercent() != 14.0 || restored.bassEnhancerWidth() != 1.2
        || restored.bassEnhancerMixPercent() != 82.0) return 1;
  }
  {
    // Stage 7 keeps its module switch independent. Presets, manual knob
    // changes, global bypass and reset must never silently own that switch.
    struct Preset { const char *id; double target; double maxGain; const char *speed; };
    const Preset presets[] = {
        {"balanced", -15.0, 10.0, "medium"},
        {"night", -20.0, 16.0, "slow"},
        {"loud", -12.0, 8.0, "fast"},
        {"speech", -14.0, 14.0, "medium"},
    };
    WebAudioEffectsController controller;
    controller.setAutoGainEnabled(false);
    for (const Preset &preset : presets) {
      controller.applyAutoGainPreset(QLatin1String(preset.id));
      if (controller.autoGainEnabled() || controller.autoGainPreset() != QLatin1String(preset.id)
          || controller.autoGainTargetDbfs() != preset.target || controller.autoGainMaxGainDb() != preset.maxGain
          || controller.autoGainSpeed() != QLatin1String(preset.speed)) return 1;
    }
    controller.setAutoGainTargetDbfs(-999.0);
    controller.setAutoGainMaxGainDb(999.0);
    if (controller.autoGainTargetDbfs() != -24.0 || controller.autoGainMaxGainDb() != 24.0) return 1;
    controller.setAutoGainTargetDbfs(999.0);
    controller.setAutoGainMaxGainDb(-999.0);
    if (controller.autoGainTargetDbfs() != 0.0 || controller.autoGainMaxGainDb() != 0.0) return 1;
    controller.setAutoGainTargetDbfs(std::numeric_limits<double>::quiet_NaN());
    controller.setAutoGainMaxGainDb(std::numeric_limits<double>::infinity());
    if (controller.autoGainTargetDbfs() != -14.0 || controller.autoGainMaxGainDb() != 12.0) return 1;

    controller.applyAutoGainPreset(QStringLiteral("night"));
    controller.setAutoGainTargetDbfs(-19.0);
    controller.setAutoGainMaxGainDb(15.0);
    if (controller.autoGainPreset() != QStringLiteral("night") || controller.autoGainSpeed() != QStringLiteral("slow")) return 1;
    controller.setAutoGainEnabled(true);
    controller.setAutoGainEnabled(false);
    if (controller.autoGainEnabled() || controller.autoGainTargetDbfs() != -19.0
        || controller.autoGainMaxGainDb() != 15.0 || controller.autoGainPreset() != QStringLiteral("night")) return 1;
    controller.setAutoGainEnabled(true);

    controller.setEnabled(false);
    controller.setPreampDb(4.0);
    controller.setEqualizerBand(11, -5.0);
    controller.setReverbEnabled(true);
    controller.setReverbRoomSizeMs(1900.0);
    controller.setCompressorEnabled(true);
    controller.applyCompressorPreset(QStringLiteral("punch"));
    controller.setLimiterEnabled(true);
    controller.applyLimiterPreset(QStringLiteral("safe"));
    controller.setBassEnhancerEnabled(true);
    controller.applyBassEnhancerDeep();
    controller.setModuleEnabled(QStringLiteral("truepeak"), true);
    controller.resetAutoGain();
    if (!controller.autoGainEnabled() || controller.enabled() || controller.autoGainTargetDbfs() != -14.0
        || controller.autoGainMaxGainDb() != 12.0 || controller.autoGainSpeed() != QStringLiteral("medium")
        || controller.autoGainPreset() != QStringLiteral("balanced") || controller.preampDb() != 4.0
        || controller.equalizerBand(11) != -5.0 || !controller.reverbEnabled()
        || controller.reverbRoomSizeMs() != 1900.0 || !controller.compressorEnabled()
        || controller.compressorPreset() != QStringLiteral("punch") || !controller.limiterEnabled()
        || controller.limiterPreset() != QStringLiteral("safe") || !controller.bassEnhancerEnabled()
        || !controller.bassEnhancerDeep() || !controller.moduleEnabled(QStringLiteral("truepeak"))) return 1;
    controller.applyAutoGainPreset(QStringLiteral("speech"));
    controller.setAutoGainTargetDbfs(-13.0);
    controller.setAutoGainMaxGainDb(13.0);
    controller.resetBassEnhancer();
    if (!controller.autoGainEnabled() || controller.autoGainPreset() != QStringLiteral("speech")
        || controller.autoGainTargetDbfs() != -13.0 || controller.autoGainMaxGainDb() != 13.0
        || controller.autoGainSpeed() != QStringLiteral("medium")) return 1;
  }
  {
    WebAudioEffectsController restored;
    if (restored.enabled() || !restored.autoGainEnabled() || restored.autoGainTargetDbfs() != -13.0
        || restored.autoGainMaxGainDb() != 13.0 || restored.autoGainSpeed() != QStringLiteral("medium")
        || restored.autoGainPreset() != QStringLiteral("speech") || restored.preampDb() != 4.0
        || restored.equalizerBand(11) != -5.0 || !restored.reverbEnabled()
        || restored.reverbRoomSizeMs() != 1900.0 || !restored.compressorEnabled()
        || restored.compressorPreset() != QStringLiteral("punch") || !restored.limiterEnabled()
        || restored.limiterPreset() != QStringLiteral("safe") || !restored.bassEnhancerEnabled()
        || restored.bassEnhancerDeep() || !restored.moduleEnabled(QStringLiteral("truepeak"))) return 1;
  }
  {
    const QString generatedPath = QCoreApplication::applicationDirPath()
        + QStringLiteral("/dali/web-dynamic-compressor.generated.js");
    QFile generated(generatedPath);
    if (!generated.open(QIODevice::ReadOnly | QIODevice::Text)) return 1;
    const QByteArray source = generated.readAll();
    if (!source.contains("DynamicsCompressorNode") || !source.contains("GainNode")
        || !source.contains("Web Dynamic Compressor v1")) return 1;
    QFile limiter(QCoreApplication::applicationDirPath() + QStringLiteral("/dali/web-limiter.generated.js"));
    if (!limiter.open(QIODevice::ReadOnly | QIODevice::Text)) return 1;
    const QByteArray limiterSource = limiter.readAll();
    if (!limiterSource.contains("DynamicsCompressorNode") || !limiterSource.contains("Web Limiter v1")) return 1;
    QFile bassEnhancer(QCoreApplication::applicationDirPath() + QStringLiteral("/dali/web-bass-enhancer.generated.js"));
    if (!bassEnhancer.open(QIODevice::ReadOnly | QIODevice::Text)) return 1;
    const QByteArray bassEnhancerSource = bassEnhancer.readAll();
    if (!bassEnhancerSource.contains("Web Bass Enhancer Native Match v1")
        || !bassEnhancerSource.contains("BiquadFilterNode") || !bassEnhancerSource.contains("lowshelf")
        || !bassEnhancerSource.contains("peaking")) return 1;
    QFile autoGain(QCoreApplication::applicationDirPath() + QStringLiteral("/dali/web-auto-gain.generated.js"));
    if (!autoGain.open(QIODevice::ReadOnly | QIODevice::Text)) return 1;
    const QByteArray autoGainSource = autoGain.readAll();
    if (!autoGainSource.contains("Web Auto Gain Normalize v1") || !autoGainSource.contains("GainNode")) return 1;
  }
  {
    WebAudioEffectsController controller;
    controller.setEnabled(true);
    controller.setReverbEnabled(false);
    controller.setCompressorEnabled(false);
    controller.setLimiterEnabled(false);
    controller.setBassEnhancerEnabled(false);
    controller.resetBassEnhancer();
    controller.setAutoGainEnabled(false);
    controller.resetAutoGain();
    AudioEffectsPage page(&controller);
    auto *navigation = page.findChild<QListWidget *>(QStringLiteral("audio-effects-navigation"));
    auto *stack = page.findChild<QStackedWidget *>(QStringLiteral("audio-effects-module-pages"));
    auto *reverbToggle = page.findChild<QCheckBox *>(QStringLiteral("audio-effects-reverb-toggle"));
    auto *compressorToggle = page.findChild<QCheckBox *>(QStringLiteral("audio-effects-compressor-toggle"));
    auto *compressorMeter = page.findChild<QProgressBar *>(QStringLiteral("audio-effects-compressor-meter"));
    auto *limiterToggle = page.findChild<QCheckBox *>(QStringLiteral("audio-effects-limiter-toggle"));
    auto *limiterMeter = page.findChild<QProgressBar *>(QStringLiteral("audio-effects-limiter-meter"));
    auto *bassEnhancerToggle = page.findChild<QCheckBox *>(QStringLiteral("audio-effects-bass-enhancer-toggle"));
    auto *deep = page.findChild<QPushButton *>(QStringLiteral("audio-effects-bass-enhancer-deep"));
    auto *autoGainToggle = page.findChild<QCheckBox *>(QStringLiteral("audio-effects-auto-gain-toggle"));
    if (!navigation || !stack || !reverbToggle || !compressorToggle || !compressorMeter || !limiterToggle || !limiterMeter
        || !bassEnhancerToggle || !deep || !autoGainToggle
        || reverbToggle->text() != QStringLiteral("Etkinleştir")
        || reverbToggle->parentWidget()->objectName() != QStringLiteral("audio-effects-reverb-header")
        || compressorToggle->text() != QStringLiteral("Etkinleştir")
        || compressorToggle->parentWidget()->objectName() != QStringLiteral("audio-effects-compressor-header")
        || limiterToggle->text() != QStringLiteral("Etkinleştir")
        || limiterToggle->parentWidget()->objectName() != QStringLiteral("audio-effects-limiter-header")
        || bassEnhancerToggle->text() != QStringLiteral("Etkinleştir")
        || bassEnhancerToggle->parentWidget()->objectName() != QStringLiteral("audio-effects-bass-enhancer-header")
        || autoGainToggle->text() != QStringLiteral("Etkinleştir")
        || autoGainToggle->parentWidget()->objectName() != QStringLiteral("audio-effects-auto-gain-header")
        || stack->count() != 22 || navigation->count() != 22) return 1;
    if (stack->widget(0)->findChild<QCheckBox *>() || stack->widget(1)->findChild<QCheckBox *>()) return 1;
    if (stack->widget(2)->findChild<QCheckBox *>(QStringLiteral("audio-effects-reverb-toggle")) != reverbToggle) return 1;
    if (stack->widget(3)->findChild<QCheckBox *>(QStringLiteral("audio-effects-compressor-toggle")) != compressorToggle) return 1;
    if (stack->widget(4)->findChild<QCheckBox *>(QStringLiteral("audio-effects-limiter-toggle")) != limiterToggle) return 1;
    if (stack->widget(5)->findChild<QCheckBox *>(QStringLiteral("audio-effects-bass-enhancer-toggle")) != bassEnhancerToggle) return 1;
    if (stack->widget(6)->findChild<QCheckBox *>(QStringLiteral("audio-effects-auto-gain-toggle")) != autoGainToggle) return 1;
    if (!stack->widget(7)->findChild<QCheckBox *>(QStringLiteral("audio-effects-module-toggle"))
        || stack->widget(7)->findChild<QDial *>(QStringLiteral("audio-effects-auto-gain-dial"))) return 1;
    const QList<QCheckBox *> moduleToggles = page.findChildren<QCheckBox *>(QStringLiteral("audio-effects-module-toggle"));
    const QList<QDial *> compressorDials = page.findChildren<QDial *>(QStringLiteral("audio-effects-compressor-dial"));
    const QList<QPushButton *> compressorPresets = page.findChildren<QPushButton *>(QStringLiteral("audio-effects-compressor-preset"));
    const QList<QDial *> limiterDials = page.findChildren<QDial *>(QStringLiteral("audio-effects-limiter-dial"));
    const QList<QPushButton *> limiterPresets = page.findChildren<QPushButton *>(QStringLiteral("audio-effects-limiter-preset"));
    const QList<QDial *> bassEnhancerDials = page.findChildren<QDial *>(QStringLiteral("audio-effects-bass-enhancer-dial"));
    const QList<QDial *> autoGainDials = page.findChildren<QDial *>(QStringLiteral("audio-effects-auto-gain-dial"));
    const QList<QPushButton *> autoGainPresets = page.findChildren<QPushButton *>(QStringLiteral("audio-effects-auto-gain-preset"));
    if (moduleToggles.size() != 15 || compressorDials.size() != 6 || compressorPresets.size() != 5
        || limiterDials.size() != 4 || limiterPresets.size() != 5
        || bassEnhancerDials.size() != 5 || autoGainDials.size() != 2 || autoGainPresets.size() != 4
        || compressorMeter->minimum() != 0 || compressorMeter->maximum() != 240) return 1;
    QDial *threshold = nullptr;
    QDial *ratio = nullptr;
    QDial *attack = nullptr;
    QDial *release = nullptr;
    QDial *makeup = nullptr;
    QDial *knee = nullptr;
    for (QDial *dial : compressorDials) {
      if (dial->accessibleName() == QStringLiteral("Threshold")) threshold = dial;
      else if (dial->accessibleName() == QStringLiteral("Ratio")) ratio = dial;
      else if (dial->accessibleName() == QStringLiteral("Attack")) attack = dial;
      else if (dial->accessibleName() == QStringLiteral("Release")) release = dial;
      else if (dial->accessibleName() == QStringLiteral("Makeup Gain")) makeup = dial;
      else if (dial->accessibleName() == QStringLiteral("Knee")) knee = dial;
    }
    if (!threshold || !ratio || !attack || !release || !makeup || !knee
        || threshold->minimum() != -600 || threshold->maximum() != 0 || threshold->singleStep() != 10
        || ratio->minimum() != 10 || ratio->maximum() != 200 || ratio->singleStep() != 10
        || attack->minimum() != 1 || attack->maximum() != 1000 || attack->singleStep() != 10
        || release->minimum() != 100 || release->maximum() != 10000 || release->singleStep() != 10
        || makeup->minimum() != -120 || makeup->maximum() != 240 || makeup->singleStep() != 10
        || knee->minimum() != 0 || knee->maximum() != 100 || knee->singleStep() != 10) return 1;
    QDial *ceiling = nullptr;
    QDial *limiterRelease = nullptr;
    QDial *lookahead = nullptr;
    QDial *gain = nullptr;
    for (QDial *dial : limiterDials) {
      if (dial->accessibleName() == QStringLiteral("Ceiling")) ceiling = dial;
      else if (dial->accessibleName() == QStringLiteral("Release")) limiterRelease = dial;
      else if (dial->accessibleName() == QStringLiteral("Lookahead")) lookahead = dial;
      else if (dial->accessibleName() == QStringLiteral("Gain")) gain = dial;
    }
    if (!ceiling || !limiterRelease || !lookahead || !gain
        || ceiling->minimum() != -120 || ceiling->maximum() != 0 || ceiling->singleStep() != 1
        || limiterRelease->minimum() != 10 || limiterRelease->maximum() != 500 || limiterRelease->singleStep() != 1
        || lookahead->minimum() != 0 || lookahead->maximum() != 20 || lookahead->singleStep() != 1
        || gain->minimum() != -12 || gain->maximum() != 12 || gain->singleStep() != 1
        || limiterMeter->minimum() != 0 || limiterMeter->maximum() != 200) return 1;
    QDial *bassFrequency = nullptr;
    QDial *bassGain = nullptr;
    QDial *bassHarmonics = nullptr;
    QDial *bassWidth = nullptr;
    QDial *bassMix = nullptr;
    for (QDial *dial : bassEnhancerDials) {
      if (dial->accessibleName() == QStringLiteral("Frequency")) bassFrequency = dial;
      else if (dial->accessibleName() == QStringLiteral("Gain")) bassGain = dial;
      else if (dial->accessibleName() == QStringLiteral("Harmonics")) bassHarmonics = dial;
      else if (dial->accessibleName() == QStringLiteral("Width")) bassWidth = dial;
      else if (dial->accessibleName() == QStringLiteral("Mix")) bassMix = dial;
    }
    if (!bassFrequency || !bassGain || !bassHarmonics || !bassWidth || !bassMix
        || bassFrequency->minimum() != 200 || bassFrequency->maximum() != 1200 || bassFrequency->singleStep() != 10
        || bassGain->minimum() != 0 || bassGain->maximum() != 180 || bassGain->singleStep() != 10
        || bassHarmonics->minimum() != 0 || bassHarmonics->maximum() != 1000 || bassHarmonics->singleStep() != 10
        || bassWidth->minimum() != 5 || bassWidth->maximum() != 30 || bassWidth->singleStep() != 1
        || bassMix->minimum() != 0 || bassMix->maximum() != 1000 || bassMix->singleStep() != 10) return 1;
    QDial *autoTarget = nullptr;
    QDial *autoMaxGain = nullptr;
    for (QDial *dial : autoGainDials) {
      if (dial->accessibleName() == QStringLiteral("Target Level")) autoTarget = dial;
      else if (dial->accessibleName() == QStringLiteral("Max Gain")) autoMaxGain = dial;
    }
    if (!autoTarget || !autoMaxGain || autoTarget->minimum() != -240 || autoTarget->maximum() != 0
        || autoTarget->singleStep() != 10 || autoMaxGain->minimum() != 0 || autoMaxGain->maximum() != 240
        || autoMaxGain->singleStep() != 10) return 1;
    navigation->setCurrentRow(2);
    if (stack->currentIndex() != 2 || reverbToggle->isChecked()) return 1;
    reverbToggle->click();
    if (!controller.reverbEnabled() || !reverbToggle->isChecked()) return 1;
    navigation->setCurrentRow(3);
    navigation->setCurrentRow(2);
    if (stack->currentIndex() != 2 || !controller.reverbEnabled() || !reverbToggle->isChecked()) return 1;
    QPushButton *cathedral = nullptr;
    for (QPushButton *button : page.findChildren<QPushButton *>(QStringLiteral("audio-effects-reverb-preset"))) {
      if (button->property("presetId").toString() == QStringLiteral("cathedral")) cathedral = button;
    }
    QPushButton *reset = nullptr;
    for (QPushButton *button : page.findChildren<QPushButton *>(QStringLiteral("audio-effects-reset"))) {
      if (button->accessibleName() == QStringLiteral("Reverb Sıfırla")) reset = button;
    }
    if (!cathedral || !reset) return 1;
    cathedral->click();
    if (!controller.reverbEnabled() || controller.reverbPreset() != QStringLiteral("cathedral")) return 1;
    reset->click();
    if (!controller.reverbEnabled() || !controller.reverbPreset().isEmpty() || controller.reverbRoomSizeMs() != 1000.0) return 1;

    navigation->setCurrentRow(3);
    QPushButton *vocal = nullptr;
    QPushButton *compressorReset = nullptr;
    for (QPushButton *button : compressorPresets) {
      if (button->property("presetId").toString() == QStringLiteral("vocal")) vocal = button;
    }
    for (QPushButton *button : page.findChildren<QPushButton *>(QStringLiteral("audio-effects-reset"))) {
      if (button->accessibleName() == QStringLiteral("Dinamik Kompresör Sıfırla")) compressorReset = button;
    }
    if (!vocal || !compressorReset || compressorToggle->isChecked()) return 1;
    vocal->click();
    if (controller.compressorEnabled() || controller.compressorPreset() != QStringLiteral("vocal")) return 1;
    compressorToggle->click();
    if (!controller.compressorEnabled() || !compressorToggle->isChecked()) return 1;
    compressorReset->click();
    if (!controller.compressorEnabled() || controller.compressorThresholdDb() != -20.0
        || !controller.compressorPreset().isEmpty()) return 1;
    navigation->setCurrentRow(2);
    navigation->setCurrentRow(1);
    navigation->setCurrentRow(3);
    if (!controller.compressorEnabled() || !compressorToggle->isChecked() || stack->currentIndex() != 3) return 1;

    navigation->setCurrentRow(4);
    QPushButton *streaming = nullptr;
    QPushButton *limiterReset = nullptr;
    for (QPushButton *button : limiterPresets) {
      if (button->property("presetId").toString() == QStringLiteral("streaming")) streaming = button;
    }
    for (QPushButton *button : page.findChildren<QPushButton *>(QStringLiteral("audio-effects-reset"))) {
      if (button->accessibleName() == QStringLiteral("Limiter Sıfırla")) limiterReset = button;
    }
    if (!streaming || !limiterReset || limiterToggle->isChecked()) return 1;
    streaming->click();
    if (controller.limiterEnabled() || controller.limiterPreset() != QStringLiteral("streaming")
        || controller.limiterCeilingDb() != -1.0 || controller.limiterLookaheadMs() != 6.0) return 1;
    limiterToggle->click();
    if (!controller.limiterEnabled() || !limiterToggle->isChecked() || !controller.compressorEnabled()) return 1;
    limiterReset->click();
    if (!controller.limiterEnabled() || controller.limiterCeilingDb() != -0.3
        || controller.limiterReleaseMs() != 50.0 || controller.limiterLookaheadMs() != 5.0
        || controller.limiterGainDb() != 0.0 || !controller.limiterPreset().isEmpty()
        || !controller.compressorEnabled() || !controller.reverbEnabled()) return 1;
    navigation->setCurrentRow(3);
    navigation->setCurrentRow(2);
    navigation->setCurrentRow(1);
    navigation->setCurrentRow(4);
    if (stack->currentIndex() != 4 || !controller.limiterEnabled() || !limiterToggle->isChecked()
        || controller.limiterCeilingDb() != -0.3) return 1;

    navigation->setCurrentRow(5);
    if (stack->currentIndex() != 5 || bassEnhancerToggle->isChecked() || controller.bassEnhancerEnabled()) return 1;
    deep->click();
    if (controller.bassEnhancerEnabled() || !controller.bassEnhancerDeep()
        || controller.bassEnhancerFrequencyHz() != 68.0 || controller.bassEnhancerGainDb() != 17.5
        || controller.bassEnhancerMixPercent() != 82.0) return 1;
    bassEnhancerToggle->click();
    if (!controller.bassEnhancerEnabled() || !bassEnhancerToggle->isChecked()
        || !controller.limiterEnabled() || !controller.compressorEnabled() || !controller.reverbEnabled()) return 1;
    QPushButton *bassReset = nullptr;
    for (QPushButton *button : page.findChildren<QPushButton *>(QStringLiteral("audio-effects-reset"))) {
      if (button->accessibleName() == QStringLiteral("Bas Güçlendirici Sıfırla")) bassReset = button;
    }
    if (!bassReset) return 1;
    bassReset->click();
    if (!controller.bassEnhancerEnabled() || controller.bassEnhancerDeep()
        || controller.bassEnhancerFrequencyHz() != 80.0 || controller.bassEnhancerGainDb() != 6.0
        || controller.bassEnhancerHarmonicsPercent() != 50.0 || controller.bassEnhancerWidth() != 1.5
        || controller.bassEnhancerMixPercent() != 50.0
        || !controller.limiterEnabled() || !controller.compressorEnabled() || !controller.reverbEnabled()) return 1;
    navigation->setCurrentRow(4);
    navigation->setCurrentRow(3);
    navigation->setCurrentRow(2);
    navigation->setCurrentRow(1);
    navigation->setCurrentRow(5);
    if (stack->currentIndex() != 5 || !controller.bassEnhancerEnabled() || !bassEnhancerToggle->isChecked()) return 1;

    navigation->setCurrentRow(6);
    if (stack->currentIndex() != 6 || autoGainToggle->isChecked() || controller.autoGainEnabled()) return 1;
    QPushButton *night = nullptr;
    QPushButton *autoGainReset = nullptr;
    for (QPushButton *button : autoGainPresets) {
      if (button->property("presetId").toString() == QStringLiteral("night")) night = button;
    }
    for (QPushButton *button : page.findChildren<QPushButton *>(QStringLiteral("audio-effects-reset"))) {
      if (button->accessibleName() == QStringLiteral("Auto Gain / Normalize Sıfırla")) autoGainReset = button;
    }
    if (!night || !autoGainReset) return 1;
    night->click();
    if (controller.autoGainEnabled() || controller.autoGainPreset() != QStringLiteral("night")
        || controller.autoGainTargetDbfs() != -20.0 || controller.autoGainMaxGainDb() != 16.0
        || controller.autoGainSpeed() != QStringLiteral("slow")) return 1;
    autoTarget->setValue(-190);
    if (controller.autoGainTargetDbfs() != -19.0 || controller.autoGainPreset() != QStringLiteral("night")
        || controller.autoGainSpeed() != QStringLiteral("slow")) return 1;
    autoGainToggle->click();
    if (!controller.autoGainEnabled() || !autoGainToggle->isChecked()) return 1;
    autoGainReset->click();
    if (!controller.autoGainEnabled() || controller.autoGainTargetDbfs() != -14.0
        || controller.autoGainMaxGainDb() != 12.0 || controller.autoGainPreset() != QStringLiteral("balanced")
        || controller.autoGainSpeed() != QStringLiteral("medium") || !controller.bassEnhancerEnabled()
        || !controller.limiterEnabled() || !controller.compressorEnabled() || !controller.reverbEnabled()) return 1;
    navigation->setCurrentRow(5);
    navigation->setCurrentRow(4);
    navigation->setCurrentRow(3);
    navigation->setCurrentRow(6);
    if (stack->currentIndex() != 6 || !controller.autoGainEnabled() || !autoGainToggle->isChecked()
        || controller.autoGainTargetDbfs() != -14.0 || controller.autoGainMaxGainDb() != 12.0
        || controller.autoGainPreset() != QStringLiteral("balanced")) return 1;
  }
  {
    // EQ preset browsing is transactional: live preview cannot leak into the
    // persisted 32-band state, while commit must survive reconstruction.
    WebAudioEffectsController controller;
    QVector<double> original(32, 0.0);
    original[0] = 2.0; original[17] = -3.5; original[31] = 4.0;
    controller.commitEqualizerBands(original);
    QVector<double> candidate(32, 0.0);
    candidate[0] = -24.0; candidate[17] = 6.25; candidate[31] = 24.0;
    controller.previewEqualizerBands(candidate);
    if (controller.equalizerBand(0) != -12.0 || controller.equalizerBand(17) != 6.25 || controller.equalizerBand(31) != 12.0) return 1;
    QSettings settings;
    if (settings.value(QStringLiteral("audioEffects/web/equalizer/band0")).toDouble() != 2.0
        || settings.value(QStringLiteral("audioEffects/web/equalizer/band17")).toDouble() != -3.5
        || settings.value(QStringLiteral("audioEffects/web/equalizer/band31")).toDouble() != 4.0) return 1;
    controller.previewEqualizerBands(original);
    controller.commitEqualizerBands(candidate);
  }
  {
    // The page owns the preview transaction.  A tab close rolls the entire
    // manual curve back, while Tamam converts the preview into persistence.
    WebAudioEffectsController controller;
    QVector<double> manual(32, 0.0);
    manual[2] = 1.75; manual[19] = -4.0; manual[28] = 3.25;
    controller.commitEqualizerBands(manual);
    {
      EqPresetPage page(&controller);
      if (page.repositoryForView().presets().size() != 1776) return 1;
      page.resize(1920, 950);
      page.show();
      app.processEvents();
      const QString screenshotPath = qEnvironmentVariable("DALINIRA_EQ_PRESET_SCREENSHOT_PATH");
      if (!screenshotPath.isEmpty() && !page.grab().save(screenshotPath)) return 1;
      QListView *list = page.findChild<QListView *>(QStringLiteral("eq-preset-list"));
      if (!list || !clickListIndex(list, list->model()->index(1, 0))) return 1;
      if (controller.equalizerBands() != page.repositoryForView().presets().at(1).bands) return 1;
    }
    if (controller.equalizerBands() != manual) return 1;
  }
  {
    WebAudioEffectsController restored;
    if (restored.equalizerBand(2) != 1.75 || restored.equalizerBand(19) != -4.0 || restored.equalizerBand(28) != 3.25) return 1;
  }
  {
    WebAudioEffectsController controller;
    EqPresetPage page(&controller);
    page.resize(1280, 800);
    page.show();
    app.processEvents();
    QListView *list = page.findChild<QListView *>(QStringLiteral("eq-preset-list"));
    QPushButton *ok = page.findChild<QPushButton *>(QStringLiteral("eq-preset-ok"));
    if (!list || !ok || !clickListIndex(list, list->model()->index(2, 0))) return 1;
    const QVector<double> expected = page.repositoryForView().presets().at(2).bands;
    ok->click();
    if (controller.equalizerBands() != expected) return 1;
    AudioEffectsPage equalizerPage(&controller);
    QVector<QSlider *> sliders;
    for (QSlider *slider : equalizerPage.findChildren<QSlider *>()) {
      if (slider->objectName() == QStringLiteral("audio-effects-eq-slider")) sliders.append(slider);
    }
    if (sliders.size() != 32) return 1;
    for (int index = 0; index < sliders.size(); ++index) {
      if (sliders[index]->value() != qRound(expected[index] * 10.0)) return 1;
    }
  }
  {
    WebAudioEffectsController restored;
    EqPresetRepository repository;
    if (!repository.load() || restored.equalizerBands() != repository.presets().at(2).bands) return 1;
  }
  {
    // Closing a tab whose effect script is still pending (always the case for
    // a discarded page) must not crash: QtWebEngine answers the script with
    // an invalid result from inside the view's destructor, as BrowserWindow's
    // pages are children of their view.
    WebAudioEffectsController controller;
    controller.setEnabled(true);
    QTemporaryDir site;
    QFile page(site.filePath(QStringLiteral("media.html")));
    if (!page.open(QIODevice::WriteOnly)) return 1;
    page.write("<html><body><audio></audio></body></html>");
    page.close();
    for (int round = 0; round < 3; ++round) {
      auto *view = new QWebEngineView;
      view->setPage(new QWebEnginePage(view));
      view->setProperty("dalinira-trusted-local-media", true);
      bool loaded = false;
      QObject::connect(view, &QWebEngineView::loadFinished, [&loaded](bool) { loaded = true; });
      view->load(QUrl::fromLocalFile(page.fileName()));
      controller.registerWebView(view, QUrl::fromLocalFile(page.fileName()));
      QElapsedTimer wait;
      wait.start();
      while (!loaded && wait.elapsed() < 15000) QApplication::processEvents(QEventLoop::AllEvents, 20);
      if (!loaded) return 1;
      // A hidden, discarded page (what the memory saver produces) never runs
      // scripts; their callbacks stay pending until the page is destroyed.
      view->page()->setLifecycleState(QWebEnginePage::LifecycleState::Discarded);
      QApplication::processEvents();
      controller.applyToView(view);
      delete view;
      QApplication::processEvents();
    }
    if (controller.audioEnabledWebViewCount() != 0) return 1;
  }
  return 0;
}
