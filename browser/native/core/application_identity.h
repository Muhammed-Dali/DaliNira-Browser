#pragma once

#include <QCoreApplication>
#include <QFileInfo>
#include <QString>

namespace dalinira::application_identity {

inline constexpr auto kApplicationName = "DaliNiraBrowser";
inline constexpr auto kDisplayName = "DaliNira";
inline constexpr auto kOrganizationName = "DaliNira";
inline constexpr auto kDesktopFileName = "dalinira";
inline constexpr auto kStartupWmClass = "DaliNiraBrowser";

void apply();

// The directory of the running executable, symlinks resolved. Packages start
// DaliNira through /usr/bin/dalinira-browser -> ../lib/dalinira-browser/, and
// Qt's applicationDirPath() names the directory the program was started from
// (/usr/bin), where none of DaliNira's files are. Use this for files installed
// next to the executable.
inline QString applicationDirectory() {
  const QString executable = QFileInfo(QStringLiteral("/proc/self/exe")).canonicalFilePath();
  if (!executable.isEmpty()) return QFileInfo(executable).absolutePath();
  return QCoreApplication::applicationDirPath();
}

}  // namespace dalinira::application_identity
