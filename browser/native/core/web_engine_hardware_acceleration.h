#pragma once

#include <QString>
#include <QStringList>

namespace dalinira {

/**
 * @brief Categorized hardware video decoding capability on the current Linux host.
 */
enum class HardwareVideoDecodeCapability {
  Supported,         ///< DRM render node and valid VA-API driver library detected
  Unsupported,       ///< No hardware DRM render node or VA-API driver library found
  DisabledByDriver,  ///< Hardware decode explicitly suppressed (e.g. software OpenGL override)
  SoftwareFallback,  ///< Fallback initiated after runtime detection or simulation
  Unknown            ///< Initial unprobed state
};

/**
 * @brief Snapshot of hardware acceleration probing results and applied Chromium flags.
 */
struct HardwareAccelerationStatus {
  HardwareVideoDecodeCapability videoDecodeCapability = HardwareVideoDecodeCapability::Unknown;
  bool vaapiDriverFound = false;
  QString vaapiDriverPath;
  bool drmRenderNodeFound = false;
  QString drmRenderNodePath;
  QStringList appliedChromiumFlags;
  QString mergedChromiumFlagsString;
};

/**
 * @brief Centralized manager for QtWebEngine GPU and VA-API hardware video decode configuration.
 *
 * Provides deterministic capability discovery, safe software fallback, and conflict-free
 * Chromium flag merging before Qt/Chromium initialization.
 */
class WebEngineHardwareAcceleration {
 public:
  /**
   * @brief Detects system hardware video decode capability.
   * @param driDir Directory to scan for VA-API driver libraries.
   * @param devDriDir Directory to scan for DRM render nodes.
   */
  static HardwareAccelerationStatus detectCapability(const QString &driDir = QStringLiteral("/usr/lib/dri"),
                                                     const QString &devDriDir = QStringLiteral("/dev/dri"));

  /**
   * @brief Generates the standard verified list of Chromium hardware flags based on probe status.
   */
  static QStringList standardHardwareFlags(const HardwareAccelerationStatus &status);

  /**
   * @brief Merges hardware flags with any existing QTWEBENGINE_CHROMIUM_FLAGS without duplication.
   */
  static QString mergeChromiumFlags(const QString &existingFlags, const QStringList &hardwareFlags);

  /**
   * @brief Performs early runtime detection and sets QTWEBENGINE_CHROMIUM_FLAGS before QApplication startup.
   */
  static void initializeEarlyRuntime(const QString &driDir = QStringLiteral("/usr/lib/dri"),
                                    const QString &devDriDir = QStringLiteral("/dev/dri"));

  /**
   * @brief Chromium network-state partitioning QtWebEngine leaves off: the
   * HTTP cache (with its code cache, HTTP auth cache and Chrome's cross-site
   * navigation bit) and the connection pools are keyed by the top-level site,
   * as in Chrome (cache) and Brave (connections).
   */
  static QStringList privacyFlags();

  /**
   * @brief Merges privacyFlags() into QTWEBENGINE_CHROMIUM_FLAGS; call before
   * QtWebEngine starts.
   */
  static void applyPrivacyFlags();

  /**
   * @brief Returns the cached status of the last initialization probe.
   */
  static HardwareAccelerationStatus currentStatus();

  /**
   * @brief Resets the cached status (useful for unit testing).
   */
  static void resetForTesting();
};

}  // namespace dalinira
