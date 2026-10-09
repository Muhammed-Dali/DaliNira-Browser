#pragma once

#include <QObject>
#include <QHash>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QStringList>
#include <QVector>
#include <functional>

class QWebEngineView;

namespace dalinira::engine {
class EngineView;
struct DocumentScript;
}

#include "../desktop_tabs/tab_performance_manager.h"

// DaliNira's one Web Audio effects engine (EQ, tone, reverb, compressor,
// limiter, bass enhancer, auto gain) for web media, shared by both browser
// engines. The DSP runs in each page as a Web Audio graph built from the DALI
// modules compiled from browser/dali/*.dl. Engines only differ in how a script
// reaches the page: Chromium tabs register their QWebEngineView, other engines
// (WebKit) their EngineView; everything else is common.
class WebAudioEffectsController final : public QObject {
  Q_OBJECT

 public:
  struct Status {
    bool engineAvailable = false;
    bool enabled = true;
    int attachedMediaCount = 0;
    QVector<int> sampleRates;
    QString detail;
    // The DSP graph cannot run (module missing, graph/runtime error). Kept as
    // state so the UI never has to parse the localized `detail` text.
    bool hardFailure = false;
    // Catalog key + arguments behind `detail`, so the UI can re-render it in
    // the current language after a live switch.
    QString detailKey;
    QStringList detailArgs;
  };

  // Lightweight counters (tests; DALINIRA_FEATURE_DIAGNOSTICS logging).
  struct Diagnostics {
    quint64 scriptsSent = 0;        // scripts run in, or installed into, pages
    quint64 scriptBytesSent = 0;
    quint64 bootstrapsSent = 0;     // full DALI runtime deliveries
    quint64 injectionBuilds = 0;    // full runtime script (re)builds
    quint64 moduleLoads = 0;        // DALI module files read (once each)
    quint64 moduleRejects = 0;      // DALI module files that failed validation
  };

  // A compiled DALI module (build/dali/*.generated.js, from a .dl preset).
  // Rejects empty, oversized, non-UTF-8, binary, foreign, truncated or
  // unsupported-version files; `error` names the reason.
  static bool validateDaliModule(const QByteArray &bytes, QString *error = nullptr);
  static constexpr qint64 kMaxDaliModuleBytes = 512 * 1024;

  explicit WebAudioEffectsController(QObject *parent = nullptr, bool persistenceEnabled = true);
  ~WebAudioEffectsController() override;

  bool enabled() const { return enabled_; }
  bool persistenceEnabled() const { return persistenceEnabled_; }
  double preampDb() const { return preampDb_; }
  static const QVector<int> &equalizerFrequencies();
  QVector<double> equalizerBands() const { return equalizerBands_; }
  double equalizerBand(int index) const;
  bool isEqualizerActive() const;
  double bassDb() const { return bassDb_; }
  double midDb() const { return midDb_; }
  double trebleDb() const { return trebleDb_; }
  bool isToneActive() const;
  double stereoExpanderPercent() const { return stereoExpanderPercent_; }
  double balance() const { return balance_; }
  bool isSpatialActive() const;
  QString acousticSpace() const { return acousticSpace_; }
  // Module state is deliberately separate from the master Web DSP switch.
  // Stages 3-6 give Reverb, Dynamic Compressor, Limiter and Bass Enhancer processing
  // implementations; the same persistence surface is reserved for later stages.
  bool moduleEnabled(const QString &moduleId) const;
  bool reverbEnabled() const;
  double reverbRoomSizeMs() const { return reverbRoomSizeMs_; }
  double reverbDamping() const { return reverbDamping_; }
  double reverbWetDryDb() const { return reverbWetDryDb_; }
  double reverbHfRatio() const { return reverbHfRatio_; }
  double reverbInputGainDb() const { return reverbInputGainDb_; }
  QString reverbPreset() const { return reverbPreset_; }
  bool compressorEnabled() const;
  double compressorThresholdDb() const { return compressorThresholdDb_; }
  double compressorRatio() const { return compressorRatio_; }
  double compressorAttackMs() const { return compressorAttackMs_; }
  double compressorReleaseMs() const { return compressorReleaseMs_; }
  double compressorMakeupDb() const { return compressorMakeupDb_; }
  double compressorKneeDb() const { return compressorKneeDb_; }
  QString compressorPreset() const { return compressorPreset_; }
  bool limiterEnabled() const;
  double limiterCeilingDb() const { return limiterCeilingDb_; }
  double limiterReleaseMs() const { return limiterReleaseMs_; }
  double limiterLookaheadMs() const { return limiterLookaheadMs_; }
  double limiterGainDb() const { return limiterGainDb_; }
  QString limiterPreset() const { return limiterPreset_; }
  bool bassEnhancerEnabled() const;
  double bassEnhancerFrequencyHz() const { return bassEnhancerFrequencyHz_; }
  double bassEnhancerGainDb() const { return bassEnhancerGainDb_; }
  double bassEnhancerHarmonicsPercent() const { return bassEnhancerHarmonicsPercent_; }
  double bassEnhancerWidth() const { return bassEnhancerWidth_; }
  double bassEnhancerMixPercent() const { return bassEnhancerMixPercent_; }
  bool bassEnhancerDeep() const { return bassEnhancerDeep_; }
  bool autoGainEnabled() const;
  double autoGainTargetDbfs() const { return autoGainTargetDbfs_; }
  double autoGainMaxGainDb() const { return autoGainMaxGainDb_; }
  QString autoGainSpeed() const { return autoGainSpeed_; }
  QString autoGainPreset() const { return autoGainPreset_; }
  dalinira::PerformancePolicyMode performancePolicyMode() const { return policyMode_; }
  bool isPanelVisible() const { return panelVisible_; }
  QString activeSubpanelId() const { return activeSubpanelId_; }
  Status status() const { return status_; }
  Diagnostics diagnostics() const { return diagnostics_; }
  void resetDiagnostics() { diagnostics_ = {}; }

