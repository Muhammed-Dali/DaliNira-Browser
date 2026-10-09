#include "engine/engine_backend.h"
#include <map>

namespace dalinira::engine {

namespace {
std::unique_ptr<EngineBackend> s_instance = nullptr;
std::map<EngineType, std::unique_ptr<EngineBackend>> s_registeredBackends;
} // namespace

EngineBackend *EngineBackend::instance() {
  auto it = s_registeredBackends.find(selectedEngineType());
  if (it != s_registeredBackends.end()) {
    return it->second.get();
  }
  if (s_instance) {
    return s_instance.get();
  }
  auto fallbackIt = s_registeredBackends.find(EngineType::Chromium);
  if (fallbackIt != s_registeredBackends.end()) {
    return fallbackIt->second.get();
  }
  return nullptr;
}

EngineBackend *EngineBackend::backendForType(EngineType type) {
  auto it = s_registeredBackends.find(type);
  if (it != s_registeredBackends.end()) {
    return it->second.get();
  }
  return nullptr;
}

void EngineBackend::registerBackend(EngineType type, std::unique_ptr<EngineBackend> backend) {
  s_registeredBackends[type] = std::move(backend);
}

void EngineBackend::setInstance(std::unique_ptr<EngineBackend> backend) {
  s_instance = std::move(backend);
}

void EngineBackend::shutdown() {
  // Views must already be gone when this is called. Destroy profiles/contexts
  // while QApplication and both Qt/GLib platform integrations still exist.
  s_registeredBackends.clear();
  s_instance.reset();
}

} // namespace dalinira::engine
