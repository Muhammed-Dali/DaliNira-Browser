#include "web_audio_effects_controller.h"

#include "audio_platform_policy.h"
#include "engine/engine_view.h"

#include <QFile>
#include <QDir>
#include <QCoreApplication>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QSharedPointer>
#include <QStringDecoder>
#include <QStringList>
#include <QWebEnginePage>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QStandardPaths>
#include <QWebEngineView>

#include <algorithm>
#include <array>
#include <cmath>
#include "i18n/i18n.h"

namespace {
// Any tab page (QWebEngineView or EngineView); the local player marks itself.
bool isAudioEligibleView(const QObject *view, const QUrl &url) {
  if (dalinira::audio::isSupportedAudioPlatform(url)) return true;
  if (!url.isLocalFile()) return false;
  if (view && view->property("dalinira-trusted-local-media").toBool()) return true;
  const QString path = url.toLocalFile();
  const QString playerDir = QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
                                .filePath(QStringLiteral("media-player"));
  if (!playerDir.isEmpty() && path.startsWith(playerDir)) return true;
  return false;
}

constexpr double kMinPreampDb = -24.0;
constexpr double kMaxPreampDb = 24.0;
constexpr double kMinEqDb = -12.0;
constexpr double kMaxEqDb = 12.0;
constexpr double kMinBalance = -100.0;
constexpr double kMaxBalance = 100.0;
constexpr double kMinStereoExpanderPercent = 0.0;
constexpr double kMaxStereoExpanderPercent = 200.0;
constexpr double kMinReverbRoomSizeMs = 0.0;
constexpr double kMaxReverbRoomSizeMs = 3000.0;
constexpr double kMinReverbDamping = 0.0;
constexpr double kMaxReverbDamping = 1.0;
constexpr double kMinReverbWetDryDb = -96.0;
constexpr double kMaxReverbWetDryDb = 0.0;
constexpr double kMinReverbHfRatio = 0.001;
constexpr double kMaxReverbHfRatio = 0.999;
constexpr double kMinReverbInputGainDb = -96.0;
constexpr double kMaxReverbInputGainDb = 12.0;
constexpr double kMinCompressorThresholdDb = -60.0;
constexpr double kMaxCompressorThresholdDb = 0.0;
constexpr double kMinCompressorRatio = 1.0;
constexpr double kMaxCompressorRatio = 20.0;
constexpr double kMinCompressorAttackMs = 0.1;
constexpr double kMaxCompressorAttackMs = 100.0;
constexpr double kMinCompressorReleaseMs = 10.0;
constexpr double kMaxCompressorReleaseMs = 1000.0;
constexpr double kMinCompressorMakeupDb = -12.0;
constexpr double kMaxCompressorMakeupDb = 24.0;
constexpr double kMinCompressorKneeDb = 0.0;
constexpr double kMaxCompressorKneeDb = 10.0;
constexpr double kMinLimiterCeilingDb = -12.0;
constexpr double kMaxLimiterCeilingDb = 0.0;
constexpr double kMinLimiterReleaseMs = 10.0;
constexpr double kMaxLimiterReleaseMs = 500.0;
constexpr double kMinLimiterLookaheadMs = 0.0;
constexpr double kMaxLimiterLookaheadMs = 20.0;
constexpr double kMinLimiterGainDb = -12.0;
constexpr double kMaxLimiterGainDb = 12.0;
constexpr double kMinBassEnhancerFrequencyHz = 20.0;
constexpr double kMaxBassEnhancerFrequencyHz = 120.0;
constexpr double kMinBassEnhancerGainDb = 0.0;
constexpr double kMaxBassEnhancerGainDb = 18.0;
constexpr double kMinBassEnhancerHarmonicsPercent = 0.0;
constexpr double kMaxBassEnhancerHarmonicsPercent = 100.0;
constexpr double kMinBassEnhancerWidth = 0.5;
constexpr double kMaxBassEnhancerWidth = 3.0;
constexpr double kMinBassEnhancerMixPercent = 0.0;
constexpr double kMaxBassEnhancerMixPercent = 100.0;
constexpr double kMinAutoGainTargetDbfs = -24.0;
constexpr double kMaxAutoGainTargetDbfs = 0.0;
constexpr double kMinAutoGainMaxGainDb = 0.0;
constexpr double kMaxAutoGainMaxGainDb = 24.0;
// Updating an AudioParam is cheap; repeatedly rescheduling the entire 32-band
// graph while a dial is dragged is not.  This matches the old engine's live
// control cadence while still feeling immediate in the UI.
constexpr int kApplyCoalesceMs = 72;

constexpr auto kReverbModuleId = "reverb";
constexpr auto kCompressorModuleId = "compressor";
constexpr auto kLimiterModuleId = "limiter";
constexpr auto kBassEnhancerModuleId = "bassboost";
constexpr auto kAutoGainModuleId = "autogain";

double finiteClamped(double value, double minimum, double maximum, double fallback) {
  return std::clamp(std::isfinite(value) ? value : fallback, minimum, maximum);
}

struct ReverbPresetDefinition {
  const char *id;
  double roomSizeMs;
  double damping;
  double wetDryDb;
  double hfRatio;
  double inputGainDb;
};

constexpr std::array<ReverbPresetDefinition, 10> kReverbPresets = {{
    {"smallRoom", 500.0, 0.80, -15.0, 0.50, 0.0},
    {"largeRoom", 1500.0, 0.50, -10.0, 0.70, 0.0},
    {"concertHall", 2500.0, 0.30, -8.0, 0.80, 0.0},
    {"cathedral", 3000.0, 0.20, -6.0, 0.90, 0.0},
    {"studioPlate", 900.0, 0.62, -12.0, 0.75, 0.0},
    {"arena", 2800.0, 0.28, -7.0, 0.84, 1.0},
    {"vocalRoom", 720.0, 0.68, -13.0, 0.72, 0.5},
    {"ambientWash", 2200.0, 0.38, -9.0, 0.86, -0.5},
    {"slapback", 280.0, 0.78, -18.0, 0.55, 0.0},
    {"dreamVox", 1400.0, 0.46, -11.0, 0.88, 0.8},
}};

struct CompressorPresetDefinition {
  const char *id;
  double thresholdDb;
  double ratio;
  double attackMs;
  double releaseMs;
  double makeupDb;
  double kneeDb;
};

constexpr std::array<CompressorPresetDefinition, 5> kCompressorPresets = {{
    {"gentle", -18.0, 2.2, 18.0, 180.0, 1.5, 5.0},
    {"vocal", -24.0, 3.2, 8.0, 120.0, 3.0, 4.0},
    {"night", -30.0, 4.8, 12.0, 260.0, 2.0, 6.0},
    {"punch", -16.0, 5.5, 4.0, 90.0, 2.5, 2.0},
    {"broadcast", -22.0, 6.0, 3.0, 160.0, 4.0, 3.0},
}};

struct LimiterPresetDefinition {
  const char *id;
  double ceilingDb;
  double releaseMs;
  double lookaheadMs;
  double gainDb;
};

struct AutoGainPresetDefinition {
  const char *id;
  double targetDbfs;
  double maxGainDb;
  const char *speed;
};

constexpr std::array<LimiterPresetDefinition, 5> kLimiterPresets = {{
    {"transparent", -1.0, 180.0, 5.0, 0.0},
    {"loud", -0.5, 90.0, 7.0, 4.0},
    {"streaming", -1.0, 130.0, 6.0, 2.0},
    {"night", -2.0, 240.0, 8.0, -1.0},
    {"safe", -3.0, 180.0, 10.0, 0.0},
}};

constexpr std::array<AutoGainPresetDefinition, 4> kAutoGainPresets = {{
    {"balanced", -15.0, 10.0, "medium"},
    {"night", -20.0, 16.0, "slow"},
    {"loud", -12.0, 8.0, "fast"},
    {"speech", -14.0, 14.0, "medium"},
}};

const QStringList &moduleEnableIds() {
  static const QStringList ids{
      QStringLiteral("reverb"), QStringLiteral("compressor"), QStringLiteral("limiter"),
      QStringLiteral("bassboost"), QStringLiteral("autogain"), QStringLiteral("truepeak"),
      QStringLiteral("peq"), QStringLiteral("dynamiceq"), QStringLiteral("exciter"),
      QStringLiteral("deesser"), QStringLiteral("noisegate"), QStringLiteral("stereowidener"),
      QStringLiteral("echo"), QStringLiteral("softecho"), QStringLiteral("convreverb"),
      QStringLiteral("crossfeed"), QStringLiteral("surround"), QStringLiteral("bassmono"),
      QStringLiteral("tapesat"), QStringLiteral("bitdither"),
  };
  return ids;
}

const ReverbPresetDefinition *reverbPresetDefinition(const QString &presetId) {
  const QString normalized = presetId.trimmed();
  const auto found = std::find_if(kReverbPresets.cbegin(), kReverbPresets.cend(), [&normalized](const ReverbPresetDefinition &preset) {
    return normalized == QLatin1String(preset.id);
  });
  return found == kReverbPresets.cend() ? nullptr : &*found;
}

const CompressorPresetDefinition *compressorPresetDefinition(const QString &presetId) {
  const QString normalized = presetId.trimmed().toLower();
  const auto found = std::find_if(kCompressorPresets.cbegin(), kCompressorPresets.cend(),
                                  [&normalized](const CompressorPresetDefinition &preset) {
    return normalized == QLatin1String(preset.id);
  });
  return found == kCompressorPresets.cend() ? nullptr : &*found;
}

const LimiterPresetDefinition *limiterPresetDefinition(const QString &presetId) {
  const QString normalized = presetId.trimmed().toLower();
  const auto found = std::find_if(kLimiterPresets.cbegin(), kLimiterPresets.cend(),
                                  [&normalized](const LimiterPresetDefinition &preset) {
    return normalized == QLatin1String(preset.id);
  });
  return found == kLimiterPresets.cend() ? nullptr : &*found;
}

const AutoGainPresetDefinition *autoGainPresetDefinition(const QString &presetId) {
  const QString normalized = presetId.trimmed().toLower();
  const auto found = std::find_if(kAutoGainPresets.cbegin(), kAutoGainPresets.cend(),
                                  [&normalized](const AutoGainPresetDefinition &preset) {
    return normalized == QLatin1String(preset.id);
  });
  return found == kAutoGainPresets.cend() ? nullptr : &*found;
}

QString jsonLiteral(const QString &value) {
  return QString::fromUtf8(QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact)).mid(1).chopped(1);
}

QString jsonNumberArray(const QVector<double> &values) {
  QJsonArray array;
  for (const double value : values) array.append(value);
  return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
}

struct CompressorMeterAggregation {
  int remaining = 0;
  double reductionDb = 0.0;
  bool available = false;
};

QString resolveDaliModulePath(const QString &relativeSubPath) {
  const QDir appDir(QCoreApplication::applicationDirPath());
  const QStringList candidates{
      appDir.absoluteFilePath(relativeSubPath),
      appDir.absoluteFilePath(QStringLiteral("../lib/dalinira-browser/") + relativeSubPath),
      appDir.absoluteFilePath(QStringLiteral("../lib64/dalinira-browser/") + relativeSubPath),
      QStringLiteral("/usr/lib/dalinira-browser/") + relativeSubPath,
      QStringLiteral("/usr/local/lib/dalinira-browser/") + relativeSubPath,
      appDir.absoluteFilePath(QStringLiteral("../share/dalinira-browser/") + relativeSubPath),
      QStringLiteral("/usr/share/dalinira-browser/") + relativeSubPath,
  };
  for (const QString &candidate : candidates) {
    if (QFile::exists(candidate)) {
      return candidate;
    }
  }
  return appDir.absoluteFilePath(relativeSubPath);
}
}  // namespace

WebAudioEffectsController::WebAudioEffectsController(QObject *parent, bool persistenceEnabled)
    : QObject(parent), persistenceEnabled_(persistenceEnabled) {
  qRegisterMetaType<WebAudioEffectsController::Status>();
  modules_[OutputModule].relativePath = DALINIRA_WEB_OUTPUT_DALI_MODULE_RELATIVE_PATH;
  modules_[EqModule].relativePath = DALINIRA_WEB_EQ32_DALI_MODULE_RELATIVE_PATH;
  modules_[CompressorModule].relativePath = DALINIRA_WEB_COMPRESSOR_DALI_MODULE_RELATIVE_PATH;
  modules_[LimiterModule].relativePath = DALINIRA_WEB_LIMITER_DALI_MODULE_RELATIVE_PATH;
  modules_[BassEnhancerModule].relativePath = DALINIRA_WEB_BASS_ENHANCER_DALI_MODULE_RELATIVE_PATH;
  modules_[AutoGainModule].relativePath = DALINIRA_WEB_AUTO_GAIN_DALI_MODULE_RELATIVE_PATH;
  persistTimer_.setSingleShot(true);
  persistTimer_.setInterval(180);
  connect(&persistTimer_, &QTimer::timeout, this, &WebAudioEffectsController::persist);
  applyTimer_.setSingleShot(true);
  applyTimer_.setInterval(kApplyCoalesceMs);
  connect(&applyTimer_, &QTimer::timeout, this, &WebAudioEffectsController::applyToAllWebViews);
  QSettings settings;
  // Audio processing is opt-in.  Once the user enables it the value is
  // persisted immediately by setEnabled(), including for installed builds.
  enabled_ = settings.value(QStringLiteral("audioEffects/web/global/enabled"), false).toBool();
  preampDb_ = finiteClamped(settings.value(QStringLiteral("audioEffects/web/output/preampDb"), 0.0).toDouble(),
                            kMinPreampDb, kMaxPreampDb, 0.0);
  if (qEnvironmentVariableIntValue("DALINIRA_FEATURE_DIAGNOSTICS") == 1) {
    qInfo().noquote() << "[AUDIO] DALI runtime initialized";
    qInfo().noquote() << "[AUDIO] DSP module loaded:" << DALINIRA_WEB_OUTPUT_DALI_MODULE_RELATIVE_PATH;
  }
  equalizerBands_.resize(equalizerFrequencies().size());
  for (int index = 0; index < equalizerBands_.size(); ++index) {
    equalizerBands_[index] = finiteClamped(
        settings.value(QStringLiteral("audioEffects/web/equalizer/band%1").arg(index), 0.0).toDouble(),
        kMinEqDb, kMaxEqDb, 0.0);
  }
  bassDb_ = finiteClamped(settings.value(QStringLiteral("audioEffects/web/equalizer/bassDb"), 0.0).toDouble(),
                           kMinEqDb, kMaxEqDb, 0.0);
  midDb_ = finiteClamped(settings.value(QStringLiteral("audioEffects/web/equalizer/midDb"), 0.0).toDouble(),
                          kMinEqDb, kMaxEqDb, 0.0);
  trebleDb_ = finiteClamped(settings.value(QStringLiteral("audioEffects/web/equalizer/trebleDb"), 0.0).toDouble(),
                             kMinEqDb, kMaxEqDb, 0.0);
  stereoExpanderPercent_ = finiteClamped(
      settings.value(QStringLiteral("audioEffects/web/equalizer/stereoExpanderPercent"), 100.0).toDouble(),
      kMinStereoExpanderPercent, kMaxStereoExpanderPercent, 100.0);
  balance_ = finiteClamped(settings.value(QStringLiteral("audioEffects/web/equalizer/balance"), 0.0).toDouble(),
                            kMinBalance, kMaxBalance, 0.0);
  acousticSpace_ = settings.value(QStringLiteral("audioEffects/web/equalizer/acousticSpace"), QStringLiteral("off")).toString();
  if (acousticSpace_ != QStringLiteral("small") && acousticSpace_ != QStringLiteral("medium")
      && acousticSpace_ != QStringLiteral("large") && acousticSpace_ != QStringLiteral("hall")) {
    acousticSpace_ = QStringLiteral("off");
  }
  for (const QString &moduleId : moduleEnableIds()) {
    moduleEnabledStates_.insert(moduleId,
                                settings.value(QStringLiteral("audioEffects/web/%1/enabled").arg(moduleId), false).toBool());
  }
  reverbRoomSizeMs_ = finiteClamped(settings.value(QStringLiteral("audioEffects/web/reverb/roomSizeMs"), 1000.0).toDouble(),
                                     kMinReverbRoomSizeMs, kMaxReverbRoomSizeMs, 1000.0);
  reverbDamping_ = finiteClamped(settings.value(QStringLiteral("audioEffects/web/reverb/damping"), 0.5).toDouble(),
                                  kMinReverbDamping, kMaxReverbDamping, 0.5);
  reverbWetDryDb_ = finiteClamped(settings.value(QStringLiteral("audioEffects/web/reverb/wetDryDb"), -10.0).toDouble(),
                                   kMinReverbWetDryDb, kMaxReverbWetDryDb, -10.0);
  reverbHfRatio_ = finiteClamped(settings.value(QStringLiteral("audioEffects/web/reverb/hfRatio"), 0.7).toDouble(),
                                  kMinReverbHfRatio, kMaxReverbHfRatio, 0.7);
  reverbInputGainDb_ = finiteClamped(settings.value(QStringLiteral("audioEffects/web/reverb/inputGainDb"), 0.0).toDouble(),
                                      kMinReverbInputGainDb, kMaxReverbInputGainDb, 0.0);
  reverbPreset_ = settings.value(QStringLiteral("audioEffects/web/reverb/preset")).toString().trimmed();
  if (!reverbPresetDefinition(reverbPreset_)) reverbPreset_.clear();
  compressorThresholdDb_ = finiteClamped(
      settings.value(QStringLiteral("audioEffects/web/compressor/thresholdDb"), -20.0).toDouble(),
      kMinCompressorThresholdDb, kMaxCompressorThresholdDb, -20.0);
  compressorRatio_ = finiteClamped(
      settings.value(QStringLiteral("audioEffects/web/compressor/ratio"), 4.0).toDouble(),
      kMinCompressorRatio, kMaxCompressorRatio, 4.0);
  compressorAttackMs_ = finiteClamped(
      settings.value(QStringLiteral("audioEffects/web/compressor/attackMs"), 10.0).toDouble(),
      kMinCompressorAttackMs, kMaxCompressorAttackMs, 10.0);
  compressorReleaseMs_ = finiteClamped(
      settings.value(QStringLiteral("audioEffects/web/compressor/releaseMs"), 100.0).toDouble(),
      kMinCompressorReleaseMs, kMaxCompressorReleaseMs, 100.0);
  compressorMakeupDb_ = finiteClamped(
      settings.value(QStringLiteral("audioEffects/web/compressor/makeupDb"), 0.0).toDouble(),
      kMinCompressorMakeupDb, kMaxCompressorMakeupDb, 0.0);
  compressorKneeDb_ = finiteClamped(
      settings.value(QStringLiteral("audioEffects/web/compressor/kneeDb"), 3.0).toDouble(),
      kMinCompressorKneeDb, kMaxCompressorKneeDb, 3.0);
  compressorPreset_ = settings.value(QStringLiteral("audioEffects/web/compressor/preset")).toString().trimmed().toLower();
  if (!compressorPresetDefinition(compressorPreset_)) compressorPreset_.clear();
  limiterCeilingDb_ = finiteClamped(
      settings.value(QStringLiteral("audioEffects/web/limiter/ceilingDb"), -0.3).toDouble(),
      kMinLimiterCeilingDb, kMaxLimiterCeilingDb, -0.3);
  limiterReleaseMs_ = finiteClamped(
      settings.value(QStringLiteral("audioEffects/web/limiter/releaseMs"), 50.0).toDouble(),
      kMinLimiterReleaseMs, kMaxLimiterReleaseMs, 50.0);
  limiterLookaheadMs_ = finiteClamped(
      settings.value(QStringLiteral("audioEffects/web/limiter/lookaheadMs"), 5.0).toDouble(),
      kMinLimiterLookaheadMs, kMaxLimiterLookaheadMs, 5.0);
  limiterGainDb_ = finiteClamped(
      settings.value(QStringLiteral("audioEffects/web/limiter/gainDb"), 0.0).toDouble(),
      kMinLimiterGainDb, kMaxLimiterGainDb, 0.0);
  limiterPreset_ = settings.value(QStringLiteral("audioEffects/web/limiter/preset")).toString().trimmed().toLower();
  if (!limiterPresetDefinition(limiterPreset_)) limiterPreset_.clear();
  bassEnhancerFrequencyHz_ = finiteClamped(
      settings.value(QStringLiteral("audioEffects/web/bassEnhancer/frequencyHz"), 80.0).toDouble(),
      kMinBassEnhancerFrequencyHz, kMaxBassEnhancerFrequencyHz, 80.0);
  bassEnhancerGainDb_ = finiteClamped(
      settings.value(QStringLiteral("audioEffects/web/bassEnhancer/gainDb"), 6.0).toDouble(),
      kMinBassEnhancerGainDb, kMaxBassEnhancerGainDb, 6.0);
  bassEnhancerHarmonicsPercent_ = finiteClamped(
      settings.value(QStringLiteral("audioEffects/web/bassEnhancer/harmonics"), 50.0).toDouble(),
      kMinBassEnhancerHarmonicsPercent, kMaxBassEnhancerHarmonicsPercent, 50.0);
  bassEnhancerWidth_ = finiteClamped(
      settings.value(QStringLiteral("audioEffects/web/bassEnhancer/width"), 1.5).toDouble(),
      kMinBassEnhancerWidth, kMaxBassEnhancerWidth, 1.5);
  bassEnhancerMixPercent_ = finiteClamped(
      settings.value(QStringLiteral("audioEffects/web/bassEnhancer/mix"), 50.0).toDouble(),
      kMinBassEnhancerMixPercent, kMaxBassEnhancerMixPercent, 50.0);
  bassEnhancerDeep_ = settings.value(QStringLiteral("audioEffects/web/bassEnhancer/deep"), false).toBool();
  autoGainTargetDbfs_ = finiteClamped(
      settings.value(QStringLiteral("audioEffects/web/autoGain/targetDbfs"), -14.0).toDouble(),
      kMinAutoGainTargetDbfs, kMaxAutoGainTargetDbfs, -14.0);
  autoGainMaxGainDb_ = finiteClamped(
      settings.value(QStringLiteral("audioEffects/web/autoGain/maxGainDb"), 12.0).toDouble(),
      kMinAutoGainMaxGainDb, kMaxAutoGainMaxGainDb, 12.0);
  autoGainSpeed_ = settings.value(QStringLiteral("audioEffects/web/autoGain/speed"),
                                  QStringLiteral("medium")).toString().trimmed().toLower();
  if (autoGainSpeed_ != QStringLiteral("slow") && autoGainSpeed_ != QStringLiteral("fast")) {
    autoGainSpeed_ = QStringLiteral("medium");
  }
  autoGainPreset_ = settings.value(QStringLiteral("audioEffects/web/autoGain/preset"),
                                   QStringLiteral("balanced")).toString().trimmed().toLower();
  if (!autoGainPresetDefinition(autoGainPreset_)) autoGainPreset_ = QStringLiteral("balanced");
  status_.enabled = enabled_;
  status_.detail = I18n::text(QStringLiteral("audio.status_waiting_media"));
  status_.detailKey = QStringLiteral("audio.status_waiting_media");
}

