#pragma once

#include <QString>

namespace DownloadDesktopActions {

// Configure whether desktop processes (file managers, media players) should be actually launched.
// During tests, this is automatically suppressed to prevent external popup windows from appearing.
void setDesktopLaunchEnabled(bool enabled);
bool isDesktopLaunchEnabled();

// Safely open the completed downloaded file using the default desktop handler.
// Checks existence, file type, readable permissions, canonical path, and optional allowedRoot containment.
bool openFile(const QString &filePath, const QString &allowedRoot = QString{});

// Safely reveal the file in file manager using org.freedesktop.FileManager1.ShowItems if available.
// Falls back to opening the containing directory via QDesktopServices::openUrl.
bool revealInFolder(const QString &filePath, const QString &fallbackDirectory = QString{});

}  // namespace DownloadDesktopActions
