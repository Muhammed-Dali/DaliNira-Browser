#include "download_desktop_actions.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QUrl>

#if defined(Q_OS_LINUX)
#include <QDBusConnection>
#include <QDBusMessage>
#endif

#include "local_media_routing.h"

namespace DownloadDesktopActions {

namespace {

static bool g_desktopLaunchExplicitlySet = false;
static bool g_desktopLaunchEnabled = true;

}  // namespace

void setDesktopLaunchEnabled(bool enabled) {
  g_desktopLaunchExplicitlySet = true;
  g_desktopLaunchEnabled = enabled;
}

bool isDesktopLaunchEnabled() {
  if (g_desktopLaunchExplicitlySet) {
    return g_desktopLaunchEnabled;
  }
  if (qEnvironmentVariableIsSet("DALINIRA_TEST_MODE") ||
      qEnvironmentVariableIsSet("DALINIRA_NO_DESKTOP_LAUNCH") ||
      qEnvironmentVariableIsSet("DALINIRA_NO_DESKTOP_ACTIONS")) {
    return false;
  }
  if (QCoreApplication::instance()) {
    const QString appName = QCoreApplication::applicationName();
    if (appName.contains(QLatin1String("Test"), Qt::CaseInsensitive)) {
      return false;
    }
  }
  if (QGuiApplication::instance() && QGuiApplication::platformName() == QLatin1String("offscreen")) {
    return false;
  }
  return true;
}

namespace {

bool openDirectorySafely(const QString &dirPath) {
  if (dirPath.isEmpty() || !QDir::isAbsolutePath(dirPath)) {
    return false;
  }
  const QFileInfo dirInfo(dirPath);
  if (!dirInfo.exists() || !dirInfo.isDir()) {
    return false;
  }
  const QString canonicalDir = dirInfo.canonicalFilePath();
  if (canonicalDir.isEmpty() || !QDir::isAbsolutePath(canonicalDir)) {
    return false;
  }

  // During tests, validate existence and directory integrity without launching desktop file manager
  if (!isDesktopLaunchEnabled()) {
    return true;
  }

#if defined(Q_OS_LINUX)
  if (QDBusConnection::sessionBus().isConnected()) {
    QDBusMessage msg = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.FileManager1"),
        QStringLiteral("/org/freedesktop/FileManager1"),
        QStringLiteral("org.freedesktop.FileManager1"),
        QStringLiteral("ShowFolders"));
    const QString uri = QUrl::fromLocalFile(canonicalDir).toString();
    msg << QStringList{uri} << QString{};
    QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::BlockWithGui, 1500);
    if (reply.type() == QDBusMessage::ReplyMessage) {
      return true;
    }
  }
#endif

  return QDesktopServices::openUrl(QUrl::fromLocalFile(canonicalDir));
}

}  // namespace

bool openFile(const QString &filePath, const QString &allowedRoot) {
  if (filePath.isEmpty() || !QDir::isAbsolutePath(filePath)) {
    return false;
  }
  const QFileInfo info(filePath);
  if (!info.exists() || !info.isFile() || !info.isReadable()) {
    return false;
  }
  const QString canonicalPath = info.canonicalFilePath();
  if (canonicalPath.isEmpty() || !QDir::isAbsolutePath(canonicalPath)) {
    return false;
  }

  if (!allowedRoot.isEmpty()) {
    if (!LocalMediaRouting::isSafeDownloadedFile(canonicalPath, allowedRoot)) {
      return false;
    }
  }

  const QUrl fileUrl = QUrl::fromLocalFile(canonicalPath);
  if (!fileUrl.isValid() || !fileUrl.isLocalFile()) {
    return false;
  }

  // During tests, validate file and permissions without launching external desktop application
  if (!isDesktopLaunchEnabled()) {
    return true;
  }

  return QDesktopServices::openUrl(fileUrl);
}

bool revealInFolder(const QString &filePath, const QString &fallbackDirectory) {
  if (!filePath.isEmpty() && QDir::isAbsolutePath(filePath)) {
    const QFileInfo fileInfo(filePath);
    if (fileInfo.exists() && fileInfo.isFile()) {
      const QString canonicalFilePath = fileInfo.canonicalFilePath();
      if (!canonicalFilePath.isEmpty()) {
        // During tests, validate file existence and directory integrity without launching desktop file manager
        if (!isDesktopLaunchEnabled()) {
          return true;
        }

#if defined(Q_OS_LINUX)
        if (QDBusConnection::sessionBus().isConnected()) {
          QDBusMessage msg = QDBusMessage::createMethodCall(
              QStringLiteral("org.freedesktop.FileManager1"),
              QStringLiteral("/org/freedesktop/FileManager1"),
              QStringLiteral("org.freedesktop.FileManager1"),
              QStringLiteral("ShowItems"));
          const QString uri = QUrl::fromLocalFile(canonicalFilePath).toString();
          msg << QStringList{uri} << QString{};
          QDBusMessage reply = QDBusConnection::sessionBus().call(msg, QDBus::BlockWithGui, 1500);
          if (reply.type() == QDBusMessage::ReplyMessage) {
            return true;
          }
        }
#endif
        const QString parentDir = fileInfo.absolutePath();
        return openDirectorySafely(parentDir);
      }
    }
  }

  // Fallback to directory if file doesn't exist or DBus/parent resolution failed
  QString dirToOpen = fallbackDirectory;
  if (dirToOpen.isEmpty() && !filePath.isEmpty()) {
    dirToOpen = QFileInfo(filePath).absolutePath();
  }
  return openDirectorySafely(dirToOpen);
}

}  // namespace DownloadDesktopActions