WebAudioEffectsController::~WebAudioEffectsController() {
  if (persistenceEnabled_ && persistTimer_.isActive()) {
    persistTimer_.stop();
    persist();
  }
}

const QVector<int> &WebAudioEffectsController::equalizerFrequencies() {
  static const QVector<int> frequencies{20, 25, 31, 40, 50, 63, 80, 100,
                                        125, 160, 200, 250, 315, 400, 500, 630,
                                        800, 1000, 1300, 1600, 2000, 2500, 3200, 4000,
                                        5000, 6300, 8000, 10000, 12500, 16000, 20000, 22000};
  return frequencies;
}

double WebAudioEffectsController::equalizerBand(int index) const {
  return index >= 0 && index < equalizerBands_.size() ? equalizerBands_[index] : 0.0;
}

bool WebAudioEffectsController::isEqualizerActive() const {
  for (const double band : equalizerBands_) {
    if (std::abs(band) > 0.01) return true;
  }
  return false;
}

bool WebAudioEffectsController::isToneActive() const {
  return std::abs(bassDb_) > 0.01 || std::abs(midDb_) > 0.01 || std::abs(trebleDb_) > 0.01;
}

bool WebAudioEffectsController::isSpatialActive() const {
  return std::abs(stereoExpanderPercent_ - 100.0) > 0.5 || std::abs(balance_) > 0.5;
}

bool WebAudioEffectsController::moduleEnabled(const QString &moduleId) const {
  return moduleEnabledStates_.value(moduleId.trimmed().toLower(), false);
}

bool WebAudioEffectsController::reverbEnabled() const {
  return moduleEnabled(QLatin1String(kReverbModuleId));
}

bool WebAudioEffectsController::compressorEnabled() const {
  return moduleEnabled(QLatin1String(kCompressorModuleId));
}

bool WebAudioEffectsController::limiterEnabled() const {
  return moduleEnabled(QLatin1String(kLimiterModuleId));
}

bool WebAudioEffectsController::bassEnhancerEnabled() const {
  return moduleEnabled(QLatin1String(kBassEnhancerModuleId));
}

bool WebAudioEffectsController::autoGainEnabled() const {
  return moduleEnabled(QLatin1String(kAutoGainModuleId));
}

void WebAudioEffectsController::setPerformancePolicyMode(dalinira::PerformancePolicyMode mode) {
  if (policyMode_ == mode) return;
  policyMode_ = mode;
  if (policyMode_ == dalinira::PerformancePolicyMode::MemorySaver) {
    applyToAllWebViews();
  }
}

void WebAudioEffectsController::setPanelVisible(bool visible, const QString &activeSubpanelId) {
  if (panelVisible_ == visible && activeSubpanelId_ == activeSubpanelId) return;
  panelVisible_ = visible;
  activeSubpanelId_ = activeSubpanelId;
  if (!panelVisible_) {
    compressorMeterRequestPending_ = false;
    limiterMeterRequestPending_ = false;
  }
}

void WebAudioEffectsController::registerWebView(QWebEngineView *view, const QUrl &initialUrl) {
  if (!view) return;
  Page page;
  page.object = view;
  page.web = view;
  registerPage(page, initialUrl);
}

void WebAudioEffectsController::registerEngineView(dalinira::engine::EngineView *view, const QUrl &initialUrl) {
  if (!view) return;
  Page page;
  page.object = view;
  page.engine = view;
  registerPage(page, initialUrl);
}

void WebAudioEffectsController::registerPage(const Page &page, const QUrl &initialUrl) {
  QObject *object = page.object;
  if (!object || findPage(object)) return;
  pages_.push_back(page);
  if (qEnvironmentVariableIntValue("DALINIRA_FEATURE_DIAGNOSTICS") == 1) {
    qInfo().noquote() << "[AUDIO] page registered:" << object << (page.web ? "chromium" : "engine-view");
  }
  updateAudioPolicyForPage(page, initialUrl.isEmpty() ? pageUrl(page) : initialUrl);
  connect(object, &QObject::destroyed, this, [this, object] {
    bootstrapPages_.remove(object);
    prunePages();
  });
  const auto onLoadFinished = [this, object](bool ok) {
    if (!ok || !enabled_) return;
    // The document bootstrap should already have attached media; this only refreshes live state.
    if (const Page *known = findPage(object)) applyToPage(*known);
  };
  const auto onUrlChanged = [this, object](const QUrl &url) {
    if (const Page *known = findPage(object)) updateAudioPolicyForPage(*known, url);
  };
  // A page that starts producing sound gets its parameters again from the
  // browser: that script may start the page's AudioContext (WebKit starts a
  // context only on resume(), and only within a user gesture or a script of
  // the embedder), so media that is already audible can run through the
  // effects. No polling: the engines report audibility themselves.
  const auto onAudibleChanged = [this, object](bool audible) {
    if (qEnvironmentVariableIsSet("DALINIRA_AUDIO_EFFECTS_TRACE")) {
      qInfo().noquote() << "[DaliNira DSP] page audible:" << audible << object;
    }
    if (!audible || !enabled_) return;
    if (const Page *known = findPage(object)) applyToPage(*known);
  };
  if (page.web) {
    connect(page.web, &QWebEngineView::loadFinished, this, onLoadFinished);
    connect(page.web, &QWebEngineView::urlChanged, this, onUrlChanged);
    if (page.web->page()) connect(page.web->page(), &QWebEnginePage::recentlyAudibleChanged, this, onAudibleChanged);
  } else if (page.engine) {
    connect(page.engine, &dalinira::engine::EngineView::loadFinished, this, onLoadFinished);
    connect(page.engine, &dalinira::engine::EngineView::urlChanged, this, onUrlChanged);
    connect(page.engine, &dalinira::engine::EngineView::audibleChanged, this, onAudibleChanged);
  }
}

void WebAudioEffectsController::unregisterWebView(QWebEngineView *view) { unregisterPage(view); }

void WebAudioEffectsController::unregisterEngineView(dalinira::engine::EngineView *view) { unregisterPage(view); }

void WebAudioEffectsController::unregisterPage(const QObject *object) {
  if (!object) return;
  disconnect(object, nullptr, this, nullptr);
  if (const Page *page = findPage(object); page && page->web && page->web->page())
    disconnect(page->web->page(), nullptr, this, nullptr);
  bootstrapPages_.remove(object);
  pages_.erase(std::remove_if(pages_.begin(), pages_.end(), [object](const Page &page) {
    return page.object.isNull() || page.object == object;
  }), pages_.end());
}

const WebAudioEffectsController::Page *WebAudioEffectsController::findPage(const QObject *object) const {
  for (const Page &page : pages_)
    if (page.object && page.object == object) return &page;
  return nullptr;
}

void WebAudioEffectsController::prunePages() {
  pages_.erase(std::remove_if(pages_.begin(), pages_.end(), [](const Page &page) { return page.object.isNull(); }),
               pages_.end());
}

QUrl WebAudioEffectsController::pageUrl(const Page &page) {
  if (page.web) return page.web->url();
  if (page.engine) return page.engine->url();
  return {};
}

bool WebAudioEffectsController::pageAlive(const Page &page) {
  return (page.web && page.web->page()) || page.engine;
}

void WebAudioEffectsController::runInPage(const Page &page, const QString &script,
                                          std::function<void(const QVariant &)> callback) {
  if (!pageAlive(page) || script.isEmpty()) return;
  ++diagnostics_.scriptsSent;
  diagnostics_.scriptBytesSent += quint64(script.size());
  if (page.web) {
    if (callback) page.web->page()->runJavaScript(script, QWebEngineScript::MainWorld, std::move(callback));
    else page.web->page()->runJavaScript(script, QWebEngineScript::MainWorld);
  } else {
    page.engine->runJavaScript(script, std::move(callback));
  }
}

int WebAudioEffectsController::registeredPageCount() const {
  return static_cast<int>(std::count_if(pages_.cbegin(), pages_.cend(), [](const Page &page) { return !page.object.isNull(); }));
}

int WebAudioEffectsController::audioEnabledWebViewCount() const {
  return static_cast<int>(std::count_if(pages_.cbegin(), pages_.cend(), [](const Page &page) {
    return page.object && isAudioEligibleView(page.object, pageUrl(page));
  }));
}

int WebAudioEffectsController::activeGraphViewCount() const {
  return static_cast<int>(std::count_if(pages_.cbegin(), pages_.cend(), [](const Page &page) {
    return page.object && page.object->property("dalinira-audio-graph-active").toBool();
  }));
}

void WebAudioEffectsController::setEnabled(bool enabled) {
  if (enabled_ == enabled) return;
  enabled_ = enabled;
  const quint64 generation = ++masterEnableGeneration_;
  persist();
  // Pages only carry the runtime while effects are on: install (or remove) the
  // document bootstrap for their future documents.
  prunePages();
  for (const Page &page : std::as_const(pages_)) {
    updateAudioPolicyForPage(page, pageUrl(page));
    if (page.engine) page.engine->refreshDocumentScripts();
  }
  if (enabled_) {
    // Deliver the runtime to the open documents once. YouTube and similar
    // players may replace their media element while the switch is being
    // clicked, so a short recovery window rescans (a small parameter script
    // that only re-delivers the runtime to a page that lost it).
    forceFullReapply_ = true;
    pendingEqualizerBands_.clear();
    for (const Page &page : std::as_const(pages_)) bootstrapPage(page);
    applyToAllWebViews();
    for (const int delayMs : {90, 300, 850}) {
      QTimer::singleShot(delayMs, this, [this, generation] {
        if (!enabled_ || generation != masterEnableGeneration_) return;
        forceFullReapply_ = true;
        pendingEqualizerBands_.clear();
        applyToAllWebViews();
      });
    }
  }
  else {
    applyTimer_.stop();
    pendingEqualizerBands_.clear();
    applyToAllWebViews();
  }
  if (qEnvironmentVariableIsSet("DALINIRA_AUDIO_EFFECTS_TRACE")) {
    qInfo().noquote() << "[DaliNira DSP] master requested:" << (enabled_ ? "enabled" : "disabled")
                      << "pages=" << pages_.size() << "bass=" << bassDb_ << "mid=" << midDb_;
  }
  emit stateChanged();
}

void WebAudioEffectsController::setPreampDb(double db) {
  const double next = std::clamp(db, kMinPreampDb, kMaxPreampDb);
  if (qFuzzyCompare(preampDb_ + 1.0, next + 1.0)) return;
  preampDb_ = next;
  pendingEqualizerBands_.clear();
  schedulePersist();
  scheduleApply();
  emit stateChanged();
}

void WebAudioEffectsController::resetOutput() {
  setPreampDb(0.0);
  persistTimer_.stop();
  persist();
}

void WebAudioEffectsController::setEqualizerBand(int index, double db) {
  if (index < 0 || index >= equalizerBands_.size()) return;
  const double next = std::clamp(db, kMinEqDb, kMaxEqDb);
  if (qFuzzyCompare(equalizerBands_[index] + 1.0, next + 1.0)) return;
  equalizerBands_[index] = next;
  pendingEqualizerBands_.insert(index);
  schedulePersist();
  scheduleApply();
  emit stateChanged();
}

void WebAudioEffectsController::previewEqualizerBands(const QVector<double> &bands) {
  if (bands.size() != equalizerBands_.size()) return;
  QVector<double> normalized;
  normalized.reserve(bands.size());
  for (const double band : bands) normalized.append(finiteClamped(band, kMinEqDb, kMaxEqDb, 0.0));
  if (normalized == equalizerBands_) return;
  equalizerBands_ = normalized;
  // A preview is intentionally excluded from persistence.  It still uses the
  // existing Web/DALI graph and sends the complete curve in one coalesced pass.
  pendingEqualizerBands_.clear();
  scheduleApply();
  emit stateChanged();
}

void WebAudioEffectsController::commitEqualizerBands(const QVector<double> &bands) {
  if (bands.size() != equalizerBands_.size()) return;
  QVector<double> normalized;
  normalized.reserve(bands.size());
  for (const double band : bands) normalized.append(finiteClamped(band, kMinEqDb, kMaxEqDb, 0.0));
  const bool changed = normalized != equalizerBands_;
  equalizerBands_ = normalized;
  pendingEqualizerBands_.clear();
  // Tamam is the transaction boundary: flush, rather than leaving a timer
  // that could make a closed page look committed before it really is.
  persistTimer_.stop();
  persist();
  scheduleApply();
  if (changed) emit stateChanged();
}

void WebAudioEffectsController::setBassDb(double db) {
  const double next = std::clamp(db, kMinEqDb, kMaxEqDb);
  if (qFuzzyCompare(bassDb_ + 1.0, next + 1.0)) return;
  bassDb_ = next;
  pendingEqualizerBands_.clear();
  schedulePersist();
  scheduleApply();
  emit stateChanged();
}

void WebAudioEffectsController::setMidDb(double db) {
  const double next = std::clamp(db, kMinEqDb, kMaxEqDb);
  if (qFuzzyCompare(midDb_ + 1.0, next + 1.0)) return;
  midDb_ = next;
  pendingEqualizerBands_.clear();
  schedulePersist();
  scheduleApply();
  emit stateChanged();
}

void WebAudioEffectsController::setTrebleDb(double db) {
  const double next = std::clamp(db, kMinEqDb, kMaxEqDb);
  if (qFuzzyCompare(trebleDb_ + 1.0, next + 1.0)) return;
  trebleDb_ = next;
  pendingEqualizerBands_.clear();
  schedulePersist();
  scheduleApply();
  emit stateChanged();
}

void WebAudioEffectsController::setStereoExpanderPercent(double percent) {
  const double next = std::clamp(percent, kMinStereoExpanderPercent, kMaxStereoExpanderPercent);
  if (qFuzzyCompare(stereoExpanderPercent_ + 1.0, next + 1.0)) return;
  stereoExpanderPercent_ = next;
  pendingEqualizerBands_.clear();
  schedulePersist();
  scheduleApply();
  emit stateChanged();
}

void WebAudioEffectsController::setBalance(double value) {
  const double next = std::clamp(value, kMinBalance, kMaxBalance);
  if (qFuzzyCompare(balance_ + 1.0, next + 1.0)) return;
  balance_ = next;
  pendingEqualizerBands_.clear();
  schedulePersist();
  scheduleApply();
  emit stateChanged();
}

void WebAudioEffectsController::setAcousticSpace(const QString &space) {
  struct AcousticPreset { const char *id; double bass; double mid; double treble; double stereo; };
  static constexpr AcousticPreset kPresets[] = {
      {"off", 0.0, 0.0, 0.0, 100.0}, {"small", 1.5, 0.5, -0.5, 110.0},
      {"medium", 2.5, 1.0, -1.0, 120.0}, {"large", 3.5, 1.2, -1.8, 132.0},
      {"hall", 4.5, 1.8, -2.5, 145.0},
  };
  const QString wanted = space.trimmed().toLower();
  const AcousticPreset *preset = &kPresets[0];
  for (const AcousticPreset &candidate : kPresets) {
    if (wanted == QLatin1String(candidate.id)) { preset = &candidate; break; }
  }
  const QString nextId = QLatin1String(preset->id);
  const bool changed = acousticSpace_ != nextId || bassDb_ != preset->bass || midDb_ != preset->mid
      || trebleDb_ != preset->treble || stereoExpanderPercent_ != preset->stereo;
  if (!changed) return;
  acousticSpace_ = nextId;
  pendingEqualizerBands_.clear();
  bassDb_ = preset->bass;
  midDb_ = preset->mid;
  trebleDb_ = preset->treble;
  stereoExpanderPercent_ = preset->stereo;
  schedulePersist();
  scheduleApply();
  emit stateChanged();
}

void WebAudioEffectsController::resetEqualizer() {
  bool changed = bassDb_ != 0.0 || midDb_ != 0.0 || trebleDb_ != 0.0 || stereoExpanderPercent_ != 100.0
      || balance_ != 0.0 || acousticSpace_ != QStringLiteral("off");
  for (double &band : equalizerBands_) {
    changed = changed || band != 0.0;
    band = 0.0;
  }
  bassDb_ = 0.0;
  midDb_ = 0.0;
  trebleDb_ = 0.0;
  stereoExpanderPercent_ = 100.0;
  balance_ = 0.0;
  acousticSpace_ = QStringLiteral("off");
  if (!changed) return;
  pendingEqualizerBands_.clear();
  persist();
  scheduleApply();
  emit stateChanged();
}

void WebAudioEffectsController::resetEqualizerModule() {
  const bool changed = bassDb_ != 0.0 || midDb_ != 0.0 || trebleDb_ != 0.0
      || stereoExpanderPercent_ != 100.0 || balance_ != 0.0 || acousticSpace_ != QStringLiteral("off");
  if (!changed) return;
  pendingEqualizerBands_.clear();
  bassDb_ = 0.0;
  midDb_ = 0.0;
  trebleDb_ = 0.0;
  stereoExpanderPercent_ = 100.0;
  balance_ = 0.0;
  acousticSpace_ = QStringLiteral("off");
  persistTimer_.stop();
  persist();
  scheduleApply();
  emit stateChanged();
}

void WebAudioEffectsController::setModuleEnabled(const QString &moduleId, bool enabled) {
  const QString id = moduleId.trimmed().toLower();
  if (id.isEmpty() || moduleEnabledStates_.value(id, false) == enabled) return;
  moduleEnabledStates_.insert(id, enabled);
  // An enable switch is an intentional, discrete user action.  Commit it
  // immediately so closing the effects tab or browser cannot lose it.
  persistTimer_.stop();
  persist();
  if (id == QLatin1String(kReverbModuleId) || id == QLatin1String(kCompressorModuleId)
      || id == QLatin1String(kLimiterModuleId) || id == QLatin1String(kBassEnhancerModuleId)
      || id == QLatin1String(kAutoGainModuleId)) scheduleApply();
  emit stateChanged();
}

void WebAudioEffectsController::setReverbEnabled(bool enabled) {
  setModuleEnabled(QLatin1String(kReverbModuleId), enabled);
}

void WebAudioEffectsController::scheduleReverbStateChange(bool clearPreset) {
  if (clearPreset) reverbPreset_.clear();
  schedulePersist();
  // scheduleApply() deliberately does nothing while the master DSP is
  // bypassed.  The independent Reverb state remains stored and is sent with
  // the full configuration when the master switch comes back on.
  scheduleApply();
  emit stateChanged();
}

void WebAudioEffectsController::setReverbRoomSizeMs(double value) {
  const double next = std::clamp(value, kMinReverbRoomSizeMs, kMaxReverbRoomSizeMs);
  if (qFuzzyCompare(reverbRoomSizeMs_ + 1.0, next + 1.0) && reverbPreset_.isEmpty()) return;
  reverbRoomSizeMs_ = next;
  scheduleReverbStateChange(true);
}

void WebAudioEffectsController::setReverbDamping(double value) {
  const double next = std::clamp(value, kMinReverbDamping, kMaxReverbDamping);
  if (qFuzzyCompare(reverbDamping_ + 1.0, next + 1.0) && reverbPreset_.isEmpty()) return;
  reverbDamping_ = next;
  scheduleReverbStateChange(true);
}

void WebAudioEffectsController::setReverbWetDryDb(double value) {
  const double next = std::clamp(value, kMinReverbWetDryDb, kMaxReverbWetDryDb);
  if (qFuzzyCompare(reverbWetDryDb_ + 1.0, next + 1.0) && reverbPreset_.isEmpty()) return;
  reverbWetDryDb_ = next;
  scheduleReverbStateChange(true);
}

void WebAudioEffectsController::setReverbHfRatio(double value) {
  const double next = std::clamp(value, kMinReverbHfRatio, kMaxReverbHfRatio);
  if (qFuzzyCompare(reverbHfRatio_ + 1.0, next + 1.0) && reverbPreset_.isEmpty()) return;
  reverbHfRatio_ = next;
  scheduleReverbStateChange(true);
}

void WebAudioEffectsController::setReverbInputGainDb(double value) {
  const double next = std::clamp(value, kMinReverbInputGainDb, kMaxReverbInputGainDb);
  if (qFuzzyCompare(reverbInputGainDb_ + 1.0, next + 1.0) && reverbPreset_.isEmpty()) return;
  reverbInputGainDb_ = next;
  scheduleReverbStateChange(true);
}

