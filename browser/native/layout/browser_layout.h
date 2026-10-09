#pragma once

#include <QString>
#include <QSettings>

class QApplication;

namespace dalinira::layout {

enum class BrowserLayoutMode {
  Classic,
  VerticalTabsLeft,
  VerticalTabsRight,
  FloatingSidebar,

  // Source compatibility for the Step 4 names. They intentionally map to
  // the same real layouts rather than creating a second set of modes.
  LeftPanel = VerticalTabsLeft,
  RightPanel = VerticalTabsRight,
  VerticalTabs = VerticalTabsLeft,
  FloatingPanel = FloatingSidebar
};

enum class AppearanceTheme { System, Light, Dark };

QString layoutModeToString(BrowserLayoutMode mode);
BrowserLayoutMode layoutModeFromString(const QString &str, BrowserLayoutMode defaultMode = BrowserLayoutMode::Classic);

BrowserLayoutMode currentLayoutMode();
void setPersistedLayoutMode(BrowserLayoutMode mode);

QString appearanceThemeToString(AppearanceTheme theme);
AppearanceTheme appearanceThemeFromString(
    const QString &str, AppearanceTheme defaultTheme = AppearanceTheme::System);
AppearanceTheme currentAppearanceTheme();
void setPersistedAppearanceTheme(AppearanceTheme theme);
bool isDarkAppearance(AppearanceTheme theme);
void applyApplicationTheme(QApplication *application, AppearanceTheme theme);

} // namespace dalinira::layout
