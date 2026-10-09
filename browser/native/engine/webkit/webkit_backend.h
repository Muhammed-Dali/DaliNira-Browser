#pragma once

#include "engine/engine_backend.h"
#include "engine/webkit/webkit_engine_profile.h"
#include "engine/webkit/webkit_network_filter.h"

namespace dalinira::engine {

class WebKitBackend : public EngineBackend {
  Q_OBJECT
 public:
  explicit WebKitBackend(QObject *parent = nullptr);
  ~WebKitBackend() override;

  EngineType engineType() const override { return EngineType::WebKit; }
  QString name() const override { return QStringLiteral("WebKit"); }
  QString layoutEngine() const override { return QStringLiteral("WebCore"); }
  QString jsEngine() const override { return QStringLiteral("JavaScriptCore"); }
  QString versionString() const override;

  bool supportsCapability(EngineCapability cap) const override;

  bool initialize(const EngineStorageManager &storage) override;
  EngineView *createView(QWidget *parent = nullptr) override;
  EngineView *createEphemeralView(QWidget *parent = nullptr) override;
  EngineProfile *profile() const override { return profile_.get(); }
  bool clearSiteData(const QString &host) override;
  WebKitEngineProfile *ephemeralProfile();

  WebKitEngineProfile *webKitProfile() const { return profile_.get(); }
  // Applies to the persistent and the private profile, and to a private
  // profile created later.
  void setCookiePolicy(const QString &policy) override;

  // Starts the network filter used by DaliNira's web process extension for
  // every profile. Call before the first view loads; views then take their
  // decisions from WebKitEngineView::setNetworkRequestEvaluator().
  bool enableNetworkFiltering();
  WebKitNetworkFilter *networkFilter() const { return networkFilter_.get(); }

  // At exit, after EngineBackend::shutdown(): WebKit keeps a web process for
  // a service worker until it ends the idle worker (SWServer's 10 s delay,
  // possibly re-armed once), and that process keeps the network process.
  // Exiting before then cuts its connection while the worker still runs, and
  // it can crash (SIGSEGV in its exit path). Keeps the event loop running
  // until this process's WebKit web processes have exited, or `timeoutMs`
  // passed; returns at once when there are none. True when none is left.
  static bool waitForRetainedWebProcesses(int timeoutMs = 25000);

 private:
  std::unique_ptr<WebKitEngineProfile> profile_;
  std::unique_ptr<WebKitEngineProfile> ephemeralProfile_;
  std::unique_ptr<WebKitNetworkFilter> networkFilter_;
  QString cookiePolicy_ = QStringLiteral("third_party");
};

} // namespace dalinira::engine