void WebAudioEffectsController::applyReverbPreset(const QString &presetId) {
  const ReverbPresetDefinition *preset = reverbPresetDefinition(presetId);
  if (!preset) return;
  const QString id = QLatin1String(preset->id);
  const bool changed = reverbPreset_ != id
      || !qFuzzyCompare(reverbRoomSizeMs_ + 1.0, preset->roomSizeMs + 1.0)
      || !qFuzzyCompare(reverbDamping_ + 1.0, preset->damping + 1.0)
      || !qFuzzyCompare(reverbWetDryDb_ + 1.0, preset->wetDryDb + 1.0)
      || !qFuzzyCompare(reverbHfRatio_ + 1.0, preset->hfRatio + 1.0)
      || !qFuzzyCompare(reverbInputGainDb_ + 1.0, preset->inputGainDb + 1.0);
  if (!changed) return;
  reverbRoomSizeMs_ = preset->roomSizeMs;
  reverbDamping_ = preset->damping;
  reverbWetDryDb_ = preset->wetDryDb;
  reverbHfRatio_ = preset->hfRatio;
  reverbInputGainDb_ = preset->inputGainDb;
  reverbPreset_ = id;
  // A preset changes sound parameters only.  It must never silently turn the
  // module on; this is deliberately unlike the old renderer behavior.
  scheduleReverbStateChange(false);
}

void WebAudioEffectsController::resetReverb() {
  const bool changed = !qFuzzyCompare(reverbRoomSizeMs_ + 1.0, 1001.0)
      || !qFuzzyCompare(reverbDamping_ + 1.0, 1.5)
      || !qFuzzyCompare(reverbWetDryDb_ + 1.0, -9.0)
      || !qFuzzyCompare(reverbHfRatio_ + 1.0, 1.7)
      || !qFuzzyCompare(reverbInputGainDb_ + 1.0, 1.0)
      || !reverbPreset_.isEmpty();
  if (!changed) return;
  reverbRoomSizeMs_ = 1000.0;
  reverbDamping_ = 0.5;
  reverbWetDryDb_ = -10.0;
  reverbHfRatio_ = 0.7;
  reverbInputGainDb_ = 0.0;
  reverbPreset_.clear();
  // Reset intentionally does not touch moduleEnabledStates_["reverb"].
  scheduleReverbStateChange(false);
}

void WebAudioEffectsController::setCompressorEnabled(bool enabled) {
  setModuleEnabled(QLatin1String(kCompressorModuleId), enabled);
  if (!enabled) emit compressorGainReductionChanged(0.0, true);
}

void WebAudioEffectsController::scheduleCompressorStateChange(bool clearPreset) {
  if (clearPreset) compressorPreset_.clear();
  schedulePersist();
  scheduleApply();
  emit stateChanged();
}

void WebAudioEffectsController::setCompressorThresholdDb(double value) {
  const double next = finiteClamped(value, kMinCompressorThresholdDb, kMaxCompressorThresholdDb, -20.0);
  if (qFuzzyCompare(compressorThresholdDb_ + 61.0, next + 61.0) && compressorPreset_.isEmpty()) return;
  compressorThresholdDb_ = next;
  scheduleCompressorStateChange(true);
}

void WebAudioEffectsController::setCompressorRatio(double value) {
  const double next = finiteClamped(value, kMinCompressorRatio, kMaxCompressorRatio, 4.0);
  if (qFuzzyCompare(compressorRatio_ + 1.0, next + 1.0) && compressorPreset_.isEmpty()) return;
  compressorRatio_ = next;
  scheduleCompressorStateChange(true);
}

void WebAudioEffectsController::setCompressorAttackMs(double value) {
  const double next = finiteClamped(value, kMinCompressorAttackMs, kMaxCompressorAttackMs, 10.0);
  if (qFuzzyCompare(compressorAttackMs_ + 1.0, next + 1.0) && compressorPreset_.isEmpty()) return;
  compressorAttackMs_ = next;
  scheduleCompressorStateChange(true);
}

void WebAudioEffectsController::setCompressorReleaseMs(double value) {
  const double next = finiteClamped(value, kMinCompressorReleaseMs, kMaxCompressorReleaseMs, 100.0);
  if (qFuzzyCompare(compressorReleaseMs_ + 1.0, next + 1.0) && compressorPreset_.isEmpty()) return;
  compressorReleaseMs_ = next;
  scheduleCompressorStateChange(true);
}

void WebAudioEffectsController::setCompressorMakeupDb(double value) {
  const double next = finiteClamped(value, kMinCompressorMakeupDb, kMaxCompressorMakeupDb, 0.0);
  if (qFuzzyCompare(compressorMakeupDb_ + 13.0, next + 13.0) && compressorPreset_.isEmpty()) return;
  compressorMakeupDb_ = next;
  scheduleCompressorStateChange(true);
}

void WebAudioEffectsController::setCompressorKneeDb(double value) {
  const double next = finiteClamped(value, kMinCompressorKneeDb, kMaxCompressorKneeDb, 3.0);
  if (qFuzzyCompare(compressorKneeDb_ + 1.0, next + 1.0) && compressorPreset_.isEmpty()) return;
  compressorKneeDb_ = next;
  scheduleCompressorStateChange(true);
}

void WebAudioEffectsController::applyCompressorPreset(const QString &presetId) {
  const CompressorPresetDefinition *preset = compressorPresetDefinition(presetId);
  if (!preset) return;
  const QString id = QLatin1String(preset->id);
  const bool changed = compressorPreset_ != id
      || !qFuzzyCompare(compressorThresholdDb_ + 61.0, preset->thresholdDb + 61.0)
      || !qFuzzyCompare(compressorRatio_ + 1.0, preset->ratio + 1.0)
      || !qFuzzyCompare(compressorAttackMs_ + 1.0, preset->attackMs + 1.0)
      || !qFuzzyCompare(compressorReleaseMs_ + 1.0, preset->releaseMs + 1.0)
      || !qFuzzyCompare(compressorMakeupDb_ + 13.0, preset->makeupDb + 13.0)
      || !qFuzzyCompare(compressorKneeDb_ + 1.0, preset->kneeDb + 1.0);
  if (!changed) return;
  compressorThresholdDb_ = preset->thresholdDb;
  compressorRatio_ = preset->ratio;
  compressorAttackMs_ = preset->attackMs;
  compressorReleaseMs_ = preset->releaseMs;
  compressorMakeupDb_ = preset->makeupDb;
  compressorKneeDb_ = preset->kneeDb;
  compressorPreset_ = id;
  // Presets only change parameters; the module switch remains user-owned.
  scheduleCompressorStateChange(false);
}

void WebAudioEffectsController::resetCompressor() {
  const bool changed = !qFuzzyCompare(compressorThresholdDb_ + 61.0, 41.0)
      || !qFuzzyCompare(compressorRatio_ + 1.0, 5.0)
      || !qFuzzyCompare(compressorAttackMs_ + 1.0, 11.0)
      || !qFuzzyCompare(compressorReleaseMs_ + 1.0, 101.0)
      || !qFuzzyCompare(compressorMakeupDb_ + 13.0, 13.0)
      || !qFuzzyCompare(compressorKneeDb_ + 1.0, 4.0)
      || !compressorPreset_.isEmpty();
  if (!changed) return;
  compressorThresholdDb_ = -20.0;
  compressorRatio_ = 4.0;
  compressorAttackMs_ = 10.0;
  compressorReleaseMs_ = 100.0;
  compressorMakeupDb_ = 0.0;
  compressorKneeDb_ = 3.0;
  compressorPreset_.clear();
  // Reset is isolated from enabled/global/output/EQ/Reverb state.
  scheduleCompressorStateChange(false);
}

void WebAudioEffectsController::setLimiterEnabled(bool enabled) {
  setModuleEnabled(QLatin1String(kLimiterModuleId), enabled);
  if (!enabled) emit limiterReductionChanged(0.0, true);
}

void WebAudioEffectsController::scheduleLimiterStateChange(bool clearPreset) {
  if (clearPreset) limiterPreset_.clear();
  schedulePersist();
  scheduleApply();
  emit stateChanged();
}

void WebAudioEffectsController::setLimiterCeilingDb(double value) {
  const double next = finiteClamped(value, kMinLimiterCeilingDb, kMaxLimiterCeilingDb, -0.3);
  if (qFuzzyCompare(limiterCeilingDb_ + 13.0, next + 13.0) && limiterPreset_.isEmpty()) return;
  limiterCeilingDb_ = next;
  scheduleLimiterStateChange(true);
}

void WebAudioEffectsController::setLimiterReleaseMs(double value) {
  const double next = finiteClamped(value, kMinLimiterReleaseMs, kMaxLimiterReleaseMs, 50.0);
  if (qFuzzyCompare(limiterReleaseMs_ + 1.0, next + 1.0) && limiterPreset_.isEmpty()) return;
  limiterReleaseMs_ = next;
  scheduleLimiterStateChange(true);
}

void WebAudioEffectsController::setLimiterLookaheadMs(double value) {
  const double next = finiteClamped(value, kMinLimiterLookaheadMs, kMaxLimiterLookaheadMs, 5.0);
  if (qFuzzyCompare(limiterLookaheadMs_ + 1.0, next + 1.0) && limiterPreset_.isEmpty()) return;
  limiterLookaheadMs_ = next;
  scheduleLimiterStateChange(true);
}

void WebAudioEffectsController::setLimiterGainDb(double value) {
  const double next = finiteClamped(value, kMinLimiterGainDb, kMaxLimiterGainDb, 0.0);
  if (qFuzzyCompare(limiterGainDb_ + 13.0, next + 13.0) && limiterPreset_.isEmpty()) return;
  limiterGainDb_ = next;
  scheduleLimiterStateChange(true);
}

void WebAudioEffectsController::applyLimiterPreset(const QString &presetId) {
  const LimiterPresetDefinition *preset = limiterPresetDefinition(presetId);
  if (!preset) return;
  const QString id = QLatin1String(preset->id);
  const bool changed = limiterPreset_ != id
      || !qFuzzyCompare(limiterCeilingDb_ + 13.0, preset->ceilingDb + 13.0)
      || !qFuzzyCompare(limiterReleaseMs_ + 1.0, preset->releaseMs + 1.0)
      || !qFuzzyCompare(limiterLookaheadMs_ + 1.0, preset->lookaheadMs + 1.0)
      || !qFuzzyCompare(limiterGainDb_ + 13.0, preset->gainDb + 13.0);
  if (!changed) return;
  limiterCeilingDb_ = preset->ceilingDb;
  limiterReleaseMs_ = preset->releaseMs;
  limiterLookaheadMs_ = preset->lookaheadMs;
  limiterGainDb_ = preset->gainDb;
  limiterPreset_ = id;
  // Presets never own the module switch.
  scheduleLimiterStateChange(false);
}

void WebAudioEffectsController::resetLimiter() {
  const bool changed = !qFuzzyCompare(limiterCeilingDb_ + 13.0, 12.7)
      || !qFuzzyCompare(limiterReleaseMs_ + 1.0, 51.0)
      || !qFuzzyCompare(limiterLookaheadMs_ + 1.0, 6.0)
      || !qFuzzyCompare(limiterGainDb_ + 13.0, 13.0)
      || !limiterPreset_.isEmpty();
  if (!changed) return;
  limiterCeilingDb_ = -0.3;
  limiterReleaseMs_ = 50.0;
  limiterLookaheadMs_ = 5.0;
  limiterGainDb_ = 0.0;
  limiterPreset_.clear();
  // Reset is isolated from master/output/EQ/Reverb/Compressor state.
  scheduleLimiterStateChange(false);
}

void WebAudioEffectsController::setBassEnhancerEnabled(bool enabled) {
  setModuleEnabled(QLatin1String(kBassEnhancerModuleId), enabled);
}

void WebAudioEffectsController::scheduleBassEnhancerStateChange(bool clearDeep) {
  if (clearDeep) bassEnhancerDeep_ = false;
  schedulePersist();
  scheduleApply();
  emit stateChanged();
}

void WebAudioEffectsController::setBassEnhancerFrequencyHz(double value) {
  const double next = finiteClamped(value, kMinBassEnhancerFrequencyHz, kMaxBassEnhancerFrequencyHz, 80.0);
  if (qFuzzyCompare(bassEnhancerFrequencyHz_ + 1.0, next + 1.0) && !bassEnhancerDeep_) return;
  bassEnhancerFrequencyHz_ = next;
  scheduleBassEnhancerStateChange(true);
}

void WebAudioEffectsController::setBassEnhancerGainDb(double value) {
  const double next = finiteClamped(value, kMinBassEnhancerGainDb, kMaxBassEnhancerGainDb, 6.0);
  if (qFuzzyCompare(bassEnhancerGainDb_ + 1.0, next + 1.0) && !bassEnhancerDeep_) return;
  bassEnhancerGainDb_ = next;
  scheduleBassEnhancerStateChange(true);
}

void WebAudioEffectsController::setBassEnhancerHarmonicsPercent(double value) {
  const double next = finiteClamped(value, kMinBassEnhancerHarmonicsPercent,
                                    kMaxBassEnhancerHarmonicsPercent, 50.0);
  if (qFuzzyCompare(bassEnhancerHarmonicsPercent_ + 1.0, next + 1.0) && !bassEnhancerDeep_) return;
  bassEnhancerHarmonicsPercent_ = next;
  scheduleBassEnhancerStateChange(true);
}

void WebAudioEffectsController::setBassEnhancerWidth(double value) {
  const double next = finiteClamped(value, kMinBassEnhancerWidth, kMaxBassEnhancerWidth, 1.5);
  if (qFuzzyCompare(bassEnhancerWidth_ + 1.0, next + 1.0) && !bassEnhancerDeep_) return;
  bassEnhancerWidth_ = next;
  scheduleBassEnhancerStateChange(true);
}

void WebAudioEffectsController::setBassEnhancerMixPercent(double value) {
  const double next = finiteClamped(value, kMinBassEnhancerMixPercent, kMaxBassEnhancerMixPercent, 50.0);
  if (qFuzzyCompare(bassEnhancerMixPercent_ + 1.0, next + 1.0) && !bassEnhancerDeep_) return;
  bassEnhancerMixPercent_ = next;
  scheduleBassEnhancerStateChange(true);
}

void WebAudioEffectsController::applyBassEnhancerDeep() {
  const bool changed = !bassEnhancerDeep_
      || !qFuzzyCompare(bassEnhancerFrequencyHz_ + 1.0, 69.0)
      || !qFuzzyCompare(bassEnhancerGainDb_ + 1.0, 18.5)
      || !qFuzzyCompare(bassEnhancerHarmonicsPercent_ + 1.0, 15.0)
      || !qFuzzyCompare(bassEnhancerWidth_ + 1.0, 2.2)
      || !qFuzzyCompare(bassEnhancerMixPercent_ + 1.0, 83.0);
  if (!changed) return;
  bassEnhancerFrequencyHz_ = 68.0;
  bassEnhancerGainDb_ = 17.5;
  bassEnhancerHarmonicsPercent_ = 14.0;
  bassEnhancerWidth_ = 1.2;
  bassEnhancerMixPercent_ = 82.0;
  bassEnhancerDeep_ = true;
  // The legacy preset auto-enabled the module; Stage 6 explicitly keeps the
  // independently persisted module switch unchanged.
  scheduleBassEnhancerStateChange(false);
}

void WebAudioEffectsController::resetBassEnhancer() {
  const bool changed = !qFuzzyCompare(bassEnhancerFrequencyHz_ + 1.0, 81.0)
      || !qFuzzyCompare(bassEnhancerGainDb_ + 1.0, 7.0)
      || !qFuzzyCompare(bassEnhancerHarmonicsPercent_ + 1.0, 51.0)
      || !qFuzzyCompare(bassEnhancerWidth_ + 1.0, 2.5)
      || !qFuzzyCompare(bassEnhancerMixPercent_ + 1.0, 51.0)
      || bassEnhancerDeep_;
  if (!changed) return;
  bassEnhancerFrequencyHz_ = 80.0;
  bassEnhancerGainDb_ = 6.0;
  bassEnhancerHarmonicsPercent_ = 50.0;
  bassEnhancerWidth_ = 1.5;
  bassEnhancerMixPercent_ = 50.0;
  bassEnhancerDeep_ = false;
  // Reset owns only Bass Enhancer parameters and its Deep preset selection.
  scheduleBassEnhancerStateChange(false);
}

void WebAudioEffectsController::setAutoGainEnabled(bool enabled) {
  setModuleEnabled(QLatin1String(kAutoGainModuleId), enabled);
}

void WebAudioEffectsController::scheduleAutoGainStateChange() {
  schedulePersist();
  scheduleApply();
  emit stateChanged();
}

void WebAudioEffectsController::setAutoGainTargetDbfs(double value) {
  const double next = finiteClamped(value, kMinAutoGainTargetDbfs, kMaxAutoGainTargetDbfs, -14.0);
  if (qFuzzyCompare(autoGainTargetDbfs_ + 25.0, next + 25.0)) return;
  autoGainTargetDbfs_ = next;
  // The legacy generic knob path retains lastPreset/speed on manual edits.
  scheduleAutoGainStateChange();
}

void WebAudioEffectsController::setAutoGainMaxGainDb(double value) {
  const double next = finiteClamped(value, kMinAutoGainMaxGainDb, kMaxAutoGainMaxGainDb, 12.0);
  if (qFuzzyCompare(autoGainMaxGainDb_ + 1.0, next + 1.0)) return;
  autoGainMaxGainDb_ = next;
  scheduleAutoGainStateChange();
}

void WebAudioEffectsController::applyAutoGainPreset(const QString &presetId) {
  const AutoGainPresetDefinition *preset = autoGainPresetDefinition(presetId);
  if (!preset) return;
  const QString id = QString::fromLatin1(preset->id);
  const QString speed = QString::fromLatin1(preset->speed);
  const bool changed = autoGainPreset_ != id || autoGainSpeed_ != speed
      || !qFuzzyCompare(autoGainTargetDbfs_ + 25.0, preset->targetDbfs + 25.0)
      || !qFuzzyCompare(autoGainMaxGainDb_ + 1.0, preset->maxGainDb + 1.0);
  if (!changed) return;
  autoGainTargetDbfs_ = preset->targetDbfs;
  autoGainMaxGainDb_ = preset->maxGainDb;
  autoGainSpeed_ = speed;
  autoGainPreset_ = id;
  // Stage 7 intentionally overrides the legacy UI's auto-enable behavior.
  scheduleAutoGainStateChange();
}

void WebAudioEffectsController::resetAutoGain() {
  const bool changed = !qFuzzyCompare(autoGainTargetDbfs_ + 25.0, 11.0)
      || !qFuzzyCompare(autoGainMaxGainDb_ + 1.0, 13.0)
      || autoGainSpeed_ != QStringLiteral("medium") || autoGainPreset_ != QStringLiteral("balanced");
  if (!changed) return;
  autoGainTargetDbfs_ = -14.0;
  autoGainMaxGainDb_ = 12.0;
  autoGainSpeed_ = QStringLiteral("medium");
  autoGainPreset_ = QStringLiteral("balanced");
  // Reset owns only Auto Gain user parameters; enabled/master/other modules stay untouched.
  scheduleAutoGainStateChange();
}

void WebAudioEffectsController::applyToAllWebViews() {
  prunePages();
  if (enabled_ && pendingEqualizerBands_.size() == 1) {
    const int index = *pendingEqualizerBands_.cbegin();
    pendingEqualizerBands_.clear();
    for (const Page &page : std::as_const(pages_)) applyEqualizerBandToPage(page, index);
    return;
  }
  pendingEqualizerBands_.clear();
  for (const Page &page : std::as_const(pages_)) applyToPage(page);
  forceFullReapply_ = false;
}

void WebAudioEffectsController::persist() {
  if (!persistenceEnabled_) return;
  QSettings settings;
  settings.setValue(QStringLiteral("audioEffects/web/global/enabled"), enabled_);
  settings.setValue(QStringLiteral("audioEffects/web/output/preampDb"), preampDb_);
  for (int index = 0; index < equalizerBands_.size(); ++index) {
    settings.setValue(QStringLiteral("audioEffects/web/equalizer/band%1").arg(index), equalizerBands_[index]);
  }
  settings.setValue(QStringLiteral("audioEffects/web/equalizer/bassDb"), bassDb_);
  settings.setValue(QStringLiteral("audioEffects/web/equalizer/midDb"), midDb_);
  settings.setValue(QStringLiteral("audioEffects/web/equalizer/trebleDb"), trebleDb_);
  settings.setValue(QStringLiteral("audioEffects/web/equalizer/stereoExpanderPercent"), stereoExpanderPercent_);
  settings.setValue(QStringLiteral("audioEffects/web/equalizer/balance"), balance_);
  settings.setValue(QStringLiteral("audioEffects/web/equalizer/acousticSpace"), acousticSpace_);
  for (auto state = moduleEnabledStates_.cbegin(); state != moduleEnabledStates_.cend(); ++state) {
    settings.setValue(QStringLiteral("audioEffects/web/%1/enabled").arg(state.key()), state.value());
  }
  settings.setValue(QStringLiteral("audioEffects/web/reverb/roomSizeMs"), reverbRoomSizeMs_);
  settings.setValue(QStringLiteral("audioEffects/web/reverb/damping"), reverbDamping_);
  settings.setValue(QStringLiteral("audioEffects/web/reverb/wetDryDb"), reverbWetDryDb_);
  settings.setValue(QStringLiteral("audioEffects/web/reverb/hfRatio"), reverbHfRatio_);
  settings.setValue(QStringLiteral("audioEffects/web/reverb/inputGainDb"), reverbInputGainDb_);
  settings.setValue(QStringLiteral("audioEffects/web/reverb/preset"), reverbPreset_);
  settings.setValue(QStringLiteral("audioEffects/web/compressor/thresholdDb"), compressorThresholdDb_);
  settings.setValue(QStringLiteral("audioEffects/web/compressor/ratio"), compressorRatio_);
  settings.setValue(QStringLiteral("audioEffects/web/compressor/attackMs"), compressorAttackMs_);
  settings.setValue(QStringLiteral("audioEffects/web/compressor/releaseMs"), compressorReleaseMs_);
  settings.setValue(QStringLiteral("audioEffects/web/compressor/makeupDb"), compressorMakeupDb_);
  settings.setValue(QStringLiteral("audioEffects/web/compressor/kneeDb"), compressorKneeDb_);
  settings.setValue(QStringLiteral("audioEffects/web/compressor/preset"), compressorPreset_);
  settings.setValue(QStringLiteral("audioEffects/web/limiter/ceilingDb"), limiterCeilingDb_);
  settings.setValue(QStringLiteral("audioEffects/web/limiter/releaseMs"), limiterReleaseMs_);
  settings.setValue(QStringLiteral("audioEffects/web/limiter/lookaheadMs"), limiterLookaheadMs_);
  settings.setValue(QStringLiteral("audioEffects/web/limiter/gainDb"), limiterGainDb_);
  settings.setValue(QStringLiteral("audioEffects/web/limiter/preset"), limiterPreset_);
  settings.setValue(QStringLiteral("audioEffects/web/bassEnhancer/frequencyHz"), bassEnhancerFrequencyHz_);
  settings.setValue(QStringLiteral("audioEffects/web/bassEnhancer/gainDb"), bassEnhancerGainDb_);
  settings.setValue(QStringLiteral("audioEffects/web/bassEnhancer/harmonics"), bassEnhancerHarmonicsPercent_);
  settings.setValue(QStringLiteral("audioEffects/web/bassEnhancer/width"), bassEnhancerWidth_);
  settings.setValue(QStringLiteral("audioEffects/web/bassEnhancer/mix"), bassEnhancerMixPercent_);
  settings.setValue(QStringLiteral("audioEffects/web/bassEnhancer/deep"), bassEnhancerDeep_);
  settings.setValue(QStringLiteral("audioEffects/web/autoGain/targetDbfs"), autoGainTargetDbfs_);
  settings.setValue(QStringLiteral("audioEffects/web/autoGain/maxGainDb"), autoGainMaxGainDb_);
  settings.setValue(QStringLiteral("audioEffects/web/autoGain/speed"), autoGainSpeed_);
  settings.setValue(QStringLiteral("audioEffects/web/autoGain/preset"), autoGainPreset_);
  settings.sync();
}

