#pragma once

#include <QSet>

namespace dalinira::engine {

enum class EngineCapability {
  Navigation,
  JavaScript,
  History,
  Popups,
  PrivateProfile,
  Favicons,
  Downloads,
  AdBlock,
  AdultContentProtection,
  Permissions,
  AudioEffects,
  FindInPage,
  Zoom,
  Fullscreen,
  Print,
  DevTools,
  MediaRouting
};

class EngineCapabilities {
 public:
  bool supports(EngineCapability cap) const { return caps_.contains(cap); }
  void setSupported(EngineCapability cap, bool supported = true) {
    if (supported) {
      caps_.insert(cap);
    } else {
      caps_.remove(cap);
    }
  }

 private:
  QSet<EngineCapability> caps_;
};

} // namespace dalinira::engine