  // Chromium tabs.
  void registerWebView(QWebEngineView *view, const QUrl &initialUrl = QUrl());
  void unregisterWebView(QWebEngineView *view);
  void applyToView(QWebEngineView *view);
  // Tabs of other engines (WebKit), through the engine-neutral EngineView.
  void registerEngineView(dalinira::engine::EngineView *view, const QUrl &initialUrl = QUrl());
  void unregisterEngineView(dalinira::engine::EngineView *view);
  void applyToEngineView(dalinira::engine::EngineView *view);
  // The runtime for engines that install document scripts at each navigation
  // (WebKit): nothing while effects are off or for a page they never touch.
  QList<dalinira::engine::DocumentScript> documentScriptsFor(const QObject *view, const QUrl &url) const;
  int registeredPageCount() const;
  int audioEnabledWebViewCount() const;
  int activeGraphViewCount() const;

 public slots:
  void setEnabled(bool enabled);
  void setPreampDb(double db);
  void resetOutput();
  void setEqualizerBand(int index, double db);
  // A preset preview must affect the live WebAudio graph but must not write
  // QSettings.  Keeping this operation on the controller prevents a second
  // EQ state from drifting away from the DSP state.
  void previewEqualizerBands(const QVector<double> &bands);
  void commitEqualizerBands(const QVector<double> &bands);
  void setBassDb(double db);
  void setMidDb(double db);
  void setTrebleDb(double db);
  void setStereoExpanderPercent(double percent);
  void setBalance(double value);
  void setAcousticSpace(const QString &space);
  void resetEqualizer();
  void resetEqualizerModule();
  void setModuleEnabled(const QString &moduleId, bool enabled);
  void setReverbEnabled(bool enabled);
  void setReverbRoomSizeMs(double value);
  void setReverbDamping(double value);
  void setReverbWetDryDb(double value);
  void setReverbHfRatio(double value);
  void setReverbInputGainDb(double value);
  void applyReverbPreset(const QString &presetId);
  void resetReverb();
  void setCompressorEnabled(bool enabled);
  void setCompressorThresholdDb(double value);
  void setCompressorRatio(double value);
  void setCompressorAttackMs(double value);
  void setCompressorReleaseMs(double value);
  void setCompressorMakeupDb(double value);
  void setCompressorKneeDb(double value);
  void applyCompressorPreset(const QString &presetId);
  void resetCompressor();
  void requestCompressorGainReduction();
  void setLimiterEnabled(bool enabled);
  void setLimiterCeilingDb(double value);
  void setLimiterReleaseMs(double value);
  void setLimiterLookaheadMs(double value);
  void setLimiterGainDb(double value);
  void applyLimiterPreset(const QString &presetId);
  void resetLimiter();
  void requestLimiterReduction();
  void setBassEnhancerEnabled(bool enabled);
  void setBassEnhancerFrequencyHz(double value);
  void setBassEnhancerGainDb(double value);
  void setBassEnhancerHarmonicsPercent(double value);
  void setBassEnhancerWidth(double value);
  void setBassEnhancerMixPercent(double value);
  void applyBassEnhancerDeep();
  void resetBassEnhancer();
  void setAutoGainEnabled(bool enabled);
  void setAutoGainTargetDbfs(double value);
  void setAutoGainMaxGainDb(double value);
  void applyAutoGainPreset(const QString &presetId);
  void resetAutoGain();
  void setPerformancePolicyMode(dalinira::PerformancePolicyMode mode);
  void setPanelVisible(bool visible, const QString &activeSubpanelId = QString());
  void applyToAllWebViews();
  QString injectionScript() const;
  QString parameterUpdateScript() const;
  QString equalizerBandUpdateScript(int index) const;

 signals:
  void stateChanged();
  void statusChanged(const WebAudioEffectsController::Status &status);
  void compressorGainReductionChanged(double reductionDb, bool available);
  void limiterReductionChanged(double reductionDb, bool available);

