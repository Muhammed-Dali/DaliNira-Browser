#include "browser_permission_policy.h"
#include "i18n/i18n.h"

namespace BrowserPermissionPolicy {

QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
QString featureName(QWebEnginePage::Feature feature) {
  switch (feature) {
    case QWebEnginePage::Geolocation: return I18n::text(QStringLiteral("permission.name_location"));
    case QWebEnginePage::MediaAudioCapture: return I18n::text(QStringLiteral("permission.name_microphone"));
    case QWebEnginePage::MediaVideoCapture: return I18n::text(QStringLiteral("permission.name_camera"));
    case QWebEnginePage::MediaAudioVideoCapture: return I18n::text(QStringLiteral("permission.name_camera_microphone"));
    case QWebEnginePage::MouseLock: return I18n::text(QStringLiteral("permission.name_pointer_lock"));
    case QWebEnginePage::DesktopVideoCapture: return I18n::text(QStringLiteral("permission.name_screen_sharing"));
    case QWebEnginePage::DesktopAudioVideoCapture: return I18n::text(QStringLiteral("permission.name_screen_audio_sharing"));
    case QWebEnginePage::Notifications: return I18n::text(QStringLiteral("permission.name_notifications"));
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    case QWebEnginePage::ClipboardReadWrite: return I18n::text(QStringLiteral("permission.name_clipboard"));
    case QWebEnginePage::LocalFontsAccess: return I18n::text(QStringLiteral("permission.name_local_fonts"));
#endif
  }
  return I18n::text(QStringLiteral("permission.name_site_feature"));
}
QT_WARNING_POP

}  // namespace BrowserPermissionPolicy
