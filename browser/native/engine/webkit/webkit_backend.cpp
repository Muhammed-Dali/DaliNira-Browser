#include "engine/webkit/webkit_backend.h"
#include "engine/webkit/webkit_engine_view.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QThread>
#include <unistd.h>

// WebKitGTK's headers use `signals` as an identifier; Qt headers come first.
#undef signals
#include <webkit2/webkit2.h>

namespace dalinira::engine {

WebKitBackend::WebKitBackend(QObject *parent)
    : EngineBackend(parent) {}

WebKitBackend::~WebKitBackend() {
  // Views are gone by now; stop answering web processes before the profiles
  // (and with them the web processes) are released.
  networkFilter_.reset();
}

bool WebKitBackend::enableNetworkFiltering() {
  if (!networkFilter_) networkFilter_ = std::make_unique<WebKitNetworkFilter>();
  if (!networkFilter_->start()) return false;
  if (profile_) profile_->setNetworkFilter(networkFilter_.get());
  if (ephemeralProfile_) ephemeralProfile_->setNetworkFilter(networkFilter_.get());
  return true;
}

QString WebKitBackend::versionString() const {
  return QStringLiteral("WebKitGTK %1.%2.%3")
      .arg(webkit_get_major_version())
      .arg(webkit_get_minor_version())
      .arg(webkit_get_micro_version());
}

bool WebKitBackend::supportsCapability(EngineCapability cap) const {
  switch (cap) {
    case EngineCapability::Navigation:
    case EngineCapability::JavaScript:
    case EngineCapability::History:
    case EngineCapability::Popups:
    case EngineCapability::PrivateProfile:
    case EngineCapability::Favicons:
    case EngineCapability::AdultContentProtection:
    case EngineCapability::Downloads:
    case EngineCapability::Zoom:
    case EngineCapability::Fullscreen:
    case EngineCapability::FindInPage:
    case EngineCapability::Print:
      return true;
    case EngineCapability::AdBlock:
      // Cosmetic filtering always runs; network blocking needs the extension.
      return networkFilter_ && networkFilter_->isRunning();
    case EngineCapability::AudioEffects:
    case EngineCapability::Permissions:
    case EngineCapability::DevTools:
    case EngineCapability::MediaRouting:
      return false;
  }
  return false;
}

bool WebKitBackend::initialize(const EngineStorageManager &storage) {
  storage.ensureDirectories(EngineType::WebKit);
  profile_ = std::make_unique<WebKitEngineProfile>(
      false,
      storage.siteStorageDir(EngineType::WebKit),
      storage.cacheDir(EngineType::WebKit),
      storage.cookiesPath(EngineType::WebKit),
      this);
  profile_->setCookiePolicy(cookiePolicy_);
  return true;
}

WebKitEngineProfile *WebKitBackend::ephemeralProfile() {
  if (!ephemeralProfile_) {
    ephemeralProfile_ = std::make_unique<WebKitEngineProfile>(true, QString{}, QString{}, QString{}, this);
    ephemeralProfile_->setCookiePolicy(cookiePolicy_);
    if (networkFilter_ && networkFilter_->isRunning()) ephemeralProfile_->setNetworkFilter(networkFilter_.get());
  }
  return ephemeralProfile_.get();
}

void WebKitBackend::setCookiePolicy(const QString &policy) {
  cookiePolicy_ = policy;
  if (profile_) profile_->setCookiePolicy(policy);
  if (ephemeralProfile_) ephemeralProfile_->setCookiePolicy(policy);
}

namespace {
// Live WebKitWebProcess descendants of this process (as /proc shows them).
QList<int> webProcessDescendants() {
  QHash<int, int> parent;
  QList<int> webProcesses;
  for (const QString &entry : QDir(QStringLiteral("/proc")).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
    bool numeric = false;
    const int pid = entry.toInt(&numeric);
    if (!numeric) continue;
    QFile stat(QStringLiteral("/proc/%1/stat").arg(pid));
    if (!stat.open(QIODevice::ReadOnly)) continue;
    const QByteArray text = stat.readAll();
    const qsizetype open = text.indexOf('('), close = text.lastIndexOf(')');
    if (open < 0 || close < open) continue;
    const QList<QByteArray> fields = text.mid(close + 2).split(' ');
    if (fields.size() < 2 || fields.at(0) == "Z") continue;  // a zombie has exited
    parent.insert(pid, fields.at(1).toInt());
    if (text.mid(open + 1, close - open - 1).startsWith("WebKitWebProces")) webProcesses.append(pid);
  }
  const int self = static_cast<int>(getpid());
  QList<int> result;
  for (const int pid : webProcesses) {
    for (int p = parent.value(pid), depth = 0; p > 1 && depth < 16; p = parent.value(p), ++depth) {
      if (p == self) {
        result.append(pid);
        break;
      }
    }
  }
  return result;
}
}  // namespace

bool WebKitBackend::waitForRetainedWebProcesses(int timeoutMs) {
  QElapsedTimer timer;
  timer.start();
  // Process termination is negotiated over IPC: keep (GLib) events flowing.
  // /proc is read at most every 50 ms.
  while (!webProcessDescendants().isEmpty()) {
    if (timer.elapsed() >= timeoutMs) return false;
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    QThread::msleep(50);
  }
  return true;
}

EngineView *WebKitBackend::createView(QWidget *parent) {
  return new WebKitEngineView(profile_.get(), parent);
}

EngineView *WebKitBackend::createEphemeralView(QWidget *parent) {
  return new WebKitEngineView(ephemeralProfile(), parent);
}

bool WebKitBackend::clearSiteData(const QString &host) {
  return profile_ && profile_->clearSiteData(host);
}

} // namespace dalinira::engine