void WebAudioEffectsController::schedulePersist() {
  persistTimer_.start();
}

void WebAudioEffectsController::scheduleApply() {
  if (!enabled_) return;
  applyTimer_.start();
}

bool WebAudioEffectsController::validateDaliModule(const QByteArray &bytes, QString *error) {
  const auto fail = [error](const char *reason) {
    if (error) *error = QString::fromLatin1(reason);
    return false;
  };
  if (bytes.isEmpty()) return fail("empty module");
  if (bytes.size() > kMaxDaliModuleBytes) return fail("module is unreasonably large");
  if (bytes.contains('\0')) return fail("binary data in module");
  QStringDecoder decoder(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
  const QString text = decoder.decode(bytes);
  if (decoder.hasError()) return fail("module is not valid UTF-8");
  // The compiler's signature and version: the runtime parses what this
  // compiler version emits (see browser/scripts/compile-dali-preset.js).
  const QString head = text.left(512);
  if (!head.contains(QStringLiteral("// Generated by dali-lang compiler"))) return fail("not a DALI compiled module");
  if (!head.contains(QStringLiteral("// Generated by dali-lang compiler (v0)"))) return fail("unsupported DALI compiler version");
  // A module ends with its export table; a cut file would break the whole
  // runtime script it is embedded in.
  const QString trimmed = text.trimmed();
  if (!trimmed.contains(QStringLiteral("module.exports = {")) || !trimmed.contains(QStringLiteral("buildGraph:"))
      || !trimmed.endsWith(QStringLiteral("};"))) {
    return fail("truncated or malformed module (no export table)");
  }
  if (error) error->clear();
  return true;
}

QString WebAudioEffectsController::moduleSource(ModuleIndex index) const {
  DaliModule &module = modules_[index];
  if (module.loaded) return module.source;
  // Read once per process: a missing or invalid module is not retried at
  // every page event, and never reaches a page.
  module.loaded = true;
  ++diagnostics_.moduleLoads;
  const QString path = resolveDaliModulePath(QString::fromLatin1(module.relativePath));
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly) || file.size() > kMaxDaliModuleBytes) {
    ++diagnostics_.moduleRejects;
    qWarning().noquote() << "[DaliNira DSP] DALI module unavailable:" << path;
    return {};
  }
  const QByteArray bytes = file.read(kMaxDaliModuleBytes + 1);
  QString reason;
  if (!validateDaliModule(bytes, &reason)) {
    ++diagnostics_.moduleRejects;
    qWarning().noquote() << "[DaliNira DSP] DALI module rejected:" << path << "-" << reason;
    return {};
  }
  module.source = QString::fromUtf8(bytes);
  if (qEnvironmentVariableIntValue("DALINIRA_FEATURE_DIAGNOSTICS") == 1) {
    qInfo().noquote() << "[AUDIO] DALI module loaded:" << module.relativePath << module.source.size() << "chars";
  }
  return module.source;
}

QString WebAudioEffectsController::parameterUpdateScript() const {
  const QString enabledJson = enabled_ ? QStringLiteral("true") : QStringLiteral("false");
  const QString forceRefreshJson = forceFullReapply_ ? QStringLiteral("true") : QStringLiteral("false");
  const QJsonObject reverbConfig{
      {QStringLiteral("enabled"), reverbEnabled()},
      {QStringLiteral("roomSizeMs"), reverbRoomSizeMs_},
      {QStringLiteral("damping"), reverbDamping_},
      {QStringLiteral("wetDryDb"), reverbWetDryDb_},
      {QStringLiteral("hfRatio"), reverbHfRatio_},
      {QStringLiteral("inputGainDb"), reverbInputGainDb_},
  };
  const QString reverbConfigJson = QString::fromUtf8(QJsonDocument(reverbConfig).toJson(QJsonDocument::Compact));
  const QJsonObject compressorConfig{
      {QStringLiteral("enabled"), compressorEnabled()},
      {QStringLiteral("thresholdDb"), compressorThresholdDb_},
      {QStringLiteral("ratio"), compressorRatio_},
      {QStringLiteral("attackMs"), compressorAttackMs_},
      {QStringLiteral("releaseMs"), compressorReleaseMs_},
      {QStringLiteral("makeupDb"), compressorMakeupDb_},
      {QStringLiteral("kneeDb"), compressorKneeDb_},
  };
  const QString compressorConfigJson = QString::fromUtf8(QJsonDocument(compressorConfig).toJson(QJsonDocument::Compact));
  const QJsonObject limiterConfig{
      {QStringLiteral("enabled"), limiterEnabled()},
      {QStringLiteral("ceilingDb"), limiterCeilingDb_},
      {QStringLiteral("releaseMs"), limiterReleaseMs_},
      {QStringLiteral("lookaheadMs"), limiterLookaheadMs_},
  };
  const QString limiterConfigJson = QString::fromUtf8(QJsonDocument(limiterConfig).toJson(QJsonDocument::Compact));
  const QJsonObject bassEnhancerConfig{
      {QStringLiteral("enabled"), bassEnhancerEnabled()},
      {QStringLiteral("frequencyHz"), bassEnhancerFrequencyHz_},
      {QStringLiteral("gainDb"), bassEnhancerGainDb_},
      {QStringLiteral("harmonicsPercent"), bassEnhancerHarmonicsPercent_},
      {QStringLiteral("width"), bassEnhancerWidth_},
      {QStringLiteral("mixPercent"), bassEnhancerMixPercent_},
      {QStringLiteral("deep"), bassEnhancerDeep_},
  };
  const QString bassEnhancerConfigJson = QString::fromUtf8(QJsonDocument(bassEnhancerConfig).toJson(QJsonDocument::Compact));
  const QJsonObject autoGainConfig{
      {QStringLiteral("enabled"), autoGainEnabled()},
      {QStringLiteral("targetDbfs"), autoGainTargetDbfs_},
      {QStringLiteral("maxGainDb"), autoGainMaxGainDb_},
      {QStringLiteral("speed"), autoGainSpeed_},
      {QStringLiteral("preset"), autoGainPreset_},
  };
  const QString autoGainConfigJson = QString::fromUtf8(QJsonDocument(autoGainConfig).toJson(QJsonDocument::Compact));
  const QString policyModeStr = policyMode_ == dalinira::PerformancePolicyMode::MemorySaver
      ? QStringLiteral("memory_saver")
      : (policyMode_ == dalinira::PerformancePolicyMode::MaximumPerformance
             ? QStringLiteral("maximum_performance")
             : QStringLiteral("balanced"));

  return QStringLiteral(R"JS(
(function() {
  try {
    const enabled = %1;
    const cfg = {
      outputPreampDb: %2,
      bands: %3,
      eqActive: %4,
      bassDb: %5,
      midDb: %6,
      trebleDb: %7,
      toneActive: %8,
      stereoExpanderPercent: %9,
      balance: %10,
      spatialActive: %11,
      reverb: %12,
      compressor: %13,
      limiter: %14,
      bassEnhancer: %15,
      autoGain: %16,
      policyMode: '%17',
      panelVisible: %18,
      activeSubpanel: '%19',
      forceRefresh: %20
    };
    const root = window.__DALINIRA_WEB_DALI_OUTPUT__;
    if (!root || !root.processMedia) {
      return { ok: false, enabled: enabled, mediaCount: 0, sampleRates: [], moduleLoaded: false, needsBootstrap: enabled };
    }
    const fullCfg = Object.assign(cfg, { enabled: enabled });
    if (enabled && fullCfg.forceRefresh && root.forceActivate) {
      return root.forceActivate(fullCfg);
    }
    const hasActiveGraph = !!(root.graphList && root.graphList.size > 0
      && Array.from(root.graphList).some(function(graph) { return graph && graph.graph && !graph.bypass && graph.runtimeGain; }));
    if (enabled && hasActiveGraph && root.applyActiveParams) {
      return root.applyActiveParams(fullCfg);
    }
    return root.processMedia(fullCfg);
  } catch (error) {
    return { ok: false, enabled: %1, mediaCount: 0, sampleRates: [], moduleLoaded: false,
             error: String(error && error.message ? error.message : error), needsBootstrap: %1 };
  }
})()
)JS")
      .arg(enabledJson)
      .arg(QString::number(preampDb_, 'f', 2))
      .arg(jsonNumberArray(equalizerBands_))
      .arg(isEqualizerActive() ? QStringLiteral("true") : QStringLiteral("false"))
      .arg(QString::number(bassDb_, 'f', 2))
      .arg(QString::number(midDb_, 'f', 2))
      .arg(QString::number(trebleDb_, 'f', 2))
      .arg(isToneActive() ? QStringLiteral("true") : QStringLiteral("false"))
      .arg(QString::number(stereoExpanderPercent_, 'f', 2))
      .arg(QString::number(balance_, 'f', 2))
      .arg(isSpatialActive() ? QStringLiteral("true") : QStringLiteral("false"))
      .arg(reverbConfigJson)
      .arg(compressorConfigJson)
      .arg(limiterConfigJson)
      .arg(bassEnhancerConfigJson)
      .arg(autoGainConfigJson)
      .arg(policyModeStr)
      .arg(panelVisible_ ? QStringLiteral("true") : QStringLiteral("false"))
      .arg(activeSubpanelId_)
      .arg(forceRefreshJson);
}

QString WebAudioEffectsController::equalizerBandUpdateScript(int index) const {
  if (index < 0 || index >= equalizerBands_.size()) return {};
  return QStringLiteral(R"JS(
(function() {
  try {
    const index = %1;
    const value = %2;
    const root = window.__DALINIRA_WEB_DALI_OUTPUT__;
    if (!root || !root.graphList || !root.smoothParam) return;
    for (const graph of root.graphList) {
      if (!graph || graph.bypass) continue;
      if (!graph.eqBandNodes || graph.eqBandNodes.length === 0) {
        if (root.currentConfig && root.applyActiveParams) {
          root.currentConfig.bands[index] = value;
          root.applyActiveParams(root.currentConfig);
        }
        continue;
      }
      const band = graph.eqBandNodes[index];
      if (!band || !band.gain) continue;
      const prior = graph._daliniraBands || (graph._daliniraBands = []);
      if (Number.isFinite(prior[index]) && Math.abs(prior[index] - value) < 0.0001) continue;
      prior[index] = value;
      root.smoothParam(band.gain, value, graph.ctx, 0.140);
    }
  } catch (_) {}
})()
)JS")
      .arg(index)
      .arg(QString::number(equalizerBands_[index], 'f', 2));
}

QString WebAudioEffectsController::injectionScript() const {
  const QString key = parameterUpdateScript();
  if (injectionScriptCache_.isEmpty() || key != injectionScriptKey_) {
    injectionScriptCache_ = buildInjectionScript();
    injectionScriptKey_ = key;
  }
  return injectionScriptCache_;
}

