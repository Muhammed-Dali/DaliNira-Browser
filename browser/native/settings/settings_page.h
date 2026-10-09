#pragma once

#include <QHash>
#include <QUrl>
#include <QWidget>

#include "browser_icons.h"

#include <functional>
#include <vector>

class BrowserProfileService;
class QComboBox;
class QLineEdit;
class QLabel;
class QListWidget;
class QStackedWidget;
class PrivacyDetailSubpage;

namespace dalinira {
class TabPerformanceManager;
}

// Trusted native browser chrome. It is never loaded into a web renderer and
// intentionally exposes no QObject or settings bridge to web content.
class SettingsPage final : public QWidget {
  Q_OBJECT
 public:
  enum class Category {
    Startup,
    Appearance,
    Performance,
    Content,
    Privacy,
    Blocker,
    AdBlock = Blocker,
    Search,
    Passwords,
    Bookmarks,
    History,
    Downloads,
    Languages,
    Accessibility,
    System,
    Reset,
    Listening,
    About
  };

  struct Hooks {
    std::function<QString()> searchEngine;
    std::function<void(const QString &)> setSearchEngine;
    std::function<void()> syncNewTabs;
    std::function<void()> refreshBookmarks;
    std::function<void()> refreshBookmarkBarVisibility;
    std::function<void()> refreshTabStyle;
    std::function<void(const QString &)> setLayoutMode;
    std::function<void(const QString &)> setTheme;
    std::function<void()> reopenOnboarding;
    std::function<dalinira::TabPerformanceManager *()> performanceManager;
  };

  SettingsPage(BrowserProfileService *profileService, Hooks hooks, QWidget *parent = nullptr);
  void setCategory(Category category);
  void refreshPreferences();
  void refreshAppearance();

 signals:
  void navigateRequested(const QUrl &url);
  // A mailto: draft for the user's own mail client, never a tab navigation.
  void emailRequested(const QUrl &url);
  void appearanceResetRequested();

 private:
  QWidget *createStartupSection();
  QWidget *createAppearanceSection();
  QWidget *createPerformanceSection();
  QWidget *createContentSection();
  QWidget *createPrivacySection();
  QWidget *createBlockerSection();
  QWidget *createAdBlockSection() { return createBlockerSection(); }
  QWidget *createSearchSection();
  QWidget *createPasswordsSection();
  QWidget *createDownloadsSection();
  QWidget *createBookmarksSection();
  QWidget *createHistorySection();
  QWidget *createLanguagesSection();
  QWidget *createAccessibilitySection();
  QWidget *createSystemSection();
  QWidget *createResetSection();
  QWidget *createListeningSection();
  QWidget *createAboutSection();
  void addCategory(Category category, BrowserIcon icon, const QString &name, const QString &keywords, QWidget *section);
  void addSidebarSeparator();
  void selectCategory(int index);
  void applyFilter(const QString &query);
  void retranslateUi();

  BrowserProfileService *profileService_ = nullptr;
  Hooks hooks_;
  QListWidget *sidebar_ = nullptr;
  QStackedWidget *content_ = nullptr;
  QLineEdit *search_ = nullptr;
  QLabel *title_ = nullptr;
  QComboBox *uiLangCombo_ = nullptr;
  QStackedWidget *privacyStack_ = nullptr;
  PrivacyDetailSubpage *privacySubpage_ = nullptr;
  std::function<void()> updatePrivacySubtitles_;
  std::function<void()> refreshHistory_;
  std::function<void()> refreshBookmarks_;
  // Lists built from composed/runtime strings; re-run after a language switch.
  std::vector<std::function<void()>> languageRefreshers_;
  QHash<Category, int> categoryIndexes_;
  QHash<int, QString> searchKeywords_;
  QHash<int, int> contentSidebarRows_;
};
