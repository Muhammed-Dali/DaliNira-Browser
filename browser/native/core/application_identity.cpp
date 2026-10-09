#include "application_identity.h"

#include <QCoreApplication>
#include <QGuiApplication>

namespace dalinira::application_identity {

void apply() {
  QCoreApplication::setApplicationName(QString::fromLatin1(kApplicationName));
  QCoreApplication::setOrganizationName(QString::fromLatin1(kOrganizationName));
  // QGuiApplication's display/desktop identity setters consult the platform
  // integration. Calling them before QApplication exists produces
  // applicationFilePath warnings and can initialize desktop services with an
  // incomplete identity.  The Core identity is safe (and needed by QSettings)
  // before QApplication; defer the GUI-only part until afterwards.
  if (qobject_cast<QGuiApplication *>(QCoreApplication::instance())) {
    QGuiApplication::setApplicationDisplayName(QString::fromLatin1(kDisplayName));
    QGuiApplication::setDesktopFileName(QString::fromLatin1(kDesktopFileName));
  }
}

}  // namespace dalinira::application_identity