QString WebAudioEffectsController::buildInjectionScript() const {
  ++diagnostics_.injectionBuilds;
  const QString outputModuleSource = daliModuleSource();
  const QString eqModuleSource = daliEqModuleSource();
  const QString compressorModuleSource = daliCompressorModuleSource();
  const QString limiterModuleSource = daliLimiterModuleSource();
  const QString bassEnhancerModuleSource = daliBassEnhancerModuleSource();
  const QString autoGainModuleSource = daliAutoGainModuleSource();
  const QString outputModuleJson = jsonLiteral(outputModuleSource);
  const QString eqModuleJson = jsonLiteral(eqModuleSource);
  const QString compressorModuleJson = jsonLiteral(compressorModuleSource);
  const QString limiterModuleJson = jsonLiteral(limiterModuleSource);
  const QString bassEnhancerModuleJson = jsonLiteral(bassEnhancerModuleSource);
  const QString autoGainModuleJson = jsonLiteral(autoGainModuleSource);
  const QString enabledJson = enabled_ ? QStringLiteral("true") : QStringLiteral("false");
  const QJsonObject reverbConfig{
      {QStringLiteral("enabled"), reverbEnabled()},
      {QStringLiteral("roomSizeMs"), reverbRoomSizeMs_},
      {QStringLiteral("damping"), reverbDamping_},
      {QStringLiteral("wetDryDb"), reverbWetDryDb_},
      {QStringLiteral("hfRatio"), reverbHfRatio_},
      {QStringLiteral("inputGainDb"), reverbInputGainDb_},
  };
  const QString reverbConfigJson = QString::fromUtf8(QJsonDocument(reverbConfig).toJson(QJsonDocument::Compact));
  const QJsonObject compressorConfig{
      {QStringLiteral("enabled"), compressorEnabled()},
      {QStringLiteral("thresholdDb"), compressorThresholdDb_},
      {QStringLiteral("ratio"), compressorRatio_},
      {QStringLiteral("attackMs"), compressorAttackMs_},
      {QStringLiteral("releaseMs"), compressorReleaseMs_},
      {QStringLiteral("makeupDb"), compressorMakeupDb_},
      {QStringLiteral("kneeDb"), compressorKneeDb_},
  };
  const QString compressorConfigJson = QString::fromUtf8(QJsonDocument(compressorConfig).toJson(QJsonDocument::Compact));
  const QJsonObject limiterConfig{
      {QStringLiteral("enabled"), limiterEnabled()},
      {QStringLiteral("ceilingDb"), limiterCeilingDb_},
      {QStringLiteral("releaseMs"), limiterReleaseMs_},
      {QStringLiteral("lookaheadMs"), limiterLookaheadMs_},
      {QStringLiteral("gainDb"), limiterGainDb_},
  };
  const QString limiterConfigJson = QString::fromUtf8(QJsonDocument(limiterConfig).toJson(QJsonDocument::Compact));
  const QJsonObject bassEnhancerConfig{
      {QStringLiteral("enabled"), bassEnhancerEnabled()},
      {QStringLiteral("frequencyHz"), bassEnhancerFrequencyHz_},
      {QStringLiteral("gainDb"), bassEnhancerGainDb_},
      {QStringLiteral("harmonicsPercent"), bassEnhancerHarmonicsPercent_},
      {QStringLiteral("width"), bassEnhancerWidth_},
      {QStringLiteral("mixPercent"), bassEnhancerMixPercent_},
      {QStringLiteral("deep"), bassEnhancerDeep_},
  };
  const QString bassEnhancerConfigJson = QString::fromUtf8(QJsonDocument(bassEnhancerConfig).toJson(QJsonDocument::Compact));
  const QJsonObject autoGainConfig{
      {QStringLiteral("enabled"), autoGainEnabled()},
      {QStringLiteral("targetDbfs"), autoGainTargetDbfs_},
      {QStringLiteral("maxGainDb"), autoGainMaxGainDb_},
      {QStringLiteral("speed"), autoGainSpeed_},
      {QStringLiteral("preset"), autoGainPreset_},
  };
  const QString autoGainConfigJson = QString::fromUtf8(QJsonDocument(autoGainConfig).toJson(QJsonDocument::Compact));
  const QString supportedDomainsJson = QString::fromUtf8(
      QJsonDocument(QJsonArray::fromStringList(dalinira::audio::supportedAudioPlatformDomains()))
          .toJson(QJsonDocument::Compact));
  const QString policyModeStr = policyMode_ == dalinira::PerformancePolicyMode::MemorySaver
      ? QStringLiteral("memory_saver")
      : (policyMode_ == dalinira::PerformancePolicyMode::MaximumPerformance
             ? QStringLiteral("maximum_performance")
             : QStringLiteral("balanced"));

  return QStringLiteral(R"JS(
(async function daliniraDaliBootstrap() {
  try {
    // Injected at document start (WebKit) the document has no elements yet:
    // run once the DOM exists.
    if (document.readyState === 'loading' || !document.documentElement) {
      document.addEventListener('DOMContentLoaded', function() { daliniraDaliBootstrap(); }, { once: true });
      return { ok: true, deferred: true, moduleLoaded: false, enabled: %3,
               mediaCount: 0, sampleRates: [], contextStates: [] };
    }
    const supportedDomains = %27;
    const protocol = String(location.protocol || '').toLowerCase();
    const hostname = String(location.hostname || '').toLowerCase();
    // file: documents receive this bootstrap only when the native view is the
    // root-confined DaliNira local player (see isAudioEligibleView()).
    const supported = protocol === 'file:' || ((protocol === 'http:' || protocol === 'https:')
      && supportedDomains.some((domain) => hostname === domain || hostname.endsWith('.' + domain)));
    if (!supported) {
      return { ok: true, supported: false, moduleLoaded: false, enabled: false,
               mediaCount: 0, sampleRates: [], contextStates: [] };
    }
    const outputModuleCode = %1;
    const eqModuleCode = %2;
    const compressorModuleCode = %12;
    const limiterModuleCode = %15;
    const bassEnhancerModuleCode = %17;
    const autoGainModuleCode = %19;
    const enabled = %3;
    const outputPreampDb = %4;
    const bands = %5;
    const eqActive = %6;
    const bassDb = %7;
    const midDb = %8;
    const trebleDb = %9;
    const toneActive = %10;
    const stereoExpanderPercent = %11;
    const balance = %13;
    const spatialActive = %14;
    const reverb = %16;
    const compressor = %18;
    const limiter = %20;
    const bassEnhancer = %21;
    const autoGain = %22;
    const policyMode = '%23';
    const panelVisible = %24;
    const activeSubpanel = '%25';

    const root = window.__DALINIRA_WEB_DALI_OUTPUT__ = window.__DALINIRA_WEB_DALI_OUTPUT__ || {};
    root.graphs = root.graphs || new WeakMap();
    root.graphList = root.graphList || new Set();

    root.dbToGain = root.dbToGain || function(db) { return Math.pow(10, (Number(db) || 0) / 20); };
    root.smoothParam = root.smoothParam || function(param, value, ctx, rampSeconds) {
      if (!param || !ctx || !Number.isFinite(Number(value))) return;
      const now = Number(ctx.currentTime) || 0;
      const target = Number(value);
      try {
        if (typeof param.cancelAndHoldAtTime === 'function') param.cancelAndHoldAtTime(now);
        else {
          const current = Number(param.value);
          param.cancelScheduledValues(now);
          param.setValueAtTime(Number.isFinite(current) ? current : target, now);
        }
        param.setTargetAtTime(target, now, Math.max(0.006, Number(rampSeconds) || 0.02));
      } catch (error) {
        try { param.value = target; }
        catch (fallbackError) {
          root.lastRuntimeError = 'Web Audio parameter apply failed: '
            + String(fallbackError && fallbackError.message ? fallbackError.message : (error && error.message ? error.message : error));
          console.error('[DaliNira DSP] ' + root.lastRuntimeError);
        }
      }
    };

    root.makeBassSaturationCurve = root.makeBassSaturationCurve || function(amountPercent) {
      const amount = Math.max(0, Math.min(100, Number(amountPercent) || 0));
      const norm = amount / 100;
      const k = 1 + amount * 0.45;
      const curve = new Float32Array(1024);
      for (let index = 0; index < curve.length; index += 1) {
        const x = index * 2 / (curve.length - 1) - 1;
        const soft = Math.tanh(k * x) / Math.tanh(k);
        const blend = 0.02 + norm * 0.12;
        curve[index] = x * (1 - blend) + soft * blend;
      }
      return curve;
    };

    root.pruneStaleGraphs = root.pruneStaleGraphs || function() {
      if (!root.graphList) return;
      const toRemove = [];
      for (const graph of root.graphList) {
        if (!graph || !graph.element || !graph.element.isConnected || !document.documentElement.contains(graph.element)) {
          toRemove.push(graph);
        }
      }
      for (const dead of toRemove) {
        root.disconnectEqGraph(dead);
        root.graphList.delete(dead);
      }
    };

    if (!root.buildOutputGraph && outputModuleCode) {
      const module = { exports: {} };
      const exports = module.exports;
      %26
      root.buildOutputGraph = module.exports.buildGraph || module.exports.buildGraphSafe || null;
    }
    if (!root.eqTemplate && eqModuleCode) {
      const bandRe = /new BiquadFilterNode\(audioContext, \{ type: '([^']+)', frequency: ([^,]+), gain: ([^,]+), Q: ([^ }]+) \}\)/g;
      const allBiquads = [];
      let match;
      while ((match = bandRe.exec(eqModuleCode)) !== null) {
        const stage = { type: String(match[1]), frequency: Number(match[2]), gain: Number(match[3]), q: Number(match[4]) };
        if (Number.isFinite(stage.frequency) && Number.isFinite(stage.gain) && Number.isFinite(stage.q)) allBiquads.push(stage);
      }
      const eqBands = allBiquads.filter(function(stage) { return stage.type === 'peaking'; });
      const gainMatch = eqModuleCode.match(/new GainNode\(audioContext, \{ gain: ([^ }]+) \}\)/);
      const limiterMatch = eqModuleCode.match(/new DynamicsCompressorNode\(audioContext, \{ threshold: ([^,]+), ratio: ([^,]+), attack: ([^,]+), release: ([^,]+), knee: ([^ }]+) \}\)/);
      const limiter = limiterMatch ? {
        threshold: Number(limiterMatch[1]), ratio: Number(limiterMatch[2]), attack: Number(limiterMatch[3]),
        release: Number(limiterMatch[4]), knee: Number(limiterMatch[5])
      } : null;
      if (eqBands.length === 32 && gainMatch && limiter && Object.values(limiter).every(Number.isFinite)) {
        root.eqTemplate = { preampGain: Number(gainMatch[1]), bands: eqBands, limiter: limiter };
      }
    }
    if (!root.compressorTemplate && compressorModuleCode) {
      const compressorMatch = compressorModuleCode.match(/new DynamicsCompressorNode\(audioContext, \{ threshold: ([^,]+), ratio: ([^,]+), attack: ([^,]+), release: ([^,]+), knee: ([^ }]+) \}\)/);
      const makeupMatch = compressorModuleCode.match(/new GainNode\(audioContext, \{ gain: ([^ }]+) \}\)/);
      const parsed = compressorMatch ? {
        threshold: Number(compressorMatch[1]), ratio: Number(compressorMatch[2]), attack: Number(compressorMatch[3]),
        release: Number(compressorMatch[4]), knee: Number(compressorMatch[5]),
        makeupGain: makeupMatch ? Number(makeupMatch[1]) : NaN
      } : null;
      if (parsed && Object.values(parsed).every(Number.isFinite)) root.compressorTemplate = parsed;
      else {
        root.compressorTemplateError = 'DALI dynamic compressor template could not be parsed';
        console.error('[DaliNira DSP] ' + root.compressorTemplateError);
      }
    }
    if (!root.userLimiterTemplate && limiterModuleCode) {
      const limiterMatch = limiterModuleCode.match(/new DynamicsCompressorNode\(audioContext, \{ threshold: ([^,]+), ratio: ([^,]+), attack: ([^,]+), release: ([^,]+), knee: ([^ }]+) \}\)/);
      const parsed = limiterMatch ? {
        threshold: Number(limiterMatch[1]), ratio: Number(limiterMatch[2]), attack: Number(limiterMatch[3]),
        release: Number(limiterMatch[4]), knee: Number(limiterMatch[5])
      } : null;
      if (parsed && Object.values(parsed).every(Number.isFinite)) root.userLimiterTemplate = parsed;
      else {
        root.userLimiterTemplateError = 'DALI user Limiter template could not be parsed';
        console.error('[DaliNira DSP] ' + root.userLimiterTemplateError);
      }
    }
    if (!root.bassEnhancerTemplate && bassEnhancerModuleCode) {
      const biquadRe = /new BiquadFilterNode\(audioContext, \{ type: '([^']+)', frequency: ([^,]+), gain: ([^,]+), Q: ([^ }]+) \}\)/g;
      const biquads = [];
      let bassMatch;
      while ((bassMatch = biquadRe.exec(bassEnhancerModuleCode)) !== null) {
        const stage = { type: String(bassMatch[1]), frequency: Number(bassMatch[2]),
                        gain: Number(bassMatch[3]), q: Number(bassMatch[4]) };
        if (Number.isFinite(stage.frequency) && Number.isFinite(stage.gain) && Number.isFinite(stage.q)) biquads.push(stage);
      }
      const shelf = biquads.find(function(stage) { return stage.type === 'lowshelf'; });
      const peaks = biquads.filter(function(stage) { return stage.type === 'peaking'; });
      const gainMatch = bassEnhancerModuleCode.match(/new GainNode\(audioContext, \{ gain: ([^ }]+) \}\)/);
      const compressorRe = /new DynamicsCompressorNode\(audioContext, \{ threshold: ([^,]+), ratio: ([^,]+), attack: ([^,]+), release: ([^,]+), knee: ([^ }]+) \}\)/g;
      const compressors = [];
      while ((bassMatch = compressorRe.exec(bassEnhancerModuleCode)) !== null) {
        compressors.push({ threshold: Number(bassMatch[1]), ratio: Number(bassMatch[2]),
                           attack: Number(bassMatch[3]), release: Number(bassMatch[4]), knee: Number(bassMatch[5]) });
      }
      const limiterProfile = compressors.length ? compressors[compressors.length - 1] : null;
      const parsed = shelf && peaks.length >= 2 && gainMatch && limiterProfile ? {
        preampGain: Number(gainMatch[1]), lowShelf: shelf, subPeak: peaks[0], upperPeak: peaks[1],
        limiterCeilingDb: Number(limiterProfile.threshold)
      } : null;
      if (parsed && Number.isFinite(parsed.preampGain) && Number.isFinite(parsed.limiterCeilingDb)) {
        root.bassEnhancerTemplate = parsed;
      } else {
        root.bassEnhancerTemplateError = 'DALI Bass Enhancer template could not be parsed';
        console.error('[DaliNira DSP] ' + root.bassEnhancerTemplateError);
      }
    }
    if (!root.autoGainTemplate && autoGainModuleCode) {
      const validName = autoGainModuleCode.indexOf('Web Auto Gain Normalize v1') >= 0;
      const hasGainStage = /new GainNode\(audioContext, \{ gain: [^ }]+ \}\)/.test(autoGainModuleCode);
      if (validName && hasGainStage) {
        root.autoGainTemplate = {
          detector: 'rms-dbfs', fftSize: 2048, detectorSmoothing: 0.22,
          envelopeRiseMs: 35, envelopeFallMs: 240, minimumGainDb: -12,
          controlIntervalMs: 60
        };
      } else {
        root.autoGainTemplateError = 'DALI Auto Gain template could not be parsed';
        console.error('[DaliNira DSP] ' + root.autoGainTemplateError);
      }
    }

    root.disconnectEqGraph = root.disconnectEqGraph || function(graph) {
      if (!graph) return;
      if (graph.autoGainTimer) {
        try { clearInterval(graph.autoGainTimer); } catch (_) {}
        graph.autoGainTimer = null;
      }
      try { if (graph.outputGraph && typeof graph.outputGraph.disconnect === 'function') graph.outputGraph.disconnect(); } catch (_) {}
      const nodes = graph.eqNodes || [];
      for (const node of nodes) { try { node.disconnect(); } catch (_) {} }
      graph.eqNodes = [];
      graph.outputGraph = null;
      graph.eqBandNodes = [];
      graph.safetyLimiter = null;
      graph.compressorNode = null;
      graph.compressorMakeupGain = null;
      graph.compressorEnabled = false;
      graph.autoGainAnalyser = null;
      graph.autoGainNode = null;
      graph.autoGainBuffer = null;
      graph.autoGainEnabled = false;
      graph.userLimiterInputGain = null;
      graph.userLimiterNode = null;
      graph.userLimiterMeterAnalyser = null;
      graph.userLimiterEnabled = false;
      graph.bassEnhancerInputGain = null;
      graph.bassEnhancerDryGain = null;
      graph.bassEnhancerFilter = null;
      graph.bassEnhancerSubPeak = null;
      graph.bassEnhancerPresencePeak = null;
      graph.bassEnhancerHarmonicLowpass = null;
      graph.bassEnhancerHarmonicsDrive = null;
      graph.bassEnhancerSaturator = null;
      graph.bassEnhancerWetGain = null;
      graph.bassEnhancerSum = null;
      graph.bassEnhancerBodyDip = null;
      graph.bassEnhancerOutputTrim = null;
      graph.bassEnhancerEnabled = false;
      graph.reverbInputGain = null;
      graph.reverbDryGain = null;
      graph.reverbDelay = null;
      graph.reverbLowpass = null;
      graph.reverbFeedbackGain = null;
      graph.reverbWetGain = null;
      graph.reverbSum = null;
      graph.reverbEnabled = false;
      graph.bass = null;
      graph.mid = null;
      graph.treble = null;
      graph.toneTrim = null;
      graph.stereoSideWidth = null;
      graph.balanceGainL = null;
      graph.balanceGainR = null;
      graph.moduleNodes = {};
    };

    root.needsGraphRebuild = root.needsGraphRebuild || function(graph, cfg) {
      if (!graph || !graph.graph || !graph.eqNodes || graph.eqNodes.length === 0) return true;
      const bands = (cfg && cfg.bands) || [];
      const eqActive = (cfg && !!cfg.eqActive) || bands.some(function(b) { return Math.abs(Number(b) || 0) > 0.01; });
      const hasEqBands = !!(graph.eqBandNodes && graph.eqBandNodes.length > 0);
      if (eqActive !== hasEqBands) return true;

      const compressorActive = !!(cfg && cfg.compressor && cfg.compressor.enabled);
      if (compressorActive !== !!graph.compressorEnabled) return true;

      const limiterActive = !!(cfg && cfg.limiter && cfg.limiter.enabled);
      if (limiterActive !== !!graph.userLimiterEnabled) return true;

      const bassActive = !!(cfg && cfg.bassEnhancer && cfg.bassEnhancer.enabled);
      if (bassActive !== !!graph.bassEnhancerEnabled) return true;

      const reverbActive = !!(cfg && cfg.reverb && cfg.reverb.enabled);
      if (reverbActive !== !!graph.reverbEnabled) return true;

      const autoGainActive = !!(cfg && cfg.autoGain && cfg.autoGain.enabled);
      if (autoGainActive !== !!graph.autoGainEnabled) return true;

      const bassDb = Number(cfg && cfg.bassDb) || 0;
      const midDb = Number(cfg && cfg.midDb) || 0;
      const trebleDb = Number(cfg && cfg.trebleDb) || 0;
      const toneActive = (cfg && !!cfg.toneActive) || (Math.abs(bassDb) > 0.01 || Math.abs(midDb) > 0.01 || Math.abs(trebleDb) > 0.01);
      const hasTone = !!graph.bass;
      if (toneActive !== hasTone) return true;

      const stereoPercent = Number(cfg && cfg.stereoExpanderPercent) || 100;
      const balance = Number(cfg && cfg.balance) || 0;
      const spatialActive = (cfg && !!cfg.spatialActive) || (Math.abs(stereoPercent - 100) > 0.5 || Math.abs(balance) > 0.5);
      const hasSpatial = !!graph.stereoSideWidth;
      if (spatialActive !== hasSpatial) return true;

      return false;
    };

    root.computeAutoGainStep = root.computeAutoGainStep || function(state, inputDb, stepMs) {
      const safeStepMs = Math.max(10, Number(stepMs) || 60);
      const level = Number.isFinite(Number(inputDb)) ? Number(inputDb) : -120;
      if (!Number.isFinite(state.envDb)) state.envDb = level;
      const envTau = Math.max(0.01, (level > state.envDb ? 35 : 240) / 1000);
      const envAlpha = 1 - Math.exp(-(safeStepMs / 1000) / envTau);
      state.envDb += (level - state.envDb) * envAlpha;
      const targetGainDb = Math.max(-12, Math.min(state.maxGainDb, state.targetDbfs - state.envDb));
      const attackMs = state.speed === 'fast' ? 45 : (state.speed === 'slow' ? 180 : 95);
      const releaseMs = state.speed === 'fast' ? 220 : (state.speed === 'slow' ? 520 : 340);
      const gainTau = Math.max(0.01, (targetGainDb > state.currentGainDb ? attackMs : releaseMs) / 1000);
      const gainAlpha = 1 - Math.exp(-(safeStepMs / 1000) / gainTau);
      state.currentGainDb += (targetGainDb - state.currentGainDb) * gainAlpha;
      state.currentGainDb = Math.max(-12, Math.min(state.maxGainDb, state.currentGainDb));
      return state.currentGainDb;
    };

    root.runAutoGainStep = root.runAutoGainStep || function(graph) {
      if (!graph || !graph.ctx || !graph.autoGainNode || !graph.autoGainState || !graph.autoGainAnalyser) return;
      const state = graph.autoGainState;
      if (graph.bypass || !state.enabled) {
        state.currentGainDb = 0;
        state.envDb = -120;
        if (Math.abs(Number(state.lastAppliedGainDb) || 0) > 0.04) {
          root.smoothParam(graph.autoGainNode.gain, 1, graph.ctx, 0.014);
          state.lastAppliedGainDb = 0;
        }
        return;
      }
      try {
        graph.autoGainAnalyser.getByteTimeDomainData(graph.autoGainBuffer);
      } catch (error) {
        root.lastRuntimeError = 'Auto Gain detector read failed: ' + String(error && error.message ? error.message : error);
        console.error('[DaliNira DSP] ' + root.lastRuntimeError);
        state.runtimeAvailable = false;
        state.enabled = false;
        graph.autoGainEnabled = false;
        graph.autoGainBypassed = true;
        root.smoothParam(graph.autoGainNode.gain, 1, graph.ctx, 0.014);
        return;
      }
      let sumSq = 0;
      for (let index = 0; index < graph.autoGainBuffer.length; index += 1) {
        const sample = (graph.autoGainBuffer[index] - 128) / 128;
        sumSq += sample * sample;
      }
      const rms = Math.sqrt(sumSq / Math.max(1, graph.autoGainBuffer.length));
      const inputDb = 20 * Math.log10(Math.max(1e-6, rms));
      state.lastInputDb = inputDb;
      state.detectorReadCount = (Number(state.detectorReadCount) || 0) + 1;
      root.computeAutoGainStep(state, inputDb, state.controlIntervalMs);
      if (Math.abs(state.currentGainDb - state.lastAppliedGainDb) > 0.04) {
        root.smoothParam(graph.autoGainNode.gain, root.dbToGain(state.currentGainDb), graph.ctx, 0.016);
        state.lastAppliedGainDb = state.currentGainDb;
      }
    };

    root.buildEqGraph = root.buildEqGraph || function(graph, cfg) {
      const ctx = graph.ctx;
      const template = root.eqTemplate;
      if (!template) return;
      graph.eqInput = new GainNode(ctx, { gain: 1 });
      graph.outputGraph = root.buildOutputGraph(ctx, graph.source, graph.eqInput);
      const nodes = [graph.eqInput];
      const eqPreamp = new GainNode(ctx, { gain: Number(template.preampGain) || 1 });
      nodes.push(eqPreamp);
      graph.eqInput.connect(eqPreamp);
      let previous = eqPreamp;

      // 1. Equalizer 32-Band - Lazy allocation
      const bands = (cfg && cfg.bands) || [];
      const eqIsActive = (cfg && !!cfg.eqActive) || bands.some(function(b) { return Math.abs(Number(b) || 0) > 0.01; });
      if (eqIsActive && template.bands && template.bands.length > 0) {
        graph.eqBandNodes = template.bands.map(function(stage, idx) {
          const gainVal = Number(bands[idx]) || 0;
          const node = new BiquadFilterNode(ctx, { type: stage.type, frequency: stage.frequency, gain: gainVal, Q: stage.q });
          previous.connect(node);
          previous = node;
          nodes.push(node);
          return node;
        });
      } else {
        graph.eqBandNodes = [];
      }

      // 2. Fixed Stage-2 safety guard
      const safetyLimiter = new DynamicsCompressorNode(ctx, template.limiter);
      nodes.push(safetyLimiter);
      previous.connect(safetyLimiter);
      previous = safetyLimiter;
      graph.safetyLimiter = safetyLimiter;

      // 3. Dynamic Compressor - Lazy allocation
      const compressorCfg = cfg && cfg.compressor;
      if (compressorCfg && compressorCfg.enabled && root.compressorTemplate) {
        const cTemplate = root.compressorTemplate;
        const compressorNode = new DynamicsCompressorNode(ctx, {
          threshold: compressorCfg.thresholdDb !== undefined ? compressorCfg.thresholdDb : cTemplate.threshold,
          ratio: compressorCfg.ratio !== undefined ? compressorCfg.ratio : cTemplate.ratio,
          attack: (compressorCfg.attackMs !== undefined ? compressorCfg.attackMs : 10) / 1000,
          release: (compressorCfg.releaseMs !== undefined ? compressorCfg.releaseMs : 100) / 1000,
          knee: compressorCfg.kneeDb !== undefined ? compressorCfg.kneeDb : cTemplate.knee
        });
        const compressorMakeupGain = new GainNode(ctx, {
          gain: root.dbToGain(compressorCfg.makeupDb !== undefined ? compressorCfg.makeupDb : (Number(cTemplate.makeupGain) || 0))
        });
        nodes.push(compressorNode, compressorMakeupGain);
        previous.connect(compressorNode);
        compressorNode.connect(compressorMakeupGain);
        previous = compressorMakeupGain;
        graph.compressorNode = compressorNode;
        graph.compressorMakeupGain = compressorMakeupGain;
        graph.compressorEnabled = true;
      } else {
        graph.compressorNode = null;
        graph.compressorMakeupGain = null;
        graph.compressorEnabled = false;
      }

      // 4. Tone controls (Bass, Mid, Treble) - Lazy allocation
      const bassDb = Number(cfg && cfg.bassDb) || 0;
      const midDb = Number(cfg && cfg.midDb) || 0;
      const trebleDb = Number(cfg && cfg.trebleDb) || 0;
      const toneIsActive = (cfg && !!cfg.toneActive) || (Math.abs(bassDb) > 0.01 || Math.abs(midDb) > 0.01 || Math.abs(trebleDb) > 0.01);
      if (toneIsActive) {
        const bass = new BiquadFilterNode(ctx, { type: 'lowshelf', frequency: 100, gain: bassDb, Q: 0.70 });
        const mid = new BiquadFilterNode(ctx, { type: 'peaking', frequency: 1200, gain: midDb, Q: 1.0 });
        const treble = new BiquadFilterNode(ctx, { type: 'highshelf', frequency: 10000, gain: trebleDb, Q: 0.707 });
        const toneTrim = new GainNode(ctx, { gain: root.dbToGain(-Math.max(0, bassDb) * 0.333) });
        nodes.push(bass, mid, treble, toneTrim);
        previous.connect(bass); bass.connect(mid); mid.connect(treble); treble.connect(toneTrim);
        previous = toneTrim;
        graph.bass = bass; graph.mid = mid; graph.treble = treble; graph.toneTrim = toneTrim;
      } else {
        graph.bass = null; graph.mid = null; graph.treble = null; graph.toneTrim = null;
      }

      // 5. Stage-6 Bass Enhancer - Lazy allocation
      const bassCfg = cfg && cfg.bassEnhancer;
      if (bassCfg && bassCfg.enabled && root.bassEnhancerTemplate) {
        const bTemplate = root.bassEnhancerTemplate;
        const bParams = root.computeBassEnhancerParams({
          enabled: true, frequencyHz: bassCfg.frequencyHz, gainDb: bassCfg.gainDb,
          harmonicsPercent: bassCfg.harmonicsPercent, width: bassCfg.width, mixPercent: bassCfg.mixPercent,
          template: bTemplate
        });
        const bassEnhancerInputGain = new GainNode(ctx, { gain: bParams.inputGain });
        const bassEnhancerDryGain = new GainNode(ctx, { gain: bParams.dryGain });
        const bassEnhancerFilter = new BiquadFilterNode(ctx, {
          type: 'lowshelf', frequency: bParams.frequencyHz, gain: bParams.shelfGain, Q: bTemplate.lowShelf.q
        });
        const bassEnhancerSubPeak = new BiquadFilterNode(ctx, {
          type: 'peaking', frequency: bParams.subFrequency, gain: bParams.subGain, Q: bParams.subQ
        });
        const bassEnhancerPresencePeak = new BiquadFilterNode(ctx, {
          type: 'peaking', frequency: bParams.presenceFrequency, gain: bParams.presenceGain, Q: bParams.presenceQ
        });
        const bassEnhancerHarmonicLowpass = new BiquadFilterNode(ctx, {
          type: 'lowpass', frequency: bParams.harmonicCutoff, Q: 0.707
        });
        const bassEnhancerHarmonicsDrive = new GainNode(ctx, { gain: bParams.harmonicDrive });
        const bassEnhancerSaturator = new WaveShaperNode(ctx, {
          curve: root.makeBassSaturationCurve(bParams.curveAmount), oversample: '2x'
        });
        const bassEnhancerWetGain = new GainNode(ctx, { gain: bParams.wetGain });
        const bassEnhancerSum = new GainNode(ctx, { gain: 1 });
        const bassEnhancerBodyDip = new BiquadFilterNode(ctx, {
          type: 'peaking', frequency: bParams.dipFrequency, gain: bParams.dipGain, Q: bParams.dipQ
        });
        const bassEnhancerOutputTrim = new GainNode(ctx, { gain: bParams.outputGain });

        nodes.push(bassEnhancerInputGain, bassEnhancerDryGain, bassEnhancerFilter,
                   bassEnhancerSubPeak, bassEnhancerPresencePeak, bassEnhancerHarmonicLowpass,
                   bassEnhancerHarmonicsDrive, bassEnhancerSaturator, bassEnhancerWetGain, bassEnhancerSum,
                   bassEnhancerBodyDip, bassEnhancerOutputTrim);

        previous.connect(bassEnhancerInputGain);
        bassEnhancerInputGain.connect(bassEnhancerFilter);
        bassEnhancerFilter.connect(bassEnhancerSubPeak);
        bassEnhancerSubPeak.connect(bassEnhancerPresencePeak);
        bassEnhancerPresencePeak.connect(bassEnhancerDryGain); bassEnhancerDryGain.connect(bassEnhancerSum);
        bassEnhancerPresencePeak.connect(bassEnhancerHarmonicLowpass);
        bassEnhancerHarmonicLowpass.connect(bassEnhancerHarmonicsDrive);
        bassEnhancerHarmonicsDrive.connect(bassEnhancerSaturator);
        bassEnhancerSaturator.connect(bassEnhancerWetGain); bassEnhancerWetGain.connect(bassEnhancerSum);
        bassEnhancerSum.connect(bassEnhancerBodyDip); bassEnhancerBodyDip.connect(bassEnhancerOutputTrim);
        previous = bassEnhancerOutputTrim;

        graph.bassEnhancerInputGain = bassEnhancerInputGain; graph.bassEnhancerDryGain = bassEnhancerDryGain;
        graph.bassEnhancerFilter = bassEnhancerFilter; graph.bassEnhancerSubPeak = bassEnhancerSubPeak;
        graph.bassEnhancerPresencePeak = bassEnhancerPresencePeak;
        graph.bassEnhancerHarmonicLowpass = bassEnhancerHarmonicLowpass;
        graph.bassEnhancerHarmonicsDrive = bassEnhancerHarmonicsDrive;
        graph.bassEnhancerSaturator = bassEnhancerSaturator; graph.bassEnhancerWetGain = bassEnhancerWetGain;
        graph.bassEnhancerSum = bassEnhancerSum; graph.bassEnhancerBodyDip = bassEnhancerBodyDip;
        graph.bassEnhancerOutputTrim = bassEnhancerOutputTrim;
        graph.bassEnhancerState = { curveAmount: bParams.curveAmount };
        graph.bassEnhancerEnabled = true;
      } else {
        graph.bassEnhancerInputGain = null; graph.bassEnhancerFilter = null;
        graph.bassEnhancerEnabled = false;
      }

      // 6. Reverb - Lazy allocation
      const reverbCfg = cfg && cfg.reverb;
      if (reverbCfg && reverbCfg.enabled) {
        const roomSizeMs = Math.max(0, Math.min(3000, Number(reverbCfg.roomSizeMs) || 1000));
        const damping = Math.max(0, Math.min(1, Number(reverbCfg.damping) || 0.5));
        const wetDryDb = Math.max(-96, Math.min(0, Number(reverbCfg.wetDryDb) || -10));
        const hfRatio = Math.max(0.001, Math.min(0.999, Number(reverbCfg.hfRatio) || 0.7));
        const inputGainDb = Math.max(-96, Math.min(12, Number(reverbCfg.inputGainDb) || 0));

        const reverbInputGain = new GainNode(ctx, { gain: root.dbToGain(inputGainDb) });
        const wetVal = root.dbToGain(wetDryDb);
        const reverbDryGain = new GainNode(ctx, { gain: Math.max(0.55, Math.min(1, 1 - wetVal * 0.35)) });
        const reverbDelay = new DelayNode(ctx, { maxDelayTime: 0.35, delayTime: Math.max(0.01, Math.min(0.28, (roomSizeMs / 1000) * 0.09)) });
        const reverbLowpass = new BiquadFilterNode(ctx, { type: 'lowpass', frequency: Math.max(900, Math.min(18000, 900 + hfRatio * 12000)), Q: 0.707 });
        const reverbFeedbackGain = new GainNode(ctx, { gain: Math.max(0.15, Math.min(0.78, 0.18 + (1 - damping) * 0.55)) });
        const reverbWetGain = new GainNode(ctx, { gain: wetVal });
        const reverbSum = new GainNode(ctx, { gain: 1 });

        nodes.push(reverbInputGain, reverbDryGain, reverbDelay, reverbLowpass, reverbFeedbackGain, reverbWetGain, reverbSum);
        previous.connect(reverbInputGain);
        reverbInputGain.connect(reverbDryGain); reverbDryGain.connect(reverbSum);
        reverbInputGain.connect(reverbDelay); reverbDelay.connect(reverbLowpass); reverbLowpass.connect(reverbWetGain);
        reverbWetGain.connect(reverbSum); reverbLowpass.connect(reverbFeedbackGain); reverbFeedbackGain.connect(reverbDelay);
        previous = reverbSum;

        graph.reverbInputGain = reverbInputGain; graph.reverbDryGain = reverbDryGain; graph.reverbDelay = reverbDelay;
        graph.reverbLowpass = reverbLowpass; graph.reverbFeedbackGain = reverbFeedbackGain;
        graph.reverbWetGain = reverbWetGain; graph.reverbSum = reverbSum;
        graph.reverbEnabled = true;
      } else {
        graph.reverbInputGain = null; graph.reverbDelay = null;
        graph.reverbEnabled = false;
      }

      // 7. Stereo Expander & Balance - Lazy allocation
      const stereoPercent = Number(cfg && cfg.stereoExpanderPercent) || 100;
      const balanceVal = Number(cfg && cfg.balance) || 0;
      const spatialIsActive = (cfg && !!cfg.spatialActive) || (Math.abs(stereoPercent - 100) > 0.5 || Math.abs(balanceVal) > 0.5);
      if (spatialIsActive) {
        const stereoSplit = new ChannelSplitterNode(ctx, { numberOfOutputs: 2 });
        const midL = new GainNode(ctx, { gain: 0.5 });
        const midR = new GainNode(ctx, { gain: 0.5 });
        const midSum = new GainNode(ctx, { gain: 1 });
        const sideL = new GainNode(ctx, { gain: 0.5 });
        const sideRInverted = new GainNode(ctx, { gain: -0.5 });
        const sideSum = new GainNode(ctx, { gain: 1 });
        const stereoSideWidth = new GainNode(ctx, { gain: Math.max(0, Math.min(2, stereoPercent / 100)) });
        const midToLeft = new GainNode(ctx, { gain: 1 });
        const midToRight = new GainNode(ctx, { gain: 1 });
        const sideToLeft = new GainNode(ctx, { gain: 1 });
        const sideToRight = new GainNode(ctx, { gain: -1 });
        const stereoMerge = new ChannelMergerNode(ctx, { numberOfInputs: 2 });
        const split = new ChannelSplitterNode(ctx, { numberOfOutputs: 2 });
        const normBal = Math.max(-1, Math.min(1, balanceVal / 100));
        const gainL = new GainNode(ctx, { gain: normBal > 0 ? 1 - normBal : 1 });
        const gainR = new GainNode(ctx, { gain: normBal < 0 ? 1 + normBal : 1 });
        const merge = new ChannelMergerNode(ctx, { numberOfInputs: 2 });

        nodes.push(stereoSplit, midL, midR, midSum, sideL, sideRInverted, sideSum,
                   stereoSideWidth, midToLeft, midToRight, sideToLeft, sideToRight, stereoMerge, split, gainL, gainR, merge);

        previous.connect(stereoSplit);
        stereoSplit.connect(midL, 0); stereoSplit.connect(midR, 1); midL.connect(midSum); midR.connect(midSum);
        stereoSplit.connect(sideL, 0); stereoSplit.connect(sideRInverted, 1); sideL.connect(sideSum); sideRInverted.connect(sideSum);
        midSum.connect(midToLeft); midSum.connect(midToRight); sideSum.connect(stereoSideWidth);
        stereoSideWidth.connect(sideToLeft); stereoSideWidth.connect(sideToRight);
        midToLeft.connect(stereoMerge, 0, 0); sideToLeft.connect(stereoMerge, 0, 0);
        midToRight.connect(stereoMerge, 0, 1); sideToRight.connect(stereoMerge, 0, 1);
        stereoMerge.connect(split);
        split.connect(gainL, 0); split.connect(gainR, 1); gainL.connect(merge, 0, 0); gainR.connect(merge, 0, 1);
        previous = merge;

        graph.stereoSideWidth = stereoSideWidth;
        graph.balanceGainL = gainL; graph.balanceGainR = gainR;
      } else {
        graph.stereoSideWidth = null; graph.balanceGainL = null; graph.balanceGainR = null;
      }

      // 8. Auto Gain - Lazy allocation & Timer Gating
      const autoGainCfg = cfg && cfg.autoGain;
      if (autoGainCfg && autoGainCfg.enabled && root.autoGainTemplate) {
        const autoGainTemplate = root.autoGainTemplate;
        const autoGainAnalyser = new AnalyserNode(ctx, {
          fftSize: autoGainTemplate.fftSize,
          smoothingTimeConstant: autoGainTemplate.detectorSmoothing,
          minDecibels: -96,
          maxDecibels: -12
        });
        const autoGainNode = new GainNode(ctx, { gain: 1 });
        nodes.push(autoGainNode);
        previous.connect(autoGainNode);
        previous = autoGainNode;
        graph.source.connect(autoGainAnalyser);

        graph.autoGainAnalyser = autoGainAnalyser;
        graph.autoGainNode = autoGainNode;
        graph.autoGainBuffer = new Uint8Array(autoGainAnalyser.fftSize);
        graph.autoGainState = {
          enabled: true, targetDbfs: autoGainCfg.targetDbfs !== undefined ? autoGainCfg.targetDbfs : -14,
          maxGainDb: autoGainCfg.maxGainDb !== undefined ? autoGainCfg.maxGainDb : 12,
          speed: autoGainCfg.speed || 'medium', preset: autoGainCfg.preset || 'balanced',
          currentGainDb: 0, envDb: -120, lastAppliedGainDb: 0, lastInputDb: -120,
          detectorReadCount: 0, controlIntervalMs: autoGainTemplate.controlIntervalMs,
          runtimeAvailable: true
        };
        graph.autoGainEnabled = true;
        graph.autoGainTimer = setInterval(function() { root.runAutoGainStep(graph); },
                                          autoGainTemplate.controlIntervalMs);
      } else {
        graph.autoGainAnalyser = null;
        graph.autoGainNode = null;
        graph.autoGainBuffer = null;
        graph.autoGainState = { enabled: false };
        graph.autoGainEnabled = false;
        graph.autoGainTimer = null;
      }

      // 9. User Limiter - Lazy allocation
      const limiterCfg = cfg && cfg.limiter;
      if (limiterCfg && limiterCfg.enabled && root.userLimiterTemplate) {
        const userLimiterTemplate = root.userLimiterTemplate;
        const userLimiterInputGain = new GainNode(ctx, { gain: root.dbToGain(limiterCfg.gainDb || 0) });
        const userLimiterNode = new DynamicsCompressorNode(ctx, {
          threshold: limiterCfg.ceilingDb !== undefined ? limiterCfg.ceilingDb : userLimiterTemplate.threshold,
          ratio: userLimiterTemplate.ratio || 20,
          attack: (limiterCfg.lookaheadMs !== undefined ? Math.max(0.5, limiterCfg.lookaheadMs) : 5) / 1000,
          release: (limiterCfg.releaseMs !== undefined ? Math.max(5, limiterCfg.releaseMs) : 50) / 1000,
          knee: userLimiterTemplate.knee || 0
        });
        nodes.push(userLimiterInputGain, userLimiterNode);
        previous.connect(userLimiterInputGain);
        userLimiterInputGain.connect(userLimiterNode);
        previous = userLimiterNode;

        graph.userLimiterInputGain = userLimiterInputGain;
        graph.userLimiterNode = userLimiterNode;
        graph.userLimiterEnabled = true;
      } else {
        graph.userLimiterInputGain = null;
        graph.userLimiterNode = null;
        graph.userLimiterEnabled = false;
      }

      // 10. Runtime Gain & Master Output
      graph.runtimeGain = graph.runtimeGain || new GainNode(ctx, { gain: root.dbToGain(cfg && cfg.outputPreampDb) });
      const wetGain = new GainNode(ctx, { gain: 1 });
      const dryGain = new GainNode(ctx, { gain: 0 });
      nodes.push(graph.runtimeGain, wetGain, dryGain);

      previous.connect(graph.runtimeGain);
      graph.runtimeGain.connect(wetGain);
      wetGain.connect(ctx.destination);

      graph.source.connect(dryGain);
      dryGain.connect(ctx.destination);

      graph.eqNodes = nodes;
      graph.wetGain = wetGain;
      graph.dryGain = dryGain;
      graph.graph = { disconnect: function() { root.disconnectEqGraph(graph); } };
    };

    root.computeBassEnhancerParams = root.computeBassEnhancerParams || function(input) {
      const clamp = function(value, minimum, maximum, fallback) {
        const numeric = Number(value);
        return Math.max(minimum, Math.min(maximum, Number.isFinite(numeric) ? numeric : fallback));
      };
      const enabled = !!input.enabled;
      const frequencyHz = clamp(input.frequencyHz, 35, 110, 68);
      const gainDb = clamp(input.gainDb, 0, 18, 6);
      const harmonics = clamp(input.harmonicsPercent, 0, 100, 50);
      const width = clamp(input.width, 0.5, 3, 1.4);
      const mix = clamp(input.mixPercent, 0, 100, 50) / 100;
      const template = input.template;
      const harmonicNorm = harmonics / 100;
      const mixFactor = 0.65 + mix * 0.55;
      const shelfGain = enabled
        ? Math.min(12.5, template.lowShelf.gain * 0.82 + gainDb * mixFactor * (1 - harmonicNorm * 0.12)) : 0;
      const subFrequency = enabled
        ? Math.max(34, Math.min(96, Math.min(template.subPeak.frequency, frequencyHz * 0.70))) : 56;
      const subGain = enabled
        ? Math.min(10.5, template.subPeak.gain * 1.15 + gainDb * 0.46 * mixFactor) : 0;
      const subQ = enabled
        ? Math.max(0.55, Math.min(1.45, template.subPeak.q * (0.86 + (width - 0.5) * 0.24))) : 0.75;
      const presenceFrequency = enabled
        ? Math.max(95, Math.min(170, Math.min(template.upperPeak.frequency, subFrequency * 1.85))) : 118;
      const presenceGain = enabled
        ? Math.min(3.2, template.upperPeak.gain + gainDb * 0.08 + mix * 0.7) : 0;
      const presenceQ = enabled
        ? Math.max(0.7, Math.min(1.45, template.upperPeak.q * (0.9 + (width - 1) * 0.2))) : 0.95;
      const wetGain = enabled ? Math.min(0.035, mix * (0.001 + harmonicNorm * 0.015)) : 0;
      const dryGain = enabled ? Math.max(0.86, 0.98 - mix * 0.08) : 1;
      const harmonicCutoff = enabled ? Math.max(120, Math.min(220, frequencyHz * 1.15)) : 170;
      const harmonicDrive = enabled ? 1 + harmonicNorm * 0.02 : 1;
      const dipFrequency = enabled ? Math.max(180, Math.min(380, frequencyHz * 2.6)) : 260;
      const dipGain = enabled ? -Math.min(0.8, gainDb * mix * 0.035) : 0;
      const dipQ = enabled ? Math.max(0.55, Math.min(1.25, 0.8 + (width - 1) * 0.16)) : 0.8;
      const preampCompDb = 20 * Math.log10(Math.max(0.1, template.preampGain));
      const inputCompDb = enabled ? preampCompDb - Math.min(2.6, mix * 1.2 + gainDb * 0.05) : 0;
      const outputTrimDb = enabled
        ? Math.max(template.limiterCeilingDb - 0.2,
                   -Math.min(2.8, shelfGain * 0.07 + subGain * 0.09 + presenceGain * 0.02 + wetGain * 4.5)) : 0;
      return {
        frequencyHz: frequencyHz, inputGain: root.dbToGain(inputCompDb), shelfGain: shelfGain, width: width,
        subFrequency: subFrequency, subGain: subGain, subQ: subQ,
        presenceFrequency: presenceFrequency, presenceGain: presenceGain, presenceQ: presenceQ,
        harmonicCutoff: harmonicCutoff, harmonicDrive: harmonicDrive, wetGain: wetGain, dryGain: dryGain,
        dipFrequency: dipFrequency, dipGain: dipGain, dipQ: dipQ,
        outputGain: root.dbToGain(outputTrimDb), curveAmount: enabled ? harmonics : 0
      };
    };

    root.applyBassEnhancerParams = root.applyBassEnhancerParams || function(graph, cfg, force) {
      if (!graph || graph.bypass || !graph.bassEnhancerFilter || !root.bassEnhancerTemplate) return;
      const source = cfg && cfg.bassEnhancer ? cfg.bassEnhancer : {};
      const params = root.computeBassEnhancerParams({
        enabled: !!source.enabled, frequencyHz: source.frequencyHz, gainDb: source.gainDb,
        harmonicsPercent: source.harmonicsPercent, width: source.width, mixPercent: source.mixPercent,
        template: root.bassEnhancerTemplate
      });
      const same = function(left, right) { return Number.isFinite(left) && Math.abs(left - right) < 0.0001; };
      const update = function(key, param, value, rampSeconds) {
        if (!param || (!force && same(graph[key], value))) return;
        graph[key] = value;
        root.smoothParam(param, value, graph.ctx, rampSeconds);
      };
      update('_daliniraBassEnhancerInputGain', graph.bassEnhancerInputGain.gain, params.inputGain, 0.012);
      update('_daliniraBassEnhancerFrequency', graph.bassEnhancerFilter.frequency, params.frequencyHz, 0.014);
      update('_daliniraBassEnhancerShelfGain', graph.bassEnhancerFilter.gain, params.shelfGain, 0.014);
      update('_daliniraBassEnhancerWidth', graph.bassEnhancerFilter.Q, params.width, 0.014);
      update('_daliniraBassEnhancerSubFrequency', graph.bassEnhancerSubPeak.frequency, params.subFrequency, 0.014);
      update('_daliniraBassEnhancerSubGain', graph.bassEnhancerSubPeak.gain, params.subGain, 0.014);
      update('_daliniraBassEnhancerSubQ', graph.bassEnhancerSubPeak.Q, params.subQ, 0.014);
      update('_daliniraBassEnhancerPresenceFrequency', graph.bassEnhancerPresencePeak.frequency, params.presenceFrequency, 0.014);
      update('_daliniraBassEnhancerPresenceGain', graph.bassEnhancerPresencePeak.gain, params.presenceGain, 0.014);
      update('_daliniraBassEnhancerPresenceQ', graph.bassEnhancerPresencePeak.Q, params.presenceQ, 0.014);
      update('_daliniraBassEnhancerHarmonicCutoff', graph.bassEnhancerHarmonicLowpass.frequency, params.harmonicCutoff, 0.014);
      update('_daliniraBassEnhancerHarmonicDrive', graph.bassEnhancerHarmonicsDrive.gain, params.harmonicDrive, 0.012);
      update('_daliniraBassEnhancerWet', graph.bassEnhancerWetGain.gain, params.wetGain, 0.012);
      update('_daliniraBassEnhancerDry', graph.bassEnhancerDryGain.gain, params.dryGain, 0.012);
      update('_daliniraBassEnhancerDipFrequency', graph.bassEnhancerBodyDip.frequency, params.dipFrequency, 0.014);
      update('_daliniraBassEnhancerDipGain', graph.bassEnhancerBodyDip.gain, params.dipGain, 0.014);
      update('_daliniraBassEnhancerDipQ', graph.bassEnhancerBodyDip.Q, params.dipQ, 0.014);
      update('_daliniraBassEnhancerOutput', graph.bassEnhancerOutputTrim.gain, params.outputGain, 0.012);
      const priorCurve = graph.bassEnhancerState && Number(graph.bassEnhancerState.curveAmount);
      if (force || !Number.isFinite(priorCurve) || Math.abs(priorCurve - params.curveAmount) > 0.5) {
        try {
          graph.bassEnhancerSaturator.curve = root.makeBassSaturationCurve(params.curveAmount);
          graph.bassEnhancerState.curveAmount = params.curveAmount;
        } catch (error) {
          root.lastRuntimeError = 'Bass Enhancer parameter apply failed: ' + String(error && error.message ? error.message : error);
          console.error('[DaliNira DSP] ' + root.lastRuntimeError);
        }
      }
      graph.bassEnhancerEnabled = !!source.enabled;
      graph.bassEnhancerBypassed = !source.enabled;
      graph.bassEnhancerDeep = !!source.deep;
    };

    root.applyCompressorParams = root.applyCompressorParams || function(graph, cfg, force) {
      if (!graph || graph.bypass || !graph.compressorNode || !graph.compressorMakeupGain) return;
      const source = cfg && cfg.compressor ? cfg.compressor : {};
      const clamp = function(value, minimum, maximum, fallback) {
        const numeric = Number(value);
        const safe = Number.isFinite(numeric) ? numeric : fallback;
        return Math.max(minimum, Math.min(maximum, safe));
      };
      const enabled = !!source.enabled;
      const thresholdDb = clamp(source.thresholdDb, -60, 0, -20);
      const ratio = clamp(source.ratio, 1, 20, 4);
      const attackMs = clamp(source.attackMs, 0.1, 100, 10);
      const releaseMs = clamp(source.releaseMs, 10, 1000, 100);
      const makeupDb = clamp(source.makeupDb, -12, 24, 0);
      const kneeDb = clamp(source.kneeDb, 0, 10, 3);
      const same = function(left, right) { return Number.isFinite(left) && Math.abs(left - right) < 0.0001; };
      const update = function(key, param, value, rampSeconds) {
        if (!param || (!force && same(graph[key], value))) return;
        graph[key] = value;
        root.smoothParam(param, value, graph.ctx, rampSeconds);
      };
      update('_daliniraCompressorThreshold', graph.compressorNode.threshold, enabled ? thresholdDb : 0, 0.010);
      update('_daliniraCompressorRatio', graph.compressorNode.ratio, enabled ? ratio : 1, 0.010);
      update('_daliniraCompressorAttack', graph.compressorNode.attack, enabled ? attackMs / 1000 : 0.003, 0.008);
      update('_daliniraCompressorRelease', graph.compressorNode.release, enabled ? releaseMs / 1000 : 0.050, 0.012);
      update('_daliniraCompressorKnee', graph.compressorNode.knee, enabled ? kneeDb : 30, 0.010);
      update('_daliniraCompressorMakeup', graph.compressorMakeupGain.gain, enabled ? root.dbToGain(makeupDb) : 1, 0.012);
      graph.compressorEnabled = enabled;
      graph.compressorBypassed = !enabled;
    };

    root.applyUserLimiterParams = root.applyUserLimiterParams || function(graph, cfg, force) {
      if (!graph || graph.bypass || !graph.userLimiterInputGain || !graph.userLimiterNode) return;
      const source = cfg && cfg.limiter ? cfg.limiter : {};
      const clamp = function(value, minimum, maximum, fallback) {
        const numeric = Number(value);
        const safe = Number.isFinite(numeric) ? numeric : fallback;
        return Math.max(minimum, Math.min(maximum, safe));
      };
      const enabled = !!source.enabled;
      const ceilingDb = clamp(source.ceilingDb, -12, 0, -0.3);
      const releaseMs = clamp(source.releaseMs, 10, 500, 50);
      const lookaheadMs = clamp(source.lookaheadMs, 0, 20, 5);
      const gainDb = clamp(source.gainDb, -12, 12, 0);
      const same = function(left, right) { return Number.isFinite(left) && Math.abs(left - right) < 0.0001; };
      const update = function(key, param, value, rampSeconds) {
        if (!param || (!force && same(graph[key], value))) return;
        graph[key] = value;
        root.smoothParam(param, value, graph.ctx, rampSeconds);
      };
      update('_daliniraUserLimiterGain', graph.userLimiterInputGain.gain, enabled ? root.dbToGain(gainDb) : 1, 0.012);
      update('_daliniraUserLimiterThreshold', graph.userLimiterNode.threshold, enabled ? ceilingDb : 0, 0.010);
      update('_daliniraUserLimiterRatio', graph.userLimiterNode.ratio, enabled ? 20 : 1, 0.010);
      update('_daliniraUserLimiterKnee', graph.userLimiterNode.knee, enabled ? 0 : 30, 0.010);
      update('_daliniraUserLimiterAttack', graph.userLimiterNode.attack,
             enabled ? Math.max(0.0005, lookaheadMs / 1000) : 0.003, 0.008);
      update('_daliniraUserLimiterRelease', graph.userLimiterNode.release,
             enabled ? Math.max(0.005, releaseMs / 1000) : 0.050, 0.014);
      graph.userLimiterEnabled = enabled;
      graph.userLimiterBypassed = !enabled;
    };

    root.applyReverbParams = root.applyReverbParams || function(graph, cfg, force) {
      if (!graph || graph.bypass || !graph.reverbInputGain || !graph.reverbDryGain || !graph.reverbDelay
          || !graph.reverbLowpass || !graph.reverbFeedbackGain || !graph.reverbWetGain) return;
      const source = cfg && cfg.reverb ? cfg.reverb : {};
      const clamp = function(value, minimum, maximum, fallback) {
        const numeric = Number(value);
        const safe = Number.isFinite(numeric) ? numeric : fallback;
        return Math.max(minimum, Math.min(maximum, safe));
      };
      const enabled = !!source.enabled;
      const roomSizeMs = clamp(source.roomSizeMs, 0, 3000, 1000);
      const damping = clamp(source.damping, 0, 1, 0.5);
      const wetDryDb = clamp(source.wetDryDb, -96, 0, -10);
      const hfRatio = clamp(source.hfRatio, 0.001, 0.999, 0.7);
      const inputGainDb = clamp(source.inputGainDb, -96, 12, 0);
      const same = function(left, right) { return Number.isFinite(left) && Math.abs(left - right) < 0.0001; };
      const update = function(key, param, value, rampSeconds) {
        if (!param || (!force && same(graph[key], value))) return;
        graph[key] = value;
        root.smoothParam(param, value, graph.ctx, rampSeconds);
      };
      const wetGain = enabled ? root.dbToGain(wetDryDb) : 0;
      const dryGain = enabled ? clamp(1 - wetGain * 0.35, 0.55, 1, 1) : 1;
      const feedback = enabled ? clamp(0.18 + (1 - damping) * 0.55, 0.15, 0.78, 0.42) : 0;
      const delaySeconds = clamp((roomSizeMs / 1000) * 0.09, 0.01, 0.28, 0.09);
      const lowpassHz = enabled ? clamp(900 + hfRatio * 12000, 900, 18000, 9300) : 18000;
      const inputGain = enabled ? root.dbToGain(inputGainDb) : 1;
      update('_daliniraReverbInputGain', graph.reverbInputGain.gain, inputGain, 0.012);
      update('_daliniraReverbDryGain', graph.reverbDryGain.gain, dryGain, 0.012);
      update('_daliniraReverbWetGain', graph.reverbWetGain.gain, wetGain, 0.012);
      update('_daliniraReverbFeedback', graph.reverbFeedbackGain.gain, feedback, 0.014);
      update('_daliniraReverbDelay', graph.reverbDelay.delayTime, delaySeconds, 0.014);
      update('_daliniraReverbLowpass', graph.reverbLowpass.frequency, lowpassHz, 0.016);
      graph.reverbEnabled = enabled;
      graph.reverbBypassed = !enabled;
    };

    root.applyAutoGainParams = root.applyAutoGainParams || function(graph, cfg, force) {
      if (!graph || graph.bypass || !graph.autoGainNode || !graph.autoGainState) return;
      const source = cfg && cfg.autoGain ? cfg.autoGain : {};
      const clamp = function(value, minimum, maximum, fallback) {
        const numeric = Number(value);
        return Math.max(minimum, Math.min(maximum, Number.isFinite(numeric) ? numeric : fallback));
      };
      const state = graph.autoGainState;
      const wasEnabled = !!state.enabled;
      state.enabled = !!source.enabled && state.runtimeAvailable !== false;
      state.targetDbfs = clamp(source.targetDbfs, -24, 0, -14);
      state.maxGainDb = clamp(source.maxGainDb, 0, 24, 12);
      state.speed = ['slow', 'medium', 'fast'].includes(String(source.speed || '').toLowerCase())
          ? String(source.speed).toLowerCase() : 'medium';
      state.preset = ['balanced', 'night', 'loud', 'speech'].includes(String(source.preset || '').toLowerCase())
          ? String(source.preset).toLowerCase() : 'balanced';
      if (!state.enabled) {
        state.currentGainDb = 0;
        state.envDb = -120;
        state.lastAppliedGainDb = 0;
        root.smoothParam(graph.autoGainNode.gain, 1, graph.ctx, 0.014);
      } else if (!wasEnabled || force) {
        state.currentGainDb = 0;
        state.envDb = -120;
        state.lastAppliedGainDb = 0;
        root.smoothParam(graph.autoGainNode.gain, 1, graph.ctx, 0.014);
      }
      graph.autoGainEnabled = state.enabled;
      graph.autoGainBypassed = !state.enabled;
    };

    root.applyEqParams = root.applyEqParams || function(graph, cfg) {
      if (!graph || graph.bypass) return;
      const ctx = graph.ctx;
      const force = !!cfg.forceRefresh;
      const same = function(left, right) { return Number.isFinite(left) && Math.abs(left - right) < 0.0001; };
      const update = function(key, param, value, rampSeconds) {
        const target = Number(value) || 0;
        if (!force && same(graph[key], target)) return;
        graph[key] = target;
        root.smoothParam(param, target, ctx, rampSeconds);
      };
      update('_daliniraPreampDb', graph.runtimeGain && graph.runtimeGain.gain, root.dbToGain(cfg.outputPreampDb), 0.090);
      const nodes = graph.eqBandNodes || [];
      const priorBands = graph._daliniraBands || [];
      const nextBands = [];
      for (let index = 0; index < nodes.length; index += 1) {
        const target = Number(cfg.bands[index]) || 0;
        nextBands.push(target);
        if (force || !same(priorBands[index], target)) root.smoothParam(nodes[index].gain, target, ctx, 0.115);
      }
      graph._daliniraBands = nextBands;
      const bassDb = Number(cfg.bassDb) || 0;
      update('_daliniraBassDb', graph.bass && graph.bass.gain, bassDb, 0.115);
      update('_daliniraToneTrim', graph.toneTrim && graph.toneTrim.gain,
             root.dbToGain(-Math.max(0, bassDb) * 0.333), 0.140);
      update('_daliniraMidDb', graph.mid && graph.mid.gain, Number(cfg.midDb) || 0, 0.115);
      update('_daliniraTrebleDb', graph.treble && graph.treble.gain, Number(cfg.trebleDb) || 0, 0.115);
      update('_daliniraStereoWidth', graph.stereoSideWidth && graph.stereoSideWidth.gain,
             Math.max(0, Math.min(2, (Number(cfg.stereoExpanderPercent) || 100) / 100)), 0.115);
      const normalized = Math.max(-1, Math.min(1, (Number(cfg.balance) || 0) / 100));
      update('_daliniraBalanceLeft', graph.balanceGainL && graph.balanceGainL.gain, normalized > 0 ? 1 - normalized : 1, 0.115);
      update('_daliniraBalanceRight', graph.balanceGainR && graph.balanceGainR.gain, normalized < 0 ? 1 + normalized : 1, 0.115);
      root.applyCompressorParams(graph, cfg, force);
      root.applyBassEnhancerParams(graph, cfg, force);
      root.applyReverbParams(graph, cfg, force);
      root.applyAutoGainParams(graph, cfg, force);
      root.applyUserLimiterParams(graph, cfg, force);
    };

    root.applyActiveParams = root.applyActiveParams || function(cfg) {
      const result = root.applyGraphParams(cfg);
      root.updateActivity();
      return result;
    };

    root.applyGraphParams = root.applyGraphParams || function(cfg) {
      root.pruneStaleGraphs();
      root.currentConfig = cfg;
      let attached = 0;
      const rates = [];
      const contextStates = [];
      for (const graph of root.graphList) {
        if (!graph) continue;
        if (root.needsGraphRebuild(graph, cfg)) {
          root.rebuildActiveGraph(graph, cfg);
        } else if (!graph.bypass && graph.runtimeGain) {
          root.applyEqParams(graph, cfg);
        }
        attached += 1;
        rates.push(Math.round(Number(graph.ctx && graph.ctx.sampleRate) || 0));
        contextStates.push(String(graph.ctx && graph.ctx.state || 'unknown'));
      }
      return {
        ok: !!root.buildOutputGraph && !!root.eqTemplate,
        enabled: !!cfg.enabled, mediaCount: attached, sampleRates: rates,
        moduleLoaded: !!root.buildOutputGraph && !!root.eqTemplate,
        eqBandCount: root.eqTemplate ? root.eqTemplate.bands.length : 0,
        contextStates: contextStates,
        error: root.autoGainTemplateError || root.bassEnhancerTemplateError || root.compressorTemplateError || root.userLimiterTemplateError || root.lastRuntimeError || '',
        needsBootstrap: false
      };
    };

    root.setMasterGain = root.setMasterGain || function(param, value, ctx) {
      if (!param || !ctx) return;
      const now = Number(ctx.currentTime) || 0;
      const target = Number(value) || 0;
      try {
        const current = Number(param.value);
        param.cancelScheduledValues(now);
        param.setValueAtTime(Number.isFinite(current) ? current : target, now);
        param.linearRampToValueAtTime(target, now + 0.035);
      } catch (_) { try { param.value = target; } catch (_) {} }
    };

    // Audible media: playing, not muted, not silent. Only such an element
    // gets a DSP graph, and only while one plays does the document's
    // AudioContext need to run.
    root.isAudibleMedia = root.isAudibleMedia || function(element) {
      return !!element && !element.paused && !element.ended && !element.muted && Number(element.volume) > 0;
    };
    // Playing, whatever its volume: its context must keep running. A muted or
    // silent element is still playing and may be turned up at any moment
    // (without a user gesture, which WebKit needs to restart a context).
    root.isPlayingMedia = root.isPlayingMedia || function(element) {
      return !!element && !element.paused && !element.ended;
    };
    root.hasAudibleGraph = root.hasAudibleGraph || function() {
      for (const graph of root.graphList) {
        if (graph && root.isPlayingMedia(graph.element)) return true;
      }
      return false;
    };
    root.elementGain = root.elementGain || function(element) {
      if (!element || element.muted) return 0;
      const volume = Number(element.volume);
      return Number.isFinite(volume) ? Math.max(0, Math.min(1, volume)) : 1;
    };
    root.applyElementVolume = root.applyElementVolume || function(graph) {
      if (!graph || !graph.mirrorsVolume || !graph.source || !graph.ctx) return;
      try {
        const param = graph.source.gain;
        const now = Number(graph.ctx.currentTime) || 0;
        param.cancelScheduledValues(now);
        param.setTargetAtTime(root.elementGain(graph.element), now, 0.012);
      } catch (_) {}
    };
    root.setAutoGainTimers = root.setAutoGainTimers || function(active) {
      for (const graph of root.graphList) {
        if (!graph || !graph.autoGainEnabled || !graph.autoGainState) continue;
        if (active && !graph.autoGainTimer) {
          graph.autoGainTimer = setInterval(function() { root.runAutoGainStep(graph); },
                                            Number(graph.autoGainState.controlIntervalMs) || 60);
        } else if (!active && graph.autoGainTimer) {
          try { clearInterval(graph.autoGainTimer); } catch (_) {}
          graph.autoGainTimer = null;
        }
      }
    };
    // A running AudioContext renders continuously whatever its media does.
    // With several media tabs that idle load competes with the audible tab's
    // audio thread, so a document whose attached media has been inaudible for
    // a while suspends its context (and auto gain) until media plays again.
    root.idleSuspendMs = root.idleSuspendMs || 4000;
    root.wakeAudio = root.wakeAudio || function() {
      if (root.idleTimer) { clearTimeout(root.idleTimer); root.idleTimer = null; }
      const ctx = root.audioContext;
      // Also after a suspend the runtime did not cause: playing media routed
      // into a stopped context is silent.
      if (ctx && ctx.state === 'suspended' && typeof ctx.resume === 'function') root.requestContextStart(ctx);
      root.idleSuspended = false;
      root.setAutoGainTimers(true);
    };
    root.scheduleIdleCheck = root.scheduleIdleCheck || function() {
      if (root.idleTimer) clearTimeout(root.idleTimer);
      root.idleTimer = setTimeout(function() {
        root.idleTimer = null;
        root.pruneStaleGraphs();
        if (root.hasAudibleGraph() || root.contextStartTimer) return;
        root.setAutoGainTimers(false);
        const ctx = root.audioContext;
        if (ctx && ctx.state === 'running' && typeof ctx.suspend === 'function') {
          root.idleSuspended = true;
          root.idleSuspendCount = (Number(root.idleSuspendCount) || 0) + 1;
          try { Promise.resolve(ctx.suspend()).catch(function () {}); } catch (_) {}
        }
      }, root.idleSuspendMs);
    };
    // WebKit starts a new context only on resume() (Chromium on creation),
    // and a resume() issued while a new page cannot start media yet may never
    // settle. Retry at 2 Hz for up to 10 s; play/volume events retry again.
    root.requestContextStart = root.requestContextStart || function(ctx) {
      const tryResume = function() {
        try { Promise.resolve(ctx.resume()).catch(function () {}); } catch (_) {}
      };
      tryResume();
      if (root.contextStartTimer) return;
      let attempts = 0;
      root.contextStartTimer = setInterval(function() {
        attempts += 1;
        if (ctx.state === 'running' || ctx.state === 'closed' || attempts > 20) {
          clearInterval(root.contextStartTimer);
          root.contextStartTimer = null;
          if (ctx.state === 'running' && root.queueMediaScan) root.queueMediaScan();
          return;
        }
        tryResume();
      }, 500);
    };
    root.updateActivity = root.updateActivity || function() {
      if (root.hasAudibleGraph()) root.wakeAudio();
      else root.scheduleIdleCheck();
    };

    root.resumeGraph = root.resumeGraph || function(graph) {
      if (!graph || !graph.ctx || graph.ctx.state === 'closed') return false;
      if (graph.ctx.state === 'suspended' && typeof graph.ctx.resume === 'function') {
        if (graph.element && !root.isPlayingMedia(graph.element)) {
          if (!graph.playListenerAttached) {
            graph.playListenerAttached = true;
            const onPlay = function() {
              if (graph.ctx && graph.ctx.state === 'suspended') {
                try { Promise.resolve(graph.ctx.resume()).catch(function () {}); } catch (_) {}
              }
            };
            graph.element.addEventListener('play', onPlay, { once: true });
          }
          return false;
        }
        try { Promise.resolve(graph.ctx.resume()).catch(function () {}); } catch (_) {}
      }
      return true;
    };

    root.setGraphBypass = root.setGraphBypass || function(graph, bypass) {
      if (!graph || !graph.ctx || !graph.wetGain || !graph.dryGain) return false;
      root.setMasterGain(graph.wetGain.gain, bypass ? 0 : 1, graph.ctx);
      root.setMasterGain(graph.dryGain.gain, bypass ? 1 : 0, graph.ctx);
      if (!bypass) root.resumeGraph(graph);
      graph.bypass = !!bypass;
      return true;
    };

    root.rebuildActiveGraph = root.rebuildActiveGraph || function(graph, cfg) {
      if (!graph || !graph.source || !root.buildOutputGraph || !root.eqTemplate) return false;
      try {
        try { graph.source.disconnect(); } catch (_) {}
        root.disconnectEqGraph(graph);
        graph.graph = null;
        graph.bypass = false;
        root.buildEqGraph(graph, cfg);
        graph.restoreRequired = false;
        root.applyEqParams(graph, cfg);
        root.setGraphBypass(graph, false);
        graph.restoreSerial = (Number(graph.restoreSerial) || 0) + 1;
        return true;
      } catch (err) {
        graph.bypass = true;
        try { graph.source.disconnect(); } catch (_) {}
        try { graph.source.connect(graph.ctx.destination); } catch (_) {}
        console.error('[DaliNira DSP] graph rebuild error: ' + String(err && err.message ? err.message : err));
        return false;
      }
    };

    root.processMedia = root.processMedia || function(cfg) {
      const result = root.scanMedia(cfg);
      root.updateActivity();
      return result;
    };

    root.scanMedia = root.scanMedia || function(cfg) {
      root.pruneStaleGraphs();
      root.currentConfig = cfg;
      const media = Array.from(document.querySelectorAll('audio, video'));
      let attached = 0;
      const rates = [];
      const contextStates = [];
      for (const element of media) {
        let graph = root.graphs.get(element);
        if (!graph) {
          // One DSP graph per element that is actually heard, attached once it
          // plays: preloaded, paused or muted media (feed previews, muted
          // autoplay) never gets one; the play/volumechange listeners below
          // rescan when that changes. Waiting for playback also keeps
          // YouTube's MSE pipeline setup untouched (no endless spinner).
          if (!cfg.enabled || !root.isAudibleMedia(element)) continue;
          const Ctx = window.AudioContext || window.webkitAudioContext;
          if (!Ctx) continue;
          try {
            // Share one context per document. Keep Chromium's playback
            // buffer; WebKit needs an interactive buffer for video A/V sync.
            let ctx = root.audioContext;
            if (!ctx || ctx.state === 'closed') {
              const options = window.__DALINIRA_DALI_ENGINE__;
              try { ctx = new Ctx({ latencyHint: options && options.mediaLatencyHint || 'playback' }); }
              catch (_) { ctx = new Ctx(); }
              root.audioContext = ctx;
              ctx.addEventListener('statechange', function() {
                if (ctx.state === 'running' && root.queueMediaScan) root.queueMediaScan();
              });
            }
            // An element routed into the graph is heard only through the
            // context, and the routing cannot be undone. Never route media
            // into a context that is not running (still starting, or held by
            // an autoplay / page-visibility policy): the element keeps playing
            // directly and is attached once the context runs (statechange).
            if (ctx.state !== 'running') {
              root.requestContextStart(ctx);
              continue;
            }
            const mediaSource = ctx.createMediaElementSource(element);
            let source = mediaSource;
            // WebKit's MediaElementAudioSourceNode ignores the element's volume
            // and muted state (Chromium applies them): the page's own volume
            // and mute controls would stop working. Apply them here, first in
            // the chain, so every path (effects or bypass) follows them.
            const engine = window.__DALINIRA_DALI_ENGINE__;
            if (engine && engine.mirrorElementVolume) {
              source = new GainNode(ctx, { gain: root.elementGain(element) });
              mediaSource.connect(source);
            }
            graph = { ctx: ctx, source: source, mediaSource: mediaSource, graph: null, bypass: false, element: element,
                      mirrorsVolume: source !== mediaSource };
            root.graphs.set(element, graph);
            root.graphList.add(graph);
          } catch (error) {
            root.lastRuntimeError = 'Web Audio graph/node creation failed: ' + String(error && error.message ? error.message : error);
            console.error('[DaliNira DSP] ' + root.lastRuntimeError);
            continue;
          }
        } else {
          graph.element = element;
          root.graphList.add(graph);
        }

        if (cfg.enabled && root.buildOutputGraph && root.eqTemplate) {
          if (!graph.graph || root.needsGraphRebuild(graph, cfg)) {
            root.rebuildActiveGraph(graph, cfg);
          } else if (graph.bypass) {
            root.setGraphBypass(graph, false);
          } else {
            root.resumeGraph(graph);
          }
          if (!graph.bypass && graph.runtimeGain) {
            root.applyEqParams(graph, cfg);
            attached += 1;
            rates.push(Math.round(Number(graph.ctx.sampleRate) || 0));
            contextStates.push(String(graph.ctx.state || 'unknown'));
          }
        } else if (graph) {
          if (!graph.bypass || graph.graph) {
            root.disconnectEqGraph(graph);
            graph.graph = null;
            try { graph.source.disconnect(); } catch (_) {}
            try { graph.source.connect(graph.ctx.destination); } catch (_) {}
            graph.bypass = true;
          }
          graph.restoreRequired = false;
        }
      }
      return {
        ok: !!root.buildOutputGraph && !!root.eqTemplate,
        enabled: !!cfg.enabled, mediaCount: attached, sampleRates: rates,
        moduleLoaded: !!root.buildOutputGraph && !!root.eqTemplate,
        eqBandCount: root.eqTemplate ? root.eqTemplate.bands.length : 0,
        contextStates: contextStates,
        error: root.autoGainTemplateError || root.bassEnhancerTemplateError || root.compressorTemplateError || root.userLimiterTemplateError || root.lastRuntimeError || '',
        needsBootstrap: false
      };
    };

    root.forceActivate = function(cfg) {
      const forcedCfg = Object.assign({}, cfg, { enabled: true, forceRefresh: true });
      const result = root.processMedia(forcedCfg);
      for (const graph of root.graphList) {
        if (!graph || !graph.graph || !graph.runtimeGain) continue;
        if (graph.bypass) root.setGraphBypass(graph, false);
        else root.resumeGraph(graph);
        root.applyEqParams(graph, forcedCfg);
      }
      return result;
    };

    if (!root.mediaObserver && document.documentElement) {
      root.queueMediaScan = function() {
        if (root.mediaScanQueued) return;
        root.mediaScanQueued = true;
        queueMicrotask(function() {
          root.mediaScanQueued = false;
          if (root.currentConfig && root.processMedia) root.processMedia(root.currentConfig);
        });
      };
      root.containsMedia = function(node) {
        if (!node || node.nodeType !== Node.ELEMENT_NODE) return false;
        if (node.matches && node.matches('audio,video')) return true;
        return !!(node.querySelector && node.querySelector('audio,video'));
      };
      root.mediaObserver = new MutationObserver(function(records) {
        for (const record of records) {
          for (const node of record.addedNodes) {
            if (root.containsMedia(node)) { root.queueMediaScan(); return; }
          }
        }
      });
      root.mediaObserver.observe(document.documentElement, { childList: true, subtree: true });
      document.addEventListener('playing', function(event) {
        if (event.target && event.target.matches && event.target.matches('audio,video')) root.queueMediaScan();
      }, true);
      document.addEventListener('loadedmetadata', function(event) {
        if (event.target && event.target.matches && event.target.matches('audio,video')) root.queueMediaScan();
      }, true);
      document.addEventListener('canplay', function(event) {
        if (event.target && event.target.matches && event.target.matches('audio,video')) root.queueMediaScan();
      }, true);
      document.addEventListener('play', function(event) {
        if (!event.target || !event.target.matches || !event.target.matches('audio,video')) return;
        // Resume before the element's audio reaches the graph.
        if (root.graphs.has(event.target)) root.wakeAudio();
        root.queueMediaScan();
      }, true);
      document.addEventListener('volumechange', function(event) {
        if (!event.target || !event.target.matches || !event.target.matches('audio,video')) return;
        root.applyElementVolume(root.graphs.get(event.target));
        root.queueMediaScan();  // an element unmuted now may need a graph
      }, true);
      for (const type of ['pause', 'ended', 'emptied']) {
        document.addEventListener(type, function(event) {
          if (event.target && event.target.matches && event.target.matches('audio,video')) root.updateActivity();
        }, true);
      }
    }
    return root.processMedia({
      enabled: enabled, outputPreampDb: outputPreampDb, bands: bands, eqActive: eqActive,
      bassDb: bassDb, midDb: midDb, trebleDb: trebleDb, toneActive: toneActive,
      stereoExpanderPercent: stereoExpanderPercent, balance: balance, spatialActive: spatialActive,
      reverb: reverb, compressor: compressor, limiter: limiter, bassEnhancer: bassEnhancer,
      autoGain: autoGain, policyMode: policyMode, panelVisible: panelVisible, activeSubpanel: activeSubpanel
    });
  } catch (error) {
    return { ok: false, enabled: %3, mediaCount: 0, sampleRates: [], error: String(error && error.message ? error.message : error) };
  }
})()
)JS")
      .arg(outputModuleJson)
      .arg(eqModuleJson)
      .arg(enabledJson)
      .arg(QString::number(preampDb_, 'f', 2))
      .arg(jsonNumberArray(equalizerBands_))
      .arg(isEqualizerActive() ? QStringLiteral("true") : QStringLiteral("false"))
      .arg(QString::number(bassDb_, 'f', 2))
      .arg(QString::number(midDb_, 'f', 2))
      .arg(QString::number(trebleDb_, 'f', 2))
      .arg(isToneActive() ? QStringLiteral("true") : QStringLiteral("false"))
      .arg(QString::number(stereoExpanderPercent_, 'f', 2))
      .arg(compressorModuleJson)
      .arg(QString::number(balance_, 'f', 2))
      .arg(isSpatialActive() ? QStringLiteral("true") : QStringLiteral("false"))
      .arg(limiterModuleJson)
      .arg(reverbConfigJson)
      .arg(bassEnhancerModuleJson)
      .arg(compressorConfigJson)
      .arg(autoGainModuleJson)
      .arg(limiterConfigJson)
      .arg(bassEnhancerConfigJson)
      .arg(autoGainConfigJson)
      .arg(policyModeStr)
      .arg(panelVisible_ ? QStringLiteral("true") : QStringLiteral("false"))
      .arg(activeSubpanelId_)
      .arg(outputModuleSource)
      .arg(supportedDomainsJson);
}

