#pragma once

#include <QColor>
#include <QIcon>

enum class BrowserIcon {
  Startup,
  Appearance,
  Performance,
  Content,
  Privacy,
  Search,
  Password,
  Bookmark,
  Download,
  Language,
  Translate,
  Accessibility,
  Settings,
  Reset,
  Info,
  NewTab,
  Window,
  Incognito,
  History,
  Zoom,
  Print,
  Save,
  Tools,
  Help,
  Exit,
  Folder,
  Trash,
  Grid,
  Clock,
  Cards,
  Video,
  Music,
  Play,
  Clipboard,
  More,
  Minimize,
  Maximize,
  Restore,
  Close,
  Audio,
  Memory,
  Location,
  Camera,
  Microphone,
  Notification,
  Cookie,
  Javascript,
  Image,
  Popup,
  ChevronRight,
  ChevronUp,
  ChevronDown,
  Fonts,
  Mouse,
  Pdf,
  LocationSlash,
  CameraSlash,
  MicrophoneSlash,
  NotificationSlash,
  ArrowLeft,
  ArrowRight,
  Reload,
  Stop,
  JavascriptSlash,
  ImageSlash,
  PopupSlash,
  ProtectedContent,
  InsecureContent,
  SiteData,
  JsOptimize,
  Fullscreen,
  Tune,
  FindInPage,
  Mail
};

namespace BrowserIcons {
QIcon icon(BrowserIcon id);
// Same icon drawn in caller-chosen colours, for surfaces that follow the light
// theme (icon() is tuned for the dark toolbar).
QIcon tintedIcon(BrowserIcon id, const QColor &normal, const QColor &active, const QColor &disabled);
QIcon backIcon();
QIcon forwardIcon();
QIcon reloadIcon();
QIcon stopIcon();
QIcon appIcon();
QIcon incognitoIcon();
QIcon youtubeIcon();
QIcon youtubeMusicIcon();
QIcon searchEngineIcon(const QString &engineName);
QIcon combinedMediaCaptureIcon();
QIcon chromiumIcon();
QIcon webkitIcon();
QString resourcePath(BrowserIcon id);
}  // namespace BrowserIcons
