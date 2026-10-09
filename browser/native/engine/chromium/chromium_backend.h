#pragma once

#include "engine/engine_backend.h"
#include <QWebEngineProfile>

namespace dalinira::engine {

class ChromiumBackend : public EngineBackend {
  Q_OBJECT
 public:
  explicit ChromiumBackend(QWebEngineProfile *profile = nullptr, QObject *parent = nullptr);
  ~ChromiumBackend() override;

  EngineType engineType() const override { return EngineType::Chromium; }
  QString name() const override { return QStringLiteral("Chromium"); }
  QString layoutEngine() const override { return QStringLiteral("Blink"); }
  QString jsEngine() const override { return QStringLiteral("V8"); }
  bool supportsCapability(EngineCapability cap) const override { Q_UNUSED(cap); return true; }

  bool initialize(const EngineStorageManager &storage) override;
  EngineView *createView(QWidget *parent = nullptr) override;
  EngineProfile *profile() const override { return profileWrapper_.get(); }

  QWebEngineProfile *webEngineProfile() const { return profile_; }

 private:
  QWebEngineProfile *profile_ = nullptr;
  std::unique_ptr<EngineProfile> profileWrapper_;
};

} // namespace dalinira::engine