void WebAudioEffectsController::requestCompressorGainReduction() {
  if (compressorMeterRequestPending_ || !panelVisible_ || !compressorEnabled()) return;
  prunePages();
  QVector<Page> liveViews;
  for (const Page &page : std::as_const(pages_)) {
    if (pageAlive(page) && page.object->property("dalinira-audio-graph-active").toBool()) liveViews.push_back(page);
  }
  if (liveViews.isEmpty()) {
    emit compressorGainReductionChanged(0.0, false);
    return;
  }

  compressorMeterRequestPending_ = true;
  const quint64 generation = ++compressorMeterRequestGeneration_;
  auto aggregate = QSharedPointer<CompressorMeterAggregation>::create();
  aggregate->remaining = liveViews.size();
  const QString script = QStringLiteral(R"JS(
(function() {
  try {
    const root = window.__DALINIRA_WEB_DALI_OUTPUT__;
    if (!root || !root.graphList) return { available: false, reductionDb: 0, activeGraphs: 0 };
    let reductionDb = 0;
    let activeGraphs = 0;
    for (const graph of root.graphList) {
      if (!graph || graph.bypass || !graph.compressorEnabled || !graph.compressorNode) continue;
      const reduction = Number(graph.compressorNode.reduction);
      if (!Number.isFinite(reduction)) continue;
      reductionDb = Math.min(reductionDb, reduction);
      activeGraphs += 1;
    }
    return { available: activeGraphs > 0, reductionDb: reductionDb, activeGraphs: activeGraphs };
  } catch (_) {
    return { available: false, reductionDb: 0, activeGraphs: 0 };
  }
})()
)JS");
  QTimer::singleShot(600, this, [this, generation] {
    if (generation == compressorMeterRequestGeneration_) compressorMeterRequestPending_ = false;
  });
  for (const Page &page : liveViews) {
    runInPage(page, script, [this, generation, aggregate](const QVariant &value) {
      if (generation != compressorMeterRequestGeneration_) return;
      const QVariantMap result = value.toMap();
      if (result.value(QStringLiteral("available")).toBool()) {
        const double reduction = result.value(QStringLiteral("reductionDb")).toDouble();
        if (std::isfinite(reduction)) {
          aggregate->available = true;
          aggregate->reductionDb = std::min(aggregate->reductionDb, reduction);
        }
      }
      if (--aggregate->remaining > 0) return;
      compressorMeterRequestPending_ = false;
      emit compressorGainReductionChanged(aggregate->available ? aggregate->reductionDb : 0.0, aggregate->available);
    });
  }
  if (aggregate->remaining == 0) {
    compressorMeterRequestPending_ = false;
    emit compressorGainReductionChanged(0.0, false);
  }
}

