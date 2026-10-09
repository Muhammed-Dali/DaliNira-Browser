#pragma once

#include <QObject>
#include <QString>
#include <memory>
#include "engine/engine_type.h"
#include "engine/engine_view.h"
#include "engine/engine_profile.h"
#include "engine/engine_storage_manager.h"

#include "engine/engine_capabilities.h"

namespace dalinira::engine {

class EngineBackend : public QObject {
  Q_OBJECT
 public:
  explicit EngineBackend(QObject *parent = nullptr) : QObject(parent) {}
  ~EngineBackend() override = default;

  virtual EngineType engineType() const = 0;
  virtual QString name() const = 0;
  virtual QString layoutEngine() const = 0;
  virtual QString jsEngine() const = 0;
  // User-facing engine and version, e.g. "WebKitGTK 2.50.1"; empty when the
  // backend cannot tell (the caller then words it itself).
  virtual QString versionString() const { return {}; }
  virtual bool supportsCapability(EngineCapability cap) const = 0;

  virtual bool initialize(const EngineStorageManager &storage) = 0;
  virtual EngineView *createView(QWidget *parent = nullptr) = 0;
  virtual EngineView *createEphemeralView(QWidget *parent = nullptr) { return createView(parent); }
  virtual EngineProfile *profile() const = 0;
  // Removes persistent website data for one registrable host when the engine
  // exposes a genuinely origin-scoped API. False means unsupported; callers
  // must not substitute a profile-wide clear.
  virtual bool clearSiteData(const QString &host) { Q_UNUSED(host); return false; }
  // DaliNira's cookie policy ("third_party": block third-party cookies,
  // "allow_all", "block_all") for engines whose cookie store enforces it
  // itself. Chromium's is enforced per request by the profile's cookie filter.
  virtual void setCookiePolicy(const QString &policy) { Q_UNUSED(policy); }

  static EngineType selectedEngineType() {
    return selectedEngineTypeHolder().load();
  }
  static void setSelectedEngineType(EngineType type) {
    selectedEngineTypeHolder().store(type);
  }
  static EngineBackend *instance();
  static EngineBackend *backendForType(EngineType type);
  static void registerBackend(EngineType type, std::unique_ptr<EngineBackend> backend);
  static void setInstance(std::unique_ptr<EngineBackend> backend);
  static void shutdown();

 private:
  static std::atomic<EngineType> &selectedEngineTypeHolder() {
    static std::atomic<EngineType> s_type{EngineType::Chromium};
    return s_type;
  }
};

} // namespace dalinira::engine
