#include "engine/chromium/chromium_backend.h"
#include "engine/chromium/chromium_engine_view.h"
#include <QWebEngineView>
#include <QWebEnginePage>

namespace dalinira::engine {

ChromiumBackend::ChromiumBackend(QWebEngineProfile *profile, QObject *parent)
    : EngineBackend(parent), profile_(profile ? profile : QWebEngineProfile::defaultProfile()) {
  profileWrapper_ = std::make_unique<EngineProfile>(EngineType::Chromium, profile_->persistentStoragePath(), this);
}

ChromiumBackend::~ChromiumBackend() = default;

bool ChromiumBackend::initialize(const EngineStorageManager &storage) {
  storage.ensureDirectories(EngineType::Chromium);
  return true;
}

EngineView *ChromiumBackend::createView(QWidget *parent) {
  auto *view = new QWebEngineView(parent);
  auto *page = new QWebEnginePage(profile_, view);
  view->setPage(page);
  return new ChromiumEngineView(view, parent);
}

} // namespace dalinira::engine