void WebAudioEffectsController::requestLimiterReduction() {
  if (limiterMeterRequestPending_ || !panelVisible_ || !limiterEnabled()) return;
  prunePages();
  QVector<Page> liveViews;
  for (const Page &page : std::as_const(pages_)) {
    if (pageAlive(page) && page.object->property("dalinira-audio-graph-active").toBool()) liveViews.push_back(page);
  }
  if (liveViews.isEmpty()) {
    emit limiterReductionChanged(0.0, false);
    return;
  }

  limiterMeterRequestPending_ = true;
  const quint64 generation = ++limiterMeterRequestGeneration_;
  auto aggregate = QSharedPointer<CompressorMeterAggregation>::create();
  aggregate->remaining = liveViews.size();
  const QString script = QStringLiteral(R"JS(
(function() {
  try {
    const root = window.__DALINIRA_WEB_DALI_OUTPUT__;
    if (!root || !root.graphList) return { available: false, reductionDb: 0, activeGraphs: 0 };
    let reductionDb = 0;
    let activeGraphs = 0;
    for (const graph of root.graphList) {
      if (!graph || graph.bypass || !graph.userLimiterEnabled || !graph.userLimiterNode) continue;
      const reduction = Number(graph.userLimiterNode.reduction);
      if (!Number.isFinite(reduction)) continue;
      reductionDb = Math.min(reductionDb, reduction);
      activeGraphs += 1;
    }
    return { available: activeGraphs > 0, reductionDb: reductionDb, activeGraphs: activeGraphs };
  } catch (_) {
    return { available: false, reductionDb: 0, activeGraphs: 0 };
  }
})()
)JS");
  QTimer::singleShot(600, this, [this, generation] {
    if (generation == limiterMeterRequestGeneration_) limiterMeterRequestPending_ = false;
  });
  for (const Page &page : liveViews) {
    runInPage(page, script, [this, generation, aggregate](const QVariant &value) {
      if (generation != limiterMeterRequestGeneration_) return;
      const QVariantMap result = value.toMap();
      if (result.value(QStringLiteral("available")).toBool()) {
        const double reduction = result.value(QStringLiteral("reductionDb")).toDouble();
        if (std::isfinite(reduction)) {
          aggregate->available = true;
          aggregate->reductionDb = std::min(aggregate->reductionDb, reduction);
        }
      }
      if (--aggregate->remaining > 0) return;
      limiterMeterRequestPending_ = false;
      emit limiterReductionChanged(aggregate->available ? aggregate->reductionDb : 0.0, aggregate->available);
    });
  }
  if (aggregate->remaining == 0) {
    limiterMeterRequestPending_ = false;
    emit limiterReductionChanged(0.0, false);
  }
}