 private:
  // A registered tab page, whichever engine renders it.
  struct Page {
    QPointer<QObject> object;
    QPointer<QWebEngineView> web;                   // Chromium
    QPointer<dalinira::engine::EngineView> engine;  // other engines
  };
  struct DaliModule {
    const char *relativePath = nullptr;
    bool loaded = false;
    QString source;  // empty when missing or invalid
  };
  enum ModuleIndex { OutputModule, EqModule, CompressorModule, LimiterModule, BassEnhancerModule, AutoGainModule, ModuleCount };

  QString moduleSource(ModuleIndex index) const;
  QString daliModuleSource() const { return moduleSource(OutputModule); }
  QString daliEqModuleSource() const { return moduleSource(EqModule); }
  QString daliCompressorModuleSource() const { return moduleSource(CompressorModule); }
  QString daliLimiterModuleSource() const { return moduleSource(LimiterModule); }
  QString daliBassEnhancerModuleSource() const { return moduleSource(BassEnhancerModule); }
  QString daliAutoGainModuleSource() const { return moduleSource(AutoGainModule); }
  QString buildInjectionScript() const;

  void registerPage(const Page &page, const QUrl &initialUrl);
  void unregisterPage(const QObject *object);
  const Page *findPage(const QObject *object) const;
  void prunePages();
  static QUrl pageUrl(const Page &page);
  static bool pageAlive(const Page &page);
  void runInPage(const Page &page, const QString &script, std::function<void(const QVariant &)> callback = {});
  void applyToPage(const Page &page);
  void applyEqualizerBandToPage(const Page &page, int index);
  void bootstrapPage(const Page &page);
  void installDocumentBootstrap(const Page &page, const QUrl &url);
  void updateAudioPolicyForPage(const Page &page, const QUrl &url);
  void updateStatusFromResult(const QVariant &result);
  void persist();
  void schedulePersist();
  void scheduleApply();
  void scheduleReverbStateChange(bool clearPreset);
  void scheduleCompressorStateChange(bool clearPreset);
  void scheduleLimiterStateChange(bool clearPreset);
  void scheduleBassEnhancerStateChange(bool clearDeep);
  void scheduleAutoGainStateChange();

  QVector<Page> pages_;
  QSet<const QObject *> bootstrapPages_;
  QSet<int> pendingEqualizerBands_;
  quint64 masterEnableGeneration_ = 0;
  bool forceFullReapply_ = false;
  bool enabled_ = true;
  double preampDb_ = 0.0;
  QVector<double> equalizerBands_;
  double bassDb_ = 0.0;
  double midDb_ = 0.0;
  double trebleDb_ = 0.0;
  double stereoExpanderPercent_ = 100.0;
  double balance_ = 0.0;
  QString acousticSpace_ = QStringLiteral("off");
  QHash<QString, bool> moduleEnabledStates_;
  double reverbRoomSizeMs_ = 1000.0;
  double reverbDamping_ = 0.5;
  double reverbWetDryDb_ = -10.0;
  double reverbHfRatio_ = 0.7;
  double reverbInputGainDb_ = 0.0;
  QString reverbPreset_;
  double compressorThresholdDb_ = -20.0;
  double compressorRatio_ = 4.0;
  double compressorAttackMs_ = 10.0;
  double compressorReleaseMs_ = 100.0;
  double compressorMakeupDb_ = 0.0;
  double compressorKneeDb_ = 3.0;
  QString compressorPreset_;
  bool compressorMeterRequestPending_ = false;
  quint64 compressorMeterRequestGeneration_ = 0;
  double limiterCeilingDb_ = -0.3;
  double limiterReleaseMs_ = 50.0;
  double limiterLookaheadMs_ = 5.0;
  double limiterGainDb_ = 0.0;
  QString limiterPreset_;
  bool limiterMeterRequestPending_ = false;
  quint64 limiterMeterRequestGeneration_ = 0;
  double bassEnhancerFrequencyHz_ = 80.0;
  double bassEnhancerGainDb_ = 6.0;
  double bassEnhancerHarmonicsPercent_ = 50.0;
  double bassEnhancerWidth_ = 1.5;
  double bassEnhancerMixPercent_ = 50.0;
  bool bassEnhancerDeep_ = false;
  double autoGainTargetDbfs_ = -14.0;
  double autoGainMaxGainDb_ = 12.0;
  QString autoGainSpeed_ = QStringLiteral("medium");
  QString autoGainPreset_ = QStringLiteral("balanced");
  dalinira::PerformancePolicyMode policyMode_ = dalinira::PerformancePolicyMode::Balanced;
  bool panelVisible_ = false;
  QString activeSubpanelId_;
  Status status_;
  mutable Diagnostics diagnostics_;
  mutable DaliModule modules_[ModuleCount];
  // The full runtime is ~50 KB plus the modules; it is rebuilt only when the
  // configuration it embeds changed (keyed by the parameter script).
  mutable QString injectionScriptCache_;
  mutable QString injectionScriptKey_;
  QTimer persistTimer_;
  QTimer applyTimer_;
  bool persistenceEnabled_ = true;
  friend int main(int argc, char *argv[]);
};

Q_DECLARE_METATYPE(WebAudioEffectsController::Status)
