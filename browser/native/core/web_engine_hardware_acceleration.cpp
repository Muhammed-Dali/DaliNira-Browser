#include "web_engine_hardware_acceleration.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <cstdlib>

namespace dalinira {

namespace {

HardwareAccelerationStatus g_cachedStatus;
bool g_initialized = false;

QStringList extractFeaturesFromFlag(const QString &flag) {
  const int eqIdx = flag.indexOf(QLatin1Char('='));
  if (eqIdx < 0) return QStringList();
  const QString val = flag.mid(eqIdx + 1).trimmed();
  return val.split(QLatin1Char(','), Qt::SkipEmptyParts);
}

}  // namespace

HardwareAccelerationStatus WebEngineHardwareAcceleration::detectCapability(const QString &driDir,
                                                                         const QString &devDriDir) {
  HardwareAccelerationStatus status;

  // 1. Check software-only environment overrides
  if (qEnvironmentVariableIsSet("DALINIRA_DISABLE_VAAPI") ||
      qEnvironmentVariableIntValue("DALINIRA_DISABLE_VAAPI") == 1 ||
      qEnvironmentVariableIsSet("LIBGL_ALWAYS_SOFTWARE")) {
    status.videoDecodeCapability = HardwareVideoDecodeCapability::DisabledByDriver;
    return status;
  }

  // 2. Discover DRM render nodes
  const QDir devDri(devDriDir);
  if (devDri.exists()) {
    const QStringList renderEntries = devDri.entryList(
        QStringList{QStringLiteral("renderD*"), QStringLiteral("card*")},
        QDir::System | QDir::Files | QDir::Readable | QDir::Writable);
    if (!renderEntries.isEmpty()) {
      status.drmRenderNodeFound = true;
      status.drmRenderNodePath = devDri.filePath(renderEntries.constFirst());
    }
  }

  // 3. Discover installed VA-API driver libraries
  const QStringList driverCandidates = {
      QStringLiteral("iHD_drv_video.so"),
      QStringLiteral("i965_drv_video.so"),
      QStringLiteral("radeonsi_drv_video.so"),
      QStringLiteral("r600_drv_video.so"),
      QStringLiteral("nouveau_drv_video.so"),
      QStringLiteral("nvidia_drv_video.so")
  };

  QStringList searchDirs = { driDir };
  if (driDir == QLatin1String("/usr/lib/dri")) {
    searchDirs << QStringLiteral("/usr/lib64/dri")
               << QStringLiteral("/usr/lib/x86_64-linux-gnu/dri");
  }

  for (const QString &dirPath : searchDirs) {
    const QDir d(dirPath);
    if (!d.exists()) continue;
    for (const QString &candidate : driverCandidates) {
      if (d.exists(candidate)) {
        status.vaapiDriverFound = true;
        status.vaapiDriverPath = d.filePath(candidate);
        break;
      }
    }
    if (status.vaapiDriverFound) break;
  }

  // 4. Determine final capability
  if (status.drmRenderNodeFound && status.vaapiDriverFound) {
    status.videoDecodeCapability = HardwareVideoDecodeCapability::Supported;
  } else {
    status.videoDecodeCapability = HardwareVideoDecodeCapability::Unsupported;
  }

  return status;
}

QStringList WebEngineHardwareAcceleration::standardHardwareFlags(const HardwareAccelerationStatus &status) {
  if (status.videoDecodeCapability != HardwareVideoDecodeCapability::Supported) {
    return QStringList();
  }

  return QStringList{
      // Qt 6.11 embeds Chromium 140. Standard stable VA-API Linux hardware video decode
      // without experimental zero-copy DMABUF surface import mode (which fails with
      // "vaCreateSurfaces (import mode) failed, VA error: resource allocation failed").
      QStringLiteral("--enable-features=AcceleratedVideoDecoder,AcceleratedVideoDecodeLinuxGL"),
      QStringLiteral("--use-gl=angle"),
      QStringLiteral("--use-angle=gl"),
      QStringLiteral("--enable-gpu-rasterization")
  };
}

QString WebEngineHardwareAcceleration::mergeChromiumFlags(const QString &existingFlags,
                                                         const QStringList &hardwareFlags) {
  const QStringList existingTokens = existingFlags.split(QLatin1Char(' '), Qt::SkipEmptyParts);

  QSet<QString> enabledFeatures;
  QSet<QString> disabledFeatures;
  QStringList otherFlags;

  // Parse existing flags
  for (const QString &token : existingTokens) {
    if (token.startsWith(QStringLiteral("--enable-features="))) {
      for (const QString &feat : extractFeaturesFromFlag(token)) {
        enabledFeatures.insert(feat.trimmed());
      }
    } else if (token.startsWith(QStringLiteral("--disable-features="))) {
      for (const QString &feat : extractFeaturesFromFlag(token)) {
        disabledFeatures.insert(feat.trimmed());
      }
    } else {
      if (!otherFlags.contains(token)) {
        otherFlags.append(token);
      }
    }
  }

  // Merge hardware flags
  for (const QString &token : hardwareFlags) {
    if (token.startsWith(QStringLiteral("--enable-features="))) {
      for (const QString &feat : extractFeaturesFromFlag(token)) {
        enabledFeatures.insert(feat.trimmed());
      }
    } else if (token.startsWith(QStringLiteral("--disable-features="))) {
      for (const QString &feat : extractFeaturesFromFlag(token)) {
        disabledFeatures.insert(feat.trimmed());
      }
    } else {
      if (!otherFlags.contains(token)) {
        otherFlags.append(token);
      }
    }
  }

  QStringList mergedTokens;

  if (!enabledFeatures.isEmpty()) {
    QStringList sortedEnabled = enabledFeatures.values();
    sortedEnabled.sort();
    mergedTokens.append(QStringLiteral("--enable-features=%1").arg(sortedEnabled.join(QLatin1Char(','))));
  }

  if (!disabledFeatures.isEmpty()) {
    QStringList sortedDisabled = disabledFeatures.values();
    sortedDisabled.sort();
    mergedTokens.append(QStringLiteral("--disable-features=%1").arg(sortedDisabled.join(QLatin1Char(','))));
  }

  mergedTokens.append(otherFlags);

  return mergedTokens.join(QLatin1Char(' '));
}

void WebEngineHardwareAcceleration::initializeEarlyRuntime(const QString &driDir,
                                                          const QString &devDriDir) {
  g_cachedStatus = detectCapability(driDir, devDriDir);
  g_cachedStatus.appliedChromiumFlags = standardHardwareFlags(g_cachedStatus);

  const QString existing = QString::fromLocal8Bit(qgetenv("QTWEBENGINE_CHROMIUM_FLAGS"));
  g_cachedStatus.mergedChromiumFlagsString = mergeChromiumFlags(existing, g_cachedStatus.appliedChromiumFlags);

  if (!g_cachedStatus.mergedChromiumFlagsString.isEmpty()) {
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", g_cachedStatus.mergedChromiumFlagsString.toUtf8());
  }

  g_initialized = true;
}

QStringList WebEngineHardwareAcceleration::privacyFlags() {
  return {
      QStringLiteral("--site-per-process"),
      QStringLiteral("--enable-features=SplitCacheByNetworkIsolationKey,SplitCodeCacheByNetworkIsolationKey,"
                     "SplitAuthCacheByNetworkIsolationKey,SplitCacheByCrossSiteMainFrameNavigationBoolean,"
                     "PartitionConnectionsByNetworkIsolationKey,UseDnsHttpsSvcb,EncryptedClientHello")
  };
}

void WebEngineHardwareAcceleration::applyPrivacyFlags() {
  const QString existing = QString::fromLocal8Bit(qgetenv("QTWEBENGINE_CHROMIUM_FLAGS"));
  qputenv("QTWEBENGINE_CHROMIUM_FLAGS", mergeChromiumFlags(existing, privacyFlags()).toUtf8());
}

HardwareAccelerationStatus WebEngineHardwareAcceleration::currentStatus() {
  if (!g_initialized) {
    g_cachedStatus = detectCapability();
  }
  return g_cachedStatus;
}

void WebEngineHardwareAcceleration::resetForTesting() {
  g_initialized = false;
  g_cachedStatus = HardwareAccelerationStatus();
}

}  // namespace dalinira