void WebAudioEffectsController::applyToView(QWebEngineView *view) {
  if (const Page *page = findPage(view)) applyToPage(*page);
}

void WebAudioEffectsController::applyToEngineView(dalinira::engine::EngineView *view) {
  if (const Page *page = findPage(view)) applyToPage(*page);
}

// Per-engine facts the shared page runtime needs, run before it (at document
// start on WebKit).
//
// WebKit (2.52) with media routed into Web Audio neither applies the element's
// volume and muted state to that audio nor reports the volume back (the getter
// keeps returning 1 and no volumechange follows; muted is reported). The page's
// own writes are the only reliable value: the setters record it (the getter
// then returns what the page set, as the spec requires) and the runtime applies
// it, and muted, in the element's volume stage (see elementGain).
static QString engineOptionsScript(const QObject *view) {
  const auto *engineView = qobject_cast<const dalinira::engine::EngineView *>(view);
  if (!engineView || engineView->engineType() != dalinira::engine::EngineType::WebKit) return {};
  return QStringLiteral(R"JS(
if (!window.__DALINIRA_DALI_ENGINE__) {
  window.__DALINIRA_DALI_ENGINE__ = Object.freeze({ mirrorElementVolume: true, mediaLatencyHint: 'interactive' });
  (function() {
    if (!window.HTMLMediaElement) return;
    const proto = HTMLMediaElement.prototype;
    for (const name of ['volume', 'muted']) {
      const native = Object.getOwnPropertyDescriptor(proto, name);
      if (!native || typeof native.set !== 'function' || typeof native.get !== 'function') continue;
      Object.defineProperty(proto, name, {
        configurable: native.configurable,
        enumerable: native.enumerable,
        get: name === 'volume'
          ? function() { return this.__daliniraVolume !== undefined ? this.__daliniraVolume : native.get.call(this); }
          : native.get,
        set: function(value) {
          native.set.call(this, value);  // throws for an invalid volume, as before
          if (name === 'volume') this.__daliniraVolume = Number(value);
          const root = window.__DALINIRA_WEB_DALI_OUTPUT__;
          if (root && root.applyElementVolume && root.graphs) root.applyElementVolume(root.graphs.get(this));
        }
      });
    }
  })();
}
)JS");
}

// Records whether a page reports a live DALI graph, from a script result.
static void markGraphState(const QPointer<QObject> &object, const QVariant &result) {
  if (!object) return;
  if (isAudioEligibleView(object, object->property("dalinira-audio-page-url").toUrl())) {
    const QVariantMap map = result.toMap();
    object->setProperty("dalinira-audio-graph-active",
                        map.value(QStringLiteral("ok")).toBool() && map.value(QStringLiteral("moduleLoaded")).toBool());
  } else {
    object->setProperty("dalinira-audio-graph-active", false);
  }
}

void WebAudioEffectsController::applyToPage(const Page &page) {
  if (!pageAlive(page)) return;
  QObject *object = page.object;
  const QUrl url = pageUrl(page);
  object->setProperty("dalinira-audio-page-url", url);
  if (!isAudioEligibleView(object, url)) {
    object->setProperty("dalinira-audio-graph-active", false);
    return;
  }
  // Effects off and never delivered to this document: nothing to bypass.
  if (!enabled_ && !object->property("dalinira-audio-graph-active").toBool()) return;
  if (qEnvironmentVariableIntValue("DALINIRA_FEATURE_DIAGNOSTICS") == 1) {
    qInfo().noquote() << "[AUDIO] effect parameter updated";
  }
  const QPointer<WebAudioEffectsController> guardedController(this);
  const QPointer<QObject> guardedView(object);
  runInPage(page, parameterUpdateScript(), [guardedController, guardedView](const QVariant &result) {
    if (!guardedController) return;
    // Engines answer pending scripts with an invalid result when their page
    // is destroyed (e.g. closing a discarded tab), possibly from inside the
    // view's destructor, so the view must not be touched then.
    if (!result.isValid()) return;
    markGraphState(guardedView, result);
    guardedController->updateStatusFromResult(result);
    if (qEnvironmentVariableIsSet("DALINIRA_AUDIO_EFFECTS_TRACE")) {
      const QVariantMap map = result.toMap();
      QStringList contextStates;
      for (const QVariant &state : map.value(QStringLiteral("contextStates")).toList()) contextStates.push_back(state.toString());
      qInfo().noquote() << "[DaliNira DSP] apply result enabled=" << map.value(QStringLiteral("enabled")).toBool()
                        << "attached=" << map.value(QStringLiteral("mediaCount")).toInt()
                        << "module=" << map.value(QStringLiteral("moduleLoaded")).toBool()
                        << "contexts=" << contextStates.join(QLatin1Char(','))
                        << "error=" << map.value(QStringLiteral("error")).toString();
    }
    if (guardedView && result.toMap().value(QStringLiteral("needsBootstrap")).toBool()) {
      if (const Page *known = guardedController->findPage(guardedView)) guardedController->bootstrapPage(*known);
    }
  });
}

void WebAudioEffectsController::applyEqualizerBandToPage(const Page &page, int index) {
  if (!pageAlive(page) || !isAudioEligibleView(page.object, pageUrl(page))) return;
  if (!page.object->property("dalinira-audio-graph-active").toBool()) return;
  runInPage(page, equalizerBandUpdateScript(index));
}

void WebAudioEffectsController::installDocumentBootstrap(const Page &page, const QUrl &url) {
  // Engines without a per-page script collection (WebKit) ask
  // documentScriptsFor() at each navigation instead.
  if (!page.web || !page.web->page()) return;
  constexpr auto kScriptName = "dalinira-web-audio-document-bootstrap";
  QWebEngineScriptCollection &scripts = page.web->page()->scripts();
  for (const QWebEngineScript &existing : scripts.find(QString::fromLatin1(kScriptName))) scripts.remove(existing);
  if (!enabled_ || !isAudioEligibleView(page.web, url)) return;
  QWebEngineScript script;
  script.setName(QString::fromLatin1(kScriptName));
  script.setSourceCode(injectionScript());
  ++diagnostics_.scriptsSent;
  diagnostics_.scriptBytesSent += quint64(script.sourceCode().size());
  script.setInjectionPoint(QWebEngineScript::DocumentReady);
  script.setWorldId(QWebEngineScript::MainWorld);
  script.setRunsOnSubFrames(false);
  scripts.insert(script);
}

QList<dalinira::engine::DocumentScript> WebAudioEffectsController::documentScriptsFor(const QObject *view,
                                                                                       const QUrl &url) const {
  if (!enabled_ || !isAudioEligibleView(view, url)) return {};
  if (moduleSource(OutputModule).isEmpty() || moduleSource(EqModule).isEmpty()) return {};
  dalinira::engine::DocumentScript script;
  script.source = engineOptionsScript(view) + injectionScript();
  script.mainWorld = true;  // the page's own media elements and AudioContext
  script.allFrames = false;
  ++diagnostics_.scriptsSent;
  diagnostics_.scriptBytesSent += quint64(script.source.size());
  return {script};
}

void WebAudioEffectsController::updateAudioPolicyForPage(const Page &page, const QUrl &url) {
  if (!pageAlive(page)) return;
  page.object->setProperty("dalinira-audio-page-url", url);
  if (!isAudioEligibleView(page.object, url)) {
    bootstrapPages_.remove(page.object);
    page.object->setProperty("dalinira-audio-graph-active", false);
  }
  installDocumentBootstrap(page, url);
}

void WebAudioEffectsController::bootstrapPage(const Page &page) {
  if (!pageAlive(page) || bootstrapPages_.contains(page.object)) return;
  if (!isAudioEligibleView(page.object, pageUrl(page))) return;
  // Output and EQ make the graph; the other modules are optional and a page
  // simply lacks the one that is unavailable.
  if (moduleSource(OutputModule).isEmpty() || moduleSource(EqModule).isEmpty()) {
    status_.engineAvailable = false;
    status_.enabled = enabled_;
    status_.detail = I18n::text(QStringLiteral("audio.status_module_missing"));
    status_.detailKey = QStringLiteral("audio.status_module_missing");
    status_.hardFailure = true;
    qWarning().noquote() << "[DaliNira DSP]" << status_.detail;
    emit statusChanged(status_);
    return;
  }
  const QObject *key = page.object;
  page.object->setProperty("dalinira-audio-page-url", pageUrl(page));
  bootstrapPages_.insert(key);
  ++diagnostics_.bootstrapsSent;
  const QPointer<WebAudioEffectsController> guardedController(this);
  const QPointer<QObject> guardedView(page.object);
  // The runtime is an async function (a Promise, which WebKit cannot return
  // to the browser); its state is reported by the next parameter update.
  const QString bootstrap = engineOptionsScript(page.object) + injectionScript() + QStringLiteral(";\nundefined;");
  runInPage(page, bootstrap, [guardedController, guardedView, key](const QVariant &result) {
    if (!guardedController) return;
    guardedController->bootstrapPages_.remove(key);
    if (!result.isValid() || result.toMap().isEmpty()) return;  // destroyed page, or no report
    markGraphState(guardedView, result);
    guardedController->updateStatusFromResult(result);
  });
}

void WebAudioEffectsController::updateStatusFromResult(const QVariant &result) {
  const QVariantMap map = result.toMap();
  Status next;
  next.enabled = map.value(QStringLiteral("enabled"), enabled_).toBool();
  next.engineAvailable = map.value(QStringLiteral("moduleLoaded")).toBool() && map.value(QStringLiteral("ok")).toBool();
  next.attachedMediaCount = std::max(0, map.value(QStringLiteral("mediaCount")).toInt());
  for (const QVariant &rate : map.value(QStringLiteral("sampleRates")).toList()) {
    const int value = rate.toInt();
    if (value > 0 && !next.sampleRates.contains(value)) next.sampleRates.push_back(value);
  }
  const QString error = map.value(QStringLiteral("error")).toString();
  if (!error.isEmpty()) {
    next.detail = I18n::text(QStringLiteral("audio.status_error")).arg(error);
    next.detailKey = QStringLiteral("audio.status_error");
    next.detailArgs = {error};
    next.hardFailure = true;
    qWarning().noquote() << "[DaliNira DSP]" << next.detail;
  }
  else if (!enabled_) {
    next.detail = I18n::text(QStringLiteral("audio.status_bypassed"));
    next.detailKey = QStringLiteral("audio.status_bypassed");
  }
  else if (!next.engineAvailable) {
    next.detail = I18n::text(QStringLiteral("audio.status_graph_failed"));
    next.detailKey = QStringLiteral("audio.status_graph_failed");
    next.hardFailure = true;
  }
  else if (next.attachedMediaCount == 0) {
    next.detail = I18n::text(QStringLiteral("audio.status_no_media"));
    next.detailKey = QStringLiteral("audio.status_no_media");
  } else {
    next.detail = I18n::text(QStringLiteral("audio.status_attached")).arg(next.attachedMediaCount);
    next.detailKey = QStringLiteral("audio.status_attached");
    next.detailArgs = {QString::number(next.attachedMediaCount)};
  }
  const bool changed = status_.enabled != next.enabled || status_.engineAvailable != next.engineAvailable
      || status_.attachedMediaCount != next.attachedMediaCount || status_.sampleRates != next.sampleRates
      || status_.detail != next.detail;
  if (!changed) return;
  status_ = next;
  emit statusChanged(status_);
}
