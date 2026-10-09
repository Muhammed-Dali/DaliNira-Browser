#include "browser_layout.h"

#include <QApplication>
#include <QColor>
#include <QPalette>
#include <QStyleHints>

namespace dalinira::layout {

QString layoutModeToString(BrowserLayoutMode mode) {
  switch (mode) {
    case BrowserLayoutMode::Classic:
      return QStringLiteral("classic");
    case BrowserLayoutMode::VerticalTabsLeft:
      return QStringLiteral("left_panel");
    case BrowserLayoutMode::VerticalTabsRight:
      return QStringLiteral("right_panel");
    case BrowserLayoutMode::FloatingSidebar:
      return QStringLiteral("floating_panel");
  }
  return QStringLiteral("classic");
}

BrowserLayoutMode layoutModeFromString(const QString &str, BrowserLayoutMode defaultMode) {
  const QString trimmed = str.trimmed().toLower();
  if (trimmed == QLatin1String("classic")) {
    return BrowserLayoutMode::Classic;
  }
  if (trimmed == QLatin1String("left_panel") || trimmed == QLatin1String("left")) {
    return BrowserLayoutMode::VerticalTabsLeft;
  }
  if (trimmed == QLatin1String("right_panel") || trimmed == QLatin1String("right")) {
    return BrowserLayoutMode::VerticalTabsRight;
  }
  if (trimmed == QLatin1String("vertical_tabs") || trimmed == QLatin1String("vertical")) {
    return BrowserLayoutMode::VerticalTabsLeft;
  }
  if (trimmed == QLatin1String("floating_panel") || trimmed == QLatin1String("floating")) {
    return BrowserLayoutMode::FloatingSidebar;
  }
  return defaultMode;
}

BrowserLayoutMode currentLayoutMode() {
  QSettings s;
  const QString val = s.value(QStringLiteral("browser/layout"), QStringLiteral("classic")).toString();
  return layoutModeFromString(val, BrowserLayoutMode::Classic);
}

void setPersistedLayoutMode(BrowserLayoutMode mode) {
  QSettings s;
  s.setValue(QStringLiteral("browser/layout"), layoutModeToString(mode));
}

QString appearanceThemeToString(AppearanceTheme theme) {
  switch (theme) {
    case AppearanceTheme::System: return QStringLiteral("system");
    case AppearanceTheme::Light: return QStringLiteral("light");
    case AppearanceTheme::Dark: return QStringLiteral("dark");
  }
  return QStringLiteral("system");
}

AppearanceTheme appearanceThemeFromString(const QString &str, AppearanceTheme defaultTheme) {
  const QString value = str.trimmed().toLower();
  if (value == QLatin1String("system")) return AppearanceTheme::System;
  if (value == QLatin1String("light")) return AppearanceTheme::Light;
  if (value == QLatin1String("dark")) return AppearanceTheme::Dark;
  return defaultTheme;
}

AppearanceTheme currentAppearanceTheme() {
  return appearanceThemeFromString(
      QSettings().value(QStringLiteral("appearance/theme"), QStringLiteral("system")).toString());
}

void setPersistedAppearanceTheme(AppearanceTheme theme) {
  QSettings settings;
  settings.setValue(QStringLiteral("appearance/theme"), appearanceThemeToString(theme));
  settings.sync();
}

bool isDarkAppearance(AppearanceTheme theme) {
  if (theme == AppearanceTheme::Dark) return true;
  if (theme == AppearanceTheme::Light) return false;
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
  return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
#else
  const QColor window = QApplication::palette().color(QPalette::Window);
  return window.lightness() < 128;
#endif
}

void applyApplicationTheme(QApplication *application, AppearanceTheme theme) {
  if (!application) return;
  const bool dark = isDarkAppearance(theme);
  QPalette palette;
  if (dark) {
    palette.setColor(QPalette::Window, QColor(0x20, 0x21, 0x24));
    palette.setColor(QPalette::WindowText, QColor(0xf1, 0xf3, 0xf4));
    palette.setColor(QPalette::Base, QColor(0x18, 0x19, 0x1c));
    palette.setColor(QPalette::AlternateBase, QColor(0x2b, 0x2a, 0x33));
    palette.setColor(QPalette::ToolTipBase, QColor(0x2b, 0x2a, 0x33));
    palette.setColor(QPalette::ToolTipText, QColor(0xf1, 0xf3, 0xf4));
    palette.setColor(QPalette::Text, QColor(0xf1, 0xf3, 0xf4));
    palette.setColor(QPalette::Button, QColor(0x2b, 0x2a, 0x33));
    palette.setColor(QPalette::ButtonText, QColor(0xf1, 0xf3, 0xf4));
    palette.setColor(QPalette::Highlight, QColor(0x8a, 0xb4, 0xf8));
    palette.setColor(QPalette::HighlightedText, QColor(0x20, 0x21, 0x24));
    palette.setColor(QPalette::Link, QColor(0x8a, 0xb4, 0xf8));
  } else {
    palette.setColor(QPalette::Window, QColor(0xf5, 0xf7, 0xfa));
    palette.setColor(QPalette::WindowText, QColor(0x20, 0x2a, 0x36));
    palette.setColor(QPalette::Base, Qt::white);
    palette.setColor(QPalette::AlternateBase, QColor(0xec, 0xf0, 0xf5));
    palette.setColor(QPalette::ToolTipBase, Qt::white);
    palette.setColor(QPalette::ToolTipText, QColor(0x20, 0x2a, 0x36));
    palette.setColor(QPalette::Text, QColor(0x20, 0x2a, 0x36));
    palette.setColor(QPalette::Button, QColor(0xea, 0xee, 0xf3));
    palette.setColor(QPalette::ButtonText, QColor(0x20, 0x2a, 0x36));
    palette.setColor(QPalette::Highlight, QColor(0x18, 0x73, 0xd1));
    palette.setColor(QPalette::HighlightedText, Qt::white);
    palette.setColor(QPalette::Link, QColor(0x18, 0x73, 0xd1));
  }
  application->setPalette(palette);
}

} // namespace dalinira::layout
